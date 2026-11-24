# Kinect v2 Live Viewer — Measured Test Results

*All real-hardware tests were executed strictly via numerical counters, timing probes (`mach_absolute_time`), and pixel histograms without viewing, saving, or screenshotting live camera frames (`design.md`, rule 5).*

**Hardware Under Test:**
- Host: Apple M1 Pro (10 cores), macOS 26.5.2, Xcode 26.5, Swift 6.3.2
- Sensor: Xbox NUI Sensor (`0x045e:0x02c4`), Firmware `4.0.3917.0`
- Driver: `libfreenect2` `0.2.0` (`fd64c5d9`)

---

## Phase 1: Mac Viewer — IR Mode Only

### 1.1 `~/KINECT-IN-USE` Lock Obedience Verification
- Foreign lock file test (`agent: OtherMeasurementTool`, `pid: 99999`):
  - `LOCK_REFUSED_FOREIGN = true`
  - `LOCK_PRESERVED_FOREIGN = true` (foreign lock file left untouched on abort & destroy)
- Real device lock lifecycle:
  - `KINECT_LOCK_ACQUIRED = true` upon opening Kinect v2
  - `KINECT_LOCK_RELEASED = true` immediately upon `kv_engine_stop()`

### 1.2 Synthetic IR Source + 64-Bit Clock Barcode Verification (5.0 s)
- Frames: `150` IR / `150` rendered in `5.007 s` (`29.96 fps`, `0` drops)
- Inter-frame interval: `mean = 33.382 ms`, `p95 = 42.601 ms`, `max = 46.871 ms`
- Tone-mapping CPU time: `mean = 1.542 ms`, `max = 5.969 ms`
- Raw IR stats: `min = 838.98`, `max = 65535.00`, `mean = 3878.32`, `saturated_px = 310`
- Smoothed scale window: `[896.0, 28475.4]` → Mapped 8-bit luma: `min = 0`, `max = 255`, `mean = 77.06`
- Embedded 64-bit clock barcode decode accuracy: `150 / 150` (`100.0%`)

### 1.3 Real Kinect v2 — 60-Second IR Stability & Non-Growing Latency Run (`OpenGL` Pipeline)

