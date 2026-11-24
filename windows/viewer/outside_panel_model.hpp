// W8, outside the home, on the PC (pet-camera.md §4, the outside rows; protocol v3 §8): what the Pet camera
// panel's "Outside the home" section and its windows show, as pure functions over plain structs, so the unit
// tests check every status's text and every control's state without a window or a host.
#pragma once

#include "connection_log.hpp" // kvhost::outside::LogLine (header only here)
#include "pet_role.hpp"
#include "router_mapper.hpp" // kvhost::outside::OutsideStatus (header only here)

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kvwin {

// The host's kvhost::OutsideView (HostController::outsideView()), field for field, so StreamHooks and this model
// don't need the host's header. The host fills it cheaply and never waits on the router for it.
struct OutsidePanelView {
    bool allowed = false;   // the setting (allowOutside)
    bool listening = false; // the outside listener is open
    kvhost::outside::OutsideStatus status = kvhost::outside::OutsideStatus::Off;
    uint16_t port = 0;
    std::string lanIPv4;    // the internal client (empty if none)
    std::string wanAddress; // as read from the router (empty if not read)
    bool endpointKnown = false; // an outsideEndpoint exists (§8.3)
    std::string endpointHost;
    bool endpointVerified = false; // false: "entered by you, not checked"
    std::string publicAddressEntered;
    std::string sentence;    // the §8.2/§8.3 sentence for the status, in plain words
    std::string manualSteps; // "Forward TCP <port> to <LAN IPv4> in your router's settings" when manual
    bool checking = false;   // the host's router worker is on it now
    bool canInvite = false;  // §8.4: listening and an outsideEndpoint exists
    std::string cannotInviteReason;
    // Why the last invite ended, as the host knows it ("used", "expired", "cancelled", "burned", "not_saved",
    // "address_changed", "address_gone", "port_changed", "outside_off"; "" when it is open or unknown).
    std::string inviteEnded;
    // The PC user removed the forward ("Remove mapping now") and it stays removed (kvhost::OutsideView).
    bool mappingRemoved = false;
    // v3 §8.3: what the pinned gateway is known by ("address and hardware address"), or "" when none is pinned;
    // gatewayChecking: a new address is being checked with it; gatewayHeld: it didn't confirm the new address.
    std::string gatewayPinKind;
    bool gatewayChecking = false;
    bool gatewayHeld = false;
    // The pinned gateway contradicts the new address (another device answered as the router); gatewayAnswer is the
    // gateway's own public answer; gatewayFailedPasses counts checks in a row that couldn't read the router.
    bool gatewayContradicts = false;
    std::string gatewayAnswer;
    uint32_t gatewayFailedPasses = 0;
    // A changed address the host won't let the PC user re-share yet: a gateway is pinned and no check has confirmed
    // the address (kvhost::OutsideView::reshareOffered is false).
    bool reshareWithheld = false;
};

// kvhost::OutsideInviteView: the open invite (§8.4). uri holds the live token.
struct OutsideInvitePanelView {
    std::string uri;
    uint64_t expiresUnix = 0;
    kvhost::PetRole role = kvhost::PetRole::Viewer;
};

// What the panel's section gets from the host, besides the view.
struct OutsidePanelInput {
    OutsidePanelView view;
    bool hostRunning = false;  // the streaming host runs (its identity can be used)
    bool hostHasOutside = false; // this build's host has outside listening at all
    bool sharingOn = false;    // "Share the camera" (§8.1: outside viewing needs it)
    std::string saveError;     // pet-camera.json can't be read or saved: changes are refused (fail closed)
    // pet-camera.md §7: whether Windows treats a connected network as Public (INetworkListManager, read on the viewer
    // side); nullopt when it couldn't be read. A test hands the panel a made-up value.
    std::optional<bool> networkPublic;
};

// Every control in the section. Off disables everything but the Allow switch itself and "Connection log...": the
// log covers the home listeners too (§8.6), so it isn't an outside-only control.
struct OutsidePanelState {
    bool allowChecked = false;
    bool allowEnabled = false;
    std::wstring allowReason; // why the switch can't be changed now; empty when it can
    std::wstring statusLine;  // the host's sentence, "Checking…" while it checks, or "Off: ..."
    std::wstring portText;    // "Port 43210 (TCP)"
    bool portEnabled = false; // Change port...
    std::wstring endpointText; // "Outside address: 203.0.113.160:43210", "(entered by you, not checked)" when unverified
    bool showManualSteps = false;
    std::wstring manualSteps;
    bool showPublicAddress = false; // the manual case's "Public address" field
    bool publicAddressEnabled = false;
    std::wstring publicAddressValue;
    bool showAddressChanged = false; // "Your home address changed; re-share." and Re-share address...
    bool reshareEnabled = false;
    bool inviteEnabled = false;
    std::wstring inviteReason; // why Invite from outside... is disabled; empty when it isn't
    bool logEnabled = false;
    // pet-camera.md §4 "Remove mapping now": while this PC's forward is on the router (mapped or address_changed).
    bool removeMappingEnabled = false;
    // "Map the port again": while outside viewing is on and the PC user
    // removed the forward. "Remove mapping now" is disabled meanwhile.
    bool remapEnabled = false;
    // pet-camera.md §7: the Public network-profile hint, while outside viewing is on; empty otherwise.
    std::wstring profileHint;
    // v3 §8.3: the router this PC's forward is on, as pinned, and what that means for a new home address; empty
    // unless the router forwards the port (mapped, or an address change waiting).
    std::wstring routerText;
};

constexpr const wchar_t* kOutsidePublicAddressLabel = L"Public address (entered by you, not checked)";
constexpr const wchar_t* kOutsideAddressChangedText = L"Your home address changed; re-share.";
constexpr const wchar_t* kOutsideCheckingText = L"Checking…";
// v3 §8.3, the router line.
constexpr const wchar_t* kOutsideRouterNotPinned =
    L"Router: not recognised yet, so a new home address waits for you to re-share it.";
constexpr const wchar_t* kOutsideRouterChecking =
    L"Checking the router… The new home address is shared once the router confirms it.";
// After this many checks in a row that couldn't read the router (about half an hour), the router line says the PC user
// can re-share the new address themselves.
constexpr uint32_t kOutsideLongCheckPasses = 30;
constexpr const wchar_t* kOutsideRouterCheckingLong =
    L"Checking the router… It hasn't confirmed the new home address for a while. To start afresh with the router you "
    L"have now, turn viewing from outside the home off and on.";
// v3 §8.3: a changed address the router hasn't confirmed: devices keep the old one, and it can't be re-shared.
constexpr const wchar_t* kOutsideUnconfirmedText =
    L"Your home address changed. Devices keep the old one until the router confirms the new one.";
// v3 §8.3: the pinned gateway contradicts the router's address. The status line, and the reason invites wait.
constexpr const wchar_t* kOutsideContradictedText =
    L"Another device on your network answered as the router. Your camera kept its old address.";
constexpr const wchar_t* kOutsideRouterOther =
    L"Router: the new address came through a router this PC doesn't recognise. If you replaced your router, turn "
    L"viewing from outside the home off and on to start afresh with it.";
// The port label while the panel shows the no-port error (v3 §8.7).
constexpr const wchar_t* kOutsideNoPortLabel = L"Port: none could be opened, so enter one below";
constexpr const wchar_t* kOutsideInviteWarning = L"Send this only to the person you're inviting.";
constexpr int kConnectionLogDays = 30; // §8.6: the PC shows the last 30 days
constexpr const wchar_t* kOutsidePublicNetworkHint =
    L"Windows treats this network as public, so its firewall may block devices outside the home. To allow them, set "
    L"this network to Private in Windows Settings (Network & internet).";

OutsidePanelState outsidePanelState(const OutsidePanelInput& in);

// The status line alone (part of outsidePanelState): "Checking…" while the host checks, else its sentence, else
// a plain one for the status.
std::wstring outsideStatusLine(const OutsidePanelView& v);
// "Forward TCP 43210 to 192.168.1.20 in your router's settings." (the host's text when it gave one).
std::wstring outsideManualSteps(const OutsidePanelView& v);
// The router line (part of outsidePanelState; `on`: outside viewing is on with the host running).
std::wstring outsideRouterText(const OutsidePanelView& v, bool on);

// The port a "Change port..." prompt typed: 1024-65535 (v3 §8.1), or nullopt.
std::optional<uint16_t> parseOutsidePort(const std::wstring& text);

// The invite window's countdown: "Expires in 9:41", "Expired".
std::wstring inviteCountdownText(uint64_t expires_unix, uint64_t now_unix);
// "Inviting as a viewer." / "Inviting as an owner."
std::wstring inviteRoleText(kvhost::PetRole role);
// The invite window's last words when it closes itself, from the host's reason (OutsidePanelView::inviteEnded): "The
// invite was used.", "The invite ran out.", ... and a plain one when the host gave none. Never "used" unless it was.
std::wstring inviteClosedText(const std::string& ended);
// The registered clipboard formats a copied invite link also carries, each as a DWORD 0, so clipboard history,
// cloud clipboard and clipboard monitors leave the live token alone.
const std::vector<std::wstring>& privateClipboardFormats();
// A kv2://invite link (§8.4) with every key the format needs, and a tok.
bool looksLikeInviteLink(const std::string& uri);
// A kv2://endpoint link (§8.4): the endpoint scheme and path, h and p, and no secret (no tok).
bool looksLikeEndpointLink(const std::string& uri);
// The invite link with the token's value replaced: what a test run shows and draws (v3 §10: test hooks never
// show a live token). The rest of the link is unchanged.
std::string maskInviteToken(const std::string& uri);

// ---- the connection log window (§8.6) --------------------------------------------------------------------------
// "Connected", "Refused: unknown certificate", ... (the event name itself for one this build doesn't know).
std::wstring connectionLogEventLabel(const std::string& event);
// The lines the window lists: the last kConnectionLogDays days before now_unix_ms, newest first.
std::vector<kvhost::outside::LogLine> connectionLogRows(std::vector<kvhost::outside::LogLine> lines, uint64_t now_unix_ms);
// "iPhone (a1b2c3)", "a1b2c3", or "" for a line without a device.
std::wstring connectionLogDeviceText(const kvhost::outside::LogLine& l);

} // namespace kvwin
