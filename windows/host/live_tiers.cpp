#include "live_tiers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>

namespace kvhost {

namespace {

using Clock = std::chrono::steady_clock;

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// Longest side becomes `limit` when it is larger; the other side keeps the aspect, rounded to the
// nearest even number (halves round up), and never below 2.
void fitLongestSide(uint32_t w, uint32_t h, uint32_t limit, uint32_t& ow, uint32_t& oh) {
    if (w == 0 || h == 0 || std::max(w, h) <= limit) {
        ow = w;
        oh = h;
        return;
    }
    auto shortSide = [limit](uint32_t s, uint32_t l) {
        const uint64_t halves = (static_cast<uint64_t>(s) * limit + l) / (2ULL * l);
        return std::max<uint32_t>(2, static_cast<uint32_t>(halves * 2));
    };
    if (w >= h) {
        ow = limit;
        oh = shortSide(h, w);
    } else {
        oh = limit;
        ow = shortSide(w, h);
    }
}

} // namespace

const char* liveTierName(LiveTier t) {
    switch (t) {
    case LiveTier::High: return "high";
    case LiveTier::Medium: return "medium";
    case LiveTier::Low: return "low";
    }
    return "high";
}

LiveTierParams liveTierParams(LiveTier t, uint32_t src_w, uint32_t src_h) {
    LiveTierParams p;
    uint32_t hw = 0, hh = 0;
    bool up = false;
    streamTargetSize(src_w, src_h, hw, hh, up);
    switch (t) {
    case LiveTier::High:
        p.width = hw;
        p.height = hh;
        p.bitrate = hw >= 1600 ? 10000000u : 5000000u; // HostController's rule today
        break;
    case LiveTier::Medium:
        fitLongestSide(hw, hh, 1280, p.width, p.height);
        p.bitrate = 2500000u;
        break;
    case LiveTier::Low:
        fitLongestSide(hw, hh, 640, p.width, p.height);
        p.bitrate = 800000u;
        p.fps = 15;
        p.gop = 15;
        p.frame_divisor = 2;
        break;
    }
    // A tier that ends up at high's size is made exactly like high (keeps the 2x nearest IR / depth upscale).
    p.same_as_high = p.width == hw && p.height == hh;
    p.upscale2x = p.same_as_high && up;
    return p;
}

StreamConfigMessage streamConfigFor(const TierAccessUnit& au) {
    StreamConfigMessage cfg;
    cfg.streamEpoch = au.epoch;
    cfg.codecRaw = static_cast<uint8_t>(CodecKind::H264);
    cfg.width = static_cast<int>(au.width);
    cfg.height = static_cast<int>(au.height);
    cfg.modeRaw = au.mode;
    cfg.sbsLayoutRaw = au.sbs;
    cfg.nalLengthHeaderBytes = 4;
    cfg.parameterSets = {au.sps, au.pps};
    return cfg;
}

uint8_t videoPayloadFor(const TierAccessUnit& au, std::vector<uint8_t>& payload) {
    VideoAccessUnitHeader h;
    h.streamEpoch = au.epoch;
    h.frameSeq = au.frame_seq;
    h.hostCaptureNs = au.capture_ns;
    h.hostEncodeDoneNs = au.encode_done_ns;
    h.flags = static_cast<uint8_t>((au.idr ? 0x01 : 0x00) | (au.overlay ? 0x02 : 0x00));
    h.encode(au.avcc.data(), au.avcc.size(), payload);
    return h.flags;
}

// ---- TierEncoder ----------------------------------------------------------------------------------

TierEncoder::TierEncoder(const Options& opts, std::atomic<uint32_t>& epochs, OutputFn on_output)
    : opts_(opts), epochs_(epochs), out_(std::move(on_output)) {
    thread_ = std::thread([this] { threadMain(); });
}

TierEncoder::~TierEncoder() { close(); }

void TierEncoder::activate() {
    std::lock_guard<std::mutex> lk(m_);
    active_ = true;
    close_scheduled_ = false;
    cv_.notify_one();
}

void TierEncoder::scheduleClose(std::chrono::steady_clock::time_point when) {
    std::lock_guard<std::mutex> lk(m_);
    if (!active_) return;
    close_scheduled_ = true;
    close_at_ = when;
    cv_.notify_one();
}

bool TierEncoder::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return active_ && !stop_;
}

