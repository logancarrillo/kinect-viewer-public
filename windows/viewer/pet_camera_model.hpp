// The PC's pet camera panel (pet-camera.md §4, protocol v3 W6): the text it shows, as pure functions
// over the host's plain structs, so the unit tests can check them without a window or a server.
#pragma once

#include "pet_camera_types.hpp"
#include "pet_settings.hpp"
#include "wire_protocol.hpp" // PetWatcher (header only here)

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kvwin {

constexpr uint64_t kStaleOwnerMs = 30ull * 24 * 3600 * 1000; // pet-camera.md §3: owners not seen for 30 days

// "Watching now: iPhone, iPhone 2 (outside the home)" for every session holding a watcher slot, in the
// order given; a playback says so. Empty when nobody watches (the banner is hidden only then).
std::wstring watchingNowText(const std::vector<kvhost::PetWatcher>& watchers);

// An owner record not seen (or, never connected, not paired) for 30 days, and not connected now. The
// device list highlights these and offers to revoke them.
bool isStaleOwner(const kvhost::HostDeviceInfo& d, uint64_t nowUnixMs);

// "Owner" / "Viewer"; "iPhone", "iPad", "Mac", "Other".
std::wstring roleLabel(kvhost::PetRole r);
std::wstring kindLabel(const std::string& kind);

// "never", "now", "5 min ago", "3 h ago", "12 days ago" (lastSeen may be absent).
std::wstring lastSeenText(const kvhost::HostDeviceInfo& d, uint64_t nowUnixMs);
// "home" / "outside the home", with what the session gets: "home, watching" or
// "home, not watching (sharing is off)". Empty when not connected.
std::wstring connectionText(const kvhost::HostDeviceInfo& d);
// The short words for a live_unavailable reason ("sharing is off", "privacy mode", ...).
std::wstring unavailableReasonText(const std::string& reason);

// Day masks (bit 0 Sunday ... bit 6 Saturday): "Every day", "Weekdays", "Weekends", or "Mon, Wed, Fri".
std::wstring daysText(uint8_t days);
// "Weekdays 22:00-07:00 (overnight)", "Every day 00:00-00:00 (24 hours)".
std::wstring scheduleRowText(const kvhost::SharingScheduleRow& r);
// The panel's one-line summary: "none", "1 row", "3 rows".
std::wstring scheduleSummary(const std::vector<kvhost::SharingScheduleRow>& rows);
// "HH:MM" (or "H:MM") to minutes since midnight; nullopt for anything else.
std::optional<uint16_t> parseClockText(const std::wstring& text);
std::wstring clockText(uint16_t minutes);

// One line for the health display (v3 §6.1: "The PC's diagnostics show each tier's encoder name"):
// "Live tiers: high <encoder> (hw) 1024x424 e3 1 session | medium off | low off".
std::wstring tierDiagnosticsLine(const std::vector<kvhost::HostTierStatus>& tiers);

// ---- the camera roll (W7; pet-camera.md §4, §6) ----------------------------------------------------------
// The v3 §9 sentence for why recording can't run (rollBlockedText); empty for None.
std::wstring rollBlockText(kvhost::RollBlock b);
// "Manual", "Motion", "Schedule" ("?" for Unknown).
std::wstring clipTriggerLabel(kvhost::ClipTrigger t);
// Why the panel greys out Motion (by depth), or empty when it can be used (pet-camera.md §6.2): motion is
// offered only while the live sensor runs, never from a replay. host_motion_available: the host's own
// motionAvailable (the sensor on, the PC not replaying).
std::wstring motionUnavailableText(bool replaying, bool sensor_running, bool host_motion_available);
// "1.2 GB", "850 MB" (decimal units, as the 20 GB floor and the 200 GB cap are).
std::wstring bytesText(uint64_t bytes);
// "Used 1.2 GB of 200 GB. 512 GB free; recording stops below 20 GB free."
std::wstring rollStorageText(uint64_t used, uint64_t cap, std::optional<uint64_t> free_bytes, uint64_t floor);
// Next to "Record to the camera roll": "Keeps clips 30 days, up to 200 GB." (the storage line says starred clips are kept).
std::wstring rollLimitsText(int keep_days, uint64_t cap_bytes);
// The panel reads the roll's storage line at most every kRollStorageRefreshMs however often the host reports a
// change, and at once after a change made on this PC (forced) or the first time (last_ms 0).
constexpr uint64_t kRollStorageRefreshMs = 4000;
bool rollStorageRefreshDue(uint64_t now_ms, uint64_t last_ms, bool forced);
// One scheduled-recording row: "Every day 08:00, 10 min". The rows: "none", or the rows joined with "; ".
std::wstring recordScheduleRowText(const kvhost::RecordScheduleRow& r);
std::wstring recordScheduleText(const std::vector<kvhost::RecordScheduleRow>& rows);
// "0:07", "12:30", "1:02:03".
std::wstring clipDurationText(double seconds);

} // namespace kvwin
