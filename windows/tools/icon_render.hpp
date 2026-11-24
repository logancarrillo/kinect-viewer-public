// The Windows app icon from the logo (design/logo.svg, the Mac's master from
// design/render-icons.swift). Direct2D draws the SVG once at its own 1024x1024 size, and WIC scales
// that down to each icon size; the .ico holds every size as a PNG. Windows' own Direct2D and WIC only:
// no new dependency. Run at build time (kv-render-icon), so the icon always follows the SVG.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kvicon {

// The sizes in the .ico: the shell's small, list, tile and jumbo icons at 100-200% scaling.
const std::vector<int>& iconSizes(); // 256, 128, 96, 64, 48, 40, 32, 24, 20, 16

struct IconImage {
    int size = 0;
    std::vector<uint8_t> png;  // PNG, 32-bit BGRA with straight alpha
    std::vector<uint8_t> bgra; // the same pixels (size * size * 4, straight alpha), for checks
};

// The tool renders the logo only: a path whose last two parts are design\logo.svg (either slash, any
// case). Anything else is refused.
bool isLogoPath(const std::wstring& path);

// Renders svg_path at every size. Needs COM on the calling thread. False with a reason.
bool renderSvgIcons(const std::wstring& svg_path, std::vector<IconImage>& out, std::string* error);
// The .ico file: an ICONDIR, one entry per image (256 written as 0), then each PNG.
std::vector<uint8_t> makeIco(const std::vector<IconImage>& images);

} // namespace kvicon
