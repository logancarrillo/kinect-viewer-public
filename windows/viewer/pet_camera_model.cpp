#include "pet_camera_model.hpp"

#include <cstdio>

namespace kvwin {

namespace {

std::wstring widenUtf8(const std::string& s) {
    // UTF-8 to UTF-16 without Windows headers (names are UTF-8 from the device list).
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
        i += len;
    }
    return out;
}

std::wstring agoText(uint64_t thenMs, uint64_t nowMs) {
    if (thenMs >= nowMs) return L"now";
    const uint64_t secs = (nowMs - thenMs) / 1000;
    wchar_t b[64];
    if (secs < 60) return L"just now";
    if (secs < 3600) swprintf(b, 64, L"%llu min ago", static_cast<unsigned long long>(secs / 60));
    else if (secs < 48 * 3600) swprintf(b, 64, L"%llu h ago", static_cast<unsigned long long>(secs / 3600));
    else swprintf(b, 64, L"%llu days ago", static_cast<unsigned long long>(secs / 86400));
    return b;
}

} // namespace

std::wstring watchingNowText(const std::vector<kvhost::PetWatcher>& watchers) {
    if (watchers.empty()) return std::wstring();
    std::wstring t = L"Watching now: ";
    for (size_t i = 0; i < watchers.size(); ++i) {
        const auto& w = watchers[i];
        if (i) t += L", ";
        t += w.deviceName.empty() ? widenUtf8(w.deviceId) : widenUtf8(w.deviceName);
        const bool outside = w.path == kvhost::PetPath::Outside;
        const bool playback = w.activity == kvhost::watcher_activity::kPlayback;
        if (outside && playback) t += L" (outside the home, a clip)";
        else if (outside) t += L" (outside the home)";
        else if (playback) t += L" (a clip)";
    }
    return t;
}

bool isStaleOwner(const kvhost::HostDeviceInfo& d, uint64_t nowUnixMs) {
    if (d.role != kvhost::PetRole::Owner || d.connected) return false;
    const uint64_t since = d.lastSeenUnixMs ? *d.lastSeenUnixMs : d.pairedAtUnixMs;
    return nowUnixMs > since && nowUnixMs - since >= kStaleOwnerMs;
}

std::wstring roleLabel(kvhost::PetRole r) {
    switch (r) {
        case kvhost::PetRole::Owner: return L"Owner";
        case kvhost::PetRole::Viewer: return L"Viewer";
        default: return L"?";
    }
}

std::wstring kindLabel(const std::string& kind) {
    if (kind == "iphone") return L"iPhone";
    if (kind == "ipad") return L"iPad";
    if (kind == "mac") return L"Mac";
    return L"Other";
}

std::wstring lastSeenText(const kvhost::HostDeviceInfo& d, uint64_t nowUnixMs) {
    if (d.connected) return L"now";
    if (!d.lastSeenUnixMs) return L"never";
    return agoText(*d.lastSeenUnixMs, nowUnixMs);
}

std::wstring unavailableReasonText(const std::string& reason) {
    if (reason == kvhost::live_unavailable::kPrivacyMode) return L"privacy mode";
    if (reason == kvhost::live_unavailable::kPcReplaying) return L"the PC is playing a recording";
    if (reason == kvhost::live_unavailable::kSharingOff) return L"sharing is off";
    if (reason == kvhost::live_unavailable::kSharingSchedule) return L"sharing is off at this time";
    if (reason == kvhost::live_unavailable::kTooManyViewers) return L"four devices already watch";
    return widenUtf8(reason);
}

std::wstring connectionText(const kvhost::HostDeviceInfo& d) {
    if (!d.connected) return std::wstring();
    std::wstring t = d.path == "outside" ? L"outside the home" : L"home";
    if (d.watching) t += L", watching";
    else if (!d.available && !d.unavailableReason.empty()) t += L", not watching (" + unavailableReasonText(d.unavailableReason) + L")";
    else t += L", not watching";
    return t;
}

std::wstring daysText(uint8_t days) {
    days &= 0x7F;
    if (days == 0x7F) return L"Every day";
    if (days == 0x3E) return L"Weekdays";
    if (days == 0x41) return L"Weekends";
    if (days == 0) return L"No days";
    static const wchar_t* const names[7] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    std::wstring t;
    for (int k = 1; k <= 7; ++k) { // Monday first, Sunday last
        const int bit = k % 7;
        if (!(days & (1u << bit))) continue;
        if (!t.empty()) t += L", ";
        t += names[bit];
    }
    return t;
}

std::wstring clockText(uint16_t minutes) {
    wchar_t b[16];
    swprintf(b, 16, L"%02u:%02u", static_cast<unsigned>((minutes / 60) % 24), static_cast<unsigned>(minutes % 60));
    return b;
}

std::wstring scheduleRowText(const kvhost::SharingScheduleRow& r) {
    std::wstring t = daysText(r.days) + L" " + clockText(r.fromMinute) + L"–" + clockText(r.toMinute);
    if (r.toMinute == r.fromMinute) t += L" (24 hours)";
    else if (r.toMinute < r.fromMinute) t += L" (overnight)";
    return t;
}

std::wstring scheduleSummary(const std::vector<kvhost::SharingScheduleRow>& rows) {
    if (rows.empty()) return L"none";
    if (rows.size() == 1) return L"1 row";
    return std::to_wstring(rows.size()) + L" rows";
}

