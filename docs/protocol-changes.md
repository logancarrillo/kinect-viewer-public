# Wire Protocol Changes (`_kv2s._tcp` / `_kv2p._tcp`)

This document tracks every additive change to the encrypted Kinect Viewer LAN protocol (`shared/WireProtocol.swift`) so the Windows host (`windows/`) stays wire-compatible with Kinect Pocket (`ios/`).

All packets continue to use the 8-byte `WireHeader` (`0x4B 0x56` `"KV"`, `type: UInt8`, `flags: UInt8`, `payloadLength: UInt32` big-endian).
- **Backward / Forward Compatibility (explicit v1/v2 gating):**
  - Original `protocolVersion == 1` endpoints (both the v1 macOS/iOS app and a v1 Windows host) reject unknown wire message type bytes (`>= 0x07`) as framing violations and drop the connection, whereas their JSON parsers decode by named key and ignore extra unknown JSON fields.
  - **Host gating (`0x07` only — new host + old v1 phone):** On connection ready, the host sends **only** `HelloServerMessage` (`0x01`, with `protocolVersion: 2`) and holds `activeClientProtocolVersion = 1`. A v2 phone announces capability by including an optional `"protocolVersion": 2` field inside its `PingMessage` (`0x04`). **Only `0x07` (`HostStateSyncMessage`) waits for the phone's v2 ping** (`PingMessage` with `protocolVersion >= 2`) before the host sends it; against a v1 phone (which omits `protocolVersion` in `PingMessage`), `0x07` is never sent. Meanwhile, the v2 host accepts client-to-host `0x08`–`0x0A` messages whenever received without waiting for a v2 ping.
  - **Phone gating (`0x08`–`0x0A` — new phone + old v1 Mac/Windows host):** **The phone sends `0x08`–`0x0A` (and `modeRaw = 4` in `0x06` `selectModeReq`) based on `hello.protocolVersion >= 2` alone** — it does not wait for `0x07` or a `PongMessage`. Against a v1 host (`hello.protocolVersion < 2`), the phone suppresses `0x08` (`remoteControlCmd`), `0x09` (`orbit3DGesture`), `0x0A` (`sensorPowerReq`), and `modeRaw = 4`, and hides all v2 UI controls (Kinect power switch, Remote Control Sheet, 3D orbit, Mode 5 segment), behaving identically to the v1 viewer while keeping local in-memory freeze functional.
  - Within v2+ builds, `WireFrameReader` also skips unrecognized future message type bytes (`ParseResult.unknownFrame`) after verifying magic bytes and `payloadLength <= 4 MiB`.
- **Privacy Guarantee:** Raw depth, IR, colour frames, 3D point clouds, and Vision joint coordinates never leave the host. Mode 5 (`3D Cloud`) continues to render on the host GPU and transmit only H.264 video (`VideoAccessUnit`, `0x03`).

---

## Phase 3 — 3D Point Cloud Mode Value (`protocolVersion >= 1`, additive enum value)

No wire framing or JSON field names changed in Phase 3. A fifth rendered view mode value (`modeRaw = 4`) was added for the Metal 3D point cloud stream.

| Message | Wire Type | Direction | Field | Added Value | Version | Notes |
|---|---|---|---|---|---|---|
| `HelloServerMessage` | `0x01` (`helloServer`) | Host → iPhone | `modeRaw: Int` | `4` (`KV_MODE_POINT_CLOUD_3D`) | `1` (additive) | Existing values `0` (`IR`), `1` (`Colour`), `2` (`Depth`), `3` (`SideBySide`) unchanged. |
| `StreamConfigMessage` | `0x02` (`streamConfig`) | Host → iPhone | `modeRaw: Int` | `4` (`KV_MODE_POINT_CLOUD_3D`) | `1` (additive) | Carries H.264 SPS/PPS for the rendered `960 × 720` 3D viewport. |
| `SelectModeMessage` | `0x06` (`selectModeReq`) | iPhone → Host | `modeRaw: Int` | `4` (`KV_MODE_POINT_CLOUD_3D`) | `1` (additive) | Switches host to Mode 5 when `allowPhoneModeSwitch` is enabled. |

---

## Phase 5 & Phase 7 — v2 Capability Negotiation, Host State Sync, Remote Control, 3D Orbit & Sensor Power (`protocolVersion = 2`)

Updated hosts advertise `protocolVersion: 2` inside `HelloServerMessage` (`0x01`) while keeping all 7 v1 `HelloServerMessage` fields unchanged, and updated iOS clients advertise `"protocolVersion": 2` inside `PingMessage` (`0x04`, ignored by v1 Mac/Windows hosts). Over the mutually pinned TLS 1.3 stream connection (`_kv2s._tcp`), **the phone sends `0x08`–`0x0A` based on `hello.protocolVersion >= 2` alone**, whereas **only `0x07` (`HostStateSyncMessage`, host → iPhone) waits for the phone's v2 ping** (`PingMessage` with `protocolVersion >= 2`). When the Kinect sensor is turned Off, the host stops `VideoAccessUnit` frames, closes USB, and deletes `~/KINECT-IN-USE`, while keeping the `_kv2s._tcp` mTLS session open so the paired iPhone receives confirmed state updates and can turn the sensor back On.

