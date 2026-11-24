// The camera roll's files and folders (pet-camera.md §6.3-§6.5, protocol v3 §5.2, §5.6-§5.8, §7):
// clip names and ids, the sidecar .json, the per-day index.json, listing with paging and cursors, the
// star rules, retention, the disk floor, leftovers from an interrupted clip, and the folder's
// protections. It handles files only: the .mp4 writer and reader (roll_media) and the thumbnailer
// (roll_triggers) write and read the bytes, and the host decides who may call what (v3 §3.1).
//
// This is part of the colour exception (pet-camera.md §6.6): the roll is the one place colour clips
// are kept. Nothing here opens or decodes a clip; a leftover .mp4 is probed through a callback.
//
// Everything a device sends (clip id, day, cursor) is checked against an exact pattern before it is
// used, and is never joined into a path unchecked (v3 §7.1). Listing never creates or rewrites a file
// (v3 §5.6); only the host's own start-up and retention passes rebuild an index.
#pragma once

#include "pet_role.hpp"
#include "wire_protocol.hpp" // ClipTrigger

#include <cstdint>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kvhost {
namespace roll {

constexpr int kDefaultKeepDays = 30;                          // pet-camera.md §6.4
constexpr uint64_t kDefaultCapBytes = 200'000'000'000ULL;     // 200 GB
constexpr uint64_t kDiskFloorBytes = 20'000'000'000ULL;       // below this, recording stops (disk_floor)
constexpr uint64_t kDiskResumeBytes = 21'000'000'000ULL;      // above this, it resumes
constexpr int kListDefaultLimit = 50;                         // v3 §5.6
constexpr int kListMaxLimit = 100;
constexpr int kMaxNameSuffix = 999;                           // "-n" has at most 3 digits (v3 §7.1)
constexpr const char* kPcDeviceId = "pc";                     // the PC stars as an owner (§6.3)
constexpr const char* kPcDeviceName = "This PC";
constexpr const wchar_t* kIndexName = L"index.json";
constexpr const wchar_t* kDeletionLogName = L"deletion-log.txt"; // same name and format as the depth/IR log
// The marker a test run's host writes in the roll folder it prepares: a test run accepts
// only a folder that is new, empty, or holds it, so a test can never record into, tidy or age out a real roll.
constexpr const wchar_t* kTestRollMarker = L".kinect-viewer-test-roll";

// ---- names, ids, days and cursors (v3 §7.1, §5.6) -------------------------------------------------

// A PC-local wall-clock time and its offset from UTC, for names and startLocal.
struct LocalTime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    int offsetMinutes = 0; // local minus UTC, e.g. -300 for UTC-05:00
};
// The PC's time zone at that instant (SystemTimeToTzSpecificLocalTime).
LocalTime pcLocalTime(int64_t unixMs);

std::string dayString(const LocalTime& t);       // "2026-11-18"
std::string isoLocalString(const LocalTime& t);  // "2026-11-18T14:03:22-05:00"
// "HH-MM-SS_<trigger>", plus "-<n>" when n >= 2. Empty for ClipTrigger::Unknown.
std::string clipName(const LocalTime& t, ClipTrigger trigger, int n = 1);

// ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ and a real date.
bool isValidDay(const std::string& day);
// The v3 §7.1 pattern, a real date and an in-range time. Fails closed on anything else (separators,
// "..", drive letters, NUL, non-ASCII digits, whitespace).
bool isValidClipId(const std::string& clipId);
// The trigger in a valid clip id's name (Unknown if the id isn't valid).
ClipTrigger triggerOfClipId(const std::string& clipId);

// Opaque cursors (v3 §5.6): base64url without padding of "<startUnixMs>:<clipId>" or "<day>".
std::string encodeClipCursor(uint64_t startUnixMs, const std::string& clipId);
std::string encodeDayCursor(const std::string& day);
struct ClipCursor {
    uint64_t startUnixMs = 0;
    std::string clipId;
};
// Only the shape is checked: the anchor doesn't have to exist any more.
std::optional<ClipCursor> decodeClipCursor(const std::string& cursor);
std::optional<std::string> decodeDayCursor(const std::string& cursor);

// ---- the sidecar (pet-camera.md §6.3) -------------------------------------------------------------

struct StarEntry {
    std::string deviceId;
    PetRole role = PetRole::Viewer;
};

struct StartedBy {
    std::optional<std::string> deviceId; // absent for the PC
    std::string deviceName;
};

