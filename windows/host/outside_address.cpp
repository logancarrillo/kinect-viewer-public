#include "outside_address.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstring>

namespace kvhost::outside {

namespace {

bool inV4(const IpAddress& a, uint8_t b0, uint8_t b1, int length) {
    IpAddress net;
    net.b[0] = b0;
    net.b[1] = b1;
    return !a.v6 && inPrefix(a, net, length);
}

bool isMapped(const IpAddress& a) {
    if (!a.v6) return false;
    for (int i = 0; i < 10; ++i)
        if (a.b[i] != 0) return false;
    return a.b[10] == 0xff && a.b[11] == 0xff;
}

} // namespace

bool isLinkLocalV6(const IpAddress& a) {
    return a.v6 && a.b[0] == 0xfe && (a.b[1] & 0xc0) == 0x80;
}

std::optional<IpAddress> parseIp(const std::string& text) {
    std::string t = text;
    uint32_t scope = 0;
    if (const size_t pct = t.find('%'); pct != std::string::npos) {
        // A numeric zone is the interface; a name, or anything else, is dropped.
        const std::string zone = t.substr(pct + 1);
        if (!zone.empty() && zone.size() <= 9 && zone.find_first_not_of("0123456789") == std::string::npos) {
            scope = static_cast<uint32_t>(std::stoul(zone));
        }
        t.resize(pct);
    }
    if (t.empty()) return std::nullopt;
    IpAddress a;
    in_addr v4{};
    if (inet_pton(AF_INET, t.c_str(), &v4) == 1) {
        std::memcpy(a.b.data(), &v4, 4);
        return a;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, t.c_str(), &v6) == 1) {
        a.v6 = true;
        std::memcpy(a.b.data(), &v6, 16);
        a.scope = isLinkLocalV6(a) ? scope : 0;
        return a;
    }
    return std::nullopt;
}

std::optional<IpAddress> fromSockaddr(const sockaddr* sa) {
    if (!sa) return std::nullopt;
    IpAddress a;
    if (sa->sa_family == AF_INET) {
        std::memcpy(a.b.data(), &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, 4);
        return a;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        a.v6 = true;
        std::memcpy(a.b.data(), &in6->sin6_addr, 16);
        // The link a link-local peer is on: fe80::/64 is every link's, so without it a peer on a VPN's TAP adapter
        // or a virtual machine's switch would read as one on the home adapter's link.
        a.scope = isLinkLocalV6(a) ? in6->sin6_scope_id : 0;
        return a;
    }
    return std::nullopt;
}

IpAddress unwrapMapped(const IpAddress& a) {
    if (!isMapped(a)) return a;
    IpAddress v4;
    std::memcpy(v4.b.data(), a.b.data() + 12, 4);
    return v4;
}

std::string toText(const IpAddress& a) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (!inet_ntop(a.v6 ? AF_INET6 : AF_INET, a.b.data(), buf, sizeof buf)) return {};
    return buf;
}

bool inPrefix(const IpAddress& a, const IpAddress& net, int length) {
    if (a.v6 != net.v6) return false;
    const int bits = a.v6 ? 128 : 32;
    if (length < 0 || length > bits) return false;
    const int whole = length / 8, rest = length % 8;
    for (int i = 0; i < whole; ++i)
        if (a.b[i] != net.b[i]) return false;
    if (rest == 0) return true;
    const uint8_t mask = static_cast<uint8_t>(0xff << (8 - rest));
    return (a.b[whole] & mask) == (net.b[whole] & mask);
}

bool isOverlayAddress(const IpAddress& raw) {
    const IpAddress a = unwrapMapped(raw);
    if (!a.v6) return inV4(a, 100, 64, 10);
    IpAddress tailscale;
    tailscale.v6 = true;
    const uint8_t head[6] = {0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0};
    for (int i = 0; i < 6; ++i) tailscale.b[i] = head[i];
    return inPrefix(a, tailscale, 48);
}

