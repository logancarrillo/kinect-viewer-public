#include "display_targets.hpp"

#include <cwchar>

namespace kvwin {

std::wstring displayTitle(const DisplayTarget& d) {
    wchar_t b[256];
    swprintf(b, 256, L"%ls (%ldx%ld)%ls", d.name.empty() ? d.device.c_str() : d.name.c_str(), d.rect.right - d.rect.left,
             d.rect.bottom - d.rect.top, d.primary ? L", main" : L"");
    return b;
}

namespace {
BOOL CALLBACK onMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<DisplayTarget>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    DisplayTarget d;
    d.device = mi.szDevice;
    d.rect = mi.rcMonitor;
    d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0]) d.name = dd.DeviceString;
    out->push_back(d);
    return TRUE;
}
} // namespace

std::vector<DisplayTarget> listDisplays() {
    std::vector<DisplayTarget> out;
    EnumDisplayMonitors(nullptr, nullptr, onMonitor, reinterpret_cast<LPARAM>(&out));
    return out;
}

int findDisplay(const std::vector<DisplayTarget>& list, const std::wstring& device) {
    if (device.empty()) return -1;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].device == device) return static_cast<int>(i);
    }
    return -1;
}

int chooseDisplay(const std::vector<DisplayTarget>& list, const std::wstring& current) {
    const int cur = findDisplay(list, current);
    if (cur >= 0) return cur;
    for (size_t i = 0; i < list.size(); ++i) {
        if (!list[i].primary) return static_cast<int>(i);
    }
    return list.empty() ? -1 : 0;
}

} // namespace kvwin
