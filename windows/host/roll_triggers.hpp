// Camera-roll triggers (pet-camera.md §6.2, protocol v3 §5.1/§5.2): the depth motion detector and the
// trigger-set state machine that decides when a clip starts, is extended, is cut into segments and ends.
//
// Both are pure logic. The detector takes one depth frame and its timestamp at a time and never reads
// the engine; the host feeds it from kv_engine_copy_latest_raw16 (depth is decoded in every view mode,
// so motion is always available). The trigger set has no I/O and no threads: it reads an injected clock
// and reports what should happen as events, which the host drains and acts on (open a writer, finalize
// it, take a thumbnail, send a 0x0B).
//
// Thread safety: none; the caller guards each object and calls it from one thread at a time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wire_protocol.hpp" // ClipTrigger

namespace kvhost {

// ---- The depth motion detector (pet-camera.md §6.2 "Motion by depth") -----------------------------

struct MotionResult {
    bool frameUsed = false;  // false when the frame was rejected (wrong size, no data)
    bool motion = false;     // at least `threshold` changed cells in each of the last 3 consecutive frames
    int changedCells = 0;    // cells changed against the background in this frame
    int validCells = 0;      // cells valid in this frame that already had a background
    int threshold = 0;       // N for this frame (0 when no cell could be compared)
    int consecutive = 0;     // consecutive frames, ending with this one, with at least N changed cells
    int relearnedCells = 0;  // cells whose background was re-learned in this frame (see below)
    int globalConsecutive = 0; // consecutive frames, ending with this one, with over 60 % of cells changed
    bool globalRelearn = false; // the whole background was re-learned on this frame (see below); never motion
};

// Background subtraction on a coarse grid of depth medians. Depth doesn't change with the light, so
// lamps and daylight can't trip it.
//
// - Grid: the 512x424 depth (mm, 0 = invalid) is cut into 64x53 cells of 8x8 pixels. A cell's value is
//   the median of its non-zero pixels (the upper middle one for an even count). A cell with fewer than
//   kMinValidPixels (16 of 64, a quarter) valid pixels is invalid in that frame: it is neither compared
//   nor learned, and its background is kept for the next frame where it is valid.
// - Background: the first valid value of a cell becomes its background (that frame never counts as
//   changed). After that, an unchanged cell adapts by an exponential average, bg += 0.02 * (d - bg).
// - Changed cell: |d - bg| > max(40 mm, 3 % of bg). Changed cells don't update the background.
// - Motion: at least N changed cells in each of 3 consecutive frames. N = ceil(valid * p), at least 1,
//   where valid is the number of compared cells in that frame and p falls linearly from 2 % at
//   sensitivity 1 to 0.2 % at sensitivity 10 (p = 2.0 % - 0.2 % per step). Frames are consecutive when
//   each one's sequence number is the previous one's plus 1, and its timestamp is after the previous
//   one's and at most 1 s later. Anything else restarts the count: the frame may start a new run but
//   never completes one with frames from before it.
//   - Sequence (W7): a depth frame that comes after a dropped frame never
//     counts as motion with the frames before the drop. A gap, a repeated number or one going backwards
//     all restart the count. The first frame after construction or reset() has no previous number.
//   - Time: a stall of more than 1 s, or a timestamp going backwards, restarts it too, so a stall can't
//     join two separate blips into one motion.
// - Re-learning, "after 60 s without motion": changed cells don't adapt, so something that moves into
//   view and then stays (a pet lying down, a chair moved) would otherwise count as motion for ever. Each
//   changed cell therefore remembers the value it settled at; while it stays changed and keeps within
//   max(40 mm, 3 %) of that value, nothing is moving there. Once a cell has been changed but still for
//   kRelearnAfterNs (60 s of frame timestamps), its background becomes its current value, and it stops
//   counting. A cell that moves again restarts its 60 s; a cell that goes back to its background, or is
//   invalid, forgets the settled value (invalid frames don't restart it).
// - Whole-scene change (W7): more than kGlobalRelearnPercent (60 %) of the
//   compared cells changing at once is the camera being moved or the scene changing, not a pet. When it
//   holds for 3 consecutive frames (the same sequence and time rules as motion), the third frame
//   re-learns the whole background at once: every cell valid in that frame takes its current value as
//   its background and forgets its settled value, and both runs restart. A frame over 60 % never
//   reports motion, and the re-learning frame sets globalRelearn so the host can log it. A big change
//   that drops back under 60 % before its third frame is judged as ordinary motion from then on.
// - reset(): forgets the whole background. Call it when the sensor starts, the cut-off changes and the
//   view is unfrozen (§6.2); the next valid frame is learned afresh.
class DepthMotionDetector {
public:
    static constexpr uint32_t kDepthWidth = 512;
    static constexpr uint32_t kDepthHeight = 424;
    static constexpr int kCellSize = 8;
    static constexpr int kGridWidth = 64;  // 512 / 8
    static constexpr int kGridHeight = 53; // 424 / 8
    static constexpr int kCellCount = kGridWidth * kGridHeight;
    static constexpr int kMinValidPixels = 16;
    static constexpr double kAlpha = 0.02;
    static constexpr double kMinChangeMm = 40.0;
    static constexpr double kChangeFraction = 0.03;
    static constexpr int kConsecutiveFrames = 3;
    static constexpr int kGlobalRelearnPercent = 60; // of the compared cells changed at once
    static constexpr uint64_t kMaxFrameGapNs = 1000000000ULL;      // 1 s
    static constexpr uint64_t kRelearnAfterNs = 60000000000ULL;    // 60 s
    static constexpr int kMinSensitivity = 1, kMaxSensitivity = 10, kDefaultSensitivity = 5;

