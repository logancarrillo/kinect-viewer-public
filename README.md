# Kinect v2 Personal Live Viewer (macOS Host + iOS Pocket Viewer)

A personal, zero-cloud live viewer for the **Microsoft Kinect v2** (`Xbox NUI Sensor`, `0x045e:0x02c4`) on macOS (`Apple Silicon`), with low-latency mutually authenticated **TLS 1.3** live streaming over home Wi-Fi to an iPhone app (`KinectPocketViewer`) and optional **host-only** lossless 16-bit **Depth and IR** recording (colour is strictly live-only in memory).

There is also a **Windows 11 host** (`windows/`, C++ with MSVC) with the same viewer, the same iPhone streaming and a pet camera: see [`docs/windows-setup.md`](docs/windows-setup.md) to build and run it, and [`docs/pet-camera.md`](docs/pet-camera.md).

Licensed under the Apache License 2.0 ([`LICENSE`](LICENSE)). [`NOTICE`](NOTICE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) cover the third-party parts.

---

## Architecture & Privacy Guarantees

1. **Zero Storage on iPhone**:
   - `URLCache.shared` is configured to `0` memory / `0` disk bytes before any view loads.
   - Video decodes directly into GPU memory via `AVSampleBufferDisplayLayer` (`sampleBufferRenderer`, `kCMSampleAttachmentKey_DisplayImmediately`, `preventsCapture = true`).
   - Display surfaces are flushed (`flush(removingDisplayedImage: true)`) on `willResignActive`, `didEnterBackground`, screen-recording (`UIScreen.capturedDidChangeNotification`), and screenshot notifications so iOS app-switcher snapshots never capture a frame.
   - Pairing keys live exclusively in the hardware-backed iOS Keychain (`kSecAttrAccessibleWhenUnlockedThisDeviceOnly`). Sandbox audit confirmed `0` files in `Documents/`, `Library/Caches/`, and `tmp/`.
2. **Host-Only Excluded Recordings**:
   - Saved strictly inside `~/kinect-viewer-recordings/` (`0700` permissions) with all three exclusions automatically applied and verified:
     - **iCloud**: `.nosync` marker file + `<timestamp>_<mode>.nosync/` session subfolder suffix + `NSURLIsExcludedFromBackupKey = true`
     - **Time Machine**: `tmutil addexclusion ~/kinect-viewer-recordings` (`[Excluded]`)
     - **Spotlight**: `.metadata_never_index`
   - Zero upload, cloud or sync code exists anywhere in the project. A paired phone can ask the host to start or stop a host recording (`remoteControlCmd`, protocol v2), but no message carries a recording to the phone. The one exception is the Windows pet camera's camera roll (below), which plays its clips to paired devices.
3. **Mutually Pinned TLS 1.3 & Silent Discovery**:
   - When unpaired and idle, **nothing** is advertised over Bonjour/mDNS.
   - Pairing opens a single-attempt, 120-second window (`_kv2p._tcp.local`) authenticated with HKDF-SHA256 + HMAC-SHA256 bound to both peers' ECDSA P-256 certificate SHA-256 digests (`shared/CryptoIdentity.swift`). A single wrong short-code guess immediately burns and closes the pairing offer.
   - After pairing, only `<12-hex-random-id>._kv2s._tcp.local` (empty TXT record) is advertised, and every stream connection requires **mutual TLS 1.3** matching the exact pinned iPhone leaf certificate.
4. **Coexistence Lock (`~/KINECT-IN-USE`)**:
   - Before opening the physical USB sensor, the host checks `~/KINECT-IN-USE`. If held by another live process (PID), the viewer refuses to open the sensor and waits cleanly without touching USB. When open, it writes its tool name, PID, and ISO-8601 timestamp, and deletes `~/KINECT-IN-USE` immediately on stop, unplug, sleep, or exit.

---

## Prerequisites

