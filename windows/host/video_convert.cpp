#include "video_convert.hpp"

#include <algorithm>
#include <cstring>

namespace kvhost {

void streamTargetSize(uint32_t w, uint32_t h, uint32_t& ow, uint32_t& oh, bool& up) {
    up = (w == 512 && h == 424);
    ow = up ? w * 2 : (w & ~1u);
    oh = up ? h * 2 : (h & ~1u);
}

namespace {

// BT.709 limited range, 8-bit fixed point (coefficients x 256).
inline uint8_t lumaOf(int r, int g, int b) {
    return static_cast<uint8_t>(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
}
inline uint8_t cbOf(int r, int g, int b) {
    return static_cast<uint8_t>(std::clamp(((-26 * r - 86 * g + 112 * b + 128) >> 8) + 128, 0, 255));
}
inline uint8_t crOf(int r, int g, int b) {
    return static_cast<uint8_t>(std::clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128, 0, 255));
}

} // namespace

void bgraToNv12(const uint8_t* bgra, uint32_t sw, uint32_t sh, uint32_t stride, bool up, std::vector<uint8_t>& out,
                uint32_t& ow, uint32_t& oh) {
    bool up_unused = false;
    streamTargetSize(sw, sh, ow, oh, up_unused);
    if (!up) {
        ow = sw & ~1u;
        oh = sh & ~1u;
    }
    out.resize(static_cast<size_t>(ow) * oh * 3 / 2);
    uint8_t* yplane = out.data();
    uint8_t* uv = out.data() + static_cast<size_t>(ow) * oh;
    const uint32_t scale = up ? 2 : 1;

    for (uint32_t y = 0; y < oh; ++y) {
        const uint8_t* srow = bgra + static_cast<size_t>(y / scale) * stride;
        uint8_t* yrow = yplane + static_cast<size_t>(y) * ow;
        for (uint32_t x = 0; x < ow; ++x) {
            const uint8_t* px = srow + static_cast<size_t>(x / scale) * 4;
            yrow[x] = lumaOf(px[2], px[1], px[0]);
        }
    }
    for (uint32_t y = 0; y < oh; y += 2) {
        uint8_t* uvrow = uv + static_cast<size_t>(y / 2) * ow;
        const uint8_t* r0 = bgra + static_cast<size_t>(y / scale) * stride;
        const uint8_t* r1 = bgra + static_cast<size_t>((y + 1) / scale) * stride;
        for (uint32_t x = 0; x < ow; x += 2) {
            const uint8_t* a = r0 + static_cast<size_t>(x / scale) * 4;
            const uint8_t* b = r0 + static_cast<size_t>((x + 1) / scale) * 4;
            const uint8_t* c = r1 + static_cast<size_t>(x / scale) * 4;
            const uint8_t* d = r1 + static_cast<size_t>((x + 1) / scale) * 4;
            const int R = (a[2] + b[2] + c[2] + d[2] + 2) >> 2;
            const int G = (a[1] + b[1] + c[1] + d[1] + 2) >> 2;
            const int B = (a[0] + b[0] + c[0] + d[0] + 2) >> 2;
            uvrow[x] = cbOf(R, G, B);
            uvrow[x + 1] = crOf(R, G, B);
        }
    }
}

// Fixed-point weights per axis: each output index's taps sum to exactly 4096, so a flat colour comes out
// exactly as bgraToNv12 makes it, and an unscaled axis is a plain copy.
void Nv12Scaler::buildAxis(Axis& a, uint32_t src, uint32_t dst) {
    a.src = src;
    a.dst = dst;
    a.first.assign(dst, 0);
    a.count.assign(dst, 0);
    a.offset.assign(dst, 0);
    a.weights.clear();
    for (uint32_t i = 0; i < dst; ++i) {
        a.offset[i] = static_cast<uint32_t>(a.weights.size());
        if (src == dst) {
            a.first[i] = i;
            a.count[i] = 1;
            a.weights.push_back(4096);
        } else if (dst < src) {
            // Area average: output i covers source [i*src/dst, (i+1)*src/dst). Work in 1/dst units so the
            // overlaps are exact integers that add up to src.
            const uint64_t lo = static_cast<uint64_t>(i) * src;
            const uint64_t hi = static_cast<uint64_t>(i + 1) * src;
            const uint32_t j0 = static_cast<uint32_t>(lo / dst);
            const uint32_t j1 = static_cast<uint32_t>((hi - 1) / dst);
            a.first[i] = j0;
            a.count[i] = j1 - j0 + 1;
            int32_t sum = 0, biggest_w = -1;
            size_t biggest = a.weights.size();
            for (uint32_t j = j0; j <= j1; ++j) {
                const uint64_t s0 = std::max<uint64_t>(lo, static_cast<uint64_t>(j) * dst);
                const uint64_t s1 = std::min<uint64_t>(hi, static_cast<uint64_t>(j + 1) * dst);
                const int32_t w = static_cast<int32_t>(((s1 - s0) * 4096 + src / 2) / src);
                if (w > biggest_w) {
                    biggest_w = w;
                    biggest = a.weights.size();
                }
                a.weights.push_back(w);
                sum += w;
            }
            a.weights[biggest] += 4096 - sum; // rounding remainder goes to the largest tap
        } else {
            // Bilinear: output centre (i + 0.5) * src / dst - 0.5, as num / den with den = 2 * dst.
            const int64_t num = static_cast<int64_t>(2 * i + 1) * src - dst;
            const int64_t den = 2 * static_cast<int64_t>(dst);
            uint32_t j0 = 0;
            int32_t w1 = 0;
            if (num > 0) {
                j0 = static_cast<uint32_t>(num / den);
                w1 = static_cast<int32_t>(((num % den) * 4096 + den / 2) / den);
            }
            if (j0 >= src - 1) {
                j0 = src - 1;
                w1 = 0;
            }
            a.first[i] = j0;
            if (w1 == 0) {
                a.count[i] = 1;
                a.weights.push_back(4096);
            } else if (w1 == 4096) {
                a.first[i] = j0 + 1;
                a.count[i] = 1;
                a.weights.push_back(4096);
            } else {
                a.count[i] = 2;
                a.weights.push_back(4096 - w1);
                a.weights.push_back(w1);
            }
        }
    }
}

void Nv12Scaler::convert(const uint8_t* bgra, uint32_t sw, uint32_t sh, uint32_t stride, uint32_t dst_w, uint32_t dst_h,
                         std::vector<uint8_t>& out) {
    const uint32_t dw = std::max<uint32_t>(2, dst_w & ~1u);
    const uint32_t dh = std::max<uint32_t>(2, dst_h & ~1u);
    const size_t yn = static_cast<size_t>(dw) * dh;
    out.resize(yn * 3 / 2);
    if (!bgra || sw == 0 || sh == 0) {
        std::memset(out.data(), 16, yn);
        std::memset(out.data() + yn, 128, yn / 2);
        return;
    }
    if (ax_.src != sw || ax_.dst != dw) buildAxis(ax_, sw, dw);
    if (ay_.src != sh || ay_.dst != dh) buildAxis(ay_, sh, dh);

    // Horizontal pass: every source row to dst_w RGB, kept at x16 precision.
    const size_t trow = static_cast<size_t>(dw) * 3;
    tmp_.resize(trow * sh);
    for (uint32_t y = 0; y < sh; ++y) {
        const uint8_t* srow = bgra + static_cast<size_t>(y) * stride;
        uint16_t* t = tmp_.data() + trow * y;
        for (uint32_t x = 0; x < dw; ++x) {
            const int32_t* w = ax_.weights.data() + ax_.offset[x];
            const uint8_t* px = srow + static_cast<size_t>(ax_.first[x]) * 4;
            int32_t r = 0, g = 0, b = 0;
            for (uint32_t k = 0; k < ax_.count[x]; ++k, px += 4) {
                b += px[0] * w[k];
                g += px[1] * w[k];
                r += px[2] * w[k];
            }
            t[x * 3 + 0] = static_cast<uint16_t>((r + 128) >> 8);
            t[x * 3 + 1] = static_cast<uint16_t>((g + 128) >> 8);
            t[x * 3 + 2] = static_cast<uint16_t>((b + 128) >> 8);
        }
    }

    // Vertical pass, two output rows at a time so the chroma of each 2x2 block is the average of its four
    // output pixels, exactly as bgraToNv12 averages its four source pixels.
    uint8_t* yplane = out.data();
    uint8_t* uv = out.data() + yn;
    rgb_.resize(trow * 2);
    for (uint32_t oy = 0; oy < dh; oy += 2) {
        for (uint32_t r = 0; r < 2; ++r) {
            const uint32_t row = oy + r;
            int32_t* acc = rgb_.data() + trow * r;
            std::fill(acc, acc + trow, 0);
            const int32_t* w = ay_.weights.data() + ay_.offset[row];
            for (uint32_t k = 0; k < ay_.count[row]; ++k) {
                const uint16_t* t = tmp_.data() + trow * (ay_.first[row] + k);
                const int32_t wk = w[k];
                for (size_t i = 0; i < trow; ++i) acc[i] += t[i] * wk;
            }
            uint8_t* yrow = yplane + static_cast<size_t>(row) * dw;
            for (uint32_t x = 0; x < dw; ++x) {
                int32_t* p = acc + static_cast<size_t>(x) * 3;
                p[0] = std::clamp((p[0] + 32768) >> 16, 0, 255);
                p[1] = std::clamp((p[1] + 32768) >> 16, 0, 255);
                p[2] = std::clamp((p[2] + 32768) >> 16, 0, 255);
                yrow[x] = lumaOf(p[0], p[1], p[2]);
            }
        }
        const int32_t* r0 = rgb_.data();
        const int32_t* r1 = rgb_.data() + trow;
        uint8_t* uvrow = uv + static_cast<size_t>(oy / 2) * dw;
        for (uint32_t x = 0; x < dw; x += 2) {
            const int32_t* a = r0 + static_cast<size_t>(x) * 3;
            const int32_t* b = a + 3;
            const int32_t* c = r1 + static_cast<size_t>(x) * 3;
            const int32_t* d = c + 3;
            const int R = (a[0] + b[0] + c[0] + d[0] + 2) >> 2;
            const int G = (a[1] + b[1] + c[1] + d[1] + 2) >> 2;
            const int B = (a[2] + b[2] + c[2] + d[2] + 2) >> 2;
            uvrow[x] = cbOf(R, G, B);
            uvrow[x + 1] = crOf(R, G, B);
        }
    }
}

void scaleBgraToNv12(const uint8_t* bgra, uint32_t sw, uint32_t sh, uint32_t stride, uint32_t dst_w, uint32_t dst_h,
                     std::vector<uint8_t>& out) {
    Nv12Scaler s;
    s.convert(bgra, sw, sh, stride, dst_w, dst_h, out);
}

std::vector<std::vector<uint8_t>> splitAnnexB(const uint8_t* d, size_t n) {
    std::vector<std::vector<uint8_t>> nals;
    size_t i = 0;
    auto startCodeAt = [&](size_t p, size_t& len) -> bool {
        if (p + 3 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1) { len = 3; return true; }
        if (p + 4 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 1) { len = 4; return true; }
        return false;
    };
    size_t sc = 0;
    while (i < n && !startCodeAt(i, sc)) ++i;
    while (i < n) {
        if (!startCodeAt(i, sc)) break;
        const size_t begin = i + sc;
        size_t j = begin;
        size_t next_sc = 0;
        while (j < n && !startCodeAt(j, next_sc)) ++j;
        size_t end = j;
        while (end > begin && d[end - 1] == 0 && j < n) --end; // trailing_zero_8bits before a 4-byte code
        if (end > begin) nals.emplace_back(d + begin, d + end);
        i = j;
    }
    return nals;
}

bool annexBToAvcc(const uint8_t* data, size_t n, AvccAccessUnit& out) {
    out = AvccAccessUnit{};
    const auto nals = splitAnnexB(data, n);
    for (const auto& nal : nals) {
        const uint8_t type = nal[0] & 0x1F;
        if (type == 7) { out.sps = nal; continue; }
        if (type == 8) { out.pps = nal; continue; }
        if (type == 9) continue;
        if (type == 5) out.idr = true;
        const uint32_t len = static_cast<uint32_t>(nal.size());
        out.avcc.push_back(static_cast<uint8_t>(len >> 24));
        out.avcc.push_back(static_cast<uint8_t>(len >> 16));
        out.avcc.push_back(static_cast<uint8_t>(len >> 8));
        out.avcc.push_back(static_cast<uint8_t>(len));
        out.avcc.insert(out.avcc.end(), nal.begin(), nal.end());
        ++out.nal_count;
    }
    return out.nal_count > 0;
}

} // namespace kvhost
