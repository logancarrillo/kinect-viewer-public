#include "inspection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kvwin {

namespace {
std::wstring fmt(const wchar_t* f, double v) {
    wchar_t b[64];
    swprintf(b, 64, f, v);
    return b;
}
constexpr const wchar_t* kDash = L"—";
} // namespace

// ---- cursor readout -------------------------------------------------------------------------------

std::wstring CursorReadout::coordText() const {
    return L"(" + std::to_wstring(pixel_x) + L", " + std::to_wstring(pixel_y) + L")";
}

std::wstring CursorReadout::depthText() const {
    if (!has_sensor_depth_ir) return kDash;
    if (depth_mm && std::isfinite(*depth_mm) && *depth_mm > 0.0f) return fmt(L"%.0f mm", *depth_mm);
    return L"No depth";
}

std::wstring CursorReadout::irText() const {
    if (!has_sensor_depth_ir) return kDash;
    if (raw_ir && std::isfinite(*raw_ir)) return fmt(L"%.0f", *raw_ir);
    return kDash;
}

// ---- region stats -------------------------------------------------------------------------------

std::optional<RegionDepthStats> RegionDepthStats::compute(const std::vector<float>& depth, int width, int height, int x0,
                                                          int y0, int x1, int y1) {
    if (width <= 0 || height <= 0 || depth.size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
        return std::nullopt;
    }
    RegionDepthStats s;
    s.sensor_min_x = std::max(0, std::min(x0, x1));
    s.sensor_max_x = std::min(width - 1, std::max(x0, x1));
    s.sensor_min_y = std::max(0, std::min(y0, y1));
    s.sensor_max_y = std::min(height - 1, std::max(y0, y1));
    if (s.sensor_min_x > s.sensor_max_x || s.sensor_min_y > s.sensor_max_y) return std::nullopt;
    s.total_pixels = (s.sensor_max_x - s.sensor_min_x + 1) * (s.sensor_max_y - s.sensor_min_y + 1);
    std::vector<double> valid;
    valid.reserve(static_cast<size_t>(s.total_pixels));
    for (int y = s.sensor_min_y; y <= s.sensor_max_y; ++y) {
        const float* row = depth.data() + static_cast<size_t>(y) * width;
        for (int x = s.sensor_min_x; x <= s.sensor_max_x; ++x) {
            const float d = row[x];
            if (std::isfinite(d) && d > 0.0f) valid.push_back(d);
            else ++s.invalid_pixels;
        }
    }
    s.valid_pixels = static_cast<int>(valid.size());
    s.invalid_share = static_cast<double>(s.invalid_pixels) / static_cast<double>(s.total_pixels);
    if (valid.empty()) return s;
    const size_t n = valid.size();
    double sum = 0.0, lo = valid[0], hi = valid[0];
    for (double v : valid) {
        sum += v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const double mean = sum / static_cast<double>(n);
    double sq = 0.0;
    for (double v : valid) sq += (v - mean) * (v - mean);
    // The median without a full sort: the middle element, and for an even count the largest of the
    // lower half (which nth_element leaves before it).
    const auto mid = valid.begin() + static_cast<std::ptrdiff_t>(n / 2);
    std::nth_element(valid.begin(), mid, valid.end());
    double median = *mid;
    if (n % 2 == 0) median = 0.5 * (median + *std::max_element(valid.begin(), mid));
    s.mean_mm = mean;
    s.median_mm = median;
    s.stddev_mm = std::sqrt(sq / static_cast<double>(n));
    s.min_mm = lo;
    s.max_mm = hi;
    return s;
}

std::wstring RegionDepthStats::text() const {
    wchar_t b[256];
    if (!mean_mm) {
        swprintf(b, 256, L"Region %dx%d: no valid depth (%.0f%% invalid)", sensor_max_x - sensor_min_x + 1,
                 sensor_max_y - sensor_min_y + 1, invalid_share * 100.0);
    } else {
        swprintf(b, 256, L"Region %dx%d: mean %.0f, median %.0f, sd %.1f, min %.0f, max %.0f mm; %.1f%% invalid",
                 sensor_max_x - sensor_min_x + 1, sensor_max_y - sensor_min_y + 1, *mean_mm, *median_mm, *stddev_mm, *min_mm,
                 *max_mm, invalid_share * 100.0);
    }
    return b;
}

// ---- ruler ----------------------------------------------------------------------------------------

std::wstring RulerMeasurement::label() const {
    if (!error.empty()) return error;
    if (distance_mm) {
        wchar_t b[96];
        swprintf(b, 96, L"%.1f mm (%.3f m)", *distance_mm, *distance_mm / 1000.0f);
        return b;
    }
    return L"Click the second point…";
}

void RulerMeasurement::evaluate(RulerMeasurement& m, const kv_ir_intrinsics_t& intr, const std::vector<float>& depth,
                                int width, int height) {
    m.distance_mm.reset();
    m.error.clear();
    if (!m.b) return;
    if (!m.a.has_sensor_coord || !m.b->has_sensor_coord) {
        m.error = kNoDepth;
        return;
    }
    if (!intr.valid || !(intr.fx > 0.0f) || !(intr.fy > 0.0f)) {
        m.error = kIntrinsicsUnavailable;
        return;
    }
    if (depth.size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
        m.error = kNoDepth;
        return;
    }
    float d = 0.0f;
    if (kv_measure_3d_distance_mm(&intr, depth.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                  m.a.sensor_x, m.a.sensor_y, m.b->sensor_x, m.b->sensor_y, &d) &&
        std::isfinite(d)) {
        m.distance_mm = d;
    } else {
        m.error = kNoDepth;
    }
}

// ---- viewport -------------------------------------------------------------------------------------

bool ViewportTransform::isZoomedOrPanned() const {
    return zoom > 1.001 || std::fabs(pan_x) > 0.5 || std::fabs(pan_y) > 0.5;
}

void ViewportTransform::reset() {
    zoom = 1.0;
    pan_x = pan_y = 0.0;
}

RectD ViewportTransform::baseAspectFitRect(double vw, double vh, double iw, double ih) {
    if (vw <= 1 || vh <= 1 || iw <= 0 || ih <= 0) return {};
    const double s = std::min(vw / iw, vh / ih);
    const double w = iw * s, h = ih * s;
    return {(vw - w) * 0.5, (vh - h) * 0.5, w, h};
}

RectD ViewportTransform::displayedImageRect(double vw, double vh, double iw, double ih) const {
    const RectD base = baseAspectFitRect(vw, vh, iw, ih);
    if (base.w <= 0 || base.h <= 0) return {};
    const double cx = vw * 0.5, cy = vh * 0.5;
    return {cx + (base.x - cx) * zoom + pan_x, cy + (base.y - cy) * zoom + pan_y, base.w * zoom, base.h * zoom};
}

void ViewportTransform::clampPan(double vw, double vh, double iw, double ih) {
    const RectD base = baseAspectFitRect(vw, vh, iw, ih);
    const double w = base.w * zoom, h = base.h * zoom;
    const double max_x = std::max(0.0, (w - vw) * 0.5 + vw * 0.25);
    const double max_y = std::max(0.0, (h - vh) * 0.5 + vh * 0.25);
    pan_x = std::min(max_x, std::max(-max_x, pan_x));
    pan_y = std::min(max_y, std::max(-max_y, pan_y));
}

void ViewportTransform::zoomAround(double cx_view, double cy_view, double vw, double vh, double iw, double ih,
                                   double factor) {
    const double old_zoom = zoom;
    const double new_zoom = std::min(kMaxZoom, std::max(kMinZoom, old_zoom * factor));
    if (new_zoom <= 1.001) {
        reset();
        return;
    }
    const double ratio = new_zoom / old_zoom;
    const double cx = vw * 0.5, cy = vh * 0.5;
    pan_x = (cx_view - cx) * (1.0 - ratio) + pan_x * ratio;
    pan_y = (cy_view - cy) * (1.0 - ratio) + pan_y * ratio;
    zoom = new_zoom;
    clampPan(vw, vh, iw, ih);
}

void ViewportTransform::panBy(double dx, double dy, double vw, double vh, double iw, double ih) {
    if (zoom <= 1.001) {
        pan_x = pan_y = 0.0;
        return;
    }
    pan_x += dx;
    pan_y += dy;
    clampPan(vw, vh, iw, ih);
}

bool ViewportTransform::viewToDisplayPixel(double vx, double vy, double vw, double vh, int iw, int ih, int& px,
                                           int& py) const {
    if (iw <= 0 || ih <= 0) return false;
    const RectD r = displayedImageRect(vw, vh, iw, ih);
    if (r.w <= 0 || r.h <= 0 || !r.contains(vx, vy)) return false;
    px = std::min(iw - 1, std::max(0, static_cast<int>((vx - r.x) / r.w * iw)));
    py = std::min(ih - 1, std::max(0, static_cast<int>((vy - r.y) / r.h * ih)));
    return true;
}

void ViewportTransform::displayPixelCenterInView(int px, int py, double vw, double vh, int iw, int ih, double& vx,
                                                 double& vy) const {
    const RectD r = displayedImageRect(vw, vh, iw, ih);
    vx = vy = 0.0;
    if (r.w <= 0 || r.h <= 0 || iw <= 0 || ih <= 0) return;
    vx = r.x + (px + 0.5) / iw * r.w;
    vy = r.y + (py + 0.5) / ih * r.h;
}

SensorPixel mapDisplayPixelToSensor(int dx, int dy, int iw, int ih, kv_view_mode_t mode, kv_sbs_layout_t sbs, bool mirror,
                                    bool rotate180) {
    SensorPixel s;
    if (iw <= 0 || ih <= 0) return s;
    const bool flip_x = mirror != rotate180;
    const bool flip_y = rotate180;
    const int cx = flip_x ? iw - 1 - dx : dx;
    const int cy = flip_y ? ih - 1 - dy : dy;
    auto clampSensor = [&](int x, int y, bool ok) {
        s.x = std::min(511, std::max(0, x));
        s.y = std::min(423, std::max(0, y));
        s.has_depth_ir = ok;
        return s;
    };
    switch (mode) {
        case KV_MODE_IR:
        case KV_MODE_DEPTH:
            return clampSensor(cx, cy, iw == 512 && ih == 424);
        case KV_MODE_SIDE_BY_SIDE:
            if (sbs == KV_SBS_IR_DEPTH) {
                // 1024x424: [0..511] IR, [512..1023] Depth.
                return clampSensor(cx >= 512 ? cx - 512 : cx, cy, iw == 1024 && ih == 424);
            }
            // 1280x424: [0..753] colour scaled from 1080p, [754..767] divider, [768..1279] depth/IR.
            if (cx >= 768) return clampSensor(cx - 768, cy, true);
            if (cx < 754) {
                s.x = std::min(1919, std::max(0, static_cast<int>(cx * 1920.0 / 754.0)));
                s.y = std::min(1079, std::max(0, static_cast<int>(cy * 1080.0 / 424.0)));
                s.has_depth_ir = false;
                return s;
            }
            s.x = cx;
            s.y = cy;
            return s;
        default: // Colour, 3D: no depth/IR pixel under the cursor
            s.x = cx;
            s.y = cy;
            return s;
    }
}

void panelColumnsAt(int start_x, int w, kv_view_mode_t mode, kv_sbs_layout_t sbs, bool mirror, bool rotate180, int& x0,
                    int& x1) {
    x0 = 0;
    x1 = std::max(0, w - 1);
    if (mode != KV_MODE_SIDE_BY_SIDE || w <= 0) return;
    const bool flip_x = mirror != rotate180;
    const int c = flip_x ? w - 1 - start_x : start_x; // canonical column
    int c0 = 0, c1 = w - 1;
    if (sbs == KV_SBS_IR_DEPTH) {
        if (c < 512) c1 = std::min(w - 1, 511);
        else c0 = 512;
    } else if (c < 754) {
        c1 = std::min(w - 1, 753); // the colour panel
    } else if (c < 768) {
        c0 = 754; // the divider
        c1 = std::min(w - 1, 767);
    } else {
        c0 = 768; // the depth / IR panel
    }
    if (flip_x) {
        x0 = w - 1 - c1;
        x1 = w - 1 - c0;
    } else {
        x0 = c0;
        x1 = c1;
    }
}

// ---- health -------------------------------------------------------------------------------------------

bool shouldShowDropBanner(double ir, double depth) {
    return ir > kDropBannerRatio10s || depth > kDropBannerRatio10s;
}

std::vector<std::wstring> healthLines(const kv_health_snapshot_t& h) {
    std::vector<std::wstring> out;
    wchar_t b[320];
    swprintf(b, 320, L"fps: IR %.1f  Depth %.1f  Colour %.1f", h.ir_fps, h.depth_fps, h.color_fps);
    out.push_back(b);
    swprintf(b, 320, L"Dropped per minute: IR %.1f  Depth %.1f  Colour %.1f", h.ir_drops_per_min, h.depth_drops_per_min,
             h.color_drops_per_min);
    out.push_back(b);
    swprintf(b, 320, L"Frame to frame: p50 %.1f ms  p95 %.1f ms   Depth processing %.2f ms", h.inter_frame_p50_ms,
             h.inter_frame_p95_ms, h.depth_processing_ms);
    out.push_back(b);
    std::wstring usb;
    for (const char* p = h.usb_location; p < h.usb_location + sizeof(h.usb_location) && *p; ++p) {
        usb += static_cast<wchar_t>(static_cast<unsigned char>(*p));
    }
    std::wstring last;
    for (const char* p = h.last_driver_error; p < h.last_driver_error + sizeof(h.last_driver_error) && *p; ++p) {
        last += static_cast<wchar_t>(static_cast<unsigned char>(*p));
    }
    std::wstring line = L"Driver errors " + std::to_wstring(h.driver_errors);
    if (!last.empty()) line += L" (last: " + last + L")";
    line += L"   USB " + (usb.empty() ? std::wstring(kDash) : usb);
    out.push_back(line);
    return out;
}

// ---- shortcuts ------------------------------------------------------------------------------------------

Shortcut shortcutForKey(unsigned vk, bool shift, bool ctrl_or_alt) {
    if (ctrl_or_alt) return Shortcut::None;
    switch (vk) {
        case '1': return shift ? Shortcut::None : Shortcut::ModeIr;
        case '2': return shift ? Shortcut::None : Shortcut::ModeColour;
        case '3': return shift ? Shortcut::None : Shortcut::ModeDepth;
        case '4': return shift ? Shortcut::None : Shortcut::ModeSideBySide;
        case '5': return shift ? Shortcut::None : Shortcut::Mode3D;
        case 'F': return Shortcut::Fullscreen;
        case 'M': return Shortcut::Mirror;
        case 'R': return Shortcut::Rotate180;
        case 'H': return Shortcut::Health;
        case 'B': return Shortcut::Blackout;
        case 0x20: return Shortcut::Freeze;     // VK_SPACE
        case 0x1B: return Shortcut::ClearTools; // VK_ESCAPE
        case 0xBF: return shift ? Shortcut::Help : Shortcut::None; // VK_OEM_2 ('/?' on US layouts)
        default: return Shortcut::None;
    }
}

Shortcut shortcutForChar(wchar_t ch) {
    return ch == L'?' ? Shortcut::Help : Shortcut::None;
}

std::vector<std::wstring> helpLines() {
    return {
        L"1  IR      2  Colour      3  Depth      4  Side by side      5  3D point cloud",
        L"F  full screen      M  mirror      R  rotate 180°",
        L"Space  freeze / unfreeze (the readout, region and ruler work on a frozen frame)",
        L"H  health display      ?  this help      Esc  clear the region and the ruler",
        L"B  black out the second display at once (again to bring the picture back)",
        L"Mouse: hover for depth and IR under the cursor",
        L"Drag a box for region stats (Shift-drag when zoomed in)",
        L"Click two points for the 3D ruler",
        L"Scroll to zoom around the cursor, drag to pan when zoomed (or right-drag), double-click to reset",
        L"3D: drag to orbit, Shift-drag or right-drag to pan, scroll to zoom, double-click to reset the view",
    };
}

} // namespace kvwin
