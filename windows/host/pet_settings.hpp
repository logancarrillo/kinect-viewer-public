// The PC's pet-camera settings (pet-camera.md §4), stored as pet-camera.json next to pairing.json,
// owner-only and replaced atomically. Read leniently: a bad value or an unknown key gives the default,
// except privacy mode, which fails closed (a privacyMode that isn't a bool, or a file that exists but
// can't be read or parsed, means privacy mode on). Only a missing file means all the defaults.
//
// W6 holds sharing, the sharing schedule and privacy mode; W7 adds the camera roll (on/off, folder,
// retention, motion and scheduled recording). W8 (outside the home) adds allowOutside, outsidePort and
// outsidePublicAddress after the roll keys; an older build ignores them.
//
// Thread safety: none; the caller guards its PetSettings and serialises loads and saves.
#pragma once

#include "roll_triggers.hpp" // RecordScheduleRow

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kvhost {

namespace outside {
struct AddressPolicy; // outside_address.hpp
}

constexpr size_t kMaxSharingScheduleRows = 32;
constexpr size_t kMaxRecordScheduleRows = 32;
// pet-camera.md §4 "Keep": 30 days and 200 GB by default. The limits a stored value must keep to; anything
// else is read as the default (lenient, as every other key).
constexpr int kRollDefaultKeepDays = 30;
constexpr int kRollMinKeepDays = 1;
constexpr int kRollMaxKeepDays = 3650;
constexpr uint64_t kRollDefaultCapBytes = 200'000'000'000ULL;
constexpr uint64_t kRollMinCapBytes = 1'000'000'000ULL; // 1 GB
constexpr int kRollDefaultMotionSensitivity = 5;

// One "days, from-to" row during which sharing is off for viewers. Times are local wall-clock minutes
// since midnight. A row whose `to` is not after `from` runs past midnight into the next day (22:00-07:00),
// and from == to means a full 24 hours from `from`. The day mask names the day the row starts on.
struct SharingScheduleRow {
    uint8_t days = 0x7F; // bit 0 = Sunday ... bit 6 = Saturday (SYSTEMTIME::wDayOfWeek order)
    uint16_t fromMinute = 0;
    uint16_t toMinute = 0;
    bool operator==(const SharingScheduleRow& o) const {
        return days == o.days && fromMinute == o.fromMinute && toMinute == o.toMinute;
    }
};

struct PetSettings {
    bool sharingEnabled = false; // "Share the camera", off by default
    std::vector<SharingScheduleRow> sharingSchedule;
    bool privacyMode = false; // saved across restarts (v3 §6.4, D3)

    // W7, the camera roll (pet-camera.md §4, §6). Every switch is off by default (§1).
    bool rollEnabled = false; // "Record to the camera roll", the master switch for every trigger (v3 D5)
    // The folder the PC app chose ("Change..."), UTF-8; empty means the app's default. Only the PC app reads
    // it, to pick the folder it passes to the host: the host itself records only into the folder its Options
    // name (v3 §10 "Test isolation"), so a test host never reaches the real camera roll through this.
    std::string rollFolder;
    int rollKeepDays = kRollDefaultKeepDays;
    uint64_t rollCapBytes = kRollDefaultCapBytes;
    // The two above are the owner's (pet-camera.md §6.4): the file held them (a key that
    // was there and read), or the owner set or confirmed them, or turned the roll on beside them on the PC.
    // Only then are they written: a file saved before that holds no limits, so a restart doesn't mistake the
    // defaults for the owner's. A W6-era file has no roll keys and leaves this false.
    bool rollLimitsKnown = false;
    bool motionEnabled = false;
    int motionSensitivity = kRollDefaultMotionSensitivity; // 1 (least) to 10 (most)
    bool recordScheduleEnabled = false;
    std::vector<RecordScheduleRow> recordSchedule; // "Scheduled recording": days, start, length 1-60 min

    // W8, outside the home (pet-camera.md §4, v3 §8.1). Off by default, PC only, and only with sharing on: a
    // value that isn't a bool reads as off (fail closed). The port is picked once (20000-44999, one that binds:
    // v3 §8.7) and saved; 0 until then (and then not written), and any value outside 1024-65535 reads as 0. The public address is what the PC user typed
    // for the manual case (a public IPv4); anything else reads as empty.
    bool allowOutside = false;
    uint16_t outsidePort = 0;
    std::string outsidePublicAddress;
    // v3 §8.7 (additive, after the W8 keys): the port this PC's router forward is on while it may still be there
    // (written when mapped, cleared when removed). A start with outside listening off, or on another port, removes
    // that forward best effort (only ours: our description and this PC's internal client) and clears it.
    uint16_t outsideMappedPort = 0;
    // Additive, after outsideMappedPort: the PC user pressed
    // "Remove mapping now". The forward stays removed, across restarts too, until outside viewing is turned off
    // (which clears this) or "Map the port again" is pressed: no check maps it meanwhile. A value that isn't a bool
    // reads as true (never map on a bad value).
    bool outsideMappingRemoved = false;
    // Additive, after outsideMappingRemoved (v3 §8.3): the default gateway outside viewing was set up through,
    // pinned when the PC user turned it on or acknowledged an address change (outside::GatewayIdentity). Written only
    // when both parts are there. Fails safe: if either key is present but isn't a dotted IPv4 / an "aa-bb-cc-dd-ee-ff"
    // hardware address, the whole pin is dropped, and an address change then waits for the PC user.
    std::string outsideGatewayIP, outsideGatewayMAC;
    // The pinned gateway's UPnP description URL that last gave its own answer for the public address, so a restart
    // asks it straight over TCP before any search. Written only with a pin, and only as an http URL on the pinned
    // gateway's own address; anything else read back is dropped (the pin stands, and the next pass searches).
    std::string outsideGatewayLocation;
    // The public address devices were last told, so a restart checks a first address that differs. Absent: none
    // yet (the first one is taken as it is). Present but not a dotted IPv4: outsideAnnouncedCorrupt, and every first
    // address is checked; the key is kept (written empty) until a checked address replaces it.
    std::string outsideAnnouncedAddress;
    bool outsideAnnouncedCorrupt = false;
};

// v3 §5.1 scheduleSummary: display text for the scheduled-recording rows, e.g. "Every day 08:00-08:10" (with
// an en dash) or "Mon, Wed 22:00-22:30; Sat 09:00-09:05". Empty when there are no rows.
std::string recordScheduleSummary(const std::vector<RecordScheduleRow>& rows);

struct LocalTime {
    int weekday = 0; // 0 = Sunday ... 6 = Saturday
    int hour = 0;    // 0-23
    int minute = 0;  // 0-59
};

LocalTime localTimeNow(); // GetLocalTime

// True when any row has sharing off at `t`. Pure, so the tests can pick the clock.
bool isSharingScheduledOff(const std::vector<SharingScheduleRow>& rows, const LocalTime& t);

// "HH:MM" (two digits each, 00:00-23:59) <-> minutes since midnight.
std::optional<uint16_t> parseClockTime(const std::string& hhmm);
std::string formatClockTime(uint16_t minutes);

std::string toJson(const PetSettings& s);
// Lenient: starts from the defaults and takes every key it can read. False only when the text isn't
// a JSON object (the defaults are left in `s` then).
bool fromJson(const std::string& json, PetSettings& s);
// The same, with the address policy a test program passes (outside::AddressPolicy); the one above is the product's.
bool fromJson(const std::string& json, PetSettings& s, const outside::AddressPolicy& policy);

// <dir>\pet-camera.json. A missing file gives the defaults. One that exists but can't be read (another
// program holds it) or parsed fails closed: the defaults with privacy mode ON, and an unparsable one is
// first copied to pet-camera.json.bad. `existed` says whether the file is there; `error` gets a sentence
// for the PC's status line when it couldn't be used (empty otherwise). `unread` is set when the file
// couldn't be read, or didn't parse and couldn't be kept as .bad: it may hold good settings, so it must
// not be written over until a later load reads it (only a file kept as .bad may be replaced).
PetSettings loadPetSettings(const std::wstring& dir, bool* existed = nullptr, std::string* error = nullptr,
                            bool* unread = nullptr);
// The same, with the address policy a test program passes through the stream server's test-only option.
PetSettings loadPetSettings(const std::wstring& dir, bool* existed, std::string* error, bool* unread,
                            const outside::AddressPolicy& policy);
bool savePetSettingsAtomically(const std::wstring& dir, const PetSettings& s, std::string* err = nullptr);

} // namespace kvhost
