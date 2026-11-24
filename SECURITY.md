# Security policy

Kinect Viewer streams a camera that usually sits inside someone's home, so security reports are welcome and
taken seriously.

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub's security advisories:

1. Open the repository's **Security** tab.
2. Choose **Report a vulnerability** and fill in the form.

Please do not open a public issue, pull request or discussion for a vulnerability before it is fixed. Include
the affected component (macOS host, Windows host, iPhone app, Mac viewer), the version or commit, steps to
reproduce, and the impact you expect. A proof of concept against your own devices is welcome; please do not
test against devices or networks you don't own.

We aim to acknowledge a report within 7 days and to agree on a disclosure date with you once a fix is ready.
This is a volunteer project, so there is no bug bounty.

## Supported versions

Only the latest commit on the default branch is supported. Fixes are not backported.

## Threat model (summary)

The full protocol and its rules are in [`docs/protocol-v3-pet-camera.md`](docs/protocol-v3-pet-camera.md)
(see §2.2 limits, §5.10 malformed messages and §8 outside the home) and
[`docs/protocol-changes.md`](docs/protocol-changes.md).

**What is protected:** the live picture, the camera roll and its thumbnails, the host's settings and device
list, and the host's and devices' private keys.

**Transport: pinned mutual TLS 1.3.**
- Every connection is TLS 1.3 only, on the hosts and on the Swift clients. The Windows host turns off session
  tickets and session resumption; the macOS host and the Swift clients leave this to the platform's defaults.
- Each side has a self-signed ECDSA P-256 certificate. The host requires a client certificate and accepts it
  only if the SHA-256 of the full leaf certificate matches a paired device (compared in constant time); the
  device pins the host's certificate the same way. No certificate authority is trusted.
- On the Windows host, a new identity's certificate has a random common name, so an internet scan can't tell
  these hosts by a fixed name. An identity made before that keeps its certificate (and its old fixed name),
  since a new certificate would mean pairing every device again.
