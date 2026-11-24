// Tests for the viewer's settings model (no window, no device).
#include "kv_test.hpp"
#include "outside_panel_model.hpp"
#include "pet_camera_model.hpp"
#include "ui_mailbox.hpp"
#include "viewer_model.hpp"
#include "wire_protocol.hpp" // RemoteControlCommandMessage (header only)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#undef near // windows.h's 16-bit leftovers; near() below is a float comparison
#undef far

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <process.h>
#include <string>
#include <vector>

using namespace kvwin;

namespace {
bool parse(std::vector<std::wstring> args, LaunchOptions& lo, ViewerSettings& s) {
    std::vector<const wchar_t*> argv;
    argv.push_back(L"kinect-viewer.exe");
    for (auto& a : args) argv.push_back(a.c_str());
    return parseLaunchArgs(static_cast<int>(argv.size()), argv.data(), lo, s);
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A pairing.json in the host's shape (synthetic bytes; nothing here does crypto), as an install from
// before the state-folder marker has it.
const char* const kHostPairingJson =
    "{\"serverIdentity\":{\"privateKeyRaw\":\"ERERERERERERERERERERERERERERERERERERERERERE=\","
    "\"certificateDER\":\"IiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiI=\"},\"serviceId\":\"0a1b2c3d4e5f\","
    "\"allowPhoneModeSwitch\":true,\"allowPhoneSensorPowerControl\":true,\"turnOffKinectOnPhoneDisconnect\":false}";
} // namespace

KV_TEST(defaults_match_the_core_defaults) {
    ViewerSettings s;
    kv_display_params_t core;
    kv_default_display_params(&core);
    const kv_display_params_t p = toDisplayParams(s);
    KV_CHECK(p.mode == core.mode);
    KV_CHECK(p.sbs_layout == core.sbs_layout);
    KV_CHECK(p.ir_auto_scale == core.ir_auto_scale);
    KV_CHECK(near(p.ir_gamma, core.ir_gamma));
    KV_CHECK(near(p.ir_ema_alpha, core.ir_ema_alpha));
    KV_CHECK(near(p.ir_low_percentile, core.ir_low_percentile));
    KV_CHECK(near(p.ir_high_percentile, core.ir_high_percentile));
    KV_CHECK(near(p.ir_manual_min, core.ir_manual_min));
    KV_CHECK(near(p.ir_manual_max, core.ir_manual_max));
    KV_CHECK(p.depth_colormap == core.depth_colormap);
    KV_CHECK(near(p.depth_min_mm, core.depth_min_mm));
    KV_CHECK(near(p.depth_max_mm, core.depth_max_mm));
    KV_CHECK(p.depth_invert == core.depth_invert);
    KV_CHECK(!p.clock_overlay);

    kv_color_controls_t cc;
    kv_default_color_controls(&cc);
    const kv_color_controls_t c = toColorControls(s);
    KV_CHECK(c.mode == cc.mode);
    KV_CHECK(near(c.semi_auto_pseudo_ms, cc.semi_auto_pseudo_ms));
    KV_CHECK(near(c.manual_integration_ms, cc.manual_integration_ms));
    KV_CHECK(near(c.manual_analog_gain, cc.manual_analog_gain));
}

KV_TEST(clock_overlay_only_reaches_the_core_on_synthetic) {
    ViewerSettings s;
    s.clock_overlay = true;
    s.synthetic = false;
    KV_CHECK(!toDisplayParams(s).clock_overlay);
    s.synthetic = true;
    KV_CHECK(toDisplayParams(s).clock_overlay);
}

KV_TEST(depth_presets_match_the_mac) {
    ViewerSettings s;
    applyDepthPreset(s, DepthPreset::Desk);
    KV_CHECK(near(s.depth_min_mm, 400.0f) && near(s.depth_max_mm, 2500.0f));
    applyDepthPreset(s, DepthPreset::Spec);
    KV_CHECK(near(s.depth_min_mm, 500.0f) && near(s.depth_max_mm, 4500.0f));
    applyDepthPreset(s, DepthPreset::Room);
    KV_CHECK(near(s.depth_min_mm, 500.0f) && near(s.depth_max_mm, 8000.0f));
}

KV_TEST(reset_ir_restores_mac_defaults) {
    ViewerSettings s;
    s.ir_auto_scale = false;
    s.ir_gamma = 1.2f;
    s.ir_ema_alpha = 1.0f;
    s.ir_manual_min = 100.0f;
    s.ir_manual_max = 900.0f;
    resetIrDefaults(s);
    KV_CHECK(s.ir_auto_scale);
    KV_CHECK(near(s.ir_gamma, 0.45f));
    KV_CHECK(near(s.ir_ema_alpha, 0.12f));
    KV_CHECK(near(s.ir_low_percentile, 0.01f));
    KV_CHECK(near(s.ir_high_percentile, 0.995f));
    KV_CHECK(near(s.ir_manual_min, 0.0f));
    KV_CHECK(near(s.ir_manual_max, 20000.0f));
}

KV_TEST(sanitize_keeps_ranges_valid) {
    ViewerSettings s;
    s.depth_min_mm = 5000.0f;
    s.depth_max_mm = 1000.0f;
    s.ir_gamma = 9.0f;
    s.ir_ema_alpha = 0.0f;
    s.ir_manual_min = 30000.0f;
    s.ir_manual_max = 100.0f;
    s.manual_analog_gain = 10.0f;
    s.manual_integration_ms = 0.0f;
    s.auto_exposure_compensation = -5.0f;
    sanitize(s);
    KV_CHECK(s.depth_min_mm >= 300.0f && s.depth_max_mm <= 8000.0f && s.depth_min_mm < s.depth_max_mm);
    KV_CHECK(s.ir_gamma >= 0.20f && s.ir_gamma <= 1.40f);
    KV_CHECK(s.ir_ema_alpha >= 0.05f && s.ir_ema_alpha <= 1.0f);
    KV_CHECK(s.ir_manual_min < s.ir_manual_max);
    KV_CHECK(s.manual_analog_gain >= 1.0f && s.manual_analog_gain <= 4.0f);
    KV_CHECK(s.manual_integration_ms > 0.0f && s.manual_integration_ms <= 66.0f);
    KV_CHECK(s.auto_exposure_compensation >= -2.0f && s.auto_exposure_compensation <= 2.0f);
}

KV_TEST(colour_need_and_restart_rule) {
    KV_CHECK(!modeNeedsColor(KV_MODE_IR, KV_SBS_COLOR_IR));
    KV_CHECK(!modeNeedsColor(KV_MODE_DEPTH, KV_SBS_COLOR_DEPTH));
    KV_CHECK(modeNeedsColor(KV_MODE_COLOR, KV_SBS_IR_DEPTH));
    KV_CHECK(!modeNeedsColor(KV_MODE_SIDE_BY_SIDE, KV_SBS_IR_DEPTH));
    KV_CHECK(modeNeedsColor(KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_DEPTH));
    KV_CHECK(modeNeedsColor(KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_IR));

    ViewerSettings s;
    s.mode = KV_MODE_COLOR;
    KV_CHECK(needsRestartForColor(true, false, s));
    KV_CHECK(!needsRestartForColor(true, true, s));
    KV_CHECK(!needsRestartForColor(false, false, s));
    s.mode = KV_MODE_IR;
    KV_CHECK(!needsRestartForColor(true, false, s));
}

KV_TEST(slider_mappings_round_trip) {
    KV_CHECK(near(sliderToGamma(gammaToSlider(0.45f)), 0.45f));
    KV_CHECK(near(sliderToEma(emaToSlider(0.12f)), 0.12f));
    KV_CHECK(near(sliderToEv(evToSlider(-1.5f)), -1.5f));
    KV_CHECK(near(sliderToMs(msToSlider(16.6f)), 16.6f));
    KV_CHECK(near(sliderToGain(gainToSlider(2.25f)), 2.25f));
    KV_CHECK(gammaToSlider(0.20f) == kGammaSliderMin && gammaToSlider(1.40f) == kGammaSliderMax);
}

KV_TEST(launch_flags) {
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({}, lo, s));
        KV_CHECK(!s.synthetic && lo.autostart);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--synthetic", L"--no-autostart"}, lo, s));
        KV_CHECK(s.synthetic && !lo.autostart);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--synthetic", L"--mode", L"sbs", L"--sbs-layout", L"ir-depth", L"--colormap", L"inferno",
                        L"--pipeline", L"cpu", L"--clock-overlay", L"--screenshot", L"C:\\tmp\\a.png",
                        L"--screenshot-after-ms", L"1500", L"--exit-after-screenshot"},
                       lo, s));
        KV_CHECK(s.mode == KV_MODE_SIDE_BY_SIDE && s.sbs_layout == KV_SBS_IR_DEPTH);
        KV_CHECK(s.depth_colormap == KV_COLORMAP_INFERNO);
        KV_CHECK(s.pipeline == KV_PIPELINE_CPU);
        KV_CHECK(s.clock_overlay);
        KV_CHECK(lo.screenshot_path == L"C:\\tmp\\a.png");
        KV_CHECK(lo.screenshot_after_ms == 1500);
        KV_CHECK(lo.exit_after_screenshot);
        KV_CHECK(!lo.pair_on_start && !lo.serve_loopback_only);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--loopback-only", L"--pair-on-start"}, lo, s));
        KV_CHECK(lo.serve_loopback_only && lo.pair_on_start);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--mode", L"bogus"}, lo, s));
        KV_CHECK(!lo.error.empty());
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--pipeline", L"cuda"}, lo, s)); // CUDA comes in a later step
    }
}

