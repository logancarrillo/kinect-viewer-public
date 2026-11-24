// The --test-pet-camera hook's fake v3 device (protocol v3, W6): a pet-camera client on this PC that pairs
// through the viewer's own pairing window and then streams over loopback, so the panel, the "Watching
// now" banner and the Devices dialog can be driven by a synthetic test. It is a test hook only: the
// viewer creates it only under --test-pet-camera (with --screenshot, so the synthetic source), and the
// host then listens on 127.0.0.1 only. It decodes nothing; it counts what arrives.
#pragma once

#include "crypto_identity.hpp"
#include "viewer_app.hpp" // TestDeviceStatus

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace kvhost {
class TlsConnection;
}

namespace kvwin {

class PetTestDevice {
public:
    PetTestDevice() = default;
    ~PetTestDevice() { stop(); }
    PetTestDevice(const PetTestDevice&) = delete;
    PetTestDevice& operator=(const PetTestDevice&) = delete;

    // Starts the device's thread: pairs with the window's QR token on pairPort (a v3 pairRequest with
    // this name), then connects to streamPort pinned to the host it paired with, pings with 3 every
    // 250 ms after hello, and records each 0x0B, 0x02, 0x03 and 0x0F.
    void start(const std::string& host, uint16_t pairPort, const kvhost::Bytes& qrToken, uint16_t streamPort,
               const std::string& name);
    void stop();
    TestDeviceStatus status() const;

private:
    void run(std::string host, uint16_t pairPort, kvhost::Bytes qrToken, uint16_t streamPort, std::string name);
    bool pair(const std::string& host, uint16_t port, const kvhost::Bytes& token, const std::string& name);
    void stream(const std::string& host, uint16_t port);
    void fail(const std::string& why);

    std::unique_ptr<kvhost::CryptoIdentity> identity_;
    kvhost::Bytes server_hash_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    mutable std::mutex m_;
    TestDeviceStatus st_;
    std::shared_ptr<kvhost::TlsConnection> conn_;
};

} // namespace kvwin
