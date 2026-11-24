#pragma once

#include "frame_types.hpp"
#include "kinect_core_c.h"

#include <libfreenect2/libfreenect2.hpp>

#include <functional>
#include <memory>
#include <string>

namespace kv {

struct SourceCallbacks {
    std::function<void(const RawFloatFrame&)> on_ir;
    std::function<void(const RawFloatFrame&)> on_depth;
    std::function<void(const RawColorFrame&)> on_color;
    std::function<void(bool benign, const std::string& msg)> on_log;
};

struct ColorReadback {
    uint32_t exposure_mode = 0;
    float integration_ms = 0.0f;
    float analog_gain = 0.0f;
};

class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    virtual bool start(
        kv_pipeline_kind_t pipeline,
        bool enable_rgb,
        bool enable_depth_ir,
        const SourceCallbacks& callbacks,
        std::string* out_error
    ) = 0;

    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    virtual bool applyColorControls(const kv_color_controls_t& controls, ColorReadback* out_readback, std::string* out_error) = 0;

    virtual std::string serialNumber() const = 0;
    virtual std::string firmwareVersion() const = 0;
    virtual bool getIrIntrinsics(kv_ir_intrinsics_t* out_intrinsics) const = 0;
    virtual bool getRegistrationParams(
        libfreenect2::Freenect2Device::IrCameraParams* out_ir,
        libfreenect2::Freenect2Device::ColorCameraParams* out_color
    ) const = 0;
    virtual std::string usbLocationString() const = 0;
};

std::unique_ptr<IFrameSource> createSyntheticSource();
std::unique_ptr<IFrameSource> createFreenect2Source();

bool checkKinectUsbPresent();
std::string queryKinectUsbLocationString();

} // namespace kv
