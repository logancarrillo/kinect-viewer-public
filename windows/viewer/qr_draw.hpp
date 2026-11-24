// The pairing window's QR drawing, shared with the outside invite and re-share windows (W8): ECC M, as
// CIQRCodeGenerator "M" on the Mac, with a 4-module quiet zone, black modules on whatever the caller painted.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace kvwin {

// Draws text's QR code centred on centre_x, from top, in at most box pixels square. Returns the side drawn.
int drawQrCode(HDC dc, const std::string& text, int centre_x, int top, int box);

} // namespace kvwin
