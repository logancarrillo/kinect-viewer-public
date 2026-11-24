#include "roll_recorder.hpp"

#include "pet_settings.hpp" // recordScheduleSummary
#include "roll_thumbnail.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace kvhost {

namespace {

constexpr int64_t kHnsPerMs = 10000;

uint64_t steadyNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

int64_t unixMsNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

// The Unix time of a capture time on the host's monotonic clock (LiveFrame::capture_ns, in hns here).
int64_t captureToUnixMs(int64_t capture_hns) {
    const int64_t now_hns = static_cast<int64_t>(steadyNs() / 100);
    const int64_t ago_ms = now_hns > capture_hns ? (now_hns - capture_hns) / kHnsPerMs : 0;
    return unixMsNow() - ago_ms;
}

// The thumbnail goes next to the clip: written to a .tmp first (the start-up pass removes a leftover one),
// then moved into place, so a half-written .jpg is never listed.
bool writeThumbnailFile(const std::wstring& path, const std::vector<uint8_t>& jpeg, std::string* err) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "can't write the thumbnail (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD wrote = 0;
    const bool ok = WriteFile(h, jpeg.data(), static_cast<DWORD>(jpeg.size()), &wrote, nullptr) && wrote == jpeg.size() &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = "can't write the thumbnail (Win32 error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::shared_ptr<roll::RollStore> makeRollStore(const std::wstring& dir, const std::function<std::optional<uint64_t>(const std::wstring&)>& freeBytes) {
    roll::RollConfig cfg;
    cfg.dir = dir;
    cfg.freeBytes = freeBytes;
    // A leftover .mp4 (a crash) is read back through Media Foundation to rebuild its sidecar (§6.3).
    cfg.probe = [](const std::wstring& path) -> std::optional<roll::ProbedClip> {
        RollProbeResult r;
        if (!probeRollClip(path, r, nullptr)) return std::nullopt;
        roll::ProbedClip c;
        c.durationSec = r.durationSec();
        c.width = static_cast<int>(r.width);
        c.height = static_cast<int>(r.height);
        if (r.start_unix_ms > 0) c.startUnixMs = static_cast<uint64_t>(r.start_unix_ms);
        return c;
    };
    return std::make_shared<roll::RollStore>(std::move(cfg));
}

} // namespace

// One clip (one segment): from the trigger set's StartClip to its file, sidecar and thumbnail.
struct RollRecorder::Job {
    uint64_t seq = 0;
    ClipTrigger trigger = ClipTrigger::Manual;
    ClipStartCause cause = ClipStartCause::Trigger;
    std::optional<ClipStartedBy> startedBy;
    double preRollSec = 0.0;
    Format format;
    bool haveFormat = false;

    // Until the writer is open: the pre-roll, then every AU since (it opens at the first IDR).
    std::vector<RollAccessUnit> pending;
    size_t pendingBytes = 0;
    bool opening = false;  // the recorder thread is reserving and opening it, outside the lock
    bool openTried = false;
    std::optional<roll::ClipPaths> paths;
    int64_t startUnixMs = 0; // its first frame (the name and the sidecar's startUnixMs)
    std::unique_ptr<RollClipWriter> writer; // AUs go straight to it once open
    int liveAppends = 0;   // AUs appended to the open writer as they came (the failAppendAfter seam)
    bool failed = false;   // the writer couldn't take the clip
    bool discard = false;  // thrown away whatever was written (a test run saw a real frame)
    std::string failure;

    // The thumbnail (§6.1): wanted 1 s after the start; one live frame, then a JPEG.
    bool thumbWanted = false;
    std::vector<uint8_t> thumbFrame;
    uint32_t thumbW = 0, thumbH = 0;
    std::vector<uint8_t> jpeg;

    bool ending = false;   // the trigger set ended it: it takes no more AUs
    bool floorBlocked = false; // below the disk floor when it was to open: a block, not a failure
    uint64_t finishAfterMs = 0; // the recorder's clock; waits this long for its thumbnail frame
    bool finishing = false;

    bool hasIdr() const {
        for (const auto& a : pending)
            if (a.idr) return true;
        return false;
    }
};

RollRecorder::RollRecorder() = default;

RollRecorder::~RollRecorder() { stop(); }

uint64_t RollRecorder::monoMs() const { return clock_->monoMs(); }

uint64_t RollRecorder::realMs() { return steadyNs() / 1000000ULL; }

