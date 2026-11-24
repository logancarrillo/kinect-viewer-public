// The camera-roll recorder (pet-camera.md §6.1-§6.4, protocol v3 §5.1-§5.2): it joins the roll's separate pieces
// into one running thing. The `medium` live tier's H.264 feeds the pre-roll ring; the trigger set
// (manual, motion by depth, schedule) decides when a clip runs; a pass-through writer records each clip
// over the .mp4 the store reserved; and each clip is finalized with its thumbnail, sidecar and index
// entry, followed by retention. The disk floor is checked before each clip and every 10 s.
//
// This is part of the colour exception (pet-camera.md §6.6): it is the one place that hands colour (the
// encoded `medium` picture and one live frame per clip for the thumbnail) to the roll's writer and
// thumbnailer, and it writes only into the camera-roll folder. Raw frames never leave it: the live frame
// becomes a JPEG in the roll and nothing else.
//
// Threads. Every input may come from any thread: the encoder's output thread (onMediumAccessUnit), the
// viewer's loop (submitDepth, onLiveFrame), the stream server's network threads (startClip, settings).
// State changes happen at once under the recorder's lock; the slow work (reserving and opening a clip,
// the thumbnail, finalizing, retention) runs on the recorder's own thread. Callbacks run on that thread
// with no lock held, so they may call into the stream server; the recorder never calls out while its
// lock is held, and nothing it calls takes it back.
#pragma once

#include "live_tiers.hpp" // TierAccessUnit
#include "roll_media.hpp"
#include "roll_store.hpp"
#include "roll_triggers.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kvhost {

// What the PC panel and 0x0B show of the roll (v3 §5.1 roll fields).
struct RollRecorderStatus {
    bool ready = false;            // the folder is prepared and the start-up pass has run
    bool enabled = false;          // "Record to the camera roll"
    RollBlock block = RollBlock::RollDisabled;
    bool armed = false;            // the `medium` tier runs for the pre-roll (enabled and nothing blocks)
    bool recording = false;        // a clip is open (clipRecording)
    std::optional<ClipTrigger> clipTrigger;
    bool manualActive = false;     // stop_clip will act
    double clipElapsedSec = 0.0;   // whole seconds, so 0x0B changes at most once a second (§5.1)
    bool motionEnabled = false;
    bool motionAvailable = false;  // the live sensor runs and the PC isn't replaying (pet-camera.md §6.2)
    int motionSensitivity = 5;
    bool scheduleEnabled = false;
    std::string scheduleSummary;   // recordScheduleSummary of the rows
    bool diskFloor = false;
    bool storageFull = false;
    uint64_t clipsFinalized = 0;   // since start
    uint64_t clipsFailed = 0;      // abandoned: the writer couldn't record them
    bool limitsApplied = false;    // the user's keep days and cap are in force and retention has run with them
    bool limitsKnown = false;      // the owner's limits are known: pet-camera.json held them, or the owner set them
    bool prepared = false;         // the folder is prepared: recording may start
    bool preparing = false;        // the roll is on and its folder is being prepared (v3 §5.1 roll_preparing)
    bool folderUnavailable = false; // a roll folder is set but can't be used; lastError says why (no recorder runs)
    std::string lastClipId;        // the last clip finalized
    std::string lastError;         // the last writer or finalize failure, for the PC's status line
};

class RollRecorder {
public:
    struct Options {
        std::wstring dir;            // the camera-roll folder; required (v3 §10: never a default here)
        std::wstring recordingsDir;  // the depth/IR recordings folder, refused as or around the roll (§6.5)
        // Test seams. clock: the trigger set's clock (nullptr: SystemRollClock). freeBytes: the disk-floor
        // check's free space (empty: GetDiskFreeSpaceExW). writerMaxQueuedBytes: what one clip's writer may
        // queue before it stops taking frames; a tiny value makes it fail at the first frame.
        const RollClock* clock = nullptr;
        std::function<std::optional<uint64_t>(const std::wstring& dir)> freeBytes;
        size_t writerMaxQueuedBytes = 64u * 1024u * 1024u;
        // More test seams. maxPendingBytes: what a clip may hold before its writer opens (past it the clip is
        // aborted). failAppendAfter: after this many live AUs reached a clip's open writer, the next append is
        // refused, as a writer that fails mid-clip (0: never). threadHook: called on the recorder's thread at
        // the top of every turn with no lock held, so a test can hold the thread busy.
        size_t maxPendingBytes = 32u * 1024u * 1024u;
        int failAppendAfter = 0;
        // The writer's WriteSample fails after this many samples (RollWriterOptions::test_fail_write_after),
        // and, when writerFailAfterFragments is not 0, only once that many whole fragments are on the disk
        // (RollWriterOptions::test_fail_after_fragments).
        uint64_t writerFailWriteAfter = 0;
        uint32_t writerFailAfterFragments = 0;
        std::function<void()> threadHook;
        // The recorder thread's tick, the disk-floor check while armed (§6.4: every 10 s), retention (every
        // hour), and how long a finished clip waits for the live frame its thumbnail is taken from.
        int tickMs = 100;
        int floorCheckMs = 10000;
        int retentionMs = 3600 * 1000;
        int thumbnailWaitMs = 1500;
        // How often the store's cached counts are walked again in full, on this thread.
        int recountMs = 60 * 1000;
        // A test run (HostController::Options::test_run): only synthetic frames may be recorded, and the
        // folder must be new, empty or a test roll.
        bool testRun = false;
    };

