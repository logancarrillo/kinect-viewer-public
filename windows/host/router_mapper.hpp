// Outside the home: the router port mapping (protocol v3 §8.2) and the public address it reads
// (§8.3). The mapping logic runs against a RouterSession seam; the real one is NATUPnP (natupnp.h, COM),
// opened by openNatUpnpSession on a thread that has COM initialised (the outside mapper's own UPnP thread, never the UI
// thread: each call can take seconds). Tests use a fake router and never reach NATUPnP.
#pragma once

#include "outside_address.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kvhost::outside {

constexpr const char* kMappingDescription = "Kinect viewer pet camera";

// One TCP port mapping as the router reports it.
struct RouterMapping {
    std::string internalClient;
    uint16_t internalPort = 0;
    bool enabled = false;
    std::string externalAddress; // the router's WAN address (IStaticPortMapping::get_ExternalIPAddress)
    std::string description;     // IStaticPortMapping::get_Description (ours: kMappingDescription)
};

// v3 §8.3, pin the gateway and ask it directly. The default gateway of the interface that owns the forward's LAN
// address: its IPv4 and its hardware address ("aa-bb-cc-dd-ee-ff", lower case). An empty part couldn't be read.
struct GatewayIdentity {
    std::string ip, mac;
    bool operator==(const GatewayIdentity&) const = default;
    bool complete() const { return !ip.empty() && !mac.empty(); }
};
// One verification pass, every part read in that same pass: the gateway of the forward's interface, the public
// address NATUPnP reads now, and the pinned gateway's own public answers (GetExternalIPAddress from each of its WAN
// connection services; only public addresses are kept, so 0.0.0.0 or a private one counts as unread). "" or none for
// a part that couldn't be read.
struct GatewayReading {
    GatewayIdentity gateway;
    std::string reportedWan;
    std::vector<std::string> gatewayWans;
};
// How a pass compares with the pin:
//   NoPin:        nothing pinned (none was read, or the saved pin was corrupt): the change waits for the PC user;
//   OtherGateway: the gateway's address or hardware address differs from the pin (another device, or a new
//                 router): held until the PC user acknowledges;
//   Contradicted: the pinned gateway answers, publicly, and none of its answers is what NATUPnP reported: another
//                 device answered as the router. Held, and never acknowledged while the gateway contradicts it;
//   Unreadable:   a part couldn't be read now (no neighbour entry yet, the gateway's UPnP didn't answer or gave no
//                 public address, NATUPnP read nothing): not evidence of anything, so it is read again later;
//   Match:        one of the pinned gateway's own answers is the address NATUPnP read.
enum class GatewayVerdict { NoPin, OtherGateway, Contradicted, Unreadable, Match };
GatewayVerdict judgeGateway(const GatewayIdentity& pin, const GatewayReading& now);
// "address and hardware address" for a complete pin, "" for none (the PC's panel).
const char* gatewayPinKind(const GatewayIdentity& pin);
// A pin part as saved: an IPv4 in dotted form; a hardware address "aa-bb-cc-dd-ee-ff" in lower case.
bool validGatewayIp(const std::string& s);
bool validGatewayMac(const std::string& s);

// One of the gateway's own UPnP URLs (cracked with WinHttpCrackUrl): http only, the host an IPv4 literal equal to
// gatewayIp in dotted form, and no user name or password. nullopt for anything else: another host, a name, IPv6,
// https, user-info, spaces or control characters.
struct GatewayUrl {
    std::string host;
    uint16_t port = 80;
    std::string path; // with the query, "/" at least
};
std::optional<GatewayUrl> crackGatewayUrl(const std::string& url, const std::string& gatewayIp);
// An SSDP search reply's LOCATION ("" when it isn't a 200 reply or has none).
std::string ssdpLocation(const std::string& reply);
// Any header of an SSDP search reply, by its lower-case name ("" when it isn't a 200 reply or has none).
std::string ssdpHeader(const std::string& reply, const std::string& lowerName);
// The LOCATION of a reply that counts: one sent from gatewayIp, whose LOCATION is on gatewayIp too. "" otherwise,
// so a device at any other address (or pointing anywhere else) has no say.
std::string gatewayLocationFrom(const std::string& sourceIp, const std::string& reply, const std::string& gatewayIp);
// The description's WAN connection services (WANIPConnection:1 and :2, WANPPPConnection:1), their control URLs
// resolved against URLBase or the description's own URL; only those on the gateway itself (crackGatewayUrl).
struct GatewayService {
    std::string type;
    GatewayUrl control;
};
std::vector<GatewayService> gatewayServices(const std::string& descriptionXml, const std::string& location,
                                            const std::string& gatewayIp);
// GetExternalIPAddress's answer: NewExternalIPAddress when it is an IPv4 in dotted form inside a SOAP envelope's
// GetExternalIPAddressResponse in a WAN connection service's namespace (WANIPConnection or WANPPPConnection, any
// version); "" for anything else.
std::string soapExternalAddress(const std::string& responseXml);