bool RollRecorder::start(const Options& opts, std::string* err) {
    std::lock_guard<std::mutex> lk(m_);
    if (started_) return true;
    opts_ = opts;
    clock_ = opts.clock ? opts.clock : &systemClock_;
    // v3 §10: the roll folder always comes from the caller; there is no default to fall back on here.
    if (opts.dir.empty()) {
        if (err) *err = "no camera-roll folder";
        return false;
    }
    // Read only: with the roll off nothing in the folder is created or changed. An existing roll is
    // opened for browsing; preparing it waits for the recorder's thread (threadMain).
    if (!roll::checkRollFolder(opts.dir, opts.recordingsDir, err)) return false;
    // A test run never opens a real roll, not even to read it: only a new or empty folder, or a test roll.
    if (opts.testRun && !roll::checkTestRollFolder(opts.dir, err)) return false;
    if (GetFileAttributesW(opts.dir.c_str()) != INVALID_FILE_ATTRIBUTES) store_ = makeRollStore(opts.dir, opts.freeBytes);
    prepared_ = false;
    prepareFailed_ = false;
    tidied_ = false;
    // A folder this host (or an earlier run) prepared carries the .nosync marker (protectPrivateFolder).
    alreadyPrepared_ = store_ && GetFileAttributesW((opts.dir + L"\\.nosync").c_str()) != INVALID_FILE_ATTRIBUTES;
    triggers_ = std::make_unique<TriggerSet>(*clock_);
    if (settingsKnown_) {
        triggers_->setMotionEnabled(settings_.motionEnabled);
        triggers_->setSchedule(settings_.schedule, settings_.scheduleEnabled);
        conditions_.rollEnabled = settings_.enabled;
        conditions_.preparing = !prepared_;
        limitsDue_ = settings_.limitsKnown;
    }
    setConditionsLocked();
    ready_ = true;
    started_ = true;
    stopping_ = false;
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void RollRecorder::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!started_ || stopping_) {
            if (!started_) return;
        } else {
            // §6.1 "the app exits": the open clip is finalized; nothing new starts.
            stopping_ = true;
            if (triggers_) {
                triggers_->shutdown();
                applyEventsLocked();
            }
            for (auto& j : jobs_) {
                j->thumbWanted = false; // no frame comes after this
                j->finishAfterMs = 0;
            }
            thumbWanted_ = false;
        }
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(m_);
    started_ = false;
    ready_ = false;
}

bool RollRecorder::running() const {
    std::lock_guard<std::mutex> lk(m_);
    return started_ && !stopping_;
}

std::shared_ptr<roll::RollStore> RollRecorder::store() const {
    std::lock_guard<std::mutex> lk(m_);
    return store_;
}

// ---- conditions ------------------------------------------------------------------------------------------

void RollRecorder::setConditionsLocked() {
    if (!triggers_ || stopping_) return;
    triggers_->setConditions(conditions_);
    applyEventsLocked();
}

void RollRecorder::setSettings(const Settings& s) {
    {
        std::lock_guard<std::mutex> lk(m_);
        const bool first = !settingsKnown_;
        const Settings old = settings_;
        settings_ = s;
        settingsKnown_ = true;
        // Retention runs only with the user's own limits (pet-camera.md §5 fail closed): while they aren't known
        // (pet-camera.json unread, or missing since start) it doesn't run at all, and defaults never delete.
        if (!s.limitsKnown) {
            limitsDue_ = false;
            limitsApplied_ = false;
        } else if (first || !old.limitsKnown || old.keepDays != s.keepDays || old.capBytes != s.capBytes) {
            limitsDue_ = true;
        }
        if (triggers_ && !stopping_) {
            if (first || old.motionEnabled != s.motionEnabled) triggers_->setMotionEnabled(s.motionEnabled);
            if (first || old.scheduleEnabled != s.scheduleEnabled || old.schedule != s.schedule) {
                triggers_->setSchedule(s.schedule, s.scheduleEnabled);
            }
            applyEventsLocked();
        }
        // Recording needs the prepared folder: until then the roll is on but preparing (roll_preparing), and a
        // folder that couldn't be prepared leaves it off (the host reports folder_unavailable).
        conditions_.rollEnabled = s.enabled && !prepareFailed_;
        conditions_.preparing = !prepared_;
        setConditionsLocked();
        statusDirty_ = true;
    }
    {
        std::lock_guard<std::mutex> mlk(motionM_);
        detector_.setSensitivity(s.motionSensitivity);
    }
    cv_.notify_all();
}

void RollRecorder::setPrivacyMode(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (conditions_.privacyMode == on) return;
        conditions_.privacyMode = on;
        setConditionsLocked();
    }
    cv_.notify_all();
}

void RollRecorder::setSensorOn(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (conditions_.sensorOn == on) return;
        conditions_.sensorOn = on;
        statusDirty_ = true; // motionAvailable
        setConditionsLocked();
    }
    cv_.notify_all();
}

void RollRecorder::setViewFrozen(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (conditions_.viewFrozen == on) return;
        conditions_.viewFrozen = on;
        setConditionsLocked();
    }
    cv_.notify_all();
}

void RollRecorder::setPcReplaying(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (conditions_.pcReplaying == on) return;
        conditions_.pcReplaying = on;
        statusDirty_ = true; // motionAvailable
        setConditionsLocked();
    }
    cv_.notify_all();
}

// ---- events -------------------------------------------------------------------------------------------------

void RollRecorder::updateArmedLocked() {
    // §6.1: `medium` runs whenever the roll is armed, so the pre-roll exists. Armed means "Record to the
    // camera roll" is on and nothing blocks recording: while privacy mode (or any block) holds, no colour is
    // kept even in memory, so a clip started right after can't carry pre-roll from before.
    const bool armed = ready_ && !stopping_ && block_ == RollBlock::None;
    if (armed != armed_) {
        armed_ = armed;
        statusDirty_ = true;
        // AUs are gated by when their frame was captured, not when they arrive: `medium` keeps encoding through
        // its hold after a block, and AUs still in the encoder land after the block clears.
        if (armed) armedSinceHns_ = static_cast<int64_t>(steadyNs() / 100);
    }
    if (!armed) ring_.clear();
}

