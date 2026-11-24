// Outside the home: the router mapping's own worker (protocol v3 §8.2, §8.3). PortMapper is
// driven from one thread of this class's own, with its own COM apartment (ComScope), never the UI thread or a
// connection's: each router call can take seconds. The host posts what it wants (open on a port, close,
// acknowledge an address change) and reads back a snapshot of the state; the worker tells it of every change
// through onChange. Nothing ever waits on the worker: shutdown() posts a best-effort removal and lets the
// thread go, so a router that never answers can't hold up the PC app's exit.
#pragma once

#include "router_mapper.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace kvhost::outside {

// What the host knows of the mapping right now.
struct MapperState {
    OutsideStatus status = OutsideStatus::Off;
    uint16_t port = 0;        // the port asked for (0 while off)
    std::string lanIPv4;      // the internal client the forward points at (empty if none)
    std::string wanAddress;   // as read from the router (empty if not read)
    std::string message;      // the PC's sentence for Manual, Error and CarrierNat (PortMapper's)
    bool checking = false;    // the worker is talking to the router (or about to) right now
    bool reported = false;    // a check has finished since the last open: status is a result, not a guess
    uint64_t generation = 0;  // bumped on every change, so the host can tell an address change from a repeat
    // Which open() this state answers (0 when none is in force: closed, or never opened). The host ignores a
    // late report for an earlier open, e.g. one for the old port after a port change (§8.7).
    uint64_t openId = 0;
    // A port this PC's forward may still be on: the one it holds, or one whose removal couldn't run yet (a stuck
    // router). 0 when nothing of this PC's is left. The host saves it as outsideMappedPort (§8.7).
    uint16_t heldPort = 0;
    // The PC user removed the forward ("Remove mapping now", or an
    // open told not to map). No check maps it until remapNow() or an open that maps.
    bool removed = false;
    // How many 10-minute checks the worker has run since it started, a skipped one (while removed) included. Tests
    // wait on it to know a check they asked for has been through the worker.
    uint64_t rechecks = 0;
    // v3 §8.3: the default gateway of the forward's interface, pinned when the PC user turned outside viewing on or
    // acknowledged an address change (the host saves it in pet-camera.json). A new public address is told to devices
    // at once when, in one pass, the gateway reads as pinned and its own answer is the address NATUPnP read. Empty:
    // none pinned.
    GatewayIdentity gatewayPin;
    // An address change is being checked: a part couldn't be read yet, so it is read again (no limit on how long).
    bool gatewayChecking = false;
    // How many passes in a row couldn't read a part (or NATUPnP), for the change being checked; 0 otherwise.
    uint32_t gatewayFailedPasses = 0;
    // Positive evidence that the new address didn't come from the pinned router (another gateway, or its own answer
    // differs): held until the PC user acknowledges it, or turns outside viewing off and on.
    bool gatewayHeld = false;
    // The pinned gateway's own public answer contradicts NATUPnP's address now: another device answered as the
    // router. Never acknowledged while it does (the re-share is withheld); checked again until the gateway agrees.
    bool gatewayContradicts = false;
    // The gateway's own public answer from the last pass that read one ("" if none), for the PC's panel.
    std::string gatewayAnswer;
    // The new address a pass CONFIRMED (the pinned gateway's own answer was NATUPnP's address) while a hold was in
    // force: the only address, with a gateway pinned, that the PC user may re-share and acknowledge. Cleared by a
    // contradiction, another gateway, or NATUPnP reading anything else. "" otherwise.
    std::string confirmedWan;
    // The pinned gateway's description URL that last gave answers (askGatewayForWan's lastGoodLocation), saved by the
    // host so a restart asks it straight over TCP before any search. "" when none has answered (or the gateway was
    // pinned anew since).
    std::string gatewayLocation;
};

