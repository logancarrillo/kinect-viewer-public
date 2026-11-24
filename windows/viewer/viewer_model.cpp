#include "viewer_model.hpp"

#include "host_state.hpp"    // kHostStateMarkerFile; fromJson (kv_json: host_state_json.cpp)
#include "wire_protocol.hpp" // text::kColourLiveOnly (header only)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace kvwin {

kv_display_params_t toDisplayParams(const ViewerSettings& s) {
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = s.mode;
    p.sbs_layout = s.sbs_layout;
    p.ir_auto_scale = s.ir_auto_scale;
    p.ir_low_percentile = s.ir_low_percentile;
    p.ir_high_percentile = s.ir_high_percentile;
    p.ir_ema_alpha = s.ir_ema_alpha;
    p.ir_gamma = s.ir_gamma;
    p.ir_manual_min = s.ir_manual_min;
    p.ir_manual_max = s.ir_manual_max;
    p.depth_colormap = s.depth_colormap;
    p.depth_min_mm = s.depth_min_mm;
    p.depth_max_mm = s.depth_max_mm;
    p.depth_invert = s.depth_invert;
    p.clock_overlay = s.synthetic && s.clock_overlay;
    p.mirror_horizontal = s.mirror_horizontal;
    p.rotate_180 = s.rotate_180;
    p.cutoff_enabled = s.cutoff_enabled;
    p.cutoff_near_mm = s.cutoff_near_mm;
    p.cutoff_far_mm = s.cutoff_far_mm;
    p.depth_color_blend_enabled = s.depth_color_blend_enabled;
    p.depth_color_blend = std::clamp(s.depth_color_blend_percent / 100.0f, 0.0f, 1.0f);
    p.point_color_source = s.point_color_source;
    return p;
}

kv_color_controls_t toColorControls(const ViewerSettings& s) {
    kv_color_controls_t c;
    kv_default_color_controls(&c);
    c.mode = s.exposure_mode;
    c.auto_exposure_compensation = s.auto_exposure_compensation;
    c.semi_auto_pseudo_ms = s.semi_auto_pseudo_ms;
    c.manual_integration_ms = s.manual_integration_ms;
    c.manual_analog_gain = s.manual_analog_gain;
    return c;
}

bool modeNeedsColor(kv_view_mode_t mode, kv_sbs_layout_t sbs) {
    if (mode == KV_MODE_COLOR) return true;
    return mode == KV_MODE_SIDE_BY_SIDE && (sbs == KV_SBS_COLOR_DEPTH || sbs == KV_SBS_COLOR_IR);
}

bool settingsNeedColor(const ViewerSettings& s) {
    if (s.depth_color_blend_enabled) return true; // the core runs the RGB stream for it in every mode
    if (s.mode == KV_MODE_POINT_CLOUD_3D) return s.point_color_source == KV_POINT_COLOR_REGISTERED_RGB;
    return modeNeedsColor(s.mode, s.sbs_layout);
}

bool needsRestartForColor(bool running, bool rgb_active, const ViewerSettings& s) {
    return running && !rgb_active && settingsNeedColor(s);
}

bool blendShown(const ViewerSettings& s) {
    return s.depth_color_blend_enabled && s.mode != KV_MODE_POINT_CLOUD_3D;
}

kv_view_mode_t imageLayoutMode(const ViewerSettings& s) {
    return blendShown(s) ? KV_MODE_DEPTH : s.mode;
}

void applyTrackMe(ViewerSettings& s) {
    s.cutoff_near_mm = 500.0f;
    s.cutoff_far_mm = 2000.0f;
    s.cutoff_enabled = true;
}

PhoneViewChange applyPhoneViewCommand(ViewerSettings& s, const kvhost::RemoteControlCommandMessage& cmd) {
    using A = kvhost::RemoteControlAction;
    if (cmd.action == A::SelectMode) {
        if (!cmd.modeRaw || *cmd.modeRaw < KV_MODE_IR || *cmd.modeRaw > KV_MODE_POINT_CLOUD_3D) return PhoneViewChange::None;
        s.mode = static_cast<kv_view_mode_t>(*cmd.modeRaw);
        if (cmd.sbsLayoutRaw && *cmd.sbsLayoutRaw >= KV_SBS_IR_DEPTH && *cmd.sbsLayoutRaw <= KV_SBS_COLOR_IR) {
            s.sbs_layout = static_cast<kv_sbs_layout_t>(*cmd.sbsLayoutRaw);
        }
        return PhoneViewChange::Mode;
    }
    if (cmd.action == A::SetCutoff) {
        if (cmd.applyTrackMePreset.value_or(false)) {
            applyTrackMe(s);
            return PhoneViewChange::Cutoff;
        }
        if (cmd.cutoffEnabled) {
            s.cutoff_enabled = *cmd.cutoffEnabled;
            return PhoneViewChange::Cutoff;
        }
    }
    return PhoneViewChange::None;
}

