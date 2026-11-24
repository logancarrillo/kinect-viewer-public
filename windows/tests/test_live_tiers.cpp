// Live quality tiers (protocol v3 §6.1-6.2) on synthetic frames only: the tier size table, the NV12
// scaler's colour math, per-tier encoder threads (IDR first, unique and rising epochs, low at half rate),
// keyframe coalescing, no stall of high while low opens, the hold time, the software fallback, and a
// decode of each tier's output at its size.
#include "kv_desk.hpp"
#include "kv_test.hpp"

#include "live_tiers.hpp"
#include "test_h264_decoder.hpp"
#include "video_convert.hpp"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace kvhost;
using Clock = std::chrono::steady_clock;

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Left half: flat grey 100 (a known luma after any scaling). Right half: a pattern that moves per frame.
std::vector<uint8_t> makeFrame(uint32_t w, uint32_t h, uint64_t seq) {
    std::vector<uint8_t> f(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = &f[static_cast<size_t>(y) * w * 4];
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = row + static_cast<size_t>(x) * 4;
            if (x < w / 2) {
                p[0] = p[1] = p[2] = 100;
            } else {
                p[0] = static_cast<uint8_t>((x + seq * 5) & 0xFF);
                p[1] = static_cast<uint8_t>((y * 2 + seq) & 0xFF);
                p[2] = static_cast<uint8_t>(((x ^ y) + seq * 3) & 0xFF);
            }
            p[3] = 255;
        }
    }
    return f;
}

struct Got {
    TierAccessUnit au;
    uint64_t arrive_ns = 0;
};

struct Collector {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Got> outs;
    void push(const TierAccessUnit& au) {
        std::lock_guard<std::mutex> lk(m);
        outs.push_back({au, nowNs()});
        cv.notify_all();
    }
    std::vector<Got> of(LiveTier t) {
        std::lock_guard<std::mutex> lk(m);
        std::vector<Got> r;
        for (auto& g : outs)
            if (g.au.tier == t) r.push_back(g);
        return r;
    }
    size_t count(LiveTier t) { return of(t).size(); }
    bool waitFor(LiveTier t, size_t n, int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] {
            size_t c = 0;
            for (auto& g : outs)
                if (g.au.tier == t) ++c;
            return c >= n;
        });
    }
};

// Feeds `frames` synthetic frames at about 30 fps. `between(i)` runs before frame i.
template <typename F>
void feed(LiveTierSet& set, uint32_t w, uint32_t h, int frames, uint64_t& seq, int mode, F between) {
    for (int i = 0; i < frames; ++i) {
        between(i);
        const auto t0 = Clock::now();
        const auto f = makeFrame(w, h, seq);
        set.submitFrame(f.data(), w, h, w * 4, seq, nowNs(), mode, 0, false);
        ++seq;
        const auto spent = Clock::now() - t0;
        const auto left = std::chrono::milliseconds(33) - spent;
        if (left > Clock::duration::zero()) std::this_thread::sleep_for(left);
    }
}
void feed(LiveTierSet& set, uint32_t w, uint32_t h, int frames, uint64_t& seq, int mode = 0) {
    feed(set, w, h, frames, seq, mode, [](int) {});
}

// Feeds frames at ~30 fps until done() (checked after each frame) or about 5 s. A hardware encoder can
// take a few hundred ms to open, more right after another session closed, so tests wait on results.
template <typename P>
bool feedUntil(LiveTierSet& set, uint32_t w, uint32_t h, uint64_t& seq, int mode, P done) {
    for (int i = 0; i < 150; ++i) {
        feed(set, w, h, 1, seq, mode);
        if (done()) return true;
    }
    return false;
}

struct DecodeResult {
    int decoded = 0;
    uint32_t w = 0, h = 0;
    double left_luma = 0; // mean luma over the flat grey half (well inside it)
};

// Decodes the first `n` AUs of one tier exactly as a phone would: AVCC + the SPS/PPS of its 0x02.
DecodeResult decodeTier(const std::vector<Got>& outs, size_t n) {
    DecodeResult r;
    kvtest::TestH264Decoder dec;
    if (!dec.open()) return r;
    for (size_t k = 0; k < outs.size() && k < n; ++k) {
        const TierAccessUnit& au = outs[k].au;
        const auto annexb = kvtest::avccToAnnexB(au.avcc.data(), au.avcc.size(),
                                                  au.idr ? std::vector<std::vector<uint8_t>>{au.sps, au.pps}
                                                         : std::vector<std::vector<uint8_t>>{});
        if (dec.decode(annexb.data(), annexb.size())) {
            ++r.decoded;
            r.w = dec.width();
            r.h = dec.height();
            const auto& y = dec.luma();
            const uint32_t x0 = au.width / 8, x1 = au.width * 3 / 8, y0 = au.height / 4, y1 = au.height * 3 / 4;
            double sum = 0;
            size_t cnt = 0;
            for (uint32_t yy = y0; yy < y1; ++yy)
                for (uint32_t xx = x0; xx < x1; ++xx) {
                    sum += y[static_cast<size_t>(yy) * dec.width() + xx];
                    ++cnt;
                }
            r.left_luma = cnt ? sum / cnt : 0;
        }
    }
    return r;
}

// The decoder may report a 16-aligned height (1080 -> 1088); the width is exact.
bool decodedSizeOk(const DecodeResult& r, uint32_t w, uint32_t h) {
    return r.w == w && r.h >= h && r.h <= ((h + 15) & ~15u);
}

