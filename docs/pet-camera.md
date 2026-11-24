# Pet camera (Windows host)

On Windows, the Kinect viewer can be a pet camera:
- watch it live from anywhere, when the camera is on and sharing is turned on in the PC app;
- record clips into a dated camera roll stored on this PC;
- browse and play the camera roll from the PC, Mac and iOS apps.

This is the feature's design, with the PC side in full. The wire protocol is in [`protocol-v3-pet-camera.md`](protocol-v3-pet-camera.md), called "v3" below. Where the two overlap, v3 is the contract and this document points to it. Decisions D1–D11 are listed in v3 §11.

## 1. The decision and the hard rules

**The owner's decision.** For the pet camera feature only, colour may be:
- streamed to paired devices as H.264, including over the internet;
- recorded to the camera roll on this PC, as H.264 MP4 clips and JPEG thumbnails in the camera-roll folder only.

This lifts "colour is live only" and "nothing leaves the capturing machine" **for this feature only**. Everything else stays as it is:
- the depth/IR recorder and its folder, retention, purge and reminder;
- the in-use lock;
- the screenshot hook's refusals;
- the additive protocol, with v1/v2 phones still working.

Raw colour, depth and IR frames and point clouds still never leave the PC; only the encoded picture does.

**Hard rules:**
- **The stream comes only from this PC.** No third-party servers, cloud, relays, TURN, Tailscale-style coordination, analytics, or "what's my IP" service. A dynamic-DNS name goes in only if the owner later chooses one.
- **Everything is off by default:** "Share the camera", "Allow viewing from outside the home" and "Record to the camera roll".
  - Each is turned on from the PC app.
  - Owner devices can also toggle sharing and recording, but not outside the home (v3 D9).
- **Development never looks at real frames.** This is a camera inside someone's home.
  - Tests are synthetic only.
  - Development never opens, decodes, screenshots or describes a real colour, depth or IR frame, live or recorded.
  - Development never opens the real camera-roll folder.
  - Test runs use temp state and temp roll folders only (v3 §10, "Test isolation").
  - The owner does the real-camera checks (§10).
- **No new dependencies or installs.** Only Windows built-ins are used:
  - Media Foundation (`mfplat`, `mfreadwrite` for `IMFSinkWriter`/`IMFSourceReader`, and the H.264 MFTs already in use);
  - WIC for JPEG;
  - `NATUPnP` (`natupnp.h`, COM, `oleaut32` for `BSTR`/`VARIANT`) for the router;
  - the existing OpenSSL 3 TLS stack.

  `vcpkg.json` is unchanged.
- **Protocol changes are additive.** v1/v2 phones keep working (v3 §1 and §2.4).
- **The Mac and iOS half is built from the same spec,** in Swift (`shared/`, `macos/`, `ios/`).

## 2. Who and where

