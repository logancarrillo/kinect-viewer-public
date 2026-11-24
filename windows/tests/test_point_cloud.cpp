// Stage 5: the 3D point cloud. The orbit camera's math (the Mac's PointCloudCameraState), the
// projection with col + 0.5 - cx (phase 7), the D3D11 renderer on made-up depth (where a patch lands,
// mirror and rotate, the depth test, point size and round points, no fallback intrinsics), the core's
// 3D inputs on the synthetic source (cut-off, colour sources), and the 3D frame rate there.
// Synthetic only: no sensor, nothing read from or written to disk.
#include "kv_test.hpp"

#include "point_cloud_camera.hpp"
#include "point_cloud_renderer.hpp"
#include "viewer_model.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace kvwin;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr uint32_t kW = PointCloudRenderer::kWidth, kH = PointCloudRenderer::kHeight;
constexpr size_t kCount = 512u * 424u;

bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

kv_ir_intrinsics_t intrinsics() {
    kv_ir_intrinsics_t i{};
    i.valid = true;
    i.fx = 365.456f; // the synthetic source's
    i.fy = 365.456f;
    i.cx = 254.878f;
    i.cy = 205.395f;
    return i;
}

std::string tempLock() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "kv-point-cloud-" + std::to_string(GetCurrentProcessId()) + ".lock";
}

struct Scene {
    std::vector<float> depth = std::vector<float>(kCount, 0.0f);
    std::vector<uint8_t> bgra = std::vector<uint8_t>(kCount * 4, 0);
    void put(int col, int row, float mm, uint8_t b, uint8_t g, uint8_t r) {
        const size_t i = static_cast<size_t>(row) * 512 + static_cast<size_t>(col);
        depth[i] = mm;
        bgra[i * 4 + 0] = b;
        bgra[i * 4 + 1] = g;
        bgra[i * 4 + 2] = r;
        bgra[i * 4 + 3] = 255;
    }
    void patch(int c0, int r0, int c1, int r1, float mm, uint8_t b, uint8_t g, uint8_t r) {
        for (int y = r0; y < r1; ++y)
            for (int x = c0; x < c1; ++x) put(x, y, mm, b, g, r);
    }
};

struct Lit {
    int count = 0;
    double cx = 0, cy = 0;
};

Lit lit(const std::vector<uint8_t>& img) {
    Lit l;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            const uint8_t* p = img.data() + (static_cast<size_t>(y) * kW + x) * 4;
            if (p[0] || p[1] || p[2]) {
                ++l.count;
                l.cx += x;
                l.cy += y;
            }
        }
    if (l.count) {
        l.cx /= l.count;
        l.cy /= l.count;
    }
    return l;
}

PointCloudRenderer& renderer() {
    static PointCloudRenderer r;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::string err;
        if (!r.init(&err)) std::printf("  renderer init failed: %s\n", err.c_str());
        else std::printf("  renderer on %s\n", r.usingWarp() ? "WARP (no hardware device)" : "the hardware device");
    }
    return r;
}

bool draw(const Scene& s, std::vector<uint8_t>& out, float size = 2.0f, bool mirror = false, bool rotate = false,
          const PointCloudCamera& cam = PointCloudCamera{}) {
    std::wstring why;
    return renderer().render(s.depth.data(), s.bgra.data(), intrinsics(), cam, size, mirror, rotate, kW, kH, out, &why);
}

} // namespace

// ---- the camera ---------------------------------------------------------------------------------

KV_TEST(camera_starts_at_the_macs_defaults) {
    PointCloudCamera c;
    KV_CHECK(c.isDefault());
    KV_CHECK(approx(c.distance, 2.05, 1e-6));
    const Vec3 t = c.target(), e = c.eye();
    KV_CHECK(approx(t.z, -1.85, 1e-6));
    KV_CHECK(approx(e.z, -1.85 + 2.05, 1e-5)); // in front of the pivot, looking down -Z
    KV_CHECK(approx(e.x, 0, 1e-6) && approx(e.y, 0, 1e-6));
}

