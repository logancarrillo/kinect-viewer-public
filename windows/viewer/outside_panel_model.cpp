#include "outside_panel_model.hpp"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>

namespace kvwin {

namespace {

using kvhost::outside::OutsideStatus;

std::wstring widenUtf8(const std::string& s) {
    // UTF-8 to UTF-16 without Windows headers (the host's sentences and names are UTF-8).
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
        i += len;
    }
    return out;
}

std::wstring hostPort(const std::string& host, uint16_t port) {
    return widenUtf8(host) + L":" + std::to_wstring(port);
}

// The query of a kv2:// link as key -> value, or nullopt when it doesn't start with prefix. §8.4: parsers read
// keys by name and ignore unknown ones.
std::optional<std::map<std::string, std::string>> linkQuery(const std::string& uri, const std::string& prefix) {
    if (uri.rfind(prefix, 0) != 0) return std::nullopt;
    std::map<std::string, std::string> q;
    size_t at = prefix.size();
    while (at <= uri.size()) {
        size_t end = uri.find('&', at);
        if (end == std::string::npos) end = uri.size();
        const std::string part = uri.substr(at, end - at);
        const size_t eq = part.find('=');
        if (eq != std::string::npos) q[part.substr(0, eq)] = part.substr(eq + 1);
        else if (!part.empty()) q[part] = std::string();
        at = end + 1;
    }
    return q;
}

bool hasValues(const std::map<std::string, std::string>& q, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const auto it = q.find(k);
        if (it == q.end() || it->second.empty()) return false;
    }
    return true;
}

} // namespace

std::wstring outsideStatusLine(const OutsidePanelView& v) {
    if (v.checking) return kOutsideCheckingText;
    // v3 §8.3: another device answered as the router; this outranks any sentence of the host's.
    if (v.status == OutsideStatus::AddressChanged && v.gatewayContradicts) return kOutsideContradictedText;
    if (v.status == OutsideStatus::AddressChanged && v.reshareWithheld) return kOutsideUnconfirmedText;
    if (!v.sentence.empty()) return widenUtf8(v.sentence); // the host's §8.2/§8.3 sentence, the carrier-NAT text included
    switch (v.status) {
        case OutsideStatus::Off: return L"Off: devices connect only at home.";
        case OutsideStatus::Mapped:
            return v.endpointKnown ? L"Devices outside the home can connect at " + hostPort(v.endpointHost, v.port) + L"."
                                   : std::wstring(L"The router forwards the port.");
        case OutsideStatus::Manual:
            return L"The router couldn't be set up automatically (UPnP), so the port has to be forwarded by hand.";
        case OutsideStatus::Error: return L"Viewing from outside the home isn't working.";
        case OutsideStatus::CarrierNat:
            return L"Your internet provider is using carrier NAT, so devices outside your home can't reach this PC.";
        case OutsideStatus::AddressChanged: return kOutsideAddressChangedText;
    }
    return std::wstring();
}

std::wstring outsideManualSteps(const OutsidePanelView& v) {
    if (!v.manualSteps.empty()) return widenUtf8(v.manualSteps);
    // §8.2: plain steps for a manual port forward.
    return L"Forward TCP " + std::to_wstring(v.port) + L" to " + (v.lanIPv4.empty() ? std::wstring(L"this PC") : widenUtf8(v.lanIPv4)) +
           L" in your router's settings.";
}

