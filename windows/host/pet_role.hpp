// PetRole (protocol v3 §1 enum table): a device's role, decoded leniently. Any value other than
// "owner" or "viewer" becomes Unknown, so a newer peer's value never fails a whole message.
#pragma once

#include <string>

namespace kvhost {

enum class PetRole { Owner, Viewer, Unknown };

inline const char* petRoleString(PetRole r) {
    switch (r) {
        case PetRole::Owner: return "owner";
        case PetRole::Viewer: return "viewer";
        default: return "unknown";
    }
}

inline PetRole petRoleFromString(const std::string& s) {
    if (s == "owner") return PetRole::Owner;
    if (s == "viewer") return PetRole::Viewer;
    return PetRole::Unknown;
}

} // namespace kvhost
