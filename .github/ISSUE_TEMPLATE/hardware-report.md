---
name: Hardware report
about: You ran it on your own Kinect, computer or iPhone. What worked and what didn't helps the next person most.
title: "Hardware report: <host computer> + <OS>"
labels: hardware-report
---

**The sensor**
- Kinect v2 model and adapter (for example an Xbox One Kinect with the Kinect Adapter for Windows):
- Firmware (the `firmware=` line of the summary):

**The host**
- Computer, CPU and OS (an M-series Mac with macOS 26.5, a Windows 11 x64 PC...):
- The USB port: rear panel or front, a hub or not, and what the health display (**H**) shows as
  the USB port:

**The numbers**
Paste the summary of `kinect-cli bench --pipeline gl` (Windows) or `kinect-cli --duration 60`
(Mac), and the same with `cpu` if you tried it. On the Mac, delete the value after `serial=`.
Never post pairing codes, `kv2://pair` QR links or invite links.

**What you tried, and how it went**
- [ ] IR, Colour, Depth, Side by Side
- [ ] 3D point cloud
- [ ] Depth or IR recording, and its replay
- [ ] Pairing by QR code / by 8-digit code
- [ ] Streaming to a physical iPhone (model and iOS version):
- [ ] The Mac's Watch a Camera with a Windows host
- [ ] Pet camera: sharing, privacy mode, the camera roll, outside the home
- [ ] Unplugging the Kinect, or sleep and wake, while it streams

For anything that didn't work: the step, what you expected, and what happened.

<!-- Please attach no picture, recording or screenshot from a real camera. -->
