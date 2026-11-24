# Third-party notices

Kinect Viewer's own code is licensed under the Apache License 2.0 (see [`LICENSE`](LICENSE)). This file lists
the third-party components that the build uses, links against, copies next to the binaries, or runs, and what
anyone **distributing binaries** built from this repository has to do for each.

None of the libraries below is committed to this repository. `vendor/` and the build folders are git-ignored.

## Summary

| Component | Licence | How it is used | Where it comes from |
|---|---|---|---|
| [libfreenect2](https://github.com/OpenKinect/libfreenect2) (commit `fd64c5d9`) | Apache-2.0 **or** GPL-2.0 (dual, your choice) | Linked dynamically, public API only. Windows: `freenect2.dll` is copied next to the binaries at build time. macOS: `libfreenect2.dylib`, found through an rpath. | You clone and build it (see README). |
| [libusb](https://libusb.info/) 1.0 | LGPL-2.1-or-later | libfreenect2's USB layer. Windows: `libusb-1.0.dll` is copied next to the binaries at build time. | Built with or alongside libfreenect2. |
| [libjpeg-turbo](https://libjpeg-turbo.org/) (TurboJPEG) | IJG License, BSD-3-Clause, zlib (see its `LICENSE.md`) | libfreenect2's colour (JPEG) decoder. Windows: `turbojpeg.dll` is copied next to the binaries. | Built with or alongside libfreenect2. |
| [GLFW](https://www.glfw.org/) 3 | zlib/libpng | libfreenect2's OpenGL depth pipeline creates a hidden GLFW context. Windows: `glfw3.dll` is copied next to the binaries. | Built with or alongside libfreenect2. |
| [OpenSSL](https://www.openssl.org/) 3.x | Apache-2.0 | Windows host only: TLS 1.3 and certificates. Linked **statically** (vcpkg triplet `x64-windows-static-md`). | vcpkg port `openssl` (`windows/vcpkg.json`). |
| [Nayuki QR Code generator](https://github.com/nayuki/QR-Code-generator) | MIT | Windows host only: draws the pairing and invite QR codes. Linked statically. | vcpkg port `nayuki-qr-code-generator` (`windows/vcpkg.json`). |
| [FFmpeg](https://ffmpeg.org/) | LGPL-2.1-or-later, or GPL-2.0-or-later/GPL-3.0 depending on how it was built | Run as a **separate process** (`ffmpeg -nostdin ...`) for 16-bit FFV1 depth/IR recordings and some tests. Not linked and not shipped. | Installed by the user and found on `PATH` (Windows also checks `KV_FFMPEG` and the WinGet links folder). |
| Turbo colormap polynomial approximation | Apache-2.0 (Copyright 2019 Google LLC) | Coefficients in `core/src/colormaps.cpp`, ported from GLSL to C++, with the copyright and SPDX lines at the top of that file. | Google Research: colour map design by Anton Mikhailov, polynomial approximation by Ruofei Du. |
| Viridis and Inferno colormap stops | CC0 1.0 | Reference values in `core/src/colormaps.cpp`. | Smith, van der Walt and Firing (matplotlib colormaps). |
| OpenCL runtime (optional) | Vendor licence | Only if you build libfreenect2 with its OpenCL pipeline (`--pipeline cl`). The app can't select CUDA. | Your GPU vendor. |

On Windows the Kinect needs a USB driver (libusbK, installed with Zadig; see `docs/windows-setup.md`). The person
installs both themselves; neither is distributed with Kinect Viewer or installed by it.

Operating-system components (Apple frameworks such as Network, Security, VideoToolbox, AVFoundation, Metal and
Vision; Windows components such as Media Foundation, WIC, Direct3D 11, Direct2D, DNS-SD and NATUPnP) are used
through their public APIs and are not redistributed. Build tools (CMake, Ninja, vcpkg, XcodeGen, Xcode, MSVC)
are not part of the binaries.

## Obligations when you distribute binaries

You only need this section if you ship built binaries (an installer, a zip of `build\windows\windows\`, a
signed app). Building and running it yourself creates no obligations beyond the licences' own terms.

The Windows install (`windows\packaging\install.ps1`) already puts this project's `LICENSE`, `NOTICE` and this
file beside the programs, and each bundled DLL's own licence texts beside it: libfreenect2's, libusb's (the
LGPL-2.1 and its copyright notices), libjpeg-turbo's and GLFW's, from
[`windows/packaging/licences/`](windows/packaging/licences/README.md). A zip of an install folder carries them. What
that doesn't cover, such as libusb's source or a written offer of it, is below.

### libfreenect2 (Apache-2.0 or GPL-2.0)

Choose the Apache-2.0 option to stay compatible with this project's licence. Include libfreenect2's licence
text and its copyright notices with the binaries, and say that the binaries use libfreenect2 at commit
`fd64c5d9`. If you modified libfreenect2, mark the files you changed.

### libusb (LGPL-2.1-or-later): runtime DLL copied next to the binaries

The Windows build copies `libusb-1.0.dll` next to `kinect-viewer.exe` and `kinect-cli.exe`, so a zip of that
folder distributes libusb. LGPL-2.1 then requires that you:

1. **Keep it a separate, replaceable shared library.** Ship it as the DLL (as the build does), so a user can
   swap in their own compatible build of libusb. Do not link it statically into the executables.
2. **Provide the source.** Ship the complete corresponding source of the exact libusb version in the
   binaries (including any changes you made and the scripts used to build it), or include a written offer,
   valid for at least three years, to provide it. Pointing to the upstream release tag you built from is
   enough only if you did not modify it and the link stays available.
3. **Include the licence and notices.** Ship the LGPL-2.1 text and libusb's copyright notices with the
   binaries, and state in your documentation that the program uses libusb under the LGPL.
4. **Allow modification and reverse engineering** of the combined work for debugging the user's own
   modifications to libusb (LGPL-2.1 section 6). Your distribution terms must not forbid it.

If libfreenect2 was built against a statically linked libusb inside `freenect2.dll`, the same obligations
apply to that DLL, and you must also make it possible to relink it (for example by shipping the object files
or building libusb as its own DLL instead, which is simpler).

**On macOS** the build copies none of these into the app: it links `libfreenect2.dylib` through an rpath to
`$(LIBFREENECT2_PREFIX)/lib` (`project.yml`), and libfreenect2 loads the Homebrew libusb, libjpeg-turbo and GLFW
it was built against from where Homebrew installed them. So an app built and run on its own Mac distributes
nothing. A standalone `KinectViewer.app` you give to others with those `.dylib`s embedded (in
`Contents/Frameworks`, say) distributes each of them: keep libusb a separate `.dylib` the user can replace, and
ship its source (or the written offer), its licence and notices as above, along with libfreenect2's,
libjpeg-turbo's and GLFW's.

### libjpeg-turbo (IJG, BSD-3-Clause, zlib)

Include libjpeg-turbo's `LICENSE.md` with the binaries. The IJG licence asks that the documentation of a
binary distribution say: *"This software is based in part on the work of the Independent JPEG Group."* The
BSD-3-Clause part requires its copyright notice and disclaimer in the documentation.

### GLFW (zlib/libpng)

No notice is strictly required in binaries, but the licence text must stay with any source you redistribute
and you must not misrepresent the origin of the library. Including its licence text is good practice.

### OpenSSL 3.x (Apache-2.0)

OpenSSL is linked statically into the Windows host. Include the Apache License 2.0 text and OpenSSL's
copyright notice with the binaries. Cryptography export or import rules may apply in some countries.

### Nayuki QR Code generator (MIT)

Include its copyright notice ("Copyright (c) Project Nayuki") and the MIT licence text with the binaries.

### FFmpeg

FFmpeg is not linked or shipped; the programs start an `ffmpeg` executable the user installed. If you choose
to ship an FFmpeg build alongside Kinect Viewer, you take on that build's obligations: LGPL or GPL, depending
on its configuration (many prebuilt Windows builds are GPL-3.0), including providing its corresponding source.

### H.264 video

The hosts encode H.264 with the operating system's own encoders (VideoToolbox, Media Foundation). Patent
licensing for distributing products that encode or decode H.264 is the distributor's responsibility and is
outside the scope of these software licences.