uint8_t expectedLumaOfGrey(int v) { return static_cast<uint8_t>(((220 * v + 128) >> 8) + 16); }

} // namespace

// ---- sizes and parameters -----------------------------------------------------------------------

KV_TEST(tier_size_table) {
    struct Row {
        uint32_t sw, sh;
        uint32_t hw, hh, mw, mh, lw, lh;
        uint32_t high_bitrate;
    };
    const Row rows[] = {
        {1920, 1080, 1920, 1080, 1280, 720, 640, 360, 10000000u},
        {512, 424, 1024, 848, 1024, 848, 640, 530, 5000000u},
        {1024, 424, 1024, 424, 1024, 424, 640, 266, 5000000u},
        {1280, 424, 1280, 424, 1280, 424, 640, 212, 5000000u},
        {960, 720, 960, 720, 960, 720, 640, 480, 5000000u},
        {1281, 425, 1280, 424, 1280, 424, 640, 212, 5000000u},
        {1080, 1920, 1080, 1920, 720, 1280, 360, 640, 5000000u},
    };
    for (const Row& r : rows) {
        const std::string tag = std::to_string(r.sw) + "x" + std::to_string(r.sh);
        const LiveTierParams hi = liveTierParams(LiveTier::High, r.sw, r.sh);
        const LiveTierParams me = liveTierParams(LiveTier::Medium, r.sw, r.sh);
        const LiveTierParams lo = liveTierParams(LiveTier::Low, r.sw, r.sh);
        KV_CHECK_MSG(hi.width == r.hw && hi.height == r.hh, tag);
        KV_CHECK_MSG(me.width == r.mw && me.height == r.mh, tag);
        KV_CHECK_MSG(lo.width == r.lw && lo.height == r.lh, tag + " low " + std::to_string(lo.width) + "x" + std::to_string(lo.height));
        KV_CHECK_MSG(hi.bitrate == r.high_bitrate && hi.fps == 30 && hi.gop == 30 && hi.frame_divisor == 1, tag);
        KV_CHECK_MSG(me.bitrate == 2500000u && me.fps == 30 && me.gop == 30 && me.frame_divisor == 1, tag);
        KV_CHECK_MSG(lo.bitrate == 800000u && lo.fps == 15 && lo.gop == 15 && lo.frame_divisor == 2, tag);
        for (const auto* p : {&hi, &me, &lo}) KV_CHECK_MSG(p->width % 2 == 0 && p->height % 2 == 0, tag);
        KV_CHECK(hi.same_as_high);
        KV_CHECK_MSG(me.same_as_high == (me.width == hi.width && me.height == hi.height), tag);
    }
    // 512x424 high and medium are made exactly like today's stream: 2x nearest.
    KV_CHECK(liveTierParams(LiveTier::High, 512, 424).upscale2x);
    KV_CHECK(liveTierParams(LiveTier::Medium, 512, 424).upscale2x);
    KV_CHECK(!liveTierParams(LiveTier::Low, 512, 424).upscale2x);
    KV_CHECK(std::string(liveTierName(LiveTier::High)) == "high");
    KV_CHECK(std::string(liveTierName(LiveTier::Medium)) == "medium");
    KV_CHECK(std::string(liveTierName(LiveTier::Low)) == "low");
}

// ---- the scaler ---------------------------------------------------------------------------------

KV_TEST(scaler_identity_matches_bgra_to_nv12_bit_for_bit) {
    const uint32_t w = 66, h = 40;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 4);
    uint32_t s = 12345;
    for (auto& b : src) {
        s = s * 1103515245u + 12345u;
        b = static_cast<uint8_t>(s >> 16);
    }
    std::vector<uint8_t> a, b;
    uint32_t ow = 0, oh = 0;
    bgraToNv12(src.data(), w, h, w * 4, false, a, ow, oh);
    scaleBgraToNv12(src.data(), w, h, w * 4, w, h, b);
    KV_CHECK(ow == w && oh == h);
    KV_CHECK(a == b);
}

KV_TEST(scaler_flat_colours_are_exact) {
    const uint8_t colours[][3] = {{255, 255, 255}, {0, 0, 0}, {255, 0, 0}, {0, 255, 0},
                                  {0, 0, 255},     {128, 128, 128}, {200, 40, 90}, {13, 177, 250}};
    const uint32_t cases[][4] = {{1920, 1080, 1280, 720}, {1920, 1080, 640, 360}, {512, 424, 640, 530},
                                 {1024, 848, 640, 530},   {100, 60, 30, 18}};
    for (const auto& c : colours) {
        // Reference: today's converter on a small flat frame.
        std::vector<uint8_t> tiny(4 * 2 * 4), ref;
        for (size_t i = 0; i < tiny.size(); i += 4) {
            tiny[i] = c[2];
            tiny[i + 1] = c[1];
            tiny[i + 2] = c[0];
            tiny[i + 3] = 255;
        }
        uint32_t rw = 0, rh = 0;
        bgraToNv12(tiny.data(), 4, 2, 16, false, ref, rw, rh);
        const uint8_t Y = ref[0], U = ref[8], V = ref[9];
        Nv12Scaler scaler;
        for (const auto& k : cases) {
            std::vector<uint8_t> src(static_cast<size_t>(k[0]) * k[1] * 4), out;
            for (size_t i = 0; i < src.size(); i += 4) {
                src[i] = c[2];
                src[i + 1] = c[1];
                src[i + 2] = c[0];
                src[i + 3] = 255;
            }
            scaler.convert(src.data(), k[0], k[1], k[0] * 4, k[2], k[3], out);
            const size_t yn = static_cast<size_t>(k[2]) * k[3];
            KV_REQUIRE(out.size() == yn * 3 / 2);
            bool ok = true;
            for (size_t i = 0; i < yn && ok; ++i) ok = out[i] == Y;
            for (size_t i = yn; i < out.size() && ok; i += 2) ok = out[i] == U && out[i + 1] == V;
            KV_CHECK_MSG(ok, std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]) + " " +
                                 std::to_string(k[0]) + "x" + std::to_string(k[1]) + "->" + std::to_string(k[2]) + "x" +
                                 std::to_string(k[3]));
        }
    }
}

