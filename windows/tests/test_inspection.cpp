// Inspection tools and presets (as on the Mac) as pure logic: region stats, the 3D ruler with
// given intrinsics, the viewport, display-to-sensor mapping, the drop banner, health lines,
// shortcuts, and the presets store in a temp folder. No sensor, no window.
#include "kv_test.hpp"

#include "display_targets.hpp"
#include "inspection.hpp"
#include "presets.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace kvwin;

namespace {
bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

kv_ir_intrinsics_t testIntrinsics() {
    kv_ir_intrinsics_t i{};
    i.valid = true;
    i.fx = 365.0f;
    i.fy = 365.0f;
    i.cx = 256.0f;
    i.cy = 212.0f;
    return i; // no distortion
}

std::wstring tempDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"kv-inspect-" + tag + L"-" + std::to_wstring(GetCurrentProcessId());
}
} // namespace

KV_TEST(region_stats_mean_median_sd_min_max_and_invalid_share) {
    std::vector<float> d(512 * 424, 0.0f);
    // A 3x2 box at (10,20)-(12,21): 1000, 1100, 1200, 0 (invalid), NaN (invalid), 1300.
    const float vals[6] = {1000.0f, 1100.0f, 1200.0f, 0.0f, std::nanf(""), 1300.0f};
    int k = 0;
    for (int y = 20; y <= 21; ++y)
        for (int x = 10; x <= 12; ++x) d[static_cast<size_t>(y) * 512 + x] = vals[k++];
    const auto s = RegionDepthStats::compute(d, 512, 424, 12, 21, 10, 20); // corners in any order
    KV_REQUIRE(s.has_value());
    KV_CHECK(s->total_pixels == 6 && s->valid_pixels == 4 && s->invalid_pixels == 2);
    KV_CHECK(approx(s->invalid_share, 2.0 / 6.0, 1e-9));
    KV_CHECK(approx(*s->mean_mm, 1150.0, 1e-6));
    KV_CHECK(approx(*s->median_mm, 1150.0, 1e-6)); // even count: the middle two averaged
    KV_CHECK(approx(*s->stddev_mm, std::sqrt(12500.0), 1e-6)); // population sd
    KV_CHECK(*s->min_mm == 1000.0 && *s->max_mm == 1300.0);
    // All invalid: counts, but no statistics.
    const auto none = RegionDepthStats::compute(d, 512, 424, 100, 100, 101, 101);
    KV_REQUIRE(none.has_value());
    KV_CHECK(none->valid_pixels == 0 && !none->mean_mm && none->invalid_share == 1.0);
    // Clamped to the image; an empty buffer gives nothing.
    const auto edge = RegionDepthStats::compute(d, 512, 424, 500, 400, 600, 500);
    KV_REQUIRE(edge.has_value());
    KV_CHECK(edge->sensor_max_x == 511 && edge->sensor_max_y == 423);
    KV_CHECK(!RegionDepthStats::compute({}, 512, 424, 0, 0, 1, 1).has_value());
}

KV_TEST(region_stats_odd_count_median_and_a_single_pixel) {
    std::vector<float> d(512 * 424, 0.0f);
    // Five valid values in a 5x1 row, out of order: the median is the middle one.
    const float vals[5] = {1500.0f, 1000.0f, 2000.0f, 1200.0f, 1800.0f};
    for (int x = 0; x < 5; ++x) d[static_cast<size_t>(50) * 512 + 300 + x] = vals[x];
    const auto s = RegionDepthStats::compute(d, 512, 424, 300, 50, 304, 50);
    KV_REQUIRE(s.has_value());
    KV_CHECK(s->valid_pixels == 5 && *s->median_mm == 1500.0 && *s->min_mm == 1000.0 && *s->max_mm == 2000.0);
    KV_CHECK(approx(*s->mean_mm, 1500.0, 1e-9));
    // One pixel: everything is that value, sd 0.
    const auto one = RegionDepthStats::compute(d, 512, 424, 302, 50, 302, 50);
    KV_REQUIRE(one.has_value());
    KV_CHECK(one->total_pixels == 1 && one->valid_pixels == 1 && *one->mean_mm == 2000.0 && *one->median_mm == 2000.0 &&
             *one->stddev_mm == 0.0 && *one->min_mm == 2000.0 && *one->max_mm == 2000.0 && one->invalid_share == 0.0);
}