void RollRecorder::applyEventsLocked() {
    if (!triggers_) return;
    for (RollEvent& e : triggers_->takeEvents()) {
        switch (e.kind) {
        case RollEventKind::StartClip: {
            auto job = std::make_shared<Job>();
            job->seq = e.clipSeq;
            job->trigger = e.trigger;
            job->cause = e.cause;
            job->startedBy = e.startedBy;
            if (e.preRollMs > 0) {
                // Manual and motion clips carry up to 5 s of pre-roll (§6.2), from the ring as it is now.
                job->pending = ring_.preRoll(static_cast<int64_t>(e.preRollMs) * kHnsPerMs);
                for (const auto& a : job->pending) job->pendingBytes += a.annexb.size();
                if (!job->pending.empty()) {
                    job->preRollSec =
                        static_cast<double>(job->pending.back().capture_hns - job->pending.front().capture_hns) / kRollHnsPerSecond;
                    job->format = lastFormat_;
                    job->haveFormat = haveFormat_;
                    // §6.2: a segment file, pre-roll included, is at most 10 minutes. The file starts at the
                    // pre-roll's first frame, so the trigger set's cap counts from there.
                    const int64_t now_hns = static_cast<int64_t>(steadyNs() / 100);
                    const int64_t since_first = now_hns - job->pending.front().capture_hns;
                    if (since_first > 0) {
                        triggers_->setClipPreRoll(e.clipSeq, static_cast<uint64_t>(since_first / kHnsPerMs));
                    }
                }
            }
            // No pre-roll (a segment, a view change, a block clearing, or an empty ring): the clip starts at
            // the next IDR, and `medium` is asked for one now rather than at its next GOP.
            if (job->pending.empty()) keyframeDue_ = true;
            jobs_.push_back(job);
            current_ = job;
            statusDirty_ = true;
            break;
        }
        case RollEventKind::ExtendClip: statusDirty_ = true; break;
        case RollEventKind::ThumbnailDue:
            for (auto& j : jobs_) {
                if (j->seq == e.clipSeq && block_ == RollBlock::None) {
                    j->thumbWanted = true;
                    thumbWanted_ = true;
                }
            }
            break;
        case RollEventKind::EndClip:
            for (auto& j : jobs_) {
                if (j->seq != e.clipSeq || j->ending) continue;
                j->ending = true;
                // A thumbnail asked for just now (a clip that ended before its first second) waits briefly for
                // the next live frame; otherwise the clip is finalized at once.
                j->finishAfterMs = realMs() + (j->thumbWanted ? static_cast<uint64_t>(std::max(0, opts_.thumbnailWaitMs)) : 0);
                if (current_ == j) current_.reset();
            }
            statusDirty_ = true;
            break;
        case RollEventKind::BlockedChanged:
            block_ = e.block;
            if (block_ != RollBlock::None) {
                // No live frame is taken behind a block (privacy, a frozen view, the sensor off...).
                for (auto& j : jobs_) j->thumbWanted = false;
                thumbWanted_ = false;
            }
            statusDirty_ = true;
            break;
        }
    }
    bool any = false;
    for (const auto& j : jobs_) any = any || j->thumbWanted;
    thumbWanted_ = any;
    updateArmedLocked();
    cv_.notify_all();
}

// The job is taken by value: callers pass current_, which abortClip's EndClip resets below, and the job must
// outlive that (a reference to the member would then be null).
void RollRecorder::abortLocked(std::shared_ptr<Job> job, const std::string& why) {
    job->failed = true;
    if (job->failure.empty()) job->failure = why;
    lastError_ = why;
    if (current_ == job && triggers_) {
        triggers_->abortClip(); // ends it (WriterFailed) and drops every trigger
        applyEventsLocked();
    }
    job->ending = true;
    // An aborted clip is never opened later (it failed), so what it held goes now.
    job->pending.clear();
    job->pendingBytes = 0;
    if (current_ == job) current_.reset();
    statusDirty_ = true;
}

// ---- triggers -----------------------------------------------------------------------------------------------

StartClipResult RollRecorder::startClip(const ClipStartedBy& who) {
    StartClipResult r;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!triggers_ || stopping_) {
            r.refused = RollBlock::RollDisabled;
            return r;
        }
        r = triggers_->startClip(who);
        applyEventsLocked();
        statusDirty_ = true;
    }
    cv_.notify_all();
    return r;
}

StopClipResult RollRecorder::stopClip() {
    StopClipResult r = StopClipResult::Ok;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!triggers_ || stopping_) return StopClipResult::Ok; // nothing records
        r = triggers_->stopClip();
        applyEventsLocked();
        statusDirty_ = true;
    }
    cv_.notify_all();
    return r;
}

bool RollRecorder::preparingLocked() const {
    return started_ && !stopping_ && settingsKnown_ && settings_.enabled && !prepared_ && !prepareFailed_;
}

bool RollRecorder::motionUsableLocked() const {
    return ready_ && !stopping_ && block_ == RollBlock::None && settings_.motionEnabled;
}