// The screenshot hook writes the window to disk: only the synthetic source, in a view with no colour.
KV_TEST(screenshot_hook_is_refused_for_the_real_source_and_any_view_with_colour) {
    struct Case {
        std::vector<std::wstring> args;
        bool allowed;
    };
    const std::vector<Case> cases = {
        {{L"--screenshot", L"x.png"}, false},                                   // the real source (the default)
        {{L"--real", L"--mode", L"ir", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"color", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"sbs", L"--sbs-layout", L"color-depth", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"sbs", L"--sbs-layout", L"color-ir", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"ir", L"--screenshot", L"x.png"}, true},
        {{L"--synthetic", L"--mode", L"depth", L"--screenshot", L"x.png"}, true},
        {{L"--synthetic", L"--mode", L"sbs", L"--sbs-layout", L"ir-depth", L"--screenshot", L"x.png"}, true},
        // Stage 5: the blend and RGB-coloured 3D points are live colour too, in any mode.
        {{L"--synthetic", L"--mode", L"depth", L"--blend", L"50", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"ir", L"--blend", L"0", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"3d", L"--blend", L"40", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"3d", L"--point-color", L"rgb", L"--screenshot", L"x.png"}, false},
        {{L"--real", L"--mode", L"3d", L"--screenshot", L"x.png"}, false},
        {{L"--synthetic", L"--mode", L"3d", L"--screenshot", L"x.png"}, true},
        {{L"--synthetic", L"--mode", L"3d", L"--point-color", L"ir", L"--track-me", L"--screenshot", L"x.png"}, true},
        {{L"--synthetic", L"--mode", L"depth", L"--track-me", L"--screenshot", L"x.png"}, true},
    };
    for (const auto& c : cases) {
        LaunchOptions lo;
        ViewerSettings s;
        const bool ok = parse(c.args, lo, s);
        KV_CHECK(ok == c.allowed);
        KV_CHECK(lo.screenshot_refused == !c.allowed);
        if (!c.allowed) KV_CHECK(!lo.error.empty());
        KV_CHECK(screenshotAllowed(s) == c.allowed);
    }
    // Without --screenshot, any source and mode parse as before.
    LaunchOptions lo;
    ViewerSettings s;
    KV_CHECK(parse({L"--mode", L"color"}, lo, s) && !lo.screenshot_refused);
    KV_CHECK(kScreenshotRefusedExit == 3);
}

KV_TEST(recording_rules_colour_is_live_only) {
    KV_CHECK(recordingAllowedInMode(KV_MODE_IR) && recordingAllowedInMode(KV_MODE_DEPTH));
    KV_CHECK(!recordingAllowedInMode(KV_MODE_COLOR) && !recordingAllowedInMode(KV_MODE_SIDE_BY_SIDE));
    KV_CHECK(!recordingAllowedInMode(static_cast<kv_view_mode_t>(4)));
    // What the phone is told (0x07 recordingDisabledReason): colour first, then playback, else nothing.
    KV_CHECK(recordingDisabledReason(KV_MODE_COLOR, false) == std::string("Colour is live only. Switch to IR or Depth to record."));
    KV_CHECK(recordingDisabledReason(KV_MODE_SIDE_BY_SIDE, true) == std::string("Colour is live only. Switch to IR or Depth to record."));
    KV_CHECK(recordingDisabledReason(KV_MODE_DEPTH, true) == std::string("Exit recording playback before starting a new live recording."));
    KV_CHECK(!recordingDisabledReason(KV_MODE_IR, false) && !recordingDisabledReason(KV_MODE_DEPTH, false));
    // A change of mode stops a recording; a change of anything else doesn't.
    ViewerSettings during;
    during.mode = KV_MODE_DEPTH;
    ViewerSettings after = during;
    after.depth_min_mm = 800.0f;
    after.depth_colormap = KV_COLORMAP_GRAY;
    KV_CHECK(!viewChangeStopsRecording(during, after));
    for (const kv_view_mode_t m : {KV_MODE_IR, KV_MODE_COLOR, KV_MODE_SIDE_BY_SIDE, KV_MODE_POINT_CLOUD_3D}) {
        after.mode = m;
        KV_CHECK(viewChangeStopsRecording(during, after));
    }
    // Stage 5: the cut-off doesn't stop it; the blend coming on does (the Mac's).
    after = during;
    applyTrackMe(after);
    KV_CHECK(!viewChangeStopsRecording(during, after));
    after.depth_color_blend_enabled = true;
    KV_CHECK(viewChangeStopsRecording(during, after));
}

KV_TEST(recording_with_the_blend_or_in_3d_is_refused_as_colour) {
    ViewerSettings s;
    s.mode = KV_MODE_DEPTH;
    KV_CHECK(recordingAllowed(s) && !recordingDisabledReason(s, false));
    s.depth_color_blend_enabled = true;
    KV_CHECK(!recordingAllowed(s));
    KV_CHECK(recordingDisabledReason(s, false) == std::string("Colour is live only. Switch to IR or Depth to record."));
    KV_CHECK(recordingDisabledReason(s, true) == std::string("Colour is live only. Switch to IR or Depth to record."));
    s.depth_color_blend_enabled = false;
    KV_CHECK(recordingDisabledReason(s, true) == std::string(kExitPlaybackFirst));
    s.mode = KV_MODE_POINT_CLOUD_3D; // 3D isn't recorded (IR and Depth only), with any point colour
    KV_CHECK(!recordingAllowed(s) && recordingDisabledReason(s, false).has_value());
}

KV_TEST(colour_in_the_view_counts_the_blend_and_rgb_points) {
    ViewerSettings s;
    s.mode = KV_MODE_POINT_CLOUD_3D;
    KV_CHECK(!settingsNeedColor(s));
    s.point_color_source = KV_POINT_COLOR_IR;
    KV_CHECK(!settingsNeedColor(s));
    s.point_color_source = KV_POINT_COLOR_REGISTERED_RGB;
    KV_CHECK(settingsNeedColor(s));
    KV_CHECK(needsRestartForColor(true, false, s) && !needsRestartForColor(true, true, s) && !needsRestartForColor(false, false, s));
    s = ViewerSettings{};
    s.mode = KV_MODE_IR;
    s.depth_color_blend_enabled = true;
    KV_CHECK(settingsNeedColor(s) && blendShown(s) && imageLayoutMode(s) == KV_MODE_DEPTH);
    s.mode = KV_MODE_SIDE_BY_SIDE; // the blend replaces the 2D picture in every 2D mode (512x424)
    KV_CHECK(imageLayoutMode(s) == KV_MODE_DEPTH);
    s.mode = KV_MODE_POINT_CLOUD_3D; // not in 3D, but the RGB stream still runs for it (the core's rule)
    KV_CHECK(!blendShown(s) && imageLayoutMode(s) == KV_MODE_POINT_CLOUD_3D && settingsNeedColor(s));
}

KV_TEST(cutoff_track_me_and_ranges) {
    ViewerSettings s;
    KV_CHECK(!s.cutoff_enabled && s.cutoff_near_mm == 500.0f && s.cutoff_far_mm == 2000.0f); // the Mac's defaults
    s.cutoff_near_mm = 1200.0f;
    s.cutoff_far_mm = 7000.0f;
    applyTrackMe(s);
    KV_CHECK(s.cutoff_enabled && s.cutoff_near_mm == 500.0f && s.cutoff_far_mm == 2000.0f);
    const kv_display_params_t p = toDisplayParams(s);
    KV_CHECK(p.cutoff_enabled && p.cutoff_near_mm == 500.0f && p.cutoff_far_mm == 2000.0f);
    // Moving one slider past the other drags it along, 50 mm apart.
    setCutoffNear(s, 1990.0f);
    KV_CHECK(s.cutoff_near_mm == 1990.0f && s.cutoff_far_mm == 2040.0f);
    setCutoffFar(s, 600.0f);
    KV_CHECK(s.cutoff_far_mm == 600.0f && s.cutoff_near_mm == 550.0f);
    setCutoffFar(s, 100.0f); // below its range
    KV_CHECK(s.cutoff_far_mm == 400.0f && s.cutoff_near_mm == 350.0f);
    setCutoffNear(s, 9000.0f);
    KV_CHECK(s.cutoff_near_mm == 5000.0f && s.cutoff_far_mm == 5050.0f);
    s.cutoff_near_mm = std::nanf("");
    s.point_size = 99.0f;
    s.depth_color_blend_percent = -5.0f;
    s.point_color_source = static_cast<kv_point_color_source_t>(7);
    sanitize(s);
    KV_CHECK(s.cutoff_near_mm == 500.0f && s.cutoff_far_mm >= 550.0f);
    KV_CHECK(s.point_size == 10.0f && s.depth_color_blend_percent == 0.0f && s.point_color_source == KV_POINT_COLOR_DEPTH);
    KV_CHECK(cutoffToSlider(1234.0f) == 1250 && sliderToCutoff(1260) == 1250.0f);
    KV_CHECK(pointSizeToSlider(2.5f) == 5 && sliderToPointSize(5) == 2.5f && pointSizeToSlider(0.1f) == kPointSizeSliderMin);
}

// The view part of the phone's 0x08 commands, as the viewer applies it.
KV_TEST(phone_select_mode_and_set_cutoff_through_the_command_path) {
    using kvhost::RemoteControlAction;
    using kvhost::RemoteControlCommandMessage;
    ViewerSettings s;
    s.mode = KV_MODE_DEPTH;
    s.sbs_layout = KV_SBS_IR_DEPTH;

    RemoteControlCommandMessage sel;
    sel.action = RemoteControlAction::SelectMode;
    sel.modeRaw = 4;
    KV_CHECK(applyPhoneViewCommand(s, sel) == PhoneViewChange::Mode && s.mode == KV_MODE_POINT_CLOUD_3D);
    sel.modeRaw = 3;
    sel.sbsLayoutRaw = 2;
    KV_CHECK(applyPhoneViewCommand(s, sel) == PhoneViewChange::Mode && s.mode == KV_MODE_SIDE_BY_SIDE &&
             s.sbs_layout == KV_SBS_COLOR_IR);
    for (int bad : {5, -1, 99}) { // refused whole: neither the mode nor the layout moves
        ViewerSettings before = s;
        sel.modeRaw = bad;
        sel.sbsLayoutRaw = 0;
        KV_CHECK(applyPhoneViewCommand(s, sel) == PhoneViewChange::None && s.mode == before.mode && s.sbs_layout == before.sbs_layout);
    }
    sel.modeRaw = 2;
    sel.sbsLayoutRaw = 7; // a bad layout with a good mode: the mode only
    KV_CHECK(applyPhoneViewCommand(s, sel) == PhoneViewChange::Mode && s.mode == KV_MODE_DEPTH && s.sbs_layout == KV_SBS_COLOR_IR);
    sel.modeRaw.reset();
    KV_CHECK(applyPhoneViewCommand(s, sel) == PhoneViewChange::None);

    RemoteControlCommandMessage cut;
    cut.action = RemoteControlAction::SetCutoff;
    s.cutoff_near_mm = 900.0f;
    s.cutoff_far_mm = 3000.0f;
    cut.applyTrackMePreset = true;
    cut.cutoffEnabled = false; // Track me wins
    KV_CHECK(applyPhoneViewCommand(s, cut) == PhoneViewChange::Cutoff);
    KV_CHECK(s.cutoff_enabled && s.cutoff_near_mm == 500.0f && s.cutoff_far_mm == 2000.0f);
    cut.applyTrackMePreset = false;
    KV_CHECK(applyPhoneViewCommand(s, cut) == PhoneViewChange::Cutoff && !s.cutoff_enabled && s.cutoff_far_mm == 2000.0f);
    cut.applyTrackMePreset.reset();
    cut.cutoffEnabled = true;
    KV_CHECK(applyPhoneViewCommand(s, cut) == PhoneViewChange::Cutoff && s.cutoff_enabled);
    cut.cutoffEnabled.reset();
    KV_CHECK(applyPhoneViewCommand(s, cut) == PhoneViewChange::None && s.cutoff_enabled); // nothing asked

    RemoteControlCommandMessage other; // not a view command
    other.action = RemoteControlAction::SetHostFrozen;
    other.modeRaw = 0;
    const ViewerSettings before = s;
    KV_CHECK(applyPhoneViewCommand(s, other) == PhoneViewChange::None && s.mode == before.mode);
}