- **The PC** is the camera and the server.
- **Mac and iOS** are viewers. The Mac app gets a "Watch a camera" client mode, and its own host mode stays as it is.
- **Roles.** The full table is v3 §3.1.
  - **Owner** (the owner's devices): power, sharing, privacy, recording on/off and motion, deleting clips, revoking devices.
  - **Viewer** (devices the owner shares the camera with): watch live, browse and play the camera roll, star clips, and start or stop a manual clip. A viewer can't delete, change settings or manage devices.
- **Devices.** A device is a pinned client certificate, as today. Each has a name, a role, a "last seen" time and a revoke button on the PC.
- **Limits:**
  - up to 16 paired devices;
  - 4 watcher slots;
  - 8 sessions in all.

  Owners can take a viewer's place under either limit (v3 §2.2–§2.3).
- **The phone that's paired today** becomes owner device "iPhone" on the first v3 start. At home it keeps working as it does now (v3 §2.4).

## 3. Pairing and invites

### At home (the default)

The existing QR or 8-digit short-code pairing is unchanged on the wire. What changes is on the PC:
- **The pairing window has a role choice,** Viewer by default. It is chosen before the code shows, and can be changed until a device pairs (v3 D1).
- **QR only, unless the PC user asks for the code.** The window shows the QR code; the 8-digit code appears, and is taken, only after **Use a code instead** (`kinect-cli serve --pair-code`). Before that a code attempt is closed unread and burns nothing. The code is weaker than the QR (below and `SECURITY.md`), so it is never offered unasked.
- **A successful pairing adds a device.** Today it replaces the only pin.
- **Re-pair** on a device's row opens the pairing window for that device. On success it replaces that record's certificate and keeps its name and role. It is for a phone that lost its identity, after a reinstall or after "Unpair" on the phone. The pairing window also points to it: "Pairing your own phone again? Use Re-pair on its row."
- **Device names.** v3 devices send their name. Old phones get "iPhone", "iPhone 2" and so on, and every device can be renamed.
- **Stale owners.** The device list highlights owner records that haven't been seen for 30 days and offers to revoke them.

### Remote invite (optional, off by default)

- **When:** only while outside viewing is on and the PC knows the public address (v3 §8.4).
- **Form:** a one-time invite, shown as a QR code and as copyable text, with the role picked on the PC.
- **Lifetime:** 10 minutes, once, one open at a time. A wrong code is counted against the address it came from, and the tenth burns it.
- **Closing it on the PC while a phone is connecting** (Cancel, a new invite, or the home address changing) doesn't count against that phone: it is told "This invite was closed. Ask for a new one." and its failed attempts stay as they were. Only a phone that was let in because the invite was open gets this; a stranger that connects while no invite is open is still turned away and counted (v3 §8.5).
- **Contents:** the public address, the outside port, the LAN address, the host certificate fingerprint, and a 128-bit one-time token.
- **"Code entry" on the devices** means pasting that text (v3 D10).
- **Pairing over the internet** happens only through an unexpired invite.
- **Warning on the PC:** "Send this only to the person you're inviting."

### Re-share after an address change

The PC shows an **endpoint link** (`kv2://endpoint?...`, v3 §8.4). It holds only the host's id, fingerprint and new address, with no secret. A new address that the gateway pinned when outside viewing was turned on confirms itself reaches devices by itself (v3 §8.3); while the gateway can't be read, the PC keeps checking ("Checking the router…") and devices keep the address they had. Only an address the gateway has confirmed is ever re-shared: one the gateway contradicts makes the PC say another device answered as the router, and is offered only once the gateway agrees. One the gateway can't confirm, or one through another gateway, isn't offered at all; turning outside viewing off and on starts afresh with the gateway there then, which is also what a replaced router needs. Something on the home network answering as the router could report any address.
- **Paired devices away from home** import it. The device tries the new address and keeps it only after a handshake pinned to the PC succeeds there (v3 §8.4 says what a relay can still do).
- **Devices at home** learn the new address on their own, once the re-share has been closed.
- **New invites** wait for the re-share too.

## 4. The PC app

A new **Pet camera** section in the side panel, and a "Watching now" banner over the view.

| Control | Default | Notes |
|---|---|---|
| Share the camera | off | Off means only owner devices watch, and only at home. Viewers get nothing. Turning it off also turns outside listening off (v3 D2, §5.2). |
| Sharing schedule | none | Optional rows ("days, from–to") during which sharing is off. Owners aren't affected. |
| Allow viewing from outside the home | off | PC only, and needs sharing on. Shows the status (mapped / manual steps / carrier NAT / address changed / error), the router line (what the gateway the forward is on is recognised by, while it forwards the port, and "Checking the router…" while a new address waits for it; v3 §8.3), the port with "Change port", an optional "Public address" field for the manual case, and "Remove mapping now" (enabled while the router holds this PC's forward: it removes the forward and leaves the listener open, so the status becomes manual). **The forward then stays removed**, even across a restart (saved as `outsideMappingRemoved` in `pet-camera.json`; a value that isn't true or false reads as removed): the 10-minute check doesn't add it back. It comes back only when the switch is turned off and on again, or with **"Map the port again"**, which is enabled exactly while it is removed (and "Remove mapping now" is disabled meanwhile). v3 §8.7. |
| Privacy mode | off | A large button. Stops live video, playback, thumbnails and recording at once, owners included. Saved across restarts (v3 §6.4, D3). |
| Record to the camera roll | off | The master switch for every trigger (v3 D5). |
| Motion (by depth) | off | Sensitivity 1–10, default 5. Greyed out, with the reason, while the live sensor isn't running or the PC is replaying a recording (`motionAvailable`, §6.2). Depth is decoded in every view mode, so the view itself never greys it out. |
| Scheduled recording | none | Rows of days, start time and length. |
| Camera roll folder | `%USERPROFILE%\kinect-viewer-camera-roll\`. You can choose any folder with "Change…". | "Change…" refuses OneDrive, the depth/IR recordings folder and network drives. |
| Keep | 30 days, 200 GB | Starred clips are kept. Shows used space, free space and the 20 GB floor. Until the limits are the owner's (§6.4), it says "Retention starts when you confirm the limits." with a **Confirm** button; turning the roll on beside them confirms them too. |
| Devices… | | The list shows name, role, kind, paired date, last seen and connected (home/outside). Actions: **Rename**, **Role**, **Re-pair**, **Revoke** (with a confirm dialog), **Pair a device…**. |
| Invite from outside… | | The QR code, copyable text and a 10-minute countdown. Disabled, with the reason, when no public address is known. |
| Re-share address… | | The endpoint link as a QR code and as text, offered after an address change. |
| Connection log… | | The last 30 days (v3 §8.6). On the PC only. |

- **"Watching now: <device names>"** shows over the view, for example "Watching now: iPhone, iPhone 2 (outside the home)", whenever any session holds a watcher slot (live or playback). It can't be hidden while anyone watches.
- **The camera roll browser** shows days, thumbnails, play, star and delete.
  - Clips play in **their own window**, decoded locally with `IMFSourceReader`.
  - Nothing from this player ever reaches the encoder, `hooks->onFrame` or the camera roll.
  - While a depth/IR recording replays in the main view, viewers are unavailable (`pc_replaying`, v3 §2.3).
- **The existing "Allow iPhone mode switch" and "Allow iPhone to turn the Kinect on and off"** now apply to owner devices only.
- **"Turn the Kinect off when the iPhone disconnects"** acts when the last *available* session ends. It is skipped while the camera roll is on and usable (its folder set, accepted and not failed), and the panel says so exactly then: one predicate serves both (v3 §2.3, D6).

## 5. Storage of settings and devices

All the files live in `%USERPROFILE%\kinect-viewer\.host-state\`, unless a test passes `--state-dir`. Each is owner-only (protected DACL) and is written atomically (`.tmp` then `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`), as `pairing.json` is today (`host_state.cpp:92-192`).

| File | Holds |
|---|---|
| `pairing.json` | Its keys are unchanged: identity, `serviceId`, and the three phone settings, except that the identity's private key is kept wrapped (below). `pinnedClientCertSHA256` becomes a **downgrade mirror**: the hash of the most recently paired (or re-paired) owner device, or no key at all when there is no owner. It changes only when a device is paired, re-paired, revoked or has its role changed, never on a routine connect. Only the start-up reconciliation (below) reads it. |
| `devices.json` | `{"version":1,"written":{"serviceId":"…","mirror":<b64?>},"before":{"serviceId":"…","mirror":<b64?>},"devices":[…],"revoked":[<b64>,…]}`. `written` holds the values this save is about to put in `pairing.json`, and `before` the values `pairing.json` held before it. Each device is `{"certSHA256":<b64>,"deviceId":"<16 hex>","name":"…","role":"owner\|viewer","kind":"iphone\|ipad\|mac\|other","pairedAtUnixMs":u64,"lastSeenUnixMs":u64?,"lastProtocolVersion":int?}`. `revoked` lists the hashes of revoked devices, at most 256 with the oldest dropped first. Pairing a revoked hash again removes it from `revoked`. |
| `pet-camera.json` | The §4 settings: sharing, sharing schedule, outside on/off, outside port, entered public address, whether the PC user removed the router forward (`outsideMappingRemoved`), privacy mode, roll on/off, folder, keep days, cap, motion on and sensitivity, and the scheduled-recording rows. |
| `connection-log.jsonl` | v3 §8.6 |

**This PC's private key at rest.** `pairing.json` holds the key wrapped with DPAPI for this PC, as `serverIdentity.privateKeyWrapped`: machine scope (`CRYPTPROTECT_LOCAL_MACHINE`) with this app's own entropy. A copy of the folder alone, taken to another PC or Windows install, holds no key that can be used; a full image of the system disk carries this PC's DPAPI secrets with it. On this PC the folder's owner-only ACL keeps non-admin users from reading the file; an administrator can read it.
- **Migration.** A `pairing.json` that holds the key plain (`privateKeyRaw`, written while DPAPI wasn't working: see Saves) is migrated by the first start after a load that worked:
  1. The key is wrapped, and the wrap is unwrapped and compared in memory.
  2. `pairing.json` is written with both keys, plain and wrapped.
  3. That file is read back from disk, and its wrapped key is unwrapped and compared with the plain one beside it.
  4. Only then is `pairing.json` written with the wrapped key alone, and read back and checked again.

  A step that fails leaves a file that still holds the plain key. The host starts with it as before (reachability comes first), the reconcile log says why, and the next start tries again. A file that is already wrapped is never rewritten by the migration, so running it again changes nothing. A file left with both keys is finished from its plain key.
- **Saves.** Every later write of `pairing.json` (a setting, a pairing, a repair, a reset) writes the key wrapped. The new wrap is unwrapped and compared before the write, and the file is read back after it. When the key can't be wrapped (DPAPI isn't working) and `pairing.json` is missing or still holds the key plain, the save writes it plain, so a phone can still pair and a setting still saves (reachability first); the next start's migration wraps it. Over a file that holds the key wrapped, a save never writes it plain: it fails instead. That holds when another start of the host on the same folder wraps the key between the save's check and its write: the plain save replaces `pairing.json` keeping what it replaced, finds the wrap there, puts it back as it was, and fails; and a first run's plain save never replaces a `pairing.json` written meanwhile. A replace that Windows fails midway (a program holding the file) never leaves the folder without `pairing.json`: what it moved to `pairing.json.prev` goes back at once. A start sorts out what an interrupted write left before it loads: a `pairing.json.prev` with no `pairing.json` is put back, so the folder is never read as a first run; one beside a `pairing.json` is removed, as are the temp files of writes that never finished.
- **A key that can't be unwrapped** fails closed, as a file that can't be parsed does (below). The error says why, by the error DPAPI gives:
  - **A data error** (`ERROR_INVALID_DATA`, `ERROR_INVALID_PARAMETER`, `NTE_BAD_DATA`, `NTE_BAD_KEY`, `NTE_BAD_KEY_STATE`): the wrap is damaged, has other entropy, or was made on another PC or Windows install. The error says resetting this PC's identity makes a new one.
  - **Anything else** (out of memory, the DPAPI service not answering): it may pass. The error says to try again, and to reset only if it still fails after a restart, so a passing fault doesn't make every device pair again.
- **Plain copies.** A `pairing.json.bad` copy of a plain file, a backup, or disk space freed by plain writes may still hold the key plain. The migration leaves `.bad` copies as they are; the owner can delete them.

**Reading.** The new files are read leniently. A bad entry, an unknown key or a wrong type is skipped or replaced by its default, and never fails the load. A missing `pairing.json` means a first run, and a new identity is made. One that exists but can't be read or parsed fails closed (below). A bad `devices.json` never makes a new identity either.

**When a file can't be read, the host fails closed.** This overrides the lenient reading above for these cases:
- **Missing means "not found".** Only "file not found" or "path not found" means a state file is missing. Any other error while checking for it, or a folder where the file should be, means it exists but can't be read.
- **`pairing.json`** (this PC's identity) exists but can't be read or parsed, holds a wrapped key that can't be unwrapped on this PC, or holds a key and certificate that can't be used:
  - The host keeps a `pairing.json.bad` copy and checks that the copy worked; the error says if it didn't.
  - It makes no new identity, and doesn't write, rename or delete `pairing.json`.
  - It doesn't serve: there is no listener, no advertisement and no pairing window. `kinect-cli serve` fails the same way.
  - The PC's Pet camera panel shows the reason with two buttons: **Try again**, for a file another program was holding, and **Reset this PC's identity (all devices must pair again)…**, after a confirm step.
  - The reset is the only way to a new identity. It loads `pairing.json` again first: if the file can be used now (another program let go of it, or a good copy was put back), nothing is reset and the host just starts with it.
  - Otherwise the reset keeps the old file as `pairing.json.bad`. If that copy fails while the file can still be read, the reset is refused with the error, so the old identity is never written over without a copy.
  - It then writes a new identity with a new `serviceId` and a first run's phone settings. The new `serviceId` makes the device list drop every device (case 4a below), so each must pair again. `pet-camera.json` is kept.
- **`pet-camera.json`** exists but can't be read or parsed: the host starts with privacy mode **on** and shows the error. It keeps a `.bad` copy of a file that was read but didn't parse. A missing file still means the defaults.
  - A `privacyMode` that is present but isn't a boolean (`"no"`, `0`, `null`, an object) means **on**.
  - While the file can't be read, a change that would have to be written over it is refused: privacy off, sharing and the schedule. The PC shows why, and a phone gets `0x0D` `internal`.
  - Privacy **on** is still accepted then. It is kept in `privacy-on.pending` (owner-only, written atomically) until `pet-camera.json` is written again, so a restart that reads an older "off" can't undo it. While that marker exists, or can't be checked, privacy is on whatever the file says. The next successful `pet-camera.json` write removes it. A later privacy off by the owner is then kept.
- **`devices.json`** exists but can't be read (an I/O failure such as a sharing lock): that isn't corruption. The host doesn't save over it, admits no device, shows the error and retries the read. Only a file that was read but didn't parse is set aside as `.bad`, and only once that copy succeeds.
  - Until it can be read, no device can pair, and revoke, Revoke all, rename and role changes are refused. The PC says "Can't change devices until the device list can be read." with the read error.
- **`.bad` copies never overwrite each other.** The newest is `<file>.bad`. Older copies move up to `.bad.1` … `.bad.4`, and the oldest beyond those five is dropped. A file that one of the kept copies already holds isn't copied again. Each copy is made to `<file>.bad.tmp` first, so a copy that fails moves nothing.
- **Folder check:** the host writes a `.kinect-viewer-host-state` marker into its state folder. A test's `--state-dir` must be new, empty, or a folder that already holds the marker or a `pairing.json` that parses as host state. Reparse points are refused.

**Write order.**
1. Write `devices.json` first: the change, any new `revoked` entry, `written` (the values `pairing.json` is about to get) and `before` (its current values).
2. Then write `pairing.json`.

A crash between the two writes leaves `devices.json` as the truth, and the next start repairs `pairing.json`.

**Reconciling on every start.** Read `pairing.json` (pin P, service id S) and `devices.json`, then check these cases in order:

| # | Situation | What it means | Action |
|---|---|---|---|
| 1 | No `devices.json` | First v3 start (migration) | If P is present, it becomes owner device "iPhone" (`kind: "iphone"`, `pairedAtUnixMs` = now). Otherwise the list starts empty. |
| 2 | (P, S) = `written` | Consistent | Nothing. |
| 3 | (P, S) = `before` | The save was interrupted between the two writes | Rewrite `pairing.json` from `written`. Change nothing else. |
| 4 | Anything else: a downgraded build changed `pairing.json` | | Apply 4a–4c, then rewrite `pairing.json` and log what happened. |
| 4a | S ≠ `written.serviceId` | The old build unpaired (it rotates `serviceId`, `stream_server.cpp:260-271`) | Drop every device. If P is present and not in `revoked`, add it as owner "iPhone". |
| 4b | S unchanged, P present and different | The old build paired a new phone (it replaces the pin) | If P is in `revoked`, ignore it. If P is already a listed device, change nothing. Otherwise drop the device whose hash is `written.mirror` and add P as owner "iPhone". |
| 4c | S unchanged, P absent | The pin was cleared without a `serviceId` change | Treat it as "unpair all": drop every device. |

A hash in `revoked` is never admitted again by reconciliation; only a new pairing on the PC removes it from the list.

**Updating `lastSeenUnixMs` and `lastProtocolVersion`.** Both are updated on connect and disconnect, and saved at most every 10 minutes and on exit.

## 6. The camera roll

### 6.1 What is recorded, and how

**What goes in a clip.** Clips are the **live sensor's** shown view, encoded once at the `medium` tier (v3 §6.1, D4):
- colour: 1280×720 at 2.5 Mbps;
- IR or depth: 1024×848 at 2.5 Mbps.

**`medium` runs whenever the roll is armed.** While "Record to the camera roll" is on, the `medium` encoder runs even when nobody watches, so the pre-roll exists.

**Pre-roll ring.** The host keeps the last **6 s** of `medium` output in memory (about 2 MB). It keeps it in the encoder's **Annex-B** form, before `annexBToAvcc`, and puts SPS/PPS in front of every IDR. The ring always starts at an IDR.

**The writer.** `IMFSinkWriter` writes the `.mp4` in **pass-through**, with no encoding:
- **Input type:** `MFVideoFormat_H264`, with `MF_MT_FRAME_SIZE`, `MF_MT_FRAME_RATE` and `MF_MT_MPEG_SEQUENCE_HEADER` set to the Annex-B SPS+PPS taken from the clip's first IDR.
- **Sample times:** each sample gets a time (the first sample is 0) and a duration, from each frame's capture time. These are real timestamps.
- **Sync samples:** IDR samples set `MFSampleExtension_CleanPoint = 1`, so each fragment's sample flags mark the keyframes for seeks (fragmented MP4 has no single sync-sample table).
- **Creation time:** the file's creation time is the clip's start.

**Container:** fragmented MP4 (`MF_TRANSCODE_CONTAINERTYPE = MFTranscodeContainerType_FMPEG4`, built into Windows), so a clip interrupted by a crash or power loss stays readable up to its last fragment. `test_roll_media.cpp` proves that `IMFSourceReader` reads and seeks it.

**Playback over the protocol.**
- `IMFSourceReader` reads native samples.
- They are repacked with `annexBToAvcc`.
- `0x18`'s `parameterSets` come from the reader's native media type.

**Changing the view.** A clip has one size. A change of mode or size finalizes the current clip, and if a trigger is still active, starts a new clip at once (no pre-roll).

**Thumbnail:**
- **Source:** the live BGRA frame about 1 s after the trigger (for schedule clips, 1 s after the start). The pre-roll exists only in encoded form, and nothing is decoded.
- **Format:** scaled to at most 320 px wide, JPEG through WIC at quality 0.75, at most 64 KiB (the quality is lowered until it fits).

**Finalizing a clip** (MP4, then the sidecar, thumbnail and index) happens when:
- the clip's last trigger ends;
- it reaches 10 minutes (the segment cap, below);
- privacy mode turns on;
- the disk floor is reached;
- the sensor turns off;
- the view freezes;
- the PC starts replaying;
- the app exits.

### 6.2 Triggers

**What every trigger needs:**
- "Record to the camera roll" on;
- privacy mode off;
- the sensor on;
- the view not frozen;
- the PC not replaying.

**When one of these is missing:**
- `rollBlockedReason` says which one (v3 §5.1).
- `start_clip` is refused with that code, and adds no trigger.
- The block finalizes any clip in progress and drops every trigger (manual, motion and schedule). It also clears the motion cooldown.
- When the block clears, a schedule row that is still inside its window starts a new clip at once, with no pre-roll. Motion starts clips again as soon as it detects something. A manual clip has to be started again.

**Triggers are a set.** A clip runs while at least one trigger is active, and ends when none is:
- Its name and `trigger` are those of the trigger that started it.
- A trigger that fires during a clip joins the set and extends the clip.
- **Every clip is cut into segments of at most 10 minutes.** Pre-roll counts towards the 10 minutes: a segment file is at most 10 minutes from its first frame. When a segment ends with triggers still active, the next segment starts at the next keyframe, which the host requests at once.

| Trigger | Active from | Until | Notes |
|---|---|---|---|
| Manual | The PC button, or `0x0C start_clip` from an owner, or from a viewer while sharing is on | `stop_clip`, the PC button, or 10 minutes after the clip's first frame | Includes up to 5 s of pre-roll when it starts a clip. Its 10 minutes count from the clip's first frame, pre-roll included, the same as the segment cut, so a full manual clip is one file with no stub after it. A second press restarts them. |
| Motion (by depth) | Motion detected (below) | 10 s after the last motion | Pre-roll of 5 s. A 30 s cooldown follows the end of a clip, during which motion can't *start* a new clip (it can still extend one). |
| Schedule | A row's start time (PC local clock) | The row's length (1–60 minutes) | |

**Motion by depth.** Depth doesn't change with the light, so lamps and daylight don't trip it.
- **Input:** the 512×424 depth in mm from `kv_engine_copy_latest_raw16`, polled with each pass of the viewer's loop (as the depth/IR recorder is), with its frame sequence number. Depth is decoded in every view mode, colour included, and keeps coming while the view is frozen.
  - Motion is offered only while the live sensor runs, never from a replay. While the sensor is off, `motionAvailable` is false, the PC greys out Motion, and the reason is shown.
  - A frame after a dropped one (its sequence number isn't the last plus 1) never completes a motion run: the 3-frame count starts again.
- **Grid:** reduce the depth to a 64×53 grid of block medians, ignoring zeros.
- **Background:** each cell keeps a slowly adapting background (an exponential average, α = 0.02 per frame).
- **Changed cell:** a cell counts as changed when `|d − bg| > max(40 mm, 3 % of bg)`.
- **Motion:** at least `N` changed cells in each of 3 consecutive frames. `N` is 2 % of the valid cells at sensitivity 1 and 0.2 % at sensitivity 10, interpolated linearly in between.
- **Re-learning:** changed cells don't update the background.
  - **Per cell:** a cell that has stayed changed but still (within `max(40 mm, 3 %)` of the value it settled at) for 60 s takes that value as its background, so a pet lying down or a moved chair stops counting as motion.
  - **Whole frame:** when more than 60 % of the valid cells change at once and hold for the 3 frames, the camera was moved or the scene changed. The whole background re-learns at once, and it is not motion.
  - Starting the sensor, changing the cut-off and unfreezing all reset it.
- **Testing:** the detector is a pure function over depth arrays, so it is tested on synthetic depth alone.

### 6.3 Layout on disk

```
<camera roll>\
  .nosync
  .metadata_never_index
  deletion-log.txt
  2026-11-18\
    index.json
    14-03-22_motion.mp4
    14-03-22_motion.json
    14-03-22_motion.jpg
    14-05-40_manual.mp4
    …
```

**Names.** Day folders are `YYYY-MM-DD`, and clips are `HH-MM-SS_<trigger>`, in PC-local time at the clip's first frame (pre-roll included). A taken name gets `-2`, `-3` and so on. The clip id is `YYYY-MM-DD/HH-MM-SS_<trigger>[-n]` (v3 §7.1).

**The sidecar `.json`** is written last, so a clip with a sidecar is complete:

```json
{"version":1,"clipId":"…","startUnixMs":0,"endUnixMs":0,"startLocal":"2026-11-18T14:03:22-05:00",
 "durationSec":0.0,"trigger":"motion","startedBy":{"deviceId":"…","deviceName":"…"},
 "starred":false,"starredBy":[{"deviceId":"…","role":"owner"}],
 "width":1280,"height":720,"modeRaw":1,"sbsLayoutRaw":0,"bytes":0,"codec":"h264","preRollSec":5.0}
```

**Sidecar fields:**
- `startedBy` is left out for motion and schedule clips. For the PC it is `{"deviceName":"This PC"}`.
- `starred` is true while `starredBy` is not empty. The PC counts as an owner with `deviceId` `"pc"`.
- Starring and unstarring rewrite only the sidecar.
- Starring adds the caller's entry. An owner's or the PC's unstar clears every entry. A viewer's unstar removes only its own entry (v3 §5.2).
- Revoking a device removes its entries (v3 §3.2).

**`index.json` per day** is a cache of that day's sidecars. It is rebuilt from the sidecars when it is missing, unreadable or older than any sidecar, by the host's start-up and retention passes, never because a device asked.

**Leftovers.** An `.mp4` with no sidecar is an interrupted clip. At start-up it gets its sidecar rebuilt from the file if `IMFSourceReader` can read it (fragmented MP4). Otherwise it is deleted and logged.

### 6.4 Retention and the disk floor

**Retention** runs every hour and after each clip, and once at start-up after `pet-camera.json` has been read. It runs only with the owner's own limits. They are known only once:
- a `pet-camera.json` that holds the roll's limits (`rollKeepDays` or `rollCapBytes`) has been read successfully; or
- the owner has set them on the PC, pressed **Confirm** beside them, or turned the roll on at the PC, where the limits are shown beside the switch. Each of these writes them, and retention runs with them at once, without a restart.

A file without the roll keys doesn't count, and neither does a missing or unreadable one. Until the limits are known:
- retention doesn't run at all, and the defaults never delete a clip;
- the host never writes the limits into `pet-camera.json`, so a later start can't take the defaults for the owner's;
- `storage_full` is never raised against the default cap; only the disk floor below stops recording;
- the PC panel says "Retention starts when you confirm the limits." with a **Confirm** button.

When it runs:
1. Delete unstarred clips older than **keep days** (default 30).
2. While the roll is over the **cap** (default 200 GB), delete the oldest unstarred clips.
3. **Starred clips are never deleted automatically** (v3 D11). If starred clips alone go over the cap, recording stops with `storage_full`.
4. Remove empty day folders.
5. Log every deletion in `deletion-log.txt`, in the same format as the depth/IR log.

**The disk floor:**
- **Checks:** before each clip, and every 10 s while recording, the host checks free space on the roll's volume (`GetDiskFreeSpaceExW`). Tests replace this check with a seam.
- **Below 20 GB free** (20,000,000,000 bytes): it finalizes the clip, refuses new ones, and shows "Recording stopped: less than 20 GB is free on the PC's disk." (`disk_floor`).
- **Resuming:** it resumes on its own above 21 GB.
- **No extra deletions:** the floor never makes retention delete anything beyond the cap and the day limit.

### 6.5 Folder protection

The camera roll gets the same protections as the depth/IR folder (`recorder.cpp:64-97`):
- it refuses OneDrive paths;
- an owner-only protected DACL;
- `FILE_ATTRIBUTE_NOT_CONTENT_INDEXED`;
- the `.nosync` and `.metadata_never_index` markers.

**Folders it refuses:**
- the depth/IR recordings folder itself;
- any folder inside the depth/IR recordings folder, whose purge deletes anything that isn't an IR or depth session;
- a network drive.

**When.** The host checks the folder at start without changing it, and prepares it (creates it and adds these protections) the first time the roll is on, once `pet-camera.json` has been read and privacy mode is off. While the folder is being prepared, the roll reports `roll_preparing` (v3 §5.1). A folder that can't be prepared leaves the roll off with `folder_unavailable` (v3 §5.1).

**While the roll is off**, the host never creates or prepares a folder. A folder that is already a prepared roll (it holds `.nosync`) is still kept tidy: its start-up pass runs (leftovers recovered or deleted, indexes rebuilt), and retention runs with the owner's limits once they are known (§6.4). It stays browsable on the PC and on devices, and its clips can be streamed for playback, never downloaded.

**Test runs** (a `--synthetic` or test-hook launch of the viewer; `kinect-cli serve` with `--synthetic` or `--loopback-only`): the folder must be new, empty, or hold the `.kinect-viewer-test-roll` marker, which the host writes when it prepares a test roll. Anything else, a real roll included, is refused as `folder_unavailable` with the reason, without being opened for writing. The real folders are also refused by their path alone, as a second line. In a test run the recorder refuses every frame that isn't from the synthetic source: the roll stops with the reason and keeps nothing of the clip in progress.

**Backups.** Excluding the folder from File History and Windows Backup stays a manual step, as for the depth/IR folder (`windows-setup.md`).

### 6.6 Where the colour exception applies, exactly

The new camera-roll code is the only place colour is written: the writer, the thumbnailer and the index, in their own source files, into the camera-roll folder.

**What stays unchanged, with its tests:**
- `Recorder::canRecord` and its refusals;
- `recordingAllowed` and `kColourLiveOnly`;
- the depth/IR library and its purge;
- `screenshotAllowed`, which is extended to also refuse while a roll clip or a replay is on screen (v3 §10);
- the reminder text;
- the CLI's `RECORD_REFUSED` paths.

**Docs.** The README and `design.md` rules that say colour is never written (README "Colour is live only", and `design.md` §2 rules 2–4) each have one line pointing to this document for the camera-roll exception.

**Test hooks.** The camera-roll test hooks refuse the real source, as `screenshotAllowed` does. Tests write synthetic clips into temp folders only.

## 7. Outside the home

v3 §8 has the details. In short:
- **The listener:** a second TLS listener on one fixed port, with its own TLS context. Clients present pinned certificates, and anyone else is refused inside the handshake.
- **Router mapping:** a UPnP mapping through `NATUPnP`, re-checked every 10 minutes, with a manual port-forward fallback.
- **Carrier NAT:** detected from the router's WAN address and stated plainly.
- **Address changes:** once the pinned gateway confirms the new address, devices are told it by `0x0B` with no click; while it can't be read, the panel says "Checking the router…". Otherwise: "Your home address changed; re-share". Devices at home learn the new address from `0x0B` once the re-share has been closed, and devices away from home learn it from the endpoint link.
- **Hardening:**
  - rate limits before TLS;
  - blocking after repeated failures (timeouts don't count, and neither does an invite the PC closes while a phone is connecting);
  - local-only LAN listeners, with IPv4-mapped addresses unwrapped;
  - a 30-day connection log kept on the PC, with repeated lines coalesced.
- **Removal:** the mapping is removed when sharing or outside viewing is turned off, and on exit.

**Firewall.** The app adds no firewall rules, which would need admin rights. Traffic from the router reaches the PC on its home network, which `windows-setup.md` already requires to be **Private**, and the existing allow rule for `kinect-viewer.exe` covers it. The PC shows a hint if the active profile is Public, while outside viewing is on: it reads the connected networks' categories (`INetworkListManager`) and never changes them.

## 8. The parts and their tests

1. **Multi-viewer and roles.**
   - **Devices:** the device list, reconciliation and the mirror (§5), roles in pairing and on the wire, and revoke and re-pair.
   - **Protocol:** per-session `0x07`, plus `0x0B`/`0x0C`/`0x0D`/`0x0E`/`0x0F`.
   - **Video:** tiers with one thread per tier, the downscaler, the host-wide epoch, and per-session keyframes and configs.
   - **Sessions:** watcher slots with owner pre-emption, the 8/16 limits, "Watching now", privacy mode, share on/off with its schedule, and the replay and freeze rules.
   - **Infrastructure:** the `--state-dir` test isolation, and the PC UI for all of this.

   **Tests:** v3 §10, multi-viewer and roles.
2. **The camera roll.**
   - **Writing:** the pass-through fMP4 writer, sidecars, thumbnails and index.
   - **Triggers:** manual, motion and schedule.
   - **Housekeeping:** retention, the floor, and handling interrupted clips.
   - **Protocol:** list, thumbnail and playback (`0x12`–`0x19`).
   - **PC:** the roll browser with its own player window, and the roll settings.

   It sets `petCamera.cameraRoll: true` while the host has a camera roll it can use.

   **Tests** (synthetic and temp folders only):
   - **The file:** a clip from the synthetic source reads back through `IMFSourceReader` with the right frame count and timestamps. The number of sync samples equals the number of IDRs, and a seek lands on an IDR.
   - **Interrupted clips:** an interrupted (never finalized) clip is recovered, or deleted and logged if it can't be read.
   - **Sidecar and index:** the sidecar and index round-trip, and the index is rebuilt from sidecars.
   - **Retention:** it deletes the oldest clips first and keeps starred ones. The day limit works. A viewer can unstar only its own star, and revoking a device drops its stars.
   - **The floor:** it stops recording, simulated through the free-space seam, never by filling a disk.
   - **Motion:** the detector fires on a synthetic moving box, and not on IR or brightness changes or on noise.
   - **Triggers:** the trigger set works (manual extending a motion clip; `stop_clip` → `busy` for a motion or schedule clip with no manual trigger, and `ok`, a no-op, when nothing records), and so does the 10-minute segment cut.
   - **Validation:** clip ids, `day` and `cursor` are checked, and path traversal is refused.
   - **Playback over the protocol:** on a fake v3 device, every row of the v3 §5.9 table works; `0x18` arrives before `0x19`, the stream id is respected, and live video stops while playing.
   - **Blocked cases:** privacy mode stops playback and refuses thumbnails. A viewer can't delete.
   - **Real source:** the roll writer refuses the real source under test hooks.
3. **Outside the home.**
   - **The listener:** the outside listener and its own TLS context, on a configurable port.
   - **The router:** the UPnP flow with the manual fallback and the 10-minute re-check, the carrier-NAT check, and mapping removal when off.
   - **Access:** remote invites (`methodRaw 2`) and endpoint links, the local-only LAN filter, rate limits and blocking.
   - **The connection log.**

   It sets `petCamera.outside: true`.

   **Tests:** v3 §10, outside the home, with UPnP behind an interface so the tests use a fake router. No test touches the real router or opens a port beyond loopback.
4. **The Mac and iOS half** is built against the same spec (v3 §10, Mac and iOS).

## 9. Honest limits (known now)

- **Carrier NAT.** If the ISP uses carrier NAT, or there's a second router, viewing from outside the home **can't work** without a relay, and this feature doesn't use one. The PC says so instead of pretending.
- **IPv6, dynamic DNS and audio** aren't part of this round. IPv6 might reach the PC even behind carrier NAT.
- **IGD:2-only routers** show "manual". That has to be confirmed on the owner's router.
- **Mobile networks share addresses.** Many phones can sit behind one IPv4 address. Someone else's failed attempts can block that address for an hour, the owner's phone included.
- **Invites can be burned by others.** Anyone who can reach the outside port while an invite is open can burn it with ten wrong codes from several addresses (one address is blocked after five). The owner then makes a new one. An invite is also only as private as the channel it's sent through.
- **The short code stays as it is on the wire.** LAN short-code pairing keeps today's unpinned path, so a fake host on the home network could collect a proof and recover the code offline. The Windows host takes the code only in a window where the PC user chose **Use a code instead**; a key exchange over the code (SPAKE2 or CPace) is the later fix. Invites don't use that path.
- **One view for everyone.** Everyone watching sees the PC's current view, and a viewer can't pick a different mode.
- **Playback:**
  - Seeks are keyframe-accurate, to within 1 s.
  - Clips play back at the recorded `medium` quality with no re-encode, so a very slow link plays late rather than at lower quality.
- **Interrupted clips** keep only what reached their last fragment before a crash or power loss.
- **Motion by depth** needs depth to be decoded (§6.2). It sees moving shapes, not what they are, and anything inside the cut-off range counts.
- **Old phones retry once a second** while they are refused or unavailable (v3 §2.4). The cost is a TLS handshake a second, and the log coalesces it.
- **Global IPv6 at home.** If phones at home reach the PC over a global IPv6 address, the local-only filter accepts it only when it's inside one of the PC's on-link prefixes. The owner should check pairing still works on their network.
- **Real-world checks only the owner can do.** No test exercises the router mapping, the WAN address, mobile-data viewing or real-camera clips. Tests use a fake router and the synthetic source.

## 10. The owner's checklist at the end

1. Pair the owner's phone as owner and a second phone as viewer, at home.
2. Turn on sharing.
3. Turn on outside viewing, check the status says "mapped", and watch from mobile data.
4. Record a clip and play it on all three apps.
5. Check that the viewer phone can't delete clips, change settings or see the device list.
6. Revoke a device and check it's refused.
7. Turn on privacy mode, and check that all live video and playback stop at once on every device, and that nothing records.
