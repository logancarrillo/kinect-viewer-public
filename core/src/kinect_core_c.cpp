#include "kinect_core_c.h"

#include "frame_mapper.hpp"
#include "frame_source.hpp"
#include "frame_types.hpp"
#include "lock_guard_file.hpp"

#include <libfreenect2/registration.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

struct kv_engine {
    kv::KinectLockFile lock_file;
    bool has_custom_lock_path = false;
    std::unique_ptr<kv::IFrameSource> source;
    kv::FrameMapper mapper;

    mutable std::mutex mutex;

    bool running = false;
    bool lock_blocked = false;
    bool rgb_stream_active = false;
    kv_source_kind_t source_kind = KV_SOURCE_REAL;
    kv_pipeline_kind_t pipeline_kind = KV_PIPELINE_OPENGL;
    kv_display_params_t display_params{};
    kv_color_controls_t color_controls{};

    uint64_t start_ns = 0;

    // Raw latest frames
    kv::RawFloatFrame latest_ir;
    kv::RawFloatFrame latest_depth;
    kv::RawColorFrame latest_color;

    // Raw 16-bit buffers for lossless recording
    std::vector<uint16_t> latest_ir_u16;
    uint64_t latest_ir_u16_seq = 0;
    uint64_t latest_ir_u16_arrival_ns = 0;
    uint32_t latest_ir_u16_ticks = 0;

    std::vector<uint16_t> latest_depth_u16;
    uint64_t latest_depth_u16_seq = 0;
    uint64_t latest_depth_u16_arrival_ns = 0;
    uint32_t latest_depth_u16_ticks = 0;

    // Mapped BGRA output
    std::vector<uint8_t> mapped_bgra;
    uint32_t mapped_w = 0;
    uint32_t mapped_h = 0;
    uint64_t mapped_seq = 0;
    uint64_t mapped_arrival_ns = 0;
    uint64_t mapped_done_ns = 0;
    uint32_t mapped_device_ticks = 0;
    kv::PixelMetrics latest_metrics{};

    // Counters & sequence tracking
    uint64_t ir_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t color_frames = 0;
    uint64_t rendered_frames = 0;

    bool has_last_ir_seq = false;
    uint32_t last_ir_seq = 0;
    uint64_t ir_seq_drops = 0;

    bool has_last_depth_seq = false;
    uint32_t last_depth_seq = 0;
    uint64_t depth_seq_drops = 0;

    bool has_last_color_seq = false;
    uint32_t last_color_seq = 0;
    uint64_t color_seq_drops = 0;

    // Timing history (sliding window)
    struct StreamSample {
        uint64_t arrival_ns = 0;
        uint64_t received = 0;
        uint64_t dropped = 0;
    };

    uint64_t last_primary_arrival_ns = 0;
    std::deque<uint64_t> primary_arrival_window;
    std::deque<uint64_t> ir_arrival_window;
    std::deque<uint64_t> depth_arrival_window;
    std::deque<uint64_t> color_arrival_window;
    std::deque<StreamSample> ir_history_60s;
    std::deque<StreamSample> depth_history_60s;
    std::deque<StreamSample> color_history_60s;
    std::deque<double> inter_frame_ms_window;
    std::deque<double> map_time_ms_window;
    std::deque<double> depth_proc_ms_window;
    double map_time_max_ms = 0.0;
    double inter_frame_max_ms = 0.0;
    uint32_t last_ir_seq_for_proc = 0;
    uint64_t last_ir_arrival_for_proc_ns = 0;

    uint32_t benign_notices = 0;
    uint32_t driver_errors = 0;
    std::string last_driver_error;
    kv_ir_intrinsics_t ir_intrinsics{};
    std::unique_ptr<libfreenect2::Registration> registration;
    libfreenect2::Frame reg_undistorted{512, 424, 4};
    libfreenect2::Frame reg_registered{512, 424, 4};
    libfreenect2::Frame reg_bigdepth{1920, 1082, 4};
    libfreenect2::Frame reg_undistorted_ir{512, 424, 4};
    std::vector<int> reg_color_depth_map = std::vector<int>(512u * 424u, -1);
    std::vector<int32_t> color_grid_480x270 = std::vector<int32_t>(480u * 270u, -1);
    std::vector<float> sanitized_depth_scratch = std::vector<float>(512u * 424u, 0.0f);
    std::vector<uint8_t> bgrx_color_scratch;
    kv::RawFloatFrame undistorted_depth_frame;
    kv::RawFloatFrame undistorted_ir_frame;
    uint64_t reg_depth_seq = 0;
    uint64_t reg_color_seq = 0;
    bool reg_used_color = false;
    uint64_t point_cloud_seq = 0;
    uint64_t point_cloud_arrival_ns = 0;
    std::string usb_location;
    mutable uint64_t last_usb_loc_poll_ns = 0;
    mutable std::string cached_usb_loc;
    kv::ColorReadback color_readback{};
    std::string serial;
    std::string firmware;
    std::string status_message = "Idle";

    explicit kv_engine(const char* lock_path)
        : lock_file(lock_path ? std::string(lock_path) : std::string()),
          has_custom_lock_path(lock_path != nullptr && lock_path[0] != '\0') {
        kv_default_display_params(&display_params);
        kv_default_color_controls(&color_controls);
    }

    void resetStatsLocked() {
        ir_frames = 0;
        depth_frames = 0;
        color_frames = 0;
        rendered_frames = 0;
        has_last_ir_seq = false;
        has_last_depth_seq = false;
        has_last_color_seq = false;
        ir_seq_drops = 0;
        depth_seq_drops = 0;
        color_seq_drops = 0;
        last_primary_arrival_ns = 0;
        primary_arrival_window.clear();
        ir_arrival_window.clear();
        depth_arrival_window.clear();
        color_arrival_window.clear();
        ir_history_60s.clear();
        depth_history_60s.clear();
        color_history_60s.clear();
        inter_frame_ms_window.clear();
        map_time_ms_window.clear();
        depth_proc_ms_window.clear();
        map_time_max_ms = 0.0;
        inter_frame_max_ms = 0.0;
        last_ir_seq_for_proc = 0;
        last_ir_arrival_for_proc_ns = 0;
        last_driver_error.clear();
        ir_intrinsics = {};
        registration.reset();
        reg_depth_seq = 0;
        reg_color_seq = 0;
        reg_used_color = false;
        std::fill(reg_color_depth_map.begin(), reg_color_depth_map.end(), -1);
        std::fill(color_grid_480x270.begin(), color_grid_480x270.end(), -1);
        // Note: mapped_seq, point_cloud_seq, latest_ir_u16_seq, and latest_depth_u16_seq stay strictly monotonic across restarts
        latest_ir = {};
        latest_depth = {};
        latest_color = {};
        latest_ir_u16.clear();
        latest_depth_u16.clear();
        mapper.resetSmoothing();
    }

    bool modeUsesColor(const kv_display_params_t& p) const {
        if (p.depth_color_blend_enabled) return true;
        if (p.mode == KV_MODE_COLOR) return true;
        if (p.mode == KV_MODE_SIDE_BY_SIDE && (p.sbs_layout == KV_SBS_COLOR_DEPTH || p.sbs_layout == KV_SBS_COLOR_IR)) return true;
        if (p.mode == KV_MODE_POINT_CLOUD_3D && p.point_color_source == KV_POINT_COLOR_REGISTERED_RGB) return true;
        return false;
    }

    static libfreenect2::Freenect2Device::ColorCameraParams defaultColorCameraParams() {
        libfreenect2::Freenect2Device::ColorCameraParams cp{};
        cp.fx = 1081.37f;
        cp.fy = 1081.37f;
        cp.cx = 959.5f;
        cp.cy = 539.5f;
        cp.shift_d = 863.0f;
        cp.shift_m = 52.0f;
        cp.mx_x1y0 = 0.650675f;
        cp.my_x0y1 = 0.650675f;
        return cp;
    }

    static libfreenect2::Freenect2Device::IrCameraParams toFreenectIrParams(const kv_ir_intrinsics_t& intr) {
        libfreenect2::Freenect2Device::IrCameraParams ip{};
        ip.fx = intr.fx;
        ip.fy = intr.fy;
        ip.cx = intr.cx;
        ip.cy = intr.cy;
        ip.k1 = intr.k1;
        ip.k2 = intr.k2;
        ip.k3 = intr.k3;
        ip.p1 = intr.p1;
        ip.p2 = intr.p2;
        return ip;
    }