KV_TEST(blend_and_point_settings_reach_the_core) {
    ViewerSettings s;
    s.mode = KV_MODE_POINT_CLOUD_3D;
    s.depth_color_blend_enabled = true;
    s.depth_color_blend_percent = 25.0f;
    s.point_color_source = KV_POINT_COLOR_IR;
    const kv_display_params_t p = toDisplayParams(s);
    KV_CHECK(p.mode == KV_MODE_POINT_CLOUD_3D && p.depth_color_blend_enabled && p.depth_color_blend == 0.25f);
    KV_CHECK(p.point_color_source == KV_POINT_COLOR_IR);
    LaunchOptions lo;
    ViewerSettings f;
    KV_CHECK(parse({L"--mode", L"3d", L"--point-color", L"ir", L"--point-size", L"6", L"--track-me"}, lo, f));
    KV_CHECK(f.mode == KV_MODE_POINT_CLOUD_3D && f.point_color_source == KV_POINT_COLOR_IR && f.point_size == 6.0f && f.cutoff_enabled);
    KV_CHECK(!parse({L"--point-color", L"purple"}, lo, f));
    KV_CHECK(parse({L"--mode", L"pointcloud", L"--blend", L"150"}, lo, f) && f.depth_color_blend_percent == 100.0f);
}

// v3 §10 "Test isolation": --state-dir, and which launches count as test runs (they never touch the
// real .host-state).
KV_TEST(state_dir_flag_and_test_launches) {
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--state-dir", L"C:/kv-test/state/"}, lo, s));
        KV_CHECK(lo.state_dir == L"C:\\kv-test\\state"); // CTest's forward slashes, no trailing one
        KV_CHECK(!isTestLaunch(s, lo));                    // a plain real launch is not a test run
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--state-dir"}, lo, s) && !lo.error.empty()); // a value is required
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--synthetic", L"--test-pet-camera", L"--screenshot", L"x.png"}, lo, s));
        KV_CHECK(lo.test_pet_camera && lo.serve_loopback_only); // its fake device is on this PC only
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--synthetic", L"--test-identity-reset", L"--screenshot", L"x.png"}, lo, s));
        KV_CHECK(lo.test_identity_reset && lo.serve_loopback_only);
    }
    const std::vector<std::vector<std::wstring>> test_runs = {
        {L"--synthetic"},
        {L"--screenshot", L"x.png", L"--synthetic"},
        {L"--exit-after-screenshot"},
        {L"--loopback-only"},
        {L"--pair-on-start"},
        {L"--test-tools"},
        {L"--test-second-display"},
        {L"--test-pet-camera"},
        {L"--test-identity-reset"},
    };
    for (const auto& args : test_runs) {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse(args, lo, s));
        KV_CHECK(isTestLaunch(s, lo));
    }
    LaunchOptions lo;
    ViewerSettings s;
    KV_CHECK(parse({L"--no-autostart", L"--mode", L"depth", L"--show-health"}, lo, s));
    KV_CHECK(!isTestLaunch(s, lo));
    KV_CHECK(normalizeStateDir(L"").empty());
}

KV_TEST(temporary_state_dir_is_per_process_and_only_it_is_removed) {
    const std::wstring tmp = temporaryStateDir(L"kv-model-test");
    KV_CHECK(tmp.find(L"\\kv-model-test-") != std::wstring::npos);
    KV_CHECK(tmp.size() > 12 && tmp.substr(tmp.size() - 12) == L"\\.host-state");
    namespace fs = std::filesystem;
    const fs::path root = fs::path(tmp).parent_path();
    fs::create_directories(tmp);
    { std::ofstream(fs::path(tmp) / "pairing.json") << "{}"; }
    // A folder that isn't this process's own temp folder is never removed.
    const fs::path other = root.parent_path() / (root.filename().wstring() + L"-other");
    fs::create_directories(other / ".host-state");
    KV_CHECK(!removeTemporaryStateDir((other / ".host-state").wstring(), L"kv-model-test"));
    KV_CHECK(fs::exists(other));
    KV_CHECK(!removeTemporaryStateDir(tmp, L"kv-other-prefix"));
    KV_CHECK(fs::exists(tmp));
    KV_CHECK(removeTemporaryStateDir(tmp, L"kv-model-test"));
    KV_CHECK(!fs::exists(root));
    std::error_code ec;
    fs::remove_all(other, ec);
}

// The host makes its state folder owner-only with a protected, inheritable DACL, which pushes down to
// everything already in it. So --state-dir takes only a folder that doesn't exist yet, an empty one, or
// one that already holds host state; never a drive root (or a share's root), never a folder of other
// files. The launch is refused before anything starts.
KV_TEST(state_dir_must_be_new_empty_or_host_state_and_never_a_root) {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / (L"kv-model-statedir-" + std::to_wstring(_getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    std::wstring why;
    KV_CHECK(stateDirAcceptable((base / L"not-yet").wstring(), &why));
    fs::create_directories(base / L"empty");
    KV_CHECK(stateDirAcceptable((base / L"empty").wstring(), &why));
    // A folder the host already keeps its state in: a pairing.json that reads as the host's (an existing
    // install, from before the marker), or the host's marker. Anything else beside them is fine.
    {
        const fs::path d = base / L"install-copy";
        fs::create_directories(d);
        { std::ofstream(d / L"pairing.json") << kHostPairingJson; }
        { std::ofstream(d / L"devices.json") << "{\"version\":1,\"devices\":[]}"; }
        { std::ofstream(d / L"pet-camera.json") << "{}"; }
        { std::ofstream(d / L"presets.json") << "{}"; }
        KV_CHECK_MSG(stateDirAcceptable(d.wstring(), &why), d.string());
    }
    {
        const fs::path d = base / L"has-marker";
        fs::create_directories(d);
        { std::ofstream(d / L".kinect-viewer-host-state") << "kinect-viewer host state\n"; }
        { std::ofstream(d / L"presets.json") << "{}"; }
        KV_CHECK_MSG(stateDirAcceptable(d.wstring(), &why), d.string());
    }
    // A file name alone proves nothing: a folder that happens to hold a devices.json, a pet-camera.json,
    // or a pairing.json that isn't the host's, is refused.
    for (const wchar_t* f : {L"pairing.json", L"devices.json", L"pet-camera.json"}) {
        const fs::path d = base / (std::wstring(L"has-") + f);
        fs::create_directories(d);
        { std::ofstream(d / f) << "{}"; }
        { std::ofstream(d / L"notes.txt") << "x"; }
        why.clear();
        KV_CHECK_MSG(!stateDirAcceptable(d.wstring(), &why) && !why.empty(), d.string());
    }
    // A folder of other things: refused, with the reason.
    fs::create_directories(base / L"documents");
    { std::ofstream(base / L"documents" / L"notes.txt") << "x"; }
    why.clear();
    KV_CHECK(!stateDirAcceptable((base / L"documents").wstring(), &why) && !why.empty());
    fs::create_directories(base / L"tree" / L"sub");
    KV_CHECK(!stateDirAcceptable((base / L"tree").wstring(), &why));
    KV_CHECK(!stateDirAcceptable((base / L"documents" / L"notes.txt").wstring(), &why)); // a file
    // Roots, however written.
    why.clear();
    KV_CHECK(!stateDirAcceptable(L"C:\\", &why) && why.find(L"root") != std::wstring::npos);
    KV_CHECK(!stateDirAcceptable(normalizeStateDir(L"C:/"), &why));
    KV_CHECK(!stateDirAcceptable(L"Z:\\", &why));
    KV_CHECK(!stateDirAcceptable(L"\\\\server\\share", &why));
    KV_CHECK(!stateDirAcceptable(L"\\\\server\\share\\", &why));
    KV_CHECK(!stateDirAcceptable(L"", &why));
    // The launch flag: refused at parse time, so the host never starts on it.
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--synthetic", L"--state-dir", (base / L"documents").wstring()}, lo, s));
        KV_CHECK(lo.state_dir_refused && !lo.error.empty() && !lo.screenshot_refused);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--state-dir", L"C:/"}, lo, s) && lo.state_dir_refused);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--state-dir", (base / L"install-copy").wstring()}, lo, s) && !lo.state_dir_refused);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--synthetic", L"--state-dir", (base / L"has-devices.json").wstring()}, lo, s) &&
                 lo.state_dir_refused);
    }
    KV_CHECK(fs::exists(base / L"documents" / L"notes.txt"));
    fs::remove_all(base, ec);
}

namespace {
// The bytes of a file, or nullopt when it can't be read.
std::optional<std::string> fileBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The DACL (and its protection flag) as SDDL, to tell whether anything re-ACLed the path.
std::wstring daclOf(const std::filesystem::path& p) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(p.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                              nullptr, &sd) != ERROR_SUCCESS) {
        return L"?";
    }
    LPWSTR s = nullptr;
    std::wstring out = L"?";
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &s, nullptr)) {
        out = s;
        LocalFree(s);
    }
    LocalFree(sd);
    return out;
}
} // namespace