void RollRecorder::resetMotion() { motionResetDue_ = true; }

void RollRecorder::refuseRealColour(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (refusedReal_ || !started_) return;
        refusedReal_ = true;
        // Real colour never reaches a test folder. Every clip in hand is thrown away (none is
        // finalized), the ring is emptied, and the roll stays off: prepareFailed_ keeps it from coming back.
        for (auto& j : jobs_) {
            j->discard = true;
            j->thumbWanted = false;
            j->thumbFrame.clear();
        }
        if (current_) abortLocked(current_, why);
        prepareFailed_ = true;
        lastError_ = why;
        conditions_.rollEnabled = false;
        setConditionsLocked();
        ring_.clear();
        thumbWanted_ = false;
        statusDirty_ = true;
    }
    cv_.notify_all();
}

void RollRecorder::submitDepth(const uint16_t* raw16, uint32_t width, uint32_t height, uint64_t seq, uint64_t timestampNs,
                               bool liveSensor) {
    bool usable = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        usable = liveSensor && motionUsableLocked();
    }
    bool motion = false;
    {
        std::lock_guard<std::mutex> mlk(motionM_);
        if (!usable) {
            // Not from the live sensor (a replay), blocked or off: the frame is ignored, and so is its number,
            // so the next frame that counts can't complete a run with frames from before (§6.2).
            motionWasUsable_ = false;
            return;
        }
        if (haveLastDepthSeq_ && seq == lastDepthSeq_) return; // the same frame polled again
        if (!motionWasUsable_ || motionResetDue_.exchange(false)) {
            // Usable again (the sensor started, the view unfroze, a block cleared, motion turned on) or the
            // cut-off changed: the background is learned afresh (§6.2 "Starting the sensor, changing the
            // cut-off and unfreezing all reset it").
            detector_.reset();
        }
        motionWasUsable_ = true;
        haveLastDepthSeq_ = true;
        lastDepthSeq_ = seq;
        motion = detector_.process(raw16, width, height, seq, timestampNs).motion;
    }
    if (!motion) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!triggers_ || !motionUsableLocked()) return;
        triggers_->onMotion();
        applyEventsLocked();
    }
    cv_.notify_all();
}

// ---- media ---------------------------------------------------------------------------------------------------

void RollRecorder::onMediumAccessUnit(const TierAccessUnit& tau) {
    if (tau.tier != LiveTier::Medium || tau.avcc.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (!armed_ || stopping_) return; // nothing is kept while blocked or off
    // Nor anything captured before the roll last armed (during privacy mode, or before it): such an AU never
    // reaches the pre-roll ring or a clip, whenever it arrives.
    if (static_cast<int64_t>(tau.capture_ns / 100) < armedSinceHns_) return;
    const Format f{tau.width, tau.height, tau.mode, tau.sbs, tau.epoch};
    if (haveFormat_ && f.epoch != lastFormat_.epoch) {
        // A new epoch may come with new parameter sets: the ring never mixes two (one clip, one format).
        ring_.clear();
    }
    lastFormat_ = f;
    haveFormat_ = true;
    RollAccessUnit au = rollAccessUnitFromAvcc(tau.avcc, tau.sps, tau.pps, tau.idr, static_cast<int64_t>(tau.capture_ns / 100),
                                               tau.width, tau.height);
    if (current_) {
        Job& j = *current_;
        if (!j.haveFormat) {
            j.format = f;
            j.haveFormat = true;
        } else if (j.format != f) {
            // §6.1: a clip has one size and one view. A change of mode, size or encoder epoch finalizes it, and
            // a trigger still active starts the next clip at once, without pre-roll.
            triggers_->onViewChanged();
            applyEventsLocked();
            if (current_) {
                current_->format = f;
                current_->haveFormat = true;
            }
        }
    }
    ring_.push(au);
    if (!current_) return;
    Job& j = *current_;
    if (j.writer) {
        std::string err;
        bool ok = false;
        if (opts_.failAppendAfter > 0 && ++j.liveAppends > opts_.failAppendAfter) {
            err = "the writer refused a frame (test seam)";
        } else {
            ok = j.writer->append(std::move(au), &err);
        }
        if (!ok) abortLocked(current_, "the clip couldn't be written: " + err);
        return;
    }
    j.pendingBytes += au.annexb.size();
    const bool idr = au.idr;
    j.pending.push_back(std::move(au));
    // AUs held for a clip whose writer hasn't opened yet (waiting for its first IDR, or the reservation in
    // progress). Past the limit the clip can't be written as it should, and it is aborted (fail closed).
    if (j.pendingBytes > opts_.maxPendingBytes) {
        abortLocked(current_, "the clip's writer didn't open in time");
        return;
    }
    if (idr && !j.opening && !j.openTried) cv_.notify_all();
}

void RollRecorder::onLiveFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t strideBytes) {
    if (!thumbWanted_.load() || !bgra || width == 0 || height == 0 || strideBytes < width * 4) return;
    // Copied outside the lock (a large colour frame); attached only if a clip still wants it and nothing
    // blocks, so no frame behind privacy mode is kept.
    std::vector<uint8_t> copy(static_cast<size_t>(width) * 4 * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(copy.data() + static_cast<size_t>(y) * width * 4, bgra + static_cast<size_t>(y) * strideBytes,
                    static_cast<size_t>(width) * 4);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        if (block_ != RollBlock::None) return;
        for (auto& j : jobs_) {
            if (!j->thumbWanted) continue;
            j->thumbWanted = false;
            j->thumbFrame = copy; // one frame each; normally only one clip is waiting
            j->thumbW = width;
            j->thumbH = height;
        }
        bool any = false;
        for (const auto& j : jobs_) any = any || j->thumbWanted;
        thumbWanted_ = any;
    }
    cv_.notify_all();
}