    bool ensureRegistrationComputedLocked(bool want_color_reg) {
        if (!registration || latest_depth.width != 512 || latest_depth.height != 424 || latest_depth.pixels.size() < 512u * 424u) {
            return false;
        }
        const bool can_color = want_color_reg &&
                               latest_color.width == 1920 &&
                               latest_color.height == 1080 &&
                               latest_color.pixels.size() >= 1920u * 1080u * 4u;

        if (depth_frames > 0 && reg_depth_seq == depth_frames && (!can_color || (reg_used_color && reg_color_seq == color_frames))) {
            return true;
        }

        const size_t n = 512u * 424u;
        sanitized_depth_scratch.resize(n);
        const float* ir_px = (latest_ir.pixels.size() >= n) ? latest_ir.pixels.data() : nullptr;
        for (size_t i = 0; i < n; ++i) {
            const float d = latest_depth.pixels[i];
            const bool ir_sat = (ir_px != nullptr) && (!std::isfinite(ir_px[i]) || ir_px[i] >= 65535.0f);
            sanitized_depth_scratch[i] = (std::isfinite(d) && d > 0.0f && !ir_sat) ? d : 0.0f;
        }

        libfreenect2::Frame depth_wrap(512, 424, 4, reinterpret_cast<unsigned char*>(sanitized_depth_scratch.data()));
        if (can_color) {
            unsigned char* rgb_ptr = nullptr;
            if (!latest_color.is_rgbx) {
                rgb_ptr = const_cast<unsigned char*>(latest_color.pixels.data());
            } else {
                const size_t cn = 1920u * 1080u;
                bgrx_color_scratch.resize(cn * 4u);
                const uint8_t* src = latest_color.pixels.data();
                for (size_t i = 0; i < cn; ++i) {
                    bgrx_color_scratch[i * 4 + 0] = src[i * 4 + 2];
                    bgrx_color_scratch[i * 4 + 1] = src[i * 4 + 1];
                    bgrx_color_scratch[i * 4 + 2] = src[i * 4 + 0];
                    bgrx_color_scratch[i * 4 + 3] = 255;
                }
                rgb_ptr = bgrx_color_scratch.data();
            }
            libfreenect2::Frame rgb_wrap(1920, 1080, 4, rgb_ptr);
            registration->apply(
                &rgb_wrap,
                &depth_wrap,
                &reg_undistorted,
                &reg_registered,
                true,
                &reg_bigdepth,
                reg_color_depth_map.data()
            );

            // Populate fast 480x270 inverse lookup from colour cells -> nearest undistorted depth pixel
            std::fill(color_grid_480x270.begin(), color_grid_480x270.end(), -1);
            const float* und = reinterpret_cast<const float*>(reg_undistorted.data);
            for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
                const int c_off = reg_color_depth_map[i];
                const float z = und[i];
                if (c_off < 0 || c_off >= 1920 * 1080 || !(z > 0.0f) || !std::isfinite(z)) continue;
                const uint32_t gx = static_cast<uint32_t>(c_off % 1920) >> 2;
                const uint32_t gy = static_cast<uint32_t>(c_off / 1920) >> 2;
                const size_t gi = static_cast<size_t>(gy) * 480u + gx;
                const int32_t prev = color_grid_480x270[gi];
                if (prev < 0 || z < und[prev]) {
                    color_grid_480x270[gi] = static_cast<int32_t>(i);
                }
            }

            reg_depth_seq = depth_frames;
            reg_color_seq = color_frames;
            reg_used_color = true;
        } else {
            registration->undistortDepth(&depth_wrap, &reg_undistorted);
            reg_depth_seq = depth_frames;
            reg_used_color = false;
        }
        return true;
    }

    void recordPrimaryTimingLocked(uint64_t arrival_ns, double map_ms) {
        if (last_primary_arrival_ns > 0 && arrival_ns > last_primary_arrival_ns) {
            double dt_ms = static_cast<double>(arrival_ns - last_primary_arrival_ns) / 1e6;
            inter_frame_ms_window.push_back(dt_ms);
            if (inter_frame_ms_window.size() > 180) {
                inter_frame_ms_window.pop_front();
            }
            if (dt_ms > inter_frame_max_ms) {
                inter_frame_max_ms = dt_ms;
            }
        }
        last_primary_arrival_ns = arrival_ns;

        primary_arrival_window.push_back(arrival_ns);
        while (primary_arrival_window.size() > 2 &&
               (arrival_ns - primary_arrival_window.front()) > 2000000000ULL) {
            primary_arrival_window.pop_front();
        }

        map_time_ms_window.push_back(map_ms);
        if (map_time_ms_window.size() > 180) {
            map_time_ms_window.pop_front();
        }
        if (map_ms > map_time_max_ms) {
            map_time_max_ms = map_ms;
        }
    }

    void renderCurrentModeLocked(uint64_t trigger_arrival_ns, uint32_t trigger_ticks) {
        if (display_params.mode == KV_MODE_POINT_CLOUD_3D) {
            if (latest_depth.pixels.empty()) return;
            const uint64_t t0 = kv::monotonicNs();
            ensureRegistrationComputedLocked(display_params.point_color_source == KV_POINT_COLOR_REGISTERED_RGB);
            const uint64_t t1 = kv::monotonicNs();
            point_cloud_seq = std::max(point_cloud_seq, depth_frames + color_frames) + 1;
            point_cloud_arrival_ns = trigger_arrival_ns;
            ++rendered_frames;
            mapped_arrival_ns = trigger_arrival_ns;
            mapped_done_ns = t1;
            mapped_device_ticks = trigger_ticks;
            recordPrimaryTimingLocked(trigger_arrival_ns, static_cast<double>(t1 - t0) / 1e6);
            return;
        }

        const uint64_t t0 = kv::monotonicNs();
        uint32_t w = 0, h = 0;
        kv::PixelMetrics metrics{};

        if (display_params.depth_color_blend_enabled) {
            if (latest_depth.pixels.empty() || latest_color.pixels.empty()) return;
            if (!ensureRegistrationComputedLocked(true)) return;
            undistorted_depth_frame.width = 512;
            undistorted_depth_frame.height = 424;
            undistorted_depth_frame.pixels.resize(512u * 424u);
            std::memcpy(undistorted_depth_frame.pixels.data(), reg_undistorted.data, 512u * 424u * sizeof(float));
            mapper.mapRegisteredDepthColorBlend(
                undistorted_depth_frame,
                reg_registered.data,
                display_params,
                mapped_bgra,
                w,
                h,
                metrics
            );
        } else {
            switch (display_params.mode) {
                case KV_MODE_IR:
                    if (latest_ir.pixels.empty()) return;
                    mapper.mapIrToBgra(
                        latest_ir,
                        display_params,
                        mapped_bgra,
                        w,
                        h,
                        metrics,
                        latest_depth.pixels.empty() ? nullptr : &latest_depth
                    );
                    break;
                case KV_MODE_DEPTH:
                    if (latest_depth.pixels.empty()) return;
                    mapper.mapDepthToBgra(
                        latest_depth,
                        latest_ir.pixels.empty() ? nullptr : &latest_ir,
                        display_params,
                        mapped_bgra,
                        w,
                        h,
                        metrics
                    );
                    break;
                case KV_MODE_COLOR: {
                    if (latest_color.pixels.empty()) return;
                    const float* bd = nullptr;
                    if (display_params.cutoff_enabled && ensureRegistrationComputedLocked(true)) {
                        bd = reinterpret_cast<const float*>(reg_bigdepth.data);
                    }
                    mapper.mapColorToBgra(latest_color, mapped_bgra, w, h, metrics, &display_params, bd);
                    break;
                }
                case KV_MODE_SIDE_BY_SIDE: {
                    if (latest_ir.pixels.empty() && latest_depth.pixels.empty() && latest_color.pixels.empty()) return;
                    const float* bd = nullptr;
                    if (display_params.cutoff_enabled &&
                        display_params.sbs_layout != KV_SBS_IR_DEPTH &&
                        ensureRegistrationComputedLocked(true)) {
                        bd = reinterpret_cast<const float*>(reg_bigdepth.data);
                    }
                    mapper.mapSideBySide(
                        latest_ir.pixels.empty() ? nullptr : &latest_ir,
                        latest_depth.pixels.empty() ? nullptr : &latest_depth,
                        latest_color.pixels.empty() ? nullptr : &latest_color,
                        display_params,
                        mapped_bgra,
                        w,
                        h,
                        metrics,
                        bd
                    );
                    break;
                }
                case KV_MODE_POINT_CLOUD_3D:
                    return;
            }
        }

        const bool flip_x = (display_params.mirror_horizontal != display_params.rotate_180);
        const bool flip_y = display_params.rotate_180;
        if ((flip_x || flip_y) && w > 0 && h > 0 && mapped_bgra.size() >= static_cast<size_t>(w) * h * 4) {
            uint32_t* px = reinterpret_cast<uint32_t*>(mapped_bgra.data());
            const size_t total = static_cast<size_t>(w) * h;
            if (flip_x && flip_y) {
                for (size_t i = 0; i < total / 2; ++i) {
                    std::swap(px[i], px[total - 1 - i]);
                }
            } else if (flip_x) {
                for (uint32_t y = 0; y < h; ++y) {
                    uint32_t* row = px + static_cast<size_t>(y) * w;
                    for (uint32_t x = 0; x < w / 2; ++x) {
                        std::swap(row[x], row[w - 1 - x]);
                    }
                }
            } else if (flip_y) {
                for (uint32_t y = 0; y < h / 2; ++y) {
                    uint32_t* top_row = px + static_cast<size_t>(y) * w;
                    uint32_t* bot_row = px + static_cast<size_t>(h - 1 - y) * w;
                    for (uint32_t x = 0; x < w; ++x) {
                        std::swap(top_row[x], bot_row[x]);
                    }
                }
            }
        }

        const uint64_t next_seq = mapped_seq + 1;
        if (display_params.clock_overlay && w > 0 && h > 0) {
            kv::FrameMapper::drawClockOverlay(mapped_bgra.data(), w, h, next_seq, trigger_arrival_ns);
        }

        const uint64_t t1 = kv::monotonicNs();
        mapped_w = w;
        mapped_h = h;
        mapped_seq = next_seq;
        ++rendered_frames;
        mapped_arrival_ns = trigger_arrival_ns;
        mapped_done_ns = t1;
        mapped_device_ticks = trigger_ticks;
        latest_metrics = metrics;

        double map_ms = static_cast<double>(t1 - t0) / 1e6;
        recordPrimaryTimingLocked(trigger_arrival_ns, map_ms);
    }

    static void pushStreamSample(std::deque<StreamSample>& q, uint64_t now_ns, uint64_t received, uint64_t dropped) {
        q.push_back(StreamSample{now_ns, received, dropped});
        constexpr uint64_t kWindow60sNs = 60000000000ULL;
        while (!q.empty() && now_ns > q.front().arrival_ns && (now_ns - q.front().arrival_ns) > kWindow60sNs) {
            q.pop_front();
        }
    }

    static void pushArrivalWindow(std::deque<uint64_t>& q, uint64_t arrival_ns) {
        q.push_back(arrival_ns);
        while (q.size() > 2 && arrival_ns > q.front() && (arrival_ns - q.front()) > 2500000000ULL) {
            q.pop_front();
        }
    }

    void onIrFrame(const kv::RawFloatFrame& f) {
        std::lock_guard<std::mutex> lk(mutex);
        if (!running) return;

        uint64_t dropped = 0;
        if (has_last_ir_seq && f.sequence > last_ir_seq + 1) {
            dropped = static_cast<uint64_t>(f.sequence - last_ir_seq - 1);
            ir_seq_drops += dropped;
        }
        has_last_ir_seq = true;
        last_ir_seq = f.sequence;
        last_ir_seq_for_proc = f.sequence;
        last_ir_arrival_for_proc_ns = f.arrival_ns;
        ++ir_frames;
        latest_ir = f;

        pushArrivalWindow(ir_arrival_window, f.arrival_ns);
        const bool past_warmup = (start_ns > 0 && f.arrival_ns > start_ns && (f.arrival_ns - start_ns) > 500000000ULL);
        pushStreamSample(ir_history_60s, f.arrival_ns, 1, past_warmup ? dropped : 0);

        // Update 16-bit lossless IR buffer
        const size_t n = f.pixels.size();
        latest_ir_u16.resize(n);
        for (size_t i = 0; i < n; ++i) {
            float v = f.pixels[i];
            if (!std::isfinite(v) || v <= 0.0f) {
                latest_ir_u16[i] = 0;
            } else if (v >= 65535.0f) {
                latest_ir_u16[i] = 65535;
            } else {
                latest_ir_u16[i] = static_cast<uint16_t>(v + 0.5f);
            }
        }
        ++latest_ir_u16_seq;
        latest_ir_u16_arrival_ns = f.arrival_ns;
        latest_ir_u16_ticks = f.device_timestamp_ticks;

        if (source_kind == KV_SOURCE_REPLAY ||
            display_params.mode == KV_MODE_IR ||
            (display_params.mode == KV_MODE_SIDE_BY_SIDE && display_params.sbs_layout == KV_SBS_COLOR_IR)) {
            renderCurrentModeLocked(f.arrival_ns, f.device_timestamp_ticks);
        }
    }

    void onDepthFrame(const kv::RawFloatFrame& f) {
        std::lock_guard<std::mutex> lk(mutex);
        if (!running) return;

        const uint64_t proc_t0 = kv::monotonicNs();
        uint64_t dropped = 0;
        if (has_last_depth_seq && f.sequence > last_depth_seq + 1) {
            dropped = static_cast<uint64_t>(f.sequence - last_depth_seq - 1);
            depth_seq_drops += dropped;
        }
        has_last_depth_seq = true;
        last_depth_seq = f.sequence;
        ++depth_frames;
        latest_depth = f;

        pushArrivalWindow(depth_arrival_window, f.arrival_ns);
        const bool past_warmup = (start_ns > 0 && f.arrival_ns > start_ns && (f.arrival_ns - start_ns) > 500000000ULL);
        pushStreamSample(depth_history_60s, f.arrival_ns, 1, past_warmup ? dropped : 0);

        // Update 16-bit lossless Depth buffer (1 mm integer units; 0 = invalid/NaN/<=0)
        const size_t n = f.pixels.size();
        latest_depth_u16.resize(n);
        for (size_t i = 0; i < n; ++i) {
            float d = f.pixels[i];
            if (!std::isfinite(d) || d <= 0.0f) {
                latest_depth_u16[i] = 0;
            } else if (d >= 65535.0f) {
                latest_depth_u16[i] = 65535;
            } else {
                uint32_t mm = static_cast<uint32_t>(std::lround(d));
                latest_depth_u16[i] = static_cast<uint16_t>(std::max(1u, std::min(65535u, mm)));
            }
        }
        ++latest_depth_u16_seq;
        latest_depth_u16_arrival_ns = f.arrival_ns;
        latest_depth_u16_ticks = f.device_timestamp_ticks;

        if (source_kind == KV_SOURCE_REPLAY ||
            display_params.depth_color_blend_enabled ||
            display_params.mode == KV_MODE_DEPTH ||
            display_params.mode == KV_MODE_POINT_CLOUD_3D ||
            (display_params.mode == KV_MODE_IR && display_params.cutoff_enabled) ||
            (display_params.mode == KV_MODE_SIDE_BY_SIDE &&
             (display_params.sbs_layout == KV_SBS_IR_DEPTH || display_params.sbs_layout == KV_SBS_COLOR_DEPTH))) {
            renderCurrentModeLocked(f.arrival_ns, f.device_timestamp_ticks);
        }

        const uint64_t proc_t1 = kv::monotonicNs();
        double stage_ms = static_cast<double>(proc_t1 - proc_t0) / 1e6;
        if (last_ir_seq_for_proc == f.sequence && f.arrival_ns >= last_ir_arrival_for_proc_ns) {
            double pipe_delta_ms = static_cast<double>(f.arrival_ns - last_ir_arrival_for_proc_ns) / 1e6;
            if (pipe_delta_ms < 50.0) {
                stage_ms += pipe_delta_ms;
            }
        }
        depth_proc_ms_window.push_back(stage_ms);
        if (depth_proc_ms_window.size() > 180) {
            depth_proc_ms_window.pop_front();
        }
    }

    void onColorFrame(const kv::RawColorFrame& f) {
        std::lock_guard<std::mutex> lk(mutex);
        if (!running) return;

        uint64_t dropped = 0;
        if (has_last_color_seq && f.sequence > last_color_seq + 1) {
            dropped = static_cast<uint64_t>(f.sequence - last_color_seq - 1);
            color_seq_drops += dropped;
        }
        has_last_color_seq = true;
        last_color_seq = f.sequence;
        ++color_frames;
        latest_color = f;

        pushArrivalWindow(color_arrival_window, f.arrival_ns);
        const bool past_warmup = (start_ns > 0 && f.arrival_ns > start_ns && (f.arrival_ns - start_ns) > 500000000ULL);
        pushStreamSample(color_history_60s, f.arrival_ns, 1, past_warmup ? dropped : 0);

        if (display_params.mode == KV_MODE_COLOR ||
            display_params.depth_color_blend_enabled ||
            (display_params.mode == KV_MODE_POINT_CLOUD_3D && display_params.point_color_source == KV_POINT_COLOR_REGISTERED_RGB)) {
            renderCurrentModeLocked(f.arrival_ns, f.device_timestamp_ticks);
        }
    }
};

