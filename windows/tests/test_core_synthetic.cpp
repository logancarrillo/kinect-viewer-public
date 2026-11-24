// Core-on-Windows tests that run against the synthetic source only (no Kinect needed).
#include "kv_load.hpp"
#include "kv_test.hpp"
#include "kinect_core_c.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Frame {
    std::vector<uint8_t> bgra;
    kv_mapped_frame_t meta{};
};

// Polls until a frame newer than last_seq arrives (or timeout).
bool waitFrame(kv_engine_t* eng, uint64_t last_seq, Frame& out, int timeout_ms = 3000) {
    out.bgra.resize(1920 * 1080 * 4);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        size_t req = 0;
        if (kv_engine_copy_latest_frame(eng, last_seq, out.bgra.data(), out.bgra.size(), &req, &out.meta)) {
            return true;
        }
        if (req > out.bgra.size()) out.bgra.resize(req);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::string tempLock() {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + "kv-core-test-lock-" + std::to_string(GetCurrentProcessId());
}

} // namespace

KV_TEST(usb_presence_check_runs_quickly_without_opening_anything) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool present = kv_is_kinect_usb_present();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  kinect_usb_present=%s (%lld ms)\n", present ? "yes" : "no", static_cast<long long>(ms));
    KV_CHECK(ms < 2000);
}

// The synthetic source at 30 fps: frames at the right size, no sequence drops, every clock barcode valid, a rate in
// 27-33 fps, all checked in every run. That 95 % of the gaps between frames stay under 40 ms (33.3 ms nominal) is a
// timing floor, judged against the PC's load: a busy PC delays the source's thread now and then with nothing wrong in it.
KV_TEST(synthetic_ir_streams_30fps_with_valid_clock_barcode) {
    kvtest::LoadSampler load;
    const std::string lock = tempLock();
    kv_engine_t* eng = kv_engine_create(lock.c_str());
    KV_REQUIRE(eng != nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = KV_MODE_IR;
    p.clock_overlay = true;
    char err[256] = {0};
    KV_REQUIRE(kv_engine_start(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, err, sizeof(err)));

    int checked = 0, valid = 0;
    uint64_t last = 0;
    Frame f;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < end) {
        if (!waitFrame(eng, last, f)) break;
        last = f.meta.frame_seq;
        KV_CHECK(f.meta.width == 512 && f.meta.height == 424);
        uint32_t seq24 = 0, ts = 0;
        ++checked;
        if (kv_decode_clock_barcode(f.bgra.data(), f.meta.width, f.meta.height, f.meta.stride_bytes, 4, &seq24, &ts) &&
            seq24 == static_cast<uint32_t>(f.meta.frame_seq & 0xFFFFFF) &&
            ts == static_cast<uint32_t>((f.meta.arrival_mach_ns / 1000ULL) & 0xFFFFFFFFULL)) {
            ++valid;
        }
    }
    kv_stats_snapshot_t st;
    kv_engine_get_stats(eng, &st);
    const kvtest::LoadSnapshot timed = load.snapshot();
    std::printf("  ir_frames=%llu fps=%.2f p95=%.1f ms barcode=%d/%d drops=%llu\n  %s\n",
                static_cast<unsigned long long>(st.ir_frames), st.current_fps, st.inter_frame_p95_ms, valid, checked,
                static_cast<unsigned long long>(st.ir_seq_drops), timed.r.line().c_str());
    KV_CHECK(st.running);
    KV_CHECK(st.source_kind == KV_SOURCE_SYNTHETIC);
    KV_CHECK(st.ir_frames >= 60);
    KV_CHECK(st.current_fps > 27.0 && st.current_fps < 33.0);
    KV_CHECK(st.ir_seq_drops == 0);
    KV_FLOOR(st.inter_frame_p95_ms < 40.0, timed, "inter-frame p95 " + std::to_string(st.inter_frame_p95_ms) + " ms");
    KV_CHECK(checked >= 50);
    KV_CHECK(valid == checked);
    kv_engine_stop(eng);
    KV_CHECK(!kv_engine_is_running(eng));
    kv_engine_destroy(eng);
}

