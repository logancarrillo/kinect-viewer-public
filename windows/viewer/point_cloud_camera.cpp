#include "point_cloud_camera.hpp"

#include <algorithm>
#include <cmath>

namespace kvwin {

namespace {
constexpr float kPi = 3.14159265358979f;
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 norm(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? mul(a, 1.0f / l) : a;
}

struct Basis {
    Vec3 eye, target, right, up, forward;
};

Basis basis(const PointCloudCamera& c) {
    Basis b;
    b.target = add(Vec3{0.0f, 0.0f, -PointCloudCamera::kPivotZ}, c.pan);
    const float cp = std::cos(c.pitch);
    b.eye = add(b.target, Vec3{c.distance * cp * std::sin(c.yaw), c.distance * std::sin(c.pitch), c.distance * cp * std::cos(c.yaw)});
    b.forward = norm(sub(b.target, b.eye));
    Vec3 r = cross(b.forward, Vec3{0.0f, 1.0f, 0.0f});
    b.right = dot(r, r) < 1e-6f ? Vec3{1.0f, 0.0f, 0.0f} : norm(r);
    b.up = norm(cross(b.right, b.forward));
    return b;
}
} // namespace

bool PointCloudCamera::isDefault() const {
    return std::fabs(yaw) < 1e-4f && std::fabs(pitch) < 1e-4f && std::fabs(distance - kDefaultDistance) < 1e-3f &&
           std::sqrt(dot(pan, pan)) < 1e-4f;
}

void PointCloudCamera::reset() {
    yaw = pitch = 0.0f;
    distance = kDefaultDistance;
    pan = {};
}

// Every input can come from the phone (0x09): a NaN or an infinity is ignored, and a huge value is
// clamped before any arithmetic, so nothing overflows into a NaN in the camera.
void PointCloudCamera::orbit(float dy, float dp) {
    if (std::isfinite(dy)) {
        dy = std::clamp(dy, -64.0f * kPi, 64.0f * kPi); // turns past this are the same turn, and fmod stays exact
        const float two_pi = 2.0f * kPi;
        float w = std::fmod(yaw + dy + kPi, two_pi); // wraps fully (by modulo)
        if (w < 0.0f) w += two_pi;
        yaw = w - kPi;
    }
    if (std::isfinite(dp)) pitch = std::clamp(pitch + std::clamp(dp, -4.0f, 4.0f), -kMaxAbsPitch, kMaxAbsPitch);
}

void PointCloudCamera::zoom(float s) {
    if (!std::isfinite(s) || s <= 0.01f) return;
    distance = std::clamp(distance / std::min(s, 1000.0f), kMinDistance, kMaxDistance);
}

void PointCloudCamera::panBy(float right_m, float up_m) {
    if (!std::isfinite(right_m) || !std::isfinite(up_m)) return;
    // The pan box is 8 m across: a bigger step only reaches its edge, and stays finite.
    right_m = std::clamp(right_m, -16.0f, 16.0f);
    up_m = std::clamp(up_m, -16.0f, 16.0f);
    const Basis b = basis(*this);
    pan = add(pan, add(mul(b.right, right_m), mul(b.up, up_m)));
    pan.x = std::clamp(pan.x, -4.0f, 4.0f);
    pan.y = std::clamp(pan.y, -4.0f, 4.0f);
    pan.z = std::clamp(pan.z, -4.0f, 4.0f);
}

Vec3 PointCloudCamera::eye() const { return basis(*this).eye; }
Vec3 PointCloudCamera::target() const { return basis(*this).target; }

std::array<float, 16> PointCloudCamera::viewProjection(float aspect) const {
    const Basis b = basis(*this);
    const Vec3 f = b.forward, r = b.right, u = b.up, e = b.eye;
    // Column-major view matrix (the camera looks down -Z), as the Mac's.
    const float view[16] = {r.x, u.x, -f.x, 0, r.y, u.y, -f.y, 0, r.z, u.z, -f.z, 0, -dot(r, e), -dot(u, e), dot(f, e), 1};
    const float a = std::max(0.1f, aspect);
    const float ys = 1.0f / std::tan(kFovY * 0.5f), xs = ys / a;
    const float zn = 0.05f, zf = 18.0f, zs = zf / (zn - zf);
    const float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zs, -1, 0, 0, zs * zn, 0};
    std::array<float, 16> m{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += proj[k * 4 + rr] * view[c * 4 + k];
            m[c * 4 + rr] = s;
        }
    return m;
}

bool projectSensorPoint(const PointCloudCamera& cam, float aspect, float fx, float fy, float cx, float cy, int col, int row,
                        float depth_mm, bool flip_x, bool flip_y, float out[4]) {
    if (!(depth_mm > 0.0f) || !std::isfinite(depth_mm)) return false;
    const float z = depth_mm * 0.001f;
    const float x = ((col + 0.5f - cx) / fx) * z * (flip_x ? -1.0f : 1.0f);
    const float y = -((row + 0.5f - cy) / fy) * z * (flip_y ? -1.0f : 1.0f);
    const float p[4] = {x, y, -z, 1.0f};
    const auto m = cam.viewProjection(aspect);
    for (int r = 0; r < 4; ++r) {
        out[r] = 0;
        for (int c = 0; c < 4; ++c) out[r] += m[c * 4 + r] * p[c];
    }
    return true;
}

void applyPhoneGesture(PointCloudCamera& c, float dyaw, float dpitch, float zoom_scale, float pan_r, float pan_u, bool reset) {
    if (reset) {
        c.reset();
        return;
    }
    if (dyaw != 0.0f || dpitch != 0.0f) c.orbit(dyaw, dpitch);
    // A huge scale goes as far as the camera goes (not ignored); zero, negative and NaN are no zoom.
    if (std::isfinite(zoom_scale) && zoom_scale > 0.01f && std::fabs(zoom_scale - 1.0f) > 0.0001f) {
        c.zoom(1.0f / std::min(zoom_scale, 50.0f)); // 50x reaches the far limit from anywhere (9.5 / 0.28 < 50)
    }
    if (pan_r != 0.0f || pan_u != 0.0f) c.panBy(pan_r, pan_u);
}

bool pointCloudIntrinsicsUsable(const kv_ir_intrinsics_t& intr) {
    return intr.valid && std::isfinite(intr.fx) && std::isfinite(intr.fy) && std::isfinite(intr.cx) && std::isfinite(intr.cy) &&
           intr.fx > 1.0f && intr.fy > 1.0f;
}

float clampPointSizePx(float px) {
    return std::isfinite(px) ? std::clamp(px, 1.0f, 16.0f) : 1.0f;
}

} // namespace kvwin