KV_TEST(camera_yaw_wraps_pitch_clamps_zoom_and_pan_are_bounded) {
    PointCloudCamera c;
    c.orbit(3.0f * kPi + 0.25f, 0.0f); // three half-turns and a bit: wraps fully, not just once
    KV_CHECK(c.yaw >= -kPi && c.yaw < kPi);
    KV_CHECK(approx(c.yaw, -kPi + 0.25f, 1e-4));
    c.orbit(-20.0f * kPi, 0.0f);
    KV_CHECK(c.yaw >= -kPi && c.yaw < kPi);
    c.orbit(0.0f, 5.0f);
    KV_CHECK(approx(c.pitch, 1.48, 1e-6));
    c.orbit(0.0f, -10.0f);
    KV_CHECK(approx(c.pitch, -1.48, 1e-6));
    c.zoom(1000.0f);
    KV_CHECK(approx(c.distance, 0.28, 1e-6));
    c.zoom(0.001f); // too small a factor: ignored, as a NaN is
    KV_CHECK(approx(c.distance, 0.28, 1e-6));
    c.zoom(std::nanf(""));
    KV_CHECK(approx(c.distance, 0.28, 1e-6));
    c.zoom(0.02f);
    KV_CHECK(approx(c.distance, 9.5, 1e-6));
    c.panBy(100.0f, -100.0f);
    KV_CHECK(std::fabs(c.pan.x) <= 4.0f + 1e-5f && std::fabs(c.pan.y) <= 4.0f + 1e-5f && std::fabs(c.pan.z) <= 4.0f + 1e-5f);
    c.reset();
    KV_CHECK(c.isDefault());
}

KV_TEST(phone_gestures_as_the_mac_applies_them) {
    PointCloudCamera c;
    applyPhoneGesture(c, 0.3f, -0.2f, 0.5f, 0.0f, 0.0f, false); // zoomScale 0.5 halves the distance
    KV_CHECK(approx(c.yaw, 0.3, 1e-6) && approx(c.pitch, -0.2, 1e-6) && approx(c.distance, 1.025, 1e-5));
    applyPhoneGesture(c, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, false);
    KV_CHECK(approx(c.distance, 2.05, 1e-5));
    applyPhoneGesture(c, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false); // a zero scale is no zoom
    KV_CHECK(approx(c.distance, 2.05, 1e-5));
    applyPhoneGesture(c, 0.0f, 0.0f, 1.0f, 0.5f, 0.25f, false);
    KV_CHECK(std::fabs(c.pan.x) + std::fabs(c.pan.y) + std::fabs(c.pan.z) > 0.5f);
    applyPhoneGesture(c, 1.0f, 1.0f, 3.0f, 1.0f, 1.0f, true); // reset wins over the rest
    KV_CHECK(c.isDefault());
}

namespace {
bool cameraSane(const PointCloudCamera& c) {
    return std::isfinite(c.yaw) && std::isfinite(c.pitch) && std::isfinite(c.distance) && std::isfinite(c.pan.x) &&
           std::isfinite(c.pan.y) && std::isfinite(c.pan.z) && c.yaw >= -kPi && c.yaw < kPi && std::fabs(c.pitch) <= 1.48f + 1e-6f &&
           c.distance >= 0.28f - 1e-6f && c.distance <= 9.5f + 1e-6f && std::fabs(c.pan.x) <= 4.0f + 1e-5f &&
           std::fabs(c.pan.y) <= 4.0f + 1e-5f && std::fabs(c.pan.z) <= 4.0f + 1e-5f;
}
} // namespace