// ---- the recorder's thread -----------------------------------------------------------------------------------

void RollRecorder::openJob(const std::shared_ptr<Job>& job, std::unique_lock<std::mutex>& lk) {
    job->opening = true;
    job->openTried = true;
    ++busy_;
    // The clip starts at its first frame, pre-roll included (§6.3 names): the first IDR's capture time.
    int64_t first_capture = 0;
    for (const auto& a : job->pending) {
        if (a.idr) {
            first_capture = a.capture_hns;
            break;
        }
    }
    const Format f = job->format;
    const ClipTrigger trigger = job->trigger;
    const std::vector<uint8_t> seq_header = ring_.sequenceHeader();
    lk.unlock();

    // §6.4: the disk floor is checked before each clip.
    const bool floor_ok = store_->checkDiskFloor();
    const roll::RollStore::StorageFlags floor_flags = store_->flags();
    std::string err;
    std::optional<roll::ClipPaths> paths;
    auto writer = std::make_unique<RollClipWriter>();
    const int64_t start_ms = first_capture > 0 ? captureToUnixMs(first_capture) : unixMsNow();
    bool opened = false;
    if (floor_ok) {
        paths = store_->reserveClip(start_ms, trigger, &err);
        if (paths) {
            RollWriterOptions wo;
            wo.path = paths->mp4;
            wo.reserved_empty_file = true;
            wo.width = f.width;
            wo.height = f.height;
            wo.start_unix_ms = start_ms;
            wo.sequence_header = seq_header;
            wo.max_queued_bytes = opts_.writerMaxQueuedBytes;
            wo.test_fail_write_after = opts_.writerFailWriteAfter;
            wo.test_fail_after_fragments = opts_.writerFailAfterFragments;
            opened = writer->open(wo, &err);
        }
    }

    lk.lock();
    --busy_;
    job->opening = false;
    job->paths = paths;
    if (job->discard) {
        // A test run saw a real frame while it opened: its pending frames are dropped, and finishJob abandons the file.
        if (opened) job->writer = std::move(writer); // closed by finishJob before the file goes
        job->pending.clear();
        job->pendingBytes = 0;
        job->ending = true;
        return;
    }
    job->startUnixMs = start_ms;
    if (!floor_ok) {
        // Below the floor: the block ends this clip (and every trigger) as any block does; its empty
        // reservation was never made.
        if (!applyStoreFlagsLocked(floor_flags) && !conditions_.diskFloor) {
            conditions_.diskFloor = true; // this check is the one that found it below the floor
            setConditionsLocked();
        }
        job->ending = true;
        job->floorBlocked = true;
        job->pending.clear();
        job->pendingBytes = 0;
        if (current_ == job) current_.reset();
        return;
    }
    if (!opened) {
        abortLocked(job, err.empty() ? std::string("the clip couldn't be opened") : err);
        return;
    }
    job->writer = std::move(writer);
    for (auto& a : job->pending) {
        if (!job->writer->append(std::move(a), &err)) {
            job->pending.clear();
            abortLocked(job, "the clip couldn't be written: " + err);
            return;
        }
    }
    job->pending.clear();
    job->pendingBytes = 0;
}