KV_TEST(unproject_puts_the_pixel_centre_at_col_plus_half_minus_cx) {
    // The principal point's own pixel is half a pixel off the optical axis (libfreenect2's
    // col + 0.5 - cx): a missing +0.5 gives exactly 0 here, so this fails if it's ever dropped.
    const kv_ir_intrinsics_t intr = testIntrinsics();
    float x = 0, y = 0, z = 0;
    KV_REQUIRE(kv_unproject_depth_pixel_mm(&intr, 256, 212, 1000.0f, &x, &y, &z));
    KV_CHECK_MSG(approx(x, 0.5 / 365.0 * 1000.0, 1e-3), std::to_string(x));
    KV_CHECK_MSG(std::fabs(std::fabs(y) - 0.5 / 365.0 * 1000.0) < 1e-3, std::to_string(y)); // sign: the core's y axis
    KV_CHECK(approx(z, 1000.0, 1e-3));
    KV_REQUIRE(kv_unproject_depth_pixel_mm(&intr, 0, 212, 1000.0f, &x, &y, &z));
    KV_CHECK_MSG(approx(x, (0.5 - 256.0) / 365.0 * 1000.0, 1e-2), std::to_string(x));
    KV_CHECK(!kv_unproject_depth_pixel_mm(&intr, 10, 10, 0.0f, &x, &y, &z)); // no depth, no point
}

KV_TEST(a_side_by_side_region_stays_in_the_panel_it_started_in) {
    int x0 = 0, x1 = 0;
    panelColumnsAt(600, 1024, KV_MODE_SIDE_BY_SIDE, KV_SBS_IR_DEPTH, false, false, x0, x1);
    KV_CHECK(x0 == 512 && x1 == 1023);
    panelColumnsAt(100, 1024, KV_MODE_SIDE_BY_SIDE, KV_SBS_IR_DEPTH, false, false, x0, x1);
    KV_CHECK(x0 == 0 && x1 == 511);
    panelColumnsAt(900, 1280, KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_DEPTH, false, false, x0, x1);
    KV_CHECK(x0 == 768 && x1 == 1279);
    panelColumnsAt(100, 1280, KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_IR, false, false, x0, x1);
    KV_CHECK(x0 == 0 && x1 == 753);
    // Mirrored, the depth panel is on the left of the display.
    panelColumnsAt(100, 1280, KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_DEPTH, true, false, x0, x1);
    KV_CHECK(x0 == 0 && x1 == 511);
    panelColumnsAt(100, 1024, KV_MODE_SIDE_BY_SIDE, KV_SBS_IR_DEPTH, true, true, x0, x1); // both flips: x back
    KV_CHECK(x0 == 0 && x1 == 511);
    panelColumnsAt(100, 512, KV_MODE_DEPTH, KV_SBS_IR_DEPTH, false, false, x0, x1); // not Side by Side: all of it
    KV_CHECK(x0 == 0 && x1 == 511);
}

