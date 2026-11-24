# Changelog

## 1.0.0 (2026-11-24)

The first public version. The Mac and iPhone apps and the Windows programs all report version 1.0.
It is built from source; there are no prebuilt downloads.

### The Mac host

- Live IR, Colour, Depth in false colour and Side by Side from a Kinect v2 through libfreenect2,
  and a 3D point cloud drawn with Metal. The OpenGL, OpenCL or CPU depth pipeline, with no silent
  fallback.
- IR percentile auto-scale, gamma and smoothing; Turbo, Viridis, Inferno and Grayscale colour maps
  with Desk, Spec and Room presets; auto, flicker-free semi-auto and manual colour exposure, read
  back from the sensor. No white balance: libfreenect2's public API has none.
- Hover readout, region statistics and a two-point 3D ruler; freeze, zoom and pan; depth cut-off
  with Track me; the depth/colour blend; an Apple Vision body and hand skeleton; a second-display
  output with instant black-out; presets; the health display.
- 16-bit lossless depth and IR recording (FFV1 `.mkv` or a PNG sequence, with timestamps) into an
  owner-only folder excluded from iCloud, Time Machine and Spotlight, with a reminder before
  recording, a library that replays sessions, and deletion after 7 days. Colour is live only.
- Streaming to one paired iPhone over pinned TLS 1.3 (protocol v2): remote controls, the Kinect on
  and off from the phone, 3D gestures, and host recordings started from the phone.
- **Watch a Camera:** the Mac as a device of a Windows pet camera, with its camera roll.
- `kinect-cli`: a stream summary, the exposure readback and self-checks for the lock, the
  no-fallback rule, streaming latency, recording and hardening.

### The Windows host

- `kinect-viewer.exe`: the Mac's views, controls, measuring tools, 3D point cloud (Direct3D 11),
  cut-off, blend, second display, presets, recordings and replay.
- The pet camera (protocol v3): up to 16 paired devices as owners or viewers, 4 watching and 8
  connected at once; privacy mode; sharing with a schedule; the **Watching now** banner; rename,
  role change, re-pair and revoke.
- Live quality tiers (high, medium, low and auto), each encoded once and sent to every session on it.
- The camera roll, off by default: H.264 clips and JPEG thumbnails on the PC, by hand, by motion
  seen in depth or on a schedule; retention by age and size with starred clips kept; a 20 GB disk
  floor; browsing, playback and starring from devices (deletion by owners), streamed and never
  downloaded.
- Viewing from outside the home, off by default and turned on at the PC only: a second TLS
  listener, a UPnP port mapping or manual forwarding, one-time invites, address re-sharing, rate
  limits before TLS, and a 30-day connection log on the PC. No relay.
- The PC's private key wrapped with DPAPI; state files written atomically and read fail-closed,
  with damaged copies kept.
- While outside viewing is on, the PC stays awake and a watchdog restarts the viewer after a crash.
  Crash dumps (stacks and modules, never the heap) stay on the PC, three at most.
- `kinect-cli.exe`: `list`, `run`, `bench`, `verify-lock`, `verify-no-fallback`, `serve`,
  `record` (synthetic only) and `router-probe`.
- A per-user install and uninstall (`windows\packaging\`), with each bundled DLL's licence texts.

### The iPhone app

- The live view decoded into memory, protected from capture and cleared in the background, during
  screen capture and on a screenshot. Nothing is written to disk; the pairing is in the Keychain.
- Pairing by QR code or 8-digit code, and the Windows host's invite and address links.
- Remote controls, the Kinect on and off, and 3D gestures, where the host allows them.
- With a Windows host: live quality, who is watching, owner controls, and the camera roll.

### Connection and security

- TLS 1.3 only, with a self-signed ECDSA P-256 certificate on each side, pinned by the SHA-256 of
  the full leaf certificate both ways. No certificate authority.
- Pairing only from the host: a 120-second, one-attempt window, a proof by HKDF-SHA256 and
  HMAC-SHA256 bound to both certificates, and a QR code pinned from the first byte.
- Nothing advertised while unpaired.

### Known limits

- The Mac host is less hardened than the Windows host, and the 8-digit code is weaker than the QR
  code. [SECURITY.md](SECURITY.md) has these and the rest.
- What has run on real hardware, and what only on synthetic data, is in the README's
  [Status](README.md#status). There is no CI yet ([CONTRIBUTING.md](CONTRIBUTING.md)).
