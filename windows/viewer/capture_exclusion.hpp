// Windows that show a live secret (the pairing window's QR code and code, an invite's link and QR code) are kept
// out of screen capture and screen sharing. WDA_EXCLUDEFROMCAPTURE (Windows 10 2004 and later) leaves the window
// out of a capture; on an older Windows, WDA_MONITOR shows it black there instead. Best effort: it keeps a live
// invite off a screen share or a screenshot tool, not away from a program that reads the window some other way.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace kvwin {

// True when the window is kept out of capture now (either way).
inline bool excludeFromCapture(HWND hwnd) {
    if (!hwnd) return false;
    if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) return true;
    return SetWindowDisplayAffinity(hwnd, WDA_MONITOR) != FALSE;
}

// Whether the window is kept out of capture now.
inline bool excludedFromCapture(HWND hwnd) {
    DWORD affinity = WDA_NONE;
    return hwnd && GetWindowDisplayAffinity(hwnd, &affinity) && affinity != WDA_NONE;
}

} // namespace kvwin