    struct Settings { // the roll part of PetSettings
        bool enabled = false;
        bool motionEnabled = false;
        int motionSensitivity = 5;
        bool scheduleEnabled = false;
        std::vector<RecordScheduleRow> schedule;
        int keepDays = roll::kDefaultKeepDays;
        uint64_t capBytes = roll::kDefaultCapBytes;
        // keepDays and capBytes are the user's: pet-camera.json was read successfully, or the owner set them
        // on the PC. Until then retention never runs (pet-camera.md §5 fail closed): defaults never delete a
        // clip, and only the disk floor, which deletes nothing, stops recording.
        bool limitsKnown = false;
        // pet-camera.json has been read (it isn't held unreadable). Until then the roll folder isn't prepared.
        bool settingsRead = false;
    };

    RollRecorder();
    ~RollRecorder();
    RollRecorder(const RollRecorder&) = delete;
    RollRecorder& operator=(const RollRecorder&) = delete;

    // Refuses the folder as pet-camera.md §6.5 says (OneDrive, a network drive, the depth/IR folder, a
    // folder holding other files), read only, and opens an existing roll for browsing. Nothing in the folder
    // is created or changed here: it is prepared (created, protected, its
    // start-up pass run: leftovers recovered or deleted, indexes rebuilt) on the recorder's thread the first
    // time recording may start: the roll on, pet-camera.json read, privacy mode off. onPrepared or
    // onUnavailable says how that went. Retention waits for the user's limits too (Settings::limitsKnown).
    // False with err when the folder is refused.
    bool start(const Options& opts, std::string* err);
    // The app is exiting (§6.1): the open clip is finalized, then the thread stops. Idempotent.
    void stop();
    bool running() const;

    // The store, for listing, thumbnails, playback, stars and deletion. Null before start(), and until the
    // folder is prepared when it didn't exist at start.
    std::shared_ptr<roll::RollStore> store() const;

    // Called from the recorder's thread with no lock held.
    std::function<void()> onStatusChanged;        // a 0x0B field changed (1 Hz while a clip records)
    std::function<void(bool armed)> onArmedChanged; // want or release the `medium` tier (§6.1)
    std::function<void()> onKeyframeWanted;       // a clip starts without pre-roll: an IDR on `medium` soon
    std::function<void(const std::string& clipId)> onClipFinalized;
    std::function<void()> onPrepared;                       // the folder is prepared; store() is the one to use
    std::function<void(const std::string& why)> onUnavailable; // the folder couldn't be prepared (fail closed)

    // ---- conditions (§6.2 "What every trigger needs") ----
    void setSettings(const Settings& s);
    void setPrivacyMode(bool on);
    void setSensorOn(bool on);
    void setViewFrozen(bool on);
    void setPcReplaying(bool on);

    // ---- triggers ----
    // 0x0C start_clip and the PC button: refused with the block (v3 §5.2) while blocked.
    StartClipResult startClip(const ClipStartedBy& who);
    StopClipResult stopClip();
    // One depth frame for motion (§6.2), from the live sensor only: ignored when live_sensor is false (a
    // replay), while recording is blocked, or while motion is off. A frame whose sequence number repeats
    // the last one given is ignored (the viewer polls faster than frames come); a gap restarts the
    // 3-frame run (the detector's rule), and so does the first frame after frames were ignored, which
    // also re-learns the background (the sensor starting, unfreezing, a block clearing).
    void submitDepth(const uint16_t* raw16, uint32_t width, uint32_t height, uint64_t seq, uint64_t timestampNs,
                     bool liveSensor);
    // The cut-off changed: the motion background starts again (§6.2).
    void resetMotion();
    // A test run was given a frame that isn't from the synthetic source. The roll stops for good (fail closed):
    // the open clip is thrown away, nothing more is kept or written, and lastError says why. Idempotent.
    void refuseRealColour(const std::string& why);