// 0x09 comes off the network: NaN, infinities and huge values never reach the camera as NaN.
KV_TEST(phone_gesture_values_are_sanitised) {
    const float nan = std::nanf(""), inf = std::numeric_limits<float>::infinity(), big = std::numeric_limits<float>::max();
    const float bad[] = {nan, inf, -inf, big, -big, 1e30f, -1e30f, std::numeric_limits<float>::denorm_min()};
    PointCloudCamera c;
    for (float v : bad) {
        c.orbit(v, v);
        KV_CHECK_MSG(cameraSane(c), "orbit " + std::to_string(v));
        c.zoom(v);
        KV_CHECK_MSG(cameraSane(c), "zoom " + std::to_string(v));
        c.panBy(v, v);
        KV_CHECK_MSG(cameraSane(c), "panBy " + std::to_string(v));
        c.panBy(v, -v); // opposite huge values: no inf - inf
        KV_CHECK_MSG(cameraSane(c), "panBy opposite " + std::to_string(v));
        for (float w : bad) {
            applyPhoneGesture(c, v, w, v, w, v, false);
            KV_CHECK_MSG(cameraSane(c), "gesture " + std::to_string(v) + " " + std::to_string(w));
        }
    }
    // NaN and infinities are ignored outright; a huge zoomScale goes as far as the camera goes.
    PointCloudCamera d;
    applyPhoneGesture(d, nan, inf, nan, -inf, nan, false);
    KV_CHECK(d.isDefault());
    applyPhoneGesture(d, 0.0f, 0.0f, 1e30f, 0.0f, 0.0f, false);
    KV_CHECK(approx(d.distance, 9.5, 1e-5));
    applyPhoneGesture(d, 0.0f, 0.0f, 1e-30f, 0.0f, 0.0f, false); // tiny: no zoom (as the Mac's > 0.01)
    KV_CHECK(approx(d.distance, 9.5, 1e-5));
    applyPhoneGesture(d, 0.0f, 0.0f, -2.0f, 0.0f, 0.0f, false);
    KV_CHECK(approx(d.distance, 9.5, 1e-5));
    PointCloudCamera e;
    e.panBy(big, -big);
    KV_CHECK(cameraSane(e) && (std::fabs(e.pan.x) > 3.9f || std::fabs(e.pan.z) > 3.9f)); // to the edge, not NaN
}

KV_TEST(projection_puts_the_principal_ray_at_the_pivot_in_the_centre) {
    const PointCloudCamera c;
    const kv_ir_intrinsics_t in = intrinsics();
    // col + 0.5 - cx = 0 at col = cx - 0.5 (254.378): the nearest pixel is 254, 0.378 px off centre.
    float clip[4];
    KV_REQUIRE(projectSensorPoint(c, 4.0f / 3.0f, in.fx, in.fy, 255.5f, 212.5f, 255, 212, 1850.0f, false, false, clip));
    KV_CHECK(approx(clip[0] / clip[3], 0.0, 1e-5));
    KV_CHECK(approx(clip[1] / clip[3], 0.0, 1e-5));
    const double z = clip[2] / clip[3];
    KV_CHECK(z > 0.0 && z < 1.0); // D3D's [0, 1] depth
    KV_CHECK(!projectSensorPoint(c, 4.0f / 3.0f, in.fx, in.fy, in.cx, in.cy, 10, 10, 0.0f, false, false, clip)); // no depth
}

KV_TEST(projection_uses_col_plus_half_and_the_flips) {
    const PointCloudCamera c;
    const kv_ir_intrinsics_t in = intrinsics();
    float a[4], b[4];
    // With cx = 100.0, col 99 is half a pixel left of centre and col 100 half a pixel right.
    KV_REQUIRE(projectSensorPoint(c, 1.0f, in.fx, in.fy, 100.0f, 100.0f, 99, 99, 1850.0f, false, false, a));
    KV_REQUIRE(projectSensorPoint(c, 1.0f, in.fx, in.fy, 100.0f, 100.0f, 100, 100, 1850.0f, false, false, b));
    KV_CHECK(approx(a[0] / a[3], -(b[0] / b[3]), 1e-6)); // symmetric about the centre: col + 0.5 - cx
    KV_CHECK(b[0] > 0.0f && b[1] < 0.0f);                // right of centre is right, below is down
    float m[4];
    KV_REQUIRE(projectSensorPoint(c, 1.0f, in.fx, in.fy, 100.0f, 100.0f, 100, 100, 1850.0f, true, true, m));
    KV_CHECK(approx(m[0], -b[0], 1e-6) && approx(m[1], -b[1], 1e-6));
    KV_CHECK(pointCloudFlipX(true, false) && !pointCloudFlipX(true, true) && pointCloudFlipX(false, true));
    KV_CHECK(pointCloudFlipY(false, true) && !pointCloudFlipY(true, false));
}

