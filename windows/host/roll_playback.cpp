#include "roll_playback.hpp"

#include "roll_media.hpp"
#include "roll_wire.hpp" // clampPlaybackPosition

#include <objbase.h>

#include <algorithm>
#include <chrono>

namespace kvhost {

namespace {

uint64_t steadyNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr uint64_t kProgressEveryNs = 1000000000ULL; // §5.9: 0x17 at most once a second while playing

} // namespace

std::shared_ptr<RollPlayback> RollPlayback::start(const Params& p, uint64_t requestId, std::function<void()> onFreed) {
    std::shared_ptr<RollPlayback> pb(new RollPlayback(p));
    pb->on_freed_ = std::move(onFreed);
    pb->seek_to_ = clampPlaybackPosition(p.startSec, p.durationSec);
    pb->position_ = pb->seek_to_;
    pb->last_command_ns_ = steadyNs();
    {
        std::lock_guard<std::mutex> lk(pb->m_);
        pb->sendStateLocked(requestId); // none -> starting, with the requestId (§5.9)
    }
    RollPlayback* raw = pb.get();
    pb->th_ = std::thread([raw] { raw->threadMain(); });
    return pb;
}

RollPlayback::~RollPlayback() {
    stopSilently();
    join();
}

void RollPlayback::join() {
    std::lock_guard<std::mutex> lk(join_m_);
    if (!th_.joinable()) return;
    if (th_.get_id() == std::this_thread::get_id()) {
        th_.detach(); // the last owner let go from the stream's own thread, which is ending anyway
        return;
    }
    th_.join();
}

bool RollPlayback::allocated() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_ != PlaybackState::Stopped && state_ != PlaybackState::Error;
}

PlaybackState RollPlayback::state() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_;
}

void RollPlayback::sendStateLocked(std::optional<uint64_t> requestId, const std::optional<std::string>& message) {
    PlaybackStateMessage m;
    m.requestId = requestId;
    m.streamId = p_.streamId;
    m.clipId = p_.clipId;
    m.state = toWire(state_);
    m.positionSec = state_ == PlaybackState::Ended ? p_.durationSec : position_;
    m.durationSec = p_.durationSec;
    m.message = message;
    p_.conn->send(packFrame(MsgType::PlaybackState, 0, toJson(m)));
}

void RollPlayback::sendConfigLocked(uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>>& parameterSets) {
    // §5.9: every 0x02 key, with the clip's own view and size, then the stream id. A new epoch (the start, a
    // seek) always gets a fresh one before its frames.
    PlaybackConfigMessage c;
    c.config.streamEpoch = epoch_;
    c.config.codecRaw = static_cast<uint8_t>(CodecKind::H264);
    c.config.width = static_cast<int>(width);
    c.config.height = static_cast<int>(height);
    c.config.modeRaw = p_.modeRaw;
    c.config.sbsLayoutRaw = p_.sbsLayoutRaw;
    c.config.nalLengthHeaderBytes = 4;
    c.config.parameterSets = parameterSets;
    c.streamId = p_.streamId;
    c.clipId = p_.clipId;
    c.durationSec = p_.durationSec;
    p_.conn->send(packFrame(MsgType::PlaybackConfig, 0, toJson(c)));
}

bool RollPlayback::pause(uint64_t requestId) {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) return false;
    // starting / playing -> paused; paused and ended: no change (§5.9).
    if (state_ == PlaybackState::Starting || state_ == PlaybackState::Playing) state_ = PlaybackState::Paused;
    last_command_ns_ = steadyNs();
    sendStateLocked(requestId);
    cv_.notify_all();
    return true;
}

bool RollPlayback::resume(uint64_t requestId) {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) return false;
    if (state_ == PlaybackState::Paused) {
        state_ = PlaybackState::Playing; // from where it paused; the clock restarts there
        rebase_ = true;
    } else if (state_ == PlaybackState::Ended) {
        // ended -> playing from 0: a new epoch from the start, with its own 0x18.
        ++epoch_;
        seek_to_ = 0.0;
        position_ = 0.0;
        state_ = PlaybackState::Playing;
    }
    // starting and playing: no change. A starting stream plays once ready, with no pause intent
    // (§5.9).
    last_command_ns_ = steadyNs();
    sendStateLocked(requestId);
    cv_.notify_all();
    return true;
}

