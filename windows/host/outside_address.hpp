// W8 (outside the home), Phase A: the address rules of protocol v3 §8.0 and §8.3. A peer address is
// unwrapped from IPv4-mapped IPv6 first; the local-source filter, the rate-limit key and the
// carrier-NAT check all read the unwrapped address. Pure functions except adaptersOfThisPc and
// overlayAddressesOfThisPc, which read this PC's adapters.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sockaddr;

namespace kvhost::outside {

// One IPv4 or IPv6 address, in network byte order (IPv4 in b[0..3]).
struct IpAddress {
    bool v6 = false;
    std::array<uint8_t, 16> b{};
    // An IPv6 link-local address's zone: the interface it is on (sockaddr_in6's sin6_scope_id, an adapter's
    // Ipv6IfIndex). fe80::/64 is on every link, so the same address on two links is two hosts. 0 when not known,
    // and for any other address. == compares the address, not its zone.
    uint32_t scope = 0;
    bool operator==(const IpAddress& o) const { return v6 == o.v6 && b == o.b; }
};

// fe80::/10.
bool isLinkLocalV6(const IpAddress& a);

// A numeric IPv4 ("192.168.1.5") or IPv6 ("fe80::1") address, or nullopt. A numeric "%zone" suffix
// ("fe80::1%12") is the scope; any other zone is dropped.
std::optional<IpAddress> parseIp(const std::string& text);
// The address of an AF_INET or AF_INET6 sockaddr (an accepted peer), with its scope, or nullopt for any other
// family.
std::optional<IpAddress> fromSockaddr(const sockaddr* sa);
// ::ffff:a.b.c.d becomes a.b.c.d; anything else is returned as it is.
IpAddress unwrapMapped(const IpAddress& a);
// Text without a port: "203.0.113.1", "2001:db8::1".
std::string toText(const IpAddress& a);

// An on-link prefix of one of this PC's interfaces (GetAdaptersAddresses OnLinkPrefixLength). A link-local
// prefix (fe80::/64) is that interface's link only: net.scope is the interface, and a link-local source is in it
// only when its own scope is the same.
struct OnLinkPrefix {
    IpAddress net;
    int length = 0;
};
// An overlay network's address (Tailscale's, and any other VPN on the carrier-grade block): 100.64.0.0/10, or
// Tailscale's IPv6 block fd7a:115c:a1e0::/48. A source on one is never home, whatever
// the PC's interfaces say. It may be any device on the overlay, not one in the house, so it gets the outside rules
// only (pinned certificates, invites, the limiter) and never the LAN listeners or a short pairing code.
// Kept apart from checkWanAddress, which reads the router's own address and is not changed by this.
bool isOverlayAddress(const IpAddress& a);

// An IPv6 source's /48 (one site, or one carrier block), as text with "/48"; "" for IPv4. The limiter caps a /48's
// handshakes in aggregate, and eviction takes from the most-represented one first.
std::string prefix48Key(const IpAddress& a);

// One unicast address of an interface that is up, with its on-link prefix length. A link-local address carries
// its adapter's Ipv6IfIndex as its scope.
struct InterfaceAddress {
    IpAddress address;
    int prefixLength = 0;
};
// The on-link prefixes of these interfaces, one list per interface. An interface holding any overlay address
// (the Tailscale adapter) gives none: its other addresses' prefixes reach the overlay, not the home network.
std::vector<OnLinkPrefix> onLinkPrefixesOf(const std::vector<std::vector<InterfaceAddress>>& interfaces);
// This PC's own overlay IPv4 addresses (100.64.0.0/10) on interfaces that are up: the Tailscale address a
// device on the same tailnet can dial. Read now, never cached here. Empty if the call fails.
std::vector<IpAddress> overlayAddressesOfThisPc();

// One network adapter that is up, as §8.0's home rule reads it.
struct AdapterInfo {
    enum class Kind { Ethernet, WiFi, Other }; // IfType 6 or 71; anything else (PPP, a tunnel, a VPN's own type)
    Kind kind = Kind::Other;
    bool hardware = false;   // MIB_IF_ROW2 HardwareInterface: a real network card, not a virtual switch or a VPN's
    bool tunnel = false;     // TunnelType isn't TUNNEL_TYPE_NONE
    bool hasGateway = false; // a gateway is set on it (GAA_FLAG_INCLUDE_GATEWAYS)
    std::vector<InterfaceAddress> addresses;
};
// §8.0, the home network: the on-link prefixes (onLinkPrefixesOf) of the adapters that are physical Ethernet or
// Wi-Fi cards with a gateway, and not tunnels. A VPN's adapter, a PPP link or a virtual adapter (a virtual switch's
// vEthernet, a Mobile Hotspot's or Wi-Fi Direct's adapter included) gives none, whichever of them holds the default
// route. And a prefix wider than a home network gives none: an IPv4 one outside the RFC 1918 ranges or wider than
// /16 (a provider's /22 on a bridged modem, a dorm's or a hotel's segment, a bogus /1), or an IPv6 one wider than
// /64 (an on-link /64 is the usual). A global IPv6 prefix is home only behind a home router: an adapter that has
// IPv4 prefixes, none of them a home's, adds no global IPv6 prefix, since a bridged modem's provider shares that
// /64 with its segment too. Its link-local and unique-local prefixes keep their own rules, and an adapter with no
// IPv4 at all keeps its global /64.
std::vector<OnLinkPrefix> homePrefixesOf(const std::vector<AdapterInfo>& adapters);
// This PC's adapters that are up, read now. nullopt if the read fails.
std::optional<std::vector<AdapterInfo>> adaptersOfThisPc();
// a lies inside prefix p (both the same family).
bool inPrefix(const IpAddress& a, const IpAddress& net, int length);

// §8.0 "local": loopback (127/8, ::1), or inside one of the home prefixes (homePrefixesOf), and never an overlay
// address (isOverlayAddress), whatever covers it. A private range alone isn't home: a VPN or another network that
// hands out 10/8 or 192.168/16 isn't the home network. A link-local IPv6 source is home only on the link of a home
// prefix: its scope must be that prefix's, and not 0, since fe80:: on a VPN's TAP adapter, a Mobile Hotspot or a
// virtual machine's switch is the same address on another link. The address is unwrapped first. The LAN listeners
// take local sources only.
bool isLocalSource(const IpAddress& a, const std::vector<OnLinkPrefix>& home);

// §8.0/§8.5 "source": the key every per-source limit counts by. IPv4 is the full unwrapped address;
// native IPv6 is its /64, since one host can rotate through a /64 freely ("2001:db8:1:2::/64").
std::string sourceKey(const IpAddress& a);

// Which ranges the outside checks call public. The default is the product's: the documentation blocks
// 192.0.2.0/24, 198.51.100.0/24 and 203.0.113.0/24 are never public. A test program turns documentationIsPublic
// on (through the stream server's test-only option, OutsideMapper::Config, PortMapper and loadPetSettings), so
// that its sample addresses come from those blocks and never from real address space. Nothing outside a test
// program sets it: no flag, no setting, no launch path, and not the viewer, even in a test launch. A row in
// test_outside_address.cpp pins that no shipping source but this header and outside_address.cpp names it.
struct AddressPolicy {
    bool documentationIsPublic = false;
};

// §8.3: what the router's WAN address says.
enum class WanCheck {
    NotConnected, // empty or 0.0.0.0 (any of 0/8): "The router isn't connected to the internet."
    CarrierNat,   // private, 100.64/10, any other non-public range, or not an IPv4 at all
    Public,
};
WanCheck checkWanAddress(const std::string& wan, AddressPolicy policy = {});
// The PC's sentence for NotConnected and CarrierNat (v3 §8.3); empty for Public.
std::string wanCheckSentence(WanCheck c, const std::string& wan);

} // namespace kvhost::outside