KV_TEST(ruler_uses_the_ir_intrinsics_and_refuses_without_them_or_without_depth) {
    std::vector<float> d(512 * 424, 1000.0f);
    RulerMeasurement m;
    m.a = {0, 0, 100, 212, true};
    m.b = RulerEndpoint{0, 0, 400, 212, true};
    RulerMeasurement::evaluate(m, testIntrinsics(), d);
    // Same depth, same row: |(400.5 - 256) - (100.5 - 256)| / 365 * 1000 mm (libfreenect2's col + 0.5 - cx).
    KV_REQUIRE(m.distance_mm.has_value());
    KV_CHECK_MSG(approx(*m.distance_mm, 300.0 / 365.0 * 1000.0, 1.0), std::to_string(*m.distance_mm));
    KV_CHECK(m.error.empty() && m.label().find(L" mm (") != std::wstring::npos);

    // No intrinsics: never a fallback.
    kv_ir_intrinsics_t none{};
    RulerMeasurement::evaluate(m, none, d);
    KV_CHECK(!m.distance_mm && m.error == L"Intrinsics unavailable" && m.label() == L"Intrinsics unavailable");
    // An end with no depth.
    d[static_cast<size_t>(212) * 512 + 400] = 0.0f;
    RulerMeasurement::evaluate(m, testIntrinsics(), d);
    KV_CHECK(!m.distance_mm && m.error == L"no depth here");
    // An end off the depth image (the colour half of a Side-by-Side view).
    m.b->has_sensor_coord = false;
    RulerMeasurement::evaluate(m, testIntrinsics(), d);
    KV_CHECK(m.error == L"no depth here");
    // Only one point yet.
    RulerMeasurement one;
    one.a = {0, 0, 1, 1, true};
    RulerMeasurement::evaluate(one, testIntrinsics(), d);
    KV_CHECK(one.error.empty() && !one.distance_mm && one.label().find(L"second point") != std::wstring::npos);
}

KV_TEST(cursor_readout_texts) {
    CursorReadout r;
    r.pixel_x = 12;
    r.pixel_y = 34;
    KV_CHECK(r.coordText() == L"(12, 34)");
    KV_CHECK(r.depthText() == L"—" && r.irText() == L"—"); // off the depth image
    r.has_sensor_depth_ir = true;
    r.depth_mm = 1234.4f;
    r.raw_ir = 5120.0f;
    KV_CHECK(r.depthText() == L"1234 mm" && r.irText() == L"5120");
    r.depth_mm = 0.0f;
    KV_CHECK(r.depthText() == L"No depth");
}

KV_TEST(viewport_zooms_around_the_cursor_pans_within_bounds_and_resets) {
    ViewportTransform v;
    const double vw = 1000, vh = 800;
    const int iw = 512, ih = 424;
    const RectD base = ViewportTransform::baseAspectFitRect(vw, vh, iw, ih);
    KV_CHECK(approx(base.h, 800.0, 1e-9) && approx(base.x, (1000.0 - 512.0 * 800.0 / 424.0) / 2.0, 1e-9));
    int px0 = 0, py0 = 0;
    KV_REQUIRE(v.viewToDisplayPixel(600, 300, vw, vh, iw, ih, px0, py0));
    v.zoomAround(600, 300, vw, vh, iw, ih, 2.0);
    KV_CHECK(approx(v.zoom, 2.0, 1e-9) && v.isZoomedOrPanned());
    int px1 = 0, py1 = 0;
    KV_REQUIRE(v.viewToDisplayPixel(600, 300, vw, vh, iw, ih, px1, py1));
    KV_CHECK(std::abs(px1 - px0) <= 1 && std::abs(py1 - py0) <= 1); // the pixel under the cursor stays put
    double cx = 0, cy = 0;
    v.displayPixelCenterInView(px1, py1, vw, vh, iw, ih, cx, cy);
    // Within half a displayed pixel (3.8 view pixels at 2x here) of the cursor.
    const double half_px = 0.5 * v.displayedImageRect(vw, vh, iw, ih).h / ih;
    KV_CHECK(std::fabs(cx - 600) <= half_px + 0.01 && std::fabs(cy - 300) <= half_px + 0.01);
    v.zoomAround(600, 300, vw, vh, iw, ih, 100.0);
    KV_CHECK(v.zoom == ViewportTransform::kMaxZoom);
    v.panBy(1e6, -1e6, vw, vh, iw, ih); // clamped
    const RectD r = v.displayedImageRect(vw, vh, iw, ih);
    KV_CHECK(r.x <= vw * 0.25 + 1e-6 && r.x + r.w >= vw * 0.75 - 1e-6);
    v.zoomAround(500, 400, vw, vh, iw, ih, 1.0 / 1000.0); // back to 1x resets the pan too
    KV_CHECK(v.zoom == 1.0 && v.pan_x == 0.0 && v.pan_y == 0.0 && !v.isZoomedOrPanned());
    v.panBy(50, 50, vw, vh, iw, ih); // no panning at 1x
    KV_CHECK(v.pan_x == 0.0 && v.pan_y == 0.0);
    int px = 0, py = 0;
    KV_CHECK(!v.viewToDisplayPixel(5, 5, vw, vh, iw, ih, px, py)); // off the image (letterbox)
}