int cutoffToSlider(float mm) {
    return static_cast<int>(std::lround(mm / 50.0f)) * 50;
}
float sliderToCutoff(int v) { return static_cast<float>(static_cast<int>(std::lround(v / 50.0)) * 50); }
int pointSizeToSlider(float px) {
    return std::clamp(static_cast<int>(std::lround(px * 2.0f)), kPointSizeSliderMin, kPointSizeSliderMax);
}
float sliderToPointSize(int v) { return static_cast<float>(v) / 2.0f; }

void setCutoffNear(ViewerSettings& s, float mm) {
    s.cutoff_near_mm = std::clamp(mm, 200.0f, 5000.0f);
    if (s.cutoff_far_mm < s.cutoff_near_mm + 50.0f) s.cutoff_far_mm = s.cutoff_near_mm + 50.0f;
    sanitize(s);
}

void setCutoffFar(ViewerSettings& s, float mm) {
    s.cutoff_far_mm = std::clamp(mm, 400.0f, 8000.0f);
    if (s.cutoff_far_mm < s.cutoff_near_mm + 50.0f) s.cutoff_near_mm = std::max(200.0f, s.cutoff_far_mm - 50.0f);
    sanitize(s);
}

void applyDepthPreset(ViewerSettings& s, DepthPreset p) {
    switch (p) {
        case DepthPreset::Desk: s.depth_min_mm = 400.0f; s.depth_max_mm = 2500.0f; break;
        case DepthPreset::Spec: s.depth_min_mm = 500.0f; s.depth_max_mm = 4500.0f; break;
        case DepthPreset::Room: s.depth_min_mm = 500.0f; s.depth_max_mm = 8000.0f; break;
    }
}

void resetIrDefaults(ViewerSettings& s) {
    s.ir_auto_scale = true;
    s.ir_gamma = 0.45f;
    s.ir_ema_alpha = 0.12f;
    s.ir_low_percentile = 0.01f;
    s.ir_high_percentile = 0.995f;
    s.ir_manual_min = 0.0f;
    s.ir_manual_max = 20000.0f;
}

void sanitize(ViewerSettings& s) {
    s.ir_gamma = std::clamp(s.ir_gamma, 0.20f, 1.40f);
    s.ir_ema_alpha = std::clamp(s.ir_ema_alpha, 0.05f, 1.0f);
    s.ir_manual_min = std::clamp(s.ir_manual_min, 0.0f, 65534.0f);
    s.ir_manual_max = std::clamp(s.ir_manual_max, s.ir_manual_min + 1.0f, 65535.0f);
    s.depth_min_mm = std::clamp(s.depth_min_mm, 300.0f, 7900.0f);
    s.depth_max_mm = std::clamp(s.depth_max_mm, s.depth_min_mm + 100.0f, 8000.0f);
    s.auto_exposure_compensation = std::clamp(s.auto_exposure_compensation, -2.0f, 2.0f);
    s.semi_auto_pseudo_ms = std::clamp(s.semi_auto_pseudo_ms, 1.0f, 66.0f);
    s.manual_integration_ms = std::clamp(s.manual_integration_ms, 1.0f, 66.0f);
    s.manual_analog_gain = std::clamp(s.manual_analog_gain, 1.0f, 4.0f);
    if (!std::isfinite(s.cutoff_near_mm)) s.cutoff_near_mm = 500.0f;
    if (!std::isfinite(s.cutoff_far_mm)) s.cutoff_far_mm = 2000.0f;
    s.cutoff_near_mm = std::clamp(s.cutoff_near_mm, 200.0f, 5000.0f);
    s.cutoff_far_mm = std::clamp(s.cutoff_far_mm, std::max(400.0f, s.cutoff_near_mm + 50.0f), 8000.0f);
    if (!std::isfinite(s.depth_color_blend_percent)) s.depth_color_blend_percent = 50.0f;
    s.depth_color_blend_percent = std::clamp(s.depth_color_blend_percent, 0.0f, 100.0f);
    if (s.point_color_source < KV_POINT_COLOR_DEPTH || s.point_color_source > KV_POINT_COLOR_REGISTERED_RGB) {
        s.point_color_source = KV_POINT_COLOR_DEPTH;
    }
    if (!std::isfinite(s.point_size)) s.point_size = 2.5f;
    s.point_size = std::clamp(s.point_size, 1.0f, 10.0f);
}

namespace {
int scaled(float v, float k, int lo, int hi) {
    return std::clamp(static_cast<int>(std::lround(v * k)), lo, hi);
}
} // namespace