struct ClipSidecar {
    int version = 1;
    std::string clipId;
    uint64_t startUnixMs = 0;
    uint64_t endUnixMs = 0;
    std::string startLocal;
    double durationSec = 0.0;
    ClipTrigger trigger = ClipTrigger::Manual;
    std::optional<StartedBy> startedBy; // manual clips only; never written for motion or schedule
    std::vector<StarEntry> starredBy;
    int width = 0, height = 0;
    int modeRaw = 0, sbsLayoutRaw = 0;
    uint64_t bytes = 0; // the .mp4's size
    std::string codec = "h264";
    double preRollSec = 0.0;

    bool starred() const { return !starredBy.empty(); }
};

// The §6.3 fields in order. "starred" is written from starredBy.
std::string sidecarJson(const ClipSidecar& s);
// Lenient: unknown keys are ignored and a missing or mistyped field keeps its default. False only when
// the text isn't a JSON object. The caller sets clipId from the file's location, not from the text.
bool parseSidecar(const std::string& text, ClipSidecar& out);

// ---- results --------------------------------------------------------------------------------------

// Maps to the v3 §9 codes (rollErrorCode).
enum class RollError { None, NotFound, Invalid, NotAllowedForRole, Internal };
const char* rollErrorCode(RollError e); // nullptr for None, otherwise a pet_error:: code

struct RollDayItem { // v3 §5.7 RollDay
    std::string day;
    int clipCount = 0;
    int starredCount = 0;
    uint64_t bytes = 0;
};

struct RollClipItem { // v3 §5.7 RollClip
    std::string clipId;
    uint64_t startUnixMs = 0;
    std::string startLocal;
    double durationSec = 0.0;
    ClipTrigger trigger = ClipTrigger::Manual;
    std::optional<std::string> startedBy; // the device name, or "This PC"
    bool starred = false;
    bool starredByMe = false;
    int width = 0, height = 0;
    uint64_t bytes = 0;
    bool hasThumbnail = false;
};

struct RollStorageInfo { // v3 §5.7 RollStorage
    uint64_t usedBytes = 0;
    uint64_t capBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t floorBytes = kDiskFloorBytes;
    int keepDays = kDefaultKeepDays;
};

// What the store knows of its own size without walking the folder: kept up to date as clips are
// finalized, deleted and aged out, and walked again in full by the host (recount) at most every 60 s, off the
// PC's UI thread. Reading it never takes the store's lock, so it never waits on retention or a start-up pass.
struct RollStorageCounts {
    bool counted = false;          // a full walk has run at least once
    uint64_t usedBytes = 0;        // every file in the roll
    uint64_t starredBytes = 0;     // the starred clips' files
    std::optional<uint64_t> freeBytes; // as last read (the disk-floor check, a walk, a 0x12)
    uint64_t capBytes = kDefaultCapBytes;
    int keepDays = kDefaultKeepDays;
    bool limitsSet = false;        // setLimits was called: the owner's limits are in force (pet-camera.md §6.4)
    uint64_t walkedAtMs = 0;       // GetTickCount64 at the last full walk
};

struct DayListResult {
    RollError error = RollError::None;
    std::vector<RollDayItem> days;
    std::optional<std::string> nextCursor;
};

struct ClipListResult {
    RollError error = RollError::None;
    std::vector<RollClipItem> clips;
    std::optional<std::string> nextCursor;
};

// Where one clip's files go. reserveClip creates the .mp4 (empty) so the name is taken.
struct ClipPaths {
    std::string clipId;
    std::wstring dayDir;
    std::wstring mp4;
    std::wstring sidecar;
    std::wstring thumbnail;
};

// What roll_media reads back from a leftover .mp4 (one with no sidecar), to rebuild its sidecar.
struct ProbedClip {
    double durationSec = 0.0;
    int width = 0, height = 0;
    std::optional<uint64_t> startUnixMs; // the file's creation time; else the mp4's file time is used
    int modeRaw = 0, sbsLayoutRaw = 0;
};
// nullopt when the file can't be read (a plain MP4 cut short, an empty reservation).
using ProbeFn = std::function<std::optional<ProbedClip>(const std::wstring& mp4Path)>;

struct PassReport {
    std::vector<std::string> deleted;   // clip ids deleted and logged
    std::vector<std::string> recovered; // leftovers whose sidecar was rebuilt
    int indexesRebuilt = 0;
    int dayFoldersRemoved = 0;
    bool storageFull = false;
};