// An unrelated folder that happens to hold a devices.json is refused, and the check leaves it exactly as
// it was: every byte, and the folder's and file's ACLs. A junction (any reparse point) is refused even
// when what it points at would be fine, since the owner-only DACL would land wherever it leads.
KV_TEST(state_dir_refuses_an_unrelated_devices_json_folder_and_a_junction) {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / (L"kv-model-statedir2-" + std::to_wstring(_getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / L"unrelated");
    const std::string devices = "{\"devices\":[\"someone else's\"],\"note\":\"not the host's\"}\r\n";
    { std::ofstream(base / L"unrelated" / L"devices.json", std::ios::binary) << devices; }
    { std::ofstream(base / L"unrelated" / L"notes.txt", std::ios::binary) << "keep me"; }
    const std::wstring dacl_dir = daclOf(base / L"unrelated");
    const std::wstring dacl_file = daclOf(base / L"unrelated" / L"devices.json");
    std::wstring why;
    KV_CHECK_MSG(!stateDirAcceptable((base / L"unrelated").wstring(), &why), "a folder with someone else's devices.json was taken");
    KV_CHECK(!why.empty());
    KV_CHECK(fileBytes(base / L"unrelated" / L"devices.json") == devices);
    KV_CHECK(fileBytes(base / L"unrelated" / L"notes.txt") == std::string("keep me"));
    KV_CHECK(daclOf(base / L"unrelated") == dacl_dir && daclOf(base / L"unrelated" / L"devices.json") == dacl_file);
    size_t n = 0;
    for (auto it = fs::directory_iterator(base / L"unrelated"); it != fs::directory_iterator(); ++it) ++n;
    KV_CHECK(n == 2);

    // A junction to an empty folder (which itself would be fine).
    fs::create_directories(base / L"target");
    KV_CHECK(stateDirAcceptable((base / L"target").wstring(), &why));
    const std::wstring cmd = L"cmd /c mklink /J \"" + (base / L"junction").wstring() + L"\" \"" + (base / L"target").wstring() +
                             L"\" >nul";
    KV_REQUIRE(_wsystem(cmd.c_str()) == 0);
    KV_REQUIRE(GetFileAttributesW((base / L"junction").wstring().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
    why.clear();
    KV_CHECK_MSG(!stateDirAcceptable((base / L"junction").wstring(), &why), "a junction was taken");
    KV_CHECK(!why.empty());
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--synthetic", L"--state-dir", (base / L"junction").wstring()}, lo, s) && lo.state_dir_refused);
    }
    RemoveDirectoryW((base / L"junction").wstring().c_str()); // the link only, never what it points at
    KV_CHECK(fs::exists(base / L"target"));
    fs::remove_all(base, ec);
}

// A run killed before its clean exit leaves %TEMP%\<prefix>-<pid> behind; a later run that gets the same
// PID must start from an empty folder, not the leftover pairing state.
KV_TEST(a_leftover_temp_state_folder_is_cleared_before_a_run_uses_it) {
    namespace fs = std::filesystem;
    const std::wstring tmp = temporaryStateDir(L"kv-model-fresh");
    fs::create_directories(tmp);
    { std::ofstream(fs::path(tmp) / L"devices.json") << "{\"devices\":[]}"; }
    std::wstring why;
    KV_CHECK(freshTemporaryStateDir(L"kv-model-fresh", &why) == tmp);
    KV_CHECK_MSG(!fs::exists(fs::path(tmp) / L"devices.json"), "the leftover state was kept");
    KV_CHECK(!fs::exists(fs::path(tmp).parent_path()));
    KV_CHECK(freshTemporaryStateDir(L"kv-model-fresh", &why) == tmp); // nothing left over: fine too
    KV_CHECK(why.empty());
}

// v3 §10: the screenshot hook (and the second-display capture) also refuses while a depth/IR
// recording replays in the view, whatever the view.
KV_TEST(screenshot_hook_is_refused_while_a_replay_is_on_screen) {
    ViewerSettings s;
    s.synthetic = true;
    s.mode = KV_MODE_DEPTH;
    KV_CHECK(screenshotAllowed(s));
    KV_CHECK(screenshotAllowed(s, false));
    std::wstring why;
    KV_CHECK(!screenshotAllowed(s, true, &why));
    KV_CHECK(why.find(L"replay") != std::wstring::npos);
    s.mode = KV_MODE_IR;
    KV_CHECK(!screenshotAllowed(s, true));
    s.mode = KV_MODE_POINT_CLOUD_3D;
    KV_CHECK(!screenshotAllowed(s, true));
}

// ---- the pet camera panel's text (pet-camera.md §4) -------------------------------------------------

KV_TEST(watching_now_banner_text) {
    KV_CHECK(watchingNowText({}).empty()); // hidden only when nobody watches
    kvhost::PetWatcher a;
    a.deviceId = "0011223344556677";
    a.deviceName = "iPhone";
    kvhost::PetWatcher b = a;
    b.deviceName = "iPhone 2";
    b.path = kvhost::PetPath::Outside;
    KV_CHECK(watchingNowText({a}) == L"Watching now: iPhone");
    KV_CHECK(watchingNowText({a, b}) == L"Watching now: iPhone, iPhone 2 (outside the home)"); // the plan's example
    kvhost::PetWatcher c = a;
    c.deviceName = "Caf\xC3\xA9 iPad"; // UTF-8 from the device list
    c.activity = kvhost::watcher_activity::kPlayback;
    KV_CHECK(watchingNowText({c}) == L"Watching now: Caf\u00E9 iPad (a clip)");
    kvhost::PetWatcher unnamed;
    unnamed.deviceId = "abcdef0123456789";
    KV_CHECK(watchingNowText({unnamed}) == L"Watching now: abcdef0123456789");
}

KV_TEST(stale_owners_are_30_days_unseen_and_not_connected) {
    const uint64_t day = 24ull * 3600 * 1000;
    const uint64_t now = 1800000000000ull;
    kvhost::HostDeviceInfo d;
    d.role = kvhost::PetRole::Owner;
    d.pairedAtUnixMs = now - 100 * day;
    d.lastSeenUnixMs = now - 31 * day;
    KV_CHECK(isStaleOwner(d, now));
    d.lastSeenUnixMs = now - 29 * day;
    KV_CHECK(!isStaleOwner(d, now));
    d.lastSeenUnixMs.reset(); // never connected: counted from pairing
    KV_CHECK(isStaleOwner(d, now));
    d.pairedAtUnixMs = now - 2 * day;
    KV_CHECK(!isStaleOwner(d, now));
    d.pairedAtUnixMs = now - 100 * day;
    d.connected = true; // connected now is seen now
    KV_CHECK(!isStaleOwner(d, now));
    d.connected = false;
    d.role = kvhost::PetRole::Viewer; // only owners are highlighted
    KV_CHECK(!isStaleOwner(d, now));
    KV_CHECK(lastSeenText(d, now) == L"never");
    d.lastSeenUnixMs = now - 3 * day;
    KV_CHECK(lastSeenText(d, now) == L"3 days ago");
    d.lastSeenUnixMs = now - 5 * 60 * 1000;
    KV_CHECK(lastSeenText(d, now) == L"5 min ago");
    d.connected = true;
    d.path = "home";
    d.watching = true;
    KV_CHECK(lastSeenText(d, now) == L"now");
    KV_CHECK(connectionText(d) == L"home, watching");
    d.watching = false;
    d.unavailableReason = kvhost::live_unavailable::kSharingOff;
    KV_CHECK(connectionText(d) == L"home, not watching (sharing is off)");
    d.connected = false;
    KV_CHECK(connectionText(d).empty());
    KV_CHECK(roleLabel(kvhost::PetRole::Owner) == L"Owner" && roleLabel(kvhost::PetRole::Viewer) == L"Viewer");
    KV_CHECK(kindLabel("ipad") == L"iPad" && kindLabel("weird") == L"Other");
}

KV_TEST(sharing_schedule_rows_text_and_clock_parsing) {
    KV_CHECK(daysText(0x7F) == L"Every day");
    KV_CHECK(daysText(0x3E) == L"Weekdays");
    KV_CHECK(daysText(0x41) == L"Weekends");
    KV_CHECK(daysText(0x2A) == L"Mon, Wed, Fri");
    KV_CHECK(daysText(0x03) == L"Mon, Sun"); // Monday first, Sunday last
    kvhost::SharingScheduleRow r;
    r.days = 0x3E;
    r.fromMinute = 22 * 60;
    r.toMinute = 7 * 60;
    KV_CHECK(scheduleRowText(r) == L"Weekdays 22:00\u201307:00 (overnight)");
    r.days = 0x7F;
    r.fromMinute = r.toMinute = 0;
    KV_CHECK(scheduleRowText(r) == L"Every day 00:00\u201300:00 (24 hours)");
    r.fromMinute = 9 * 60 + 5;
    r.toMinute = 17 * 60 + 30;
    KV_CHECK(scheduleRowText(r) == L"Every day 09:05\u201317:30");
    KV_CHECK(scheduleSummary({}) == L"none" && scheduleSummary({r}) == L"1 row" && scheduleSummary({r, r}) == L"2 rows");
    KV_CHECK(parseClockText(L"22:00") == std::optional<uint16_t>(1320));
    KV_CHECK(parseClockText(L" 7:30 ") == std::optional<uint16_t>(450));
    KV_CHECK(parseClockText(L"00:00") == std::optional<uint16_t>(0));
    KV_CHECK(parseClockText(L"23:59") == std::optional<uint16_t>(1439));
    for (const wchar_t* bad : {L"24:00", L"12:60", L"1200", L"12:5", L"", L"ab:cd", L"123:00", L":30"}) {
        KV_CHECK(!parseClockText(bad));
    }
    KV_CHECK(clockText(450) == L"07:30");
}

KV_TEST(tier_diagnostics_line_names_each_encoder) {
    KV_CHECK(tierDiagnosticsLine({}).empty());
    kvhost::HostTierStatus hi;
    hi.tier = "high";
    hi.active = hi.open = true;
    hi.encoderName = "Synthetic MFT";
    hi.hardware = true;
    hi.width = 1024;
    hi.height = 424;
    hi.epoch = 3;
    hi.sessions = 1;
    kvhost::HostTierStatus med;
    med.tier = "medium";
    kvhost::HostTierStatus low;
    low.tier = "low";
    low.active = low.open = true;
    low.encoderName = "Microsoft H264 MFT";
    low.width = 640;
    low.height = 265;
    low.epoch = 4;
    KV_CHECK(tierDiagnosticsLine({hi, med, low}) ==
             L"Live tiers: high Synthetic MFT (hw) 1024x424 e3, 1 session | medium off | "
             L"low Microsoft H264 MFT (sw) 640x265 e4, 0 sessions, held");
    hi.encoderName = "NVIDIA H.264 Encoder MFT (hardware)"; // the tier already says which: not twice
    KV_CHECK(tierDiagnosticsLine({hi}) == L"Live tiers: high NVIDIA H.264 Encoder MFT (hardware) 1024x424 e3, 1 session");
}

// ---- W7: the camera roll on the PC (pet-camera.md §4, §6; v3 §10 "Test isolation") ------------------------

KV_TEST(camera_roll_flags_need_synthetic_and_a_folder_of_the_tests_own) {
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--synthetic", L"--camera-roll-dir", L"C:/kv-test/roll/", L"--test-camera-roll", L"--screenshot",
                        L"x.png"},
                       lo, s));
        KV_CHECK(lo.camera_roll_dir == L"C:\\kv-test\\roll"); // CTest's forward slashes, no trailing one
        KV_CHECK(lo.test_camera_roll && lo.serve_loopback_only); // nothing listens on the LAN
        KV_CHECK(isTestLaunch(s, lo));
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--synthetic", L"--camera-roll-dir", L"C:/kv-test/roll"}, lo, s)); // the dir alone is a test run
        KV_CHECK(isTestLaunch(s, lo) && !lo.test_camera_roll);
    }
    struct Refused {
        std::vector<std::wstring> args;
        const wchar_t* says;
    };
    const std::wstring home = [] {
        wchar_t b[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"USERPROFILE", b, MAX_PATH);
        return std::wstring(b);
    }();
    const std::vector<Refused> refused = {
        {{L"--test-camera-roll", L"--camera-roll-dir", L"C:/kv-test/roll", L"--no-autostart"}, L"--synthetic"},
        {{L"--synthetic", L"--test-camera-roll"}, L"--camera-roll-dir"},
        {{L"--camera-roll-dir", L"C:/kv-test/roll", L"--no-autostart"}, L"synthetic"},
        {{L"--synthetic", L"--camera-roll-dir", home + L"\\kinect-viewer-camera-roll"}, L"real camera roll"}, // the default
        {{L"--synthetic", L"--camera-roll-dir", home + L"\\kinect-viewer-camera-roll\\2026-11-21"}, L"real camera roll"},
        {{L"--synthetic", L"--camera-roll-dir", home}, L"recordings"}, // it holds the real folders (the first listed)
        {{L"--synthetic", L"--camera-roll-dir", home + L"\\kinect-viewer-recordings"}, L"recordings"},
        {{L"--synthetic", L"--camera-roll-dir", home + L"\\kinect-viewer-recordings\\roll"}, L"recordings"},
        {{L"--synthetic", L"--camera-roll-dir", home + L"\\kinect-viewer\\.host-state"}, L"host state"},
        {{L"--synthetic", L"--camera-roll-dir", L"E:\\"}, L"drive root"},
        {{L"--synthetic", L"--camera-roll-dir", L"\\\\server\\share\\roll"}, L"local drive"},
    };
    for (const auto& r : refused) {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse(r.args, lo, s));
        KV_CHECK(lo.camera_roll_refused);
        KV_CHECK(lo.error.find(r.says) != std::wstring::npos);
    }
    // A folder that merely shares a prefix with a real one is not inside it.
    KV_CHECK(cameraRollTestDirAcceptable(L"E:\\kv-tests\\camera-roll-test", nullptr));
    KV_CHECK(!pathIsSameOrInside(L"E:\\kv-tests\\camera-roll-test", L"E:\\kv-tests\\camera-roll"));
    KV_CHECK(pathIsSameOrInside(L"E:/KV-Tests/Camera-Roll/x/", L"e:\\kv-tests\\camera-roll"));
    KV_CHECK(knownRecordingsDir() == home + L"\\kinect-viewer-recordings");
}