KV_TEST(display_pixels_map_back_to_the_depth_ir_sensor) {
    // IR/Depth, then mirrored, then rotated.
    auto s = mapDisplayPixelToSensor(10, 20, 512, 424, KV_MODE_DEPTH, KV_SBS_IR_DEPTH, false, false);
    KV_CHECK(s.x == 10 && s.y == 20 && s.has_depth_ir);
    s = mapDisplayPixelToSensor(10, 20, 512, 424, KV_MODE_DEPTH, KV_SBS_IR_DEPTH, true, false);
    KV_CHECK(s.x == 501 && s.y == 20);
    s = mapDisplayPixelToSensor(10, 20, 512, 424, KV_MODE_IR, KV_SBS_IR_DEPTH, false, true);
    KV_CHECK(s.x == 501 && s.y == 403);
    s = mapDisplayPixelToSensor(10, 20, 512, 424, KV_MODE_IR, KV_SBS_IR_DEPTH, true, true); // both: x back
    KV_CHECK(s.x == 10 && s.y == 403);
    // Side by Side IR+Depth: either half is the same sensor pixel.
    s = mapDisplayPixelToSensor(600, 5, 1024, 424, KV_MODE_SIDE_BY_SIDE, KV_SBS_IR_DEPTH, false, false);
    KV_CHECK(s.x == 88 && s.y == 5 && s.has_depth_ir);
    // Colour+Depth: the right panel is depth, the left is colour (no depth there), the divider neither.
    s = mapDisplayPixelToSensor(800, 5, 1280, 424, KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_DEPTH, false, false);
    KV_CHECK(s.x == 32 && s.has_depth_ir);
    s = mapDisplayPixelToSensor(377, 212, 1280, 424, KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_DEPTH, false, false);
    KV_CHECK(!s.has_depth_ir && s.x == 960 && s.y == 540);
    s = mapDisplayPixelToSensor(760, 5, 1280, 424, KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_IR, false, false);
    KV_CHECK(!s.has_depth_ir);
    // Colour: never depth.
    s = mapDisplayPixelToSensor(100, 100, 1920, 1080, KV_MODE_COLOR, KV_SBS_IR_DEPTH, false, false);
    KV_CHECK(!s.has_depth_ir);
    // A frame of the wrong size for its mode isn't trusted as depth.
    s = mapDisplayPixelToSensor(10, 10, 640, 480, KV_MODE_DEPTH, KV_SBS_IR_DEPTH, false, false);
    KV_CHECK(!s.has_depth_ir);
}

KV_TEST(drop_banner_and_health_lines) {
    KV_CHECK(std::wstring(kDropBannerText) ==
             L"Frames are dropping. Try a different USB port, ideally a rear port directly on the motherboard.");
    KV_CHECK(!shouldShowDropBanner(0.02, 0.0) && shouldShowDropBanner(0.021, 0.0) && shouldShowDropBanner(0.0, 0.05));
    kv_health_snapshot_t h{};
    h.ir_fps = 30.0;
    h.depth_fps = 29.9;
    h.depth_drops_per_min = 12.0;
    h.inter_frame_p50_ms = 33.3;
    h.inter_frame_p95_ms = 35.0;
    h.depth_processing_ms = 1.25;
    h.driver_errors = 2;
    std::strcpy(h.last_driver_error, "isochronous transfer error");
    std::strcpy(h.usb_location, "Port_#0003.Hub_#0001");
    const auto lines = healthLines(h);
    KV_REQUIRE(lines.size() == 4);
    KV_CHECK(lines[0] == L"fps: IR 30.0  Depth 29.9  Colour 0.0");
    KV_CHECK(lines[1].find(L"Depth 12.0") != std::wstring::npos);
    KV_CHECK(lines[2].find(L"p50 33.3 ms") != std::wstring::npos && lines[2].find(L"p95 35.0 ms") != std::wstring::npos &&
             lines[2].find(L"1.25 ms") != std::wstring::npos);
    KV_CHECK(lines[3] == L"Driver errors 2 (last: isochronous transfer error)   USB Port_#0003.Hub_#0001");
}