namespace {

void copyTruncated(char* dst, size_t cap, const std::string& src) {
    if (!dst || cap == 0) return;
    size_t n = std::min(cap - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // namespace

extern "C" {

uint64_t kv_monotonic_ns(void) {
    return kv::monotonicNs();
}

void kv_default_display_params(kv_display_params_t* out_params) {
    if (!out_params) return;
    std::memset(out_params, 0, sizeof(*out_params));
    out_params->mode = KV_MODE_IR;
    out_params->sbs_layout = KV_SBS_IR_DEPTH;
    out_params->ir_auto_scale = true;
    out_params->ir_low_percentile = 0.01f;
    out_params->ir_high_percentile = 0.995f;
    out_params->ir_ema_alpha = 0.12f;
    out_params->ir_gamma = 0.45f;
    out_params->ir_manual_min = 0.0f;
    out_params->ir_manual_max = 20000.0f;
    out_params->depth_colormap = KV_COLORMAP_TURBO;
    out_params->depth_min_mm = 500.0f;
    out_params->depth_max_mm = 4500.0f;
    out_params->depth_invert = true; // Near = warm red/yellow, Far = cool blue
    out_params->clock_overlay = false;
    out_params->cutoff_enabled = false;
    out_params->cutoff_near_mm = 500.0f;
    out_params->cutoff_far_mm = 2000.0f;
    out_params->depth_color_blend_enabled = false;
    out_params->depth_color_blend = 0.5f;
    out_params->point_color_source = KV_POINT_COLOR_DEPTH;
}

void kv_default_color_controls(kv_color_controls_t* out_controls) {
    if (!out_controls) return;
    std::memset(out_controls, 0, sizeof(*out_controls));
    out_controls->mode = KV_EXPOSURE_AUTO;
    out_controls->auto_exposure_compensation = 0.0f;
    out_controls->semi_auto_pseudo_ms = 16.0f;
    out_controls->manual_integration_ms = 16.6f;
    out_controls->manual_analog_gain = 1.5f;
}

bool kv_is_kinect_usb_present(void) {
    return kv::checkKinectUsbPresent();
}

bool kv_is_locked_by_other(const char* lock_path, char* out_owner, size_t out_owner_cap) {
    kv::KinectLockFile lf(lock_path ? std::string(lock_path) : std::string());
    std::string owner;
    bool locked = lf.isLockedByOther(&owner);
    copyTruncated(out_owner, out_owner_cap, owner);
    return locked;
}

kv_engine_t* kv_engine_create(const char* lock_file_path) {
    return new kv_engine(lock_file_path);
}

void kv_engine_destroy(kv_engine_t* engine) {
    if (!engine) return;
    kv_engine_stop(engine);
    delete engine;
}

bool kv_engine_start(
    kv_engine_t* engine,
    kv_source_kind_t source_kind,
    kv_pipeline_kind_t pipeline,
    const kv_display_params_t* initial_params,
    char* out_error,
    size_t out_error_cap
) {
    bool want_rgb = false;
    if (initial_params) {
        if (initial_params->depth_color_blend_enabled ||
            initial_params->mode == KV_MODE_COLOR ||
            (initial_params->mode == KV_MODE_POINT_CLOUD_3D && initial_params->point_color_source == KV_POINT_COLOR_REGISTERED_RGB) ||
            (initial_params->mode == KV_MODE_SIDE_BY_SIDE &&
             (initial_params->sbs_layout == KV_SBS_COLOR_DEPTH || initial_params->sbs_layout == KV_SBS_COLOR_IR))) {
            want_rgb = true;
        }
    }
    return kv_engine_start_ex(engine, source_kind, pipeline, initial_params, want_rgb, out_error, out_error_cap);
}

bool kv_engine_start_ex(
    kv_engine_t* engine,
    kv_source_kind_t source_kind,
    kv_pipeline_kind_t pipeline,
    const kv_display_params_t* initial_params,
    bool force_enable_rgb,
    char* out_error,
    size_t out_error_cap
) {
    if (!engine) return false;
    kv_engine_stop(engine);

    if (source_kind == KV_SOURCE_REAL && std::getenv("XCTestConfigurationFilePath") != nullptr) {
        const std::string err = "Real sensor source is forbidden when XCTestConfigurationFilePath is set (synthetic only).";
        {
            std::lock_guard<std::mutex> lk(engine->mutex);
            engine->running = false;
            engine->lock_blocked = false;
            engine->status_message = err;
        }
        copyTruncated(out_error, out_error_cap, err);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->resetStatsLocked();
        engine->source_kind = source_kind;
        engine->pipeline_kind = pipeline;
        if (initial_params) {
            engine->display_params = *initial_params;
        }
        engine->benign_notices = 0;
        engine->driver_errors = 0;
    }

    if (source_kind == KV_SOURCE_REAL || engine->has_custom_lock_path) {
        std::string lock_err;
        if (!engine->lock_file.acquire(&lock_err)) {
            std::lock_guard<std::mutex> lk(engine->mutex);
            engine->lock_blocked = true;
            engine->status_message = lock_err;
            copyTruncated(out_error, out_error_cap, lock_err);
            return false;
        }
    }
    engine->lock_blocked = false;
    engine->source = (source_kind == KV_SOURCE_REAL)
        ? kv::createFreenect2Source()
        : kv::createSyntheticSource();

    kv::SourceCallbacks cbs;
    cbs.on_ir = [engine](const kv::RawFloatFrame& f) { engine->onIrFrame(f); };
    cbs.on_depth = [engine](const kv::RawFloatFrame& f) { engine->onDepthFrame(f); };
    cbs.on_color = [engine](const kv::RawColorFrame& f) { engine->onColorFrame(f); };
    cbs.on_log = [engine](bool benign, const std::string& msg) {
        std::lock_guard<std::mutex> lk(engine->mutex);
        if (benign) {
            ++engine->benign_notices;
        } else {
            ++engine->driver_errors;
            engine->last_driver_error = msg;
            engine->status_message = "Driver: " + msg;
        }
    };

    const bool enable_rgb = force_enable_rgb || engine->modeUsesColor(engine->display_params);
    const bool enable_depth_ir = true;

    std::string start_err;
    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->running = true;
        engine->start_ns = kv::monotonicNs();
    }

    if (!engine->source->start(pipeline, enable_rgb, enable_depth_ir, cbs, &start_err)) {
        {
            std::lock_guard<std::mutex> lk(engine->mutex);
            engine->running = false;
            engine->status_message = start_err;
        }
        engine->source.reset();
        engine->lock_file.release();
        copyTruncated(out_error, out_error_cap, start_err);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->rgb_stream_active = enable_rgb;
        engine->serial = engine->source->serialNumber();
        engine->firmware = engine->source->firmwareVersion();
        engine->source->getIrIntrinsics(&engine->ir_intrinsics);
        libfreenect2::Freenect2Device::IrCameraParams ir_p{};
        libfreenect2::Freenect2Device::ColorCameraParams color_p{};
        if (engine->source->getRegistrationParams(&ir_p, &color_p)) {
            engine->registration = std::make_unique<libfreenect2::Registration>(ir_p, color_p);
        } else if (engine->ir_intrinsics.valid && engine->ir_intrinsics.fx > 0.0f && engine->ir_intrinsics.fy > 0.0f) {
            engine->registration = std::make_unique<libfreenect2::Registration>(
                kv_engine::toFreenectIrParams(engine->ir_intrinsics),
                kv_engine::defaultColorCameraParams()
            );
        } else {
            engine->registration.reset();
        }
        engine->usb_location = engine->source->usbLocationString();
        const char* pname = (pipeline == KV_PIPELINE_OPENGL) ? "OpenGL" :
                            (pipeline == KV_PIPELINE_OPENCL) ? "OpenCL" : "CPU";
        engine->status_message = (source_kind == KV_SOURCE_REAL)
            ? (std::string("Streaming Kinect v2 (") + pname + ")")
            : "Streaming Synthetic Pattern";
    }

    return true;
}

bool kv_engine_is_rgb_active(const kv_engine_t* engine) {
    if (!engine) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);
    return engine->rgb_stream_active;
}

bool kv_engine_start_replay(
    kv_engine_t* engine,
    const kv_display_params_t* initial_params,
    const kv_ir_intrinsics_t* ir_intrinsics
) {
    if (!engine) return false;
    kv_engine_stop(engine);

    std::lock_guard<std::mutex> lk(engine->mutex);
    engine->resetStatsLocked();
    engine->source_kind = KV_SOURCE_REPLAY;
    engine->pipeline_kind = KV_PIPELINE_CPU;
    engine->lock_blocked = false;
    engine->rgb_stream_active = false;
    if (initial_params) {
        engine->display_params = *initial_params;
    }
    if (ir_intrinsics && ir_intrinsics->valid && ir_intrinsics->fx > 0.0f && ir_intrinsics->fy > 0.0f) {
        engine->ir_intrinsics = *ir_intrinsics;
        engine->registration = std::make_unique<libfreenect2::Registration>(
            kv_engine::toFreenectIrParams(engine->ir_intrinsics),
            kv_engine::defaultColorCameraParams()
        );
    } else {
        engine->ir_intrinsics = kv_ir_intrinsics_t{};
        engine->registration.reset();
    }

    // Seed silent 512x424 IR/Depth and dark 1920x1080 Colour placeholders so switching to ANY view mode during
    // playback renders cleanly through FrameMapper (Colour is never recorded per Rule 2 & 3).
    engine->latest_ir.width = 512;
    engine->latest_ir.height = 424;
    engine->latest_ir.pixels.assign(512u * 424u, 0.0f);

    engine->latest_depth.width = 512;
    engine->latest_depth.height = 424;
    engine->latest_depth.pixels.assign(512u * 424u, 0.0f);

    engine->latest_color.width = 1920;
    engine->latest_color.height = 1080;
    engine->latest_color.is_rgbx = false;
    engine->latest_color.pixels.assign(1920u * 1080u * 4u, 0);
    for (size_t i = 0; i < 1920u * 1080u; ++i) {
        engine->latest_color.pixels[i * 4u + 3u] = 255;
    }

    engine->serial = "REPLAY-HOST";
    engine->firmware = "lossless-16bit";
    engine->usb_location = "Host Recording Playback";
    engine->status_message = "Replaying Host Recording";
    engine->start_ns = kv::monotonicNs();
    engine->running = true;
    return true;
}

bool kv_engine_push_replay_frame16(
    kv_engine_t* engine,
    bool is_depth,
    const uint16_t* pixels_u16,
    uint32_t width,
    uint32_t height,
    uint32_t frame_index,
    uint32_t device_ticks
) {
    if (!engine || !pixels_u16 || width == 0 || height == 0) return false;
    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        if (!engine->running || engine->source_kind != KV_SOURCE_REPLAY) {
            return false;
        }
    }

    const size_t count = static_cast<size_t>(width) * height;
    kv::RawFloatFrame frame;
    frame.width = width;
    frame.height = height;
    frame.sequence = frame_index;
    frame.device_timestamp_ticks = device_ticks;
    frame.arrival_ns = kv::monotonicNs();
    frame.local_counter = static_cast<uint64_t>(frame_index) + 1ULL;
    frame.pixels.resize(count);
    for (size_t i = 0; i < count; ++i) {
        frame.pixels[i] = static_cast<float>(pixels_u16[i]);
    }

    if (is_depth) {
        engine->onDepthFrame(frame);
    } else {
        engine->onIrFrame(frame);
    }
    return true;
}

void kv_engine_stop(kv_engine_t* engine) {
    if (!engine) return;
    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->running = false;
    }
    if (engine->source) {
        engine->source->stop();
        engine->source.reset();
    }
    engine->lock_file.release();
    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->status_message = "Stopped";
    }
}

