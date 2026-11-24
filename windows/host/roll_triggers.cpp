#include "roll_triggers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace kvhost {

// ---- DepthMotionDetector ---------------------------------------------------------------------------

namespace {

// pet-camera.md §6.2: a cell counts as changed when |d - ref| > max(40 mm, 3 % of ref).
float changeThreshold(float ref) {
    return std::max(static_cast<float>(DepthMotionDetector::kMinChangeMm),
                    static_cast<float>(DepthMotionDetector::kChangeFraction) * ref);
}

} // namespace

DepthMotionDetector::DepthMotionDetector() : cells_(kCellCount) {}

void DepthMotionDetector::setSensitivity(int sensitivity) {
    sensitivity_ = std::clamp(sensitivity, kMinSensitivity, kMaxSensitivity);
}

void DepthMotionDetector::reset() {
    std::fill(cells_.begin(), cells_.end(), Cell{});
    consecutive_ = 0;
    globalConsecutive_ = 0;
    haveLastTs_ = false;
    lastTsNs_ = 0;
    haveLastSeq_ = false;
    lastSeq_ = 0;
}

int DepthMotionDetector::thresholdCells(int validCells, int sensitivity) {
    if (validCells <= 0) return 0;
    const int s = std::clamp(sensitivity, kMinSensitivity, kMaxSensitivity);
    // 2 % at 1 down to 0.2 % at 10, linearly: 20 per mille less 2 per step, rounded up so a small grid
    // still needs a whole cell.
    const int64_t perMille = 20 - 2 * (s - 1);
    const int64_t n = (static_cast<int64_t>(validCells) * perMille + 999) / 1000;
    return static_cast<int>(std::max<int64_t>(1, n));
}

MotionResult DepthMotionDetector::process(const uint16_t* depth, uint32_t width, uint32_t height, uint64_t seq,
                                          uint64_t timestampNs) {
    MotionResult r;
    if (!depth || width != kDepthWidth || height != kDepthHeight) return r;
    r.frameUsed = true;

    // Consecutive means the next frame: not one after a dropped frame (W7: the
    // sequence number must be the last plus 1), a repeat, a stall, or from before a clock step.
    const bool nextSeq = haveLastSeq_ && seq == lastSeq_ + 1;
    const bool nextTime = haveLastTs_ && timestampNs > lastTsNs_ && timestampNs - lastTsNs_ <= kMaxFrameGapNs;
    const bool follows = nextSeq && nextTime;
    haveLastSeq_ = true;
    lastSeq_ = seq;
    haveLastTs_ = true;
    lastTsNs_ = timestampNs;

    uint16_t px[kCellSize * kCellSize];
    for (int cy = 0; cy < kGridHeight; ++cy) {
        for (int cx = 0; cx < kGridWidth; ++cx) {
            int n = 0;
            for (int y = 0; y < kCellSize; ++y) {
                const uint16_t* row = depth + static_cast<size_t>(cy * kCellSize + y) * kDepthWidth + cx * kCellSize;
                for (int x = 0; x < kCellSize; ++x) {
                    if (row[x] != 0) px[n++] = row[x];
                }
            }
            Cell& c = cells_[static_cast<size_t>(cy) * kGridWidth + cx];
            c.currentValid = false;
            if (n < kMinValidPixels) continue; // invalid this frame: neither compared nor learned
            std::nth_element(px, px + n / 2, px + n);
            const float d = static_cast<float>(px[n / 2]);
            c.current = d;
            c.currentValid = true;

            if (!c.hasBg) {
                c.bg = d;
                c.hasBg = true;
                c.hasSettled = false;
                continue;
            }
            ++r.validCells;
            if (std::fabs(d - c.bg) <= changeThreshold(c.bg)) {
                c.bg += static_cast<float>(kAlpha) * (d - c.bg);
                c.hasSettled = false;
                continue;
            }
            // Changed: the background stays as it was. Track whether the change is still (§6.2 re-learning).
            if (c.hasSettled && timestampNs >= c.settledSinceNs && std::fabs(d - c.settled) <= changeThreshold(c.settled)) {
                if (timestampNs - c.settledSinceNs >= kRelearnAfterNs) {
                    c.bg = d;
                    c.hasSettled = false;
                    ++r.relearnedCells;
                    continue;
                }
            } else {
                c.settled = d;
                c.hasSettled = true;
                c.settledSinceNs = timestampNs;
            }
            ++r.changedCells;
        }
    }

    r.threshold = thresholdCells(r.validCells, sensitivity_);

    // W7: over 60 % of the cells at once, held for a run, is the camera or the
    // scene, not a pet. Re-learn everything instead of reporting motion.
    const bool wholeScene = r.validCells > 0 && static_cast<int64_t>(r.changedCells) * 100 >
                                                    static_cast<int64_t>(r.validCells) * kGlobalRelearnPercent;
    globalConsecutive_ = wholeScene ? (follows ? globalConsecutive_ + 1 : 1) : 0;
    r.globalConsecutive = globalConsecutive_;
    if (globalConsecutive_ >= kConsecutiveFrames) {
        for (auto& c : cells_) {
            if (!c.currentValid) continue;
            c.bg = c.current;
            c.hasBg = true;
            c.hasSettled = false;
        }
        globalConsecutive_ = 0;
        consecutive_ = 0;
        r.globalRelearn = true;
        return r; // motion false, consecutive 0
    }

    const bool hit = r.threshold > 0 && r.changedCells >= r.threshold;
    consecutive_ = hit ? (follows ? consecutive_ + 1 : 1) : 0;
    r.consecutive = consecutive_;
    r.motion = !wholeScene && consecutive_ >= kConsecutiveFrames;
    return r;
}