KV_TEST(no_fallback_intrinsics) {
    kv_ir_intrinsics_t in = intrinsics();
    KV_CHECK(pointCloudIntrinsicsUsable(in));
    in.valid = false;
    KV_CHECK(!pointCloudIntrinsicsUsable(in));
    in = intrinsics();
    in.fx = 1.0f; // the Mac refuses fx <= 1
    KV_CHECK(!pointCloudIntrinsicsUsable(in));
    in = intrinsics();
    in.cy = std::nanf("");
    KV_CHECK(!pointCloudIntrinsicsUsable(in));
    KV_CHECK(approx(clampPointSizePx(0.2f), 1.0, 0) && approx(clampPointSizePx(40.0f), 16.0, 0));
}

// ---- the renderer -------------------------------------------------------------------------------

KV_TEST(renderer_refuses_without_intrinsics_and_draws_nothing) {
    KV_REQUIRE(renderer().ready());
    Scene s;
    s.patch(230, 190, 280, 220, 1850.0f, 0, 0, 255);
    std::vector<uint8_t> out;
    std::wstring why;
    kv_ir_intrinsics_t none{};
    KV_CHECK(!renderer().render(s.depth.data(), s.bgra.data(), none, PointCloudCamera{}, 2.0f, false, false, kW, kH, out, &why));
    KV_CHECK(why == kNo3DIntrinsics);
    KV_CHECK(out.empty());
}

KV_TEST(renderer_places_a_patch_at_the_pivot_in_the_centre_on_black) {
    KV_REQUIRE(renderer().ready());
    Scene s;
    s.patch(235, 186, 275, 226, 1850.0f, 0, 0, 255); // centred on (cx, cy) = (254.9, 205.4)
    std::vector<uint8_t> out;
    KV_REQUIRE(draw(s, out));
    KV_REQUIRE(out.size() == static_cast<size_t>(kW) * kH * 4);
    const Lit l = lit(out);
    std::printf("  lit=%d centroid=(%.1f, %.1f)\n", l.count, l.cx, l.cy);
    KV_CHECK(l.count > 500);
    KV_CHECK(approx(l.cx, kW / 2.0, 4.0) && approx(l.cy, kH / 2.0, 4.0));
    const uint8_t* corner = out.data();
    KV_CHECK(corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255); // black, opaque
    const uint8_t* mid = out.data() + (static_cast<size_t>(kH / 2) * kW + kW / 2) * 4;
    KV_CHECK(mid[2] == 255 && mid[1] == 0 && mid[0] == 0); // the point colour as given (BGRA)
}

KV_TEST(renderer_mirror_and_rotate_flip_the_picture) {
    KV_REQUIRE(renderer().ready());
    Scene s;
    s.patch(380, 300, 420, 340, 1850.0f, 0, 255, 0); // right of and below centre
    std::vector<uint8_t> out;
    KV_REQUIRE(draw(s, out));
    const Lit plain = lit(out);
    KV_REQUIRE(draw(s, out, 2.0f, true, false));
    const Lit mirrored = lit(out);
    KV_REQUIRE(draw(s, out, 2.0f, false, true));
    const Lit rotated = lit(out);
    KV_REQUIRE(draw(s, out, 2.0f, true, true));
    const Lit both = lit(out);
    std::printf("  plain (%.0f, %.0f) mirror (%.0f, %.0f) rotate (%.0f, %.0f) both (%.0f, %.0f)\n", plain.cx, plain.cy,
                mirrored.cx, mirrored.cy, rotated.cx, rotated.cy, both.cx, both.cy);
    KV_CHECK(plain.cx > kW / 2.0 + 50 && plain.cy > kH / 2.0 + 50);
    KV_CHECK(approx(mirrored.cx, kW - plain.cx, 3.0) && approx(mirrored.cy, plain.cy, 3.0));
    KV_CHECK(approx(rotated.cx, kW - plain.cx, 3.0) && approx(rotated.cy, kH - plain.cy, 3.0));
    KV_CHECK(approx(both.cx, plain.cx, 3.0) && approx(both.cy, kH - plain.cy, 3.0)); // mirror undoes rotate's X
}