// ---- the folder (pet-camera.md §6.5) --------------------------------------------------------------

struct FolderCheckOptions {
    std::vector<std::wstring> extraOneDriveRoots;           // tests
    std::function<unsigned(const std::wstring& root)> driveType; // default GetDriveTypeW
};
// Refuses OneDrive, a network drive or UNC path, the depth/IR recordings folder and anything inside it,
// and an existing folder holding anything that isn't part of a camera roll. Then creates the folder
// with the owner-only DACL, NOT_CONTENT_INDEXED and the .nosync and .metadata_never_index markers.
bool prepareRollFolder(const std::wstring& dir, const std::wstring& recordingsDir, std::string* err,
                       const FolderCheckOptions& opts = {});
// A test run's folder must be new (not there yet), empty, or a test roll (it holds kTestRollMarker). Read only.
// False with err for anything else, a real roll included, so a test can never touch one.
bool checkTestRollFolder(const std::wstring& dir, std::string* err);
// Writes the marker into a prepared test roll.
bool writeTestRollMarker(const std::wstring& dir, std::string* err);
// The same refusals, read only: nothing is created or changed. True for a folder that doesn't exist yet on a
// drive that does (prepareRollFolder creates it later).
bool checkRollFolder(const std::wstring& dir, const std::wstring& recordingsDir, std::string* err,
                     const FolderCheckOptions& opts = {});

// ---- the store ------------------------------------------------------------------------------------

struct RollConfig {
    std::wstring dir; // the camera-roll folder, already prepared with prepareRollFolder
    uint64_t capBytes = kDefaultCapBytes;
    int keepDays = kDefaultKeepDays;
    std::function<int64_t()> nowUnixMs;                                      // default: the system clock
    std::function<LocalTime(int64_t unixMs)> toLocal;                         // default: pcLocalTime
    std::function<std::optional<uint64_t>(const std::wstring& dir)> freeBytes; // default: GetDiskFreeSpaceExW
    ProbeFn probe; // roll_media's reader; empty means a leftover .mp4 is always deleted and logged
};

// One store per camera-roll folder. Every call takes the store's lock, so the host may call it from any
// thread; a star's read-modify-write of a sidecar can't interleave with another.
class RollStore {
public:
    explicit RollStore(RollConfig cfg);

    const RollConfig& config() const { return cfg_; }
    // The owner's retention settings. Until it is called, storage_full is never raised: the default cap is
    // not the owner's (pet-camera.md §6.4), and only the disk floor stops recording.
    void setLimits(uint64_t capBytes, int keepDays);

    // The cached counts (no lock but a leaf one, no walk), and a full walk that refreshes them.
    RollStorageCounts counts() const;
    void recount();

    // -- writing a clip (pet-camera.md §6.1, §6.3)
    // Picks the day folder and the first free name for a clip starting at startUnixMs, and creates
    // the empty .mp4 so the name stays taken. nullopt (with err) if no name is free or a file fails.
    std::optional<ClipPaths> reserveClip(int64_t startUnixMs, ClipTrigger trigger, std::string* err = nullptr);
    // Writes the sidecar last, atomically, once the .mp4 (and thumbnail) are closed. Fills clipId,
    // version, startLocal, bytes (the .mp4's size) and codec; the caller fills the rest.
    bool finalizeClip(const ClipPaths& paths, ClipSidecar sidecar, std::string* err = nullptr);
    // A reservation that never became a clip (the writer failed): removes its files, unlogged.
    void abandonClip(const ClipPaths& paths);

    // -- the host's own passes: the only calls that write an index.json
    // Start-up, before any clip is recorded: removes .tmp files, orphan sidecars and thumbnails,
    // recovers or deletes leftover .mp4 files, removes empty day folders and rebuilds stale indexes. It
    // never runs retention: the user's limits aren't known yet, and defaults never delete a clip.
    PassReport startupPass();
    // Retention (§6.4) every hour and after each clip, then rebuilds every stale day index.
    PassReport retentionPass();

    // -- listing (v3 §5.6-§5.7): reads only, newest first
    DayListResult listDays(const std::optional<std::string>& cursor, std::optional<int> limit) const;
    ClipListResult listClips(const std::string& day, const std::optional<std::string>& cursor,
                             std::optional<int> limit, const std::string& requesterDeviceId) const;
    RollStorageInfo storage() const;