// Each step of the gateway read as a line, for the read-only router probe (kinect-cli router-probe); the host passes
// none. A line can hold addresses: the probe masks them (maskProbeLine) before anything is printed.
using GatewayTrace = std::function<void(const std::string&)>;

// The real reads, one step at a time on the calling thread (the mapper's worker): nothing runs beside them.
// readGatewayOn: the default gateway of the interface that owns lanIPv4 (GetBestRoute2 on that interface; nothing
// is sent) and its hardware address (ResolveIpNetEntry2: at most one ARP request). Never called by a test.
GatewayIdentity readGatewayOn(const std::string& lanIPv4);
// The pinned gateway's own answers for the public address (dotted IPv4s, one per WAN connection service that
// answered; none if it can't be had within deadlineMs). First the description at lastGoodLocation, straight over TCP
// to the gateway (no SSDP reply, forged or not, is involved); then an SSDP search for the gateway device on the
// interface that owns lanIPv4, where only a reply from gatewayIp whose LOCATION is on it counts, every distinct
// location tried until the deadline. Each description, then GetExternalIPAddress over SOAP to each WAN connection
// service. A location that gave answers is kept in lastGoodLocation. The caller keeps only public answers. A test
// calls it only with a loopback gateway whose description is already known (no search is reached).
// The description at lastGoodLocation gets at most kLastGoodTryMs of the deadline, so an old port that has gone dead
// leaves the rest to the search.
constexpr uint32_t kLastGoodTryMs = 3'000;
std::vector<std::string> askGatewayForWan(const std::string& gatewayIp, const std::string& lanIPv4, uint32_t deadlineMs,
                                          std::string& lastGoodLocation, const GatewayTrace& trace = {});
// The same from a known description URL, with no search: what a test drives against its own loopback gateway.
std::vector<std::string> askGatewayForWanAt(const std::string& gatewayIp, const std::string& location, uint32_t deadlineMs,
                                            const GatewayTrace& trace = {});

// The router probe's survey (never the host, never a test): one SSDP M-SEARCH for st, sent from lanIPv4 to the group
// and to gatewayIp, and every reply until deadlineMs, with its source and the ST it answered as. A search changes
// nothing anywhere. Empty when nothing answered (or nothing could be sent).
struct SsdpSeen {
    std::string source, st;
};
std::vector<SsdpSeen> ssdpSurvey(const std::string& lanIPv4, const std::string& gatewayIp, const std::string& st,
                                 uint32_t deadlineMs);

// A probe line with every address in it masked, so that its output can be posted: an IPv4 named in names becomes its
// label in brackets; any other gets a numbered label by its kind (checkWanAddress), added to names so the same
// address keeps its label on every line ("[public address 1]", "[non-public address 1]"), except 0.0.0.0, which says
// nothing about anyone and is kept; any hardware address becomes "[hardware address]", any UUID (a device's own, as in
// a description URL) "[uuid]", and any other run of 8 or more hex digits (a device id or serial in a path, a hardware
// address written without separators) "[id]". What is left, such as service types and URL paths, may still reveal the
// router's make or model.
std::string maskProbeLine(const std::string& line, std::vector<std::pair<std::string, std::string>>& names,
                          AddressPolicy policy = {});

// One look at the router: a fresh IUPnPNAT and its static port mapping collection. §8.2: the objects are
// snapshots, so every check opens a new session.
class RouterSession {
public:
    virtual ~RouterSession() = default;
    // The TCP mapping for an external port, or nullopt when there is none (or it can't be read).
    virtual std::optional<RouterMapping> find(uint16_t port) = 0;
    // Add(port, "TCP", port, internalClient, TRUE, description).
    virtual bool add(uint16_t port, const std::string& internalClient, const std::string& description) = 0;
    // Remove(port, "TCP").
    virtual bool remove(uint16_t port) = 0;
};

// Opens a session, or returns nullptr while the collection is NULL (no IGD found yet, or UPnP is off).
using RouterOpener = std::function<std::unique_ptr<RouterSession>()>;

// The real router. COM must be initialised on the calling thread (ComScope). Never called by a test.
std::unique_ptr<RouterSession> openNatUpnpSession();

// CoInitializeEx(COINIT_MULTITHREADED) for the scope; uninitialises only what it initialised.
class ComScope {
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
    bool owned_ = false;
};

// §8.2 step 2: the IPv4 of the interface whose default gateway is the router (the lowest metric when
// there are several), if it is a private address. nullopt otherwise.
std::optional<std::string> internalClientIPv4();
// A usable internal client: a private IPv4 (10/8, 172.16/12, 192.168/16), never loopback.
bool usableInternalClient(const std::string& ip);

// The status the PC and outsideStatus show (v3 §5.1, §8.2, §8.3).
enum class OutsideStatus { Off, Mapped, Manual, Error, CarrierNat, AddressChanged };
const char* outsideStatusName(OutsideStatus s); // "off", "mapped", "manual", "error", "carrier_nat", "address_changed"

struct MappingReport {
    OutsideStatus status = OutsideStatus::Off;
    std::string wanAddress; // as read from the router; empty when none was read
    std::string message;    // the PC's sentence for Manual, Error and CarrierNat; empty otherwise
};

