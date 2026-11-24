// kinect-viewer.exe: Win32 + Direct3D 11 live viewer for the Kinect v2 (or the synthetic source).
// Modes: IR, Colour, Depth false colour, Side by Side, 3D point cloud. Controls mirror the Mac host's
// sidebar, with the depth cut-off and the depth/colour blend (stage 5).
// Flags: --synthetic, --no-autostart, --mode, --sbs-layout, --colormap, --pipeline, --clock-overlay,
//        --point-color, --point-size, --track-me, --blend,
//        test hook --screenshot PATH [--screenshot-after-ms N] [--exit-after-screenshot].

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <objbase.h>
#include <shellapi.h>
#include <timeapi.h>
#include <dwmapi.h>

#include "d3d_preview.hpp"
#include "display_targets.hpp"
#include "inspection.hpp"
#include "pet_camera_model.hpp"
#include "pet_camera_ui.hpp"
#include "point_cloud_camera.hpp"
#include "point_cloud_renderer.hpp"
#include "presets.hpp"
#include "second_display.hpp"
#include "viewer_app.hpp"
#include "viewer_model.hpp"
#include "window_capture.hpp"
#include "capture_exclusion.hpp"
#include "ui_mailbox.hpp"
#ifdef KV_WITH_RECORDER
#include "recorder.hpp"
#include "recordings.hpp"
#endif
#ifdef KV_WITH_STREAMING
#include "roll_ui.hpp" // W7: the camera roll's browser, player and schedule editor (they need the streaming host)
#include "outside_ui.hpp" // W8: the invite, re-share and connection log windows (they need the streaming host)
#endif
#include <shobjidl.h> // the camera roll's "Change..." folder picker

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace kvwin {

namespace {

enum ControlId : int {
    ID_SOURCE = 100, ID_PIPELINE, ID_START, ID_STOP, ID_MODE, ID_SBS, ID_OVERLAY,
    ID_IR_AUTO, ID_GAMMA, ID_EMA, ID_BLACK, ID_WHITE, ID_RESET_IR,
    ID_COLORMAP, ID_DMIN, ID_DMAX, ID_DESK, ID_SPEC, ID_ROOM, ID_INVERT,
    ID_EXPOSURE, ID_EV, ID_SEMI, ID_MANUAL, ID_GAIN,
    ID_PAIR, ID_UNPAIR, ID_ALLOW_SWITCH, ID_REC_FORMAT, ID_RECORD,
    ID_ALLOW_POWER, ID_OFF_ON_DISCONNECT,
    ID_PRESET, ID_PRESET_SAVE, ID_PRESET_DELETE, ID_MIRROR, ID_ROTATE, ID_HEALTH, ID_HELP, ID_RECORDINGS,
    ID_PC_SOURCE, ID_PC_SIZE, ID_PC_RESET, ID_CUT_ENABLE, ID_CUT_TRACKME, ID_CUT_NEAR, ID_CUT_FAR, ID_BLEND_ENABLE, ID_BLEND,
    ID_OUT_DISPLAY, ID_OUT_REFRESH, ID_OUT_TOGGLE, ID_OUT_BLACKOUT,
    ID_PET_PRIVACY, ID_PET_SHARE, ID_PET_SCHEDULE, ID_PET_DEVICES, ID_PET_ID_RETRY, ID_PET_ID_RESET,
    ID_ROLL_ENABLE, ID_ROLL_RECORD, ID_ROLL_STOP, ID_ROLL_MOTION, ID_ROLL_SENS, ID_ROLL_SCHED_ON, ID_ROLL_SCHED,
    ID_ROLL_FOLDER, ID_ROLL_KEEP_APPLY, ID_ROLL_BROWSER, ID_ROLL_CONFIRM_LIMITS,
    ID_OUTSIDE_ALLOW, ID_OUTSIDE_PORT, ID_OUTSIDE_ADDR_SAVE, ID_OUTSIDE_RESHARE, ID_OUTSIDE_INVITE, ID_OUTSIDE_LOG,
    ID_OUTSIDE_REMOVE_MAPPING, ID_OUTSIDE_REMAP,
};

constexpr UINT_PTR kTimerStats = 1;
constexpr UINT_PTR kTimerScreenshot = 2;
constexpr UINT_PTR kTimerReconnect = 3;
constexpr UINT_PTR kTimerTestTools = 4;
constexpr UINT_PTR kTimerRetention = 5; // hourly: recordings 7 days old are deleted
constexpr UINT_PTR kTimerPetTest = 6;   // --test-pet-camera: the hook's steps, every 100 ms
constexpr UINT_PTR kTimerIdTest = 7;    // --test-identity-reset: the hook's steps, every 100 ms
constexpr UINT_PTR kTimerRollTest = 8;  // --test-camera-roll: the hook's steps, every 100 ms
constexpr UINT_PTR kTimerOutsideTest = 9; // --test-outside: the hook's steps, every 100 ms
constexpr UINT WM_APP_PHONE_MODE = WM_APP + 1;
constexpr UINT WM_APP_STREAM_STATE = WM_APP + 2;
constexpr UINT WM_APP_PHONE_POWER = WM_APP + 3;   // wParam: 1 On, 0 Off
// The phone's commands, its orbit gestures and the clips a phone owner deletes come through mailboxes, never as a
// pointer in the message: the message only says "take what is waiting" (ui_mailbox.hpp).
constexpr UINT WM_APP_PHONE_COMMAND = WM_APP + 4; // g_phone_commands
constexpr UINT WM_APP_PHONE_ORBIT = WM_APP + 5;   // g_phone_orbits
constexpr UINT WM_APP_PET_STATE = WM_APP + 6;     // devices, sessions or pet-camera settings changed
constexpr UINT WM_APP_ROLL_CLIP = WM_APP + 7;     // a camera-roll clip was finalized (the browser refreshes)
// A phone owner is deleting a camera-roll clip the PC may be playing (its id in g_roll_clips_deleted); the player
// closes on it and lets go of the clip before the host deletes it.
constexpr UINT WM_APP_ROLL_CLIP_DELETED = WM_APP + 8;
UiMailbox<kvhost::RemoteControlCommandMessage> g_phone_commands;
UiMailbox<kvhost::Orbit3DGestureMessage> g_phone_orbits;
UiMailbox<std::string> g_roll_clips_deleted;
// A WM_APP_PET_STATE is in the queue: the host's reports coalesce into one refresh.
std::atomic<bool> g_pet_state_posted{false};
// The Mac's 3D mouse (PointCloud3DCanvasView.swift): radians and metres per pixel dragged, and the
// zoom per wheel notch.
constexpr float kOrbitRadPerPx = 0.0065f;
constexpr float kPanMetresPerPx = 0.0032f;
constexpr double kZoomPerNotch = 0.08; // factor exp(0.08) a notch


std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, n - 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

const wchar_t* modeLabel(kv_view_mode_t m) {
    switch (m) {
        case KV_MODE_IR: return L"IR";
        case KV_MODE_COLOR: return L"Colour";
        case KV_MODE_DEPTH: return L"Depth";
        case KV_MODE_SIDE_BY_SIDE: return L"Side by Side";
        case KV_MODE_POINT_CLOUD_3D: return L"3D";
    }
    return L"?";
}

const wchar_t* pointSourceLabel(kv_point_color_source_t c) {
    switch (c) {
        case KV_POINT_COLOR_DEPTH: return L"depth colours";
        case KV_POINT_COLOR_IR: return L"IR";
        case KV_POINT_COLOR_REGISTERED_RGB: return L"live colour";
    }
    return L"?";
}

uint64_t unixMsNow() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000ull;
}

const wchar_t* pipelineLabel(kv_pipeline_kind_t p) {
    switch (p) {
        case KV_PIPELINE_OPENGL: return L"OpenGL";
        case KV_PIPELINE_OPENCL: return L"OpenCL";
        case KV_PIPELINE_CPU: return L"CPU";
    }
    return L"?";
}

} // namespace

struct ViewerApp::Impl final : kvhost::SensorBackend {
    // Every On/Off (Start/Stop here, the phone's switch, sleep, USB) goes through this, on the UI
    // thread, so the phone hears only states this PC confirmed.
    std::unique_ptr<kvhost::HostPowerModel> power;
    uint64_t reopen_generation = 0; // what a wake or replug reopen must still match
    std::string reopen_missing_note;

    // ---- kvhost::SensorBackend (UI thread) ----
    std::optional<std::string> externalLockHolder() override {
        if (s.synthetic) return std::nullopt; // the synthetic source takes no lock
        char owner[256] = {0};
        if (!kv_is_locked_by_other(nullptr, owner, sizeof(owner))) return std::nullopt;
        return std::string(owner);
    }
    OpenResult open() override {
        startEngine();
        return lastOpenResult();
    }
    void close() override { stopEngine(); } // stops a recording first, then the engine (USB and lock)
    bool isOpen() const override { return running; }

    OpenResult lastOpenResult() {
        kv_stats_snapshot_t st;
        kv_engine_get_stats(eng, &st);
        OpenResult r;
        r.ok = running;
        r.lock_blocked = !running && st.lock_blocked;
        r.error = narrow(error);
        return r;
    }
    // Restart an open sensor in place (source, pipeline, colour) and tell the phone how it went.
    void restartInPlace(bool force_rgb = false) {
        startEngine(force_rgb);
        if (power) power->reportRestart(lastOpenResult());
    }
    void onPowerChanged(const kvhost::HostPowerModel::Snapshot& p) {
        if (hooks) hooks->onPowerChanged(p);
        refreshEnabled();
        updateStatus();
    }
    kvhost::HostViewState viewState() {
        kvhost::HostViewState v;
        v.modeRaw = static_cast<int>(s.mode);
        v.sbsLayoutRaw = static_cast<int>(s.sbs_layout);
        // IR and Depth record (with the reminder, here or on the phone); colour never (nor the blend);
        // not during a playback.
        v.canRecordCurrentMode = recordingAllowed(s) && !replaying;
        v.recordingDisabledReason = recordingDisabledReason(s, replaying);
        v.cutoffEnabled = s.cutoff_enabled;
        v.cutoffNearMm = s.cutoff_near_mm;
        v.cutoffFarMm = s.cutoff_far_mm;
#ifdef KV_WITH_RECORDER
        v.recordingReminderNeeded = reminder.needed(kvrec::unixNow());
#endif
        v.isReplayingOnHost = replaying;
        v.isHostFrozen = frozen;
#ifdef KV_WITH_RECORDER
        if (recorder.isRecording()) {
            const kvrec::RecorderStatus st = recorder.status();
            v.isRecording = true;
            v.recordingFrames = st.frames_written;
            v.recordingElapsedSec = st.seconds;
        }
#endif
        return v;
    }
    void publishViewState() {
        if (hooks) hooks->onViewStateChanged(viewState());
    }

    HINSTANCE inst = nullptr;
    HWND main = nullptr;
    HWND preview = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;
    HFONT bold = nullptr;
    int dpi = 96;
    int panel_w = 0;
    int panel_bottom = 0;

    kv_engine_t* eng = nullptr;
    ViewerSettings s;
    LaunchOptions lo;
    D3DPreview d3d;
    std::vector<uint8_t> frame;
    uint64_t last_seq = 0;
    kv_mapped_frame_t meta{};
    bool running = false;
    bool needs_redraw = true;
    std::wstring error;
    std::wstring hardware_note;
    bool resume_after_wake = false;
    bool awaiting_replug = false;

    StreamHooks* hooks = nullptr;

    // Controls
    HWND cbSource{}, cbPipeline{}, btnStart{}, btnStop{}, cbMode{}, cbSbs{}, chkOverlay{};
    HWND chkIrAuto{}, tbGamma{}, lblGamma{}, tbEma{}, lblEma{}, tbBlack{}, lblBlack{}, tbWhite{}, lblWhite{}, btnResetIr{};
    HWND cbColormap{}, tbDmin{}, lblDmin{}, tbDmax{}, lblDmax{}, btnDesk{}, btnSpec{}, btnRoom{}, chkInvert{};
    HWND cbExposure{}, tbEv{}, lblEv{}, tbSemi{}, lblSemi{}, tbManual{}, lblManual{}, tbGain{}, lblGain{};
    HWND btnPair{}, btnUnpair{}, chkAllowSwitch{}, chkAllowPower{}, chkOffOnDisconnect{}, lblStream{};
    HWND cbRecFormat{}, btnRecord{}, lblRec{}, ttRecord{};
    HWND cbPcSource{}, tbPcSize{}, lblPcSize{}, btnPcReset{}, lblPcHint{};
    HWND chkCut{}, btnTrackMe{}, tbCutNear{}, lblCutNear{}, tbCutFar{}, lblCutFar{};
    HWND chkBlend{}, tbBlend{}, lblBlend{};
    std::wstring rec_tip_text; // kept alive for the tooltip

    // ---- stage 5: the 3D point cloud ------------------------------------------------------------------
    // The core's inputs (undistorted depth, point colours, intrinsics) are kept from the last frame so a
    // camera move re-renders at once, a frozen 3D view included. Memory only.
    PointCloudRenderer pc;
    std::string pc_init_error;
    PointCloudCamera cam;
    std::vector<float> pc_depth;
    std::vector<uint8_t> pc_bgra;
    kv_ir_intrinsics_t pc_intr{};
    uint64_t pc_seq = 0;
    uint64_t pc_arrival_ns = 0;
    uint64_t pc_frame_seq = 0;
    bool pc_have = false;   // inputs are cached
    bool pc_dirty = false;  // the camera, the point size or the orientation changed: render again
    std::wstring pc_note;   // why nothing is drawn (no intrinsics, no renderer)
    std::vector<uint64_t> pc_times; // render times (ns) in the last second, for the 3D frame rate
    uint64_t pc_rendered = 0;
    double pc_render_ms = 0.0;
    bool in3d() const { return s.mode == KV_MODE_POINT_CLOUD_3D; }
#ifdef KV_WITH_RECORDER
    kvrec::Recorder recorder;
    std::vector<uint16_t> raw16;
    uint64_t last_raw_seq = 0;
    ViewerSettings rec_settings; // the view the running recording was started in
    std::wstring rec_note;
#endif

    int S(int v) const { return MulDiv(v, dpi, 96); }

    // ---- control creation ---------------------------------------------------------------------
    int y = 0;
    const int x0 = 12;

    HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int yy, int w, int h, int id) {
        // In the scrolling panel (it forwards its controls' notifications to the main window).
        HWND hw = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(yy), S(w), S(h), panelWnd ? panelWnd : main,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        panel_controls.push_back(hw); // every panel control; hidden in full screen
        return hw;
    }
    void section(const wchar_t* text) {
        y += 6;
        HWND h = make(L"STATIC", text, SS_LEFT, x0, y, 280, 18, 0);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bold), TRUE);
        y += 20;
    }
    HWND labeledCombo(const wchar_t* label, int id, std::initializer_list<const wchar_t*> items, int sel) {
        make(L"STATIC", label, SS_LEFT | SS_CENTERIMAGE, x0, y, 82, 22, 0);
        HWND cb = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x0 + 86, y, 196, 200, id);
        for (auto* it : items) SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it));
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
        y += 27;
        return cb;
    }
    HWND slider(int id, HWND* label_out, int lo_v, int hi_v, int value) {
        *label_out = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, x0, y, 110, 22, 0);
        HWND tb = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x0 + 112, y, 172, 24, id);
        SendMessageW(tb, TBM_SETRANGEMIN, FALSE, lo_v);
        SendMessageW(tb, TBM_SETRANGEMAX, FALSE, hi_v);
        SendMessageW(tb, TBM_SETPOS, TRUE, value);
        y += 26;
        return tb;
    }
    HWND check(const wchar_t* text, int id, bool on) {
        HWND c = make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, 280, 22, id);
        SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 24;
        return c;
    }

    void createControls() {
        y = 10;
        section(L"Source");
        cbSource = labeledCombo(L"Source", ID_SOURCE, {L"Kinect v2", L"Synthetic pattern"}, s.synthetic ? 1 : 0);
        cbPipeline = labeledCombo(L"Pipeline", ID_PIPELINE, {L"OpenGL (gl)", L"CPU (cpu)"},
                                  s.pipeline == KV_PIPELINE_CPU ? 1 : 0);
        btnStart = make(L"BUTTON", L"Start", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 136, 26, ID_START);
        btnStop = make(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, x0 + 146, y, 136, 26, ID_STOP);
        y += 32;

        section(L"View");
        cbMode = labeledCombo(L"Mode", ID_MODE, {L"IR", L"Colour", L"Depth (false colour)", L"Side by Side", L"3D point cloud"},
                              s.mode);
        cbSbs = labeledCombo(L"Side by side", ID_SBS, {L"IR + Depth", L"Colour + Depth", L"Colour + IR"}, s.sbs_layout);
        chkOverlay = check(L"Clock barcode overlay (synthetic only)", ID_OVERLAY, s.clock_overlay);
        chkMirror = make(L"BUTTON", L"Mirror (M)", BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, 100, 22, ID_MIRROR);
        chkRotate = make(L"BUTTON", L"Rotate 180° (R)", BS_AUTOCHECKBOX | WS_TABSTOP, x0 + 104, y, 130, 22, ID_ROTATE);
        SendMessageW(chkMirror, BM_SETCHECK, s.mirror_horizontal ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(chkRotate, BM_SETCHECK, s.rotate_180 ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 24;
        chkHealth = make(L"BUTTON", L"Health display (H)", BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, 150, 22, ID_HEALTH);
        SendMessageW(chkHealth, BM_SETCHECK, show_health ? BST_CHECKED : BST_UNCHECKED, 0);
        btnHelp = make(L"BUTTON", L"Shortcuts (?)", BS_PUSHBUTTON | WS_TABSTOP, x0 + 186, y, 98, 22, ID_HELP);
        y += 26;
        cbPreset = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x0, y, 150, 240, ID_PRESET);
        btnPresetSave = make(L"BUTTON", L"Save as…", BS_PUSHBUTTON | WS_TABSTOP, x0 + 154, y, 66, 24, ID_PRESET_SAVE);
        btnPresetDelete = make(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, x0 + 224, y, 60, 24, ID_PRESET_DELETE);
        y += 28;
        populatePresets();

        // Stage 5 (the Mac's phase 3).
        section(L"3D point cloud (mode 5)");
        cbPcSource = labeledCombo(L"Point colour", ID_PC_SOURCE, {L"Depth colours", L"IR", L"Live colour (RGB)"},
                                  static_cast<int>(s.point_color_source));
        tbPcSize = slider(ID_PC_SIZE, &lblPcSize, kPointSizeSliderMin, kPointSizeSliderMax, pointSizeToSlider(s.point_size));
        btnPcReset = make(L"BUTTON", L"Reset 3D view", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 110, 24, ID_PC_RESET);
        lblPcHint = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, x0 + 116, y, 168, 24, 0);
        y += 28;

        section(L"Depth cut-off (all modes, 3D too)");
        chkCut = make(L"BUTTON", L"Cut off outside", BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, 150, 22, ID_CUT_ENABLE);
        SendMessageW(chkCut, BM_SETCHECK, s.cutoff_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        btnTrackMe = make(L"BUTTON", L"Track me (0.5-2.0 m)", BS_PUSHBUTTON | WS_TABSTOP, x0 + 150, y, 134, 22, ID_CUT_TRACKME);
        y += 26;
        tbCutNear = slider(ID_CUT_NEAR, &lblCutNear, kCutNearSliderMin, kCutNearSliderMax, cutoffToSlider(s.cutoff_near_mm));
        tbCutFar = slider(ID_CUT_FAR, &lblCutFar, kCutFarSliderMin, kCutFarSliderMax, cutoffToSlider(s.cutoff_far_mm));
        for (HWND tb : {tbCutNear, tbCutFar}) {
            SendMessageW(tb, TBM_SETLINESIZE, 0, 50);
            SendMessageW(tb, TBM_SETPAGESIZE, 0, 250);
        }

        section(L"Depth / colour blend (live only)");
        chkBlend = check(L"Depth colours over the live colour", ID_BLEND_ENABLE, s.depth_color_blend_enabled);
        tbBlend = slider(ID_BLEND, &lblBlend, kBlendSliderMin, kBlendSliderMax,
                         static_cast<int>(std::lround(s.depth_color_blend_percent)));

        // Stage 6 (the Mac's phase 4.2).
        section(L"Second display (projector)");
        cbOutDisplay = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x0, y, 250, 200, ID_OUT_DISPLAY);
        btnOutRefresh = make(L"BUTTON", L"↻", BS_PUSHBUTTON | WS_TABSTOP, x0 + 254, y, 30, 24, ID_OUT_REFRESH);
        y += 28;
        btnOutToggle = make(L"BUTTON", L"Show full screen there", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 160, 26, ID_OUT_TOGGLE);
        btnOutBlackout = make(L"BUTTON", L"Black out (B)", BS_PUSHBUTTON | WS_TABSTOP, x0 + 166, y, 118, 26, ID_OUT_BLACKOUT);
        y += 30;
        make(L"STATIC", L"The view alone, full screen, nothing on top. B blacks it out at once; the controls stay here.",
             SS_LEFT, x0, y, 284, 34, 0);
        y += 36;
        refreshDisplays();

        section(L"IR 16-bit to 8-bit");
        chkIrAuto = check(L"Percentile auto-scale (P1-P99.5)", ID_IR_AUTO, s.ir_auto_scale);
        tbGamma = slider(ID_GAMMA, &lblGamma, kGammaSliderMin, kGammaSliderMax, gammaToSlider(s.ir_gamma));
        tbEma = slider(ID_EMA, &lblEma, kEmaSliderMin, kEmaSliderMax, emaToSlider(s.ir_ema_alpha));
        tbBlack = slider(ID_BLACK, &lblBlack, kIrSliderMin, kIrSliderMax, static_cast<int>(s.ir_manual_min));
        tbWhite = slider(ID_WHITE, &lblWhite, kIrSliderMin, kIrSliderMax, static_cast<int>(s.ir_manual_max));
        btnResetIr = make(L"BUTTON", L"Reset IR mapping", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 136, 24, ID_RESET_IR);
        y += 28;

        section(L"Depth false colour");
        cbColormap = labeledCombo(L"Colormap", ID_COLORMAP, {L"Turbo", L"Viridis", L"Inferno", L"Grayscale"},
                                  s.depth_colormap);
        tbDmin = slider(ID_DMIN, &lblDmin, kDepthSliderMin, kDepthSliderMax, static_cast<int>(s.depth_min_mm));
        tbDmax = slider(ID_DMAX, &lblDmax, kDepthSliderMin, kDepthSliderMax, static_cast<int>(s.depth_max_mm));
        btnDesk = make(L"BUTTON", L"Desk 0.4-2.5m", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 92, 24, ID_DESK);
        btnSpec = make(L"BUTTON", L"Spec 0.5-4.5m", BS_PUSHBUTTON | WS_TABSTOP, x0 + 96, y, 92, 24, ID_SPEC);
        btnRoom = make(L"BUTTON", L"Room 0.5-8m", BS_PUSHBUTTON | WS_TABSTOP, x0 + 192, y, 92, 24, ID_ROOM);
        y += 28;
        chkInvert = check(L"Near = warm / bright", ID_INVERT, s.depth_invert);

        section(L"Colour exposure and gain");
        cbExposure = labeledCombo(L"Exposure", ID_EXPOSURE, {L"Auto + EV comp", L"Semi-auto (flicker-free)", L"Manual lock"},
                                  s.exposure_mode);
        tbEv = slider(ID_EV, &lblEv, kEvSliderMin, kEvSliderMax, evToSlider(s.auto_exposure_compensation));
        tbSemi = slider(ID_SEMI, &lblSemi, 40, kMsSliderMax, msToSlider(s.semi_auto_pseudo_ms));
        tbManual = slider(ID_MANUAL, &lblManual, kMsSliderMin, kMsSliderMax, msToSlider(s.manual_integration_ms));
        tbGain = slider(ID_GAIN, &lblGain, kGainSliderMin, kGainSliderMax, gainToSlider(s.manual_analog_gain));

        if (hooks) {
            // W6: the pet camera (pet-camera.md §4). Privacy mode first, and large: it stops every video at once.
            section(L"Pet camera");
            const int pet_top = y;
            btnPrivacy = make(L"BUTTON", L"Privacy mode: off", BS_OWNERDRAW | WS_TABSTOP, x0, y, 284, 34, ID_PET_PRIVACY);
            y += 38;
            lblPrivacyNote = make(L"STATIC", L"Stops live video to every device at once, owners included. The Kinect and this preview stay on.",
                                  SS_LEFT, x0, y, 284, 30, 0);
            y += 32;
            chkShare = check(L"Share the camera (viewers may watch)", ID_PET_SHARE, false);
            lblSchedule = make(L"STATIC", L"Sharing schedule: none", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, x0, y, 196, 24, 0);
            btnSchedule = make(L"BUTTON", L"Schedule\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0 + 200, y, 84, 24, ID_PET_SCHEDULE);
            y += 28;
            lblShareNote = make(L"STATIC", L"Off: only owner devices watch, at home. The schedule turns sharing off for viewers at set times.",
                                SS_LEFT, x0, y, 284, 30, 0);
            y += 32;
            // This PC's identity (pairing.json) can't be used, so the host is off: in place of the controls above
            // (which need the host) while that is so, the reason, "Try again" and the reset (refreshIdentity).
            lblIdentity = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, pet_top, 284, 80, 0);
            btnIdRetry = make(L"BUTTON", L"Try again", BS_PUSHBUTTON | WS_TABSTOP, x0, pet_top + 84, 284, 26, ID_PET_ID_RETRY);
            btnIdReset = make(L"BUTTON", L"Reset this PC's identity (all devices must pair again)\u2026",
                              BS_PUSHBUTTON | BS_MULTILINE | WS_TABSTOP, x0, pet_top + 114, 284, 36, ID_PET_ID_RESET);
            for (HWND h : {lblIdentity, btnIdRetry, btnIdReset}) ShowWindow(h, SW_HIDE);
            btnDevices = make(L"BUTTON", L"Devices\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 86, 26, ID_PET_DEVICES);
            btnPair = make(L"BUTTON", L"Pair a device\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0 + 92, y, 104, 26, ID_PAIR);
            btnUnpair = make(L"BUTTON", L"Revoke all\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0 + 202, y, 82, 26, ID_UNPAIR);
            y += 30;
            lblPet = make(L"STATIC", L"", SS_LEFT, x0, y, 284, 34, 0);
            y += 36;

#ifdef KV_WITH_STREAMING
            // W8: outside the home (pet-camera.md §4, v3 §8). Off by default, turned on on this PC only, and only
            // with sharing on. refreshOutside sets every control from outsidePanelState.
            section(L"Outside the home");
            chkOutside = check(L"Allow viewing from outside the home", ID_OUTSIDE_ALLOW, false);
            lblOutsideReason = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0 + 18, y, 266, 30, 0);
            y += 32;
            // The carrier-NAT sentence is the longest (§8.3): six lines at this width.
            lblOutsideStatus = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 96, 0);
            y += 98;
            lblOutsidePort = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, x0, y, 180, 24, 0);
            btnOutsidePort = make(L"BUTTON", L"Change port…", BS_PUSHBUTTON | WS_TABSTOP, x0 + 186, y, 98, 24, ID_OUTSIDE_PORT);
            y += 28;
            lblOutsideEndpoint = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 30, 0);
            y += 32;
            // v3 §8.3: the router the forward is on, as pinned, and what a new home address does (shown when mapped).
            lblOutsideRouter = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 30, 0);
            y += 32;
            // pet-camera.md §4 "Remove mapping now": the router forward goes; the listener stays as it is.
            btnOutsideRemove = make(L"BUTTON", L"Remove mapping now", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 150, 24,
                                    ID_OUTSIDE_REMOVE_MAPPING);
            // The removed forward stays removed until this, or until
            // outside viewing is turned off and on again.
            btnOutsideRemap = make(L"BUTTON", L"Map the port again", BS_PUSHBUTTON | WS_TABSTOP, x0 + 156, y, 128, 24,
                                   ID_OUTSIDE_REMAP);
            y += 28;
            // pet-camera.md §7: the Public network-profile hint (shown only when it applies).
            lblOutsideProfile = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 48, 0);
            y += 50;
            lblOutsideManual = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 46, 0);
            y += 48;
            lblOutsideAddr = make(L"STATIC", kOutsidePublicAddressLabel, SS_LEFT | SS_NOPREFIX, x0, y, 284, 18, 0);
            y += 20;
            edOutsideAddr = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x0, y, 196, 22, 0);
            SendMessageW(edOutsideAddr, EM_LIMITTEXT, 64, 0);
            btnOutsideAddrSave = make(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, x0 + 200, y, 84, 22, ID_OUTSIDE_ADDR_SAVE);
            y += 26;
            lblOutsideChanged = make(L"STATIC", kOutsideAddressChangedText, SS_LEFT | SS_NOPREFIX, x0, y, 156, 30, 0);
            btnOutsideReshare = make(L"BUTTON", L"Re-share address…", BS_PUSHBUTTON | WS_TABSTOP, x0 + 160, y, 124, 26,
                                     ID_OUTSIDE_RESHARE);
            y += 32;
            btnOutsideInvite = make(L"BUTTON", L"Invite from outside…", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 140, 26, ID_OUTSIDE_INVITE);
            btnOutsideLog = make(L"BUTTON", L"Connection log…", BS_PUSHBUTTON | WS_TABSTOP, x0 + 146, y, 138, 26, ID_OUTSIDE_LOG);
            y += 30;
            lblOutsideInvite = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 30, 0); // why invites are off, or the open one
            y += 32;
            lblOutsideNote = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 30, 0); // the last refusal or change
            y += 32;
#endif

            // W7: the camera roll (pet-camera.md §4, §6). Every switch is off by default (§1).
            section(L"Camera roll (clips kept on this PC)");
            chkRoll = check(L"Record to the camera roll", ID_ROLL_ENABLE, false);
            // The limits retention keeps to, next to the switch that writes them (turning the roll on confirms
            // them); until they are known, the panel says retention waits and offers Confirm.
            lblRollLimits = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0 + 18, y, 266, 20, 0);
            y += 22;
            lblRollConfirm = make(L"STATIC", L"Retention starts when you confirm the limits.", SS_LEFT | SS_NOPREFIX, x0 + 18, y, 186, 30, 0);
            btnRollConfirm = make(L"BUTTON", L"Confirm", BS_PUSHBUTTON | WS_TABSTOP, x0 + 212, y, 72, 24, ID_ROLL_CONFIRM_LIMITS);
            y += 32;
            lblRollStatus = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 34, 0);
            y += 36;
            btnRollRecord = make(L"BUTTON", L"Record a clip", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 136, 26, ID_ROLL_RECORD);
            btnRollStop = make(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, x0 + 146, y, 138, 26, ID_ROLL_STOP);
            y += 30;
            chkMotion = make(L"BUTTON", L"Motion (by depth)", BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, 150, 22, ID_ROLL_MOTION);
            lblMotion = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, x0 + 154, y, 130, 22, 0);
            y += 24;
            tbMotion = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, x0 + 12, y, 272, 26, ID_ROLL_SENS);
            SendMessageW(tbMotion, TBM_SETRANGEMIN, FALSE, 1);
            SendMessageW(tbMotion, TBM_SETRANGEMAX, TRUE, 10);
            SendMessageW(tbMotion, TBM_SETPOS, TRUE, kvhost::kRollDefaultMotionSensitivity);
            y += 28;
            lblMotionNote = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0 + 12, y, 272, 30, 0);
            y += 32;
            chkRecSched = make(L"BUTTON", L"Scheduled recording", BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, 190, 22, ID_ROLL_SCHED_ON);
            btnRecSched = make(L"BUTTON", L"Rows\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0 + 200, y, 84, 24, ID_ROLL_SCHED);
            y += 26;
            lblRecSched = make(L"STATIC", L"none", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x0 + 12, y, 272, 20, 0);
            y += 24;
            make(L"STATIC", L"Camera roll folder", SS_LEFT | SS_CENTERIMAGE, x0, y, 196, 24, 0);
            btnRollFolder = make(L"BUTTON", L"Change\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0 + 200, y, 84, 24, ID_ROLL_FOLDER);
            y += 26;
            // A path wraps at any character (SS_EDITCONTROL), so a long one stays readable.
            lblRollFolder = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, x0 + 12, y, 272, 48, 0);
            y += 50;
            make(L"STATIC", L"Keep", SS_LEFT | SS_CENTERIMAGE, x0, y, 34, 22, 0);
            edKeepDays = make(L"EDIT", L"30", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, x0 + 36, y, 42, 22, 0);
            make(L"STATIC", L"days", SS_LEFT | SS_CENTERIMAGE, x0 + 82, y, 30, 22, 0);
            edKeepGb = make(L"EDIT", L"200", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, x0 + 114, y, 50, 22, 0);
            make(L"STATIC", L"GB", SS_LEFT | SS_CENTERIMAGE, x0 + 168, y, 24, 22, 0);
            btnKeepApply = make(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, x0 + 200, y, 84, 22, ID_ROLL_KEEP_APPLY);
            y += 26;
            lblRollStorage = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 30, 0);
            y += 32;
            btnRollBrowser = make(L"BUTTON", L"Camera roll\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 136, 26, ID_ROLL_BROWSER);
            y += 30;
            lblRollNote = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, y, 284, 44, 0);
            y += 46;

            section(L"iPhone live stream");
            chkAllowSwitch = check(L"Allow iPhone mode switch (owners)", ID_ALLOW_SWITCH, hooks->allowPhoneModeSwitch());
            chkAllowPower = check(L"Allow iPhone to turn the Kinect on/off (owners)", ID_ALLOW_POWER, hooks->allowPhoneSensorPower());
            make(L"STATIC", L"Both apply to owner devices only: a viewer can't change the view or the power.", SS_LEFT, x0 + 18,
                 y, 266, 30, 0);
            y += 32;
            chkOffOnDisconnect = check(L"Turn the Kinect off when the iPhone disconnects", ID_OFF_ON_DISCONNECT,
                                       hooks->turnOffOnPhoneDisconnect());
            make(L"STATIC", L"Acts when the last available session ends: not on revoking one device, a replacement or privacy mode.",
                 SS_LEFT, x0 + 18, y, 266, 30, 0);
            y += 32;
            lblD6Note = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0 + 18, y, 266, 30, 0); // v3 D6, with the roll on
            y += 32;
            lblStream = make(L"STATIC", L"", SS_LEFT, x0, y, 284, 50, 0); // three lines: sessions, rate, encoder
            y += 52;
        }
