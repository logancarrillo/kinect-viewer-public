// Test hook: captures a window (including its D3D content) with PrintWindow and saves a PNG via WIC.
// No input is synthesised; the window is only read.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace kvwin {

// out_lit (optional): how many captured pixels aren't pure black (any of B, G, R non-zero), and
// out_total the pixel count, so a test can check a black-out pixel by pixel.
bool captureWindowToPng(HWND hwnd, const std::wstring& path, std::string* out_error, size_t* out_lit = nullptr,
                        size_t* out_total = nullptr);

} // namespace kvwin