int gammaToSlider(float g) { return scaled(g, 100.0f, kGammaSliderMin, kGammaSliderMax); }
float sliderToGamma(int v) { return static_cast<float>(v) / 100.0f; }
int emaToSlider(float a) { return scaled(a, 100.0f, kEmaSliderMin, kEmaSliderMax); }
float sliderToEma(int v) { return static_cast<float>(v) / 100.0f; }
int evToSlider(float ev) { return scaled(ev, 100.0f, kEvSliderMin, kEvSliderMax); }
float sliderToEv(int v) { return static_cast<float>(v) / 100.0f; }
int msToSlider(float ms) { return scaled(ms, 10.0f, kMsSliderMin, kMsSliderMax); }
float sliderToMs(int v) { return static_cast<float>(v) / 10.0f; }
int gainToSlider(float g) { return scaled(g, 100.0f, kGainSliderMin, kGainSliderMax); }
float sliderToGain(int v) { return static_cast<float>(v) / 100.0f; }

bool parseLaunchArgs(int argc, const wchar_t* const* argv, LaunchOptions& lo, ViewerSettings& s) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](std::wstring* out) -> bool {
            if (i + 1 >= argc) {
                lo.error = L"missing value for " + a;
                return false;
            }
            *out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--synthetic") s.synthetic = true;
        else if (a == L"--real") s.synthetic = false;
        else if (a == L"--no-autostart") lo.autostart = false;
        else if (a == L"--clock-overlay") s.clock_overlay = true;
        else if (a == L"--exit-after-screenshot") lo.exit_after_screenshot = true;
        else if (a == L"--loopback-only") lo.serve_loopback_only = true;
        else if (a == L"--pair-on-start") lo.pair_on_start = true;
        else if (a == L"--show-health") lo.show_health = true;
        else if (a == L"--test-tools") lo.test_tools = true;
        else if (a == L"--test-second-display") lo.test_second_display = true;
        else if (a == L"--test-pet-camera") {
            lo.test_pet_camera = true;
            lo.serve_loopback_only = true; // its fake device is on this PC; nothing listens on the LAN
        } else if (a == L"--test-identity-reset") {
            lo.test_identity_reset = true;
            lo.serve_loopback_only = true; // as --test-pet-camera
        } else if (a == L"--test-camera-roll") {
            lo.test_camera_roll = true;
            lo.serve_loopback_only = true; // as --test-pet-camera: nothing listens on the LAN
        } else if (a == L"--test-outside") {
            lo.test_outside = true;
            lo.serve_loopback_only = true; // as --test-pet-camera: every listener on 127.0.0.1, the outside one too
        } else if (a == L"--camera-roll-dir") {
            if (!value(&v)) return false;
            lo.camera_roll_dir = normalizeStateDir(v);
            if (lo.camera_roll_dir.empty()) {
                lo.error = L"--camera-roll-dir needs a folder";
                return false;
            }
        } else if (a == L"--state-dir") {
            if (!value(&v)) return false;
            lo.state_dir = normalizeStateDir(v);
            if (lo.state_dir.empty()) {
                lo.error = L"--state-dir needs a folder";
                return false;
            }
        }
        else if (a == L"--screenshot") { if (!value(&lo.screenshot_path)) return false; }
        else if (a == L"--screenshot-after-ms") {
            if (!value(&v)) return false;
            lo.screenshot_after_ms = std::max(0, _wtoi(v.c_str()));
        } else if (a == L"--mode") {
            if (!value(&v)) return false;
            if (v == L"ir") s.mode = KV_MODE_IR;
            else if (v == L"depth") s.mode = KV_MODE_DEPTH;
            else if (v == L"color" || v == L"colour") s.mode = KV_MODE_COLOR;
            else if (v == L"sbs" || v == L"side-by-side") s.mode = KV_MODE_SIDE_BY_SIDE;
            else if (v == L"3d" || v == L"pointcloud") s.mode = KV_MODE_POINT_CLOUD_3D;
            else { lo.error = L"unknown mode: " + v; return false; }
        } else if (a == L"--point-color" || a == L"--point-colour") {
            if (!value(&v)) return false;
            if (v == L"depth") s.point_color_source = KV_POINT_COLOR_DEPTH;
            else if (v == L"ir") s.point_color_source = KV_POINT_COLOR_IR;
            else if (v == L"rgb" || v == L"color" || v == L"colour") s.point_color_source = KV_POINT_COLOR_REGISTERED_RGB;
            else { lo.error = L"unknown point colour: " + v; return false; }
        } else if (a == L"--point-size") {
            if (!value(&v)) return false;
            s.point_size = static_cast<float>(_wtof(v.c_str()));
            sanitize(s);
        } else if (a == L"--track-me") {
            applyTrackMe(s);
        } else if (a == L"--blend") {
            if (!value(&v)) return false;
            s.depth_color_blend_enabled = true;
            s.depth_color_blend_percent = static_cast<float>(_wtof(v.c_str()));
            sanitize(s);
        } else if (a == L"--sbs-layout") {
            if (!value(&v)) return false;
            if (v == L"ir-depth") s.sbs_layout = KV_SBS_IR_DEPTH;
            else if (v == L"color-depth") s.sbs_layout = KV_SBS_COLOR_DEPTH;
            else if (v == L"color-ir") s.sbs_layout = KV_SBS_COLOR_IR;
            else { lo.error = L"unknown sbs layout: " + v; return false; }
        } else if (a == L"--colormap") {
            if (!value(&v)) return false;
            if (v == L"turbo") s.depth_colormap = KV_COLORMAP_TURBO;
            else if (v == L"viridis") s.depth_colormap = KV_COLORMAP_VIRIDIS;
            else if (v == L"inferno") s.depth_colormap = KV_COLORMAP_INFERNO;
            else if (v == L"gray" || v == L"grayscale") s.depth_colormap = KV_COLORMAP_GRAY;
            else { lo.error = L"unknown colormap: " + v; return false; }
        } else if (a == L"--pipeline") {
            if (!value(&v)) return false;
            if (v == L"gl" || v == L"opengl") s.pipeline = KV_PIPELINE_OPENGL;
            else if (v == L"cpu") s.pipeline = KV_PIPELINE_CPU;
            else if (v == L"cl" || v == L"opencl") s.pipeline = KV_PIPELINE_OPENCL;
            else { lo.error = L"unknown or unsupported pipeline: " + v; return false; }
        } else {
            lo.error = L"unknown option: " + a;
            return false;
        }
    }
    // The roll's flags first: a test run that could reach a real folder or the real source stops here, before
    // the screenshot rule or anything else is looked at (v3 §10, pet-camera.md §6.6 "Test hooks").
    if ((!lo.camera_roll_dir.empty() || lo.test_camera_roll) && !cameraRollFlagsAcceptable(s, lo, &lo.error)) {
        lo.camera_roll_refused = true;
        return false;
    }
    if (lo.test_outside && !outsideTestFlagsAcceptable(s, lo, &lo.error)) {
        lo.outside_refused = true;
        return false;
    }
    if (!lo.screenshot_path.empty() && !screenshotAllowed(s, &lo.error)) {
        lo.screenshot_refused = true;
        return false;
    }
    // Checked here, before anything starts: the host would make the folder owner-only (stateDirAcceptable).
    if (!lo.state_dir.empty() && !stateDirAcceptable(lo.state_dir, &lo.error)) {
        lo.state_dir_refused = true;
        return false;
    }
    return true;
}