// A test's --camera-roll-dir must be new, empty, or hold .kinect-viewer-test-roll.
// Only temp folders here: a fake roll is made to look like a real one (day folders and the roll's markers), and no
// real folder is named, opened or listed.
KV_TEST(camera_roll_test_dir_must_be_new_empty_or_a_test_roll) {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / (L"kv-model-rolldir-" + std::to_wstring(_getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    auto acceptable = [](const fs::path& p, std::wstring* why) {
        LaunchOptions lo;
        ViewerSettings s;
        const bool ok = parse({L"--synthetic", L"--test-camera-roll", L"--camera-roll-dir", p.wstring()}, lo, s);
        if (why) *why = lo.error;
        KV_CHECK(ok || lo.camera_roll_refused);
        return ok;
    };
    std::wstring why;
    KV_CHECK(acceptable(base / L"new", &why));             // not there yet: the host creates it
    KV_CHECK(acceptable(base / L"new" / L"deeper", &why)); // its parent isn't there either
    fs::create_directories(base / L"empty");
    KV_CHECK(acceptable(base / L"empty", &why));
    // A test roll from an earlier run: the marker, and clips beside it.
    fs::create_directories(base / L"test-roll" / L"2026-11-18");
    { std::ofstream(base / L"test-roll" / L".kinect-viewer-test-roll", std::ios::binary) << "kinect-viewer test roll\n"; }
    { std::ofstream(base / L"test-roll" / L".nosync", std::ios::binary) << ""; }
    KV_CHECK(acceptable(base / L"test-roll", &why));
    // A folder that looks like a real roll: day folders, .nosync and .metadata_never_index, a clip; no test marker.
    const fs::path fake = base / L"looks-real";
    fs::create_directories(fake / L"2026-11-17");
    fs::create_directories(fake / L"2026-11-18");
    { std::ofstream(fake / L".nosync", std::ios::binary) << ""; }
    { std::ofstream(fake / L".metadata_never_index", std::ios::binary) << ""; }
    { std::ofstream(fake / L"2026-11-18" / L"2026-11-18_101500_motion.mp4", std::ios::binary) << "not a real clip"; }
    KV_CHECK(!acceptable(fake, &why));
    KV_CHECK(why.find(L".kinect-viewer-test-roll") != std::wstring::npos && why.find(L"real camera roll") != std::wstring::npos);
    // Anything else in the folder counts too; so does a folder (not a file) named like the marker.
    fs::create_directories(base / L"other");
    { std::ofstream(base / L"other" / L"notes.txt", std::ios::binary) << "keep me"; }
    KV_CHECK(!acceptable(base / L"other", &why));
    fs::create_directories(base / L"marker-dir" / L".kinect-viewer-test-roll");
    KV_CHECK(!acceptable(base / L"marker-dir", &why));
    // A file, not a folder.
    { std::ofstream(base / L"a-file", std::ios::binary) << "x"; }
    KV_CHECK(!acceptable(base / L"a-file", &why) && why.find(L"not a folder") != std::wstring::npos);
    // Compared by the path the file system resolves: a junction to the fake roll is the fake roll, and a folder
    // not made yet under the junction resolves under its target.
    const fs::path link = base / L"link";
    const std::wstring cmd = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + fake.wstring() + L"\" >nul";
    const bool junction_made = _wsystem(cmd.c_str()) == 0 && fs::exists(link / L".nosync");
    KV_CHECK(junction_made); // mklink /J needs no elevation
    if (junction_made) {
        KV_CHECK(!acceptable(link, &why));
        const auto r = resolvedFolderPath((link / L"new").wstring(), &why);
        const auto target = resolvedFolderPath(fake.wstring(), &why); // %TEMP% itself may be an 8.3 name
        KV_CHECK(r && target && pathIsSameOrInside(*r, *target) && r->size() > 4 && r->substr(r->size() - 4) == L"\\new");
        KV_CHECK(!pathIsSameOrInside((link / L"new").wstring(), fake.wstring())); // the text alone doesn't say so
    }
    fs::remove(link, ec); // the junction only, never its target's contents
    fs::remove_all(base, ec);
}

KV_TEST(camera_roll_folder_for_a_launch) {
    // v3 §10: a test launch records only into --camera-roll-dir, or nowhere; the setting is never used then.
    KV_CHECK(cameraRollDirForLaunch(true, L"", "E:\\somewhere").empty());
    KV_CHECK(cameraRollDirForLaunch(true, L"C:\\kv-test\\roll", "E:\\somewhere") == L"C:\\kv-test\\roll");
    // A real launch with the rollFolder setting set: that folder.
    KV_CHECK(cameraRollDirForLaunch(false, L"", "E:/Pets/roll/") == L"E:\\Pets\\roll");
}

// pet-camera.md §4: a real launch's camera roll, the stored folder or else the profile's. The default is passed
// in, so no test looks at a real folder.
KV_TEST(a_real_launch_uses_the_stored_folder_and_otherwise_the_profile) {
    const std::wstring fresh = L"C:\\Users\\someone\\kinect-viewer-camera-roll";
    // A folder is stored: used as it is.
    KV_CHECK(realLaunchRollFolder("E:/Pets/roll/", fresh) == L"E:\\Pets\\roll");
    // None stored: the profile's folder.
    KV_CHECK(realLaunchRollFolder("", fresh) == fresh);
    // No USERPROFILE, so no default: no roll rather than a guess.
    KV_CHECK(realLaunchRollFolder("", L"").empty());
    // Today's default is in the profile, beside the depth/IR recordings, and outside OneDrive.
    wchar_t b[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"USERPROFILE", b, MAX_PATH);
    KV_CHECK(defaultCameraRollDir() == std::wstring(b) + L"\\kinect-viewer-camera-roll");
}

KV_TEST(camera_roll_folder_change_refuses_onedrive_the_recordings_and_network_drives) {
    const std::wstring rec = L"C:\\Users\\someone\\kinect-viewer-recordings";
    const std::vector<std::wstring> od = {L"C:\\Users\\someone\\OneDrive"};
    std::wstring why;
    KV_CHECK(rollFolderChoiceAcceptable(L"E:\\Pets\\roll", rec, od, DRIVE_FIXED, &why));
    KV_CHECK(rollFolderChoiceAcceptable(L"F:\\roll", rec, od, DRIVE_REMOVABLE, &why));
    KV_CHECK(!rollFolderChoiceAcceptable(L"C:\\Users\\someone\\OneDrive\\Pets", rec, od, DRIVE_FIXED, &why) &&
             why.find(L"OneDrive") != std::wstring::npos);
    KV_CHECK(!rollFolderChoiceAcceptable(rec, rec, od, DRIVE_FIXED, &why) && why.find(L"recordings") != std::wstring::npos);
    KV_CHECK(!rollFolderChoiceAcceptable(rec + L"\\roll", rec, od, DRIVE_FIXED, &why));
    KV_CHECK(!rollFolderChoiceAcceptable(L"C:\\Users\\someone", rec, od, DRIVE_FIXED, &why)); // it holds the recordings
    KV_CHECK(!rollFolderChoiceAcceptable(L"Z:\\roll", rec, od, DRIVE_REMOTE, &why) && why.find(L"network") != std::wstring::npos);
    KV_CHECK(!rollFolderChoiceAcceptable(L"\\\\nas\\pets", rec, od, DRIVE_FIXED, &why) && why.find(L"network") != std::wstring::npos);
    KV_CHECK(!rollFolderChoiceAcceptable(L"Q:\\roll", rec, od, DRIVE_NO_ROOT_DIR, &why));
    KV_CHECK(!rollFolderChoiceAcceptable(L"E:\\", rec, od, DRIVE_FIXED, &why));
    KV_CHECK(!rollFolderChoiceAcceptable(L"", rec, od, DRIVE_FIXED, &why));
}

KV_TEST(screenshot_is_refused_while_a_roll_clip_is_on_screen) {
    ViewerSettings s;
    s.synthetic = true;
    s.mode = KV_MODE_DEPTH;
    std::wstring why;
    KV_CHECK(screenshotAllowed(s, false, false, &why));
    KV_CHECK(!screenshotAllowed(s, false, true, &why) && why.find(L"camera-roll clip") != std::wstring::npos);
    KV_CHECK(!screenshotAllowed(s, true, false, &why)); // a replay, as before
}

// The limits beside the switch. The storage line's cadence (every 4 s at most, however often the host
// reports a change; at once after a change made on the PC, or the first time).
KV_TEST(camera_roll_limits_text_and_storage_cadence) {
    KV_CHECK(rollLimitsText(30, 200000000000ULL) == L"Keeps clips 30 days, up to " + bytesText(200000000000ULL) + L".");
    KV_CHECK(rollLimitsText(1, 50000000000ULL).find(L"1 day,") != std::wstring::npos);
    KV_CHECK(rollStorageRefreshDue(1000, 0, false));             // never read yet
    KV_CHECK(!rollStorageRefreshDue(5000, 2000, false));         // 3 s later: a pet-state report doesn't read it
    KV_CHECK(rollStorageRefreshDue(5000, 2000, true));           // a change made here does
    KV_CHECK(rollStorageRefreshDue(6000, 2000, false));          // 4 s later
    KV_CHECK(!rollStorageRefreshDue(2000 + kRollStorageRefreshMs - 1, 2000, false));
}

KV_TEST(camera_roll_panel_texts) {
    KV_CHECK(rollBlockText(kvhost::RollBlock::None).empty());
    KV_CHECK(rollBlockText(kvhost::RollBlock::SensorOff) == L"The camera is off.");
    KV_CHECK(rollBlockText(kvhost::RollBlock::DiskFloor) == L"Recording stopped: less than 20 GB is free on the PC's disk.");
    KV_CHECK(rollBlockText(kvhost::RollBlock::RollDisabled) == L"Recording to the camera roll is off.");
    KV_CHECK(clipTriggerLabel(kvhost::ClipTrigger::Motion) == L"Motion");
    // Motion (by depth) is offered only while the live sensor runs, never from a replay (§6.2).
    KV_CHECK(motionUnavailableText(false, true, true).empty());
    KV_CHECK(motionUnavailableText(true, true, true).find(L"replay") != std::wstring::npos);
    KV_CHECK(motionUnavailableText(true, false, false).find(L"replay") != std::wstring::npos);
    KV_CHECK(motionUnavailableText(false, false, false).find(L"live sensor") != std::wstring::npos);
    KV_CHECK(motionUnavailableText(false, true, false).find(L"live sensor") != std::wstring::npos);
    KV_CHECK(rollStorageText(1200000000ULL, 200000000000ULL, 512000000000ULL, 20000000000ULL) ==
             L"Used 1.2 GB of 200.0 GB. 512.0 GB free; recording stops below 20.0 GB free.");
    KV_CHECK(rollStorageText(0, 200000000000ULL, std::nullopt, 20000000000ULL).find(L"Free space unknown") != std::wstring::npos);
    kvhost::RecordScheduleRow row;
    row.days = 0x3E;
    row.startMinute = 22 * 60;
    row.lengthMinutes = 30;
    KV_CHECK(recordScheduleText({}) == L"none");
    KV_CHECK(recordScheduleText({row}) == L"Weekdays 22:00, 30 min");
    row.days = 0x7F;
    KV_CHECK(recordScheduleText({row, row}) == L"Every day 22:00, 30 min; Every day 22:00, 30 min");
    KV_CHECK(clipDurationText(7.4) == L"0:07" && clipDurationText(750) == L"12:30" && clipDurationText(3723) == L"1:02:03");
}

// ---- W8, outside the home (pet-camera.md §4; protocol v3 §8, §10) ---------------------------------------------------

// --test-outside needs --synthetic and a --state-dir of the test's own; checked by parsing alone, so no test launches
// the viewer without --synthetic.
KV_TEST(outside_test_flag_needs_synthetic_and_a_state_dir) {
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(parse({L"--synthetic", L"--test-outside", L"--state-dir", L"C:/kv-test/outside-state", L"--screenshot", L"x.png"},
                       lo, s));
        KV_CHECK(lo.test_outside && lo.serve_loopback_only); // every listener on loopback, the outside one too
        KV_CHECK(isTestLaunch(s, lo));
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--test-outside", L"--state-dir", L"C:/kv-test/outside-state", L"--no-autostart"}, lo, s));
        KV_CHECK(lo.outside_refused && lo.error.find(L"--synthetic") != std::wstring::npos);
    }
    {
        LaunchOptions lo;
        ViewerSettings s;
        KV_CHECK(!parse({L"--synthetic", L"--test-outside"}, lo, s));
        KV_CHECK(lo.outside_refused && lo.error.find(L"--state-dir") != std::wstring::npos);
    }
    LaunchOptions plain;
    ViewerSettings ps;
    ps.synthetic = true;
    KV_CHECK(outsideTestFlagsAcceptable(ps, plain, nullptr)); // without the hook there is nothing to refuse
}

// v3 §10: the screenshot hook (and the second display's capture, which asks the same) refuses while the invite
// window is open, whatever else the view shows.
KV_TEST(screenshot_is_refused_while_the_invite_window_is_open) {
    ViewerSettings s;
    s.synthetic = true;
    s.mode = KV_MODE_DEPTH;
    std::wstring why;
    KV_CHECK(screenshotAllowed(s, false, false, false, &why));
    KV_CHECK(!screenshotAllowed(s, false, false, true, &why) && why.find(L"invite") != std::wstring::npos);
    KV_CHECK(screenshotAllowed(s, false, false, &why)); // the older overload: no invite on screen
}

namespace {
using kvhost::outside::OutsideStatus;
OutsidePanelInput outsideOn(OutsideStatus st) {
    OutsidePanelInput in;
    in.hostRunning = in.hostHasOutside = in.sharingOn = true;
    in.view.allowed = in.view.listening = true;
    in.view.status = st;
    in.view.port = 43210;
    in.view.lanIPv4 = "192.168.1.20";
    return in;
}
} // namespace

// Off disables everything; the switch needs sharing on, a host with the interface, and settings that can be saved.
KV_TEST(outside_panel_off_and_the_switch) {
    OutsidePanelInput in = outsideOn(OutsideStatus::Off);
    in.view.allowed = in.view.listening = false;
    OutsidePanelState st = outsidePanelState(in);
    KV_CHECK(st.allowEnabled && !st.allowChecked && st.allowReason.empty());
    KV_CHECK(!st.portEnabled && !st.inviteEnabled && !st.publicAddressEnabled && !st.reshareEnabled);
    KV_CHECK(!st.showManualSteps && !st.showPublicAddress && !st.showAddressChanged && st.endpointText.empty());
    KV_CHECK(st.inviteReason == L"Turn on viewing from outside the home first.");
    KV_CHECK(st.statusLine == L"Off: devices connect only at home.");
    KV_CHECK(st.portText == L"Port 43210 (TCP)");
    KV_CHECK(st.logEnabled); // the log covers the home listeners too (§8.6)
    in.sharingOn = false;
    st = outsidePanelState(in);
    KV_CHECK(!st.allowEnabled && st.allowReason.find(L"Share the camera") != std::wstring::npos);
    // Still on while sharing went off (the host clears it): it can always be turned off.
    OutsidePanelInput on = outsideOn(OutsideStatus::Mapped);
    on.sharingOn = false;
    KV_CHECK(outsidePanelState(on).allowEnabled);
    on.saveError = "pet-camera.json can't be read (error 5)";
    st = outsidePanelState(on);
    KV_CHECK(!st.allowEnabled && st.allowReason.find(L"pet-camera.json can't be read") != std::wstring::npos);
    KV_CHECK(!st.portEnabled); // fail closed: every change refused while the file can't be read
    on.saveError.clear();
    on.hostHasOutside = false;
    st = outsidePanelState(on);
    KV_CHECK(!st.allowEnabled && !st.allowChecked && !st.inviteEnabled && !st.logEnabled && !st.allowReason.empty());
    on.hostHasOutside = true;
    on.hostRunning = false;
    st = outsidePanelState(on);
    KV_CHECK(!st.allowEnabled && st.allowReason == L"The streaming host is off." && !st.inviteEnabled);
    OutsidePanelInput unpicked = in;
    unpicked.view.port = 0;
    KV_CHECK(outsidePanelState(unpicked).portText.find(L"picked") != std::wstring::npos);
}

// While the panel shows the no-port error (turned on, and none of
// the default's tries bound), the port label says none could be opened and to enter one, not "picked when it's
// turned on". Change port... stays enabled: it is how one is entered.
KV_TEST(outside_panel_port_label_during_the_no_port_error) {
    OutsidePanelInput in = outsideOn(OutsideStatus::Error);
    in.view.listening = false;
    in.view.port = 0;
    in.view.sentence = "Couldn't find a port this PC can open for viewing from outside the home. Enter a port below.";
    OutsidePanelState st = outsidePanelState(in);
    KV_CHECK_MSG(st.portText == L"Port: none could be opened, so enter one below",
                 "the no-port error's port label still says the port is picked when it's turned on");
    KV_CHECK(st.portEnabled);
    // Not turned on: no port is picked until it is.
    OutsidePanelInput off = in;
    off.view.allowed = false;
    off.view.status = OutsideStatus::Off;
    off.view.sentence.clear();
    KV_CHECK(outsidePanelState(off).portText == L"Port: picked when it's turned on");
    // Another error with a port (one this PC can't open, or one taken on the router) keeps the port.
    OutsidePanelInput taken = outsideOn(OutsideStatus::Error);
    taken.view.listening = false;
    KV_CHECK(outsidePanelState(taken).portText == L"Port 43210 (TCP)");
}

// Each status's line is the host's sentence (the carrier-NAT text included), "Checking..." while it checks, and a
// plain one of the viewer's when the host gave none.
KV_TEST(outside_panel_status_lines) {
    OutsidePanelInput in = outsideOn(OutsideStatus::CarrierNat);
    in.view.sentence = "Your router's internet address is 100.64.1.2, which isn't a public address. Your internet provider is "
                       "using carrier NAT, or there's a second router in front of this one, so devices outside your home "
                       "can't reach this PC. It can't work without a relay, and this app doesn't use one.";
    OutsidePanelState st = outsidePanelState(in);
    KV_CHECK(st.statusLine.find(L"carrier NAT") != std::wstring::npos && st.statusLine.find(L"100.64.1.2") != std::wstring::npos);
    KV_CHECK(!st.inviteEnabled && !st.inviteReason.empty() && st.endpointText.empty());
    in.view.checking = true;
    st = outsidePanelState(in);
    KV_CHECK(st.statusLine == kOutsideCheckingText && !st.inviteEnabled);
    for (const auto status : {OutsideStatus::Off, OutsideStatus::Mapped, OutsideStatus::Manual, OutsideStatus::Error,
                              OutsideStatus::CarrierNat, OutsideStatus::AddressChanged}) {
        OutsidePanelView v = outsideOn(status).view;
        KV_CHECK(!outsideStatusLine(v).empty());
        v.sentence = "The host's own words.";
        KV_CHECK(outsideStatusLine(v) == L"The host's own words.");
    }
    OutsidePanelView mapped = outsideOn(OutsideStatus::Mapped).view;
    mapped.endpointKnown = true;
    mapped.endpointHost = "203.0.113.160";
    KV_CHECK(outsideStatusLine(mapped) == L"Devices outside the home can connect at 203.0.113.160:43210.");
    KV_CHECK(outsideStatusLine(outsideOn(OutsideStatus::CarrierNat).view).find(L"carrier NAT") != std::wstring::npos);
}

// Mapped: the endpoint and invites. Manual: the steps and the "Public address" field; an address typed in is the
// endpoint, "entered by you, not checked" (§8.3). Address changed: the line and Re-share (§8.3, §8.4).
KV_TEST(outside_panel_mapped_manual_and_address_changed) {
    OutsidePanelInput in = outsideOn(OutsideStatus::Mapped);
    in.view.endpointKnown = in.view.endpointVerified = in.view.canInvite = true;
    in.view.endpointHost = "203.0.113.160";
    OutsidePanelState st = outsidePanelState(in);
    KV_CHECK(st.allowChecked && st.portEnabled && st.inviteEnabled && st.inviteReason.empty());
    KV_CHECK(st.endpointText == L"Outside address: 203.0.113.160:43210");
    KV_CHECK(!st.showManualSteps && !st.showPublicAddress && !st.showAddressChanged);
    in.view.listening = false; // canInvite alone isn't enough while the listener is closed
    KV_CHECK(!outsidePanelState(in).inviteEnabled);

    OutsidePanelInput man = outsideOn(OutsideStatus::Manual);
    man.view.cannotInviteReason = "Invites need the public address.";
    st = outsidePanelState(man);
    KV_CHECK(st.showManualSteps && st.manualSteps == L"Forward TCP 43210 to 192.168.1.20 in your router's settings.");
    KV_CHECK(st.showPublicAddress && st.publicAddressEnabled && st.publicAddressValue.empty());
    KV_CHECK(!st.inviteEnabled && st.inviteReason == L"Invites need the public address.");
    man.view.manualSteps = "Forward TCP 43210 to 192.168.1.20 in your router's settings, as the host says.";
    KV_CHECK(outsidePanelState(man).manualSteps == L"Forward TCP 43210 to 192.168.1.20 in your router's settings, as the host says.");
    man.view.publicAddressEntered = man.view.endpointHost = "203.0.113.161";
    man.view.endpointKnown = man.view.canInvite = true;
    man.view.cannotInviteReason.clear();
    st = outsidePanelState(man);
    KV_CHECK(st.endpointText == L"Outside address: 203.0.113.161:43210 (entered by you, not checked)");
    KV_CHECK(st.publicAddressValue == L"203.0.113.161" && st.inviteEnabled);
    KV_CHECK(std::wstring(kOutsidePublicAddressLabel).find(L"entered by you, not checked") != std::wstring::npos);

    OutsidePanelInput ch = outsideOn(OutsideStatus::AddressChanged);
    ch.view.endpointKnown = ch.view.endpointVerified = ch.view.canInvite = true;
    ch.view.endpointHost = "203.0.113.170";
    st = outsidePanelState(ch);
    KV_CHECK(st.showAddressChanged && st.reshareEnabled && !st.inviteEnabled); // §8.3: re-share first (it acknowledges)
    KV_CHECK(st.inviteReason.find(L"Re-share it first") != std::wstring::npos);
    KV_CHECK(std::wstring(kOutsideAddressChangedText) == L"Your home address changed; re-share.");
    KV_CHECK(!outsidePanelState(outsideOn(OutsideStatus::Error)).inviteEnabled);
}

KV_TEST(outside_port_invite_and_endpoint_links) {
    KV_CHECK(parseOutsidePort(L"43210") == std::optional<uint16_t>(43210));
    KV_CHECK(parseOutsidePort(L" 1024 ") == std::optional<uint16_t>(1024));
    KV_CHECK(parseOutsidePort(L"65535") == std::optional<uint16_t>(65535));
    for (const wchar_t* bad : {L"1023", L"65536", L"0", L"", L"80a", L"-1", L"123456", L"4321 0"}) KV_CHECK(!parseOutsidePort(bad));
    KV_CHECK(inviteCountdownText(1000 + 600, 1000) == L"Expires in 10:00");
    KV_CHECK(inviteCountdownText(1000 + 581, 1000) == L"Expires in 9:41");
    KV_CHECK(inviteCountdownText(1000, 1000) == L"Expired" && inviteCountdownText(1000, 2000) == L"Expired");
    KV_CHECK(inviteRoleText(kvhost::PetRole::Viewer) == L"Inviting as a viewer.");
    KV_CHECK(inviteRoleText(kvhost::PetRole::Owner) == L"Inviting as an owner.");
    // Synthetic links in the §8.4 shape (made-up values; no real token).
    const std::string invite = "kv2://invite?v=3&s=svc1&fp=AAAA&tok=QUJDREVGR0hJSktMTU5PUA&h=203.0.113.160&p=43210"
                               "&lh=192.168.1.20&exp=1800000000&r=viewer";
    KV_CHECK(looksLikeInviteLink(invite));
    KV_CHECK(!looksLikeInviteLink("kv2://invite?v=3&s=svc1&fp=AAAA&h=203.0.113.160&p=43210&lh=192.168.1.20&exp=1&r=viewer"));
    KV_CHECK(!looksLikeInviteLink("kv2://pair?v=3&s=svc1&fp=AAAA&tok=x&h=a&p=1&lh=b&exp=1&r=viewer"));
    const std::string masked = maskInviteToken(invite);
    KV_CHECK(masked.size() == invite.size() && masked.find("QUJDREVGR0hJSktMTU5PUA") == std::string::npos);
    KV_CHECK(masked.find("tok=**********************&h=203.0.113.160") != std::string::npos);
    KV_CHECK(masked.find("s=svc1&fp=AAAA") != std::string::npos); // the rest unchanged
    KV_CHECK(maskInviteToken("kv2://invite?tok=abc") == "kv2://invite?tok=***");
    const std::string endpoint = "kv2://endpoint?v=3&s=svc1&fp=AAAA&h=203.0.113.160&p=43210";
    KV_CHECK(looksLikeEndpointLink(endpoint));
    KV_CHECK(!looksLikeEndpointLink(endpoint + "&tok=abc")); // never a secret in it
    KV_CHECK(!looksLikeEndpointLink("kv2://endpoint?v=3&s=svc1&fp=AAAA&p=43210"));
    KV_CHECK(!looksLikeEndpointLink(invite));
}

KV_TEST(connection_log_rows_are_the_last_30_days_newest_first) {
    const uint64_t day = 24ull * 60 * 60 * 1000;
    const uint64_t now = 1800000000000ull;
    std::vector<kvhost::outside::LogLine> lines(4);
    lines[0].t = now - 31 * day; // older than 30 days: not shown
    lines[0].event = "connected";
    lines[1].t = now - 2 * day;
    lines[1].event = "paired";
    lines[2].t = now - 1000;
    lines[2].event = "refused_unknown_cert";
    lines[3].t = now - 5 * day;
    lines[3].event = "disconnected";
    const auto rows = connectionLogRows(lines, now);
    KV_CHECK(rows.size() == 3);
    KV_CHECK(rows.size() == 3 && rows[0].event == "refused_unknown_cert" && rows[1].event == "paired" && rows[2].event == "disconnected");
    KV_CHECK(connectionLogEventLabel("refused_unknown_cert") == L"Refused: unknown certificate");
    KV_CHECK(connectionLogEventLabel("invite_burned") == L"Invite burned");
    KV_CHECK(connectionLogEventLabel("something_new") == L"something_new");
    for (const char* e : {"connected", "disconnected", "refused_unknown_cert", "refused_rate_limit", "refused_blocked",
                          "refused_local_only", "refused_too_many", "refused_unavailable", "paired", "invite_redeemed",
                          "invite_failed", "invite_burned", "revoked", "role_changed", "invite_closed",
                          "pairing_failed", "pairing_burned", "suppressed"}) {
        const std::string name = e;
        KV_CHECK(connectionLogEventLabel(name) != std::wstring(name.begin(), name.end())); // every §8.6 event has words
    }
    kvhost::outside::LogLine l;
    l.deviceId = "a1b2c3";
    KV_CHECK(connectionLogDeviceText(l) == L"a1b2c3");
    l.deviceName = "iPhone";
    KV_CHECK(connectionLogDeviceText(l) == L"iPhone");
}

// pet-camera.md §4, "Remove mapping now": enabled only while this PC's forward is on the router.
KV_TEST(outside_panel_remove_mapping_now) {
    KV_CHECK(outsidePanelState(outsideOn(OutsideStatus::Mapped)).removeMappingEnabled);
    KV_CHECK(outsidePanelState(outsideOn(OutsideStatus::AddressChanged)).removeMappingEnabled);
    for (OutsideStatus st : {OutsideStatus::Manual, OutsideStatus::Error, OutsideStatus::CarrierNat}) {
        KV_CHECK(!outsidePanelState(outsideOn(st)).removeMappingEnabled);
    }
    OutsidePanelInput checking = outsideOn(OutsideStatus::Mapped);
    checking.view.checking = true;
    KV_CHECK(!outsidePanelState(checking).removeMappingEnabled);
    OutsidePanelInput off = outsideOn(OutsideStatus::Mapped);
    off.view.allowed = off.view.listening = false;
    KV_CHECK(!outsidePanelState(off).removeMappingEnabled);
    OutsidePanelInput unsaved = outsideOn(OutsideStatus::Mapped);
    unsaved.saveError = "pet-camera.json can't be read";
    KV_CHECK(!outsidePanelState(unsaved).removeMappingEnabled);
}

// "Map the port again" only while outside viewing is on and the PC
// user removed the forward; "Remove mapping now" is disabled meanwhile.
KV_TEST(outside_panel_map_the_port_again) {
    KV_CHECK(!outsidePanelState(outsideOn(OutsideStatus::Mapped)).remapEnabled);
    KV_CHECK(!outsidePanelState(outsideOn(OutsideStatus::Manual)).remapEnabled); // manual for another reason
    OutsidePanelInput removed = outsideOn(OutsideStatus::Manual);
    removed.view.mappingRemoved = true;
    OutsidePanelState st = outsidePanelState(removed);
    KV_CHECK(st.remapEnabled && !st.removeMappingEnabled && st.showManualSteps);
    OutsidePanelInput odd = outsideOn(OutsideStatus::Mapped); // never both at once
    odd.view.mappingRemoved = true;
    KV_CHECK(!outsidePanelState(odd).removeMappingEnabled && outsidePanelState(odd).remapEnabled);
    OutsidePanelInput checking = removed;
    checking.view.checking = true;
    KV_CHECK(!outsidePanelState(checking).remapEnabled);
    OutsidePanelInput off = removed;
    off.view.allowed = off.view.listening = false;
    KV_CHECK(!outsidePanelState(off).remapEnabled);
    OutsidePanelInput unsaved = removed;
    unsaved.saveError = "pet-camera.json can't be read";
    KV_CHECK(!outsidePanelState(unsaved).remapEnabled);
}

// v3 §8.3: the router line says what the forward's gateway is pinned by, and what a new home address does: shared by
// itself once the pinned gateway confirms it, "Checking the router…" while that is under way (for as long as it
// takes), held when the gateway didn't confirm it, waiting for the PC user when none is pinned. Only while the router
// forwards the port.
KV_TEST(outside_panel_router_line) {
    OutsidePanelInput pinned = outsideOn(OutsideStatus::Mapped);
    pinned.view.gatewayPinKind = "address and hardware address";
    KV_CHECK(outsidePanelState(pinned).routerText ==
             L"Router recognised by its address and hardware address. A new home address through it is shared by itself.");
    KV_CHECK(outsidePanelState(outsideOn(OutsideStatus::Mapped)).routerText == kOutsideRouterNotPinned);
    OutsidePanelInput checking = outsideOn(OutsideStatus::AddressChanged);
    checking.view.gatewayPinKind = "address and hardware address";
    checking.view.gatewayChecking = true;
    KV_CHECK(outsidePanelState(checking).routerText == kOutsideRouterChecking);
    KV_CHECK(std::wstring(kOutsideRouterChecking).rfind(L"Checking the router…", 0) == 0);
    OutsidePanelInput held = checking;
    held.view.gatewayChecking = false;
    held.view.gatewayHeld = true;
    KV_CHECK(outsidePanelState(held).routerText == kOutsideRouterOther);
    KV_CHECK(outsidePanelState(held).showAddressChanged && outsidePanelState(held).reshareEnabled);
    KV_CHECK(outsidePanelState(outsideOn(OutsideStatus::AddressChanged)).routerText == kOutsideRouterNotPinned);
    // After about half an hour of checks that couldn't read the router, the line says the PC user can re-share.
    OutsidePanelInput longCheck = checking;
    longCheck.view.gatewayFailedPasses = kOutsideLongCheckPasses;
    KV_CHECK(outsidePanelState(longCheck).routerText == kOutsideRouterCheckingLong);
    // With a gateway pinned, an address no check has confirmed can't be re-shared: the status line says devices keep
    // the old one, invites wait, and the router line names the way out (outside viewing off and on).
    OutsidePanelInput unconfirmed = longCheck;
    unconfirmed.view.reshareWithheld = true;
    const OutsidePanelState us = outsidePanelState(unconfirmed);
    KV_CHECK(us.statusLine == kOutsideUnconfirmedText && !us.showAddressChanged && !us.reshareEnabled);
    KV_CHECK(!us.inviteEnabled && us.inviteReason == kOutsideUnconfirmedText);
    KV_CHECK(std::wstring(kOutsideRouterCheckingLong).find(L"turn viewing from outside the home off and on") != std::wstring::npos);
    KV_CHECK(std::wstring(kOutsideRouterOther).find(L"off and on") != std::wstring::npos);
    // The pinned gateway contradicts the address: another device answered as the router. The status line says so,
    // the router line gives the gateway's own answer, and neither re-share nor an invite is offered.
    OutsidePanelInput contradicted = held;
    contradicted.view.gatewayContradicts = true;
    contradicted.view.gatewayAnswer = "203.0.113.142";
    const OutsidePanelState cs = outsidePanelState(contradicted);
    KV_CHECK(cs.statusLine == kOutsideContradictedText);
    KV_CHECK(cs.routerText == L"The router itself reports 203.0.113.142. Re-share is offered once the router agrees.");
    KV_CHECK(!cs.showAddressChanged && !cs.reshareEnabled);
    KV_CHECK(!cs.inviteEnabled && cs.inviteReason == kOutsideContradictedText);
    KV_CHECK(std::wstring(kOutsideContradictedText) ==
             L"Another device on your network answered as the router. Your camera kept its old address.");
    // No line without a forward: manual, carrier NAT, an error, a removed forward, or outside viewing off.
    for (OutsideStatus st : {OutsideStatus::Manual, OutsideStatus::Error, OutsideStatus::CarrierNat}) {
        OutsidePanelInput in = outsideOn(st);
        in.view.gatewayPinKind = "address and hardware address";
        KV_CHECK(outsidePanelState(in).routerText.empty());
    }
    OutsidePanelInput removed = pinned;
    removed.view.mappingRemoved = true;
    KV_CHECK(outsidePanelState(removed).routerText.empty());
    OutsidePanelInput off = pinned;
    off.view.allowed = off.view.listening = false;
    KV_CHECK(outsidePanelState(off).routerText.empty());
}
// pet-camera.md §7: the Public network-profile hint, while outside viewing is on, from a made-up category.
KV_TEST(outside_panel_public_network_hint) {
    OutsidePanelInput in = outsideOn(OutsideStatus::Mapped);
    KV_CHECK(outsidePanelState(in).profileHint.empty()); // unknown: no hint
    in.networkPublic = false;
    KV_CHECK(outsidePanelState(in).profileHint.empty());
    in.networkPublic = true;
    const std::wstring hint = outsidePanelState(in).profileHint;
    KV_CHECK(hint == kOutsidePublicNetworkHint);
    KV_CHECK(hint.find(L"public") != std::wstring::npos && hint.find(L"Private") != std::wstring::npos);
    in.view.allowed = in.view.listening = false; // off: no hint
    KV_CHECK(outsidePanelState(in).profileHint.empty());
}

// The invite window's sentence says why it closed, and never "used" unless it was.
KV_TEST(outside_invite_closed_text_says_why) {
    KV_CHECK(inviteClosedText("used").find(L"used") != std::wstring::npos);
    for (const char* why : {"expired", "cancelled", "burned", "not_saved", "address_changed", "address_gone", "port_changed",
                            "outside_off", "", "something_new"}) {
        KV_CHECK_MSG(inviteClosedText(why).find(L"used") == std::wstring::npos, why);
        KV_CHECK_MSG(!inviteClosedText(why).empty(), why);
    }
    KV_CHECK(inviteClosedText("expired") == L"The invite ran out.");
    KV_CHECK(inviteClosedText("cancelled") == L"The invite was cancelled.");
    KV_CHECK(inviteClosedText("address_changed").find(L"home address changed") != std::wstring::npos);
    KV_CHECK(inviteClosedText("address_gone").find(L"no longer knows your home address") != std::wstring::npos);
    KV_CHECK(inviteClosedText("") == L"The invite was closed.");
}

// Copy link marks the invite link private with exactly these registered formats (each a DWORD 0).
KV_TEST(outside_copy_link_clipboard_formats) {
    const auto& f = privateClipboardFormats();
    KV_REQUIRE(f.size() == 3);
    KV_CHECK(f[0] == L"ExcludeClipboardContentFromMonitorProcessing");
    KV_CHECK(f[1] == L"CanIncludeInClipboardHistory");
    KV_CHECK(f[2] == L"CanUploadToCloudClipboard");
}

KV_TEST(outside_log_labels_the_invite_closed_event) { KV_CHECK(connectionLogEventLabel("invite_closed") == L"Invite closed"); }

// The host hands the UI thread its phone commands, orbit gestures and deleted clip ids through a mailbox: the
// window message carries no pointer, so a message another program posts finds nothing, or only what was sent.
KV_TEST(the_ui_mailbox_hands_over_in_order_and_a_stray_message_finds_nothing) {
    UiMailbox<std::string, 3> box;
    KV_CHECK(!box.take()); // a message no host thread sent
    box.put("a");
    box.put("b");
    KV_CHECK(box.take() == std::optional<std::string>("a"));
    KV_CHECK(box.take() == std::optional<std::string>("b"));
    KV_CHECK(!box.take());
    // Bounded: past three waiting, the oldest goes.
    for (const char* s : {"c", "d", "e", "f"}) box.put(s);
    KV_CHECK(box.take() == std::optional<std::string>("d"));
    KV_CHECK(box.take() == std::optional<std::string>("e"));
    KV_CHECK(box.take() == std::optional<std::string>("f"));
    KV_CHECK(!box.take());
    // A command keeps every field across the hand-over.
    UiMailbox<kvhost::RemoteControlCommandMessage> commands;
    kvhost::RemoteControlCommandMessage cmd;
    cmd.modeRaw = 42;
    commands.put(cmd);
    const auto got = commands.take();
    KV_CHECK(got && got->modeRaw == std::optional<int>(42));
}

KV_TEST_MAIN()