    // ---- media ----
    // Every AU of the `medium` tier (the encoder's output thread). Kept only while armed.
    void onMediumAccessUnit(const TierAccessUnit& au);
    // A thumbnail is due (cheap to ask every frame); then one live BGRA frame is taken (§6.1).
    bool wantsLiveFrame() const { return thumbWanted_.load(); }
    void onLiveFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t strideBytes);

    RollRecorderStatus status() const;

    // A star, an unstar or a delete changed the store outside the recorder: storage_full and the disk floor are
    // read again now (any thread, no lock held), and a change goes out in 0x0B at once, rather than at the next
    // clip or the hourly retention (v3 §5.1).
    void refreshStorageState();

    // Test hook: waits until nothing is being opened or finalized and no clip is open.
    bool waitIdle(int timeoutMs) const;
    // Test hook: the capture times (hns) of what the pre-roll ring holds, and of what the open clip holds
    // while its writer hasn't opened yet.
    struct HeldForTest {
        std::vector<int64_t> ring, clip;
    };
    HeldForTest heldForTest() const;

private:
    struct Job;
    struct Format {
        uint32_t width = 0, height = 0;
        int mode = 0, sbs = 0;
        uint32_t epoch = 0;
        bool operator==(const Format& o) const {
            return width == o.width && height == o.height && mode == o.mode && sbs == o.sbs && epoch == o.epoch;
        }
        bool operator!=(const Format& o) const { return !(*this == o); }
    };

    void threadMain();
    void applyEventsLocked();
    void setConditionsLocked();
    void updateArmedLocked();
    void abortLocked(std::shared_ptr<Job> job, const std::string& why);
    void openJob(const std::shared_ptr<Job>& job, std::unique_lock<std::mutex>& lk);
    void finishJob(const std::shared_ptr<Job>& job, bool retain);
    void checkFloor();
    void runRetention();
    // Applies the store's flags unless a newer reading was applied already. True when a condition changed.
    bool applyStoreFlagsLocked(const roll::RollStore::StorageFlags& f);
    bool motionUsableLocked() const;
    // The roll is on and its folder is still to be prepared, or being prepared.
    bool preparingLocked() const;
    // The trigger set's clock (a test may replace it), and the real clock for the recorder's own timers
    // (the floor check, retention, the wait for a thumbnail frame), which no test needs to move.
    uint64_t monoMs() const;
    static uint64_t realMs();

    Options opts_;
    SystemRollClock systemClock_;
    const RollClock* clock_ = &systemClock_;
    std::shared_ptr<roll::RollStore> store_;
    std::unique_ptr<TriggerSet> triggers_;
    RollPreRollRing ring_;

    mutable std::mutex m_;
    std::condition_variable cv_;
    mutable std::condition_variable idle_cv_;
    std::thread thread_;
    bool started_ = false;
    bool stopping_ = false;
    bool ready_ = false;
    RollConditions conditions_;
    Settings settings_;
    bool settingsKnown_ = false;
    bool limitsDue_ = false;     // the user's limits arrived or changed: apply them and run retention
    bool limitsApplied_ = false; // retention may run (hourly, after each clip): the user's limits are in force
    bool prepared_ = false;      // the folder is prepared and its start-up pass has run: recording may start
    bool prepareFailed_ = false; // it couldn't be: the roll stays off (folder_unavailable)
    // The folder was already a prepared roll when the host started (it holds .nosync). With the roll off it is
    // still kept tidy: the start-up pass (leftovers, indexes) runs once, and retention with the owner's limits.
    bool alreadyPrepared_ = false;
    bool tidied_ = false; // the start-up pass has run on this folder (by preparing it, or tidying it)
    bool refusedReal_ = false; // a test run saw a real frame; the roll is off for good
    uint64_t flagsGen_ = 0;    // the generation of the store's flags last applied
    RollBlock block_ = RollBlock::RollDisabled;
    bool armed_ = false;
    // When the roll last armed (a block cleared: privacy off, the sensor on...), on the capture clock (hns).
    // An AU captured before it is never kept, however late it arrives (§6.4: nothing from behind a block).
    int64_t armedSinceHns_ = 0;
    bool armedReported_ = false;
    bool keyframeDue_ = false;
    bool statusDirty_ = true;
    std::vector<std::shared_ptr<Job>> jobs_; // open, opening or finishing
    std::shared_ptr<Job> current_;           // the job AUs go to (the trigger set's open clip)
    int busy_ = 0;                           // jobs being opened or finalized outside the lock
    Format lastFormat_;
    bool haveFormat_ = false;
    uint64_t clipsFinalized_ = 0, clipsFailed_ = 0;
    std::string lastClipId_, lastError_;
    std::vector<std::string> finalizedIds_; // for onClipFinalized, called from the thread
    std::atomic<bool> thumbWanted_{false};

    // Motion (its own lock: the detector's work never holds up the media path).
    mutable std::mutex motionM_;
    DepthMotionDetector detector_;
    bool motionWasUsable_ = false;
    bool haveLastDepthSeq_ = false;
    uint64_t lastDepthSeq_ = 0;
    std::atomic<bool> motionResetDue_{false};
};

} // namespace kvhost