bool kv_engine_is_running(const kv_engine_t* engine) {
    if (!engine) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);
    return engine->running;
}

void kv_engine_set_display_params(kv_engine_t* engine, const kv_display_params_t* params) {
    if (!engine || !params) return;
    std::lock_guard<std::mutex> lk(engine->mutex);
    bool mode_changed = (engine->display_params.mode != params->mode) ||
                        (engine->display_params.sbs_layout != params->sbs_layout);
    engine->display_params = *params;
    if (mode_changed) {
        engine->last_primary_arrival_ns = 0;
        engine->primary_arrival_window.clear();
        engine->inter_frame_ms_window.clear();
        engine->inter_frame_max_ms = 0.0;
    }
    if (!engine->latest_ir.pixels.empty() || !engine->latest_depth.pixels.empty() || !engine->latest_color.pixels.empty()) {
        uint64_t ts_ns = engine->mapped_arrival_ns > 0 ? engine->mapped_arrival_ns : kv::monotonicNs();
        engine->renderCurrentModeLocked(ts_ns, engine->mapped_device_ticks);
    }
}

void kv_engine_get_display_params(const kv_engine_t* engine, kv_display_params_t* out_params) {
    if (!engine || !out_params) return;
    std::lock_guard<std::mutex> lk(engine->mutex);
    *out_params = engine->display_params;
}