KV_TEST(scaler_area_average_on_gradients) {
    // Horizontal grey ramp, 1920 -> 640: each output pixel is the mean of 3 source pixels.
    const uint32_t sw = 1920, sh = 4;
    std::vector<uint8_t> src(static_cast<size_t>(sw) * sh * 4), out;
    auto grey = [](uint32_t x) { return static_cast<int>(x * 255 / 1919); };
    for (uint32_t y = 0; y < sh; ++y)
        for (uint32_t x = 0; x < sw; ++x) {
            uint8_t* p = &src[(static_cast<size_t>(y) * sw + x) * 4];
            p[0] = p[1] = p[2] = static_cast<uint8_t>(grey(x));
            p[3] = 255;
        }
    scaleBgraToNv12(src.data(), sw, sh, sw * 4, 640, 2, out);
    int worst = 0;
    for (uint32_t x = 0; x < 640; ++x) {
        const double mean = (grey(3 * x) + grey(3 * x + 1) + grey(3 * x + 2)) / 3.0;
        const int want = expectedLumaOfGrey(static_cast<int>(std::lround(mean)));
        worst = std::max(worst, std::abs(static_cast<int>(out[x]) - want));
    }
    KV_CHECK_MSG(worst <= 1, "worst luma error " + std::to_string(worst));

    // Vertical ramp, 1080 -> 360 rows (the vertical pass).
    const uint32_t vw = 8, vh = 1080;
    std::vector<uint8_t> vsrc(static_cast<size_t>(vw) * vh * 4);
    auto vgrey = [](uint32_t y) { return static_cast<int>(y * 255 / 1079); };
    for (uint32_t y = 0; y < vh; ++y)
        for (uint32_t x = 0; x < vw; ++x) {
            uint8_t* p = &vsrc[(static_cast<size_t>(y) * vw + x) * 4];
            p[0] = p[1] = p[2] = static_cast<uint8_t>(vgrey(y));
            p[3] = 255;
        }
    scaleBgraToNv12(vsrc.data(), vw, vh, vw * 4, 4, 360, out);
    worst = 0;
    for (uint32_t y = 0; y < 360; ++y) {
        const double mean = (vgrey(3 * y) + vgrey(3 * y + 1) + vgrey(3 * y + 2)) / 3.0;
        const int want = expectedLumaOfGrey(static_cast<int>(std::lround(mean)));
        worst = std::max(worst, std::abs(static_cast<int>(out[static_cast<size_t>(y) * 4]) - want));
    }
    KV_CHECK_MSG(worst <= 1, "worst vertical luma error " + std::to_string(worst));

    // Non-integer ratio 1920 -> 1280 (1.5 source pixels per output): monotonic, ends kept.
    scaleBgraToNv12(src.data(), sw, sh, sw * 4, 1280, 2, out);
    bool mono = true;
    for (uint32_t x = 1; x < 1280; ++x) mono = mono && out[x] >= out[x - 1];
    KV_CHECK(mono);
    KV_CHECK(out[0] <= 17 && out[1279] >= 234);

    // Left half red, right half blue, 1920 -> 640: away from the edge the chroma is exact.
    std::vector<uint8_t> rb(static_cast<size_t>(sw) * 2 * 4), ref;
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < sw; ++x) {
            uint8_t* p = &rb[(static_cast<size_t>(y) * sw + x) * 4];
            p[0] = x < sw / 2 ? 0 : 255;
            p[1] = 0;
            p[2] = x < sw / 2 ? 255 : 0;
            p[3] = 255;
        }
    uint32_t rw = 0, rh = 0;
    bgraToNv12(rb.data(), sw, 2, sw * 4, false, ref, rw, rh);
    scaleBgraToNv12(rb.data(), sw, 2, sw * 4, 640, 2, out);
    KV_CHECK(out[10] == ref[10] && out[630] == ref[1900]);         // luma
    KV_CHECK(out[1280 + 10] == ref[3840 + 10] && out[1280 + 11] == ref[3840 + 11]);     // red Cb Cr
    KV_CHECK(out[1280 + 630] == ref[3840 + 1900] && out[1280 + 631] == ref[3840 + 1901]); // blue Cb Cr
}

