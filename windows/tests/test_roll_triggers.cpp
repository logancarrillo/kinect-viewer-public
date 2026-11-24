// Camera-roll triggers (pet-camera.md §6.1/§6.2, protocol v3 §5.1/§5.2): the depth motion detector, the
// trigger-set state machine and the JPEG thumbnailer. Synthetic inputs only: depth frames and BGRA
// frames are generated here (flat floors, boxes, noise, gradients), and nothing touches the engine, a
// camera, a file or a real camera-roll folder.
#include "kv_test.hpp"

#include "roll_thumbnail.hpp"
#include "roll_triggers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace kvhost;
using Microsoft::WRL::ComPtr;

namespace {

// ---- Synthetic depth -------------------------------------------------------------------------------

constexpr uint32_t W = DepthMotionDetector::kDepthWidth;
constexpr uint32_t H = DepthMotionDetector::kDepthHeight;
constexpr uint64_t kMs = 1000000ULL; // ns per ms

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed) {}
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

using Depth = std::vector<uint16_t>;

// A flat floor at `mm` with uniform noise of +-noise mm per pixel.
Depth floorFrame(int mm, int noise, Rng& rng) {
    Depth d(static_cast<size_t>(W) * H);
    for (auto& v : d) v = static_cast<uint16_t>(mm + (noise ? rng.range(-noise, noise) : 0));
    return d;
}

void drawBox(Depth& d, int x0, int y0, int bw, int bh, int mm) {
    for (int y = y0; y < y0 + bh; ++y) {
        if (y < 0 || y >= static_cast<int>(H)) continue;
        for (int x = x0; x < x0 + bw; ++x) {
            if (x < 0 || x >= static_cast<int>(W)) continue;
            d[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(mm);
        }
    }
}

// Frame sequence numbers for the tests that don't test them: one counter, so every frame follows the
// one before (a new detector, or one after reset(), has no previous number anyway).
uint64_t nextSeq() {
    static uint64_t seq = 1000;
    return ++seq;
}

// Feeds `frames` floor frames, 33 ms apart from *t; returns true if any reported motion.
bool learnFloor(DepthMotionDetector& det, int mm, int noise, int frames, uint64_t& t, Rng& rng) {
    bool any = false;
    for (int i = 0; i < frames; ++i) {
        const Depth d = floorFrame(mm, noise, rng);
        any |= det.process(d.data(), W, H, nextSeq(), t).motion;
        t += 33 * kMs;
    }
    return any;
}

// ---- The fake clock and the trigger-set harness ----------------------------------------------------

class FakeClock : public RollClock {
public:
    // Local time starts on Monday 07:00:00; `ms` counts from there, as the monotonic clock does.
    uint64_t monoMs() const override { return kMonoBase + ms_; }
    RollLocalTime localTime() const override {
        const uint64_t local = kLocalBase + ms_;
        RollLocalTime t;
        t.weekday = static_cast<int>((local / 86400000ULL) % 7);
        t.secondOfDay = static_cast<int>((local % 86400000ULL) / 1000ULL);
        return t;
    }
    void set(uint64_t ms) { ms_ = ms; }
    uint64_t ms() const { return ms_; }

private:
    static constexpr uint64_t kMonoBase = 123456789;
    static constexpr uint64_t kLocalBase = 1ULL * 86400000ULL + 7ULL * 3600000ULL; // Monday (1) 07:00
    uint64_t ms_ = 0;
};

constexpr uint64_t kMin = 60000;
constexpr uint8_t kMonday = 1 << 1;

const char* causeName(ClipStartCause c) {
    switch (c) {
    case ClipStartCause::Trigger: return "trigger";
    case ClipStartCause::Segment: return "segment";
    case ClipStartCause::ViewChanged: return "view";
    case ClipStartCause::Unblocked: return "unblocked";
    }
    return "?";
}

std::string describe(const std::vector<RollEvent>& events) {
    std::string s;
    for (const auto& e : events) {
        if (!s.empty()) s += ' ';
        switch (e.kind) {
        case RollEventKind::StartClip:
            s += std::string("start:") + clipTriggerName(e.trigger) + ":" + causeName(e.cause) + ":" +
                 std::to_string(e.preRollMs);
            break;
        case RollEventKind::ExtendClip: s += std::string("extend:") + clipTriggerName(e.trigger); break;
        case RollEventKind::EndClip:
            s += std::string("end:") + clipEndReasonName(e.endReason);
            if (e.endReason == ClipEndReason::Blocked) s += std::string(":") + rollBlockCode(e.block);
            break;
        case RollEventKind::ThumbnailDue: s += "thumb"; break;
        case RollEventKind::BlockedChanged:
            s += std::string("blocked:") + (e.block == RollBlock::None ? "none" : rollBlockCode(e.block));
            break;
        }
    }
    return s;
}

RollConditions ready() {
    RollConditions c;
    c.rollEnabled = true;
    c.sensorOn = true;
    return c;
}

struct Harness {
    FakeClock clock;
    TriggerSet ts{clock};
    std::vector<RollEvent> log; // every event drained so far

    Harness() {
        ts.setConditions(ready());
        ts.setMotionEnabled(true);
        ts.takeEvents(); // "blocked:none"
    }
    std::string drain() {
        auto ev = ts.takeEvents();
        log.insert(log.end(), ev.begin(), ev.end());
        return describe(ev);
    }
    std::string tick(uint64_t ms) {
        clock.set(ms);
        ts.tick();
        return drain();
    }
    std::string motion(uint64_t ms) {
        clock.set(ms);
        ts.onMotion();
        return drain();
    }
    std::string start(uint64_t ms, const ClipStartedBy& who = ClipStartedBy::pc(), StartClipResult* r = nullptr) {
        clock.set(ms);
        const StartClipResult res = ts.startClip(who);
        if (r) *r = res;
        return drain();
    }
    std::string stop(uint64_t ms, StopClipResult* r = nullptr) {
        clock.set(ms);
        const StopClipResult res = ts.stopClip();
        if (r) *r = res;
        return drain();
    }
    std::string view(uint64_t ms) {
        clock.set(ms);
        ts.onViewChanged();
        return drain();
    }
    std::string conditions(uint64_t ms, const RollConditions& c) {
        clock.set(ms);
        ts.setConditions(c);
        return drain();
    }
    std::string schedule(uint64_t ms, std::vector<RecordScheduleRow> rows, bool enabled) {
        clock.set(ms);
        ts.setSchedule(std::move(rows), enabled);
        return drain();
    }
    // Ticks every `step` ms over (from, to], calling onMotion too while `motionTo` hasn't passed;
    // returns the events of the whole run.
    std::string run(uint64_t from, uint64_t to, uint64_t step, uint64_t motionTo = 0) {
        std::string all;
        for (uint64_t t = from + step; t <= to; t += step) {
            clock.set(t);
            if (t <= motionTo) ts.onMotion();
            ts.tick();
            const std::string s = drain();
            if (!s.empty()) all += (all.empty() ? "" : " ") + s;
        }
        return all;
    }
};

#define EXPECT_EVENTS(expr, expected)                                                                 \
    do {                                                                                              \
        const std::string got_ = (expr);                                                              \
        KV_CHECK_MSG(got_ == (expected), std::string("got \"") + got_ + "\", want \"" + (expected) + "\""); \
    } while (0)

// ---- WIC helpers for the thumbnail tests -----------------------------------------------------------

struct Com {
    bool owned;
    Com() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~Com() {
        if (owned) CoUninitialize();
    }
};

// Decodes the JPEG with WIC; false if it isn't one.
bool decodeJpeg(const std::vector<uint8_t>& jpeg, uint32_t* w, uint32_t* h) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> s;
    if (FAILED(f->CreateStream(&s))) return false;
    if (FAILED(s->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), static_cast<DWORD>(jpeg.size())))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    GUID fmt = {};
    if (FAILED(dec->GetContainerFormat(&fmt)) || fmt != GUID_ContainerFormatJpeg) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT fw = 0, fh = 0;
    if (FAILED(frame->GetSize(&fw, &fh))) return false;
    *w = fw;
    *h = fh;
    return true;
}

std::vector<uint8_t> gradientBgra(uint32_t w, uint32_t h, uint32_t stride) {
    std::vector<uint8_t> px(static_cast<size_t>(stride) * h, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[static_cast<size_t>(y) * stride + x * 4];
            p[0] = static_cast<uint8_t>(x * 255 / (w ? w : 1));
            p[1] = static_cast<uint8_t>(y * 255 / (h ? h : 1));
            p[2] = static_cast<uint8_t>((x + y) & 0xFF);
            p[3] = 255;
        }
    }
    return px;
}