KV_TEST(shortcuts) {
    KV_CHECK(shortcutForKey('1', false, false) == Shortcut::ModeIr);
    KV_CHECK(shortcutForKey('2', false, false) == Shortcut::ModeColour);
    KV_CHECK(shortcutForKey('3', false, false) == Shortcut::ModeDepth);
    KV_CHECK(shortcutForKey('4', false, false) == Shortcut::ModeSideBySide);
    KV_CHECK(shortcutForKey('5', false, false) == Shortcut::Mode3D);
    KV_CHECK(shortcutForKey('F', false, false) == Shortcut::Fullscreen);
    KV_CHECK(shortcutForKey('M', false, false) == Shortcut::Mirror);
    KV_CHECK(shortcutForKey('R', false, false) == Shortcut::Rotate180);
    KV_CHECK(shortcutForKey('H', false, false) == Shortcut::Health);
    KV_CHECK(shortcutForKey(0x20, false, false) == Shortcut::Freeze);
    KV_CHECK(shortcutForKey(0x1B, false, false) == Shortcut::ClearTools);
    KV_CHECK(shortcutForKey(0xBF, true, false) == Shortcut::Help && shortcutForChar(L'?') == Shortcut::Help);
    KV_CHECK(shortcutForKey('M', false, true) == Shortcut::None); // Ctrl/Alt combinations are left alone
    KV_CHECK(shortcutForKey('X', false, false) == Shortcut::None);
    KV_CHECK(shortcutForKey('B', false, false) == Shortcut::Blackout && shortcutForKey('B', true, false) == Shortcut::Blackout);
    KV_CHECK(shortcutForKey('B', false, true) == Shortcut::None); // Ctrl+B / Alt+B left alone
    // The help lists every key.
    std::wstring all;
    for (const auto& l : helpLines()) all += l + L"\n";
    for (const wchar_t* k : {L"1  IR", L"2  Colour", L"3  Depth", L"4  Side by side", L"5  3D", L"F  full screen", L"M  mirror",
                             L"R  rotate", L"Space", L"H  health", L"?  this help", L"Esc", L"B  black out"}) {
        std::string key;
        for (const wchar_t* p = k; *p; ++p) key += static_cast<char>(*p < 128 ? *p : '?');
        KV_CHECK_MSG(all.find(k) != std::wstring::npos, key);
    }
}

KV_TEST(built_in_presets_have_the_macs_values) {
    const auto& b = builtInPresets();
    KV_REQUIRE(b.size() == 3);
    KV_CHECK(b[0].name == "Desk" && b[1].name == "Room" && b[2].name == "Dark room IR");
    KV_CHECK(b[0].settings.mode == KV_MODE_DEPTH && b[0].settings.depth_min_mm == 400.0f && b[0].settings.depth_max_mm == 2200.0f &&
             b[0].settings.depth_colormap == KV_COLORMAP_TURBO && b[0].settings.exposure_mode == KV_EXPOSURE_SEMI_AUTO);
    KV_CHECK(b[1].settings.depth_max_mm == 6000.0f && b[1].settings.depth_colormap == KV_COLORMAP_VIRIDIS &&
             b[1].settings.exposure_mode == KV_EXPOSURE_AUTO);
    KV_CHECK(b[2].settings.mode == KV_MODE_IR && b[2].settings.ir_gamma == 0.36f && b[2].settings.ir_manual_max == 14000.0f &&
             b[2].settings.exposure_mode == KV_EXPOSURE_MANUAL && b[2].settings.manual_analog_gain == 3.0f);
    for (const auto& p : b) KV_CHECK(p.built_in);
    // Applying keeps the source.
    ViewerSettings s;
    s.synthetic = true;
    s.clock_overlay = true;
    applyPreset(b[2], s);
    KV_CHECK(s.synthetic && s.clock_overlay && s.mode == KV_MODE_IR && s.manual_integration_ms == 33.3f);
}