bool isTestLaunch(const ViewerSettings& s, const LaunchOptions& lo) {
    return s.synthetic || !lo.screenshot_path.empty() || lo.exit_after_screenshot || lo.serve_loopback_only ||
           lo.pair_on_start || lo.test_tools || lo.test_second_display || lo.test_pet_camera || lo.test_identity_reset ||
           lo.test_camera_roll || !lo.camera_roll_dir.empty() || lo.test_outside;
}

bool outsideTestFlagsAcceptable(const ViewerSettings& s, const LaunchOptions& lo, std::wstring* why) {
    if (!lo.test_outside) return true;
    if (!s.synthetic) {
        if (why) *why = L"--test-outside works only with --synthetic: the outside test hook never runs with the real sensor.";
        return false;
    }
    if (lo.state_dir.empty()) {
        if (why) {
            *why = L"--test-outside needs --state-dir: its pet-camera.json, invite and connection log stay in a folder of "
                   L"the test's own.";
        }
        return false;
    }
    return true;
}

namespace {
std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {0};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return std::wstring();
    return std::wstring(buf, n);
}

// A full path for comparing folders: GetFullPathNameW (a string operation only; nothing is opened), lower
// case, backslashes, no trailing one (but "c:\" kept).
std::wstring comparablePath(const std::wstring& p) {
    std::wstring d = normalizeStateDir(p);
    std::transform(d.begin(), d.end(), d.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return d;
}

bool isNamespaceOrUncPath(const std::wstring& d) {
    return d.size() >= 2 && (d[0] == L'\\' || d[0] == L'/') && (d[1] == L'\\' || d[1] == L'/');
}

bool isDriveRoot(const std::wstring& full) { return full.size() <= 3; } // "c:\" (comparablePath keeps it)

std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
} // namespace