std::optional<uint16_t> parseClockText(const std::wstring& text) {
    size_t a = 0, b = text.size();
    while (a < b && text[a] == L' ') ++a;
    while (b > a && text[b - 1] == L' ') --b;
    const std::wstring t = text.substr(a, b - a);
    const size_t colon = t.find(L':');
    if (colon == std::wstring::npos || colon == 0 || colon > 2 || t.size() - colon - 1 != 2) return std::nullopt;
    int h = 0, m = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i == colon) continue;
        if (t[i] < L'0' || t[i] > L'9') return std::nullopt;
        if (i < colon) h = h * 10 + (t[i] - L'0');
        else m = m * 10 + (t[i] - L'0');
    }
    if (h > 23 || m > 59) return std::nullopt;
    return static_cast<uint16_t>(h * 60 + m);
}

std::wstring tierDiagnosticsLine(const std::vector<kvhost::HostTierStatus>& tiers) {
    if (tiers.empty()) return std::wstring();
    std::wstring t = L"Live tiers:";
    for (size_t i = 0; i < tiers.size(); ++i) {
        const auto& s = tiers[i];
        t += i ? L" | " : L" ";
        t += widenUtf8(s.tier);
        if (!s.open && !s.active) {
            t += L" off";
            continue;
        }
        // The tier names its encoder "(hardware)" or "(software fallback)" itself; otherwise say which.
        const bool says = s.encoderName.find("hardware") != std::string::npos || s.encoderName.find("software") != std::string::npos;
        const wchar_t* hw = (says || s.encoderName.empty()) ? L"" : (s.hardware ? L" (hw)" : L" (sw)");
        wchar_t b[200];
        swprintf(b, 200, L" %ls%ls %ux%u e%u, %d session%ls", s.encoderName.empty() ? L"(opening)" : widenUtf8(s.encoderName).c_str(),
                 hw, s.width, s.height, s.epoch, s.sessions, s.sessions == 1 ? L"" : L"s");
        t += b;
        if (!s.open) t += L", closed";
        else if (s.sessions == 0) t += L", held";
    }
    return t;
}

// ---- the camera roll ----------------------------------------------------------------------------------

std::wstring rollBlockText(kvhost::RollBlock b) {
    namespace t = kvhost::text;
    switch (b) {
        case kvhost::RollBlock::None: return std::wstring();
        case kvhost::RollBlock::PrivacyMode: return widenUtf8(t::kPrivacyMode);
        case kvhost::RollBlock::RollDisabled: return widenUtf8(t::kRollDisabled);
        case kvhost::RollBlock::SensorOff: return widenUtf8(t::kSensorOff);
        case kvhost::RollBlock::ViewFrozen: return widenUtf8(t::kViewFrozen);
        case kvhost::RollBlock::PcReplaying: return widenUtf8(t::kPcReplaying);
        case kvhost::RollBlock::DiskFloor: return widenUtf8(t::kDiskFloor);
        case kvhost::RollBlock::StorageFull: return widenUtf8(t::kStorageFull);
    }
    return std::wstring();
}

std::wstring clipTriggerLabel(kvhost::ClipTrigger t) {
    switch (t) {
        case kvhost::ClipTrigger::Manual: return L"Manual";
        case kvhost::ClipTrigger::Motion: return L"Motion";
        case kvhost::ClipTrigger::Schedule: return L"Schedule";
        case kvhost::ClipTrigger::Unknown: break;
    }
    return L"?";
}

std::wstring motionUnavailableText(bool replaying, bool sensor_running, bool host_motion_available) {
    if (replaying) return L"Motion is unavailable while a recording plays: a replay's depth is never motion.";
    if (!sensor_running || !host_motion_available) return L"Motion needs the live sensor: start the Kinect.";
    return std::wstring();
}

std::wstring bytesText(uint64_t bytes) {
    wchar_t b[64];
    if (bytes >= 1000000000ULL) swprintf(b, 64, L"%.1f GB", static_cast<double>(bytes) / 1e9);
    else if (bytes >= 1000000ULL) swprintf(b, 64, L"%.0f MB", static_cast<double>(bytes) / 1e6);
    else swprintf(b, 64, L"%.0f KB", static_cast<double>(bytes) / 1e3);
    return b;
}

std::wstring rollStorageText(uint64_t used, uint64_t cap, std::optional<uint64_t> free_bytes, uint64_t floor) {
    std::wstring t = L"Used " + bytesText(used) + L" of " + bytesText(cap) + L". ";
    t += free_bytes ? bytesText(*free_bytes) + L" free" : std::wstring(L"Free space unknown");
    t += L"; recording stops below " + bytesText(floor) + L" free.";
    return t;
}

std::wstring rollLimitsText(int keep_days, uint64_t cap_bytes) {
    return L"Keeps clips " + std::to_wstring(keep_days) + (keep_days == 1 ? L" day" : L" days") + L", up to " + bytesText(cap_bytes) + L".";
}

bool rollStorageRefreshDue(uint64_t now_ms, uint64_t last_ms, bool forced) {
    return forced || last_ms == 0 || now_ms < last_ms || now_ms - last_ms >= kRollStorageRefreshMs;
}

std::wstring recordScheduleRowText(const kvhost::RecordScheduleRow& r) {
    return daysText(r.days) + L" " + clockText(r.startMinute) + L", " + std::to_wstring(r.lengthMinutes) + L" min";
}

std::wstring recordScheduleText(const std::vector<kvhost::RecordScheduleRow>& rows) {
    if (rows.empty()) return L"none";
    std::wstring t;
    for (const auto& r : rows) {
        if (!t.empty()) t += L"; ";
        t += recordScheduleRowText(r);
    }
    return t;
}

std::wstring clipDurationText(double seconds) {
    const long long s = seconds > 0 ? static_cast<long long>(seconds + 0.5) : 0;
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%lld:%02lld", s / 60, s % 60);
    return b;
}

} // namespace kvwin