void TierEncoder::submit(const LiveFrame& f) {
    std::lock_guard<std::mutex> lk(m_);
    if (stop_ || !active_ || !f.bgra) return;
    const uint32_t div = opts_.tier == LiveTier::Low ? 2u : 1u;
    if (div > 1) {
        // A due IDR takes the frame at once: while the view is frozen, the one resubmitted frame must
        // not be the one we skip.
        const bool kf_due = force_next_ ||
                            (kf_wanted_ && (!kf_ever_ || Clock::now() - last_forced_ >= opts_.keyframe_gap));
        if (kf_due) {
            phase_ = 1;
        } else if ((phase_++ % div) != 0) {
            ++frames_skipped_;
            return;
        }
    }
    if (have_slot_) ++frames_replaced_;
    slot_ = f;
    have_slot_ = true;
    cv_.notify_one();
}

void TierEncoder::requestKeyframe(bool with_config) {
    std::lock_guard<std::mutex> lk(m_);
    kf_wanted_ = true;
    if (with_config) config_.request();
    cv_.notify_one();
}

void TierEncoder::requestNewEpoch() {
    std::lock_guard<std::mutex> lk(m_);
    kf_wanted_ = true;
    epoch_wanted_ = true;
    config_.request();
    cv_.notify_one();
}

TierDiagnostics TierEncoder::diagnostics() const {
    std::lock_guard<std::mutex> lk(m_);
    TierDiagnostics d;
    d.tier = opts_.tier;
    d.active = active_ && !stop_;
    d.open = enc_open_;
    d.encoder_name = encoder_name_;
    d.hardware = hardware_;
    d.width = enc_w_;
    d.height = enc_h_;
    d.epoch = epoch_;
    d.frames_in = frames_in_;
    d.frames_out = frames_out_;
    d.frames_skipped = frames_skipped_;
    d.frames_replaced = frames_replaced_;
    d.forced_keyframes = forced_;
    d.frames_before_idr = frames_before_idr_;
    d.opens = opens_;
    d.last_error = last_error_;
    return d;
}

void TierEncoder::close() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        cv_.notify_one();
    }
    // The thread closes the encoder before it exits, so no output callback runs after this.
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void TierEncoder::threadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (true) {
        LiveFrame f;
        bool replay = false, do_close = false;
        {
            std::unique_lock<std::mutex> lk(m_);
            while (!stop_) {
                const auto now = Clock::now();
                // The hold deadline first, so a busy slot can't keep an unwanted tier open.
                if (close_scheduled_ && now >= close_at_) {
                    close_scheduled_ = false;
                    active_ = false;
                    do_close = true;
                    break;
                }
                if (have_slot_) {
                    f = std::move(slot_);
                    slot_ = LiveFrame{};
                    have_slot_ = false;
                    break;
                }
                auto wake = Clock::time_point::max();
                if (close_scheduled_) wake = close_at_;
                // A forced IDR is waiting but no frame comes (the view is frozen, or the coalescing gap
                // swallowed the one resubmitted frame): re-encode the last frame so the IDR still goes out.
                if (active_ && enc_open_ && (kf_wanted_ || force_next_) && last_frame_.bgra) {
                    const auto gap_ok = (kf_ever_ && !force_next_) ? last_forced_ + opts_.keyframe_gap : now;
                    const auto due = std::max(gap_ok, last_input_ + opts_.replay_after);
                    if (now >= due) {
                        f = last_frame_;
                        replay = true;
                        break;
                    }
                    wake = std::min(wake, due);
                }
                if (wake == Clock::time_point::max()) cv_.wait(lk);
                else cv_.wait_until(lk, wake);
            }
            if (stop_) break;
        }
        if (do_close) {
            closeEncoder();
            continue;
        }
        encodeOne(f, replay);
    }
    closeEncoder();
    CoUninitialize();
}