std::vector<uint8_t> noiseBgra(uint32_t w, uint32_t h, uint32_t seed) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    Rng rng(seed);
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>(rng.next());
    return px;
}

} // namespace

// =====================================================================================================
// The depth motion detector
// =====================================================================================================

KV_TEST(motion_threshold_interpolates_from_2_percent_to_0_2_percent) {
    const int valid = DepthMotionDetector::kCellCount; // 64 x 53 = 3392
    KV_CHECK(valid == 3392);
    KV_CHECK(DepthMotionDetector::thresholdCells(valid, 1) == 68);  // 2 % of 3392 = 67.84, rounded up
    KV_CHECK(DepthMotionDetector::thresholdCells(valid, 5) == 41);  // 1.2 % = 40.7
    KV_CHECK(DepthMotionDetector::thresholdCells(valid, 10) == 7);  // 0.2 % = 6.78
    KV_CHECK(DepthMotionDetector::thresholdCells(valid, 0) == 68);  // clamped to 1
    KV_CHECK(DepthMotionDetector::thresholdCells(valid, 11) == 7);  // clamped to 10
    KV_CHECK(DepthMotionDetector::thresholdCells(1000, 1) == 20);
    KV_CHECK(DepthMotionDetector::thresholdCells(10, 10) == 1); // never below one cell
    KV_CHECK(DepthMotionDetector::thresholdCells(0, 5) == 0);
    for (int s = 1; s < 10; ++s) {
        KV_CHECK(DepthMotionDetector::thresholdCells(valid, s) > DepthMotionDetector::thresholdCells(valid, s + 1));
    }
}

KV_TEST(motion_rejects_a_frame_of_the_wrong_size) {
    DepthMotionDetector det;
    Depth d(640 * 480, static_cast<uint16_t>(1000));
    KV_CHECK(!det.process(d.data(), 640, 480, nextSeq(), 0).frameUsed);
    KV_CHECK(!det.process(nullptr, W, H, nextSeq(), 0).frameUsed);
    Rng rng(1);
    const Depth ok = floorFrame(2000, 0, rng);
    const MotionResult r = det.process(ok.data(), W, H, nextSeq(), 0);
    KV_CHECK(r.frameUsed);
    KV_CHECK(r.validCells == 0); // the first frame only learns
    KV_CHECK(!r.motion);
}

KV_TEST(motion_a_flat_floor_with_noise_never_fires) {
    for (int sensitivity : {1, 5, 10}) {
        DepthMotionDetector det;
        det.setSensitivity(sensitivity);
        Rng rng(42);
        uint64_t t = 0;
        int maxChanged = 0;
        for (int i = 0; i < 300; ++i) {
            Depth d = floorFrame(2500, 25, rng);
            // Speckle: 10 % of pixels drop out (0 = invalid) at random, as depth edges and dark spots do.
            for (size_t k = 0; k < d.size(); ++k) {
                if (rng.next() % 10 == 0) d[k] = 0;
            }
            const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
            KV_CHECK(!r.motion);
            if (r.changedCells > maxChanged) maxChanged = r.changedCells;
            t += 33 * kMs;
        }
        KV_CHECK_MSG(maxChanged == 0, "changed cells on noise: " + std::to_string(maxChanged));
    }
}

KV_TEST(motion_a_moving_box_fires_on_its_third_frame) {
    for (int sensitivity : {1, 5, 10}) {
        DepthMotionDetector det;
        det.setSensitivity(sensitivity);
        Rng rng(7);
        uint64_t t = 0;
        KV_CHECK(!learnFloor(det, 2500, 10, 30, t, rng));
        // An 80 x 80 px box (about 10 x 10 cells) at 1.5 m crossing the floor, 4 px a frame.
        int firedAt = -1;
        for (int i = 0; i < 10; ++i) {
            Depth d = floorFrame(2500, 10, rng);
            drawBox(d, 100 + 4 * i, 150, 80, 80, 1500);
            const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
            t += 33 * kMs;
            KV_CHECK(r.changedCells >= 81);
            if (r.motion && firedAt < 0) firedAt = i;
        }
        KV_CHECK_MSG(firedAt == 2, "fired at box frame " + std::to_string(firedAt) + ", sensitivity " +
                                       std::to_string(sensitivity));
    }
}

KV_TEST(motion_needs_three_consecutive_frames) {
    DepthMotionDetector det;
    Rng rng(3);
    uint64_t t = 0;
    learnFloor(det, 2500, 10, 30, t, rng);
    // A box that blinks (present, absent, ...) never makes three frames in a row.
    for (int i = 0; i < 20; ++i) {
        Depth d = floorFrame(2500, 10, rng);
        if (i % 2 == 0) drawBox(d, 200, 160, 80, 80, 1500);
        const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
        KV_CHECK(!r.motion);
        t += 33 * kMs;
    }
    // Two frames, a 2 s stall, then a third: the stall restarts the count.
    for (int i = 0; i < 2; ++i) {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 200, 160, 80, 80, 1500);
        KV_CHECK(!det.process(d.data(), W, H, nextSeq(), t).motion);
        t += 33 * kMs;
    }
    t += 2000 * kMs;
    {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 200, 160, 80, 80, 1500);
        const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
        KV_CHECK(r.consecutive == 1);
        KV_CHECK(!r.motion);
        t += 33 * kMs;
    }
    // A timestamp going backwards restarts it too.
    {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 200, 160, 80, 80, 1500);
        const MotionResult r = det.process(d.data(), W, H, nextSeq(), t - 500 * kMs);
        KV_CHECK(r.consecutive == 1);
    }
}

// A depth frame after a dropped frame never counts as motion with the
// frames before the drop. Each case feeds a moving box with explicit sequence numbers, 33 ms apart.
namespace {
std::vector<bool> boxRun(const std::vector<uint64_t>& seqs) {
    DepthMotionDetector det;
    Rng rng(23);
    uint64_t t = 0;
    uint64_t seq = 1;
    for (int i = 0; i < 30; ++i) {
        const Depth d = floorFrame(2500, 10, rng);
        det.process(d.data(), W, H, seq++, t);
        t += 33 * kMs;
    }
    std::vector<bool> fired;
    for (size_t i = 0; i < seqs.size(); ++i) {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 100 + 4 * static_cast<int>(i), 150, 80, 80, 1500);
        fired.push_back(det.process(d.data(), W, H, seqs[i], t).motion);
        t += 33 * kMs;
    }
    return fired;
}
std::string bits(const std::vector<bool>& v) {
    std::string s;
    for (bool b : v) s += b ? '1' : '0';
    return s;
}
} // namespace

KV_TEST(motion_a_contiguous_run_fires_on_its_third_frame) {
    // The floor took seqs 1-30; the box continues at 31.
    const std::string got = bits(boxRun({31, 32, 33, 34}));
    KV_CHECK_MSG(got == "0011", got);
}

KV_TEST(motion_a_dropped_frame_restarts_the_count) {
    // Box frames 1 and 2, then frame 33 is dropped: 34 starts a new run, which fires on its third frame.
    std::string got = bits(boxRun({31, 32, 34, 35, 36, 37}));
    KV_CHECK_MSG(got == "000011", got);
    // A drop right before the third frame of a run, and a drop of several frames: the same.
    got = bits(boxRun({31, 32, 40, 41, 42}));
    KV_CHECK_MSG(got == "00001", got);
    // A sequence number going backwards (an engine that counted wrong) restarts it too.
    got = bits(boxRun({31, 32, 20, 21, 22}));
    KV_CHECK_MSG(got == "00001", got);
}

KV_TEST(motion_a_repeated_frame_does_not_count) {
    // 32 arrives twice: the repeat restarts the count, so motion needs 32 (again), 33 and 34.
    std::string got = bits(boxRun({31, 32, 32, 33, 34}));
    KV_CHECK_MSG(got == "00001", got);
    // A repeat after a run has fired stops it until three frames follow on again.
    got = bits(boxRun({31, 32, 33, 33, 34, 35, 36}));
    KV_CHECK_MSG(got == "0010011", got);
}

