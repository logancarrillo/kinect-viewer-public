// The 3D point cloud's orbit camera (the Mac's PointCloudCameraState, macos/Rendering3D/
// MetalPointCloudRenderer.swift): yaw wraps fully, pitch is clamped, zoom and pan
// are bounded, and the view-projection matrix is the same right-handed one with [0, 1] depth (as
// D3D11 uses too). Pure math, so it is unit-tested; the phone's 0x09 gestures drive the same calls.
#pragma once

#include "kinect_core_c.h"

#include <array>

namespace kvwin {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct PointCloudCamera {
    static constexpr float kDefaultDistance = 2.05f; // metres
    static constexpr float kPivotZ = 1.85f;          // the orbit's centre, in front of the sensor
    static constexpr float kMinDistance = 0.28f;
    static constexpr float kMaxDistance = 9.50f;
    static constexpr float kMaxAbsPitch = 1.48f;     // radians
    static constexpr float kFovY = 54.0f * 3.14159265358979f / 180.0f;

    float yaw = 0.0f;
    float pitch = 0.0f;
    float distance = kDefaultDistance;
    Vec3 pan{};

    bool isDefault() const;
    void reset();
    void orbit(float delta_yaw, float delta_pitch); // yaw wraps into [-pi, pi)
    void zoom(float scale_factor);                   // > 1 closer
    void panBy(float right_m, float up_m);           // along the camera's right and up, each axis within +-4 m
    Vec3 eye() const;
    Vec3 target() const;
    // Column-major 4x4 (m[col * 4 + row]); clip = M * (x, y, z, 1) with the sensor's points at -z.
    std::array<float, 16> viewProjection(float aspect) const;
};

// The phone's 0x09 gesture, applied as the Mac does: reset, or orbit, zoom and pan. The wire's
// zoomScale multiplies the camera distance (< 1 closer), so the camera zooms by 1 / zoomScale.
void applyPhoneGesture(PointCloudCamera& c, float delta_yaw, float delta_pitch, float zoom_scale, float pan_right_m,
                       float pan_up_m, bool reset_view);

// Clip-space position of a sensor point (col, row, depth_mm) the way the shader computes it:
// x = (col + 0.5 - cx) / fx * z, y = -(row + 0.5 - cy) / fy * z (libfreenect2's centre convention).
// For tests; false when it would be dropped (no depth).
bool projectSensorPoint(const PointCloudCamera& cam, float aspect, float fx, float fy, float cx, float cy, int col, int row,
                        float depth_mm, bool flip_x, bool flip_y, float out_clip[4]);

// The rule: the 3D view uses the depth camera's own intrinsics or nothing (no made-up
// fallback values). A recording without them can't be shown in 3D.
bool pointCloudIntrinsicsUsable(const kv_ir_intrinsics_t& intr);
constexpr const wchar_t* kNo3DIntrinsics =
    L"3D needs the depth camera's intrinsics, and there are none (an older recording?). Nothing is drawn.";

// The render's orientation: flip X when mirror and rotate-180 differ, flip Y with rotate-180 (the Mac's).
inline bool pointCloudFlipX(bool mirror, bool rotate180) { return mirror != rotate180; }
inline bool pointCloudFlipY(bool, bool rotate180) { return rotate180; }
// The Mac's point size range, and the size from which points are drawn round.
float clampPointSizePx(float px); // [1, 16]
constexpr float kRoundPointsFromPx = 2.5f;

} // namespace kvwin