OutsidePanelState outsidePanelState(const OutsidePanelInput& in) {
    const OutsidePanelView& v = in.view;
    OutsidePanelState st;
    const bool host = in.hostHasOutside && in.hostRunning;
    st.allowChecked = host && v.allowed;
    // §8.1: off by default, turned on on the PC only, and only with sharing on. A pet-camera.json that can't be
    // read or saved refuses every change (fail closed), so the switch says why rather than flip and fall back.
    if (!in.hostHasOutside) st.allowReason = L"This build's streaming host can't serve devices outside the home.";
    else if (!in.hostRunning) st.allowReason = L"The streaming host is off.";
    else if (!in.saveError.empty()) st.allowReason = L"Can't be changed until the settings can be saved: " + widenUtf8(in.saveError);
    else if (!in.sharingOn && !v.allowed)
        st.allowReason = L"Turn on “Share the camera” first: viewing from outside the home needs it.";
    st.allowEnabled = st.allowReason.empty();

    const bool on = host && v.allowed;
    const bool can_change = on && in.saveError.empty();
    st.statusLine = on ? outsideStatusLine(v) : std::wstring(L"Off: devices connect only at home.");
    if (v.port != 0) st.portText = L"Port " + std::to_wstring(v.port) + L" (TCP)";
    // Turned on with no port at all is the no-port error (none of
    // the default's tries bound, v3 §8.7; every other error has a port), so the label says what to do.
    else if (on && v.status == OutsideStatus::Error) st.portText = kOutsideNoPortLabel;
    else st.portText = L"Port: picked when it's turned on";
    st.portEnabled = can_change;
    if (on && v.endpointKnown) {
        st.endpointText = L"Outside address: " + hostPort(v.endpointHost, v.port);
        if (!v.endpointVerified) st.endpointText += L" (entered by you, not checked)"; // §8.3
    }
    // §8.2/§8.3, the manual case: the steps, and the address the PC user may type in.
    st.showManualSteps = on && v.status == OutsideStatus::Manual;
    if (st.showManualSteps) st.manualSteps = outsideManualSteps(v);
    st.showPublicAddress = st.showManualSteps;
    st.publicAddressEnabled = st.showPublicAddress && can_change;
    st.publicAddressValue = widenUtf8(v.publicAddressEntered);
    // §8.3: an address change the pinned gateway confirms is shared without the PC user; one it doesn't (or with no
    // gateway pinned) waits for the PC user, who re-shares the endpoint link.
    // Only an address the host lets the PC user re-share: never one the pinned gateway contradicts (re-sharing it
    // would tell devices another device's), nor, with a gateway pinned, one no check has confirmed.
    st.showAddressChanged = on && v.status == OutsideStatus::AddressChanged && !v.gatewayContradicts && !v.reshareWithheld;
    st.reshareEnabled = st.showAddressChanged;
    // §8.4: invites only while listening with an outsideEndpoint; otherwise disabled with the reason.
    // §8.3: while an address change waits, Re-share address... comes first (it acknowledges the new address).
    const bool contradicted = on && v.status == OutsideStatus::AddressChanged && v.gatewayContradicts;
    const bool unconfirmed = on && v.status == OutsideStatus::AddressChanged && v.reshareWithheld;
    st.inviteEnabled = on && v.listening && v.canInvite && !v.checking && !st.showAddressChanged && !contradicted && !unconfirmed;
    if (!st.inviteEnabled) {
        if (!on) st.inviteReason = L"Turn on viewing from outside the home first.";
        else if (contradicted) st.inviteReason = kOutsideContradictedText;
        else if (unconfirmed) st.inviteReason = kOutsideUnconfirmedText;
        else if (v.checking) st.inviteReason = kOutsideCheckingText;
        else if (st.showAddressChanged) st.inviteReason = L"Your home address changed. Re-share it first, then make the invite.";
        else if (!v.cannotInviteReason.empty()) st.inviteReason = widenUtf8(v.cannotInviteReason);
        else if (!v.listening) st.inviteReason = L"The outside listener isn't open.";
        else st.inviteReason = L"No outside address is known yet.";
    }
    // The log covers the home listeners too (§8.6), so it stays open to the PC user while outside viewing is off.
    st.logEnabled = host;
    // pet-camera.md §4: the forward can be removed while this PC holds one; the listener stays as it is.
    st.removeMappingEnabled = can_change && v.listening && !v.checking && !v.mappingRemoved &&
                              (v.status == OutsideStatus::Mapped || v.status == OutsideStatus::AddressChanged);
    // A removed forward stays removed until the PC user maps it again (or turns outside viewing off and on).
    st.remapEnabled = can_change && v.listening && !v.checking && v.mappingRemoved;
    // pet-camera.md §7: a Public profile's firewall may block the outside listener. A hint only; never changed here.
    if (on && in.networkPublic.value_or(false)) st.profileHint = kOutsidePublicNetworkHint;
    st.routerText = outsideRouterText(v, on);
    return st;
}

std::wstring outsideRouterText(const OutsidePanelView& v, bool on) {
    if (!on || !v.listening || v.mappingRemoved ||
        (v.status != OutsideStatus::Mapped && v.status != OutsideStatus::AddressChanged))
        return std::wstring();
    if (v.gatewayPinKind.empty()) return kOutsideRouterNotPinned;
    if (v.status == OutsideStatus::AddressChanged) {
        if (v.gatewayContradicts) {
            const std::wstring answer = v.gatewayAnswer.empty() ? std::wstring(L"another address") : widenUtf8(v.gatewayAnswer);
            return L"The router itself reports " + answer + L". Re-share is offered once the router agrees.";
        }
        if (v.gatewayHeld) return kOutsideRouterOther;
        return v.gatewayFailedPasses >= kOutsideLongCheckPasses ? kOutsideRouterCheckingLong : kOutsideRouterChecking;
    }
    return L"Router recognised by its " + widenUtf8(v.gatewayPinKind) + L". A new home address through it is shared by itself.";
}

std::optional<uint16_t> parseOutsidePort(const std::wstring& text) {
    size_t a = 0, b = text.size();
    while (a < b && std::iswspace(text[a])) ++a;
    while (b > a && std::iswspace(text[b - 1])) --b;
    if (a == b || b - a > 5) return std::nullopt;
    uint32_t v = 0;
    for (size_t i = a; i < b; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return std::nullopt;
        v = v * 10 + static_cast<uint32_t>(text[i] - L'0');
    }
    if (v < 1024 || v > 65535) return std::nullopt; // v3 §8.1
    return static_cast<uint16_t>(v);
}