KV_TEST(scaler_bilinear_when_enlarging) {
    // 512 -> 640 (the IR low tier): a ramp stays a ramp, within one step of the exact bilinear value.
    const uint32_t sw = 512, sh = 2;
    std::vector<uint8_t> src(static_cast<size_t>(sw) * sh * 4), out;
    for (uint32_t y = 0; y < sh; ++y)
        for (uint32_t x = 0; x < sw; ++x) {
            uint8_t* p = &src[(static_cast<size_t>(y) * sw + x) * 4];
            p[0] = p[1] = p[2] = static_cast<uint8_t>(x / 2);
            p[3] = 255;
        }
    scaleBgraToNv12(src.data(), sw, sh, sw * 4, 640, 2, out);
    int worst = 0;
    bool mono = true;
    for (uint32_t x = 0; x < 640; ++x) {
        const double c = std::clamp((x + 0.5) * 512.0 / 640.0 - 0.5, 0.0, 511.0);
        const uint32_t j = static_cast<uint32_t>(c);
        const double f = c - j;
        const double v = (j / 2) * (1 - f) + (std::min<uint32_t>(j + 1, 511) / 2) * f;
        worst = std::max(worst, std::abs(static_cast<int>(out[x]) - expectedLumaOfGrey(static_cast<int>(std::lround(v)))));
        if (x) mono = mono && out[x] >= out[x - 1];
    }
    KV_CHECK_MSG(worst <= 1, "worst luma error " + std::to_string(worst));
    KV_CHECK(mono);
}

KV_TEST(scaler_output_sizes) {
    std::vector<uint8_t> src(64 * 48 * 4, 255), out;
    scaleBgraToNv12(src.data(), 64, 48, 64 * 4, 33, 21, out); // odd sizes round down to even
    KV_CHECK(out.size() == 32u * 20u * 3u / 2u);
    scaleBgraToNv12(src.data(), 64, 48, 64 * 4, 1, 1, out); // never below 2x2
    KV_CHECK(out.size() == 2u * 2u * 3u / 2u);
    // A padded stride is honoured: the padding bytes (0) must not leak in.
    const uint32_t stride = 64 * 4 + 32;
    std::vector<uint8_t> padded(static_cast<size_t>(stride) * 48, 0);
    for (uint32_t y = 0; y < 48; ++y) std::fill_n(&padded[static_cast<size_t>(y) * stride], 64 * 4, uint8_t{255});
    scaleBgraToNv12(padded.data(), 64, 48, stride, 32, 24, out);
    KV_CHECK(std::all_of(out.begin(), out.begin() + 32 * 24, [](uint8_t v) { return v == 235; }));
}

// ---- encoders -----------------------------------------------------------------------------------

KV_TEST(high_and_low_together_idr_first_distinct_epochs_half_rate_and_decode) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{0};
    Collector col;
    LiveTierSet::Options o;
    LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
    set.want(LiveTier::High);
    set.want(LiveTier::Low);
    KV_CHECK(set.active(LiveTier::High) && set.active(LiveTier::Low) && !set.active(LiveTier::Medium));
    uint64_t seq = 1;
    feed(set, 1920, 1080, 90, seq);
    // More frames until high has 60: the frames that arrive while the hardware encoder opens are replaced, and that
    // open can take a second on a busy GPU (58 out of 90 in one run), so a fixed 90 raced it.
    feedUntil(set, 1920, 1080, seq, 0, [&] { return col.count(LiveTier::High) >= 60; });
    col.waitFor(LiveTier::High, 60, 2000);
    sleepMs(300);
    const auto hi = col.of(LiveTier::High);
    const auto lo = col.of(LiveTier::Low);
    const auto dh = set.diagnostics(LiveTier::High), dl = set.diagnostics(LiveTier::Low);
    std::printf("  high: %s, in=%llu out=%zu replaced=%llu | low: %s, in=%llu out=%zu skipped=%llu\n", dh.encoder_name.c_str(),
                static_cast<unsigned long long>(dh.frames_in), hi.size(), static_cast<unsigned long long>(dh.frames_replaced), dl.encoder_name.c_str(),
                static_cast<unsigned long long>(dl.frames_in), lo.size(),
                static_cast<unsigned long long>(dl.frames_skipped));
    // Frames that arrive while a hardware encoder opens (a few hundred ms) are replaced, latest wins.
    KV_REQUIRE(hi.size() >= 60 && !lo.empty());
    KV_CHECK_MSG(hi[0].au.idr && hi[0].au.config_due, "high starts with an IDR and its config");
    KV_CHECK_MSG(lo[0].au.idr && lo[0].au.config_due, "low starts with an IDR and its config");
    KV_CHECK(hi[0].au.epoch != lo[0].au.epoch && hi[0].au.epoch > 0 && lo[0].au.epoch > 0);
    KV_CHECK(!hi[0].au.sps.empty() && !hi[0].au.pps.empty() && !lo[0].au.sps.empty());
    // One epoch per tier for the whole run (no size or mode change).
    for (const auto& g : hi) KV_CHECK(g.au.epoch == hi[0].au.epoch);
    for (const auto& g : lo) KV_CHECK(g.au.epoch == lo[0].au.epoch);
    // Exactly one config_due per tier here: the first IDR.
    KV_CHECK(std::count_if(hi.begin(), hi.end(), [](const Got& g) { return g.au.config_due; }) == 1);
    const double ratio = static_cast<double>(lo.size()) / hi.size();
    std::printf("  low/high output ratio %.2f\n", ratio);
    KV_CHECK_MSG(ratio > 0.40 && ratio < 0.60, std::to_string(ratio));
    KV_CHECK(hi[0].au.width == 1920 && hi[0].au.height == 1080 && lo[0].au.width == 640 && lo[0].au.height == 360);
    KV_CHECK(hi[0].au.encoder_name == dh.encoder_name && dh.width == 1920 && dl.width == 640);
    // Real capture timestamps pass through, and the encode-done time is after them.
    for (const auto& g : lo) KV_CHECK(g.au.encode_done_ns >= g.au.capture_ns && g.au.capture_ns > 0);
    // low gets every second frame: once open, its frame sequence numbers step by 2 (more only while the
    // encoder was opening and the latest-wins slot replaced frames).
    if (lo.size() > 10) {
        std::vector<uint64_t> steps;
        for (size_t k = 1; k < lo.size(); ++k) steps.push_back(lo[k].au.frame_seq - lo[k - 1].au.frame_seq);
        std::sort(steps.begin(), steps.end());
        KV_CHECK_MSG(steps[steps.size() / 2] == 2, std::to_string(steps[steps.size() / 2]));
        KV_CHECK(steps.front() >= 2);
    }

    // The wire helpers build today's 0x02 and 0x03 from an AU.
    const StreamConfigMessage cfg = streamConfigFor(hi[0].au);
    KV_CHECK(cfg.streamEpoch == hi[0].au.epoch && cfg.width == 1920 && cfg.height == 1080 &&
             cfg.parameterSets.size() == 2 && cfg.nalLengthHeaderBytes == 4);
    std::vector<uint8_t> payload;
    KV_CHECK(videoPayloadFor(hi[0].au, payload) == 0x01);
    KV_CHECK(payload.size() == VideoAccessUnitHeader::kBinarySize + hi[0].au.avcc.size());

    // Valid H.264 at the right size, and the flat grey half survives the scaling.
    const DecodeResult rh = decodeTier(hi, 12), rl = decodeTier(lo, 12);
    std::printf("  decoded high %d (%ux%u, grey %.1f) low %d (%ux%u, grey %.1f), expected grey %d\n", rh.decoded, rh.w, rh.h,
                rh.left_luma, rl.decoded, rl.w, rl.h, rl.left_luma, expectedLumaOfGrey(100));
    KV_CHECK(rh.decoded >= 10 && rl.decoded >= 10);
    KV_CHECK(decodedSizeOk(rh, 1920, 1080) && decodedSizeOk(rl, 640, 360));
    KV_CHECK(std::abs(rh.left_luma - expectedLumaOfGrey(100)) < 6 && std::abs(rl.left_luma - expectedLumaOfGrey(100)) < 6);
    set.close();
}