bool kv_engine_set_color_controls(kv_engine_t* engine, const kv_color_controls_t* controls, char* out_error, size_t out_error_cap) {
    if (!engine || !controls) return false;
    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->color_controls = *controls;
        if (!engine->running || !engine->source) {
            return true;
        }
    }
    std::string err;
    kv::ColorReadback rb{};
    bool ok = engine->source->applyColorControls(*controls, &rb, &err);
    if (ok) {
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->color_readback = rb;
    } else {
        copyTruncated(out_error, out_error_cap, err);
    }
    return ok;
}

bool kv_engine_copy_latest_frame(
    kv_engine_t* engine,
    uint64_t last_seen_seq,
    uint8_t* dst_bgra,
    size_t dst_cap,
    size_t* out_required_bytes,
    kv_mapped_frame_t* out_meta
) {
    if (!engine) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);
    if (engine->mapped_seq == 0 || engine->mapped_seq <= last_seen_seq || engine->mapped_bgra.empty()) {
        if (out_required_bytes) *out_required_bytes = 0;
        return false;
    }

    const size_t req = engine->mapped_bgra.size();
    if (out_required_bytes) *out_required_bytes = req;
    if (!dst_bgra || dst_cap < req) {
        return false;
    }

    std::memcpy(dst_bgra, engine->mapped_bgra.data(), req);
    if (out_meta) {
        out_meta->frame_seq = engine->mapped_seq;
        out_meta->arrival_mach_ns = engine->mapped_arrival_ns;
        out_meta->map_done_mach_ns = engine->mapped_done_ns;
        out_meta->device_timestamp_ticks = engine->mapped_device_ticks;
        out_meta->width = engine->mapped_w;
        out_meta->height = engine->mapped_h;
        out_meta->stride_bytes = engine->mapped_w * 4u;
        out_meta->mode = engine->display_params.mode;
        out_meta->is_synthetic = (engine->source_kind == KV_SOURCE_SYNTHETIC);
        out_meta->bgra_pixels = dst_bgra;
    }
    return true;
}

bool kv_engine_copy_latest_raw16(
    kv_engine_t* engine,
    bool want_depth,
    uint64_t last_seen_seq,
    uint16_t* dst_u16,
    size_t dst_u16_count,
    uint64_t* out_seq,
    uint64_t* out_arrival_ns,
    uint32_t* out_device_ticks,
    uint32_t* out_width,
    uint32_t* out_height
) {
    if (!engine) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);

    const auto& vec = want_depth ? engine->latest_depth_u16 : engine->latest_ir_u16;
    const uint64_t seq = want_depth ? engine->latest_depth_u16_seq : engine->latest_ir_u16_seq;
    const uint64_t arr = want_depth ? engine->latest_depth_u16_arrival_ns : engine->latest_ir_u16_arrival_ns;
    const uint32_t ticks = want_depth ? engine->latest_depth_u16_ticks : engine->latest_ir_u16_ticks;
    const uint32_t w = want_depth ? engine->latest_depth.width : engine->latest_ir.width;
    const uint32_t h = want_depth ? engine->latest_depth.height : engine->latest_ir.height;

    if (seq == 0 || seq <= last_seen_seq || vec.empty() || !dst_u16 || dst_u16_count < vec.size()) {
        return false;
    }

    std::memcpy(dst_u16, vec.data(), vec.size() * sizeof(uint16_t));
    if (out_seq) *out_seq = seq;
    if (out_arrival_ns) *out_arrival_ns = arr;
    if (out_device_ticks) *out_device_ticks = ticks;
    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    return true;
}

