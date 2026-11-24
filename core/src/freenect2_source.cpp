#include "frame_source.hpp"

#include <libfreenect2/color_settings.h>
#include <libfreenect2/config.h>
#include <libfreenect2/frame_listener.hpp>
#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/logger.h>
#include <libfreenect2/packet_pipeline.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <dlfcn.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <cwctype>
#include <string>
#endif

namespace kv {

namespace {

std::mutex g_log_cb_mutex;
std::function<void(libfreenect2::Logger::Level, const std::string&)> g_log_sink;
std::string g_last_error_msg;
std::atomic<uint64_t> g_last_device_close_ns{0};

class QuietForwardingLogger final : public libfreenect2::Logger {
public:
    QuietForwardingLogger() {
        level_ = libfreenect2::Logger::Warning;
    }

    void log(Level level, const std::string& message) override {
        std::lock_guard<std::mutex> lk(g_log_cb_mutex);
        if (level == libfreenect2::Logger::Error) {
            g_last_error_msg = message;
        }
        if (g_log_sink) {
            g_log_sink(level, message);
        }
    }
};

void ensureGlobalLoggerInstalled() {
    static std::once_flag once;
    std::call_once(once, []() {
        libfreenect2::setGlobalLogger(new QuietForwardingLogger());
    });
}

bool isKnownBenignDriverMessage(const std::string& msg) {
    return msg.find("packets were lost") != std::string::npos ||
           msg.find("LIBUSB_ERROR_NO_DEVICE") != std::string::npos ||
           msg.find("failed to release interface") != std::string::npos;
}

#ifdef __APPLE__
void configureGlfwCocoaHintsIfLoaded() {
    using GlfwInitHintFn = void (*)(int, int);
    auto fn = reinterpret_cast<GlfwInitHintFn>(dlsym(RTLD_DEFAULT, "glfwInitHint"));
    if (fn) {
        constexpr int kGlfwCocoaChdirResources = 0x00051001;
        constexpr int kGlfwCocoaMenubar = 0x00051002;
        fn(kGlfwCocoaChdirResources, 0);
        fn(kGlfwCocoaMenubar, 0);
    }
}
#endif

class DirectFrameListener final : public libfreenect2::FrameListener {
public:
    explicit DirectFrameListener(SourceCallbacks callbacks)
        : callbacks_(std::move(callbacks)) {}

    bool onNewFrame(libfreenect2::Frame::Type type, libfreenect2::Frame* frame) override {
        const uint64_t arrival_ns = monotonicNs();
        if (!frame || frame->status != 0 || !frame->data || frame->width == 0 || frame->height == 0) {
            return false; // Let libfreenect2 recycle invalid/dropped packet frame
        }

        const uint32_t w = static_cast<uint32_t>(frame->width);
        const uint32_t h = static_cast<uint32_t>(frame->height);
        const size_t px_count = static_cast<size_t>(w) * h;

        if (type == libfreenect2::Frame::Ir) {
            if (callbacks_.on_ir && frame->bytes_per_pixel == 4) {
                ir_buf_.width = w;
                ir_buf_.height = h;
                ir_buf_.sequence = frame->sequence;
                ir_buf_.device_timestamp_ticks = frame->timestamp;
                ir_buf_.arrival_ns = arrival_ns;
                ir_buf_.local_counter = ++ir_count_;
                ir_buf_.pixels.resize(px_count);
                std::memcpy(ir_buf_.pixels.data(), frame->data, px_count * sizeof(float));
                callbacks_.on_ir(ir_buf_);
            }
        } else if (type == libfreenect2::Frame::Depth) {
            if (callbacks_.on_depth && frame->bytes_per_pixel == 4) {
                depth_buf_.width = w;
                depth_buf_.height = h;
                depth_buf_.sequence = frame->sequence;
                depth_buf_.device_timestamp_ticks = frame->timestamp;
                depth_buf_.arrival_ns = arrival_ns;
                depth_buf_.local_counter = ++depth_count_;
                depth_buf_.pixels.resize(px_count);
                std::memcpy(depth_buf_.pixels.data(), frame->data, px_count * sizeof(float));
                callbacks_.on_depth(depth_buf_);
            }
        } else if (type == libfreenect2::Frame::Color) {
            if (callbacks_.on_color && frame->bytes_per_pixel == 4) {
                color_buf_.width = w;
                color_buf_.height = h;
                color_buf_.sequence = frame->sequence;
                color_buf_.device_timestamp_ticks = frame->timestamp;
                color_buf_.arrival_ns = arrival_ns;
                color_buf_.local_counter = ++color_count_;
                color_buf_.exposure = frame->exposure;
                color_buf_.gain = frame->gain;
                color_buf_.gamma = frame->gamma;
                color_buf_.is_rgbx = (frame->format == libfreenect2::Frame::RGBX);
                color_buf_.pixels.resize(px_count * 4);
                std::memcpy(color_buf_.pixels.data(), frame->data, px_count * 4);
                callbacks_.on_color(color_buf_);
            }
        }

        return false; // Caller (libfreenect2) retains ownership and reuses/frees frame
    }

private:
    SourceCallbacks callbacks_;
    RawFloatFrame ir_buf_;
    RawFloatFrame depth_buf_;
    RawColorFrame color_buf_;
    uint64_t ir_count_ = 0;
    uint64_t depth_count_ = 0;
    uint64_t color_count_ = 0;
};

class Freenect2FrameSource final : public IFrameSource {
public:
    ~Freenect2FrameSource() override {
        stop();
    }