KV_TEST(motion_the_first_frame_after_reset_has_no_previous_seq) {
    DepthMotionDetector det;
    Rng rng(29);
    uint64_t t = 0;
    uint64_t seq = 1;
    for (int i = 0; i < 30; ++i) {
        const Depth d = floorFrame(2500, 10, rng);
        det.process(d.data(), W, H, seq++, t);
        t += 33 * kMs;
    }
    det.reset();
    // After the reset the floor is learned again from a frame whose number follows on; the run of box
    // frames after it counts from its own first frame.
    {
        const Depth d = floorFrame(2500, 10, rng);
        KV_CHECK(det.process(d.data(), W, H, seq++, t).validCells == 0);
        t += 33 * kMs;
    }
    std::string got;
    for (int i = 0; i < 3; ++i) {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 200, 160, 80, 80, 1500);
        got += det.process(d.data(), W, H, seq++, t).motion ? '1' : '0';
        t += 33 * kMs;
    }
    KV_CHECK_MSG(got == "001", got);
}

KV_TEST(motion_a_global_offset_below_the_threshold_does_not_fire) {
    // A floor stepping from 1 m to 4 m, one step per cell row; every pixel then moves 35 mm (below the
    // 40 mm minimum and below 3 % everywhere), as a sensor warming up might. Even at the highest
    // sensitivity nothing fires.
    auto sloped = [](int offset, Rng& rng) {
        Depth d(static_cast<size_t>(W) * H);
        for (uint32_t y = 0; y < H; ++y) {
            const int base = 1000 + static_cast<int>(y / 8) * 3000 / 53;
            for (uint32_t x = 0; x < W; ++x) {
                d[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(base + offset + rng.range(-5, 5));
            }
        }
        return d;
    };
    DepthMotionDetector det;
    det.setSensitivity(10);
    Rng rng(11);
    uint64_t t = 0;
    for (int i = 0; i < 30; ++i) {
        const Depth d = sloped(0, rng);
        KV_CHECK(!det.process(d.data(), W, H, nextSeq(), t).motion);
        t += 33 * kMs;
    }
    for (int i = 0; i < 100; ++i) {
        const Depth d = sloped(35, rng);
        const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
        KV_CHECK(!r.motion);
        KV_CHECK(r.changedCells == 0);
        t += 33 * kMs;
    }
    // Beyond 3.6 m, 3 % is over 108 mm: an 80 mm step there (over 40 mm) doesn't fire either.
    for (int i = 0; i < 10; ++i) {
        Depth d = sloped(35, rng);
        for (size_t k = static_cast<size_t>(W) * 46 * 8; k < d.size(); ++k) d[k] = static_cast<uint16_t>(d[k] + 80);
        KV_CHECK(!det.process(d.data(), W, H, nextSeq(), t).motion);
        t += 33 * kMs;
    }
    // The same kind of step above the threshold does fire: 60 mm (> 40 mm, > 3 %) on the near 40 % of a
    // flat 1 m floor. (Not on all of it: over 60 % of the cells at once is a whole-scene change, below.)
    DepthMotionDetector closeUp;
    closeUp.setSensitivity(10);
    t = 0;
    learnFloor(closeUp, 1000, 5, 30, t, rng);
    bool fired = false;
    for (int i = 0; i < 3; ++i) {
        Depth d = floorFrame(1000, 5, rng);
        for (size_t k = 0; k < static_cast<size_t>(W) * 21 * 8; ++k) d[k] = static_cast<uint16_t>(d[k] + 60);
        fired = closeUp.process(d.data(), W, H, nextSeq(), t).motion;
        t += 33 * kMs;
    }
    KV_CHECK(fired);
}

KV_TEST(motion_ignores_zeros) {
    DepthMotionDetector det;
    det.setSensitivity(10);
    Rng rng(5);
    uint64_t t = 0;
    learnFloor(det, 2500, 10, 30, t, rng);
    for (int i = 0; i < 30; ++i) {
        Depth d = floorFrame(2500, 10, rng);
        // A moving hole of invalid depth (a black object, an IR-absorbing surface).
        drawBox(d, 50 + 8 * i, 100, 120, 120, 0);
        // A region where three pixels in four are invalid: the cell median comes from the valid ones,
        // which are still the floor.
        for (uint32_t y = 300; y < 400; ++y) {
            for (uint32_t x = 300; x < 450; ++x) {
                if ((x + y * 3) % 4 != 0) d[static_cast<size_t>(y) * W + x] = 0;
            }
        }
        // A region with too few valid pixels to count (2 in 64), all of them near: the cells are invalid.
        for (uint32_t y = 16; y < 96; ++y) {
            for (uint32_t x = 400; x < 496; ++x) {
                d[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(((x % 8) == 0 && (y % 4) == 0) ? 800 : 0);
            }
        }
        const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
        KV_CHECK(!r.motion);
        KV_CHECK(r.changedCells == 0);
        t += 33 * kMs;
    }
}

KV_TEST(motion_relearns_a_change_that_stays_still_for_60_s) {
    DepthMotionDetector det;
    Rng rng(9);
    uint64_t t = 0;
    const uint64_t step = 100 * kMs; // 10 fps keeps the test short; the rule runs on timestamps
    for (int i = 0; i < 20; ++i) {
        const Depth d = floorFrame(2500, 10, rng);
        det.process(d.data(), W, H, nextSeq(), t);
        t += step;
    }
    // A box arrives and stays (a pet lying down): motion while it is "new", then the background takes it
    // in 60 s after it stopped moving.
    const uint64_t arrived = t;
    bool sawMotion = false;
    uint64_t stoppedAt = 0;
    int relearned = 0;
    while (t < arrived + 70000 * kMs) {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 200, 160, 80, 80, 1500); // 8-aligned: exactly 10 x 10 cells
        const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
        if (r.motion) sawMotion = true;
        if (sawMotion && !r.motion && stoppedAt == 0) stoppedAt = t;
        relearned += r.relearnedCells;
        t += step;
    }
    KV_CHECK(sawMotion);
    KV_CHECK_MSG(stoppedAt == arrived + 60000 * kMs, "stopped " + std::to_string((stoppedAt - arrived) / kMs) + " ms after");
    KV_CHECK(relearned == 100);
    // Still there: quiet. Then it leaves: the floor it uncovers is a change again, seen within 3 frames.
    {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 200, 160, 80, 80, 1500);
        KV_CHECK(det.process(d.data(), W, H, nextSeq(), t).changedCells == 0);
        t += step;
    }
    int firedAt = -1;
    for (int i = 0; i < 5; ++i) {
        const Depth d = floorFrame(2500, 10, rng);
        if (det.process(d.data(), W, H, nextSeq(), t).motion && firedAt < 0) firedAt = i;
        t += step;
    }
    KV_CHECK(firedAt == 2);
}

KV_TEST(motion_a_change_that_keeps_moving_is_not_relearned) {
    DepthMotionDetector det;
    Rng rng(13);
    uint64_t t = 0;
    const uint64_t step = 100 * kMs;
    for (int i = 0; i < 20; ++i) {
        const Depth d = floorFrame(2500, 10, rng);
        det.process(d.data(), W, H, nextSeq(), t);
        t += step;
    }
    // The box stays in place but its depth swings by 200 mm every 5 s (something moving there): it is
    // never still for 60 s, so it keeps counting for the whole 90 s.
    int quiet = 0;
    for (int i = 0; i < 900; ++i) {
        Depth d = floorFrame(2500, 10, rng);
        drawBox(d, 200, 160, 80, 80, (i / 50) % 2 ? 1500 : 1700);
        const MotionResult r = det.process(d.data(), W, H, nextSeq(), t);
        if (i >= 2 && !r.motion) ++quiet;
        t += step;
    }
    KV_CHECK_MSG(quiet == 0, std::to_string(quiet) + " quiet frames");
}

KV_TEST(motion_sensitivity_extremes) {
    // 25 changed cells (a 40 x 40 px box): over N at 10 (7 cells), under N at 1 (68 cells).
    // 9 changed cells: over N at 10 only, not at 9 (14 cells).
    struct Case {
        int boxPx;
        int sensitivity;
        bool fires;
    };
    const Case cases[] = {{40, 1, false}, {40, 5, false}, {40, 10, true}, {24, 10, true}, {24, 9, false},
                          {88, 1, true}}; // 88 px = 11 x 11 = 121 cells
    for (const auto& c : cases) {
        DepthMotionDetector det;
        det.setSensitivity(c.sensitivity);
        Rng rng(17);
        uint64_t t = 0;
        learnFloor(det, 2500, 10, 30, t, rng);
        bool fired = false;
        for (int i = 0; i < 5; ++i) {
            Depth d = floorFrame(2500, 10, rng);
            drawBox(d, 256, 208, c.boxPx, c.boxPx, 1500); // 8-aligned
            fired |= det.process(d.data(), W, H, nextSeq(), t).motion;
            t += 33 * kMs;
        }
        KV_CHECK_MSG(fired == c.fires, "box " + std::to_string(c.boxPx) + " px at sensitivity " + std::to_string(c.sensitivity));
    }
    DepthMotionDetector det;
    det.setSensitivity(0);
    KV_CHECK(det.sensitivity() == 1);
    det.setSensitivity(99);
    KV_CHECK(det.sensitivity() == 10);
}

