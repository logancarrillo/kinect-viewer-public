// kinect-cli.exe: Windows command-line test tool for the kinect-viewer core.
// The Mac's kinect-cli is Swift; this is a new C++ CLI with the same verbs where they make sense.
//
//   kinect-cli list                       Kinect presence, libfreenect2 enumeration, lock status
//   kinect-cli run   [options]            Stream for --duration seconds (default 5) and print a summary
//   kinect-cli bench [options]            60 s benchmark with a PASS/FAIL verdict (real sensor by default)
//   kinect-cli verify-lock                Lock-guard self-test on a temp lock file (no device opened)
//   kinect-cli verify-no-fallback         Ask for a pipeline this build lacks and check it fails cleanly
//   kinect-cli serve [--pair] [options]   Headless iPhone streaming host; --pair opens a 120 s window.
//                                         --state-dir DIR keeps its pairing and pet-camera state there; a
//                                         --synthetic or --loopback-only serve without it uses a temp folder.
//                                         --camera-roll-dir DIR records the camera roll there; without it the
//                                         roll is off (there is no default folder here, v3 §10). In a test run
//                                         (--synthetic or --loopback-only) the folder must be new, empty or a test
//                                         roll, and never a real folder by its path. --until-roll-prepared
//                                         ends serve once the roll's folder is prepared (exit 4 if --duration
//                                         runs out first): a test waits on that, not on a fixed duration.
//                                         --outside-port N listens outside the home on port N too, when
//                                         sharing is on in pet-camera.json; a test run (--synthetic or
//                                         --loopback-only) binds it to 127.0.0.1 and never uses the router.
//                                         --until-outside-reported ends serve once the outside status is a
//                                         result (not "checking") and printed, or outside listening was refused
//                                         (exit 4 if --duration runs out first): a test waits on that.
//   kinect-cli record [options]           Host-only recording for --duration seconds (default 10)
//   kinect-cli router-probe --outside-port N
//                                         Read-only: the v3 §8.3 gateway read, once, against this PC's router,
//                                         and how a new public address would be judged (Confirmed, Contradicted,
//                                         Unreadable, or no pin). It changes nothing: no settings, no state folder,
//                                         and no port mapping added or removed (NATUPnP is only asked for the
//                                         forward already on port N). Every address, hardware address and device id
//                                         it prints is masked; service types and URL paths are kept, and may reveal
//                                         the router's make or model.
//
// Options: --synthetic | --real, --duration S, --pipeline gl|cpu|cl, --mode ir|depth|color|sbs,
//          --sbs-layout ir-depth|color-depth|color-ir, --colormap turbo|viridis|inferno|gray,
//          --depth-min MM, --depth-max MM, --verify-barcode, --test-exposure, --json PATH, --show-serial
// Mac-style flags --list-devices, --benchmark, --verify-lock and --verify-no-fallback are accepted too.

#include "kinect_core_c.h"

#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/logger.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

#ifdef KV_WITH_RECORDER
#include "recorder.hpp"
#include "wire_protocol.hpp" // text::kColourLiveOnly
#endif
#ifdef KV_WITH_STREAMING
#include "host_controller.hpp"
#include "host_power.hpp"
#include "router_mapper.hpp" // router-probe
#include "viewer_model.hpp" // normalizeStateDir, stateDirAcceptable, freshTemporaryStateDir (v3 §10)
#include <shellapi.h>       // CommandLineToArgvW
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string verb = "run";
    bool synthetic = false;
    bool source_explicit = false;
    double duration_sec = 5.0;
    bool duration_explicit = false;
    kv_pipeline_kind_t pipeline = KV_PIPELINE_OPENGL;
    kv_view_mode_t mode = KV_MODE_IR;
    kv_sbs_layout_t sbs = KV_SBS_IR_DEPTH;
    kv_colormap_kind_t colormap = KV_COLORMAP_TURBO;
    float depth_min = 500.0f;
    float depth_max = 4500.0f;
    bool verify_barcode = false;
    bool test_exposure = false;
    bool show_serial = false;
    std::string json_path;
    bool pair = false;
    bool pair_owner = false; // serve --pair-role owner (the PC's default is viewer, protocol v3 D1)
    bool pair_code = false;  // serve --pair-code: the window also takes the 8-digit code (QR only otherwise)
    bool loopback_only = false;
    bool clock_overlay = false;
    bool png16 = false;
    std::string out_dir;
    std::string lock_path; // list: the lock file to check (default ~\\KINECT-IN-USE); tests use a temp one
    // serve: where the host keeps pairing.json, devices.json and pet-camera.json. Every verb takes it, so
    // the tests can pass one everywhere; only serve reads it.
    std::string state_dir;
    int state_dir_arg = -1; // its index in argv, to read it again from the wide command line
    // serve: the camera-roll folder. Without it the roll is off: serve never picks a folder of its own,
    // so a test can't reach the real roll (protocol v3 §10 "Test isolation").
    std::string camera_roll_dir;
    int camera_roll_dir_arg = -1;
    bool until_roll_prepared = false; // serve: exit 0 once the roll folder is prepared; --duration is the deadline
    bool until_outside_reported = false; // serve: exit 0 once the outside status is a result; --duration is the deadline
    // serve --outside-port N (v3 §8.1): turn outside listening on, on this port, for the host's own state. 0: as
    // pet-camera.json says.
    int outside_port = 0;
};

const char* pipelineName(kv_pipeline_kind_t p) {
    switch (p) {
        case KV_PIPELINE_OPENGL: return "gl";
        case KV_PIPELINE_OPENCL: return "cl";
        case KV_PIPELINE_CPU: return "cpu";
    }
    return "?";
}

const char* modeName(kv_view_mode_t m) {
    switch (m) {
        case KV_MODE_IR: return "ir";
        case KV_MODE_COLOR: return "color";
        case KV_MODE_DEPTH: return "depth";
        case KV_MODE_SIDE_BY_SIDE: return "sbs";
    }
    return "?";
}

void usage() {
    std::fputs(
        "usage: kinect-cli <list|run|bench|verify-lock|verify-no-fallback> [options]\n"
        "  --synthetic | --real         source (run: synthetic only if asked; bench: real unless --synthetic)\n"
        "  --duration S                 seconds to stream (run default 5, bench default 60)\n"
        "  --pipeline gl|cpu|cl         libfreenect2 depth pipeline; never falls back silently\n"
        "  --mode ir|depth|color|sbs    view mode; --sbs-layout ir-depth|color-depth|color-ir\n"
        "  --colormap turbo|viridis|inferno|gray, --depth-min MM, --depth-max MM\n"
        "  --verify-barcode             synthetic only: draw and decode the clock barcode every frame\n"
        "  --test-exposure              apply auto / semi-auto / manual colour exposure and print readback\n"
        "  --json PATH                  also write the summary as JSON\n"
        "  --show-serial                print the sensor serial (hidden by default)\n"
        "  list: --lock-path PATH       check this lock file instead of %USERPROFILE%\\KINECT-IN-USE (tests)\n"
        "  --state-dir DIR              serve: host state here (tests); --synthetic or --loopback-only without it\n"
        "  --camera-roll-dir DIR        serve: record the camera roll here; without it the roll is off\n"
        "  --until-roll-prepared        serve: stop once the roll folder is prepared (exit 4 at --duration first)\n"
        "  --until-outside-reported     serve: stop once the outside status is printed as a result (exit 4 at --duration first)\n"
        "  --outside-port N             serve: listen outside the home on N (1024-65535) too; needs sharing on.\n"
        "                               A test run binds it to 127.0.0.1 and never uses the router\n"
        "                               uses a temp folder, never %USERPROFILE%\\kinect-viewer\\.host-state.\n"
        "                               DIR must be new, empty, or hold host state already (never a drive root)\n"
        "  serve: --pair (open the 120 s pairing window, QR only), --pair-code (it also takes the 8-digit code),\n"
        "         --pair-role owner|viewer (default viewer),\n"
        "         --clock-overlay (synthetic latency barcode),\n"
        "         --loopback-only (127.0.0.1, no mDNS), --duration S (0 = until Ctrl+C)\n"
        "  router-probe --outside-port N: read-only; how this PC's router would judge a new public address.\n"
        "         Changes nothing (no settings, no mapping added or removed); every address it prints is masked\n"
        "  record: --synthetic only (the Kinect is recorded in the viewer, after its reminder); --mode ir|depth (16-bit FFV1\n"
        "    .mkv, or --png16); colour is live only and refused, --duration S,\n"
        "          --out DIR (default %USERPROFILE%\\kinect-viewer-recordings)\n",
        stderr);
}