    bool start(
        kv_pipeline_kind_t pipeline,
        bool enable_rgb,
        bool enable_depth_ir,
        const SourceCallbacks& callbacks,
        std::string* out_error
    ) override {
        stop();
        ensureGlobalLoggerInstalled();

        {
            std::lock_guard<std::mutex> lk(g_log_cb_mutex);
            g_last_error_msg.clear();
            g_log_sink = [callbacks](libfreenect2::Logger::Level level, const std::string& msg) {
                const bool benign = isKnownBenignDriverMessage(msg);
                if (callbacks.on_log && (benign || level <= libfreenect2::Logger::Warning)) {
                    callbacks.on_log(benign, msg);
                }
            };
        }

        freenect2_ = std::make_unique<libfreenect2::Freenect2>();
        int num_devices = freenect2_->enumerateDevices();
        // After libfreenect2 device_->close() (which resets USB) or fresh hotplug, macOS XHCI re-enumeration can take ~1.0-1.8s.
        const uint64_t since_close_ns = monotonicNs() - g_last_device_close_ns.load();
        if (num_devices <= 0 && (since_close_ns < 3500000000ULL || checkKinectUsbPresent())) {
            const uint64_t wait_deadline = monotonicNs() + 2800000000ULL;
            while (num_devices <= 0 && monotonicNs() < wait_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                freenect2_ = std::make_unique<libfreenect2::Freenect2>();
                num_devices = freenect2_->enumerateDevices();
            }
        }
        if (num_devices <= 0) {
            freenect2_.reset();
            if (out_error) {
                *out_error = "No Kinect v2 sensor found on USB 3 bus.";
            }
            return false;
        }

        std::string serial = freenect2_->getDefaultDeviceSerialNumber();

        auto make_requested_pipeline = [&](libfreenect2::PacketPipeline** out_pipe) -> bool {
            *out_pipe = nullptr;
            switch (pipeline) {
                case KV_PIPELINE_OPENGL:
#ifdef LIBFREENECT2_WITH_OPENGL_SUPPORT
#ifdef __APPLE__
                    configureGlfwCocoaHintsIfLoaded();
#endif
                    *out_pipe = new libfreenect2::OpenGLPacketPipeline(nullptr, false);
                    return true;
#else
                    if (out_error) *out_error = "Requested OpenGL pipeline, but libfreenect2 was built without OpenGL support.";
                    return false;
#endif
                case KV_PIPELINE_OPENCL:
#ifdef LIBFREENECT2_WITH_OPENCL_SUPPORT
                    *out_pipe = new libfreenect2::OpenCLPacketPipeline(-1);
                    return true;
#else
                    if (out_error) *out_error = "Requested OpenCL pipeline, but libfreenect2 was built without OpenCL support.";
                    return false;
#endif
                case KV_PIPELINE_CPU:
                    *out_pipe = new libfreenect2::CpuPacketPipeline();
                    return true;
                default:
                    if (out_error) *out_error = "Unknown packet pipeline requested.";
                    return false;
            }
        };

        // Construct the exact requested PacketPipeline — NEVER fall back silently.
        libfreenect2::PacketPipeline* pipe = nullptr;
        if (!make_requested_pipeline(&pipe)) {
            freenect2_.reset();
            return false;
        }

        // Note: openDevice takes ownership of `pipe` and frees it even on failure.
        device_ = freenect2_->openDevice(serial, pipe);
        if (!device_ && since_close_ns < 3500000000ULL) {
            // Single settle retry with the EXACT SAME pipeline if XHCI interface claim was mid-reset
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            freenect2_ = std::make_unique<libfreenect2::Freenect2>();
            if (freenect2_->enumerateDevices() > 0 && make_requested_pipeline(&pipe)) {
                serial = freenect2_->getDefaultDeviceSerialNumber();
                device_ = freenect2_->openDevice(serial, pipe);
            }
        }
        if (!device_) {
            std::string detail;
            {
                std::lock_guard<std::mutex> lk(g_log_cb_mutex);
                detail = g_last_error_msg;
            }
            if (out_error) {
                const char* pname = (pipeline == KV_PIPELINE_OPENGL) ? "OpenGL" :
                                    (pipeline == KV_PIPELINE_OPENCL) ? "OpenCL" : "CPU";
                *out_error = std::string("Failed to open Kinect v2 with ") + pname +
                             " pipeline (no fallback performed)." +
                             (detail.empty() ? "" : (" Driver: " + detail));
            }
            freenect2_.reset();
            return false;
        }

        listener_ = std::make_unique<DirectFrameListener>(callbacks);
        device_->setColorFrameListener(listener_.get());
        device_->setIrAndDepthFrameListener(listener_.get());

        // Set wide hardware depth clip [0.3 m, 8.0 m] so live UI range sliders [300..8000 mm] work instantaneously.
        libfreenect2::Freenect2Device::Config cfg;
        cfg.MinDepth = 0.3f;
        cfg.MaxDepth = 8.0f;
        cfg.EnableBilateralFilter = true;
        cfg.EnableEdgeAwareFilter = true;
        device_->setConfiguration(cfg);

        if (!device_->startStreams(enable_rgb, enable_depth_ir)) {
            std::string detail;
            {
                std::lock_guard<std::mutex> lk(g_log_cb_mutex);
                detail = g_last_error_msg;
            }
            device_->close();
            delete device_;
            device_ = nullptr;
            listener_.reset();
            freenect2_.reset();
            if (out_error) {
                const char* pname = (pipeline == KV_PIPELINE_OPENGL) ? "OpenGL" :
                                    (pipeline == KV_PIPELINE_OPENCL) ? "OpenCL" : "CPU";
                *out_error = std::string("Kinect v2 startStreams failed on ") + pname +
                             " pipeline (no fallback performed)." +
                             (detail.empty() ? "" : (" Driver: " + detail));
            }
            return false;
        }

        serial_ = device_->getSerialNumber();
        firmware_ = device_->getFirmwareVersion();
        const auto ir_p = device_->getIrCameraParams();
        ir_cam_params_ = ir_p;
        color_cam_params_ = device_->getColorCameraParams();
        ir_intrinsics_.valid = (ir_p.fx > 0.0f && ir_p.fy > 0.0f);
        ir_intrinsics_.fx = ir_p.fx;
        ir_intrinsics_.fy = ir_p.fy;
        ir_intrinsics_.cx = ir_p.cx;
        ir_intrinsics_.cy = ir_p.cy;
        ir_intrinsics_.k1 = ir_p.k1;
        ir_intrinsics_.k2 = ir_p.k2;
        ir_intrinsics_.k3 = ir_p.k3;
        ir_intrinsics_.p1 = ir_p.p1;
        ir_intrinsics_.p2 = ir_p.p2;
        usb_location_ = queryKinectUsbLocationString();
        rgb_enabled_ = enable_rgb;
        running_.store(true);
        return true;
    }