// Over 60 % of the cells changing at once, for 3 consecutive frames, is the
// camera moving or the scene changing, not a pet: the whole background is re-learned instead of firing.
namespace {
// '.' quiet, 'M' motion, 'R' whole-background re-learn.
char mark(const MotionResult& r) { return r.globalRelearn ? 'R' : (r.motion ? 'M' : '.'); }
} // namespace

KV_TEST(motion_a_whole_scene_change_relearns_and_never_fires) {
    DepthMotionDetector det;
    det.setSensitivity(10);
    Rng rng(31);
    uint64_t t = 0, seq = 1;
    for (int i = 0; i < 30; ++i) {
        const Depth d = floorFrame(2500, 10, rng);
        det.process(d.data(), W, H, seq++, t);
        t += 33 * kMs;
    }
    // The whole floor plane moves 200 mm (the camera was knocked): every cell changes.
    std::string got;
    for (int i = 0; i < 10; ++i) {
        const Depth d = floorFrame(2700, 10, rng);
        const MotionResult r = det.process(d.data(), W, H, seq++, t);
        got += mark(r);
        if (r.globalRelearn) KV_CHECK(r.validCells == DepthMotionDetector::kCellCount && r.consecutive == 0);
        t += 33 * kMs;
    }
    KV_CHECK_MSG(got == "..R.......", got);
    // The new background is the moved floor: a small box moving over it fires as usual.
    got.clear();
    for (int i = 0; i < 4; ++i) {
        Depth d = floorFrame(2700, 10, rng);
        drawBox(d, 100 + 4 * i, 150, 80, 80, 1500);
        got += mark(det.process(d.data(), W, H, seq++, t));
        t += 33 * kMs;
    }
    KV_CHECK_MSG(got == "..MM", got);
}

KV_TEST(motion_a_whole_scene_change_needs_three_consecutive_frames) {
    // The same run rule as motion: a dropped frame (seq 33) restarts it, so it re-learns on the third
    // frame after the drop, and no frame of it is motion.
    DepthMotionDetector det;
    Rng rng(37);
    uint64_t t = 0;
    for (uint64_t seq = 1; seq <= 30; ++seq) {
        const Depth d = floorFrame(2500, 10, rng);
        det.process(d.data(), W, H, seq, t);
        t += 33 * kMs;
    }
    std::string got;
    for (const uint64_t seq : {31, 32, 34, 35, 36, 37}) {
        const Depth d = floorFrame(2700, 10, rng);
        got += mark(det.process(d.data(), W, H, seq, t));
        t += 33 * kMs;
    }
    KV_CHECK_MSG(got == "....R.", got);
}

KV_TEST(motion_a_box_over_half_the_view_still_fires) {
    // Full width, cell rows 0-25: 1664 of 3392 cells (49 %) is something big and near, and fires. Cell
    // rows 0-37 (2432 cells, 72 %) is over the 60 % and re-learns instead.
    struct Case {
        int rows;
        const char* want;
    };
    const Case cases[] = {{26 * 8, "..MMM"}, {38 * 8, "..R.."}};
    for (const auto& c : cases) {
        DepthMotionDetector det;
        Rng rng(41);
        uint64_t t = 0, seq = 1;
        for (int i = 0; i < 30; ++i) {
            const Depth d = floorFrame(2500, 10, rng);
            det.process(d.data(), W, H, seq++, t);
            t += 33 * kMs;
        }
        std::string got;
        for (int i = 0; i < 5; ++i) {
            Depth d = floorFrame(2500, 10, rng);
            drawBox(d, 0, 0, static_cast<int>(W), c.rows, 1500);
            got += mark(det.process(d.data(), W, H, seq++, t));
            t += 33 * kMs;
        }
        KV_CHECK_MSG(got == c.want, std::to_string(c.rows) + " rows: " + got);
    }
}

KV_TEST(motion_reset_forgets_the_background) {
    DepthMotionDetector det;
    Rng rng(19);
    uint64_t t = 0;
    learnFloor(det, 2500, 10, 30, t, rng);
    det.reset();
    // The first frame after a reset is learned (the box is part of the new background) ...
    Depth boxed = floorFrame(2500, 10, rng);
    drawBox(boxed, 200, 160, 80, 80, 1500);
    MotionResult r = det.process(boxed.data(), W, H, nextSeq(), t);
    KV_CHECK(r.validCells == 0 && !r.motion);
    t += 33 * kMs;
    for (int i = 0; i < 5; ++i) {
        r = det.process(boxed.data(), W, H, nextSeq(), t);
        KV_CHECK(r.changedCells == 0 && !r.motion);
        t += 33 * kMs;
    }
    // ... and detection works against it: the box leaving is motion.
    bool fired = false;
    for (int i = 0; i < 3; ++i) {
        const Depth d = floorFrame(2500, 10, rng);
        fired = det.process(d.data(), W, H, nextSeq(), t).motion;
        t += 33 * kMs;
    }
    KV_CHECK(fired);
}

// =====================================================================================================
// The trigger set
// =====================================================================================================

KV_TEST(triggers_block_reason_precedence_follows_v3_5_1) {
    RollConditions c; // the defaults: roll off, sensor off
    KV_CHECK(rollBlockFor(c) == RollBlock::RollDisabled);
    c = ready();
    KV_CHECK(rollBlockFor(c) == RollBlock::None);
    struct Row {
        void (*apply)(RollConditions&);
        RollBlock want;
        const char* code;
    };
    const Row rows[] = {
        {[](RollConditions& x) { x.privacyMode = true; }, RollBlock::PrivacyMode, "privacy_mode"},
        {[](RollConditions& x) { x.rollEnabled = false; }, RollBlock::RollDisabled, "roll_disabled"},
        {[](RollConditions& x) { x.preparing = true; }, RollBlock::RollPreparing, "roll_preparing"},
        {[](RollConditions& x) { x.sensorOn = false; }, RollBlock::SensorOff, "sensor_off"},
        {[](RollConditions& x) { x.viewFrozen = true; }, RollBlock::ViewFrozen, "view_frozen"},
        {[](RollConditions& x) { x.pcReplaying = true; }, RollBlock::PcReplaying, "pc_replaying"},
        {[](RollConditions& x) { x.diskFloor = true; }, RollBlock::DiskFloor, "disk_floor"},
        {[](RollConditions& x) { x.storageFull = true; }, RollBlock::StorageFull, "storage_full"},
    };
    // Each alone gives its own code; each together with every later one still gives its own (the list order).
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        RollConditions one = ready();
        rows[i].apply(one);
        KV_CHECK(rollBlockFor(one) == rows[i].want);
        KV_CHECK(std::string(rollBlockCode(rows[i].want)) == rows[i].code);
        RollConditions many = ready();
        for (size_t j = i; j < sizeof(rows) / sizeof(rows[0]); ++j) rows[j].apply(many);
        KV_CHECK(rollBlockFor(many) == rows[i].want);
    }
    KV_CHECK(std::string(rollBlockCode(RollBlock::None)).empty());
}

KV_TEST(triggers_start_clip_while_blocked_is_refused_with_the_code_and_adds_nothing) {
    {
        FakeClock clock;
        TriggerSet ts(clock); // fresh: the roll is off
        KV_CHECK(ts.blocked() == RollBlock::RollDisabled);
        const StartClipResult r = ts.startClip(ClipStartedBy::pc());
        KV_CHECK(!r.ok && r.refused == RollBlock::RollDisabled);
        KV_CHECK(ts.takeEvents().empty());
        KV_CHECK(!ts.manualActive());
    }
    const RollBlock blocks[] = {RollBlock::PrivacyMode, RollBlock::RollDisabled, RollBlock::SensorOff, RollBlock::ViewFrozen,
                                RollBlock::PcReplaying, RollBlock::DiskFloor,    RollBlock::StorageFull};
    for (const RollBlock b : blocks) {
        Harness h;
        RollConditions c = ready();
        switch (b) {
        case RollBlock::PrivacyMode: c.privacyMode = true; break;
        case RollBlock::RollDisabled: c.rollEnabled = false; break;
        case RollBlock::SensorOff: c.sensorOn = false; break;
        case RollBlock::ViewFrozen: c.viewFrozen = true; break;
        case RollBlock::PcReplaying: c.pcReplaying = true; break;
        case RollBlock::DiskFloor: c.diskFloor = true; break;
        case RollBlock::StorageFull: c.storageFull = true; break;
        case RollBlock::None: break;
        }
        EXPECT_EVENTS(h.conditions(0, c), std::string("blocked:") + rollBlockCode(b));
        StartClipResult r;
        EXPECT_EVENTS(h.start(1000, ClipStartedBy::pc(), &r), "");
        KV_CHECK(!r.ok && r.refused == b);
        KV_CHECK(!h.ts.manualActive());
        EXPECT_EVENTS(h.motion(2000), ""); // motion is ignored too
        // The block clears: the refused start left no trigger behind, so nothing starts.
        EXPECT_EVENTS(h.conditions(3000, ready()), "blocked:none");
        EXPECT_EVENTS(h.tick(4000), "");
        KV_CHECK(!h.ts.recording());
    }
}