void kv_engine_get_stats(const kv_engine_t* engine, kv_stats_snapshot_t* out_stats) {
    if (!out_stats) return;
    std::memset(out_stats, 0, sizeof(*out_stats));
    if (!engine) return;

    const bool usb_now = kv::checkKinectUsbPresent();
    std::string other_owner;
    const bool locked_other = engine->lock_file.isLockedByOther(&other_owner);

    std::lock_guard<std::mutex> lk(engine->mutex);
    out_stats->running = engine->running;
    out_stats->device_present = usb_now;
    out_stats->lock_blocked = locked_other || engine->lock_blocked;
    out_stats->source_kind = engine->source_kind;
    out_stats->active_pipeline = engine->pipeline_kind;
    out_stats->active_mode = engine->display_params.mode;

    out_stats->ir_frames = engine->ir_frames;
    out_stats->depth_frames = engine->depth_frames;
    out_stats->color_frames = engine->color_frames;
    out_stats->rendered_frames = engine->rendered_frames;

    out_stats->ir_seq_drops = engine->ir_seq_drops;
    out_stats->depth_seq_drops = engine->depth_seq_drops;
    out_stats->color_seq_drops = engine->color_seq_drops;

    const uint64_t now_ns = kv::monotonicNs();
    if (engine->running && engine->start_ns > 0 && now_ns > engine->start_ns) {
        out_stats->uptime_sec = static_cast<double>(now_ns - engine->start_ns) / 1e9;
    }

    if (engine->primary_arrival_window.size() >= 2) {
        uint64_t span_ns = engine->primary_arrival_window.back() - engine->primary_arrival_window.front();
        if (span_ns > 0 && (now_ns - engine->primary_arrival_window.back()) < 1500000000ULL) {
            out_stats->current_fps = static_cast<double>(engine->primary_arrival_window.size() - 1) * 1e9 / static_cast<double>(span_ns);
        }
    }

    if (engine->color_arrival_window.size() >= 2) {
        uint64_t span_ns = engine->color_arrival_window.back() - engine->color_arrival_window.front();
        if (span_ns > 0 && (now_ns - engine->color_arrival_window.back()) < 1500000000ULL) {
            out_stats->color_fps = static_cast<double>(engine->color_arrival_window.size() - 1) * 1e9 / static_cast<double>(span_ns);
        }
    }

    if (!engine->inter_frame_ms_window.empty()) {
        double sum = 0.0;
        std::vector<double> sorted(engine->inter_frame_ms_window.begin(), engine->inter_frame_ms_window.end());
        for (double v : sorted) sum += v;
        std::sort(sorted.begin(), sorted.end());
        size_t p95_idx = std::min(sorted.size() - 1, static_cast<size_t>(std::floor(0.95 * static_cast<double>(sorted.size()))));
        out_stats->inter_frame_mean_ms = sum / static_cast<double>(sorted.size());
        out_stats->inter_frame_p95_ms = sorted[p95_idx];
        out_stats->inter_frame_max_ms = engine->inter_frame_max_ms;
    }

    if (!engine->map_time_ms_window.empty()) {
        double sum = 0.0;
        for (double v : engine->map_time_ms_window) sum += v;
        out_stats->map_time_mean_ms = sum / static_cast<double>(engine->map_time_ms_window.size());
        out_stats->map_time_max_ms = engine->map_time_max_ms;
    }

    if (engine->mapped_done_ns >= engine->mapped_arrival_ns && engine->mapped_arrival_ns > 0) {
        out_stats->arrival_to_map_done_ms = static_cast<double>(engine->mapped_done_ns - engine->mapped_arrival_ns) / 1e6;
    }

    out_stats->raw_min = engine->latest_metrics.raw_min;
    out_stats->raw_max = engine->latest_metrics.raw_max;
    out_stats->raw_mean = engine->latest_metrics.raw_mean;
    out_stats->saturated_pixels = engine->latest_metrics.saturated_pixels;
    out_stats->invalid_pixels = engine->latest_metrics.invalid_pixels;
    out_stats->smoothed_scale_low = engine->latest_metrics.smoothed_low;
    out_stats->smoothed_scale_high = engine->latest_metrics.smoothed_high;
    out_stats->mapped_luma_min = engine->latest_metrics.mapped_luma_min;
    out_stats->mapped_luma_max = engine->latest_metrics.mapped_luma_max;
    out_stats->mapped_luma_mean = engine->latest_metrics.mapped_luma_mean;
    for (size_t i = 0; i < 16; ++i) {
        out_stats->histogram16[i] = engine->latest_metrics.histogram16[i];
    }

    out_stats->color_exposure = engine->latest_color.exposure;
    out_stats->color_gain = engine->latest_color.gain;
    out_stats->color_gamma = engine->latest_color.gamma;
    out_stats->readback_exposure_mode = engine->color_readback.exposure_mode;
    out_stats->readback_integration_ms = engine->color_readback.integration_ms;
    out_stats->readback_analog_gain = engine->color_readback.analog_gain;
    if (engine->color_frames > 0 && engine->depth_frames > 0) {
        out_stats->color_minus_depth_ticks = static_cast<int32_t>(engine->latest_color.device_timestamp_ticks) -
                                             static_cast<int32_t>(engine->latest_depth.device_timestamp_ticks);
    }

    out_stats->benign_driver_notices = engine->benign_notices;
    out_stats->driver_errors = engine->driver_errors;

    copyTruncated(out_stats->serial, sizeof(out_stats->serial), engine->serial);
    copyTruncated(out_stats->firmware, sizeof(out_stats->firmware), engine->firmware);
    if (locked_other && !engine->running) {
        copyTruncated(out_stats->status_message, sizeof(out_stats->status_message),
                      "Kinect in use by another program (" + other_owner + "); waiting.");
    } else {
        copyTruncated(out_stats->status_message, sizeof(out_stats->status_message), engine->status_message);
    }
}

void kv_engine_get_health(const kv_engine_t* engine, kv_health_snapshot_t* out_health) {
    if (!out_health) return;
    std::memset(out_health, 0, sizeof(*out_health));
    if (!engine) return;

    const uint64_t now_ns = kv::monotonicNs();
    std::string live_usb_loc;
    {
        std::lock_guard<std::mutex> lk(engine->mutex);
        if (engine->running && (engine->source_kind == KV_SOURCE_SYNTHETIC || engine->source_kind == KV_SOURCE_REPLAY)) {
            live_usb_loc = engine->usb_location.empty() ? "Synthetic Virtual Bus (sim)" : engine->usb_location;
        } else if (engine->last_usb_loc_poll_ns == 0 ||
                   (now_ns > engine->last_usb_loc_poll_ns && (now_ns - engine->last_usb_loc_poll_ns) > 1000000000ULL)) {
            // Will refresh outside lock below
        } else {
            live_usb_loc = engine->cached_usb_loc;
        }
    }
    if (live_usb_loc.empty()) {
        live_usb_loc = kv::queryKinectUsbLocationString();
        std::lock_guard<std::mutex> lk(engine->mutex);
        engine->cached_usb_loc = live_usb_loc;
        engine->last_usb_loc_poll_ns = now_ns;
    }

    std::lock_guard<std::mutex> lk(engine->mutex);
    auto compute_fps = [&](const std::deque<uint64_t>& w) -> double {
        if (!engine->running || w.size() < 2) return 0.0;
        const uint64_t span = w.back() - w.front();
        if (span == 0 || (now_ns > w.back() && (now_ns - w.back()) >= 1500000000ULL)) return 0.0;
        return static_cast<double>(w.size() - 1) * 1e9 / static_cast<double>(span);
    };

    out_health->ir_fps = compute_fps(engine->ir_arrival_window);
    out_health->depth_fps = compute_fps(engine->depth_arrival_window);
    out_health->color_fps = compute_fps(engine->color_arrival_window);

    auto sum_window = [&](const std::deque<kv_engine::StreamSample>& q, uint64_t horizon_ns, uint64_t* out_recv) -> uint64_t {
        uint64_t dropped = 0;
        uint64_t recv = 0;
        for (const auto& s : q) {
            if (now_ns <= s.arrival_ns || (now_ns - s.arrival_ns) <= horizon_ns) {
                dropped += s.dropped;
                recv += s.received;
            }
        }
        if (out_recv) *out_recv = recv;
        return dropped;
    };

    constexpr uint64_t k60sNs = 60000000000ULL;
    constexpr uint64_t k10sNs = 10000000000ULL;

    out_health->ir_drops_per_min = static_cast<double>(sum_window(engine->ir_history_60s, k60sNs, nullptr));
    out_health->depth_drops_per_min = static_cast<double>(sum_window(engine->depth_history_60s, k60sNs, nullptr));
    out_health->color_drops_per_min = static_cast<double>(sum_window(engine->color_history_60s, k60sNs, nullptr));

    uint64_t ir_recv_10s = 0, depth_recv_10s = 0;
    const uint64_t ir_drop_10s = sum_window(engine->ir_history_60s, k10sNs, &ir_recv_10s);
    const uint64_t depth_drop_10s = sum_window(engine->depth_history_60s, k10sNs, &depth_recv_10s);

    const uint64_t ir_total_10s = ir_recv_10s + ir_drop_10s;
    const uint64_t depth_total_10s = depth_recv_10s + depth_drop_10s;
    out_health->ir_drop_ratio_10s = (ir_total_10s >= 15) ? (static_cast<double>(ir_drop_10s) / static_cast<double>(ir_total_10s)) : 0.0;
    out_health->depth_drop_ratio_10s = (depth_total_10s >= 15) ? (static_cast<double>(depth_drop_10s) / static_cast<double>(depth_total_10s)) : 0.0;
    out_health->drop_warning_active = engine->running &&
                                      (out_health->ir_drop_ratio_10s > 0.02 || out_health->depth_drop_ratio_10s > 0.02);

    if (!engine->inter_frame_ms_window.empty()) {
        std::vector<double> sorted(engine->inter_frame_ms_window.begin(), engine->inter_frame_ms_window.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        if ((n & 1u) == 1u) {
            out_health->inter_frame_p50_ms = sorted[n / 2];
        } else {
            out_health->inter_frame_p50_ms = 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
        }
        const size_t p95_idx = std::min(n - 1, static_cast<size_t>(std::floor(0.95 * static_cast<double>(n))));
        out_health->inter_frame_p95_ms = sorted[p95_idx];
    }

    if (!engine->depth_proc_ms_window.empty()) {
        double sum = 0.0;
        for (double v : engine->depth_proc_ms_window) sum += v;
        out_health->depth_processing_ms = sum / static_cast<double>(engine->depth_proc_ms_window.size());
    } else if (!engine->map_time_ms_window.empty()) {
        double sum = 0.0;
        for (double v : engine->map_time_ms_window) sum += v;
        out_health->depth_processing_ms = sum / static_cast<double>(engine->map_time_ms_window.size());
    }

    out_health->driver_errors = engine->driver_errors;
    copyTruncated(out_health->last_driver_error, sizeof(out_health->last_driver_error), engine->last_driver_error);
    copyTruncated(out_health->usb_location, sizeof(out_health->usb_location), live_usb_loc);
}

bool kv_engine_get_ir_intrinsics(const kv_engine_t* engine, kv_ir_intrinsics_t* out_intrinsics) {
    if (!engine || !out_intrinsics) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);
    *out_intrinsics = engine->ir_intrinsics;
    return engine->ir_intrinsics.valid;
}