KV_TEST(presets_store_saves_loads_refuses_built_in_names_and_remembers_the_last_used) {
    const std::wstring dir = tempDir(L"presets");
    DeleteFileW((dir + L"\\presets.json").c_str());
    RemoveDirectoryW(dir.c_str());
    {
        PresetsStore st(dir);
        KV_CHECK(st.custom().empty() && !st.lastUsed());
        KV_CHECK(st.all().size() == 3);
        ViewerSettings s;
        s.mode = KV_MODE_SIDE_BY_SIDE;
        s.sbs_layout = KV_SBS_COLOR_IR;
        s.depth_min_mm = 700.0f;
        s.mirror_horizontal = true;
        KV_CHECK(st.saveCustom("  Bench  ", s));
        KV_CHECK(!st.saveCustom("desk", s));  // a built-in's name, any case
        KV_CHECK(!st.saveCustom("   ", s));
        s.depth_min_mm = 800.0f;
        KV_CHECK(st.saveCustom("BENCH", s)); // same name, any case: replaced
        KV_CHECK(st.custom().size() == 1 && st.custom()[0].name == "BENCH");
        s.mode = KV_MODE_DEPTH;
        s.rotate_180 = true;
        st.saveLastUsed(s);
    }
    {
        PresetsStore st(dir); // as at the next launch
        KV_REQUIRE(st.custom().size() == 1);
        const ViewerSettings& c = st.custom()[0].settings;
        KV_CHECK(c.mode == KV_MODE_SIDE_BY_SIDE && c.sbs_layout == KV_SBS_COLOR_IR && c.depth_min_mm == 800.0f &&
                 c.mirror_horizontal && !st.custom()[0].built_in);
        KV_REQUIRE(st.lastUsed().has_value());
        KV_CHECK(st.lastUsed()->settings.mode == KV_MODE_DEPTH && st.lastUsed()->settings.rotate_180);
        KV_CHECK(st.all().size() == 4);
        st.deleteCustom("BENCH");
        KV_CHECK(st.custom().empty());
    }
    {
        // A damaged file: nothing loaded, nothing thrown.
        std::ofstream(dir + L"\\presets.json", std::ios::trunc) << "{not json";
        PresetsStore st(dir);
        KV_CHECK(st.custom().empty() && !st.lastUsed());
        // A file can't claim a built-in. The 3D mode (4) exists since the 3D point cloud; a mode this viewer
        // doesn't have falls back to IR.
        std::vector<ViewerPreset> c;
        std::optional<ViewerPreset> l;
        ViewerPreset p = capturePreset("x", ViewerSettings{});
        p.built_in = true;
        const std::string j0 = PresetsStore::toJson({p}, std::nullopt);
        const auto at = j0.find("\"modeRaw\":0");
        KV_REQUIRE(at != std::string::npos);
        std::string j = j0;
        j.replace(at, 11, "\"modeRaw\":4");
        KV_REQUIRE(PresetsStore::fromJson(j, c, l));
        KV_CHECK(c.size() == 1 && !c[0].built_in && c[0].settings.mode == KV_MODE_POINT_CLOUD_3D);
        j = j0;
        j.replace(at, 11, "\"modeRaw\":9");
        KV_REQUIRE(PresetsStore::fromJson(j, c, l));
        KV_CHECK(c.size() == 1 && c[0].settings.mode == KV_MODE_IR);
    }
    DeleteFileW((dir + L"\\presets.json").c_str());
    RemoveDirectoryW(dir.c_str());
}