std::wstring defaultCameraRollDir() {
    const std::wstring home = envVar(L"USERPROFILE");
    return home.empty() ? std::wstring() : home + L"\\kinect-viewer-camera-roll";
}

std::wstring realLaunchRollFolder(const std::string& roll_folder_setting, const std::wstring& default_dir) {
    if (!roll_folder_setting.empty()) return normalizeStateDir(widenUtf8(roll_folder_setting));
    return default_dir;
}

std::wstring realLaunchRollFolderNow(const std::string& roll_folder_setting) {
    return realLaunchRollFolder(roll_folder_setting, defaultCameraRollDir());
}

std::wstring knownRecordingsDir() {
    const std::wstring home = envVar(L"USERPROFILE");
    return (home.empty() ? std::wstring(L".") : home) + L"\\kinect-viewer-recordings";
}

bool pathIsSameOrInside(const std::wstring& a, const std::wstring& b) {
    const std::wstring pa = comparablePath(a), pb = comparablePath(b);
    if (pa.empty() || pb.empty()) return false;
    if (pa == pb) return true;
    const std::wstring prefix = pb.back() == L'\\' ? pb : pb + L'\\';
    return pa.size() > prefix.size() && pa.compare(0, prefix.size(), prefix) == 0;
}

bool cameraRollTestDirAcceptable(const std::wstring& dir, std::wstring* why) {
    auto refuse = [&](const std::wstring& reason) {
        if (why) *why = reason;
        return false;
    };
    if (dir.empty()) return refuse(L"--camera-roll-dir needs a folder.");
    if (isNamespaceOrUncPath(dir)) {
        return refuse(L"--camera-roll-dir must be an ordinary folder on a local drive, not " + dir);
    }
    if (isDriveRoot(comparablePath(dir))) return refuse(L"--camera-roll-dir can't be a drive root: " + dir);
    // The real folders, by path only: none of them is opened, listed or created to decide.
    const std::wstring home = envVar(L"USERPROFILE");
    struct Known {
        std::wstring path;
        const wchar_t* what;
    };
    std::vector<Known> known = {{knownRecordingsDir(), L"the depth/IR recordings folder"}};
    if (const std::wstring roll = defaultCameraRollDir(); !roll.empty()) known.push_back({roll, L"the real camera roll"});
    if (!home.empty()) known.push_back({home + L"\\kinect-viewer", L"the real host state"});
    for (const auto& k : known) {
        if (pathIsSameOrInside(dir, k.path) || pathIsSameOrInside(k.path, dir)) {
            return refuse(L"--camera-roll-dir can't be " + std::wstring(k.what) + L" (" + k.path +
                          L"), a folder inside it or one holding it: a test records only into a folder of its own, "
                          L"such as one under the build tree. Refused: " + dir);
        }
    }
    return true;
}

std::optional<std::wstring> resolvedFolderPath(const std::wstring& dir, std::wstring* why) {
    const std::wstring full = normalizeStateDir(dir);
    if (full.empty()) {
        if (why) *why = L"--camera-roll-dir needs a folder.";
        return std::nullopt;
    }
    // The nearest part of the path that is there (the folder itself, or a parent of a folder not made yet), as
    // the file system has it; the parts not made yet are put back after it. Only attributes are opened.
    std::wstring head = full, tail;
    for (;;) {
        const DWORD attrs = GetFileAttributesW(head.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) break;
        const DWORD e = GetLastError();
        const size_t cut = head.find_last_of(L'\\');
        if ((e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) || cut == std::wstring::npos || cut < 2) {
            if (why) *why = L"--camera-roll-dir couldn't be checked (error " + std::to_wstring(e) + L"): " + full;
            return std::nullopt;
        }
        tail = head.substr(cut + 1) + (tail.empty() ? L"" : L"\\" + tail);
        head = head.substr(0, cut);
        if (head.size() == 2 && head[1] == L':') head += L'\\'; // "c:" is the drive's current folder; "c:\" its root
    }
    HANDLE h = CreateFileW(head.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why) *why = L"--camera-roll-dir couldn't be checked (error " + std::to_wstring(GetLastError()) + L"): " + full;
        return std::nullopt;
    }
    std::wstring buf(1024, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buf.size()) {
        buf.assign(static_cast<size_t>(n) + 1, L'\0');
        n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    const DWORD e = GetLastError();
    CloseHandle(h);
    if (n == 0 || n >= buf.size()) {
        if (why) *why = L"--camera-roll-dir couldn't be checked (error " + std::to_wstring(e) + L"): " + full;
        return std::nullopt;
    }
    buf.resize(n);
    // "\\?\C:\x" is "C:\x"; "\\?\UNC\server\share" is "\\server\share" (refused as a network path by the caller).
    if (buf.rfind(L"\\\\?\\UNC\\", 0) == 0) buf = L"\\\\" + buf.substr(8);
    else if (buf.rfind(L"\\\\?\\", 0) == 0) buf = buf.substr(4);
    if (!tail.empty()) buf += (buf.back() == L'\\' ? L"" : L"\\") + tail;
    return buf;
}