bool kv_engine_copy_inspection_buffers(
    const kv_engine_t* engine,
    float* dst_depth_mm,
    size_t dst_depth_count,
    float* dst_ir_raw,
    size_t dst_ir_count,
    uint32_t* out_width,
    uint32_t* out_height,
    kv_ir_intrinsics_t* out_intrinsics
) {
    if (!engine) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);

    const uint32_t w = !engine->latest_depth.pixels.empty() ? engine->latest_depth.width : engine->latest_ir.width;
    const uint32_t h = !engine->latest_depth.pixels.empty() ? engine->latest_depth.height : engine->latest_ir.height;
    const size_t n = static_cast<size_t>(w) * h;
    if (w == 0 || h == 0 || n == 0) {
        return false;
    }

    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    if (out_intrinsics) *out_intrinsics = engine->ir_intrinsics;

    const bool has_ir = (engine->latest_ir.pixels.size() == n);
    const bool has_depth = (engine->latest_depth.pixels.size() == n);

    if (dst_ir_raw) {
        if (dst_ir_count < n) return false;
        if (has_ir) {
            for (size_t i = 0; i < n; ++i) {
                float v = engine->latest_ir.pixels[i];
                dst_ir_raw[i] = (std::isfinite(v) && v >= 0.0f) ? std::min(65535.0f, v) : 0.0f;
            }
        } else {
            std::fill(dst_ir_raw, dst_ir_raw + n, 0.0f);
        }
    }

    if (dst_depth_mm) {
        if (dst_depth_count < n) return false;
        if (has_depth) {
            for (size_t i = 0; i < n; ++i) {
                const bool ir_sat = has_ir && (!std::isfinite(engine->latest_ir.pixels[i]) || engine->latest_ir.pixels[i] >= 65535.0f);
                float d = engine->latest_depth.pixels[i];
                dst_depth_mm[i] = (!ir_sat && std::isfinite(d) && d > 0.0f) ? d : 0.0f;
            }
        } else {
            std::fill(dst_depth_mm, dst_depth_mm + n, 0.0f);
        }
    }

    return has_depth || has_ir;
}

bool kv_unproject_depth_pixel_mm(
    const kv_ir_intrinsics_t* intrinsics,
    int col_x,
    int row_y,
    float depth_mm,
    float* out_x_mm,
    float* out_y_mm,
    float* out_z_mm
) {
    if (!intrinsics || !intrinsics->valid || intrinsics->fx <= 0.0f || intrinsics->fy <= 0.0f) {
        return false;
    }
    if (!std::isfinite(depth_mm) || depth_mm <= 0.0f) {
        return false;
    }

    const float xd = (static_cast<float>(col_x) + 0.5f - intrinsics->cx) / intrinsics->fx;
    const float yd = (static_cast<float>(row_y) + 0.5f - intrinsics->cy) / intrinsics->fy;

    // Iterative Brown-Conrady radial (k1, k2, k3) + tangential (p1, p2) undistortion using libfreenect2's IrCameraParams
    float xu = xd;
    float yu = yd;
    for (int iter = 0; iter < 8; ++iter) {
        const float r2 = xu * xu + yu * yu;
        const float r4 = r2 * r2;
        const float r6 = r4 * r2;
        const float radial = 1.0f + intrinsics->k1 * r2 + intrinsics->k2 * r4 + intrinsics->k3 * r6;
        const float dx_tang = 2.0f * intrinsics->p1 * xu * yu + intrinsics->p2 * (r2 + 2.0f * xu * xu);
        const float dy_tang = intrinsics->p1 * (r2 + 2.0f * yu * yu) + 2.0f * intrinsics->p2 * xu * yu;
        if (std::fabs(radial) < 1e-6f) {
            break;
        }
        xu = (xd - dx_tang) / radial;
        yu = (yd - dy_tang) / radial;
    }

    if (out_x_mm) *out_x_mm = xu * depth_mm;
    if (out_y_mm) *out_y_mm = yu * depth_mm;
    if (out_z_mm) *out_z_mm = depth_mm;
    return true;
}

bool kv_measure_3d_distance_mm(
    const kv_ir_intrinsics_t* intrinsics,
    const float* depth_mm_buf,
    uint32_t width,
    uint32_t height,
    int x1,
    int y1,
    int x2,
    int y2,
    float* out_distance_mm
) {
    if (!intrinsics || !intrinsics->valid || !depth_mm_buf || width == 0 || height == 0) {
        return false;
    }
    if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 ||
        static_cast<uint32_t>(x1) >= width || static_cast<uint32_t>(y1) >= height ||
        static_cast<uint32_t>(x2) >= width || static_cast<uint32_t>(y2) >= height) {
        return false;
    }

    const float d1 = depth_mm_buf[static_cast<size_t>(y1) * width + static_cast<size_t>(x1)];
    const float d2 = depth_mm_buf[static_cast<size_t>(y2) * width + static_cast<size_t>(x2)];

    float x1_mm = 0.0f, y1_mm = 0.0f, z1_mm = 0.0f;
    float x2_mm = 0.0f, y2_mm = 0.0f, z2_mm = 0.0f;
    if (!kv_unproject_depth_pixel_mm(intrinsics, x1, y1, d1, &x1_mm, &y1_mm, &z1_mm) ||
        !kv_unproject_depth_pixel_mm(intrinsics, x2, y2, d2, &x2_mm, &y2_mm, &z2_mm)) {
        return false;
    }

    const float dx = x2_mm - x1_mm;
    const float dy = y2_mm - y1_mm;
    const float dz = z2_mm - z1_mm;
    if (out_distance_mm) {
        *out_distance_mm = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return true;
}