std::vector<OnLinkPrefix> onLinkPrefixesOf(const std::vector<std::vector<InterfaceAddress>>& interfaces) {
    std::vector<OnLinkPrefix> out;
    for (const auto& ifc : interfaces) {
        bool overlay = false;
        for (const auto& u : ifc) overlay = overlay || isOverlayAddress(u.address);
        if (overlay) continue;
        for (const auto& u : ifc) {
            OnLinkPrefix p;
            p.length = u.prefixLength;
            p.net = u.address;
            p.net.scope = isLinkLocalV6(u.address) ? u.address.scope : 0; // a link-local prefix is its link's only
            const int bits = p.net.v6 ? 128 : 32;
            if (p.length <= 0 || p.length > bits) continue;
            for (int i = 0; i < bits / 8; ++i) {
                const int keep = p.length - i * 8;
                if (keep >= 8) continue;
                p.net.b[i] = keep <= 0 ? 0 : static_cast<uint8_t>(p.net.b[i] & (0xff << (8 - keep)));
            }
            out.push_back(p);
        }
    }
    return out;
}

std::vector<IpAddress> overlayAddressesOfThisPc() {
    std::vector<IpAddress> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp) continue;
        for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
            const auto a = fromSockaddr(u->Address.lpSockaddr);
            if (a && !a->v6 && isOverlayAddress(*a)) out.push_back(*a);
        }
    }
    return out;
}

namespace {

// An IPv4 prefix no wider than a home network: inside 10/8, 172.16/12 or 192.168/16 at /16 or narrower.
bool homeSizedV4(const OnLinkPrefix& p) {
    return !p.net.v6 && p.length >= 16 &&
           (inV4(p.net, 10, 0, 8) || inV4(p.net, 172, 16, 12) || inV4(p.net, 192, 168, 16));
}

// fc00::/7, the unique local addresses.
bool isUniqueLocalV6(const IpAddress& a) {
    return a.v6 && (a.b[0] & 0xfe) == 0xfc;
}

} // namespace

std::vector<OnLinkPrefix> homePrefixesOf(const std::vector<AdapterInfo>& adapters) {
    std::vector<OnLinkPrefix> out;
    for (const auto& ad : adapters) {
        const bool physical = ad.kind != AdapterInfo::Kind::Other && ad.hardware && !ad.tunnel;
        if (!physical || !ad.hasGateway) continue;
        // Its own prefixes: an adapter holding an overlay address still gives none.
        const std::vector<OnLinkPrefix> prefixes =
            onLinkPrefixesOf(std::vector<std::vector<InterfaceAddress>>{ad.addresses});
        bool anyV4 = false, homeV4 = false;
        for (const auto& p : prefixes) {
            if (p.net.v6) continue;
            anyV4 = true;
            homeV4 = homeV4 || homeSizedV4(p);
        }
        // Behind a home router: its IPv4 network is a home's. One whose IPv4 isn't (a provider's segment on a
        // bridged modem) has its global IPv6 /64 from the same provider, shared with that segment, so that adds
        // nothing either. An adapter with no IPv4 at all has none to judge it by and keeps its global /64.
        const bool behindHomeRouter = !anyV4 || homeV4;
        for (const auto& p : prefixes) {
            if (!p.net.v6) {
                if (homeSizedV4(p)) out.push_back(p);
                continue;
            }
            if (p.length < 64) continue; // /64 or narrower: an on-link /64 is the usual
            // Link-local (its own link only, isLocalSource) and unique-local prefixes keep their own rules.
            if (isLinkLocalV6(p.net) || isUniqueLocalV6(p.net) || behindHomeRouter) out.push_back(p);
        }
    }
    return out;
}