KV_TEST(triggers_manual_start_and_stop) {
    Harness h;
    StopClipResult sr;
    EXPECT_EVENTS(h.stop(0, &sr), "");
    KV_CHECK(sr == StopClipResult::Ok); // not recording: a no-op
    StartClipResult r;
    EXPECT_EVENTS(h.start(1000, ClipStartedBy::device("a1b2c3d4e5f60718", "Kitchen iPhone"), &r), "start:manual:trigger:5000");
    KV_CHECK(r.ok);
    KV_CHECK(h.log.back().startedBy.has_value() && h.log.back().startedBy->deviceName == "Kitchen iPhone");
    KV_CHECK(h.ts.recording() && h.ts.manualActive() && h.ts.clipTrigger() == ClipTrigger::Manual);
    EXPECT_EVENTS(h.tick(2000), "thumb");
    h.clock.set(4500);
    KV_CHECK(h.ts.clipElapsedSec() == 3.5);
    EXPECT_EVENTS(h.stop(5000, &sr), "end:triggers_ended");
    KV_CHECK(sr == StopClipResult::Ok);
    KV_CHECK(h.log.back().thumbnailTaken);
    KV_CHECK(!h.ts.recording() && h.ts.clipElapsedSec() == 0.0);
    // The clip has ended: a stop that arrives now (it may have crossed the end) is still Ok.
    EXPECT_EVENTS(h.stop(6000, &sr), "");
    KV_CHECK(sr == StopClipResult::Ok);
}

KV_TEST(triggers_stop_clip_answers_by_what_is_recording) {
    // Not recording -> Ok (a no-op); recording with a manual trigger -> Ok,
    // and only the manual trigger goes; recording with no manual trigger -> Busy.
    StopClipResult sr;
    {
        Harness h; // a motion clip ends by itself just before the stop arrives
        EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
        EXPECT_EVENTS(h.tick(10000), "thumb end:triggers_ended");
        EXPECT_EVENTS(h.stop(10050, &sr), "");
        KV_CHECK(sr == StopClipResult::Ok && !h.ts.recording());
    }
    {
        Harness h; // a schedule clip with no manual trigger
        EXPECT_EVENTS(h.schedule(0, {RecordScheduleRow{kMonday, 7 * 60, 10}}, true), "start:schedule:trigger:0");
        EXPECT_EVENTS(h.stop(2000, &sr), "thumb");
        KV_CHECK(sr == StopClipResult::Busy && h.ts.recording());
        // Manual joins it; stopping drops manual only, and the schedule keeps the clip going.
        EXPECT_EVENTS(h.start(3000), "extend:manual");
        EXPECT_EVENTS(h.stop(4000, &sr), "");
        KV_CHECK(sr == StopClipResult::Ok && h.ts.recording() && !h.ts.manualActive());
        EXPECT_EVENTS(h.stop(5000, &sr), "");
        KV_CHECK(sr == StopClipResult::Busy); // back to a schedule clip with no manual trigger
        EXPECT_EVENTS(h.tick(10 * kMin), "end:triggers_ended");
    }
    {
        Harness h; // a motion clip: Busy, and the clip goes on
        EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
        EXPECT_EVENTS(h.stop(500, &sr), "");
        KV_CHECK(sr == StopClipResult::Busy && h.ts.recording() && h.ts.motionActive());
    }
}

KV_TEST(triggers_a_short_clip_gets_its_thumbnail_before_it_ends) {
    Harness h;
    EXPECT_EVENTS(h.start(0), "start:manual:trigger:5000");
    KV_CHECK(h.log.back().startedBy == ClipStartedBy::pc());
    KV_CHECK(h.log.back().startedBy->deviceName == "This PC" && h.log.back().startedBy->deviceId.empty());
    EXPECT_EVENTS(h.stop(500), "thumb end:triggers_ended");
    KV_CHECK(h.log.back().thumbnailTaken);
}

KV_TEST(triggers_manual_lasts_10_minutes_and_a_second_start_extends_it) {
    Harness h;
    EXPECT_EVENTS(h.start(0), "start:manual:trigger:5000");
    EXPECT_EVENTS(h.tick(1000), "thumb");
    EXPECT_EVENTS(h.tick(10 * kMin - 1), "");
    // The manual trigger alone reaching 10 minutes ends the clip; it isn't cut into a new segment.
    EXPECT_EVENTS(h.tick(10 * kMin), "end:triggers_ended");

    Harness g;
    EXPECT_EVENTS(g.start(0), "start:manual:trigger:5000");
    StartClipResult r;
    EXPECT_EVENTS(g.start(5 * kMin, ClipStartedBy::device("0011223344556677", "Other"), &r), "thumb");
    KV_CHECK(r.ok);
    EXPECT_EVENTS(g.tick(10 * kMin), "end:segment_cap start:manual:segment:0");
    KV_CHECK(g.log.back().startedBy == ClipStartedBy::pc()); // the startedBy that added it
    EXPECT_EVENTS(g.tick(15 * kMin - 1), "thumb");
    EXPECT_EVENTS(g.tick(15 * kMin), "end:triggers_ended");
}

KV_TEST(triggers_motion_lasts_10_s_after_the_last_motion_then_cools_down_30_s) {
    Harness h;
    EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
    KV_CHECK(!h.log.back().startedBy.has_value());
    EXPECT_EVENTS(h.tick(1000), "thumb");
    EXPECT_EVENTS(h.motion(5000), "");
    EXPECT_EVENTS(h.tick(14999), "");
    EXPECT_EVENTS(h.tick(15000), "end:triggers_ended");
    KV_CHECK(h.ts.motionCooldown());
    // During the cooldown motion can't start a clip, and isn't remembered for later.
    EXPECT_EVENTS(h.motion(16000), "");
    EXPECT_EVENTS(h.motion(44999), "");
    EXPECT_EVENTS(h.tick(45000), "");
    KV_CHECK(!h.ts.motionCooldown());
    EXPECT_EVENTS(h.motion(45000), "start:motion:trigger:5000");
}

KV_TEST(triggers_motion_extends_a_clip_during_the_cooldown) {
    Harness h;
    EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
    EXPECT_EVENTS(h.tick(10000), "thumb end:triggers_ended");
    // A manual clip during the cooldown: motion joins it and holds it after the manual trigger stops.
    EXPECT_EVENTS(h.start(12000), "start:manual:trigger:5000");
    EXPECT_EVENTS(h.motion(13000), "thumb extend:motion");
    StopClipResult sr;
    EXPECT_EVENTS(h.stop(14000, &sr), "");
    KV_CHECK(sr == StopClipResult::Ok && h.ts.recording());
    KV_CHECK(h.ts.clipTrigger() == ClipTrigger::Manual); // still named by the trigger that started it
    EXPECT_EVENTS(h.tick(22999), "");
    EXPECT_EVENTS(h.tick(23000), "end:triggers_ended");
}

KV_TEST(triggers_manual_extends_a_motion_clip_and_stop_clip_is_busy_without_it) {
    Harness h;
    EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
    StopClipResult sr;
    EXPECT_EVENTS(h.stop(500, &sr), "");
    KV_CHECK(sr == StopClipResult::Busy); // held by motion only: no manual trigger to remove
    KV_CHECK(h.ts.recording());
    StartClipResult r;
    EXPECT_EVENTS(h.start(2000, ClipStartedBy::device("a1b2c3d4e5f60718", "Phone"), &r), "thumb extend:manual");
    KV_CHECK(r.ok && h.ts.manualActive());
    KV_CHECK(h.ts.clipTrigger() == ClipTrigger::Motion);
    EXPECT_EVENTS(h.tick(30000), ""); // motion ran out at 10 s; manual holds the clip
    KV_CHECK(h.ts.recording() && !h.ts.motionActive());
    EXPECT_EVENTS(h.stop(40000, &sr), "end:triggers_ended");
    KV_CHECK(sr == StopClipResult::Ok);
}

