// One camera-roll playback stream (protocol v3 §5.9): a clip read from the roll and sent to one session as
// 0x18 playbackConfig and 0x19 playbackAccessUnit, at the clip's own timing, with 0x17 playbackState
// for every state change. Nothing is decoded or re-encoded: the samples the file holds go out as they
// are (pet-camera.md §6.1 "Playback over the protocol"), so only encoded H.264 leaves the PC.
//
// This is part of the colour exception (pet-camera.md §6.6): it reads clips from the camera-roll folder
// only, through the roll's own reader, and sends them only to the session that asked.
//
// The stream server owns one per playing session and makes every decision about who may play what
// (roles, privacy, slots). This class keeps the §5.9 state table: the server's commands come in under the
// server's lock, take this object's lock, update the state and send their 0x17 answer; the stream's own
// thread reads the file outside any lock and sends each frame under this object's lock only if the
// stream and its epoch are still the ones it read for. So no frame of a stopped stream, or of an epoch a
// seek replaced, is sent after the 0x17 that ended it. The thread never takes the server's lock.
#pragma once

#include "tls_conn.hpp"
#include "wire_protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace kvhost {

// §5.9 "Pacing": past this many queued bytes for the session, playback waits instead of reading on.
constexpr size_t kPlaybackBacklogBytes = 2u * 1024u * 1024u;
// v3 §2.3, §5.9: a stream paused or ended for this long without a command is stopped, freeing its slot.
constexpr int kPlaybackIdleMs = 60000;

class RollPlayback {
public:
    struct Params {
        std::shared_ptr<TlsConnection> conn;
        uint32_t streamId = 1;
        std::string clipId;
        std::wstring path;        // the clip's .mp4 (from the roll's index, never from the device)
        double durationSec = 0.0; // the sidecar's
        int modeRaw = 0, sbsLayoutRaw = 0; // the view the clip was recorded in (0x18)
        double startSec = 0.0;    // already clamped to [0, durationSec]
        size_t backlogBytes = kPlaybackBacklogBytes;
        int idleMs = kPlaybackIdleMs; // tests shorten it
        // Test seam: called on the stream's thread before it opens the clip, with no lock held (a test holds a
        // stream in `starting` there).
        std::function<void()> beforeOpen;
    };

    // Sends 0x17 starting with requestId, then starts the stream's thread, which sends 0x18, the frames,
    // and 0x17 playing once the first frame is out. onFreed runs on that thread, with no lock held, when the
    // stream frees itself (a read error ends it with 0x17 error; paused or ended for idleMs without a command
    // ends it with 0x17 stopped): the server then gives back its slot.
    static std::shared_ptr<RollPlayback> start(const Params& p, uint64_t requestId, std::function<void()> onFreed);
    ~RollPlayback();
    RollPlayback(const RollPlayback&) = delete;
    RollPlayback& operator=(const RollPlayback&) = delete;

    // The §5.9 actions on this stream. Each sends its one 0x17 answer (with requestId) and returns true;
    // false, with nothing sent, when the stream is already freed (stopped or error): the server answers
    // not_found then.
    bool pause(uint64_t requestId);
    bool resume(uint64_t requestId);
    bool seek(uint64_t requestId, double positionSec);
    // A stop by the device (with its requestId), or by the host (none, and the §9 sentence in message).
    bool stop(std::optional<uint64_t> requestId, const std::optional<std::string>& message);
    // The session is gone: the stream stops with nothing sent.
    void stopSilently();
    // Waits for the stream's thread; call it without the server's lock (it may be reading the file). Safe
    // from several threads at once.
    void join();

    bool allocated() const; // not stopped or failed: the stream id still answers
    uint32_t streamId() const { return p_.streamId; }
    const std::string& clipId() const { return p_.clipId; }
    PlaybackState state() const;
    uint64_t framesSent() const { return frames_sent_.load(); }

private:
    explicit RollPlayback(const Params& p) : p_(p) {}
    void threadMain();
    // m_ held.
    void sendStateLocked(std::optional<uint64_t> requestId, const std::optional<std::string>& message = std::nullopt);
    void sendConfigLocked(uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>>& parameterSets);

    Params p_;
    std::function<void()> on_freed_;
    std::thread th_;
    std::mutex join_m_; // join() may be called from two threads (a delete waiting for the file, the server's loop)
    mutable std::mutex m_;
    std::condition_variable cv_;
    PlaybackState state_ = PlaybackState::Starting;
    bool stop_ = false;
    uint64_t last_command_ns_ = 0; // the last command, or entering paused or ended: the idle clock (§5.9)
    uint32_t epoch_ = 1;         // §5.9: per stream, 1 at the start, one higher on each seek
    uint32_t read_epoch_ = 0;    // the epoch the thread has seeked the reader for
    double seek_to_ = 0.0;       // where the current epoch starts
    bool show_one_ = false;      // a seek while paused: send that one keyframe, stay paused
    bool rebase_ = true;         // the clip's clock restarts at the next frame (start, seek, resume, backlog)
    double position_ = 0.0;      // the media time of the last frame sent
    std::atomic<uint64_t> frames_sent_{0};
};

} // namespace kvhost