    void stop() override {
        running_.store(false);
        if (device_) {
            device_->stop();
            device_->close();
            delete device_;
            device_ = nullptr;
            g_last_device_close_ns.store(monotonicNs());
        }
        listener_.reset();
        freenect2_.reset();
        {
            std::lock_guard<std::mutex> lk(g_log_cb_mutex);
            g_log_sink = nullptr;
        }
    }

    bool isRunning() const override {
        return running_.load();
    }

    bool applyColorControls(const kv_color_controls_t& controls, ColorReadback* out_readback, std::string* out_error) override {
        std::lock_guard<std::mutex> lk(ctrl_mutex_);
        if (!device_ || !running_.load()) {
            if (out_error) *out_error = "Device is not running.";
            return false;
        }
        switch (controls.mode) {
            case KV_EXPOSURE_AUTO:
                device_->setColorAutoExposure(controls.auto_exposure_compensation);
                break;
            case KV_EXPOSURE_SEMI_AUTO:
                device_->setColorSemiAutoExposure(controls.semi_auto_pseudo_ms);
                break;
            case KV_EXPOSURE_MANUAL:
                device_->setColorManualExposure(controls.manual_integration_ms, controls.manual_analog_gain);
                break;
        }
        if (out_readback) {
            out_readback->exposure_mode = device_->getColorSetting(libfreenect2::COLOR_SETTING_GET_EXPOSURE_MODE);
            out_readback->integration_ms = device_->getColorSettingFloat(libfreenect2::COLOR_SETTING_GET_INTEGRATION_TIME);
            out_readback->analog_gain = device_->getColorSettingFloat(libfreenect2::COLOR_SETTING_GET_ANALOG_GAIN);
        }
        return true;
    }

