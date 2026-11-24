// Media Foundation H.264 encoder: hardware MFT first (NVENC / AMF / QSV), low latency, no B-frames,
// GOP 30, CBR. Input NV12, output Annex-B access units through a callback.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kvhost {

struct EncoderOptions {
    bool prefer_hardware = true;
    bool allow_software = true; // used only if no hardware MFT accepts the settings; reported in name()
    uint32_t fps = 30;
    uint32_t gop = 30;
    uint32_t bitrate = 5000000;
};

// How closed encoders have been handled (mf_h264_encoder.cpp, parkEncoder): closing parks an encoder's COM objects
// and releases them at least 3 s later. For tests and diagnostics.
struct EncoderCloseStats {
    uint64_t parked = 0;        // encoders closed and parked
    uint64_t released = 0;      // parked encoders released
    uint64_t drainTimeouts = 0; // async MFTs whose drain didn't complete within 1 s
    uint64_t overCap = 0;       // closes that found more than 8 parked, none old enough to release
    double drainMsMax = 0;      // the longest drain of an async MFT at close
    double closeMsMax = 0;      // the longest whole close (drain, shutdown, parking)
    double releaseMsMax = 0;    // the longest release of parked encoders, done by a later close
    size_t parkedNow = 0;
};
EncoderCloseStats encoderCloseStats();

class MfH264Encoder {
public:
    // annexb: one access unit; pts_100ns: the input time it belongs to; token: the caller's value.
    using OutputFn = std::function<void(const uint8_t* annexb, size_t n, int64_t pts_100ns, uint64_t token)>;

    MfH264Encoder();
    ~MfH264Encoder();
    MfH264Encoder(const MfH264Encoder&) = delete;
    MfH264Encoder& operator=(const MfH264Encoder&) = delete;

    bool open(uint32_t width, uint32_t height, const EncoderOptions& opts, OutputFn on_output, std::string* err);
    void close();
    bool isOpen() const;

    // Submits one NV12 frame (width*height*3/2 bytes). With an async hardware MFT the frame waits for the
    // MFT's next input request; a newer frame replaces a still-waiting one (latest wins).
    bool encode(const uint8_t* nv12, size_t n, int64_t pts_100ns, bool force_keyframe, uint64_t token, std::string* err);

    std::string name() const;     // friendly name of the MFT in use
    bool isHardware() const;
    // Annex-B SPS+PPS from the output media type (MF_MT_MPEG_SEQUENCE_HEADER), if the MFT exposes it.
    std::vector<uint8_t> sequenceHeader() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace kvhost