    // -- one clip, by a device's id (validated; not_found otherwise)
    std::optional<ClipSidecar> findClip(const std::string& clipId) const;
    std::optional<std::wstring> mp4Path(const std::string& clipId) const;       // for playback
    std::optional<std::wstring> thumbnailPath(const std::string& clipId) const; // nullopt without a .jpg

    // -- stars and deletion (v3 §5.2, §3.2)
    // starred: adds the caller's entry. Unstar: an owner (or the PC, deviceId "pc") clears every entry;
    // a viewer removes only its own, and NotAllowedForRole if it has none. Rewrites only the sidecar.
    RollError setStar(const std::string& clipId, const std::string& deviceId, PetRole role, bool starred);
    // The owner's or the PC's delete: the .mp4, .json and .jpg, logged as "user".
    RollError deleteClip(const std::string& clipId);
    // Revoke (v3 §3.2): drops the device's stars across the roll. Returns the number of clips changed.
    int removeStarsByDevice(const std::string& deviceId);
    // Drops every star set by one of these devices (the PC's own stars, "pc", always stay): devices revoked while
    // this roll wasn't running (v3 §3.2). Only explicitly revoked devices: one merely missing from a device list
    // keeps its stars (a list rebuilt from pairing.json lists only the paired owner). Returns the clips changed.
    int removeStarsOfDevices(const std::set<std::string>& revoked);

    // -- playing clips: retention skips a clip that is being played (it is retried at the next pass); an
    // owner's delete still deletes it, after the host has stopped its playbacks (v3 §5.2)
    void pinClip(const std::string& clipId);
    void unpinClip(const std::string& clipId);

    // -- the disk floor (§6.4), with hysteresis
    // Reads free space through the seam and updates the state: below 20 GB blocks, above 21 GB
    // resumes. True while recording may run. A failed read counts as no space (fails closed).
    bool checkDiskFloor();
    bool diskFloorBlocked() const;
    // Starred clips alone over the cap (§6.4 step 3), as of the last pass or star change.
    bool storageFull() const;
    // Both flags read together, with a generation that grows each time either is measured, so a caller that
    // applies them later can tell an older reading from a newer one and never let the older win.
    struct StorageFlags {
        bool storageFull = false;
        bool floorBlocked = false;
        uint64_t gen = 0;
    };
    StorageFlags flags() const;

private:
    struct ClipFiles;
    struct DaySnapshot;
    DaySnapshot snapshotDay(const std::string& day) const;
    std::vector<std::string> dayFolders() const;
    // unreadable: the names whose sidecar couldn't be read, listed with defaults but never taken as unstarred.
    std::vector<ClipSidecar> clipsOfDay(const DaySnapshot& snap, bool* indexFresh,
                                        std::set<std::string>* unreadable = nullptr) const;
    bool writeIndex(const DaySnapshot& snap, const std::vector<ClipSidecar>& clips, std::string* err) const;
    bool writeSidecar(const std::wstring& dayDir, const std::string& name, const ClipSidecar& s, std::string* err) const;
    uint64_t removeClipFiles(const std::wstring& dayDir, const std::string& name, bool* complete) const;
    void logDeletion(const std::string& clipId, uint64_t bytes, bool user, bool complete) const;
    void runRetentionLocked(PassReport& report);
    void tidyDaysLocked(PassReport& report); // empty day folders removed, stale indexes rebuilt
    void refreshStorageFullLocked();
    void recountLocked();
    void addUsedBytes(int64_t delta);
    std::optional<std::pair<std::string, std::string>> splitValidId(const std::string& clipId) const;
    std::optional<uint64_t> readFreeBytes() const;
    int64_t now() const;
    LocalTime local(int64_t unixMs) const;

    bool pinnedLocked(const std::string& clipId) const;
    int removeStarsIfLocked(const std::function<bool(const std::string& deviceId)>& drop);

    RollConfig cfg_;
    mutable std::mutex m_;
    mutable std::mutex pinM_; // a leaf lock: taken under m_ by retention, and alone by the host
    std::map<std::string, int> pins_;
    bool floorBlocked_ = false;
    bool storageFull_ = false;
    bool limitsSet_ = false; // under m_: setLimits was called
    uint64_t flagsGen_ = 0;  // under m_: bumped each time storageFull_ or floorBlocked_ is measured
    mutable std::mutex countsM_; // a leaf lock: taken under m_ and alone by readers
    mutable RollStorageCounts counts_;
};

} // namespace roll
} // namespace kvhost