class OutsideMapper {
public:
    struct Config {
        // The router. Empty: there is no router to use (a test run without a fake router), so every open
        // reports Manual with the manual steps, and nothing is ever called. The real NATUPnP session is passed
        // in only by HostController outside a test run (v3 §10 "Test isolation").
        RouterOpener opener;
        // This PC's LAN IPv4 for the forward; default internalClientIPv4 (no network traffic, just the
        // adapters). Tests pass a fixed private address.
        std::function<std::optional<std::string>()> lanAddress;
        uint64_t recheckEveryMs = 10ull * 60 * 1000; // §8.2: every 10 minutes while it is on
        PortMapper::Timing timing;                   // PortMapper's own (tests shorten the deadlines)
        AddressPolicy addressPolicy;                 // the product's; a test program's own (AddressPolicy)
        // v3 §8.3, the gateway of the interface that owns the forward's LAN address (readGatewayOn on a real
        // launch) and its own answer for the public address (askGatewayForWan). A test passes fakes, or its own
        // loopback gateway. Empty: the gateway can't be read, so nothing is pinned and an address change waits for the
        // PC user.
        std::function<GatewayIdentity(const std::string& lanIPv4)> readGateway;
        // lastGoodLocation: the description URL that last gave answers, asked first; set to the one that answers.
        std::function<std::vector<std::string>(const std::string& gatewayIp, const std::string& lanIPv4,
                                               std::string& lastGoodLocation)>
            askGatewayWan;
        GatewayIdentity pinned; // the pin pet-camera.json held at start (empty when none, or it was corrupt)
        std::string gatewayLocation; // the description URL pet-camera.json held at start, on the pinned gateway ("" if none)
        // The address devices were last told (pet-camera.json), so a restart checks a first address that differs
        // rather than tell it at once; kAnnouncedUnknown when the saved one was corrupt (any first address is checked).
        std::string announcedWan;
        // A pass that couldn't read a part (NATUPnP included) is tried again after these delays, the last one
        // repeating, for as long as it takes: an unreachable gateway is no evidence, and an address is told as soon as
        // one pass confirms it. A held change is checked at the same pace, so a contradiction that ends is seen.
        std::vector<uint64_t> gatewayRetryMs = {60'000};
        // A change checked for gatewaySlowAfterMs (held, or a gateway that still can't be read) is checked only every
        // gatewaySlowEveryMs from then on.
        uint64_t gatewaySlowAfterMs = 30ull * 60 * 1000;
        uint64_t gatewaySlowEveryMs = 5ull * 60 * 1000;
    };
    // Config::announcedWan when the saved address was corrupt: no address read ever equals it.
    static constexpr const char* kAnnouncedUnknown = "unknown";

    // onChange is called from the worker thread, with no lock of the mapper's held, after every change of the
    // state. It must not block for long (it takes the server's lock and returns).
    OutsideMapper(Config cfg, std::function<void(const MapperState&)> onChange);
    ~OutsideMapper(); // shutdown()
    OutsideMapper(const OutsideMapper&) = delete;
    OutsideMapper& operator=(const OutsideMapper&) = delete;

    // The outside listener opened on port: map it (§8.2 steps 1-4, §8.3), then re-check every 10 minutes.
    // Returns the open's id (MapperState::openId of the reports that answer it); 0 after shutdown. map false: the
    // PC user removed the forward and it stays removed (outsideMappingRemoved): no router call, the status is
    // manual with the steps, and the checks don't map it until remapNow().
    // pinAnew: the PC user turned outside viewing on, so the first address read is the one (nothing announced yet),
    // and the gateway is pinned again at the first mapped result.
    uint64_t open(uint16_t port, bool map = true, bool pinAnew = false);
    // Outside listening turned off: remove the mapping (best effort) and report Off.
    void close();
    // §8.3: the PC user saw "Your home address changed; re-share." with shownWan in the link. It applies only if
    // shownWan is still the address read now and, with a gateway pinned, only if a pass confirmed it (confirmedWan):
    // the status returns to Mapped, and the gateway is pinned again.
    void acknowledgeAddressChange(const std::string& shownWan);
    // The 10-minute check, now (tests; the PC's "check again").
    void recheckNow();
    // pet-camera.md §4 "Remove mapping now": the forward is removed while the listener stays open, and the status
    // is manual with the steps. It stays removed: the 10-minute check
    // doesn't map it again, and makes no router call for it (the public address is read from the mapping, so there
    // is none to read). Only remapNow(), or a close and an open that maps, maps it again.
    void removeNow();
    // "Map the port again": the forward the PC user removed is mapped now (§8.2 steps 1-4), and the checks keep it.
    void remapNow();
    // §8.7: a forward an earlier run left on port (its exit couldn't remove it). Removed best effort, only if it
    // is this PC's and carries our description; never someone else's. The attempt goes to the private debug
    // log only (OutputDebugString). Then the state's heldPort says 0, so the host clears outsideMappedPort.
    void removeLeftover(uint16_t port);

    MapperState state() const;

    // A clean exit: a best-effort removal is posted and the worker is let go. Never waits on the router; after
    // it returns onChange is never called again. Idempotent.
    void shutdown();

private:
    struct Shared;
    std::shared_ptr<Shared> sh_;
};

} // namespace kvhost::outside