### 0. `PingMessage` Capability Field (`0x04` / `ping`, additive JSON field)
- **Direction:** iPhone → Host
- **Version:** `2` (optional field in existing `0x04` JSON message)
- **Fields added:** `protocolVersion: Int?` (`2` from v2 iOS clients; omitted/`nil` from v1 clients). Ignored by v1 hosts; unlocks `0x07` (`hostStateSync`) on v2 hosts (`0x08`–`0x0A` from the phone depend on `hello.protocolVersion >= 2` alone and do not wait for this ping).

### 1. `HostStateSyncMessage` (`0x07` / `hostStateSync`)
- **Direction:** Host → iPhone
- **Version:** `2`
- **When sent:** Immediately after the host receives the client's first `PingMessage` with `protocolVersion >= 2`, and whenever host power state, view mode, permissions, depth cut-off, host freeze, or host recording state changes (during active recording, frame-count syncs are throttled to ~1 Hz unless recording starts/stops or another state field changes).

| Field | JSON Type | Description |
|---|---|---|
| `protocolVersion` | `Int` | `2` |
| `stateSeq` | `UInt64` | Monotonic host state sequence counter (`1, 2, ...`). Client ignores out-of-order states with smaller `stateSeq`. |
| `powerState` | `String` | Confirmed sensor state: `"off"`, `"starting"`, `"on"`, `"in_use_elsewhere"`, or `"error"`. Never assumed by client. |
| `inUseHolder` | `String?` | Present when `powerState == "in_use_elsewhere"` (e.g. `"agent: Protonect pid: 4242"`); client renders `"The Kinect is in use by <holder>"` and keeps switch Off. |
| `errorMessage` | `String?` | Present when `powerState == "error"` or a command fails. |
| `modeRaw` | `Int` | Active host mode (`0` IR, `1` Colour, `2` Depth, `3` Side-by-Side, `4` 3D Cloud). |
| `sbsLayoutRaw` | `Int` | Active Side-by-Side layout (`0` IR+Depth, `1` Colour+Depth, `2` Colour+IR). |
| `allowPhoneModeSwitch` | `Bool` | Whether remote mode, cut-off, freeze, 3D orbit, and recording controls are permitted by the host. |
| `allowPhoneSensorPower` | `Bool` | Independent host setting (`true` by default) controlling whether the iPhone can turn the Kinect sensor On/Off. Defaults to `true` if omitted. |
| `sensorPowerDisabledReason` | `String?` | Present when `allowPhoneSensorPower == false` (e.g. `"Power control is turned off on the Mac"`), displayed below the disabled iPhone power switch. |
| `cutoffEnabled` | `Bool` | Whether near/far depth cut-off planes are active on the host. |
| `cutoffNearMm` | `Float` | Near cut-off plane in millimetres (e.g. `500.0`). |
| `cutoffFarMm` | `Float` | Far cut-off plane in millimetres (e.g. `2000.0`). |
| `isHostFrozen` | `Bool` | Whether the host display is currently frozen in memory. |
| `isRecording` | `Bool` | Whether lossless 16-bit Depth/IR host recording is currently active. |
| `canRecordCurrentMode` | `Bool` | `true` only in non-colour modes (`IR` or `Depth` without Depth/Colour Blend). |
| `recordingDisabledReason` | `String?` | Tooltip/reason when recording is unavailable (e.g. `"Colour is live only. Switch to IR or Depth to record."`). |
| `recordingReminderNeeded` | `Bool` | `true` when starting a recording requires confirming the Phase 2 privacy reminder (`"Only you in view. Not pointed at a window. Depth and IR only. Deleted after 7 days."`). |
| `recordingFrames` | `UInt64` | Frames written in the active host recording session (`0` when idle). |
| `recordingElapsedSec` | `Double` | Elapsed seconds in the active host recording session (`0.0` when idle). |
| `isReplayingOnHost` | `Bool` | `true` when the host is currently replaying a saved Depth/IR recording. |

### 2. `RemoteControlCommandMessage` (`0x08` / `remoteControlCmd`)
- **Direction:** iPhone → Host
- **Version:** `2`
- **When sent:** Remote Control Sheet actions or unfreezing local iPhone playback.

| Field | JSON Type | Description |
|---|---|---|
| `action` | `String` | `"select_mode"`, `"set_cutoff"`, `"set_host_frozen"`, `"start_recording"`, `"stop_recording"`, or `"request_keyframe"`. |
| `modeRaw` | `Int?` | Target mode (`0..4`) when `action == "select_mode"`. |
| `sbsLayoutRaw` | `Int?` | Target Side-by-Side layout (`0..2`) when `action == "select_mode"`. |
| `cutoffEnabled` | `Bool?` | Enables/disables host depth cut-off when `action == "set_cutoff"`. |
| `applyTrackMePreset` | `Bool?` | If `true` with `action == "set_cutoff"`, applies `"Track me"` (`500–2000 mm`, enabled). |
| `hostFrozen` | `Bool?` | Target host freeze state when `action == "set_host_frozen"`. |
| `reminderConfirmedOnPhone` | `Bool?` | When `action == "start_recording"`, `true` indicates the user confirmed the exact Phase 2 reminder on the iPhone; if `false`/`nil` while `recordingReminderNeeded` is `true`, the host presents its confirmation dialog on the Mac before starting. |
| `snoozeReminderOneHour` | `Bool?` | Optional 1-hour reminder snooze (`3600 s`) when `reminderConfirmedOnPhone == true`. |