std::optional<std::vector<AdapterInfo>> adaptersOfThisPc() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return std::nullopt;
    std::vector<AdapterInfo> out;
    for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp) continue;
        AdapterInfo info;
        info.kind = ad->IfType == IF_TYPE_ETHERNET_CSMACD ? AdapterInfo::Kind::Ethernet
                    : ad->IfType == IF_TYPE_IEEE80211     ? AdapterInfo::Kind::WiFi
                                                          : AdapterInfo::Kind::Other;
        info.tunnel = ad->TunnelType != TUNNEL_TYPE_NONE;
        info.hasGateway = ad->FirstGatewayAddress != nullptr;
        // Whether it is a real network card: a virtual switch's vEthernet and a VPN's TAP adapter say Ethernet as
        // their type, but not this. One that can't be read counts as not.
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = ad->Luid;
        info.hardware = GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface;
        for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
            auto a = fromSockaddr(u->Address.lpSockaddr);
            if (!a) continue;
            if (isLinkLocalV6(*a)) a->scope = ad->Ipv6IfIndex; // this adapter's link, whatever the sockaddr said
            info.addresses.push_back(InterfaceAddress{*a, static_cast<int>(u->OnLinkPrefixLength)});
        }
        out.push_back(std::move(info));
    }
    return out;
}

bool isLocalSource(const IpAddress& raw, const std::vector<OnLinkPrefix>& home) {
    const IpAddress a = unwrapMapped(raw);
    if (isOverlayAddress(a)) return false; // before any home prefix that might cover it
    if (!a.v6) {
        if (inV4(a, 127, 0, 8)) return true;
    } else {
        IpAddress loop;
        loop.v6 = true;
        loop.b[15] = 1;
        if (a == loop) return true;
    }
    const bool linkLocal = isLinkLocalV6(a);
    for (const auto& p : home) {
        if (!inPrefix(a, p.net, p.length)) continue;
        // fe80::/64 is on every link: only the home prefix's own link counts, and an unknown link never does.
        if (linkLocal && (a.scope == 0 || p.net.scope != a.scope)) continue;
        return true;
    }
    return false;
}

std::string prefix48Key(const IpAddress& raw) {
    IpAddress a = unwrapMapped(raw);
    if (!a.v6) return {};
    for (int i = 6; i < 16; ++i) a.b[i] = 0;
    return toText(a) + "/48";
}

std::string sourceKey(const IpAddress& raw) {
    IpAddress a = unwrapMapped(raw);
    if (!a.v6) return toText(a);
    for (int i = 8; i < 16; ++i) a.b[i] = 0;
    return toText(a) + "/64";
}

WanCheck checkWanAddress(const std::string& text, AddressPolicy policy) {
    const char* const space = " \t\r\n";
    const size_t first = text.find_first_not_of(space);
    const std::string wan =
        first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(space) - first + 1);
    if (wan.empty()) return WanCheck::NotConnected;
    const auto p = parseIp(wan);
    if (!p || p->v6 || wan.find('%') != std::string::npos) return WanCheck::CarrierNat;
    const IpAddress& a = *p;
    if (inV4(a, 0, 0, 8)) return WanCheck::NotConnected;
    // §8.3's private ranges and carrier NAT, then every other range that isn't reachable from the internet
    // (loopback, link-local, the IETF and documentation blocks, benchmarking, multicast and reserved).
    if (inV4(a, 10, 0, 8) || inV4(a, 172, 16, 12) || inV4(a, 192, 168, 16) || inV4(a, 100, 64, 10) ||
        inV4(a, 127, 0, 8) || inV4(a, 169, 254, 16) || inV4(a, 192, 0, 24) || inV4(a, 198, 18, 15) ||
        a.b[0] >= 224)
        return WanCheck::CarrierNat;
    const bool documentation = (a.b[0] == 192 && a.b[1] == 0 && a.b[2] == 2) ||
                               (a.b[0] == 198 && a.b[1] == 51 && a.b[2] == 100) ||
                               (a.b[0] == 203 && a.b[1] == 0 && a.b[2] == 113);
    if (documentation && !policy.documentationIsPublic) return WanCheck::CarrierNat;
    return WanCheck::Public;
}

std::string wanCheckSentence(WanCheck c, const std::string& wan) {
    switch (c) {
        case WanCheck::NotConnected:
            return "The router isn't connected to the internet.";
        case WanCheck::CarrierNat:
            return "Your router's internet address is " + wan +
                   ", which isn't a public address. Your internet provider is using carrier NAT, or there's a second "
                   "router in front of this one, so devices outside your home can't reach this PC. It can't work "
                   "without a relay, and this app doesn't use one.";
        case WanCheck::Public:
            break;
    }
    return {};
}

} // namespace kvhost::outside
