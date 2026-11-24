// Media Foundation H.264 encoder tests on synthetic frames: AVCC repacking, SPS/PPS extraction, GOP 30,
// no frame reordering, and a full encode -> decode -> clock-barcode round trip.
#include "kv_load.hpp"
#include "kv_test.hpp"

#include "frame_mapper.hpp"
#include "kinect_core_c.h"
#include "mf_h264_encoder.hpp"
#include "test_h264_decoder.hpp"
#include "video_convert.hpp"

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace kvhost;

namespace {

struct Out {
    std::vector<uint8_t> annexb;
    int64_t pts;
    uint64_t token;
    uint64_t done_ns;
};

std::vector<uint8_t> makeBgra(uint32_t w, uint32_t h, uint64_t seq, uint64_t ts_ns, bool overlay) {
    std::vector<uint8_t> f(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &f[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>((x + seq * 3) & 0xFF);
            p[1] = static_cast<uint8_t>((y * 2) & 0xFF);
            p[2] = static_cast<uint8_t>(((x ^ y) + seq) & 0xFF);
            p[3] = 255;
        }
    }
    if (overlay) kv::FrameMapper::drawClockOverlay(f.data(), w, h, seq, ts_ns);
    return f;
}

struct Collector {
    std::mutex m;
    std::condition_variable cv;
    std::vector<Out> outs;
    void push(const uint8_t* d, size_t n, int64_t pts, uint64_t token) {
        std::lock_guard<std::mutex> lk(m);
        outs.push_back({std::vector<uint8_t>(d, d + n), pts, token, kv_monotonic_ns()});
        cv.notify_all();
    }
    bool waitFor(size_t count, int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return outs.size() >= count; });
    }
    size_t size() {
        std::lock_guard<std::mutex> lk(m);
        return outs.size();
    }
};

} // namespace

KV_TEST(nv12_conversion_and_upscale_sizes) {
    uint32_t w = 0, h = 0;
    bool up = false;
    streamTargetSize(512, 424, w, h, up);
    KV_CHECK(up && w == 1024 && h == 848);
    streamTargetSize(1920, 1080, w, h, up);
    KV_CHECK(!up && w == 1920 && h == 1080);
    streamTargetSize(1281, 425, w, h, up);
    KV_CHECK(!up && w == 1280 && h == 424);

    std::vector<uint8_t> white(512 * 424 * 4, 255), nv12;
    bgraToNv12(white.data(), 512, 424, 512 * 4, true, nv12, w, h);
    KV_CHECK(w == 1024 && h == 848 && nv12.size() == 1024u * 848u * 3u / 2u);
    KV_CHECK(nv12[0] == 235 && nv12[1024 * 848] == 128 && nv12[1024 * 848 + 1] == 128);
    std::vector<uint8_t> black(4 * 2 * 4, 0);
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
    bgraToNv12(black.data(), 4, 2, 16, false, nv12, w, h);
    KV_CHECK(nv12[0] == 16);
}

KV_TEST(annexb_to_avcc_moves_parameter_sets_out) {
    const uint8_t au[] = {0, 0, 0, 1, 0x09, 0xF0,                    // AUD
                          0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1F,        // SPS
                          0, 0, 1, 0x68, 0xEE, 0x3C, 0x80,           // PPS (3-byte start code)
                          0, 0, 0, 1, 0x06, 0x05, 0x01, 0x80,        // SEI
                          0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21, 0xA0}; // IDR slice
    AvccAccessUnit a;
    KV_REQUIRE(annexBToAvcc(au, sizeof(au), a));
    KV_CHECK(a.idr);
    KV_CHECK(a.sps == (std::vector<uint8_t>{0x67, 0x64, 0x00, 0x1F}));
    KV_CHECK(a.pps == (std::vector<uint8_t>{0x68, 0xEE, 0x3C, 0x80}));
    KV_CHECK(a.nal_count == 2);
    const std::vector<uint8_t> want = {0, 0, 0, 4, 0x06, 0x05, 0x01, 0x80, 0, 0, 0, 5, 0x65, 0x88, 0x84, 0x21, 0xA0};
    KV_CHECK(a.avcc == want);
}