void TierEncoder::closeEncoder() {
    encoder_.close(); // joins the MFT's event thread; never under m_ (its callback takes m_)
    std::lock_guard<std::mutex> lk(m_);
    enc_open_ = false;
    enc_w_ = enc_h_ = 0;
    enc_mode_ = enc_sbs_ = -1;
    force_next_ = false;
    force_software_ = false;
    kf_wanted_ = false;
    epoch_wanted_ = false;
    awaiting_forced_ = false;
    reforces_ = 0;
    sps_.clear();
    pps_.clear();
    ctx_.clear();
    last_frame_ = LiveFrame{};
    encoder_name_.clear();
    hardware_ = false;
    phase_ = 0;
    if (!active_) {
        have_slot_ = false;
        slot_ = LiveFrame{};
    }
}

bool TierEncoder::openFor(const LiveTierParams& p) {
    encoder_.close();
    EncoderOptions eo = opts_.base;
    eo.bitrate = p.bitrate;
    bool force_sw = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        force_sw = force_software_;
    }
    if (opts_.tier != LiveTier::High) {
        // Hardware first; consumer GPUs limit concurrent sessions, so medium / low may fall back to the
        // software MFT. high keeps today's options untouched.
        eo.fps = p.fps;
        eo.gop = p.gop;
        eo.prefer_hardware = eo.prefer_hardware && opts_.hardware_allowed && !force_sw;
        eo.allow_software = true;
    } else if (!opts_.hardware_allowed) {
        eo.prefer_hardware = false;
        eo.allow_software = true;
    }
    std::string err;
    const bool ok = encoder_.open(p.width, p.height, eo, [this](const uint8_t* d, size_t n, int64_t, uint64_t token) {
        onEncoded(d, n, token);
    }, &err);
    // Fallback parameter sets for encoders that do not repeat SPS/PPS in-band before IDRs.
    std::vector<uint8_t> sps, pps;
    if (ok) {
        const std::vector<uint8_t> seq = encoder_.sequenceHeader();
        for (auto& nal : splitAnnexB(seq.data(), seq.size())) {
            if ((nal[0] & 0x1F) == 7) sps = nal;
            if ((nal[0] & 0x1F) == 8) pps = nal;
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    if (!ok) {
        last_error_ = "encoder: " + err;
        enc_open_ = false;
        enc_w_ = enc_h_ = 0;
        return false;
    }
    encoder_name_ = encoder_.name() + (encoder_.isHardware() ? " (hardware)" : " (software fallback)");
    hardware_ = encoder_.isHardware();
    enc_open_ = true;
    enc_w_ = p.width;
    enc_h_ = p.height;
    epoch_ = epochs_.fetch_add(1) + 1; // StreamConfig epoch, like VideoToolboxEncoder.recreateSession
    config_.request();
    sps_ = std::move(sps);
    pps_ = std::move(pps);
    ctx_.clear();
    ++opens_;
    return true;
}

void TierEncoder::encodeOne(const LiveFrame& f, bool replay) {
    const LiveTierParams p = liveTierParams(opts_.tier, f.width, f.height);
    if (p.width == 0 || p.height == 0 || !f.bgra) return;

    bool reopen = false, fresh_epoch = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        reopen = !enc_open_ || p.width != enc_w_ || p.height != enc_h_;
        if (!reopen && (f.mode != enc_mode_ || f.sbs != enc_sbs_)) {
            // Same size, new mode (e.g. IR <-> Depth): keep the encoder session but start a new epoch with
            // a keyframe and a fresh config, exactly as HostController does today.
            enc_mode_ = f.mode;
            enc_sbs_ = f.sbs;
            epoch_ = epochs_.fetch_add(1) + 1;
            config_.request();
            force_next_ = true;
            fresh_epoch = true;
        }
    }
    if (reopen) {
        if (!openFor(p)) return;
        std::lock_guard<std::mutex> lk(m_);
        enc_mode_ = f.mode;
        enc_sbs_ = f.sbs;
        force_next_ = true;
        fresh_epoch = true;
    }

    if (p.same_as_high) {
        uint32_t ow = 0, oh = 0;
        bgraToNv12(f.bgra->data(), f.width, f.height, f.stride, p.upscale2x, nv12_, ow, oh);
    } else {
        // Scale the even-cropped picture high would show.
        scaler_.convert(f.bgra->data(), f.width & ~1u, f.height & ~1u, f.stride, p.width, p.height, nv12_);
    }

    bool key = false;
    uint64_t token = 0;
    int64_t pts = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto now = Clock::now();
        // Opening, a mode change and a failed encode force the IDR at once (today's force_keyframe_);
        // requests from sessions wait for the coalescing gap.
        key = force_next_ || (kf_wanted_ && (!kf_ever_ || now - last_forced_ >= opts_.keyframe_gap));
        if (key) {
            if (epoch_wanted_ && !fresh_epoch) {
                epoch_ = epochs_.fetch_add(1) + 1;
                config_.request();
            }
            force_next_ = false;
            kf_wanted_ = false;
            epoch_wanted_ = false;
            kf_ever_ = true;
            last_forced_ = now;
            ++forced_;
        }
        pts = static_cast<int64_t>(f.capture_ns / 100ULL);
        if (replay) {
            // The same picture again, later: keep encoder timestamps moving forward.
            const int64_t since = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_input_).count() / 100;
            pts = last_pts_ + std::max<int64_t>(1, since);
        } else {
            last_frame_ = f;
        }
        last_input_ = now;
        last_pts_ = pts;
        token = ++next_token_;
        if (key) {
            last_forced_token_ = token;
            awaiting_forced_ = true;
        }
        FrameCtx c;
        c.frame_seq = f.frame_seq;
        c.capture_ns = f.capture_ns;
        c.overlay = f.overlay;
        c.epoch = epoch_;
        c.mode = f.mode;
        c.sbs = f.sbs;
        c.width = p.width;
        c.height = p.height;
        ctx_[token] = c;
        while (ctx_.size() > 64) ctx_.erase(ctx_.begin());
        ++frames_in_;
    }

    std::string err;
    if (!encoder_.encode(nv12_.data(), nv12_.size(), pts, key, token, &err)) {
        std::lock_guard<std::mutex> lk(m_);
        last_error_ = "encode: " + err;
        force_next_ = true;
        // A hardware session that opened but cannot encode (GPU session limits): medium / low reopen on
        // the software MFT at the next frame. high keeps today's behaviour.
        if (opts_.tier != LiveTier::High && hardware_) {
            force_software_ = true;
            enc_w_ = enc_h_ = 0;
        }
    }
}

