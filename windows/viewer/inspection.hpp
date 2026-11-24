// Inspection tools for the Windows viewer (as on the Mac, macos/App/InspectionModels.swift):
// cursor readout, region depth stats, the 3D ruler, the zoom/pan viewport, the
// display-to-sensor pixel mapping, the dropped-frames banner, the health lines and the shortcuts.
// Pure logic, no window, so it is unit-tested.
#pragma once

#include "kinect_core_c.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kvwin {

// ---- cursor readout ---------------------------------------------------------------------------
struct CursorReadout {
    int pixel_x = 0; // display-image pixel
    int pixel_y = 0;
    bool has_sensor_depth_ir = false; // the pixel maps onto the 512x424 depth/IR image
    std::optional<float> depth_mm;
    std::optional<float> raw_ir;

    std::wstring coordText() const; // "(x, y)"
    std::wstring depthText() const; // "1234 mm", "No depth", or an em dash off the depth image
    std::wstring irText() const;    // "5120", or an em dash
};

// ---- region stats -------------------------------------------------------------------------------
struct RegionDepthStats {
    int sensor_min_x = 0, sensor_min_y = 0, sensor_max_x = 0, sensor_max_y = 0;
    int total_pixels = 0;
    int valid_pixels = 0;
    int invalid_pixels = 0;
    double invalid_share = 0.0;
    std::optional<double> mean_mm, median_mm, stddev_mm, min_mm, max_mm;

    // Over the inclusive box (any corner order, clamped to the image); invalid = not finite or <= 0.
    // The standard deviation is the population one, as on the Mac. nullopt for an empty buffer.
    static std::optional<RegionDepthStats> compute(const std::vector<float>& depth_mm, int width, int height, int x0,
                                                   int y0, int x1, int y1);
    std::wstring text() const;
};

// ---- ruler --------------------------------------------------------------------------------------
struct RulerEndpoint {
    int disp_x = 0, disp_y = 0;
    int sensor_x = 0, sensor_y = 0;
    bool has_sensor_coord = false;
};

struct RulerMeasurement {
    static constexpr const wchar_t* kNoDepth = L"no depth here";
    static constexpr const wchar_t* kIntrinsicsUnavailable = L"Intrinsics unavailable";

    RulerEndpoint a;
    std::optional<RulerEndpoint> b;
    std::optional<float> distance_mm;
    std::wstring error;

    // "1234.5 mm (1.235 m)", the error, or a prompt for the second point.
    std::wstring label() const;
    // The real 3D distance through libfreenect2's IR camera parameters (kv_measure_3d_distance_mm):
    // never hard-coded intrinsics; "no depth here" if either end has none.
    static void evaluate(RulerMeasurement& m, const kv_ir_intrinsics_t& intrinsics, const std::vector<float>& depth_mm,
                         int width = 512, int height = 424);
};

// ---- viewport (zoom around the cursor, pan, reset) ------------------------------------------------
struct RectD {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct ViewportTransform {
    static constexpr double kMinZoom = 1.0;
    static constexpr double kMaxZoom = 16.0;
    double zoom = 1.0;
    double pan_x = 0.0, pan_y = 0.0;

    bool isZoomedOrPanned() const;
    void reset();
    static RectD baseAspectFitRect(double view_w, double view_h, double image_w, double image_h);
    RectD displayedImageRect(double view_w, double view_h, double image_w, double image_h) const;
    // Keeps the image point under the cursor fixed; back to 1x resets.
    void zoomAround(double cursor_x, double cursor_y, double view_w, double view_h, double image_w, double image_h,
                    double factor);
    void panBy(double dx, double dy, double view_w, double view_h, double image_w, double image_h);
    // View point (top-left origin) to display-image pixel; false off the image.
    bool viewToDisplayPixel(double vx, double vy, double view_w, double view_h, int image_w, int image_h, int& px,
                            int& py) const;
    // The centre of a display pixel, in view coordinates.
    void displayPixelCenterInView(int px, int py, double view_w, double view_h, int image_w, int image_h, double& vx,
                                  double& vy) const;

private:
    void clampPan(double view_w, double view_h, double image_w, double image_h);
};

struct SensorPixel {
    int x = 0, y = 0;
    bool has_depth_ir = false; // a pixel of the 512x424 depth/IR image (not the colour one)
};

// A display-image pixel back through mirror / rotate-180 and the Side-by-Side panels to the
// depth/IR sensor pixel (ViewportTransform.mapDisplayPixelToSensor on the Mac).
SensorPixel mapDisplayPixelToSensor(int disp_x, int disp_y, int image_w, int image_h, kv_view_mode_t mode,
                                    kv_sbs_layout_t sbs, bool mirror, bool rotate180);

// The columns (display pixels, inclusive) of the Side-by-Side panel that holds display column
// start_x, through mirror and rotate-180: a region dragged from one panel stays in it, so its two
// corners never come from different images. The whole width in any other mode.
void panelColumnsAt(int start_x, int image_w, kv_view_mode_t mode, kv_sbs_layout_t sbs, bool mirror, bool rotate180,
                    int& x0, int& x1);

// ---- health ---------------------------------------------------------------------------------------
constexpr const wchar_t* kDropBannerText =
    L"Frames are dropping. Try a different USB port, ideally a rear port directly on the motherboard.";
constexpr double kDropBannerRatio10s = 0.02;
bool shouldShowDropBanner(double ir_drop_ratio_10s, double depth_drop_ratio_10s);
// The health display's lines: fps per stream, drops per minute, frame-to-frame p50/p95, depth
// processing time, driver errors (and the last one), USB location.
std::vector<std::wstring> healthLines(const kv_health_snapshot_t& h);

// ---- shortcuts --------------------------------------------------------------------------------------
enum class Shortcut {
    None,
    ModeIr, ModeColour, ModeDepth, ModeSideBySide, Mode3D, // 1-5
    Fullscreen,  // F
    Mirror,      // M
    Rotate180,   // R
    Freeze,      // Space
    Health,      // H
    Help,        // ?
    ClearTools,  // Esc
    Blackout,    // B: the second display goes black at once (and back)
};
// vk: the virtual key of a WM_KEYDOWN (letters and digits as their ASCII code); shift held.
// '?' is Shift+/ on the US layout; on other layouts it arrives through shortcutForChar.
Shortcut shortcutForKey(unsigned vk, bool shift, bool ctrl_or_alt);
Shortcut shortcutForChar(wchar_t ch);
// The help overlay: every shortcut and mouse gesture, one per line.
std::vector<std::wstring> helpLines();

} // namespace kvwin