// What the encoder promises is checked in every run: the first access unit an IDR, SPS and PPS, no reordering, an IDR
// at least every 30 encoded frames, and every decoded frame's clock barcode right. How many of the 90 frames come back
// is a timing floor (KV_FLOOR, judged against the PC's load): a 30 fps source outruns a hardware MFT that asks for input
// late, the latest frame then wins (MfH264Encoder::encode), and fewer frames come out with nothing wrong in the
// encoder. So the GOP is counted in frames the encoder encoded (output order), not in submitted tokens: a frame
// replaced before the MFT asked for it never reached its GOP counter.
KV_TEST(encode_ir_stream_gop30_no_reorder_and_barcode_round_trip) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    kvtest::LoadSampler load;
    MfH264Encoder enc;
    Collector col;
    EncoderOptions opts;
    std::string err;
    const bool opened = enc.open(1024, 848, opts,
                                 [&](const uint8_t* d, size_t n, int64_t pts, uint64_t token) { col.push(d, n, pts, token); }, &err);
    KV_REQUIRE(opened);
    std::printf("  encoder: %s (%s)\n", enc.name().c_str(), enc.isHardware() ? "hardware" : "software");

    const int kFrames = 90;
    std::vector<uint64_t> submit_ns(kFrames), capture_ns(kFrames);
    std::vector<uint8_t> nv12;
    for (int i = 0; i < kFrames; ++i) {
        capture_ns[i] = kv_monotonic_ns();
        auto bgra = makeBgra(512, 424, static_cast<uint64_t>(i + 1), capture_ns[i], true);
        uint32_t w = 0, h = 0;
        bgraToNv12(bgra.data(), 512, 424, 512 * 4, true, nv12, w, h);
        submit_ns[i] = kv_monotonic_ns();
        KV_CHECK(enc.encode(nv12.data(), nv12.size(), static_cast<int64_t>(i) * 333333, i == 0, static_cast<uint64_t>(i), &err));
        // Pace like a 30 fps source so an async hardware MFT can ask for each frame.
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    col.waitFor(kFrames, 3000);
    const kvtest::LoadSnapshot timed = load.snapshot(); // the stretch the floors time ends here
    const std::vector<uint8_t> seq_hdr = enc.sequenceHeader();
    enc.close();

    std::lock_guard<std::mutex> lk(col.m);
    std::printf("  outputs=%zu of %d\n  %s\n", col.outs.size(), kFrames, timed.r.line().c_str());
    KV_REQUIRE(!col.outs.empty());
    // Async MFTs may hold the last frame or two.
    KV_FLOOR(col.outs.size() >= static_cast<size_t>(kFrames - 2), timed,
             std::to_string(col.outs.size()) + " of " + std::to_string(kFrames) + " frames came back");

    std::vector<uint8_t> sps, pps;
    std::vector<int> key_tokens;
    std::vector<size_t> key_at; // the IDRs' places in output order
    int64_t last_pts = -1;
    bool ordered = true;
    double lat_sum = 0, lat_max = 0;
    kvtest::TestH264Decoder dec;
    KV_REQUIRE(dec.open());
    int decoded = 0, barcode_ok = 0;
    for (size_t k = 0; k < col.outs.size(); ++k) {
        const Out& o = col.outs[k];
        AvccAccessUnit au;
        KV_REQUIRE(annexBToAvcc(o.annexb.data(), o.annexb.size(), au));
        if (!au.sps.empty()) sps = au.sps;
        if (!au.pps.empty()) pps = au.pps;
        if (au.idr) {
            key_tokens.push_back(static_cast<int>(o.token));
            key_at.push_back(k);
        }
        if (o.pts <= last_pts) ordered = false;
        last_pts = o.pts;
        if (k == 0) KV_CHECK_MSG(au.idr, "first access unit must be an IDR");
        const double lat = static_cast<double>(o.done_ns - submit_ns[o.token]) / 1e6;
        lat_sum += lat;
        lat_max = std::max(lat_max, lat);

        if (sps.empty() && !seq_hdr.empty()) {
            for (auto& nal : splitAnnexB(seq_hdr.data(), seq_hdr.size())) {
                if ((nal[0] & 0x1F) == 7) sps = nal;
                if ((nal[0] & 0x1F) == 8) pps = nal;
            }
        }
        // Decode the AVCC exactly as sent on the wire.
        const auto annexb = kvtest::avccToAnnexB(au.avcc.data(), au.avcc.size(),
                                                  au.idr ? std::vector<std::vector<uint8_t>>{sps, pps}
                                                         : std::vector<std::vector<uint8_t>>{});
        if (dec.decode(annexb.data(), annexb.size())) {
            ++decoded;
            uint32_t seq24 = 0, ts = 0;
            if (kv_decode_clock_barcode(dec.luma().data(), dec.width(), dec.height(), dec.width(), 1, &seq24, &ts) &&
                seq24 == static_cast<uint32_t>(o.token + 1) &&
                ts == static_cast<uint32_t>((capture_ns[o.token] / 1000ULL) & 0xFFFFFFFFULL)) {
                ++barcode_ok;
            }
        }
    }
    std::printf("  sps=%zu B pps=%zu B keyframes at tokens:", sps.size(), pps.size());
    for (int t : key_tokens) std::printf(" %d", t);
    std::printf(", at outputs:");
    for (size_t a : key_at) std::printf(" %zu", a);
    std::printf("\n  submit->output latency mean=%.2f ms max=%.2f ms; decoded=%d barcode_ok=%d\n",
                col.outs.empty() ? 0.0 : lat_sum / col.outs.size(), lat_max, decoded, barcode_ok);
    KV_CHECK(!sps.empty() && !pps.empty());
    KV_CHECK((sps[0] & 0x1F) == 7 && (pps[0] & 0x1F) == 8);
    KV_CHECK(ordered); // no B-frames: output order == input order
    // GOP 30: an IDR at least every 30 encoded frames, the last stretch included (90 back means IDRs at 0, 30 and 60).
    KV_REQUIRE(!key_at.empty() && key_at.front() == 0);
    for (size_t i = 1; i < key_at.size(); ++i) KV_CHECK(key_at[i] - key_at[i - 1] <= 30);
    const size_t tail = col.outs.size() - key_at.back();
    KV_CHECK_MSG(tail <= 30, "no IDR in the last " + std::to_string(tail) + " frames");
    KV_CHECK(decoded >= static_cast<int>(col.outs.size()) - 2);
    KV_CHECK(barcode_ok >= decoded - 1);
    KV_FLOOR(barcode_ok >= kFrames - 4, timed,
             std::to_string(barcode_ok) + " of " + std::to_string(kFrames) + " barcodes round-tripped");
}