void TierEncoder::onEncoded(const uint8_t* annexb, size_t n, uint64_t token) {
    const uint64_t done_ns = nowNs();
    AvccAccessUnit au;
    if (!annexBToAvcc(annexb, n, au)) return;

    TierAccessUnit o;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = ctx_.find(token);
        if (it == ctx_.end()) return;
        const FrameCtx c = it->second;
        ctx_.erase(ctx_.begin(), std::next(it));
        if (!au.sps.empty()) sps_ = au.sps;
        if (!au.pps.empty()) pps_ = au.pps;
        // An async MFT reads the force-keyframe property when it gets to a frame, so two forces queued
        // close together (an open, then a mode change) can land on one frame. If the forced frame came out
        // without an IDR, force the next one (a few times at most; the GOP brings one anyway).
        if (awaiting_forced_ && token >= last_forced_token_) {
            awaiting_forced_ = false;
            if (au.idr) {
                reforces_ = 0;
            } else if (reforces_ < 3) {
                ++reforces_;
                force_next_ = true;
                cv_.notify_one();
            }
        }
        if (au.idr) reforces_ = 0;
        // A new epoch starts with an IDR: AUs of it before that IDR are dropped (a phone would drop them
        // anyway, since they would arrive before the epoch's 0x02).
        if (c.epoch != started_epoch_) {
            if (!au.idr) {
                ++frames_before_idr_;
                return;
            }
            started_epoch_ = c.epoch;
        }
        // Per epoch (v3 §6.2): this epoch's first IDR always carries the config, and a request made for
        // the current epoch is used only by an IDR of that epoch, never by an older one still in flight.
        if (au.idr && config_.onIdr(c.epoch, epoch_, !sps_.empty() && !pps_.empty())) o.config_due = true;
        o.tier = opts_.tier;
        o.epoch = c.epoch;
        o.idr = au.idr;
        o.sps = sps_;
        o.pps = pps_;
        o.width = c.width;
        o.height = c.height;
        o.mode = c.mode;
        o.sbs = c.sbs;
        o.overlay = c.overlay;
        o.frame_seq = c.frame_seq;
        o.capture_ns = c.capture_ns;
        o.encoder_name = encoder_name_;
        o.hardware = hardware_;
        ++frames_out_;
    }
    o.encode_done_ns = done_ns;
    o.avcc = std::move(au.avcc);
    if (out_) out_(o);
}