KV_TEST(synthetic_all_modes_have_expected_dimensions) {
    const std::string lock = tempLock();
    kv_engine_t* eng = kv_engine_create(lock.c_str());
    KV_REQUIRE(eng != nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = KV_MODE_IR;
    char err[256] = {0};
    KV_REQUIRE(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, true, err, sizeof(err)));
    KV_CHECK(kv_engine_is_rgb_active(eng));

    struct Want {
        kv_view_mode_t mode;
        kv_sbs_layout_t sbs;
        uint32_t w, h;
    };
    const Want wants[] = {
        {KV_MODE_IR, KV_SBS_IR_DEPTH, 512, 424},
        {KV_MODE_DEPTH, KV_SBS_IR_DEPTH, 512, 424},
        {KV_MODE_COLOR, KV_SBS_IR_DEPTH, 1920, 1080},
        {KV_MODE_SIDE_BY_SIDE, KV_SBS_IR_DEPTH, 1024, 424},
        {KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_DEPTH, 1280, 424},
        {KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_IR, 1280, 424},
    };
    uint64_t last = 0;
    Frame f;
    for (const auto& w : wants) {
        p.mode = w.mode;
        p.sbs_layout = w.sbs;
        kv_engine_set_display_params(eng, &p);
        // Skip any frame rendered before the switch.
        bool ok = false;
        for (int i = 0; i < 5 && !ok; ++i) {
            if (!waitFrame(eng, last, f)) break;
            last = f.meta.frame_seq;
            ok = (f.meta.mode == w.mode && f.meta.width == w.w && f.meta.height == w.h);
        }
        KV_CHECK_MSG(ok, "mode " + std::to_string(w.mode) + " sbs " + std::to_string(w.sbs) + " got " +
                             std::to_string(f.meta.width) + "x" + std::to_string(f.meta.height));
    }
    kv_engine_destroy(eng);
}

// The core's contract changed with kv_engine_start_ex: the
// synthetic source takes no lock at the default path (~\KINECT-IN-USE), but does take a custom lock
// path it was created with, so lock handling can be tested without a sensor.
KV_TEST(synthetic_source_never_takes_the_kinect_lock) {
    char home[MAX_PATH] = {0};
    GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH);
    const std::string real_lock = std::string(home) + "\\KINECT-IN-USE";
    // Its content and write time, so a lock another program holds right now can be checked untouched.
    auto snapshot = [&](std::string& content, FILETIME& written) -> bool {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExA(real_lock.c_str(), GetFileExInfoStandard, &fa)) return false;
        written = fa.ftLastWriteTime;
        std::ifstream in(real_lock, std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    };
    std::string before_content, after_content;
    FILETIME before_time{}, after_time{};
    const bool real_existed = snapshot(before_content, before_time);
    kv_engine_t* eng = kv_engine_create(nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    char err[256] = {0};
    KV_REQUIRE(kv_engine_start(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, err, sizeof(err)));
    const bool real_exists_now = snapshot(after_content, after_time);
    if (!real_existed) {
        KV_CHECK(!real_exists_now); // not created
    } else {
        // Someone else's lock: still there, same bytes, not rewritten.
        KV_CHECK(real_exists_now);
        KV_CHECK(after_content == before_content);
        KV_CHECK(CompareFileTime(&after_time, &before_time) == 0);
    }
    kv_stats_snapshot_t st;
    kv_engine_get_stats(eng, &st);
    // The stats say whether another program holds the Kinect, whichever source runs here.
    char owner[256] = {0};
    KV_CHECK(st.lock_blocked == kv_is_locked_by_other(nullptr, owner, sizeof(owner)));
    kv_engine_stop(eng);
    if (real_existed) KV_CHECK(GetFileAttributesA(real_lock.c_str()) != INVALID_FILE_ATTRIBUTES); // not released either
    kv_engine_destroy(eng);
}

KV_TEST(synthetic_source_takes_a_custom_lock_path_and_releases_it_on_stop) {
    const std::string lock = tempLock();
    DeleteFileA(lock.c_str());
    kv_engine_t* eng = kv_engine_create(lock.c_str());
    kv_display_params_t p;
    kv_default_display_params(&p);
    char err[256] = {0};
    KV_REQUIRE(kv_engine_start(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, err, sizeof(err)));
    KV_CHECK(GetFileAttributesA(lock.c_str()) != INVALID_FILE_ATTRIBUTES);
    kv_engine_stop(eng);
    KV_CHECK(GetFileAttributesA(lock.c_str()) == INVALID_FILE_ATTRIBUTES);
    kv_engine_destroy(eng);
}

KV_TEST(color_controls_round_trip_on_synthetic) {
    const std::string lock = tempLock();
    kv_engine_t* eng = kv_engine_create(lock.c_str());
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = KV_MODE_COLOR;
    char err[256] = {0};
    KV_REQUIRE(kv_engine_start(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, err, sizeof(err)));
    kv_color_controls_t c;
    kv_default_color_controls(&c);
    c.mode = KV_EXPOSURE_MANUAL;
    c.manual_integration_ms = 20.0f;
    c.manual_analog_gain = 2.5f;
    KV_CHECK(kv_engine_set_color_controls(eng, &c, err, sizeof(err)));
    kv_stats_snapshot_t st;
    kv_engine_get_stats(eng, &st);
    KV_CHECK(st.readback_exposure_mode == 4);
    KV_CHECK(st.readback_integration_ms == 20.0f);
    KV_CHECK(st.readback_analog_gain == 2.5f);
    kv_engine_destroy(eng);
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    return ::kvtest::runAll(argc, argv);
}
