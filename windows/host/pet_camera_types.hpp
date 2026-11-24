// What the PC app sees of the pet camera host (protocol v3, pet-camera.md §4): plain structs with no
// Windows or network headers, so the viewer's StreamHooks interface can carry them.
#pragma once

#include "pet_role.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace kvhost {

// One row of the PC's device list.
struct HostDeviceInfo {
    std::string deviceId; // 16 hex characters
    std::string name;
    PetRole role = PetRole::Viewer;
    std::string kind; // "iphone", "ipad", "mac", "other"
    uint64_t pairedAtUnixMs = 0;
    std::optional<uint64_t> lastSeenUnixMs;
    std::optional<int> lastProtocolVersion;
    bool connected = false;  // a session of this device is open
    std::string path;        // "home" while connected ("outside" over the outside listener), empty otherwise
    int sessionVersion = 0;  // the open session's protocol version (0 when not connected)
    bool watching = false;   // receiving live video now
    bool available = false;  // v3 §2.3 conditions 1, 2, 3 and 5 hold for the open session
    std::string unavailableReason; // live_unavailable::* when connected and not available
};

// One live tier, for the PC's diagnostics (v3 §6.1: "The PC's diagnostics show each tier's encoder name").
struct HostTierStatus {
    std::string tier; // "high", "medium", "low"
    bool active = false;  // taking frames (wanted, or inside its 60 s hold)
    bool open = false;    // an encoder is open
    std::string encoderName;
    bool hardware = false;
    uint32_t width = 0, height = 0;
    uint32_t epoch = 0;
    uint64_t framesOut = 0;
    uint64_t forcedKeyframes = 0;
    int sessions = 0; // sessions watching this tier now
};

} // namespace kvhost
