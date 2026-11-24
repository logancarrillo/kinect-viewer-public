// The displays the second-display (projector) output can go to (the Mac's DisplayTargetInfo and
// refreshAvailableDisplays, macos/Display/SecondDisplayOutputController.swift).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace kvwin {

struct DisplayTarget {
    std::wstring device; // \\.\DISPLAYn: what the choice is kept by while the display stays connected
    std::wstring name;   // the monitor's name (EnumDisplayDevices), or the device when it has none
    RECT rect{};         // in virtual-screen pixels
    bool primary = false;
};

// "Generic PnP Monitor (1920x1080), main"
std::wstring displayTitle(const DisplayTarget& d);
// Every connected display, the main one included, in Windows' order.
std::vector<DisplayTarget> listDisplays();
// Index of device in list, or -1.
int findDisplay(const std::vector<DisplayTarget>& list, const std::wstring& device);
// The one to use (the Mac's): the current choice while it's still connected; otherwise the first that
// isn't the main display (a projector, when one is plugged in); otherwise the first. -1 when none.
int chooseDisplay(const std::vector<DisplayTarget>& list, const std::wstring& current_device);

} // namespace kvwin
