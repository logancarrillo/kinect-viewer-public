// Frame preparation for the H.264 encoder and the Annex-B to AVCC repacking.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kvhost {

// Mirrors VideoToolboxEncoder.encodeBGRAFrame sizing: 512x424 (IR / Depth) is upscaled 2x nearest to
// 1024x848 so 4:2:0 chroma keeps 512x424 detail; other sizes are cropped to even dimensions.
void streamTargetSize(uint32_t src_w, uint32_t src_h, uint32_t& out_w, uint32_t& out_h, bool& upscale2x);

// BGRA8 -> NV12, BT.709 limited range (Y 16..235), optionally 2x nearest upscaled. out is resized.
void bgraToNv12(const uint8_t* bgra, uint32_t src_w, uint32_t src_h, uint32_t src_stride, bool upscale2x,
                std::vector<uint8_t>& out, uint32_t& out_w, uint32_t& out_h);

// BGRA8 -> NV12 at any even size, with the same BT.709 limited-range math as bgraToNv12. Each axis is
// area-averaged when it shrinks and bilinear when it grows. Built for the live medium / low tiers:
// the axis tables and the scratch rows are kept, so a scaler reused at one size does no allocation.
class Nv12Scaler {
public:
    // dst_w / dst_h are rounded down to even (at least 2). out is resized to dst_w * dst_h * 3 / 2.
    void convert(const uint8_t* bgra, uint32_t src_w, uint32_t src_h, uint32_t src_stride, uint32_t dst_w,
                 uint32_t dst_h, std::vector<uint8_t>& out);

private:
    struct Axis {
        uint32_t src = 0, dst = 0;
        std::vector<uint32_t> first; // first source index per output index
        std::vector<uint32_t> count; // taps per output index
        std::vector<uint32_t> offset; // into weights
        std::vector<int32_t> weights; // fixed point, each output's taps sum to 4096
    };
    static void buildAxis(Axis& a, uint32_t src, uint32_t dst);
    Axis ax_, ay_;
    std::vector<uint16_t> tmp_; // horizontal pass: src_h rows of dst_w RGB, values x16
    std::vector<int32_t> rgb_;  // two output rows of RGB
};

// One-off convenience wrapper around Nv12Scaler.
void scaleBgraToNv12(const uint8_t* bgra, uint32_t src_w, uint32_t src_h, uint32_t src_stride, uint32_t dst_w,
                     uint32_t dst_h, std::vector<uint8_t>& out);

struct AvccAccessUnit {
    std::vector<uint8_t> avcc;           // 4-byte big-endian length-prefixed NAL units
    std::vector<uint8_t> sps;            // last SPS seen in this access unit (no start code)
    std::vector<uint8_t> pps;            // last PPS seen in this access unit (no start code)
    bool idr = false;                    // contains an IDR slice (nal_unit_type 5)
    size_t nal_count = 0;                // NAL units kept in avcc
};

// Splits Annex-B (00 00 01 / 00 00 00 01 start codes). SPS (7), PPS (8) and AUD (9) are moved out of
// the access unit (SPS/PPS go to StreamConfigMessage); everything else is kept, length-prefixed.
bool annexBToAvcc(const uint8_t* data, size_t n, AvccAccessUnit& out);

// Splits an Annex-B blob into NAL units without start codes.
std::vector<std::vector<uint8_t>> splitAnnexB(const uint8_t* data, size_t n);

} // namespace kvhost
