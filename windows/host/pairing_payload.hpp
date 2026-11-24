// C++ port of shared/PairingPayload.swift: the kv2://pair QR URI, base64url and the 8-digit short code.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kvhost {

constexpr const char* kStreamServiceType = "_kv2s._tcp";
constexpr const char* kPairingServiceType = "_kv2p._tcp";

struct QRPairingPayload {
    std::string serviceId;
    std::vector<uint8_t> serverCertSHA256;
    std::vector<uint8_t> oneTimeToken;
    std::string hostIP;
    uint16_t pairPort = 0;

    std::string toURIString() const;
    // Same acceptance rules as the Swift parser (fp 32 bytes, token >= 16 bytes, port > 0, ...).
    static bool parse(const std::string& uri, QRPairingPayload& out);
};

std::string base64URL(const std::vector<uint8_t>& data);
bool fromBase64URL(const std::string& s, std::vector<uint8_t>& out);

std::string normalizeShortCode(const std::string& code); // keeps ASCII digits only
// Swift: 4 random bytes big-endian % 100_000_000, "%08u", formatted "dddd-dddd".
void shortCodeFromBytes(const uint8_t bytes4[4], std::string& formatted, std::string& normalized);
bool generateShortCode(std::string& formatted, std::string& normalized);

std::string randomHexId(size_t bytes); // HostStreamServer.randomHexId(bytes: 6) -> 12 lowercase hex chars

} // namespace kvhost