KV_TEST(every_tier_decodes_at_its_size_ir_source) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{100};
    Collector col;
    LiveTierSet::Options o;
    LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
    for (int i = 0; i < kLiveTierCount; ++i) set.want(static_cast<LiveTier>(i));
    uint64_t seq = 1;
    KV_CHECK(feedUntil(set, 512, 424, seq, 2, [&] {
        return col.count(LiveTier::High) >= 12 && col.count(LiveTier::Medium) >= 12 && col.count(LiveTier::Low) >= 12;
    }));
    const uint32_t sizes[3][2] = {{1024, 848}, {1024, 848}, {640, 530}};
    std::set<uint32_t> seen;
    for (int i = 0; i < kLiveTierCount; ++i) {
        const LiveTier t = static_cast<LiveTier>(i);
        const auto outs = col.of(t);
        const auto d = set.diagnostics(t);
        std::printf("  %s: %s out=%zu\n", liveTierName(t), d.encoder_name.c_str(), outs.size());
        KV_REQUIRE(outs.size() >= 10);
        KV_CHECK(outs[0].au.idr && outs[0].au.mode == 2);
        KV_CHECK(outs[0].au.epoch > 100);
        KV_CHECK_MSG(seen.insert(outs[0].au.epoch).second, "epochs are unique across tiers");
        const DecodeResult r = decodeTier(outs, 10);
        KV_CHECK_MSG(r.decoded >= 8 && decodedSizeOk(r, sizes[i][0], sizes[i][1]),
                     std::string(liveTierName(t)) + " " + std::to_string(r.w) + "x" + std::to_string(r.h));
        KV_CHECK(std::abs(r.left_luma - expectedLumaOfGrey(100)) < 6);
    }
    set.close();
}

