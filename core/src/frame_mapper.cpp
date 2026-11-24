#include "frame_mapper.hpp"
#include "colormaps.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace kv {

namespace {

// Compact 5x7 bitmap font for 0-9, '.', ':', '#', 'S', 'E', 'Q', 'M', 'S', ' '
const uint8_t* glyph5x7(char c) {
    static const uint8_t kDigits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}  // 9
    };
    static const uint8_t kDot[7]   = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
    static const uint8_t kColon[7] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00};
    static const uint8_t kHash[7]  = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A};
    static const uint8_t kS[7]     = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t kM[7]     = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t kT[7]     = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t kEq[7]    = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00};
    static const uint8_t kBlank[7] = {0,0,0,0,0,0,0};

    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    switch (c) {
        case '.': return kDot;
        case ':': return kColon;
        case '#': return kHash;
        case 's':
        case 'S': return kS;
        case 'm':
        case 'M': return kM;
        case 'T': return kT;
        case '=': return kEq;
        default: return kBlank;
    }
}

void drawText5x7(uint8_t* bgra, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0, uint32_t scale, const char* text) {
    uint32_t cx = x0;
    for (const char* p = text; *p; ++p) {
        const uint8_t* rows = glyph5x7(*p);
        for (uint32_t gy = 0; gy < 7; ++gy) {
            uint8_t row_bits = rows[gy];
            for (uint32_t gx = 0; gx < 5; ++gx) {
                bool on = ((row_bits >> (4 - gx)) & 1) != 0;
                if (!on) continue;
                for (uint32_t sy = 0; sy < scale; ++sy) {
                    uint32_t py = y0 + gy * scale + sy;
                    if (py >= h) continue;
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        uint32_t px = cx + gx * scale + sx;
                        if (px >= w) continue;
                        size_t off = (static_cast<size_t>(py) * w + px) * 4;
                        bgra[off + 0] = 60;
                        bgra[off + 1] = 255;
                        bgra[off + 2] = 120;
                        bgra[off + 3] = 255;
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

} // namespace

namespace {
inline bool depthPassesCutoff(float d_mm, const kv_display_params_t& params) {
    if (!params.cutoff_enabled) return true;
    return std::isfinite(d_mm) && d_mm > 0.0f && d_mm >= params.cutoff_near_mm && d_mm <= params.cutoff_far_mm;
}
} // namespace

FrameMapper::FrameMapper() {
    updateGammaLut(0.45f);
}

void FrameMapper::resetSmoothing() {
    smoothing_initialized_ = false;
}

void FrameMapper::updateGammaLut(float gamma) {
    gamma = std::clamp(gamma, 0.15f, 2.5f);
    if (std::fabs(gamma - cached_gamma_) < 1e-4f) {
        return;
    }
    cached_gamma_ = gamma;
    for (int i = 0; i <= 1024; ++i) {
        float norm = static_cast<float>(i) / 1024.0f;
        float curved = std::pow(norm, gamma);
        gamma_lut_[i] = static_cast<uint8_t>(std::clamp(std::lround(curved * 255.0f), 0L, 255L));
    }
}

void FrameMapper::mapIrToBgra(
    const RawFloatFrame& ir,
    const kv_display_params_t& params,
    std::vector<uint8_t>& out_bgra,
    uint32_t& out_w,
    uint32_t& out_h,
    PixelMetrics& out_metrics,
    const RawFloatFrame* co_depth
) {
    out_w = ir.width;
    out_h = ir.height;
    const size_t n = static_cast<size_t>(out_w) * out_h;
    out_bgra.resize(n * 4);
    out_metrics = PixelMetrics{};

    if (n == 0 || ir.pixels.size() < n) {
        std::fill(out_bgra.begin(), out_bgra.end(), 0);
        return;
    }

    updateGammaLut(params.ir_gamma > 0.0f ? params.ir_gamma : 0.45f);

    std::array<uint32_t, 1024> hist1024{};
    uint32_t non_sat_count = 0;
    float min_v = std::numeric_limits<float>::max();
    float max_v = 0.0f;
    double sum_v = 0.0;
    uint32_t sat_count = 0;

    for (size_t i = 0; i < n; ++i) {
        float v = ir.pixels[i];
        if (!std::isfinite(v) || v < 0.0f) {
            v = 0.0f;
        }
        if (v >= 65535.0f) {
            v = 65535.0f;
            ++sat_count;
        } else {
            uint32_t bin1024 = std::min<uint32_t>(1023u, static_cast<uint32_t>(v * (1024.0f / 65535.0f)));
            ++hist1024[bin1024];
            ++non_sat_count;
        }
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
        sum_v += v;

        uint32_t bin16 = std::min<uint32_t>(15u, static_cast<uint32_t>(v * (16.0f / 65536.0f)));
        ++out_metrics.histogram16[bin16];
    }

    out_metrics.raw_min = (min_v == std::numeric_limits<float>::max()) ? 0.0f : min_v;
    out_metrics.raw_max = max_v;
    out_metrics.raw_mean = static_cast<float>(sum_v / static_cast<double>(n));
    out_metrics.saturated_pixels = sat_count;

    float scale_low = params.ir_manual_min;
    float scale_high = params.ir_manual_max;

    if (params.ir_auto_scale) {
        float p_low = std::clamp(params.ir_low_percentile, 0.0f, 0.45f);
        float p_high = std::clamp(params.ir_high_percentile, 0.55f, 1.0f);
        uint32_t target_low = static_cast<uint32_t>(p_low * static_cast<float>(non_sat_count));
        uint32_t target_high = static_cast<uint32_t>(p_high * static_cast<float>(non_sat_count));

        uint32_t acc = 0;
        int low_bin = 0;
        int high_bin = 1023;
        bool found_low = false;
        for (int b = 0; b < 1024; ++b) {
            acc += hist1024[b];
            if (!found_low && acc >= target_low) {
                low_bin = b;
                found_low = true;
            }
            if (acc >= target_high) {
                high_bin = b;
                break;
            }
        }

        float frame_low = static_cast<float>(low_bin) * (65535.0f / 1024.0f);
        float frame_high = static_cast<float>(high_bin + 1) * (65535.0f / 1024.0f);
        if (frame_high - frame_low < 256.0f) {
            frame_high = std::min(65535.0f, frame_low + 256.0f);
        }

        float alpha = std::clamp(params.ir_ema_alpha, 0.01f, 1.0f);
        if (!smoothing_initialized_) {
            smoothed_ir_low_ = frame_low;
            smoothed_ir_high_ = frame_high;
            smoothing_initialized_ = true;
        } else {
            smoothed_ir_low_ = (1.0f - alpha) * smoothed_ir_low_ + alpha * frame_low;
            smoothed_ir_high_ = (1.0f - alpha) * smoothed_ir_high_ + alpha * frame_high;
        }
        if (smoothed_ir_high_ - smoothed_ir_low_ < 256.0f) {
            smoothed_ir_high_ = smoothed_ir_low_ + 256.0f;
        }
        scale_low = smoothed_ir_low_;
        scale_high = smoothed_ir_high_;
    } else {
        if (scale_high - scale_low < 1.0f) {
            scale_high = scale_low + 1.0f;
        }
    }

    out_metrics.smoothed_low = scale_low;
    out_metrics.smoothed_high = scale_high;

    const float inv_span = 1024.0f / (scale_high - scale_low);
    uint8_t luma_min = 255;
    uint8_t luma_max = 0;
    uint64_t luma_sum = 0;

    const float* depth_px = (co_depth && co_depth->pixels.size() >= n) ? co_depth->pixels.data() : nullptr;
    uint8_t* dst = out_bgra.data();
    for (size_t i = 0; i < n; ++i) {
        if (params.cutoff_enabled) {
            const float d = depth_px ? depth_px[i] : 0.0f;
            if (!depthPassesCutoff(d, params)) {
                dst[i * 4 + 0] = 0;
                dst[i * 4 + 1] = 0;
                dst[i * 4 + 2] = 0;
                dst[i * 4 + 3] = 255;
                luma_min = 0;
                continue;
            }
        }
        float v = ir.pixels[i];
        uint8_t y = 0;
        if (!std::isfinite(v) || v <= scale_low) {
            y = 0;
        } else if (v >= 65535.0f || v >= scale_high) {
            y = 255;
        } else {
            int idx = static_cast<int>((v - scale_low) * inv_span);
            idx = std::clamp(idx, 0, 1024);
            y = gamma_lut_[idx];
        }
        dst[i * 4 + 0] = y;
        dst[i * 4 + 1] = y;
        dst[i * 4 + 2] = y;
        dst[i * 4 + 3] = 255;

        if (y < luma_min) luma_min = y;
        if (y > luma_max) luma_max = y;
        luma_sum += y;
    }

    out_metrics.mapped_luma_min = luma_min;
    out_metrics.mapped_luma_max = luma_max;
    out_metrics.mapped_luma_mean = static_cast<float>(static_cast<double>(luma_sum) / static_cast<double>(n));
}

void FrameMapper::mapDepthToBgra(
    const RawFloatFrame& depth,
    const RawFloatFrame* co_ir,
    const kv_display_params_t& params,
    std::vector<uint8_t>& out_bgra,
    uint32_t& out_w,
    uint32_t& out_h,
    PixelMetrics& out_metrics
) {
    out_w = depth.width;
    out_h = depth.height;
    const size_t n = static_cast<size_t>(out_w) * out_h;
    out_bgra.resize(n * 4);
    out_metrics = PixelMetrics{};

    if (n == 0 || depth.pixels.size() < n) {
        std::fill(out_bgra.begin(), out_bgra.end(), 0);
        return;
    }

    const float* ir_px = (co_ir && co_ir->pixels.size() == n) ? co_ir->pixels.data() : nullptr;

    const auto& lut = getColormapLut(params.depth_colormap);
    float min_mm = std::max(100.0f, params.depth_min_mm);
    float max_mm = std::max(min_mm + 50.0f, params.depth_max_mm);
    const float inv_span = 255.0f / (max_mm - min_mm);

    float valid_min = std::numeric_limits<float>::max();
    float valid_max = 0.0f;
    double valid_sum = 0.0;
    uint32_t valid_count = 0;
    uint32_t invalid_count = 0;
    uint32_t sat_ir_count = 0;

    uint8_t luma_min = 255;
    uint8_t luma_max = 0;
    uint64_t luma_sum = 0;

    uint8_t* dst = out_bgra.data();
    for (size_t i = 0; i < n; ++i) {
        const bool ir_saturated = (ir_px != nullptr) && (!std::isfinite(ir_px[i]) || ir_px[i] >= 65535.0f);
        if (ir_saturated) {
            ++sat_ir_count;
        }
        float d = depth.pixels[i];
        if (!std::isfinite(d) || d <= 0.0f || ir_saturated) {
            ++invalid_count;
            dst[i * 4 + 0] = 0;
            dst[i * 4 + 1] = 0;
            dst[i * 4 + 2] = 0;
            dst[i * 4 + 3] = 255;
            luma_min = 0;
            continue;
        }
        if (!depthPassesCutoff(d, params)) {
            dst[i * 4 + 0] = 0;
            dst[i * 4 + 1] = 0;
            dst[i * 4 + 2] = 0;
            dst[i * 4 + 3] = 255;
            luma_min = 0;
            continue;
        }

        ++valid_count;
        if (d < valid_min) valid_min = d;
        if (d > valid_max) valid_max = d;
        valid_sum += d;

        uint32_t bin16 = std::min<uint32_t>(15u, static_cast<uint32_t>((d / 8000.0f) * 16.0f));
        ++out_metrics.histogram16[bin16];

        float scaled = std::clamp((d - min_mm) * inv_span, 0.0f, 255.0f);
        int idx = static_cast<int>(std::lround(scaled));
        if (params.depth_invert) {
            idx = 255 - idx;
        }
        const BgraColor c = lut[std::clamp(idx, 0, 255)];
        dst[i * 4 + 0] = c.b;
        dst[i * 4 + 1] = c.g;
        dst[i * 4 + 2] = c.r;
        dst[i * 4 + 3] = 255;

        uint8_t y = static_cast<uint8_t>((77u * c.r + 150u * c.g + 29u * c.b) >> 8);
        if (y < luma_min) luma_min = y;
        if (y > luma_max) luma_max = y;
        luma_sum += y;
    }

    out_metrics.raw_min = (valid_count > 0) ? valid_min : 0.0f;
    out_metrics.raw_max = valid_max;
    out_metrics.raw_mean = (valid_count > 0) ? static_cast<float>(valid_sum / valid_count) : 0.0f;
    out_metrics.saturated_pixels = sat_ir_count;
    out_metrics.invalid_pixels = invalid_count;
    out_metrics.smoothed_low = min_mm;
    out_metrics.smoothed_high = max_mm;
    out_metrics.mapped_luma_min = (n > 0) ? luma_min : 0;
    out_metrics.mapped_luma_max = luma_max;
    out_metrics.mapped_luma_mean = static_cast<float>(static_cast<double>(luma_sum) / static_cast<double>(n));
}

void FrameMapper::mapColorToBgra(
    const RawColorFrame& color,
    std::vector<uint8_t>& out_bgra,
    uint32_t& out_w,
    uint32_t& out_h,
    PixelMetrics& out_metrics,
    const kv_display_params_t* params,
    const float* bigdepth_1920x1082
) {
    out_w = color.width;
    out_h = color.height;
    const size_t n = static_cast<size_t>(out_w) * out_h;
    out_bgra.resize(n * 4);
    out_metrics = PixelMetrics{};

    if (n == 0 || color.pixels.size() < n * 4) {
        std::fill(out_bgra.begin(), out_bgra.end(), 0);
        return;
    }

    const uint8_t* src = color.pixels.data();
    uint8_t* dst = out_bgra.data();
    const bool apply_cutoff = (params != nullptr && params->cutoff_enabled);
    const bool valid_bigdepth = (bigdepth_1920x1082 != nullptr && out_w == 1920 && out_h == 1080);

    uint8_t luma_min = 255;
    uint8_t luma_max = 0;
    uint64_t luma_sum = 0;
    uint64_t sampled = 0;

    if (!color.is_rgbx && !apply_cutoff) {
        std::memcpy(dst, src, n * 4);
        // Ensure opaque alpha and sample every 8th pixel for fast numerical stats on 1080p
        for (size_t i = 0; i < n; ++i) {
            dst[i * 4 + 3] = 255;
            if ((i & 7u) == 0) {
                uint8_t b = dst[i * 4 + 0];
                uint8_t g = dst[i * 4 + 1];
                uint8_t r = dst[i * 4 + 2];
                uint8_t y = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
                if (y < luma_min) luma_min = y;
                if (y > luma_max) luma_max = y;
                luma_sum += y;
                ++out_metrics.histogram16[y >> 4];
                ++sampled;
            }
        }
    } else {
        for (uint32_t row_y = 0; row_y < out_h; ++row_y) {
            const float* bd_row = valid_bigdepth
                ? (bigdepth_1920x1082 + static_cast<size_t>(row_y + 1u) * 1920u)
                : nullptr;
            for (uint32_t col_x = 0; col_x < out_w; ++col_x) {
                const size_t i = static_cast<size_t>(row_y) * out_w + col_x;
                uint8_t b = 0, g = 0, r = 0;
                if (apply_cutoff && (!bd_row || !depthPassesCutoff(bd_row[col_x], *params))) {
                    b = 0; g = 0; r = 0;
                } else if (!color.is_rgbx) {
                    b = src[i * 4 + 0];
                    g = src[i * 4 + 1];
                    r = src[i * 4 + 2];
                } else {
                    r = src[i * 4 + 0];
                    g = src[i * 4 + 1];
                    b = src[i * 4 + 2];
                }
                dst[i * 4 + 0] = b;
                dst[i * 4 + 1] = g;
                dst[i * 4 + 2] = r;
                dst[i * 4 + 3] = 255;
                if ((i & 7u) == 0) {
                    uint8_t y = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
                    if (y < luma_min) luma_min = y;
                    if (y > luma_max) luma_max = y;
                    luma_sum += y;
                    ++out_metrics.histogram16[y >> 4];
                    ++sampled;
                }
            }
        }
    }

    float mean_y = sampled > 0 ? static_cast<float>(static_cast<double>(luma_sum) / static_cast<double>(sampled)) : 0.0f;
    out_metrics.raw_min = static_cast<float>(luma_min);
    out_metrics.raw_max = static_cast<float>(luma_max);
    out_metrics.raw_mean = mean_y;
    out_metrics.mapped_luma_min = luma_min;
    out_metrics.mapped_luma_max = luma_max;
    out_metrics.mapped_luma_mean = mean_y;
}

void FrameMapper::mapSideBySide(
    const RawFloatFrame* ir,
    const RawFloatFrame* depth,
    const RawColorFrame* color,
    const kv_display_params_t& params,
    std::vector<uint8_t>& out_bgra,
    uint32_t& out_w,
    uint32_t& out_h,
    PixelMetrics& out_metrics,
    const float* bigdepth_1920x1082
) {
    PixelMetrics m_left{}, m_right{};

    if (params.sbs_layout == KV_SBS_IR_DEPTH) {
        out_w = 1024;
        out_h = 424;
        out_bgra.assign(static_cast<size_t>(out_w) * out_h * 4, 0);

        uint32_t lw = 0, lh = 0, rw = 0, rh = 0;
        if (ir && !ir->pixels.empty()) {
            mapIrToBgra(*ir, params, scratch_left_, lw, lh, m_left, depth);
        }
        if (depth && !depth->pixels.empty()) {
            mapDepthToBgra(*depth, ir, params, scratch_right_, rw, rh, m_right);
        }

        for (uint32_t y = 0; y < out_h; ++y) {
            uint8_t* dst_row = out_bgra.data() + static_cast<size_t>(y) * out_w * 4;
            if (lw == 512 && y < lh) {
                std::memcpy(dst_row, scratch_left_.data() + static_cast<size_t>(y) * 512 * 4, 512 * 4);
            }
            if (rw == 512 && y < rh) {
                std::memcpy(dst_row + 512 * 4, scratch_right_.data() + static_cast<size_t>(y) * 512 * 4, 512 * 4);
            }
        }
        out_metrics = m_right;
        out_metrics.saturated_pixels = m_left.saturated_pixels;
        return;
    }

    // KV_SBS_COLOR_DEPTH or KV_SBS_COLOR_IR -> 1280 x 424 (754x424 16:9 scaled Color + 14px divider + 512x424 Depth/IR)
    out_w = 1280;
    out_h = 424;
    out_bgra.assign(static_cast<size_t>(out_w) * out_h * 4, 0);
    for (size_t i = 0; i < static_cast<size_t>(out_w) * out_h; ++i) {
        out_bgra[i * 4 + 3] = 255;
    }

    const uint32_t color_dst_w = 754;
    if (color && color->width > 0 && color->height > 0 && color->pixels.size() >= static_cast<size_t>(color->width) * color->height * 4) {
        const uint8_t* csrc = color->pixels.data();
        const uint32_t cw = color->width;
        const uint32_t ch = color->height;
        const bool rgbx = color->is_rgbx;
        const bool valid_bd = (bigdepth_1920x1082 != nullptr && cw == 1920 && ch == 1080);
        for (uint32_t y = 0; y < out_h; ++y) {
            uint32_t sy = (y * ch) / out_h;
            const uint8_t* srow = csrc + static_cast<size_t>(sy) * cw * 4;
            const float* bd_row = valid_bd ? (bigdepth_1920x1082 + static_cast<size_t>(sy + 1u) * 1920u) : nullptr;
            uint8_t* drow = out_bgra.data() + static_cast<size_t>(y) * out_w * 4;
            for (uint32_t x = 0; x < color_dst_w; ++x) {
                uint32_t sx = (x * cw) / color_dst_w;
                if (params.cutoff_enabled && (!bd_row || !depthPassesCutoff(bd_row[sx], params))) {
                    drow[x * 4 + 0] = 0;
                    drow[x * 4 + 1] = 0;
                    drow[x * 4 + 2] = 0;
                    drow[x * 4 + 3] = 255;
                    continue;
                }
                const uint8_t* sp = srow + sx * 4;
                if (!rgbx) {
                    drow[x * 4 + 0] = sp[0];
                    drow[x * 4 + 1] = sp[1];
                    drow[x * 4 + 2] = sp[2];
                } else {
                    drow[x * 4 + 0] = sp[2];
                    drow[x * 4 + 1] = sp[1];
                    drow[x * 4 + 2] = sp[0];
                }
                drow[x * 4 + 3] = 255;
            }
        }
    }

    uint32_t rw = 0, rh = 0;
    if (params.sbs_layout == KV_SBS_COLOR_DEPTH && depth && !depth->pixels.empty()) {
        mapDepthToBgra(*depth, ir, params, scratch_right_, rw, rh, m_right);
    } else if (params.sbs_layout == KV_SBS_COLOR_IR && ir && !ir->pixels.empty()) {
        mapIrToBgra(*ir, params, scratch_right_, rw, rh, m_right, depth);
    }

    if (rw == 512) {
        const uint32_t right_x = 1280 - 512;
        for (uint32_t y = 0; y < std::min(out_h, rh); ++y) {
            std::memcpy(
                out_bgra.data() + (static_cast<size_t>(y) * out_w + right_x) * 4,
                scratch_right_.data() + static_cast<size_t>(y) * 512 * 4,
                512 * 4
            );
        }
    }
    out_metrics = m_right;
}

void FrameMapper::mapRegisteredDepthColorBlend(
    const RawFloatFrame& undistorted_depth,
    const uint8_t* registered_bgrx_512x424,
    const kv_display_params_t& params,
    std::vector<uint8_t>& out_bgra,
    uint32_t& out_w,
    uint32_t& out_h,
    PixelMetrics& out_metrics
) {
    mapDepthToBgra(undistorted_depth, nullptr, params, scratch_right_, out_w, out_h, out_metrics);
    const size_t n = static_cast<size_t>(out_w) * out_h;
    out_bgra.resize(n * 4);
    if (n == 0 || !registered_bgrx_512x424 || scratch_right_.size() < n * 4) {
        std::fill(out_bgra.begin(), out_bgra.end(), 0);
        return;
    }

    const float alpha = std::clamp(params.depth_color_blend, 0.0f, 1.0f);
    const float inv_alpha = 1.0f - alpha;
    uint8_t* dst = out_bgra.data();
    const uint8_t* dcol = scratch_right_.data();

    uint8_t luma_min = 255;
    uint8_t luma_max = 0;
    uint64_t luma_sum = 0;

    for (size_t i = 0; i < n; ++i) {
        const float d = undistorted_depth.pixels[i];
        if (!std::isfinite(d) || d <= 0.0f || !depthPassesCutoff(d, params)) {
            dst[i * 4 + 0] = 0;
            dst[i * 4 + 1] = 0;
            dst[i * 4 + 2] = 0;
            dst[i * 4 + 3] = 255;
            luma_min = 0;
            continue;
        }
        const uint8_t cb = registered_bgrx_512x424[i * 4 + 0];
        const uint8_t cg = registered_bgrx_512x424[i * 4 + 1];
        const uint8_t cr = registered_bgrx_512x424[i * 4 + 2];

        const uint8_t db = dcol[i * 4 + 0];
        const uint8_t dg = dcol[i * 4 + 1];
        const uint8_t dr = dcol[i * 4 + 2];

        const uint8_t b = static_cast<uint8_t>(std::clamp(std::lround(inv_alpha * cb + alpha * db), 0L, 255L));
        const uint8_t g = static_cast<uint8_t>(std::clamp(std::lround(inv_alpha * cg + alpha * dg), 0L, 255L));
        const uint8_t r = static_cast<uint8_t>(std::clamp(std::lround(inv_alpha * cr + alpha * dr), 0L, 255L));

        dst[i * 4 + 0] = b;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = r;
        dst[i * 4 + 3] = 255;

        const uint8_t y = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
        if (y < luma_min) luma_min = y;
        if (y > luma_max) luma_max = y;
        luma_sum += y;
    }

    out_metrics.mapped_luma_min = luma_min;
    out_metrics.mapped_luma_max = luma_max;
    out_metrics.mapped_luma_mean = static_cast<float>(static_cast<double>(luma_sum) / static_cast<double>(n));
}

void FrameMapper::drawClockOverlay(
    uint8_t* bgra,
    uint32_t width,
    uint32_t height,
    uint64_t frame_seq,
    uint64_t timestamp_ns
) {
    if (!bgra || width < 128 || height < 48) {
        return;
    }

    const uint32_t ts_us_low32 = static_cast<uint32_t>((timestamp_ns / 1000ULL) & 0xFFFFFFFFULL);
    const uint64_t word = (0xA5ULL << 56) | ((frame_seq & 0xFFFFFFULL) << 32) | static_cast<uint64_t>(ts_us_low32);

    const uint32_t bar_h = std::max(16u, height / 22u);
    const uint32_t banner_h = std::min(height, bar_h + 32u);

    // Dark backdrop for readability
    for (uint32_t y = bar_h; y < banner_h; ++y) {
        uint8_t* row = bgra + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = 16;
            row[x * 4 + 1] = 16;
            row[x * 4 + 2] = 16;
            row[x * 4 + 3] = 255;
        }
    }

    // 64-cell resolution-independent binary barcode across full frame width
    for (uint32_t bit = 0; bit < 64; ++bit) {
        bool one = ((word >> (63 - bit)) & 1ULL) != 0;
        uint8_t val = one ? 255 : 0;
        uint32_t x0 = (bit * width) / 64u;
        uint32_t x1 = ((bit + 1u) * width) / 64u;
        for (uint32_t y = 0; y < bar_h; ++y) {
            uint8_t* row = bgra + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = x0; x < x1; ++x) {
                row[x * 4 + 0] = val;
                row[x * 4 + 1] = val;
                row[x * 4 + 2] = val;
                row[x * 4 + 3] = 255;
            }
        }
    }

    // Human-readable millisecond stopwatch string
    double total_ms = static_cast<double>(timestamp_ns % 1000000000000ULL) / 1e6;
    char label[96];
    std::snprintf(label, sizeof(label), "#%06llu T=%.3fMS", static_cast<unsigned long long>(frame_seq), total_ms);
    uint32_t scale = width >= 960 ? 3u : 2u;
    drawText5x7(bgra, width, height, 10u, bar_h + 5u, scale, label);
}

bool FrameMapper::decodeClockBarcode(
    const uint8_t* row_base,
    uint32_t width,
    uint32_t height,
    uint32_t row_stride_bytes,
    uint32_t bytes_per_pixel,
    uint32_t* out_seq24,
    uint32_t* out_timestamp_low32_us
) {
    if (!row_base || width < 64 || height < 16 || (bytes_per_pixel != 1 && bytes_per_pixel != 4)) {
        return false;
    }

    const uint32_t bar_h = std::max(16u, height / 22u);
    const uint32_t sample_y = std::min(height - 1u, bar_h / 2u);
    const uint8_t* row = row_base + static_cast<size_t>(sample_y) * row_stride_bytes;

    uint64_t word = 0;
    for (uint32_t bit = 0; bit < 64; ++bit) {
        uint32_t x0 = (bit * width) / 64u;
        uint32_t x1 = ((bit + 1u) * width) / 64u;
        uint32_t cx = (x0 + x1) / 2u;
        uint8_t luma = 0;
        if (bytes_per_pixel == 1) {
            luma = row[cx];
        } else {
            const uint8_t* px = row + static_cast<size_t>(cx) * 4u;
            luma = static_cast<uint8_t>((29u * px[0] + 150u * px[1] + 77u * px[2]) >> 8);
        }
        if (luma >= 128u) {
            word |= (1ULL << (63 - bit));
        }
    }

    uint8_t magic = static_cast<uint8_t>((word >> 56) & 0xFFULL);
    if (magic != 0xA5u) {
        return false;
    }

    if (out_seq24) {
        *out_seq24 = static_cast<uint32_t>((word >> 32) & 0xFFFFFFULL);
    }
    if (out_timestamp_low32_us) {
        *out_timestamp_low32_us = static_cast<uint32_t>(word & 0xFFFFFFFFULL);
    }
    return true;
}

} // namespace kv