    std::string serialNumber() const override {
        return serial_;
    }

    std::string firmwareVersion() const override {
        return firmware_;
    }

    bool getIrIntrinsics(kv_ir_intrinsics_t* out_intrinsics) const override {
        if (!out_intrinsics) return false;
        *out_intrinsics = ir_intrinsics_;
        return ir_intrinsics_.valid;
    }

    bool getRegistrationParams(
        libfreenect2::Freenect2Device::IrCameraParams* out_ir,
        libfreenect2::Freenect2Device::ColorCameraParams* out_color
    ) const override {
        if (!ir_intrinsics_.valid) return false;
        if (out_ir) *out_ir = ir_cam_params_;
        if (out_color) *out_color = color_cam_params_;
        return true;
    }

    std::string usbLocationString() const override {
        return usb_location_;
    }

private:
    std::unique_ptr<libfreenect2::Freenect2> freenect2_;
    libfreenect2::Freenect2Device* device_ = nullptr;
    std::unique_ptr<DirectFrameListener> listener_;
    std::atomic<bool> running_{false};
    bool rgb_enabled_ = false;
    std::mutex ctrl_mutex_;
    std::string serial_;
    std::string firmware_;
    kv_ir_intrinsics_t ir_intrinsics_{};
    libfreenect2::Freenect2Device::IrCameraParams ir_cam_params_{};
    libfreenect2::Freenect2Device::ColorCameraParams color_cam_params_{};
    std::string usb_location_;
};

#ifdef __APPLE__
bool matchUsbClassForKinect(const char* class_name, std::string* out_location = nullptr) {
    CFMutableDictionaryRef matching = IOServiceMatching(class_name);
    if (!matching) {
        return false;
    }
    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != KERN_SUCCESS) {
        return false;
    }

