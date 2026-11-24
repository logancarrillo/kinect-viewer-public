// Live quality tiers (protocol v3 §6.1-6.2): the shown view is encoded once per tier (high / medium /
// low) and fanned out. Each tier has its own MfH264Encoder and its own thread, so opening, closing or a
// slow software encode on one tier never stalls another; `high` is today's v1/v2 stream byte for byte.
// Every (re)open and every new epoch takes a number from one host-wide counter shared by all tiers (and
// the stream server takes one from it for a session moving onto a tier whose epoch it has already passed,
// without starting a new epoch on that tier).
#pragma once

#include "mf_h264_encoder.hpp"
#include "video_convert.hpp"
#include "wire_protocol.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kvhost {

// LiveTier is defined in wire_protocol.hpp (High = 0, Medium = 1, Low = 2; Unknown is decode-only).
constexpr int kLiveTierCount = 3;

const char* liveTierName(LiveTier t); // "high", "medium", "low" (the v3 LiveTier strings)

// What one tier encodes for a given source frame size.
struct LiveTierParams {
    uint32_t width = 0, height = 0; // encoded size (even)
    bool upscale2x = false;         // high (and a tier the same size as high): 512x424 -> 1024x848 nearest
    bool same_as_high = false;      // the tier's frame is made exactly like high's (bgraToNv12)
    uint32_t bitrate = 0;
    uint32_t fps = 30;
    uint32_t gop = 30;
    uint32_t frame_divisor = 1; // low takes every second submitted frame
};

// high: streamTargetSize; medium: high's size with the longest side <= 1280; low: <= 640. Aspect is
// kept, the longest side becomes exactly the limit and the other side is rounded to the nearest even.
LiveTierParams liveTierParams(LiveTier t, uint32_t src_w, uint32_t src_h);

// The source pixels for one submitted frame, shared read-only by every tier that encodes it.
struct LiveFrame {
    std::shared_ptr<const std::vector<uint8_t>> bgra;
    uint32_t width = 0, height = 0, stride = 0;
    uint64_t frame_seq = 0;
    uint64_t capture_ns = 0; // kv_mapped_frame_t::arrival_mach_ns; also the encoder timestamp (/100)
    int mode = 0, sbs = 0;   // passed through to the output (and a change starts a new epoch)
    bool overlay = false;    // clock overlay flag, passed through
};

// Which IDRs coming out of a tier's encoder carry config_due (v3 §6.2). Tied to the AU's own epoch, not
// to one shared flag: with an async (hardware) MFT a few frames are still inside the encoder when a new
// epoch starts, and an old epoch's IDR coming out then (a GOP IDR, an earlier forced one) must not use
// up the new epoch's config. Not thread-safe: TierEncoder guards it with its lock. IDRs are fed in the
// order they come out of the encoder.
class TierConfigGate {
public:
    // The tier opened, started an epoch, or was asked for a keyframe with a config.
    void request() { requested_ = true; }
    // An IDR of epoch au_epoch came out while the tier's current epoch is current_epoch: true when it
    // carries config_due. That is every epoch's first IDR (once SPS/PPS are known), and the next IDR of
    // the current epoch after a request; an IDR of an older epoch never uses up a request.
    bool onIdr(uint32_t au_epoch, uint32_t current_epoch, bool params_known) {
        if (!params_known) return false;
        const bool first_of_epoch = !configured_ || au_epoch != last_config_epoch_;
        const bool asked = requested_ && au_epoch == current_epoch;
        if (!first_of_epoch && !asked) return false;
        configured_ = true;
        last_config_epoch_ = au_epoch;
        if (au_epoch == current_epoch) requested_ = false;
        return true;
    }

private:
    bool requested_ = true;
    bool configured_ = false;
    uint32_t last_config_epoch_ = 0;
};

// One encoded access unit of one tier, with everything a 0x02 / 0x03 needs.
struct TierAccessUnit {
    LiveTier tier = LiveTier::High;
    uint32_t epoch = 0;
    bool idr = false;
    // The tier's TierConfigGate: true on every epoch's first IDR (with SPS/PPS known), and on the next
    // IDR of the current epoch after a keyframe-with-config request.
    bool config_due = false;
    std::vector<uint8_t> avcc;     // annexBToAvcc output: 4-byte lengths, no SPS/PPS/AUD
    std::vector<uint8_t> sps, pps; // the tier's current parameter sets (no start codes)
    uint32_t width = 0, height = 0;
    int mode = 0, sbs = 0;
    bool overlay = false;
    uint64_t frame_seq = 0;
    uint64_t capture_ns = 0;
    uint64_t encode_done_ns = 0; // steady_clock ns, as HostController::onEncoded uses
    std::string encoder_name;    // e.g. "NVIDIA H.264 Encoder MFT (hardware)"
    bool hardware = false;
};

