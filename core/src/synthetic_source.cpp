#include "frame_source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace kv {

namespace {

class SyntheticFrameSource final : public IFrameSource {
public:
    ~SyntheticFrameSource() override {
        stop();
    }

    bool start(
        kv_pipeline_kind_t /*pipeline*/,
        bool enable_rgb,
        bool enable_depth_ir,
        const SourceCallbacks& callbacks,
        std::string* /*out_error*/
    ) override {
        stop();
        callbacks_ = callbacks;
        enable_rgb_ = enable_rgb;
        enable_depth_ir_ = enable_depth_ir;
        running_.store(true);
        worker_ = std::thread([this]() { runLoop(); });
        return true;
    }

    void stop() override {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool isRunning() const override {
        return running_.load();
    }

    bool applyColorControls(const kv_color_controls_t& controls, ColorReadback* out_readback, std::string* /*out_error*/) override {
        controls_ = controls;
        if (out_readback) {
            switch (controls.mode) {
                case KV_EXPOSURE_AUTO:
                    out_readback->exposure_mode = 0;
                    out_readback->integration_ms = 16.6f;
                    out_readback->analog_gain = 1.2f;
                    break;
                case KV_EXPOSURE_SEMI_AUTO:
                    out_readback->exposure_mode = 3;
                    out_readback->integration_ms = controls.semi_auto_pseudo_ms;
                    out_readback->analog_gain = 1.5f;
                    break;
                case KV_EXPOSURE_MANUAL:
                    out_readback->exposure_mode = 4;
                    out_readback->integration_ms = controls.manual_integration_ms;
                    out_readback->analog_gain = controls.manual_analog_gain;
                    break;
            }
        }
        return true;
    }

    std::string serialNumber() const override {
        return "SYNTHETIC-KV2-0001";
    }

    std::string firmwareVersion() const override {
        return "sim-1.0.0";
    }

    bool getIrIntrinsics(kv_ir_intrinsics_t* out_intrinsics) const override {
        if (!out_intrinsics) return false;
        out_intrinsics->valid = true;
        out_intrinsics->fx = 365.456f;
        out_intrinsics->fy = 365.456f;
        out_intrinsics->cx = 254.878f;
        out_intrinsics->cy = 205.395f;
        out_intrinsics->k1 = 0.0905474f;
        out_intrinsics->k2 = -0.26819f;
        out_intrinsics->k3 = 0.0950862f;
        out_intrinsics->p1 = 0.0f;
        out_intrinsics->p2 = 0.0f;
        return true;
    }

    bool getRegistrationParams(
        libfreenect2::Freenect2Device::IrCameraParams* out_ir,
        libfreenect2::Freenect2Device::ColorCameraParams* out_color
    ) const override {
        if (out_ir) {
            *out_ir = {};
            out_ir->fx = 365.456f;
            out_ir->fy = 365.456f;
            out_ir->cx = 254.878f;
            out_ir->cy = 205.395f;
            out_ir->k1 = 0.0905474f;
            out_ir->k2 = -0.26819f;
            out_ir->k3 = 0.0950862f;
            out_ir->p1 = 0.0f;
            out_ir->p2 = 0.0f;
        }
        if (out_color) {
            *out_color = {};
            out_color->fx = 1081.37f;
            out_color->fy = 1081.37f;
            out_color->cx = 959.5f;
            out_color->cy = 539.5f;
            out_color->shift_d = 863.0f;
            out_color->shift_m = 52.0f;
            // Matches libfreenect2 polynomial scaling (depth_q=0.01, color_q=0.002199)
            out_color->mx_x1y0 = 0.650675f;
            out_color->my_x0y1 = 0.650675f;
        }
        return true;
    }

    std::string usbLocationString() const override {
        return "Synthetic Virtual Bus (sim)";
    }

private:
    std::atomic<bool> running_{false};
    bool enable_rgb_ = true;
    bool enable_depth_ir_ = true;
    kv_color_controls_t controls_{KV_EXPOSURE_AUTO, 0.0f, 16.0f, 16.0f, 1.0f};
    SourceCallbacks callbacks_;
    std::thread worker_;

    void runLoop() {
        constexpr uint32_t kIrW = 512;
        constexpr uint32_t kIrH = 424;
        constexpr uint32_t kColorW = 1920;
        constexpr uint32_t kColorH = 1080;

        RawFloatFrame ir_frame;
        ir_frame.width = kIrW;
        ir_frame.height = kIrH;
        ir_frame.pixels.resize(static_cast<size_t>(kIrW) * kIrH);

        RawFloatFrame depth_frame;
        depth_frame.width = kIrW;
        depth_frame.height = kIrH;
        depth_frame.pixels.resize(static_cast<size_t>(kIrW) * kIrH);

        RawColorFrame color_frame;
        color_frame.width = kColorW;
        color_frame.height = kColorH;
        color_frame.is_rgbx = false;
        color_frame.pixels.resize(static_cast<size_t>(kColorW) * kColorH * 4);

        // Pre-fill base SMPTE-like gradient bars for 1080p synthetic colour so per-frame cost stays tiny
        for (uint32_t y = 0; y < kColorH; ++y) {
            uint8_t* row = color_frame.pixels.data() + static_cast<size_t>(y) * kColorW * 4;
            for (uint32_t x = 0; x < kColorW; ++x) {
                uint32_t band = (x * 8) / kColorW;
                uint8_t r = 0, g = 0, b = 0;
                switch (band) {
                    case 0: r = 210; g = 210; b = 210; break;
                    case 1: r = 210; g = 210; b = 20;  break;
                    case 2: r = 20;  g = 210; b = 210; break;
                    case 3: r = 20;  g = 210; b = 20;  break;
                    case 4: r = 210; g = 20;  b = 210; break;
                    case 5: r = 210; g = 20;  b = 20;  break;
                    case 6: r = 20;  g = 60;  b = 210; break;
                    default: {
                        uint8_t ramp = static_cast<uint8_t>((y * 255) / kColorH);
                        r = ramp; g = ramp; b = ramp;
                        break;
                    }
                }
                row[x * 4 + 0] = b;
                row[x * 4 + 1] = g;
                row[x * 4 + 2] = r;
                row[x * 4 + 3] = 255;
            }
        }

        uint32_t seq = 0;
        uint32_t ticks = 1000;
        auto next_deadline = std::chrono::steady_clock::now();
        const auto period = std::chrono::microseconds(33333); // 30.00 fps

        while (running_.load()) {
            next_deadline += period;
            const uint64_t arrival = monotonicNs();
            const float phase = static_cast<float>(seq) * 0.07f;
            const float cx = 256.0f + 120.0f * std::cos(phase);
            const float cy = 212.0f + 80.0f * std::sin(phase * 1.3f);

            if (enable_depth_ir_) {
                for (uint32_t y = 0; y < kIrH; ++y) {
                    for (uint32_t x = 0; x < kIrW; ++x) {
                        size_t idx = static_cast<size_t>(y) * kIrW + x;
                        float dx = static_cast<float>(x) - cx;
                        float dy = static_cast<float>(y) - cy;
                        float r2 = dx * dx + dy * dy;

                        // Simulated depth: planar sloped room (1400..3800 mm) + foreground sphere at ~950 mm
                        float d_mm = 1500.0f + static_cast<float>(y) * 5.2f + static_cast<float>(x) * 0.8f;
                        if (r2 < 65.0f * 65.0f) {
                            float z_cap = std::sqrt(65.0f * 65.0f - r2) * 4.5f;
                            d_mm = 1150.0f - z_cap;
                        }

                        // Inject realistic invalid border & NaN test patch to exercise gating
                        if (x < 6 || x >= kIrW - 6 || y < 4) {
                            d_mm = 0.0f;
                        } else if (x >= 490 && y >= 405) {
                            d_mm = std::numeric_limits<float>::quiet_NaN();
                        }
                        depth_frame.pixels[idx] = d_mm;

                        // Simulated IR: inverse-square irradiance + checkerboard albedo + saturated retroreflector core
                        float checker = (((x / 32) + (y / 32)) & 1) ? 1.25f : 0.78f;
                        float valid_d = (std::isfinite(d_mm) && d_mm > 300.0f) ? d_mm : 3500.0f;
                        float ir_val = (1.8e10f / (valid_d * valid_d)) * checker;
                        if (r2 < 10.0f * 10.0f) {
                            ir_val = 65535.0f; // Saturated glint
                        }
                        ir_frame.pixels[idx] = std::clamp(ir_val, 0.0f, 65535.0f);
                    }
                }

                ir_frame.sequence = seq;
                ir_frame.device_timestamp_ticks = ticks;
                ir_frame.arrival_ns = arrival;
                ir_frame.local_counter = seq + 1;

                depth_frame.sequence = seq;
                depth_frame.device_timestamp_ticks = ticks;
                depth_frame.arrival_ns = arrival;
                depth_frame.local_counter = seq + 1;

                if (callbacks_.on_ir) callbacks_.on_ir(ir_frame);
                if (callbacks_.on_depth) callbacks_.on_depth(depth_frame);
            }

            if (enable_rgb_) {
                // Draw moving white box onto a small strip of the 1080p frame
                uint32_t bar_x = (seq * 12u) % (kColorW - 80u);
                for (uint32_t y = 500; y < 580; ++y) {
                    uint8_t* row = color_frame.pixels.data() + static_cast<size_t>(y) * kColorW * 4;
                    for (uint32_t x = 0; x < kColorW; ++x) {
                        if (x >= bar_x && x < bar_x + 80u) {
                            row[x * 4 + 0] = 255;
                            row[x * 4 + 1] = 255;
                            row[x * 4 + 2] = 255;
                        } else {
                            uint32_t band = (x * 8) / kColorW;
                            row[x * 4 + 0] = (band & 1) ? 40 : 200;
                            row[x * 4 + 1] = (band & 2) ? 200 : 40;
                            row[x * 4 + 2] = (band & 4) ? 200 : 40;
                        }
                    }
                }
                color_frame.sequence = seq;
                color_frame.device_timestamp_ticks = ticks + 180u; // Steady ~180 ticks offset like real Kinect v2
                color_frame.arrival_ns = monotonicNs();
                color_frame.local_counter = seq + 1;
                color_frame.exposure = (controls_.mode == KV_EXPOSURE_MANUAL) ? controls_.manual_integration_ms : 16.6f;
                color_frame.gain = (controls_.mode == KV_EXPOSURE_MANUAL) ? controls_.manual_analog_gain : 1.2f;
                color_frame.gamma = 2.2f;
                if (callbacks_.on_color) callbacks_.on_color(color_frame);
            }

            ++seq;
            ticks += 267u; // 266-267 ticks of 0.125 ms (~33.375 ms)
            std::this_thread::sleep_until(next_deadline);
        }
    }
};

} // namespace

std::unique_ptr<IFrameSource> createSyntheticSource() {
    return std::make_unique<SyntheticFrameSource>();
}

} // namespace kv