bool RollPlayback::seek(uint64_t requestId, double positionSec) {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) return false;
    // Every seek starts a new epoch, with a fresh 0x18 before its frames, from the keyframe at or before
    // the position (§5.9).
    ++epoch_;
    seek_to_ = clampPlaybackPosition(positionSec, p_.durationSec);
    position_ = seek_to_;
    switch (state_) {
    case PlaybackState::Starting:
    case PlaybackState::Playing: state_ = PlaybackState::Playing; break;
    case PlaybackState::Paused: show_one_ = true; break; // stays paused; the new 0x18 and that one keyframe
    // -> playing. (A paused stream never reaches ended, so "paused if paused before it ended" can't happen.)
    case PlaybackState::Ended: state_ = PlaybackState::Playing; break;
    default: break;
    }
    last_command_ns_ = steadyNs();
    sendStateLocked(requestId);
    cv_.notify_all();
    return true;
}

bool RollPlayback::stop(std::optional<uint64_t> requestId, const std::optional<std::string>& message) {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) return false;
    state_ = PlaybackState::Stopped; // frees the stream: a later action on it gets not_found
    stop_ = true;
    // A host-initiated stop (privacy, sharing off, the slot taken, a delete, a replacement) drops the frames
    // still queued for the session before its 0x17, as dropSessionLocked does with dropQueuedVideo: nothing of the
    // clip reaches the device after the reason to stop. Under m_, so this stream's thread can't queue one between.
    if (!requestId) p_.conn->dropQueued(kDropPlaybackFrames);
    sendStateLocked(requestId, message);
    cv_.notify_all();
    return true;
}

void RollPlayback::stopSilently() {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    if (state_ != PlaybackState::Error) state_ = PlaybackState::Stopped;
    cv_.notify_all();
}