KV_TEST(renderer_depth_test_keeps_the_nearer_point) {
    KV_REQUIRE(renderer().ready());
    Scene s;
    // Two neighbouring pixels at the centre, big points so they overlap: the nearer (green) is drawn
    // first and the farther (red) after it, so only the depth test keeps green in front.
    s.put(254, 205, 1500.0f, 0, 255, 0);
    s.put(255, 205, 2600.0f, 0, 0, 255);
    std::vector<uint8_t> out;
    KV_REQUIRE(draw(s, out, 16.0f));
    float clip[4];
    const kv_ir_intrinsics_t in = intrinsics();
    KV_REQUIRE(projectSensorPoint(PointCloudCamera{}, static_cast<float>(kW) / kH, in.fx, in.fy, in.cx, in.cy, 254, 205, 1500.0f,
                                  false, false, clip));
    const int px = static_cast<int>((clip[0] / clip[3] * 0.5 + 0.5) * kW);
    const int py = static_cast<int>((0.5 - clip[1] / clip[3] * 0.5) * kH);
    const uint8_t* p = out.data() + (static_cast<size_t>(py) * kW + px) * 4;
    std::printf("  at (%d, %d): b=%d g=%d r=%d\n", px, py, p[0], p[1], p[2]);
    KV_CHECK(p[1] == 255 && p[2] == 0);
}

// A lost device (here: shut down, as a removal ends up) is made again on the next render.
KV_TEST(renderer_starts_again_after_losing_its_device) {
    KV_REQUIRE(renderer().ready());
    const int before = renderer().recoveries();
    Scene s;
    s.patch(235, 186, 275, 226, 1850.0f, 0, 0, 255);
    std::vector<uint8_t> out;
    renderer().shutdown();
    KV_CHECK(!renderer().ready());
    KV_REQUIRE(draw(s, out));
    KV_CHECK(renderer().ready());
    KV_CHECK(renderer().recoveries() == before + 1);
    const Lit l = lit(out);
    KV_CHECK(l.count > 500 && approx(l.cx, kW / 2.0, 4.0)); // the same picture as before the loss
    // A renderer that was never started starts on its first render too (the viewer's init can fail).
    PointCloudRenderer fresh;
    std::wstring why;
    KV_CHECK(fresh.render(s.depth.data(), s.bgra.data(), intrinsics(), PointCloudCamera{}, 2.0f, false, false, kW, kH, out, &why));
    KV_CHECK(fresh.recoveries() == 0); // a first start, not a recovery
    fresh.shutdown();
}

KV_TEST(renderer_point_size_and_round_points) {
    KV_REQUIRE(renderer().ready());
    Scene s;
    s.put(254, 205, 1850.0f, 255, 255, 255);
    std::vector<uint8_t> out;
    KV_REQUIRE(draw(s, out, 1.0f));
    const int one = lit(out).count;
    KV_REQUIRE(draw(s, out, 2.0f));
    const int two = lit(out).count;
    KV_REQUIRE(draw(s, out, 10.0f));
    const int ten = lit(out).count;
    std::printf("  1 px -> %d, 2 px -> %d, 10 px (round) -> %d\n", one, two, ten);
    KV_CHECK(one >= 1 && one <= 2);
    KV_CHECK(two >= 2 && two <= 6);                  // a square
    KV_CHECK(ten > 60 && ten < 100);                 // a disc (pi * 25 = 78.5), not the 100 px square
}

// ---- the core's 3D inputs on the synthetic source, and the frame rate -------------------------------