    DepthMotionDetector();

    // 1 (least sensitive) to 10 (most); values outside are clamped. Takes effect from the next frame and
    // keeps the background.
    void setSensitivity(int sensitivity);
    int sensitivity() const { return sensitivity_; }

    // One depth frame: `width` x `height` uint16 millimetres, row-major with no padding, 0 = invalid.
    // `seq` is the frame's sequence number (kv_engine_copy_latest_raw16's out_seq, which counts up across
    // engine restarts) and `timestampNs` its capture time (that call's arrival time). A frame of any other
    // size than 512x424, or a null pointer, is rejected (frameUsed false) and changes nothing, its
    // sequence number included.
    MotionResult process(const uint16_t* depth, uint32_t width, uint32_t height, uint64_t seq, uint64_t timestampNs);

    void reset();

    // N for `validCells` compared cells at `sensitivity` (clamped to 1-10); 0 when validCells <= 0.
    static int thresholdCells(int validCells, int sensitivity);

private:
    struct Cell {
        float bg = 0.0f;
        bool hasBg = false;
        float settled = 0.0f;     // the value a changed cell settled at
        bool hasSettled = false;
        uint64_t settledSinceNs = 0;
        float current = 0.0f; // this frame's value, when currentValid
        bool currentValid = false;
    };
    std::vector<Cell> cells_;
    int sensitivity_ = kDefaultSensitivity;
    int consecutive_ = 0;
    int globalConsecutive_ = 0;
    bool haveLastTs_ = false;
    uint64_t lastTsNs_ = 0;
    bool haveLastSeq_ = false;
    uint64_t lastSeq_ = 0;
};

// ---- The trigger set (pet-camera.md §6.2) ----------------------------------------------------------

// ClipTrigger is wire_protocol's (the v3 values, plus Unknown for lenient decoding), so the trigger set,
// the roll store and the wire share one type. The clip's name and sidecar use the string.
const char* clipTriggerName(ClipTrigger t); // "manual", "motion", "schedule"; "unknown" for Unknown

// Why recording can't run now: the v3 §5.1 rollBlockedReason values, which are also start_clip's 0x0D
// error codes (§5.2, §9).
enum class RollBlock { None, PrivacyMode, RollDisabled, RollPreparing, SensorOff, ViewFrozen, PcReplaying, DiskFloor, StorageFull };
const char* rollBlockCode(RollBlock b); // "privacy_mode", ...; "" for None

// Everything a trigger needs (§6.2 "What every trigger needs", plus the disk floor and storage_full of
// §6.4). The host keeps this up to date.
struct RollConditions {
    bool rollEnabled = false; // "Record to the camera roll" (D5: gates every trigger, manual included)
    bool preparing = false;   // the roll is on and its folder is being prepared (roll_preparing)
    bool privacyMode = false;
    bool sensorOn = false;
    bool viewFrozen = false;
    bool pcReplaying = false;
    bool diskFloor = false;   // below the 20 GB floor and not yet back above 21 GB (roll_store's hysteresis)
    bool storageFull = false; // starred clips alone are over the cap
};

// The one block to report when several hold, in the order v3 §5.1 lists rollBlockedReason: privacy_mode,
// roll_disabled, roll_preparing, sensor_off, view_frozen, pc_replaying, disk_floor, storage_full.
RollBlock rollBlockFor(const RollConditions& c);

// The sidecar's startedBy for a manual clip (§6.3): a device's id and name, or the PC as
// {"deviceName":"This PC"} with no id.
struct ClipStartedBy {
    std::string deviceId; // empty for the PC
    std::string deviceName;
    static ClipStartedBy pc() { return ClipStartedBy{std::string(), "This PC"}; }
    static ClipStartedBy device(std::string id, std::string name) { return ClipStartedBy{std::move(id), std::move(name)}; }
    bool operator==(const ClipStartedBy& o) const { return deviceId == o.deviceId && deviceName == o.deviceName; }
};

// One scheduled-recording row (§4 "Rows of days, start time and length"), in PC-local time. The day
// mask uses the same bits as SharingScheduleRow (bit 0 = Sunday ... bit 6 = Saturday) and names the day
// the row starts on; a row that starts late in the evening runs on past midnight into the next day. A row
// with no days, a start past 23:59 or a length outside 1-60 minutes never fires.
struct RecordScheduleRow {
    uint8_t days = 0x7F;
    uint16_t startMinute = 0;   // minutes since local midnight
    uint16_t lengthMinutes = 0; // 1-60
    bool operator==(const RecordScheduleRow& o) const {
        return days == o.days && startMinute == o.startMinute && lengthMinutes == o.lengthMinutes;
    }
};

// PC-local wall-clock time, to the second.
struct RollLocalTime {
    int weekday = 0;     // 0 = Sunday ... 6 = Saturday
    int secondOfDay = 0; // 0 - 86399
};

// True when any row's window contains `t`.
bool isScheduledRecording(const std::vector<RecordScheduleRow>& rows, const RollLocalTime& t);

// The trigger set's clock: a monotonic millisecond count for every timer, and the local wall clock for
// the schedule (so a DST change or a clock change moves schedule windows as the user expects).
class RollClock {
public:
    virtual ~RollClock() = default;
    virtual uint64_t monoMs() const = 0;
    virtual RollLocalTime localTime() const = 0;
};

// GetTickCount64 and GetLocalTime.
class SystemRollClock : public RollClock {
public:
    uint64_t monoMs() const override;
    RollLocalTime localTime() const override;
};

enum class RollEventKind {
    StartClip,      // open a new clip (a new file); `trigger` names it
    ExtendClip,     // `trigger` joined the running clip's set
    EndClip,        // finalize the running clip
    ThumbnailDue,   // take the clip's thumbnail from the live BGRA frame now
    BlockedChanged, // rollBlockedReason changed to `block` (None: recording may run)
};

enum class ClipStartCause {
    Trigger,     // a trigger started it from idle
    Segment,     // the previous segment reached 10 minutes with triggers still active
    ViewChanged, // the view's mode or size changed with triggers still active (§6.1)
    Unblocked,   // a block cleared inside a schedule row's window (§6.2)
};

enum class ClipEndReason {
    TriggersEnded, // no trigger left active
    SegmentCap,    // 10 minutes; a StartClip with cause Segment follows at once
    ViewChanged,   // a StartClip with cause ViewChanged follows at once
    Blocked,       // `block` says which; every trigger was dropped
    Shutdown,      // the app is exiting
    WriterFailed,  // abortClip(): the host couldn't write the clip; every trigger was dropped
};

const char* clipEndReasonName(ClipEndReason r); // "triggers_ended", "segment_cap", ... (for logs)

struct RollEvent {
    RollEventKind kind = RollEventKind::StartClip;
    uint64_t clipSeq = 0;  // the clip (segment) this is about: 1, 2, 3, ... per TriggerSet
    uint64_t atMonoMs = 0; // the clock when it happened
    ClipTrigger trigger = ClipTrigger::Manual; // StartClip: names the clip; ExtendClip: the joiner
    ClipStartCause cause = ClipStartCause::Trigger; // StartClip
    uint32_t preRollMs = 0;                         // StartClip: wanted pre-roll (the ring gives up to this)
    std::optional<ClipStartedBy> startedBy;         // StartClip: set when the clip is named `manual`
    ClipEndReason endReason = ClipEndReason::TriggersEnded; // EndClip
    bool thumbnailTaken = false; // EndClip: a ThumbnailDue was sent for this clip
    RollBlock block = RollBlock::None; // EndClip with Blocked, and BlockedChanged
};

struct StartClipResult {
    bool ok = false;
    RollBlock refused = RollBlock::None; // when !ok: the 0x0D error is rollBlockCode(refused)
};

enum class StopClipResult { Ok, Busy };

// The set of active triggers and the clip they hold open (pet-camera.md §6.2). Every input first brings
// the timers up to the clock's now (as tick() does), then applies itself; the resulting events queue up
// in order until takeEvents().
//
// Rules:
// - A clip runs while at least one trigger is active. Its name and trigger are those of the trigger
//   that started it; a trigger that fires during a clip joins the set (ExtendClip, sent only when the
//   trigger wasn't active already).
// - Manual: added by startClip() (the PC button, or 0x0C start_clip once the host has checked the role).
//   It lasts until stopClip() or 10 minutes after the start. A startClip() while the manual trigger is
//   active restarts its 10 minutes; the startedBy recorded is the one that added it. Pre-roll 5 s when it
//   starts a clip, and then its 10 minutes count from the clip's first frame, pre-roll included, as the
//   segment cut does (setClipPreRoll), so a full manual clip leaves no stub.
// - Motion: onMotion() each time the detector reports motion. Active until 10 s after the last report.
//   Pre-roll 5 s. For 30 s after a clip ends because its last trigger ended, motion can't start a clip
//   (reports then are ignored, not remembered), but it can join one that is running. A clip ended by a
//   block or by shutdown starts no cooldown: §6.2 says motion starts clips again as soon as it detects
//   something once the block clears. Reports while motion is disabled are ignored, and disabling it
//   drops the motion trigger.
// - Schedule: active while the local clock is inside an enabled row's window (isScheduledRecording).
//   It needs no event: tick() starts it, at the row's start or whenever recording becomes possible inside
//   the window (the app starting, the schedule being turned on, a block clearing). No pre-roll: the
//   clip starts at the scheduled time, and §6.2 gives pre-roll only to manual and motion.
// - Segments: a clip that reaches 10 minutes ends (SegmentCap) and, with triggers still active, the next
//   segment starts at once with no pre-roll. The 10 minutes are the file's, pre-roll included (§6.2): the
//   host reports the pre-roll it gave a clip (setClipPreRoll), and the cap counts from the clip's first
//   frame. Trigger timers are applied first, so a manual trigger alone that reaches its own 10 minutes
//   just ends the clip.
// - View change: onViewChanged() ends the running clip (ViewChanged) and starts the next at once with no
//   pre-roll (§6.1); with no clip running it does nothing.
// - Naming a continuing clip (Segment, ViewChanged): the previous clip's trigger if it is still active,
//   otherwise the first active one in the order manual, schedule, motion (a person's request first, then
//   the planned recording). The spec gives no precedence; this is this module's choice.
// - Blocks: while rollBlockFor(conditions) isn't None nothing starts. A block arriving ends the clip
//   (Blocked) and drops every trigger: the manual one as §6.2 says, and motion too, since motion "starts
//   clips again as soon as it detects something". When it clears, a schedule window still open restarts
//   at once (cause Unblocked, no pre-roll). BlockedChanged is sent for every change of the reason,
//   before the EndClip it causes.
// - Thumbnail: ThumbnailDue 1 s after each clip (segment) starts (§6.1: 1 s after the trigger, or the
//   start for schedule clips; a clip starts when its trigger fires). A clip that ends sooner because its
//   triggers ended, at a segment cut or at shutdown gets its ThumbnailDue just before its EndClip. A clip
//   ended by a block or a view change doesn't: the live frame then is behind privacy, frozen, off or a
//   different view, so EndClip says thumbnailTaken false and the host decides (for example, no .jpg).
class TriggerSet {
public:
    static constexpr uint64_t kManualMaxMs = 10ULL * 60 * 1000;
    static constexpr uint64_t kMotionHoldMs = 10ULL * 1000;
    static constexpr uint64_t kMotionCooldownMs = 30ULL * 1000;
    static constexpr uint64_t kSegmentMaxMs = 10ULL * 60 * 1000;
    static constexpr uint64_t kThumbnailDelayMs = 1000;
    static constexpr uint32_t kPreRollMs = 5000;