bool parseArgs(int argc, char** argv, Options& o) {
    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        o.verb = argv[i++];
    }
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--synthetic") { o.synthetic = true; o.source_explicit = true; }
        else if (a == "--real") { o.synthetic = false; o.source_explicit = true; }
        else if (a == "--duration") { o.duration_sec = std::atof(next("--duration").c_str()); o.duration_explicit = true; }
        else if (a == "--pipeline") {
            const std::string v = next("--pipeline");
            if (v == "gl" || v == "opengl") o.pipeline = KV_PIPELINE_OPENGL;
            else if (v == "cl" || v == "opencl") o.pipeline = KV_PIPELINE_OPENCL;
            else if (v == "cpu") o.pipeline = KV_PIPELINE_CPU;
            else { std::fprintf(stderr, "Unknown pipeline: %s\n", v.c_str()); return false; }
        } else if (a == "--mode") {
            const std::string v = next("--mode");
            if (v == "ir") o.mode = KV_MODE_IR;
            else if (v == "depth") o.mode = KV_MODE_DEPTH;
            else if (v == "color" || v == "colour") o.mode = KV_MODE_COLOR;
            else if (v == "sbs" || v == "side-by-side") o.mode = KV_MODE_SIDE_BY_SIDE;
            else { std::fprintf(stderr, "Unknown mode: %s\n", v.c_str()); return false; }
        } else if (a == "--sbs-layout") {
            const std::string v = next("--sbs-layout");
            if (v == "ir-depth") o.sbs = KV_SBS_IR_DEPTH;
            else if (v == "color-depth") o.sbs = KV_SBS_COLOR_DEPTH;
            else if (v == "color-ir") o.sbs = KV_SBS_COLOR_IR;
            else { std::fprintf(stderr, "Unknown sbs layout: %s\n", v.c_str()); return false; }
        } else if (a == "--colormap") {
            const std::string v = next("--colormap");
            if (v == "turbo") o.colormap = KV_COLORMAP_TURBO;
            else if (v == "viridis") o.colormap = KV_COLORMAP_VIRIDIS;
            else if (v == "inferno") o.colormap = KV_COLORMAP_INFERNO;
            else if (v == "gray" || v == "grayscale") o.colormap = KV_COLORMAP_GRAY;
            else { std::fprintf(stderr, "Unknown colormap: %s\n", v.c_str()); return false; }
        } else if (a == "--depth-min") o.depth_min = static_cast<float>(std::atof(next("--depth-min").c_str()));
        else if (a == "--depth-max") o.depth_max = static_cast<float>(std::atof(next("--depth-max").c_str()));
        else if (a == "--verify-barcode") o.verify_barcode = true;
        else if (a == "--test-exposure") o.test_exposure = true;
        else if (a == "--show-serial") o.show_serial = true;
        else if (a == "--json") o.json_path = next("--json");
        else if (a == "--pair") o.pair = true;
        else if (a == "--pair-code") o.pair_code = true;
        else if (a == "--pair-role") {
            const std::string r = next("--pair-role");
            if (r != "owner" && r != "viewer") {
                std::fprintf(stderr, "--pair-role must be owner or viewer\n");
                return false;
            }
            o.pair_owner = r == "owner";
        }
        else if (a == "--png16") o.png16 = true;
        else if (a == "--out") o.out_dir = next("--out");
        else if (a == "--lock-path") o.lock_path = next("--lock-path");
        else if (a == "--state-dir") {
            o.state_dir = next("--state-dir");
            o.state_dir_arg = i;
            if (o.state_dir.empty()) {
                std::fputs("--state-dir needs a folder\n", stderr);
                return false;
            }
        }
        else if (a == "--camera-roll-dir") {
            o.camera_roll_dir = next("--camera-roll-dir");
            o.camera_roll_dir_arg = i;
            if (o.camera_roll_dir.empty()) {
                std::fputs("--camera-roll-dir needs a folder\n", stderr);
                return false;
            }
        }
        else if (a == "--loopback-only") o.loopback_only = true;
        else if (a == "--outside-port") {
            const std::string v = next("--outside-port");
            char* end = nullptr;
            const long port = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || !end || *end != '\0' || port < 1024 || port > 65535) {
                std::fputs("--outside-port needs a port from 1024 to 65535\n", stderr);
                return false;
            }
            o.outside_port = static_cast<int>(port);
        }
        else if (a == "--until-roll-prepared") o.until_roll_prepared = true;
        else if (a == "--until-outside-reported") o.until_outside_reported = true;
        else if (a == "--clock-overlay") o.clock_overlay = true;
        else if (a == "--list-devices") o.verb = "list";
        else if (a == "--benchmark") o.verb = "bench";
        else if (a == "--verify-lock") o.verb = "verify-lock";
        else if (a == "--verify-no-fallback") o.verb = "verify-no-fallback";
        else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        else { std::fprintf(stderr, "Unknown option: %s\n", a.c_str()); return false; }
    }
    if (o.verb == "bench" && !o.duration_explicit) o.duration_sec = 60.0;
    if (o.verb == "serve" && !o.duration_explicit) o.duration_sec = 0.0; // until Ctrl+C
    if (o.verb == "record" && !o.duration_explicit) o.duration_sec = 10.0;
    if (o.verify_barcode && !o.synthetic) {
        std::fputs("--verify-barcode needs --synthetic (the barcode is drawn on synthetic frames only)\n", stderr);
        return false;
    }
    return true;
}

// ---- list -----------------------------------------------------------------------------------

int cmdList(const Options& o) {
    const bool present = kv_is_kinect_usb_present();
    std::printf("kinect_usb_present=%s   (SetupDi, VID_045E&PID_02C4; device not opened)\n", present ? "yes" : "no");

    char owner[256] = {0};
    const bool locked = kv_is_locked_by_other(o.lock_path.empty() ? nullptr : o.lock_path.c_str(), owner, sizeof(owner));
    std::printf("lock=%s%s%s\n", locked ? "held by another program" : "free", locked ? ": " : "", locked ? owner : "");
    if (locked) {
        std::puts("libfreenect2 enumeration skipped: the Kinect lock is held by someone else.");
        return 3;
    }
    if (!present) {
        std::puts("libfreenect2 enumeration skipped: no Kinect v2 on the USB bus.");
        return 0;
    }

    // Enumeration reads the USB serial descriptor; it does not start any stream.
    libfreenect2::setGlobalLogger(libfreenect2::createConsoleLogger(libfreenect2::Logger::Warning));
    libfreenect2::Freenect2 ctx;
    const int n = ctx.enumerateDevices();
    std::printf("libfreenect2_devices=%d\n", n);
    for (int i = 0; i < n; ++i) {
        const std::string serial = ctx.getDeviceSerialNumber(i);
        std::printf("  device[%d] serial=%s\n", i, o.show_serial ? serial.c_str() : "(hidden; pass --show-serial)");
    }
    if (n == 0) {
        std::puts("Kinect is on the bus but libfreenect2 cannot see it: check the driver is libusbK (Zadig).");
        return 5;
    }
    return 0;
}

// ---- verify-lock ----------------------------------------------------------------------------