KV_TEST(triggers_segments_are_cut_at_10_minutes) {
    Harness h;
    EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
    // Motion every second for 25 minutes, then quiet.
    const std::string all = h.run(0, 26 * kMin, 1000, 25 * kMin);
    EXPECT_EVENTS(all, "thumb end:segment_cap start:motion:segment:0 thumb end:segment_cap start:motion:segment:0 thumb "
                       "end:triggers_ended");
    // Each segment is its own clip with its own number, and the cuts are exactly 10 minutes apart.
    std::vector<uint64_t> starts;
    for (const auto& e : h.log) {
        if (e.kind == RollEventKind::StartClip) starts.push_back(e.atMonoMs);
    }
    KV_REQUIRE(starts.size() == 3);
    KV_CHECK(starts[1] - starts[0] == 10 * kMin && starts[2] - starts[1] == 10 * kMin);
    KV_CHECK(h.log.back().clipSeq == 3);
    KV_CHECK(h.log.back().atMonoMs - starts[0] == 25 * kMin + 10000);
}

// A segment file, pre-roll included, is at most 10 minutes. The host reports
// the pre-roll it gave the clip, and the cut comes 10 minutes after the file's first frame.
KV_TEST(triggers_the_segment_cap_counts_the_pre_roll) {
    Harness h;
    EXPECT_EVENTS(h.start(10000), "start:manual:trigger:5000");
    const uint64_t seq = h.log.back().clipSeq;
    h.ts.setClipPreRoll(seq + 7, 4000); // not the running clip: ignored
    h.ts.setClipPreRoll(seq, 4500);     // the file starts 4.5 s before the trigger
    EXPECT_EVENTS(h.start(5 * kMin), "thumb"); // the manual trigger's own 10 minutes restart (§6.2)
    EXPECT_EVENTS(h.tick(10000 + 10 * kMin - 4500 - 1), "");
    EXPECT_EVENTS(h.tick(10000 + 10 * kMin - 4500), "end:segment_cap start:manual:segment:0");
    // The next segment has no pre-roll: its 10 minutes count from its start.
    const uint64_t cut = 10000 + 10 * kMin - 4500;
    EXPECT_EVENTS(h.tick(cut + 1000), "thumb");
    EXPECT_EVENTS(h.start(cut + 4 * kMin), ""); // before the first manual trigger runs out at 15:00
    EXPECT_EVENTS(h.tick(cut + 10 * kMin - 1), "");
    EXPECT_EVENTS(h.tick(cut + 10 * kMin), "end:segment_cap start:manual:segment:0");
    // A pre-roll longer than the clock's own time (a clock that starts near 0) can't make it negative.
    Harness g;
    EXPECT_EVENTS(g.start(0), "start:manual:trigger:5000");
    g.ts.setClipPreRoll(g.log.back().clipSeq, 5000);
    EXPECT_EVENTS(g.tick(1000), "thumb");
}

// The manual trigger's 10 minutes count from the clip's first frame, pre-roll included,
// the same as the segment cut. A full 10-minute manual clip with a full 5 s pre-roll is one file: the old rule
// (10 minutes from the press) cut a segment at 9:55 and left a 5 s stub clip after it.
KV_TEST(triggers_a_full_manual_clip_with_pre_roll_leaves_no_stub) {
    Harness h;
    EXPECT_EVENTS(h.start(20000), "start:manual:trigger:5000");
    h.ts.setClipPreRoll(h.log.back().clipSeq, 5000); // the file starts 5 s before the press
    EXPECT_EVENTS(h.tick(21000), "thumb");
    EXPECT_EVENTS(h.tick(20000 + 10 * kMin - 5000 - 1), "");
    // At the cut the manual trigger has run its 10 minutes too: the clip ends, and nothing follows it.
    EXPECT_EVENTS(h.tick(20000 + 10 * kMin - 5000), "end:triggers_ended");
    EXPECT_EVENTS(h.run(20000 + 10 * kMin - 5000, 20000 + 11 * kMin, 1000), "");
    KV_CHECK(!h.ts.recording() && !h.ts.manualActive());
    int starts = 0;
    for (const auto& e : h.log) starts += e.kind == RollEventKind::StartClip ? 1 : 0;
    KV_CHECK(starts == 1);
}

KV_TEST(triggers_a_segment_continues_under_the_trigger_still_active) {
    // Manual starts it, motion joins; at 10 minutes manual runs out and the next segment is motion's.
    Harness h;
    EXPECT_EVENTS(h.start(0), "start:manual:trigger:5000");
    EXPECT_EVENTS(h.motion(0), "extend:motion");
    EXPECT_EVENTS(h.run(0, 12 * kMin, 1000, 12 * kMin),
                  "thumb end:segment_cap start:motion:segment:0 thumb");
    KV_REQUIRE(h.log.size() >= 2 && h.log[h.log.size() - 2].kind == RollEventKind::StartClip);
    KV_CHECK(!h.log[h.log.size() - 2].startedBy.has_value()); // a motion clip has no startedBy

    // Manual starts it, a schedule window opens, motion joins: the next segment is the schedule's
    // (manual, then schedule, then motion).
    Harness g;
    EXPECT_EVENTS(g.schedule(0, {RecordScheduleRow{kMonday, 7 * 60 + 1, 60}}, true), ""); // 07:01 for 60 min
    EXPECT_EVENTS(g.start(0), "start:manual:trigger:5000");
    EXPECT_EVENTS(g.run(0, 61000, 1000), "thumb extend:schedule");
    EXPECT_EVENTS(g.motion(120000), "extend:motion");
    EXPECT_EVENTS(g.run(120000, 601000, 1000, 601000), "end:segment_cap start:schedule:segment:0 thumb");
}

KV_TEST(triggers_a_block_ends_the_clip_and_drops_the_triggers) {
    Harness h;
    EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
    EXPECT_EVENTS(h.start(500), "extend:manual");
    RollConditions privacy = ready();
    privacy.privacyMode = true;
    EXPECT_EVENTS(h.conditions(5000, privacy), "thumb blocked:privacy_mode end:blocked:privacy_mode");
    KV_CHECK(!h.ts.recording() && !h.ts.manualActive() && !h.ts.motionActive());
    KV_CHECK(h.ts.blocked() == RollBlock::PrivacyMode);
    StartClipResult r;
    EXPECT_EVENTS(h.start(6000, ClipStartedBy::pc(), &r), "");
    KV_CHECK(!r.ok && r.refused == RollBlock::PrivacyMode);
    EXPECT_EVENTS(h.motion(7000), "");
    // Cleared: the manual clip has to be started again; motion starts one as soon as it detects
    // something, with no cooldown.
    EXPECT_EVENTS(h.conditions(10000, ready()), "blocked:none");
    StopClipResult sr;
    EXPECT_EVENTS(h.stop(10100, &sr), "");
    KV_CHECK(sr == StopClipResult::Ok && !h.ts.recording()); // nothing recording: a no-op
    KV_CHECK(!h.ts.motionCooldown());
    EXPECT_EVENTS(h.motion(10500), "start:motion:trigger:5000");

    // A block before the thumbnail: no thumbnail (the frame is behind privacy now), and EndClip says so.
    Harness g;
    EXPECT_EVENTS(g.start(0), "start:manual:trigger:5000");
    RollConditions frozen = ready();
    frozen.viewFrozen = true;
    EXPECT_EVENTS(g.conditions(400, frozen), "blocked:view_frozen end:blocked:view_frozen");
    KV_CHECK(!g.log.back().thumbnailTaken);
}

KV_TEST(triggers_a_change_of_block_reason_only_changes_the_reason) {
    Harness h;
    RollConditions c = ready();
    c.sensorOn = false;
    EXPECT_EVENTS(h.conditions(0, c), "blocked:sensor_off");
    c.privacyMode = true;
    EXPECT_EVENTS(h.conditions(1000, c), "blocked:privacy_mode");
    EXPECT_EVENTS(h.conditions(2000, c), ""); // no change, no event
    c.privacyMode = false;
    EXPECT_EVENTS(h.conditions(3000, c), "blocked:sensor_off");
    c.diskFloor = true;
    c.sensorOn = true;
    EXPECT_EVENTS(h.conditions(4000, c), "blocked:disk_floor");
    c.diskFloor = false;
    c.storageFull = true;
    EXPECT_EVENTS(h.conditions(5000, c), "blocked:storage_full");
    EXPECT_EVENTS(h.conditions(6000, ready()), "blocked:none");
}