void RollRecorder::finishJob(const std::shared_ptr<Job>& job, bool retain) {
    // Outside the lock: the job is out of jobs_' reach for every other path (finishing).
    RollClipResult result;
    if (job->writer) result = job->writer->finalize(); // waits for the queue and Finalize
    job->writer.reset();
    std::string err;
    bool kept = false;
    // The writer failed after fragments reached the disk (a WriteSample or Finalize error mid-clip). What reads
    // back is kept as a recovered clip, as the start-up pass would keep it after a crash, with a sidecar and the
    // reason in lastError, rather than thrown away. A failure before the first fragment leaves nothing readable,
    // and the reservation is abandoned as before.
    int64_t duration_hns = result.duration_hns;
    bool recovered = false;
    if (!job->discard && job->paths && !result.ok && result.frames > 0) {
        RollProbeResult pr;
        std::string perr;
        if (probeRollClip(job->paths->mp4, pr, &perr) && pr.frames > 0 && pr.duration_hns > 0) {
            recovered = true;
            duration_hns = pr.duration_hns;
            job->failure = "the clip was cut short (" + (result.error.empty() ? std::string("the writer failed") : result.error) +
                           "); it was kept up to its last whole fragment";
        }
    }
    if (job->discard) {
        // Nothing of it is kept, the .mp4 included.
        if (job->paths) store_->abandonClip(*job->paths);
        job->paths.reset();
        job->failed = true;
    } else if (job->paths && (result.ok || recovered) && !job->paths->mp4.empty()) {
        // Thumbnail first, then the sidecar last, so a clip with a sidecar is complete (§6.3).
        std::string thumb_err;
        if (!job->jpeg.empty() && !writeThumbnailFile(job->paths->thumbnail, job->jpeg, &thumb_err)) {
            job->failure = thumb_err; // the clip is kept without one; the PC's status line says why
        }
        roll::ClipSidecar sc;
        sc.startUnixMs = static_cast<uint64_t>(job->startUnixMs);
        sc.durationSec = static_cast<double>(duration_hns) / kRollHnsPerSecond;
        sc.endUnixMs = sc.startUnixMs + static_cast<uint64_t>(duration_hns / kHnsPerMs);
        sc.trigger = job->trigger;
        if (job->trigger == ClipTrigger::Manual && job->startedBy) {
            roll::StartedBy sb;
            if (!job->startedBy->deviceId.empty()) sb.deviceId = job->startedBy->deviceId;
            sb.deviceName = job->startedBy->deviceName;
            sc.startedBy = sb;
        }
        sc.width = static_cast<int>(job->format.width);
        sc.height = static_cast<int>(job->format.height);
        sc.modeRaw = job->format.mode;
        sc.sbsLayoutRaw = job->format.sbs;
        sc.preRollSec = job->preRollSec;
        kept = store_->finalizeClip(*job->paths, sc, &err);
        if (!kept) store_->abandonClip(*job->paths);
    } else if (job->paths) {
        // Nothing usable was written (no IDR reached it, or the writer failed before its first fragment).
        store_->abandonClip(*job->paths);
        if (err.empty()) err = result.error;
    } else if (!job->failed && !job->floorBlocked && !job->pending.empty()) {
        // Frames came for it but never an IDR to open it on: it can't be written. Counted as failed, with the
        // reason on the PC's status line, rather than dropped without a trace.
        job->failed = true;
        job->failure = "the clip ended before its first keyframe";
    }
    if (kept && retain) {
        // §6.4: retention runs after each clip, once the user's limits are in force.
        store_->retentionPass();
    }
    store_->checkDiskFloor();
    const roll::RollStore::StorageFlags flags = store_->flags();
    std::string clip_id = job->paths ? job->paths->clipId : std::string();

    std::lock_guard<std::mutex> lk(m_);
    --busy_;
    if (kept) {
        ++clipsFinalized_;
        lastClipId_ = clip_id;
        if (!job->failure.empty()) lastError_ = job->failure; // cut short, or no thumbnail; the clip is kept
    } else if (job->paths || job->failed) {
        ++clipsFailed_;
        lastError_ = !job->failure.empty() ? job->failure : (err.empty() ? std::string("the clip couldn't be finalized") : err);
    }
    applyStoreFlagsLocked(flags);
    statusDirty_ = true;
    if (kept && onClipFinalized) {
        // Called after the lock is released (threadMain picks it up).
        finalizedIds_.push_back(clip_id);
    }
    idle_cv_.notify_all();
}

void RollRecorder::refreshStorageState() {
    std::shared_ptr<roll::RollStore> store;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!started_ || stopping_ || !prepared_ || !store_) return;
        store = store_;
    }
    store->checkDiskFloor();
    const roll::RollStore::StorageFlags flags = store->flags();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!applyStoreFlagsLocked(flags)) return;
    }
    cv_.notify_all();
}

void RollRecorder::checkFloor() {
    store_->checkDiskFloor();
    const roll::RollStore::StorageFlags flags = store_->flags();
    std::lock_guard<std::mutex> lk(m_);
    applyStoreFlagsLocked(flags);
}

void RollRecorder::runRetention() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!limitsApplied_) return; // the user's limits aren't known: nothing is deleted
    }
    store_->retentionPass();
    const roll::RollStore::StorageFlags flags = store_->flags();
    std::lock_guard<std::mutex> lk(m_);
    applyStoreFlagsLocked(flags);
}

bool RollRecorder::applyStoreFlagsLocked(const roll::RollStore::StorageFlags& f) {
    // storage_full and the disk floor are read from the store outside this lock, on several threads (a finished
    // clip, a star or delete, the floor check, retention). A reading older than one already applied is dropped, so a
    // stale storage_full can never overwrite a newer one.
    if (f.gen < flagsGen_) return false;
    flagsGen_ = f.gen;
    if (conditions_.storageFull == f.storageFull && conditions_.diskFloor == f.floorBlocked) return false;
    conditions_.storageFull = f.storageFull;
    conditions_.diskFloor = f.floorBlocked;
    setConditionsLocked();
    statusDirty_ = true;
    return true;
}