KV_TEST(epochs_strictly_increase_across_reopens_modes_and_tiers) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{0};
    Collector col;
    LiveTierSet::Options o;
    LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
    uint64_t seq = 1;
    std::vector<uint32_t> order; // each new epoch as it first appears, in time order
    auto latest = [&](LiveTier t) {
        const auto v = col.of(t);
        return v.empty() ? 0u : v.back().au.epoch;
    };
    // Feeds until tier t shows an epoch above every epoch recorded so far, then records it.
    auto step = [&](LiveTier t, uint32_t w, uint32_t h, int mode, const char* what) {
        const uint32_t floor = order.empty() ? 0u : order.back();
        const bool ok = feedUntil(set, w, h, seq, mode, [&] { return latest(t) > floor; });
        KV_CHECK_MSG(ok, what);
        order.push_back(latest(t));
    };

    set.want(LiveTier::High);
    step(LiveTier::High, 1280, 720, 0, "first open");
    step(LiveTier::High, 1024, 424, 0, "size change: reopen");
    step(LiveTier::High, 1024, 424, 1, "same size, new mode: new epoch without reopening");
    KV_CHECK(set.diagnostics(LiveTier::High).opens == 2);
    set.want(LiveTier::Low);
    step(LiveTier::Low, 1024, 424, 1, "low opens while high runs");
    set.requestNewEpoch(LiveTier::High);
    step(LiveTier::High, 1024, 424, 1, "requested new epoch");
    set.want(LiveTier::Medium);
    step(LiveTier::Medium, 1920, 1080, 3, "every tier changes size at once; medium opens");
    // High and low reopen at the new size too.
    KV_CHECK(feedUntil(set, 1920, 1080, seq, 3, [&] { return latest(LiveTier::High) > order[4] && latest(LiveTier::Low) > order[3]; }));
    feed(set, 1920, 1080, 5, seq, 3);
    sleepMs(200);

    std::printf("  epochs in order:");
    for (uint32_t e : order) std::printf(" %u", e);
    std::printf("\n");
    KV_CHECK(order.size() == 6);
    for (size_t i = 1; i < order.size(); ++i) KV_CHECK(order[i] > order[i - 1]);

    // Across everything: per tier, epochs never go down; no epoch is used by two tiers; each new epoch
    // starts with an IDR that carries config_due.
    set.close(); // before reading: no callback runs (or waits on col.m) after this
    std::map<uint32_t, LiveTier> owner;
    std::array<uint32_t, kLiveTierCount> last{0, 0, 0};
    for (const auto& g : col.outs) {
        const size_t ti = static_cast<size_t>(g.au.tier);
        KV_CHECK(g.au.epoch >= last[ti]);
        if (g.au.epoch != last[ti]) KV_CHECK_MSG(g.au.idr && g.au.config_due, "epoch " + std::to_string(g.au.epoch));
        last[ti] = g.au.epoch;
        auto it = owner.find(g.au.epoch);
        if (it == owner.end()) owner[g.au.epoch] = g.au.tier;
        else KV_CHECK(it->second == g.au.tier);
    }
    KV_CHECK(epochs.load() == std::max({last[0], last[1], last[2]}));
    for (int i = 0; i < kLiveTierCount; ++i)
        std::printf("  %s: %llu AU(s) dropped before an epoch's IDR\n", liveTierName(static_cast<LiveTier>(i)),
                    static_cast<unsigned long long>(set.diagnostics(static_cast<LiveTier>(i)).frames_before_idr));
}

// v3 §6.2, the in-flight race: with an async (hardware) MFT a few frames are inside the encoder when a
// new epoch starts. An IDR of the old epoch that comes out after that (a GOP IDR, or an earlier forced
// one) must neither take the new epoch's config nor use it up: the new epoch's first IDR always
// carries config_due. The gate is what TierEncoder::onEncoded asks, in the order AUs come out.
KV_TEST(config_due_is_per_epoch_and_an_old_epochs_idr_in_flight_cant_use_it_up) {
    TierConfigGate g; // a tier that just opened (epoch 5)
    KV_CHECK(!g.onIdr(5, 5, /*params_known=*/false)); // no SPS/PPS yet: nothing to send
    KV_CHECK(g.onIdr(5, 5, true));                     // the open's first IDR
    KV_CHECK(!g.onIdr(5, 5, true));                    // a GOP IDR of the same epoch: no new 0x02
    // A mode change (or requestNewEpoch's forced key) starts epoch 6 at encode time...
    g.request();
    // ...while a GOP IDR of epoch 5 is still in the encoder: it comes out first.
    KV_CHECK_MSG(!g.onIdr(5, 6, true), "an old epoch's IDR took the new epoch's config");
    KV_CHECK_MSG(g.onIdr(6, 6, true), "the new epoch's first IDR lost its config");
    KV_CHECK(!g.onIdr(6, 6, true));
    // Two epochs started back to back, before either IDR came out: each first IDR gets its own.
    g.request();
    g.request();
    KV_CHECK(g.onIdr(7, 8, true)); // epoch 7 was never configured: its IDR still gets one
    KV_CHECK(g.onIdr(8, 8, true));
    KV_CHECK(!g.onIdr(8, 8, true));
    // A keyframe-with-config request within the current epoch (requestKeyframe(true)) is used once.
    g.request();
    KV_CHECK(g.onIdr(8, 8, true));
    KV_CHECK(!g.onIdr(8, 8, true));
}