    explicit TriggerSet(const RollClock& clock); // the clock must outlive the set

    void setConditions(const RollConditions& c);
    void setMotionEnabled(bool enabled);
    // Rows with an invalid day mask, start or length are kept but never fire.
    void setSchedule(std::vector<RecordScheduleRow> rows, bool enabled);

    // 0x0C start_clip / the PC button. Refused, with no trigger added, while blocked.
    StartClipResult startClip(const ClipStartedBy& who);
    // 0x0C stop_clip / the PC button (W7):
    // - not recording: Ok, a no-op (a clip can end by itself just before the request arrives);
    // - recording with the manual trigger active: Ok, and the manual trigger is dropped; the clip goes on
    //   while motion or the schedule still holds it;
    // - recording with no manual trigger (a motion or schedule clip): Busy, as the manual stop doesn't
    //   apply to it (v3 §5.2).
    // A manual trigger is only ever active while a clip records (a block drops it), so these are all
    // the cases.
    StopClipResult stopClip();

    void onMotion();
    void onViewChanged();
    // The host gave clip `clipSeq` this much pre-roll (its file starts that long before the trigger): the
    // 10-minute segment cap counts from the file's first frame (pet-camera.md §6.2). Ignored for a clip
    // that isn't the running one.
    void setClipPreRoll(uint64_t clipSeq, uint64_t preRollMs);
    // Applies the timers: trigger expiry, the 10-minute segment cut, schedule windows, thumbnails. Call it
    // often (each frame, or a few times a second); every other input also calls it first.
    void tick();
    // The app is exiting: ends the running clip (Shutdown) and drops every trigger. The host sends no
    // input after it (a later tick() inside a schedule window would start a clip again).
    void shutdown();
    // The host couldn't write the running clip (its writer failed to open or fell over): ends it
    // (WriterFailed, with no ThumbnailDue) and drops every trigger, failing closed so a broken writer is
    // not retried at once: the manual trigger must be started again, motion gets its 30 s cooldown, and
    // the schedule stays off until its current window has closed. Nothing happens when no clip runs.
    void abortClip();