std::wstring inviteCountdownText(uint64_t expires_unix, uint64_t now_unix) {
    if (now_unix >= expires_unix) return L"Expired";
    const uint64_t left = expires_unix - now_unix;
    wchar_t b[48];
    swprintf(b, 48, L"Expires in %llu:%02llu", static_cast<unsigned long long>(left / 60),
             static_cast<unsigned long long>(left % 60));
    return b;
}

std::wstring inviteRoleText(kvhost::PetRole role) {
    return role == kvhost::PetRole::Owner ? L"Inviting as an owner." : L"Inviting as a viewer.";
}

std::wstring inviteClosedText(const std::string& ended) {
    if (ended == "used") return L"The invite was used: the device was added.";
    if (ended == "expired") return L"The invite ran out.";
    if (ended == "cancelled") return L"The invite was cancelled.";
    if (ended == "burned") return L"The invite was closed after a wrong code. Make a new one.";
    if (ended == "not_saved") return L"The invite was closed: the PC couldn't save its use.";
    if (ended == "address_changed") return L"The invite was closed: your home address changed. Make a new one.";
    if (ended == "address_gone") return L"The invite was closed: the PC no longer knows your home address.";
    if (ended == "port_changed") return L"The invite was closed: the outside port changed. Make a new one.";
    if (ended == "outside_off") return L"The invite was closed: viewing from outside the home was turned off.";
    return L"The invite was closed.";
}

const std::vector<std::wstring>& privateClipboardFormats() {
    static const std::vector<std::wstring> formats = {L"ExcludeClipboardContentFromMonitorProcessing",
                                                      L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"};
    return formats;
}

bool looksLikeInviteLink(const std::string& uri) {
    const auto q = linkQuery(uri, "kv2://invite?");
    return q && hasValues(*q, {"v", "s", "fp", "tok", "h", "p", "lh", "exp", "r"});
}

bool looksLikeEndpointLink(const std::string& uri) {
    const auto q = linkQuery(uri, "kv2://endpoint?");
    return q && hasValues(*q, {"v", "s", "fp", "h", "p"}) && q->find("tok") == q->end();
}

std::string maskInviteToken(const std::string& uri) {
    std::string out = uri;
    const size_t q = out.find('?');
    if (q == std::string::npos) return out;
    for (size_t at = q + 1; at < out.size();) {
        size_t end = out.find('&', at);
        if (end == std::string::npos) end = out.size();
        if (out.compare(at, 4, "tok=") == 0) {
            for (size_t i = at + 4; i < end; ++i) out[i] = '*';
        }
        at = end + 1;
    }
    return out;
}

std::wstring connectionLogEventLabel(const std::string& event) {
    static const std::map<std::string, const wchar_t*> labels = {
        {"connected", L"Connected"},
        {"disconnected", L"Disconnected"},
        {"refused_unknown_cert", L"Refused: unknown certificate"},
        {"refused_rate_limit", L"Refused: too many attempts"},
        {"refused_blocked", L"Refused: blocked after failures"},
        {"refused_local_only", L"Refused: home network only"},
        {"refused_too_many", L"Refused: too many connections"},
        {"refused_unavailable", L"Refused: unavailable"},
        {"paired", L"Paired"},
        {"invite_redeemed", L"Invite used"},
        {"invite_failed", L"Invite attempt failed"},
        {"invite_burned", L"Invite burned"},
        {"revoked", L"Revoked"},
        {"role_changed", L"Role changed"},
        {"invite_closed", L"Invite closed"},
        {"pairing_failed", L"Pairing attempt failed"},
        {"pairing_burned", L"Pairing window burned"},
        {"suppressed", L"More attempts, not listed one by one"},
    };
    const auto it = labels.find(event);
    return it == labels.end() ? widenUtf8(event) : std::wstring(it->second);
}

std::vector<kvhost::outside::LogLine> connectionLogRows(std::vector<kvhost::outside::LogLine> lines, uint64_t now_unix_ms) {
    const uint64_t keep = static_cast<uint64_t>(kConnectionLogDays) * 24 * 60 * 60 * 1000;
    const uint64_t from = now_unix_ms > keep ? now_unix_ms - keep : 0;
    lines.erase(std::remove_if(lines.begin(), lines.end(), [from](const kvhost::outside::LogLine& l) { return l.t < from; }),
                lines.end());
    std::stable_sort(lines.begin(), lines.end(),
                     [](const kvhost::outside::LogLine& a, const kvhost::outside::LogLine& b) { return a.t > b.t; });
    return lines;
}

std::wstring connectionLogDeviceText(const kvhost::outside::LogLine& l) {
    if (!l.deviceName.empty()) return widenUtf8(l.deviceName);
    return widenUtf8(l.deviceId);
}

} // namespace kvwin