KV_TEST(keyframe_requests_coalesce_to_one_per_500ms_and_reach_a_frozen_view) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{0};
    Collector col;
    LiveTierSet::Options o;
    o.encoder.gop = 600; // no natural IDRs in this test: every IDR here is a forced one
    LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
    set.want(LiveTier::High);
    uint64_t seq = 1;
    // Well past the opening IDR before the burst.
    KV_CHECK(feedUntil(set, 1280, 720, seq, 0, [&] { return col.count(LiveTier::High) >= 20; }));
    const uint64_t forced0 = set.diagnostics(LiveTier::High).forced_keyframes;
    const uint64_t burst_ns = nowNs();
    // 60 requests over ~200 ms (two per frame), then ~1.3 s of frames.
    feed(set, 1280, 720, 45, seq, 0, [&](int i) {
        if (i < 6)
            for (int k = 0; k < 10; ++k) set.requestKeyframe(LiveTier::High);
    });
    sleepMs(200);
    const uint64_t forced = set.diagnostics(LiveTier::High).forced_keyframes - forced0;
    std::vector<uint64_t> idr_ns;
    for (const auto& g : col.of(LiveTier::High))
        if (g.au.idr && g.arrive_ns >= burst_ns) idr_ns.push_back(g.au.encode_done_ns);
    std::printf("  forced IDRs for 60 requests: %llu; output IDRs:", static_cast<unsigned long long>(forced));
    for (uint64_t t : idr_ns) std::printf(" +%.0f ms", (t - burst_ns) / 1e6);
    std::printf("\n");
    // The first request is served at once; the other 59 merge into one IDR 500 ms later.
    KV_CHECK(forced == 2);
    KV_CHECK(idr_ns.size() == 2);
    if (idr_ns.size() == 2) KV_CHECK((idr_ns[1] - idr_ns[0]) / 1e6 >= 450.0);

    // Frozen view: no new frames, a session asks for an IDR -> the last frame is encoded again as one.
    sleepMs(300);
    const size_t before = col.count(LiveTier::High);
    set.requestKeyframe(LiveTier::High);
    KV_CHECK_MSG(col.waitFor(LiveTier::High, before + 1, 1500), "replayed IDR while frozen");
    const auto all = col.of(LiveTier::High);
    KV_CHECK(all.size() > before && all[before].au.idr);
    set.close();
}

KV_TEST(opening_low_does_not_stall_high) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{0};
    Collector col;
    LiveTierSet::Options o;
    LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
    set.want(LiveTier::High);
    uint64_t seq = 1;
    feed(set, 1920, 1080, 30, seq);
    uint64_t want_ns = 0, max_submit_ns = 0;
    feed(set, 1920, 1080, 60, seq, 0, [&](int i) {
        if (i == 0) {
            want_ns = nowNs();
            set.want(LiveTier::Low);
        }
    });
    // The submitter itself never blocks on an encoder.
    for (int i = 0; i < 10; ++i) {
        const auto f = makeFrame(1920, 1080, seq);
        const uint64_t t0 = nowNs();
        set.submitFrame(f.data(), 1920, 1080, 1920 * 4, seq++, nowNs(), 0, 0, false);
        max_submit_ns = std::max(max_submit_ns, nowNs() - t0);
        sleepMs(33);
    }
    sleepMs(300);
    const auto hi = col.of(LiveTier::High);
    const auto lo = col.of(LiveTier::Low);
    KV_REQUIRE(!lo.empty());
    const double low_open_ms = (lo[0].arrive_ns - want_ns) / 1e6;
    double max_gap = 0;
    for (size_t k = 1; k < hi.size(); ++k) {
        if (hi[k].arrive_ns < want_ns || hi[k - 1].arrive_ns > lo[0].arrive_ns + 500000000ULL) continue;
        max_gap = std::max(max_gap, (hi[k].arrive_ns - hi[k - 1].arrive_ns) / 1e6);
    }
    std::printf("  low (%s) first AU %.0f ms after want; high's largest gap meanwhile %.0f ms; submit max %.2f ms\n",
                set.diagnostics(LiveTier::Low).encoder_name.c_str(), low_open_ms, max_gap, max_submit_ns / 1e6);
    KV_CHECK_MSG(max_gap < 150.0, std::to_string(max_gap));
    KV_CHECK(max_submit_ns < 30000000ULL);
    set.close();
}