// ---- LiveTierSet ----------------------------------------------------------------------------------

LiveTierSet::LiveTierSet(const Options& opts, std::atomic<uint32_t>& epochs, TierEncoder::OutputFn on_output)
    : opts_(opts) {
    for (int i = 0; i < kLiveTierCount; ++i) {
        TierEncoder::Options o;
        o.tier = static_cast<LiveTier>(i);
        o.base = opts.encoder;
        o.hardware_allowed = opts.hardware_allowed[static_cast<size_t>(i)];
        o.keyframe_gap = opts.keyframe_gap;
        o.replay_after = opts.replay_after;
        tiers_[static_cast<size_t>(i)] = std::make_unique<TierEncoder>(o, epochs, on_output);
    }
}

LiveTierSet::~LiveTierSet() { close(); }

void LiveTierSet::want(LiveTier t) {
    std::lock_guard<std::mutex> lk(m_);
    ++wanters_[static_cast<size_t>(t)];
    tiers_[static_cast<size_t>(t)]->activate();
}

void LiveTierSet::release(LiveTier t) {
    std::lock_guard<std::mutex> lk(m_);
    int& n = wanters_[static_cast<size_t>(t)];
    if (n == 0) return;
    if (--n == 0) tiers_[static_cast<size_t>(t)]->scheduleClose(Clock::now() + opts_.hold);
}

int LiveTierSet::wanters(LiveTier t) const {
    std::lock_guard<std::mutex> lk(m_);
    return wanters_[static_cast<size_t>(t)];
}

bool LiveTierSet::active(LiveTier t) const { return tiers_[static_cast<size_t>(t)]->active(); }

bool LiveTierSet::anyActive() const {
    for (const auto& t : tiers_)
        if (t->active()) return true;
    return false;
}

std::shared_ptr<std::vector<uint8_t>> LiveTierSet::takeBuffer(size_t n) {
    std::lock_guard<std::mutex> lk(m_);
    // A pooled buffer nobody else holds any more (only this pool hands out references).
    for (auto& b : pool_) {
        if (b.use_count() == 1) {
            b->resize(n);
            return b;
        }
    }
    auto b = std::make_shared<std::vector<uint8_t>>(n);
    if (pool_.size() < 6) pool_.push_back(b);
    return b;
}

void LiveTierSet::submitFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride,
                              uint64_t frame_seq, uint64_t capture_ns, int mode, int sbs, bool overlay) {
    if (!bgra || width == 0 || height == 0 || stride < width * 4) return;
    std::array<bool, kLiveTierCount> on{};
    bool any = false;
    for (size_t i = 0; i < tiers_.size(); ++i) {
        on[i] = tiers_[i]->active();
        if (on[i]) any = true;
    }
    if (!any) return; // zero copying while nobody watches
    const size_t n = static_cast<size_t>(stride) * height;
    auto buf = takeBuffer(n);
    std::memcpy(buf->data(), bgra, n);
    LiveFrame f;
    f.bgra = buf;
    f.width = width;
    f.height = height;
    f.stride = stride;
    f.frame_seq = frame_seq;
    f.capture_ns = capture_ns;
    f.mode = mode;
    f.sbs = sbs;
    f.overlay = overlay;
    for (size_t i = 0; i < tiers_.size(); ++i)
        if (on[i]) tiers_[i]->submit(f);
}

void LiveTierSet::requestKeyframe(LiveTier t, bool with_config) {
    tiers_[static_cast<size_t>(t)]->requestKeyframe(with_config);
}

void LiveTierSet::requestKeyframeAll(bool with_config) {
    for (auto& t : tiers_)
        if (t->active()) t->requestKeyframe(with_config);
}

void LiveTierSet::requestNewEpoch(LiveTier t) { tiers_[static_cast<size_t>(t)]->requestNewEpoch(); }

TierDiagnostics LiveTierSet::diagnostics(LiveTier t) const { return tiers_[static_cast<size_t>(t)]->diagnostics(); }

void LiveTierSet::close() {
    for (auto& t : tiers_)
        if (t) t->close();
}

} // namespace kvhost