// The mapping flow. Every router call (opening a session included) runs on a worker thread of its own, with
// COM initialised there, and is given up at callDeadlineMs: a router that never answers can't hang the
// host, and nothing ever waits on an abandoned worker (it ends when the router lets go of it). While one is
// stuck, later calls give up at once rather than start another. Not thread-safe: one owner thread.
class PortMapper {
public:
    struct Timing {
        uint64_t findRouterForMs = 10'000; // step 1: retry a NULL collection for about 10 s
        uint64_t retryEveryMs = 1'000;
        uint64_t callDeadlineMs = 5'000;   // each router call, on its worker
        std::function<uint64_t()> nowMs;   // monotonic; default GetTickCount64
        std::function<void(uint64_t)> sleepMs; // default Sleep
        // This PC's LAN address, read again on every check; default internalClientIPv4.
        std::function<std::optional<std::string>()> lanAddress;
    };
    explicit PortMapper(RouterOpener opener);
    PortMapper(RouterOpener opener, Timing timing);
    // policy: which WAN addresses are public (AddressPolicy); a test program's own, else the product's.
    PortMapper(RouterOpener opener, Timing timing, AddressPolicy policy);

    // The outside listener opened on port, to be reached at internalClient (§8.2 steps 1-4, then §8.3). A
    // forward whose WAN check fails (carrier NAT, no internet) is removed again at once.
    MappingReport open(uint16_t port, const std::string& internalClient);
    // The 10-minute check, only while this PC holds a working forward: a fresh session; this PC's LAN address
    // read again (a new one is mapped instead of the old); a mapping that has gone added again; the WAN
    // address read again. A change from the last public address reads AddressChanged until
    // acknowledgeAddressChange. The last public address is kept for the life of the mapper, across open() and
    // remove() and any failed check in between, and so is an unacknowledged change (§8.3). After a WAN failure, a taken port or no LAN address it adds nothing: the
    // status stays as it is until open() is called again.
    MappingReport refresh();
    // v3 §8.3: the address devices were last told, saved in pet-camera.json, before the first open: a first read
    // that differs is a change (AddressChanged), so a restart checks it rather than tell devices at once.
    void seedAnnouncedAddress(const std::string& wan);
    // The PC user turned outside viewing on: nothing is announced yet, so the first address read is the one.
    void forgetAddress();
    // Acknowledges a change only when wan is the address read now (the one the PC user was shown, or the one the
    // pinned gateway confirmed in the same pass). True when it applied.
    bool acknowledgeAddressChange(const std::string& wan);
    // Outside listening turned off, or a clean exit: Remove(port, "TCP") if the mapping is still this PC's.
    // Status Off whatever the router says; never waits on a stuck worker. A removal that can't run because an
    // earlier call is stuck (or that is itself given up) is kept, and runs once the router lets go (§8.2: no
    // forward is left behind): see setOnUnstuck and retryPendingRemovals.
    bool remove();
    // §8.7, a forward a crash or an exit left behind: removes the mapping on port only if it is this PC's own
    // (internal client) and carries kMappingDescription. Someone else's mapping on that port is never touched.
    // Returns what it found: Removed, Absent (none, or not ours), NoRouter or TimedOut.
    struct Outcome;
    Outcome removeLeftover(uint16_t port, const std::string& internalClient);

    // Called on the abandoned call's own thread once a stuck router call returns (no lock of the mapper's held):
    // the owner posts itself a retryPendingRemovals. Set before any call.
    void setOnUnstuck(std::function<void()> f);
    // Runs the removals kept while a call was stuck (owner thread). True when none is left.
    bool retryPendingRemovals();
    // A port this PC's forward may still be on: the one it holds, or one whose removal hasn't run yet; 0 if none.
    uint16_t mayHoldPort() const;

    MappingReport report() const { return report_; }
    uint16_t port() const { return port_; }

    // What one worker found (internal; public for the worker's own use).
    struct Outcome {
        enum Kind { NoRouter, TimedOut, Taken, AddRefused, NotReadBack, Mapped, Removed, Absent };
        Kind kind = NoRouter;
        RouterMapping mapping;
    };

private:
    Outcome onWorker(std::function<Outcome(RouterSession&)> work);
    // Removes this PC's mapping on port (if it is still ours), or keeps it for later when the router is stuck.
    bool removeOrKeep(uint16_t port, const std::string& client);
    MappingReport afterMapping(const Outcome& o, bool fromRefresh);
    MappingReport manual(const std::string& why);
    MappingReport error(const std::string& why);

    RouterOpener opener_;
    Timing timing_;
    AddressPolicy policy_;
    uint16_t port_ = 0;
    std::string client_;
    bool holding_ = false; // a forward this PC made is up and passed the WAN check: refresh() keeps it
    std::string lastPublicWan_;
    bool changeUnacknowledged_ = false;
    MappingReport report_;
    std::shared_ptr<std::atomic<bool>> stuck_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::function<void()>> onUnstuck_ = std::make_shared<std::function<void()>>();
    std::vector<std::pair<uint16_t, std::string>> pendingRemovals_; // (port, internal client)
};

} // namespace kvhost::outside
