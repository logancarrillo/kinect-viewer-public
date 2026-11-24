#pragma once

#include "frame_types.hpp"
#include "kinect_core_c.h"

#include <array>
#include <cstdint>
#include <vector>

namespace kv {

struct PixelMetrics {
    float raw_min = 0.0f;
    float raw_max = 0.0f;
    float raw_mean = 0.0f;
    uint32_t saturated_pixels = 0;
    uint32_t invalid_pixels = 0;
    float smoothed_low = 0.0f;
    float smoothed_high = 65535.0f;
    uint8_t mapped_luma_min = 0;
    uint8_t mapped_luma_max = 0;
    float mapped_luma_mean = 0.0f;
    std::array<uint32_t, 16> histogram16{};
};

class FrameMapper {
public:
    FrameMapper();

    void resetSmoothing();

    void mapIrToBgra(
        const RawFloatFrame& ir,
        const kv_display_params_t& params,
        std::vector<uint8_t>& out_bgra,
        uint32_t& out_w,
        uint32_t& out_h,
        PixelMetrics& out_metrics,
        const RawFloatFrame* co_depth = nullptr
    );

    void mapDepthToBgra(
        const RawFloatFrame& depth,
        const RawFloatFrame* co_ir,
        const kv_display_params_t& params,
        std::vector<uint8_t>& out_bgra,
        uint32_t& out_w,
        uint32_t& out_h,
        PixelMetrics& out_metrics
    );

    void mapColorToBgra(
        const RawColorFrame& color,
        std::vector<uint8_t>& out_bgra,
        uint32_t& out_w,
        uint32_t& out_h,
        PixelMetrics& out_metrics,
        const kv_display_params_t* params = nullptr,
        const float* bigdepth_1920x1082 = nullptr
    );

    void mapSideBySide(
        const RawFloatFrame* ir,
        const RawFloatFrame* depth,
        const RawColorFrame* color,
        const kv_display_params_t& params,
        std::vector<uint8_t>& out_bgra,
        uint32_t& out_w,
        uint32_t& out_h,
        PixelMetrics& out_metrics,
        const float* bigdepth_1920x1082 = nullptr
    );

    void mapRegisteredDepthColorBlend(
        const RawFloatFrame& undistorted_depth,
        const uint8_t* registered_bgrx_512x424,
        const kv_display_params_t& params,
        std::vector<uint8_t>& out_bgra,
        uint32_t& out_w,
        uint32_t& out_h,
        PixelMetrics& out_metrics
    );

    static void drawClockOverlay(
        uint8_t* bgra,
        uint32_t width,
        uint32_t height,
        uint64_t frame_seq,
        uint64_t timestamp_ns
    );

    static bool decodeClockBarcode(
        const uint8_t* row_base,
        uint32_t width,
        uint32_t height,
        uint32_t row_stride_bytes,
        uint32_t bytes_per_pixel,
        uint32_t* out_seq24,
        uint32_t* out_timestamp_low32_us
    );

private:
    bool smoothing_initialized_ = false;
    float smoothed_ir_low_ = 0.0f;
    float smoothed_ir_high_ = 16000.0f;

    float cached_gamma_ = -1.0f;
    std::array<uint8_t, 1025> gamma_lut_{};

    std::vector<uint8_t> scratch_left_;
    std::vector<uint8_t> scratch_right_;

    void updateGammaLut(float gamma);
};

} // namespace kv