// The second display: which display the projector output goes to (the Mac's refreshAvailableDisplays).
KV_TEST(second_display_choice_keeps_the_pick_else_prefers_a_projector) {
    auto disp = [](const wchar_t* dev, const wchar_t* name, LONG x, LONG w, LONG h, bool primary) {
        DisplayTarget d;
        d.device = dev;
        d.name = name;
        d.rect = {x, 0, x + w, h};
        d.primary = primary;
        return d;
    };
    const std::vector<DisplayTarget> one = {disp(L"\\\\.\\DISPLAY1", L"Laptop panel", 0, 2560, 1600, true)};
    const std::vector<DisplayTarget> two = {disp(L"\\\\.\\DISPLAY1", L"Laptop panel", 0, 2560, 1600, true),
                                            disp(L"\\\\.\\DISPLAY2", L"Projector", 2560, 1920, 1080, false)};
    KV_CHECK(chooseDisplay({}, L"") == -1);
    KV_CHECK(chooseDisplay(one, L"") == 0);                     // only the main display: that one
    KV_CHECK(chooseDisplay(two, L"") == 1);                     // a projector first
    KV_CHECK(chooseDisplay(two, L"\\\\.\\DISPLAY1") == 0);      // a choice is kept while it's there
    KV_CHECK(chooseDisplay(one, L"\\\\.\\DISPLAY2") == 0);      // gone: back to what's left
    KV_CHECK(findDisplay(two, L"\\\\.\\DISPLAY2") == 1 && findDisplay(two, L"\\\\.\\DISPLAY9") == -1 && findDisplay(two, L"") == -1);
    KV_CHECK(displayTitle(two[0]) == L"Laptop panel (2560x1600), main");
    KV_CHECK(displayTitle(two[1]) == L"Projector (1920x1080)");
    DisplayTarget unnamed = two[1];
    unnamed.name.clear();
    KV_CHECK(displayTitle(unnamed) == L"\\\\.\\DISPLAY2 (1920x1080)");
    // This PC's own displays: at least one, exactly one main, each with an area.
    const auto here = listDisplays();
    std::printf("  this PC has %zu display(s)\n", here.size());
    KV_CHECK(!here.empty());
    size_t mains = 0;
    for (const auto& d : here) {
        mains += d.primary;
        KV_CHECK(d.rect.right > d.rect.left && d.rect.bottom > d.rect.top && !d.device.empty());
    }
    KV_CHECK(mains == 1);
}

KV_TEST(presets_keep_the_cutoff_and_point_cloud_but_never_the_blend) {
    ViewerSettings s;
    s.cutoff_enabled = true;
    s.cutoff_near_mm = 700.0f;
    s.cutoff_far_mm = 1600.0f;
    s.point_size = 4.5f;
    s.point_color_source = KV_POINT_COLOR_IR;
    s.depth_color_blend_enabled = true; // live colour: not saved
    s.depth_color_blend_percent = 80.0f;
    const ViewerPreset saved = capturePreset("Tracking", s);
    KV_CHECK(!saved.settings.depth_color_blend_enabled);
    std::vector<ViewerPreset> c;
    std::optional<ViewerPreset> l;
    const std::string j = PresetsStore::toJson({saved}, saved);
    KV_CHECK(j.find("cutoffNearMm") != std::string::npos && j.find("pointCloudColorSourceRaw") != std::string::npos);
    KV_CHECK(j.find("lend") == std::string::npos); // no blend key of any spelling
    KV_REQUIRE(PresetsStore::fromJson(j, c, l));
    KV_REQUIRE(c.size() == 1 && l.has_value());
    ViewerSettings target; // blend on in the current view: a preset leaves it alone
    target.depth_color_blend_enabled = true;
    applyPreset(c[0], target);
    KV_CHECK(target.cutoff_enabled && approx(target.cutoff_near_mm, 700, 1e-3) && approx(target.cutoff_far_mm, 1600, 1e-3));
    KV_CHECK(approx(target.point_size, 4.5, 1e-3) && target.point_color_source == KV_POINT_COLOR_IR);
    KV_CHECK(target.depth_color_blend_enabled);

    // A preset from before the point cloud (a built-in, or an old file) leaves the cut-off and points as they are.
    ViewerSettings mine;
    applyTrackMe(mine);
    mine.point_size = 6.0f;
    applyPreset(builtInPresets()[0], mine);
    KV_CHECK(mine.cutoff_enabled && approx(mine.cutoff_far_mm, 2000, 1e-3) && approx(mine.point_size, 6.0, 1e-3));
    std::string old = PresetsStore::toJson({saved}, std::nullopt);
    for (const char* key : {"cutoffEnabled", "cutoffNearMm", "cutoffFarMm", "pointCloudSize", "pointCloudColorSourceRaw"}) {
        const auto k = old.find(std::string(",\"") + key + "\"");
        KV_REQUIRE(k != std::string::npos);
        const auto end = old.find_first_of(",}", k + 1);
        old.erase(k, end - k);
    }
    KV_REQUIRE(PresetsStore::fromJson(old, c, l));
    KV_REQUIRE(c.size() == 1);
    KV_CHECK(!c[0].cutoff_points.cutoff_enabled && !c[0].cutoff_points.point_size);
    ViewerSettings kept;
    kept.point_size = 7.0f;
    applyPreset(c[0], kept);
    KV_CHECK(approx(kept.point_size, 7.0, 1e-3) && !kept.cutoff_enabled);

    // A point cloud field of the wrong type makes that entry bad (skipped), not the file.
    std::string bad = j;
    const auto n = bad.find("\"cutoffNearMm\":");
    KV_REQUIRE(n != std::string::npos);
    bad.insert(n + 15, "\"x\",\"ignored\":");
    int skipped = 0;
    KV_REQUIRE(PresetsStore::fromJson(bad, c, l, &skipped));
    KV_CHECK(c.empty() && skipped == 1 && l.has_value()); // the custom one; the last-used is untouched
}