### 3. `Orbit3DGestureMessage` (`0x09` / `orbit3DGesture`)
- **Direction:** iPhone → Host
- **Version:** `2`
- **When sent:** Touch orbit, pinch-zoom, two-finger pan, or reset gestures on the iPhone while viewing Mode 5 (`modeRaw == 4`). Host updates its 3D camera, renders via GPU, and streams H.264 video frames.

| Field | JSON Type | Description |
|---|---|---|
| `deltaYawRadians` | `Float` | Incremental horizontal orbit angle in radians (`0` if none). |
| `deltaPitchRadians` | `Float` | Incremental vertical orbit angle in radians (`0` if none). |
| `zoomScale` | `Float` | Multiplicative camera distance factor (`1.0` = unchanged; `< 1.0` = closer; `> 1.0` = farther). |
| `panRightMeters` | `Float` | Camera target pan along right axis in metres (`0` if none). |
| `panUpMeters` | `Float` | Camera target pan along up axis in metres (`0` if none). |
| `resetView` | `Bool` | If `true`, resets the 3D camera to default `(yaw: 0, pitch: 0, distance: 2.05 m)`. |

### 4. `SensorPowerControlMessage` (`0x0A` / `sensorPowerReq`)
- **Direction:** iPhone → Host
- **Version:** `2`
- **When sent:** Toggling the large Kinect On/Off switch on the iPhone (top bar or Remote Control Sheet).

| Field | JSON Type | Description |
|---|---|---|
| `requestId` | `UInt64` | Client-monotonic request identifier for diagnostics/ordering. |
| `turnOn` | `Bool` | `true` = check `~/KINECT-IN-USE` and open Kinect sensor (never starts recording); `false` = stop video streaming, finalize & stop any active recording, close USB sensor, and delete `~/KINECT-IN-USE` while keeping the host app running and paired. |

---

## Windows host (`windows/`)

From W5 stage 1 the Windows host speaks v2 as described above: `protocolVersion: 2` in hello, every client held at v1 until its Ping says `protocolVersion >= 2`, then one `0x07` at once and one per state change, never to a v1 phone. Its stream reader skips unknown types (the pairing reader still drops them). Where it differs from the Mac host for now:
- **Mode 4 (3D Cloud)** (stage 5): `selectModeReq` / `select_mode` with `modeRaw = 4` switches to it. The host renders the point cloud on its GPU (Direct3D 11) at `960 × 720` and sends only that picture as H.264, as the Mac does; the point cloud itself never leaves the PC. `0x09` moves the 3D camera as on the Mac (reset, or orbit, `zoomScale` as a distance multiplier, pan), when `allowPhoneModeSwitch` is on.
- **Cut-off** (stage 5): `set_cutoff` with `applyTrackMePreset: true` applies Track me (500–2000 mm, on); otherwise `cutoffEnabled` turns it on or off. `0x07` carries `cutoffEnabled`, `cutoffNearMm` and `cutoffFarMm`, and a new 0x07 goes out when they change on the PC. **Host freeze** (stage 3): `set_host_frozen` freezes the shown frame in memory, `0x07` reports `isHostFrozen`, and `request_keyframe` re-sends the frozen frame. A frozen 3D view still follows `0x09`.
- **Recording from the phone** (stage 4): `start_recording` with `reminderConfirmedOnPhone: true` starts it (and `snoozeReminderOneHour` snoozes the reminder); otherwise the PC shows its own reminder. `canRecordCurrentMode` is false in colour modes, in 3D, with the depth/colour blend on, and during a playback, with the reason.
- `sensorPowerDisabledReason` is the Mac's sentence, word for word, until there's PC wording.
- A resume from sleep (and a USB replug) reopens the sensor only if no On or Off arrived during the delay.

---

## Protocol v3: the pet camera (Windows host, planned)

v3 adds multiple devices with owner and viewer roles, privacy mode, live quality tiers, a camera roll on the PC (listed, thumbnailed and played over the protocol), and viewing from outside the home through a separate listener with one-time invites. It is specified in [`protocol-v3-pet-camera.md`](protocol-v3-pet-camera.md), with the feature plan in [`pet-camera.md`](pet-camera.md).
- It is additive. v1 and v2 phones see the same messages as today, and the only changes they can see are listed in that document's §2.4.
- New type bytes are `0x0B`–`0x0F` and `0x12`–`0x19`.
- Until W6 merges, the Windows host still speaks v2 as described above.