- A paired device's remote commands (`0x08`, a key-frame request from any role included) are limited to a burst
  of 8 and then 4 a second per device on the Windows host (a reconnect doesn't refill it); past that they are
  dropped.
- Every frame has an explicit length that is capped before anything is allocated. On the Windows host the cap
  is 64 KiB for device sessions and pairing and 16 KiB for invite redemption, and the JSON parser limits depth
  and size. The macOS host currently accepts frames up to 4 MiB, including from unpaired peers (see Known
  limitations).

**Pairing needs someone at the host.**
- A pairing listener exists only while a pairing window is open at the host (120 seconds). On the Windows host
  it accepts only local (home-network) sources, allows one attempt, and closes on success, failure or timeout.
  A connection that hasn't sent its request within 10 seconds is closed, and every pairing connection closes
  with the window.
  The macOS host does not yet check the source address or limit pairing connections (see Known limitations).
- The QR code carries a 256-bit one-time token and the host's certificate fingerprint, so a QR pairing is
  pinned from the first byte. The proof is HKDF-SHA256 and HMAC-SHA256 bound to both certificates.
- The role a device gets (owner or viewer) is chosen at the host, never by the device.
- The name a device sends is cleaned on the Windows host: control characters become spaces, bidirectional
  controls (which can make a name show its text out of order) and characters that show nothing (a zero-width
  space, a soft hyphen) are removed, and a name already on the device list
  gets a number ("iPhone 2"), so a new device can't pass for one already paired.
- On Windows, the pairing window and an invite's window are kept out of screenshots and screen sharing, so a
  screen share doesn't show a live QR code, code or invite link. This is best effort: it stops capture tools,
  not a program that reads the window some other way.

**Invites (outside the home, optional).**
- An invite is created at the host only, only while outside viewing is on. It carries the host's fingerprint
  and a 128-bit one-time token, lasts 10 minutes and can be redeemed once. A wrong proof is counted as a failure for
  its source, and the tenth burns the invite.
- Invites are never sent by the app: the user passes the link on through a private channel of their choice.
- On the Windows host the open invite's token is stored wrapped with DPAPI for the Windows user, so a copy of
  the host-state folder (a backup, for example) can't redeem it. The host's private key is stored wrapped with
  DPAPI for the PC (machine scope, with the app's own entropy), so a copy of the folder alone, taken to another
  PC or Windows install, holds no key that can be used. A full image of the system disk carries the PC's DPAPI
  secrets with it.

**The outside listener is opt-in.**
- Sharing, outside viewing and the camera roll are all **off by default**. Outside viewing can only be
  turned on at the host (a device can turn it off, never on).
- When it is on, the host listens on a separate port with its own TLS context, asks the home router for a
  port mapping (UPnP), and removes the mapping when outside viewing is turned off. On exit it tries to (best
  effort: the router may not answer in time), and a forward of its own left behind is removed at the next start.
- Before any TLS work, a limiter applies per-source, per-IPv6-/48 and global caps on unauthenticated
  connections, and blocks a source for an hour after repeated failed handshakes. When the global cap is full, a
  stranger's handshake that hasn't completed gives its slot to the newcomer, so a few addresses can't keep
  everyone else out. Sources are grouped by IPv4 address or IPv6 /64; IPv4-mapped IPv6 addresses are unwrapped
  first.
- Connections are classified as local or outside by source address. On the Windows host a source is local only
  if it is loopback or on the network of a physical Ethernet or Wi-Fi adapter with a gateway; a VPN's, a PPP
  link's or a virtual adapter's network is not, whichever holds the default route. A link-local IPv6 source is
  local only if it came in on such an adapter's own link. The carrier-grade block
  100.64.0.0/10 (Tailscale's IPv4 addresses, and other VPNs that use it) and Tailscale's IPv6 block
  fd7a:115c:a1e0::/48 are never treated as local.

## Known limitations

- **Home is the physical adapters' networks (Windows).** A Hyper-V external switch's vEthernet counts as virtual,
  so a PC whose only network path goes through one treats no device as home. A router that puts Wi-Fi and Ethernet
  on separate subnets makes the other subnet "not home". On a shared network the PC's own Wi-Fi or Ethernet joins
  (a café, a dorm), everyone on it counts as home, if that network is no wider than a home's: IPv4 inside the
  private ranges at /16 or narrower, IPv6 /64 or narrower. A wider one, such as a provider's segment on a bridged
  modem, counts as nobody's home, and so does the global IPv6 /64 of an adapter whose IPv4 network isn't a home's.
  An adapter with IPv6 alone has no IPv4 to judge it by, so its global /64 counts as home: on an IPv6-only
  connection to a provider, that /64 may be shared with others. After the PC changes networks, the old network
  can count as home for up to 30 seconds.
- **Short-code pairing is a convenience for a trusted home network.** Unlike the QR code, the 8-digit code
  does not pin the host's certificate, so it is not protected against an active attacker on the same network
  during the 120-second window. Prefer the QR code. The iPhone app and the Mac viewer say so before short-code
  entry and ask you to confirm you're on your own network. The Windows host takes the code only in a pairing
  window where you chose **Use a code instead**; otherwise it pairs by QR alone.
- **The macOS host is less hardened than the Windows host.** Its pairing listener has no local-source check
  and no cap on connections, it reads frames of up to 4 MiB from peers that haven't paired, and builds are
  ad-hoc signed without the hardened runtime. Prefer the Windows host where the network isn't fully trusted.
- **Run the host on a network you trust,** with the Windows network profile set to Private only for your own
  network: the home rule above trusts whoever shares the PC's own Wi-Fi or Ethernet network.
- **The home network can hold the LAN listeners' slots.** Someone on the home network can keep all 16 handshake
  slots on the stream listener, or all 8 pairing slots, busy. While they keep it up, new home connections are
  refused (paired devices already connected stay connected). The LAN listeners have no reserve. This is accepted,
  because a full cap turns connections away instead of ending the host.
- **Invites can be burned.** Anyone who can reach the outside port while an invite is open can make it fail
  with a wrong proof; the host user then creates a new one.
- **Shared carrier addresses.** Many mobile users can share one IPv4 address, so other people's failures can
  block that address for an hour. A recently paired device keeps reserved slots.
- **Keys at rest.** On Windows the host's private key is wrapped with DPAPI in machine scope, so any program
  on the same PC that can read the file can unwrap it. On that PC, the host-state folder's owner-only ACL keeps
  non-admin users out; an administrator can read the file. A folder written by a build from before the wrap
  holds the key plain until the first start migrates it, and so does one saved while DPAPI isn't working (the
  save keeps a plain file plain, so pairing still works; over a wrap another start of the host on the same
  folder makes meanwhile, it puts the wrap back and fails. Only if putting it back fails too, with both files
  left, is the key plain in `pairing.json` beside the wrapped `pairing.json.prev`, until the next start wraps
  it again). A `pairing.json.bad` kept by such a build is left as it
  is and still holds it plain, as may disk space those older writes freed. The running host holds the key in
  memory, so a memory dump or a crash dump of it holds the key too. On macOS and iOS keys are kept in the
  Keychain.
- **Copies an interrupted save leaves (Windows).** A save that Windows fails midway can leave a
  `pairing.json.prev` beside `pairing.json`. A start removes it only when it can tell it holds the same identity.
  While DPAPI isn't working it can't unwrap a wrapped key to compare them, so it keeps `.prev` as
  `pairing.json.prev.bad` even when it is the same identity, and that copy may hold the key plain. A file's `.bad`
  copies rotate (five are kept, and the oldest goes first), so a kept copy of another identity can eventually
  rotate out.
- **Going back to a build from before the key wrap loses the identity.** Such a build can't read a wrapped key.
  One from `df8af2c` (2026-11-19) on refuses to start the host, and running it means resetting the PC's identity.
  One from before `df8af2c` makes a new identity over the file it can't parse. Either way, every device must pair
  again.
- **No relay.** Outside viewing can't work behind carrier-grade NAT or a second router; the host says so.
- **Listening ports aren't held exclusively.** The Windows host's listeners don't set `SO_EXCLUSIVEADDRUSE`, so a
  program running as the same user (or an administrator) could bind the same port. It's left off for now because
  Windows won't let an exclusive port be bound again while its old connections are in `TIME_WAIT`: a host
  restarted after a crash could then be unable to listen for minutes.
- **An owner's gestures and mode requests aren't rate-limited.** On the Windows host, an owner's orbit gestures
  (`0x09`) and mode requests aren't held to a bucket as `0x08` commands are, since a drag sends many a second.
  Both need an owner with mode switching allowed.
- **A burst can drop a hand-over to the PC's window.** The host hands the window a phone's commands, gestures and
  deleted clip ids through a queue of 256 each; past that the oldest goes. A burst of more than 256 clip deletions
  between two window messages could lose one, so the PC's player wouldn't close on that clip first.
- **Build hardening.** The Windows programs are built with Control Flow Guard and marked CET shadow-stack
  compatible. They aren't built with `/Qspectre`, which needs Spectre-mitigated runtime libraries the build
  doesn't use yet.

## Out of scope

- Attackers who already control the host account or have administrator rights on the host, or physical
  access to an unlocked paired device.
- Vulnerabilities in libfreenect2, libusb, OpenSSL, FFmpeg, the operating systems or the router's UPnP
  implementation: please report those upstream (we are happy to help coordinate).
- Denial of service by flooding from a large number of addresses, or from the home network itself, unless it
  bypasses a documented limit.
- Social engineering, such as convincing a user to forward an invite link to the wrong person.
- What the camera sees: pointing it and deciding who may watch is the owner's responsibility.