// ---- Names -----------------------------------------------------------------------------------------

const char* clipTriggerName(ClipTrigger t) {
    switch (t) {
    case ClipTrigger::Manual: return "manual";
    case ClipTrigger::Motion: return "motion";
    case ClipTrigger::Schedule: return "schedule";
    case ClipTrigger::Unknown: return "unknown";
    }
    return "unknown";
}

const char* rollBlockCode(RollBlock b) {
    switch (b) {
    case RollBlock::None: return "";
    case RollBlock::PrivacyMode: return "privacy_mode";
    case RollBlock::RollDisabled: return "roll_disabled";
    case RollBlock::RollPreparing: return "roll_preparing";
    case RollBlock::SensorOff: return "sensor_off";
    case RollBlock::ViewFrozen: return "view_frozen";
    case RollBlock::PcReplaying: return "pc_replaying";
    case RollBlock::DiskFloor: return "disk_floor";
    case RollBlock::StorageFull: return "storage_full";
    }
    return "";
}

const char* clipEndReasonName(ClipEndReason r) {
    switch (r) {
    case ClipEndReason::TriggersEnded: return "triggers_ended";
    case ClipEndReason::SegmentCap: return "segment_cap";
    case ClipEndReason::ViewChanged: return "view_changed";
    case ClipEndReason::Blocked: return "blocked";
    case ClipEndReason::Shutdown: return "shutdown";
    case ClipEndReason::WriterFailed: return "writer_failed";
    }
    return "";
}

RollBlock rollBlockFor(const RollConditions& c) {
    if (c.privacyMode) return RollBlock::PrivacyMode;
    if (!c.rollEnabled) return RollBlock::RollDisabled;
    if (c.preparing) return RollBlock::RollPreparing;
    if (!c.sensorOn) return RollBlock::SensorOff;
    if (c.viewFrozen) return RollBlock::ViewFrozen;
    if (c.pcReplaying) return RollBlock::PcReplaying;
    if (c.diskFloor) return RollBlock::DiskFloor;
    if (c.storageFull) return RollBlock::StorageFull;
    return RollBlock::None;
}

// ---- Schedule --------------------------------------------------------------------------------------

bool isScheduledRecording(const std::vector<RecordScheduleRow>& rows, const RollLocalTime& t) {
    if (t.weekday < 0 || t.weekday > 6 || t.secondOfDay < 0 || t.secondOfDay >= 86400) return false;
    const int today = 1 << t.weekday;
    const int yesterday = 1 << ((t.weekday + 6) % 7);
    for (const auto& r : rows) {
        if ((r.days & 0x7F) == 0 || r.startMinute >= 24 * 60 || r.lengthMinutes < 1 || r.lengthMinutes > 60) continue;
        const int start = r.startMinute * 60;
        const int end = start + r.lengthMinutes * 60; // may pass midnight
        // The part on the start day belongs to the days in the mask; the part after midnight to the day
        // after each of them (as SharingScheduleRow does).
        if ((r.days & today) && t.secondOfDay >= start && t.secondOfDay < std::min(end, 86400)) return true;
        if (end > 86400 && (r.days & yesterday) && t.secondOfDay < end - 86400) return true;
    }
    return false;
}