void RollRecorder::threadMain() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the thumbnailer (WIC)
    uint64_t last_floor = realMs(), last_retention = realMs(), last_progress = 0, last_recount = 0;
    bool last_recording = false;
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        if (opts_.threadHook) {
            lk.unlock();
            opts_.threadHook();
            lk.lock();
        }
        if (!stopping_) {
            triggers_->tick();
            applyEventsLocked();
        }
        // A clip waiting for its first IDR (a segment cut, a view change, a block clearing) asks `medium` for
        // one first, before any slow work below (finalizing the previous segment, retention), so the next
        // segment starts about one frame after the cut (pet-camera.md §6.2).
        if (keyframeDue_) {
            keyframeDue_ = false;
            if (onKeyframeWanted) {
                lk.unlock();
                onKeyframeWanted();
                lk.lock();
            }
        }
        // Settings for retention, which the store applies.
        // The folder is prepared the first time recording may start (the roll on, pet-camera.json read,
        // privacy mode off), never before: until then nothing in it is created or changed. Fail closed: a folder
        // that can't be prepared leaves the roll off, and the host says why (folder_unavailable).
        if (!prepared_ && !prepareFailed_ && !stopping_ && settingsKnown_ && settings_.enabled && settings_.settingsRead &&
            !conditions_.privacyMode) {
            const std::wstring dir = opts_.dir, recordings = opts_.recordingsDir;
            std::shared_ptr<roll::RollStore> store = store_;
            lk.unlock();
            std::string err;
            bool ok = roll::prepareRollFolder(dir, recordings, &err);
            // A test run's roll is marked as one, so a later test may use it and nothing takes it for a real roll.
            if (ok && opts_.testRun) ok = roll::writeTestRollMarker(dir, &err);
            roll::RollStore::StorageFlags flags;
            if (ok) {
                if (!store) store = makeRollStore(dir, opts_.freeBytes);
                store->startupPass(); // before any clip is recorded; its probe uses this thread's COM
                store->checkDiskFloor();
                flags = store->flags();
            }
            lk.lock();
            if (ok) {
                store_ = store;
                prepared_ = true;
                tidied_ = true;
                // Read just now, under the store's own lock: it is the newest reading (a new store counts from 0).
                flagsGen_ = flags.gen;
                conditions_.diskFloor = flags.floorBlocked;
                conditions_.storageFull = flags.storageFull;
                conditions_.rollEnabled = settings_.enabled;
                conditions_.preparing = false;
                if (settings_.limitsKnown) limitsDue_ = true;
                setConditionsLocked();
            } else {
                prepareFailed_ = true;
                conditions_.rollEnabled = false; // off, with the reason (folder_unavailable)
                setConditionsLocked();
                lastError_ = err.empty() ? std::string("the camera-roll folder couldn't be prepared") : err;
            }
            statusDirty_ = true;
            const std::string why = lastError_;
            lk.unlock();
            if (ok) {
                if (onPrepared) onPrepared();
            } else if (onUnavailable) {
                onUnavailable(why);
            }
            lk.lock();
            continue;
        }
        // The roll is off, but the folder is an already-prepared roll: it is kept tidy (only creating or
        // preparing a folder is ruled out here). Its start-up pass runs once the settings are read, and
        // retention follows below once the owner's limits are known. It stays browsable and playable.
        if (!prepared_ && !tidied_ && alreadyPrepared_ && store_ && !stopping_ && settingsKnown_ && settings_.settingsRead) {
            const std::shared_ptr<roll::RollStore> store = store_;
            lk.unlock();
            store->startupPass(); // its probe uses this thread's COM
            lk.lock();
            tidied_ = true;
            if (settings_.limitsKnown) limitsDue_ = true;
            statusDirty_ = true;
            continue;
        }
        if (limitsDue_ && (prepared_ || tidied_)) {
            limitsDue_ = false;
            const int days = settings_.keepDays;
            const uint64_t cap = settings_.capBytes;
            lk.unlock();
            // The first retention after start runs here, once the real limits are applied.
            store_->setLimits(cap, days);
            store_->retentionPass();
            const roll::RollStore::StorageFlags flags = store_->flags();
            lk.lock();
            limitsApplied_ = settings_.limitsKnown;
            applyStoreFlagsLocked(flags);
            statusDirty_ = true;
            continue;
        }
        // Open a clip that has its first IDR. One that already ended (the thread was busy finalizing another
        // when it started and stopped) is opened too, so its pre-roll and frames are written and it is
        // finalized (pet-camera.md §6.1), at exit too. One that failed is not.
        std::shared_ptr<Job> to_open;
        for (auto& j : jobs_) {
            if (!j->failed && !j->opening && !j->openTried && !j->writer && j->hasIdr()) {
                to_open = j;
                break;
            }
        }
        if (to_open) {
            openJob(to_open, lk);
            continue;
        }
        // A thumbnail frame to encode.
        std::shared_ptr<Job> to_thumb;
        for (auto& j : jobs_) {
            if (!j->thumbFrame.empty() && j->jpeg.empty()) {
                to_thumb = j;
                break;
            }
        }
        if (to_thumb) {
            std::vector<uint8_t> frame;
            frame.swap(to_thumb->thumbFrame);
            const uint32_t w = to_thumb->thumbW, h = to_thumb->thumbH;
            lk.unlock();
            RollThumbnail thumb;
            std::string err;
            const bool ok = makeRollThumbnail(frame.data(), w, h, w * 4, thumb, &err);
            lk.lock();
            if (ok) to_thumb->jpeg = std::move(thumb.jpeg);
            else lastError_ = "thumbnail: " + err;
            continue;
        }
        // A clip that has ended and is ready to be finalized.
        std::shared_ptr<Job> to_finish;
        const uint64_t now = realMs();
        for (auto& j : jobs_) {
            if (j->ending && !j->opening && !j->finishing &&
                (stopping_ || now >= j->finishAfterMs || !j->thumbWanted)) {
                to_finish = j;
                break;
            }
        }
        if (to_finish) {
            to_finish->finishing = true;
            to_finish->thumbWanted = false;
            jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), to_finish), jobs_.end());
            ++busy_;
            const bool retain = limitsApplied_;
            lk.unlock();
            finishJob(to_finish, retain);
            lk.lock();
            continue;
        }
        if (armed_ != armedReported_) {
            armedReported_ = armed_;
            const bool a = armed_;
            if (onArmedChanged) {
                lk.unlock();
                onArmedChanged(a);
                lk.lock();
            }
            continue;
        }
        std::vector<std::string> finalized;
        finalized.swap(finalizedIds_);
        if (!finalized.empty() && onClipFinalized) {
            lk.unlock();
            for (const auto& id : finalized) onClipFinalized(id);
            lk.lock();
        }
        // Status: on any change, and once a second while a clip records (clipElapsedSec, §5.1).
        const bool recording = triggers_ && triggers_->recording();
        if (recording && now - last_progress >= 1000) {
            last_progress = now;
            statusDirty_ = true;
        }
        if (recording != last_recording) {
            last_recording = recording;
            statusDirty_ = true;
        }
        if (statusDirty_) {
            statusDirty_ = false;
            if (onStatusChanged) {
                lk.unlock();
                onStatusChanged();
                lk.lock();
            }
            continue;
        }
        if (stopping_ && jobs_.empty() && busy_ == 0) break;
        idle_cv_.notify_all();
        // §6.4: the floor every 10 s (also while idle, so recording resumes on its own above 21 GB), and
        // retention every hour.
        if (!stopping_ && prepared_ && now - last_floor >= static_cast<uint64_t>(std::max(100, opts_.floorCheckMs))) {
            last_floor = now;
            lk.unlock();
            checkFloor();
            lk.lock();
            continue;
        }
        if (!stopping_ && now - last_retention >= static_cast<uint64_t>(std::max(1000, opts_.retentionMs))) {
            last_retention = now;
            lk.unlock();
            runRetention();
            lk.lock();
            continue;
        }
        // The store's cached counts are walked again in full now and then, here and never on the PC's UI
        // thread, so a file changed behind the store's back is counted within a minute.
        if (!stopping_ && store_ && (last_recount == 0 || now - last_recount >= static_cast<uint64_t>(std::max(1000, opts_.recountMs)))) {
            last_recount = now;
            const std::shared_ptr<roll::RollStore> store = store_;
            lk.unlock();
            store->recount();
            lk.lock();
            continue;
        }
        cv_.wait_for(lk, std::chrono::milliseconds(std::max(10, opts_.tickMs)));
    }
    lk.unlock();
    if (armedReported_ && onArmedChanged) onArmedChanged(false);
    armedReported_ = false;
    if (onStatusChanged) onStatusChanged();
    if (SUCCEEDED(co)) CoUninitialize();
    idle_cv_.notify_all();
}