bool cameraRollTestDirContentsAcceptable(const std::wstring& dir, std::wstring* why) {
    auto refuse = [&](const std::wstring& reason) {
        if (why) *why = reason;
        return false;
    };
    const std::optional<std::wstring> real = resolvedFolderPath(dir, why);
    if (!real) return false;
    // The path-text refusals again, on the path the file system resolves (a junction, a subst drive or an 8.3
    // name can reach a real folder under another name); still nothing in the folder is read to decide.
    std::wstring text_why;
    if (!cameraRollTestDirAcceptable(*real, &text_why)) {
        return refuse(text_why + (_wcsicmp(real->c_str(), normalizeStateDir(dir).c_str()) != 0 ? L" (it resolves to " + *real + L")" : L""));
    }
    const DWORD attrs = GetFileAttributesW(real->c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true; // new: the host creates it
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return refuse(L"--camera-roll-dir names a file, not a folder: " + dir);
    // Empty, or a folder a test made (the host leaves kTestRollMarker in a test roll it prepares). Anything
    // else may be someone's real camera roll, so a test never records into it.
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileExW((*real + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return true; // nothing in it
        return refuse(L"--camera-roll-dir couldn't be checked (error " + std::to_wstring(e) + L"): " + dir);
    }
    bool marker = false, other = false;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (_wcsicmp(name.c_str(), kTestRollMarker) == 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) marker = true;
        else other = true;
    } while (!marker && FindNextFileW(f, &fd));
    FindClose(f);
    if (marker || !other) return true;
    return refuse(L"--camera-roll-dir must be a new or empty folder, or one a test made (it holds " +
                  std::wstring(kTestRollMarker) + L"). " + dir +
                  L" holds other files and no such marker, so it may be a real camera roll: a test never records "
                  L"into one.");
}

bool cameraRollFlagsAcceptable(const ViewerSettings& s, const LaunchOptions& lo, std::wstring* why) {
    auto refuse = [&](const std::wstring& reason) {
        if (why) *why = reason;
        return false;
    };
    if (lo.test_camera_roll && !s.synthetic) {
        return refuse(L"--test-camera-roll works only with --synthetic: the roll's test hook never records the real "
                      L"sensor.");
    }
    if (lo.test_camera_roll && lo.camera_roll_dir.empty()) {
        return refuse(L"--test-camera-roll needs --camera-roll-dir: a test never records into the real camera roll.");
    }
    if (!lo.camera_roll_dir.empty() && !s.synthetic) {
        return refuse(L"--camera-roll-dir is for synthetic test runs only (with --synthetic): the real sensor records "
                      L"only into the camera roll the PC app chose.");
    }
    // The path text first (nothing is opened for a real folder named outright), then what is on disk.
    return cameraRollTestDirAcceptable(lo.camera_roll_dir, why) && cameraRollTestDirContentsAcceptable(lo.camera_roll_dir, why);
}

std::wstring cameraRollDirForLaunch(bool test_launch, const std::wstring& camera_roll_dir,
                                    const std::string& roll_folder_setting) {
    if (test_launch) return camera_roll_dir; // v3 §10: only the folder the test named, or no roll at all
    return realLaunchRollFolderNow(roll_folder_setting);
}

std::vector<std::wstring> oneDriveRootsFromEnvironment() {
    std::vector<std::wstring> roots;
    for (const wchar_t* name : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
        const std::wstring r = envVar(name);
        if (!r.empty()) roots.push_back(r);
    }
    return roots;
}

bool rollFolderChoiceAcceptable(const std::wstring& dir, const std::wstring& recordings_dir,
                                const std::vector<std::wstring>& onedrive_roots, unsigned drive_type, std::wstring* why) {
    auto refuse = [&](const std::wstring& reason) {
        if (why) *why = reason;
        return false;
    };
    if (dir.empty()) return refuse(L"Choose a folder for the camera roll.");
    if (isNamespaceOrUncPath(dir)) return refuse(L"The camera roll can't be on a network path: " + dir);
    const std::wstring full = comparablePath(dir);
    if (full.size() < 3 || full[1] != L':' || full[2] != L'\\') {
        return refuse(L"The camera roll needs a folder on a local drive: " + dir);
    }
    if (drive_type == DRIVE_REMOTE) return refuse(L"The camera roll can't be on a network drive: " + dir);
    if (drive_type == DRIVE_NO_ROOT_DIR || drive_type == DRIVE_UNKNOWN) return refuse(L"There is no such drive: " + dir);
    if (isDriveRoot(full)) return refuse(L"The camera roll can't be a whole drive; choose a folder on it: " + dir);
    for (const auto& r : onedrive_roots) {
        if (!r.empty() && pathIsSameOrInside(dir, r)) {
            return refuse(L"The camera roll can't be inside OneDrive (it would leave this PC): " + dir);
        }
    }
    if (!recordings_dir.empty() && (pathIsSameOrInside(dir, recordings_dir) || pathIsSameOrInside(recordings_dir, dir))) {
        return refuse(L"The camera roll can't be the depth/IR recordings folder, a folder inside it or one holding it "
                      L"(that folder's purge deletes anything else): " + dir);
    }
    return true;
}

std::wstring normalizeStateDir(const std::wstring& dir) {
    if (dir.empty()) return std::wstring();
    std::wstring d = dir;
    std::replace(d.begin(), d.end(), L'/', L'\\');
    wchar_t full[MAX_PATH * 2] = {0};
    const DWORD n = GetFullPathNameW(d.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    if (n > 0 && n < std::size(full)) d = full;
    while (d.size() > 3 && d.back() == L'\\') d.pop_back(); // keep "C:\"
    return d;
}

std::wstring temporaryStateDir(const wchar_t* prefix) {
    wchar_t tmp[MAX_PATH + 1] = {0};
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    std::wstring base = (n > 0 && n <= MAX_PATH) ? std::wstring(tmp) : std::wstring(L".\\");
    if (base.back() != L'\\') base += L'\\';
    return base + prefix + L"-" + std::to_wstring(GetCurrentProcessId()) + L"\\.host-state";
}

bool removeTemporaryStateDir(const std::wstring& state_dir, const wchar_t* prefix) {
    const std::wstring mine = temporaryStateDir(prefix);
    if (state_dir.empty() || _wcsicmp(state_dir.c_str(), mine.c_str()) != 0) return false;
    const std::filesystem::path root = std::filesystem::path(mine).parent_path(); // %TEMP%\<prefix>-<pid>
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return !std::filesystem::exists(root, ec);
}

std::wstring freshTemporaryStateDir(const wchar_t* prefix, std::wstring* why) {
    if (why) why->clear();
    const std::wstring dir = temporaryStateDir(prefix);
    const std::filesystem::path root = std::filesystem::path(dir).parent_path(); // %TEMP%\<prefix>-<pid>
    std::error_code ec;
    if (std::filesystem::exists(root, ec) && !removeTemporaryStateDir(dir, prefix)) {
        if (why) *why = L"a folder an earlier run left behind could not be removed: " + root.wstring();
        return std::wstring();
    }
    return dir;
}

bool stateDirAcceptable(const std::wstring& dir, std::wstring* why) {
    namespace fs = std::filesystem;
    auto refuse = [&](const std::wstring& reason) {
        if (why) *why = reason;
        return false;
    };
    if (dir.empty()) return refuse(L"--state-dir needs a folder.");
    // Win32 namespace paths (\\?\, \\.\) hide what they point at: only ordinary paths.
    if (dir.size() >= 4 && (dir[0] == L'\\' || dir[0] == L'/') && (dir[1] == L'\\' || dir[1] == L'/') &&
        (dir[2] == L'?' || dir[2] == L'.') && (dir[3] == L'\\' || dir[3] == L'/')) {
        return refuse(L"--state-dir must be an ordinary folder path, not " + dir);
    }
    // A drive root ("C:\") or a share's root ("\\server\share"), whatever is in it.
    const fs::path p(dir);
    const std::wstring root_name = p.root_name().wstring();
    const bool unc = root_name.size() > 2 && (root_name[0] == L'\\' || root_name[0] == L'/') &&
                     (root_name[1] == L'\\' || root_name[1] == L'/');
    size_t parts = 0;
    for (const auto& part : p.relative_path()) parts += part.empty() ? 0 : 1;
    if (parts == 0 || (unc && parts <= 1)) {
        return refuse(L"--state-dir can't be a drive root or a share's root: " + dir);
    }
    // A junction or symbolic link (any reparse point) is refused whatever it points at: the owner-only
    // DACL would land on the folder it leads to. Checked on the path itself, before anything follows it.
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true; // the host creates it, owner-only
        return refuse(L"--state-dir can't be read: " + dir);
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        return refuse(L"--state-dir can't be a junction or a link: " + dir);
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return refuse(L"--state-dir is not a folder: " + dir);
    std::error_code ec;
    bool empty = true;
    fs::directory_iterator it(p, ec);
    if (ec) return refuse(L"--state-dir can't be read: " + dir);
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) return refuse(L"--state-dir can't be read: " + dir);
        empty = false;
        break;
    }
    if (ec) return refuse(L"--state-dir can't be read: " + dir);
    if (empty) return true;
    // Not empty: only the host's own folder. A file name proves nothing (any folder can hold a
    // devices.json); the host's marker, or a pairing.json that reads as the host's (an install from
    // before the marker), does.
    const fs::path marker = p / kvhost::kHostStateMarkerFile;
    const DWORD marker_attrs = GetFileAttributesW(marker.wstring().c_str());
    if (marker_attrs != INVALID_FILE_ATTRIBUTES &&
        !(marker_attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        return true;
    }
    {
        const fs::path pairing = p / L"pairing.json";
        std::error_code fec;
        const DWORD pairing_attrs = GetFileAttributesW(pairing.wstring().c_str());
        const uintmax_t size = fs::file_size(pairing, fec);
        if (pairing_attrs != INVALID_FILE_ATTRIBUTES &&
            !(pairing_attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && !fec &&
            size <= 64u * 1024u) {
            std::ifstream f(pairing, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            kvhost::HostPersistedState st;
            if (f.good() || f.eof()) {
                if (kvhost::fromJson(text, st)) return true; // the host's own folder
            }
        }
    }
    return refuse(L"--state-dir must be a new or empty folder, or one the host already keeps its state in (its "
                  L"pairing.json, or its " + std::wstring(kvhost::kHostStateMarkerFile) +
                  L" marker). The host makes it owner-only, down to every file in it, and " + dir +
                  L" holds other things.");
}

bool screenshotAllowed(const ViewerSettings& s, std::wstring* why) { return screenshotAllowed(s, false, why); }

bool screenshotAllowed(const ViewerSettings& s, bool replaying, std::wstring* why) {
    return screenshotAllowed(s, replaying, false, why);
}

bool screenshotAllowed(const ViewerSettings& s, bool replaying, bool roll_on_screen, std::wstring* why) {
    return screenshotAllowed(s, replaying, roll_on_screen, false, why);
}

bool screenshotAllowed(const ViewerSettings& s, bool replaying, bool roll_on_screen, bool invite_on_screen, std::wstring* why) {
    if (invite_on_screen) {
        // v3 §10: the invite window shows a token anyone could redeem once; no capture ever holds one.
        if (why) *why = L"--screenshot is refused while the invite window is open: it shows a live invite token.";
        return false;
    }
    if (roll_on_screen) {
        // pet-camera.md §6.6: colour from the camera roll is shown only in its own player and browser, and never
        // captured, whatever the main view shows.
        if (why) {
            *why = L"--screenshot is refused while a camera-roll clip is on screen (the player window, or the roll "
                   L"browser's thumbnails): colour from the roll is never captured.";
        }
        return false;
    }
    if (replaying) {
        if (why) *why = L"--screenshot is refused while a depth/IR recording replays in the view: those are real frames.";
        return false;
    }
    if (!s.synthetic) {
        if (why) *why = L"--screenshot works only with --synthetic: a capture of the real sensor would put its frames on disk.";
        return false;
    }
    if (settingsNeedColor(s)) {
        if (why) {
            *why = L"--screenshot is refused in Colour, in Side-by-Side layouts with colour, with the depth/colour blend and "
                   L"in 3D with RGB points: colour is live only.";
        }
        return false;
    }
    return true;
}

bool recordingAllowedInMode(kv_view_mode_t mode) {
    return mode == KV_MODE_IR || mode == KV_MODE_DEPTH;
}

bool recordingAllowed(const ViewerSettings& s) {
    return recordingAllowedInMode(s.mode) && !s.depth_color_blend_enabled;
}

std::optional<std::string> recordingDisabledReason(kv_view_mode_t mode, bool replaying) {
    if (!recordingAllowedInMode(mode)) return std::string(kvhost::text::kColourLiveOnly);
    if (replaying) return std::string(kExitPlaybackFirst);
    return std::nullopt;
}

std::optional<std::string> recordingDisabledReason(const ViewerSettings& s, bool replaying) {
    if (!recordingAllowed(s)) return std::string(kvhost::text::kColourLiveOnly);
    return recordingDisabledReason(s.mode, replaying);
}

bool viewChangeStopsRecording(const ViewerSettings& during, const ViewerSettings& after) {
    return during.mode != after.mode || (during.mode == KV_MODE_SIDE_BY_SIDE && during.sbs_layout != after.sbs_layout) ||
           (!during.depth_color_blend_enabled && after.depth_color_blend_enabled);
}

} // namespace kvwin
