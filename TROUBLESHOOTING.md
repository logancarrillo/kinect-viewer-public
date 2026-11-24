# Troubleshooting

Start with the command-line check for your host:

```sh
kinect-cli list                       # Windows: is the Kinect on the bus, is it free, does libfreenect2 see it
kinect-cli bench --pipeline gl        # Windows: 60 s, PASS or FAIL with the reason
kinect-cli --duration 10              # Mac: 10 s on the Kinect, then a summary
```

Each prints plain `key=value` lines. When you ask for help, paste the whole output, with three
cautions:

- **The Mac's summary includes the sensor's serial number.** Delete the value after `serial=`.
  (Windows hides it unless you pass `--show-serial`.)
- **Never post pairing codes, `kv2://pair` QR links or invite links.** Anyone who has one while it's
  still valid can pair or join.
- **Never attach a picture, a recording or a screenshot from a real camera.** It is a camera in
  someone's home. A screenshot of `--synthetic` shows the same window with a test pattern, and
  `router-probe` already masks every address it prints.

## The sensor and USB

| It says | What to do |
|---|---|
| `kinect_usb_present=no`, or `NO_DEVICE: no Kinect v2 ... on the USB bus` | The computer doesn't see the Kinect at all. Check that the adapter has power and that its cable is in a USB 3 port, then try another port |
| `Kinect is on the bus but libfreenect2 cannot see it: check the driver is libusbK (Zadig).` | Windows still has its own driver on the Kinect. Swap it to **libusbK** with Zadig. Never UsbDk, which is reported to stop Windows 11 24H2 and later from booting |
| `LOCK_BLOCKED: ...`, or `lock=held by another program: ...` (exit 3) | Another program has the Kinect open, and the message names it. Close it. See [the lock](#the-lock), below |
| `BENCHMARK=FAIL ... sequence drops` | The depth stream is losing USB packets on that port. See [frames dropping](#frames-dropping), below |
| `START_FAILED: Requested OpenCL pipeline, but libfreenect2 was built without OpenCL support.` | Pick `gl` or `cpu`, or rebuild libfreenect2 with OpenCL (Mac only). A pipeline never falls back to another by itself; that's on purpose |
| `RECORD_REFUSED: ...` | `kinect-cli record` takes the synthetic source only, and never colour. Record the Kinect in the viewer, after its reminder |
| `STATE_DIR_REFUSED: ...` (exit 2) | `--state-dir` must be a new folder, an empty one, or one the host already keeps its state in. Drive roots, junctions and links are refused |

### Frames dropping

The viewers show *Frames are dropping. Try a different USB port, ideally a rear port directly on
the motherboard.* when more than 2% of depth or IR frames are lost in 10 seconds. The health display
(**H**) shows the fps of each stream, the drops per minute and the USB port.

On the one Windows PC tried so far, the Kinect sat behind a USB 3 controller on the motherboard's
chipset and depth and IR lost about half their frames, while colour, which uses a different kind of
USB transfer, was clean. libfreenect2's own Protonect showed the same packet losses, so the GPU and
the decoder weren't the cause. What to try:

1. A rear-panel USB 3 port wired to the CPU rather than the chipset, with no hub in between.
2. `kinect-cli bench --pipeline gl` again after each change.
3. libfreenect2's transfer settings, `LIBFREENECT2_IR_PACKETS` and `LIBFREENECT2_IR_TRANSFERS`.
   These haven't been tried with this project.

Microsoft's own SDK has been seen to find a Kinect on a chipset controller and get no frames at all,
so whether a given PC streams is known only once `bench` has run there. A
[hardware report](.github/ISSUE_TEMPLATE/hardware-report.md) of what worked on yours helps.

### The lock

Before opening the Kinect, each host checks `KINECT-IN-USE` in your home folder
(`%USERPROFILE%\KINECT-IN-USE`, `~/KINECT-IN-USE`). The file names the program holding the Kinect
and its process id.

- **On Windows** a lock whose process no longer exists is taken over by itself. A lock with no
  process id, or one whose process can't be checked, counts as held.
- **On the Mac** any lock written by another process counts as held, even if that process has
  ended. Once you're sure nothing is using the Kinect, delete the file.

## Building

- **`libfreenect2 isn't installed at FREENECT2_PREFIX=...`** or **`vcpkg isn't at
  KV_VCPKG_ROOT=...`** (Windows). `build.cmd` looks beside the checkout. Install them there, or set
  the variable ([docs/windows-setup.md](docs/windows-setup.md#build-and-test)).
- **`Visual Studio C++ build tools not found.`** Install the Visual Studio 18 2026 Build Tools with
  the C++ workload.
- **The first Windows build takes a long time.** vcpkg builds OpenSSL from source the first time.
- **The recording tests fail on Windows.** They need FFmpeg: `winget install --id Gyan.FFmpeg
  --scope user`, or set `KV_FFMPEG` to `ffmpeg.exe`'s full path.
- **Xcode can't find libfreenect2.** It looks in `~/libfreenect2`. If you installed it elsewhere,
  set `LIBFREENECT2_PREFIX` in `Config/Local.xcconfig`.
- **Mac recordings come out as PNG files, not `.mkv`.** The host looks for FFmpeg only in
  `/opt/homebrew/bin` and `/usr/local/bin`, and records a 16-bit PNG sequence without it. `brew
  install ffmpeg`.
- **The iPhone app builds only for the Simulator.** Set `KV_DEVELOPMENT_TEAM` in
  `Config/Local.xcconfig`. If iOS won't open the app, turn on **Developer Mode** (Settings › Privacy
  & Security). With free provisioning the app stops opening after 7 days; install it again.

## Pairing and the network

- **The iPhone can't find a Windows host.** The first run raised a Windows Firewall prompt: allow
  `kinect-viewer.exe` on **Private** networks, and set your Wi-Fi or Ethernet profile to Private
  (only for your own network). With a Public profile, the iPhone can't see the host's Bonjour name
  or reach it.
- **The Windows pairing window shows no 8-digit code.** It pairs by QR code unless you choose **Use
  a code instead**. The code is weaker than the QR code ([SECURITY.md](SECURITY.md)), so it's never
  offered unasked.
- **The pairing window closed.** It allows one attempt: a wrong code closes it, and so do 120
  seconds. Open a new one.
- **`This PC has 16 paired devices. Revoke one first.`** Revoke one in **Devices…**.
- **A phone that was paired is refused after a reinstall, or after Unpair on the phone.** It has a
  new identity. On Windows, use **Re-pair** on its row in **Devices…**: it keeps its name and role.
  On the Mac, pair it again.
- **An iPhone that used to connect to the Mac is refused.** The Mac host pins one iPhone at a time,
  and another was paired since. Pair this one again.

On the iPhone (and the Mac's **Watch a Camera**), these messages mean:

| The app says | What to do |
|---|---|
| *Kinect Viewer can't use your home network. Turn on Local Network in Settings → Privacy & Security → Local Network.* | As it says: without Local Network the app can't find or reach the host |
| *Wi-Fi is off for Kinect Viewer. Turn it on in Settings → Kinect Viewer.* | As it says |
| *iPhone is blocking cellular data for this app. Settings → Cellular → turn on Kinect Viewer.* | Needed only to watch a Windows host from outside the home |
| *Can't reach the PC. It may be off or asleep, sharing may be off, or its home address may have changed.* | Check the PC, and that the viewer is running there. A viewer device needs **Share the camera** on |
| *The PC's home address changed. Ask for the new link, or connect at home once.* | On the PC, **Re-share address…**, and open that link on the device; or connect once at home |
| *The camera refused this phone. It may be starting up — trying again shortly.* | Usually passes. If it doesn't, the device may have been revoked or the PC's identity reset: pair again |
| *This phone was removed from the camera* | It was revoked on the PC. Pair it again if that was a mistake |
| *The PC is busy right now. Trying again shortly.* | The PC's connection limiter turned it away before any TLS; it retries by itself |
| *Too many people are watching right now* / *Four devices are already watching.* | The PC allows 8 connections and 4 watching at once. Owners take a viewer's place |
| *Sharing is off on the PC.* / *Sharing is off at this time of day.* | Viewers watch only while sharing is on, outside its schedule |
| *Privacy mode is on. Turn it off on the PC or an owner's device.* | As it says |
| *The PC is playing a recording.* | Viewers wait while the PC replays a depth/IR recording |
| **Live View Paused During Screen Capture** | Stop screen recording or AirPlay mirroring. The app never shows the picture while the screen is being captured |

## Recording

- **Record is off.** *Colour is live only. Switch to IR or Depth to record.* Colour and Side by Side
  are never recorded, nor is the depth/colour blend. On Windows, Record is also off in 3D and during
  a playback.
- **A recording disappeared.** Depth and IR recordings are deleted after 7 days. Each deletion adds
  one line to `deletion-log.txt` in the recordings folder: the time, the folder name, its size, and
  whether it was automatic or yours.
- **Backups.** The Mac excludes the folder from Time Machine and iCloud itself. On Windows, add
  `%USERPROFILE%\kinect-viewer-recordings\` and the camera roll's folder to File History's or
  Windows Backup's exclusions by hand. The recorder refuses any folder inside OneDrive.

## The Windows pet camera

| The PC says | What it means |
|---|---|
| *Saving the paired devices failed (...); retrying.* or *Saving the pet-camera settings failed (...); retrying.* | Another program (a backup, a sync tool, an antivirus scan) holds the file, or the disk is full. The change is already in force; the host retries, backing off to every 5 seconds |
| Privacy mode is on after a restart, and the status line says the settings couldn't be read | `pet-camera.json` couldn't be read or parsed, so the host started with privacy mode on. One that didn't parse is kept as `pet-camera.json.bad`, and turning privacy mode off at the PC saves fresh settings. While the file can't be read at all, changes are refused and the host keeps trying |
| *Can't change devices until the device list can be read.* | `devices.json` is there but can't be read. Nothing is written over it, no device can connect, and the host keeps trying |
| A reason with **Try again** and **Reset this PC's identity (all devices must pair again)…** | `pairing.json` can't be used, so the host doesn't serve. Try again first: another program may have held the file. Reset makes a new identity, keeps the old file as `pairing.json.bad`, and every device must pair again |
| *Retention starts when you confirm the limits.* | Nothing in the camera roll is deleted until you press **Confirm** beside the limits (or turn the roll on at the PC) |
| *Recording stopped: less than 20 GB is free on the PC's disk.* | It starts again by itself above 21 GB free |
| *Recording stopped: the camera roll is full of starred clips.* | Starred clips are never deleted automatically. Unstar some, or raise the cap |
| *The camera roll folder on the PC can't be used.* | **Change…** refuses OneDrive, the depth/IR recordings folder and network drives |
| **Motion** is greyed out | Motion works only while the live Kinect runs, not during a replay |

Damaged files are kept, never overwritten: the newest copy is `<file>.bad`, older ones move up to
`.bad.1` … `.bad.4`, and the oldest beyond five is dropped.

## Outside the home

- **`Your internet provider is using carrier NAT, so devices outside your home can't reach this
  PC.`** Outside viewing can't work then, and there is no relay to fall back on. The same goes for a
  second router in front of yours.
- **The status says manual.** The router didn't give a port mapping: UPnP may be off, or the router
  offers only IGD:2. Forward the port by hand with the steps the panel shows. `kinect-cli
  router-probe --outside-port N` says how the router answers; it changes nothing and masks every
  address.
- **`Checking the router… The new home address is shared once the router confirms it.`** The home
  address changed and the PC is waiting for the router to confirm the new one. Devices keep the
  address they had meanwhile.
- **A Public network profile.** The panel shows a hint while outside viewing is on: Windows Firewall
  may block the outside listener on a Public network.
- **An invite didn't work.** Each lasts 10 minutes and works once. *This invite has expired or was
  already used* and *This invite was closed* both mean: ask for a new one.
- **Blocked from mobile data for an hour.** Many phones share one address on a mobile network, so
  other people's failed attempts can get that address blocked. A recently paired device keeps
  reserved slots.

## The Mac

| The Mac says | What to do |
|---|---|
| A request for Keychain access | Allow it, then click **Retry**. The pairing is untouched while access is refused |
| *This Mac's pairing key is missing from the Keychain.* / *doesn't match its saved pairing.* / *is damaged.* / *The saved pairing file is damaged.* | Click **Retry** first. If it persists, **Reset pairing** keeps the old `pairing.json` as `pairing.json.bad`, makes a new key in the Keychain, and the iPhone pairs again |
| An error banner after **Start** | Streaming stopped and says why; a failed pipeline never falls back to another. Pick another pipeline, or see [the sensor](#the-sensor-and-usb) |

The Mac host's state is in `~/kinect-viewer/.host-state/` (`pairing.json`, `presets.json`); its
private key is in the Keychain.