uint64_t SystemRollClock::monoMs() const { return GetTickCount64(); }

RollLocalTime SystemRollClock::localTime() const {
    SYSTEMTIME st;
    GetLocalTime(&st);
    RollLocalTime t;
    t.weekday = st.wDayOfWeek;
    t.secondOfDay = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    return t;
}

// ---- TriggerSet ------------------------------------------------------------------------------------

TriggerSet::TriggerSet(const RollClock& clock) : clock_(clock) {}

std::vector<RollEvent> TriggerSet::takeEvents() {
    std::vector<RollEvent> out;
    out.swap(events_);
    return out;
}

std::optional<ClipTrigger> TriggerSet::clipTrigger() const {
    if (!clipOpen_) return std::nullopt;
    return clipTrigger_;
}

double TriggerSet::clipElapsedSec() const {
    if (!clipOpen_) return 0.0;
    const uint64_t now = clock_.monoMs();
    return now > clipStartMs_ ? static_cast<double>(now - clipStartMs_) / 1000.0 : 0.0;
}

bool TriggerSet::motionCooldown() const {
    return cooldownUntilMs_.has_value() && clock_.monoMs() < *cooldownUntilMs_;
}

bool TriggerSet::isActive(ClipTrigger t) const {
    switch (t) {
    case ClipTrigger::Manual: return manual_.has_value();
    case ClipTrigger::Motion: return motionUntilMs_.has_value();
    case ClipTrigger::Schedule: return scheduleActive_;
    case ClipTrigger::Unknown: return false;
    }
    return false;
}

ClipTrigger TriggerSet::nextName(ClipTrigger previous) const {
    if (isActive(previous)) return previous;
    if (manual_) return ClipTrigger::Manual;
    if (scheduleActive_) return ClipTrigger::Schedule;
    return ClipTrigger::Motion;
}

void TriggerSet::emitThumbnail(uint64_t now) {
    RollEvent e;
    e.kind = RollEventKind::ThumbnailDue;
    e.clipSeq = clipSeq_;
    e.atMonoMs = now;
    e.trigger = clipTrigger_;
    push(std::move(e));
    thumbnailTaken_ = true;
}

void TriggerSet::startClip_(ClipTrigger t, ClipStartCause cause, uint32_t preRollMs, uint64_t now) {
    ++clipSeq_;
    clipOpen_ = true;
    clipStartMs_ = now;
    segmentStartMs_ = now; // until the host reports the pre-roll it gave the clip
    clipTrigger_ = t;
    thumbnailTaken_ = false;
    RollEvent e;
    e.kind = RollEventKind::StartClip;
    e.clipSeq = clipSeq_;
    e.atMonoMs = now;
    e.trigger = t;
    e.cause = cause;
    e.preRollMs = preRollMs;
    if (t == ClipTrigger::Manual && manual_) e.startedBy = manual_->who;
    push(std::move(e));
}

void TriggerSet::endClip_(ClipEndReason r, uint64_t now, RollBlock block) {
    if (!clipOpen_) return;
    // The live frame still shows the clip's view unless a block or a view change ended it (§6.1).
    const bool frameStillShowsClip =
        r == ClipEndReason::TriggersEnded || r == ClipEndReason::SegmentCap || r == ClipEndReason::Shutdown;
    if (frameStillShowsClip && !thumbnailTaken_) emitThumbnail(now);
    RollEvent e;
    e.kind = RollEventKind::EndClip;
    e.clipSeq = clipSeq_;
    e.atMonoMs = now;
    e.trigger = clipTrigger_;
    e.endReason = r;
    e.thumbnailTaken = thumbnailTaken_;
    e.block = block;
    push(std::move(e));
    clipOpen_ = false;
}