// The 0x02 HostController sends today for this AU's tier state (epoch, size, mode, SPS/PPS).
StreamConfigMessage streamConfigFor(const TierAccessUnit& au);
// The 0x03 body as HostController builds it today; returns its flags (bit 0 IDR, bit 1 overlay).
uint8_t videoPayloadFor(const TierAccessUnit& au, std::vector<uint8_t>& payload);

struct TierDiagnostics {
    LiveTier tier = LiveTier::High;
    bool active = false;     // accepting frames (wanted, or inside its hold time)
    bool open = false;       // an MF encoder is open
    std::string encoder_name; // with " (hardware)" / " (software fallback)", as today's encoderName()
    bool hardware = false;
    uint32_t width = 0, height = 0;
    uint32_t epoch = 0;
    uint64_t frames_in = 0;       // frames handed to the encoder
    uint64_t frames_out = 0;      // access units delivered
    uint64_t frames_skipped = 0;  // not taken by frame_divisor (low)
    uint64_t frames_replaced = 0; // overwritten in the latest-wins slot before the thread took them
    uint64_t forced_keyframes = 0;
    uint64_t frames_before_idr = 0; // AUs of a new epoch dropped because its IDR had not come out yet
    uint64_t opens = 0;
    std::string last_error;
};

// One tier: a latest-wins frame slot, a thread, and an MfH264Encoder opened off the submit path.
class TierEncoder {
public:
    using OutputFn = std::function<void(const TierAccessUnit&)>;

    struct Options {
        LiveTier tier = LiveTier::High;
        EncoderOptions base;   // HostController::Options::encoder; bitrate/fps/gop come from the tier
        bool hardware_allowed = true; // tests turn this off to take the software path for medium/low
        std::chrono::milliseconds keyframe_gap{500}; // forced IDRs at most this often
        // With a forced IDR due and no new frame for this long (frozen view), re-encode the last frame.
        std::chrono::milliseconds replay_after{150};
    };

    // epochs: the host-wide live epoch counter; it must outlive this object. on_output runs on the
    // encoder's output thread (the MFT event thread, or this tier's thread for a software MFT).
    TierEncoder(const Options& opts, std::atomic<uint32_t>& epochs, OutputFn on_output);
    ~TierEncoder();
    TierEncoder(const TierEncoder&) = delete;
    TierEncoder& operator=(const TierEncoder&) = delete;

    // Start taking frames (cancels a scheduled close). The encoder opens on the next frame.
    void activate();
    // Close the encoder at `when` unless activate() comes first (the hold time). Runs on this thread.
    void scheduleClose(std::chrono::steady_clock::time_point when);
    bool active() const;

    // Never blocks on encoding: replaces whatever frame is still waiting (latest wins).
    void submit(const LiveFrame& f);

    // Forced IDR, coalesced to at most one per keyframe_gap. with_config also marks the next IDR
    // config_due (today's requestKeyframeAndConfig). A request made while a forced IDR is waiting
    // for the gap is merged into it.
    void requestKeyframe(bool with_config = true);
    // Start a new epoch on the next frame without reopening: new epoch number, IDR, config_due.
    void requestNewEpoch();

    TierDiagnostics diagnostics() const;
    LiveTier tier() const { return opts_.tier; }

    // Stops the thread and closes the encoder; no output callback runs after it returns. Idempotent.
    void close();

private:
    struct FrameCtx {
        uint64_t frame_seq = 0, capture_ns = 0;
        bool overlay = false;
        uint32_t epoch = 0;
        int mode = 0, sbs = 0;
        uint32_t width = 0, height = 0;
    };
    void threadMain();
    void encodeOne(const LiveFrame& f, bool replay);
    bool openFor(const LiveTierParams& p);
    void closeEncoder(); // thread_ only, without m_ held
    void onEncoded(const uint8_t* annexb, size_t n, uint64_t token);