KV_TEST(triggers_schedule_records_its_window_without_pre_roll) {
    Harness h;
    // Monday 08:00 for 10 minutes. The clock starts on Monday 07:00:00.
    EXPECT_EVENTS(h.schedule(0, {RecordScheduleRow{kMonday, 8 * 60, 10}}, true), "");
    EXPECT_EVENTS(h.tick(60 * kMin - 1000), "");
    EXPECT_EVENTS(h.tick(60 * kMin), "start:schedule:trigger:0");
    KV_CHECK(h.ts.scheduleActive() && h.ts.clipTrigger() == ClipTrigger::Schedule);
    EXPECT_EVENTS(h.tick(60 * kMin + 1000), "thumb");
    EXPECT_EVENTS(h.tick(70 * kMin - 1000), "");
    EXPECT_EVENTS(h.tick(70 * kMin), "end:triggers_ended");
    // Not on Tuesday.
    EXPECT_EVENTS(h.tick(24 * 60 * kMin + 60 * kMin), "");
    EXPECT_EVENTS(h.tick(24 * 60 * kMin + 65 * kMin), "");
    // Schedule ignores the motion cooldown.
    Harness g;
    EXPECT_EVENTS(g.schedule(0, {RecordScheduleRow{kMonday, 7 * 60 + 1, 5}}, true), "");
    EXPECT_EVENTS(g.motion(40000), "start:motion:trigger:5000");
    EXPECT_EVENTS(g.tick(50000), "thumb end:triggers_ended");
    KV_CHECK(g.ts.motionCooldown());
    EXPECT_EVENTS(g.tick(60000), "start:schedule:trigger:0");
}

KV_TEST(triggers_schedule_restarts_after_a_block_inside_its_window) {
    Harness h;
    EXPECT_EVENTS(h.schedule(0, {RecordScheduleRow{kMonday, 8 * 60, 10}}, true), "");
    EXPECT_EVENTS(h.tick(60 * kMin), "start:schedule:trigger:0");
    EXPECT_EVENTS(h.tick(61 * kMin), "thumb");
    RollConditions privacy = ready();
    privacy.privacyMode = true;
    EXPECT_EVENTS(h.conditions(63 * kMin, privacy), "blocked:privacy_mode end:blocked:privacy_mode");
    EXPECT_EVENTS(h.tick(64 * kMin), "");
    EXPECT_EVENTS(h.conditions(65 * kMin, ready()), "blocked:none start:schedule:unblocked:0");
    EXPECT_EVENTS(h.tick(70 * kMin), "thumb end:triggers_ended");
    // A block that clears after the window has closed starts nothing.
    EXPECT_EVENTS(h.tick(24 * 60 * kMin + 60 * kMin + 1), "");
}

KV_TEST(triggers_schedule_inside_its_window_at_start_up_or_when_enabled_starts_at_once) {
    Harness h;
    h.clock.set(62 * kMin); // Monday 08:02
    EXPECT_EVENTS(h.schedule(62 * kMin, {RecordScheduleRow{kMonday, 8 * 60, 10}}, false), "");
    EXPECT_EVENTS(h.schedule(63 * kMin, {RecordScheduleRow{kMonday, 8 * 60, 10}}, true), "start:schedule:trigger:0");
    // Turning the schedule off ends its trigger.
    EXPECT_EVENTS(h.schedule(63 * kMin + 500, {RecordScheduleRow{kMonday, 8 * 60, 10}}, false), "thumb end:triggers_ended");
}

KV_TEST(triggers_a_schedule_window_joins_a_running_clip) {
    Harness h;
    EXPECT_EVENTS(h.schedule(0, {RecordScheduleRow{kMonday, 7 * 60 + 1, 2}}, true), "");
    EXPECT_EVENTS(h.motion(55000), "start:motion:trigger:5000");
    EXPECT_EVENTS(h.tick(60000), "extend:schedule thumb");
    // Motion ran out at 65 s; the schedule holds the clip until 07:03.
    EXPECT_EVENTS(h.tick(179000), "");
    EXPECT_EVENTS(h.tick(180000), "end:triggers_ended");
    KV_CHECK(h.log.back().trigger == ClipTrigger::Motion);
}

KV_TEST(triggers_schedule_windows_by_day_and_across_midnight) {
    const auto at = [](int weekday, int h, int m, int s = 0) { return RollLocalTime{weekday, h * 3600 + m * 60 + s}; };
    const std::vector<RecordScheduleRow> sunLate = {RecordScheduleRow{1 << 0, 23 * 60 + 55, 10}}; // Sun 23:55-00:05
    KV_CHECK(!isScheduledRecording(sunLate, at(0, 23, 54, 59)));
    KV_CHECK(isScheduledRecording(sunLate, at(0, 23, 55)));
    KV_CHECK(isScheduledRecording(sunLate, at(1, 0, 4, 59))); // Monday morning, the row's tail
    KV_CHECK(!isScheduledRecording(sunLate, at(1, 0, 5)));
    KV_CHECK(!isScheduledRecording(sunLate, at(6, 23, 58))); // Saturday: not a row day
    KV_CHECK(!isScheduledRecording(sunLate, at(0, 0, 3)));   // Sunday morning belongs to Saturday's row
    const std::vector<RecordScheduleRow> weekdays = {RecordScheduleRow{0x3E, 8 * 60, 60}}; // Mon-Fri 08:00-09:00
    KV_CHECK(isScheduledRecording(weekdays, at(5, 8, 59, 59)));
    KV_CHECK(!isScheduledRecording(weekdays, at(5, 9, 0)));
    KV_CHECK(!isScheduledRecording(weekdays, at(6, 8, 30)));
    // Rows that can't fire: no days, a length outside 1-60, a start past 23:59.
    const std::vector<RecordScheduleRow> bad = {RecordScheduleRow{0, 8 * 60, 10}, RecordScheduleRow{0x7F, 8 * 60, 0},
                                                RecordScheduleRow{0x7F, 8 * 60, 61}, RecordScheduleRow{0x7F, 24 * 60, 10}};
    KV_CHECK(!isScheduledRecording(bad, at(1, 8, 5)));
    KV_CHECK(!isScheduledRecording(bad, at(1, 0, 5)));
    KV_CHECK(!isScheduledRecording(weekdays, RollLocalTime{7, 8 * 3600}));
    // One minute and sixty minutes are both valid lengths.
    KV_CHECK(isScheduledRecording({RecordScheduleRow{0x7F, 600, 1}}, at(3, 10, 0, 59)));
    KV_CHECK(!isScheduledRecording({RecordScheduleRow{0x7F, 600, 1}}, at(3, 10, 1)));
    KV_CHECK(isScheduledRecording({RecordScheduleRow{0x7F, 600, 60}}, at(3, 10, 59, 59)));
}

KV_TEST(triggers_a_view_change_cuts_the_clip_and_continues_without_pre_roll) {
    Harness h;
    EXPECT_EVENTS(h.view(0), ""); // nothing running
    EXPECT_EVENTS(h.motion(1000), "start:motion:trigger:5000");
    EXPECT_EVENTS(h.view(1500), "end:view_changed start:motion:view:0");
    KV_CHECK(h.log.size() >= 2 && !h.log[h.log.size() - 2].thumbnailTaken); // the frame is the new view's
    EXPECT_EVENTS(h.tick(2500), "thumb");
    EXPECT_EVENTS(h.tick(11000), "end:triggers_ended");
    // A manual clip keeps its startedBy across the cut.
    Harness g;
    EXPECT_EVENTS(g.start(0, ClipStartedBy::device("a1b2c3d4e5f60718", "Phone")), "start:manual:trigger:5000");
    EXPECT_EVENTS(g.view(3000), "thumb end:view_changed start:manual:view:0");
    KV_CHECK(g.log.back().startedBy == ClipStartedBy::device("a1b2c3d4e5f60718", "Phone"));
}

KV_TEST(triggers_motion_disabled_is_ignored_and_disabling_drops_it) {
    Harness h;
    h.ts.setMotionEnabled(false);
    EXPECT_EVENTS(h.motion(0), "");
    h.ts.setMotionEnabled(true);
    EXPECT_EVENTS(h.motion(1000), "start:motion:trigger:5000");
    h.clock.set(1500);
    h.ts.setMotionEnabled(false);
    EXPECT_EVENTS(h.drain(), "thumb end:triggers_ended");
}