bool RollRecorder::waitIdle(int timeoutMs) const {
    std::unique_lock<std::mutex> lk(m_);
    return idle_cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                             [&] { return jobs_.empty() && busy_ == 0 && !(triggers_ && triggers_->recording()); });
}

RollRecorder::HeldForTest RollRecorder::heldForTest() const {
    std::lock_guard<std::mutex> lk(m_);
    HeldForTest h;
    h.ring = ring_.captureTimes();
    if (current_) {
        for (const auto& a : current_->pending) h.clip.push_back(a.capture_hns);
    }
    return h;
}

RollRecorderStatus RollRecorder::status() const {
    std::lock_guard<std::mutex> lk(m_);
    RollRecorderStatus s;
    s.ready = ready_ && !stopping_;
    s.enabled = settings_.enabled;
    s.block = block_;
    s.armed = armed_;
    s.motionEnabled = settings_.motionEnabled;
    s.motionSensitivity = settings_.motionSensitivity;
    s.motionAvailable = conditions_.sensorOn && !conditions_.pcReplaying;
    s.scheduleEnabled = settings_.scheduleEnabled;
    s.scheduleSummary = recordScheduleSummary(settings_.schedule);
    s.diskFloor = conditions_.diskFloor;
    s.storageFull = conditions_.storageFull;
    if (triggers_) {
        s.recording = triggers_->recording();
        s.clipTrigger = triggers_->clipTrigger();
        s.manualActive = triggers_->manualActive();
        // Whole seconds: the 0x0B that carries it changes at most once a second (v3 §5.1).
        s.clipElapsedSec = static_cast<double>(static_cast<uint64_t>(triggers_->clipElapsedSec()));
    }
    s.clipsFinalized = clipsFinalized_;
    s.clipsFailed = clipsFailed_;
    s.limitsApplied = limitsApplied_;
    s.limitsKnown = settings_.limitsKnown;
    s.prepared = prepared_;
    s.preparing = preparingLocked();
    s.lastClipId = lastClipId_;
    s.lastError = lastError_;
    return s;
}

} // namespace kvhost
