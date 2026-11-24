#include "pet_settings.hpp"

#include "json_lite.hpp"
#include "outside_settings.hpp"
#include "router_mapper.hpp"
#include "state_files.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace kvhost {

LocalTime localTimeNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    LocalTime t;
    t.weekday = st.wDayOfWeek;
    t.hour = st.wHour;
    t.minute = st.wMinute;
    return t;
}

bool isSharingScheduledOff(const std::vector<SharingScheduleRow>& rows, const LocalTime& t) {
    if (t.weekday < 0 || t.weekday > 6) return false;
    const int now = t.hour * 60 + t.minute;
    const int today = 1 << t.weekday;
    const int yesterday = 1 << ((t.weekday + 6) % 7);
    for (const auto& r : rows) {
        const int from = r.fromMinute, to = r.toMinute;
        if (from < to) {
            if ((r.days & today) && now >= from && now < to) return true;
        } else {
            // Runs past midnight: the evening part belongs to the start day, the morning part to the
            // day after it.
            if ((r.days & today) && now >= from) return true;
            if ((r.days & yesterday) && now < to) return true;
        }
    }
    return false;
}

std::optional<uint16_t> parseClockTime(const std::string& s) {
    const auto digit = [&](size_t i) { return s[i] >= '0' && s[i] <= '9'; };
    if (s.size() != 5 || s[2] != ':' || !digit(0) || !digit(1) || !digit(3) || !digit(4)) return std::nullopt;
    const int h = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return std::nullopt;
    return static_cast<uint16_t>(h * 60 + m);
}

std::string formatClockTime(uint16_t minutes) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>((minutes / 60) % 24),
                  static_cast<unsigned>(minutes % 60));
    return buf;
}

std::string toJson(const PetSettings& s) {
    std::string rows = "[";
    for (size_t i = 0; i < s.sharingSchedule.size(); ++i) {
        const auto& r = s.sharingSchedule[i];
        if (i) rows += ',';
        rows += json::ObjectWriter()
                    .field("days", static_cast<uint8_t>(r.days & 0x7F))
                    .field("from", formatClockTime(r.fromMinute))
                    .field("to", formatClockTime(r.toMinute))
                    .finish();
    }
    rows += ']';
    json::ArrayWriter rec;
    for (const auto& r : s.recordSchedule) {
        rec.object(json::ObjectWriter()
                       .field("days", static_cast<uint8_t>(r.days & 0x7F))
                       .field("start", formatClockTime(r.startMinute))
                       .field("minutes", r.lengthMinutes));
    }
    json::ObjectWriter w;
    w.field("sharingEnabled", s.sharingEnabled)
        .raw("sharingSchedule", rows)
        .field("privacyMode", s.privacyMode)
        .field("rollEnabled", s.rollEnabled)
        .field("rollFolder", s.rollFolder);
    // The limits are written only once they are the owner's, so the defaults are never read back as a choice.
    if (s.rollLimitsKnown) w.field("rollKeepDays", s.rollKeepDays).field("rollCapBytes", s.rollCapBytes);
    w.field("motionEnabled", s.motionEnabled)
        .field("motionSensitivity", s.motionSensitivity)
        .field("recordScheduleEnabled", s.recordScheduleEnabled)
        .array("recordSchedule", rec)
        // The outside keys, after the roll keys.
        .field("allowOutside", s.allowOutside);
    // v3 §8.7: no port yet (none of the default's tries bound) is no key at all, so none is ever read back.
    if (outside::isValidOutsidePort(s.outsidePort)) w.field("outsidePort", s.outsidePort);
    w.field("outsidePublicAddress", s.outsidePublicAddress);
    // v3 §8.7 (additive): only while a forward of this PC's may be on the router; absent means none.
    if (s.outsideMappedPort != 0) w.field("outsideMappedPort", s.outsideMappedPort);
    // After the other outside keys.
    w.field("outsideMappingRemoved", s.outsideMappingRemoved);
    // Additive: the pinned gateway, only while one is pinned; the address devices were last told.
    if (!s.outsideGatewayIP.empty() && !s.outsideGatewayMAC.empty()) {
        w.field("outsideGatewayIP", s.outsideGatewayIP);
        w.field("outsideGatewayMAC", s.outsideGatewayMAC);
        // The pinned gateway's description that last gave answers, only on that gateway.
        if (outside::crackGatewayUrl(s.outsideGatewayLocation, s.outsideGatewayIP)) {
            w.field("outsideGatewayLocation", s.outsideGatewayLocation);
        }
    }
    if (!s.outsideAnnouncedAddress.empty() || s.outsideAnnouncedCorrupt) {
        w.field("outsideAnnouncedAddress", s.outsideAnnouncedAddress);
    }
    return w.finish();
}