// Each size opens and encodes: a frame comes back (within a generous 10 s), in every run. That 4 of the 6 frames are
// back within 2 s of the last is a timing floor, judged against the PC's load.
KV_TEST(encoder_opens_colour_and_side_by_side_sizes) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    kvtest::LoadSampler load;
    const uint32_t sizes[][2] = {{1920, 1080}, {1280, 424}, {1024, 424}};
    for (const auto& s : sizes) {
        MfH264Encoder enc;
        Collector col;
        EncoderOptions opts;
        opts.bitrate = s[0] >= 1600 ? 10000000u : 5000000u;
        std::string err;
        KV_REQUIRE(enc.open(s[0], s[1], opts, [&](const uint8_t* d, size_t n, int64_t p, uint64_t t) { col.push(d, n, p, t); }, &err));
        std::vector<uint8_t> nv12;
        for (int i = 0; i < 6; ++i) {
            auto bgra = makeBgra(s[0], s[1], i + 1, kv_monotonic_ns(), true);
            uint32_t w = 0, h = 0;
            bgraToNv12(bgra.data(), s[0], s[1], s[0] * 4, false, nv12, w, h);
            KV_CHECK(enc.encode(nv12.data(), nv12.size(), i * 333333LL, i == 0, i, &err));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        const std::string size = std::to_string(s[0]) + "x" + std::to_string(s[1]);
        const bool four = col.waitFor(4, 2000);
        const kvtest::LoadSnapshot timed = load.snapshot();
        const size_t back = col.size();
        KV_CHECK_MSG(col.waitFor(1, 10000), size + ": no frame came back in 10 s");
        std::printf("  %s: %zu of 6 back within 2 s; %s\n", size.c_str(), back, timed.r.line().c_str());
        KV_FLOOR(four, timed, size + ": " + std::to_string(back) + " of 6 frames back within 2 s");
        enc.close();
    }
}

// Closing an encoder parks its COM objects and releases them only once they have been parked for at least 3 s, never at
// once (mf_h264_encoder.cpp, parkEncoder): the hardware MFT may still have work queued right after it was shut down.
// A then B, closed one right after the other: both are still parked after B's close, since neither is 3 s old. Opening
// an encoder also parks the candidates that failed to open (another vendor's MFT), and any close may release entries
// left by earlier tests once they are over 3 s old, so the test checks what must still be parked, not that nothing was
// released. 3.2 s later, C's close releases A and B.
KV_TEST(a_closed_encoder_is_released_seconds_later_never_at_once) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto openAndClose = [] {
        MfH264Encoder enc;
        EncoderOptions opts;
        std::string err;
        KV_REQUIRE(enc.open(640, 360, opts, [](const uint8_t*, size_t, int64_t, uint64_t) {}, &err));
        enc.close();
    };
    openAndClose(); // A (its close may release encoders parked by earlier tests, over 3 s ago)
    const EncoderCloseStats a = encoderCloseStats();
    openAndClose(); // B, well within 3 s of A
    const EncoderCloseStats b = encoderCloseStats();
    KV_CHECK(b.parked >= a.parked + 1);
    KV_CHECK_MSG(b.parkedNow >= 2, "A or B was released under 3 s after it was parked"); // A and B at least
    std::this_thread::sleep_for(std::chrono::milliseconds(3200)); // the park time itself, not a race: >= 3 s is the rule
    openAndClose(); // C
    const EncoderCloseStats c = encoderCloseStats();
    std::printf("  parked %llu, released %llu, parked now %zu, drain timeouts %llu\n", static_cast<unsigned long long>(c.parked),
                static_cast<unsigned long long>(c.released), c.parkedNow, static_cast<unsigned long long>(c.drainTimeouts));
    KV_CHECK_MSG(c.released >= b.released + 2, "A and B, parked over 3 s ago, weren't released by the next close");
    KV_CHECK(c.parkedNow >= 1); // C
}

KV_TEST_MAIN()