void RollPlayback::threadMain() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the reader's thread must have COM
    bool freed_self = false;
    if (p_.beforeOpen) p_.beforeOpen();
    {
        RollClipReader reader;
        std::string err;
        const bool opened = reader.open(p_.path, &err);
        std::unique_lock<std::mutex> lk(m_);
        auto fail = [&]() {
            if (stop_ || state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) return;
            state_ = PlaybackState::Error; // frees the stream (§5.9)
            // A host-side failure: the §9 sentence, with no path or system detail.
            sendStateLocked(std::nullopt, std::string(text::kClipUnreadable));
            freed_self = true;
        };
        if (!opened) fail();
        uint32_t frame_seq = 0;
        bool need_config = true;
        std::optional<RollSample> held; // read for the current epoch, not sent yet
        uint64_t base_wall_ns = 0;
        int64_t base_pts = 0;
        uint64_t last_progress_ns = 0;
        while (opened && !stop_ && state_ != PlaybackState::Stopped && state_ != PlaybackState::Error) {
            if (read_epoch_ != epoch_) {
                // The start, or a seek: the reader goes to the keyframe at or before the position.
                const uint32_t e = epoch_;
                const double pos = seek_to_;
                held.reset();
                lk.unlock();
                const bool ok = reader.seek(static_cast<int64_t>(pos * kRollHnsPerSecond), &err);
                lk.lock();
                if (!ok) {
                    fail();
                    break;
                }
                read_epoch_ = e; // if another seek came meanwhile, the next turn seeks again
                frame_seq = 0;   // §5.9: frameSeq counts from 0 per streamEpoch
                need_config = true;
                rebase_ = true;
                continue;
            }
            if (state_ == PlaybackState::Ended || (state_ == PlaybackState::Paused && !show_one_)) {
                // v3 §2.3, §5.9: paused or ended keeps the slot only for idleMs without a command; then the host
                // stops the stream (0x17 stopped, no requestId), so a device that went away can't hold a slot.
                const uint64_t idle_ns = static_cast<uint64_t>(std::max(1, p_.idleMs)) * 1000000ULL;
                const uint64_t now = steadyNs();
                if (now - last_command_ns_ >= idle_ns) {
                    state_ = PlaybackState::Stopped;
                    stop_ = true;
                    sendStateLocked(std::nullopt, std::string(text::kPlaybackIdle));
                    freed_self = true;
                    break;
                }
                cv_.wait_for(lk, std::chrono::nanoseconds(last_command_ns_ + idle_ns - now));
                continue;
            }
            if (!held) {
                lk.unlock();
                RollSample smp;
                const RollClipReader::Read r = reader.next(smp, &err);
                lk.lock();
                if (read_epoch_ != epoch_) continue; // a seek came while reading: that sample is for the old epoch
                if (stop_ || state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) break;
                if (r == RollClipReader::Read::End) {
                    if (show_one_) show_one_ = false; // nothing to show at the very end
                    if (state_ != PlaybackState::Paused) {
                        // §5.9: the last frame has gone; the stream stays allocated for seek or resume.
                        state_ = PlaybackState::Ended;
                        position_ = p_.durationSec;
                        last_command_ns_ = steadyNs(); // the idle clock starts when it ends
                        sendStateLocked(std::nullopt);
                    }
                    continue;
                }
                if (r == RollClipReader::Read::Error) {
                    fail();
                    break;
                }
                held = std::move(smp);
                continue;
            }
            const uint64_t now = steadyNs();
            if (rebase_) {
                // The clip's clock (re)starts at this frame: after a start, a seek or a resume.
                base_wall_ns = now;
                base_pts = held->pts_hns;
                rebase_ = false;
            }
            if (!show_one_) {
                // §5.9: frames go at the clip's own timing.
                const int64_t ahead_hns = held->pts_hns - base_pts;
                const uint64_t due = base_wall_ns + static_cast<uint64_t>(ahead_hns > 0 ? ahead_hns : 0) * 100ULL;
                if (now < due) {
                    cv_.wait_for(lk, std::chrono::nanoseconds(due - now));
                    continue;
                }
            }
            // §5.9 "Pacing": past 2 MiB queued for this session it waits rather than drop; playback may run
            // late but never skips. The clock restarts once the backlog has drained, so it doesn't burst.
            if (p_.conn->queuedBytes() > p_.backlogBytes) {
                cv_.wait_for(lk, std::chrono::milliseconds(5));
                rebase_ = true;
                continue;
            }
            if (need_config) {
                sendConfigLocked(reader.width(), reader.height(), reader.parameterSets());
                need_config = false;
            }
            PlaybackAccessUnitHeader h;
            h.streamId = p_.streamId;
            h.au.streamEpoch = epoch_;
            h.au.frameSeq = frame_seq++;
            h.au.hostCaptureNs = static_cast<uint64_t>(held->pts_hns > 0 ? held->pts_hns : 0) * 100ULL; // media time
            h.au.hostEncodeDoneNs = 0;
            h.au.flags = held->keyframe ? 0x01 : 0x00;
            std::vector<uint8_t> payload;
            if (!h.encode(held->avcc.data(), held->avcc.size(), payload)) {
                fail(); // an AU that can't be sent: playback never skips silently
                break;
            }
            p_.conn->send(packFrame(MsgType::PlaybackAccessUnit, 0, payload), true, kDropPlaybackFrames);
            frames_sent_.fetch_add(1);
            position_ = static_cast<double>(held->pts_hns) / kRollHnsPerSecond;
            held.reset();
            if (show_one_) {
                show_one_ = false; // a seek while paused: that one keyframe shown, still paused
                rebase_ = true;
            } else if (state_ == PlaybackState::Starting) {
                state_ = PlaybackState::Playing; // starting -> playing once the first frame is out
                sendStateLocked(std::nullopt);
                last_progress_ns = now;
            } else if (state_ == PlaybackState::Playing && now - last_progress_ns >= kProgressEveryNs) {
                sendStateLocked(std::nullopt);
                last_progress_ns = now;
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (freed_self && on_freed_) on_freed_();
}

} // namespace kvhost