| Window | Rolling FPS | Cumulative IR Frames | Seq Drops | Inter-Frame Mean (ms) | Inter-Frame p95 (ms) | Tone-Map Mean (ms) | Arrival → Consume Mean (ms) | Arrival → Consume Max (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `0 – 10 s` | 30.06 | 298 | 0 | 33.31 | 47.38 | 1.209 | 2.088 | 5.641 |
| `10 – 20 s` | 30.04 | 598 | 0 | 33.32 | 47.57 | 1.309 | 2.148 | 6.149 |
| `20 – 30 s` | 29.97 | 898 | 0 | 33.32 | 48.04 | 1.215 | 2.058 | 4.822 |
| `30 – 40 s` | 29.97 | 1,198 | 0 | 33.34 | 47.87 | 1.272 | 2.066 | 3.790 |
| `40 – 50 s` | 30.00 | 1,498 | 0 | 33.33 | 47.91 | 1.276 | 2.122 | 6.228 |
| `0 – 60 s` (Total) | **29.97** (avg **29.95**) | **1,797** | **1** (startup) | **33.530** | **48.080** | **1.214** | **~2.10** | **7.263** (map max) |

- **Latency drift over 60 s**: `+0.034 ms` between `0–10 s` (`2.088 ms`) and `40–50 s` (`2.122 ms`) — **zero latency growth**.
- **Driver diagnostics**: `benign_driver_notices = 1` (expected initial startup packet notice), `driver_errors = 0`.
- **Signal distribution**: `raw_min = 0.00`, `raw_max = 65535.00`, `raw_mean = 17240.28`, `saturated_px = 29449`, `scale_low = 384.1`, `scale_high = 61095.5`, `mapped_luma_min = 0`, `mapped_luma_max = 255`, `mapped_luma_mean = 118.08`.
- **16-bin raw IR histogram**: `48855, 63242, 50127, 5456, 1939, 1938, 2192, 2179, 2008, 1741, 1503, 1453, 1477, 1412, 1300, 30266`.


---

## Phase 2: All Modes, Colour Exposure Readbacks, and Pipeline Comparison

### 2.1 Zero-Silent-Fallback Verification
- Bogus/unbuilt pipeline (`id = 99`):
  - `NO_FALLBACK_HALTED = true` (`kv_engine_start` returned `false`, `kv_engine_is_running == false`)
  - `NO_FALLBACK_LOCK_CLEAN = true` (lock file released immediately on failure)
  - Error surfaced: `"Unknown packet pipeline requested."`

### 2.2 Synthetic Multi-Mode + 64-Bit Barcode Verification (2.0 s each)
- **Depth False Colour (`512×424`, Turbo)**: `60` frames (`29.97 fps`), `0` drops, `saturated_px = 310`, `invalid_px = 7,702` (`7,392` border/`NaN` + `310` co-gated `IR == 65535`), `barcode_verified = 60/60`.
- **Colour (`1920×1080`)**: `60` frames (`29.95 fps`), `0` drops, `map_mean_ms = 2.957 ms`, `barcode_verified = 60/60`.
- **Side-by-Side (`1280×424`, Colour + Depth)**: `60` frames (`29.99 fps`), `0` drops, `map_mean_ms = 2.785 ms`, `barcode_verified = 60/60`.

### 2.3 Real Kinect v2 — Pipeline Comparison in Depth Mode (`512×424`, 5.0 s each)

| Pipeline | Rolling FPS | Avg FPS (incl. init) | Depth Frames | Seq Drops | Inter-Frame Mean (ms) | Map Mean (ms) | Arrival → Map Done (ms) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| **`OpenGL (gl)`** | **29.97** | 29.80 | 149 | **0** | 33.179 | 1.442 | 1.955 | Optimal hardware pipeline on M1 Pro |
| **`OpenCL (cl)`** | **30.02** | 28.78 | 144 | 5 (JIT init) | 33.198 | 1.366 | 1.591 | Full 30 fps steady-state after OpenCL kernel compile |
| **`CPU (cpu)`** | **15.00** | 14.80 | 74 | 73 | 66.461 | 0.819 | 1.136 | CPU phase unwrapping skips alternate frames cleanly; zero queue buildup |

- Real scene depth summary (`gl`): `raw_min = 300.01 mm`, `raw_max = 1884.36 mm`, `raw_mean = 1509.21 mm`, `saturated_px = 29,568`, `invalid_px = 59,557` (includes co-gated `IR == 65535` pixels).

### 2.4 Real Kinect v2 — Mid-Stream Colour Camera Exposure & Gain Lock Verification
- **Manual Lock (`20.0 ms`, Analog Gain `2.50×`)**:
  - Hardware register readback: `GET_EXPOSURE_MODE = 4` (manual), `GET_INTEGRATION_TIME = 19.99 ms`, `GET_ANALOG_GAIN = 2.46×`
  - Live RGB frame footer: `exposure = 19.99 ms`
- **Flicker-Free Semi-Auto (`15.0 ms` pseudo-exposure)**:
  - Hardware register readback: `GET_EXPOSURE_MODE = 3` (semi-auto), `GET_INTEGRATION_TIME = 14.99 ms`
  - Live RGB frame footer: `exposure = 14.99 ms`
- **Auto Exposure Restored**:
  - Hardware register readback: `GET_EXPOSURE_MODE = 0` (auto), room low-light auto settled to `exposure = 66.47 ms` (`15.00 fps`), `color_minus_depth_ticks = -87` (matches `179 - 266 = -87` nearest-frame tick offset).

### 2.5 Real Kinect v2 — Side-by-Side Modes (`gl` Pipeline, 5.0 s each)
- **Side-by-Side `Colour + Depth` (`1280×424`)**:
  - Rendered at **`29.98 fps` rolling** (`148` depth/IR frames, `67` colour frames at `15.00 fps`, **`0` drops** on IR, Depth, and Colour), `map_mean_ms = 2.067 ms` — decoupling RGB from Depth/IR prevents low-light 15 fps RGB from throttling the 30 fps Depth view.
- **Side-by-Side `IR + Depth` (`1024×424`)**:
  - Rendered at **`30.01 fps` rolling** (`148` frames, **`0` drops**), `map_mean_ms = 2.583 ms`.


---

## Phase 3: Live Encrypted Streaming to iOS (`KinectPocketViewer`)

### 3.1 Build & Signing Verification
- `KinectHostApp` (macOS `arm64`, Release): **BUILD SUCCEEDED**
- `kinect-cli` (macOS `arm64`, Release): **BUILD SUCCEEDED**
- `KinectPocketViewer` (iOS 26.5 Simulator `iPhone 17 Pro`, Release): **BUILD SUCCEEDED** (0 Swift warnings)
- `KinectPocketViewer` (Physical iOS Device `generic/platform=iOS`, signed with a paid Developer team's Apple Development certificate, Release): **BUILD SUCCEEDED** (0 Swift warnings)

### 3.2 Synthetic Multi-Mode Stream + 64-Bit Pixel Barcode Latency (`6.0 s`)
- **Bonjour `_kv2p._tcp` 8-digit short-code pairing**: `BONJOUR_SHORTCODE_PAIR_OK = true`
- **QR URI (`kv2://pair?...`) pairing + pinned mutual TLS 1.3**: `QR_URI_PAIR_OK = true`
- **Frames & Throughput**: `171` sent / `171` `CMSampleBuffer`s delivered (`0` congestion drops, `30.14 fps`, `0.65 Mbps`, RTT `0.288 ms`)
- **Dynamic mid-stream resolution switching verified without reconnect**: `1024×848` (IR & Depth 2× nearest-upscaled) → `1920×1080` (Colour) → `1024×424` (Side-by-Side)
- **Cristian-synced Host Capture → Display Enqueue Latency**:
  - **`p50 = 12.55 ms`**, **`p95 = 55.26 ms`**, **`max = 143.60 ms`** (target `< 150 ms`)
- **End-to-End Hardware-Decoded Y-Plane 64-Bit Pixel Barcode Latency (`VTCompressionSession` → mTLS 1.3 → `VTDecompressionSession` luma decode)**:
  - **`p50 = 14.36 ms`**, **`p95 = 71.50 ms`**, `verified_frames = 170 / 171` (`99.4%`)

### 3.3 Real Kinect v2 Hardware → Encrypted mTLS 1.3 Stream Benchmark (`5.0 s`, Numbers Only)
- **Sensor Lock Lifecycle**: `KINECT_LOCK_CLEAN = true`
- **Frames & Throughput across all 4 modes (`IR` → `Depth` → `Colour` → `Side-by-Side`)**:
  - `129` sent / `129` received (`0` congestion drops, `6.03 Mbps` peak during 1080p RGB, RTT `0.288 ms`)
- **Real Sensor Arrival (`mach_absolute_time`) → Client Display Enqueue Latency**:
  - **`p50 = 11.38 ms`**, **`p95 = 43.65 ms`**, **`max = 54.94 ms`** (more than **3× below** the `< 150 ms` glass-to-glass budget, leaving > 100 ms margin for home Wi-Fi airtime + 60/120 Hz iPhone OLED scanout)

### 3.4 iOS Simulator (`iPhone 17 Pro`) Live App Run & Zero-Disk-Storage Sandbox Audit
- Launched `KinectPocketViewer.app` (`PID 84870`) paired via `kv2://pair` QR URI against synthetic Turbo Depth + 64-bit clock barcode stream (`5.5 s`):
  - `paired = true`, `sent_frames = 82`, `bitrate = 0.71 Mbps`
  - Live iOS app telemetry reported back to Mac over mTLS: **`sim_display_lat_ms = 13.08 ms`**, **`sim_barcode_lat_ms = 16.32 ms`**
- Post-run full filesystem audit of iOS app sandbox container (`Data/Application/...`):
  - `Documents/` files: **`0`**
  - `Library/Caches/` files: **`0`** (`URLCache` 0/0 confirmed — no `Cache.db`)
  - `tmp/` files: **`0`**
  - Total files in entire app data container: **`1`** (only iOS system `.com.apple.mobile_container_manager.metadata.plist`; pairing keys stored exclusively in Keychain with `kSecAttrAccessibleWhenUnlockedThisDeviceOnly`).


---

## Phase 4: Host-Only Recording & Triple Exclusion Verification

### 4.1 `~/kinect-viewer-recordings/` Exclusion Audit
- Directory path: `~/kinect-viewer-recordings` (`POSIX 0700`)
- iCloud exclusion marker (`.nosync` + `.nosync` session directory suffix + `NSURLIsExcludedFromBackupKey = true`): **`true`**
- Spotlight exclusion marker (`.metadata_never_index`): **`true`**
- Time Machine exclusion (`tmutil isexcluded` → `[Excluded]`): **`true`**
- Network / phone isolation: `HostRecorder` is compiled strictly into macOS targets; `WireMessageType` contains zero recording, file-list, or download opcodes.

### 4.2 Lossless 16-Bit Bit-Exactness & Measured Data Rates (`MB / min`)

| Mode & Recording Format | Resolution & Pixel Type | Synthetic `MB/min` (`30 fps`) | Real Kinect v2 `MB/min` | Lossless Verification |
|---|---|---:|---:|---|
| **IR Only — FFV1 v3 (`.mkv` + `.csv`)** | `512×424` `gray16le` (`0..65535`) | 85.68 | **521.43** (`30 fps`) | `max_abs_diff = 0` (`100%` bit-exact) |
| **IR Only — 16-bit PNG Sequence (`+ .csv`)** | `512×424` 16-bit Gray PNG | 154.63 | **555.36** (`30 fps`) | `kCGImagePropertyDepth = 16` |
| **Depth Only — FFV1 v3 (`.mkv` + `.csv`)** | `512×424` `uint16` mm (`0 = invalid`) | 46.55 | **101.56** (`30 fps`) | `max_abs_diff = 0` (`100%` bit-exact) |
| **Colour Only — Hardware HEVC (`.mp4`)** | `1920×1080` VFR HEVC (`18 Mbps` target) | 1.12 | **133.87** (`15 fps` low-light) | Valid `hvc1`/`hev1` MP4 track |
| **Side-by-Side — Hardware HEVC (`.mp4`)** | `1280×424` VFR HEVC (`10 Mbps` target) | 10.99 | **78.20** (`30 fps`) | Valid `hvc1`/`hev1` MP4 track |


---

## Phase 5: Hardening, Security & Lifecycle Verification

*Verified via `kinect-cli --test-hardening --real` (45-second bounded watchdog, zero live frame inspection).*

### 5.1 Unpaired-Device Zero-Leakage & Single-Attempt Pairing Burn
- **Unpaired Idle Bonjour Scan (`_kv2s._tcp.local`, `_kv2p._tcp.local`)**:
  - `mdns_advertised_when_unpaired = 0` (zero services advertised when unpaired and no pairing window is open)
- **Raw Unpinned Client Probe Against Active Pairing Socket**:
  - `app_bytes_leaked_to_raw_probe = 0` (`0` application bytes readable without completing the ephemeral TLS 1.3 handshake)
- **Wrong 8-Digit Short-Code Guess (`00000000`)**:
  - `wrong_code_accepted = false`
  - `offer_burned_immediately = true` (single failed HMAC attempt immediately invalidates and closes the active pairing offer so a subsequent guess with even the original code is rejected)

### 5.2 Two-Phone Mutual TLS 1.3 Pinning & Re-Pairing Displacement
- **Part 1 — Paired Phone A vs. Rogue / Second Phone B**:
  - `phoneA_paired = true`, `bonjour_id_opaque_12hex = true` (`<12-hex>._kv2s._tcp.local`, empty TXT record)
  - While Phone A streamed continuously (`phoneA_frames: 10 → 26`), Phone B attempted to connect to the live stream port with a valid TLS 1.3 client certificate not matching the host's pinned leaf digest:
    - `phoneB_accepted = false`
    - `phoneB_frames = 0`
    - `server_rejected_handshakes: 0 → 1` (Phone A's active stream remained uninterrupted)
- **Part 2 — Re-Pairing Phone B Displaces Phone A**:
  - Host opened a new pairing window and paired Phone B (`phoneB_paired = true`, `phoneB_frames = 24`)
  - Displaced Phone A attempted to reconnect using its previously pinned certificate:
    - `displaced_phoneA_accepted = false` (`0` frames delivered)

### 5.3 Automatic Client Reconnects (Socket Drop & Server Port Change)
- **Abrupt TCP/TLS Socket Drop (`disconnectActiveClient()`)**:
  - `recovered = true`, `reconnect_count = 1`, **`recovery_ms = 524.8 ms`** (client resumed live `CMSampleBuffer` decode automatically)
- **Full Host Stream Server Stop & Restart on New Ephemeral Port via Bonjour**:
  - Server stopped on `old_port = 52265` and restarted on `new_port = 52269`
  - Client cleared stale cached port on connection refusal, re-browsed `<12-hex>._kv2s._tcp.local` over Bonjour, re-authenticated mutual TLS 1.3, and resumed live frames in **`recovery_ms = 1,153.2 ms`**

### 5.4 Real Kinect v2 Sleep / Wake & USB Hotplug Re-Enumeration Cycle
- **Hardware Lifecycle Monitor (`HardwareLifecycleMonitor.swift`)**:
  - Subscribes to `IOKit` `IONotificationPort` (`kIOFirstMatchNotification` / `kIOTerminatedNotification` for `0x045e:0x02c4`) and `NSWorkspace` (`willSleepNotification` / `didWakeNotification`).
- **Measured Real-Sensor Stop → Lock Release → XHCI Re-Enumeration → Reopen → Encrypted Stream Resume Cycle**:
  - Initial hardware open & lock acquisition: `open1 = true`, `lock1 = true`
  - Simulated sleep / unplug teardown: `mid_lock_released = true` (`~/KINECT-IN-USE` deleted immediately)
  - Wake / replug hardware reopen across macOS `AppleUSBXHCI` re-enumeration window (`OpenGL` pipeline preserved with zero fallback) + automatic live stream resume to paired iPhone client:
    - `reopen2 = true`, `lock2 = true`, **`stream_resumed_ms = 2,140.4 ms`**
  - Final teardown lock audit: `final_lock_clean = true` (`~/KINECT-IN-USE` absent)