bool kv_engine_copy_point_cloud_inputs(
    kv_engine_t* engine,
    uint64_t last_seen_seq,
    float* dst_undistorted_depth_mm,
    size_t dst_depth_count,
    uint8_t* dst_point_bgra,
    size_t dst_bgra_bytes,
    uint64_t* out_seq,
    uint64_t* out_arrival_ns,
    kv_ir_intrinsics_t* out_intrinsics
) {
    if (!engine) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);

    constexpr size_t kCount = 512u * 424u;
    if (engine->latest_depth.pixels.size() < kCount || !engine->registration) {
        return false;
    }
    if (!dst_undistorted_depth_mm || dst_depth_count < kCount || !dst_point_bgra || dst_bgra_bytes < kCount * 4u) {
        return false;
    }

    const uint64_t cur_seq = std::max(engine->point_cloud_seq, engine->depth_frames + engine->color_frames);
    if (cur_seq == 0 || cur_seq <= last_seen_seq) {
        return false;
    }

    const bool want_reg_rgb = (engine->display_params.point_color_source == KV_POINT_COLOR_REGISTERED_RGB);
    if (!engine->ensureRegistrationComputedLocked(want_reg_rgb)) {
        return false;
    }

    const float* und = reinterpret_cast<const float*>(engine->reg_undistorted.data);
    const auto& p = engine->display_params;
    for (size_t i = 0; i < kCount; ++i) {
        float d = und[i];
        if (!std::isfinite(d) || d <= 0.0f) {
            d = 0.0f;
        } else if (p.cutoff_enabled && (d < p.cutoff_near_mm || d > p.cutoff_far_mm)) {
            d = 0.0f;
        }
        dst_undistorted_depth_mm[i] = d;
    }

    uint32_t mw = 0, mh = 0;
    kv::PixelMetrics metrics{};
    if (p.point_color_source == KV_POINT_COLOR_REGISTERED_RGB && engine->reg_used_color) {
        std::memcpy(dst_point_bgra, engine->reg_registered.data, kCount * 4u);
        for (size_t i = 0; i < kCount; ++i) {
            dst_point_bgra[i * 4 + 3] = 255;
        }
    } else if (p.point_color_source == KV_POINT_COLOR_IR && engine->latest_ir.pixels.size() >= kCount) {
        libfreenect2::Frame ir_wrap(
            512,
            424,
            4,
            reinterpret_cast<unsigned char*>(const_cast<float*>(engine->latest_ir.pixels.data()))
        );
        engine->registration->undistortDepth(&ir_wrap, &engine->reg_undistorted_ir);
        engine->undistorted_ir_frame.width = 512;
        engine->undistorted_ir_frame.height = 424;
        engine->undistorted_ir_frame.pixels.resize(kCount);
        std::memcpy(engine->undistorted_ir_frame.pixels.data(), engine->reg_undistorted_ir.data, kCount * sizeof(float));

        kv_display_params_t ir_p = p;
        ir_p.cutoff_enabled = false;
        std::vector<uint8_t> tmp_bgra;
        engine->mapper.mapIrToBgra(engine->undistorted_ir_frame, ir_p, tmp_bgra, mw, mh, metrics, nullptr);
        if (tmp_bgra.size() >= kCount * 4u) {
            std::memcpy(dst_point_bgra, tmp_bgra.data(), kCount * 4u);
        }
    } else {
        engine->undistorted_depth_frame.width = 512;
        engine->undistorted_depth_frame.height = 424;
        engine->undistorted_depth_frame.pixels.assign(dst_undistorted_depth_mm, dst_undistorted_depth_mm + kCount);
        std::vector<uint8_t> tmp_bgra;
        engine->mapper.mapDepthToBgra(engine->undistorted_depth_frame, nullptr, p, tmp_bgra, mw, mh, metrics);
        if (tmp_bgra.size() >= kCount * 4u) {
            std::memcpy(dst_point_bgra, tmp_bgra.data(), kCount * 4u);
        }
    }

    if (out_seq) *out_seq = cur_seq;
    if (out_arrival_ns) *out_arrival_ns = engine->point_cloud_arrival_ns > 0 ? engine->point_cloud_arrival_ns : engine->mapped_arrival_ns;
    if (out_intrinsics) *out_intrinsics = engine->ir_intrinsics;
    return true;
}

bool kv_engine_copy_latest_color_bgra(
    kv_engine_t* engine,
    uint64_t last_seen_color_seq,
    uint8_t* dst_bgra,
    size_t dst_cap,
    uint64_t* out_color_seq,
    uint32_t* out_width,
    uint32_t* out_height
) {
    if (!engine || !dst_bgra) return false;
    std::lock_guard<std::mutex> lk(engine->mutex);
    if (!engine->running || engine->source_kind == KV_SOURCE_REPLAY || !engine->rgb_stream_active) {
        return false;
    }
    if (engine->color_frames == 0 || engine->color_frames <= last_seen_color_seq) {
        return false;
    }
    const uint32_t w = engine->latest_color.width;
    const uint32_t h = engine->latest_color.height;
    const size_t pixel_count = static_cast<size_t>(w) * h;
    const size_t req_bytes = pixel_count * 4u;
    if (w == 0 || h == 0 || engine->latest_color.pixels.size() < req_bytes || dst_cap < req_bytes) {
        return false;
    }

    const uint8_t* src = engine->latest_color.pixels.data();
    if (engine->latest_color.is_rgbx) {
        for (size_t i = 0; i < pixel_count; ++i) {
            dst_bgra[i * 4u + 0u] = src[i * 4u + 2u];
            dst_bgra[i * 4u + 1u] = src[i * 4u + 1u];
            dst_bgra[i * 4u + 2u] = src[i * 4u + 0u];
            dst_bgra[i * 4u + 3u] = 255;
        }
    } else {
        std::memcpy(dst_bgra, src, req_bytes);
        for (size_t i = 0; i < pixel_count; ++i) {
            dst_bgra[i * 4u + 3u] = 255;
        }
    }

    if (out_color_seq) *out_color_seq = engine->color_frames;
    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    return true;
}

bool kv_engine_map_color_norm_to_depth_and_3d(
    kv_engine_t* engine,
    float norm_cx,
    float norm_cy,
    float* out_depth_x,
    float* out_depth_y,
    float* out_x_m,
    float* out_y_m,
    float* out_z_m
) {
    if (!engine) return false;
    if (!std::isfinite(norm_cx) || !std::isfinite(norm_cy)) return false;

    std::lock_guard<std::mutex> lk(engine->mutex);
    if (!engine->ensureRegistrationComputedLocked(true) || !engine->reg_used_color) {
        return false;
    }

    const int cx = std::clamp(static_cast<int>(std::lround(norm_cx * 1919.0f)), 0, 1919);
    const int cy = std::clamp(static_cast<int>(std::lround(norm_cy * 1079.0f)), 0, 1079);
    const int gx0 = cx >> 2;
    const int gy0 = cy >> 2;

    int32_t best_idx = -1;
    int best_d2 = 64 * 64;
    const float* und = reinterpret_cast<const float*>(engine->reg_undistorted.data);
    const auto& p = engine->display_params;

    for (int dy = -4; dy <= 4; ++dy) {
        const int gy = gy0 + dy;
        if (gy < 0 || gy >= 270) continue;
        for (int dx = -4; dx <= 4; ++dx) {
            const int gx = gx0 + dx;
            if (gx < 0 || gx >= 480) continue;
            const int32_t cand = engine->color_grid_480x270[static_cast<size_t>(gy) * 480u + static_cast<size_t>(gx)];
            if (cand < 0) continue;
            const float z = und[cand];
            if (!std::isfinite(z) || z <= 0.0f) continue;
            if (p.cutoff_enabled && (z < p.cutoff_near_mm || z > p.cutoff_far_mm)) continue;

            const int c_off = engine->reg_color_depth_map[cand];
            if (c_off < 0) continue;
            const int mcx = c_off % 1920;
            const int mcy = c_off / 1920;
            const int d2 = (mcx - cx) * (mcx - cx) + (mcy - cy) * (mcy - cy);
            if (d2 < best_d2) {
                best_d2 = d2;
                best_idx = cand;
            }
        }
    }

    if (best_idx < 0) return false;

    const int ux = best_idx % 512;
    const int uy = best_idx / 512;
    float x_m = 0.0f, y_m = 0.0f, z_m = 0.0f;
    engine->registration->getPointXYZ(&engine->reg_undistorted, uy, ux, x_m, y_m, z_m);
    if (!std::isfinite(z_m) || z_m <= 0.0f) return false;

    float dx_px = static_cast<float>(ux);
    float dy_px = static_cast<float>(uy);
    if (!p.depth_color_blend_enabled) {
        // Compute distorted 512x424 sensor coordinate so 2D overlays on raw Depth/IR land on the exact pixel
        const auto& intr = engine->ir_intrinsics;
        if (intr.valid && intr.fx > 0.0f && intr.fy > 0.0f) {
            const float nx = (static_cast<float>(ux) - intr.cx) / intr.fx;
            const float ny = (static_cast<float>(uy) - intr.cy) / intr.fy;
            const float r2 = nx * nx + ny * ny;
            const float xy2 = 2.0f * nx * ny;
            const float kr = 1.0f + ((intr.k3 * r2 + intr.k2) * r2 + intr.k1) * r2;
            dx_px = intr.fx * (nx * kr + intr.p2 * (r2 + 2.0f * nx * nx) + intr.p1 * xy2) + intr.cx;
            dy_px = intr.fy * (ny * kr + intr.p1 * (r2 + 2.0f * ny * ny) + intr.p2 * xy2) + intr.cy;
        }
    }

    if (out_depth_x) *out_depth_x = std::clamp(dx_px, 0.0f, 511.0f);
    if (out_depth_y) *out_depth_y = std::clamp(dy_px, 0.0f, 423.0f);
    if (out_x_m) *out_x_m = x_m;
    if (out_y_m) *out_y_m = y_m;
    if (out_z_m) *out_z_m = z_m;
    return true;
}

bool kv_decode_clock_barcode(
    const uint8_t* row_base,
    uint32_t width,
    uint32_t height,
    uint32_t row_stride_bytes,
    uint32_t bytes_per_pixel,
    uint32_t* out_seq24,
    uint32_t* out_timestamp_low32_us
) {
    return kv::FrameMapper::decodeClockBarcode(
        row_base, width, height, row_stride_bytes, bytes_per_pixel, out_seq24, out_timestamp_low32_us
    );
}

} // extern "C"