namespace {
std::string readAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void clearDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
} // namespace

KV_TEST(presets_skip_a_bad_entry_and_never_write_over_a_file_they_could_not_read) {
    // One good preset, one with a wrong type (modeRaw as a string), one that isn't an object, and a
    // last-used with a missing field.
    const std::string good = PresetsStore::toJson({capturePreset("Good", ViewerSettings{})}, std::nullopt);
    std::string bad_entry = PresetsStore::toJson({capturePreset("Bad", ViewerSettings{})}, std::nullopt);
    bad_entry.replace(bad_entry.find("\"modeRaw\":0"), 11, "\"modeRaw\":\"ir\"");
    const std::string good_obj = good.substr(good.find('[') + 1, good.rfind(']') - good.find('[') - 1);
    const std::string bad_obj = bad_entry.substr(bad_entry.find('[') + 1, bad_entry.rfind(']') - bad_entry.find('[') - 1);
    const std::string mixed = "{\"customPresets\":[" + good_obj + "," + bad_obj + ",42],\"lastUsedSettings\":{\"name\":\"x\"}}";
    std::vector<ViewerPreset> c;
    std::optional<ViewerPreset> l;
    int skipped = 0;
    KV_REQUIRE(PresetsStore::fromJson(mixed, c, l, &skipped));
    KV_CHECK(c.size() == 1 && c[0].name == "Good" && !l && skipped == 3);
    KV_CHECK(!PresetsStore::fromJson("{\"customPresets\":7}", c, l)); // not the file's shape at all

    // In a store: the good one loads, and the first write moves the original aside instead of over it.
    const std::wstring dir = tempDir(L"presets-bad");
    clearDir(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream(dir + L"\\presets.json", std::ios::binary | std::ios::trunc) << mixed;
    {
        PresetsStore st(dir);
        KV_CHECK(st.custom().size() == 1 && st.keepsUnreadableFile());
        st.saveLastUsed(ViewerSettings{});
        KV_CHECK(readAll(dir + L"\\presets.json.unreadable-1") == mixed); // kept, byte for byte
        KV_CHECK(!st.keepsUnreadableFile());
        PresetsStore again(dir);
        KV_CHECK(again.custom().size() == 1 && again.lastUsed().has_value()); // the good one carried over
    }
    // A file that isn't JSON at all: nothing loads, and it's kept too.
    std::ofstream(dir + L"\\presets.json", std::ios::binary | std::ios::trunc) << "{oops";
    {
        PresetsStore st(dir);
        KV_CHECK(st.custom().empty() && st.keepsUnreadableFile());
        KV_CHECK(st.saveCustom("Mine", ViewerSettings{}));
        KV_CHECK(readAll(dir + L"\\presets.json.unreadable-2") == "{oops");
        KV_CHECK(readAll(dir + L"\\presets.json.unreadable-1") == mixed); // the earlier one untouched
    }
    clearDir(dir);
}

KV_TEST_MAIN()
