// The camera-roll thumbnail (pet-camera.md §6.1 "Thumbnail"): one live BGRA frame, scaled to at most
// 320 px wide with its aspect ratio kept, as a WIC JPEG of at most 64 KiB. This is part of the colour
// exception (§6.6): it lives in its own roll_* file and only returns the bytes; the host writes them
// into the camera-roll folder.
//
// COM: it initialises COM (multithreaded) on the calling thread for the call if the thread hasn't, and
// works in an existing apartment of either kind. It is synchronous and quick (one small encode, a few
// more when the quality has to come down), but the host should still call it off the frame thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kvhost {

constexpr uint32_t kRollThumbMaxWidth = 320;
constexpr size_t kRollThumbMaxBytes = 64 * 1024;
constexpr float kRollThumbStartQuality = 0.75f;

struct RollThumbnail {
    std::vector<uint8_t> jpeg;
    uint32_t width = 0, height = 0;
    float quality = 0.0f; // the WIC ImageQuality that fitted
};

// The JPEG quality steps tried, in order: 0.75, then down by 0.1 to 0.15, then 0.10 and 0.05. The first
// result of at most kRollThumbMaxBytes is kept.
const std::vector<float>& rollThumbQualitySteps();

// The thumbnail's size for a `width` x `height` frame: the same when width <= 320, otherwise 320 wide
// and the height scaled to match (rounded, at least 1).
void rollThumbSize(uint32_t width, uint32_t height, uint32_t* outWidth, uint32_t* outHeight);

// `bgra` is `height` rows of `strideBytes` (at least width * 4) bytes, 8-bit B, G, R, A; alpha is
// ignored. False, with a reason in `err`, for a bad frame, a WIC failure, or a JPEG still over 64 KiB at
// the lowest quality (not expected at 320 px wide; the caller then writes no thumbnail).
bool makeRollThumbnail(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t strideBytes,
                       RollThumbnail& out, std::string* err = nullptr);

} // namespace kvhost
