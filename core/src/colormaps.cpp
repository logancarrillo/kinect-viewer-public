// The Turbo colour map below (evalTurbo) is ported from GLSL to C++ from Google's polynomial approximation of Turbo:
//   Copyright 2019 Google LLC.
//   SPDX-License-Identifier: Apache-2.0
//   Colour map design: Anton Mikhailov. Polynomial approximation (GLSL): Ruofei Du.
// The coefficients are unchanged. The rest of this file is this project's own (see LICENSE), and the Viridis and
// Inferno stops are CC0 1.0 (THIRD_PARTY_NOTICES.md).

#include "colormaps.hpp"

#include <algorithm>
#include <cmath>

namespace kv {

namespace {

inline uint8_t clampByte(float v) {
    float c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(c * 255.0f));
}

// Turbo's sRGB polynomial approximation (Google, 2019; Apache-2.0, see the top of this file)
BgraColor evalTurbo(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    const float x2 = x * x;
    const float x3 = x2 * x;
    const float x4 = x2 * x2;
    const float x5 = x4 * x;

    const float r = 0.13572138f + x * 4.61539260f - x2 * 42.66032258f + x3 * 132.13108234f - x4 * 152.94239396f + x5 * 59.28637943f;
    const float g = 0.09140261f + x * 2.19418839f + x2 * 4.84296658f - x3 * 14.18503333f + x4 * 4.27729857f + x5 * 2.82956604f;
    const float b = 0.10667330f + x * 12.64194608f - x2 * 60.58204836f + x3 * 110.36276771f - x4 * 89.90310912f + x5 * 27.34824973f;

    return BgraColor{clampByte(b), clampByte(g), clampByte(r), 255};
}

struct RgbStop {
    float r, g, b;
};

BgraColor evalPiecewise(float x, const RgbStop* stops, size_t count) {
    x = std::clamp(x, 0.0f, 1.0f);
    float scaled = x * static_cast<float>(count - 1);
    size_t idx = std::min(static_cast<size_t>(std::floor(scaled)), count - 2);
    float frac = scaled - static_cast<float>(idx);
    const RgbStop& a = stops[idx];
    const RgbStop& b = stops[idx + 1];
    float rr = a.r + (b.r - a.r) * frac;
    float gg = a.g + (b.g - a.g) * frac;
    float bb = a.b + (b.b - a.b) * frac;
    return BgraColor{clampByte(bb), clampByte(gg), clampByte(rr), 255};
}

// Official 17-knot sRGB reference stops from Smith & van der Walt (BIDS Viridis / Inferno)
constexpr RgbStop kViridisStops[17] = {
    {0.267004f, 0.004874f, 0.329415f}, {0.282623f, 0.094955f, 0.417331f},
    {0.278826f, 0.175490f, 0.483397f}, {0.258965f, 0.251537f, 0.524736f},
    {0.229739f, 0.322361f, 0.545706f}, {0.199430f, 0.387607f, 0.554642f},
    {0.172719f, 0.448791f, 0.557885f}, {0.149039f, 0.508051f, 0.557250f},
    {0.127568f, 0.566949f, 0.550556f}, {0.119699f, 0.625828f, 0.533488f},
    {0.157851f, 0.683765f, 0.501686f}, {0.246070f, 0.738910f, 0.452024f},
    {0.369214f, 0.788888f, 0.382914f}, {0.515992f, 0.831158f, 0.294279f},
    {0.678489f, 0.863742f, 0.189503f}, {0.845561f, 0.887322f, 0.119738f},
    {0.993248f, 0.906157f, 0.143936f}
};

constexpr RgbStop kInfernoStops[17] = {
    {0.001462f, 0.000466f, 0.013866f}, {0.056174f, 0.029803f, 0.157935f},
    {0.158588f, 0.043918f, 0.330221f}, {0.280267f, 0.043088f, 0.415026f},
    {0.401769f, 0.057411f, 0.436331f}, {0.519082f, 0.104526f, 0.427241f},
    {0.633188f, 0.165693f, 0.391046f}, {0.741381f, 0.233642f, 0.333861f},
    {0.835270f, 0.312554f, 0.260265f}, {0.910631f, 0.406620f, 0.175797f},
    {0.961293f, 0.514504f, 0.089171f}, {0.986964f, 0.630485f, 0.030908f},
    {0.987622f, 0.750104f, 0.140771f}, {0.968443f, 0.865495f, 0.317362f},
    {0.954506f, 0.955344f, 0.542516f}, {0.976511f, 0.990175f, 0.645910f},
    {0.988362f, 0.998364f, 0.644924f}
};

std::array<BgraColor, 256> buildLut(kv_colormap_kind_t kind) {
    std::array<BgraColor, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        float t = static_cast<float>(i) / 255.0f;
        switch (kind) {
            case KV_COLORMAP_TURBO:
                lut[i] = evalTurbo(t);
                break;
            case KV_COLORMAP_VIRIDIS:
                lut[i] = evalPiecewise(t, kViridisStops, 17);
                break;
            case KV_COLORMAP_INFERNO:
                lut[i] = evalPiecewise(t, kInfernoStops, 17);
                break;
            case KV_COLORMAP_GRAY:
            default: {
                uint8_t v = static_cast<uint8_t>(i);
                lut[i] = BgraColor{v, v, v, 255};
                break;
            }
        }
    }
    return lut;
}

} // namespace

const std::array<BgraColor, 256>& getColormapLut(kv_colormap_kind_t kind) {
    static const auto kTurbo = buildLut(KV_COLORMAP_TURBO);
    static const auto kViridis = buildLut(KV_COLORMAP_VIRIDIS);
    static const auto kInferno = buildLut(KV_COLORMAP_INFERNO);
    static const auto kGray = buildLut(KV_COLORMAP_GRAY);

    switch (kind) {
        case KV_COLORMAP_TURBO: return kTurbo;
        case KV_COLORMAP_VIRIDIS: return kViridis;
        case KV_COLORMAP_INFERNO: return kInferno;
        case KV_COLORMAP_GRAY:
        default: return kGray;
    }
}

} // namespace kv