namespace {
struct Inputs {
    std::vector<float> depth = std::vector<float>(kCount);
    std::vector<uint8_t> bgra = std::vector<uint8_t>(kCount * 4);
    kv_ir_intrinsics_t intr{};
    uint64_t seq = 0;
};

bool nextInputs(kv_engine_t* eng, Inputs& in, int timeout_ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        uint64_t seq = 0, arr = 0;
        if (kv_engine_copy_point_cloud_inputs(eng, in.seq, in.depth.data(), in.depth.size(), in.bgra.data(), in.bgra.size(), &seq,
                                              &arr, &in.intr)) {
            in.seq = seq;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
} // namespace

KV_TEST(synthetic_3d_inputs_cutoff_and_colour_sources) {
    const std::string lock = tempLock();
    kv_engine_t* eng = kv_engine_create(lock.c_str());
    KV_REQUIRE(eng != nullptr);
    ViewerSettings vs;
    vs.synthetic = true;
    vs.mode = KV_MODE_POINT_CLOUD_3D;
    kv_display_params_t p = toDisplayParams(vs);
    char err[256] = {0};
    KV_REQUIRE(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, true, err, sizeof(err)));

    Inputs in;
    KV_REQUIRE(nextInputs(eng, in));
    KV_CHECK(pointCloudIntrinsicsUsable(in.intr));
    KV_CHECK(approx(in.intr.fx, 365.456, 1e-3)); // the source's own, not a made-up one
    size_t full = 0;
    for (float d : in.depth) full += d > 0.0f;
    const std::vector<uint8_t> depth_colours = in.bgra;

    applyTrackMe(vs);
    p = toDisplayParams(vs);
    kv_engine_set_display_params(eng, &p);
    KV_REQUIRE(nextInputs(eng, in));
    KV_REQUIRE(nextInputs(eng, in)); // one made after the change
    size_t kept = 0, outside = 0;
    for (float d : in.depth) {
        if (d > 0.0f) {
            ++kept;
            if (d < 500.0f || d > 2000.0f) ++outside;
        }
    }
    std::printf("  points: %zu without the cut-off, %zu with Track me (500-2000 mm), %zu outside it\n", full, kept, outside);
    KV_CHECK(full > kCount / 2);
    KV_CHECK(kept > 0 && kept < full);
    KV_CHECK(outside == 0);

    vs.cutoff_enabled = false;
    vs.point_color_source = KV_POINT_COLOR_IR;
    p = toDisplayParams(vs);
    kv_engine_set_display_params(eng, &p);
    KV_REQUIRE(nextInputs(eng, in));
    KV_REQUIRE(nextInputs(eng, in));
    size_t grey = 0, differ = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const uint8_t* c = in.bgra.data() + i * 4;
        grey += (c[0] == c[1] && c[1] == c[2]);
        differ += std::memcmp(c, depth_colours.data() + i * 4, 3) != 0;
    }
    KV_CHECK(grey > kCount * 9 / 10); // IR points are grey
    KV_CHECK(differ > kCount / 4);

    vs.point_color_source = KV_POINT_COLOR_REGISTERED_RGB;
    KV_CHECK(settingsNeedColor(vs)); // RGB points are live colour
    p = toDisplayParams(vs);
    kv_engine_set_display_params(eng, &p);
    KV_REQUIRE(nextInputs(eng, in));
    KV_REQUIRE(nextInputs(eng, in));
    KV_CHECK(kv_engine_is_rgb_active(eng));
    kv_engine_stop(eng);
    kv_engine_destroy(eng);
}

KV_TEST(synthetic_3d_frame_rate) {
    KV_REQUIRE(renderer().ready());
    const std::string lock = tempLock();
    kv_engine_t* eng = kv_engine_create(lock.c_str());
    KV_REQUIRE(eng != nullptr);
    ViewerSettings vs;
    vs.synthetic = true;
    vs.mode = KV_MODE_POINT_CLOUD_3D;
    const kv_display_params_t p = toDisplayParams(vs);
    char err[256] = {0};
    KV_REQUIRE(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, false, err, sizeof(err)));
    Inputs in;
    KV_REQUIRE(nextInputs(eng, in));
    std::vector<uint8_t> out;
    std::wstring why;
    PointCloudCamera cam;
    int frames = 0;
    double render_ms = 0, worst_ms = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto end = t0 + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < end) {
        if (!nextInputs(eng, in, 500)) continue;
        cam.orbit(0.01f, 0.0f); // a moving camera, as when the phone orbits
        KV_REQUIRE(renderer().render(in.depth.data(), in.bgra.data(), in.intr, cam, 2.5f, false, false, kW, kH, out, &why));
        render_ms += renderer().lastRenderMs();
        worst_ms = std::max(worst_ms, renderer().lastRenderMs());
        ++frames;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double fps = frames / secs;
    std::printf("  3D on the synthetic source: %.1f fps over %.1f s (%d frames); render + readback %.2f ms mean, %.2f ms worst "
                "(960x720, 2.5 px points, %s)\n",
                fps, secs, frames, frames ? render_ms / frames : 0.0, worst_ms, renderer().usingWarp() ? "WARP" : "hardware");
    KV_CHECK(fps > 15.0); // the synthetic source makes about 30 a second
    kv_engine_stop(eng);
    kv_engine_destroy(eng);
}

KV_TEST_MAIN()
