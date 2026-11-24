// W8 (outside the home), Phase A: the outside settings in a struct of their own (pet-camera.md §4, v3 §8.1).
// Phase B adds them to pet-camera.json as new keys after W7's, with no reordering and under the same
// fail-closed rules; until then nothing reads or writes them.
#pragma once

#include "crypto_identity.hpp"
#include "outside_address.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>

namespace kvhost::outside {

constexpr uint16_t kOutsidePortMin = 1024;
// The default is picked once in 20000-44999 and saved: below Windows' dynamic range (49152 up), and clear of most of
// the excluded port ranges Windows reserves just below it on some PCs, which refuse binding even on 127.0.0.1 with
// nothing listening. The host's probe still skips any port that doesn't bind (v3 §8.7).
constexpr uint16_t kOutsideDefaultPortLow = 20000;
constexpr uint16_t kOutsideDefaultPortHigh = 44999;
// v3 §8.7: how many random ports the host tries for the default, taking the first that binds.
constexpr int kOutsideDefaultPortTries = 20;
// v3 §8.7: the PC's status line while outside viewing is on and none of those ports bound (nothing is saved).
constexpr const char* kNoBindableOutsidePortText =
    "Couldn't find a port this PC can open for viewing from outside the home. Enter a port below.";

struct OutsideSettings {
    bool allowOutside = false;  // "Allow viewing from outside the home": off by default, PC only, needs sharing
    uint16_t port = 0;          // 0 until one is picked (pickDefaultOutsidePort)
    std::string publicAddress;  // entered by the PC user for the manual case; "entered by you, not checked"
};

inline bool isValidOutsidePort(uint32_t port) { return port >= kOutsidePortMin && port <= 65535; }

// A random port in 20000-44999, or 0 when no random bytes could be had.
inline uint16_t pickDefaultOutsidePort() {
    uint8_t b[4] = {};
    if (!kvhost::randomBytes(b, sizeof b)) return 0;
    const uint32_t r = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    return static_cast<uint16_t>(kOutsideDefaultPortLow + r % (kOutsideDefaultPortHigh - kOutsideDefaultPortLow + 1u));
}

// v3 §8.7: the default port, the first of up to kOutsideDefaultPortTries random ports in 20000-44999 that `binds`
// accepts, each asked about once. 0 when none did (or no random bytes could be had).
inline uint16_t pickBindableOutsidePort(const std::function<bool(uint16_t)>& binds) {
    uint16_t tried[kOutsideDefaultPortTries] = {};
    int n = 0;
    // A port drawn again is drawn over without asking; the bound on draws only guards a broken random source.
    for (int draws = 0; n < kOutsideDefaultPortTries && draws < 10 * kOutsideDefaultPortTries; ++draws) {
        const uint16_t p = pickDefaultOutsidePort();
        if (p == 0) return 0;
        if (std::find(tried, tried + n, p) != tried + n) continue;
        tried[n++] = p;
        if (binds(p)) return p;
    }
    return 0;
}

// A public address the PC user typed: a public IPv4, spaces around it ignored (a DNS name is a possible later option).
// The policy is the product's unless a test program passes its own (AddressPolicy).
inline bool isUsablePublicAddress(const std::string& text, AddressPolicy policy = {}) {
    return checkWanAddress(text, policy) == WanCheck::Public;
}

// The address as it is stored and shown: the spaces around it dropped.
inline std::string trimmedAddress(const std::string& text) {
    const char* const space = " \t\r\n";
    const size_t first = text.find_first_not_of(space);
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

} // namespace kvhost::outside
