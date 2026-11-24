# Kinect v2 Live Viewer — Technical Research

*Prepared 2026-11-10 on a MacBook Pro (M1 Pro), macOS 26.5.2, Xcode 26.5.*

## Evidence Grading Key

Every technical claim below carries an explicit evidence grade:
- **[A] Primary**: Peer-reviewed paper, IETF RFC, official Apple SDK documentation/header, or upstream `libfreenect2` public header (`include/libfreenect2/` as installed).
- **[B] Secondary**: Technical engineering post by original authors, archival standards documentation, or empirical benchmark synthesis.
- **[C] Forum / Vendor**: Maintainer issue threads, developer Q&A, or vendor/community integration guides.
- **[D] Inference / Calculation**: First-principles mathematical derivation or architectural deduction from [A]/[B] facts.

---

## 1. Mapping IR (16-bit) and Depth (Float mm) for Human Viewing

### 1.1 Infrared (512×424, `[0.0, 65535.0]` float from `libfreenect2`)

1. **Sensor signal physics and dynamic range**:
   - `libfreenect2::Frame::Ir` delivers 512×424 `float` values in `[0.0, 65535.0]`, saturating at `65535.0` **[A]** ([`frame_listener.hpp`](https://github.com/OpenKinect/libfreenect2/blob/master/include/libfreenect2/frame_listener.hpp), [OpenKinect libfreenect2 docs](https://openkinect.github.io/libfreenect2/)).
   - Kinect v2 is an active continuous-wave Time-of-Flight (CW-ToF) sensor whose modulated 850 nm illumination intensity falls off inversely with the square of distance ($I \propto 1/r^2$) plus surface albedo **[A]** ([Sarbolandi et al., CVIU 2015, DOI: 10.1016/j.cviu.2015.05.006](https://doi.org/10.1016/j.cviu.2015.05.006); [arXiv:1505.05459](https://arxiv.org/abs/1505.05459)).
   - Consequently, linear scaling `val / 65535.0 * 255` crushes room-scale objects at 1.5–4.5 m into the bottom 1–5% of 8-bit luma whenever a near object or retroreflector is in view **[D]**.

2. **Percentile auto-scaling**:
   - Computing per-frame lower and upper percentiles ($P_{\text{low}} = P_{1.0}$, $P_{\text{high}} = P_{99.5}$) over non-saturated valid pixels (`0 < v < 65535`) rejects specular glints and dead pixels while using the full `[0, 255]` output span **[D]**.
   - Using a 1024-bin or 4096-bin integer histogram over 217,088 pixels (512×424) requires a single $O(N)$ pass (< 0.15 ms on M1 Pro) rather than $O(N \log N)$ sorting **[D]**.
   - Pixels at `v >= 65535.0f` must be explicitly gated as saturated (clamped to 255 and counted in telemetry) **[A]** ([`frame_listener.hpp`](https://github.com/OpenKinect/libfreenect2/blob/master/include/libfreenect2/frame_listener.hpp)).

3. **Temporal smoothing of scale bounds**:
   - Raw per-frame percentile endpoints jitter by several counts when small bright objects cross the frame edge, causing visible brightness "pumping" at 30 fps **[D]**.
   - Applying an Asymmetric or Exponential Moving Average (EMA) to the black and white points:
     $$B_t = (1 - \alpha) B_{t-1} + \alpha P_{\text{low}, t}, \qquad W_t = (1 - \alpha) W_{t-1} + \alpha P_{\text{high}, t}$$
     with $\alpha \in [0.08, 0.15]$ at 30 Hz ($\tau = \Delta t / \ln(1/(1-\alpha)) \approx 0.20\text{–}0.40\text{ s}$) eliminates flicker while adapting smoothly within ~0.5 s when moving between rooms, alongside a minimum span floor ($W_t - B_t \ge 256.0$) so pitch-dark or covered-lens scenes do not amplify read noise into snow **[D]**.

4. **Gamma compression**:
   - Applying power-law gamma after percentile normalization:
     $$y = \operatorname{round}\!\left(255 \cdot \operatorname{clamp}\!\left(\frac{x - B_t}{W_t - B_t}, 0, 1\right)^{\gamma}\right)$$
     with $\gamma \approx 0.45\text{–}0.50$ ($\gamma = 0.5$ is square-root compression) counteracts the $1/r^2$ inverse-square active IR irradiance falloff (since $\sqrt{1/r^2} = 1/r$), lifting mid-room and background detail into perceptual midtones **[D]**.

### 1.2 Depth (512×424, float mm) to False Colour

1. **Valid range and invalidity gating**:
   - `libfreenect2::Frame::Depth` outputs 32-bit `float` values in millimetres where *"Non-positive, NaN, and infinity are invalid or missing data"* **[A]** ([`frame_listener.hpp`](https://github.com/OpenKinect/libfreenect2/blob/master/include/libfreenect2/frame_listener.hpp)).
   - Default `Freenect2Device::Config` clips depth to `[0.5 m, 4.5 m]` (`500 mm – 4500 mm`) with bilateral and edge-aware flying-pixel filters enabled **[A]** ([`libfreenect2.hpp`](https://github.com/OpenKinect/libfreenect2/blob/master/include/libfreenect2/libfreenect2.hpp)).
   - Metrological evaluations confirm Kinect v2's reliable operative window is **0.5 m to 4.5 m** (precision 2–5 mm at 1.2 m, rising quadratically toward 4.5 m), while raw phase-unwrapped returns can reach ~8.0 m indoors with higher noise **[A]** ([Lachat et al., ISPRS Archives 2015, DOI: 10.5194/isprsarchives-XL-5-W4-93-2015](https://doi.org/10.5194/isprsarchives-XL-5-W4-93-2015); [Fankhauser et al., IEEE ICAR 2015, DOI: 10.1109/ICAR.2015.7251485](https://doi.org/10.1109/ICAR.2015.7251485)).
   - Therefore, every depth pixel MUST first check `std::isfinite(d) && d > 0.0f`; invalid pixels map strictly to pure black `(0, 0, 0)` so missing/occluded regions never blend into near-range colours **[A, D]**. User sliders should default to `500 mm – 4500 mm` and allow expansion up to `8000 mm` (updating `Freenect2Device::Config::MaxDepth` accordingly) **[D]**.

2. **Colormap selection (Turbo vs. Viridis vs. Inferno vs. Jet)**:
   - Legacy **Jet** has sharp luminance reversals at cyan and yellow knots that create false Mach bands and confuse deuteranopic/protanopic vision **[B]** ([Mikhailov, Google Research 2019](https://research.google/blog/turbo-an-improved-rainbow-colormap-for-visualization/)).
   - **Turbo** (Anton Mikhailov, Google Research, 2019) is a $C^2$-continuous cubic-spline rainbow replacement with smooth lightness progression, roughly twice the lightness contrast slope of Viridis, and strong discriminability for millimeter-scale depth relief **[B, A]** ([Blog](https://research.google/blog/turbo-an-improved-rainbow-colormap-for-visualization/); [Official 256-entry sRGB LUT](https://gist.github.com/mikhailov-work/ee72ba4191942acecc03fe6da94fc73f)). It is the standard default for real-time depth cameras because near (warm red/yellow, or inverted depending on convention—we map near = warm/bright or standard 1 - normalized so closer objects pop against black invalid background) vs. far (cool blue/violet) reads instantaneously across a room **[D]**.
   - **Viridis** and **Inferno** (Smith & van der Walt, 2015) are strictly perceptually uniform with monotonically increasing luminance ($L^*$), making them ideal when monotonic brightness-to-distance ordering is preferred over hue span **[B]** ([BIDS Colormap Specification](https://bids.github.io/colormap/)).
   - Supporting **Turbo (default)**, **Viridis**, **Inferno**, and **Grayscale** via precomputed 256-entry LUTs adds zero runtime overhead (< 0.1 ms/frame) **[D]**.

---

## 2. Lowest-Latency LAN Streaming to iOS

### 2.1 Architecture Comparison

| Option | Typical Home Wi-Fi Glass-to-Glass Latency | Host + iPhone CPU Cost | Typical Wi-Fi Bitrate (30 fps) | Trade-offs |
|---|---|---|---|---|
| **Hardware H.264 / HEVC (`VTCompressionSession`) over `Network.framework` TLS 1.3** | **45 – 95 ms** **[D]** (sensor ~25 ms + map/encode ~5 ms + Wi-Fi/TLS ~4 ms + decode/display ~10–16 ms) | **Very low** (< 4% of 1 core; dedicated Apple Media Engine silicon) **[A, D]** | **1.5 – 4.0 Mbps** (512×424) / **6 – 12 Mbps** (1080p) **[D]** | Zero third-party dependencies; native mTLS 1.3 cert pinning via `Security.framework`; exact frame-level control **[A]**. |
| **Hardware H.264 / HEVC over `Network.framework` QUIC (`NWProtocolQUIC`)** | **45 – 90 ms** **[D]** | **Very low** (< 5% of 1 core) **[D]** | **1.5 – 12 Mbps** **[D]** | Avoids TCP head-of-line blocking on lossy Wi-Fi via per-frame streams/datagrams, though `NWProtocolQUIC` TLS client-cert mutual auth quirks on iOS require more care than `NWProtocolTLS` over TCP with `TCP_NODELAY` **[C, D]**. |
| **WebRTC (SRTP/UDP + ICE)** | **60 – 160 ms** **[B, C]** | **Moderate** (10–25% of 1 core + large ~30–60 MB `libwebrtc` binary) **[C]** | **2 – 12 Mbps** **[C]** | Built-in adaptive jitter buffer adds 20–60 ms unless aggressively patched; pulls in massive third-party C++ stack with STUN/ICE machinery we explicitly do not want **[C, D]**. |
| **MJPEG over TLS/TCP** | **65 – 150 ms** (spikes under Wi-Fi contention) **[C, D]** | **Moderate** (JPEG encode/decode per frame) **[D]** | **12 – 20 Mbps** (512×424) / **45 – 90 Mbps** (1080p) **[D]** | Zero inter-frame state, but 10× higher Wi-Fi airtime causes 802.11 bufferbloat and heats iPhone Wi-Fi radio **[D]**. |
| **RTSP (RTP/UDP or TCP)** | **150 – 400 ms** **[C]** | **Low–Moderate** **[C]** | **2 – 12 Mbps** **[C]** | iOS `AVPlayer` does not natively play raw low-latency RTSP without custom RTP depacketizers + client jitter buffers; no native mTLS pinning **[C, D]**. |

### 2.2 Selected Transport & Encoder Configuration

We select **Hardware H.264/HEVC via `VTCompressionSession` over `Network.framework` TLS 1.3 + TCP (`noDelay = true`)** **[A, D]**:
- Set `kVTCompressionPropertyKey_RealTime = kCFBooleanTrue` **[A]** ([Apple VTCompressionSession Docs](https://developer.apple.com/documentation/videotoolbox/vtcompressionsession)).
- Set `kVTCompressionPropertyKey_AllowFrameReordering = kCFBooleanFalse` (zero B-frames -> zero reorder latency) **[A]**.
- Set `kVTCompressionPropertyKey_MaxFrameDelayCount = 0` and keyframe interval `30` frames (1.0 s), plus force an immediate IDR keyframe (`kVTEncodeFrameOptionKey_ForceKeyFrame = true`) whenever a client connects or switches modes **[A, D]**.
- Set `NWProtocolTCP.Options.noDelay = true` (`TCP_NODELAY`) to disable Nagle's algorithm so every encoded access unit (5–40 KB) flushes to Wi-Fi immediately **[A]** ([Apple Network Framework Docs](https://developer.apple.com/documentation/network)).
- Implement **sender backpressure**: if a slow Wi-Fi moment leaves an unacknowledged frame queued in the per-client send slot, drop intermediate non-keyframe sends and trigger a fresh IDR once drained so glass-to-glass latency never accumulates **[D]**.

### 2.3 Sending IR and Depth Without Crushing Detail

- An 8-bit preview stream (`kCVPixelFormatType_32BGRA` fed to VideoToolbox, encoded at generous bitrate: **4 Mbps** for 512×424, **6 Mbps** for 1024×424, **10 Mbps** for 1920×1080) preserves perceptual detail cleanly once tone-mapping (percentile + EMA + gamma for IR; Turbo/Viridis LUT for depth) runs in floating-point *before* 8-bit quantization **[D]**.
- Standard hardware H.264/HEVC 4:2:0 chroma subsampling halves chroma resolution (`256×212` for a `512×424` frame), which can slightly soften 1-pixel false-colour boundaries. Upscaling 512×424 single-stream modes by **2× nearest-neighbour (`1024×848`)** before 4:2:0 hardware encoding gives full `512×424` chroma samples in 4:2:0 (`1024/2 × 848/2 = 512×424` exact!), preserving pixel-sharp depth edges and barcode bits at negligible M1 Pro hardware-encoder cost (< 1.5 ms/frame) **[D]**.

---

## 3. Pairing and Security Without a Cloud

### 3.1 TLS 1.3 Mutual Authentication (mTLS) vs. Noise Protocol

- **TLS 1.3 (RFC 8446)** provides 1-RTT mutual authentication using `CertificateRequest`, `Certificate`, and `CertificateVerify` during the initial handshake, encrypting client certificates under handshake traffic keys so passive LAN observers cannot even read the client certificate **[A]** ([IETF RFC 8446 §4.3.2, §4.4](https://datatracker.ietf.org/doc/html/rfc8446)).
- **Noise Protocol (`XX` / `IK`)** ([Noise Protocol Spec rev 34](https://noiseprotocol.org/noise.html)) **[A]** is minimalist and excellent when writing custom UDP sockets, whereas `Network.framework` natively integrates BoringSSL/corecrypto **TLS 1.3** in kernel/userspace with `sec_protocol_options_set_min_tls_protocol_version(..., .TLSv13)`, `sec_protocol_options_set_peer_authentication_required(..., true)`, and custom certificate pinning via `sec_protocol_options_set_verify_block` **[A]** ([Apple Security/Network Docs](https://developer.apple.com/documentation/network)).
- Using TLS 1.3 with pinned self-signed ECDSA P-256 certificates gives us hardware-accelerated AES-GCM / ChaCha20-Poly1305 in `Network.framework` with zero external crypto dependencies **[A, D]**.

### 3.2 Pairing Flow (QR Code and Short Code)

1. **Persistent Identities**:
   - Host generates an ECDSA P-256 keypair + self-signed X.509 v3 certificate (stored locally with mode `0600` / Keychain).
   - iPhone generates an ECDSA P-256 keypair + self-signed X.509 v3 certificate stored **exclusively in the iOS Keychain** (`kSecAttrAccessibleWhenUnlockedThisDeviceOnly` — never synced to iCloud, never written to app container files) **[A]**.
2. **Explicit Time-Boxed Pairing Window**:
   - Pairing is **completely closed** unless the user opens **"Pair iPhone…"** on the Mac (120-second expiry, single use).
   - **QR Code**: Encodes `kv2://pair?s=<serviceName>&fp=<serverCertSHA256>&tok=<256bitToken>&h=<hostIP>&p=<pairPort>`. Scanning pins `serverCertSHA256` before connecting and proves possession of `tok` **[D]**.
   - **Short Code fallback**: Displays a random 8-digit pairing code (`XXXX-XXXX`). Over TLS 1.3 on the ephemeral pairing listener, both peers compute transcript-bound proofs:
     $$\text{Key} = \operatorname{HKDF-SHA256}(\text{IKM}=\text{secret}, \text{salt}=\operatorname{SHA256}(\text{Cert}_{\text{Mac}}) \parallel \operatorname{SHA256}(\text{Cert}_{\text{iPhone}}))$$
     Binding both certificate hashes into the HKDF salt defeats active MITM attacks: a MITM cannot substitute its own TLS certificate without changing the salt and invalidating the HMAC **[D]**. Any failed verification immediately invalidates and rotates the short code so online guessing is limited to 1 attempt per displayed code **[D]**.
3. **Post-Pairing Enforcement**:
   - The Mac stores **only one** `pinnedClientCertSHA256` at a time.
   - On the live stream port, `sec_protocol_options_set_peer_authentication_required(opts, true)` demands a client certificate inside the TLS 1.3 handshake, and `sec_protocol_options_set_verify_block` rejects any connection whose leaf certificate SHA-256 does not constant-time-match `pinnedClientCertSHA256` **[A, D]**.

### 3.3 Bonjour / mDNS & Making Unpaired Devices Learn Nothing

- DNS-SD / Bonjour ([IETF RFC 6763](https://datatracker.ietf.org/doc/html/rfc6763)) **[A]** broadcasts service instance names and optional TXT records over multicast UDP 5353.
- On iOS 14+, discovering a Bonjour service requires `NSLocalNetworkUsageDescription` and `NSBonjourServices` in `Info.plist`; iOS displays a system Local Network permission alert on first `NWBrowser` / local `NWConnection` start **[A]** ([Apple TN3179 / Network Docs](https://developer.apple.com/documentation/network)).
- **Minimizing information leakage**:
  1. When **no phone is paired** and the **"Pair iPhone"** sheet is closed, the Mac **stops Bonjour advertising completely** (`NWListener.service = nil` or listener paused) — an unpaired scanner on the LAN sees **nothing at all** **[D]**.
  2. When paired (so the paired iPhone can auto-discover across DHCP IP changes) or while pairing is open, the Mac advertises an opaque service type `_kv2s._tcp` with a random 12-hex-character instance ID (e.g. `7f3c91a04e2b`), **no Mac hostname in the service label**, and an **empty TXT record** **[D]**.
  3. Any unpaired device probing the TCP port fails inside the TLS 1.3 handshake (`certificate_required` / `access_denied`) before a single application byte is ever sent **[A, D]**.

---

## 4. The iOS App Architecture, Zero-Disk Guarantee, and Signing

### 4.1 Rendering, Mode Switching, Reconnects, Battery & Heat

- **`AVSampleBufferDisplayLayer`** renders compressed H.264/HEVC `CMSampleBuffer`s directly via the iPhone's hardware media decoder onto a Core Animation video surface without round-tripping decoded YUV/RGB textures through CPU or custom Metal shaders **[A]** ([Apple AVSampleBufferDisplayLayer Docs](https://developer.apple.com/documentation/avfoundation/avsamplebufferdisplaylayer)).
- Attaching `kCMSampleAttachmentKey_DisplayImmediately = kCFBooleanTrue` to each `CMSampleBuffer` bypasses presentation-time buffering so frames render on the very next ProMotion (120 Hz, 8.3 ms) or 60 Hz (16.7 ms) display refresh **[A, C]**.
- **Mode switching**: When the stream switches between `1024×848` (2× upscaled IR/Depth), `1024×424` (side-by-side), and `1920×1080` (Colour), the host prepends a `STREAM_CONFIG` message + fresh SPS/PPS (or VPS/SPS/PPS) + IDR frame. The iOS client recreates its `CMVideoFormatDescription` in-memory and seamlessly feeds the new `CMSampleBuffer`s to `AVSampleBufferDisplayLayer` (calling `flush()` only if `status == .failed`) **[A, D]**.
- **Battery & heat**: Hardware H.264/HEVC decode at ≤ 1080p30 uses < 200 mW on Apple A-series media silicon. Keeping Wi-Fi bitrate around 3–10 Mbps and avoiding Metal/CPU pixel copies keeps the iPhone cool over extended viewing **[D]**.

### 4.2 Guaranteeing Nothing Is Written to Disk on iOS

1. **Zero network/URL caches**: Do not use `URLSession` or `WKWebView` anywhere; additionally execute `URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)` at app startup **[A, D]**.
2. **In-memory decode pipeline**: `NWConnection.receive` reads encrypted frames into transient `Data` buffers -> constructs `CMBlockBuffer` + `CMSampleBuffer` in RAM -> hands to `AVSampleBufferDisplayLayer`. Zero temporary files, zero `FileManager` writes, `UIFileSharingEnabled = false` **[D]**.
3. **Keychain-only pairing state**: Only the 32-byte server cert SHA-256, 12-char opaque service ID, and client ECDSA identity live in iOS Keychain (`WhenUnlockedThisDeviceOnly`) **[A, D]**.
4. **Preventing iOS App-Switcher Disk Snapshots**: When an iOS app moves to inactive/background (`scenePhase != .active` / `willResignActiveNotification`), SpringBoard captures a snapshot PNG of the window to disk for the multitasking switcher **[C]**. We immediately call `sampleBufferLayer.flushAndRemoveImage()` and cover the view with an opaque black curtain on `willResignActive` *before* SpringBoard snapshots the window, ensuring not even the OS switcher caches a camera frame to flash storage **[D]**.

### 4.3 Screenshot and Screen-Recording Protection

- **`AVSampleBufferDisplayLayer.preventsCapture = true`** (public API on iOS 13+ / macOS 10.15+) instructs the system WindowServer / RenderServer compositor to exclude the video layer from **all** screenshots, Control Center screen recordings, QuickTime wired recordings, and AirPlay mirroring **[A]** ([Apple `preventsCapture` Docs](https://developer.apple.com/documentation/avfoundation/avsamplebufferdisplaylayer/preventscapture)).
- Unlike `UIApplication.userDidTakeScreenshotNotification` (which fires *after* a screenshot has already been written to Photos), `preventsCapture = true` prevents capture *proactively* at the hardware compositor level **[A]**.
- **Is screen-recording detection (`UIScreen.isCaptured` / `scene.isCaptured`) still worth adding?** **Yes, as defense-in-depth** **[D]**: observing `UIScreen.capturedDidChangeNotification` lets us also hide HUD overlays, pause decoding, and display a clear `"Screen Capture Blocked"` banner if AirPlay or screen recording starts, and observing `userDidTakeScreenshotNotification` logs a warning banner. Both cost ~15 lines of Swift and zero runtime overhead **[D]**.

### 4.4 Code Signing for Personal Use

- **Free Apple ID ("Personal Team")**: Free, works offline after signing, **expires strictly after 7 days**, limited to 3 simultaneous apps per device, no TestFlight **[A, B]** ([Apple Compare Memberships](https://developer.apple.com/support/compare-memberships/)).
- **Paid Apple Developer Program ($99/yr) — Local Xcode Signing**: Local development provisioning profiles remain valid for **1 full year** without weekly reinstalls and **never upload your app binary anywhere** **[B, C]**.
- **TestFlight**: Requires uploading the `.ipa` to Apple's App Store Connect cloud servers and expires every 90 days **[A]**. Because the design keeps everything local (no cloud; `design.md`) and privacy favours that, **direct local Xcode signing with a Paid Developer Team is strictly best** **[D]**.
- **What a direct install needs**: an Apple Development certificate for a paid Developer team in the Mac's Keychain, and the iPhone paired with Xcode **[A]**.

---

## 5. Host App Design and Windows Portability

### 5.1 Architecture Comparison

| Host Option | Pros | Cons |
|---|---|---|
| **SwiftUI macOS UI + VideoToolbox/Network.framework + Portable C++20 Core (`kinect_core` via C ABI)** | Zero-copy access to Apple VideoToolbox, `Network.framework` mTLS 1.3, Keychain, `IOKit` sleep/wake, and native macOS UI; 100% of `libfreenect2` capture, pixel mapping, synthetic generator, and stats stay in portable C++20 **[A, D]**. | Thin UI + OS media/TLS shell (~25% of host code) is rewritten per OS (Swift on macOS, Win32/WinUI + Media Foundation/Schannel or OpenSSL on Windows) **[D]**. |
| **Pure C++ (GLFW/ImGui or Qt) + FFmpeg + OpenSSL** | Single codebase across macOS and Windows **[D]**. | Loses native `Network.framework` Bonjour/TLS integration, adds heavy OpenSSL/Qt dependencies, and non-native macOS look & feel **[D]**. |
| **Electron / Tauri** | Rapid cross-platform UI **[D]**. | High RAM/CPU overhead, browser cache/disk risks, complex native addon IPC for 1080p30 frames **[D]**. |

### 5.2 Chosen Portability Split

- **`core/` (Standard C++20 + pure C99 header `kinect_core_c.h`, built via CMake or Xcode)**:
  - `Freenect2FrameSource`: wraps `libfreenect2` public API (`OpenGLPacketPipeline`, `OpenCLPacketPipeline`, `CpuPacketPipeline` — **never falls back silently**; returns explicit error if requested pipeline fails to initialize), custom `libfreenect2::Logger` filtering known harmless startup/teardown noise, exposure/gain setters, and `~/KINECT-IN-USE` lock guard.
  - `SyntheticFrameSource`: deterministic 30 fps IR/Depth/Colour test generator + dual visual/64-bit-barcode latency clock.
  - `FrameMapper`: IR percentile + EMA + gamma tone-mapper, Depth invalidity gate + Turbo/Viridis/Inferno/Grayscale LUTs, Side-by-Side compositor, and numerical histogram/min/max/mean computer.
- **`macos/` (Swift 6, AppKit/SwiftUI + VideoToolbox + Network.framework + AVFoundation)**:
  - Consumes `kinect_core_c.h` directly without C++ ABI fragility **[A]** ([Swift C/C++ Interop Docs](https://www.swift.org/documentation/cxx-interop/)).
  - On Windows (the porting plan; the host as built is in `docs/windows-setup.md`), `core/` compiles unchanged under MSVC + CMake against `libfreenect2` (UsbDk/libusbK), paired with a lightweight WinUI 3 / Direct3D 11 + Media Foundation H.264/HEVC + mDNS (`DnsServiceRegister`) + TLS 1.3 host shell **[D]**.

---

## 6. Host Recording Formats & Storage Rates

### 6.1 Format Analysis & Bitrate Calculations

At **30 fps**:
- **IR / Depth raw stream**: $512 \times 424 \times 2\text{ bytes} = 434,176\text{ B/frame} = 13.025\text{ MB/s} = \mathbf{781.5\text{ MB/min}}$ ($745.3\text{ MiB/min}$) **[D]**.
- **Colour raw BGRX stream**: $1920 \times 1080 \times 4\text{ bytes} = 8,294,400\text{ B/frame} = 248.83\text{ MB/s} = \mathbf{14,930\text{ MB/min}}$ **[D]**.

| Stream / Mode | Format Candidate | Fidelity | Expected Size (MB / min @ 30 fps) | Notes |
|---|---|---|---|---|
| **IR (512×424, 16-bit)** | **FFV1 v3 (`gray16le`) in `.mkv`** ([RFC 9043](https://datatracker.ietf.org/doc/html/rfc9043)) **[A]** | **100% Mathematically Lossless (16-bit)** **[A]** | **240 – 430 MB/min** (~30–55% of raw, scene-dependent) **[B, D]** | Single `.mkv` file + `.timestamps.csv` sidecar; multithreaded slice coding runs < 8% CPU on M1 Pro **[D]**. |
| **IR (512×424, 16-bit)** | **16-bit Grayscale PNG sequence** | **100% Mathematically Lossless (16-bit)** **[A]** | **280 – 500 MB/min** **[D]** | Zero external binary dependency (Apple `ImageIO` / `zlib`); creates 1,800 files/min in session directory **[D]**. |
| **IR / Depth** | **HEVC 4:0:0 Main 12 / Main 16** | **Truncates 16-bit to 10/12-bit** (Main 16 not supported by Apple VideoToolbox hardware) **[A, C]** | **15 – 60 MB/min** (lossy) / **220 – 380 MB/min** (`libx265` lossless) **[C, D]** | Apple Silicon VideoToolbox supports Main / Main10 / Main 4:2:2 10, **not** monochrome 16-bit lossless; software `libx265` 12-bit still loses 4 LSBs **[C, D]**. |
| **Depth (512×424, float mm -> `uint16` mm or 0.1 mm)** | **FFV1 v3 (`gray16le`) in `.mkv`** (or 16-bit PNG sequence) | **Lossless 1 mm `uint16` (`0` = invalid, `1..65535` mm)** or **0.1 mm (`0..6553.5` mm)** **[D]** | **140 – 320 MB/min** (smooth planar walls compress better than noisy IR) **[B, D]** | Preserves exact integer millimetre (well below Kinect v2's 2–5 mm physical noise floor **[A]**) with zero colourmap loss **[D]**. |
| **Colour (1920×1080)** | **Hardware HEVC (`AVAssetWriter` / VideoToolbox `.mp4`)** | **Visually lossless / high-quality 8-bit 4:2:0** (12–16 Mbps VBR; ~6–8 Mbps at 15 fps low light) **[A, D]** | **90 – 120 MB/min** @ 30 fps (**45 – 65 MB/min** @ 15 fps) **[D]** | Dedicated Media Engine (< 2% CPU), exact VFR `CMSampleBuffer` presentation timestamps **[A, D]**. |
| **Side-by-Side** | **Hardware HEVC `.mp4`** (composited view, 10 Mbps) | **High-quality visual archive** **[D]** | **60 – 80 MB/min** **[D]** | Matches exact side-by-side layout watched on Mac/iPhone **[D]**. |

### 6.2Chosen Recording Design & Folder Isolation

- **Selected formats**:
  - **IR Only**: Lossless 16-bit `gray16le` **FFV1 v3 `.mkv`** (via `/opt/homebrew/bin/ffmpeg` pipe, with automatic built-in fallback/toggle to native **16-bit Grayscale PNG sequence** via `ImageIO` if preferred) + `.timestamps.csv`.
  - **Depth Only**: Lossless 16-bit `uint16` millimetres (`0` = invalid/`NaN`/`<=0`, `1..65535` = rounded mm) in **FFV1 v3 `.mkv`** (with 16-bit PNG sequence option) + `.timestamps.csv`.
  - **Colour Only**: Native hardware **HEVC `.mp4`** via `AVAssetWriter` (`AVVideoCodecType.hevc`, VFR `mach_absolute_time` timestamps).
  - **Side-by-Side**: Native hardware **HEVC `.mp4`** via `AVAssetWriter`.
- **Host-only folder isolation (`~/kinect-viewer-recordings/`)**:
  1. **iCloud exclusion**: Create `~/kinect-viewer-recordings/.nosync` marker file *and* name every recording session subfolder `<YYYYMMDD-HHMMSS>_<mode>.nosync/` (the `.nosync` suffix is macOS `bird`'s native exclusion rule) **[C]**.
  2. **Time Machine exclusion**: Apply sticky extended-attribute exclusion via `tmutil addexclusion ~/kinect-viewer-recordings` (`com.apple.metadata:com_apple_backup_excludeItem`), verified via `tmutil isexcluded` **[B, C]**.
  3. **Spotlight exclusion**: Create `~/kinect-viewer-recordings/.metadata_never_index` so `mds`/`mdworker` never indexes filenames or video metadata **[B, C]**.

---

## References

1. **[A]** OpenKinect Contributors, *libfreenect2 Public API Documentation & Headers* (`0.2.0`, commit `fd64c5d9`), https://openkinect.github.io/libfreenect2/ and https://github.com/OpenKinect/libfreenect2
2. **[A]** H. Sarbolandi, D. Lefloch, A. Kolb, *"Kinect range sensing: Structured-light versus Time-of-Flight Kinect"*, Computer Vision and Image Understanding, Vol. 139, pp. 1–20, 2015. https://doi.org/10.1016/j.cviu.2015.05.006 (Preprint: https://arxiv.org/abs/1505.05459)
3. **[A]** E. Lachat, H. Macher, M.-A. Mittet, T. Landes, P. Grussenmeyer, *"First Experiences with Kinect v2 Sensor for Close Range 3D Modelling"*, ISPRS Archives, XL-5/W4, pp. 93–100, 2015. https://doi.org/10.5194/isprsarchives-XL-5-W4-93-2015
4. **[A]** P. Fankhauser, M. Bloesch, D. Rodriguez, R. Kaestner, M. Hutter, R. Siegwart, *"Kinect v2 for Mobile Robot Navigation: Evaluation and Modeling"*, IEEE ICAR, 2015. https://doi.org/10.1109/ICAR.2015.7251485
5. **[B]** A. Mikhailov, *"Turbo, An Improved Rainbow Colormap for Visualization"*, Google Research Blog, Aug 2019. https://research.google/blog/turbo-an-improved-rainbow-colormap-for-visualization/ (LUT: https://gist.github.com/mikhailov-work/ee72ba4191942acecc03fe6da94fc73f; the polynomial approximation the code uses, GLSL by Ruofei Du, Apache-2.0: https://gist.github.com/mikhailov-work/0d177465a8151eb6ede1768d51d476c7)
6. **[B]** N. J. Smith, S. van der Walt, *"A Better Default Colormap for Matplotlib (Viridis / Inferno)"*, SciPy 2015. https://bids.github.io/colormap/
7. **[A]** E. Rescorla, *RFC 8446: The Transport Layer Security (TLS) Protocol Version 1.3*, IETF, Aug 2018. https://datatracker.ietf.org/doc/html/rfc8446
8. **[A]** S. Cheshire, M. Krochmal, *RFC 6763: DNS-Based Service Discovery*, IETF, Feb 2013. https://datatracker.ietf.org/doc/html/rfc6763
9. **[A]** M. Niedermayer, D. Rice, J. Martinez, *RFC 9043: FFV1 Video Coding Format Versions 0, 1, and 3*, IETF, Aug 2021. https://datatracker.ietf.org/doc/html/rfc9043
10. **[A]** T. Perrin, *The Noise Protocol Framework (Revision 34)*, Jul 2018. https://noiseprotocol.org/noise.html
11. **[A]** Apple Inc., *VTCompressionSession — VideoToolbox Framework Reference*. https://developer.apple.com/documentation/videotoolbox/vtcompressionsession
12. **[A]** Apple Inc., *AVSampleBufferDisplayLayer & `preventsCapture` — AVFoundation Reference*. https://developer.apple.com/documentation/avfoundation/avsamplebufferdisplaylayer and https://developer.apple.com/documentation/avfoundation/avsamplebufferdisplaylayer/preventscapture
13. **[A]** Apple Inc., *Network Framework Reference (`NWListener`, `NWBrowser`, `NWConnection`, `NWProtocolTLS`)*. https://developer.apple.com/documentation/network
14. **[A]** Apple Inc., *Choosing a Membership (Personal Team vs. Apple Developer Program)*. https://developer.apple.com/support/compare-memberships/
15. **[A]** Swift Project, *Mixing Swift and C++ / C Interoperability*. https://www.swift.org/documentation/cxx-interop/
