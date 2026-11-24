#pragma once

#include "kinect_core_c.h"
#include <array>
#include <cstdint>

namespace kv {

struct BgraColor {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
};

const std::array<BgraColor, 256>& getColormapLut(kv_colormap_kind_t kind);

} // namespace kv