int cmdVerifyLock() {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    const std::string path = std::string(dir) + "kv-cli-verify-lock-" + std::to_string(GetCurrentProcessId());
    DeleteFileA(path.c_str());

    // 1. A lock held by a live process blocks start without opening the device.
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::puts("LOCK_VERIFY=FAIL (cannot write temp lock)"); return 1; }
        std::fputs("agent: verify-lock - held by the System process\npid: 4\n", f);
        std::fclose(f);
    }
    kv_engine_t* eng = kv_engine_create(path.c_str());
    kv_display_params_t p;
    kv_default_display_params(&p);
    char err[256] = {0};
    const bool started = kv_engine_start(eng, KV_SOURCE_REAL, KV_PIPELINE_CPU, &p, err, sizeof(err));
    kv_stats_snapshot_t st;
    kv_engine_get_stats(eng, &st);
    kv_engine_destroy(eng);
    const bool blocked_ok = !started && st.lock_blocked;
    std::printf("LOCK_BLOCKS_START=%s (%s)\n", blocked_ok ? "true" : "false", err);
    DeleteFileA(path.c_str());

    // 2. A stale lock from a pid that no longer exists is reclaimed by a synthetic-free acquire check.
    {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        wchar_t cmd[] = L"cmd.exe /c exit 0";
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::puts("LOCK_VERIFY=FAIL (cannot spawn helper)");
            return 1;
        }
        WaitForSingleObject(pi.hProcess, 10000);
        const DWORD dead_pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        FILE* f = std::fopen(path.c_str(), "wb");
        std::fprintf(f, "agent: verify-lock - exited helper\npid: %lu\n", static_cast<unsigned long>(dead_pid));
        std::fclose(f);
    }
    char owner[256] = {0};
    const bool stale_seen_as_free = !kv_is_locked_by_other(path.c_str(), owner, sizeof(owner));
    std::printf("STALE_LOCK_IS_FREE=%s\n", stale_seen_as_free ? "true" : "false");
    DeleteFileA(path.c_str());

    const bool ok = blocked_ok && stale_seen_as_free;
    std::printf("LOCK_VERIFY=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- verify-no-fallback ---------------------------------------------------------------------

int cmdVerifyNoFallback() {
    if (!kv_is_kinect_usb_present()) {
        std::puts("NO_FALLBACK_VERIFY=SKIPPED (needs the real Kinect; the check is about libfreenect2 pipelines)");
        return 0;
    }
    char owner[256] = {0};
    if (kv_is_locked_by_other(nullptr, owner, sizeof(owner))) {
        std::printf("LOCK_BLOCKED: %s\n", owner);
        return 3;
    }
    // This libfreenect2 build has no OpenCL, so asking for it must fail without trying another pipeline.
    kv_engine_t* eng = kv_engine_create(nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    char err[256] = {0};
    const bool started = kv_engine_start(eng, KV_SOURCE_REAL, KV_PIPELINE_OPENCL, &p, err, sizeof(err));
    kv_engine_destroy(eng);
    const bool ok = !started && std::strstr(err, "OpenCL") != nullptr;
    std::printf("requested=cl started=%s error=\"%s\"\nNO_FALLBACK_VERIFY=%s\n", started ? "true" : "false", err,
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- run / bench ----------------------------------------------------------------------------

void printExposureReadback(kv_engine_t* eng) {
    kv_color_controls_t c;
    kv_default_color_controls(&c);
    const kv_exposure_mode_t modes[] = {KV_EXPOSURE_AUTO, KV_EXPOSURE_SEMI_AUTO, KV_EXPOSURE_MANUAL};
    const char* names[] = {"auto", "semi_auto", "manual"};
    for (int i = 0; i < 3; ++i) {
        c.mode = modes[i];
        char err[256] = {0};
        const bool ok = kv_engine_set_color_controls(eng, &c, err, sizeof(err));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        kv_stats_snapshot_t st;
        kv_engine_get_stats(eng, &st);
        std::printf("EXPOSURE_%s: applied=%s rb_mode=%u rb_int_ms=%.2f rb_gain=%.2f frame_exposure=%.2f frame_gain=%.2f %s\n",
                    names[i], ok ? "true" : "false", st.readback_exposure_mode, st.readback_integration_ms,
                    st.readback_analog_gain, st.color_exposure, st.color_gain, err);
    }
}

std::string jsonEscape(const char* s) {
    std::string out;
    for (; *s; ++s) {
        const char c = *s;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    return out;
}

int cmdRun(const Options& o, bool bench) {
    const bool synthetic = o.synthetic;
    if (!synthetic) {
        char owner[256] = {0};
        if (kv_is_locked_by_other(nullptr, owner, sizeof(owner))) {
            std::printf("LOCK_BLOCKED: %%USERPROFILE%%\\KINECT-IN-USE is held by another program (%s). Waiting without opening device.\n", owner);
            return 3;
        }
        if (!kv_is_kinect_usb_present()) {
            std::puts("NO_DEVICE: no Kinect v2 (VID_045E&PID_02C4) on the USB bus. Use --synthetic to test without it.");
            return 5;
        }
    }

    kv_engine_t* eng = kv_engine_create(nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = o.mode;
    p.sbs_layout = o.sbs;
    p.depth_colormap = o.colormap;
    p.depth_min_mm = o.depth_min;
    p.depth_max_mm = o.depth_max;
    p.clock_overlay = synthetic && o.verify_barcode;

    const bool want_rgb = o.test_exposure || o.mode == KV_MODE_COLOR ||
                          (o.mode == KV_MODE_SIDE_BY_SIDE && o.sbs != KV_SBS_IR_DEPTH);
    char err[256] = {0};
    if (!kv_engine_start_ex(eng, synthetic ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL, o.pipeline, &p, want_rgb, err,
                            sizeof(err))) {
        std::fprintf(stderr, "START_FAILED: %s\n", err);
        kv_engine_destroy(eng);
        return 1;
    }
    if (!synthetic) {
        std::printf("KINECT_LOCK_ACQUIRED=true\n");
    }
    std::printf("source=%s pipeline=%s mode=%s duration=%.0fs%s\n", synthetic ? "synthetic" : "real_kinect_v2",
                pipelineName(o.pipeline), modeName(o.mode), o.duration_sec, bench ? " (benchmark)" : "");
    std::fflush(stdout);

    if (o.test_exposure) {
        printExposureReadback(eng);
    }

    std::vector<uint8_t> buf(1920 * 1080 * 4);
    uint64_t last_seq = 0;
    kv_mapped_frame_t last_meta{};
    std::vector<double> slice_lat;
    int slice = 1;
    const double slice_len = 10.0;
    uint64_t bc_checked = 0, bc_valid = 0;
    double lat_sum = 0.0, lat_max = 0.0;
    uint64_t lat_n = 0;

    const uint64_t start = kv_monotonic_ns();
    const uint64_t end = start + static_cast<uint64_t>(o.duration_sec * 1e9);
    while (kv_monotonic_ns() < end) {
        size_t req = 0;
        kv_mapped_frame_t meta{};
        if (kv_engine_copy_latest_frame(eng, last_seq, buf.data(), buf.size(), &req, &meta)) {
            last_seq = meta.frame_seq;
            last_meta = meta;
            const uint64_t now = kv_monotonic_ns();
            const double lat = now >= meta.arrival_mach_ns ? static_cast<double>(now - meta.arrival_mach_ns) / 1e6 : 0.0;
            slice_lat.push_back(lat);
            lat_sum += lat;
            lat_max = std::max(lat_max, lat);
            ++lat_n;
            if (p.clock_overlay) {
                ++bc_checked;
                uint32_t s24 = 0, ts = 0;
                if (kv_decode_clock_barcode(buf.data(), meta.width, meta.height, meta.stride_bytes, 4, &s24, &ts) &&
                    s24 == static_cast<uint32_t>(meta.frame_seq & 0xFFFFFF) &&
                    ts == static_cast<uint32_t>((meta.arrival_mach_ns / 1000ULL) & 0xFFFFFFFFULL)) {
                    ++bc_valid;
                }
            }
        } else if (req > buf.size()) {
            buf.resize(req);
        }

        const double elapsed = static_cast<double>(kv_monotonic_ns() - start) / 1e9;
        if (o.duration_sec >= 20.0 && elapsed >= slice * slice_len) {
            kv_stats_snapshot_t st;
            kv_engine_get_stats(eng, &st);
            double m = 0.0, mx = 0.0;
            for (double v : slice_lat) { m += v; mx = std::max(mx, v); }
            if (!slice_lat.empty()) m /= static_cast<double>(slice_lat.size());
            std::printf("SLICE_%02ds: fps=%.2f ir_frames=%llu depth_frames=%llu color_frames=%llu drops=%llu/%llu/%llu "
                        "dt_mean_ms=%.2f dt_p95_ms=%.2f map_ms=%.3f consume_lat_mean_ms=%.3f consume_lat_max_ms=%.3f\n",
                        slice * static_cast<int>(slice_len), st.current_fps,
                        static_cast<unsigned long long>(st.ir_frames), static_cast<unsigned long long>(st.depth_frames),
                        static_cast<unsigned long long>(st.color_frames), static_cast<unsigned long long>(st.ir_seq_drops),
                        static_cast<unsigned long long>(st.depth_seq_drops),
                        static_cast<unsigned long long>(st.color_seq_drops), st.inter_frame_mean_ms,
                        st.inter_frame_p95_ms, st.map_time_mean_ms, m, mx);
            std::fflush(stdout);
            slice_lat.clear();
            ++slice;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    kv_stats_snapshot_t st;
    kv_engine_get_stats(eng, &st);
    const double wall = static_cast<double>(kv_monotonic_ns() - start) / 1e9;
    kv_engine_stop(eng);
    kv_engine_destroy(eng);
    if (!synthetic) {
        wchar_t home[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        const std::wstring lock = std::wstring(home, n < MAX_PATH ? n : 0) + L"\\KINECT-IN-USE";
        std::printf("KINECT_LOCK_RELEASED=%s\n", GetFileAttributesW(lock.c_str()) == INVALID_FILE_ATTRIBUTES ? "true" : "false");
    }

    double avg_rate = 0.0;
    if (o.mode == KV_MODE_COLOR) avg_rate = static_cast<double>(st.color_frames) / std::max(0.001, wall);
    else if (o.mode == KV_MODE_DEPTH) avg_rate = static_cast<double>(st.depth_frames) / std::max(0.001, wall);
    else avg_rate = static_cast<double>(st.rendered_frames) / std::max(0.001, wall);
    const unsigned long long drops = st.ir_seq_drops + st.depth_seq_drops + (o.mode == KV_MODE_COLOR ? st.color_seq_drops : 0);

    std::puts("=== SUMMARY ===");
    std::printf("source=%s mode=%s pipeline=%s output=%ux%u\n", synthetic ? "synthetic" : "real_kinect_v2",
                modeName(o.mode), pipelineName(o.pipeline), last_meta.width, last_meta.height);
    std::printf("serial=%s firmware=%s\n", o.show_serial ? st.serial : "(hidden)", st.firmware);
    std::printf("wall_sec=%.3f avg_fps=%.2f rolling_fps=%.2f color_fps=%.2f\n", wall, avg_rate, st.current_fps, st.color_fps);
    std::printf("ir_frames=%llu depth_frames=%llu color_frames=%llu rendered_frames=%llu\n",
                static_cast<unsigned long long>(st.ir_frames), static_cast<unsigned long long>(st.depth_frames),
                static_cast<unsigned long long>(st.color_frames), static_cast<unsigned long long>(st.rendered_frames));
    std::printf("ir_seq_drops=%llu depth_seq_drops=%llu color_seq_drops=%llu\n",
                static_cast<unsigned long long>(st.ir_seq_drops), static_cast<unsigned long long>(st.depth_seq_drops),
                static_cast<unsigned long long>(st.color_seq_drops));
    std::printf("dt_mean_ms=%.3f dt_p95_ms=%.3f dt_max_ms=%.3f\n", st.inter_frame_mean_ms, st.inter_frame_p95_ms, st.inter_frame_max_ms);
    std::printf("map_mean_ms=%.3f map_max_ms=%.3f arrival_to_map_done_ms=%.3f consume_lat_mean_ms=%.3f consume_lat_max_ms=%.3f\n",
                st.map_time_mean_ms, st.map_time_max_ms, st.arrival_to_map_done_ms, lat_n ? lat_sum / lat_n : 0.0, lat_max);
    std::printf("raw_min=%.2f raw_max=%.2f raw_mean=%.2f saturated_px=%u invalid_px=%u\n", st.raw_min, st.raw_max,
                st.raw_mean, st.saturated_pixels, st.invalid_pixels);
    std::printf("benign_driver_notices=%u driver_errors=%u status=\"%s\"\n", st.benign_driver_notices, st.driver_errors,
                st.status_message);
    if (p.clock_overlay) {
        std::printf("barcode_verified=%llu/%llu\n", static_cast<unsigned long long>(bc_valid),
                    static_cast<unsigned long long>(bc_checked));
    }

    int rc = 0;
    std::string verdict;
    if (bench) {
        // Kinect v2 depth/IR runs at 30 fps; colour may drop to 15 fps in low light, so it is judged on depth/IR.
        const double primary_fps = (o.mode == KV_MODE_COLOR)
            ? static_cast<double>(st.color_frames) / std::max(0.001, wall)
            : static_cast<double>(std::max(st.ir_frames, st.depth_frames)) / std::max(0.001, wall);
        const double min_fps = (o.mode == KV_MODE_COLOR) ? 14.0 : 29.0;
        std::vector<std::string> reasons;
        if (primary_fps < min_fps) { char b[96]; std::snprintf(b, sizeof(b), "fps %.2f < %.0f", primary_fps, min_fps); reasons.push_back(b); }
        if (drops > 0) reasons.push_back(std::to_string(drops) + " sequence drops");
        if (st.driver_errors > 0) reasons.push_back(std::to_string(st.driver_errors) + " driver errors");
        if (p.clock_overlay && bc_valid != bc_checked) reasons.push_back("barcode mismatches");
        verdict = reasons.empty() ? "PASS" : "FAIL";
        std::printf("BENCHMARK=%s primary_fps=%.2f", verdict.c_str(), primary_fps);
        for (const auto& r : reasons) std::printf(" [%s]", r.c_str());
        std::puts("");
        rc = reasons.empty() ? 0 : 4;
    } else if (p.clock_overlay && (bc_checked == 0 || bc_valid != bc_checked)) {
        rc = 4;
    }

    if (!o.json_path.empty()) {
        FILE* f = std::fopen(o.json_path.c_str(), "wb");
        if (f) {
            std::fprintf(f,
                         "{\n  \"source\": \"%s\",\n  \"mode\": \"%s\",\n  \"pipeline\": \"%s\",\n  \"wall_sec\": %.3f,\n"
                         "  \"avg_fps\": %.3f,\n  \"color_fps\": %.3f,\n  \"ir_frames\": %llu,\n  \"depth_frames\": %llu,\n"
                         "  \"color_frames\": %llu,\n  \"ir_seq_drops\": %llu,\n  \"depth_seq_drops\": %llu,\n"
                         "  \"color_seq_drops\": %llu,\n  \"dt_mean_ms\": %.3f,\n  \"dt_p95_ms\": %.3f,\n"
                         "  \"dt_max_ms\": %.3f,\n  \"map_mean_ms\": %.3f,\n  \"driver_errors\": %u,\n"
                         "  \"benign_driver_notices\": %u,\n  \"firmware\": \"%s\",\n  \"verdict\": \"%s\"\n}\n",
                         synthetic ? "synthetic" : "real_kinect_v2", modeName(o.mode), pipelineName(o.pipeline), wall,
                         avg_rate, st.color_fps, static_cast<unsigned long long>(st.ir_frames),
                         static_cast<unsigned long long>(st.depth_frames), static_cast<unsigned long long>(st.color_frames),
                         static_cast<unsigned long long>(st.ir_seq_drops), static_cast<unsigned long long>(st.depth_seq_drops),
                         static_cast<unsigned long long>(st.color_seq_drops), st.inter_frame_mean_ms, st.inter_frame_p95_ms,
                         st.inter_frame_max_ms, st.map_time_mean_ms, st.driver_errors, st.benign_driver_notices,
                         jsonEscape(st.firmware).c_str(), verdict.empty() ? "n/a" : verdict.c_str());
            std::fclose(f);
        }
    }
    return rc;
}

// ---- record ---------------------------------------------------------------------------------------

#ifdef KV_WITH_RECORDER
int cmdRecord(const Options& o) {
    // Colour is live only: refused before the sensor, or a folder, is opened.
    if (!kvrec::Recorder::canRecord(o.mode)) {
        std::fprintf(stderr, "RECORD_REFUSED: %s\n", kvhost::text::kColourLiveOnly);
        return 2;
    }
    // The real sensor is recorded only in the viewer, after the reminder and into the recordings
    // folder (as on the Mac, where --test-recording became synthetic-only). Refused before anything opens.
    if (!o.synthetic) {
        std::fprintf(stderr, "RECORD_REFUSED: kinect-cli records the synthetic source only. Record the Kinect in the viewer, after its reminder.\n");
        return 2;
    }
    kv_engine_t* eng = kv_engine_create(nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = o.mode;
    p.sbs_layout = o.sbs;
    p.depth_colormap = o.colormap;
    char e[256] = {0};
    if (!kv_engine_start(eng, o.synthetic ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL, o.pipeline, &p, e, sizeof(e))) {
        std::fprintf(stderr, "START_FAILED: %s\n", e);
        kv_engine_destroy(eng);
        return 1;
    }
    std::vector<uint8_t> buf(1920 * 1080 * 4);
    std::vector<uint16_t> raw16(512 * 424);
    // Wait for the first frame, so a source that produces none fails here.
    kv_mapped_frame_t m{};
    for (int i = 0; i < 300; ++i) {
        size_t req = 0;
        if (kv_engine_copy_latest_frame(eng, 0, buf.data(), buf.size(), &req, &m)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (m.width == 0) {
        std::fputs("NO_FRAMES: the source produced no frame within 3 s\n", stderr);
        kv_engine_destroy(eng);
        return 1;
    }
    kvrec::Recorder rec;
    kvrec::RecorderOptions ro;
    ro.lossless = o.png16 ? kvrec::Lossless16::Png16Sequence : kvrec::Lossless16::Ffv1Mkv;
    if (!o.out_dir.empty()) {
        const int n = MultiByteToWideChar(CP_ACP, 0, o.out_dir.c_str(), -1, nullptr, 0);
        ro.base_dir.assign(static_cast<size_t>(n > 0 ? n - 1 : 0), L' ');
        MultiByteToWideChar(CP_ACP, 0, o.out_dir.c_str(), -1, ro.base_dir.data(), n);
    }
    std::string err;
    if (!rec.start(o.mode, 512u, 424u, ro, &err)) {
        std::fprintf(stderr, "RECORD_FAILED: %s\n", err.c_str());
        kv_engine_destroy(eng);
        return 1;
    }
    const uint64_t start = kv_monotonic_ns();
    uint64_t last_raw = 0;
    while (kv_monotonic_ns() - start < static_cast<uint64_t>(o.duration_sec * 1e9)) {
        uint64_t seq = 0, arr = 0;
        uint32_t ticks = 0, w = 0, h = 0;
        if (kv_engine_copy_latest_raw16(eng, o.mode == KV_MODE_DEPTH, last_raw, raw16.data(), raw16.size(), &seq, &arr, &ticks,
                                        &w, &h)) {
            last_raw = seq;
            rec.pushRaw16(raw16.data(), w, h, seq, ticks, arr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    kv_stats_snapshot_t st;
    kv_engine_get_stats(eng, &st);
    kv_engine_destroy(eng);
    rec.stop();
    const kvrec::RecorderStatus rs = rec.status();
    std::string dir;
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, rs.session_dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
        dir.assign(static_cast<size_t>(n > 0 ? n - 1 : 0), ' ');
        WideCharToMultiByte(CP_UTF8, 0, rs.session_dir.c_str(), -1, dir.data(), n, nullptr, nullptr);
    }
    std::printf("RECORDED %s: frames=%llu dropped=%llu bytes=%llu (%.1f MB/min over %.1f s)\n  folder=%s\n  source "
                "ir_seq_drops=%llu depth_seq_drops=%llu color_seq_drops=%llu %s\n",
                rs.summary.c_str(), static_cast<unsigned long long>(rs.frames_written),
                static_cast<unsigned long long>(rs.frames_dropped), static_cast<unsigned long long>(rs.bytes), rs.mb_per_min,
                rs.seconds, dir.c_str(), static_cast<unsigned long long>(st.ir_seq_drops),
                static_cast<unsigned long long>(st.depth_seq_drops), static_cast<unsigned long long>(st.color_seq_drops),
                rs.error.c_str());
    return rs.error.empty() && rs.frames_written > 0 ? 0 : 1;
}
#endif

// ---- serve ----------------------------------------------------------------------------------------

#ifdef KV_WITH_STREAMING
std::atomic<bool> g_stop_serve{false};
// Manual-reset, set once serve has finished its clean exit (the host's shutdown(), the temp folder).
HANDLE g_serve_done = nullptr;

BOOL WINAPI onConsoleCtrl(DWORD type) {
    g_stop_serve = true;
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // Windows ends the process as soon as this returns (and after about 5 s anyway): wait, bounded,
        // for serve to finish its clean exit first, so the device list and settings are saved and a
        // temp state folder is removed (pet-camera.md §5, v3 §10).
        if (g_serve_done) WaitForSingleObject(g_serve_done, 4000);
    }
    return TRUE;
}

// Sets g_serve_done when serve returns, whichever way.
struct ServeDoneSignal {
    ServeDoneSignal() { g_serve_done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ServeDoneSignal() {
        if (g_serve_done) SetEvent(g_serve_done);
    }
    ServeDoneSignal(const ServeDoneSignal&) = delete;
    ServeDoneSignal& operator=(const ServeDoneSignal&) = delete;
};

// --state-dir as typed. main's argv is in the ANSI code page, which can't hold every path (a profile
// folder like C:\Users\Zoë on some systems), so it is read again from the wide command line, at the
// index parseArgs found it; the ANSI value (through CP_ACP, as record's --out) only if that fails.
std::wstring wideArgument(const char* ansi, int index, const wchar_t* flag) {
    std::wstring out;
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        if (index > 0 && index < n && wcscmp(w[index - 1], flag) == 0) out = w[index];
        LocalFree(w);
    }
    if (out.empty()) {
        const int len = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
        out.assign(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
        MultiByteToWideChar(CP_ACP, 0, ansi, -1, out.data(), len);
    }
    return out;
}

std::wstring stateDirArgument(const Options& o) { return wideArgument(o.state_dir.c_str(), o.state_dir_arg, L"--state-dir"); }

// The depth/IR recordings folder (recorder.cpp's default), which the roll refuses as or around itself
// (pet-camera.md §6.5).
std::wstring recordingsDirForRoll() {
#ifdef KV_WITH_RECORDER
    return kvrec::defaultRecordingsDir();
#else
    wchar_t profile[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(profile) + L"\\kinect-viewer-recordings";
#endif
}

// The headless host's sensor, for the shared power model (protocol v2's On/Off from the phone).
class ServeBackend final : public kvhost::SensorBackend {
public:
    ServeBackend(kv_engine_t* eng, const Options& o, kv_display_params_t* p) : eng_(eng), o_(o), p_(p) {}
    std::optional<std::string> externalLockHolder() override {
        if (o_.synthetic) return std::nullopt;
        char owner[256] = {0};
        if (!kv_is_locked_by_other(nullptr, owner, sizeof(owner))) return std::nullopt;
        return std::string(owner);
    }
    OpenResult open() override {
        char e[256] = {0};
        const bool want_rgb = p_->mode == KV_MODE_COLOR || (p_->mode == KV_MODE_SIDE_BY_SIDE && p_->sbs_layout != KV_SBS_IR_DEPTH);
        OpenResult r;
        r.ok = kv_engine_start_ex(eng_, o_.synthetic ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL, o_.pipeline, p_, want_rgb, e, sizeof(e));
        kv_stats_snapshot_t st;
        kv_engine_get_stats(eng_, &st);
        r.lock_blocked = !r.ok && st.lock_blocked;
        r.error = e;
        return r;
    }
    void close() override { kv_engine_stop(eng_); } // serve records nothing; this closes USB and the lock
    bool isOpen() const override { return kv_engine_is_running(eng_); }

private:
    kv_engine_t* eng_;
    const Options& o_;
    kv_display_params_t* p_;
};

int cmdServe(const Options& o) {
    kvhost::HostController::Options ho;
    ho.server.loopback_only = o.loopback_only;
    ho.server.advertise = !o.loopback_only;
    // v3 §10 "Test isolation": --state-dir wins; a synthetic or loopback-only serve without it keeps its
    // state in a temp folder of its own (deleted when it stops), never in the real .host-state. Pairing
    // a real phone against the synthetic source and keeping it needs an explicit --state-dir. A
    // --state-dir the host may not make owner-only is refused before anything opens.
    constexpr const wchar_t* kTempPrefix = L"kinect-cli-test";
    bool temp_state = false;
    if (!o.state_dir.empty()) {
        ho.server.state_dir = kvwin::normalizeStateDir(stateDirArgument(o));
        std::wstring why;
        if (!kvwin::stateDirAcceptable(ho.server.state_dir, &why)) {
            std::fprintf(stderr, "STATE_DIR_REFUSED: %ls\n", why.c_str());
            return 2;
        }
    }
    if (!o.synthetic) {
        char owner[256] = {0};
        if (kv_is_locked_by_other(nullptr, owner, sizeof(owner))) {
            std::printf("LOCK_BLOCKED: %s\n", owner);
            return 3;
        }
    }
    // A synthetic or loopback-only serve is a test run, as the viewer's --synthetic launch is. Its roll refuses
    // any frame that isn't synthetic, and its folder must be new, empty or a test roll (checked by the host).
    const bool test_run = o.synthetic || o.loopback_only;
    ho.test_run = test_run;
    if (o.until_roll_prepared && o.camera_roll_dir.empty()) {
        std::fputs("--until-roll-prepared needs --camera-roll-dir\n", stderr);
        return 2;
    }
    if (!o.camera_roll_dir.empty()) {
        // Only the folder named here is ever recorded into (v3 §10). It is checked when the host starts.
        ho.roll.dir = wideArgument(o.camera_roll_dir.c_str(), o.camera_roll_dir_arg, L"--camera-roll-dir");
        ho.roll.recordingsDir = recordingsDirForRoll();
        // A second line of refusal, as the viewer's: in a test run the real folders are refused by their path alone (none
        // of them is opened to decide), before the host's own rule (new, empty or a test roll) is even reached.
        std::wstring why;
        if (test_run && !kvwin::cameraRollTestDirAcceptable(ho.roll.dir, &why)) {
            std::fprintf(stderr, "CAMERA_ROLL_REFUSED: %ls\n", why.c_str());
            return 2;
        }
        // Then the viewer's own contents check: the path resolved through junctions, subst drives and
        // 8.3 names (GetFinalPathNameByHandleW) must pass the path-text refusals too, and the folder must be new,
        // empty or a test roll. The host checks a test roll again when it starts (roll::checkTestRollFolder).
        if (test_run && !kvwin::cameraRollTestDirContentsAcceptable(ho.roll.dir, &why)) {
            std::fprintf(stderr, "CAMERA_ROLL_REFUSED: in a test run, %ls\n", why.c_str());
            return 2;
        }
    }
    if (o.state_dir.empty() && (o.synthetic || o.loopback_only)) {
        // Fresh: what a killed run with the same PID left there is removed first.
        std::wstring why;
        ho.server.state_dir = kvwin::freshTemporaryStateDir(kTempPrefix, &why);
        if (ho.server.state_dir.empty()) {
            std::fprintf(stderr, "SERVE_FAILED: %ls\n", why.c_str());
            return 1;
        }
        temp_state = true;
    }
    ServeDoneSignal done_signal; // after every cleanup below, including ctl's
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
    kvhost::HostController ctl;
    if (!ho.server.state_dir.empty()) {
        std::printf("state dir: %ls%s\n", ho.server.state_dir.c_str(),
                    temp_state ? " (temporary: a synthetic or loopback-only run; deleted when serve stops)" : "");
    }
    std::atomic<int> want_mode{-1}, want_sbs{-1};
    ctl.onClientRequestedMode = [&](int m, int sb) {
        want_sbs = sb;
        want_mode = m;
    };
    // v2 requests, handled on this thread in arrival order (network threads only queue them).
    std::mutex req_m;
    std::deque<bool> power_reqs;
    std::atomic<bool> push_state{false};
    ctl.onClientSensorPowerRequest = [&](const kvhost::SensorPowerControlMessage& r) {
        std::lock_guard<std::mutex> lk(req_m);
        power_reqs.push_back(r.turnOn);
    };
    ctl.onPhoneDisconnected = [&](bool turn_off) {
        if (!turn_off) return;
        std::lock_guard<std::mutex> lk(req_m);
        power_reqs.push_back(false);
    };
    ctl.onClientRemoteCommand = [&](const kvhost::RemoteControlCommandMessage& cmd) {
        // serve has no recorder, cut-off or freeze: only a mode switch does anything here.
        if (cmd.action == kvhost::RemoteControlAction::SelectMode && cmd.modeRaw) {
            want_sbs = cmd.sbsLayoutRaw.value_or(want_sbs.load() < 0 ? 0 : want_sbs.load());
            want_mode = *cmd.modeRaw;
        }
        push_state = true;
    };
    std::string err;
    bool identity_unusable = false;
    if (!ctl.start(ho, &err, &identity_unusable)) {
        // pairing.json there but unusable: serve fails closed and never makes a new identity. There is no CLI
        // reset; the viewer's Pet camera panel has one (every device then pairs again).
        if (identity_unusable) std::fprintf(stderr, "IDENTITY_UNUSABLE: %s\n", err.c_str());
        std::fprintf(stderr, "SERVE_FAILED: %s\n",
                     identity_unusable ? "this PC's identity (pairing.json) can't be used; nothing was changed" : err.c_str());
        if (temp_state) kvwin::removeTemporaryStateDir(ho.server.state_dir, kTempPrefix);
        return 1;
    }
    if (!o.camera_roll_dir.empty() && !ctl.rollAvailable()) {
        // The folder was refused (OneDrive, a network drive, the depth/IR folder, a folder holding other files):
        // serve doesn't run without the roll it was asked for.
        std::fprintf(stderr, "CAMERA_ROLL_REFUSED: %s\n", ctl.rollError().c_str());
        ctl.stop();
        if (temp_state) kvwin::removeTemporaryStateDir(ho.server.state_dir, kTempPrefix);
        return 2;
    }
    if (ctl.rollAvailable()) std::printf("camera roll: %ls\n", ho.roll.dir.c_str());
    bool outside_refused = false; // --until-outside-reported: nothing more to wait for
    if (o.outside_port != 0) {
        // Outside listening on, on this port, for this host's state (saved in its pet-camera.json, as the PC's
        // switch does). Refused with the reason while sharing is off or pet-camera.json can't be read.
        std::string oerr;
        if (!ctl.setOutsidePort(static_cast<uint16_t>(o.outside_port), &oerr) || !ctl.setAllowOutside(true, &oerr)) {
            std::fprintf(stderr, "OUTSIDE_REFUSED: %s\n", oerr.c_str());
            outside_refused = true;
        } else if (ctl.server().outsideListeningPort() == 0) {
            std::fprintf(stderr, "OUTSIDE_NOT_LISTENING: %s\n", ctl.outsideView().sentence.c_str());
            outside_refused = true;
        } else {
            // The address the socket is really bound to (v3 §10: a test run's is 127.0.0.1), not what the flags imply.
            const std::string bound = ctl.server().outsideListenerAddress();
            const std::string shown = bound.find(':') != std::string::npos ? "[" + bound + "]" : bound;
            std::printf("outside: listening on %s:%u%s\n", shown.c_str(), ctl.server().outsideListeningPort(),
                        test_run ? " (a test run: no router is used)" : "");
        }
    }
    kv_engine_t* eng = kv_engine_create(nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = o.mode;
    p.sbs_layout = o.sbs;
    p.depth_colormap = o.colormap;
    p.depth_min_mm = o.depth_min;
    p.depth_max_mm = o.depth_max;
    p.clock_overlay = o.synthetic && o.clock_overlay;
    char e[256] = {0};
    ServeBackend backend(eng, o, &p);
    kvhost::HostPowerModel power(backend, [&](const kvhost::HostPowerModel::Snapshot& s) {
        ctl.setPowerSnapshot(s);
        std::printf("power: %s%s%s\n", kvhost::toWire(s.state), s.errorMessage ? " " : "",
                    s.errorMessage ? s.errorMessage->c_str() : "");
        std::fflush(stdout);
    });
    kvhost::HostViewState view;
    view.modeRaw = static_cast<int>(o.mode);
    view.sbsLayoutRaw = static_cast<int>(o.sbs);
    view.canRecordCurrentMode = false; // serve never records
    view.recordingDisabledReason = "This host records nothing.";
    ctl.setViewState(view);
    power.request(true);
    if (power.snapshot().state != kvhost::SensorPowerState::On) {
        const auto snap = power.snapshot();
        std::fprintf(stderr, "START_FAILED: %s\n", snap.errorMessage ? snap.errorMessage->c_str() : "in use elsewhere");
        kv_engine_destroy(eng);
        ctl.stop();
        if (temp_state) kvwin::removeTemporaryStateDir(ho.server.state_dir, kTempPrefix);
        return 1;
    }
    int cur_mode = static_cast<int>(o.mode);
    int cur_sbs = static_cast<int>(o.sbs);
    auto& srv = ctl.server();
    std::printf("host fingerprint %s..., service id %s, stream port %u, %s\n", srv.serverFingerprintHex().substr(0, 12).c_str(),
                srv.serviceId().c_str(), srv.streamPort(), srv.isPhonePaired() ? "paired" : "not paired");
    if (o.pair) {
        kvhost::ActivePairingOffer offer;
        const kvhost::PetRole role = o.pair_owner ? kvhost::PetRole::Owner : kvhost::PetRole::Viewer;
        if (!srv.beginPairingWindow(120, offer, &err, role)) {
            std::fprintf(stderr, "PAIRING_FAILED: %s\n", err.c_str());
        } else {
            // The code only when asked for (--pair-code): QR only by default.
            const bool code = o.pair_code && srv.allowShortCodeForPairing();
            std::printf("PAIRING OPEN for 120 s (one try, as %s)\n  code: %s\n  QR URI: %s\n", kvhost::petRoleString(role),
                        code ? offer.shortCodeFormatted.c_str() : "off (QR only; --pair-code takes the code too)",
                        offer.qrURI.c_str());
        }
    }
    std::printf("%s\n", srv.advertisementStatus().c_str());
    std::fflush(stdout);

    std::vector<uint8_t> buf(1920 * 1080 * 4);
    uint64_t last = 0;
    // Camera roll motion (pet-camera.md §6.2): the live sensor's depth, each new frame, while there is a roll.
    std::vector<uint16_t> depth(512 * 424);
    uint64_t last_depth = 0;
    const uint64_t start = kv_monotonic_ns();
    uint64_t next_status = start + 5000000000ULL;
    uint64_t next_outside_check = start;
    std::string last_outside; // the outside status line last printed
    int serve_rc = 0;
    while (!g_stop_serve) {
        if (o.duration_sec > 0 && kv_monotonic_ns() - start >= static_cast<uint64_t>(o.duration_sec * 1e9)) {
            if (o.until_roll_prepared) {
                std::fprintf(stderr, "ROLL_NOT_PREPARED: the camera roll's folder wasn't prepared in %.0f s\n", o.duration_sec);
                serve_rc = 4;
            }
            if (o.until_outside_reported) {
                std::fprintf(stderr, "OUTSIDE_NOT_REPORTED: no outside status in %.0f s\n", o.duration_sec);
                serve_rc = 4;
            }
            break;
        }
        if (o.until_outside_reported && outside_refused) break;
        if (o.until_roll_prepared && ctl.rollStatus().prepared) {
            std::printf("camera roll prepared\n");
            break;
        }
        while (true) {
            bool turn_on = false;
            {
                std::lock_guard<std::mutex> lk(req_m);
                if (power_reqs.empty()) break;
                turn_on = power_reqs.front();
                power_reqs.pop_front();
            }
            power.request(turn_on);
        }
        const int wm = want_mode.exchange(-1);
        if (wm >= 0 && wm <= KV_MODE_SIDE_BY_SIDE) {
            kv_display_params_t q;
            kv_engine_get_display_params(eng, &q);
            q.mode = static_cast<kv_view_mode_t>(wm);
            q.sbs_layout = static_cast<kv_sbs_layout_t>(std::clamp(want_sbs.load(), 0, 2));
            const bool needs_rgb = q.mode == KV_MODE_COLOR || (q.mode == KV_MODE_SIDE_BY_SIDE && q.sbs_layout != KV_SBS_IR_DEPTH);
            p.mode = q.mode; // what a later On reopens with
            p.sbs_layout = q.sbs_layout;
            if (backend.isOpen()) {
                if (needs_rgb && !kv_engine_is_rgb_active(eng)) {
                    kvhost::SensorBackend::OpenResult r;
                    r.ok = kv_engine_start_ex(eng, o.synthetic ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL, o.pipeline, &q, true, e,
                                              sizeof(e));
                    kv_stats_snapshot_t st;
                    kv_engine_get_stats(eng, &st);
                    r.lock_blocked = !r.ok && st.lock_blocked;
                    r.error = e;
                    power.reportRestart(r);
                } else {
                    kv_engine_set_display_params(eng, &q);
                }
            }
            cur_mode = q.mode;
            cur_sbs = q.sbs_layout;
            view.modeRaw = cur_mode;
            view.sbsLayoutRaw = cur_sbs;
            ctl.setViewState(view);
            ctl.notifyModeChanged(cur_mode, cur_sbs, 0, 0);
            std::printf("phone switched mode to %d (sbs %d)\n", cur_mode, cur_sbs);
        } else if (wm > KV_MODE_SIDE_BY_SIDE) {
            push_state = true; // mode 4 (3D) isn't here: the phone's picker snaps back
        }
        if (push_state.exchange(false)) ctl.pushHostState();
        size_t req = 0;
        kv_mapped_frame_t m{};
        if (kv_engine_copy_latest_frame(eng, last, buf.data(), buf.size(), &req, &m)) {
            last = m.frame_seq;
            ctl.submitFrame(buf.data(), m, cur_mode, cur_sbs, p.clock_overlay);
        } else if (req > buf.size()) {
            buf.resize(req);
        }
        if (ctl.rollAvailable() && backend.isOpen()) {
            uint64_t dseq = 0, darrival = 0;
            uint32_t dticks = 0, dw = 0, dh = 0;
            if (kv_engine_copy_latest_raw16(eng, true, last_depth, depth.data(), depth.size(), &dseq, &darrival, &dticks, &dw,
                                            &dh)) {
                last_depth = dseq;
                ctl.submitDepthForMotion(depth.data(), dw, dh, dseq, darrival, true);
            }
        }
        if (kv_monotonic_ns() >= next_outside_check) {
            // The outside status, printed when it changes (the mapper works on a thread of its own).
            next_outside_check = kv_monotonic_ns() + 250000000ULL;
            const kvhost::OutsideView ov = ctl.outsideView();
            if (ov.allowed || ov.listening) {
                const std::string line = std::string("outside: status=") + kvhost::outside::outsideStatusName(ov.status) +
                                         (ov.checking ? " (checking)" : "") + " port=" + std::to_string(ov.port) +
                                         (ov.endpointKnown ? " endpoint=" + ov.endpointHost : std::string()) + " " + ov.sentence;
                if (line != last_outside) {
                    last_outside = line;
                    std::printf("%s\n", line.c_str());
                    std::fflush(stdout);
                }
                if (o.until_outside_reported && ov.listening && !ov.checking) break; // printed above, as a result
            }
        }
        if (kv_monotonic_ns() >= next_status) {
            next_status += 5000000000ULL;
            std::printf("devices=%zu sessions=%d sent=%llu dropped=%llu %.2f Mbps phone_display_ms=%.1f phone_barcode_ms=%.1f "
                        "encoder=\"%s\" %s\n",
                        srv.devices().size(), srv.sessionCount(),
                        static_cast<unsigned long long>(srv.sentFramesCount()),
                        static_cast<unsigned long long>(srv.droppedCongestionFrames()), srv.currentBitrateMbps(),
                        srv.lastClientDisplayLatencyMs(), srv.lastClientBarcodeLatencyMs(), ctl.encoderName().c_str(),
                        ctl.lastError().c_str());
            const std::string save_error = srv.saveError(); // a failed state write, until a retry succeeds
            if (!save_error.empty()) std::printf("SAVE_FAILED: %s\n", save_error.c_str());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ctl.stop();
    kv_engine_destroy(eng);
    if (temp_state) kvwin::removeTemporaryStateDir(ho.server.state_dir, kTempPrefix);
    std::puts("serve stopped");
    return serve_rc;
}

// router-probe: the host's v3 §8.3 verification pass, read once against this PC's real router, changing nothing. The
// three parts the host reads in a pass: NATUPnP's public address (only from the forward already on the given port:
// nothing is added, so with no forward there it reads as unread), the gateway of this PC's interface (address and
// hardware address), and that gateway's own GetExternalIPAddress answers (a search first, as after a restart, then
// once more straight to the description that answered, as on a later pass). Then the verdict the host would reach
// with this gateway pinned. Every line is masked (maskProbeLine) before it is printed.
int cmdRouterProbe(const Options& o) {
    namespace outside = kvhost::outside;
    if (o.outside_port == 0) {
        std::fputs("router-probe needs --outside-port N: the port the host's forward is on\n", stderr);
        return 2;
    }
    std::vector<std::string> lines;
    const auto say = [&](const std::string& s) { lines.push_back(s); };
    const auto yes = [](bool b) { return b ? "yes" : "no"; };
    const auto kindOf = [](const std::string& a) {
        switch (outside::checkWanAddress(a)) {
            case outside::WanCheck::Public: return "public";
            case outside::WanCheck::NotConnected: return "not connected";
            case outside::WanCheck::CarrierNat: break;
        }
        return "not public";
    };

    outside::ComScope com;
    if (!com.ok()) {
        std::fputs("router-probe: COM didn't start\n", stderr);
        return 3;
    }
    // 1. NATUPnP, as the host's mapper reads it: a session (retried for about 10 s while the collection is NULL),
    // then only the mapping already on the port. find() reads; nothing here calls add() or remove().
    std::unique_ptr<outside::RouterSession> session;
    for (int tries = 0; tries < 10 && !session; ++tries) {
        session = outside::openNatUpnpSession();
        if (!session) Sleep(1000);
    }
    std::string reported, forwardClient;
    if (!session) {
        say("NATUPnP: no router found in about 10 s (UPnP off on the router, or no gateway device answered)");
    } else if (const auto m = session->find(static_cast<uint16_t>(o.outside_port)); !m) {
        say("NATUPnP: no forward on the given port, so its public address can't be read without adding one, which "
            "this probe never does (turn outside viewing on and run it again)");
    } else {
        forwardClient = m->internalClient;
        reported = m->externalAddress;
        say("NATUPnP: the forward is there (internal client " + m->internalClient + ", enabled " + yes(m->enabled) +
            ", ours " + yes(m->description == outside::kMappingDescription) + "); public address " +
            (reported.empty() ? std::string("not read") : reported + " (" + kindOf(reported) + ")"));
    }
    session.reset();

    // 2. The gateway of the interface that owns this PC's LAN address (the host's lanNow: internalClientIPv4).
    const std::optional<std::string> lanRead = outside::internalClientIPv4();
    const std::string lan = lanRead && outside::usableInternalClient(*lanRead) ? *lanRead : std::string();
    say("this PC's LAN address: " + (lan.empty() ? std::string("none usable") : lan) +
        (!forwardClient.empty() && forwardClient != lan ? " (the forward points at another address)" : ""));
    const outside::GatewayIdentity gateway = lan.empty() ? outside::GatewayIdentity{} : outside::readGatewayOn(lan);
    say("gateway: address " + (gateway.ip.empty() ? std::string("not read") : gateway.ip) + ", hardware address " +
        (gateway.mac.empty() ? std::string("not read") : gateway.mac));

    // 3. The gateway's own answers: a search (nothing answered before, as after a restart), then the description
    // that answered, straight over TCP (as every later pass does). Each with the host's 8 s bound.
    std::vector<std::string> answers;
    if (!gateway.ip.empty()) {
        std::string lastGood;
        for (int pass = 1; pass <= 2; ++pass) {
            if (pass == 2 && lastGood.empty()) break;
            say(pass == 1 ? "--- the gateway's own answers, first pass (search)"
                          : "--- second pass (the description that answered)");
            const auto start = std::chrono::steady_clock::now();
            std::vector<std::string> got = outside::askGatewayForWan(gateway.ip, lan, 8'000, lastGood, say);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::string list;
            for (const std::string& a : got) list += (list.empty() ? "" : ", ") + a + " (" + kindOf(a) + ")";
            say("pass " + std::to_string(pass) + ": " + std::to_string(ms.count()) + " ms, " +
                (got.empty() ? std::string("no answer") : list));
            if (pass == 1) answers = got;
        }
    }

    // 4. What the host would decide, with this gateway pinned: only public answers count, as in its pass.
    outside::GatewayReading reading;
    reading.gateway = gateway;
    reading.reportedWan = outside::checkWanAddress(reported) == outside::WanCheck::Public ? reported : std::string();
    for (const std::string& a : answers) {
        if (outside::checkWanAddress(a) == outside::WanCheck::Public) reading.gatewayWans.push_back(a);
    }
    say("---");
    say(std::string("the gateway answers publicly: ") + yes(!reading.gatewayWans.empty()));
    switch (outside::judgeGateway(gateway, reading)) {
        case outside::GatewayVerdict::Match:
            say("verdict: Confirmed. The gateway's own answer is NATUPnP's address: a new public address would be told "
                "to devices with no click.");
            break;
        case outside::GatewayVerdict::Contradicted:
            say("verdict: Contradicted. The gateway answers publicly, but not with NATUPnP's address: a new address "
                "would be held and never re-shared until they agree.");
            break;
        case outside::GatewayVerdict::Unreadable: {
            std::string why;
            if (!gateway.complete()) why = "the gateway's hardware address wasn't read";
            else if (reading.gatewayWans.empty()) why = "the gateway gave no public answer";
            else why = "NATUPnP's public address wasn't read";
            say("verdict: Unreadable (" + why + "). A new address would be checked every minute and never told "
                "until a pass reads all three; meanwhile the PC user can turn outside viewing off and on.");
            break;
        }
        case outside::GatewayVerdict::NoPin:
            say("verdict: no pin. The gateway's address or hardware address couldn't be read, so nothing would be "
                "pinned: a new address would wait for the PC user.");
            break;
        case outside::GatewayVerdict::OtherGateway:
            say("verdict: another gateway (can't happen with the pin read now)");
            break;
    }

    // 5. Why, when NATUPnP or the search found nothing: a survey of what answers SSDP at all, to tell UPnP off on the
    // router, an IGD:2-only router (NATUPnP and the host search for IGD:1) and searches that get no answer from
    // anything. Read-only: searches only. Other devices are only counted, never named.
    if (!gateway.ip.empty() && !lan.empty()) {
        say("--- what answers SSDP on this network (3 s each)");
        bool gwIgd1 = false, gwIgd2 = false, gwAny = false, othersAny = false;
        for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                               "urn:schemas-upnp-org:device:InternetGatewayDevice:2", "ssdp:all"}) {
            const std::vector<outside::SsdpSeen> seen = outside::ssdpSurvey(lan, gateway.ip, st, 3'000);
            std::vector<std::string> gwTypes, others;
            for (const auto& s : seen) {
                if (s.source == gateway.ip) {
                    // A device's own uuid identifies it: masked like an address.
                    const std::string t = s.st.rfind("uuid:", 0) == 0 ? std::string("uuid:[masked]") : s.st;
                    if (std::find(gwTypes.begin(), gwTypes.end(), t) == gwTypes.end()) gwTypes.push_back(t);
                } else if (std::find(others.begin(), others.end(), s.source) == others.end()) {
                    others.push_back(s.source);
                }
            }
            gwAny = gwAny || !gwTypes.empty();
            othersAny = othersAny || !others.empty();
            for (const std::string& t : gwTypes) {
                gwIgd1 = gwIgd1 || t == "urn:schemas-upnp-org:device:InternetGatewayDevice:1";
                gwIgd2 = gwIgd2 || t == "urn:schemas-upnp-org:device:InternetGatewayDevice:2";
            }
            std::string types;
            for (const std::string& t : gwTypes) types += (types.empty() ? "" : ", ") + (t.empty() ? std::string("(no ST)") : t);
            say(std::string("search ") + st + ": " + std::to_string(seen.size()) + " repl" + (seen.size() == 1 ? "y" : "ies") +
                "; the gateway answered as " + (types.empty() ? std::string("nothing") : types) + "; " +
                std::to_string(others.size()) + " other device(s) answered");
        }
        if (gwIgd1) say("so: the gateway offers IGD:1 when asked; the earlier search or NATUPnP missed it (timing?)");
        else if (gwIgd2) say("so: the gateway is IGD:2-only. NATUPnP and the host's search ask for IGD:1, so it reads as manual");
        else if (gwAny) say("so: the gateway answers SSDP but offers no Internet Gateway Device: UPnP port mapping is off on it");
        else if (othersAny) say("so: the gateway doesn't answer SSDP at all (UPnP off, or it ignores searches); other devices do, "
                                "so searches work on this network");
        else say("so: nothing answered any search. UPnP may be off on the router and on every device, or this PC's firewall "
                 "or the network drops the replies");
    }

    // Masked, with the parts named: addresses, hardware addresses and device ids. Service types and URL paths are kept
    // (they may reveal the router's make or model).
    std::vector<std::pair<std::string, std::string>> names;
    if (!gateway.ip.empty()) names.emplace_back(gateway.ip, "gateway");
    if (!lan.empty()) names.emplace_back(lan, "this PC");
    if (!forwardClient.empty() && forwardClient != lan) names.emplace_back(forwardClient, "the forward's internal client");
    if (!reported.empty() && reported != "0.0.0.0") names.emplace_back(reported, "NATUPnP's public address");
    for (const std::string& line : lines) std::puts(outside::maskProbeLine(line, names).c_str());
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    timeBeginPeriod(1); // 1 ms sleep granularity for this process (synthetic pacing and frame polling)
    Options o;
    if (!parseArgs(argc, argv, o)) {
        usage();
        return 2;
    }
    if (o.verb == "list") return cmdList(o);
    if (o.verb == "verify-lock") return cmdVerifyLock();
    if (o.verb == "verify-no-fallback") return cmdVerifyNoFallback();
    if (o.verb == "run") return cmdRun(o, false);
    if (o.verb == "bench") return cmdRun(o, true);
#ifdef KV_WITH_STREAMING
    if (o.verb == "serve") return cmdServe(o);
    if (o.verb == "router-probe") return cmdRouterProbe(o);
#endif
#ifdef KV_WITH_RECORDER
    if (o.verb == "record") return cmdRecord(o);
#endif
    std::fprintf(stderr, "Unknown verb: %s\n", o.verb.c_str());
    usage();
    return 2;
}