std::string recordScheduleSummary(const std::vector<RecordScheduleRow>& rows) {
    static const char* const kDays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string out;
    for (const auto& r : rows) {
        if (!out.empty()) out += "; ";
        const uint8_t days = r.days & 0x7F;
        if (days == 0x7F) {
            out += "Every day";
        } else if (days == 0) {
            out += "No days";
        } else {
            bool first = true;
            for (int d = 0; d < 7; ++d) {
                if (!(days & (1 << d))) continue;
                if (!first) out += ", ";
                out += kDays[d];
                first = false;
            }
        }
        // An en dash (UTF-8), as in the spec's example "Every day 08:00-08:10".
        out += " " + formatClockTime(r.startMinute) + "\xE2\x80\x93" +
               formatClockTime(static_cast<uint16_t>((r.startMinute + r.lengthMinutes) % (24 * 60)));
    }
    return out;
}

bool fromJson(const std::string& text, PetSettings& s) { return fromJson(text, s, outside::AddressPolicy{}); }

bool fromJson(const std::string& text, PetSettings& s, const outside::AddressPolicy& policy) {
    s = PetSettings{};
    json::Value v;
    if (!json::parse(text, v) || v.type != json::Value::Type::Object) return false;
    if (const json::Value* b = v.get("sharingEnabled")) b->asBool(s.sharingEnabled);
    // Fail closed: a privacyMode that is there but isn't a bool is on, not the default.
    if (const json::Value* b = v.get("privacyMode"); b && !b->asBool(s.privacyMode)) s.privacyMode = true;
    if (const json::Value* a = v.get("sharingSchedule"); a && a->type == json::Value::Type::Array) {
        for (const auto& item : a->items) {
            if (s.sharingSchedule.size() >= kMaxSharingScheduleRows) break;
            if (item.type != json::Value::Type::Object) continue;
            uint64_t days = 0;
            std::string from, to;
            const json::Value* d = item.get("days");
            const json::Value* f = item.get("from");
            const json::Value* t = item.get("to");
            if (!d || !d->asUInt64(days) || days > 0x7F || !f || !f->asString(from) || !t || !t->asString(to)) continue;
            const auto fm = parseClockTime(from), tm = parseClockTime(to);
            if (!fm || !tm) continue; // a bad row is dropped, never guessed at
            SharingScheduleRow r;
            r.days = static_cast<uint8_t>(days);
            r.fromMinute = *fm;
            r.toMinute = *tm;
            s.sharingSchedule.push_back(r);
        }
    }
    // The roll keys, each read leniently: a missing or bad value keeps its default, and the defaults record
    // nothing (fail closed).
    if (const json::Value* b = v.get("rollEnabled")) b->asBool(s.rollEnabled);
    if (const json::Value* f = v.get("rollFolder")) f->asString(s.rollFolder);
    // The limits are the owner's only when the file holds them and every one it holds reads; an older file
    // (no roll keys) or a bad value leaves them unknown, and retention waits (fail closed).
    bool limit_read = false, limit_bad = false;
    if (const json::Value* k = v.get("rollKeepDays")) {
        int64_t days = 0;
        if (k->asInt64(days) && days >= kRollMinKeepDays && days <= kRollMaxKeepDays) {
            s.rollKeepDays = static_cast<int>(days);
            limit_read = true;
        } else {
            limit_bad = true;
        }
    }
    if (const json::Value* c = v.get("rollCapBytes")) {
        uint64_t cap = 0;
        if (c->asUInt64(cap) && cap >= kRollMinCapBytes) {
            s.rollCapBytes = cap;
            limit_read = true;
        } else {
            limit_bad = true;
        }
    }
    s.rollLimitsKnown = limit_read && !limit_bad;
    if (const json::Value* b = v.get("motionEnabled")) b->asBool(s.motionEnabled);
    if (const json::Value* m = v.get("motionSensitivity")) {
        int64_t sens = 0;
        if (m->asInt64(sens) && sens >= DepthMotionDetector::kMinSensitivity && sens <= DepthMotionDetector::kMaxSensitivity) {
            s.motionSensitivity = static_cast<int>(sens);
        }
    }
    if (const json::Value* b = v.get("recordScheduleEnabled")) b->asBool(s.recordScheduleEnabled);
    if (const json::Value* a = v.get("recordSchedule"); a && a->type == json::Value::Type::Array) {
        for (const auto& item : a->items) {
            if (s.recordSchedule.size() >= kMaxRecordScheduleRows) break;
            if (item.type != json::Value::Type::Object) continue;
            uint64_t days = 0, minutes = 0;
            std::string start;
            const json::Value* d = item.get("days");
            const json::Value* st = item.get("start");
            const json::Value* mi = item.get("minutes");
            if (!d || !d->asUInt64(days) || days > 0x7F || !st || !st->asString(start) || !mi || !mi->asUInt64(minutes) ||
                minutes < 1 || minutes > 60) {
                continue;
            }
            const auto sm = parseClockTime(start);
            if (!sm) continue; // a bad row is dropped, never guessed at
            RecordScheduleRow r;
            r.days = static_cast<uint8_t>(days);
            r.startMinute = *sm;
            r.lengthMinutes = static_cast<uint16_t>(minutes);
            s.recordSchedule.push_back(r);
        }
    }
    // The outside keys, read leniently and failing closed: anything but true leaves outside listening off; a port
    // outside 1024-65535 is 0 (a new one is picked); an address that isn't a public IPv4 is empty.
    if (const json::Value* b = v.get("allowOutside")) {
        if (!b->asBool(s.allowOutside)) s.allowOutside = false;
    }
    if (const json::Value* p = v.get("outsidePort")) {
        uint64_t port = 0;
        if (p->asUInt64(port) && outside::isValidOutsidePort(static_cast<uint32_t>(std::min<uint64_t>(port, 0xFFFFFFFFull)))) {
            s.outsidePort = static_cast<uint16_t>(port);
        }
    }
    if (const json::Value* a = v.get("outsidePublicAddress")) {
        std::string addr;
        if (a->asString(addr) && outside::isUsablePublicAddress(addr, policy)) {
            s.outsidePublicAddress = outside::trimmedAddress(addr);
        }
    }
    // v3 §8.7: a port this PC's forward may have been left on; anything outside 1024-65535 reads as none.
    if (const json::Value* m = v.get("outsideMappedPort")) {
        uint64_t port = 0;
        if (m->asUInt64(port) && outside::isValidOutsidePort(static_cast<uint32_t>(std::min<uint64_t>(port, 0xFFFFFFFFull)))) {
            s.outsideMappedPort = static_cast<uint16_t>(port);
        }
    }
    // The owner removed the forward: anything but a real false keeps it removed (never map on a bad value).
    if (const json::Value* b = v.get("outsideMappingRemoved")) {
        if (!b->asBool(s.outsideMappingRemoved)) s.outsideMappingRemoved = true;
    }
    // The pinned gateway (v3 §8.3). Fails safe: a key present with anything but a valid value drops the whole pin.
    {
        bool corrupt = false;
        auto gatewayPart = [&](const char* key, std::string& out, bool (*valid)(const std::string&)) {
            out.clear();
            if (const json::Value* g = v.get(key)) {
                if (!g->asString(out) || !valid(out)) {
                    out.clear();
                    corrupt = true;
                }
            }
        };
        gatewayPart("outsideGatewayIP", s.outsideGatewayIP, outside::validGatewayIp);
        gatewayPart("outsideGatewayMAC", s.outsideGatewayMAC, outside::validGatewayMac);
        if (corrupt || s.outsideGatewayIP.empty() || s.outsideGatewayMAC.empty()) {
            s.outsideGatewayIP.clear();
            s.outsideGatewayMAC.clear();
        }
        // Its description that last gave answers: kept only as an http URL on the pinned gateway's own address
        // (crackGatewayUrl). Anything else is dropped, and the next pass searches; the pin stands either way.
        s.outsideGatewayLocation.clear();
        if (const json::Value* l = v.get("outsideGatewayLocation")) {
            if (!l->asString(s.outsideGatewayLocation) || s.outsideGatewayIP.empty() ||
                !outside::crackGatewayUrl(s.outsideGatewayLocation, s.outsideGatewayIP)) {
                s.outsideGatewayLocation.clear();
            }
        }
    }
    // The address devices were last told: present but not a dotted IPv4 is corrupt (every first address is checked).
    s.outsideAnnouncedAddress.clear();
    s.outsideAnnouncedCorrupt = false;
    if (const json::Value* a = v.get("outsideAnnouncedAddress")) {
        if (!a->asString(s.outsideAnnouncedAddress) || !outside::validGatewayIp(s.outsideAnnouncedAddress)) {
            s.outsideAnnouncedAddress.clear();
            s.outsideAnnouncedCorrupt = true;
        }
    }
    return true;
}