    Options opts_;
    std::atomic<uint32_t>& epochs_;
    OutputFn out_;
    MfH264Encoder encoder_; // used only on thread_ (and its own output thread)
    Nv12Scaler scaler_;     // thread_ only
    std::vector<uint8_t> nv12_;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stop_ = false;
    bool active_ = false;
    bool close_scheduled_ = false;
    std::chrono::steady_clock::time_point close_at_{};
    bool have_slot_ = false;
    LiveFrame slot_;
    uint64_t phase_ = 0; // frame_divisor counter

    // Encoder state, guarded by m_ (read by the output callback).
    bool enc_open_ = false;
    bool force_software_ = false; // medium / low after a hardware encoder failed to encode
    uint32_t enc_w_ = 0, enc_h_ = 0;
    int enc_mode_ = -1, enc_sbs_ = -1;
    uint32_t epoch_ = 0;
    TierConfigGate config_; // which IDRs carry config_due
    bool force_next_ = false; // IDR on the next frame, no coalescing (open, mode change, failed encode)
    bool kf_wanted_ = false;  // a session asked; waits for keyframe_gap
    bool epoch_wanted_ = false;
    bool kf_ever_ = false;
    std::chrono::steady_clock::time_point last_forced_{};
    std::chrono::steady_clock::time_point last_input_{};
    LiveFrame last_frame_;
    int64_t last_pts_ = 0;
    uint64_t next_token_ = 0;
    uint64_t last_forced_token_ = 0;
    bool awaiting_forced_ = false; // the forced frame's AU has not come out yet
    int reforces_ = 0;
    uint32_t started_epoch_ = 0; // the epoch whose first IDR has gone out
    std::map<uint64_t, FrameCtx> ctx_;
    std::vector<uint8_t> sps_, pps_;
    std::string encoder_name_;
    bool hardware_ = false;
    std::string last_error_;
    uint64_t frames_in_ = 0, frames_out_ = 0, frames_skipped_ = 0, frames_replaced_ = 0, forced_ = 0, opens_ = 0;
    uint64_t frames_before_idr_ = 0;
};

// The three tiers: reference-counted want/release with a hold time before a tier closes, one shared
// BGRA copy per submitted frame fanned out to the active tiers, and per-tier diagnostics.
class LiveTierSet {
public:
    struct Options {
        EncoderOptions encoder; // HostController::Options::encoder
        std::chrono::milliseconds hold{60000}; // a tier stays open this long after its last release
        std::chrono::milliseconds keyframe_gap{500};
        std::chrono::milliseconds replay_after{150};
        std::array<bool, kLiveTierCount> hardware_allowed{true, true, true}; // tests only
    };

    LiveTierSet(const Options& opts, std::atomic<uint32_t>& epochs, TierEncoder::OutputFn on_output);
    ~LiveTierSet();
    LiveTierSet(const LiveTierSet&) = delete;
    LiveTierSet& operator=(const LiveTierSet&) = delete;

    // A session (or the camera roll for medium) needs the tier. Opens it on the next frame.
    void want(LiveTier t);
    // One user fewer; at zero the tier closes after the hold time unless wanted again.
    void release(LiveTier t);
    int wanters(LiveTier t) const;
    bool active(LiveTier t) const;
    bool anyActive() const;

    // Copies the frame once (only if a tier is active) and hands it to every active tier.
    void submitFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride, uint64_t frame_seq,
                     uint64_t capture_ns, int mode, int sbs, bool overlay);

    void requestKeyframe(LiveTier t, bool with_config = true);
    void requestKeyframeAll(bool with_config = true);
    void requestNewEpoch(LiveTier t);

    TierDiagnostics diagnostics(LiveTier t) const;
    TierEncoder& tier(LiveTier t) { return *tiers_[static_cast<int>(t)]; }

    void close(); // joins every tier thread

private:
    std::shared_ptr<std::vector<uint8_t>> takeBuffer(size_t n);

    Options opts_;
    std::array<std::unique_ptr<TierEncoder>, kLiveTierCount> tiers_;
    mutable std::mutex m_;
    std::array<int, kLiveTierCount> wanters_{0, 0, 0};
    std::vector<std::shared_ptr<std::vector<uint8_t>>> pool_; // reused BGRA copies
};

} // namespace kvhost