#ifdef KV_WITH_RECORDER
        section(L"Host-only recording (stays on this PC)");
        make(L"STATIC", L"16-bit", SS_LEFT | SS_CENTERIMAGE, x0, y, 40, 24, 0);
        cbRecFormat = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, x0 + 42, y, 140, 200, ID_REC_FORMAT);
        SendMessageW(cbRecFormat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"FFV1 lossless (.mkv)"));
        SendMessageW(cbRecFormat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PNG16 sequence"));
        SendMessageW(cbRecFormat, CB_SETCURSEL, 0, 0);
        btnRecord = make(L"BUTTON", L"Record", BS_PUSHBUTTON | WS_TABSTOP, x0 + 190, y, 94, 24, ID_RECORD);
        y += 28;
        lblRec = make(L"STATIC", L"IR/Depth only, 16-bit lossless. Stays on this PC.", SS_LEFT, x0, y, 284, 34, 0);
        // The Record button's tooltip. A disabled button gets no mouse input, so the tool is the
        // button's rectangle on the main window, which does.
        ttRecord = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, main, nullptr, inst, nullptr);
        if (ttRecord) {
            TTTOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = panelWnd;
            ti.uId = ID_RECORD;
            GetWindowRect(btnRecord, &ti.rect);
            MapWindowPoints(nullptr, panelWnd, reinterpret_cast<POINT*>(&ti.rect), 2);
            ti.lpszText = const_cast<wchar_t*>(L"");
            SendMessageW(ttRecord, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        y += 36;
        make(L"BUTTON", L"Recordings\u2026", BS_PUSHBUTTON | WS_TABSTOP, x0, y, 136, 26, ID_RECORDINGS);
        y += 32;
#endif
        panel_bottom = y;
        refreshLabels();
        refreshEnabled();
    }

    // ---- phase 1: inspection, freeze, zoom/pan, health, presets, shortcuts --------------------------
    ViewportTransform viewport;
    bool frozen = false;
    std::vector<uint8_t> frozen_frame;
    kv_mapped_frame_t frozen_meta{};
    ViewerSettings frozen_settings;
    // What the tools read: the live buffers, or the ones copied at the freeze. Memory only.
    std::vector<float> insp_depth, insp_ir;
    kv_ir_intrinsics_t insp_intr{};
    bool insp_valid = false;
    std::optional<CursorReadout> readout;
    bool region_on = false;
    int reg_x0 = 0, reg_y0 = 0, reg_x1 = 0, reg_y1 = 0; // display pixels
    int reg_col0 = 0, reg_col1 = 0;                     // the panel the region started in
    std::optional<RulerMeasurement> ruler_before_click;
    std::wstring region_text;
    std::optional<RulerMeasurement> ruler;
    enum class Drag { None, Pending, Region, Pan } drag = Drag::None;
    POINT down_pt{}, last_pt{};
    bool down_shift = false;
    bool tracking_leave = false;
    bool show_health = false;
    bool banner_on = false;
    bool fullscreen = false;
    WINDOWPLACEMENT saved_placement{};
    LONG_PTR saved_style = 0;
    HWND lblBanner{}, lblHealth{}, lblInspect{}, helpWnd{};
    HWND cbPreset{}, btnPresetSave{}, btnPresetDelete{}, chkMirror{}, chkRotate{}, chkHealth{}, btnHelp{};
    std::vector<HWND> panel_controls; // every control in the panel
    HWND panelWnd{};                  // the scrolling panel that holds them (hidden in full screen)
    int panel_scroll = 0;

    int panelContentHeight() const { return S(panel_bottom + 10); }
    void scrollPanelTo(int pos) {
        RECT pr{};
        GetClientRect(panelWnd, &pr);
        pos = std::clamp(pos, 0, std::max(0, panelContentHeight() - static_cast<int>(pr.bottom)));
        if (pos == panel_scroll) return;
        ScrollWindowEx(panelWnd, 0, panel_scroll - pos, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
        panel_scroll = pos;
        SetScrollPos(panelWnd, SB_VERT, pos, TRUE);
        if (ttRecord && btnRecord) { // the Record tooltip's rectangle moved with the button
            TTTOOLINFOW ti{sizeof(ti)};
            ti.hwnd = panelWnd;
            ti.uId = ID_RECORD;
            GetWindowRect(btnRecord, &ti.rect);
            MapWindowPoints(nullptr, panelWnd, reinterpret_cast<POINT*>(&ti.rect), 2);
            SendMessageW(ttRecord, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }
    void updatePanelScroll() {
        RECT pr{};
        GetClientRect(panelWnd, &pr);
        SCROLLINFO si{sizeof(si)};
        si.fMask = SIF_RANGE | SIF_PAGE;
        si.nMin = 0;
        si.nMax = panelContentHeight() - 1;
        si.nPage = static_cast<UINT>(std::max<LONG>(1, pr.bottom));
        SetScrollInfo(panelWnd, SB_VERT, &si, TRUE);
        scrollPanelTo(panel_scroll); // clamps after a resize
    }
    std::unique_ptr<PresetsStore> presets; // null in a test launch: no user state read or written
    std::vector<ViewerPreset> preset_list;  // what cbPreset shows, after its placeholder row
    std::vector<uint8_t> shown;             // the shown frame with the region box and ruler drawn in
    std::wstring tool_note;
    HBRUSH banner_brush = nullptr;

    const std::vector<uint8_t>& shownFrame() const { return frozen ? frozen_frame : frame; }
    const kv_mapped_frame_t& shownMeta() const { return frozen ? frozen_meta : meta; }
    const ViewerSettings& shownSettings() const { return frozen ? frozen_settings : s; }

    void refreshInspectionBuffers() {
        insp_depth.resize(512 * 424);
        insp_ir.resize(512 * 424);
        uint32_t w = 0, h = 0;
        insp_valid = running &&
                     kv_engine_copy_inspection_buffers(eng, insp_depth.data(), insp_depth.size(), insp_ir.data(), insp_ir.size(),
                                                       &w, &h, &insp_intr) &&
                     w == 512 && h == 424;
        if (!insp_valid) insp_intr = kv_ir_intrinsics_t{};
    }
    bool toolsActive() const { return region_on || ruler.has_value() || readout.has_value(); }

    bool viewToPixel(int vx, int vy, int& px, int& py) const {
        RECT rc{};
        GetClientRect(preview, &rc);
        const auto& m = shownMeta();
        return viewport.viewToDisplayPixel(vx, vy, rc.right, rc.bottom, static_cast<int>(m.width), static_cast<int>(m.height),
                                           px, py);
    }
    // Like viewToPixel, but a point off the image is clamped to its edge (a region dragged past it).
    void viewToPixelClamped(int vx, int vy, int& px, int& py) const {
        RECT rc{};
        GetClientRect(preview, &rc);
        const auto& m = shownMeta();
        const RectD r = viewport.displayedImageRect(rc.right, rc.bottom, m.width, m.height);
        if (r.w <= 0 || r.h <= 0 || m.width == 0) {
            px = py = 0;
            return;
        }
        px = std::clamp(static_cast<int>((vx - r.x) / r.w * m.width), 0, static_cast<int>(m.width) - 1);
        py = std::clamp(static_cast<int>((vy - r.y) / r.h * m.height), 0, static_cast<int>(m.height) - 1);
    }
    SensorPixel sensorAt(int px, int py) const {
        const auto& m = shownMeta();
        const auto& st = shownSettings();
        // With the blend, the picture is the 512x424 depth layout whatever the mode (the Mac's).
        return mapDisplayPixelToSensor(px, py, static_cast<int>(m.width), static_cast<int>(m.height), imageLayoutMode(st),
                                       st.sbs_layout, st.mirror_horizontal, st.rotate_180);
    }

    void updateReadoutAt(int vx, int vy) {
        int px = 0, py = 0;
        if (shownMeta().width == 0 || !viewToPixel(vx, vy, px, py)) {
            readout.reset();
            return;
        }
        if (!frozen && !insp_valid && running) refreshInspectionBuffers();
        CursorReadout r;
        r.pixel_x = px;
        r.pixel_y = py;
        const SensorPixel sp = sensorAt(px, py);
        r.has_sensor_depth_ir = sp.has_depth_ir && insp_valid;
        if (r.has_sensor_depth_ir) {
            const size_t i = static_cast<size_t>(sp.y) * 512 + static_cast<size_t>(sp.x);
            r.depth_mm = insp_depth[i];
            r.raw_ir = insp_ir[i];
        }
        readout = r;
    }

    void recomputeTools() {
        if (region_on) {
            const SensorPixel a = sensorAt(reg_x0, reg_y0), b = sensorAt(reg_x1, reg_y1);
            if (a.has_depth_ir && b.has_depth_ir && insp_valid) {
                const auto st = RegionDepthStats::compute(insp_depth, 512, 424, a.x, a.y, b.x, b.y);
                region_text = st ? st->text() : std::wstring(L"Region: no depth here");
            } else {
                region_text = L"Region: no depth here";
            }
        }
        if (ruler && ruler->b) {
            static const std::vector<float> none;
            RulerMeasurement::evaluate(*ruler, insp_intr, insp_valid ? insp_depth : none);
        }
    }

    std::wstring inspectText() const {
        if (in3d()) {
            // No depth/IR readout in 3D (as on the Mac): the mouse moves the camera.
            std::wstring t = pc_note.empty() ? L"3D: drag to orbit, Shift-drag or right-drag to pan, scroll to zoom, double-click "
                                               L"to reset."
                                             : pc_note;
            wchar_t b[160];
            swprintf(b, 160, L"    %ls points, %.1f px%ls", pointSourceLabel(s.point_color_source), s.point_size,
                     s.cutoff_enabled ? L", cut-off on" : L"");
            t += b;
            if (frozen) t = L"FROZEN (Space)    " + t;
            return t;
        }
        std::wstring t;
        if (readout) {
            t = L"Cursor " + readout->coordText() + L"  depth " + readout->depthText() + L"  IR " + readout->irText();
        }
        if (region_on) t += (t.empty() ? L"" : L"    ") + region_text;
        if (ruler) t += (t.empty() ? L"" : L"    ") + std::wstring(L"Ruler: ") + ruler->label();
        if (frozen) t = L"FROZEN (Space)    " + t;
        if (!tool_note.empty()) t += (t.empty() ? L"" : L"    ") + tool_note;
        if (t.empty()) t = L"Hover for depth and IR. Drag a box for region stats, click two points for the ruler. ? for help.";
        return t;
    }
    void refreshInspectLine() {
        if (lblInspect) SetWindowTextW(lblInspect, inspectText().c_str());
    }

    // ---- stage 6: the second display (projector) ------------------------------------------------------
    SecondDisplayOutput out;
    std::vector<DisplayTarget> displays; // what the combo lists
    std::wstring out_device;             // the chosen display (\\.\DISPLAYn)
    bool out_blackout = false;           // kept while the output is off too (the Mac's)
    RECT out_test_rect{};                // --test-second-display: a small window instead of a display
    HWND cbOutDisplay{}, btnOutRefresh{}, btnOutToggle{}, btnOutBlackout{};

    // The second display gets the clean frame: no region box or ruler on it. Smoothed in Colour and 3D
    // (the Mac's crisp scaling elsewhere), always the whole picture (no zoom or pan).
    void mirrorToOutput() {
        if (!out.active()) return;
        const auto& f = shownFrame();
        const auto& m = shownMeta();
        if (frame_stale || m.width == 0 || m.height == 0 || f.size() < static_cast<size_t>(m.stride_bytes) * m.height) return;
        const kv_view_mode_t shown_mode = shownSettings().mode; // the shown frame's mode (a frozen one's, too)
        out.showFrame(f.data(), m.width, m.height, m.stride_bytes,
                      shown_mode != KV_MODE_POINT_CLOUD_3D && shown_mode != KV_MODE_COLOR);
    }
    void clearShown() {
        d3d.clearFrame();
        out.clearFrame();
    }

    // The connected displays, keeping the choice while it's still there (else a projector first).
    void refreshDisplays() {
        displays = listDisplays();
        const int pick = chooseDisplay(displays, out_device);
        out_device = pick >= 0 ? displays[static_cast<size_t>(pick)].device : std::wstring();
        if (!cbOutDisplay) return;
        SendMessageW(cbOutDisplay, CB_RESETCONTENT, 0, 0);
        for (const auto& d : displays) {
            const std::wstring t = displayTitle(d);
            SendMessageW(cbOutDisplay, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
        }
        SendMessageW(cbOutDisplay, CB_SETCURSEL, pick, 0);
        updateOutputControls();
    }
    void updateOutputControls() {
        if (!btnOutToggle) return;
        SetWindowTextW(btnOutToggle, out.active() ? L"Stop full screen" : L"Show full screen there");
        SetWindowTextW(btnOutBlackout, out_blackout ? L"Unblank (B)" : L"Black out (B)");
        EnableWindow(btnOutToggle, out.active() || !displays.empty() || out_test_rect.right > out_test_rect.left);
    }
    // The output is on the display the main window is on (it covers the controls): Esc stops it.
    bool outputOnMainDisplay() const {
        if (!out.active()) return false;
        return MonitorFromWindow(out.window(), MONITOR_DEFAULTTONULL) == MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST);
    }
    void startOutput() {
        RECT r = out_test_rect; // the test hook's small window, never a whole display
        if (r.right <= r.left) {
            refreshDisplays();
            const int i = findDisplay(displays, out_device);
            if (i < 0) {
                tool_note = L"No display for the full-screen output.";
                refreshInspectLine();
                updateOutputControls();
                return;
            }
            r = displays[static_cast<size_t>(i)].rect;
        }
        std::string err;
        if (!out.start(inst, r, &err)) {
            tool_note = L"The full-screen output couldn't open: " + widen(err.c_str());
            refreshInspectLine();
        } else {
            out.setBlackout(out_blackout);
            mirrorToOutput();
            if (shownMeta().width == 0) out.clearFrame();
        }
        // (The output never activates, so the keyboard stays here: B and Esc keep working.)
        updateOutputControls();
        updateStatus();
    }
    void stopOutput() {
        out.stop();
        updateOutputControls();
        updateStatus();
    }
    // B: works whether or not the output is open (it opens blacked out then), as on the Mac.
    void toggleBlackout() {
        out_blackout = !out_blackout;
        out.setBlackout(out_blackout);
        updateOutputControls();
        updateStatus();
    }
    // A display came or went, or changed size: follow it, or stop when it's gone.
    void onDisplaysChanged() {
        const std::wstring showing_on = out_device;
        refreshDisplays();
        if (!out.active() || out_test_rect.right > out_test_rect.left) return;
        const int i = findDisplay(displays, showing_on); // not another display picked in its place
        if (i < 0) {
            stopOutput();
            tool_note = L"The output's display went away; the full-screen output stopped.";
            refreshInspectLine();
            return;
        }
        std::string err;
        out.start(inst, displays[static_cast<size_t>(i)].rect, &err);
    }

    // After a change of mode or layout the last frame is the old view's (Colour, say): it isn't shown
    // again, here or on the second display, until a frame of the new view arrives.
    bool frame_stale = false;

    // Uploads the shown frame (frozen or live) with the region box and the ruler drawn into a copy.
    void presentShown() {
        const auto& f = shownFrame();
        const auto& m = shownMeta();
        if (frame_stale || m.width == 0 || m.height == 0 || f.size() < static_cast<size_t>(m.stride_bytes) * m.height) return;
        mirrorToOutput();
        if (!region_on && !ruler) {
            d3d.upload(f.data(), m.width, m.height, m.stride_bytes);
            needs_redraw = true;
            return;
        }
        shown.assign(f.begin(), f.begin() + static_cast<size_t>(m.stride_bytes) * m.height);
        const int w = static_cast<int>(m.width), h = static_cast<int>(m.height);
        auto put = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
            if (x < 0 || y < 0 || x >= w || y >= h) return;
            uint8_t* p = shown.data() + static_cast<size_t>(y) * m.stride_bytes + static_cast<size_t>(x) * 4;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 255;
        };
        const int t = std::max(1, w / 512); // thicker on the big colour frame
        if (region_on) {
            const int x0 = std::min(reg_x0, reg_x1), x1 = std::max(reg_x0, reg_x1);
            const int y0 = std::min(reg_y0, reg_y1), y1 = std::max(reg_y0, reg_y1);
            for (int k = 0; k < t; ++k) {
                for (int x = x0; x <= x1; ++x) {
                    put(x, y0 + k, 0, 220, 255);
                    put(x, y1 - k, 0, 220, 255);
                }
                for (int y = y0; y <= y1; ++y) {
                    put(x0 + k, y, 0, 220, 255);
                    put(x1 - k, y, 0, 220, 255);
                }
            }
        }
        if (ruler) {
            auto dot = [&](int cx, int cy) {
                for (int dy = -2 * t; dy <= 2 * t; ++dy)
                    for (int dx = -2 * t; dx <= 2 * t; ++dx) put(cx + dx, cy + dy, 255, 255, 0);
            };
            dot(ruler->a.disp_x, ruler->a.disp_y);
            if (ruler->b) {
                dot(ruler->b->disp_x, ruler->b->disp_y);
                int x = ruler->a.disp_x, y = ruler->a.disp_y;
                const int x2 = ruler->b->disp_x, y2 = ruler->b->disp_y;
                const int dx = std::abs(x2 - x), sx = x < x2 ? 1 : -1, dy = -std::abs(y2 - y), sy = y < y2 ? 1 : -1;
                int err = dx + dy;
                while (true) {
                    for (int k = 0; k < t; ++k) put(x, y + k, 255, 255, 0);
                    if (x == x2 && y == y2) break;
                    const int e2 = 2 * err;
                    if (e2 >= dy) { err += dy; x += sx; }
                    if (e2 <= dx) { err += dx; y += sy; }
                }
            }
        }
        d3d.upload(shown.data(), m.width, m.height, m.stride_bytes);
        needs_redraw = true;
    }

    void clearTools() {
        region_on = false;
        ruler.reset();
        region_text.clear();
        tool_note.clear();
        presentShown();
        refreshInspectLine();
    }

    // Space, or the phone's set_host_frozen. The frozen frame and its depth/IR stay in memory only.
    void setFrozen(bool on) {
        if (on == frozen) return;
        if (on) {
            if (meta.width == 0 || frame.empty() || !running || frame_stale) return; // never the old view's frame
            frozen_frame.assign(frame.begin(), frame.begin() + static_cast<size_t>(meta.stride_bytes) * meta.height);
            frozen_meta = meta;
            frozen_settings = s;
            // The depth and IR copied with the shown frame (pollFrame), not the engine's newer ones.
            frozen = true;
            // The phone sees the frozen frame (encoded once), not the live one, until it's unfrozen.
            sendFrameToHost(frozen_frame.data(), frozen_meta, frozen_settings);
        } else {
            frozen = false;
        }
        recomputeTools();
        presentShown();
        refreshInspectLine();
        publishViewState();
    }

    void setFullscreen(bool on) {
        if (on == fullscreen) return;
        if (on) {
            saved_placement.length = sizeof(saved_placement);
            GetWindowPlacement(main, &saved_placement);
            saved_style = GetWindowLongPtrW(main, GWL_STYLE);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST), &mi);
            fullscreen = true;
            ShowWindow(panelWnd, SW_HIDE);
            ShowWindow(status, SW_HIDE);
            if (ttRecord) SendMessageW(ttRecord, TTM_ACTIVATE, FALSE, 0);
            SetWindowLongPtrW(main, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        } else {
            fullscreen = false;
            SetWindowLongPtrW(main, GWL_STYLE, saved_style);
            SetWindowPlacement(main, &saved_placement);
            SetWindowPos(main, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            ShowWindow(panelWnd, SW_SHOW);
            ShowWindow(status, SW_SHOW);
            if (ttRecord) SendMessageW(ttRecord, TTM_ACTIVATE, TRUE, 0);
        }
        layout();
    }

    void setOrientation(bool mirror, bool rotate) {
        if (mirror == s.mirror_horizontal && rotate == s.rotate_180) return;
        setFrozen(false); // the frozen frame was drawn with the old orientation
        pc_dirty = true;  // 3D flips in its renderer, not in the core's mapping
        s.mirror_horizontal = mirror;
        s.rotate_180 = rotate;
        if (chkMirror) SendMessageW(chkMirror, BM_SETCHECK, mirror ? BST_CHECKED : BST_UNCHECKED, 0);
        if (chkRotate) SendMessageW(chkRotate, BM_SETCHECK, rotate ? BST_CHECKED : BST_UNCHECKED, 0);
        clearTools(); // their pixels were in the old orientation
        pushDisplay();
    }

    void setShowHealth(bool on) {
        show_health = on;
        if (chkHealth) SendMessageW(chkHealth, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        updateHealth();
        layout();
    }

    void updateHealth() {
        kv_health_snapshot_t h{};
        kv_engine_get_health(eng, &h);
        const bool banner = running && (h.drop_warning_active || shouldShowDropBanner(h.ir_drop_ratio_10s, h.depth_drop_ratio_10s));
        if (banner != banner_on) {
            banner_on = banner;
            layout();
        }
        if (show_health && lblHealth) {
            std::wstring t;
            for (const auto& l : healthLines(h)) t += (t.empty() ? L"" : L"\n") + l;
            if (hooks) {
                // v3 §6.1: each live tier's encoder (name, hardware or not, size, epoch, sessions).
                health_tier_line = tierDiagnosticsLine(hooks->tierStatus());
                t += L"\n" + (health_tier_line.empty() ? std::wstring(L"Live tiers: streaming host off") : health_tier_line);
            }
            SetWindowTextW(lblHealth, t.c_str());
        }
    }

    void toggleHelp();

    // One frame's work: playback, a new frame, the recorder, and a redraw if needed. The main loop and
    // the modal loops (reminder, preset name) both run it, so the view and the phone's video never
    // stop while a dialog is up. Returns whether anything was drawn.
    bool stepView() {
#ifdef KV_WITH_RECORDER
        pollReplay();
#endif
        bool got = pollFrame();
        // A camera move, point size or orientation change in 3D: once per loop, however many mouse
        // moves or phone gestures came in.
        if (!got && pc_dirty && in3d()) got = renderPointCloud();
        feedRecorder();
        feedMotion();
        if (!got && !needs_redraw) return false;
        const auto& sm = shownMeta();
        // The 3D picture is a rendering, smoothed when scaled; the sensor's images keep their pixels.
        const bool nearest = !in3d() && (sm.width <= 1280 || viewport.zoom > 1.001);
        RECT pr{};
        GetClientRect(preview, &pr);
        const RectD r = viewport.displayedImageRect(pr.right, pr.bottom, sm.width, sm.height);
        const float rect[4] = {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)};
        d3d.render(nearest, r.w > 0 ? rect : nullptr);
        needs_redraw = false;
        return true;
    }
    // A modal dialog's loop: its Enter/Esc first, then everyone's messages, and the view keeps running.
    // Ends when done(), or on WM_QUIT (posted again for the main loop).
    template <typename Done, typename Key>
    void modalLoop(HWND dlg, Done done, Key on_key) {
        MSG m{};
        while (!done()) {
            while (!done() && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(m.wParam));
                    return;
                }
                if (m.message == WM_KEYDOWN && IsWindow(dlg) && on_key(m)) continue;
                if (IsWindow(dlg) && IsDialogMessageW(dlg, &m)) continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (done()) break;
            if (!stepView()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 3, QS_ALLINPUT);
        }
    }

    // ---- recordings and playback (phase 2) --------------------------------------------------------------
    bool replaying = false;    // a recording plays through the pipeline (the sensor is closed meanwhile)
    bool user_state = false;   // false in a test launch: no recordings purged, no presets touched
#ifdef KV_WITH_RECORDER
    kvrec::ReminderPolicy reminder;
    HWND recWnd{}, recList{}, recPlay{}, recDelete{}, recPause{}, recBack{}, recFwd{}, recScrub{}, recExit{}, recInfo{};
    std::vector<kvrec::SessionItem> rec_items;
    std::unique_ptr<kvrec::ReplayDecoder> replay;
    kvrec::SessionItem replay_item;
    bool replay_playing = false, replay_pending = false;
    int replay_index = 0, replay_shown = -1;
    uint64_t replay_next_ns = 0;
    int list_refresh_ticks = 0;

    std::wstring activeRecordingFolder() const { return recorder.isRecording() ? recorder.status().session_dir : std::wstring(); }

    // At launch and once an hour: old colour sessions, then everything 7 days old (logged).
    void purgeRecordings() {
        if (!user_state) return;
        kvrec::purgeExpiredSessions(kvrec::defaultRecordingsDir(), kvrec::unixNow(), activeRecordingFolder());
        if (recWnd) refreshRecordingsList();
    }

    void refreshRecordingsList() {
        if (!recList) return;
        const int was = selectedRecording();
        const std::wstring was_folder = was >= 0 && was < static_cast<int>(rec_items.size()) ? rec_items[static_cast<size_t>(was)].folder : L"";
        rec_items = kvrec::listSessions(kvrec::defaultRecordingsDir(), activeRecordingFolder());
        ListView_DeleteAllItems(recList);
        const int64_t now = kvrec::unixNow();
        for (size_t i = 0; i < rec_items.size(); ++i) {
            const auto& it = rec_items[i];
            // Metadata only: no thumbnail, no preview of the content (phase 2.1).
            std::wstring cols[5] = {it.dateText(), it.modeTitle(), it.lengthText(), it.sizeText(), it.countdownText(now)};
            LVITEMW lv{};
            lv.mask = LVIF_TEXT;
            lv.iItem = static_cast<int>(i);
            lv.pszText = cols[0].data();
            ListView_InsertItem(recList, &lv);
            for (int c = 1; c < 5; ++c) ListView_SetItemText(recList, static_cast<int>(i), c, cols[c].data());
            if (!was_folder.empty() && it.folder == was_folder) {
                ListView_SetItemState(recList, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
        updatePlaybackControls();
    }

    int selectedRecording() const {
        return recList ? ListView_GetNextItem(recList, -1, LVNI_SELECTED) : -1;
    }

    void updatePlaybackControls() {
        if (!recWnd) return;
        const int sel = selectedRecording();
        EnableWindow(recPlay, sel >= 0);
        EnableWindow(recDelete, sel >= 0 && !(replaying && sel < static_cast<int>(rec_items.size()) &&
                                              rec_items[static_cast<size_t>(sel)].folder == replay_item.folder));
        for (HWND h : {recPause, recBack, recFwd, recScrub, recExit}) EnableWindow(h, replaying);
        SetWindowTextW(recPause, replay_playing ? L"Pause" : L"Play");
        if (replaying && replay) {
            SendMessageW(recScrub, TBM_SETRANGEMAX, TRUE, std::max(0, replay->totalFrames() - 1));
            SendMessageW(recScrub, TBM_SETPOS, TRUE, replay_index);
            wchar_t b[256];
            swprintf(b, 256, L"Playing %ls %ls: frame %d of %d", replay_item.modeTitle().c_str(), replay_item.dateText().c_str(),
                     replay_index + 1, replay->totalFrames());
            SetWindowTextW(recInfo, b);
        } else {
            SetWindowTextW(recInfo, L"Select a recording to play it through the live view, or delete it. Deleted after 7 days.");
        }
    }

    // A recording replays as if it were the sensor: every mode and tool works on it (phase 2.2).
    void startPlayback(int sel) {
        if (sel < 0 || sel >= static_cast<int>(rec_items.size())) return;
        const kvrec::SessionItem item = rec_items[static_cast<size_t>(sel)];
        if (replaying) exitPlayback();
        if (recorder.isRecording()) stopRecording();
        power->request(false); // the sensor closes (and its lock goes); the phone hears Off
        setFrozen(false);
        clearTools();
        s.mode = item.is_depth ? KV_MODE_DEPTH : KV_MODE_IR;
        // A recording has no colour: no blend, no RGB points while it plays (the Mac's).
        s.depth_color_blend_enabled = false;
        if (s.point_color_source == KV_POINT_COLOR_REGISTERED_RGB) s.point_color_source = KV_POINT_COLOR_DEPTH;
        pc_seq = 0;
        pc_have = false;
        syncControlsFromSettings();
        const kv_display_params_t p = toDisplayParams(s);
        const kv_ir_intrinsics_t intr = item.ir_intrinsics.value_or(kv_ir_intrinsics_t{}); // none: the ruler says so
        engine_source = KV_SOURCE_REPLAY; // its depth is never motion (pet-camera.md §6.2)
        motion_seq = 0;
        if (!kv_engine_start_replay(eng, &p, &intr)) {
            error = L"Could not start playback.";
            updateStatus();
            return;
        }
        replay = std::make_unique<kvrec::ReplayDecoder>(item);
        replay_item = item;
        replaying = true;
        replay_playing = true;
        replay_index = 0;
        replay_shown = -1;
        replay_pending = true;
        replay_next_ns = 0;
        replay->request(0);
        last_seq = 0;
        clearShown();
        needs_redraw = true;
        if (hooks) hooks->onModeOrSourceChanged();
        publishViewState();
        updatePlaybackControls();
        updateStatus();
    }

    void exitPlayback() {
        if (!replaying) return;
        replay.reset(); // joins the decode worker
        replaying = false;
        replay_playing = false;
        setFrozen(false);
        clearTools();
        pc_have = false;
        kv_engine_stop(eng);
        clearShown();
        needs_redraw = true;
        publishViewState();
        updatePlaybackControls();
        refreshEnabled();
        updateStatus();
    }

    void seekPlayback(int index) {
        if (!replaying || !replay) return;
        replay_index = std::clamp(index, 0, std::max(0, replay->totalFrames() - 1));
        replay->request(replay_index);
        replay_pending = true;
        updatePlaybackControls();
    }

    // Each loop: hand a decoded frame to the engine, and ask for the next one on time.
    void pollReplay() {
        if (!replaying || !replay) return;
        if (auto f = replay->take()) {
            replay_pending = false;
            if (f->ok && f->index == replay_index) {
                kv_engine_push_replay_frame16(eng, replay_item.is_depth, f->pixels.data(), static_cast<uint32_t>(replay_item.width),
                                              static_cast<uint32_t>(replay_item.height), static_cast<uint32_t>(f->index), f->ticks);
                replay_shown = f->index;
                replay_next_ns = kv_monotonic_ns() + static_cast<uint64_t>(replay->frameIntervalSeconds(f->index) * 1e9);
                if (recWnd && (f->index % 5 == 0 || !replay_playing)) updatePlaybackControls();
            } else if (f->index != replay_index) {
                seekPlayback(replay_index); // superseded: ask for the current one
            } else {
                replay_playing = false; // a frame that won't decode: stop there and say so
                error = L"Frame " + std::to_wstring(f->index + 1) + L" of this recording couldn't be decoded.";
                replay_shown = f->index;
                updatePlaybackControls();
                updateStatus();
            }
        }
        if (replay_playing && !replay_pending && replay_shown == replay_index && kv_monotonic_ns() >= replay_next_ns) {
            if (replay_index + 1 < replay->totalFrames()) {
                seekPlayback(replay_index + 1);
            } else {
                replay_playing = false; // stops on the last frame
                updatePlaybackControls();
            }
        }
    }

    void onRecordingsCommand(int id, int code) {
        switch (id) {
            case 1: startPlayback(selectedRecording()); break;
            case 2: {
                const int sel = selectedRecording();
                if (sel < 0 || sel >= static_cast<int>(rec_items.size())) break;
                const auto& it = rec_items[static_cast<size_t>(sel)];
                const std::wstring q = L"Delete the " + it.modeTitle() + L" recording of " + it.dateText() + L"?";
                if (MessageBoxW(recWnd, q.c_str(), L"Delete recording", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK) {
                    if (!kvrec::deleteSessionByUser(it, kvrec::defaultRecordingsDir(), kvrec::unixNow())) {
                        MessageBoxW(recWnd, L"It couldn't be deleted (is a file open?).", L"Delete recording", MB_ICONWARNING);
                    }
                    refreshRecordingsList();
                }
                break;
            }
            case 3:
                replay_playing = !replay_playing;
                updatePlaybackControls();
                break;
            case 4: replay_playing = false; seekPlayback(replay_index - 1); break;
            case 5: replay_playing = false; seekPlayback(replay_index + 1); break;
            case 6: exitPlayback(); break;
            default: (void)code; break;
        }
    }

    void openRecordingsWindow();
#endif

    // The last-used settings are saved when they change (checked each 250 ms tick), not only at close.
    std::string last_saved_settings;
    void saveLastUsedIfChanged() {
        if (!presets) return;
        const std::string now = PresetsStore::toJson({}, capturePreset("_last_used", s));
        if (now == last_saved_settings) return;
        presets->saveLastUsed(s);
        last_saved_settings = now;
    }

    // --test-tools: a region box and a ruler on the depth image (its right half in Side by Side
    // IR+Depth), then a freeze, all through the same code the mouse and Space use.
    void applyTestTools() {
        const auto& m = meta;
        if (m.width == 0) return;
        if (in3d()) { // no region or ruler in 3D: an orbit, then the freeze (and the camera still moves)
            cam.orbit(0.6f, 0.25f);
            cameraChanged();
            renderPointCloud();
            setFrozen(true);
            cam.zoom(1.2f);
            cameraChanged();
            renderPointCloud();
            return;
        }
        const int off = (imageLayoutMode(s) == KV_MODE_SIDE_BY_SIDE) ? static_cast<int>(m.width) - 512 : 0;
        region_on = true;
        reg_x0 = off + 200;
        reg_y0 = 180;
        reg_x1 = off + 240;
        reg_y1 = 220;
        ruler = RulerMeasurement{};
        const SensorPixel a = sensorAt(off + 100, 212), b = sensorAt(off + 400, 212);
        ruler->a = {off + 100, 212, a.x, a.y, a.has_depth_ir};
        ruler->b = RulerEndpoint{off + 400, 212, b.x, b.y, b.has_depth_ir};
        recomputeTools();
        setFrozen(true);
        presentShown();
        refreshInspectLine();
    }
    void selectMode(kv_view_mode_t m) {
        setFrozen(false);
        s.mode = m;
        syncControlsFromSettings();
        onViewChanged();
    }

    // A key from the message loop (any control may have focus). True when it was a shortcut.
    bool handleShortcutMessage(const MSG& msg) {
        if (msg.message != WM_KEYDOWN && msg.message != WM_CHAR) return false;
        // Typing into an edit box (a preset's name), or a dropped-down list, keeps its keys.
        HWND focus = GetFocus();
        wchar_t cls[32] = {0};
        if (focus) GetClassNameW(focus, cls, 32);
        if (_wcsicmp(cls, L"Edit") == 0) return false;
        if (_wcsicmp(cls, WC_COMBOBOXW) == 0 && SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0)) return false;
        if (msg.hwnd != main && !IsChild(main, msg.hwnd) && msg.hwnd != helpWnd) return false; // the pairing window etc.
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl_alt = (GetKeyState(VK_CONTROL) & 0x8000) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0;
        const Shortcut sc = msg.message == WM_KEYDOWN ? shortcutForKey(static_cast<unsigned>(msg.wParam), shift, ctrl_alt)
                                                      : shortcutForChar(static_cast<wchar_t>(msg.wParam));
        // A held key repeats (bit 30 of lParam): taken, but not toggled again and again.
        if (sc != Shortcut::None && msg.message == WM_KEYDOWN && (msg.lParam & (1 << 30))) return true;
        switch (sc) {
            case Shortcut::None: return false;
            case Shortcut::ModeIr: selectMode(KV_MODE_IR); break;
            case Shortcut::ModeColour: selectMode(KV_MODE_COLOR); break;
            case Shortcut::ModeDepth: selectMode(KV_MODE_DEPTH); break;
            case Shortcut::ModeSideBySide: selectMode(KV_MODE_SIDE_BY_SIDE); break;
            case Shortcut::Mode3D: selectMode(KV_MODE_POINT_CLOUD_3D); break;
            case Shortcut::Fullscreen: setFullscreen(!fullscreen); break;
            case Shortcut::Mirror: setOrientation(!s.mirror_horizontal, s.rotate_180); break;
            case Shortcut::Rotate180: setOrientation(s.mirror_horizontal, !s.rotate_180); break;
            case Shortcut::Freeze: setFrozen(!frozen); break;
            case Shortcut::Health: setShowHealth(!show_health); break;
            case Shortcut::Help: toggleHelp(); break;
            case Shortcut::ClearTools:
                // Esc: the help, then the tools; with nothing else, an output covering this display stops
                // (the Mac's), so the controls can't end up hidden under it.
                if (helpWnd) toggleHelp();
                else if (toolsActive() || region_on) clearTools();
                else if (outputOnMainDisplay()) stopOutput();
                else clearTools();
                break;
            case Shortcut::Blackout: toggleBlackout(); break;
        }
        return true;
    }

    // ---- mouse on the preview ----
    // 3D (the Mac's PointCloud3DCanvasView): left-drag orbits, Shift-drag, right-drag or middle-drag
    // pans, double-click resets. No readout, region or ruler there.
    bool pan3d = false;
    void onPreviewMouse3D(UINT msg, WPARAM wp, LPARAM lp) {
        const int vx = static_cast<short>(LOWORD(lp)), vy = static_cast<short>(HIWORD(lp));
        switch (msg) {
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetCapture(preview);
                last_pt = {vx, vy};
                pan3d = msg != WM_LBUTTONDOWN || (wp & MK_SHIFT) != 0;
                drag = Drag::Pan; // dragging: the camera moves
                break;
            case WM_MOUSEMOVE:
                if (drag == Drag::Pan) {
                    const float dx = static_cast<float>(vx - last_pt.x), dy = static_cast<float>(vy - last_pt.y);
                    if (pan3d || (wp & MK_SHIFT)) cam.panBy(-dx * kPanMetresPerPx, dy * kPanMetresPerPx);
                    else cam.orbit(-dx * kOrbitRadPerPx, dy * kOrbitRadPerPx);
                    last_pt = {vx, vy};
                    cameraChanged();
                }
                break;
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                ReleaseCapture();
                drag = Drag::None;
                break;
            case WM_LBUTTONDBLCLK:
                cam.reset();
                cameraChanged();
                break;
            default:
                break;
        }
    }

    void onPreviewMouse(UINT msg, WPARAM wp, LPARAM lp) {
        if (in3d()) {
            onPreviewMouse3D(msg, wp, lp);
            return;
        }
        const int vx = static_cast<short>(LOWORD(lp)), vy = static_cast<short>(HIWORD(lp));
        RECT rc{};
        GetClientRect(preview, &rc);
        const auto& m = shownMeta();
        switch (msg) {
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetCapture(preview);
                down_pt = last_pt = {vx, vy};
                down_shift = (wp & MK_SHIFT) != 0;
                drag = msg == WM_LBUTTONDOWN ? Drag::Pending : Drag::Pan;
                break;
            case WM_MOUSEMOVE: {
                if (!tracking_leave) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, preview, 0};
                    tracking_leave = TrackMouseEvent(&tme) != 0;
                }
                if (drag == Drag::Pending && (std::abs(vx - down_pt.x) > 4 || std::abs(vy - down_pt.y) > 4)) {
                    int px = 0, py = 0;
                    if (viewport.isZoomedOrPanned() && !down_shift) {
                        drag = Drag::Pan; // zoomed in: drag pans, Shift-drag draws a region
                    } else if (viewToPixel(down_pt.x, down_pt.y, px, py)) {
                        drag = Drag::Region;
                        region_on = true;
                        reg_x0 = reg_x1 = px;
                        reg_y0 = reg_y1 = py;
                        // A Side-by-Side region stays in the panel it started in.
                        const auto& st = shownSettings();
                        panelColumnsAt(px, static_cast<int>(m.width), imageLayoutMode(st), st.sbs_layout, st.mirror_horizontal,
                                       st.rotate_180, reg_col0, reg_col1);
                    } else {
                        drag = Drag::None;
                    }
                }
                if (drag == Drag::Pan) {
                    viewport.panBy(vx - last_pt.x, vy - last_pt.y, rc.right, rc.bottom, m.width, m.height);
                    needs_redraw = true;
                } else if (drag == Drag::Region) {
                    viewToPixelClamped(vx, vy, reg_x1, reg_y1);
                    reg_x1 = std::clamp(reg_x1, reg_col0, reg_col1);
                    recomputeTools();
                    presentShown();
                }
                last_pt = {vx, vy};
                updateReadoutAt(vx, vy);
                refreshInspectLine();
                break;
            }
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP: {
                ReleaseCapture();
                if (msg == WM_LBUTTONUP && drag == Drag::Pending) {
                    // A click: a ruler point (the third click starts a new ruler).
                    int px = 0, py = 0;
                    if (viewToPixel(vx, vy, px, py)) {
                        const SensorPixel sp = sensorAt(px, py);
                        RulerEndpoint e{px, py, sp.x, sp.y, sp.has_depth_ir};
                        ruler_before_click = ruler; // a double-click takes this click back
                        if (!ruler || ruler->b) {
                            ruler = RulerMeasurement{};
                            ruler->a = e;
                        } else {
                            ruler->b = e;
                        }
                        recomputeTools(); // on the buffers that belong to the shown frame
                        presentShown();
                    }
                }
                drag = Drag::None;
                refreshInspectLine();
                break;
            }
            case WM_LBUTTONDBLCLK:
                // Its first click already placed a ruler point: take it back.
                ruler = ruler_before_click;
                recomputeTools();
                presentShown();
                refreshInspectLine();
                viewport.reset();
                needs_redraw = true;
                break;
            case WM_MOUSELEAVE:
                tracking_leave = false;
                readout.reset();
                refreshInspectLine();
                break;
            default:
                break;
        }
    }

    void onWheel(int delta, POINT screen_pt) {
        POINT p = screen_pt;
        ScreenToClient(preview, &p);
        RECT rc{};
        GetClientRect(preview, &rc);
        if (p.x < 0 || p.y < 0 || p.x >= rc.right || p.y >= rc.bottom) return;
        if (in3d()) {
            cam.zoom(static_cast<float>(std::exp(kZoomPerNotch * delta / WHEEL_DELTA))); // up: closer
            cameraChanged();
            return;
        }
        const auto& m = shownMeta();
        if (m.width == 0) return;
        viewport.zoomAround(p.x, p.y, rc.right, rc.bottom, m.width, m.height, std::pow(1.15, delta / 120.0));
        updateReadoutAt(p.x, p.y);
        refreshInspectLine();
        needs_redraw = true;
    }

    // ---- presets ----
    void populatePresets() {
        if (!cbPreset) return;
        SendMessageW(cbPreset, CB_RESETCONTENT, 0, 0);
        SendMessageW(cbPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(presets ? L"Presets…" : L"Presets (off in test runs)"));
        preset_list = presets ? presets->all() : std::vector<ViewerPreset>{};
        for (const auto& p : preset_list) {
            const std::wstring label = widen(p.name.c_str()) + (p.built_in ? L"" : L" (yours)");
            SendMessageW(cbPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(cbPreset, CB_SETCURSEL, 0, 0);
        EnableWindow(cbPreset, presets != nullptr);
        EnableWindow(btnPresetSave, presets != nullptr);
        EnableWindow(btnPresetDelete, FALSE);
    }

    void applySettings(const ViewerSettings& next) {
        const ViewerSettings old = s;
        setFrozen(false);
        s = next;
        // RGB points go through setPointColorSource's guard (none in a playback), as from the panel.
        const bool rgb_refused = replaying && next.point_color_source == KV_POINT_COLOR_REGISTERED_RGB;
        if (rgb_refused) s.point_color_source = old.point_color_source;
        syncControlsFromSettings();
        clearTools();
        if (running && old.pipeline != s.pipeline) {
            restartInPlace(settingsNeedColor(s));
        } else {
            onViewChanged();
            pushDisplay();
            pushColor();
        }
        if (rgb_refused) setPointColorSource(next.point_color_source); // refused there, with its note
    }

    void onPresetSelected() {
        const int sel = comboSel(cbPreset);
        const bool custom = sel > 0 && sel - 1 < static_cast<int>(preset_list.size()) && !preset_list[sel - 1].built_in;
        EnableWindow(btnPresetDelete, custom);
        if (sel <= 0 || sel - 1 >= static_cast<int>(preset_list.size())) return;
        ViewerSettings next = s;
        applyPreset(preset_list[sel - 1], next);
        applySettings(next);
    }

    std::optional<std::wstring> askPresetName();

    void onPresetSave() {
        if (!presets) return;
        const auto name = askPresetName();
        if (!name) return;
        if (!presets->saveCustom(narrow(*name), s)) {
            MessageBoxW(main, L"That name is empty, or belongs to a built-in preset.", L"Save preset", MB_ICONINFORMATION);
            return;
        }
        populatePresets();
    }

    void onPresetDelete() {
        const int sel = comboSel(cbPreset);
        if (!presets || sel <= 0 || sel - 1 >= static_cast<int>(preset_list.size()) || preset_list[sel - 1].built_in) return;
        presets->deleteCustom(preset_list[sel - 1].name);
        populatePresets();
    }

    void setLabel(HWND h, const wchar_t* fmt, double v) {
        wchar_t buf[64];
        swprintf(buf, 64, fmt, v);
        SetWindowTextW(h, buf);
    }

    void refreshLabels() {
        setLabel(lblGamma, L"Gamma %.2f", s.ir_gamma);
        setLabel(lblEma, L"EMA \x03b1 %.2f", s.ir_ema_alpha);
        setLabel(lblBlack, L"Black %.0f", s.ir_manual_min);
        setLabel(lblWhite, L"White %.0f", s.ir_manual_max);
        setLabel(lblDmin, L"Min %.0f mm", s.depth_min_mm);
        setLabel(lblDmax, L"Max %.0f mm", s.depth_max_mm);
        setLabel(lblEv, L"EV comp %+.2f", s.auto_exposure_compensation);
        setLabel(lblSemi, L"Semi-auto %.1f ms", s.semi_auto_pseudo_ms);
        setLabel(lblManual, L"Shutter %.1f ms", s.manual_integration_ms);
        setLabel(lblGain, L"Analog gain %.2fx", s.manual_analog_gain);
        setLabel(lblPcSize, L"Point size %.1f px", s.point_size);
        setLabel(lblCutNear, L"Near %.0f mm", s.cutoff_near_mm);
        setLabel(lblCutFar, L"Far %.0f mm", s.cutoff_far_mm);
        setLabel(lblBlend, L"Depth fade %.0f%%", s.depth_color_blend_percent);
        setLabel(lblPcHint, L"Distance %.2f m", cam.distance);
    }

    void syncControlsFromSettings() {
        SendMessageW(tbGamma, TBM_SETPOS, TRUE, gammaToSlider(s.ir_gamma));
        SendMessageW(tbEma, TBM_SETPOS, TRUE, emaToSlider(s.ir_ema_alpha));
        SendMessageW(tbBlack, TBM_SETPOS, TRUE, static_cast<int>(s.ir_manual_min));
        SendMessageW(tbWhite, TBM_SETPOS, TRUE, static_cast<int>(s.ir_manual_max));
        SendMessageW(tbDmin, TBM_SETPOS, TRUE, static_cast<int>(s.depth_min_mm));
        SendMessageW(tbDmax, TBM_SETPOS, TRUE, static_cast<int>(s.depth_max_mm));
        SendMessageW(chkIrAuto, BM_SETCHECK, s.ir_auto_scale ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cbMode, CB_SETCURSEL, s.mode, 0);
        SendMessageW(cbSbs, CB_SETCURSEL, s.sbs_layout, 0);
        // Everything a preset can change.
        SendMessageW(cbPipeline, CB_SETCURSEL, s.pipeline == KV_PIPELINE_CPU ? 1 : 0, 0);
        SendMessageW(cbColormap, CB_SETCURSEL, s.depth_colormap, 0);
        SendMessageW(chkInvert, BM_SETCHECK, s.depth_invert ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cbExposure, CB_SETCURSEL, s.exposure_mode, 0);
        SendMessageW(tbEv, TBM_SETPOS, TRUE, evToSlider(s.auto_exposure_compensation));
        SendMessageW(tbSemi, TBM_SETPOS, TRUE, msToSlider(s.semi_auto_pseudo_ms));
        SendMessageW(tbManual, TBM_SETPOS, TRUE, msToSlider(s.manual_integration_ms));
        SendMessageW(tbGain, TBM_SETPOS, TRUE, gainToSlider(s.manual_analog_gain));
        if (chkMirror) SendMessageW(chkMirror, BM_SETCHECK, s.mirror_horizontal ? BST_CHECKED : BST_UNCHECKED, 0);
        if (chkRotate) SendMessageW(chkRotate, BM_SETCHECK, s.rotate_180 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(cbPcSource, CB_SETCURSEL, static_cast<int>(s.point_color_source), 0);
        SendMessageW(tbPcSize, TBM_SETPOS, TRUE, pointSizeToSlider(s.point_size));
        SendMessageW(chkCut, BM_SETCHECK, s.cutoff_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(tbCutNear, TBM_SETPOS, TRUE, cutoffToSlider(s.cutoff_near_mm));
        SendMessageW(tbCutFar, TBM_SETPOS, TRUE, cutoffToSlider(s.cutoff_far_mm));
        SendMessageW(chkBlend, BM_SETCHECK, s.depth_color_blend_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(tbBlend, TBM_SETPOS, TRUE, static_cast<int>(std::lround(s.depth_color_blend_percent)));
        refreshLabels();
        refreshEnabled();
    }

    void refreshEnabled() {
        EnableWindow(btnStart, !running);
        EnableWindow(btnStop, running);
        EnableWindow(cbSbs, s.mode == KV_MODE_SIDE_BY_SIDE);
        EnableWindow(chkOverlay, s.synthetic);
        EnableWindow(tbBlack, !s.ir_auto_scale);
        EnableWindow(tbWhite, !s.ir_auto_scale);
        EnableWindow(tbEma, s.ir_auto_scale);
        const bool colour = settingsNeedColor(s);
        // 3D controls in 3D; the blend is live only (not in a playback) and doesn't show in 3D.
        for (HWND h : {cbPcSource, tbPcSize, btnPcReset}) EnableWindow(h, in3d());
        EnableWindow(chkBlend, !replaying && !in3d());
        EnableWindow(tbBlend, !replaying && !in3d() && s.depth_color_blend_enabled);
        EnableWindow(tbCutNear, s.cutoff_enabled);
        EnableWindow(tbCutFar, s.cutoff_enabled);
        EnableWindow(cbExposure, colour);
        EnableWindow(tbEv, colour && s.exposure_mode == KV_EXPOSURE_AUTO);
        EnableWindow(tbSemi, colour && s.exposure_mode == KV_EXPOSURE_SEMI_AUTO);
        EnableWindow(tbManual, colour && s.exposure_mode == KV_EXPOSURE_MANUAL);
        EnableWindow(tbGain, colour && s.exposure_mode == KV_EXPOSURE_MANUAL);
        if (hooks) {
            EnableWindow(btnUnpair, hooks->isPaired());
        }
#ifdef KV_WITH_RECORDER
        // Colour is live only: Record is off in Colour, every Side-by-Side layout, 3D and with the
        // blend (a running recording can still be stopped, though changing mode stops it anyway).
        const bool can_record = recordingAllowed(s);
        if (btnRecord) EnableWindow(btnRecord, can_record || recorder.isRecording());
        if (ttRecord) {
            rec_tip_text = can_record ? std::wstring() : widen(kvhost::text::kColourLiveOnly);
            TTTOOLINFOW ti{sizeof(ti)};
            ti.hwnd = panelWnd;
            ti.uId = ID_RECORD;
            ti.lpszText = rec_tip_text.data();
            SendMessageW(ttRecord, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        updateRecordingLabel();
#endif
    }

    // ---- engine --------------------------------------------------------------------------------
    void startEngine(bool force_rgb = false) {
#ifdef KV_WITH_RECORDER
        if (replaying) exitPlayback(); // the sensor again: the playback ends
#endif
        setFrozen(false); // a (re)start is a new picture
        clearTools();
        sanitize(s);
        kv_display_params_t p = toDisplayParams(s);
        const bool want_rgb = force_rgb || settingsNeedColor(s);
        pc_seq = 0; // a new stream: the 3D inputs start again
        pc_have = false;
        last_seq = 0;
        clearShown();
        needs_redraw = true;
        awaiting_replug = false;
        char err[256] = {0};
        engine_source = s.synthetic ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL;
        motion_seq = 0; // a new stream numbers its frames again
        running = kv_engine_start_ex(eng, engine_source, s.pipeline, &p, want_rgb, err, sizeof(err));
        if (!running) {
            error = widen(err);
            if (error.empty()) error = L"Failed to start the Kinect source (no fallback performed).";
        } else {
            error.clear();
            if (want_rgb) pushColor();
            if (hooks) hooks->onModeOrSourceChanged();
        }
        refreshEnabled();
        updateStatus();
    }

    void stopRecording() {
#ifdef KV_WITH_RECORDER
        if (recorder.isRecording()) {
            recorder.stop();
            const kvrec::RecorderStatus st = recorder.status();
            wchar_t b[256];
            swprintf(b, 256, L"Saved %llu frames, %.1f MB", static_cast<unsigned long long>(st.frames_written), st.bytes / 1e6);
            rec_note = b;
            SetWindowTextW(btnRecord, L"Record");
            publishViewState(); // recording stop goes to the phone at once
        }
#endif
    }

    void toggleRecording() {
#ifdef KV_WITH_RECORDER
        if (recorder.isRecording()) {
            stopRecording();
            return;
        }
        requestRecordingStart(false, false);
#endif
    }

#ifdef KV_WITH_RECORDER
    // Every start, from this PC or the phone, goes through the reminder (phase 2.4) unless it was
    // snoozed: the phone may have shown it and been confirmed (phase 7 fix 5), otherwise it's shown here.
    bool reminder_open = false; // one reminder at a time
    void requestRecordingStart(bool confirmed_on_phone, bool snooze_from_phone) {
        if (reminder_open) return; // a second start (the phone's, say) while it's up is ignored
        if (const auto why = recordingDisabledReason(s, replaying)) {
            rec_note = widen(why->c_str());
            updateRecordingLabel();
            publishViewState();
            return;
        }
        if (!running || meta.width == 0) {
            rec_note = L"Start streaming first.";
            updateRecordingLabel();
            return;
        }
        const int64_t now = kvrec::unixNow();
        if (reminder.needed(now)) {
            bool snooze = false;
            if (confirmed_on_phone) {
                snooze = snooze_from_phone;
            } else if (!askRecordingReminder(&snooze)) {
                rec_note = L"Not recording: the reminder was cancelled.";
                updateRecordingLabel();
                publishViewState();
                return;
            }
            if (snooze) reminder.snoozeOneHour(now);
        }
        // While the reminder was up, the phone may have turned the Kinect off, or a playback may have
        // started, or the mode changed (or the blend came on): check it all again before recording.
        if (const auto why = recordingDisabledReason(s, replaying)) {
            rec_note = widen(why->c_str());
            updateRecordingLabel();
            publishViewState();
            return;
        }
        if (!running || meta.width == 0 || recorder.isRecording()) {
            rec_note = L"Not recording: the Kinect isn't streaming any more.";
            updateRecordingLabel();
            publishViewState();
            return;
        }
        startRecordingNow();
    }
    bool askRecordingReminder(bool* snooze);

    void startRecordingNow() {
        kvrec::RecorderOptions o;
        o.lossless = comboSel(cbRecFormat) == 1 ? kvrec::Lossless16::Png16Sequence : kvrec::Lossless16::Ffv1Mkv;
        // The depth camera's intrinsics go into session_meta.json, for the ruler on playback.
        kv_ir_intrinsics_t intr{};
        if (kv_engine_get_ir_intrinsics(eng, &intr) && intr.valid) o.ir_intrinsics = intr;
        std::string err;
        if (!recorder.start(s.mode, 512u, 424u, o, &err)) {
            rec_note = widen(err.c_str());
            return;
        }
        last_raw_seq = 0;
        rec_settings = s; // the view it records; a change of mode or layout stops it
        rec_note.clear();
        SetWindowTextW(btnRecord, L"Stop recording");
        publishViewState(); // recording start goes to the phone at once
    }
#endif

    void feedRecorder() {
#ifdef KV_WITH_RECORDER
        if (!recorder.isRecording()) return;
        raw16.resize(512 * 424);
        uint64_t seq = 0, arr = 0;
        uint32_t ticks = 0, w = 0, h = 0;
        if (kv_engine_copy_latest_raw16(eng, s.mode == KV_MODE_DEPTH, last_raw_seq, raw16.data(), raw16.size(), &seq, &arr,
                                        &ticks, &w, &h)) {
            last_raw_seq = seq;
            recorder.pushRaw16(raw16.data(), w, h, seq, ticks, arr);
        }
#endif
    }

    // ---- W7: the camera roll's motion feed (pet-camera.md §6.2) ------------------------------------------------
    // Depth is decoded in every view mode, so each new depth frame is polled here, as feedRecorder polls raw frames,
    // and handed to the host with its sequence number and capture time while motion is on. live_sensor is true
    // only for the live sensor's own frames: never during a replay, nor from a replay source.
    std::vector<uint16_t> motion_raw;
    uint64_t motion_seq = 0;            // the last depth frame handed over
    uint64_t motion_fed = 0, motion_fed_live = 0;
    bool motion_feed_wanted = false;    // the roll runs, is on, and motion is on (refreshRoll)
    bool motion_feed_paused = false;    // the --test-camera-roll hook feeds frames of its own for a while
    kv_source_kind_t engine_source = KV_SOURCE_SYNTHETIC;
    void feedMotion() {
        if (!hooks || !motion_feed_wanted || motion_feed_paused || (!running && !replaying)) return;
        motion_raw.resize(512 * 424);
        uint64_t seq = 0, arr = 0;
        uint32_t ticks = 0, w = 0, h = 0;
        if (!kv_engine_copy_latest_raw16(eng, true, motion_seq, motion_raw.data(), motion_raw.size(), &seq, &arr, &ticks, &w,
                                         &h)) {
            return;
        }
        motion_seq = seq;
        const bool live = running && !replaying && engine_source != KV_SOURCE_REPLAY;
        hooks->submitDepthForMotion(motion_raw.data(), w, h, seq, arr, live);
        ++motion_fed;
        if (live) ++motion_fed_live;
    }

    void updateRecordingLabel() {
#ifdef KV_WITH_RECORDER
        if (!lblRec) return;
        wchar_t b[512];
        if (recorder.isRecording()) {
            const kvrec::RecorderStatus st = recorder.status();
            const std::wstring dir = st.session_dir.substr(st.session_dir.find_last_of(L'\\') + 1);
            swprintf(b, 512, L"REC %.0f s | %llu frames | %.1f MB (%.0f MB/min) | dropped %llu\n%ls%ls%ls", st.seconds,
                     static_cast<unsigned long long>(st.frames_written), st.bytes / 1e6, st.mb_per_min,
                     static_cast<unsigned long long>(st.frames_dropped), dir.c_str(), st.error.empty() ? L"" : L" | ",
                     widen(st.error.c_str()).c_str());
        } else if (!recordingAllowed(s)) {
            swprintf(b, 512, L"%ls", widen(kvhost::text::kColourLiveOnly).c_str());
        } else {
            swprintf(b, 512, L"%ls", rec_note.empty() ? L"IR/Depth only, 16-bit lossless. Stays on this PC." : rec_note.c_str());
        }
        SetWindowTextW(lblRec, b);
#endif
    }

    void stopEngine() {
        setFrozen(false);
        clearTools();
        stopRecording();
        kv_engine_stop(eng);
        running = false;
        clearShown();
        needs_redraw = true;
        refreshEnabled();
        updateStatus();
    }

    void pushDisplay() {
        sanitize(s);
        const kv_display_params_t p = toDisplayParams(s);
        kv_engine_set_display_params(eng, &p);
    }

    void pushColor() {
        if (!running || !kv_engine_is_rgb_active(eng)) return;
        const kv_color_controls_t c = toColorControls(s);
        char err[256] = {0};
        if (!kv_engine_set_color_controls(eng, &c, err, sizeof(err))) {
            error = widen(err);
        }
    }

    // A frame the core mapped before the view changed is taken now and dropped. The core applies
    // display parameters under the same lock it maps under, so every later frame is the new view's.
    void dropFramesMappedBefore() {
        if (in3d()) return; // 3D takes fresh inputs (pc_seq = 0) and renders them with the new settings
        if (frame.size() < 1920u * 1080u * 4u) frame.resize(1920u * 1080u * 4u);
        size_t req = 0;
        kv_mapped_frame_t m{};
        if (kv_engine_copy_latest_frame(eng, last_seq, frame.data(), frame.size(), &req, &m)) last_seq = m.frame_seq;
    }

    void onViewChanged() {
        setFrozen(false); // a new mode or layout is a new picture
        clearTools();
        // The old view's frame (Colour before IR, say) goes from here and from the second display, and
        // isn't shown again: a capture right after the change can't hold it.
        frame_stale = true;
        clearShown();
        needs_redraw = true;
        if (in3d()) {
            viewport.reset(); // the 3D view zooms and pans its camera, not the picture
            pc_seq = 0;       // fresh inputs with the new settings
            pc_have = false;
        } else {
            setPcNote(L"");
        }
#ifdef KV_WITH_RECORDER
        if (recorder.isRecording() && viewChangeStopsRecording(rec_settings, s)) stopRecording();
#endif
        refreshEnabled();
        publishViewState();
        if (needsRestartForColor(running, kv_engine_is_rgb_active(eng), s)) {
            restartInPlace(true);
            dropFramesMappedBefore();
            return;
        }
        pushDisplay();
        dropFramesMappedBefore();
        if (hooks) hooks->onModeOrSourceChanged();
    }

    // ---- stage 5 controls (the panel and the phone go through these) ----
    void setPointColorSource(kv_point_color_source_t src) {
        if (src == KV_POINT_COLOR_REGISTERED_RGB && replaying) { // a recording has no colour (the Mac's)
            SendMessageW(cbPcSource, CB_SETCURSEL, static_cast<int>(s.point_color_source), 0);
            tool_note = L"Playback has no colour: the points can be depth colours or IR.";
            refreshInspectLine();
            return;
        }
        if (src == s.point_color_source) return;
        s.point_color_source = src;
        onViewChanged(); // RGB points need the colour stream: a restart with it when it's off
    }
    void setCutoffEnabled(bool on) {
        s.cutoff_enabled = on;
        SendMessageW(chkCut, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        refreshEnabled();
        pushDisplay();
        publishViewState(); // the phone shows the cut-off (0x07)
        refreshInspectLine();
    }
    void trackMe() {
        applyTrackMe(s);
        syncControlsFromSettings();
        pushDisplay();
        publishViewState();
        refreshInspectLine();
    }
    void setBlendEnabled(bool on) {
        if (on && (replaying || in3d())) { // live colour only; and it doesn't show in 3D
            SendMessageW(chkBlend, BM_SETCHECK, BST_UNCHECKED, 0);
            return;
        }
        if (on == s.depth_color_blend_enabled) return;
        s.depth_color_blend_enabled = on;
        onViewChanged(); // stops a recording, starts the colour stream if it's off
    }
    void onPhoneOrbit(const kvhost::Orbit3DGestureMessage& g) {
        if (!in3d()) return; // as the Mac: a gesture outside 3D doesn't move the hidden 3D camera
        applyPhoneGesture(cam, g.deltaYawRadians, g.deltaPitchRadians, g.zoomScale, g.panRightMeters, g.panUpMeters, g.resetView);
        cameraChanged();
    }

    // A v2 phone's remote-control command (already allowed by the server). Whatever happens, the
    // phone gets a fresh 0x07 afterwards, so a refused request shows as unchanged.
    void onPhoneCommand(const kvhost::RemoteControlCommandMessage& cmd) {
        using A = kvhost::RemoteControlAction;
        switch (cmd.action) {
            case A::SelectMode: {
                // Any mode, 3D included; an unknown one is refused whole and the phone's picker snaps back.
                ViewerSettings next = s;
                if (applyPhoneViewCommand(next, cmd) == PhoneViewChange::Mode) {
                    s = next;
                    syncControlsFromSettings();
                    onViewChanged();
                }
                break;
            }
            case A::StopRecording:
                stopRecording(); // always safe: it finishes the file
                break;
            case A::StartRecording:
#ifdef KV_WITH_RECORDER
                if (!recorder.isRecording()) {
                    requestRecordingStart(cmd.reminderConfirmedOnPhone.value_or(false), cmd.snoozeReminderOneHour.value_or(false));
                }
#endif
                break;
            case A::SetHostFrozen:
                if (cmd.hostFrozen) setFrozen(*cmd.hostFrozen);
                break;
            case A::RequestKeyframe:
                // The controller asked the encoder for a keyframe; a frozen view sends its frame again.
                if (frozen) sendFrameToHost(frozen_frame.data(), frozen_meta, frozen_settings);
                break;
            case A::SetCutoff: { // the Mac's: Track me wins; otherwise on or off
                ViewerSettings next = s;
                if (applyPhoneViewCommand(next, cmd) == PhoneViewChange::Cutoff) {
                    s.cutoff_enabled = next.cutoff_enabled;
                    s.cutoff_near_mm = next.cutoff_near_mm;
                    s.cutoff_far_mm = next.cutoff_far_mm;
                    syncControlsFromSettings();
                    pushDisplay();
                    refreshInspectLine();
                }
                break;
            }
        }
        publishViewState();
        if (hooks) hooks->pushHostState();
    }

    // ---- frames and status -----------------------------------------------------------------------
    // 3D: the core makes no mapped frame in mode 4, only the point cloud's inputs. New ones are taken
    // unless the view is frozen (then the frozen inputs stay, and the camera can still move around them).
    bool pollPointCloud() {
        if (frozen) return false;
        pc_depth.resize(512u * 424u);
        pc_bgra.resize(512u * 424u * 4u);
        uint64_t seq = 0, arr = 0;
        kv_ir_intrinsics_t intr{};
        if (!kv_engine_copy_point_cloud_inputs(eng, pc_seq, pc_depth.data(), pc_depth.size(), pc_bgra.data(), pc_bgra.size(), &seq,
                                               &arr, &intr)) {
            // Nothing new. Without intrinsics there never will be (the core keeps no registration then):
            // say so rather than show an empty view.
            // (A playback's engine has the recording's intrinsics, or none.)
            if (!pc_have) {
                kv_ir_intrinsics_t now{};
                const bool usable = kv_engine_get_ir_intrinsics(eng, &now) && pointCloudIntrinsicsUsable(now);
                if (usable) setPcNote(L"");
                else if (pc_note != kNo3DIntrinsics) return showBlank3D(kNo3DIntrinsics); // once, not every loop
            }
            return false;
        }
        pc_seq = seq;
        pc_arrival_ns = arr;
        pc_intr = intr;
        pc_have = true;
        return renderPointCloud();
    }

    // Renders the cached inputs with the camera now; the picture goes to the screen and the phone as a
    // 960x720 frame. Frozen: the frozen frame is replaced (the Mac re-renders a frozen view on a camera
    // move too).
    bool renderPointCloud() {
        pc_dirty = false;
        if (!pc_have) return false;
        // (A renderer that never started, or lost its device, starts again inside render.)
        std::vector<uint8_t>& target = frozen ? frozen_frame : frame;
        std::wstring why;
        if (!pc.render(pc_depth.data(), pc_bgra.data(), pc_intr, cam, s.point_size, s.mirror_horizontal, s.rotate_180,
                       PointCloudRenderer::kWidth, PointCloudRenderer::kHeight, target, &why)) {
            // No intrinsics (nothing drawn, no made-up values: phase 7), or the GPU: black, with why.
            if (why != pc_note) showBlank3D(why);
            return false;
        }
        setPcNote(L"");
        ++pc_rendered;
        pc_render_ms = pc.lastRenderMs();
        const uint64_t now = kv_monotonic_ns();
        pc_times.push_back(now);
        while (!pc_times.empty() && now - pc_times.front() > 1000000000ull) pc_times.erase(pc_times.begin());
        publish3DFrame();
        return true;
    }

    // The 3D view can't draw: a black 960x720 frame on the PC and to the phone, so the last picture
    // (a 2D one, before the switch) isn't left up under the note.
    bool showBlank3D(const std::wstring& why) {
        setPcNote(why);
        std::vector<uint8_t>& target = frozen ? frozen_frame : frame;
        target.assign(static_cast<size_t>(PointCloudRenderer::kWidth) * PointCloudRenderer::kHeight * 4u, 0);
        for (size_t i = 3; i < target.size(); i += 4) target[i] = 255;
        publish3DFrame();
        return true;
    }

    // The 960x720 picture in frame (or frozen_frame) goes to the screen and the phone.
    void publish3DFrame() {
        std::vector<uint8_t>& target = frozen ? frozen_frame : frame;
        kv_mapped_frame_t m{};
        m.frame_seq = ++pc_frame_seq;
        m.arrival_mach_ns = pc_arrival_ns;
        m.map_done_mach_ns = kv_monotonic_ns();
        m.width = PointCloudRenderer::kWidth;
        m.height = PointCloudRenderer::kHeight;
        m.stride_bytes = PointCloudRenderer::kWidth * 4u;
        m.mode = KV_MODE_POINT_CLOUD_3D;
        m.is_synthetic = s.synthetic;
        (frozen ? frozen_meta : meta) = m;
        if (frozen) frozen_settings = s;
        frame_stale = false;
        presentShown();
        sendFrameToHost(target.data(), m, s);
    }
    void setPcNote(const std::wstring& n) {
        if (n == pc_note) return;
        pc_note = n;
        refreshInspectLine();
    }
    void cameraChanged() {
        pc_dirty = true;
        setLabel(lblPcHint, L"Distance %.2f m", cam.distance);
    }

    // Every frame the streaming host gets (the live tiers, the roll's thumbnail) goes through here, counted: the
    // live view's frames only. The roll's player has no path to it (roll_ui.hpp); --test-camera-roll checks the
    // count doesn't move while the player decodes.
    // Each is also counted by source. A live-source frame is one of the live view's own buffers (the frame just
    // mapped, or the frozen one); the hook compares that with every frame the hooks handed the host
    // (framesHandedToHost), so a single frame from anywhere else (the player) shows up.
    uint64_t on_frame_calls = 0;
    uint64_t on_frame_live = 0;
    void sendFrameToHost(const uint8_t* bgra, const kv_mapped_frame_t& m, const ViewerSettings& st) {
        if (!hooks) return;
        ++on_frame_calls;
        if (bgra && (bgra == frame.data() || bgra == frozen_frame.data())) ++on_frame_live;
        hooks->onFrame(bgra, m, st);
    }

    bool pollFrame() {
        if (!running && !replaying) return false;
        if (in3d()) return pollPointCloud();
        if (frame.size() < 1920u * 1080u * 4u) frame.resize(1920u * 1080u * 4u);
        size_t req = 0;
        kv_mapped_frame_t m{};
        if (!kv_engine_copy_latest_frame(eng, last_seq, frame.data(), frame.size(), &req, &m)) {
            if (req > frame.size()) frame.resize(req);
            return false;
        }
        last_seq = m.frame_seq;
        meta = m;
        frame_stale = false; // a frame of the current view
        if (frozen) return false; // the frozen frame stays on screen (and on the phone)
        // The depth and IR that go with the frame shown, so a freeze, the readout and the tools all read
        // the same moment. The tools are recomputed only here, once per new frame.
        refreshInspectionBuffers();
        if (region_on || ruler) recomputeTools();
        presentShown();
        sendFrameToHost(frame.data(), m, s);
        return true; // the recorder takes raw IR/Depth (feedRecorder), never a rendered or colour frame
    }

    void updateStatus() {
        kv_stats_snapshot_t st;
        kv_engine_get_stats(eng, &st);
        wchar_t buf[1024];
        if (running) {
            swprintf(buf, 1024,
                     L"%ls | %ls | %ls %ux%u | %.1f fps%ls | IR %llu  Depth %llu  Colour %llu | drops %llu/%llu/%llu | "
                     L"map %.2f ms | arrival->mapped %.2f ms | driver errors %u\n%ls%ls%ls",
                     s.synthetic ? L"Synthetic" : L"Kinect v2", pipelineLabel(st.active_pipeline),
                     modeLabel(st.active_mode), meta.width, meta.height, st.current_fps,
                     st.color_fps > 0.0 ? (std::wstring(L" (colour ") + std::to_wstring(static_cast<int>(st.color_fps + 0.5)) + L")").c_str() : L"",
                     static_cast<unsigned long long>(st.ir_frames), static_cast<unsigned long long>(st.depth_frames),
                     static_cast<unsigned long long>(st.color_frames), static_cast<unsigned long long>(st.ir_seq_drops),
                     static_cast<unsigned long long>(st.depth_seq_drops),
                     static_cast<unsigned long long>(st.color_seq_drops), st.map_time_mean_ms,
                     st.arrival_to_map_done_ms, st.driver_errors, widen(st.status_message).c_str(),
                     error.empty() ? L"" : L" | ", error.c_str());
#ifdef KV_WITH_RECORDER
        } else if (replaying && replay) {
            swprintf(buf, 1024, L"Playback (the Kinect is off) | %ls recording of %ls | frame %d of %d%ls\n%ls",
                     replay_item.modeTitle().c_str(), replay_item.dateText().c_str(), replay_index + 1, replay->totalFrames(),
                     replay_playing ? L"" : L" | paused", error.c_str());
#endif
        } else {
            swprintf(buf, 1024, L"Stopped | Kinect on USB: %ls%ls\n%ls%ls", st.device_present ? L"yes" : L"no",
                     st.lock_blocked ? L" | lock held by another program" : L"", error.c_str(),
                     hardware_note.empty() ? L"" : (L"  " + hardware_note).c_str());
        }
        std::wstring line = buf;
        if (in3d() && (running || replaying)) {
            // The 3D view's own rate: renders in the last second (each new input, plus camera moves).
            const uint64_t now = kv_monotonic_ns();
            size_t recent = 0;
            for (uint64_t t : pc_times) recent += (now - t) <= 1000000000ull;
            wchar_t b[200];
            swprintf(b, 200, L" | 3D %zu fps, render + readback %.1f ms (%ls)", recent, pc_render_ms,
                     pc.usingWarp() ? L"WARP" : L"GPU");
            const size_t nl = line.find(L'\n');
            line.insert(nl == std::wstring::npos ? line.size() : nl, b);
        }
        if (out.active()) {
            // OUT: what the second display shows now (the Mac's badge).
            const size_t nl = line.find(L'\n');
            line.insert(nl == std::wstring::npos ? line.size() : nl, out.blackout() ? L" | OUT: BLACKED OUT (B)" : L" | OUT: LIVE");
        }
        SetWindowTextW(status, line.c_str());
        updateRecordingLabel();
        updateHealth();
        refreshInspectLine();
        if (hooks && lblStream) {
            SetWindowTextW(lblStream, hooks->statusLine().c_str());
            EnableWindow(btnUnpair, hooks->isPaired());
        }
        refreshPet(false);
    }

    void layout() {
        RECT rc{};
        GetClientRect(main, &rc);
        panel_w = fullscreen ? 0 : S(300) + GetSystemMetricsForDpi(SM_CXVSCROLL, static_cast<UINT>(dpi));
        if (panelWnd && !fullscreen) {
            MoveWindow(panelWnd, 0, 0, panel_w, rc.bottom, TRUE);
            updatePanelScroll();
        }
        const int status_h = fullscreen ? 0 : S(40);
        const int insp_h = S(20);
        const int area_w = std::max<int>(1, rc.right - panel_w);
        // Strips above the image (the drop banner, the health display) and below it (the inspector
        // line): plain windows next to the D3D surface, never drawn over it.
        int top = 0;
        if (lblWatching) {
            // "Watching now": whenever any session holds a watcher slot, in full screen too. No control hides it.
            const bool on = !watching_text.empty();
            ShowWindow(lblWatching, on ? SW_SHOW : SW_HIDE);
            if (on) {
                MoveWindow(lblWatching, panel_w, top, area_w, S(24), TRUE);
                top += S(24);
            }
        }
        if (lblBanner) {
            ShowWindow(lblBanner, banner_on ? SW_SHOW : SW_HIDE);
            if (banner_on) {
                MoveWindow(lblBanner, panel_w, top, area_w, S(22), TRUE);
                top += S(22);
            }
        }
        if (lblHealth) {
            ShowWindow(lblHealth, show_health ? SW_SHOW : SW_HIDE);
            if (show_health) {
                const int hh = hooks ? S(82) : S(66); // a fifth line: the live tiers
                MoveWindow(lblHealth, panel_w + S(6), top + S(2), area_w - S(8), hh, TRUE);
                top += hh + S(4);
            }
        }
        const int bottom = std::max<int>(top + 1, rc.bottom - status_h - insp_h);
        MoveWindow(preview, panel_w, top, area_w, bottom - top, TRUE);
        if (lblInspect) MoveWindow(lblInspect, panel_w + S(6), bottom + S(2), std::max<int>(1, area_w - S(8)), insp_h - S(2), TRUE);
        if (!fullscreen) {
            MoveWindow(status, panel_w + S(6), rc.bottom - status_h + S(2), std::max<LONG>(1, rc.right - panel_w - S(8)),
                       status_h - S(4), TRUE);
        }
        d3d.resize();
        needs_redraw = true;
    }

    void writeScreenshotReport(bool ok, const std::string& err) {
        const std::wstring path = lo.screenshot_path + L".txt";
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return;
        kv_stats_snapshot_t st;
        kv_engine_get_stats(eng, &st);
        std::fprintf(f, "ok=%d\nerror=%s\nrunning=%d\nsynthetic=%d\nmode=%d\nsbs=%d\nframe=%ux%u\nfps=%.2f\nrendered=%llu\n"
                        "ir_frames=%llu\ncolor_frames=%llu\ndrops=%llu\nhealth_shown=%d\nfrozen=%d\nmirror=%d\nrotate=%d\ninspect=%s\n",
                     ok ? 1 : 0, err.c_str(), running ? 1 : 0, s.synthetic ? 1 : 0, static_cast<int>(s.mode),
                     static_cast<int>(s.sbs_layout), meta.width, meta.height, st.current_fps,
                     static_cast<unsigned long long>(st.rendered_frames), static_cast<unsigned long long>(st.ir_frames),
                     static_cast<unsigned long long>(st.color_frames),
                     static_cast<unsigned long long>(st.ir_seq_drops + st.depth_seq_drops), show_health ? 1 : 0, frozen ? 1 : 0,
                     s.mirror_horizontal ? 1 : 0, s.rotate_180 ? 1 : 0, narrow(inspectText()).c_str());
        // Stage 5: the 3D view (renders, the last render's time, the camera), the cut-off and the blend.
        const auto& sm = shownMeta();
        std::fprintf(f, "shown=%ux%u\npc_rendered=%llu\npc_render_ms=%.2f\npc_warp=%d\npc_note=%s\ncam_yaw=%.3f\ncam_pitch=%.3f\n"
                        "cam_distance=%.3f\npoint_source=%d\npoint_size=%.1f\ncutoff=%d\ncutoff_near=%.0f\ncutoff_far=%.0f\nblend=%d\n",
                     sm.width, sm.height, static_cast<unsigned long long>(pc_rendered), pc_render_ms, pc.usingWarp() ? 1 : 0,
                     narrow(pc_note).c_str(), cam.yaw, cam.pitch, cam.distance, static_cast<int>(s.point_color_source), s.point_size,
                     s.cutoff_enabled ? 1 : 0, s.cutoff_near_mm, s.cutoff_far_mm, s.depth_color_blend_enabled ? 1 : 0);
        // Stage 6: the second display (the test hook's small window), B and Esc, through the keys' own code.
        const SecondTest& t = second_test;
        std::fprintf(f,
                     "displays=%zu\nsecond_ran=%d\nsecond_opened_blacked=%d\nsecond_took_focus=%d\nsecond_click_through=%d\n"
                     "second_hit_through=%d\nsecond_frame_clean=%d\nsecond_tools_on_main=%d\nsecond_frames=%llu\n"
                     "second_live_lit=%zu\nsecond_live_total=%zu\nsecond_blackout_after_b=%d\nsecond_black_lit=%zu\n"
                     "second_black_total=%zu\nsecond_black_presents=%llu\nsecond_esc_presses=%d\nsecond_esc_stopped=%d\n"
                     "switch_cleared=%d\nsecond_black_after_frames=%d\n",
                     displays.size(), t.ran, t.opened_blacked, t.took_focus, t.click_through, t.hit_through, t.frame_clean,
                     t.tools_on_main, static_cast<unsigned long long>(t.frames), t.live_lit, t.live_total, t.blackout_after_b,
                     t.black_lit, t.black_total, static_cast<unsigned long long>(t.black_presents), t.esc_presses, t.esc_stopped,
                     t.switch_cleared, t.black_after_frames);
        // W6: where the host kept its state (v3 §10 isolation), and the pet camera as the panel shows it.
        if (hooks) {
            const kvhost::PetSettings ps = hooks->petSettings();
            std::fprintf(f, "state_dir=%s\nstate_dir_temp=%d\npet_devices=%zu\npet_sharing=%d\npet_privacy=%d\npet_schedule_rows=%zu\n"
                            "watching_banner=%s\nhealth_tiers=%s\n",
                         narrow(hooks->stateDir()).c_str(), hooks->stateDirIsTemporary() ? 1 : 0, hooks->petDevices().size(),
                         ps.sharingEnabled ? 1 : 0, ps.privacyMode ? 1 : 0, ps.sharingSchedule.size(), narrow(watching_text).c_str(),
                         narrow(health_tier_line).c_str());
            // This PC's identity, and the status line's first line (it says why the host is off).
            const std::wstring status_line = hooks->statusLine();
            std::fprintf(f, "host_identity_unusable=%d\nhost_status=%s\n", hooks->hostIdentityUnusable() ? 1 : 0,
                         narrow(status_line.substr(0, status_line.find(L'\n'))).c_str());
            // The pairing window, when it is open, is kept out of screen capture.
            std::fprintf(f, "pairing_open=%d\npairing_capture_excluded=%d\n", hooks->pairingWindow() ? 1 : 0,
                         kvwin::excludedFromCapture(hooks->pairingWindow()) ? 1 : 0);
        }
        if (lo.test_identity_reset) {
            const IdTest& t = id_test;
            std::fprintf(f,
                         "id_ran=%d\nid_done=%d\nid_failed=%s\nid_unusable_at_start=%d\nid_panel_shown=%d\nid_panel_text=%s\n"
                         "id_status_text=%s\nid_retry_still_unusable=%d\nid_retry_left_file=%d\nid_reset_ok=%d\n"
                         "id_reset_note=%s\nid_running_after=%d\nid_panel_hidden_after=%d\nid_device_paired=%d\n"
                         "id_device_connected=%d\n",
                         t.ran, t.done ? 1 : 0, t.failed.c_str(), t.unusable_at_start, t.panel_shown,
                         narrow(oneLine(t.panel_text)).c_str(), narrow(t.status_text).c_str(), t.retry_still_unusable,
                         t.retry_left_file, t.reset_ok, t.reset_note.c_str(), t.running_after, t.panel_hidden_after,
                         t.device_paired, t.device_connected);
        }
        if (lo.test_pet_camera) {
            const PetTest& p = pet_test;
            std::fprintf(f,
                         "pet_ran=%d\npet_done=%d\npet_failed=%s\npet_loaded_devices=%zu\npet_loaded_names=%s\n"
                         "pet_loaded_sharing=%d\npet_loaded_privacy=%d\npet_loaded_stale_owners=%d\npet_sharing_turned_on=%d\n"
                         "pet_device_role=%s\npet_banner_shown=%d\npet_banner_text=%s\npet_tiers=%s\npet_tier_high_sessions=%d\n"
                         "pet_health_tiers_shown=%d\npet_privacy_hid_banner=%d\npet_privacy_button_on=%d\n"
                         "pet_privacy_frames_while_on=%d\npet_privacy_off_back=%d\npet_sharing_off_hid=%d\n"
                         "pet_sharing_on_back=%d\npet_schedule_row_added=%d\npet_schedule_hid=%d\npet_schedule_removed=%d\n"
                         "pet_schedule_back=%d\npet_renamed=%d\npet_renamed_banner=%s\npet_devices_dialog_rows=%d\n"
                         "pet_devices_highlighted=%d\npet_devices_captured=%d\npet_revoked_notice=%d\n"
                         "pet_devices_after_revoke=%zu\npet_devices_dialog_rows_after=%d\npet_banner_after_revoke=%d\n",
                         p.ran, p.done ? 1 : 0, p.failed.c_str(), p.loaded_devices, p.loaded_names.c_str(), p.loaded_sharing,
                         p.loaded_privacy, p.loaded_stale_owners, p.sharing_turned_on, p.device_role.c_str(), p.banner_shown,
                         narrow(p.banner_text).c_str(), narrow(p.tiers).c_str(), p.tier_high_sessions, p.health_tiers_shown,
                         p.privacy_hid_banner, p.privacy_button_on, p.privacy_frames_while_on, p.privacy_off_back,
                         p.sharing_off_hid, p.sharing_on_back, p.schedule_row_added, p.schedule_hid, p.schedule_removed,
                         p.schedule_back, p.renamed, narrow(p.renamed_banner).c_str(), p.devices_dialog_rows,
                         p.devices_highlighted, p.devices_captured, p.revoked_notice, p.devices_after_revoke,
                         p.devices_dialog_rows_after, p.banner_after_revoke);
        }
        // W7: the camera roll as this run has it (v3 §10: a test run's roll is only --camera-roll-dir's folder).
        if (hooks) {
            const kvhost::PetSettings ps = hooks->petSettings();
            const kvhost::RollRecorderStatus rs = hooks->rollStatus();
            wchar_t note[512] = {0}, status_text[512] = {0};
            if (lblRollNote) GetWindowTextW(lblRollNote, note, 512);
            if (lblRollStatus) GetWindowTextW(lblRollStatus, status_text, 512);
            std::fprintf(f, "roll_available=%d\nroll_error=%s\nroll_dir=%s\nroll_enabled=%d\nroll_motion=%d\nroll_sensitivity=%d\n"
                            "roll_schedule_on=%d\nroll_schedule_rows=%zu\nroll_keep_days=%d\nroll_cap_bytes=%llu\nroll_folder_setting=%s\n"
                            "roll_clips_finalized=%llu\nroll_status=%s\nroll_note=%s\nmotion_fed=%llu\nmotion_fed_live=%llu\n"
                            "roll_test_run=%d\nroll_limits_known=%d\nroll_limits=%s\nroll_storage_reads=%llu\n",
                         hooks->rollAvailable() ? 1 : 0, hooks->rollError().c_str(), narrow(hooks->rollDir()).c_str(),
                         ps.rollEnabled ? 1 : 0, ps.motionEnabled ? 1 : 0, ps.motionSensitivity, ps.recordScheduleEnabled ? 1 : 0,
                         ps.recordSchedule.size(), ps.rollKeepDays, static_cast<unsigned long long>(ps.rollCapBytes),
                         ps.rollFolder.c_str(), static_cast<unsigned long long>(rs.clipsFinalized),
                         narrow(oneLine(status_text)).c_str(), narrow(oneLine(note)).c_str(),
                         static_cast<unsigned long long>(motion_fed), static_cast<unsigned long long>(motion_fed_live),
                         hooks->rollHostTestRun() ? 1 : 0, hooks->rollLimitsKnown() ? 1 : 0, narrow(windowText(lblRollLimits)).c_str(),
                         static_cast<unsigned long long>(roll_storage_reads));
        }
#ifdef KV_WITH_STREAMING
        if (lo.test_camera_roll) {
            const RollTest& r = roll_test;
            std::fprintf(f,
                         "rt_ran=%d\nrt_done=%d\nrt_failed=%s\nrt_available_at_start=%d\nrt_unreadable=%d\nrt_refused_unreadable=%d\n"
                         "rt_roll_on=%d\nrt_sens_set=%d\nrt_sched_row_added=%d\nrt_sched_on=%d\nrt_keep_set=%d\n"
                         "rt_folder_refused_recordings=%d\nrt_folder_refusal=%s\nrt_folder_changed=%d\nrt_folder_note=%s\n"
                         "rt_d6_note_shown=%d\nrt_d6_note=%s\nrt_d6_mismatch=%d\nrt_preparing_seen=%d\nrt_preparing_mismatch=%d\n"
                         "rt_limits_text=%s\nrt_confirm_shown=%d\nrt_confirm_ok=%d\nrt_limits_known_later=%d\nrt_confirm_hidden_later=%d\n",
                         r.ran, r.done ? 1 : 0, r.failed.c_str(), r.available_at_start, r.unreadable, r.refused_unreadable, r.roll_on,
                         r.sens_set, r.sched_row_added, r.sched_on, r.keep_set, r.folder_refused_recordings,
                         narrow(r.folder_refusal).c_str(), r.folder_changed, narrow(r.folder_note).c_str(), r.d6_note_shown,
                         narrow(r.d6_note).c_str(), r.d6_mismatch, r.preparing_seen, r.preparing_mismatch,
                         narrow(r.limits_text).c_str(), r.confirm_shown, r.confirm_ok, r.limits_known_later, r.confirm_hidden_later);
            std::fprintf(f,
                         "rt_armed=%d\nrt_motion_from_replay_frames_started=%d\nrt_motion_viewer_clip=%d\nrt_motion_clip_finalized=%d\n"
                         "rt_manual_started=%d\nrt_manual_stopped=%d\nrt_manual2_stopped=%d\nrt_clips_finalized=%llu\n"
                         "rt_motion_greyed=%d\nrt_motion_greyed_text=%s\nrt_browser_days=%d\nrt_browser_clips=%d\nrt_browser_thumbs=%d\n"
                         "rt_manual_found=%d\nrt_manual_has_thumb=%d\nrt_manual_by_pc=%d\nrt_motion_found=%d\nrt_manual2_found=%d\n"
                         "rt_manual_id=%s\nrt_motion_id=%s\nrt_manual2_id=%s\nrt_starred=%d\nrt_shot_refused_browser=%d\n",
                         r.armed, r.motion_from_replay_frames_started, r.motion_viewer_clip, r.motion_clip_finalized,
                         r.manual_started, r.manual_stopped, r.manual2_stopped, static_cast<unsigned long long>(r.clips_finalized),
                         r.motion_greyed, narrow(r.motion_greyed_text).c_str(), r.browser_days, r.browser_clips, r.browser_thumbs,
                         r.manual_found, r.manual_has_thumb, r.manual_by_pc, r.motion_found, r.manual2_found, r.manual_id.c_str(),
                         r.motion_id.c_str(), r.manual2_id.c_str(), r.starred, r.shot_refused_browser);
            std::fprintf(f,
                         "rt_player_opened=%d\nrt_player_frames=%llu\nrt_player_size=%ux%u\nrt_player_seeked=%d\n"
                         "rt_player_seek_target=%lld\nrt_player_seek_pts=%lld\nrt_player_live_frames=%llu\nrt_player_foreign_frames=%llu\n"
                         "rt_seek_burst_baseline_live=%llu\nrt_seek_burst_live=%llu\nrt_seek_burst_requests=%llu\n"
                         "rt_seek_burst_completed=%llu\nrt_seek_burst_ui_max_ms=%llu\nrt_onframe_during_player=%llu\n"
                         "rt_tier_frames_during_player=%llu\nrt_player_decoded_while_off=%llu\nrt_shot_refused_player=%d\n"
                         "rt_shot_refused_browser_only=%d\n",
                         r.player_opened, static_cast<unsigned long long>(r.player_frames), r.player_w, r.player_h,
                         r.player_seeked, static_cast<long long>(r.player_seek_target), static_cast<long long>(r.player_seek_pts),
                         static_cast<unsigned long long>(r.t6_live), static_cast<unsigned long long>(r.t6_foreign),
                         static_cast<unsigned long long>(r.k4_baseline), static_cast<unsigned long long>(r.k4_burst),
                         static_cast<unsigned long long>(r.k4_requests), static_cast<unsigned long long>(r.k4_completed),
                         static_cast<unsigned long long>(r.k4_ui_max_ms),
                         static_cast<unsigned long long>(r.onframe_after - r.onframe_before),
                         static_cast<unsigned long long>(r.tier_frames_after - r.tier_frames_before),
                         static_cast<unsigned long long>(r.player_decoded_while_off), r.shot_refused_player,
                         r.shot_refused_browser_only);
            std::fprintf(f,
                         "rt_pins_while_open=%zu\nrt_pins_after_close=%zu\nrt_owner_delete_hooked=%d\nrt_owner_delete=%s\n"
                         "rt_owner_delete_player_closed=%d\nrt_owner_delete_notice=%s\nrt_owner_delete_pins_after=%zu\n"
                         "rt_owner_delete_gone=%d\nrt_deleted=%d\nrt_pc_delete_closed_player=%d\nrt_deleted_gone=%d\n"
                         "rt_shot_allowed_after=%d\n",
                         r.pins_while_open, r.pins_after_close, roll_delete_hooked ? 1 : 0, r.owner_delete_result.c_str(),
                         r.owner_delete_player_closed, narrow(r.owner_delete_notice).c_str(), r.pins_after_owner_delete,
                         r.owner_delete_gone, r.deleted, r.pc_delete_closed_player, r.deleted_gone, r.shot_allowed_after);
        }
        // W8: outside the home as the panel shows it. The invite's link is never written here (v3 §10): only that it
        // was shown, masked, and its length.
        if (hooks) {
            const OutsidePanelView ov = hooks->outsideView();
            std::fprintf(f, "outside_available=%d\noutside_allowed=%d\noutside_listening=%d\noutside_status=%d\noutside_status_text=%s\n"
                            "outside_invite_open=%d\n",
                         hooks->outsideAvailable() ? 1 : 0, ov.allowed ? 1 : 0, ov.listening ? 1 : 0, static_cast<int>(ov.status),
                         narrow(oneLine(windowText(lblOutsideStatus))).c_str(), hooks->currentOutsideInvite() ? 1 : 0);
        }
        if (lo.test_outside) {
            const OutsideTest& o = outside_test;
            std::fprintf(f,
                         "ot_ran=%d\not_done=%d\not_failed=%s\not_host_interface=%d\not_router_injected=%d\not_s_off=%d\n"
                         "ot_s_sharing_off=%d\not_s_save_error=%d\not_s_mapped=%d\not_s_manual_field=%d\not_s_manual_entered=%d\n"
                         "ot_s_error=%d\not_s_carrier=%d\not_s_address_changed=%d\not_s_checking=%d\not_off_at_start=%d\n"
                         "ot_sharing_off_reason=%d\not_w_invite_shown=%d\not_w_invite_masked=%d\not_w_invite_countdown=%d\n"
                         "ot_w_invite_warning=%d\not_w_invite_role=%d\not_w_shot_refused=%d\not_w_self_closed=%d\n"
                         "ot_w_shot_allowed_after=%d\not_w_reshare_shown=%d\not_w_reshare_acknowledged=%d\not_w_log_opened=%d\n",
                         o.ran, o.done ? 1 : 0, o.failed.c_str(), o.host_interface, o.router_injected, o.s_off, o.s_sharing_off,
                         o.s_save_error, o.s_mapped, o.s_manual_field, o.s_manual_entered, o.s_error, o.s_carrier,
                         o.s_address_changed, o.s_checking, o.off_at_start, o.sharing_off_reason, o.w_invite_shown,
                         o.w_invite_masked, o.w_invite_countdown, o.w_invite_warning, o.w_invite_role, o.w_shot_refused,
                         o.w_self_closed, o.w_shot_allowed_after, o.w_reshare_shown, o.w_reshare_acknowledged, o.w_log_opened);
            std::fprintf(f,
                         "ot_mapped=%d\not_mapped_endpoint=%d\not_mapped_invite_enabled=%d\not_invite_shown=%d\not_invite_link_ok=%d\n"
                         "ot_invite_link_length=%zu\not_invite_masked=%d\not_invite_token_hidden=%d\not_invite_countdown=%d\n"
                         "ot_invite_warning=%d\not_invite_role_viewer=%d\not_invite_still_open=%d\not_shot_refused_invite=%d\n"
                         "ot_invite_cancelled=%d\not_invite_window_gone=%d\not_shot_allowed_after_invite=%d\not_invite_role_owner=%d\n"
                         "ot_invite_self_closed=%d\not_log_opened=%d\not_log_rows=%d\not_invite_capture_excluded=%d\n",
                         o.mapped, o.mapped_endpoint, o.mapped_invite_enabled, o.invite_shown, o.invite_link_ok,
                         o.invite_link_length, o.invite_masked, o.invite_token_hidden, o.invite_countdown, o.invite_warning,
                         o.invite_role_viewer, o.invite_still_open, o.shot_refused_invite, o.invite_cancelled,
                         o.invite_window_gone, o.shot_allowed_after_invite, o.invite_role_owner, o.invite_self_closed,
                         o.log_opened, o.log_rows, o.invite_capture_excluded);
            std::fprintf(f,
                         "ot_changed_shown=%d\not_reshare_opened=%d\not_reshare_link_ok=%d\not_reshare_no_secret=%d\n"
                         "ot_changed_acknowledged=%d\not_off_disables=%d\not_mapping_removed=%d\not_carrier_nat=%d\n"
                         "ot_carrier_invite_disabled=%d\not_manual=%d\not_manual_steps=%d\not_manual_field=%d\n"
                         "ot_manual_bad_refused=%d\not_manual_entered=%d\not_manual_unverified=%d\not_manual_invite_enabled=%d\n"
                         "ot_error_taken=%d\not_router_opens=%d\not_router_adds=%d\not_router_removes=%d\not_router_mappings=%zu\n",
                         o.changed_shown, o.reshare_opened, o.reshare_link_ok, o.reshare_no_secret, o.changed_acknowledged,
                         o.off_disables, o.mapping_removed, o.carrier_nat, o.carrier_invite_disabled, o.manual, o.manual_steps,
                         o.manual_field, o.manual_bad_refused, o.manual_entered, o.manual_unverified, o.manual_invite_enabled,
                         o.error_taken, o.router.opens, o.router.adds, o.router.removes, o.router.mappings);
            std::fprintf(f,
                         "ot_s_remove_enabled=%d\not_s_remove_disabled_off=%d\not_s_profile_hint=%d\not_s_profile_hint_off=%d\n"
                         "ot_invite_closed_reason=%d\not_remove_enabled=%d\not_remove_mapping=%d\not_remove_listening=%d\n"
                         "ot_lan_test_address=%d\not_port_retries=%d\n",
                         o.s_remove_enabled, o.s_remove_disabled_off, o.s_profile_hint, o.s_profile_hint_off,
                         o.invite_closed_reason, o.remove_enabled, o.remove_mapping, o.remove_listening, o.lan_test_address,
                         o.port_retries);
            std::fprintf(f, "ot_s_remap=%d\not_remap_enabled_removed=%d\not_recheck_kept_removed=%d\not_remapped=%d\n",
                         o.s_remap, o.remap_enabled_removed, o.recheck_kept_removed, o.remapped);
            std::fprintf(f, "ot_s_no_port_label=%d\n", o.s_no_port_label);
        }
#endif
        std::fclose(f);
    }

    // --test-second-display. Each step goes through the same code as the user's key or click.
    struct SecondTest {
        int ran = 0, opened_blacked = 0, took_focus = 0, click_through = 0, hit_through = 0, frame_clean = 0, tools_on_main = 0;
        int blackout_after_b = 0, esc_presses = 0, esc_stopped = 0, switch_cleared = 0, black_after_frames = 0;
        uint64_t frames = 0, black_presents = 0;
        size_t live_lit = 0, live_total = 0, black_lit = 0, black_total = 0;
    } second_test;
    void pressKey(UINT vk) {
        MSG m{};
        m.hwnd = main;
        m.message = WM_KEYDOWN;
        m.wParam = vk;
        handleShortcutMessage(m); // the main window's shortcut path, as a real key press
    }
    // At launch: B while the output is off, then open it: it opens blacked out (the Mac's), and B again
    // brings it back.
    void startSecondDisplayForTest() {
        pressKey('B');
        startOutput();
        second_test.opened_blacked = out.active() && out.blackout() && out.blackPresents() > 0 && out.framesShown() == 0;
        pressKey('B');
    }
    // At the capture (the screenshot hook itself was allowed; checked again here): the output live,
    // then B, then Esc until the output stops (the first Esc clears any tools, as the key does).
    void captureSecondDisplayForTest() {
        if (!lo.test_second_display || !out.active()) return;
        std::wstring why;
        if (!shotAllowed(&why)) return; // only the synthetic source, no colour, no replay
        SecondTest& t = second_test;
        t.ran = 1;
        const HWND w = out.window();
        t.took_focus = (GetForegroundWindow() == w || GetActiveWindow() == w || GetFocus() == w) ? 1 : 0;
        t.click_through = out.clickThrough() ? 1 : 0;
        RECT r{};
        GetWindowRect(w, &r);
        const POINT mid{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        t.hit_through = WindowFromPoint(mid) != w ? 1 : 0; // the mouse finds what's under it
        // The output holds the clean frame, never the copy with the region box and the ruler.
        const auto& f = shownFrame();
        const auto& m = shownMeta();
        t.tools_on_main = (region_on || ruler) ? 1 : 0;
        t.frame_clean = m.width > 0 && out.lastFrameHash() == frameHash(f.data(), m.width, m.height, m.stride_bytes) ? 1 : 0;
        if (t.tools_on_main && t.frame_clean) { // and the main window's copy really has them drawn in
            t.frame_clean = shown.size() >= static_cast<size_t>(m.stride_bytes) * m.height &&
                                    out.lastFrameHash() != frameHash(shown.data(), m.width, m.height, m.stride_bytes)
                                ? 1
                                : 0;
        }
        t.frames = out.framesShown();
        std::string err;
        DwmFlush();
        captureWindowToPng(w, lo.screenshot_path + L".second.png", &err, &t.live_lit, &t.live_total);
        pressKey('B');
        t.blackout_after_b = out.blackout() ? 1 : 0;
        // The black present goes out at once; DWM shows it at its next composition. Capture once per
        // composed frame until it's all black (at most 10), and report how many frames that took.
        for (t.black_after_frames = 1; t.black_after_frames <= 10; ++t.black_after_frames) {
            DwmFlush();
            captureWindowToPng(w, lo.screenshot_path + L".second-black.png", &err, &t.black_lit, &t.black_total);
            if (t.black_lit == 0) break;
        }
        t.black_presents = out.blackPresents();
        // A mode change (3: Depth) right before a capture: the old view's frame is gone at once, from the
        // main view and the output, and can't be shown again until the new view's frame arrives.
        const bool had = d3d.hasFrame();
        pressKey('3');
        t.switch_cleared = had && s.mode == KV_MODE_DEPTH && frame_stale && !d3d.hasFrame() && !out.hasFrame() ? 1 : 0;
        presentShown(); // (a tool redraw, say): still nothing of the old frame
        t.switch_cleared = t.switch_cleared && !d3d.hasFrame() && !out.hasFrame() ? 1 : 0;
        // Esc: the tools first (if any), then the output covering this display.
        while (out.active() && t.esc_presses < 3) {
            pressKey(VK_ESCAPE);
            ++t.esc_presses;
        }
        t.esc_stopped = out.active() ? 0 : 1;
    }

    // ---- the pet camera (protocol v3, W6; pet-camera.md §4) ----------------------------------------------
    PetCameraUi petui; // the Devices dialog and the sharing-schedule editor
    HWND btnPrivacy{}, chkShare{}, lblSchedule{}, btnSchedule{}, btnDevices{}, lblPet{}, lblWatching{};
    HWND lblPrivacyNote{}, lblShareNote{}, lblIdentity{}, btnIdRetry{}, btnIdReset{};
    bool identity_shown = false;    // the identity block is in place of the pet-camera controls
    std::wstring identity_message;  // what the last "Try again" or reset said, under the reason
    std::string last_reset_note;    // what the last reset did with the old pairing.json
    std::wstring watching_text; // the banner's text: empty only when no session holds a watcher slot
    std::wstring health_tier_line;
    bool pet_privacy_on = false;
    HBRUSH watching_brush = nullptr;
    int pet_ticks = 0;

    static std::wstring windowText(HWND h) {
        wchar_t b[512] = {0};
        if (h) GetWindowTextW(h, b, 512);
        return b;
    }
    static void setTextIfChanged(HWND h, const std::wstring& t) {
        wchar_t cur[512] = {0};
        GetWindowTextW(h, cur, 512);
        if (t != cur) SetWindowTextW(h, t.c_str());
    }

    // The panel, the banner and the open windows from the host's current state. full: a change was
    // reported (WM_APP_PET_STATE); otherwise the 250 ms status tick (the lists once a second).
    void refreshPet(bool full) {
        if (!hooks) return;
        refreshIdentity();
        const kvhost::PetSettings ps = hooks->petSettings();
        if (ps.privacyMode != pet_privacy_on || full) {
            pet_privacy_on = ps.privacyMode;
            if (btnPrivacy) {
                SetWindowTextW(btnPrivacy, pet_privacy_on ? L"Privacy mode: on" : L"Privacy mode: off"); // for screen readers
                InvalidateRect(btnPrivacy, nullptr, TRUE);
            }
        }
        if (chkShare) SendMessageW(chkShare, BM_SETCHECK, ps.sharingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        if (lblSchedule) {
            std::wstring t = L"Sharing schedule: " + scheduleSummary(ps.sharingSchedule);
            if (!ps.sharingSchedule.empty() && hooks->sharingScheduledOffNow()) t += L" (off now)";
            setTextIfChanged(lblSchedule, t);
        }
        const std::vector<kvhost::PetWatcher> watchers = hooks->watchingNow();
        if (lblPet) {
            const auto devs = hooks->petDevices();
            int connected = 0;
            for (const auto& d : devs) connected += d.connected ? 1 : 0;
            wchar_t b[300];
            swprintf(b, 300, L"%zu device(s) paired, %d connected, %zu watching.%ls", devs.size(), connected, watchers.size(),
                     ps.privacyMode ? L" Privacy mode: no video leaves this PC."
                                    : (ps.sharingEnabled ? L"" : L" Viewers can't watch: sharing is off."));
            // While this PC's identity can't be used the host is off: nothing above applies.
            setTextIfChanged(lblPet, identity_shown ? std::wstring(L"No device can connect or pair until this PC's identity can be used.") : std::wstring(b));
        }
        const std::wstring w = watchingNowText(watchers);
        if (w != watching_text) {
            watching_text = w;
            if (lblWatching) SetWindowTextW(lblWatching, (L"  " + w).c_str());
            layout();
        }
        if (full || ++pet_ticks % 4 == 0) petui.refreshDevices(); // connected, last seen
        if (full) petui.refreshSchedule();
        refreshRoll(full);
#ifdef KV_WITH_STREAMING
        refreshOutside();
#endif
    }

    // This PC's identity (pairing.json) is there but can't be used: the host failed closed and is off. The panel
    // says why and offers "Try again" and the reset, in place of the pet-camera controls.
    void refreshIdentity() {
        if (!hooks || !lblIdentity) return;
        const bool bad = hooks->hostIdentityUnusable();
        if (bad != identity_shown) {
            identity_shown = bad;
            for (HWND h : {btnPrivacy, lblPrivacyNote, chkShare, lblSchedule, btnSchedule, lblShareNote}) {
                ShowWindow(h, bad ? SW_HIDE : SW_SHOW);
            }
            for (HWND h : {lblIdentity, btnIdRetry, btnIdReset}) ShowWindow(h, bad ? SW_SHOW : SW_HIDE);
            if (!bad) identity_message.clear();
        }
        if (!bad) return;
        // The reason is the error's first sentence (what is wrong with the file, and its .bad copy).
        const std::string e = hooks->hostStartError();
        std::wstring t = L"Streaming is off: " + widen(e.substr(0, e.find(". ")).c_str()) +
                         L". No new identity was made, so no device is unpaired.";
        if (!identity_message.empty()) t += L"\n" + identity_message;
        setTextIfChanged(lblIdentity, t);
    }
    void onIdentityRetry() {
        if (!hooks) return;
        std::string err;
        identity_message = hooks->retryHostStart(&err) ? std::wstring() : L"Tried again: it still can't be used.";
        refreshPet(true);
        updateStatus();
        refreshEnabled();
    }
    // "Reset this PC's identity": ask is the confirm dialog (No is the default, as Revoke all's); the test hook
    // passes false, as it does past the other confirm dialogs. Nothing else makes a new identity.
    bool onIdentityReset(bool ask) {
        if (!hooks || !hooks->hostIdentityUnusable()) return false;
        if (ask && MessageBoxW(main,
                               L"Reset this PC's identity?\n\nThis PC gets a new identity. Every paired device must be paired "
                               L"again, and none can connect until it is. The current pairing.json is kept as "
                               L"pairing.json.bad.\n\nIf another program was only holding the file, use Try again instead.",
                               L"Reset this PC's identity", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return false;
        }
        std::string err, note;
        bool kept = false;
        const bool ok = hooks->resetHostIdentityAndStart(&err, &note, &kept);
        last_reset_note = note;
        identity_message = ok ? std::wstring() : widen(err.c_str());
        if (ask) {
            if (ok && kept) {
                MessageBoxW(main, L"pairing.json can be read now, so nothing was reset: this PC kept its identity, and "
                                  L"paired devices connect as before.",
                            L"Reset this PC's identity", MB_ICONINFORMATION);
            } else if (ok) {
                MessageBoxW(main, (L"This PC has a new identity. Pair each device again (Pair a device\u2026).\n\nThe old "
                                   L"pairing.json: " + widen(note.c_str()) + L".").c_str(),
                            L"Reset this PC's identity", MB_ICONINFORMATION);
            } else {
                MessageBoxW(main, widen(err.c_str()).c_str(), L"Reset this PC's identity", MB_ICONERROR);
            }
        }
        refreshPet(true);
        updateStatus();
        refreshEnabled();
        return ok;
    }

    // The large Privacy mode button: red with white text while it is on.
    void drawPrivacyButton(const DRAWITEMSTRUCT& di) {
        const bool on = pet_privacy_on;
        RECT r = di.rcItem;
        HBRUSH br = CreateSolidBrush(on ? RGB(176, 32, 32) : GetSysColor(COLOR_BTNFACE));
        FillRect(di.hDC, &r, br);
        DeleteObject(br);
        DrawEdge(di.hDC, &r, (di.itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
        if (di.itemState & ODS_FOCUS) {
            RECT f = r;
            InflateRect(&f, -S(4), -S(4));
            DrawFocusRect(di.hDC, &f);
        }
        SetBkMode(di.hDC, TRANSPARENT);
        SetTextColor(di.hDC, on ? RGB(255, 255, 255) : GetSysColor(COLOR_BTNTEXT));
        HGDIOBJ old = SelectObject(di.hDC, bold);
        const wchar_t* t = on ? L"PRIVACY MODE ON \u2014 click to turn off" : L"Privacy mode: off \u2014 click to stop all video";
        DrawTextW(di.hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(di.hDC, old);
    }

    // The controls' own handlers (the test hook goes through these too).
    void onPrivacyClicked() {
        if (!hooks) return;
        hooks->setPrivacyMode(!hooks->petSettings().privacyMode); // v3 §6.4: at once, every session; saved
        refreshPet(true);
    }
    void onShareClicked() {
        if (!hooks) return;
        hooks->setSharingEnabled(checked(chkShare));
        refreshPet(true);
    }
    void setShareFromPanel(bool on) { // as a click on the checkbox
        SendMessageW(chkShare, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        onCommand(ID_PET_SHARE, BN_CLICKED);
    }

    // ---- the camera roll (W7; pet-camera.md §4, §6) ----------------------------------------------------------------
    HWND chkRoll{}, lblRollStatus{}, btnRollRecord{}, btnRollStop{}, chkMotion{}, lblMotion{}, tbMotion{}, lblMotionNote{};
    HWND chkRecSched{}, btnRecSched{}, lblRecSched{}, lblRollFolder{}, btnRollFolder{}, edKeepDays{}, edKeepGb{}, btnKeepApply{};
    HWND lblRollStorage{}, btnRollBrowser{}, lblRollNote{}, lblD6Note{}, lblRollLimits{}, lblRollConfirm{}, btnRollConfirm{};
    uint64_t roll_storage_at = 0;   // when the storage line was last read (GetTickCount64)
    bool roll_storage_forced = true; // a change made here: read it at the next refresh
    uint64_t roll_storage_reads = 0; // for the --test-camera-roll report
    std::wstring roll_note;     // the last refusal, or what a folder change does
    int roll_ticks = 0;
    bool launch_is_test = false; // isTestLaunch at launch: the roll's folder comes from --camera-roll-dir only
#ifdef KV_WITH_STREAMING
    RollUi rollui; // the browser, its player and the scheduled-recording rows
#endif
    // A camera-roll clip is on screen: the player window, or the browser's thumbnails (pet-camera.md §6.6). The
    // screenshot hook refuses then.
    bool rollOnScreen() const {
#ifdef KV_WITH_STREAMING
        return rollui.playerOpen() || rollui.showingThumbnails();
#else
        return false;
#endif
    }
    // W8 (v3 §10): the invite window is open, and with it a live token (a test run masks it, and is refused all the
    // same).
    bool inviteOnScreen() const {
#ifdef KV_WITH_STREAMING
        return outsideui.inviteOpen();
#else
        return false;
#endif
    }
    // The screenshot hook's rule at every capture, the main window's and the second display's: the synthetic source,
    // no colour, no replay, no camera-roll clip and no invite on screen.
    bool shotAllowed(std::wstring* why) const { return screenshotAllowed(s, replaying, rollOnScreen(), inviteOnScreen(), why); }
#ifdef KV_WITH_STREAMING
    // A phone owner is deleting a clip. If the PC's player has it, the player closes first and lets go of
    // it (the host waits up to 2 s for that), and the panel says why it closed.
    void onRollClipDeletedByOwner(const std::string& clipId) {
        if (rollui.closePlayerForDeletedClip(clipId)) roll_note = L"This clip was deleted.";
        roll_storage_forced = true;
        refreshPet(true);
    }
#endif
    static std::wstring recordingsDirForRoll() {
#ifdef KV_WITH_RECORDER
        return kvrec::defaultRecordingsDir();
#else
        return knownRecordingsDir();
#endif
    }

    // The roll's status line: recording or not, the trigger, and the v3 §9 sentence for what blocks it; or why
    // there is no roll at all (rollError: a folder that was refused, a test run without --camera-roll-dir).
    std::wstring rollStatusLine(bool avail, const kvhost::RollRecorderStatus& st) const {
        if (!avail) return L"No camera roll: " + widen(hooks->rollError().c_str());
        // The roll stopped (in a test run, a frame that isn't the synthetic source's; or a folder that couldn't
        // be prepared): the host's reason.
        const std::string stopped = hooks->rollError();
        if (!stopped.empty()) return L"Camera roll stopped: " + widen(stopped.c_str());
        // The folder is being prepared (roll_preparing, "The camera roll is getting ready.").
        if (hooks->rollPreparing()) return L"Preparing the camera roll\u2026";
        // Not prepared and not being prepared: the roll is off, so the folder isn't made yet.
        if (!st.ready) return L"Not recording: " + rollBlockText(st.block == kvhost::RollBlock::None ? kvhost::RollBlock::RollDisabled : st.block);
        std::wstring t;
        if (st.recording) {
            t = L"Recording a " + (st.clipTrigger ? clipTriggerLabel(*st.clipTrigger) : std::wstring(L"?")) + L" clip, " +
                clipDurationText(st.clipElapsedSec) + L".";
        } else if (st.block != kvhost::RollBlock::None) {
            t = L"Not recording: " + rollBlockText(st.block);
        } else {
            t = L"Ready. Not recording now.";
        }
        if (!st.lastError.empty()) t += L"\nLast problem: " + widen(st.lastError.c_str());
        return t;
    }
    std::wstring rollFolderText(const kvhost::PetSettings& ps) const {
        const std::wstring now = hooks->rollDir();
        std::wstring t = now.empty() ? std::wstring(L"None") : now;
        if (launch_is_test) return t + L" (test run: --camera-roll-dir)";
        // The setting a "Change..." saved: it applies at the next start.
        const std::wstring next = cameraRollDirForLaunch(false, L"", ps.rollFolder);
        if (_wcsicmp(next.c_str(), now.c_str()) != 0) t += L"\nAfter a restart: " + next;
        return t;
    }

    void refreshRoll(bool full) {
        if (!hooks || !chkRoll) return;
        const kvhost::PetSettings ps = hooks->petSettings();
        const bool avail = hooks->rollAvailable();
        const kvhost::RollRecorderStatus st = hooks->rollStatus();
        SendMessageW(chkRoll, BM_SETCHECK, ps.rollEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        setTextIfChanged(lblRollStatus, rollStatusLine(avail, st));
        EnableWindow(btnRollRecord, avail && st.ready && st.block == kvhost::RollBlock::None);
        EnableWindow(btnRollStop, avail && st.manualActive);
        EnableWindow(btnRollBrowser, avail);
        // Motion (by depth): greyed out, with the reason, while it can't work (no live sensor, a replay; §6.2).
        const std::wstring why = avail ? motionUnavailableText(replaying, running, st.motionAvailable)
                                       : std::wstring(L"No camera roll on this run.");
        SendMessageW(chkMotion, BM_SETCHECK, ps.motionEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(chkMotion, why.empty());
        EnableWindow(tbMotion, why.empty());
        if (GetCapture() != tbMotion) SendMessageW(tbMotion, TBM_SETPOS, TRUE, ps.motionSensitivity);
        setTextIfChanged(lblMotion, L"Sensitivity " + std::to_wstring(ps.motionSensitivity) + L" of 10");
        setTextIfChanged(lblMotionNote, why);
        SendMessageW(chkRecSched, BM_SETCHECK, ps.recordScheduleEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        setTextIfChanged(lblRecSched, recordScheduleText(ps.recordSchedule));
        setTextIfChanged(lblRollFolder, rollFolderText(ps));
        if (full && GetFocus() != edKeepDays && GetFocus() != edKeepGb) {
            setTextIfChanged(edKeepDays, std::to_wstring(ps.rollKeepDays));
            setTextIfChanged(edKeepGb, std::to_wstring((ps.rollCapBytes + 500000000ULL) / 1000000000ULL));
        }
        // The limits retention keeps to, next to the switch; Confirm while they aren't the owner's yet.
        setTextIfChanged(lblRollLimits, rollLimitsText(ps.rollKeepDays, ps.rollCapBytes));
        const bool confirm = avail && !hooks->rollLimitsKnown();
        if (confirm != (IsWindowVisible(btnRollConfirm) != FALSE)) {
            ShowWindow(lblRollConfirm, confirm ? SW_SHOW : SW_HIDE);
            ShowWindow(btnRollConfirm, confirm ? SW_SHOW : SW_HIDE);
        }
        // The storage line is the host's cached counts (rollStorageSnapshot: no walk of the folder, no wait on
        // the store's lock), read at most every 4 s however often the host reports a change, and at once after a
        // change made here (Keep, a star, a delete, a clip finalized).
        const uint64_t now = GetTickCount64();
        if (rollStorageRefreshDue(now, roll_storage_at, roll_storage_forced)) {
            roll_storage_at = now;
            roll_storage_forced = false;
            ++roll_storage_reads;
            const auto info = hooks->rollStorageSnapshot();
            setTextIfChanged(lblRollStorage, info ? rollStorageText(info->usedBytes, info->capBytes, info->freeBytes,
                                                                    info->floorBytes) + L" Starred clips are kept."
                                                  : std::wstring());
        }
        setTextIfChanged(lblRollNote, roll_note);
        // v3 §2.3, D6: the note shows exactly when the host skips "turn the Kinect off": the roll is on and
        // usable (the host's own predicate).
        setTextIfChanged(lblD6Note, hooks->turnOffSkippedForRoll() ? std::wstring(L"Skipped while the camera roll is on: motion "
                                                                                  L"and scheduled clips need the Kinect on.")
                                                                   : std::wstring());
        motion_feed_wanted = avail && ps.rollEnabled && ps.motionEnabled;
    }

    // A roll setting the host refused: pet-camera.json couldn't be read (the host's saveError says so), as the other
    // pet-camera controls are refused then (pet-camera.md §5), or a value out of range.
    void rollRefused(const std::wstring& what) {
        const std::string e = hooks ? hooks->petSaveError() : std::string();
        roll_note = what + L" wasn't changed: " + (e.empty() ? std::wstring(L"that value is out of range.") : widen(e.c_str()));
    }
    void onRollEnableClicked() {
        if (!hooks) return;
        // Turning it on writes the limits shown beside it (the host does), which makes them the owner's.
        if (hooks->setRollEnabled(checked(chkRoll))) roll_note.clear();
        else rollRefused(L"Record to the camera roll");
        roll_storage_forced = true;
        refreshPet(true);
    }
    // Confirm writes the limits shown (the current settings) as the owner's, so retention runs with them.
    void onRollConfirmLimits() {
        if (!hooks) return;
        std::string err;
        if (hooks->confirmRollLimits(&err)) roll_note.clear();
        else roll_note = L"The limits weren't confirmed: " + widen((err.empty() ? std::string("they couldn't be saved.") : err).c_str());
        roll_storage_forced = true;
        refreshPet(true);
    }
    void onMotionClicked() {
        if (!hooks) return;
        if (hooks->setMotionSettings(checked(chkMotion), hooks->petSettings().motionSensitivity)) roll_note.clear();
        else rollRefused(L"Motion");
        refreshPet(true);
    }
    void onMotionSensitivity() {
        if (!hooks) return;
        const int v = static_cast<int>(SendMessageW(tbMotion, TBM_GETPOS, 0, 0));
        const kvhost::PetSettings ps = hooks->petSettings();
        if (v == ps.motionSensitivity) return;
        if (hooks->setMotionSettings(ps.motionEnabled, v)) roll_note.clear();
        else rollRefused(L"The motion sensitivity");
        refreshPet(true);
    }
    void onRecSchedClicked() {
        if (!hooks) return;
        if (hooks->setRecordSchedule(hooks->petSettings().recordSchedule, checked(chkRecSched))) roll_note.clear();
        else rollRefused(L"Scheduled recording");
        refreshPet(true);
    }
    void onKeepApply() {
        if (!hooks) return;
        wchar_t d[16] = {0}, g[16] = {0};
        GetWindowTextW(edKeepDays, d, 16);
        GetWindowTextW(edKeepGb, g, 16);
        const int days = _wtoi(d), gb = _wtoi(g);
        if (days < kvhost::kRollMinKeepDays || days > kvhost::kRollMaxKeepDays || gb < 1) {
            roll_note = L"Keep: 1 to " + std::to_wstring(kvhost::kRollMaxKeepDays) + L" days, and at least 1 GB.";
        } else if (hooks->setRollRetention(days, static_cast<uint64_t>(gb) * 1000000000ULL)) {
            roll_note.clear();
        } else {
            rollRefused(L"Keep");
        }
        roll_storage_forced = true;
        refreshPet(true);
    }
    void onRollRecord() {
        if (!hooks) return;
        const kvhost::StartClipResult r = hooks->startManualClip(); // startedBy "This PC"
        roll_note = r.ok ? std::wstring() : L"Not recording: " + rollBlockText(r.refused);
        refreshPet(true);
    }
    void onRollStop() {
        if (!hooks) return;
        roll_note = hooks->stopManualClip() == kvhost::StopClipResult::Busy
                        ? std::wstring(L"This clip was started by motion or the schedule: Stop ends only a clip started "
                                       L"with Record a clip.")
                        : std::wstring();
        refreshPet(true);
    }
    // "Change..." (pet-camera.md §4, §6.5): refuses OneDrive, the depth/IR recordings folder and network drives by the
    // path alone, then saves the setting. The new folder applies after a restart (the host
    // prepares it then), so the PC says so. ask: message boxes (the test hook passes false).
    bool changeRollFolder(const std::wstring& picked, bool ask) {
        if (!hooks) return false;
        const std::wstring dir = normalizeStateDir(picked);
        const std::wstring root = dir.size() >= 3 ? dir.substr(0, 3) : dir;
        std::wstring why;
        if (!rollFolderChoiceAcceptable(dir, recordingsDirForRoll(), oneDriveRootsFromEnvironment(), GetDriveTypeW(root.c_str()),
                                        &why)) {
            roll_note = why;
            if (ask) MessageBoxW(main, why.c_str(), L"Camera roll folder", MB_ICONWARNING);
            refreshPet(true);
            return false;
        }
        if (!hooks->setRollFolderSetting(narrow(dir))) {
            rollRefused(L"The camera roll's folder");
            if (ask) MessageBoxW(main, roll_note.c_str(), L"Camera roll folder", MB_ICONWARNING);
            refreshPet(true);
            return false;
        }
        const std::wstring now = hooks->rollDir();
        roll_note = L"The camera roll's folder is now " + dir + L". It applies after a restart of this app" +
                    (now.empty() ? std::wstring(L".") : L"; until then clips go to " + now + L".");
        if (launch_is_test) roll_note += L" (A test run records only into --camera-roll-dir.)";
        if (ask) MessageBoxW(main, roll_note.c_str(), L"Camera roll folder", MB_ICONINFORMATION);
        refreshPet(true);
        return true;
    }
    std::optional<std::wstring> pickFolder() {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
            return std::nullopt;
        }
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Choose the camera roll's folder");
        std::optional<std::wstring> out;
        if (SUCCEEDED(dlg->Show(main))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out = std::wstring(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
        return out;
    }
    void onRollFolderChange() {
        if (const auto dir = pickFolder()) changeRollFolder(*dir, true);
    }

#ifdef KV_WITH_STREAMING
    // ---- outside the home (W8; pet-camera.md §4, v3 §8) ------------------------------------------------------------
    OutsideUi outsideui; // the invite, re-share and connection log windows, and the role and port prompts
    HWND chkOutside{}, lblOutsideReason{}, lblOutsideStatus{}, lblOutsidePort{}, btnOutsidePort{}, lblOutsideEndpoint{};
    HWND lblOutsideRouter{};
    HWND lblOutsideManual{}, lblOutsideAddr{}, edOutsideAddr{}, btnOutsideAddrSave{}, lblOutsideChanged{}, btnOutsideReshare{};
    HWND btnOutsideInvite{}, btnOutsideLog{}, lblOutsideInvite{}, lblOutsideNote{}, btnOutsideRemove{}, lblOutsideProfile{};
    HWND btnOutsideRemap{};
    std::wstring outside_note;         // the last refusal, or what a change did
    std::wstring outside_addr_applied; // the public address last put in the field from the host
    // --test-outside only: a whole input the panel shows instead of the host's (its scripted pass), and the
    // address-changed status laid over the host's real view until the re-share window acknowledges it.
    std::optional<OutsidePanelInput> outside_input_override;
    int outside_ack_mark = 0;

    static bool ctlShown(HWND h) { return h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0; }
    static bool ctlEnabled(HWND h) { return h && IsWindowEnabled(h) != FALSE; }
    static void showIf(HWND h, bool on) {
        if (h && ctlShown(h) != on) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    }

    OutsidePanelInput outsideInput() const {
        if (outside_input_override) return *outside_input_override;
        OutsidePanelInput in;
        in.hostHasOutside = hooks->outsideAvailable();
        in.hostRunning = !hooks->hostIdentityUnusable() && hooks->hostStartError().empty();
        in.view = hooks->outsideView();
        in.sharingOn = hooks->petSettings().sharingEnabled;
        in.saveError = hooks->petSaveError();
        in.networkPublic = outsideui.networkPublic(); // pet-camera.md §7, read off the UI thread
        return in;
    }

    // The section from the host's view (outsidePanelState decides every control; this only applies it).
    void refreshOutside() {
        if (!hooks || !chkOutside) return;
        const OutsidePanelState st = outsidePanelState(outsideInput());
        SendMessageW(chkOutside, BM_SETCHECK, st.allowChecked ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(chkOutside, st.allowEnabled);
        setTextIfChanged(lblOutsideReason, st.allowReason);
        setTextIfChanged(lblOutsideStatus, st.statusLine);
        setTextIfChanged(lblOutsidePort, st.portText);
        EnableWindow(btnOutsidePort, st.portEnabled);
        setTextIfChanged(lblOutsideEndpoint, st.endpointText);
        showIf(lblOutsideRouter, !st.routerText.empty());
        setTextIfChanged(lblOutsideRouter, st.routerText);
        showIf(lblOutsideManual, st.showManualSteps);
        setTextIfChanged(lblOutsideManual, st.manualSteps);
        for (HWND h : {lblOutsideAddr, edOutsideAddr, btnOutsideAddrSave}) showIf(h, st.showPublicAddress);
        EnableWindow(edOutsideAddr, st.publicAddressEnabled);
        EnableWindow(btnOutsideAddrSave, st.publicAddressEnabled);
        // The field follows the host's value, but never while the PC user is typing in it.
        if (GetFocus() != edOutsideAddr && st.publicAddressValue != outside_addr_applied) {
            setTextIfChanged(edOutsideAddr, st.publicAddressValue);
            outside_addr_applied = st.publicAddressValue;
        }
        showIf(lblOutsideChanged, st.showAddressChanged);
        showIf(btnOutsideReshare, st.showAddressChanged);
        EnableWindow(btnOutsideReshare, st.reshareEnabled);
        EnableWindow(btnOutsideInvite, st.inviteEnabled);
        EnableWindow(btnOutsideLog, st.logEnabled);
        EnableWindow(btnOutsideRemove, st.removeMappingEnabled);
        EnableWindow(btnOutsideRemap, st.remapEnabled);
        showIf(lblOutsideProfile, !st.profileHint.empty());
        setTextIfChanged(lblOutsideProfile, st.profileHint);
        // Under the buttons: an open invite and its countdown, or why invites can't be made now.
        std::wstring inv = st.inviteReason;
        if (!outside_input_override) {
            if (const auto cur = hooks->currentOutsideInvite()) {
                inv = L"An invite is open. " + inviteCountdownText(cur->expiresUnix, unixMsNow() / 1000) + L".";
            }
        }
        setTextIfChanged(lblOutsideInvite, inv);
        setTextIfChanged(lblOutsideNote, outside_note);
    }

    // The controls' own handlers (the test hook goes through these too, past the prompts).
    void onOutsideAllowClicked() {
        if (!hooks) return;
        const bool on = checked(chkOutside);
        std::string err;
        if (hooks->setAllowOutside(on, &err)) outside_note.clear();
        else outside_note = std::wstring(on ? L"Not turned on: " : L"Not turned off: ") + widen(err.c_str());
        refreshPet(true);
    }
    bool changeOutsidePort(uint16_t port) {
        if (!hooks) return false;
        std::string err;
        const bool ok = hooks->setOutsidePort(port, &err);
        outside_note = ok ? std::wstring() : L"The port wasn't changed: " + widen(err.c_str());
        refreshPet(true);
        return ok;
    }
    void onOutsidePort() {
        if (!hooks) return;
        if (const auto port = outsideui.askPort(main, hooks->outsideView().port)) changeOutsidePort(*port);
    }
    bool onOutsideAddressSave() {
        if (!hooks) return false;
        std::wstring text = windowText(edOutsideAddr);
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
        std::string err;
        const bool ok = hooks->setOutsidePublicAddress(narrow(text), &err);
        if (ok) outside_note = text.empty() ? std::wstring(L"Public address cleared.") : std::wstring();
        else outside_note = L"The public address wasn't saved: " + widen(err.c_str());
        outside_addr_applied = L"\x1"; // the field takes the host's value again at once (a refusal puts the old one back)
        SetFocus(main);
        refreshPet(true);
        return ok;
    }
    void onOutsideReshare() {
        std::string err;
        outside_ack_mark = outsideui.acknowledgedCount();
        if (!outsideui.openReshare(true, &err)) outside_note = widen(err.c_str());
        refreshPet(true);
    }
    void onOutsideRemoveMapping() {
        if (!hooks) return;
        std::string err;
        if (hooks->removeOutsideMappingNow(&err)) {
            outside_note = L"The port forward is being removed from the router. It stays removed, and devices outside the "
                           L"home can't reach this PC, until you press \u201cMap the port again\u201d or turn this off and on.";
        } else {
            outside_note = L"Nothing was removed: " + widen(err.c_str());
        }
        refreshPet(true);
    }
    void onOutsideRemap() {
        if (!hooks) return;
        std::string err;
        if (hooks->remapOutsideNow(&err)) outside_note = L"The port is being forwarded on the router again.";
        else outside_note = L"The port wasn't mapped again: " + widen(err.c_str());
        refreshPet(true);
    }
    void onOutsideInvite() {
        if (const auto role = outsideui.askInviteRole(main)) {
            std::string err;
            if (!outsideui.openInvite(*role, &err)) outside_note = L"No invite was made: " + widen(err.c_str());
            else outside_note.clear();
        }
        refreshPet(true);
    }

    // --test-outside (synthetic, a --state-dir of the test's own, loopback only): the section and its windows, each
    // through the panel's or the window's own code, past the prompts. First a scripted pass: the panel is handed a
    // view for every status and checked control by control. Then, when this build's host has outside listening, the
    // real one against the viewer's fake router (Options::router_factory): mapped, an invite shown and cancelled
    // and one the host closes, the connection log, the address-changed flow and re-share (the status laid over the
    // real view: the host re-reads the address only every 10 minutes), off removing the mapping, carrier NAT,
    // manual with a refused and an accepted public address, and a port another machine holds. No token is written
    // to the report: only that the invite was shown, masked, and its length.
    struct OutsideTest {
        int ran = 0;
        int step = 0;
        uint64_t deadline = 0, hold_until = 0;
        bool done = false;
        std::string failed;
        int host_interface = 0, router_injected = 0;
        // The scripted pass.
        int s_off = 0, s_sharing_off = 0, s_save_error = 0, s_mapped = 0, s_manual_field = 0, s_manual_entered = 0;
        int s_error = 0, s_carrier = 0, s_address_changed = 0, s_checking = 0;
        // The windows in every build, on made-up links (no real token): the invite window (masked, its countdown,
        // its warning, the screenshot refusal, and closing itself when the host has no such invite), the re-share
        // window acknowledging on Done, and the connection log window.
        int w_invite_shown = 0, w_invite_masked = 0, w_invite_countdown = 0, w_invite_warning = 0, w_invite_role = 0;
        int w_shot_refused = 0, w_self_closed = 0, w_shot_allowed_after = 0, w_reshare_shown = 0, w_reshare_acknowledged = 0;
        int w_log_opened = 0;
        // Against the host and the fake router.
        int off_at_start = 0, sharing_off_reason = 0, mapped = 0, mapped_endpoint = 0, mapped_invite_enabled = 0;
        int invite_shown = 0, invite_link_ok = 0, invite_masked = 0, invite_token_hidden = 0, invite_countdown = 0;
        int invite_capture_excluded = 0;
        int invite_warning = 0, invite_role_viewer = 0, invite_still_open = 0, shot_refused_invite = 0, invite_cancelled = 0;
        int invite_window_gone = 0, shot_allowed_after_invite = 0, invite_role_owner = 0, invite_self_closed = 0;
        size_t invite_link_length = 0;
        int log_opened = 0, log_rows = -1;
        int changed_shown = 0, reshare_opened = 0, reshare_link_ok = 0, reshare_no_secret = 0, changed_acknowledged = 0;
        int off_disables = 0, mapping_removed = 0, carrier_nat = 0, carrier_invite_disabled = 0;
        int manual = 0, manual_steps = 0, manual_field = 0, manual_bad_refused = 0, manual_entered = 0, manual_unverified = 0;
        int manual_invite_enabled = 0, error_taken = 0;
        // The invite window's sentence, Remove mapping now, the Public-profile hint.
        int s_remove_enabled = 0, s_remove_disabled_off = 0, s_profile_hint = 0, s_profile_hint_off = 0;
        int invite_closed_reason = 0, remove_enabled = 0, remove_mapping = 0, remove_listening = 0;
        int lan_test_address = 0;
        // Map the port again, in a made-up view and against the host.
        int s_remap = 0;
        int s_no_port_label = 0; // the port label during the no-port error
        int remap_enabled_removed = 0, recheck_kept_removed = 0, remapped = 0;
        int adds_at_remove = 0;
        uint64_t rechecks_mark = 0;
        uint16_t port_a = 0;
        int port_retries = 0; // the random default port was in use on this PC: another one was tried
        TestRouterCounts router;
        bool main_captured = false, main_ok = false;
        std::string main_err;
    } outside_test;

    void outsideTestNext(int step, int timeout_ms) {
        outside_test.step = step;
        outside_test.deadline = GetTickCount64() + static_cast<uint64_t>(timeout_ms);
    }
    // Only the switch and the log may be enabled, and none of the status rows shows.
    bool outsideAllOff() {
        return !ctlEnabled(btnOutsidePort) && !ctlEnabled(btnOutsideInvite) && !ctlEnabled(edOutsideAddr) &&
               !ctlEnabled(btnOutsideAddrSave) && !ctlEnabled(btnOutsideReshare) && !ctlShown(lblOutsideManual) &&
               !ctlShown(edOutsideAddr) && !ctlShown(lblOutsideChanged) && !windowText(lblOutsideInvite).empty();
    }

    void outsideScriptedPass() {
        OutsideTest& t = outside_test;
        using St = kvhost::outside::OutsideStatus;
        auto apply = [&](const OutsidePanelInput& in) {
            outside_input_override = in;
            refreshOutside();
        };
        OutsidePanelInput base;
        base.hostRunning = true;
        base.hostHasOutside = true;
        base.sharingOn = true;
        auto view = [](St st) {
            OutsidePanelView v;
            v.allowed = true;
            v.listening = true;
            v.status = st;
            v.port = 43210;
            v.lanIPv4 = "192.168.1.20";
            return v;
        };
        {
            OutsidePanelInput in = base;
            in.view.port = 43210;
            in.networkPublic = true; // the hint is for outside viewing only: off shows none
            apply(in);
            t.s_off = ctlEnabled(chkOutside) && !checked(chkOutside) && outsideAllOff() ? 1 : 0;
            t.s_remove_disabled_off = !ctlEnabled(btnOutsideRemove) && !ctlShown(lblOutsideProfile) ? 1 : 0;
        }
        {
            OutsidePanelInput in = base;
            in.sharingOn = false;
            apply(in);
            t.s_sharing_off = !ctlEnabled(chkOutside) && windowText(lblOutsideReason).find(L"Share the camera") != std::wstring::npos &&
                                      outsideAllOff()
                                  ? 1
                                  : 0;
        }
        {
            OutsidePanelInput in = base;
            in.saveError = "pet-camera.json can't be read";
            apply(in);
            t.s_save_error = !ctlEnabled(chkOutside) && windowText(lblOutsideReason).find(L"can't be read") != std::wstring::npos ? 1 : 0;
        }
        {
            OutsidePanelInput in = base;
            in.view = view(St::Mapped);
            in.view.sentence = "Devices outside the home can connect at 203.0.113.160:43210.";
            in.view.wanAddress = in.view.endpointHost = "203.0.113.160";
            in.view.endpointKnown = in.view.endpointVerified = in.view.canInvite = true;
            apply(in);
            t.s_mapped = checked(chkOutside) && windowText(lblOutsideStatus) == widen(in.view.sentence.c_str()) &&
                                 windowText(lblOutsideEndpoint) == L"Outside address: 203.0.113.160:43210" && ctlEnabled(btnOutsideInvite) &&
                                 ctlEnabled(btnOutsidePort) && !ctlShown(lblOutsideManual) && !ctlShown(edOutsideAddr) && !ctlShown(lblOutsideChanged)
                             ? 1
                             : 0;
            t.s_remove_enabled = ctlEnabled(btnOutsideRemove) ? 1 : 0;
            const bool remap_off_while_mapped = !ctlEnabled(btnOutsideRemap);
            // A forward the PC user removed: manual, "Map the port again" on, "Remove mapping now" off.
            {
                OutsidePanelInput rm = base;
                rm.view = view(St::Manual);
                rm.view.mappingRemoved = true;
                rm.view.sentence = "The port forward was removed from the router. Forward TCP port 43210 to 192.168.1.20 in your "
                                   "router's settings, then type your public address here if you know it.";
                apply(rm);
                t.s_remap = remap_off_while_mapped && ctlEnabled(btnOutsideRemap) && !ctlEnabled(btnOutsideRemove) &&
                                    ctlShown(lblOutsideManual)
                                ? 1
                                : 0;
                apply(in);
            }
            // pet-camera.md §7: a made-up Public category (the test seam: the panel's input), never the PC's own.
            in.networkPublic = true;
            apply(in);
            t.s_profile_hint = ctlShown(lblOutsideProfile) && windowText(lblOutsideProfile) == kOutsidePublicNetworkHint ? 1 : 0;
            in.networkPublic = false;
            apply(in);
            t.s_profile_hint_off = !ctlShown(lblOutsideProfile) ? 1 : 0;
        }
        {
            OutsidePanelInput in = base;
            in.view = view(St::Manual);
            in.view.sentence = "No router that takes automatic port forwarding (UPnP) was found.";
            in.view.manualSteps = "Forward TCP 43210 to 192.168.1.20 in your router's settings.";
            in.view.cannotInviteReason = "Invites need the public address: type it in below.";
            apply(in);
            t.s_manual_field = windowText(lblOutsideStatus) == widen(in.view.sentence.c_str()) && ctlShown(lblOutsideManual) &&
                                       windowText(lblOutsideManual) == widen(in.view.manualSteps.c_str()) && ctlShown(lblOutsideAddr) &&
                                       windowText(lblOutsideAddr) == kOutsidePublicAddressLabel && ctlShown(edOutsideAddr) &&
                                       ctlEnabled(edOutsideAddr) && ctlEnabled(btnOutsideAddrSave) && !ctlEnabled(btnOutsideInvite) &&
                                       windowText(lblOutsideInvite) == widen(in.view.cannotInviteReason.c_str())
                                   ? 1
                                   : 0;
            in.view.publicAddressEntered = in.view.endpointHost = "203.0.113.161";
            in.view.endpointKnown = in.view.canInvite = true;
            in.view.cannotInviteReason.clear();
            apply(in);
            t.s_manual_entered = windowText(lblOutsideEndpoint) == L"Outside address: 203.0.113.161:43210 (entered by you, not checked)" &&
                                         windowText(edOutsideAddr) == L"203.0.113.161" && ctlEnabled(btnOutsideInvite)
                                     ? 1
                                     : 0;
        }
        {
            OutsidePanelInput in = base;
            in.view = view(St::Error);
            in.view.sentence = "That port is already in use on the router. Pick another port.";
            apply(in);
            t.s_error = windowText(lblOutsideStatus) == widen(in.view.sentence.c_str()) && !ctlEnabled(btnOutsideInvite) &&
                                ctlEnabled(btnOutsidePort) && !ctlShown(lblOutsideManual)
                            ? 1
                            : 0;
        }
        {
            // Turned on and no port binds (v3 §8.7): the port label
            // says none could be opened, and Change port... is how to enter one.
            OutsidePanelInput in = base;
            in.view = view(St::Error);
            in.view.listening = false;
            in.view.port = 0;
            in.view.sentence = "Couldn't find a port this PC can open for viewing from outside the home. Enter a port below.";
            apply(in);
            t.s_no_port_label = windowText(lblOutsidePort) == L"Port: none could be opened, so enter one below" &&
                                        windowText(lblOutsideStatus) == widen(in.view.sentence.c_str()) && ctlEnabled(btnOutsidePort)
                                    ? 1
                                    : 0;
        }
        {
            OutsidePanelInput in = base;
            in.view = view(St::CarrierNat);
            in.view.wanAddress = "100.64.1.2";
            in.view.sentence = "Your router's internet address is 100.64.1.2, which isn't a public address. Your internet "
                               "provider is using carrier NAT, or there's a second router in front of this one, so devices "
                               "outside your home can't reach this PC. It can't work without a relay, and this app doesn't use one.";
            apply(in);
            t.s_carrier = windowText(lblOutsideStatus) == widen(in.view.sentence.c_str()) && !ctlEnabled(btnOutsideInvite) &&
                                  !windowText(lblOutsideInvite).empty() && windowText(lblOutsideEndpoint).empty()
                              ? 1
                              : 0;
        }
        {
            OutsidePanelInput in = base;
            in.view = view(St::AddressChanged);
            in.view.wanAddress = in.view.endpointHost = "203.0.113.170";
            in.view.endpointKnown = in.view.endpointVerified = in.view.canInvite = true;
            apply(in);
            t.s_address_changed = ctlShown(lblOutsideChanged) && windowText(lblOutsideChanged) == kOutsideAddressChangedText &&
                                          ctlShown(btnOutsideReshare) && ctlEnabled(btnOutsideReshare) && !ctlEnabled(btnOutsideInvite)
                                      ? 1
                                      : 0;
        }
        {
            OutsidePanelInput in = base;
            in.view = view(St::Mapped);
            in.view.checking = true;
            apply(in);
            t.s_checking = windowText(lblOutsideStatus) == kOutsideCheckingText && !ctlEnabled(btnOutsideInvite) ? 1 : 0;
        }
        outside_input_override.reset();
        outside_addr_applied = L"\x1";
        refreshOutside();
    }

    void outsideTestStart() {
        OutsideTest& t = outside_test;
        t.ran = 1;
        t.host_interface = hooks->outsideAvailable() ? 1 : 0;
        t.router_injected = hooks->testRouterInjected() ? 1 : 0;
        outsideScriptedPass();
        refreshPet(true);
        // What a fresh state shows: off, and the switch waits for sharing.
        t.off_at_start = !checked(chkOutside) && outsideAllOff() ? 1 : 0;
        // The invite window on a made-up invite (v3 §8.4's shape; its token is no one's). The host has no such
        // invite, so the window's own timer must close it.
        OutsideInvitePanelView fake;
        fake.expiresUnix = unixMsNow() / 1000 + 600;
        fake.uri = "kv2://invite?v=3&s=testservice&fp=ZmFrZWZpbmdlcnByaW50&tok=TESTTOKENNOTREAL000000&h=203.0.113.160&p=43210"
                   "&lh=192.168.77.10&exp=" + std::to_string(fake.expiresUnix) + "&r=owner";
        fake.role = kvhost::PetRole::Owner;
        outsideui.showInvite(fake);
        std::wstring why;
        const std::string shown_link = narrow(outsideui.inviteShownLink());
        t.w_invite_shown = outsideui.inviteOpen() ? 1 : 0;
        t.w_invite_masked = outsideui.inviteMasked() && shown_link == maskInviteToken(fake.uri) &&
                                    shown_link.find("TESTTOKENNOTREAL") == std::string::npos
                                ? 1
                                : 0;
        const std::wstring cd = outsideui.inviteCountdown();
        t.w_invite_countdown = cd == L"Expires in 10:00" || cd == L"Expires in 9:59" ? 1 : 0;
        t.w_invite_warning = outsideui.inviteWarning() == kOutsideInviteWarning ? 1 : 0;
        t.w_invite_role = outsideui.inviteRoleLine() == inviteRoleText(kvhost::PetRole::Owner) ? 1 : 0;
        t.w_shot_refused = shotAllowed(&why) ? 0 : 1;
        outsideTestNext(10, 5000);
    }

    // The re-share and connection log windows on made-up data, then the host and the fake router (or the end).
    void outsideTestWindowsThenHost() {
        OutsideTest& t = outside_test;
        std::wstring why;
        t.w_self_closed = 1;
        t.w_shot_allowed_after = shotAllowed(&why) ? 1 : 0;
        const int acks = outsideui.acknowledgedCount();
        // A whole endpoint link (a 32-byte made-up fingerprint), so closing the window acknowledges its host.
        outsideui.showReshare("kv2://endpoint?v=3&s=testservice&fp=ZmFrZWZha2VmYWtlZmFrZWZha2VmYWtlZmFrZWZha2U&h=203.0.113.160&p=43210",
                              true);
        t.w_reshare_shown = outsideui.reshareWindow() && outsideui.reshareLink().rfind("kv2://endpoint?", 0) == 0 ? 1 : 0;
        outsideui.closeReshare();
        // v3 §8.3: the acknowledgement carries the address the PC user was shown, and only that one.
        t.w_reshare_acknowledged =
            !outsideui.reshareWindow() && outsideui.acknowledgedCount() == acks + 1 && outsideui.acknowledgedHost() == "203.0.113.160"
                ? 1
                : 0;
        onCommand(ID_OUTSIDE_LOG, BN_CLICKED);
        t.w_log_opened = outsideui.logWindow() ? 1 : 0;
        if (outsideui.logWindow()) DestroyWindow(outsideui.logWindow());
        refreshPet(true);
        if (!t.host_interface) {
            // A host without the outside interface: the switch must say why it's off. Not reached in this tree (the
            // host always has it, and viewer_outside.cmake requires host_interface=1); kept as the hook's guard.
            t.sharing_off_reason = !ctlEnabled(chkOutside) && !windowText(lblOutsideReason).empty() ? 1 : 0;
            outsideTestFinish();
            return;
        }
        if (!t.router_injected) {
            t.failed = "the host didn't take the fake router; nothing here may reach a real one";
            outsideTestFinish();
            return;
        }
        if (hooks->petSettings().sharingEnabled) setShareFromPanel(false);
        t.sharing_off_reason = !ctlEnabled(chkOutside) && windowText(lblOutsideReason).find(L"Share the camera") != std::wstring::npos ? 1 : 0;
        TestRouterScript r; // a UPnP router with a public WAN address
        hooks->setTestRouter(r);
        setShareFromPanel(true);
        setChecked(chkOutside, true);
        onCommand(ID_OUTSIDE_ALLOW, BN_CLICKED);
        outsideTestNext(1, 40000);
    }

    void outsideTestTick() {
        OutsideTest& t = outside_test;
        if (t.step < 0 || !hooks) return; // finished
        refreshPet(false);
        using St = kvhost::outside::OutsideStatus;
        const OutsidePanelView v = hooks->outsideView();
        const uint64_t now = GetTickCount64();
        std::wstring why;
        switch (t.step) {
            case 10: // the made-up invite's window closed itself: the host has no such invite
                if (!outsideui.inviteOpen() && outsideui.inviteClosedBecause() == "closed") {
                    outsideTestWindowsThenHost();
                    return;
                }
                break;
            case 1: // mapped through the fake router: the sentence, the endpoint, invites on
                // The host's random default port can be one this PC won't let it bind (the listener couldn't open:
                // an error, not listening). Not what this checks: another port, up to three. The step is 13001,
                // wrapping in 20000-59999, so a retry leaves a whole blocked range: this PC has one of about 4100
                // ports (44625-48724) that can't be bound, and a small step stayed inside it on every retry.
                if (v.allowed && !v.listening && v.status == St::Error && !v.checking && t.port_retries < 3) {
                    ++t.port_retries;
                    const uint32_t from = v.port >= 20000 && v.port <= 59999 ? v.port : 20000;
                    changeOutsidePort(static_cast<uint16_t>(20000 + (from - 20000 + 13001) % 40000));
                    break;
                }
                if (v.allowed && v.listening && v.status == St::Mapped && !v.checking) {
                    t.port_a = v.port;
                    t.lan_test_address = v.lanIPv4 == "192.168.77.10" ? 1 : 0; // v3 §10: never this PC's own adapters
                    t.mapped = windowText(lblOutsideStatus) == outsideStatusLine(v) && !windowText(lblOutsideStatus).empty() ? 1 : 0;
                    t.mapped_endpoint = windowText(lblOutsideEndpoint).find(L"81.2.69.160") != std::wstring::npos ? 1 : 0;
                    t.mapped_invite_enabled = ctlEnabled(btnOutsideInvite) ? 1 : 0;
                    // Invite from outside..., past its role prompt (Viewer).
                    std::string err;
                    if (!outsideui.openInvite(kvhost::PetRole::Viewer, &err)) {
                        t.failed = "invite: " + err;
                        outsideTestFinish();
                        return;
                    }
                    t.invite_shown = outsideui.inviteOpen() ? 1 : 0;
                    t.invite_capture_excluded = kvwin::excludedFromCapture(outsideui.inviteWindow()) ? 1 : 0;
                    t.invite_link_length = outsideui.inviteLinkLength();
                    const auto cur = hooks->currentOutsideInvite();
                    const std::string uri = cur ? cur->uri : std::string(); // read here, never written anywhere
                    t.invite_link_ok = looksLikeInviteLink(uri) && uri.size() == t.invite_link_length ? 1 : 0;
                    const size_t tk = uri.find("tok=");
                    const std::string token = tk == std::string::npos ? std::string() : uri.substr(tk + 4, uri.find('&', tk) - tk - 4);
                    const std::string shown_link = narrow(outsideui.inviteShownLink());
                    t.invite_masked = outsideui.inviteMasked() ? 1 : 0;
                    t.invite_token_hidden = !token.empty() && shown_link.find(token) == std::string::npos &&
                                                    shown_link == maskInviteToken(uri)
                                                ? 1
                                                : 0;
                    t.invite_countdown = outsideui.inviteCountdown().rfind(L"Expires in ", 0) == 0 ? 1 : 0;
                    t.invite_warning = outsideui.inviteWarning() == kOutsideInviteWarning ? 1 : 0;
                    t.invite_role_viewer = outsideui.inviteRoleLine() == inviteRoleText(kvhost::PetRole::Viewer) ? 1 : 0;
                    t.shot_refused_invite = shotAllowed(&why) ? 0 : 1;
                    t.hold_until = now + 1500; // the window's own timer runs a few times: it must stay open
                    outsideTestNext(2, 8000);
                }
                break;
            case 2: // still open and the host's; then Cancel; then an owner invite the host closes
                if (now >= t.hold_until) {
                    t.invite_still_open = outsideui.inviteOpen() && outsideui.inviteMatchesHost() ? 1 : 0;
                    outsideui.cancelInvite(); // the window's Cancel
                    t.invite_cancelled = !hooks->currentOutsideInvite() ? 1 : 0;
                    t.invite_window_gone = !outsideui.inviteOpen() ? 1 : 0;
                    t.shot_allowed_after_invite = shotAllowed(&why) ? 1 : 0;
                    std::string err;
                    if (!outsideui.openInvite(kvhost::PetRole::Owner, &err)) {
                        t.failed = "second invite: " + err;
                        outsideTestFinish();
                        return;
                    }
                    t.invite_role_owner = outsideui.inviteRoleLine() == inviteRoleText(kvhost::PetRole::Owner) ? 1 : 0;
                    hooks->cancelOutsideInvite(); // closed by the host, not the window: the window must notice and go
                    outsideTestNext(3, 8000);
                }
                break;
            case 3: // the window closed itself, saying why (the host cancelled it); the log; the address-changed flow
                if (!outsideui.inviteOpen() && !outsideui.inviteClosedBecause().empty()) {
                    t.invite_self_closed = 1;
                    t.invite_closed_reason = outsideui.inviteClosedBecause() == "cancelled" &&
                                                     outsideui.inviteClosedNote() == inviteClosedText("cancelled")
                                                 ? 1
                                                 : 0;
                    onCommand(ID_OUTSIDE_LOG, BN_CLICKED);
                    t.log_opened = outsideui.logWindow() ? 1 : 0;
                    t.log_rows = outsideui.logRows();
                    if (outsideui.logWindow()) DestroyWindow(outsideui.logWindow());
                    // §8.3's address change, for real: the fake router now reports another WAN address, and the host
                    // re-checks at once (the test seam for its 10-minute check), so the panel shows what the host found.
                    TestRouterScript r;
                    r.wan = "81.2.69.161"; // MaxMind's test network, not a documentation block: TestRouterScript::wan
                    hooks->setTestRouter(r);
                    hooks->testOutsideRecheckNow();
                    outsideTestNext(30, 20000);
                }
                break;
            case 30: // the host found the new address: the panel says so; re-share shows the new endpoint and acknowledges
                if (v.allowed && v.status == St::AddressChanged && !v.checking) {
                    t.changed_shown = ctlShown(lblOutsideChanged) && ctlEnabled(btnOutsideReshare) &&
                                              windowText(lblOutsideStatus) == outsideStatusLine(v)
                                          ? 1
                                          : 0;
                    onCommand(ID_OUTSIDE_RESHARE, BN_CLICKED);
                    t.reshare_opened = outsideui.reshareWindow() ? 1 : 0;
                    const std::string link = outsideui.reshareLink();
                    t.reshare_link_ok = looksLikeEndpointLink(link) && link.find("81.2.69.161") != std::string::npos ? 1 : 0;
                    t.reshare_no_secret = link.find("tok=") == std::string::npos ? 1 : 0;
                    outsideui.closeReshare(); // Done: acknowledges
                    outsideTestNext(31, 10000);
                }
                break;
            case 31: // acknowledged: back to mapped, the notice gone; then off
                if (v.allowed && v.status == St::Mapped && !v.checking) {
                    t.changed_acknowledged = !ctlShown(lblOutsideChanged) && outsideui.acknowledgedCount() == outside_ack_mark + 1
                                                 ? 1
                                                 : 0;
                    // pet-camera.md §4 "Remove mapping now", through its button.
                    t.remove_enabled = ctlEnabled(btnOutsideRemove) ? 1 : 0;
                    onCommand(ID_OUTSIDE_REMOVE_MAPPING, BN_CLICKED);
                    outsideTestNext(32, 20000);
                }
                break;
            case 32: // the forward is gone from the fake router, the listener stays, the status is manual; then off
                if (v.allowed && v.status == St::Manual && !v.checking) {
                    const TestRouterCounts c = hooks->testRouterCounts();
                    t.remove_mapping = c.mappings == 0 && c.removes > 0 && !ctlEnabled(btnOutsideRemove) ? 1 : 0;
                    t.remove_listening = v.listening && ctlShown(lblOutsideManual) ? 1 : 0;
                    // Removed by the PC user, so "Map the port again" is offered and Remove is not. Then the
                    // forced 10-minute check, which must not add it back.
                    t.remap_enabled_removed = v.mappingRemoved && ctlEnabled(btnOutsideRemap) && !ctlEnabled(btnOutsideRemove) ? 1 : 0;
                    t.adds_at_remove = c.adds;
                    t.rechecks_mark = hooks->testOutsideRechecks();
                    hooks->testOutsideRecheckNow();
                    outsideTestNext(33, 20000);
                }
                break;
            case 33: // the check has been through the host's router worker: nothing added; then Map the port again
                if (hooks->testOutsideRechecks() > t.rechecks_mark && !v.checking) {
                    const TestRouterCounts c = hooks->testRouterCounts();
                    t.recheck_kept_removed = c.adds == t.adds_at_remove && c.mappings == 0 && v.status == St::Manual &&
                                                     v.mappingRemoved && ctlEnabled(btnOutsideRemap)
                                                 ? 1
                                                 : 0;
                    onCommand(ID_OUTSIDE_REMAP, BN_CLICKED);
                    outsideTestNext(34, 20000);
                }
                break;
            case 34: // mapped again through the fake router; Remove is back and Map the port again is gone; then off
                if (v.allowed && v.status == St::Mapped && !v.checking) {
                    const TestRouterCounts c = hooks->testRouterCounts();
                    t.remapped = c.adds > t.adds_at_remove && c.mappings == 1 && !v.mappingRemoved &&
                                         !ctlEnabled(btnOutsideRemap) && ctlEnabled(btnOutsideRemove)
                                     ? 1
                                     : 0;
                    setChecked(chkOutside, false);
                    onCommand(ID_OUTSIDE_ALLOW, BN_CLICKED);
                    outsideTestNext(4, 20000);
                }
                break;
            case 4: // off: everything disabled, the mapping gone from the router; then on again behind carrier NAT
                if (!v.allowed && !v.listening && v.status == St::Off && !v.checking) {
                    t.off_disables = !checked(chkOutside) && outsideAllOff() ? 1 : 0;
                    const TestRouterCounts c = hooks->testRouterCounts();
                    t.mapping_removed = c.mappings == 0 && c.removes > 0 ? 1 : 0;
                    TestRouterScript r;
                    r.wan = "100.64.1.2";
                    hooks->setTestRouter(r);
                    setChecked(chkOutside, true);
                    onCommand(ID_OUTSIDE_ALLOW, BN_CLICKED);
                    outsideTestNext(5, 40000);
                }
                break;
            case 5: // carrier NAT: the sentence, no endpoint, no invites; then no router at all on a new port
                if (v.allowed && v.status == St::CarrierNat && !v.checking) {
                    t.carrier_nat = windowText(lblOutsideStatus) == outsideStatusLine(v) &&
                                            windowText(lblOutsideStatus).find(L"carrier NAT") != std::wstring::npos
                                        ? 1
                                        : 0;
                    t.carrier_invite_disabled = !ctlEnabled(btnOutsideInvite) && !windowText(lblOutsideInvite).empty() ? 1 : 0;
                    TestRouterScript r;
                    r.present = false;
                    hooks->setTestRouter(r);
                    t.port_a = static_cast<uint16_t>(v.port < 65000 ? v.port + 1 : 20001);
                    changeOutsidePort(t.port_a); // Change port..., past its prompt
                    outsideTestNext(6, 60000);    // §8.2: the host looks for a router for about 10 s first
                }
                break;
            case 6: // manual: the steps, the field; a private address refused, a public one taken
                if (v.allowed && v.status == St::Manual && !v.checking && v.port == t.port_a) {
                    t.manual = windowText(lblOutsideStatus) == outsideStatusLine(v) ? 1 : 0;
                    t.manual_steps = ctlShown(lblOutsideManual) && windowText(lblOutsideManual) == outsideManualSteps(v) &&
                                             windowText(lblOutsideManual).find(std::to_wstring(v.port)) != std::wstring::npos
                                         ? 1
                                         : 0;
                    t.manual_field = ctlShown(edOutsideAddr) && ctlEnabled(edOutsideAddr) &&
                                             windowText(lblOutsideAddr) == kOutsidePublicAddressLabel && !ctlEnabled(btnOutsideInvite)
                                         ? 1
                                         : 0;
                    SetWindowTextW(edOutsideAddr, L"192.168.1.5");
                    t.manual_bad_refused = !onOutsideAddressSave() && !outside_note.empty() &&
                                                   hooks->outsideView().publicAddressEntered.empty()
                                               ? 1
                                               : 0;
                    SetWindowTextW(edOutsideAddr, L"81.2.69.161"); // the host's real check: see TestRouterScript::wan
                    if (!onOutsideAddressSave()) {
                        t.failed = "the public address was refused: " + narrow(outside_note);
                        outsideTestFinish();
                        return;
                    }
                    outsideTestNext(7, 20000);
                }
                break;
            case 7: // the entered address is the endpoint, "entered by you, not checked", and invites are on
                if (v.endpointKnown && v.endpointHost == "81.2.69.161" && !v.checking) {
                    t.manual_entered = 1;
                    t.manual_unverified = !v.endpointVerified &&
                                                  windowText(lblOutsideEndpoint).find(L"(entered by you, not checked)") != std::wstring::npos
                                              ? 1
                                              : 0;
                    t.manual_invite_enabled = v.canInvite && ctlEnabled(btnOutsideInvite) ? 1 : 0;
                    // A port another machine on the home network holds on the router.
                    TestRouterScript r;
                    t.port_a = static_cast<uint16_t>(v.port < 65000 ? v.port + 1 : 20002);
                    r.takenPort = t.port_a;
                    hooks->setTestRouter(r);
                    changeOutsidePort(t.port_a);
                    outsideTestNext(8, 40000);
                }
                break;
            case 8: // error: the host's sentence, no invites
                if (v.allowed && v.status == St::Error && !v.checking && v.port == t.port_a) {
                    t.error_taken = windowText(lblOutsideStatus) == outsideStatusLine(v) && !windowText(lblOutsideStatus).empty() &&
                                            !ctlEnabled(btnOutsideInvite)
                                        ? 1
                                        : 0;
                    outsideTestFinish();
                    return;
                }
                break;
            default: break;
        }
        if (t.step > 0 && now > t.deadline) {
            char b[300];
            std::snprintf(b, sizeof(b), "step %d timed out (allowed=%d listening=%d status=%d checking=%d port=%u endpoint=%d)", t.step,
                          v.allowed ? 1 : 0, v.listening ? 1 : 0, static_cast<int>(v.status), v.checking ? 1 : 0,
                          static_cast<unsigned>(v.port), v.endpointKnown ? 1 : 0);
            t.failed = b;
            outsideTestFinish();
        }
    }

    void outsideTestFinish() {
        OutsideTest& t = outside_test;
        t.done = t.failed.empty();
        t.step = -1;
        KillTimer(main, kTimerOutsideTest);
        t.router = hooks ? hooks->testRouterCounts() : TestRouterCounts{};
        // No hook captures the invite or the re-share window (v3 §10); both are closed before the main window is.
        if (outsideui.inviteOpen()) outsideui.cancelInvite();
        outsideui.closeAll();
        refreshPet(true);
        if (!t.main_captured) {
            if (chkOutside) { // the panel's Outside the home section, scrolled to
                RECT br{};
                GetWindowRect(chkOutside, &br);
                MapWindowPoints(nullptr, panelWnd, reinterpret_cast<POINT*>(&br), 2);
                scrollPanelTo(panel_scroll + br.top - S(40));
                // Every control repainted as it is now (enabled or not) before the capture.
                RedrawWindow(panelWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
            std::wstring why;
            if (shotAllowed(&why)) t.main_ok = captureWindowToPng(main, lo.screenshot_path, &t.main_err);
            else t.main_err = "refused: " + narrow(why);
            t.main_captured = true;
        }
        writeScreenshotReport(t.main_ok, t.main_err);
        if (lo.exit_after_screenshot) PostMessageW(main, WM_CLOSE, 0, 0);
    }
#endif

    // --test-pet-camera: each step goes through the panel's, the dialog's or the editor's own code, and
    // waits for what the fake v3 device and the banner show. Synthetic, loopback only, a temp state folder.
    struct PetTest {
        int ran = 0;
        int step = 0;
        uint64_t deadline = 0, hold_until = 0;
        int aus_mark = 0;
        bool done = false;
        std::string failed;
        size_t loaded_devices = 0;
        std::string loaded_names;
        int loaded_sharing = 0, loaded_privacy = 0, loaded_stale_owners = 0, sharing_turned_on = 0;
        std::string device_role;
        std::wstring banner_text, renamed_banner, tiers;
        int banner_shown = 0, tier_high_sessions = 0, health_tiers_shown = 0;
        int privacy_hid_banner = 0, privacy_button_on = 0, privacy_frames_while_on = -1, privacy_off_back = 0;
        int sharing_off_hid = 0, sharing_on_back = 0;
        int schedule_row_added = 0, schedule_hid = 0, schedule_removed = 0, schedule_back = 0;
        int renamed = 0, devices_dialog_rows = 0, devices_highlighted = 0, devices_captured = 0;
        int revoked_notice = 0, devices_dialog_rows_after = 0, banner_after_revoke = -1;
        size_t devices_after_revoke = 0;
        bool main_captured = false, main_ok = false;
        std::string main_err;
    } pet_test;

    void petTestNext(int step, int timeout_ms) {
        pet_test.step = step;
        pet_test.deadline = GetTickCount64() + static_cast<uint64_t>(timeout_ms);
    }

    void petTestStart() {
        PetTest& t = pet_test;
        t.ran = 1;
        // What the run loaded from its --state-dir (devices.json, pet-camera.json), before anything changes.
        const auto devs = hooks->petDevices();
        t.loaded_devices = devs.size();
        const uint64_t now = unixMsNow();
        for (const auto& d : devs) {
            if (!t.loaded_names.empty()) t.loaded_names += ";";
            t.loaded_names += d.name;
            t.loaded_stale_owners += isStaleOwner(d, now) ? 1 : 0;
        }
        const kvhost::PetSettings ps = hooks->petSettings();
        t.loaded_sharing = ps.sharingEnabled ? 1 : 0;
        t.loaded_privacy = ps.privacyMode ? 1 : 0;
        if (ps.privacyMode) onCommand(ID_PET_PRIVACY, BN_CLICKED);
        if (!ps.sharingEnabled) {
            setShareFromPanel(true); // the device pairs as a viewer: it watches only while sharing is on
            t.sharing_turned_on = 1;
        }
        // "Pair a device..." (the role is chosen in the window: Viewer by default), then the fake device
        // pairs through that window and connects.
        onCommand(ID_PAIR, BN_CLICKED);
        std::string err;
        if (!hooks->startTestDevice("Test viewer", &err)) {
            t.failed = "start: " + err;
            petTestFinish();
            return;
        }
        petTestNext(1, 25000);
    }

    void petTestTick() {
        PetTest& t = pet_test;
        if (t.step < 0 || !hooks) return; // finished
        refreshPet(false);
        const TestDeviceStatus d = hooks->testDeviceStatus();
        if (!d.error.empty()) {
            t.failed = "device: " + d.error;
            petTestFinish();
            return;
        }
        const bool banner = !watching_text.empty() && lblWatching && IsWindowVisible(lblWatching);
        const uint64_t now = GetTickCount64();
        switch (t.step) {
            case 1: // paired through the window, watching, and the banner names it
                if (d.paired && d.watching && d.aus >= 5 && banner) {
                    t.banner_shown = 1;
                    t.banner_text = watching_text;
                    t.device_role = d.role;
                    const auto tiers = hooks->tierStatus();
                    t.tiers = tierDiagnosticsLine(tiers);
                    for (const auto& ts : tiers) {
                        if (ts.tier == "high") t.tier_high_sessions = ts.sessions;
                    }
                    updateHealth();
                    t.health_tiers_shown = show_health && health_tier_line.rfind(L"Live tiers:", 0) == 0 ? 1 : 0;
                    onCommand(ID_PET_PRIVACY, BN_CLICKED); // privacy on
                    petTestNext(2, 8000);
                }
                break;
            case 2: // privacy mode: the video stops, the device is told why, the banner goes
                if (!banner && d.privacy && !d.watching && d.reason == kvhost::live_unavailable::kPrivacyMode) {
                    t.privacy_hid_banner = 1;
                    t.privacy_button_on = pet_privacy_on ? 1 : 0;
                    t.aus_mark = d.aus;
                    t.hold_until = now + 1200;
                    petTestNext(3, 5000);
                }
                break;
            case 3: // ...and no frame arrives while it is on
                if (now >= t.hold_until) {
                    t.privacy_frames_while_on = d.aus - t.aus_mark;
                    t.aus_mark = d.aus;
                    onCommand(ID_PET_PRIVACY, BN_CLICKED); // privacy off
                    petTestNext(4, 8000);
                }
                break;
            case 4: // watching again
                if (banner && d.watching && !d.privacy && d.aus >= t.aus_mark + 3) {
                    t.privacy_off_back = 1;
                    setShareFromPanel(false);
                    petTestNext(5, 8000);
                }
                break;
            case 5: // sharing off: a viewer can't watch
                if (!banner && !d.watching && d.reason == kvhost::live_unavailable::kSharingOff) {
                    t.sharing_off_hid = 1;
                    setShareFromPanel(true);
                    petTestNext(6, 8000);
                }
                break;
            case 6: // sharing on again; then a schedule row covering every day, all day, in the editor
                if (banner && d.watching) {
                    t.sharing_on_back = 1;
                    onCommand(ID_PET_SCHEDULE, BN_CLICKED);
                    petui.setScheduleEditorFields(0x7F, L"00:00", L"00:00");
                    t.schedule_row_added = petui.addScheduleRowFromEditor() ? 1 : 0;
                    petTestNext(7, 8000);
                }
                break;
            case 7: // the schedule has sharing off now: the viewer stops, with that reason
                if (!banner && !d.watching && d.reason == kvhost::live_unavailable::kSharingSchedule) {
                    t.schedule_hid = 1;
                    petui.selectScheduleRow(0);
                    t.schedule_removed = petui.removeSelectedScheduleRow() ? 1 : 0;
                    petTestNext(8, 8000);
                }
                break;
            case 8: // the row removed: watching again; then Devices... and a rename there
                if (banner && d.watching) {
                    t.schedule_back = 1;
                    if (petui.scheduleWindow()) DestroyWindow(petui.scheduleWindow());
                    onCommand(ID_PET_DEVICES, BN_CLICKED);
                    t.renamed = petui.renameDevice(d.deviceId, L"Garden phone") ? 1 : 0;
                    petTestNext(9, 8000);
                }
                break;
            case 9: // the new name reaches the banner and the device; capture the dialog and the window
                if (banner && watching_text.find(L"Garden phone") != std::wstring::npos && d.device_name == "Garden phone") {
                    t.renamed_banner = watching_text;
                    petui.refreshDevices();
                    t.devices_dialog_rows = petui.deviceRows();
                    t.devices_highlighted = petui.highlightedRows();
                    // The screenshot hook's rule for every capture: the synthetic source, no colour, no replay.
                    // The capture shows the panel's Pet camera section (scrolled to it) with the banner.
                    if (btnPrivacy) {
                        RECT br{};
                        GetWindowRect(btnPrivacy, &br);
                        MapWindowPoints(nullptr, panelWnd, reinterpret_cast<POINT*>(&br), 2);
                        scrollPanelTo(panel_scroll + br.top - S(40));
                        UpdateWindow(panelWnd);
                    }
                    std::wstring why;
                    if (shotAllowed(&why)) {
                        std::string err;
                        t.devices_captured = petui.devicesWindow() &&
                                                     captureWindowToPng(petui.devicesWindow(), lo.screenshot_path + L".devices.png", &err)
                                                 ? 1
                                                 : 0;
                        t.main_ok = captureWindowToPng(main, lo.screenshot_path, &t.main_err);
                    } else {
                        t.main_err = "refused: " + narrow(why);
                    }
                    t.main_captured = true;
                    petui.revokeDevice(d.deviceId, false); // the dialog's Revoke, past its confirm dialog
                    petTestNext(10, 8000);
                }
                break;
            case 10: // revoked: 0x0F revoked and closed, out of the list, the banner gone
                if (d.notice == "revoked" && d.closed && !banner) {
                    t.revoked_notice = 1;
                    t.devices_after_revoke = hooks->petDevices().size();
                    t.devices_dialog_rows_after = petui.deviceRows();
                    t.banner_after_revoke = banner ? 1 : 0;
                    petTestFinish();
                    return;
                }
                break;
            default: break;
        }
        if (t.step > 0 && now > t.deadline) {
            char b[300];
            std::snprintf(b, sizeof(b), "step %d timed out (paired=%d watching=%d privacy=%d reason=%s aus=%d notice=%s banner=%d)",
                          t.step, d.paired ? 1 : 0, d.watching ? 1 : 0, d.privacy ? 1 : 0, d.reason.c_str(), d.aus,
                          d.notice.c_str(), banner ? 1 : 0);
            t.failed = b;
            petTestFinish();
        }
    }

    // --test-identity-reset: a --state-dir whose pairing.json can't be used. Each step goes through the panel's
    // own code (its buttons' handlers; the reset past its confirm dialog), then the fake v3 device pairs through
    // the pairing window once the host serves. Synthetic, loopback only.
    struct IdTest {
        int ran = 0;
        int step = 0;
        uint64_t deadline = 0;
        bool done = false;
        std::string failed;
        int unusable_at_start = 0, panel_shown = 0, retry_still_unusable = 0, retry_left_file = 0;
        int reset_ok = 0, running_after = 0, panel_hidden_after = 0, device_paired = 0, device_connected = 0;
        std::wstring panel_text, status_text;
        std::string reset_note;
        bool main_captured = false, main_ok = false;
        std::string main_err;
    } id_test;

    static std::wstring oneLine(std::wstring t) {
        std::replace(t.begin(), t.end(), L'\n', L' ');
        return t;
    }
    std::string pairingJsonBytes() const {
        std::ifstream f(hooks->stateDir() + L"\\pairing.json", std::ios::binary);
        if (!f) return "<unreadable>";
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    static bool visible(HWND h) { return h && IsWindowVisible(h); }

    void idTestStart() {
        IdTest& t = id_test;
        t.ran = 1;
        refreshPet(true);
        updateStatus();
        t.unusable_at_start = hooks->hostIdentityUnusable() ? 1 : 0;
        t.panel_shown = visible(lblIdentity) && visible(btnIdRetry) && visible(btnIdReset) && !visible(btnPrivacy) && !visible(chkShare) ? 1 : 0;
        wchar_t b[512] = {0};
        GetWindowTextW(lblIdentity, b, 512);
        t.panel_text = b;
        const std::wstring status_line = hooks->statusLine();
        t.status_text = status_line.substr(0, status_line.find(L'\n'));
        // The capture: the panel's Pet camera section with the reason and both buttons (the synthetic source).
        RECT br{};
        GetWindowRect(lblIdentity, &br);
        MapWindowPoints(nullptr, panelWnd, reinterpret_cast<POINT*>(&br), 2);
        scrollPanelTo(panel_scroll + br.top - S(40));
        UpdateWindow(panelWnd);
        std::wstring why;
        if (shotAllowed(&why)) t.main_ok = captureWindowToPng(main, lo.screenshot_path, &t.main_err);
        else t.main_err = "refused: " + narrow(why);
        t.main_captured = true;
        // "Try again": the file still can't be used, and nothing was written.
        const std::string before = pairingJsonBytes();
        onCommand(ID_PET_ID_RETRY, BN_CLICKED);
        t.retry_still_unusable = hooks->hostIdentityUnusable() ? 1 : 0;
        t.retry_left_file = pairingJsonBytes() == before ? 1 : 0;
        // "Reset this PC's identity", past its confirm dialog.
        t.reset_ok = onIdentityReset(false) ? 1 : 0;
        t.reset_note = last_reset_note;
        t.running_after = !hooks->hostIdentityUnusable() && hooks->hostStartError().empty() ? 1 : 0;
        t.panel_hidden_after = !visible(lblIdentity) && !visible(btnIdReset) && visible(btnPrivacy) && visible(chkShare) ? 1 : 0;
        if (!t.reset_ok || !t.running_after) {
            t.failed = "reset: " + narrow(identity_message) + " " + hooks->hostStartError();
            idTestFinish();
            return;
        }
        // The host serves with the new identity: the fake device pairs through the window and connects.
        onCommand(ID_PAIR, BN_CLICKED);
        std::string err;
        if (!hooks->startTestDevice("Test viewer", &err)) {
            t.failed = "start: " + err;
            idTestFinish();
            return;
        }
        t.step = 1;
        t.deadline = GetTickCount64() + 25000;
    }

    void idTestTick() {
        IdTest& t = id_test;
        if (t.step < 0 || !hooks) return; // finished
        refreshPet(false);
        const TestDeviceStatus d = hooks->testDeviceStatus();
        if (!d.error.empty()) {
            t.failed = "device: " + d.error;
            idTestFinish();
            return;
        }
        t.device_paired = d.paired ? 1 : 0;
        if (d.paired && d.connected && d.got_hello) {
            t.device_connected = 1;
            idTestFinish();
            return;
        }
        if (GetTickCount64() > t.deadline) {
            t.failed = "the device didn't pair and connect (paired=" + std::to_string(d.paired ? 1 : 0) +
                       " connected=" + std::to_string(d.connected ? 1 : 0) + ")";
            idTestFinish();
        }
    }

    void idTestFinish() {
        IdTest& t = id_test;
        t.done = t.failed.empty();
        t.step = -1;
        KillTimer(main, kTimerIdTest);
        if (!t.main_captured) {
            std::wstring why;
            if (shotAllowed(&why)) t.main_ok = captureWindowToPng(main, lo.screenshot_path, &t.main_err);
            else t.main_err = "refused: " + narrow(why);
        }
        refreshPet(true);
        writeScreenshotReport(t.main_ok, t.main_err);
        if (hooks) hooks->stopTestDevice();
        if (lo.exit_after_screenshot) PostMessageW(main, WM_CLOSE, 0, 0);
    }

    void petTestFinish() {
        PetTest& t = pet_test;
        t.done = t.failed.empty();
        t.step = -1;
        KillTimer(main, kTimerPetTest);
        if (!t.main_captured) {
            std::wstring why;
            if (shotAllowed(&why)) t.main_ok = captureWindowToPng(main, lo.screenshot_path, &t.main_err);
            else t.main_err = "refused: " + narrow(why);
        }
        writeScreenshotReport(t.main_ok, t.main_err);
        if (hooks) hooks->stopTestDevice();
        if (lo.exit_after_screenshot) PostMessageW(main, WM_CLOSE, 0, 0);
    }

#ifdef KV_WITH_STREAMING
    // --test-camera-roll (synthetic, --camera-roll-dir under the build tree, loopback only): the roll's settings, a
    // motion clip from the viewer's own depth feed, a manual clip, the browser and the player, each through the
    // panel's, the browser's or the player's own code; and the rules around them: motion from frames that aren't
    // the live sensor's never starts a clip, Motion greys out with the sensor off, the player's frames never reach
    // the host, and the screenshot hook refuses while a roll clip is on screen.
    struct RollTest {
        int ran = 0;
        int step = 0;
        uint64_t deadline = 0, hold_until = 0, window_start = 0;
        bool done = false;
        std::string failed;
        int available_at_start = 0, unreadable = 0, refused_unreadable = 0;
        int roll_on = 0, sens_set = 0, sched_row_added = 0, sched_on = 0, keep_set = 0;
        int folder_refused_recordings = 0, folder_changed = 0, d6_note_shown = 0;
        std::wstring folder_refusal, folder_note, d6_note;
        // The limits beside the switch, Confirm while they aren't known, and gone once they are.
        std::wstring limits_text;
        int confirm_shown = 0, confirm_ok = 0, confirm_hidden_later = 0, limits_known_later = 0;
        // The D6 note and "Preparing..." follow the host's predicates on every tick.
        int d6_mismatch = 0, preparing_mismatch = 0, preparing_seen = 0;
        int armed = 0, motion_from_replay_frames_started = -1, motion_viewer_clip = 0, motion_clip_finalized = 0;
        int manual_started = 0, manual_stopped = 0, manual2_stopped = 0, motion_greyed = 0;
        uint64_t clips_finalized = 0;
        std::wstring motion_greyed_text;
        int browser_days = 0, browser_clips = 0, browser_thumbs = 0, manual_found = 0, manual_has_thumb = 0, manual_by_pc = 0;
        int motion_found = 0, manual2_found = 0, starred = 0;
        int shot_refused_browser = 0, player_opened = 0, player_seeked = 0, shot_refused_player = 0;
        int shot_refused_browser_only = 0, shot_allowed_after = 0, deleted = 0, deleted_gone = 0;
        uint64_t player_frames = 0;
        // The sensor off: nothing reaches the host while the player decodes.
        uint64_t onframe_before = 0, onframe_after = 0, tier_frames_before = 0, tier_frames_after = 0;
        uint64_t player_decoded_mark = 0, player_decoded_while_off = 0;
        // With the sensor running: every frame the host was handed during playback was a live-source frame.
        uint64_t t6_live_mark = 0, t6_hooks_mark = 0, t6_live = 0, t6_foreign = 0;
        // Live frames to the host in a window of playback, then in a window of scrubbing.
        uint64_t k4_mark = 0, k4_baseline = 0, k4_burst = 0, k4_requests_mark = 0, k4_requests = 0, k4_completed_mark = 0;
        uint64_t k4_completed = 0, k4_ui_max_ms = 0;
        int k4_pos = 50;
        int64_t player_seek_target = 0, player_seek_pts = 0;
        uint64_t seek_mark = 0;
        uint32_t player_w = 0, player_h = 0;
        std::string manual_id, motion_id, manual2_id;
        uint64_t finalized_mark = 0;
        // The pins, and a phone owner's delete of the clip the player has open.
        size_t pins_while_open = 0, pins_after_close = 0, pins_after_owner_delete = 0;
        int owner_delete_hooked = 0, owner_delete_player_closed = 0, owner_delete_gone = 0, pc_delete_closed_player = 0;
        std::string owner_delete_result;
        std::wstring owner_delete_notice;
        bool main_captured = false, main_ok = false;
        std::string main_err;
    } roll_test;
    bool roll_delete_hooked = false; // setRollClipDeletedCallback: the host tells the PC about an owner's delete

    void rollTestNext(int step, int timeout_ms) {
        roll_test.step = step;
        roll_test.deadline = GetTickCount64() + static_cast<uint64_t>(timeout_ms);
    }
    void setChecked(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    uint64_t tierFramesOut() const {
        uint64_t n = 0;
        for (const auto& t : hooks->tierStatus()) n += t.framesOut;
        return n;
    }
    // The midnight rule: a clip is found by its id, in whichever day it landed (two clips can straddle midnight).
    bool rollTestSelectClip(const std::string& clipId) {
        if (clipId.empty()) return false;
        for (int d = 0; d < rollui.dayRows(); ++d) {
            if (rollui.selectDay(d) && rollui.selectClip(clipId)) return true;
        }
        return false;
    }
    bool rollTestClipListed(const std::string& clipId) {
        for (int d = 0; d < rollui.dayRows(); ++d) {
            if (!rollui.selectDay(d)) continue;
            for (const auto& c : rollui.clips()) {
                if (c.clipId == clipId) return true;
            }
        }
        return false;
    }
    // Synthetic depth for the motion feed: a floor at 2.5 m with a little noise, and from frame `box_from` on an
    // 80 x 80 px box at 1.5 m moving 4 px a frame (the detector's own test shape, test_roll_triggers.cpp).
    void feedTestDepth(int frames, int box_from, uint64_t seq0, bool live) {
        std::vector<uint16_t> d(512u * 424u);
        uint32_t rng = 12345;
        for (int i = 0; i < frames; ++i) {
            for (auto& v : d) {
                rng = rng * 1664525u + 1013904223u;
                v = static_cast<uint16_t>(2500 + static_cast<int>((rng >> 8) % 21) - 10);
            }
            if (i >= box_from) {
                const int x0 = 100 + 4 * (i - box_from);
                for (int y = 150; y < 230; ++y) {
                    for (int x = x0; x < x0 + 80 && x < 512; ++x) d[static_cast<size_t>(y) * 512 + static_cast<size_t>(x)] = 1500;
                }
            }
            hooks->submitDepthForMotion(d.data(), 512, 424, seq0 + static_cast<uint64_t>(i),
                                        static_cast<uint64_t>(i) * 33000000ULL, live);
        }
    }

    void rollTestStart() {
        RollTest& t = roll_test;
        t.ran = 1;
        refreshPet(true);
        t.available_at_start = hooks->rollAvailable() ? 1 : 0;
        if (!hooks->petSaveError().empty() && hooks->petSettings().privacyMode) {
            // pet-camera.json can't be read: the roll's settings are refused with the reason, as the others are.
            t.unreadable = 1;
            setChecked(chkRoll, true);
            onCommand(ID_ROLL_ENABLE, BN_CLICKED);
            t.refused_unreadable = !hooks->petSettings().rollEnabled && !checked(chkRoll) &&
                                           roll_note.find(L"wasn't changed") != std::wstring::npos
                                       ? 1
                                       : 0;
            rollTestFinish();
            return;
        }
        if (!t.available_at_start) {
            t.failed = "no roll: " + hooks->rollError();
            rollTestFinish();
            return;
        }
        // A fresh state: the limits beside the switch, and Confirm (they aren't the owner's yet). Confirm
        // writes them; with the roll on they are known (turning it on counts as confirming).
        t.limits_text = windowText(lblRollLimits);
        t.confirm_shown = IsWindowVisible(btnRollConfirm) && IsWindowVisible(lblRollConfirm) &&
                                  windowText(lblRollConfirm) == L"Retention starts when you confirm the limits."
                              ? 1
                              : 0;
        onCommand(ID_ROLL_CONFIRM_LIMITS, BN_CLICKED);
        t.confirm_ok = roll_note.empty() ? 1 : 0;
        // The settings, each through the panel's own control.
        setChecked(chkRoll, true);
        onCommand(ID_ROLL_ENABLE, BN_CLICKED);
        t.roll_on = hooks->petSettings().rollEnabled ? 1 : 0;
        SendMessageW(tbMotion, TBM_SETPOS, TRUE, 7);
        onSlider(tbMotion);
        t.sens_set = hooks->petSettings().motionSensitivity == 7 ? 1 : 0;
        onCommand(ID_ROLL_SCHED, BN_CLICKED);
        SYSTEMTIME lt{};
        GetLocalTime(&lt);
        const int start = (lt.wHour * 60 + lt.wMinute + 6 * 60) % (24 * 60); // six hours from now: it never fires here
        rollui.setScheduleEditorFields(0x7F, clockText(static_cast<uint16_t>(start)), L"5");
        t.sched_row_added = rollui.addScheduleRowFromEditor() ? 1 : 0;
        if (rollui.scheduleWindow()) DestroyWindow(rollui.scheduleWindow());
        setChecked(chkRecSched, true);
        onCommand(ID_ROLL_SCHED_ON, BN_CLICKED);
        t.sched_on = hooks->petSettings().recordScheduleEnabled ? 1 : 0;
        SetWindowTextW(edKeepDays, L"14");
        SetWindowTextW(edKeepGb, L"50");
        onCommand(ID_ROLL_KEEP_APPLY, BN_CLICKED);
        t.keep_set = hooks->petSettings().rollKeepDays == 14 && hooks->petSettings().rollCapBytes == 50000000000ULL ? 1 : 0;
        // "Change...": the recordings folder is refused by its path (nothing is opened); a folder of the test's own
        // is saved, and applies after a restart.
        t.folder_refused_recordings = changeRollFolder(recordingsDirForRoll(), false) ? 0 : 1;
        t.folder_refusal = roll_note;
        t.folder_changed = changeRollFolder(lo.camera_roll_dir + L"-next", false) ? 1 : 0;
        t.folder_note = roll_note;
        refreshPet(true);
        t.d6_note = windowText(lblD6Note);
        t.d6_note_shown = !t.d6_note.empty() && IsWindowVisible(lblD6Note) ? 1 : 0;
        motion_feed_paused = true; // until the frames that aren't the live sensor's are done
        rollTestNext(1, 20000);
    }

    // What the panel shows follows the host's predicates, checked on every tick of the hook.
    void rollTestCheckPredicates() {
        RollTest& t = roll_test;
        const bool skipped = hooks->turnOffSkippedForRoll();
        if (skipped == windowText(lblD6Note).empty()) t.d6_mismatch = 1;
        if (hooks->rollPreparing()) {
            t.preparing_seen = 1;
            if (windowText(lblRollStatus) != L"Preparing the camera roll…") t.preparing_mismatch = 1;
        }
    }

    void rollTestTick() {
        RollTest& t = roll_test;
        if (t.step < 0 || !hooks) return; // finished
        refreshPet(false);
        rollTestCheckPredicates();
        const kvhost::RollRecorderStatus st = hooks->rollStatus();
        const uint64_t now = GetTickCount64();
        const bool motion_clip = st.recording && st.clipTrigger && *st.clipTrigger == kvhost::ClipTrigger::Motion;
        RollPlayer& pl = rollui.player();
        switch (t.step) {
            case 1: // armed (the pre-roll runs); motion on; frames marked as not the live sensor's
                if (st.ready && st.armed) {
                    t.armed = 1;
                    setChecked(chkMotion, true);
                    onCommand(ID_ROLL_MOTION, BN_CLICKED);
                    feedTestDepth(42, 30, 1000000000ULL, false); // 30 floor frames, then a moving box: motion, if live
                    t.hold_until = now + 2000;
                    rollTestNext(2, 6000);
                }
                break;
            case 2: // ...never start a clip; then the viewer's own feed (the synthetic sphere moves)
                if (now >= t.hold_until) {
                    t.motion_from_replay_frames_started = st.recording || st.clipsFinalized > 0 ? 1 : 0;
                    motion_feed_paused = false;
                    rollTestNext(3, 20000);
                }
                break;
            case 3: // a motion clip from the live (synthetic) depth the viewer polls
                if (motion_clip && motion_fed_live > 0) {
                    t.motion_viewer_clip = 1;
                    t.hold_until = now + 2500;
                    rollTestNext(4, 6000);
                }
                break;
            case 4: // motion off: its trigger drops and the clip ends
                if (now >= t.hold_until) {
                    setChecked(chkMotion, false);
                    onCommand(ID_ROLL_MOTION, BN_CLICKED);
                    rollTestNext(5, 30000);
                }
                break;
            case 5: // finalized (its id kept); then "Record a clip"
                if (!st.recording && st.clipsFinalized >= 1) {
                    t.motion_clip_finalized = 1;
                    t.motion_id = st.lastClipId;
                    t.finalized_mark = st.clipsFinalized;
                    // By now the limits are the owner's (confirmed, the roll turned on, Keep applied).
                    t.limits_known_later = hooks->rollLimitsKnown() ? 1 : 0;
                    t.confirm_hidden_later = !IsWindowVisible(btnRollConfirm) && !IsWindowVisible(lblRollConfirm) ? 1 : 0;
                    onCommand(ID_ROLL_RECORD, BN_CLICKED);
                    rollTestNext(6, 15000);
                }
                break;
            case 6: // recording, named manual; Stop after 3 s
                if (st.recording && st.clipTrigger && *st.clipTrigger == kvhost::ClipTrigger::Manual && st.manualActive &&
                    st.clipElapsedSec >= 3.0) {
                    t.manual_started = 1;
                    onCommand(ID_ROLL_STOP, BN_CLICKED);
                    rollTestNext(7, 30000);
                }
                break;
            case 7: // finalized (its id kept); a second manual clip, for the PC's own delete later
                if (!st.recording && st.clipsFinalized > t.finalized_mark) {
                    t.manual_stopped = 1;
                    t.manual_id = st.lastClipId;
                    t.finalized_mark = st.clipsFinalized;
                    onCommand(ID_ROLL_RECORD, BN_CLICKED);
                    rollTestNext(8, 15000);
                }
                break;
            case 8:
                if (st.recording && st.manualActive && st.clipElapsedSec >= 1.0) {
                    onCommand(ID_ROLL_STOP, BN_CLICKED);
                    rollTestNext(9, 30000);
                }
                break;
            case 9: // finalized; the browser (every day, the clips by id); a star; the player, with the sensor running
                if (!st.recording && st.clipsFinalized > t.finalized_mark) {
                    t.manual2_stopped = 1;
                    t.manual2_id = st.lastClipId;
                    t.clips_finalized = st.clipsFinalized;
                    onCommand(ID_ROLL_BROWSER, BN_CLICKED);
                    t.browser_days = rollui.dayRows();
                    for (int d = 0; d < rollui.dayRows(); ++d) {
                        if (!rollui.selectDay(d)) continue;
                        t.browser_clips += rollui.clipRows();
                        t.browser_thumbs += rollui.thumbnailsShown();
                        for (const auto& c : rollui.clips()) {
                            if (c.clipId == t.manual_id && c.trigger == kvhost::ClipTrigger::Manual) {
                                t.manual_found = 1;
                                t.manual_has_thumb = c.hasThumbnail ? 1 : 0;
                                t.manual_by_pc = c.startedBy && *c.startedBy == kvhost::roll::kPcDeviceName ? 1 : 0;
                            } else if (c.clipId == t.motion_id && c.trigger == kvhost::ClipTrigger::Motion) {
                                t.motion_found = 1;
                            } else if (c.clipId == t.manual2_id) {
                                t.manual2_found = 1;
                            }
                        }
                    }
                    if (!t.manual_found || !rollTestSelectClip(t.manual_id)) {
                        t.failed = "the manual clip isn't in the browser";
                        rollTestFinish();
                        return;
                    }
                    rollui.starSelected();
                    rollTestSelectClip(t.manual_id);
                    for (const auto& c : rollui.clips()) {
                        if (c.clipId == t.manual_id) t.starred = c.starred ? 1 : 0;
                    }
                    std::wstring why;
                    t.shot_refused_browser = shotAllowed(&why) ? 0 : 1;
                    t.player_opened = rollui.playSelected() ? 1 : 0;
                    if (!t.player_opened) {
                        t.failed = "the player didn't open: " + rollui.lastError();
                        rollTestFinish();
                        return;
                    }
                    t.pins_while_open = hooks->rollPcPinsHeld();
                    rollTestNext(10, 15000);
                }
                break;
            case 10: // decoding (on its worker); counting starts of the frames the host is handed, by source
                if (!pl.isOpen()) {
                    t.failed = "the player closed: " + pl.lastError();
                    rollTestFinish();
                    return;
                }
                if (pl.framesPresented() >= 15) {
                    t.player_w = pl.frameWidth();
                    t.player_h = pl.frameHeight();
                    t.t6_live_mark = on_frame_live;
                    t.t6_hooks_mark = hooks->framesHandedToHost();
                    t.k4_mark = on_frame_live;
                    t.window_start = now;
                    rollTestNext(11, 10000);
                }
                break;
            case 11: // the scrub check's baseline: live frames to the host in 3 s of playback
                if (now - t.window_start >= 3000) {
                    t.k4_baseline = on_frame_live - t.k4_mark;
                    t.k4_mark = on_frame_live;
                    t.k4_requests_mark = pl.seeksRequested();
                    t.k4_completed_mark = pl.seeksCompleted();
                    t.window_start = now;
                    rollTestNext(12, 10000);
                }
                break;
            case 12: { // 3 s of the owner dragging the scrub bar back and forth, 30 positions a turn
                const uint64_t turn = GetTickCount64();
                for (int i = 0; i < 30 && pl.isOpen(); ++i) {
                    t.k4_pos = t.k4_pos == 50 ? 950 : 50;
                    SendMessageW(pl.window(), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, t.k4_pos), reinterpret_cast<LPARAM>(pl.scrubBar()));
                }
                t.k4_ui_max_ms = std::max<uint64_t>(t.k4_ui_max_ms, GetTickCount64() - turn);
                if (GetTickCount64() - t.window_start >= 3000) {
                    t.k4_burst = on_frame_live - t.k4_mark;
                    t.k4_requests = pl.seeksRequested() - t.k4_requests_mark;
                    rollTestNext(13, 15000);
                }
                break;
            }
            case 13: // the drag's last seek decoded; then a seek to the middle, paused, to see where it lands
                if (!pl.seekPending()) {
                    t.k4_completed = pl.seeksCompleted() - t.k4_completed_mark;
                    pl.pause();
                    t.seek_mark = pl.seeksCompleted();
                    t.player_seek_target = pl.durationHns() / 2;
                    t.player_seeked = pl.seek(t.player_seek_target) ? 1 : 0;
                    rollTestNext(14, 15000);
                }
                break;
            case 14:
                if (!pl.seekPending() && pl.seeksCompleted() > t.seek_mark) {
                    t.hold_until = now + 300; // its frame presented
                    rollTestNext(15, 5000);
                }
                break;
            case 15: // the count ends; then the sensor off, the player playing on
                if (now >= t.hold_until) {
                    t.player_seek_pts = pl.positionHns();
                    t.t6_live = on_frame_live - t.t6_live_mark;
                    const uint64_t handed = hooks->framesHandedToHost() - t.t6_hooks_mark;
                    t.t6_foreign = handed > t.t6_live ? handed - t.t6_live : 0;
                    pl.seek(0);
                    pl.play();
                    onCommand(ID_STOP, BN_CLICKED);
                    rollTestNext(16, 10000);
                }
                break;
            case 16: // the sensor off: Motion greys out; nothing reaches the host while the player decodes
                if (!running) {
                    refreshPet(true);
                    t.motion_greyed_text = windowText(lblMotionNote);
                    t.motion_greyed = !IsWindowEnabled(chkMotion) && !IsWindowEnabled(tbMotion) && !t.motion_greyed_text.empty() ? 1 : 0;
                    t.onframe_before = on_frame_calls;
                    t.tier_frames_before = tierFramesOut();
                    t.player_decoded_mark = pl.framesDecoded();
                    t.hold_until = now + 1500;
                    rollTestNext(17, 6000);
                }
                break;
            case 17: // the rules while it is open, and once it closes; then the player on the motion clip
                if (now >= t.hold_until) {
                    t.onframe_after = on_frame_calls;
                    t.tier_frames_after = tierFramesOut();
                    t.player_decoded_while_off = pl.framesDecoded() - t.player_decoded_mark;
                    t.player_frames = pl.framesDecoded();
                    std::wstring why;
                    t.shot_refused_player = shotAllowed(&why) ? 0 : 1;
                    pl.close();
                    t.pins_after_close = hooks->rollPcPinsHeld();
                    t.shot_refused_browser_only = shotAllowed(&why) ? 0 : 1;
                    if (!rollTestSelectClip(t.motion_id) || !rollui.playSelected()) {
                        t.failed = "the player didn't open the motion clip: " + rollui.lastError();
                        rollTestFinish();
                        return;
                    }
                    rollTestNext(18, 15000);
                }
                break;
            case 18: // a phone owner deletes the clip the player has open (as the host runs it)
                if (pl.framesPresented() >= 1) {
                    std::string err;
                    if (!hooks->startTestOwnerDelete(t.motion_id, &err)) {
                        t.failed = "the owner delete didn't start: " + err;
                        rollTestFinish();
                        return;
                    }
                    rollTestNext(19, 10000);
                }
                break;
            case 19: // the player closed first, said why, let go of the clip, and the clip is gone
                if (hooks->testOwnerDeleteDone(&t.owner_delete_result)) {
                    t.owner_delete_player_closed = !rollui.playerOpen() ? 1 : 0;
                    t.owner_delete_notice = rollui.notice();
                    t.pins_after_owner_delete = hooks->rollPcPinsHeld();
                    rollui.refreshBrowser();
                    t.owner_delete_gone = rollTestClipListed(t.motion_id) ? 0 : 1;
                    // The PC's own Delete of a clip the player has open: the player closes first, then the delete.
                    if (!rollTestSelectClip(t.manual2_id) || !rollui.playSelected()) {
                        t.failed = "the player didn't open the second manual clip: " + rollui.lastError();
                        rollTestFinish();
                        return;
                    }
                    rollTestNext(20, 15000);
                }
                break;
            case 20:
                if (pl.framesPresented() >= 1) {
                    rollTestSelectClip(t.manual2_id);
                    t.deleted = rollui.deleteSelected(false) ? 1 : 0;
                    t.pc_delete_closed_player = !rollui.playerOpen() && hooks->rollPcPinsHeld() == 0 ? 1 : 0;
                    rollui.refreshBrowser();
                    t.deleted_gone = rollTestClipListed(t.manual2_id) ? 0 : 1;
                    rollui.closeBrowser();
                    std::wstring why;
                    t.shot_allowed_after = shotAllowed(&why) ? 1 : 0;
                    rollTestFinish();
                    return;
                }
                break;
            default: break;
        }
        if (t.step > 0 && now > t.deadline) {
            char b[400];
            std::snprintf(b, sizeof(b),
                          "step %d timed out (ready=%d armed=%d recording=%d block=%d finalized=%llu fed_live=%llu player=%s)",
                          t.step, st.ready ? 1 : 0, st.armed ? 1 : 0, st.recording ? 1 : 0, static_cast<int>(st.block),
                          static_cast<unsigned long long>(st.clipsFinalized), static_cast<unsigned long long>(motion_fed_live),
                          pl.lastError().c_str());
            t.failed = b;
            rollTestFinish();
        }
    }

    void rollTestFinish() {
        RollTest& t = roll_test;
        t.done = t.failed.empty();
        t.step = -1;
        KillTimer(main, kTimerRollTest);
        motion_feed_paused = false;
        rollui.closeAll();
        refreshPet(true);
        if (!t.main_captured) {
            // The panel's camera-roll section, scrolled to; the player and the browser are closed by now.
            if (chkRoll) {
                RECT br{};
                GetWindowRect(chkRoll, &br);
                MapWindowPoints(nullptr, panelWnd, reinterpret_cast<POINT*>(&br), 2);
                scrollPanelTo(panel_scroll + br.top - S(40));
                UpdateWindow(panelWnd);
            }
            std::wstring why;
            if (shotAllowed(&why)) t.main_ok = captureWindowToPng(main, lo.screenshot_path, &t.main_err);
            else t.main_err = "refused: " + narrow(why);
            t.main_captured = true;
        }
        writeScreenshotReport(t.main_ok, t.main_err);
        if (lo.exit_after_screenshot) PostMessageW(main, WM_CLOSE, 0, 0);
    }
#endif

    // The screenshot hook's capture and report (its timer).
    void captureAndReport() {
        std::string err;
        std::wstring why;
        // Checked again now: the source, the mode or a replay may have changed since launch (a phone's
        // mode switch included). Refused: nothing of the window reaches the disk.
        const bool allowed = shotAllowed(&why);
        const bool ok = allowed && captureWindowToPng(main, lo.screenshot_path, &err);
        if (!allowed) err = "refused: " + narrow(why);
        if (allowed) captureSecondDisplayForTest();
        writeScreenshotReport(ok, err);
        if (allowed && hooks && hooks->pairingWindow()) {
            // A test run's shot of its own temporary host's pairing window (screenshots are refused otherwise): the
            // capture exclusion is lifted for this shot only, and put back. The report above has read it first.
            const HWND pw = hooks->pairingWindow();
            const bool excluded = kvwin::excludedFromCapture(pw);
            if (excluded) SetWindowDisplayAffinity(pw, WDA_NONE);
            std::string perr;
            captureWindowToPng(pw, lo.screenshot_path + L".pairing.png", &perr);
            if (excluded) kvwin::excludeFromCapture(pw);
        }
        if (lo.exit_after_screenshot) PostMessageW(main, WM_CLOSE, 0, 0);
    }

    // ---- messages ------------------------------------------------------------------------------
    int comboSel(HWND cb) { return static_cast<int>(SendMessageW(cb, CB_GETCURSEL, 0, 0)); }
    bool checked(HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; }

    void onCommand(int id, int code) {
        switch (id) {
            case ID_SOURCE:
                if (code == CBN_SELCHANGE) {
                    s.synthetic = comboSel(cbSource) == 1;
                    if (running) restartInPlace();
                    refreshEnabled();
                }
                break;
            case ID_PIPELINE:
                if (code == CBN_SELCHANGE) {
                    s.pipeline = comboSel(cbPipeline) == 1 ? KV_PIPELINE_CPU : KV_PIPELINE_OPENGL;
                    if (running) restartInPlace();
                }
                break;
            // The PC's Start/Stop and the phone's switch are the same request, so they stay in sync.
            case ID_START: power->request(true); break;
            case ID_STOP: power->request(false); break;
            case ID_MODE:
                if (code == CBN_SELCHANGE) { s.mode = static_cast<kv_view_mode_t>(comboSel(cbMode)); onViewChanged(); }
                break;
            case ID_SBS:
                if (code == CBN_SELCHANGE) { s.sbs_layout = static_cast<kv_sbs_layout_t>(comboSel(cbSbs)); onViewChanged(); }
                break;
            case ID_OVERLAY: s.clock_overlay = checked(chkOverlay); pushDisplay(); break;
            case ID_IR_AUTO: s.ir_auto_scale = checked(chkIrAuto); refreshEnabled(); pushDisplay(); break;
            case ID_RESET_IR: resetIrDefaults(s); syncControlsFromSettings(); pushDisplay(); break;
            case ID_COLORMAP:
                if (code == CBN_SELCHANGE) { s.depth_colormap = static_cast<kv_colormap_kind_t>(comboSel(cbColormap)); pushDisplay(); }
                break;
            case ID_DESK: applyDepthPreset(s, DepthPreset::Desk); syncControlsFromSettings(); pushDisplay(); break;
            case ID_SPEC: applyDepthPreset(s, DepthPreset::Spec); syncControlsFromSettings(); pushDisplay(); break;
            case ID_ROOM: applyDepthPreset(s, DepthPreset::Room); syncControlsFromSettings(); pushDisplay(); break;
            case ID_INVERT: s.depth_invert = checked(chkInvert); pushDisplay(); break;
            case ID_EXPOSURE:
                if (code == CBN_SELCHANGE) { s.exposure_mode = static_cast<kv_exposure_mode_t>(comboSel(cbExposure)); refreshEnabled(); pushColor(); }
                break;
            case ID_PAIR: petui.pairDevice(main); break; // the role is picked in the window, Viewer by default
            case ID_UNPAIR: // the old whole-PC unpair: revokes every device (v3 §2.4), after a confirm
                petui.revokeAll(true, main);
                refreshEnabled();
                break;
            case ID_PET_PRIVACY: onPrivacyClicked(); break;
            case ID_PET_SHARE: onShareClicked(); break;
            case ID_PET_SCHEDULE: petui.openSchedule(); break;
            case ID_PET_DEVICES: petui.openDevices(); break;
            case ID_PET_ID_RETRY: onIdentityRetry(); break;
            case ID_PET_ID_RESET: onIdentityReset(true); break;
            case ID_ROLL_ENABLE: onRollEnableClicked(); break;
            case ID_ROLL_MOTION: onMotionClicked(); break;
            case ID_ROLL_SCHED_ON: onRecSchedClicked(); break;
            case ID_ROLL_KEEP_APPLY: onKeepApply(); break;
            case ID_ROLL_CONFIRM_LIMITS: onRollConfirmLimits(); break;
            case ID_ROLL_RECORD: onRollRecord(); break;
            case ID_ROLL_STOP: onRollStop(); break;
            case ID_ROLL_FOLDER: onRollFolderChange(); break;
#ifdef KV_WITH_STREAMING
            case ID_ROLL_SCHED: rollui.openSchedule(); break;
            case ID_ROLL_BROWSER: rollui.openBrowser(); break;
            case ID_OUTSIDE_ALLOW: onOutsideAllowClicked(); break;
            case ID_OUTSIDE_PORT: onOutsidePort(); break;
            case ID_OUTSIDE_ADDR_SAVE: onOutsideAddressSave(); break;
            case ID_OUTSIDE_RESHARE: onOutsideReshare(); break;
            case ID_OUTSIDE_INVITE: onOutsideInvite(); break;
            case ID_OUTSIDE_LOG: outsideui.openLog(); break;
            case ID_OUTSIDE_REMOVE_MAPPING: onOutsideRemoveMapping(); break;
            case ID_OUTSIDE_REMAP: onOutsideRemap(); break;
#endif
            case ID_ALLOW_SWITCH: if (hooks) hooks->setAllowPhoneModeSwitch(checked(chkAllowSwitch)); break;
            case ID_ALLOW_POWER: if (hooks) hooks->setAllowPhoneSensorPower(checked(chkAllowPower)); break;
            case ID_MIRROR: setOrientation(checked(chkMirror), s.rotate_180); break;
            case ID_ROTATE: setOrientation(s.mirror_horizontal, checked(chkRotate)); break;
            case ID_HEALTH: setShowHealth(checked(chkHealth)); break;
            case ID_HELP: toggleHelp(); break;
            case ID_PRESET: if (code == CBN_SELCHANGE) onPresetSelected(); break;
            case ID_PRESET_SAVE: onPresetSave(); break;
            case ID_PRESET_DELETE: onPresetDelete(); break;
            case ID_OFF_ON_DISCONNECT: if (hooks) hooks->setTurnOffOnPhoneDisconnect(checked(chkOffOnDisconnect)); break;
            case ID_RECORD: toggleRecording(); updateRecordingLabel(); break;
            case ID_PC_SOURCE:
                if (code == CBN_SELCHANGE) setPointColorSource(static_cast<kv_point_color_source_t>(comboSel(cbPcSource)));
                break;
            case ID_PC_RESET: cam.reset(); cameraChanged(); break;
            case ID_CUT_ENABLE: setCutoffEnabled(checked(chkCut)); break;
            case ID_CUT_TRACKME: trackMe(); break;
            case ID_BLEND_ENABLE: setBlendEnabled(checked(chkBlend)); break;
            case ID_OUT_DISPLAY:
                if (code == CBN_SELCHANGE) {
                    const int sel = comboSel(cbOutDisplay);
                    if (sel >= 0 && sel < static_cast<int>(displays.size())) out_device = displays[static_cast<size_t>(sel)].device;
                    if (out.active()) startOutput(); // moves it there
                }
                break;
            case ID_OUT_REFRESH: refreshDisplays(); break;
            case ID_OUT_TOGGLE:
                if (out.active()) stopOutput();
                else startOutput();
                break;
            case ID_OUT_BLACKOUT: toggleBlackout(); break;
#ifdef KV_WITH_RECORDER
            case ID_RECORDINGS: openRecordingsWindow(); break;
#endif
            default: break;
        }
    }

    void onSlider(HWND tb) {
        if (tb && tb == tbMotion) { // the roll's motion sensitivity, saved in pet-camera.json
            onMotionSensitivity();
            return;
        }
        const int v = static_cast<int>(SendMessageW(tb, TBM_GETPOS, 0, 0));
        bool colour = false;
        if (tb == tbGamma) s.ir_gamma = sliderToGamma(v);
        else if (tb == tbEma) s.ir_ema_alpha = sliderToEma(v);
        else if (tb == tbBlack) s.ir_manual_min = static_cast<float>(v);
        else if (tb == tbWhite) s.ir_manual_max = static_cast<float>(v);
        else if (tb == tbDmin) s.depth_min_mm = static_cast<float>(v);
        else if (tb == tbDmax) s.depth_max_mm = static_cast<float>(v);
        else if (tb == tbEv) { s.auto_exposure_compensation = sliderToEv(v); colour = true; }
        else if (tb == tbSemi) { s.semi_auto_pseudo_ms = sliderToMs(v); colour = true; }
        else if (tb == tbManual) { s.manual_integration_ms = sliderToMs(v); colour = true; }
        else if (tb == tbGain) { s.manual_analog_gain = sliderToGain(v); colour = true; }
        else if (tb == tbPcSize) { s.point_size = sliderToPointSize(v); pc_dirty = true; }
        else if (tb == tbBlend) s.depth_color_blend_percent = static_cast<float>(v);
        else if (tb == tbCutNear || tb == tbCutFar) {
            if (tb == tbCutNear) setCutoffNear(s, sliderToCutoff(v));
            else setCutoffFar(s, sliderToCutoff(v));
            // The other slider follows (50 mm apart), and the phone hears the new range (0x07).
            SendMessageW(tbCutNear, TBM_SETPOS, TRUE, cutoffToSlider(s.cutoff_near_mm));
            SendMessageW(tbCutFar, TBM_SETPOS, TRUE, cutoffToSlider(s.cutoff_far_mm));
            publishViewState();
        }
        sanitize(s);
        refreshLabels();
        if (colour) pushColor();
        else pushDisplay();
    }

    // W4 hardening: USB unplug/replug and sleep/wake, through the power model so the phone hears
    // Error or Off as it happens. Each reopen carries the generation from the moment it was set up:
    // an Off (or On) from the phone or this PC during the settle delay wins.
    void onDeviceChange(WPARAM wp) {
        if (wp != DBT_DEVICEARRIVAL && wp != DBT_DEVICEREMOVECOMPLETE && wp != DBT_DEVNODES_CHANGED) return;
        if (s.synthetic) return;
        const bool present = kv_is_kinect_usb_present();
        if (running && !present) {
            reopen_generation = power->sensorLost("Kinect USB unplugged; waiting for it to be plugged back in.");
            reopen_missing_note = "Kinect USB unplugged; waiting for it to be plugged back in.";
            awaiting_replug = true;
            hardware_note = L"Kinect unplugged; will restart when it is plugged back in.";
        } else if (awaiting_replug && present) {
            awaiting_replug = false;
            hardware_note = L"Kinect is back; restarting.";
            SetTimer(main, kTimerReconnect, 1500, nullptr); // let USB re-enumeration settle first
        }
    }

    void onPower(WPARAM wp) {
        if (wp == PBT_APMSUSPEND) {
            reopen_generation = power->suspend(); // reports Off to the phone
            resume_after_wake = power->resumeWanted();
            hardware_note = L"Paused for sleep.";
        } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            if (resume_after_wake) {
                resume_after_wake = false;
                reopen_missing_note = "Woke from sleep; waiting for the Kinect's USB.";
                hardware_note = L"Resumed from sleep; restarting.";
                SetTimer(main, kTimerReconnect, 2500, nullptr);
            }
        }
    }

    void onReconnectTimer() {
        const bool usb = s.synthetic || kv_is_kinect_usb_present();
        if (power->resumeIfCurrent(reopen_generation, usb, reopen_missing_note)) {
            if (running) hardware_note.clear();
        } else {
            hardware_note.clear(); // superseded by a newer On/Off
        }
    }
};

// ---- window procedures ----------------------------------------------------------------------------

static ViewerApp::Impl* g_impl = nullptr;

static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (g_impl) g_impl->needs_redraw = true;
            return 0;
        }
        case WM_SIZE:
            if (g_impl) {
                g_impl->d3d.resize();
                g_impl->needs_redraw = true;
            }
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_MOUSELEAVE:
            if (g_impl) g_impl->onPreviewMouse(msg, wp, lp);
            return 0;
        case WM_MOUSEWHEEL:
            if (g_impl) g_impl->onWheel(GET_WHEEL_DELTA_WPARAM(wp), POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The control panel scrolls (it's taller than many screens); its controls' notifications go on to the
// main window, which handles them as before.
static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ViewerApp::Impl* a = g_impl;
    switch (msg) {
        case WM_COMMAND:
        case WM_HSCROLL:
        case WM_NOTIFY:
        case WM_DRAWITEM:
            return a ? SendMessageW(a->main, msg, wp, lp) : 0;
        case WM_VSCROLL: {
            if (!a) return 0;
            SCROLLINFO si{sizeof(si)};
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= a->S(24); break;
                case SB_LINEDOWN: pos += a->S(24); break;
                case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = si.nMax; break;
                default: break;
            }
            a->scrollPanelTo(pos);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (a) a->scrollPanelTo(a->panel_scroll - GET_WHEEL_DELTA_WPARAM(wp) * a->S(48) / WHEEL_DELTA);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The shortcut list (?): a small window beside the main one.
static LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            HGDIOBJ old = g_impl ? SelectObject(dc, g_impl->font) : nullptr;
            SetBkMode(dc, TRANSPARENT);
            std::wstring text;
            for (const auto& l : helpLines()) text += l + L"\n";
            InflateRect(&rc, -12, -10);
            DrawTextW(dc, text.c_str(), -1, &rc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            if (old) SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: if (g_impl && g_impl->helpWnd == hwnd) g_impl->helpWnd = nullptr; return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ViewerApp::Impl::toggleHelp() {
    if (helpWnd) {
        DestroyWindow(helpWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW hc{sizeof(hc)};
        hc.lpfnWndProc = HelpProc;
        hc.hInstance = inst;
        hc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hc.lpszClassName = L"KinectViewerHelp";
        RegisterClassExW(&hc);
        registered = true;
    }
    RECT mr{};
    GetWindowRect(main, &mr);
    helpWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"KinectViewerHelp", L"Shortcuts", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                              mr.left + S(320), mr.top + S(80), S(620), S(230), main, nullptr, inst, nullptr);
    ShowWindow(helpWnd, SW_SHOWNOACTIVATE);
}

// A small modal prompt for a preset's name (Enter saves, Esc cancels).
namespace {
struct PromptState {
    HWND edit = nullptr;
    bool done = false;
    bool ok = false;
    std::wstring text;
};
PromptState* g_prompt = nullptr;

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (g_prompt && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
                g_prompt->ok = LOWORD(wp) == IDOK;
                wchar_t b[128] = {0};
                GetWindowTextW(g_prompt->edit, b, 128);
                g_prompt->text = b;
                g_prompt->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            if (g_prompt) g_prompt->done = true;
            DestroyWindow(hwnd);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

std::optional<std::wstring> ViewerApp::Impl::askPresetName() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW pc{sizeof(pc)};
        pc.lpfnWndProc = PromptProc;
        pc.hInstance = inst;
        pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        pc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        pc.lpszClassName = L"KinectViewerPrompt";
        RegisterClassExW(&pc);
        registered = true;
    }
    PromptState st;
    g_prompt = &st;
    RECT mr{};
    GetWindowRect(main, &mr);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"KinectViewerPrompt", L"Save preset", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               mr.left + S(60), mr.top + S(200), S(300), S(130), main, nullptr, inst, nullptr);
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int yy, int w, int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(yy), S(w), S(h), dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    child(L"STATIC", L"Name for the current settings:", SS_LEFT, 12, 10, 270, 18, 0);
    st.edit = child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 12, 32, 270, 24, 100);
    SendMessageW(st.edit, EM_LIMITTEXT, 64, 0);
    child(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 120, 66, 76, 26, IDOK);
    child(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 204, 66, 78, 26, IDCANCEL);
    EnableWindow(main, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit);
    modalLoop(dlg, [&] { return st.done; }, [&](const MSG& m) {
        if (m.wParam != VK_RETURN && m.wParam != VK_ESCAPE) return false;
        SendMessageW(dlg, WM_COMMAND, m.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
        return true;
    });
    if (IsWindow(dlg)) DestroyWindow(dlg);
    EnableWindow(main, TRUE);
    SetForegroundWindow(main);
    g_prompt = nullptr;
    if (!st.ok) return std::nullopt;
    return st.text;
}
#ifdef KV_WITH_RECORDER
// ---- recordings window (phase 2) --------------------------------------------------------------------
static LRESULT CALLBACK RecordingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ViewerApp::Impl* a = g_impl;
    switch (msg) {
        case WM_COMMAND:
            if (a) a->onRecordingsCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_HSCROLL:
            if (a && reinterpret_cast<HWND>(lp) == a->recScrub) {
                a->replay_playing = false; // scrubbing pauses
                a->seekPlayback(static_cast<int>(SendMessageW(a->recScrub, TBM_GETPOS, 0, 0)));
            }
            return 0;
        case WM_NOTIFY: {
            const auto* nm = reinterpret_cast<const NMHDR*>(lp);
            if (a && nm->hwndFrom == a->recList) {
                if (nm->code == LVN_ITEMCHANGED) a->updatePlaybackControls();
                if (nm->code == NM_DBLCLK) a->startPlayback(a->selectedRecording());
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (a && a->recWnd == hwnd) {
                a->exitPlayback(); // the playback controls are here: closing them ends it
                a->recWnd = a->recList = a->recPlay = a->recDelete = a->recPause = a->recBack = a->recFwd = a->recScrub =
                    a->recExit = a->recInfo = nullptr;
            }
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ViewerApp::Impl::openRecordingsWindow() {
    if (recWnd) {
        refreshRecordingsList();
        SetForegroundWindow(recWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW rc{sizeof(rc)};
        rc.lpfnWndProc = RecordingsProc;
        rc.hInstance = inst;
        rc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        rc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        rc.lpszClassName = L"KinectViewerRecordings";
        RegisterClassExW(&rc);
        registered = true;
    }
    RECT mr{};
    GetWindowRect(main, &mr);
    recWnd = CreateWindowExW(0, L"KinectViewerRecordings", L"Recordings (stay on this PC, deleted after 7 days)",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, mr.left + S(320), mr.top + S(60), S(640),
                             S(420), main, nullptr, inst, nullptr);
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int yy, int w, int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(yy), S(w), S(h), recWnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    recList = child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 10, 10, 604, 250, 100);
    ListView_SetExtendedListViewStyle(recList, LVS_EX_FULLROWSELECT);
    const struct {
        const wchar_t* title;
        int width;
    } cols[] = {{L"Date", 130}, {L"Mode", 60}, {L"Length", 130}, {L"Size", 90}, {L"Deleted in", 120}};
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(cols[i].title);
        col.cx = S(cols[i].width);
        ListView_InsertColumn(recList, i, &col);
    }
    recPlay = child(L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP, 10, 268, 90, 26, 1);
    recDelete = child(L"BUTTON", L"Delete\u2026", BS_PUSHBUTTON | WS_TABSTOP, 106, 268, 90, 26, 2);
    recPause = child(L"BUTTON", L"Pause", BS_PUSHBUTTON | WS_TABSTOP, 10, 306, 70, 26, 3);
    recBack = child(L"BUTTON", L"\u25C0 1", BS_PUSHBUTTON | WS_TABSTOP, 86, 306, 50, 26, 4);
    recFwd = child(L"BUTTON", L"1 \u25B6", BS_PUSHBUTTON | WS_TABSTOP, 142, 306, 50, 26, 5);
    recScrub = child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 198, 306, 300, 26, 7);
    recExit = child(L"BUTTON", L"Back to live", BS_PUSHBUTTON | WS_TABSTOP, 504, 306, 110, 26, 6);
    recInfo = child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 10, 342, 604, 36, 0);
    refreshRecordingsList();
    ShowWindow(recWnd, SW_SHOW);
}

// The reminder before a recording (phase 2.4): the shared sentence, Cancel as the default (Enter and
// Esc both cancel), and "Don't remind me for 1 hour". True only for Record.
namespace {
struct ReminderState {
    HWND snooze = nullptr;
    bool done = false;
    bool record = false;
    bool snoozed = false;
};
ReminderState* g_reminder = nullptr;

LRESULT CALLBACK ReminderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (g_reminder && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
                g_reminder->record = LOWORD(wp) == IDOK;
                g_reminder->snoozed = SendMessageW(g_reminder->snooze, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_reminder->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            if (g_reminder) g_reminder->done = true; // closing is Cancel
            DestroyWindow(hwnd);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool ViewerApp::Impl::askRecordingReminder(bool* snooze) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW rc{sizeof(rc)};
        rc.lpfnWndProc = ReminderProc;
        rc.hInstance = inst;
        rc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        rc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        rc.lpszClassName = L"KinectViewerReminder";
        RegisterClassExW(&rc);
        registered = true;
    }
    ReminderState st;
    g_reminder = &st;
    RECT mr{};
    GetWindowRect(main, &mr);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"KinectViewerReminder", L"Before recording",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, mr.left + S(360), mr.top + S(220), S(420), S(170), main, nullptr,
                               inst, nullptr);
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int yy, int w, int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(yy), S(w), S(h), dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(bold), TRUE);
        return c;
    };
    child(L"STATIC", widen(kvhost::text::kRecordingReminder).c_str(), SS_LEFT | SS_NOPREFIX, 14, 12, 390, 48, 0);
    st.snooze = child(L"BUTTON", L"Don't remind me for 1 hour", BS_AUTOCHECKBOX | WS_TABSTOP, 14, 66, 300, 22, 101);
    SendMessageW(st.snooze, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    child(L"BUTTON", L"Record", BS_PUSHBUTTON | WS_TABSTOP, 196, 100, 100, 28, IDOK);
    HWND cancel = child(L"BUTTON", L"Cancel", BS_DEFPUSHBUTTON | WS_TABSTOP, 304, 100, 100, 28, IDCANCEL);
    reminder_open = true;
    EnableWindow(main, FALSE);
    if (recWnd) EnableWindow(recWnd, FALSE); // no Play or Delete under the reminder
    ShowWindow(dlg, SW_SHOW);
    SetFocus(cancel); // the default is Cancel
    modalLoop(dlg, [&] { return st.done; }, [&](const MSG& m) {
        if (m.wParam != VK_RETURN && m.wParam != VK_ESCAPE) return false;
        // Enter presses the focused button only when it's Record; otherwise Enter and Esc cancel.
        const bool record = m.wParam == VK_RETURN && GetFocus() == GetDlgItem(dlg, IDOK);
        SendMessageW(dlg, WM_COMMAND, record ? IDOK : IDCANCEL, 0);
        return true;
    });
    if (IsWindow(dlg)) DestroyWindow(dlg);
    reminder_open = false;
    if (recWnd) EnableWindow(recWnd, TRUE);
    EnableWindow(main, TRUE);
    SetForegroundWindow(main);
    g_reminder = nullptr;
    if (snooze) *snooze = st.record && st.snoozed;
    return st.record;
}
#endif

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ViewerApp::Impl* a = g_impl;
    switch (msg) {
        case WM_SIZE: if (a && a->preview) a->layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            const int dpi = a ? a->dpi : 96;
            mm->ptMinTrackSize.x = MulDiv(760, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(560, dpi, 96); // the panel scrolls
            return 0;
        }
        case WM_COMMAND: if (a) a->onCommand(LOWORD(wp), HIWORD(wp)); return 0;
        case WM_HSCROLL: if (a && lp) a->onSlider(reinterpret_cast<HWND>(lp)); return 0;
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (a && di && di->hwndItem == a->btnPrivacy) {
                a->drawPrivacyButton(*di);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            if (a && reinterpret_cast<HWND>(lp) == a->lblWatching) {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, RGB(255, 255, 255));
                SetBkColor(dc, RGB(24, 88, 160));
                if (!a->watching_brush) a->watching_brush = CreateSolidBrush(RGB(24, 88, 160));
                return reinterpret_cast<LRESULT>(a->watching_brush);
            }
            if (a && reinterpret_cast<HWND>(lp) == a->lblBanner) {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, RGB(255, 255, 255));
                SetBkColor(dc, RGB(176, 32, 32));
                if (!a->banner_brush) a->banner_brush = CreateSolidBrush(RGB(176, 32, 32));
                return reinterpret_cast<LRESULT>(a->banner_brush);
            }
            break;
        case WM_MOUSEWHEEL:
            if (a) a->onWheel(GET_WHEEL_DELTA_WPARAM(wp), POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
            return 0;
        case WM_TIMER:
            if (!a) return 0;
            if (wp == kTimerStats) {
                a->updateStatus();
                a->publishViewState(); // recording progress: the controller sends it at about 1 Hz
                a->saveLastUsedIfChanged();
#ifdef KV_WITH_RECORDER
                if (a->recWnd && ++a->list_refresh_ticks >= 240) { // the countdowns, once a minute
                    a->list_refresh_ticks = 0;
                    a->refreshRecordingsList();
                }
#endif
                if (a->hooks) a->hooks->onTick();
            } else if (wp == kTimerScreenshot) {
                KillTimer(hwnd, kTimerScreenshot);
                a->captureAndReport();
            } else if (wp == kTimerPetTest) {
                if (a->pet_test.ran == 0) a->petTestStart();
                else a->petTestTick();
            } else if (wp == kTimerIdTest) {
                if (a->id_test.ran == 0) a->idTestStart();
                else a->idTestTick();
#ifdef KV_WITH_STREAMING
            } else if (wp == kTimerRollTest) {
                if (a->roll_test.ran == 0) a->rollTestStart();
                else a->rollTestTick();
            } else if (wp == kTimerOutsideTest) {
                if (a->outside_test.ran == 0) a->outsideTestStart();
                else a->outsideTestTick();
#endif
#ifdef KV_WITH_RECORDER
            } else if (wp == kTimerRetention) {
                a->purgeRecordings();
#endif
            } else if (wp == kTimerTestTools) {
                KillTimer(hwnd, kTimerTestTools);
                a->applyTestTools();
            } else if (wp == kTimerReconnect) {
                KillTimer(hwnd, kTimerReconnect);
                a->onReconnectTimer();
            }
            return 0;
        case WM_APP_PHONE_MODE:
            if (a) {
                // The paired iPhone asked for a mode (only delivered when switching is allowed). An
                // unknown one is refused whole, and a v2 phone gets the state back so its picker snaps
                // back to the real mode.
                if (wp <= KV_MODE_POINT_CLOUD_3D) {
                    a->s.mode = static_cast<kv_view_mode_t>(wp);
                    if (lp <= KV_SBS_COLOR_IR) a->s.sbs_layout = static_cast<kv_sbs_layout_t>(lp);
                    a->syncControlsFromSettings();
                    a->onViewChanged();
                } else if (a->hooks) {
                    a->hooks->pushHostState();
                }
            }
            return 0;
        case WM_APP_PHONE_POWER:
            if (a && a->power) a->power->request(wp != 0); // in arrival order: the queue is FIFO
            return 0;
        case WM_APP_PHONE_COMMAND: // the message's own wParam and lParam are never read (ui_mailbox.hpp)
            for (auto cmd = g_phone_commands.take(); cmd; cmd = g_phone_commands.take()) {
                if (a) a->onPhoneCommand(*cmd);
            }
            return 0;
        case WM_APP_PHONE_ORBIT:
            for (auto g = g_phone_orbits.take(); g; g = g_phone_orbits.take()) {
                if (a) a->onPhoneOrbit(*g);
            }
            return 0;
        case WM_APP_STREAM_STATE: if (a) a->updateStatus(); return 0;
        case WM_APP_PET_STATE:
            g_pet_state_posted = false; // a change reported from now on posts again
            if (a) {
                a->refreshPet(true);
                a->updateStatus();
            }
            return 0;
        case WM_APP_ROLL_CLIP: // a clip was finalized: the browser shows it
            if (a) {
                a->roll_storage_forced = true;
#ifdef KV_WITH_STREAMING
                a->rollui.refreshBrowser();
#endif
                a->refreshPet(true);
            }
            return 0;
        case WM_APP_ROLL_CLIP_DELETED:
            for (auto id = g_roll_clips_deleted.take(); id; id = g_roll_clips_deleted.take()) {
#ifdef KV_WITH_STREAMING
                if (a) a->onRollClipDeletedByOwner(*id);
#endif
            }
            return 0;
        case WM_DEVICECHANGE: if (a) a->onDeviceChange(wp); return TRUE;
        case WM_POWERBROADCAST: if (a) a->onPower(wp); return TRUE;
        case WM_DISPLAYCHANGE: if (a) a->onDisplaysChanged(); return 0; // a projector plugged in or out
        case WM_CLOSE:
            if (a && a->hooks) a->hooks->markCleanStop(); // first: nothing in the teardown below is ever restarted
            if (a && a->presets) a->presets->saveLastUsed(a->s); // remembered for the next launch
            if (a) a->out.stop();
            if (a) a->petui.closeAll();
#ifdef KV_WITH_STREAMING
            if (a) a->rollui.closeAll();
            if (a) a->outsideui.closeAll();
#endif
            DestroyWindow(hwnd);
            return 0;
        case WM_ENDSESSION:
            // Logoff or shutdown: Windows ends the process after this returns, without a WM_QUIT, so the
            // message loop's detach never runs. Stop the host here, synchronously: its shutdown() tells v3
            // sessions, writes last-seen times and any save still failing, and a test run's temp state
            // folder goes (pet-camera.md §5 "saved on exit").
            if (wp && a && a->hooks) a->hooks->markCleanStop();
            if (wp && a && a->hooks) a->hooks->detach();
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- ViewerApp ------------------------------------------------------------------------------------

ViewerApp::ViewerApp() : impl_(new Impl) {}
ViewerApp::~ViewerApp() { delete impl_; }

int ViewerApp::run(HINSTANCE inst, int argc, const wchar_t* const* argv, StreamHooks* hooks) {
    Impl& a = *impl_;
    g_impl = &a;
    a.inst = inst;
    a.hooks = hooks;
    if (!parseLaunchArgs(argc, argv, a.lo, a.s)) {
        // A refused screenshot is a test hook's run: no dialog to wait on, nothing opened, and
        // the reason goes where the hook's report would have gone. So is a refused --state-dir given
        // with --screenshot (exit 2, as for any refused flag).
        if (a.lo.screenshot_refused ||
            ((a.lo.state_dir_refused || a.lo.camera_roll_refused || a.lo.outside_refused) && !a.lo.screenshot_path.empty())) {
            if (FILE* f = _wfopen((a.lo.screenshot_path + L".refused.txt").c_str(), L"wb")) {
                std::fprintf(f, "refused=%ls\n", a.lo.error.c_str());
                std::fclose(f);
            }
            return a.lo.screenshot_refused ? kScreenshotRefusedExit : 2;
        }
        if (a.lo.state_dir_refused || a.lo.camera_roll_refused || a.lo.outside_refused) {
            MessageBoxW(nullptr, a.lo.error.c_str(), L"Kinect Viewer", MB_ICONERROR);
            return 2;
        }
        MessageBoxW(nullptr, (a.lo.error + L"\n\nFlags: --synthetic --no-autostart --mode ir|color|depth|sbs|3d "
                                           L"--sbs-layout ir-depth|color-depth|color-ir --colormap turbo|viridis|inferno|gray "
                                           L"--pipeline gl|cpu --clock-overlay --point-color depth|ir|rgb --point-size PX "
                                           L"--track-me --blend PERCENT --screenshot PATH").c_str(),
                    L"Kinect Viewer", MB_ICONERROR);
        return 2;
    }

    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KinectViewerMain";
    // The app icon from phase 6's logo (resource 1, rendered from design/logo.svg at build time).
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    WNDCLASSEXW pc{sizeof(pc)};
    pc.lpfnWndProc = PreviewProc;
    pc.style = CS_DBLCLKS; // double-click resets the zoom
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.lpszClassName = L"KinectViewerPreview";
    RegisterClassExW(&pc);

    a.dpi = static_cast<int>(GetDpiForSystem());
    a.main = CreateWindowExW(0, wc.lpszClassName, L"Kinect Viewer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, a.S(1360), a.S(1000), nullptr, nullptr, inst, nullptr);
    if (!a.main) return 1;
    a.dpi = static_cast<int>(GetDpiForWindow(a.main));
    a.font = CreateFontW(-a.S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    a.bold = CreateFontW(-a.S(12), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    a.preview = CreateWindowExW(0, pc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, a.main,
                                nullptr, inst, nullptr);
    a.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, a.main,
                               nullptr, inst, nullptr);
    SendMessageW(a.status, WM_SETFONT, reinterpret_cast<WPARAM>(a.font), TRUE);
    auto strip = [&](DWORD style) {
        HWND h = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_NOPREFIX | style, 0, 0, 10, 10, a.main, nullptr, inst, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(a.font), TRUE);
        return h;
    };
    a.lblWatching = strip(SS_CENTERIMAGE | SS_ENDELLIPSIS);
    SendMessageW(a.lblWatching, WM_SETFONT, reinterpret_cast<WPARAM>(a.bold), TRUE);
    a.lblBanner = strip(SS_CENTERIMAGE);
    SetWindowTextW(a.lblBanner, (std::wstring(L"  ") + kDropBannerText).c_str());
    a.lblHealth = strip(0);
    a.lblInspect = strip(WS_VISIBLE | SS_ENDELLIPSIS);

    std::string err;
    if (!a.d3d.init(a.preview, &err)) {
        MessageBoxW(a.main, widen(err.c_str()).c_str(), L"Direct3D 11 init failed", MB_ICONERROR);
        return 1;
    }
    // The 3D view's renderer (its own device). Without it the viewer still runs; 3D says why.
    if (!a.pc.init(&a.pc_init_error) && a.pc_init_error.empty()) a.pc_init_error = "no Direct3D 11 device";

    // Presets and the last-used settings: not in a test launch (--synthetic or any test hook, v3 §10: no
    // user state read or written there, even without --screenshot). The launch flags still win over the
    // remembered settings.
    const bool test_hook = !a.lo.screenshot_path.empty();
    const bool test_launch = isTestLaunch(a.s, a.lo);
    if (!test_launch) {
        a.presets = std::make_unique<PresetsStore>(defaultPresetsDir());
        if (a.presets->lastUsed()) {
            ViewerSettings remembered;
            applyPreset(*a.presets->lastUsed(), remembered);
            LaunchOptions again;
            if (parseLaunchArgs(argc, argv, again, remembered)) a.s = remembered;
        }
    }
    a.show_health = a.lo.show_health;
    a.launch_is_test = test_launch;

    a.eng = kv_engine_create(nullptr);
    a.power = std::make_unique<kvhost::HostPowerModel>(a, [&a](const kvhost::HostPowerModel::Snapshot& p) { a.onPowerChanged(p); });
    if (hooks) a.power->setOnRequest([hooks](bool on) { hooks->onSensorRequested(on); });
    if (hooks) {
        HWND main_hwnd = a.main;
        hooks->setUiCallbacks(
            [main_hwnd](int mode, int sbs) {
                PostMessageW(main_hwnd, WM_APP_PHONE_MODE, static_cast<WPARAM>(std::max(0, mode)), static_cast<LPARAM>(std::max(0, sbs)));
            },
            [main_hwnd]() { PostMessageW(main_hwnd, WM_APP_STREAM_STATE, 0, 0); });
        hooks->setPhoneCallbacks(
            [main_hwnd](bool turn_on) { PostMessageW(main_hwnd, WM_APP_PHONE_POWER, turn_on ? 1 : 0, 0); },
            [main_hwnd](const kvhost::RemoteControlCommandMessage& cmd) {
                g_phone_commands.put(cmd);
                PostMessageW(main_hwnd, WM_APP_PHONE_COMMAND, 0, 0); // not posted: the next message takes it too
            });
        hooks->setOrbitCallback([main_hwnd](const kvhost::Orbit3DGestureMessage& g) {
            g_phone_orbits.put(g);
            PostMessageW(main_hwnd, WM_APP_PHONE_ORBIT, 0, 0);
        });
        // One WM_APP_PET_STATE at a time; however many changes the host reports meanwhile, the panel refreshes once.
        hooks->setPetStateCallback([main_hwnd]() {
            if (!g_pet_state_posted.exchange(true) && !PostMessageW(main_hwnd, WM_APP_PET_STATE, 0, 0)) g_pet_state_posted = false;
        });
#ifdef KV_WITH_STREAMING
        // A phone owner deletes a clip (a host thread): the player closes on it on the UI thread and unpins.
        a.roll_delete_hooked = hooks->setRollClipDeletedCallback([main_hwnd](const std::string& clipId) {
            g_roll_clips_deleted.put(clipId);
            PostMessageW(main_hwnd, WM_APP_ROLL_CLIP_DELETED, 0, 0);
        });
#endif
        hooks->setRollCallback([main_hwnd](const std::string&) { PostMessageW(main_hwnd, WM_APP_ROLL_CLIP, 0, 0); });
        hooks->attach(a.eng, &a.s, a.lo);
    }
    WNDCLASSEXW pnc{sizeof(pnc)};
    pnc.lpfnWndProc = PanelProc;
    pnc.hInstance = inst;
    pnc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pnc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    pnc.lpszClassName = L"KinectViewerPanel";
    RegisterClassExW(&pnc);
    a.panelWnd = CreateWindowExW(WS_EX_CONTROLPARENT, pnc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                 0, 0, a.S(320), a.S(600), a.main, nullptr, inst, nullptr);
    a.createControls();
    {
        PetCameraUi::Env env;
        env.inst = inst;
        env.main = a.main;
        env.font = a.font;
        env.bold = a.bold;
        env.dpi = a.dpi;
        env.hooks = hooks;
        env.run_modal = [&a](HWND dlg, const std::function<bool()>& done) {
            a.modalLoop(dlg, [&] { return done(); }, [dlg](const MSG& m) {
                if (m.wParam != VK_RETURN && m.wParam != VK_ESCAPE) return false;
                SendMessageW(dlg, WM_COMMAND, m.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
                return true;
            });
        };
        env.on_changed = [&a]() { a.refreshPet(true); };
        a.petui.init(env);
    }
#ifdef KV_WITH_STREAMING
    {
        RollUi::Env env;
        env.inst = inst;
        env.main = a.main;
        env.font = a.font;
        env.bold = a.bold;
        env.dpi = a.dpi;
        env.hooks = hooks;
        env.on_changed = [&a]() {
            a.roll_storage_forced = true; // a star or a delete here: the storage line now
            a.refreshPet(false);
        };
        a.rollui.init(env);
    }
    {
        OutsideUi::Env env;
        env.inst = inst;
        env.main = a.main;
        env.font = a.font;
        env.bold = a.bold;
        env.dpi = a.dpi;
        env.hooks = hooks;
        env.mask_tokens = test_launch; // v3 §10: a test run never shows a live invite token
        env.run_modal = [&a](HWND dlg, const std::function<bool()>& done) {
            a.modalLoop(dlg, [&] { return done(); }, [dlg](const MSG& m) {
                if (m.wParam != VK_RETURN && m.wParam != VK_ESCAPE) return false;
                SendMessageW(dlg, WM_COMMAND, m.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
                return true;
            });
        };
        env.on_changed = [&a](const std::wstring& note) {
            if (!note.empty()) a.outside_note = note;
            a.refreshPet(true);
        };
        a.outsideui.init(env);
    }
#endif
    a.refreshPet(true);
    a.layout();
    a.pushDisplay(); // the orientation (and the rest) before the first frame

    ShowWindow(a.main, test_hook ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
    UpdateWindow(a.main);

    a.publishViewState();
    if (a.lo.autostart) a.power->request(true);
    if (hooks && a.lo.pair_on_start) hooks->openPairingWindow(a.main);
    if (test_hook && a.lo.test_second_display) {
        // A small output window beside the main one: a test never covers a whole display.
        RECT mr{};
        GetWindowRect(a.main, &mr);
        a.out_test_rect = {mr.left + a.S(340), mr.top + a.S(120), mr.left + a.S(340) + 480, mr.top + a.S(120) + 360};
        a.startSecondDisplayForTest();
    }
    a.updateStatus();
    SetTimer(a.main, kTimerStats, 250, nullptr);
#ifdef KV_WITH_RECORDER
    // Recordings 7 days old (and old colour sessions) go at launch and once an hour; not in a test run.
    a.user_state = !test_launch;
    a.purgeRecordings();
    SetTimer(a.main, kTimerRetention, 3600 * 1000, nullptr);
#endif
    if (test_hook && a.lo.test_pet_camera && hooks) {
        // The pet camera hook runs its steps and captures when they are done (or one times out).
        SetTimer(a.main, kTimerPetTest, 100, nullptr);
#ifdef KV_WITH_STREAMING
    } else if (test_hook && a.lo.test_camera_roll && hooks) {
        // The camera roll hook: its settings, two clips, the browser and the player (or a step fails).
        SetTimer(a.main, kTimerRollTest, 100, nullptr);
    } else if (test_hook && a.lo.test_outside && hooks) {
        // The outside hook: the panel's scripted pass, then the host against the fake router (or a step fails).
        SetTimer(a.main, kTimerOutsideTest, 100, nullptr);
#endif
    } else if (test_hook && a.lo.test_identity_reset && hooks) {
        // The identity hook: the panel, Try again, the reset, then a device pairs (or a step fails).
        SetTimer(a.main, kTimerIdTest, 100, nullptr);
    } else if (test_hook) {
        SetTimer(a.main, kTimerScreenshot, static_cast<UINT>(std::max(100, a.lo.screenshot_after_ms)), nullptr);
    }
    if (test_hook && a.lo.test_tools) SetTimer(a.main, kTimerTestTools, static_cast<UINT>(std::max(100, a.lo.screenshot_after_ms - 700)), nullptr);

    MSG msg{};
    bool quit = false;
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            if (a.handleShortcutMessage(msg)) continue; // 1-5, F, M, R, Space, H, ?, Esc
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (!a.stepView()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 3, QS_ALLINPUT);
    }

    if (hooks) hooks->detach();
#ifdef KV_WITH_RECORDER
    a.exitPlayback();
#endif
    a.stopRecording();
    kv_engine_destroy(a.eng);
    a.eng = nullptr;
    a.out.stop();
    a.pc.shutdown();
    a.d3d.shutdown();
    DeleteObject(a.font);
    DeleteObject(a.bold);
    if (a.banner_brush) DeleteObject(a.banner_brush);
    CoUninitialize();
    timeEndPeriod(1);
    g_impl = nullptr;
    return static_cast<int>(msg.wParam);
}

} // namespace kvwin
