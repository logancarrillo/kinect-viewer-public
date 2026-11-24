# Protocol v3: the pet camera (Windows host)

Status: **implemented.** The Windows host implements the host side, and the iPhone app and the Mac's "Watch a camera" mode the device side. If code and this document disagree, fix one of them in the same PR and say which.

The feature plan and the PC-side design are in [`pet-camera.md`](pet-camera.md). The v1 and v2 protocol is in [`protocol-changes.md`](protocol-changes.md) and in `shared/WireProtocol.swift` / `windows/host/wire_protocol.{hpp,cpp}`. This document adds to them and never changes what a v1 or v2 peer sees, except where §2.4 says so.

The owner's decision applies to this feature only:
- colour may be streamed to paired devices as H.264, on the home network and over the internet;
- colour may be recorded to the camera roll on the PC (H.264 MP4 clips and JPEG thumbnails, in the camera-roll folder only).

Everything else stands:
- Raw colour, depth and IR frames and point clouds never leave the PC.
- The depth/IR recorder, its folder and its rules are untouched.
- The screenshot test hook still refuses colour and the real source.
- Development and tests use the synthetic source only.

Words used below:
- **host**: the Windows PC app.
- **device**: a paired client (iPhone app, or the Mac's "Watch a camera" mode), identified by its pinned client certificate.
- **session**: one authenticated stream connection from a device.
- **owner** / **viewer**: the device's role (§3).

---

## 1. Version negotiation

| Peer | Says | Where |
|---|---|---|
| v3 host | `protocolVersion: 3` | `0x01 helloServer`, every `0x07` and every `0x0B` |
| v3 device | `protocolVersion: 3` | every `0x04 ping` |
| v2 phone | `protocolVersion: 2` | every ping |
| v1 phone | no `protocolVersion` key | ping |

Rules:
1. **The connection's version** starts at 1 when the session becomes active. The first ping carrying `protocolVersion >= 2` raises it once, to `min(ping.protocolVersion, 3)`. After that it never changes, up or down.
   - On Windows this replaces today's clamp to `kHostProtocolVersion` (`stream_server.cpp:345-350`), which already behaves this way at 2.
2. **Host sends:**
   - `0x07` only to connections at version 2 or more, as today.
   - Any v3 type (§5) only to connections at version 3.
   - A v1 connection still gets only `0x01`–`0x05`.
3. **Host receives:** the host ignores any v3 type from a connection below version 3. Unlike v2, the check is also made on the receiving side.
4. **Device sends:** a v3 device sends v3 types only after it has received `helloServer.protocolVersion >= 3` together with a `petCamera` object, and has sent a ping with `protocolVersion: 3`.
   - Against a v2 host (the Mac host, or an older Windows build), it behaves exactly like a v2 phone and hides every pet-camera control.
5. **Why version 3 in hello is safe for v1 and v2 phones:**
   - v2 phones only compare `>= 2` and `< 2` (`PocketStreamClientCore.swift:385,397,411,424`; `PocketViewModel.swift:113,133`), so they treat 3 as 2.
   - v1 phones never read the field.
   - The extra `petCamera` key in hello (§4.1) is ignored by synthesized `Codable` and by `json_lite`.
6. **Unknown types and values:**
   - v2+ readers skip unknown types after the length check (≤ 4 MiB on devices; the host allows ≤ 64 KiB from a device, see below).
   - A v3 device must skip unknown types too, and must decode every **v3 enum string leniently**: an unknown value becomes `unknown` and is ignored. This avoids the v2 trap, where one new `powerState` value dropped the whole `0x07`.
   - The host answers an unknown v3 `action` with `0x0D` `error: "unsupported"`.
7. **Type bytes:** this spec takes `0x0B`–`0x0F` and `0x12`–`0x19`; `0x1A` and up stay free.
   - `windows/tests/test_protocol_golden.cpp` uses `0x7E` as its "nobody knows this type" sentinel, the value the Swift test uses.

**Unchanged:**
- the 8-byte `KV` header;
- the 4 MiB payload cap for host → device frames;
- TLS 1.3 with pinned P-256 certificates.

**Changed:** a frame from a device (stream or pairing connection) is capped at **64 KiB**, and a larger length closes the connection at the header, before the payload is read. No device → host message needs more. The host's JSON parser also caps each array or object at 1024 items and the whole document at 65,536 values.

**JSON conventions, unchanged:**
- compact UTF-8 objects;
- camelCase keys and snake_case enum strings;
- binary as padded standard base64;
- an absent optional is left out, and `null` is accepted on decode;
- integers as plain digits;
- booleans as JSON `true`/`false`, never 0/1.

The header `flags` byte is `0` on every v3 type.

**Integer widths.** `json_lite` `getInt` is 32-bit. Anything that can exceed 2^31 (counts, sizes, ids, times) is therefore `UInt64`:
- byte counts and Unix times in milliseconds are `UInt64`;
- seconds are `Double`.

**Enums used below.** Every one decodes leniently, as rule 6 says.

| Enum | Values | Swift / C++ |
|---|---|---|
| `PetRole` | `owner`, `viewer` | `enum PetRole: String { case owner, viewer, unknown }` / `enum class PetRole { Owner, Viewer, Unknown }` |
| `PetPath` | `home` (the LAN stream listener), `outside` (the outside listener) | the same pattern |
| `LiveTier` | `high`, `medium`, `low` | the same pattern |
| `LiveQuality` | `auto` plus the `LiveTier` values | the same pattern |
| `ClipTrigger` | `manual`, `motion`, `schedule` | the same pattern |

---

## 2. Sessions, limits and availability

### 2.1 Devices instead of one pin

The single `pinnedClientCertSHA256` becomes a **device list** (storage: `pet-camera.md` §5).

**A device record has:**
- `deviceId`: the first 8 bytes of the certificate SHA-256, as 16 lowercase hex characters;
- the full hash;
- `name`, `role`, `kind`;
- `pairedAtUnixMs`, `lastSeenUnixMs`;
- `lastProtocolVersion`: the version of its last session.

**Admission.** The LAN stream listener, and the outside listener (§8), admit a client in the TLS 1.3 handshake only if its leaf hash equals a listed device's hash, compared in constant time. As today, no application byte is read from or written to anyone else.
- **After migration**, `pinnedClientCertSHA256` is only a write-only downgrade mirror (`pet-camera.md` §5). Nothing reads it to decide admission, advertising, the "paired" state or status text: those all use the device list.

**Becoming a session.** `on_ready` looks the leaf hash up in the device list again under the server lock that revoke holds (§3.2), and closes the connection if the device is gone. It also reads the device's role then. Every later permission check reads the device's **current** role, never a cached copy.

**Limits:** at most **16 paired devices**. A 17th pairing is refused with its sentence (§9).

**Replacement.** A new session from a device that already has one replaces the older one. This is today's rule, now per device:
- the replacement is not reported as a disconnect;
- it never counts against the session limit;
- the old session gets `0x0F replaced` when it is v3;
- it happens at the new session's **first ping** (its hello), not when its TLS handshake completes. A device may race several dials, and the loser's handshake can land just after the winner's. A connection that closes without a ping, or never sends one, never ends the live session. Until the new session's first ping both sessions exist, and the session limit doesn't count the ones it will replace. Until then the new one also takes no watcher slot, so it never stops a viewer's picture.

Sessions from **different** devices run side by side.

### 2.2 Limits

| Limit | Value | Past it |
|---|---|---|
| Sessions at once (all devices, home and outside) | 8 | **An owner's new session** takes the place of the viewer session that connected most recently; that viewer is closed as below. When all 8 are owners, or the newcomer is a viewer, the newcomer is the one turned away. A turned-away session is closed **before hello**, unless the device's `lastProtocolVersion` is 3 (set at pairing when the `pairRequest` said 3, and updated by every session). Such a device gets a **notice-only** connection: hello, then on its v3 ping a `0x0F too_many_sessions` and a close, or a close after 1 s with no ping. A notice-only connection doesn't count toward the 8, never holds a slot, and gets no `0x02`, `0x03`, `0x07` or `0x0B`. An old phone therefore never sees a connect followed by a disconnect. |
| Watcher slots | 4 | §2.3 |
| Playbacks | 1 per session | A new `start` replaces the session's playback. |
| Dead-session watchdog | 5 s with no byte received, every session | Today's watchdog, per session |
| Send queue per session | 16 MiB queued | The session is closed as dead. Today the queue is unbounded (`tls_conn.hpp:99`). |

### 2.3 Watcher slots and availability

**What a slot is.** A session holds a **watcher slot** while it receives video: live `0x03`, or playback `0x19`.
- A session has at most one video stream at a time (§5.9), so it holds at most one slot.
- A playback holds its session's slot while `starting` or `playing`, and while `paused` or `ended` for at most 60 s without a command. After that the host stops it (§5.9) and the slot is free. The slot is also freed when the session ends.
- There are 4 slots.

**When a session watches live.** A session **watches** (receives live `0x03`) when all of these hold:
1. **Privacy mode is off.**
2. **The PC isn't replaying.** "Replaying" means a depth/IR recording is replaying in the PC's main view, the state `0x07` reports as `isReplayingOnHost`. This condition applies to **viewers only**; owners keep today's behaviour and get the replay, as v2 phones do now. The PC's camera-roll player draws in its own window, never feeds the encoder and doesn't count as replaying (`pet-camera.md` §4).
3. **For viewers:** "Share the camera" is on, and the sharing schedule doesn't have sharing off right now.
4. **The session wants live.** v1/v2 sessions always do. v3 sessions do unless they sent `0x0E watch: false`, or are playing a clip.
5. **It holds a slot or can get one.**
   - A free slot is taken in arrival order.
   - **An owner always gets a slot.** If none is free, it takes the slot of the viewer session that most recently took one: that viewer becomes unavailable with `too_many_viewers`, and a viewer playback is stopped.
   - Only when all 4 slots are held by owners is an owner unavailable with `too_many_viewers`.
6. **The sensor is on**, as today. With it off there is simply no video, and `0x07` says `"off"`.

**Unavailable.** When condition 1, 2, 3 or 5 fails, the session is **unavailable**. What the device sees depends on its version:

| Version | What the host does |
|---|---|
| v3 | It sends `0x0B` with `liveAllowed: false` and `liveUnavailableReason`. The session stays open for browsing and commands, and no `0x03` is sent. |
| v2 | It sends that session's `0x07` with `powerState: "off"`, `allowPhoneSensorPower: false`, and `sensorPowerDisabledReason` = the reason's sentence (§9). No `0x03` is sent. The phone shows its "Kinect is off" card, with the sentence under the disabled switch. When the session becomes available again, it gets a `0x07` with the real state, then a `0x02` and a keyframe. |
| v1 | It is closed, because a v1 phone can't be told why. This happens at its first ping if it is already unavailable, and **immediately** if it becomes unavailable later. It reconnects on its own and is closed again until it is available (§2.4). |

- Until a session's first ping, the host sends it video only if it is available. An available v1/v2 session therefore streams from its first keyframe, as today.
- The `"off"` that an unavailable v2 session sees is only that session's view. The real sensor state is unchanged, and other sessions see the truth.

**"Turn the Kinect off when the iPhone disconnects"** now means: when the last **available** session ends. It doesn't fire on any of these:
- a host-initiated close of an unavailable or over-limit session (v1 unavailability, `too_many_sessions`);
- a replacement;
- a revoke of one device.

**The exception is the whole-PC unpair** ("Revoke all", §2.4). Revoking every device fires the rule if any session was available, as unpairing did before v3: once no device is left, off is the right state. This is host-side only, so phones see no protocol change.

It is skipped while the camera roll is on (D6). The camera roll and privacy mode therefore can't power the Kinect off through this rule.

### 2.4 What v1/v2 phones can see change

**From role (§3).** A viewer-role phone gets:
- `allowPhoneModeSwitch: false` in hello and `0x07`, so both v1 and v2 phones hide their mode picker (v1 reads it from hello);
- `allowPhoneSensorPower: false` and `canRecordCurrentMode: false`, each with the viewer sentence (§9);
- its `0x06`, `0x08` (except `request_keyframe`), `0x09` and `0x0A` ignored or refused.

**From availability (§2.3).** A phone becomes unavailable when:
- privacy mode is on;
- a viewer phone has sharing off or scheduled off, or the PC is replaying;
- 4 slots are full (for an owner, only when 4 owners already hold them).

**Reconnect loops.** Old phones retry about once a second, with no back-off (`PocketStreamClientCore.swift:568-580`). Today an unpaired phone goes quiet because unpairing rotates `serviceId`. Under v3, a phone keeps retrying a handshake about once a second for as long as it is unavailable (v1), refused (revoked while other devices remain, so `serviceId` doesn't rotate), or the 9th session. That costs one TLS handshake a second, and the connection log coalesces it (§8.6).

**A second pairing adds a device** instead of unpairing the first. That is the point of the feature. To re-pair a phone that lost its identity (after a reinstall, or "Unpair" on the phone), the PC uses **Re-pair** on that device's row. It keeps the name and role (`pet-camera.md` §3).

**Unpairing is now revoke (§3.2), per device.**
- The old whole-PC unpair revokes every device. Unlike a single revoke, it fires the turn-off rule (§2.3).
- `serviceId` rotates only when the **last** device is revoked, because v1/v2 phones find the host by `serviceId` (`PocketStreamClientCore.swift:449-484`).

**The phone paired before the upgrade** becomes owner device "iPhone". On the home network it watches exactly as today, whatever the sharing settings say. Only privacy mode can stop it, apart from two rare cases: four owners already watching, or eight owner sessions already open.

---

## 3. Roles

### 3.1 What each role may do

| Action | Wire | Owner | Viewer |
|---|---|---|---|
| Watch live | `0x03`, `0x0E` | ✓ | ✓ (while sharing is on) |
| Live quality | `0x0E` | ✓ | ✓ |
| Request a keyframe | `0x08 request_keyframe` | ✓ | ✓ |
| View mode, cut-off, freeze, 3D orbit | `0x06`, `0x08`, `0x09` | ✓ (if the PC allows phone mode switch) | ✗ ignored |
| Sensor power | `0x0A` | ✓ (if the PC allows phone power) | ✗ refused with a `0x07` |
| Depth/IR recording | `0x08 start/stop_recording` | ✓ (today's rules) | ✗ ignored |
| Share on/off, privacy mode, camera roll on/off, motion on/off and sensitivity | `0x0C` | ✓ | ✗ `not_allowed_for_role` |
| Start or stop a manual clip | `0x0C start_clip` / `stop_clip` | ✓ | ✓ (while sharing is on) |
| Browse the camera roll, thumbnails, playback | `0x12`, `0x14`, `0x16` | ✓ | ✓ (while sharing is on) |
| Star or unstar a clip | `0x0C star_clip` | ✓ (any star) | ✓ (while sharing is on; may unstar only its own stars) |
| Delete a clip | `0x0C delete_clip` | ✓ | ✗ |
| "Watching now" and the device list | `0x0B` | ✓ | ✗ (left out) |
| Revoke another device | `0x0C revoke_device` | ✓ (not itself) | ✗ |
| Rename, change role, pair, re-pair, create invites, outside-the-home settings, schedules, retention, the roll folder | PC only | — | — |

Notes on the table:
- **The host enforces every row.** A device's UI only mirrors the rules.
- **Why some things are PC only (D9).** Pairing, invites, outside listening, schedules and retention stay on the PC, so a lost owner phone can't add devices, open the PC to the internet, or quietly change what is recorded or kept.
- **"While sharing is on"** also means the sharing schedule allows it and the PC isn't replaying (§2.3). Otherwise a viewer's request gets `sharing_off`, `sharing_schedule` or `pc_replaying`.
- **Privacy mode applies to every role.** It refuses live video, playback, thumbnails and `start_clip` with `privacy_mode`. Listing, starring, `stop_clip` and the owner commands keep working (§6.4).
- **Owners** are limited only by privacy mode, apart from two rare cases: all four slots held by owners, or eight owner sessions open (§2.2).

### 3.2 Revoke and role changes

**Revoke** comes from the PC's device list, or from an owner's `0x0C revoke_device`. It runs **under the server lock** that `on_ready` also takes (§2.1), so a handshake in flight can't slip through:
1. Remove the device, add its hash to the revoked list (`pet-camera.md` §5) and save.
2. Send that device's sessions `0x0F revoked` (v3 only).
3. Close them.
4. Log it (§8.6): `revoked`, with the detail "on the PC" or "by the owner device" and that device's `deviceId`. An owner's revoke carries its session's path and address.
5. Drop the stars that device set (§5.2).

If no device is left, rotate `serviceId` and stop `_kv2s`, as today's unpair does. The downgrade mirror is updated in the same save sequence (`pet-camera.md` §5).

**A role change on the PC** takes effect at once. The device's sessions immediately get a new hello and `0x07` (as `setAllowPhoneModeSwitch` does today, `stream_server.cpp:145-153`) and a `0x0B`. Owner-only keys then appear in or disappear from that `0x0B`. Commands are checked against the role current at the time of the command.

---

## 4. Changes to existing messages

### 4.1 `0x01 helloServer`: one optional key

| Key | JSON type | Swift | C++ | Req/Opt | Value |
|---|---|---|---|---|---|
| `protocolVersion` | int | `Int` | `int` | Req (unchanged) | `3` |
| `allowPhoneModeSwitch` | bool | `Bool` | `bool` | Req (unchanged) | The PC setting, and `false` for a viewer-role device |
| `petCamera` | object | `PetCameraHello?` | `optional<PetCameraHello>` | **Opt** | Always present from a v3 host |

The other hello keys are unchanged.

`PetCameraHello`:

| Key | JSON type | Swift | C++ | Req/Opt | Value |
|---|---|---|---|---|---|
| `specVersion` | int | `Int` | `int` | Req | `1` (this document) |
| `maxWatchers` | int | `Int` | `int` | Req | `4` |
| `cameraRoll` | bool | `Bool` | `bool` | Req | `true` while the host has a camera roll it can use (§7.3) |
| `outside` | bool | `Bool` | `bool` | Req | `true`: the host can listen outside the home, whether or not that is on (`0x0B`'s `outsideEnabled` says) |
| `qualities` | array of string | `[String]` | `vector<string>` | Req | `["auto","high","medium","low"]` |

No role or settings travel in hello. They come in `0x0B` once the connection is v3.

### 4.2 `0x04 ping`

A v3 device sends `protocolVersion: 3` and pings every 250 ms, like v2.

**Excluded from telemetry:** playback frames (`0x19`), which never count in latency, `lastDecodedSeq` or drop statistics.

### 4.3 `0x07 hostStateSync`: per session

The JSON is unchanged: same keys, same types, same optionals. The host now builds it **per session**:
- `protocolVersion` is `3`, the host's version. v2 phones take `max`, so this is harmless.
- `allowPhoneModeSwitch`, `allowPhoneSensorPower`, `sensorPowerDisabledReason`, `canRecordCurrentMode` and `recordingDisabledReason` follow the session's role (§2.4).
- `powerState` and its reason follow the unavailable presentation (§2.3) when that applies.
- `stateSeq` stays one host-wide counter, as today, and never goes down within a session.

v3 sessions keep receiving `0x07`. Power, mode, cut-off, freeze and depth/IR recording still live there.

### 4.4 `0x10 pairRequest` and `0x11 pairResponse`: optional keys

`pairRequest`. v3 devices add these keys; old phones send none of them.

| Key | JSON type | Swift | C++ | Req/Opt | Rules |
|---|---|---|---|---|---|
| `methodRaw` | int (u8) | `UInt8` | `uint8_t` | Req (unchanged) | `0` QR token, `1` short code, **`2` invite** (new, §8.4) |
| `protocolVersion` | int | `Int?` | `optional<int>` | Opt | `3` |
| `deviceName` | string | `String?` | `optional<string>` | Opt | 1–64 bytes of UTF-8 after trimming. The host replaces control characters with spaces and drops the bidirectional controls (U+061C, U+200E, U+200F, U+202A–U+202E, U+2066–U+2069) and the characters that show nothing (U+00AD, U+180E, U+200B, U+2060–U+2064, U+FEFF). A name already on the device list gets the first free number ("iPhone 2"), cut to fit 64 bytes; a rename on the PC is kept as typed. |
| `deviceKind` | string | `String?` | `optional<string>` | Opt | `"iphone"`, `"ipad"`, `"mac"` or `"other"`; any other value becomes `"other"` |

**Which methods each listener takes:**
- The **LAN pairing listener** takes `methodRaw` 0 or 1 only. On anything else it closes the connection without checking a proof and without burning the window.
- The **outside listener** takes `methodRaw` 2 only. On anything else it closes the connection and counts a failure (§8.5).

**Default names.** A device that sends no name is called `"iPhone"`, then `"iPhone 2"`, `"iPhone 3"` and so on, and can be renamed on the PC.

**The proof is unchanged for every method**, with these exact bytes:
- `prk = HKDF-SHA256(ikm = secret, salt = hostCertSHA256 ‖ clientCertSHA256, info = "kv2-pair-v1", L = 32)`;
- `clientProofHMAC = HMAC-SHA256(prk, "kv2-pair-client")`;
- `serverProofHMAC = HMAC-SHA256(prk, "kv2-pair-server")`.

The literal labels are the ones the Swift code passes as its `role` parameter (`CryptoIdentity.swift:96-113`). They are labels only. **The device's owner/viewer role is never an input to the proof.**
- For `methodRaw 2`, `secret` is the 16 raw invite-token bytes (§8.4).
- The device verifies `serverProofHMAC` for every method, as today.

`pairResponse` adds these keys on acceptance:

| Key | JSON type | Swift | C++ | Req/Opt | Value |
|---|---|---|---|---|---|
| `role` | string enum `PetRole` | `String?` | `optional<string>` | Opt | |
| `deviceId` | string | `String?` | `optional<string>` | Opt | 16 hex characters |
| `deviceName` | string | `String?` | `optional<string>` | Opt | The name as stored |
| `protocolVersion` | int | `Int?` | `optional<int>` | Opt | `3` |

Rules for the response:
- **`streamPort`** keeps its v1 meaning for every method, `methodRaw 2` included: it is the LAN stream listener's port.
- **It must still be the first and only frame**, under 16 KiB, because the phone reads it in one receive (`PocketStreamClientCore.swift:275-288`).
- **Rejections** keep today's shape. The texts are in §9.
- **A wrong proof** gets `accepted: false`. It burns a LAN pairing window, as today. For an invite it is counted as a failure for its source, and the tenth wrong attempt on that invite burns it (§8.4).
- **A TLS leaf that differs from `clientCertDER`** also burns it, but the connection is closed without a reply, as today (`stream_server.cpp:573-578`).

---

## 5. New message types (v3 only)

| Type | Name | Direction | Payload |
|---|---|---|---|
| `0x0B` | `petCameraState` | host → device | JSON |
| `0x0C` | `petCameraCmd` | device → host | JSON |
| `0x0D` | `commandResult` | host → device | JSON |
| `0x0E` | `liveViewReq` | device → host | JSON |
| `0x0F` | `sessionNotice` | host → device | JSON |
| `0x12` | `rollListReq` | device → host | JSON |
| `0x13` | `rollList` | host → device | JSON |
| `0x14` | `rollThumbReq` | device → host | JSON |
| `0x15` | `rollThumb` | host → device | JSON |
| `0x16` | `playbackReq` | device → host | JSON |
| `0x17` | `playbackState` | host → device | JSON |
| `0x18` | `playbackConfig` | host → device | JSON |
| `0x19` | `playbackAccessUnit` | host → device | binary |

**Request ids.** Every device → host request except `0x0E` carries a `requestId` (`UInt64`, chosen by the client, increasing). The answer echoes it.

**One answer per request.** Every **well-formed** request gets exactly one answer:

| Request | Answer |
|---|---|
| `0x0C` | `0x0D` |
| `0x12` | `0x13`, or `0x0D` on error |
| `0x14` | `0x15`, or `0x0D` on error |
| `0x16` | `0x17`, or `0x0D` on error |
| `0x0E` | always the next `0x0B`, with a new `stateSeq`, even when nothing changed |

Malformed messages are not answered (§5.10).

### 5.1 `0x0B petCameraState` (host → device)

Sent:
- immediately after the connection becomes v3;
- whenever any field below changes for that session, with clip progress throttled to 1 Hz;
- in answer to every `0x0E`.

| Key | JSON type | Swift | C++ | Req/Opt | Meaning |
|---|---|---|---|---|---|
| `protocolVersion` | int | `Int` | `int` | Req | `3` |
| `stateSeq` | int (u64) | `UInt64` | `uint64_t` | Req | Host-wide counter. Never goes down within a session; ignore lower values. |
| `deviceId` | string | `String` | `string` | Req | This session's device |
| `deviceName` | string | `String` | `string` | Req | As stored on the PC |
| `role` | `PetRole` | `PetRole` | `PetRole` | Req | |
| `path` | `PetPath` | `PetPath` | `PetPath` | Req | |
| `sharingEnabled` | bool | `Bool` | `bool` | Req | "Share the camera" |
| `sharingScheduleOff` | bool | `Bool` | `bool` | Req | `true` while the sharing schedule has sharing off |
| `outsideEnabled` | bool | `Bool` | `bool` | Req | Outside listening is on |
| `privacyMode` | bool | `Bool` | `bool` | Req | |
| `liveAllowed` | bool | `Bool` | `bool` | Req | §2.3 conditions 1, 2, 3 and 5 hold |
| `liveUnavailableReason` | string enum | `String?` | `optional<string>` | Opt | When `liveAllowed` is false: `privacy_mode`, `pc_replaying`, `sharing_off`, `sharing_schedule`, `too_many_viewers` |
| `watching` | bool | `Bool` | `bool` | Req | Receiving live `0x03` now |
| `liveQuality` | `LiveQuality` | `String` | `string` | Req | Requested |
| `liveTier` | `LiveTier` | `String` | `string` | Req | Being sent (`high` when not watching) |
| `rollAvailable` | bool | `Bool` | `bool` | Req | This session may list the camera roll now (role, sharing, PC not replaying for viewers, a camera roll on the host). Thumbnails and playback also need privacy mode off. |
| `playbackAvailable` | bool | `Bool` | `bool` | Req | It may start a playback now: `rollAvailable`, privacy mode off, and a watcher slot free or takeable (§2.3). Thumbnails need only `rollAvailable` and privacy mode off. |
| `rollEnabled` | bool | `Bool` | `bool` | Req | "Record to the camera roll", the master switch |
| `motionEnabled` | bool | `Bool` | `bool` | Req | |
| `motionAvailable` | bool | `Bool` | `bool` | Req | The live sensor runs and the PC isn't replaying a recording, so motion can work (`pet-camera.md` §6.2). Depth is decoded in every view mode, so the view itself never makes it false. |
| `motionSensitivity` | int | `Int` | `int` | Req | 1 (least) to 10 (most); default 5 |
| `scheduleEnabled` | bool | `Bool` | `bool` | Req | Scheduled recording is on |
| `scheduleSummary` | string | `String?` | `optional<string>` | Opt | Display text, e.g. `"Every day 08:00–08:10"` |
| `clipRecording` | bool | `Bool` | `bool` | Req | A clip is being written |
| `clipTrigger` | `ClipTrigger` | `String?` | `optional<string>` | Opt | While recording: the trigger that started the clip |
| `clipManualActive` | bool | `Bool` | `bool` | Req | A manual trigger is active, so `stop_clip` will work |
| `clipElapsedSec` | number | `Double` | `double` | Req | `0` when idle |
| `rollBlockedReason` | string enum | `String?` | `optional<string>` | Opt | Why recording can't run now: `privacy_mode`, `roll_disabled`, `roll_preparing`, `folder_unavailable`, `sensor_off`, `view_frozen`, `pc_replaying`, `disk_floor`, `storage_full`. Absent when it can. `roll_preparing` (additive) means the roll is on and its folder is being prepared; a device that doesn't know the value shows `rollBlockedText`. `folder_unavailable` means a camera-roll folder is set on the PC but can't be used (a missing drive, or a folder `pet-camera.md` §6.5 refuses); `cameraRoll` and `rollAvailable` are then false, and the PC shows why. |
| `rollBlockedText` | string | `String?` | `optional<string>` | Opt | The sentence for `rollBlockedReason` (§9) |
| `outsideEndpoint` | object | `PetEndpoint?` | `optional<PetEndpoint>` | Opt | The address to use from outside, when outside listening is on and an address is known (§8.3). Sent to **both** roles, so a device on the home network learns a new address automatically. |
| `outsideCandidates` | array of `PetEndpoint` | `[PetEndpoint]?` | `optional<vector<PetEndpoint>>` | Opt | Every address a device may dial from outside, in order: `outsideEndpoint` (when there is one), then this PC's own overlay (Tailscale) IPv4 address in `100.64.0.0/10` at the outside port, `verified: false`. That address is read from the PC's own interfaces when the message is built (at most 5 s old) and is never typed in, and only while outside listening is on. Sent to **both** roles in **every** 0x0B, empty (`[]`) when there is nothing to dial, so a device clears its stored list. An older host leaves it out: a device then uses `outsideEndpoint` alone. |
| `outsideStatus` | string enum | `String?` | `optional<string>` | Opt | **Owners only:** `off`, `mapping`, `mapped`, `manual`, `carrier_nat`, `address_changed`, `error` |
| `outsideStatusText` | string | `String?` | `optional<string>` | Opt | **Owners only:** the PC's sentence for that status |
| `watchingNow` | array of object | `[PetWatcher]?` | `optional<vector<PetWatcher>>` | Opt | **Owners only:** every session holding a watcher slot, this one included |
| `devices` | array of object | `[PetDevice]?` | `optional<vector<PetDevice>>` | Opt | **Owners only:** every paired device |

`PetEndpoint`:

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `host` | string | `String` | `string` | Req (dotted IPv4; a DNS name only if the owner later sets one) |
| `port` | int (u16) | `UInt16` | `uint16_t` | Req |
| `verified` | bool | `Bool` | `bool` | Req (`false` when the address was typed in on the PC and couldn't be read from the router) |

`PetWatcher` (one entry per session):

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `deviceId` | string | `String` | `string` | Req |
| `deviceName` | string | `String` | `string` | Req |
| `role` | `PetRole` | `PetRole` | `PetRole` | Req |
| `path` | `PetPath` | `PetPath` | `PetPath` | Req |
| `activity` | string enum | `String` | `string` | Req: `live` or `playback`. A session has only one video stream (§5.9), so one value is enough. |

`PetDevice`:

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `deviceId` | string | `String` | `string` | Req |
| `name` | string | `String` | `string` | Req |
| `role` | `PetRole` | `PetRole` | `PetRole` | Req |
| `kind` | string enum | `String` | `string` | Req (`iphone`, `ipad`, `mac`, `other`) |
| `pairedAtUnixMs` | int (u64) | `UInt64` | `uint64_t` | Req |
| `lastSeenUnixMs` | int (u64) | `UInt64?` | `optional<uint64_t>` | Opt (absent if it never connected) |
| `connected` | bool | `Bool` | `bool` | Req |

**Owner-only keys** are left out of a viewer's message entirely; they are not sent as empty values. Nesting is at most 3 levels, well under `json_lite`'s 32.

**A host without a camera roll** (no roll folder, `cameraRoll: false`) sends:
- `rollAvailable`, `playbackAvailable`, `rollEnabled`, `motionEnabled`, `motionAvailable`, `scheduleEnabled`, `clipRecording` and `clipManualActive` all `false`;
- `motionSensitivity: 5` and `clipElapsedSec: 0`;
- no roll-blocked keys.

It answers the roll and clip actions with `unsupported`.

### 5.2 `0x0C petCameraCmd` (device → host)

| Key | JSON type | Swift | C++ | Req/Opt | Used by |
|---|---|---|---|---|---|
| `requestId` | int (u64) | `UInt64` | `uint64_t` | Req | all |
| `action` | string enum | `String` | `string` | Req | see the next table |
| `enabled` | bool | `Bool?` | `optional<bool>` | Opt | `set_sharing`, `set_privacy`, `set_roll`, `set_motion` |
| `sensitivity` | int | `Int?` | `optional<int>` | Opt | `set_motion` (1–10; otherwise `invalid`) |
| `clipId` | string | `String?` | `optional<string>` | Opt | `star_clip`, `delete_clip` |
| `starred` | bool | `Bool?` | `optional<bool>` | Opt | `star_clip` |
| `deviceId` | string | `String?` | `optional<string>` | Opt | `revoke_device` |

| `action` | Who | Effect |
|---|---|---|
| `set_sharing` | owner | Turns "Share the camera" on or off. **Turning it off also turns outside listening off and clears that setting**: the router mapping is removed and outside sessions get `0x0F outside_disabled` (§8.1). Outside listening comes back only when it is turned on again on the PC (D9). The `0x0D` answer goes out before any close, so an owner outside the home gets it before its own session closes. Device UIs must warn before an outside session turns sharing off: "You'll be disconnected, and it can only be turned back on at home or on the PC." |
| `set_privacy` | owner | Privacy mode on or off (§6.4) |
| `set_roll` | owner | "Record to the camera roll", the master switch |
| `set_motion` | owner | Motion trigger on/off (`enabled`) and/or `sensitivity`; either may be left out |
| `start_clip` | owner, viewer | Adds the **manual** trigger (`pet-camera.md` §6.2). If a clip is already recording, it is extended rather than restarted, and the answer is `ok: true`. If recording is blocked, the answer is a `0x0D` error with the `rollBlockedReason` code and its sentence (§9; `privacy_mode`, `roll_disabled`, `roll_preparing`, `folder_unavailable`, `sensor_off`, `view_frozen`, `pc_replaying`, `disk_floor` or `storage_full`; when several apply, the first in this order, as in §5.1), and no trigger is added. A viewer is checked for roll access (§3.1) right after `privacy_mode` and before the rest: during a PC replay, with sharing off or scheduled off, it gets `pc_replaying`, `sharing_off` or `sharing_schedule` whatever else blocks recording. |
| `stop_clip` | owner, viewer | Removes the manual trigger, and the clip ends when no trigger is left active. `ok` when a manual trigger was active, and also `ok` (a no-op) when nothing is recording, since a clip can end by itself just before the request arrives. `busy` only while a motion or schedule clip is recording with no manual trigger, which the manual stop doesn't apply to. |
| `star_clip` | owner, viewer | `starred: true` adds the caller's star, if it isn't already there. `starred: false` from an owner (or the PC) clears every star on the clip. From a viewer it removes only that viewer's own star, and if the viewer has none on the clip the answer is `not_allowed_for_role`. |
| `delete_clip` | owner | Deletes the clip's `.mp4`, `.json` and `.jpg`, and logs it. A playing clip is stopped first (`0x17 stopped`). |
| `revoke_device` | owner | §3.2. Its own `deviceId` gets `invalid`. |

State changes also arrive as the next `0x0B`, and as a `0x07` where they show there.

### 5.3 `0x0D commandResult` (host → device)

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `requestId` | int (u64) | `UInt64` | `uint64_t` | Req |
| `ok` | bool | `Bool` | `bool` | Req |
| `error` | string enum | `String?` | `optional<string>` | Opt (§9 codes; present when `ok` is false) |
| `message` | string | `String?` | `optional<string>` | Opt (a sentence to show) |

### 5.4 `0x0E liveViewReq` (device → host)

| Key | JSON type | Swift | C++ | Req/Opt | Rules |
|---|---|---|---|---|---|
| `watch` | bool | `Bool` | `bool` | Req | `false` stops this session's live video and frees its slot (for example while browsing). `true` asks for it again. |
| `quality` | `LiveQuality` | `String?` | `optional<string>` | Opt | Absent keeps the current value. A new v3 session starts at `"auto"`. |

The answer is always the next `0x0B`. When live video starts, or the tier changes, the session gets a `0x02` and then an IDR (§6.2).

### 5.5 `0x0F sessionNotice` (host → device)

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `reason` | string enum | `String` | `string` | Req |
| `closing` | bool | `Bool` | `bool` | Req |
| `message` | string | `String?` | `optional<string>` | Opt (the sentence, §9) |

| `reason` | Sent when | `closing` |
|---|---|---|
| `revoked` | The device was revoked (§3.2) | `true` |
| `too_many_sessions` | It is a 9th session with `lastProtocolVersion` 3 (§2.2) | `true` |
| `replaced` | A newer session from the same device took over. The device must **not** reconnect on its own. | `true` |
| `outside_disabled` | Outside listening was turned off. Sent to outside sessions. | `true` |
| `host_shutting_down` | The PC app is exiting cleanly | `true` |
| `address_changed` | The router's public address changed: confirmed by the pinned gateway itself, or re-shared by the PC user (§8.3). Sent to every v3 session, together with a `0x0B` carrying the new `outsideEndpoint`. | `false` |

**What the device keeps.** It holds the last notice, and the last `sharingEnabled`, `outsideEnabled` and `outsideEndpoint` it saw, in memory. The endpoint is also kept in the Keychain. When the host is unreachable, the device uses them to explain why (§10).

### 5.6 `0x12 rollListReq` (device → host)

| Key | JSON type | Swift | C++ | Req/Opt | Rules |
|---|---|---|---|---|---|
| `requestId` | int (u64) | `UInt64` | `uint64_t` | Req | |
| `day` | string | `String?` | `optional<string>` | Opt | Absent lists the **days**. Present lists that day's **clips**: it must match `^[0-9]{4}-[0-9]{2}-[0-9]{2}$`, be a real date and be a day in the host's index, otherwise `not_found`. |
| `cursor` | string | `String?` | `optional<string>` | Opt | Must come from an earlier `nextCursor`; opaque to the device |
| `limit` | int | `Int?` | `optional<int>` | Opt | 1–100, default 50 |

**How cursors work.** The host builds a cursor as the base64url of `"<startUnixMs>:<clipId>"` for clips, or of `"<day>"` for days.
- **Decoding:** the host decodes a cursor only into those forms. It checks only their **shape**: the §7.1 clip-id pattern, or the day pattern.
- **Position:** paging continues after that ordering key. The anchor doesn't need to exist any more, so a clip deleted between pages doesn't break paging.
- **Bad cursors:** a malformed cursor gets `invalid`. **A list request never creates or rewrites a file.** A missing or stale day index is rebuilt by the host's own retention and start-up passes, not because a device asked.

### 5.7 `0x13 rollList` (host → device)

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `requestId` | int (u64) | `UInt64` | `uint64_t` | Req |
| `day` | string | `String?` | `optional<string>` | Opt (echoed when clips are listed) |
| `days` | array of object | `[RollDay]?` | `optional<vector<RollDay>>` | Opt (present when listing days) |
| `clips` | array of object | `[RollClip]?` | `optional<vector<RollClip>>` | Opt (present when listing clips) |
| `nextCursor` | string | `String?` | `optional<string>` | Opt (absent on the last page) |
| `storage` | object | `RollStorage` | `RollStorage` | Req |

Both lists are **newest first**.

`RollDay`:

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `day` | string | `String` | `string` | Req (`YYYY-MM-DD`) |
| `clipCount` | int | `Int` | `int` | Req |
| `starredCount` | int | `Int` | `int` | Req |
| `bytes` | int (u64) | `UInt64` | `uint64_t` | Req |

`RollClip`:

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `clipId` | string | `String` | `string` | Req (§7.1) |
| `startUnixMs` | int (u64) | `UInt64` | `uint64_t` | Req |
| `startLocal` | string | `String` | `string` | Req (PC-local ISO 8601 with offset, e.g. `2026-11-18T14:03:22-05:00`) |
| `durationSec` | number | `Double` | `double` | Req |
| `trigger` | `ClipTrigger` | `String` | `string` | Req (the trigger that started it) |
| `startedBy` | string | `String?` | `optional<string>` | Opt (a device name or `"This PC"`; absent for motion and schedule) |
| `starred` | bool | `Bool` | `bool` | Req |
| `starredByMe` | bool | `Bool` | `bool` | Req (this device set the star, so a viewer may unstar it) |
| `width` | int | `Int` | `int` | Req |
| `height` | int | `Int` | `int` | Req |
| `bytes` | int (u64) | `UInt64` | `uint64_t` | Req |
| `hasThumbnail` | bool | `Bool` | `bool` | Req |

`RollStorage` (the whole roll):

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `usedBytes` | int (u64) | `UInt64` | `uint64_t` | Req |
| `capBytes` | int (u64) | `UInt64` | `uint64_t` | Req |
| `freeBytes` | int (u64) | `UInt64` | `uint64_t` | Req (free space on the roll's disk) |
| `floorBytes` | int (u64) | `UInt64` | `uint64_t` | Req (20,000,000,000) |
| `keepDays` | int | `Int` | `int` | Req |

A clip still being written isn't listed until it is finalized.

### 5.8 `0x14 rollThumbReq` / `0x15 rollThumb`

`0x14 rollThumbReq` (device → host) has two keys, both required: `requestId` (u64) and `clipId` (string).

`0x15 rollThumb` (host → device):

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `requestId` | int (u64) | `UInt64` | `uint64_t` | Req |
| `clipId` | string | `String` | `string` | Req |
| `jpeg` | string (base64) | `Data` | `vector<uint8_t>` | Req (at most 64 KiB decoded) |

- **Size:** the thumbnail is at most 320 px wide and keeps the clip's aspect.
- **Errors:** a missing clip or a missing thumbnail gets `0x0D not_found`. In privacy mode, `privacy_mode` (§6.4).
- **Devices keep thumbnails in memory only.**

### 5.9 Playback: `0x16`, `0x17`, `0x18`, `0x19`

Playback streams a recorded clip from the PC in real time. Nothing is downloaded: the device feeds the frames straight into its display layer, as it does live video.

**One video stream per session.** Starting a playback stops the session's live video (`watching` becomes false), and the playback keeps the session's watcher slot. When the playback stops, live video doesn't resume by itself; the device sends `0x0E watch: true` if it wants it. This keeps one back-pressure state per session.

**Why playback has its own types.** A v2-style decoder follows the latest `0x02` and drops every `0x03` whose `streamEpoch` differs (`PocketStreamClientCore.swift:792,803`). Mixing playback into `0x02`/`0x03` would make live video and playback fight over one decoder. So playback reuses:
- **the `0x03` body byte for byte**, behind a 4-byte stream id (`0x19`);
- **the `0x02` JSON**, plus a stream id (`0x18`).

A device parses both with its existing code, through a second decoder. **Live video stays on `0x02`/`0x03`, for every version.**

#### `0x16 playbackReq` (device → host)

| Key | JSON type | Swift | C++ | Req/Opt | Rules |
|---|---|---|---|---|---|
| `requestId` | int (u64) | `UInt64` | `uint64_t` | Req | |
| `action` | string enum | `String` | `string` | Req | `start`, `pause`, `resume`, `seek`, `stop` |
| `clipId` | string | `String?` | `optional<string>` | Opt | Required for `start` |
| `streamId` | int (u32) | `UInt32?` | `optional<uint32_t>` | Opt | Required for every action except `start`. It must be this session's current stream, otherwise `not_found`. |
| `positionSec` | number | `Double?` | `optional<double>` | Opt | Used by `start` (default 0) and `seek` (required); clamped to `[0, durationSec]` |

**States and transitions.** The answer to every `0x16` is one `0x17` carrying its `requestId`, or a `0x0D` on error.

| State | `start` | `pause` | `resume` | `seek` | `stop` |
|---|---|---|---|---|---|
| none | → `starting` → `playing` | `not_found` | `not_found` | `not_found` | `not_found` |
| `starting` / `playing` | Old stream → `stopped` (no `requestId`); new stream → `starting` (with the `requestId`) | → `paused` | No change: `starting` stays `starting` and plays once ready, with no pause intent; `playing` stays `playing` | → `playing`, from the keyframe at or before the position | → `stopped` |
| `paused` | as above | → `paused` (no change) | → `playing` | Stays `paused`, and sends a new `0x18` plus that one keyframe, so the device shows the new position | → `stopped` |
| `ended` | as above | → `ended` (no change) | → `playing` from 0 | → `playing` | → `stopped` |

- **When a stream is freed:** `stopped` and `error` both free it, and a later action on that stream gets `not_found`.
- **What `ended` means:** the last frame has been sent, and the stream stays allocated for `seek` or `resume`. A paused stream never reaches `ended`.
- **Idle streams:** a stream `paused` or `ended` for 60 s without a command is stopped by the host (`stopped`, no `requestId`), which frees it and its watcher slot (§2.3).
- **Seeks are keyframe-accurate.** GOPs are 1 s.
- **Every seek** sends a fresh `0x18` with a higher `streamEpoch` before its frames.
- **Errors on `start`:**
  - a missing clip gets `not_found`;
  - privacy mode gets `privacy_mode`;
  - no slot (§2.3) gets `too_many_viewers`;
  - a viewer without roll access gets `sharing_off`, `sharing_schedule` or `pc_replaying`.

#### `0x17 playbackState` (host → device)

Sent on every state change, as the answer to `0x16`, and at most once a second while playing.

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `requestId` | int (u64) | `UInt64?` | `optional<uint64_t>` | Opt (present when it answers a `0x16`) |
| `streamId` | int (u32) | `UInt32` | `uint32_t` | Req |
| `clipId` | string | `String` | `string` | Req |
| `state` | string enum | `String` | `string` | Req: `starting`, `playing`, `paused`, `ended`, `stopped`, `error` |
| `positionSec` | number | `Double` | `double` | Req |
| `durationSec` | number | `Double` | `double` | Req |
| `message` | string | `String?` | `optional<string>` | Opt (with `error`, and with a host-initiated `stopped`) |

**Host-initiated stops.** The host stops a playback, without a `requestId`, when any of these happens:
- the clip is deleted;
- privacy mode turns on;
- an owner takes the slot;
- a viewer loses roll access;
- the stream has been `paused` or `ended` for 60 s without a command;
- the session ends.

Before a host-initiated `0x17`, the stream's `0x19` frames still queued for the session are dropped, so nothing of the clip reaches the device after the reason to stop (a frame already started on the wire is finished). When privacy mode turns on, or a viewer loses roll access, `0x15` thumbnails still queued for that session are dropped too, and each of those requests is answered with a `0x0D` carrying the reason instead.

#### `0x18 playbackConfig` (host → device)

It carries every `0x02 streamConfig` key, with the same types:
- `streamEpoch`;
- `codecRaw` = 0;
- `width` and `height`;
- `modeRaw` and `sbsLayoutRaw`, set to the view the clip was recorded in;
- `nalLengthHeaderBytes` = 4;
- `parameterSets` = `[SPS, PPS]` in base64.

It adds these keys:

| Key | JSON type | Swift | C++ | Req/Opt |
|---|---|---|---|---|
| `streamId` | int (u32) | `UInt32` | `uint32_t` | Req (≥ 1, unique per session) |
| `clipId` | string | `String` | `string` | Req |
| `durationSec` | number | `Double` | `double` | Req |

`streamEpoch` is per stream. It starts at 1 and goes up on each seek.

#### `0x19 playbackAccessUnit` (host → device), binary

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `streamId`, `UInt32` big-endian |
| 4 | 29 | **Exactly the `0x03` body header**: `streamEpoch` u32 BE, `frameSeq` u64 BE, `hostCaptureNs` u64 BE, `hostEncodeDoneNs` u64 BE, `flags` u8 |
| 33 | n | AVCC payload (4-byte lengths), `n ≥ 1` |

**Field meanings for playback:**
- `hostCaptureNs` is the frame's **media time**, in ns from the clip start.
- `hostEncodeDoneNs` is `0`.
- `frameSeq` counts from 0 per `streamEpoch`.
- `flags` bit 0 is keyframe; every other bit is 0.

**Telemetry.** Devices don't count playback frames in latency or drop statistics (§4.2).

**Pacing.** The host sends frames at the clip's own timing, and paces on the session's own queued bytes:
- When the backlog passes 2 MiB, it waits instead of dropping. Playback isn't live, so it may run late but never skips.
- Frames for an old `streamEpoch` that arrive after a seek must be dropped by the device.

### 5.10 Malformed v3 messages

- **Bad JSON, a key of the wrong type, or a missing required key:** the message is ignored, as in v2, and isn't answered. A device should time out a request after 10 s.
- **A well-formed message with an unknown `action`:** answered with `0x0D unsupported`.

---

## 6. Live video

### 6.1 Tiers: encode once per tier and fan out

There is one picture: the PC's shown view, as today, and it is the same for everyone. Whoever may change it (the PC, and owners while phone mode switching is allowed) changes it for all.

| Tier | Size | Bitrate | Frame rate, GOP |
|---|---|---|---|
| `high` | Today's encoded size per mode (`video_convert.cpp:8-12`) | Today's rule: 10 Mbps when width ≥ 1600, otherwise 5 Mbps | as today (≤ 30 fps), GOP 30 |
| `medium` | Longest side ≤ 1280, even, aspect kept (1920×1080 → 1280×720; 1024×848 unchanged) | 2.5 Mbps | ≤ 30 fps, GOP 30 |
| `low` | Longest side ≤ 640 (1920×1080 → 640×360; 1024×848 → 640×530) | 0.8 Mbps | The encoder opens at 15 fps and GOP 15, and gets every second frame with real timestamps. |

**Scaling.** `medium` and `low` frames are made from the BGRA frame by a CPU area-average downscale straight to NV12 at the tier's size. That is a new function next to `bgraToNv12`, with no new dependency. It runs once per tier per frame.

**Encoders:**
- **A thread per tier.** Each tier has its own encoder and encode thread, so opening, closing or a slow software encode on one tier never stalls another. The `high` tier, the v1/v2 path, is never blocked by `medium` or `low`.
- **When a tier runs.** A tier opens when a session first needs it. It stays open for 60 s after its last session leaves, so `auto` stepping doesn't churn encoders. The camera roll keeps `medium` open while it is armed (`pet-camera.md` §6.1).
- **Hardware first.** If a hardware encoder can't be opened for `medium` or `low` (consumer GPUs limit concurrent sessions), that tier falls back to the software MFT. `high` keeps today's selection. The PC's diagnostics show each tier's encoder name.

**Who gets which tier:**
- Every session gets exactly one tier.
- **v1 and v2 sessions are always on `high`.** That is today's stream byte for byte, so the home path stays as fast as now.
- **v3 sessions:**
  - `high`, `medium` and `low` pin the tier.
  - `auto` starts at `high` on the `home` path and at `medium` on the `outside` path. It steps down one tier after 3 congestion drops within 5 s (§6.3), and steps back up after 30 s with no drop.

### 6.2 Epochs, configs and keyframes

**Epochs.** Live `streamEpoch` values come from **one host-wide, strictly increasing counter** shared by all tiers. No two tiers ever share an epoch.

**Changing a session's tier** happens under the fan-out lock. The session gets the new tier's `0x02`, whose epoch is higher than any it has seen, and no AU of the old tier is queued after it.

**Configs.** Before a session's first live AU, and after any tier change, the session gets its tier's current `0x02`. That covers connecting, becoming available, and starting to watch again. The first `0x03` after a `0x02` is always an IDR.

**IDR requests.** The tier's encoder is asked for an IDR when:
- a session joins the tier;
- a session sends `request_keyframe`;
- a session dropped a frame (§6.3).

Forced IDRs are coalesced to at most one per tier per 500 ms. A session that is waiting gets nothing until that IDR.

**A device's `0x08` commands** (`request_keyframe` from any role, an owner's controls) are held to a bucket of 8 that refills at 4 a second. The bucket is the device's (its certificate's), not the session's, so a reconnect doesn't refill it. Past it a command is dropped, unanswered as `0x08` always is, and the session stays open. Each one costs the PC's window a message and may send every session a state message, so a device flooding them is held to that; a person tapping never meets it.

**Mode or size change.** It starts a new epoch on every open tier, with a `0x02` and an IDR for every session, as today.

**While the view is frozen** the viewer feeds no new frames. Any IDR request on any tier therefore makes the viewer submit the frozen frame again, as `request_keyframe` does today (`RequestKeyframe` in the viewer's `onPhoneCommand`, `viewer_app.cpp`). A session that joins during a freeze still gets its `0x02` and an IDR.

**State per tier and per session.** The single global `emit_config_` / `force_keyframe_` becomes per tier, and "needs a config" becomes per session.

### 6.3 Back-pressure per session

- **Dropping.** As today, a non-key AU is dropped for a session whose previous video is still in flight (`stream_server.cpp:429-433`). That session then waits for the next IDR on its tier, requested under the coalescing rule.
- **Containment.** A slow device no longer forces keyframes for everyone faster than every 500 ms, and never affects a device on another tier.
- **Feedback.** Drops feed `auto` (§6.1).

### 6.4 Privacy mode

**What it is.** Privacy mode is the one switch that stops everything visual leaving the PC, for every session and every role.

**At once, it stops:**
- all live video;
- all camera-roll playback (`0x17 stopped`);
- camera-roll recording: a clip in progress is finalized, and no trigger fires.

**While it is on, it refuses:**
- thumbnails (`0x14` gets `privacy_mode`);
- new playbacks (`0x16 start` gets `privacy_mode`).

**What still works.** Clip **listing** (metadata only), starring, `stop_clip` and the owner commands keep working, so an owner can turn privacy mode off from a device. `start_clip` is refused with `privacy_mode`.

**What it doesn't touch:**
- the sensor, which stays on;
- the PC's own preview;
- depth/IR recording started on the PC (that is the owner's own tool, and it stays separate).

**Sessions and persistence.** v2 and v3 sessions stay open and are unavailable with `privacy_mode`. v1 sessions are closed (§2.3). Privacy mode is saved and survives a restart.

---

## 7. Camera roll on the wire

### 7.1 Clip ids

A clip id is the clip's path below the camera-roll folder, without its extension and using `/`:
- `YYYY-MM-DD/HH-MM-SS_<trigger>`, or
- `YYYY-MM-DD/HH-MM-SS_<trigger>-<n>`.

**Validation.** The host accepts only ids that:
- match `^[0-9]{4}-[0-9]{2}-[0-9]{2}/[0-9]{2}-[0-9]{2}-[0-9]{2}_(manual|motion|schedule)(-[0-9]{1,3})?$`, **and**
- are in its index.

Anything else gets `not_found`. Nothing a device sends (clip id, day, cursor) is ever joined into a path unchecked.

**Times** in ids are PC-local. `startUnixMs` is the exact instant.

### 7.2 What the host stores

`pet-camera.md` §6 has the layout, sidecars, triggers and retention. Over the wire a device sees only §5.6–5.9.

### 7.3 Availability

`rollAvailable` is false in these cases:
- for a viewer, while sharing is off or scheduled off, or while the PC is replaying (§2.3);
- for everyone, when a camera-roll folder is set on the PC but can't be used (`rollBlockedReason` `folder_unavailable`: a missing drive, a refused folder, one that couldn't be prepared, or a test run's refusal); `cameraRoll` is false then too;
- for everyone, on a host without a camera roll.

While it is false, `0x12`, `0x14` and `0x16` get `0x0D` with `sharing_off`, `sharing_schedule`, `pc_replaying` or `unsupported` (`unsupported` for `folder_unavailable` and on a host without a camera roll). In privacy mode, `0x14` and `0x16 start` get `privacy_mode` (§6.4).

While the folder is being prepared (`roll_preparing`), the roll stays available: listing shows what the folder already holds (nothing, for a new folder), and only `start_clip` is refused, with `roll_preparing`.

---

## 8. Outside the home

### 8.0 Source addresses

Every listener reads the peer address at `accept`. Today it passes `nullptr` (`tls_conn.cpp:391`).

**Normalising the address.** The non-loopback listeners are one dual-stack `AF_INET6` socket with `IPV6_V6ONLY` off (`tls_conn.cpp:362-372`). So the first step is always to **unwrap IPv4-mapped addresses** (`::ffff:0:0/96`) to plain IPv4. Every rule below, and the log, uses the unwrapped address.

**Local sources.** A source counts as **local** if it is one of:
- loopback (`127/8`, `::1`);
- any address inside a **home prefix**: an on-link prefix (`GetAdaptersAddresses` `OnLinkPrefixLength`) of an adapter that is up, is a physical Ethernet or Wi-Fi card (`IfType` 6 or 71, `MIB_IF_ROW2` `HardwareInterface`), isn't a tunnel, and has a gateway. That covers the home network's IPv4 subnet, its global IPv6 /64, its ULA /64, and that adapter's own link-local `fe80::/64`.

A private range alone isn't home. A VPN's adapter, a PPP link, a tunnel or a virtual adapter adds no home prefix, whichever of them holds the default route, so a VPN or another network that hands out `10/8` or `192.168/16` reaches the outside listener's rules only. A Mobile Hotspot's or Wi-Fi Direct's adapter is virtual too.

**A home prefix is no wider than a home network.** An IPv4 one must lie inside `10/8`, `172.16/12` or `192.168/16` at `/16` or narrower, and an IPv6 one must be `/64` or narrower (an on-link `/64` is the usual). A wider prefix on a physical card with a gateway adds nothing: a provider's `/22` on a bridged modem, a dorm's or a hotel's segment, or a bogus `/1`.

**A global IPv6 prefix is home only behind a home router.** A PC on a bridged modem has its provider's IPv4 segment, which is refused above, and a global `/64` the provider shares with that segment. So an adapter that has IPv4 prefixes, none of them a home's, adds no global IPv6 prefix. Its unique-local (`fc00::/7`) and link-local prefixes keep their own rules. An adapter with no IPv4 at all has nothing to judge it by, and keeps its global `/64`.

**A link-local source is home only on its own link.** `fe80::/64` is on every link, so the same address on a VPN's TAP adapter, a Mobile Hotspot or a virtual machine's switch is another host on another link. A link-local source is local only when its scope (the accepted socket's `sin6_scope_id`) is the `Ipv6IfIndex` of the home adapter whose prefix holds it. One with no scope never is.

The home prefixes are read again at most every 30 s, and once more when a source they don't hold arrives and the list is over 2 s old. A read that fails keeps the last list, but only for about 5 minutes after the last read that worked. After that the list may be another network's (the PC moved, and every read since has failed), so home is loopback alone until a read works again. The time is checked when a read happens, and a source the list holds skips reads for up to 30 s, so the list can outlast the 5 minutes by up to that. The read runs before the server's lock is taken: `GetAdaptersAddresses` and a `GetIfEntry2` per adapter can take tens of milliseconds, and a stranger's connection must not hold up the sessions.

**Known limits of the adapter rule:**
- A Hyper-V external switch's `vEthernet` adapter counts as virtual. A PC whose only path to the home network goes through one has no home prefix, so its LAN listeners refuse every home device (they can still come in through the outside listener when it is on).
- A home whose router puts devices on another subnet than the PC's (some routers split Wi-Fi and Ethernet) isn't one network here: those devices aren't local.
- After the PC changes networks, the old network's prefixes can still count as home until the next read: for up to 30 s, or until a source they don't hold arrives 2 s after the last read.

**Overlay networks are never local.** A source in `100.64.0.0/10` (Tailscale's IPv4 block, and the carrier-grade block other VPNs use) or `fd7a:115c:a1e0::/48` (Tailscale's IPv6 block, inside `fc00::/7`) is never local, whatever covers it above. An interface holding such an address (the Tailscale adapter) adds no on-link prefix. A device on the overlay may be anywhere, so it gets the outside listener's rules only (pinned certificates, invites, the limiter), and never the LAN listeners or a short pairing code. This is apart from §8.3's carrier-NAT check of the router's own address, which is unchanged.

**Grouping for rate limits.** IPv4 sources are keyed by the full address. Native IPv6 sources are keyed by their /64.

**The LAN listeners' limits.** The home stream listener holds at most 16 connections in their TLS handshake, 4 per address. The pairing listener holds at most 8 connections open, 2 per address. Both count the full address, IPv6 too, since a household shares one /64. Past those, a new connection is closed before TLS and logged as `refused_too_many`; sessions of paired devices never count. A pairing connection must send its request within 10 s of its handshake, and one that hasn't is closed when the pairing window closes, so a connection kept from one window can't speak into a later one. If the host can't set a connection up (its thread, its TLS state or its event can't be made on a PC short of memory, or anything throws before the connection takes over), that connection is closed with its slot given back and logged as `refused_unavailable`. Anything a connection's handling throws closes that connection only. None of these ends the host.

### 8.1 The outside listener

**Off by default.** "Allow viewing from outside the home" is off by default, needs "Share the camera" on, and is turned on **on the PC only**. When it is on, the host opens a **second** stream listener on one fixed TCP port:
- configurable from 1024 to 65535;
- by default a random port in 20000–44999 that this PC can bind (§8.7), picked once and saved.

**Saved in `pet-camera.json`** (`pet-camera.md` §4), after the roll keys and in this order: `allowOutside`, `outsidePort`, `outsidePublicAddress`, `outsideMappedPort` (§8.7), and `outsideMappingRemoved` (bool, default `false`: the PC user pressed "Remove mapping now", and the forward stays removed until outside viewing is turned off or "Map the port again" is pressed; a value that isn't a bool reads as `true`; §8.7). Each is additive: an older build ignores the ones it doesn't know.

**TLS.** It uses the same host certificate and TLS 1.3 settings, in **its own `TlsContext`**. Each context binds its own verify callback (`stream_server.cpp:54-67`), and the LAN stream context **never** admits an unknown certificate. The outside context's verify callback accepts:
- a listed device's hash; or
- any leaf, **while an invite is open** (§8.4).

Anything else fails inside the handshake and is closed, and no application byte is read from or written to it.

**Classifying a connection.** `on_ready` classifies it under the server lock:
- **A listed device** → an ordinary session (`path: "outside"`, with every rule above).
- **Otherwise, while an invite is still open** → a **redemption-only** connection, which:
  - sends no hello and nothing else first;
  - reads exactly one frame within 10 s, at most 16 KiB, which must be `0x10 pairRequest` with `methodRaw: 2`;
  - answers with one `0x11` (§4.4) and closes after flushing.

  Anything else (another type, oversize, too slow, bad JSON) closes the connection, counts as a failure for the address (§8.5) and is logged; a peer that closes before sending its frame is logged but not counted. Only a wrong proof or a certificate mismatch burns the invite (§4.4).
- **Otherwise, when the verify callback took the certificate because an invite was open, and the PC has closed that invite since** (cancelled it, made a new one in its place, or closed it with the endpoint) → still a redemption-only connection as above, but its one frame isn't checked against any invite: it gets `accepted: false` with "This invite was closed. Ask for a new one." (§9), counts as neither a success nor a failure (§8.5), burns nothing and is logged as `invite_closed` with the source. The same holds when the PC closes the invite while the frame is on the way. Which invite admitted the certificate is recorded during the handshake (§8.7), so "no invite open now" alone never decides it.
- **Otherwise** (a certificate taken as a listed device's that was revoked since, with no invite open; or one whose invite ran out, or was used or burned by another device, during the handshake) → closed, counted as a failure and logged.

**Handshake timeout:** 5 s on this listener; the LAN listeners keep 10 s.

**The LAN listeners accept local sources only.** That covers the stream listener and the pairing window. Any other source is closed before TLS. Every connection from the internet therefore goes through the outside listener and its limits.

**Turning it off.** Outside listening turns off when it is switched off, or when sharing turns off (from the PC or an owner's `set_sharing`, which also clears the setting; §5.2). Then the host:
1. closes the outside listener;
2. sends outside sessions `0x0F outside_disabled` and closes them;
3. removes the router mapping (§8.2);
4. closes any open invite.

**Privacy mode keeps the listener up.** Outside sessions stay connected and get no video, so an owner outside the home can still turn privacy mode off.

### 8.2 Router mapping (UPnP IGD through `NATUPnP`)

All UPnP calls run on their own COM thread, never the UI thread, because each can take seconds.

**Opening a mapping** (when the outside listener opens):
1. Get `IUPnPNAT::get_StaticPortMappingCollection`. A `NULL` collection means "not found yet". Retry for about 10 s, then report `manual`.
2. Pick the **internal client**: the IPv4 of the interface whose default gateway is the router. Never map to loopback, or to an address outside the private ranges. If no such address exists, the status is `error`.
3. Check `get_Item(port, L"TCP")`:
   - It exists, points at this PC's address and carries our description ("Kinect viewer pet camera") → reuse it. That is a mapping left over from a crash.
   - It exists and points elsewhere, or points at this PC with another description (another app on this PC) → `error`: "That port is already in use on the router. Pick another port." Such a mapping is never removed, on any path: turning off, "Remove mapping now", a port change, the exit, or a check.
4. Otherwise `Add(port, L"TCP", port, <internal client>, TRUE, L"Kinect viewer pet camera")`. The external and internal ports are the same. Read the mapping back.

| Result | `outsideStatus` | PC shows |
|---|---|---|
| Added (or reused) and read back | `mapped` | The endpoint (§8.3) |
| No UPnP router, UPnP off, or `Add` failed | `manual` | Plain steps for a manual port forward: forward TCP `<port>` to `<LAN IPv4>` in the router's settings |
| Port taken, or no usable internal address | `error` | The sentence for the case |

**Every 10 minutes** while it is on, the host builds a fresh `IUPnPNAT` and collection (the earlier objects are snapshots). It re-adds the mapping if it has gone, for example after a router reboot, and re-reads the WAN address (§8.3). Not while the PC user has removed the mapping ("Remove mapping now", §8.7): then the check makes no router call at all.

**Removing the mapping.** It is removed with `Remove(port, L"TCP")` when outside listening turns off and on a clean exit. At start-up, if outside listening is on, the host adds it again, unless the PC user had removed it (`outsideMappingRemoved`, §8.7).

**Limits.** `NATUPnP` finds IGD:1 gateways. A router that offers only IGD:2 shows `manual`. That is an honest limit to confirm on real hardware.

### 8.3 Carrier-NAT check and the public address

**Where the address comes from.** The host reads the router's external address from the mapping (`IStaticPortMapping::get_ExternalIPAddress`). **No outside "what's my IP" service is ever called.**

**Reading the address:**

| WAN address read | Status | Meaning |
|---|---|---|
| empty or `0.0.0.0` | `error` | "The router isn't connected to the internet." |
| `10/8`, `172.16/12`, `192.168/16`, `100.64.0.0/10`, or not a valid IPv4 | `carrier_nat` | The PC says: "Your router's internet address is <addr>, which isn't a public address. Your internet provider is using carrier NAT, or there's a second router in front of this one, so devices outside your home can't reach this PC. It can't work without a relay, and this app doesn't use one." **No `outsideEndpoint` is sent**, and invites can't be created. |
| a public IPv4 | `mapped` | `outsideEndpoint = {host: <WAN IPv4>, port, verified: true}` |

**`manual` status.** UPnP couldn't be used, so the PC can't read or verify the address, and it says so. The PC user may type the public address in.
- **With an address entered:** `outsideEndpoint = {host: <entered>, port, verified: false}`, and the PC labels it "entered by you, not checked".
- **Without one:** no `outsideEndpoint`, and invites can't be created.

**The gateway is pinned, and asked directly.** A device on the home network can answer as a router and report any address. So the host remembers the gateway it set the forward up through, and asks that gateway itself.
- **What is pinned.** When the PC user turns outside viewing on, the first `mapped` result pins the **default gateway of the interface that owns the forward's LAN address**: its IPv4, and its hardware address from the neighbour table.
- **Where it is saved.** In `pet-camera.json` (`outsideGatewayIP`, `outsideGatewayMAC`). A key that isn't a valid address drops the whole pin.
- **Its description that last answered** is saved with it (`outsideGatewayLocation`), only as a plain `http` URL on the pinned gateway's own address. Anything else read back is dropped, and the pin stands.
- **The address devices were last told** is saved too (`outsideAnnouncedAddress`). So after a restart, a different first address is checked like any change.
- **The PC's panel** says what the gateway is recognised by.

**Address changes.** While outside listening is on, the 10-minute check re-reads the address. If it has changed, one verification pass reads three things together:
- the gateway of the forward's interface (its address and hardware address);
- the address NATUPnP reports now;
- **the pinned gateway's own answers**:
  - first the description that answered last time (saved, so after a restart too), fetched straight from the gateway over TCP;
  - then, if that gives nothing, an SSDP search on that interface. Only a reply sent from the gateway's address, whose `LOCATION` is on that address, counts, and every distinct location is tried until the deadline;
  - then the gateway's description, which must be a UPnP device description;
  - then `GetExternalIPAddress` over SOAP, asked of **each** of its WAN connection services, at control URLs that must be plain `http` on the gateway's IPv4 with no user name or password. Each answer must be a SOAP answer from a WAN connection service (WANIPConnection or WANPPPConnection, any version).
  - Only public answers count; `0.0.0.0` or a private address counts as unread. A device at any other address has no say.

The gateway's answers are given up after 8 s. A whole pass also reads NATUPnP and the neighbour table, so it can take longer.

**Only a confirmed address is re-shared.** With a gateway pinned, the re-share link and the acknowledgement are only ever for an address that some pass **confirmed** (the gateway's own answer was NATUPnP's address), and only while NATUPnP still reads it. The way out of anything else is turning outside viewing off and on, which takes the gateway there then, as at first.

The pass then ends one of four ways:
- **Confirmed** (the gateway reads as pinned, and one of its public answers is the address NATUPnP reported): the address is told at once, with no click at the PC.
  - Every v3 session gets a `0x0B` with the new `outsideEndpoint` and a `0x0F address_changed` (`closing: false`).
  - The open invite carries the old address and is closed; new invites carry the new one.
  - So a new lease or a router restart while the owner is away keeps outside viewing working.
- **Not readable yet** (the gateway's UPnP doesn't answer or gives no public address, the neighbour table has no entry yet, or NATUPnP itself fails): no evidence either way.
  - The pass is read again every minute (every five after half an hour), for as long as it takes, and the first pass that confirms the address tells it.
  - Meanwhile the PC says "Checking the router…", `outsideStatus` is `address_changed`, devices keep the address they were told, and no invite is made.
  - No re-share is offered: nothing has confirmed the address. After about half an hour of passes that couldn't read it, the PC says how to start afresh: turn viewing from outside the home off and on.
- **The gateway contradicts it** (the gateway answers publicly, but none of its answers is NATUPnP's address): another device answered as the router.
  - The PC says "Another device on your network answered as the router. Your camera kept its old address." and shows the gateway's own answer.
  - No re-share link is offered, and an acknowledgement is refused while the gateway contradicts it.
  - The host keeps checking every minute (every five after half an hour). Once the gateway agrees with NATUPnP, that address is confirmed, and the PC user may re-share it; it still isn't told without them. If NATUPnP then reads another address, that one isn't confirmed, and isn't offered.
- **Another gateway** (its address or hardware address differs from the pin): held. It is never confirmed, so it isn't re-shared. Turning outside viewing off and on starts afresh with the gateway there now, which is what a **replaced router** needs.
- **No pin** (none could be read, or the saved one was corrupt): waits for the PC user's re-share, as before. Later checks don't tell it.
  - `outsideStatus` is `address_changed`, and the PC shows "Your home address changed; re-share.".
  - Devices keep the address they were told, with no notice. Open invites are closed, and no new invite is made until the re-share.
  - The PC's **re-share** shows an **endpoint link** (§8.4) with the new address. Closing it acknowledges **that** address, and only while it is still the one read.
  - Then the status returns to `mapped`, every v3 session gets the `0x0B` and the `0x0F address_changed`, and the gateway read then is pinned from now on.

**What this doesn't stop.**
- Turning outside viewing on, and acknowledging a change, trust the gateway at that moment.
- A device on the home network that impersonates the gateway's address and hardware address (layer-2 ARP spoofing) answers as the gateway, passes the check, and the address it reports is told to devices.
- SSDP replies are UDP, and a device on the home network can forge their source. Such a reply can point the search at another URL on the gateway, but the answer still has to come over TCP from the gateway's own address, as a SOAP answer from a WAN connection service. The description that answered last (saved across restarts) is fetched first, so forged replies don't come into it once the gateway has answered. Before that (the first change after turning outside viewing on), every distinct location is tried: one at a port that refuses the connection costs almost nothing, but one that drops it costs up to 3 s of the 8, so enough of them can make the gateway look unreadable for a pass. Then nothing is confirmed, so nothing is told or re-shared, and the next pass tries again.
- The forward itself is still set up through whichever device NATUPnP finds, so a device answering as the router can take it. Then the gateway contradicts the address, and nothing is told or re-shared. Mapping the forward through the verified gateway directly is a later change.
- None of these gets the picture. Every session is TLS to the host's pinned certificate, so a device sent elsewhere either reaches this host through it (which shows the traffic's sizes and timing and the device's address) or fails.

**Later options, not defaults.** A DNS name in `outsideEndpoint.host` is a later option; no dynamic-DNS service is called. IPv6 isn't mapped or advertised this round (`pet-camera.md` §9).

### 8.4 Remote invites and endpoint links

**Creating an invite.** Only the PC user creates invites, and only while outside listening is on and an `outsideEndpoint` exists: status `mapped`, or status `manual` with an address entered. While the status is `address_changed` the PC user re-shares first ("Your home address changed. Re-share it first, then make the invite."). Otherwise "Invite from outside…" is disabled, with the reason. The PC picks the role when it creates the invite.
- At most one invite is open at a time.
- It is valid for **10 minutes** and works **once**.

**The invite is a URI**, shown as a QR code and as copyable text:

```
kv2://invite?v=3&s=<serviceId>&fp=<b64url(hostCertSHA256)>&tok=<b64url(16 random bytes)>&h=<outsideEndpoint.host>&p=<outside port>&lh=<LAN IPv4>&exp=<unix seconds>&r=<owner|viewer>
```

**The endpoint link**, for re-sharing an address change with devices that are already paired, carries no secret:

```
kv2://endpoint?v=3&s=<serviceId>&fp=<b64url(hostCertSHA256)>&h=<outsideEndpoint.host>&p=<outside port>
```

**Format rules for both:**
- `b64url` is base64url without padding, as in today's QR (`PairingPayload.swift:67-83`).
- Keys come in this fixed order. Parsers read them by name and ignore unknown keys.
- The scheme and path are `kv2://invite` and `kv2://endpoint`, never `kv2://pair`. The iOS scanner's `hasPrefix("kv2://pair")` (`QRScannerView.swift:132`) ignores them, so old phones never act on either.
- **"Code entry" (plan and Mac prompt) means pasting this text**, with whitespace trimmed (D10). A typeable short code can't carry the fingerprint and the address, and pairing without the fingerprint would bring back the unpinned short-code risk (`pet-camera.md` §9).
- `r` is display-only on the device ("You're invited as a viewer"). The host takes the role from its own invite record.

**What a v3 device does with a pasted or scanned link:**
- **`s` and `fp` match its stored pairing:** it treats `h:p` as a **candidate** endpoint. That works for an invite too, without redeeming it. The device keeps its old endpoint and replaces it only after a handshake at `h:p`, pinned to its stored host hash, has succeeded and produced a session. If the handshake fails, the device keeps the old endpoint and says "That link didn't reach your camera." `s` and `fp` aren't secrets. The handshake is pinned end to end, so a forged link can't put a device in touch with another host. A relay that forwards to the real host passes that check, though, so a forged link can move a device's stored endpoint onto the relay: no picture (the session stays TLS to the real host), but the relay sees the traffic's sizes and timing and the device's address, and can cut it off. This is how **re-share** reaches a paired device that is away from home.
- **`kv2://endpoint` that doesn't match its pairing:** refused ("This link is for a camera you aren't paired with").
- **`kv2://invite` that doesn't match its pairing:** redeemed as below.

**Redeeming an invite:**
1. Refuse the invite if `exp` has passed on the device's clock (with 60 s of slack), or if any key is missing or malformed.
2. Connect to `lh:p`, then to `h:p`. Both are the outside listener: `lh:p` is used only for redemption. **Pin the server certificate to `fp`.** A mismatch is a hard failure.
3. Send `0x10 pairRequest` with:
   - `methodRaw: 2`;
   - `clientCertDER`;
   - `clientProofHMAC` (§4.4, with the 16 `tok` bytes as the secret);
   - `protocolVersion: 3`, `deviceName`, `deviceKind`.
4. **The host, under the server lock,** checks:
   - an invite is open;
   - it hasn't expired on the host clock;
   - the TLS leaf equals `clientCertDER` (a mismatch burns the invite and closes without a reply);
   - the proof matches, in constant time (a mismatch burns the invite and gets `accepted: false`).

   On success it adds the device with the invite's role, closes the invite, replies `accepted: true` (`streamPort` = the LAN stream port) and logs it. An expired or already-used invite gets `accepted: false` with the expired message (§9).
5. **The device then:**
   - verifies `serverProofHMAC`;
   - stores `serviceId`, the pinned host hash and the outside endpoint `h:p`;
   - reconnects as a paired device: first today's **paired discovery** (Bonjour `_kv2s._tcp` by `serviceId`, plus `PairResponse.streamPort` as the address hint, path `home`), then `h:p` (path `outside`).

**Why invites can't be guessed:**
- Each token is 128 bits, so guessing one is hopeless. A wrong proof, or a TLS leaf that isn't `clientCertDER`, is counted as a failure for its source (§8.5), and the tenth such attempt on an invite burns it; before that the invite stays open for the person it was sent to.
- The device pins the host from the invite, so there is no unpinned path like LAN short-code pairing, where a fake host could collect a proof to attack offline.
- An invite never travels over this protocol. The PC user passes it on through a private channel of their choice. Anyone holding it can redeem it once within 10 minutes, so the PC says "Send this only to the person you're inviting."
- **Honest limit:** anyone who can reach `h:p` while an invite is open can still burn it, with ten wrong attempts from several addresses (one address is blocked after five failures). The owner then creates a new one.

### 8.5 Rate limits and hardening (outside listener)

| Limit | Value |
|---|---|
| New TCP connections per source (§8.0 keys) | A token bucket with a burst of 4, refilling 1 every 10 s. When it is empty, the socket is closed **before TLS**. |
| Unauthenticated connections at once, all sources (in handshake or redemption) | 8. When all 8 are taken, a handshake that **hasn't completed** gives up its slot to the newcomer, counting nothing, and is logged as `disconnected` "dropped for a newer connection". It comes from the IPv6 /48 with the most of them first, then the oldest. A completed handshake is never evicted (it is marked only once the client's CertificateVerify has proved its key, never at its Certificate message), and neither is a reserved slot. One IPv6 /48 holds at most 4 of the 8. Established sessions of paired devices don't count. |
| Unauthenticated connections at once, per source (§8.0 key: an IPv4 address or an IPv6 /64) | 2, inside the 8, so one source can't take them all. A connection counts only until its handshake (or redemption) ends; paired sessions never count, so strangers sharing a carrier address can't lock a paired device out. Decided at accept, before any thread or TLS. |
| Reserve for a recently paired source | A source key (IPv4 address or /64) that completed a paired handshake in the last 24 h gets its own unauthenticated slots outside the global 8, still held to its own bucket, its per-source 2 and its failures. A shared carrier IPv4 keeps the reserve too, which is acceptable: strangers behind it still have to pass TLS with a pinned certificate. The limiter learns it from `noteAuthenticated`. |
| New unauthenticated connections, all sources | No cap. A cap of 30 a minute let about five addresses spend the whole budget and lock out everyone else. The per-source buckets and the in-flight caps hold the load. |
| Failures per source | 5 within 1 hour blocks that source for 1 hour, counted from the fifth failure. Nothing during a block extends it or counts. A blocked source is still let in under its bucket, its 2 and the 8, with blocked sources together holding at most 4 of the 8 and never a reserved slot. Past those it is closed before TLS. A blocked source's handshake takes **only a certificate the device list holds at that moment** (proved by the handshake, never a fingerprint the client states). No invite is ever looked at for it, and anything else is refused inside the verify step, before any application byte, and logged as `refused_blocked` (a phone behind a carrier address that strangers' failures blocked still gets in). Only two things are a **failure**: a **completed** handshake whose client certificate matches no listed device while no invite is open; and a **failed or malformed invite redemption** after a completed handshake (wrong proof, certificate mismatch, another method, a frame over 16 KiB, bad JSON, no frame within 10 s, or the invite running out, or being used or burned by another device, during the handshake). An invite **the PC closes** while a device is connecting (cancelled, replaced by a new one, or closed with the endpoint) is not that device's failure: it is told "This invite was closed. Ask for a new one." and nothing is counted (§8.1). This covers only a certificate the verify callback took because an invite was open: one that arrives while no invite is open is still refused inside the handshake and counted. Nothing else counts: a peer that closes, resets or stalls past the handshake timeout, or sends something that isn't TLS, before its certificate arrives, or closes before sending its redemption frame (with a TLS `close_notify`, a reset, or a bare TCP close with no `close_notify`, which OpenSSL 3 reports as "unexpected eof"). A weak mobile link must never block the owner's own phone. |
| Success | A paired device's successful handshake refills that source's bucket and **clears no failure**: a phone on a shared address can't keep strangers' guesses fresh. Failures age out by time only. |
| Handshake timeout | 5 s, and the ClientHello read whole within 1.5 s: a connection that says nothing, or only a few bytes, is dropped as timed out, counting nothing. After that, only the 5 s applies. |
| Redemption frame | 1 frame, ≤ 16 KiB, within 10 s |

- **Back-off.** v3 devices back off exponentially on the outside path (1, 2, 4 … 30 s, reset after a session lasts 30 s), so they never drain their own bucket.
- **Memory.** Limits live in memory and reset on restart.
- **Honest limit.** Mobile carriers can put many subscribers behind one IPv4 address. Someone else's failures can then block that address for an hour, the owner's phone included (`pet-camera.md` §9).

### 8.6 Connection log

`%USERPROFILE%\kinect-viewer\.host-state\connection-log.jsonl`:
- owner-only ACL, as `pairing.json`;
- one JSON object per line;
- kept for 30 days and pruned at start-up and daily;
- capped at 20 MB. When it would pass that, the oldest lines go until it is at 15 MB: lines that aren't history first (see "A budget" below), and history only once none of those is left.

**It is never sent anywhere.** Owners don't get it over the wire either; it is shown on the PC only.

**Coalescing.** Repeated events with the same (event, device or certificate hash, source, detail) are coalesced to at most one line per minute. The line carries a `repeat` count. A `disconnected` line's session length (`42 s`, or `replaced, 42 s`) isn't part of the key, and the line keeps the first one's. A reconnect loop from an old phone (§2.4) or a paired device, or one host rotating through its IPv6 /64, adds one line a minute.

**A budget.** History is a line that names a device, or a pairing, `pairing_burned`, revoke, role change, invite event or endpoint notice. A stranger can't make those at will. Every other line (a refusal, a failed attempt, a handshake that ended early) counts against a budget of 60 new lines a minute; a repeat of a line already there isn't a new line. Past the budget, the rest are counted on one `suppressed` line, its count in `repeat`. So many sources, or varied errors, add at most 60 lines and one `suppressed` line a minute. History is never held back, and the cap drops it last, so a scanner can't flush the devices' history out.

**Writing.** At most every 10 s, the lines recorded since the last write are appended. A line whose count grows after it was written is appended again once its minute is over (or at exit): the same line and time with the larger `repeat`, and a reader keeps the larger. So a line is in the file at most twice. The whole file is written, through a temporary file and a rename, only for its first write, after the cap or the daily prune drops lines, after lines that didn't parse (kept as `.bad` first), and after a write that failed (a failed flush counts). An append never creates the file. A last line without its newline that doesn't parse is a write the host didn't finish: it is dropped and the file written whole without it, with no `.bad` copy. Bad lines anywhere else are kept as `.bad` first, at most five copies, the oldest dropped.

| Key | JSON type | Meaning |
|---|---|---|
| `t` | int (u64) | Unix ms |
| `event` | string | `connected`, `disconnected`, `refused_unknown_cert`, `refused_rate_limit`, `refused_blocked`, `refused_local_only`, `refused_too_many`, `refused_unavailable`, `paired`, `pairing_failed`, `pairing_burned`, `invite_redeemed`, `invite_failed`, `invite_burned`, `invite_closed`, `revoked`, `role_changed`, `outside_endpoint_told` (a device was told an outside endpoint, or that there is none: its kind in `detail`, never the address), `suppressed` (lines past the minute's budget, counted in `repeat`, with no address) |
| `path` | string | `home` or `outside` |
| `addr` | string | Source IP, unwrapped (§8.0), without the port |
| `deviceId` | string | Opt, when known |
| `deviceName` | string | Opt, when known |
| `repeat` | int | Opt, coalesced count (on `suppressed`, how many lines were held back) |
| `detail` | string | Opt, e.g. the session length on `disconnected` |

### 8.7 What the Windows host decided where §8 is silent

- **The certificate's name.** A new identity's certificate (a first run, or a reset) has 16 random hex characters as its common name, never a fixed one an internet scan could look for. Devices pin the certificate's hash and never read the name. An existing identity keeps its certificate, and with it the fixed name `kv2-host` it was made with: a new certificate would change the hash every device pinned.
- **Handshakes.** The outside listener's handshakes and redemptions run on a fixed pool of 12 threads: 8 for the global cap, and 4 for the reserve of a recently paired source (§8.5), whose connections run before any queued ones. A paired device's session then gets a thread of its own, as on the LAN.
- **Results the limiter counts.** A successful redemption counts as a success (it refills the source's bucket and clears no failure). A 17th device gets the 16-device sentence and leaves the invite open; that, a `devices.json` that can't be read, and an invite whose use couldn't be saved count as neither success nor failure. A certificate refused inside the handshake is told apart from every other handshake failure by the verify result (`X509_V_ERR_CERT_REJECTED`): only that one counts (§8.5).
- **The log.** A handshake that ends before a certificate was refused (a timeout, a close, a reset, bytes that aren't TLS) is logged as `disconnected` with the error as the detail, since §8.6 has no event for it and it isn't a failure. A refused certificate is `refused_unknown_cert`. A device whose invite the PC closed while it was connecting is `invite_closed`, with its source and the detail "closed while this device was redeeming it" (the PC's own close of the invite is `invite_closed` too, without a source). An outside session the host ends (outside listening off, a revoke, a replacement, too many sessions) is logged once as `disconnected`, with the reason and its length as the detail. Home sessions, pairings, revokes and role changes are logged too. On the LAN, a certificate the stream listener refuses inside the handshake is `refused_unknown_cert`, with its source, as on the outside listener. A pairing attempt that doesn't pair is logged with its source:
  - `pairing_failed` when it is closed unread and the window stays open: not a pairRequest, a method the LAN listener doesn't take, a code while the window takes the QR code only, or the window had closed;
  - `pairing_failed` too when it ends before sending its request: `no pairRequest in time` (its deadline passed), the window closed first, or the connection itself ended, with how in the detail (a peer that closed, a TLS handshake that failed or timed out);
  - `pairing_failed` too when it is refused after its proof checked (16 devices, a re-pair that can't be made, a device that couldn't be added); the window closes then;
  - `pairing_burned` for a wrong proof, or a TLS certificate that isn't its `clientCertDER`; the window closes.

  Lines are queued under the server lock and recorded after it is released, and the file is written with the log's own lock released too, so a slow disk or a flood of refused connections never holds up the server.
- **The invite at rest.** `invite.json` (owner-only, written atomically) holds the token wrapped with DPAPI for this Windows user, as `tokenProtected` (version 2), so a copy of the state folder holds no token that can be redeemed. A token that can't be unwrapped (another Windows user or PC, or a damaged value) reads as a closed invite, with the reason on the PC, and the PC makes a new one; no paired device is touched. A version 1 file with a plain `token` is read once and written again wrapped. A build from before can't read a version 2 file, so after a downgrade an open invite is lost (and kept as `.bad`), never redeemable.
- **The invite follows the endpoint.** An invite carries the endpoint it was made with. It is closed (in `invite.json`, so the outside listener stops taking unknown certificates for it) and logged as `invite_closed` as soon as there is no endpoint (a check that reads `manual`, `error` or `carrier_nat`, or an entered address cleared) or the endpoint's address or port changes, so it is redeemable exactly while the PC shows it. The PC is told why each invite ended: used, ran out, cancelled, burned by a wrong code, couldn't be saved, the home address changed or went away, the port changed, or outside listening turned off.
- **Address changes across failed checks.** The last public address devices were told is kept across failed checks, removals and re-opens. A later `mapped` check with another public address is `address_changed`, whatever failed in between, and a failed check never drops an unacknowledged change.
- **Late reports.** Every router report says which open it answers. A report for an earlier open (the old port after a port change, or before outside listening was turned off and on) is ignored, so the PC never shows a port as reachable before it is mapped.
- **A router that is stuck.** A removal that can't run because an earlier router call hasn't returned (turning off, or the clean-up after a status other than `mapped`) is kept, and runs on the mapper's worker as soon as that call returns. Shutdown never waits for it.
- **An invite closed while a device connects.** The outside verify callback runs on the pool thread that runs the handshake, so it records for that handshake which invite it admitted the certificate for (a serial each invite gets in memory when it is made or loaded, never saved). On classifying, and again under the server lock just before the redemption is checked, the host asks what became of that invite: still open → redeemed as usual; closed by the PC or replaced by a newer one → the `accepted: false` "This invite was closed" reply, neither success nor failure, and the newer invite is never checked against (so a device can't burn the next invite with the old one's proof); ran out, used or burned → as before. A malformed frame, or none within 10 s, still counts, as for any redemption; a device that closes first is logged and not counted.
- **A committed redemption.** Once a redemption is accepted (the device added, the invite used), its reply is sent even if outside listening turns off, the port changes or the app exits meanwhile, within 2 s. Every redemption reply is closed the way a session's close-after-flush is: `close_notify`, then the FIN, then what the device still sends is read and dropped until it closes, for at most 2 s and 1 MiB, so a ping it sends while it waits can't turn the close into a reset that loses the reply. If it still can't be delivered, the device stays listed (it holds a valid pairing; the PC can revoke it) and the log says the reply wasn't delivered.
- **A forward left on the router.** The port this PC's forward may still be on is saved as `outsideMappedPort` in `pet-camera.json` (an additive key: written when mapped, cleared once removed). A clean exit still only posts the removal and never waits, so it often doesn't finish. At the next start with outside listening off (or on another port) and the key set, the host removes that mapping best effort on the mapper's worker, **only if** its internal client is this PC and its description is "Kinect viewer pet camera" (never someone else's mapping on that port), notes the attempt in the private debug log only (`OutputDebugString`), and clears the key. It uses the real router only on a real launch, never in a test run.
- **Remove mapping now** (`pet-camera.md` §4). The forward is removed on the mapper's worker; the outside listener stays as it is; the status becomes `manual` with the steps (no `outsideEndpoint` unless an address was entered, so an open invite closes). **It stays removed**: the 10-minute check doesn't map it again, and makes no router call while it is removed, since the public address is read from the mapping (§8.3) and there is none. A port change keeps it removed. The PC saves it as `outsideMappingRemoved: true` in `pet-camera.json`, so it stays removed across a restart too: a start with outside listening on and the key true opens the listener and adds no mapping (and removes a forward of ours an earlier exit left on that port). It ends when the PC user turns outside viewing off (which clears the key, so the next turn-on maps; sharing turning off does the same) or presses **Map the port again** (which clears it and maps now). `outsideMappedPort` is cleared once the removal is confirmed, never set again by a check while removed, and set again when "Map the port again" or a turn-on maps.
- **Statuses.** Owners always get `outsideStatus` (`off` while outside listening is off), and `mapping` until the first check after the listener opens has finished. While the status is `manual`, `error` or `carrier_nat`, the 10-minute check tries the whole mapping again, so UPnP turned on later is found; except while the PC user has removed the mapping (above), when it stays `manual` and nothing is tried.
- **A new outside port** moves the listener and the mapping, and closes the open invite (it carries the old port). Outside sessions already connected stay, and learn the new endpoint from `0x0B`. A connection that finished its handshake on the old listener but isn't a session yet is closed instead, as it is when outside listening turns off, or turns off and on again, before it becomes one: only a connection from the listener open now becomes a session. It reconnects, as a device does after any close.
- **The default port** is picked when the host starts with none saved, and written to `pet-camera.json` at once (the start's first save), so every later start keeps it (§8.1 "picked once and saved"). Never over a `pet-camera.json` that couldn't be read; one that couldn't be parsed gets it with its next save.
- **A default port that binds.** The default is the first of up to 20 random ports in 20000–44999 that binds on the listener's address (`127.0.0.1` when the listener is loopback-only, otherwise `[::]` dual-stack; bind only, never listen, so no firewall prompt); if none binds, the panel shows an error ("Couldn't find a port this PC can open for viewing from outside the home. Enter a port below.") and its port label reads "Port: none could be opened, so enter one below", nothing is saved (`outsidePort` stays out of `pet-camera.json`), and the pick is tried again the next time outside viewing is turned on. A port the PC user chooses is taken as given.
- **Test runs** (`--synthetic`, test hooks, `--loopback-only`) use no router at all unless the test injects a fake one, so the status is `manual` with the steps. Their outside listener binds `127.0.0.1` only, and an invite's `lh` is then `127.0.0.1`. The forward's internal client is the fixed test value `192.168.77.10`, never read from this PC's adapters, so no test depends on this PC's network and its LAN address never reaches a test's output.

- **Keeping the host running** (the camera stays reachable while the PC is left running). On a real launch only, never a test run: while outside listening is open the PC holds a system-required power request (it doesn't sleep; the display may), released when outside listening closes, fails to bind, or the host stops. The viewer runs a watchdog, a second copy of its exe started as `--keep-running <pid> <state folder>`. A clean stop (outside off, the window closed, a logoff, the host or its hooks stopping) makes it stand down, and so does `install.ps1`, which signals the install folder's watchdogs before it copies. On any other end it writes the time and exit code to `keep-running.log` in the state folder and starts the viewer again with the command line it was launched with, adding `--no-autostart` when the sensor was last asked off and taking it away when it was last asked on, after 2, 5, 15, 30 and 60 s. It gives up after 5 restarts in 10 minutes, and never starts a second viewer. Clean is decided by the clean-stop mark alone, set as the first thing the window's close handler does (and at a logoff): a fault after outside viewing was turned off or the window was closed is recorded, never restarted. A restart happens only after a recorded fault or a crash's exit code (3, or one of a fixed list of NTSTATUS crash codes such as 0xC0000005 or 0xC0000409), and only for a viewer whose own filled record was read: End task and taskkill (exit code 1) end it on purpose, and it stays ended. If any viewer was started during the wait, even one closed again, the watchdog stands down. `install.ps1` and `uninstall.ps1` refuse while the viewer is open, and stand the folder's watchdogs down first. The sensor's choice is written to the block before the sensor opens or closes, and the watchdog keeps the last one it read until a restarted viewer has filled its own block. A command line longer than 2,047 characters is refused, never cut, so that viewer isn't restarted. A fault the viewer can see (an SEH exception, `std::terminate`, `abort`, a CRT invalid parameter, a pure call) is recorded out of the faulting process: the viewer puts the thread, its exception pointers and what happened (a code and module+offset, or a C++ type, never text) in a shared block and signals, and the watchdog writes the minidump (stacks and modules, never the heap) and one line to `crash\` in the state folder, keeping at most three dumps. A dump not written in 30 s is given up, and a viewer that hasn't ended 10 s later is ended. With no watchdog the viewer writes them itself, waiting at most 30 s. Either way it then ends itself with `TerminateProcess`, never reaching a debugger or an error dialog. `__fastfail` is seen only as the exit code. Dumps stay on the PC.

### 8.8 Open items

- **One viewer per user session:** a real single-instance check, a mutex per session, so a second launch hands over to the first rather than running beside it.
- **Home by the arriving adapter.** §8.0 takes home prefixes from the physical Ethernet and Wi-Fi adapters only, and a link-local source must come in on a home adapter's link. A still sturdier rule would check the adapter every connection arrived on (the accepted socket's local address, mapped to its interface).
- **A network change seen at once.** The home prefixes are read at most every 30 s, so after the PC moves networks the old one's prefixes can count as home for up to that long. Reading them on `NotifyIpInterfaceChange` would close that window.
---

## 9. Strings and codes

**Error codes** (`0x0D error`):
- `not_allowed_for_role`, `privacy_mode`, `sharing_off`, `sharing_schedule`;
- `roll_disabled`, `roll_preparing`, `folder_unavailable`, `sensor_off`, `view_frozen`, `pc_replaying`, `disk_floor`, `storage_full`;
- `too_many_viewers`, `not_found`, `busy`, `invalid`, `unsupported`, `internal`.

**Sentences.** One constant per sentence lives in `wire_protocol.hpp`, like `kvhost::text` today. They are used for:
- `sensorPowerDisabledReason` and `recordingDisabledReason` on v2 phones;
- `rollBlockedText`;
- the `message` of `0x0D`, `0x0F` and `0x17`.

| Code / use | Sentence |
|---|---|
| `privacy_mode` | "Privacy mode is on. Turn it off on the PC or an owner's device." |
| `pc_replaying` | "The PC is playing a recording." |
| `sharing_off` | "Sharing is off on the PC." |
| `sharing_schedule` | "Sharing is off at this time of day." |
| `too_many_viewers` | "Four devices are already watching." |
| viewer power (v2 `sensorPowerDisabledReason`) | "Only an owner can turn the camera on or off." |
| viewer recording (v2 `recordingDisabledReason`) | "Only an owner can record." |
| `roll_disabled` | "Recording to the camera roll is off." |
| `roll_preparing` | "The camera roll is getting ready." |
| `folder_unavailable` | "The camera roll folder on the PC can't be used." |
| `sensor_off` | "The camera is off." |
| `view_frozen` | "The view on the PC is frozen." |
| `disk_floor` | "Recording stopped: less than 20 GB is free on the PC's disk." |
| `storage_full` | "Recording stopped: the camera roll is full of starred clips." |
| playback: the clip was deleted (`0x17 stopped`; `0x17` carries no code) | "This clip was deleted." |
| playback: the clip can't be read (`0x17 error`) | "The PC couldn't read this clip." |
| playback: idle for 60 s (`0x17 stopped`) | "Playback stopped after a minute without a command." |
| `revoked` | "This device was removed from the PC." |
| `replaced` | "This device connected again somewhere else." |
| `outside_disabled` | "Viewing from outside the home was turned off on the PC." |
| `address_changed` | "The PC's home address changed." |
| `host_shutting_down` | "The camera app on the PC was closed." |
| `too_many_sessions` | "Too many devices are connected to this PC." |
| pairing: too many devices | "This PC has 16 paired devices. Revoke one first." |
| invite: expired or used | "This invite has expired or was already used. Ask for a new one." |
| invite: wrong code, counted (the invite stays open) | "That invite didn't match. Check that you used the whole link you were sent." |
| invite: wrong code, the tenth (burned) | "That invite didn't match. It can't be used again; ask for a new one." |
| invite: closed by the PC while the device was connecting | "This invite was closed. Ask for a new one." |

---

## 10. Checklist for each side

### Windows host

| Part | Covers |
|---|---|
| Multi-viewer and roles | §1–§6 and §9 for those messages |
| Camera roll | §5.6–5.9, §6.4's roll parts, §7, and the roll fields of §5.1 |
| Outside the home | §8, and the invite parts of §4.4 |

**Test isolation.** Whenever the viewer runs with `--synthetic` or any test hook, every piece of pet-camera state lives in temp folders the test passes in (`--state-dir`, `--camera-roll-dir`): the state directory, `devices.json`, `pet-camera.json`, the connection log and the camera roll. That process never opens the real camera roll and never loads real paired devices. It may start the outside listener, but only on `127.0.0.1`, and it never uses a real router: no UPnP session is ever created, only a fake one the test injects, or none (§8.7).
- **The smoke test:** `viewer_smoke.cmake` launches the viewer with a temp `--state-dir`, never the real `.host-state`.
- **Screenshots:** `screenshotAllowed`, and the second-display capture, also refuse while a camera-roll clip or any replay is on screen. Test hooks never capture the invite window or show a live token.

### Mac and iOS

- **Gating:** check `hello.protocolVersion >= 3` and `hello.petCamera`, and ping with 3.
- **Decoding:** decode §5 leniently.
- **v2 hosts:** keep the v2 path unchanged against a v2 host.
- **Playback:** play `0x18`/`0x19` through a second decoder, and leave playback out of telemetry.
- **Outside path:** back off exponentially (§8.5).
- **Before turning sharing off from outside:** warn (§5.2).
- **Links:** handle the invite and endpoint links (§8.4).
- **Storage:** store nothing but Keychain pairing state, which here means the stored outside endpoint, updated from `outsideEndpoint` and from links.
- **When the host can't be reached,** use the last notice and state from §5.5:

| Last thing the device saw | Message |
|---|---|
| `outside_disabled` | "Viewing from outside the home was turned off on the PC." |
| `address_changed`, or a stored endpoint that no longer answers after an address change | "The PC's home address changed. Ask for the new link, or connect at home once." |
| Otherwise | "Can't reach the PC. It may be off or asleep, sharing may be off, or its home address may have changed." |

Privacy mode never makes the host unreachable (§8.1), so the device shows it from `0x0B`.

### Tests each part includes (synthetic only, loopback, temp folders)

**Multi-viewer and roles:**
- **Mixed versions:** two fake v3 devices and one v1 phone at once.
  - The v1 phone never gets `0x07` or a v3 type.
  - Both v3 devices get `0x0B`.
  - All three decode frames.
- **Viewer-role v2 phone:** it gets `allowPhoneModeSwitch: false`, and its `0x06`/`0x08`/`0x0A` are ignored or refused.
- **Revoke:** a revoked device is refused in the handshake, and its open session is closed with `0x0F revoked`. A handshake held between verify and `on_ready` while revoke runs doesn't become a session.
- **Role change:** a demoted owner's next owner command gets `not_allowed_for_role` without reconnecting.
- **Slots:** with 4 viewers watching, an owner v2 phone connects and gets `0x02` and an IDR, and the newest viewer becomes unavailable.
- **Sessions:** with 8 viewer sessions open, an owner v2 phone connects and streams, and the most recently connected viewer is closed. A 9th v3 device with `lastProtocolVersion` 3 gets the notice-only connection.
- **Privacy mode:**
  - It stops video to every session, including a v1 phone that is streaming (it is closed).
  - v2 sessions get the `"off"` presentation.
  - With "turn off when the last session ends" on, the sensor stays on.
- **Replacement:** the same device reconnecting replaces only its own session.
- **9th session:** a 9th v2 session is closed before hello.
- **Tiers:**
  - each tier sends its `0x02` before its IDR;
  - epochs are unique across tiers;
  - a tier switch with frames in flight decodes cleanly;
  - a slow session doesn't force IDRs on others faster than every 500 ms;
  - opening `low` doesn't stall `high`.
- **Freeze:** with the view frozen, a new v3 session gets `0x02` and an IDR.
- **Replay:** during a PC replay, a viewer is unavailable with `pc_replaying` and an owner still gets the frames.
- **Migration and downgrade** (`pet-camera.md` §5):
  - the existing pin becomes owner "iPhone";
  - an unpair on a downgraded build, followed by an upgrade, leaves that device refused;
  - a crash between the two file writes doesn't re-admit a revoked device, and doesn't drop a listed device. Three cases are tested: revoking the last device (`serviceId` rotates), revoking all devices, and pairing the first owner;
  - after the owner is revoked, a remaining viewer still connects and `_kv2s` is still advertised.
- **Re-scoped tests.** These `test_stream_e2e.cpp` assertions were re-scoped for v3, not deleted:
  - the hello and `0x07` version checks at `:454`, `:630`, `:652` and `:662` (now `== 3` from the host, still read correctly by the v1/v2 fakes);
  - the "unknown type" sends at `:790-791`, which use `0x0B` and `0x0C`. They move to unused bytes (`0x7E`, `0x1A`+), plus a separate check that a v2 connection's `0x0C` is ignored under §1 rule 3;
  - "only one phone is pinned" at `:551-565` (now "a second pairing adds a device").
- **Golden sentinel:** the golden test's unknown-type sentinel moves to `0x7E`.
- **Isolation:** a `--synthetic` launch with a `pet-camera.json` that has everything on opens no roll folder, makes no UPnP call and loads no real devices.

**Camera roll:** see `pet-camera.md` §8.

**Outside the home:**
- **Admission:**
  - With no invite open, an unpaired certificate is refused before any application byte.
  - An open invite admits a redemption-only connection, which gets no hello and can do nothing else.
- **Invites:**
  - An expired invite fails, and so does a used one.
  - A wrong proof is counted, and the tenth burns the invite.
  - A certificate mismatch burns it and gets no reply.
  - `methodRaw 0/1` on the outside listener, and `2` on the LAN pairing listener, are refused without burning anything.
- **Proof:** a `methodRaw 2` golden proof vector.
- **Rate limits:**
  - The per-source limit trips before TLS.
  - Two different IPv4-mapped sources (`::ffff:203.0.113.1`, `::ffff:203.0.113.2`) get separate buckets. This is a unit test of the limiter's key function, because a loopback listener never produces mapped addresses.
  - A blocked source stays blocked.
  - A paired device's handshake refills its source's bucket and clears none of its failures (§8.5).
- **Local-source filter**, as a unit test on the address classifier: `::ffff:192.168.1.5` is accepted inside a home prefix and refused outside one, an on-link global IPv6 address is accepted, and a public address is refused. The home prefixes come only from physical Ethernet and Wi-Fi adapters with a gateway; a VPN holding the default route leaves home as it is, and a virtual adapter's network is refused.
- **Router**, against a fake `IUPnPNAT` seam (no test touches a real router):
  - the mapping is reused, taken, re-added after it disappears, and removed when outside listening turns off;
  - a WAN address of `0.0.0.0` reports `error`, and `100.64.x` reports `carrier_nat`.
- **Endpoint link** (in the device fakes): a matching link replaces the stored endpoint only after a pinned handshake at the new address succeeds. A link to an address that fails keeps the old endpoint.

---

## 11. Decisions this spec makes (for review)

**D1. The role is chosen on the PC** when it opens the pairing window or creates an invite, not in a prompt after the device has paired. Holding back `0x11` until someone clicks would break old phones, which read the response once.
- The device list can change a role at any time.
- The default is **Viewer**. To pair their own phone again, the owner uses **Re-pair** on its row, which keeps its role.

**D2. With "Share the camera" off, owner devices still watch on the home network,** as today. Viewers get nothing and the outside listener is closed. The owner's existing phone is unchanged until they turn something on.

**D3. Privacy mode stops everything visual from leaving the PC, for every role:** live video, playback and thumbnails. It also stops recording. It keeps listing, commands and the sensor, so an owner can turn it off from a device. This follows the plan's "stops streaming and recording at once".

**D4. The camera roll records the shown view at the `medium` tier**, reusing the live encode. Colour 1080p is recorded as 1280×720 at 2.5 Mbps. Clips are then cheap to play from outside the home, and the roll needs no view or encoder of its own.
- The alternative is a dedicated full-size colour encoder that ignores the PC's view.

**D5. The "Record to the camera roll" master switch gates every trigger,** manual included. Off means nothing is ever written.

**D6. "Turn the Kinect off when the iPhone disconnects"** now means the last available session ends. It is skipped while the camera roll is on, so motion recording doesn't end whenever nobody is watching.

**D7. Playback uses `0x18`/`0x19`**, which carry the `0x02`/`0x03` bodies behind a stream id. The reason is in §5.9.

**D8. Limits:**
- 4 watcher slots, counting live and playback, with owners able to pre-empt viewers;
- 8 sessions;
- 16 devices;
- invites last 10 minutes and work once;
- the rate limits in §8.5.

**D9. These stay on the PC:** outside listening, schedules, retention, the roll folder, pairing, re-pairing and invites, so a lost owner phone can't change them. Owner devices get sharing, privacy, roll on/off and motion. This narrows the plan's "recording settings" for owner devices.

**D10. "Code entry" for invites means pasting the invite text.** No typeable short code: it couldn't carry the host fingerprint and address safely.

**D11. Viewer stars keep clips past retention, as the plan says.** The consequence is that a viewer can push the roll to `storage_full`. The safeguards are:
- stars record who set them;
- a viewer can unstar only its own stars;
- revoking a device drops its stars.

The owner may choose instead that only owner stars count for retention.
