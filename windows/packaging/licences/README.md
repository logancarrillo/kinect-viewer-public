# The bundled DLLs' own licence texts

The Windows build copies four DLLs next to the programs, and `install.ps1` puts each one's texts here beside it in the
install folder, named after the DLL (`<dll name>.*`). They're recorded like the rest, so the uninstall removes them.
`packaging_install` checks that every DLL a build has carries at least one text from this folder.

| DLL | Library and version | Texts | From |
|---|---|---|---|
| `freenect2.dll` | libfreenect2, upstream `fd64c5d9b214df6f6a55b4419357e51083f15d93` (Apache-2.0 or GPL-2.0; used under Apache-2.0) | `freenect2.LICENSE.txt` (`APACHE20`), `freenect2.CONTRIB.txt` (its contributors) | the libfreenect2 source at that commit |
| `libusb-1.0.dll` | libusb 1.0.30 (LGPL-2.1-or-later) | `libusb-1.0.COPYING.txt`, `libusb-1.0.AUTHORS.txt` (its copyright notices) | github.com/libusb/libusb at `v1.0.30` |
| `turbojpeg.dll` | libjpeg-turbo 3.2.1 (IJG, BSD-3-Clause, zlib) | `turbojpeg.LICENSE.md`, `turbojpeg.README.ijg.txt` (the IJG licence `LICENSE.md` refers to) | the 3.2.1 Visual C++ x64 build's `doc` folder |
| `glfw3.dll` | GLFW 3.5.1 (zlib/libpng) | `glfw3.LICENSE.md` | the 3.5.1 64-bit Windows binaries |

The texts are the upstream files as they are. What a distributor of binaries has to do beyond shipping them (libusb's
source, or an offer of it) is in [`THIRD_PARTY_NOTICES.md`](../../../THIRD_PARTY_NOTICES.md).