    std::vector<RollEvent> takeEvents();

    // For 0x0B (v3 §5.1).
    RollBlock blocked() const { return block_; }
    bool recording() const { return clipOpen_; }
    std::optional<ClipTrigger> clipTrigger() const; // the running clip's trigger
    bool manualActive() const { return manual_.has_value(); }
    bool motionActive() const { return motionUntilMs_.has_value(); }
    bool scheduleActive() const { return scheduleActive_; }
    double clipElapsedSec() const; // the running clip (segment) since it started; 0 when idle
    bool motionCooldown() const;   // motion can't start a clip now
    uint64_t clipSeq() const { return clipOpen_ ? clipSeq_ : 0; }

private:
    struct Manual {
        ClipStartedBy who;
        uint64_t sinceMs = 0;
    };

    void advance(uint64_t now);
    void applyBlock(RollBlock b, uint64_t now);
    void startClip_(ClipTrigger t, ClipStartCause cause, uint32_t preRollMs, uint64_t now);
    void endClip_(ClipEndReason r, uint64_t now, RollBlock block = RollBlock::None);
    void continueClip_(ClipEndReason r, ClipStartCause cause, uint64_t now);
    void endIfIdle(uint64_t now);
    bool anyTrigger() const { return manual_.has_value() || motionUntilMs_.has_value() || scheduleActive_; }
    bool isActive(ClipTrigger t) const;
    ClipTrigger nextName(ClipTrigger previous) const;
    void emitThumbnail(uint64_t now);
    void push(RollEvent e) { events_.push_back(std::move(e)); }

    const RollClock& clock_;
    RollConditions conditions_;
    RollBlock block_ = RollBlock::RollDisabled; // the default conditions have the roll off
    bool motionEnabled_ = false;
    std::vector<RecordScheduleRow> schedule_;
    bool scheduleEnabled_ = false;

    std::optional<Manual> manual_;
    std::optional<uint64_t> motionUntilMs_;
    bool scheduleActive_ = false;
    std::optional<uint64_t> cooldownUntilMs_;

    bool clipOpen_ = false;
    uint64_t clipSeq_ = 0;
    uint64_t clipStartMs_ = 0;
    uint64_t segmentStartMs_ = 0; // the clip's first frame: clipStartMs_ less its pre-roll
    ClipTrigger clipTrigger_ = ClipTrigger::Manual;
    bool thumbnailTaken_ = false;
    bool justUnblocked_ = false;
    bool scheduleSuppressed_ = false; // abortClip() inside a window: off until that window closes

    std::vector<RollEvent> events_;
};

} // namespace kvhost
