# Windows host: setup, build and run

The Windows host is a port of the Mac host to Windows 11 (MSVC). The portable core
(`core/`) builds unchanged apart from small `#ifdef _WIN32` blocks; everything under
`windows/` is new C++ code, because the Mac's CLI, UI and streaming layers are Swift.

## Status: what has been proven where

| Piece | Proven on | How |
|---|---|---|
| Core + lock guard + SetupDi presence check | PC, **synthetic only** | `ctest` (lock tests, synthetic core tests) |
| `kinect-cli` run / bench / verify-lock / list | PC, **synthetic only** | CTest + manual runs below |
| `kinect-viewer.exe` (Win32 + D3D11), all four modes | PC, **synthetic only** | CTest screenshot hook (PrintWindow) |
| Unplug/replug and sleep/wake restart in the viewer | **not run** | needs someone to unplug the sensor / sleep the PC |
| Real Kinect v2 on a Windows 11 PC | **runs, but depth/IR lost about half their frames there** | 2026-11-11 results below |
| iPhone streaming host (W3) | PC loopback, **synthetic + fake phone**; real iPhone **not run** | CTest `stream_e2e_loopback`, `protocol_golden` |
| Protocol v2 host (W5 stage 1) | PC loopback, **synthetic + v1 and v2 fake phones**; real iPhone **not run** | CTest `stream_e2e_loopback` (`v2_*`), `host_power`, `protocol_golden` |
| Host recorder (W4; colour removed in W5 stage 2) | PC, **synthetic only** | CTest `recorder` (FFV1 bit-exact, PNG16 exact, colour refused), `cli_record_refuses_*` |
| 3D point cloud, cut-off, blend (W5 stage 5) | PC, **synthetic only**; real sensor and iPhone **not run** | CTest `point_cloud` (camera, renderer, cut-off, 30 fps), `viewer_3d_*`, refusals |
| Second display + B black-out, app icon (W5 stage 6) | PC, **synthetic only**, as a small test window; a real projector **not run** | CTest `viewer_second_display_screenshot`, `app_icon`, `inspection` |
| Pet camera W6: many devices, roles, privacy, sharing, the panel (protocol v3) | PC loopback, **synthetic + fake v1/v2/v3 devices**; real iPhone, Mac and second phone **not run** | CTest `stream_e2e_loopback` (`v3_*`), `device_store`, `live_tiers`, `viewer_pet_camera_panel`, `viewer_test_launch_isolated_state`, `viewer_model` |

## Real-sensor results on a Windows 11 PC (2026-11-11, libusbK, firmware 4.0.3917.0)

The Kinect enumerates under libfreenect2 (`kinect-cli list`: 1 device). On that PC its path was: Xbox NUI Sensor
(libusbK), then a generic SuperSpeed USB hub, then **a USB 3 controller on the motherboard chipset** (behind a PCIe
switch), not one of the CPU's own USB controllers.

| Run | Depth/IR fps | Depth/IR sequence drops | Colour fps / drops | Driver errors |
|---|---|---|---|---|
| `bench --pipeline gl` (60 s, IR) | **12.82** (rolling 7.6 to 21) | 1029 IR + 1029 depth over 769 frames | not on | 0 errors, 45 "packets were lost" notices |
| `bench --pipeline cpu` (60 s, IR) | **6.98** | 1372 + 1372 over 419 frames | not on | 0 errors, 46 notices |
| `run --mode color` (20 s, gl) | 15.5 (311 frames) | 285 + 285 | **30.0 fps, 0 drops** | 0 |
| `run --mode sbs --sbs-layout color-depth` (20 s) | 16.6 | 264 + 264 | 30.0 fps, 16 drops | 0 |
| Viewer IR / Colour / Depth / SBS IR+Depth / SBS Colour+IR (8 s each, gl) | 16 / (colour 29) / 17 / 16 / 19 | 194 to 216 per run | colour 29 to 30 fps | 0 |