void TriggerSet::continueClip_(ClipEndReason r, ClipStartCause cause, uint64_t now) {
    const ClipTrigger previous = clipTrigger_;
    endClip_(r, now);
    if (anyTrigger()) startClip_(nextName(previous), cause, 0, now); // no pre-roll (§6.1, §6.2)
}

void TriggerSet::endIfIdle(uint64_t now) {
    if (!clipOpen_ || anyTrigger()) return;
    endClip_(ClipEndReason::TriggersEnded, now);
    // §6.2: the 30 s cooldown follows the end of a clip.
    cooldownUntilMs_ = now + kMotionCooldownMs;
}

void TriggerSet::advance(uint64_t now) {
    if (block_ != RollBlock::None) return; // a block dropped every trigger and ended the clip

    // Triggers that ran out, first: a manual trigger alone that reaches its 10 minutes ends the clip
    // rather than being cut into a new segment.
    if (manual_ && now >= manual_->sinceMs + kManualMaxMs) manual_.reset();
    if (motionUntilMs_ && now >= *motionUntilMs_) motionUntilMs_.reset();
    const bool inWindow = scheduleEnabled_ && isScheduledRecording(schedule_, clock_.localTime());
    if (!inWindow) {
        scheduleActive_ = false;
        scheduleSuppressed_ = false; // the window an abortClip() fell in has closed
    } else if (!scheduleActive_ && clipOpen_ && !scheduleSuppressed_) {
        // A window opening while a clip runs joins it, before endIfIdle below could end that clip.
        scheduleActive_ = true;
        RollEvent e;
        e.kind = RollEventKind::ExtendClip;
        e.clipSeq = clipSeq_;
        e.atMonoMs = now;
        e.trigger = ClipTrigger::Schedule;
        push(std::move(e));
    }
    endIfIdle(now);

    if (clipOpen_ && now >= segmentStartMs_ + kSegmentMaxMs) {
        continueClip_(ClipEndReason::SegmentCap, ClipStartCause::Segment, now);
    }

    if (inWindow && !scheduleActive_ && !scheduleSuppressed_) {
        scheduleActive_ = true;
        startClip_(ClipTrigger::Schedule, justUnblocked_ ? ClipStartCause::Unblocked : ClipStartCause::Trigger, 0, now);
    }

    if (clipOpen_ && !thumbnailTaken_ && now >= clipStartMs_ + kThumbnailDelayMs) emitThumbnail(now);
}

void TriggerSet::tick() { advance(clock_.monoMs()); }

void TriggerSet::setClipPreRoll(uint64_t clipSeq, uint64_t preRollMs) {
    if (!clipOpen_ || clipSeq != clipSeq_) return;
    segmentStartMs_ = clipStartMs_ - std::min(preRollMs, clipStartMs_);
    // A manual trigger's 10 minutes count from the clip's first frame, pre-roll included,
    // the same as the segment cut. So the manual trigger that started this clip runs out exactly at the cut, and
    // a full 10-minute manual clip ends there instead of leaving a few seconds' stub in a second file.
    if (manual_ && manual_->sinceMs == clipStartMs_) manual_->sinceMs = segmentStartMs_;
}

void TriggerSet::applyBlock(RollBlock b, uint64_t now) {
    if (b == block_) return;
    block_ = b;
    RollEvent e;
    e.kind = RollEventKind::BlockedChanged;
    e.clipSeq = clipOpen_ ? clipSeq_ : 0;
    e.atMonoMs = now;
    e.block = b;
    push(std::move(e));
    if (b != RollBlock::None) {
        endClip_(ClipEndReason::Blocked, now, b);
        // §6.2: the block drops the manual trigger. Motion and schedule are dropped too and come back on
        // their own: motion at its next detection, with no cooldown, and a schedule row still open when
        // the block clears.
        manual_.reset();
        motionUntilMs_.reset();
        scheduleActive_ = false;
        cooldownUntilMs_.reset();
        return;
    }
    justUnblocked_ = true;
    advance(now);
    justUnblocked_ = false;
}

void TriggerSet::setConditions(const RollConditions& c) {
    const uint64_t now = clock_.monoMs();
    advance(now);
    conditions_ = c;
    applyBlock(rollBlockFor(c), now);
}