KV_TEST(hold_time_closes_an_unwanted_tier) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{0};
    Collector col;
    LiveTierSet::Options o;
    o.hold = std::chrono::milliseconds(400);
    LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
    uint64_t seq = 1;
    set.want(LiveTier::Medium);
    set.want(LiveTier::Medium); // two users
    KV_CHECK(feedUntil(set, 1920, 1080, seq, 0, [&] { return col.count(LiveTier::Medium) >= 10; }));
    KV_CHECK(set.diagnostics(LiveTier::Medium).open && set.wanters(LiveTier::Medium) == 2);
    KV_CHECK(set.diagnostics(LiveTier::Medium).width == 1280 && set.diagnostics(LiveTier::Medium).height == 720);
    // Medium at 1280x720 decodes.
    const DecodeResult r = decodeTier(col.of(LiveTier::Medium), 8);
    std::printf("  medium decoded %d at %ux%u\n", r.decoded, r.w, r.h);
    KV_CHECK(r.decoded >= 6 && decodedSizeOk(r, 1280, 720));

    set.release(LiveTier::Medium);
    feed(set, 1920, 1080, 15, seq); // still one user: stays open
    KV_CHECK(set.active(LiveTier::Medium) && set.diagnostics(LiveTier::Medium).open);

    // Last user leaves, comes back inside the hold: the same encoder and epoch carry on.
    const uint64_t opens = set.diagnostics(LiveTier::Medium).opens;
    const uint32_t epoch = set.diagnostics(LiveTier::Medium).epoch;
    set.release(LiveTier::Medium);
    feed(set, 1920, 1080, 6, seq);
    set.want(LiveTier::Medium);
    feed(set, 1920, 1080, 15, seq); // past the original hold deadline
    KV_CHECK(set.active(LiveTier::Medium) && set.diagnostics(LiveTier::Medium).opens == opens);
    KV_CHECK(set.diagnostics(LiveTier::Medium).epoch == epoch);

    // Released for good: open during the hold, closed after it; frames then go nowhere. The encoder closing at all,
    // within 5 s of the deadline, and the tier inactive by then are checked in every run. How soon after the deadline
    // the encoder has closed is a timing floor, judged against the desk: closing joins the hardware MFT's event thread
    // (TierEncoder::closeEncoder), which a busy PC can stretch with nothing wrong in the hold.
    kvtest::DeskSampler desk;
    set.release(LiveTier::Medium);
    const uint64_t deadline_ns = nowNs() + 400000000ULL; // o.hold
    feed(set, 1920, 1080, 6, seq); // ~200 ms
    KV_CHECK(set.active(LiveTier::Medium) && set.diagnostics(LiveTier::Medium).open);
    const bool closed = feedUntil(set, 1920, 1080, seq, 0, [&] {
        const auto d = set.diagnostics(LiveTier::Medium);
        return !d.open && d.encoder_name.empty();
    });
    const double close_ms = (static_cast<double>(nowNs()) - static_cast<double>(deadline_ns)) / 1e6;
    const kvtest::DeskSnapshot timed = desk.snapshot();
    std::printf("  closed %.0f ms after the hold deadline; %s\n", close_ms, timed.r.line().c_str());
    KV_CHECK_MSG(closed, "the encoder hadn't closed 5 s after the hold deadline");
    KV_CHECK(!set.diagnostics(LiveTier::Medium).active);
    KV_CHECK(!set.anyActive());
    KV_FLOOR(close_ms < 300.0, timed, "the encoder closed " + std::to_string(close_ms) + " ms after the hold deadline");
    const size_t n = col.count(LiveTier::Medium);
    feed(set, 1920, 1080, 6, seq);
    sleepMs(100);
    KV_CHECK(col.count(LiveTier::Medium) == n);

    // Wanted again: reopens with a new, higher epoch and an IDR first.
    set.want(LiveTier::Medium);
    KV_CHECK(feedUntil(set, 1920, 1080, seq, 0, [&] { return col.count(LiveTier::Medium) > n; }));
    const auto again = col.of(LiveTier::Medium);
    KV_REQUIRE(again.size() > n);
    KV_CHECK(again[n].au.idr && again[n].au.config_due && again[n].au.epoch > epoch);
    set.close();
}

KV_TEST(medium_and_low_fall_back_to_software_alone) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{0};
    Collector col;
    LiveTierSet::Options o;
    o.hardware_allowed = {true, true, false}; // as if the GPU had no session left for low
    LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
    set.want(LiveTier::High);
    set.want(LiveTier::Low);
    uint64_t seq = 1;
    // Until high is open too, not just until low has 10 AUs: the hardware open can take longer than 10 software
    // frames (~0.33 s) on a busy GPU, and the old wait checked high before it had opened.
    KV_CHECK(feedUntil(set, 1920, 1080, seq, 0, [&] {
        return col.count(LiveTier::Low) >= 10 && set.diagnostics(LiveTier::High).open;
    }));
    const auto dh = set.diagnostics(LiveTier::High), dl = set.diagnostics(LiveTier::Low);
    std::printf("  high: %s | low: %s\n", dh.encoder_name.c_str(), dl.encoder_name.c_str());
    KV_CHECK(!dl.hardware && dl.encoder_name.find("(software fallback)") != std::string::npos);
    KV_CHECK(dh.open && dl.open);
    const auto lo = col.of(LiveTier::Low);
    KV_REQUIRE(lo.size() >= 8);
    KV_CHECK(!lo[0].au.hardware && lo[0].au.idr);
    const DecodeResult r = decodeTier(lo, 8);
    KV_CHECK(r.decoded >= 6 && decodedSizeOk(r, 640, 360));
    set.close();
}

KV_TEST(close_joins_and_stops_output) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::atomic<uint32_t> epochs{0};
    Collector col;
    {
        LiveTierSet::Options o;
        LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
        set.want(LiveTier::High);
        set.want(LiveTier::Low);
        uint64_t seq = 1;
        feed(set, 1280, 720, 8, seq);
        set.close();
        const size_t n = col.outs.size();
        set.close(); // idempotent
        set.submitFrame(makeFrame(1280, 720, 99).data(), 1280, 720, 1280 * 4, 99, nowNs(), 0, 0, false);
        sleepMs(100);
        KV_CHECK(col.outs.size() == n);
    } // destructor after close
    // Destroyed while frames are in flight.
    {
        LiveTierSet::Options o;
        LiveTierSet set(o, epochs, [&](const TierAccessUnit& au) { col.push(au); });
        set.want(LiveTier::Medium);
        uint64_t seq = 1;
        feed(set, 1920, 1080, 3, seq);
    }
    KV_CHECK(true);
}

// Not a check: how long closing encoders took in this run, for the stress logs (mf_h264_encoder.cpp, parkEncoder).
KV_TEST(zz_report_encoder_close_times) {
    const EncoderCloseStats st = encoderCloseStats();
    std::printf("  closes: parked %llu released %llu drain timeouts %llu over cap %llu; max drain %.0f ms, close %.0f ms, "
                "release %.0f ms\n",
                static_cast<unsigned long long>(st.parked), static_cast<unsigned long long>(st.released),
                static_cast<unsigned long long>(st.drainTimeouts), static_cast<unsigned long long>(st.overCap), st.drainMsMax,
                st.closeMsMax, st.releaseMsMax);
}

KV_TEST_MAIN()