PetSettings loadPetSettings(const std::wstring& dir, bool* existed, std::string* error, bool* unread) {
    return loadPetSettings(dir, existed, error, unread, outside::AddressPolicy{});
}

PetSettings loadPetSettings(const std::wstring& dir, bool* existed, std::string* error, bool* unread,
                            const outside::AddressPolicy& policy) {
    PetSettings s;
    if (error) error->clear();
    if (unread) *unread = false;
    const std::wstring path = dir + L"\\pet-camera.json";
    unsigned long state_error = 0;
    const FileState state = fileState(path, &state_error);
    if (existed) *existed = state != FileState::Missing;
    if (state == FileState::Missing) return s; // never saved: the defaults, privacy off
    std::string text;
    if (state == FileState::Unknown || !readWholeFile(path, text)) {
        // Unknown: whether it is there couldn't be checked (an error other than "not found"), so it is taken
        // as there but unreadable.
        const DWORD e = state == FileState::Unknown ? state_error : GetLastError();
        // Another program holds it, or it can't be opened: what it says is unknown, so privacy is on. Not
        // corruption: the file may be fine, so it must never be written over (unread).
        s.privacyMode = true;
        if (unread) *unread = true;
        if (error) {
            *error = "pet-camera.json couldn't be read (Win32 error " + std::to_string(e) +
                     "); privacy mode is on, and the pet-camera settings can't be changed, until it can be read; "
                     "retrying";
        }
        return s;
    }
    if (fromJson(text, s, policy)) return s;
    // Read but not parsed: kept as pet-camera.json.bad for a human to look at, and privacy is on.
    s = PetSettings{};
    s.privacyMode = true;
    std::string kept_what;
    const bool kept = keepBadCopy(dir, L"pet-camera.json", &kept_what);
    // Without the copy, it is left alone like an unreadable file: only a file kept as .bad is replaced.
    if (!kept && unread) *unread = true;
    if (error) {
        *error = kept ? "pet-camera.json couldn't be parsed (" + kept_what +
                            "); privacy mode is on until the settings are saved again"
                      : "pet-camera.json couldn't be parsed, and " + kept_what +
                            "; privacy mode is on, and the pet-camera settings can't be changed, until it can be "
                            "kept; retrying";
    }
    return s;
}

bool savePetSettingsAtomically(const std::wstring& dir, const PetSettings& s, std::string* err) {
    return writeOwnerOnlyFileAtomically(dir, L"pet-camera.json", toJson(s), err);
}

} // namespace kvhost