void TriggerSet::setMotionEnabled(bool enabled) {
    const uint64_t now = clock_.monoMs();
    advance(now);
    motionEnabled_ = enabled;
    if (!enabled) {
        motionUntilMs_.reset();
        endIfIdle(now);
    }
}

void TriggerSet::setSchedule(std::vector<RecordScheduleRow> rows, bool enabled) {
    const uint64_t now = clock_.monoMs();
    advance(now);
    schedule_ = std::move(rows);
    scheduleEnabled_ = enabled;
    advance(now); // a window that closed ends the trigger; one that is open starts it
}

StartClipResult TriggerSet::startClip(const ClipStartedBy& who) {
    const uint64_t now = clock_.monoMs();
    advance(now);
    StartClipResult r;
    if (block_ != RollBlock::None) {
        r.refused = block_; // v3 §5.2: refused with the block's code, and no trigger is added
        return r;
    }
    r.ok = true;
    if (manual_) {
        manual_->sinceMs = now; // extended rather than restarted
        return r;
    }
    manual_ = Manual{who, now};
    if (clipOpen_) {
        RollEvent e;
        e.kind = RollEventKind::ExtendClip;
        e.clipSeq = clipSeq_;
        e.atMonoMs = now;
        e.trigger = ClipTrigger::Manual;
        push(std::move(e));
    } else {
        startClip_(ClipTrigger::Manual, ClipStartCause::Trigger, kPreRollMs, now);
    }
    return r;
}

StopClipResult TriggerSet::stopClip() {
    const uint64_t now = clock_.monoMs();
    advance(now);
    if (!clipOpen_) return StopClipResult::Ok;  // nothing recording: a no-op
    if (!manual_) return StopClipResult::Busy; // held by motion or the schedule only (v3 §5.2)
    manual_.reset();
    endIfIdle(now);
    return StopClipResult::Ok;
}

void TriggerSet::onMotion() {
    const uint64_t now = clock_.monoMs();
    advance(now);
    if (block_ != RollBlock::None || !motionEnabled_) return;
    if (clipOpen_) {
        const bool joined = !motionUntilMs_.has_value();
        motionUntilMs_ = now + kMotionHoldMs;
        if (joined) {
            RollEvent e;
            e.kind = RollEventKind::ExtendClip;
            e.clipSeq = clipSeq_;
            e.atMonoMs = now;
            e.trigger = ClipTrigger::Motion;
            push(std::move(e));
        }
        return;
    }
    if (cooldownUntilMs_ && now < *cooldownUntilMs_) return; // can't start a clip; not remembered
    motionUntilMs_ = now + kMotionHoldMs;
    startClip_(ClipTrigger::Motion, ClipStartCause::Trigger, kPreRollMs, now);
}

void TriggerSet::onViewChanged() {
    const uint64_t now = clock_.monoMs();
    advance(now);
    if (clipOpen_) continueClip_(ClipEndReason::ViewChanged, ClipStartCause::ViewChanged, now);
}

void TriggerSet::shutdown() {
    const uint64_t now = clock_.monoMs();
    advance(now);
    endClip_(ClipEndReason::Shutdown, now);
    manual_.reset();
    motionUntilMs_.reset();
    scheduleActive_ = false;
    cooldownUntilMs_.reset();
}

void TriggerSet::abortClip() {
    const uint64_t now = clock_.monoMs();
    advance(now);
    if (!clipOpen_) return;
    // No ThumbnailDue: the clip is being thrown away or cut short because it couldn't be written.
    RollEvent e;
    e.kind = RollEventKind::EndClip;
    e.clipSeq = clipSeq_;
    e.atMonoMs = now;
    e.trigger = clipTrigger_;
    e.endReason = ClipEndReason::WriterFailed;
    e.thumbnailTaken = thumbnailTaken_;
    push(std::move(e));
    clipOpen_ = false;
    // Fail closed: nothing restarts a clip on its own straight away, or a writer that keeps failing would
    // churn reservations. The manual trigger is gone (it must be started again), motion waits out the
    // usual cooldown, and an open schedule window stays off until it closes.
    manual_.reset();
    motionUntilMs_.reset();
    if (scheduleActive_) scheduleSuppressed_ = true;
    scheduleActive_ = false;
    cooldownUntilMs_ = now + kMotionCooldownMs;
}

} // namespace kvhost
