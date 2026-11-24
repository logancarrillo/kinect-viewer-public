#pragma once

#include "kinect_core_c.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <chrono>
#endif

namespace kv {

inline uint64_t monotonicNs() {
#ifdef __APPLE__
    static mach_timebase_info_data_t info = {0, 0};
    if (info.denom == 0) {
        mach_timebase_info(&info);
    }
    const uint64_t ticks = mach_absolute_time();
    // Avoid uint64 overflow over multi-day uptimes via 128-bit arithmetic
    return static_cast<uint64_t>((static_cast<__uint128_t>(ticks) * info.numer) / info.denom);
#else
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

struct RawFloatFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sequence = 0;
    uint32_t device_timestamp_ticks = 0; // 0.125 ms ticks
    uint64_t arrival_ns = 0;
    uint64_t local_counter = 0;
    std::vector<float> pixels;
};

struct RawColorFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sequence = 0;
    uint32_t device_timestamp_ticks = 0;
    uint64_t arrival_ns = 0;
    uint64_t local_counter = 0;
    float exposure = 0.0f;
    float gain = 1.0f;
    float gamma = 1.0f;
    bool is_rgbx = false; // false = BGRX, true = RGBX
    std::vector<uint8_t> pixels; // 4 bytes per pixel
};

} // namespace kv
