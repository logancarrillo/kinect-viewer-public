// DNS-SD advertisement through the Windows built-in mDNS responder (DnsServiceRegister, windns.h).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace kvhost {

class MdnsAdvertisement {
public:
    MdnsAdvertisement();
    ~MdnsAdvertisement();
    MdnsAdvertisement(const MdnsAdvertisement&) = delete;
    MdnsAdvertisement& operator=(const MdnsAdvertisement&) = delete;

    // Registers "<instance>.<type>.local" (type like "_kv2s._tcp") on port. ipv4 is dotted-quad; empty
    // lets the responder use the host record only. Waits up to timeout_ms for the completion status.
    bool start(const std::string& instance, const std::string& type, uint16_t port, const std::string& ipv4,
               std::string* err, int timeout_ms = 3000);
    void stop();
    bool active() const;
    std::string fullName() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Best LAN IPv4 of this PC (interface of the default route), or "127.0.0.1".
std::string primaryLanIPv4();

} // namespace kvhost