    bool found = false;
    io_service_t service = 0;
    while ((service = IOIteratorNext(iter)) != 0) {
        CFTypeRef vid_ref = IORegistryEntryCreateCFProperty(service, CFSTR("idVendor"), kCFAllocatorDefault, 0);
        CFTypeRef pid_ref = IORegistryEntryCreateCFProperty(service, CFSTR("idProduct"), kCFAllocatorDefault, 0);
        int vid = 0, pid = 0;
        if (vid_ref && CFGetTypeID(vid_ref) == CFNumberGetTypeID()) {
            CFNumberGetValue(static_cast<CFNumberRef>(vid_ref), kCFNumberIntType, &vid);
        }
        if (pid_ref && CFGetTypeID(pid_ref) == CFNumberGetTypeID()) {
            CFNumberGetValue(static_cast<CFNumberRef>(pid_ref), kCFNumberIntType, &pid);
        }
        if (vid_ref) CFRelease(vid_ref);
        if (pid_ref) CFRelease(pid_ref);

        if (vid == 0x045E && (pid == 0x02C4 || pid == 0x02D8)) {
            if (out_location) {
                uint32_t loc_id = 0;
                int port_num = -1;
                CFTypeRef loc_ref = IORegistryEntryCreateCFProperty(service, CFSTR("locationID"), kCFAllocatorDefault, 0);
                if (loc_ref && CFGetTypeID(loc_ref) == CFNumberGetTypeID()) {
                    CFNumberGetValue(static_cast<CFNumberRef>(loc_ref), kCFNumberSInt32Type, &loc_id);
                }
                if (loc_ref) CFRelease(loc_ref);

                CFTypeRef port_ref = IORegistryEntryCreateCFProperty(service, CFSTR("PortNum"), kCFAllocatorDefault, 0);
                if (port_ref && CFGetTypeID(port_ref) == CFNumberGetTypeID()) {
                    CFNumberGetValue(static_cast<CFNumberRef>(port_ref), kCFNumberIntType, &port_num);
                }
                if (port_ref) CFRelease(port_ref);

                char buf[96];
                if (loc_id != 0 && port_num >= 0) {
                    std::snprintf(buf, sizeof(buf), "USB 3 · loc 0x%08x (port %d)", loc_id, port_num);
                } else if (loc_id != 0) {
                    std::snprintf(buf, sizeof(buf), "USB 3 · loc 0x%08x", loc_id);
                } else {
                    std::snprintf(buf, sizeof(buf), "USB 3 · 045e:%04x", pid);
                }
                *out_location = buf;
            }
            IOObjectRelease(service);
            found = true;
            break;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return found;
}
#endif

#ifdef _WIN32
// Enumerates present USB device nodes via SetupDi (no device is opened) and matches the
// Kinect v2 composite parent VID_045E&PID_02C4 (or the 02D8 variant), whatever driver is bound.
bool setupDiFindKinect() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    wchar_t id[512];
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, static_cast<DWORD>(sizeof(id) / sizeof(id[0])), nullptr)) {
            continue;
        }
        std::wstring upper(id);
        for (wchar_t& c : upper) c = static_cast<wchar_t>(std::towupper(c));
        if (upper.find(L"VID_045E&PID_02C4") != std::wstring::npos ||
            upper.find(L"VID_045E&PID_02D8") != std::wstring::npos) {
            found = true;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
#endif

} // namespace

std::unique_ptr<IFrameSource> createFreenect2Source() {
    return std::make_unique<Freenect2FrameSource>();
}

bool checkKinectUsbPresent() {
#ifdef __APPLE__
    if (matchUsbClassForKinect("IOUSBHostDevice")) {
        return true;
    }
    return matchUsbClassForKinect(kIOUSBDeviceClassName);
#elif defined(_WIN32)
    return setupDiFindKinect();
#else
    return false;
#endif
}

std::string queryKinectUsbLocationString() {
#ifdef __APPLE__
    std::string loc;
    if (matchUsbClassForKinect("IOUSBHostDevice", &loc) && !loc.empty()) {
        return loc;
    }
    if (matchUsbClassForKinect(kIOUSBDeviceClassName, &loc) && !loc.empty()) {
        return loc;
    }
    return "USB Disconnected";
#elif defined(_WIN32)
    return setupDiFindKinect() ? "USB 3 · VID_045E&PID_02C4" : "USB Disconnected";
#else
    return "USB Disconnected";
#endif
}

} // namespace kv