KV_TEST(triggers_shutdown_ends_the_clip) {
    Harness h;
    EXPECT_EVENTS(h.motion(0), "start:motion:trigger:5000");
    EXPECT_EVENTS(h.start(100), "extend:manual");
    h.clock.set(300);
    h.ts.shutdown();
    EXPECT_EVENTS(h.drain(), "thumb end:shutdown");
    KV_CHECK(!h.ts.recording() && !h.ts.manualActive() && !h.ts.motionActive());
    EXPECT_EVENTS(h.tick(400), "");
    // No clip running: nothing to say.
    h.ts.shutdown();
    EXPECT_EVENTS(h.drain(), "");
}

// The host couldn't write the clip (its writer failed). The clip ends without a thumbnail and,
// failing closed, nothing restarts it on its own straight away.
KV_TEST(triggers_abort_clip_ends_it_and_drops_every_trigger) {
    Harness h;
    // Nothing running: nothing happens.
    h.clock.set(1000);
    h.ts.abortClip();
    EXPECT_EVENTS(h.drain(), "");
    // A manual clip: ended, the manual trigger gone (stop_clip is then the no-op), no thumbnail.
    EXPECT_EVENTS(h.start(2000), "start:manual:trigger:5000");
    h.clock.set(2500);
    h.ts.abortClip();
    EXPECT_EVENTS(h.drain(), "end:writer_failed");
    KV_CHECK(!h.ts.recording() && !h.ts.manualActive() && !h.log.back().thumbnailTaken);
    StopClipResult stop = StopClipResult::Busy;
    EXPECT_EVENTS(h.stop(3000, &stop), "");
    KV_CHECK(stop == StopClipResult::Ok);
    // Motion waits out the cooldown before it can start another clip.
    EXPECT_EVENTS(h.motion(4000), "");
    KV_CHECK(h.ts.motionCooldown());
    EXPECT_EVENTS(h.motion(2500 + 30000), "start:motion:trigger:5000");
    h.clock.set(2500 + 30500);
    h.ts.abortClip();
    EXPECT_EVENTS(h.drain(), "end:writer_failed");
    KV_CHECK(!h.ts.motionActive());
    // A manual start right after is a person asking again: it may start at once.
    EXPECT_EVENTS(h.start(2500 + 31000), "start:manual:trigger:5000");
}

KV_TEST(triggers_abort_clip_keeps_the_schedule_off_until_its_window_closes) {
    Harness h;
    EXPECT_EVENTS(h.schedule(0, {RecordScheduleRow{kMonday, 8 * 60, 10}}, true), "");
    EXPECT_EVENTS(h.tick(60 * kMin), "start:schedule:trigger:0");
    h.clock.set(60 * kMin + 200);
    h.ts.abortClip();
    EXPECT_EVENTS(h.drain(), "end:writer_failed");
    // Still inside the window: no restart, and a block clearing doesn't restart it either.
    EXPECT_EVENTS(h.tick(61 * kMin), "");
    RollConditions frozen = ready();
    frozen.viewFrozen = true;
    EXPECT_EVENTS(h.conditions(62 * kMin, frozen), "blocked:view_frozen");
    EXPECT_EVENTS(h.conditions(63 * kMin, ready()), "blocked:none");
    EXPECT_EVENTS(h.tick(69 * kMin), "");
    // The window closes, and the next one (Monday next week) records again.
    EXPECT_EVENTS(h.tick(71 * kMin), "");
    EXPECT_EVENTS(h.tick(7 * 24 * 60 * kMin + 60 * kMin), "start:schedule:trigger:0");
}

KV_TEST(triggers_clip_numbers_increase_and_events_carry_them) {
    Harness h;
    h.motion(0);
    h.view(2000);
    h.tick(20000);
    h.start(60000);
    h.stop(61000);
    uint64_t lastStart = 0;
    for (const auto& e : h.log) {
        if (e.kind == RollEventKind::StartClip) {
            KV_CHECK(e.clipSeq == lastStart + 1);
            lastStart = e.clipSeq;
        } else {
            KV_CHECK(e.clipSeq == lastStart);
        }
    }
    KV_CHECK(lastStart == 3);
}

// =====================================================================================================
// The thumbnailer
// =====================================================================================================

KV_TEST(thumbnail_quality_steps_start_at_0_75_and_fall) {
    const auto& q = rollThumbQualitySteps();
    KV_REQUIRE(!q.empty());
    KV_CHECK(q.front() == kRollThumbStartQuality);
    for (size_t i = 1; i < q.size(); ++i) KV_CHECK(q[i] < q[i - 1] && q[i] > 0.0f);
}

KV_TEST(thumbnail_sizes_keep_the_aspect_ratio_at_most_320_wide) {
    struct Case {
        uint32_t w, h, tw, th;
    };
    const Case cases[] = {{1280, 720, 320, 180}, {1024, 848, 320, 265}, {512, 424, 320, 265},
                          {320, 240, 320, 240},  {200, 100, 200, 100},  {4000, 1, 320, 1}};
    for (const auto& c : cases) {
        uint32_t tw = 0, th = 0;
        rollThumbSize(c.w, c.h, &tw, &th);
        KV_CHECK_MSG(tw == c.tw && th == c.th, std::to_string(c.w) + "x" + std::to_string(c.h) + " -> " +
                                                   std::to_string(tw) + "x" + std::to_string(th));
    }
}

KV_TEST(thumbnail_encodes_a_jpeg_at_0_75_within_64_KiB) {
    Com com;
    struct Case {
        uint32_t w, h, stride, tw, th;
    };
    const Case cases[] = {{1280, 720, 1280 * 4, 320, 180}, {1024, 848, 1024 * 4 + 64, 320, 265}, {200, 100, 200 * 4, 200, 100}};
    for (const auto& c : cases) {
        const auto px = gradientBgra(c.w, c.h, c.stride);
        RollThumbnail t;
        std::string err;
        KV_REQUIRE(makeRollThumbnail(px.data(), c.w, c.h, c.stride, t, &err));
        KV_CHECK(t.width == c.tw && t.height == c.th);
        KV_CHECK(t.quality == 0.75f); // a smooth frame fits at the first quality
        KV_CHECK(!t.jpeg.empty() && t.jpeg.size() <= kRollThumbMaxBytes);
        KV_CHECK(t.jpeg.size() > 2 && t.jpeg[0] == 0xFF && t.jpeg[1] == 0xD8);
        uint32_t dw = 0, dh = 0;
        KV_CHECK(decodeJpeg(t.jpeg, &dw, &dh));
        KV_CHECK(dw == c.tw && dh == c.th);
    }
}

KV_TEST(thumbnail_noise_lowers_the_quality_until_it_fits) {
    Com com;
    // Full-range noise is the worst case for JPEG: at 320 px wide (no scaling to smooth it) it is far
    // over 64 KiB at 0.75, so the loop has to come down.
    bool lowered = false;
    const struct {
        uint32_t w, h;
    } sizes[] = {{320, 265}, {320, 400}, {1280, 720}, {1024, 848}};
    for (const auto& s : sizes) {
        const auto px = noiseBgra(s.w, s.h, s.w * 7 + s.h);
        RollThumbnail t;
        std::string err;
        KV_REQUIRE(makeRollThumbnail(px.data(), s.w, s.h, s.w * 4, t, &err));
        KV_CHECK(t.jpeg.size() <= kRollThumbMaxBytes);
        KV_CHECK(t.width <= kRollThumbMaxWidth);
        uint32_t dw = 0, dh = 0;
        KV_CHECK(decodeJpeg(t.jpeg, &dw, &dh) && dw == t.width && dh == t.height);
        if (t.quality < kRollThumbStartQuality) lowered = true;
        std::printf("  %ux%u noise -> %ux%u, %zu bytes at quality %.2f\n", s.w, s.h, t.width, t.height, t.jpeg.size(),
                    static_cast<double>(t.quality));
    }
    KV_CHECK(lowered);
}

KV_TEST(thumbnail_refuses_a_bad_frame) {
    Com com;
    RollThumbnail t;
    std::string err;
    std::vector<uint8_t> px(64 * 64 * 4, 128);
    KV_CHECK(!makeRollThumbnail(nullptr, 64, 64, 256, t, &err) && !err.empty());
    KV_CHECK(!makeRollThumbnail(px.data(), 0, 64, 256, t, &err));
    KV_CHECK(!makeRollThumbnail(px.data(), 64, 64, 200, t, &err)); // stride shorter than a row
    KV_CHECK(t.jpeg.empty());
}

KV_TEST_MAIN()