Protonect (`gl -noviewer -norgb -frames 300`) shows the cause verbatim and nothing else: 13 messages like
`[Info] [DepthPacketStreamParser] 30 packets were lost` (300 packets in total), no libusb errors or
warnings, and the OpenGL depth decoder at ~280-300 Hz. So the GPU and decode are not the bottleneck: the depth
stream (isochronous USB) loses packets on that controller/hub path, while the colour stream (bulk USB) is clean.

If depth/IR drop frames like this, try a rear-panel USB 3 port wired to the CPU (not the chipset), with no hub in
between, and re-run `kinect-cli bench --pipeline gl`. libfreenect2's IR transfer-pool sizes can also be tuned
(`LIBFREENECT2_IR_PACKETS`, `LIBFREENECT2_IR_TRANSFERS`); that's a USB transfer setting, not tried for these results.

## Prerequisites

In this order. Everything goes in one folder that also holds the checkout (`C:\src` below, but any folder works):
`build.cmd` looks for libfreenect2 and vcpkg beside the checkout. To keep them somewhere else, set the variables
under [Build and test](#build-and-test). [What the build needs](#what-the-build-needs) lists the versions.

1. **Tools:** Visual Studio 18 2026 Build Tools with the C++ workload (MSVC 19.50), CMake 4.x, Ninja and Git, the
   last three on `PATH` (for example `winget install --id Kitware.CMake`, `--id Ninja-build.Ninja` and
   `--id Git.Git`). The tests use Git too.
2. **This repository**, cloned (or its source snapshot unpacked) into `C:\src\kinect-viewer`.
3. **libfreenect2's three dependencies**, as their prebuilt Windows x64 releases, each unpacked under `C:\src\deps`:
   - libusb 1.0.30 (github.com/libusb/libusb releases) into `deps\libusb`, which then holds `include\` and
     `VS2022\MS64\dll\`;
   - libjpeg-turbo 3.2.1, the Visual C++ x64 build (github.com/libjpeg-turbo/libjpeg-turbo releases), into
     `deps\turbojpeg`, which then holds `include\`, `lib\turbojpeg.lib` and `bin\turbojpeg.dll`;
   - GLFW 3.5.1, the 64-bit Windows binaries (glfw.org), into `deps\glfw`, which then holds
     `glfw-3.5.1.bin.WIN64\include\` and `glfw-3.5.1.bin.WIN64\lib-vc2022\`.
4. **libfreenect2** at upstream `fd64c5d9b214df6f6a55b4419357e51083f15d93`, built and installed to
   `C:\src\libfreenect2\install`, with the OpenGL and CPU pipelines and no OpenCL or CUDA:

   ```bat
   cd /d C:\src
   git clone https://github.com/OpenKinect/libfreenect2.git libfreenect2
   git -C libfreenect2 checkout fd64c5d9b214df6f6a55b4419357e51083f15d93
   set D=C:/src/deps
   set G=%D%/glfw/glfw-3.5.1.bin.WIN64
   cmake -S libfreenect2 -B build-fn2 -G "Visual Studio 18 2026" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
     -DBUILD_SHARED_LIBS=ON -DENABLE_OPENGL=ON -DENABLE_OPENCL=OFF -DENABLE_CUDA=OFF ^
     -DCMAKE_INSTALL_PREFIX=C:/src/libfreenect2/install ^
     -DLibUSB_INCLUDE_DIRS=%D%/libusb/include -DLibUSB_LIBRARIES=%D%/libusb/VS2022/MS64/dll/libusb-1.0.lib ^
     -DLibUSB_DLL=%D%/libusb/VS2022/MS64/dll/libusb-1.0.dll ^
     -DTurboJPEG_INCLUDE_DIRS=%D%/turbojpeg/include -DTurboJPEG_LIBRARIES=%D%/turbojpeg/lib/turbojpeg.lib ^
     -DTurboJPEG_DLL=%D%/turbojpeg/bin/turbojpeg.dll ^
     -DGLFW3_INCLUDE_DIRS=%G%/include -DGLFW3_LIBRARIES=%G%/lib-vc2022/glfw3dll.lib -DGLFW3_DLL=%G%/lib-vc2022/glfw3.dll
   cmake --build build-fn2 --config RelWithDebInfo --target install
   ```

   The install's `bin\` then holds `freenect2.dll`, `libusb-1.0.dll`, `turbojpeg.dll` and `glfw3.dll`. The build
   links against libfreenect2 and copies those four next to each exe.
5. **vcpkg** at the baseline `windows\vcpkg.json` names, into `C:\src\vcpkg`:

   ```bat
   cd /d C:\src
   git clone https://github.com/microsoft/vcpkg.git vcpkg
   git -C vcpkg checkout b58529e33332fbb6dfe20317a82009b78ceeab36
   vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```

   The first build installs OpenSSL and the QR code generator into `C:\src\vcpkg-installed\kinect-viewer`.
   OpenSSL builds from source, so that first build takes a while.
6. **FFmpeg**, for the recorder and its tests (the recording rows fail without it):
   `winget install --id Gyan.FFmpeg --scope user`. The host finds it in winget's Links folder, or set `KV_FFMPEG`
   to `ffmpeg.exe`'s full path.

## Build and test

```bat
cd /d C:\src\kinect-viewer
windows\scripts\build.cmd test
```

`build.cmd configure` makes the build folder and stops, without building. The script finds Visual Studio
with vswhere and uses 4 parallel jobs. If libfreenect2 or vcpkg isn't where it
looks, it stops and says which, before anything is configured. vcpkg is required: without it there's no OpenSSL,
and so no iPhone streaming host, and the script no longer goes on to build the rest without it (a plain CMake
configure still does, with a warning). Output goes to `build\windows\` in the repo
(git-ignored). These variables change where it looks (`..` is the folder that holds the checkout):

| Variable | Default |
|---|---|
| `FREENECT2_PREFIX` | `..\libfreenect2\install` |
| `KV_VCPKG_ROOT` | `..\vcpkg` |
| `KV_VCPKG_INSTALLED` | `..\vcpkg-installed\kinect-viewer` |
| `KV_BUILD_DIR` | `build\windows` |

## `kinect-cli.exe`

```bat
set CLI=build\windows\windows\kinect-cli.exe
%CLI% list                                         & rem presence (SetupDi), lock, libfreenect2 devices
%CLI% run --synthetic --duration 5 --verify-barcode
%CLI% bench --synthetic --duration 20 --verify-barcode
%CLI% verify-lock                                  & rem lock guard self-test, temp file, no device opened
%CLI% bench --pipeline gl                          & rem 60 s real-sensor benchmark (needs the Kinect)
%CLI% bench --pipeline cpu --json %USERPROFILE%\kinect-bench-cpu.json
%CLI% run --mode color --test-exposure --duration 10
```

`bench` exits 0 on PASS (depth/IR at least 29 fps over the run, zero sequence drops, zero
driver errors), 4 on FAIL, 3 if `%USERPROFILE%\KINECT-IN-USE` is held by someone else and
5 if no Kinect is on the bus. The serial number is hidden unless `--show-serial` is given.

## `kinect-viewer.exe`

```bat
set VIEWER=build\windows\windows\kinect-viewer.exe
%VIEWER% --synthetic                     & rem synthetic test pattern, starts streaming at once
%VIEWER%                                 & rem real Kinect v2, OpenGL pipeline, autostart
%VIEWER% --no-autostart --pipeline cpu   & rem pick settings first, then press Start
```

Win32 + Direct3D 11 (no WinUI). The left panel mirrors the Mac sidebar:

- Source (Kinect v2 or synthetic) and pipeline (`gl`, `cpu`; CUDA comes in a later step), plus Start and Stop.
- Mode: IR, Colour, Depth false colour, Side by Side (IR + Depth, Colour + Depth, Colour + IR), 3D point
  cloud, and the synthetic clock-barcode overlay. Then the 3D controls, the depth cut-off and the
  depth/colour blend (stage 5, below).
- IR mapping: percentile auto-scale, gamma 0.20-1.40, EMA alpha, black and white points, reset.
- Depth: Turbo, Viridis, Inferno or Grayscale; min and max distance; Desk, Spec and Room presets;
  near = warm.
- Colour exposure, the controls libfreenect2 exposes: auto with EV compensation, semi-auto
  (flicker-free pseudo exposure time), and manual shutter + analog gain.

Choosing a colour mode while the engine runs without the RGB stream restarts it with RGB on, as the
Mac does. A pipeline never falls back silently: a failed start shows the error in the status line.

The window restarts streaming when the Kinect is plugged back in (`WM_DEVICECHANGE`) and after
sleep (`WM_POWERBROADCAST`). Both are untested with a real sensor.

Test hook, used by CTest: `--screenshot PATH --screenshot-after-ms N --exit-after-screenshot`
captures the window with `PrintWindow(PW_RENDERFULLCONTENT)` and writes `PATH.txt` with frame stats.
It never synthesises input, and it refuses the real source, any view with colour, and a depth/IR replay on
screen. `--state-dir DIR` keeps the streaming host's state in DIR (see the pet camera below). Further flags: `--mode` (`3d` too), `--sbs-layout`, `--colormap`, `--clock-overlay`, `--point-color depth|ir|rgb`, `--point-size PX`, `--track-me`, `--blend PERCENT`.

## Real sensor (not run yet)

1. Plug the Kinect v2 into a USB 3 port. Swap its driver to **libusbK** with Zadig
   (a person does this, never a script; never UsbDk, which is reported to stop Windows 11 24H2+ booting).
2. `%CLI% list` must show `kinect_usb_present=yes` and `libfreenect2_devices=1`.
3. `%CLI% bench --pipeline gl`, then `%CLI% bench --pipeline cpu`.
4. `%VIEWER%`, then try each mode, Side by Side layout and colour exposure mode.

Microsoft's own SDK has been seen to find the Kinect on a chipset USB controller and get 0 frames, so whether
libfreenect2 streams on a given PC is unproven until step 3 is run there.

## Lock file

`%USERPROFILE%\KINECT-IN-USE` is created atomically with `CreateFileW(CREATE_NEW)`. A lock
whose `pid:` names a process that no longer exists (`OpenProcess` fails with
`ERROR_INVALID_PARAMETER`, or `GetExitCodeProcess` reports it has exited) is stale and is
reclaimed; a lock without a pid, or whose process cannot be queried, is treated as live.

## What the build needs

| What | Version | Source | Size | For |
|---|---|---|---|---|
| vcpkg (tool, shallow git clone + bootstrap) | commit b58529e, vcpkg.exe 2026-10-29 | github.com/microsoft/vcpkg | ~55 MB clone + 6.8 MB exe, at `KV_VCPKG_ROOT` | W3 deps |
| OpenSSL (vcpkg, x64-windows-static-md, built from source) | 3.6.5#2 | vcpkg port `openssl` | static libs in `KV_VCPKG_INSTALLED` (build tools such as perl/nasm are downloaded into vcpkg's `downloads`) | W3 TLS 1.3 |
| nayuki-qr-code-generator (vcpkg) | 1.8.0#1 | vcpkg port | small static lib, same folder | W3 pairing QR |
| FFmpeg (winget `Gyan.FFmpeg`, user scope, portable zip, GPL-3.0) | 9.0.2 full_build | github.com/GyanD/codexffmpeg | 665 MB under `%LOCALAPPDATA%\Microsoft\WinGet\Packages` | W4 FFV1 recording, test decode |

Nothing CUDA-related is installed or built.

## iPhone streaming (W3, not run with the iPhone yet)

```bat
%VIEWER%                                         & rem then Pair a device... (QR; "Use a code instead" adds the 8-digit code; 120 s, one try)
%CLI% serve --clock-overlay --pair --pair-role owner                 & rem headless host on the Kinect
%CLI% serve --synthetic --clock-overlay --pair --state-dir %USERPROFILE%\kinect-viewer\.host-state
```

Pairing state lives in `%USERPROFILE%\kinect-viewer\.host-state\pairing.json` (owner-only ACL, with the PC's
private key wrapped by DPAPI for this PC; `docs/pet-camera.md` §5), with the device list in `devices.json` next
to it (W6, below). A `--synthetic` or `--loopback-only` serve keeps its state
in a temp folder that is deleted when it stops, unless `--state-dir` names one (as on the last line). The first
run that listens on the LAN raises a Windows Firewall prompt: allow it on **Private** networks, and make
sure the Wi-Fi/Ethernet profile is Private, otherwise the iPhone can't reach the host or see its mDNS.

**Protocol v2 (W5 stage 1).** The host speaks v2 (`docs/protocol-changes.md`): the new iPhone app gets
its Kinect On/Off switch and remote sheet; the old app still works as before. Under "iPhone live stream":
- **Allow iPhone mode switch** (as before) also gates the remote sheet's commands.
- **Allow iPhone to turn the Kinect on and off** (on by default). Off: the phone's switch is disabled and
  says why.
- **Turn the Kinect off when the iPhone disconnects** (off by default). A phone that sends nothing for
  5 s counts as gone.

Start/Stop here and the phone's switch are the same request, so they stay in sync. Off finishes a
recording, stops the video, closes the Kinect and removes `KINECT-IN-USE`; the phone stays connected.
From stage 5 the phone can pick the 3D mode, orbit it, and set the cut-off (Track me) too.

## Inspection tools (W5 stage 3, the Mac's phase 1)

- **Hover** the image: the line under it shows the pixel, the depth in mm and the raw IR value (a dash off the
  depth image, e.g. the colour half of a Side by Side view).
- **Drag a box** for region stats: mean, median, standard deviation, min and max depth, and the share of
  invalid (0) pixels. When zoomed in, a drag pans and **Shift-drag** draws the box. **Esc** clears.
- **Click two points** for the 3D ruler, through libfreenect2's IR camera parameters (never fixed values):
  *Intrinsics unavailable* without them, *no depth here* if an end has none.
- **Space** freezes the shown frame (in memory only); the tools work on it, and the phone sees it frozen.
  **Scroll** zooms around the cursor, **drag** (or right-drag) pans, **double-click** resets.
- **H** toggles the health display above the image (fps per stream, drops per minute, frame-to-frame p50/p95,
  depth processing, driver errors, USB location). Over 2% of depth or IR frames dropped in 10 s shows a red
  banner: *Frames are dropping. Try a different USB port, ideally a rear port directly on the motherboard.*
- **Presets**: Desk, Room and Dark room IR (the Mac's values), plus your own (Save as, Delete). The last-used
  settings come back at the next launch; launch flags still win. Kept in
  `%USERPROFILE%\kinect-viewer\.host-state\presets.json`.
- **Keys**: 1 IR, 2 Colour, 3 Depth, 4 Side by Side, 5 3D point cloud, F full screen, M mirror,
  R rotate 180 degrees, ? the shortcut list. The control panel scrolls.

## 3D point cloud, cut-off and blend (W5 stage 5, the Mac's phase 3)

- **3D point cloud** (mode 5, or **5**): every depth pixel is a point, placed with the depth camera's own
  intrinsics, `x = (col + 0.5 - cx) / fx * z`, `y = -(row + 0.5 - cy) / fy * z` (phase 7). Without
  intrinsics (an older recording) nothing is drawn and the line under the view says why: no made-up values.
  Rendered with Direct3D 11 at 960x720 on black, then shown (and sent to the phone) as a picture.
  **Drag** orbits, **Shift-drag** or right-drag pans, **scroll** zooms, **double-click** (or *Reset 3D view*)
  resets; the phone's gestures move the same camera. Points are coloured by depth, IR, or the live colour
  (RGB, which is colour: no screenshot hook, and not during a playback), 1 to 10 px, round from 2.5 px.
  M and R flip it as the Mac does. Space freezes it; the camera still moves around a frozen view.
- **Depth cut-off** (every mode, 3D too): *Cut off outside* with Near (200–5000 mm) and Far (400–8000 mm),
  kept 50 mm apart; **Track me** sets 0.5–2.0 m and turns it on. Saved in presets and the last-used settings.
- **Depth / colour blend** (live only): the depth colours faded over the registered live colour, in every 2D
  mode. It is colour, so it is never recorded (turning it on stops a recording, and Record is off while it
  is on), never in a preset, refused by the screenshot hook, and off during a playback.
- The status line shows the 3D view's own frame rate and its render + readback time.

## Second display and app icon (W5 stage 6, the Mac's phases 4.2 and 6)

- **Second display (projector)** in the panel: pick a display (a projector is picked first when one is
  plugged in; refresh with the arrow), then *Show full screen there*. That display shows the current view
  alone, whole and aspect-fit on black: no controls, no cursor readout, no region box or ruler, no zoom.
  The window never takes the keyboard or the mouse, so every control and key stays on the main window.
- **B** blacks it out at once, and B again brings the picture back (also the *Black out* button). B works
  with the output off too: it then opens blacked out. The status line says `OUT: LIVE` or
  `OUT: BLACKED OUT (B)`.
- If the output is on the same display as the main window, **Esc** (with nothing else to clear) stops it.
  Unplugging its display stops it; a display that changes size is followed.
- It shows the screen only; nothing is kept. Colour on it is live, like the main view.
- **App icon**: phase 6's logo, `design/logo.svg`, rendered at build time by `kv-render-icon` (Windows'
  own Direct2D SVG renderer and WIC, no new dependency) into `kinect-viewer.ico` at 16 to 256 px. Both
  `kinect-viewer.exe` and `kinect-cli.exe` carry it; change the SVG and the next build follows.
## Recordings in the app (W5 stage 4, the Mac's phase 2)

- **Recordings...** (under the Record button) lists every IR and Depth session in
  `%USERPROFILE%\kinect-viewer-recordings\` with its date, mode, length, size and time left: metadata only,
  never a thumbnail or a preview.
- **Play** replays a session through the live pipeline, as if it were the sensor (which is closed meanwhile,
  and the phone sees Off). Every mode and tool works on it; Pause/Play, one frame back or forward, and the
  slider to scrub. **Back to live** ends it (Start opens the sensor again). Decoding runs on its own thread.
- **Deleted after 7 days**: at launch and once an hour, with a countdown in the list. Each deletion, automatic
  or by **Delete...**, adds one line to `deletion-log.txt` in that folder: the time, the folder name, its size
  and `automatic` or `user`, nothing about the content. Old colour sessions from W4 (`*_color1080p.nosync`,
  `*_sidebyside.nosync`) are deleted at launch the same way, and never listed.
- **Before every recording**, from the PC or the phone, a reminder: *Only you in view. Not pointed at a
  window. Depth and IR only. Deleted after 7 days.* Cancel is the default; "Don't remind me for 1 hour"
  skips it for an hour. A phone that showed it and was confirmed starts the recording directly.
- Each session now has `session_meta.json` (the Mac's format), with the depth camera's intrinsics, so the
  ruler works on playback. A W4 session without it plays, but its ruler says *Intrinsics unavailable*.
## Host-only recording (W4)

```bat
%CLI% record --mode ir --duration 10                 & rem 16-bit IR, FFV1 lossless .mkv + timestamps CSV
%CLI% record --mode depth --duration 10 --png16      & rem 16-bit depth (mm), PNG sequence + CSV
%CLI% record --mode color --duration 10              & rem refused: "Colour is live only. Switch to IR or Depth to record."
```

In the viewer, the "Host-only recording" section has the 16-bit format choice and Record / Stop recording.
Changing mode, or stopping the stream, stops a recording (as on the Mac).

- Folder: `%USERPROFILE%\kinect-viewer-recordings\<yyyyMMdd-HHmmss-SSS>_<mode>.nosync\`. The recorder refuses any
  path inside OneDrive (`%OneDrive%`, `%OneDriveConsumer%`, `%OneDriveCommercial%`).
- Protected owner-only DACL (current user only, inherited by the files) and `FILE_ATTRIBUTE_NOT_CONTENT_INDEXED`
  (Windows Search skips it). `.nosync` / `.metadata_never_index` markers are kept for parity with the Mac.
  If File History or Windows Backup is on, add the folder to its exclusions by hand.
- FFV1 runs `ffmpeg -nostdin` with the Mac's arguments (`ffv1 -level 3 -coder 1 -context 1 -g 1 -slicecrc 1`,
  gray16le). The CSV has `frame_index,engine_seq,device_ticks,arrival_mach_ns`.
- ffmpeg is found through `%KV_FFMPEG%` (a full path to the file), the PATH entries that are full paths, or the winget Links folder; never the program's own folder or the current folder. It is started with only its own pipe and NUL as handles, so none of the host's connections or files reach it.

Proof (synthetic): FFV1 decodes back bit-exact for IR and depth (`max_abs_diff=0`, 45 frames each), PNG16 reads
back identical, and Colour / Side by Side are refused with nothing written (not even the folder).
Real-sensor recording: not part of these tests (they run the synthetic source). The viewer's `--screenshot` test hook only captures the synthetic source in a
view without colour (exit 3 otherwise, nothing written).

## Pet camera (W6, protocol v3; not run with a real device yet)

The host now speaks protocol v3 ([`protocol-v3-pet-camera.md`](protocol-v3-pet-camera.md), design in
[`pet-camera.md`](pet-camera.md)): many paired devices, each an **owner** or a **viewer**, up to 8 sessions and
4 watching at once. v1 and v2 phones keep working (v3 §2.4). The camera roll (W7) and viewing from outside the
home (W8) are not in this stage.

**The Pet camera section** in the panel:
- **Privacy mode** (the large button, red while on) stops live video to every device at once, owners
  included. The Kinect and this preview stay on. It is saved across restarts.
- **Share the camera** (off by default). Off: only owner devices watch, at home; viewers get nothing.
  **Schedule...** edits the sharing schedule: rows of days and from-to times during which sharing is off for
  viewers (a row that ends before it starts runs past midnight; the same time twice is 24 hours).
- **Devices...** lists every paired device: name, role, kind, paired date, last seen, and whether it is
  connected (home) and watching. **Rename**, **Make owner / Make viewer** (at once, on its open session),
  **Re-pair** (a phone that lost its identity: keeps its name and role), **Revoke** (with a confirm), and
  **Revoke all**. Owners not seen for 30 days are highlighted.
- **Pair a device...** opens the pairing window with a role choice, **Viewer** by default, changeable until a
  device pairs. Pairing a second device adds it; to pair your own phone again, use **Re-pair** on its row.
  The phone paired before the upgrade became owner device "iPhone".
- **Revoke all...** is the old Unpair: every device is revoked, and the service id rotates.
- The "iPhone live stream" switches (**mode switch**, **turn the Kinect on and off**) apply to owner devices
  only. **Turn the Kinect off when the iPhone disconnects** acts when the last *available* session ends, not
  on revoking one device, a replacement or privacy mode (Revoke all still counts as the old unpair did).
- **Watching now: <names>** shows over the view whenever any device holds a watcher slot (with
  "(outside the home)" from W8). Nothing hides it while anyone watches, full screen included.
- The health display (H) has a fifth line with each live tier (high / medium / low): its encoder, size,
  epoch and sessions.

**State.** `devices.json` and `pet-camera.json` live next to `pairing.json` in
`%USERPROFILE%\kinect-viewer\.host-state\` (owner-only, written atomically; `pet-camera.md` §5).
A change is in force as soon as it is made, even when its file can't be written (another program holding
the file, a full disk): a revoked device stays refused and privacy mode stays on. The panel's status line
then says *Saving ... failed (...); retrying.* and the host tries again, a tenth of a second later at first and
then backing off (doubling up to every 5 s, and back to a tenth of a second once a write goes through), and
once more at a clean exit, until the write goes through. The writes happen outside the streaming host's
lock, so a file that keeps failing never holds up the video. Logging off or shutting Windows down with the viewer (or a
`kinect-cli serve` console) open is a clean exit too.

Reading fails closed too. A `pet-camera.json` that is there but can't be read (another program holds it) or
parsed starts the host with **privacy mode on**. An unparsable one is first kept as `pet-camera.json.bad`, and
the status line says so until the settings are saved again (turning privacy mode off on the PC does that). One
that can't be read is never written over: until the host can read it (it tries again, backing off to every
5 s), the status line says so and a change that would have to be saved (privacy off, sharing, the schedule) is
refused, on the PC and from a phone; privacy on is kept and still holds once the file is read. Only a missing
`pet-camera.json` means the defaults. A `devices.json` that is there but can't be read is not treated
as damaged: nothing is rebuilt or written over it, no device can connect or pair, the status line says so, and
the host reads it again (backing off to every 5 s) until it can. Only one that was read but doesn't parse is
kept as `devices.json.bad` and rebuilt from `pairing.json`, and only once that copy has been made. A new `.bad`
copy never overwrites an older one: the older ones move up to `.bad.1` … `.bad.4`, and the oldest beyond five is
dropped.

**Test isolation (v3 §10).** `kinect-viewer.exe --state-dir DIR` and `kinect-cli ... --state-dir DIR` keep the
host's state (`pairing.json`, `devices.json`, `pet-camera.json`, the marker) in DIR. A viewer launched with `--synthetic` or
any test hook (`--screenshot`, `--test-*`, `--exit-after-*`, `--loopback-only`, `--pair-on-start`) and no
`--state-dir` never touches the real `.host-state`: it uses `%TEMP%\kinect-viewer-test-<pid>\.host-state`,
says so in the panel's status line and the hook's report (`state_dir=`, `state_dir_temp=1`), and deletes it on
exit. A folder of that name left by an earlier run that didn't exit cleanly (Windows reuses process ids) is
removed first, so every such run starts empty. Every CTest that starts the viewer or the CLI passes a folder
under `build\windows\windows\test-state\`. Presets (and the last-used settings) are neither read nor written
in any of these launches, and recordings aren't purged.

**Camera roll (W7, [`pet-camera.md`](pet-camera.md) §6).** Clips go to `%USERPROFILE%\kinect-viewer-camera-roll\` unless the panel's **Change…** picked another folder (you can choose any folder; the change applies after a restart). Tests pass `--camera-roll-dir DIR` (with `--synthetic` only), and a test run without it has no camera roll at all.

**Outside the home (W8, [`pet-camera.md`](pet-camera.md) §7).** "Allow viewing from outside the home" in the Pet camera section is off by default and needs sharing on; it opens a second listener and asks the router for a port mapping (UPnP), or shows the manual port-forward steps, and it can't work behind carrier NAT (the panel says so). Invites: send only to the person you're inviting. Tests use `--test-outside` with `--synthetic`, a `--state-dir` and a fake router, never the real one.

`--state-dir DIR` must be a folder that doesn't exist yet, an empty one, or one the host already keeps its
state in: the host makes it owner-only, and that reaches every file already in it. The host writes a marker,
`.kinect-viewer-host-state`, into every state folder it creates or uses; a folder from before the marker (the
real `.host-state` of an existing install) is known by a `pairing.json` that reads as the host's, and gets the
marker on its next start. A file name alone is not enough: a folder that merely holds a `devices.json` or a
`pet-camera.json` is refused and left exactly as it was. Anything else, any drive root, and any junction or
link is refused before the host starts (the viewer says why and exits; `kinect-cli` prints
`STATE_DIR_REFUSED: ...` and exits with 2). Paths with characters outside the ANSI code page work in both.

**The synthetic test** (`viewer_pet_camera_panel`) seeds a `--state-dir` with two made-up devices and sharing
on, and runs `--test-pet-camera`: a fake v3 device on loopback pairs through the pairing window (as a viewer),
the banner shows its name, privacy mode, sharing and a schedule row each stop it (through the panel's own
handlers), the Devices dialog is captured, a rename reaches the banner and a revoke closes it with
`0x0F revoked`. Captures are of the synthetic source only, never colour, never a replay.

**What the owner should check with the real devices:** their iPhone still watches as before (it is owner "iPhone");
pairing a second phone as a viewer, with sharing off and on; privacy mode from the PC; the banner.