- **macOS 26** or later and **Xcode 26** or later (the apps' deployment targets are macOS 26.0 and iOS 26.0, Swift 6), on Apple Silicon. Built and tested with macOS 26.5 and Xcode 26.5.
- **[Homebrew](https://brew.sh)**, for the tools and libfreenect2's dependencies:
  ```bash
  brew install cmake pkgconf libusb jpeg-turbo glfw xcodegen ffmpeg
  ```
  The host runs `ffmpeg` (`/opt/homebrew/bin/ffmpeg`, with `-nostdin`) for 16-bit lossless FFV1 `.mkv` recording; a native 16-bit PNG sequence fallback is also built in.
- **`libfreenect2`** (`0.2.0` / upstream `fd64c5d9b214df6f6a55b4419357e51083f15d93`; there's no Homebrew formula for it), built and installed under one prefix. The default prefix is `~/libfreenect2`; if you install it elsewhere, set `LIBFREENECT2_PREFIX` in `Config/Local.xcconfig`. Its source isn't committed to this repo (`vendor/` is gitignored). From the checkout's folder:
  ```bash
  git clone https://github.com/OpenKinect/libfreenect2.git vendor/libfreenect2-src
  git -C vendor/libfreenect2-src checkout fd64c5d9b214df6f6a55b4419357e51083f15d93
  cmake -S vendor/libfreenect2-src -B vendor/libfreenect2-build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_CXX11=ON -DENABLE_CUDA=OFF \
    -DCMAKE_INSTALL_PREFIX="$HOME/libfreenect2"
  cmake --build vendor/libfreenect2-build --target install
  ```
  CMake should find OpenGL, OpenCL, VideoToolbox and TurboJPEG. The prefix then holds `include/`, `lib/libfreenect2.dylib`, `lib/cmake/freenect2` and `lib/pkgconfig`; other CMake projects can use it with `-DCMAKE_PREFIX_PATH="$HOME/libfreenect2"`.

---

## How to Build & Run

Clone this repository (or unpack its source snapshot) and run these from its folder.

### 1. Generate the Xcode Project (if `project.yml` changed)
```bash
xcodegen generate
```

### 2. Build & Run the macOS Host App (`KinectViewer.app`)
```bash
xcodebuild -project KinectViewer.xcodeproj \
  -scheme KinectHostApp \
  -destination 'platform=macOS,arch=arm64' \
  -configuration Release \
  -derivedDataPath build/DerivedData \
  build

open build/DerivedData/Build/Products/Release/KinectViewer.app
```

The command-line tool and the tests:
```bash
xcodebuild -project KinectViewer.xcodeproj -scheme kinect-cli \
  -destination 'platform=macOS,arch=arm64' -configuration Release -derivedDataPath build/DerivedData build
xcodebuild -project KinectViewer.xcodeproj -scheme KinectHostApp \
  -destination 'platform=macOS,arch=arm64' -derivedDataPath build/DerivedData test
xcodebuild -project KinectViewer.xcodeproj -scheme KinectPocketViewer \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' -derivedDataPath build/DerivedData test
```

Optional CLI flags when launching the Mac app directly:
- `--synthetic`: Start on the synthetic test-pattern generator instead of opening the USB Kinect.
- `--clock-overlay`: Enable the synthetic millisecond stopwatch + 64-bit pixel barcode (synthetic mode only).
- `--no-autostart`: Launch idle without opening a stream until **Start** is clicked.

### 3. Build & Install the iOS App (`KinectPocketViewer`) on Your iPhone
1. Open `KinectViewer.xcodeproj` in Xcode:
   ```bash
   open KinectViewer.xcodeproj
   ```
2. Select the **`KinectPocketViewer`** scheme at the top of Xcode and choose your iPhone as the run destination (over USB or Wi-Fi).
3. Signing uses Automatic Signing. Copy `Config/Local.xcconfig.example` to `Config/Local.xcconfig` (gitignored) and set your team (`KV_DEVELOPMENT_TEAM`) and bundle-ID prefix (`KV_BUNDLE_ID_PREFIX`). Without it, the apps build as `com.example.kinectviewer.*` with no team (simulator only).
4. Press **Cmd+R** (**Run**) to install and launch **Kinect Pocket** on your iPhone.
   - On first launch on iOS, tap **Allow** when iOS prompts for **Local Network** permission (required for Bonjour discovery and LAN mTLS) and **Camera** permission (only used if scanning the pairing QR code).

---

## How to Pair Your iPhone

1. Launch **KinectViewer** on your Mac and click **Pair iPhone…** in the **iPhone Live Stream (mTLS 1.3)** sidebar panel.
2. A modal sheet displays a **one-time QR code** and an **8-digit short code** (`XXXX-XXXX`), valid for 120 seconds (single-attempt protected).
3. Open **Kinect Pocket** on your iPhone:
   - **Option A (QR Scan)**: Point the iPhone camera at the QR code on the Mac screen. It pairs and starts the live encrypted stream immediately.
   - **Option B (Short Code)**: Type the 8-digit code shown on the Mac and tap **Pair**. Bonjour discovers `_kv2p._tcp.local` on your home Wi-Fi and pins both certificates.
4. Once paired, future launches reconnect automatically over home Wi-Fi using the pinned keys. The Mac host pairs **one** iPhone at a time; pairing a new device immediately invalidates the old one, or click **Unpair** on either device to wipe the pin. (The Windows pet camera pairs several devices, each with a role: see [`docs/pet-camera.md`](docs/pet-camera.md).)

---

## Settings Reference (Mac Host & iOS App)

### View Mode & Pipeline
- **Mode**:
  - **IR Only (`512×424`)**: 16-bit ToF active infrared amplitude tone-mapped to 8-bit for display (streamed 2× nearest-upscaled at `1024×848` so 4:2:0 chroma preserves pixel-sharp detail).
  - **Colour (`1920×1080`)**: 1080p RGB camera decoded via TurboJPEG (`~30 fps` in bright light; sensor auto-drops to `15 fps` in low indoor light).
  - **Depth (`512×424`)**: Millimetre time-of-flight depth rendered in false colour (`1024×848` over stream), gating invalid pixels (`<= 0`, `NaN`, `Inf`) and co-registered saturated IR pixels (`IR == 65535`) strictly to black (`#000000`).
  - **Side by Side**: Choose **IR + Depth (`1024×424`)**, **Colour + Depth (`1280×424`)**, or **Colour + IR (`1280×424`)**. RGB is decoupled from Depth/IR so 15 fps low-light RGB never throttles the 30 fps Depth/IR panel.
- **Depth Pipeline (`OpenGL (gl)` / `OpenCL (cl)` / `CPU (cpu)`)**:
  - Selects the exact `libfreenect2::PacketPipeline`. **Never falls back silently** — if a pipeline fails to initialize, streaming stops immediately and displays the error banner. `OpenGL (gl)` is default and fastest (`30 fps`, `0` drops).

### IR 16-bit → 8-bit Mapping
- **Percentile Auto-Scale (`P1–P99.5`)**: Computes the 1st and 99.5th percentiles of non-zero, unsaturated (`< 65535`) IR pixels each frame so tiny retroreflective glints do not crush room visibility.
- **Gamma (`0.20 – 1.40`, default `0.45`)**: Applies power-law compression `(x)^γ` via a 65,536-entry LUT to compensate for `1 / r²` active-IR falloff.
- **Temporal Smoothing (`EMA α`, default `0.12`)**: Exponentially smooths the auto-scale `[low, high]` bounds across frames (`1.00` = instant/unsmoothed, `0.05` = very slow adaptation) to prevent brightness pumping when someone walks near the sensor.
- **Black / White Sliders**: Fixed manual 16-bit window `[0..65535]` when Percentile Auto-Scale is turned off.

### Depth False Colour
- **Colormap**: **Turbo** (default high-contrast rainbow), **Viridis**, **Inferno**, or **Grayscale** (256-entry perceptually uniform LUTs).
- **Min / Max Distance (`300 – 8000 mm`) & Presets**: Sets the active millimetre window (`Desk 0.4–2.5m`, `Spec 0.5–4.5m` matching Xbox NUI operating range, `Room 0.5–8m`).
- **Near = Warm / Bright**: Maps closer objects to warm red/yellow (Turbo) or bright yellow/white (Viridis/Inferno) and farther surfaces to cool blue/purple.

### Colour Exposure & Gain
- **Auto + Exposure Comp (`-2.0` to `+2.0 EV`)**: Sensor auto-exposure with EV bias.
- **Semi-Auto (Flicker-Free, `4.0 – 66.0 ms`)**: Locks integration time to mains-Safe flicker-free multiples (`8.33 ms` / `16.67 ms` at 60 Hz) while allowing sensor auto-gain.
- **Manual Lock (`Shutter 1.0 – 66.0 ms`, `Analog Gain 1.0 – 4.0×`)**: Locks both integration time and analog gain via `libfreenect2` RGB USB register commands, verified with live hardware register readback.

### Host-Only Recording
- **Depth & IR Only (Colour is Live-Only)**:
  - Colour and Side-by-Side frames stay strictly in memory and are never written to disk (no `.mp4` recordings, snapshots, thumbnails, or caches). In Colour or Side-by-Side mode, the **Start Recording** control is disabled with the tooltip *"Colour is live only. Switch to IR or Depth to record."*
  - The one ruled exception is the Windows pet camera's **camera roll** (off by default): clips kept on that PC and played to its paired devices; see [`docs/pet-camera.md`](docs/pet-camera.md) §6.
  - The Windows pet camera can also be watched **from outside the home** (off by default, turned on on the PC only): a second listener and a router port mapping, straight from that PC with no relay, so it can't work behind carrier NAT; invites: send only to the person you're inviting. See [`docs/pet-camera.md`](docs/pet-camera.md) §7.
- **16-bit Lossless Formats (IR & Depth modes)**:
  - **FFV1 16-bit Lossless (`.mkv`)**: Pipes raw `uint16` `512×424` frames (`0..65535` IR amplitude or `1..65535` integer mm depth with `0 = invalid`) into `ffmpeg -nostdin` (`ffv1 -level 3 -slicecrc 1`) alongside `<mode>_timestamps.csv` (`frame_index, engine_seq, device_ticks, arrival_mach_ns`). Mathematically bit-exact (`max_abs_diff = 0`).
  - **16-bit PNG Sequence**: Native macOS `ImageIO` 16-bit grayscale `.png` per frame + `<mode>_timestamps.csv`.
- **Write Rate (`MB / min`)**: Live disk write rate and cumulative megabytes shown in both the sidebar and top HUD.

### iPhone Live Stream
- **Allow iPhone Mode Switch**: When enabled (default), the iPhone's bottom segmented bar can switch between the 4 live view modes. Turn off to lock the iPhone strictly to whatever mode is selected on the Mac.

---

## Documentation Index

- [`docs/research.md`](docs/research.md) — Graded (`[A]`–`[D]`) technical research with verified primary sources across all 6 topics.
- [`docs/design.md`](docs/design.md) — Portable C++20 core, security/pairing protocol, iOS zero-storage architecture, and host recording design.
- [`docs/test-results.md`](docs/test-results.md) — Every numerical measurement across Phases 1–5 (60s real IR stability, pipeline benchmarks, RGB register readbacks, glass-to-glass latency, lossless `MB/min`, and hardening tests).
- [`docs/windows-setup.md`](docs/windows-setup.md) — The Windows 11 host: what's proven where, the prerequisites in order, building and testing, and running it.
- [`docs/pet-camera.md`](docs/pet-camera.md) — The Windows pet camera: many devices and roles, privacy, sharing, the camera roll and viewing from outside the home.
- [`docs/protocol-changes.md`](docs/protocol-changes.md) and [`docs/protocol-v3-pet-camera.md`](docs/protocol-v3-pet-camera.md) — Wire protocol v2 and v3.
- [`SECURITY.md`](SECURITY.md) — What the hosts protect, how, and the known limits.

---

## Before First Use & Honest Limits

### Needed to run live on a physical iPhone
1. **Install onto your iPhone once from Xcode**: Open `KinectViewer.xcodeproj`, select `KinectPocketViewer` → your iPhone, and hit **Run** (`Cmd+R`). A paid Apple Developer certificate for the team in `Config/Local.xcconfig` signs it (valid up to 1 year, unlike 7-day free provisioning). If iOS has not trusted your development certificate on the phone yet, enable **Developer Mode** under *Settings → Privacy & Security → Developer Mode* on the iPhone.
2. **Grant Local Network Permission on First Launch**: Tap **Allow** on the iOS *Local Network* prompt (and *Camera* if scanning the QR code; the 8-digit short code works without camera access).

### Honest Limits & What Couldn't Be Tested Automatically
1. **Physical Wi-Fi Air-Link & Physical iPhone OLED Scanout**: Automated end-to-end streaming and zero-storage sandbox audits were executed against the real `KinectPocketViewer.app` in the iOS 26.5 `iPhone 17 Pro` Simulator and loopback Network.framework stack (`11.4 – 16.3 ms` median capture-to-decode/enqueue latency), rather than pushing unprompted to your physical iPhone over 5 GHz Wi-Fi. Adding typical home 5 GHz Wi-Fi single-hop RTT (`2–8 ms`) + 120 Hz ProMotion scanout (`~8.3 ms`) projects **`~25–35 ms`** physical glass-to-glass latency (well under the `150 ms` target), but only a person can visually confirm the live picture on an iPhone.
2. **Physical Cable Pull & Physical Lid-Close Sleep**: Because these checks need someone to physically pull the USB 3.0 plug or close the MacBook lid, `IONotificationPort` (`0x045e:0x02c4` hotplug) and `NSWorkspace` (`willSleep` / `didWake`) handlers were wired in `HardwareLifecycleMonitor.swift` and verified via programmatic real-hardware stop → lock release → reopen cycles on the physical Kinect v2.
3. **White Balance Controls**: `libfreenect2`'s public API has methods for auto, flicker-free semi-auto, and manual shutter/analog-gain exposure (all exposed in the UI). It has none for white balance: only raw colour-setting codes, whose mode values and gain ranges its header doesn't document. So manual white balance isn't offered, and the driver wasn't patched for it (the driver is a thin dependency; `design.md`).
