// The address rules (v3 §8.0 local sources and source keys, §8.3 the carrier-NAT check).
#include "kv_test.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> // sockaddr_in6, for fromSockaddr's row
#include <ws2tcpip.h>
// windef.h's 16-bit leftovers: `near` is a word this file uses.
#undef near
#undef far

#include "outside_address.hpp"
#include "outside_settings.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace kvhost::outside;

namespace {
IpAddress ip(const char* s) {
    const auto a = parseIp(s);
    KV_CHECK(a.has_value());
    return a.value_or(IpAddress{});
}
} // namespace

KV_TEST(mapped_addresses_unwrap_to_ipv4) {
    KV_CHECK(toText(unwrapMapped(ip("::ffff:192.168.1.5"))) == "192.168.1.5");
    KV_CHECK(!unwrapMapped(ip("::ffff:192.168.1.5")).v6);
    KV_CHECK(toText(unwrapMapped(ip("2001:db8::1"))) == "2001:db8::1");
    KV_CHECK(toText(unwrapMapped(ip("203.0.113.9"))) == "203.0.113.9");
    KV_CHECK(!parseIp("not an address").has_value());
    KV_CHECK(!parseIp("").has_value());
    KV_CHECK(!parseIp("192.168.1.5:443").has_value());
    KV_CHECK(toText(ip("fe80::1%12")) == "fe80::1");
    // A numeric zone is a link-local address's scope; a named one, or one on any other address, isn't.
    KV_CHECK(ip("fe80::1%12").scope == 12);
    KV_CHECK(ip("fe80::1%eth0").scope == 0);
    KV_CHECK(ip("fe80::1").scope == 0);
    KV_CHECK(ip("2001:db8::1%5").scope == 0);
}

// v3 §8.0: local is loopback, or inside a home prefix. A private range alone isn't home: a VPN or another network
// that hands out 10/8 or 192.168/16 isn't the home network.
KV_TEST(local_sources_are_the_home_network_only) {
    const std::vector<OnLinkPrefix> none;
    for (const char* local : {"127.0.0.1", "127.9.9.9", "::1", "::ffff:127.0.0.1"})
        KV_CHECK_MSG(isLocalSource(ip(local), none), local);
    for (const char* other : {"10.1.2.3", "172.16.0.1", "192.168.1.5", "169.254.3.4", "fe80::1234", "fd12:3456::1",
                              "::ffff:192.168.1.5", "203.0.113.1", "8.8.8.8", "100.64.0.1", "2001:db8::1", "fec0::1"})
        KV_CHECK_MSG(!isLocalSource(ip(other), none), other);
    // With the home network's prefixes: its own addresses, mapped or not, and no others. The link-local prefix is
    // the home adapter's link (interface 7): fe80:: on another link, or on none known, isn't home.
    std::vector<OnLinkPrefix> home(3);
    home[0].net = ip("192.168.1.0");
    home[0].length = 24;
    home[1].net = ip("fe80::%7");
    home[1].length = 64;
    home[2].net = ip("fd12:3456::");
    home[2].length = 64;
    for (const char* local : {"192.168.1.5", "::ffff:192.168.1.5", "fe80::1234%7", "fd12:3456::1"})
        KV_CHECK_MSG(isLocalSource(ip(local), home), local);
    for (const char* other :
         {"192.168.2.5", "10.1.2.3", "fd12:3457::1", "203.0.113.1", "fe80::1234%9", "fe80::1234", "fe80::1234%0"})
        KV_CHECK_MSG(!isLocalSource(ip(other), home), other);
}

// The review's must-fix: fe80::/64 is on every link. A peer that dials the PC's link-local address over a VPN's TAP
// adapter, a Mobile Hotspot or a virtual machine's switch has an fe80:: address too, on that link. Only a source on
// a home adapter's own link (its scope, the adapter's Ipv6IfIndex) is home; an accepted peer's sockaddr carries it.
KV_TEST(a_link_local_source_is_home_only_on_its_home_adapters_link) {
    using Kind = AdapterInfo::Kind;
    auto adapter = [](Kind kind, bool hardware, std::vector<InterfaceAddress> addrs) {
        AdapterInfo a;
        a.kind = kind;
        a.hardware = hardware;
        a.hasGateway = true;
        a.addresses = std::move(addrs);
        return a;
    };
    const std::vector<AdapterInfo> adapters = {
        adapter(Kind::WiFi, true, {{ip("192.168.1.20"), 24}, {ip("fe80::20%7"), 64}}), // the home Wi-Fi, link 7
        adapter(Kind::Ethernet, false, {{ip("172.20.0.1"), 20}, {ip("fe80::99%12"), 64}}), // a virtual switch
        adapter(Kind::Other, false, {{ip("10.8.0.2"), 24}, {ip("fe80::42%15"), 64}}),      // a VPN's TAP adapter
    };
    const auto home = homePrefixesOf(adapters);
    KV_CHECK_MSG(home.size() == 2, std::to_string(home.size()));
    KV_CHECK_MSG(isLocalSource(ip("fe80::5%7"), home), "a phone on the home Wi-Fi's link");
    for (const char* other : {"fe80::5%12", "fe80::5%15", "fe80::5%3", "fe80::5"})
        KV_CHECK_MSG(!isLocalSource(ip(other), home), other);
    // The accepted peer's own sockaddr says which link it came in on.
    sockaddr_in6 peer{};
    peer.sin6_family = AF_INET6;
    KV_REQUIRE(inet_pton(AF_INET6, "fe80::5", &peer.sin6_addr) == 1);
    peer.sin6_scope_id = 12;
    const auto viaSwitch = fromSockaddr(reinterpret_cast<const sockaddr*>(&peer));
    KV_REQUIRE(viaSwitch.has_value());
    KV_CHECK(viaSwitch->scope == 12);
    KV_CHECK_MSG(!isLocalSource(*viaSwitch, home), "a peer on the virtual switch's link");
    peer.sin6_scope_id = 7;
    const auto viaWifi = fromSockaddr(reinterpret_cast<const sockaddr*>(&peer));
    KV_REQUIRE(viaWifi.has_value());
    KV_CHECK_MSG(isLocalSource(*viaWifi, home), "a peer on the home Wi-Fi's link");
    // A global address keeps no scope, whatever the sockaddr says.
    KV_REQUIRE(inet_pton(AF_INET6, "2001:db8::5", &peer.sin6_addr) == 1);
    const auto global = fromSockaddr(reinterpret_cast<const sockaddr*>(&peer));
    KV_REQUIRE(global.has_value());
    KV_CHECK(global->scope == 0);
}

// A physical card with a gateway can still sit on a network far wider than a home: a provider's /22 on a bridged
// modem, a dorm's or a hotel's segment, a bogus /1. An IPv4 home prefix is inside RFC 1918 at /16 or narrower, and an
// IPv6 one /64 or narrower. (Each card here has one address: one with IPv6 alone has no IPv4 to judge it by.)
KV_TEST(home_prefixes_are_no_wider_than_a_home_network) {
    using Kind = AdapterInfo::Kind;
    auto card = [](const char* addr, int len) {
        AdapterInfo a;
        a.kind = Kind::Ethernet;
        a.hardware = true;
        a.hasGateway = true;
        a.addresses.push_back(InterfaceAddress{*parseIp(addr), len});
        return a;
    };
    const std::vector<AdapterInfo> adapters = {
        card("198.51.100.7", 22), // a provider's segment, outside RFC 1918
        card("10.20.30.40", 8),   // RFC 1918, but wider than /16
        card("172.16.5.5", 12),   // the whole of 172.16/12
        card("192.0.2.1", 1),     // a bogus /1
        card("2001:db8::5", 32),  // an IPv6 /32
        card("192.168.4.2", 24),  // home
        card("172.20.5.5", 16),   // home: RFC 1918 at /16
        card("2001:db8:1::5", 48),     // an IPv6 /48: wider than /64
        card("2001:db8:1:2::5", 64),   // home: the usual on-link /64
    };
    const auto home = homePrefixesOf(adapters);
    KV_CHECK_MSG(home.size() == 3, std::to_string(home.size()));
    for (const char* local : {"192.168.4.9", "172.20.200.1", "2001:db8:1:2::9"})
        KV_CHECK_MSG(isLocalSource(ip(local), home), local);
    // (203.0.113.9 lies in the bogus /1, 128.0.0.0/1.)
    for (const char* other :
         {"198.51.101.9", "10.99.0.1", "172.31.0.1", "203.0.113.9", "2001:db8:ff::1", "2001:db8:1:ff::1"})
        KV_CHECK_MSG(!isLocalSource(ip(other), home), other);
}

// A global IPv6 prefix is home only behind a home router. A PC on a bridged modem has a provider's IPv4 segment
// (refused above) and a global /64 the provider shares with that segment: an adapter whose IPv4 prefixes are none of
// them a home's adds no global IPv6 prefix either. Its unique-local and link-local prefixes keep their own rules, a
// home router's adapter keeps its global /64, and an adapter with no IPv4 has none to judge it by.
KV_TEST(a_global_ipv6_prefix_is_home_only_behind_a_home_router) {
    using Kind = AdapterInfo::Kind;
    auto card = [](Kind kind, std::vector<InterfaceAddress> addrs) {
        AdapterInfo a;
        a.kind = kind;
        a.hardware = true;
        a.hasGateway = true;
        a.addresses = std::move(addrs);
        return a;
    };
    const std::vector<AdapterInfo> adapters = {
        card(Kind::Ethernet, {{ip("198.51.100.7"), 22}, {ip("2001:db8:aa:1::7"), 64}, {ip("fd00:1:2:3::7"), 64},
                              {ip("fe80::7%5"), 64}}), // a bridged modem
        card(Kind::WiFi, {{ip("192.168.1.20"), 24}, {ip("2001:db8:bb:1::20"), 64}}), // behind a home router
        card(Kind::Ethernet, {{ip("2001:db8:cc:1::9"), 64}}),                         // IPv6 alone
    };
    const auto home = homePrefixesOf(adapters);
    KV_CHECK_MSG(home.size() == 5, std::to_string(home.size()));
    for (const char* local :
         {"fd00:1:2:3::99", "fe80::99%5", "192.168.1.99", "2001:db8:bb:1::99", "2001:db8:cc:1::1"})
        KV_CHECK_MSG(isLocalSource(ip(local), home), local);
    for (const char* other : {"2001:db8:aa:1::99", "198.51.100.99", "fe80::99%6"})
        KV_CHECK_MSG(!isLocalSource(ip(other), home), other);
}

// The home prefixes come from physical Ethernet and Wi-Fi adapters with a gateway. A VPN's adapter (whichever holds
// the default route), a virtual switch's vEthernet, a PPP link, a tunnel and an adapter with no gateway give none.
KV_TEST(home_prefixes_come_from_physical_ethernet_and_wifi_with_a_gateway) {
    using Kind = AdapterInfo::Kind;
    auto adapter = [](Kind kind, bool hardware, bool tunnel, bool gateway, std::vector<InterfaceAddress> addrs) {
        AdapterInfo a;
        a.kind = kind;
        a.hardware = hardware;
        a.tunnel = tunnel;
        a.hasGateway = gateway;
        a.addresses = std::move(addrs);
        return a;
    };
    const std::vector<AdapterInfo> adapters = {
        adapter(Kind::Ethernet, true, false, true, {{ip("192.168.7.20"), 24}, {ip("2001:db8:aa:bb::20"), 64}}),
        adapter(Kind::WiFi, true, false, true, {{ip("192.168.8.5"), 24}}),
        adapter(Kind::Other, false, false, true, {{ip("10.8.0.2"), 24}}),       // a VPN, holding the default route
        adapter(Kind::Ethernet, false, false, true, {{ip("172.20.0.1"), 20}}),  // a virtual switch's vEthernet
        adapter(Kind::Ethernet, true, true, true, {{ip("10.9.0.2"), 24}}),      // a tunnel on a real card
        adapter(Kind::Other, true, false, true, {{ip("10.64.1.1"), 32}}),       // a PPP link
        adapter(Kind::Ethernet, true, false, false, {{ip("192.168.9.1"), 24}}), // no gateway
        adapter(Kind::WiFi, true, false, true, {{ip("100.100.1.2"), 32}, {ip("192.168.10.2"), 24}}), // an overlay's
        adapter(Kind::WiFi, false, false, true, {{ip("192.168.137.1"), 24}}), // a Mobile Hotspot's or Wi-Fi Direct's
    };
    const auto home = homePrefixesOf(adapters);
    KV_CHECK_MSG(home.size() == 3, std::to_string(home.size()));
    for (const char* local : {"192.168.7.99", "192.168.8.9", "2001:db8:aa:bb::99"})
        KV_CHECK_MSG(isLocalSource(ip(local), home), local);
    for (const char* other :
         {"10.8.0.5", "172.20.0.9", "10.9.0.5", "10.64.1.1", "192.168.9.2", "192.168.10.3", "192.168.137.5"})
        KV_CHECK_MSG(!isLocalSource(ip(other), home), other);
}

KV_TEST(an_on_link_global_ipv6_address_is_local) {
    std::vector<OnLinkPrefix> onLink(1);
    onLink[0].net = ip("2001:db8:aa:bb::");
    onLink[0].length = 64;
    KV_CHECK(isLocalSource(ip("2001:db8:aa:bb:1:2:3:4"), onLink));
    KV_CHECK(!isLocalSource(ip("2001:db8:aa:bc::1"), onLink));
    KV_CHECK(!isLocalSource(ip("203.0.113.1"), onLink));
}

// An overlay network (Tailscale) is never home. Its IPv6 block sits inside fc00::/7, and an
// on-link prefix may cover its IPv4 block; neither makes one of its sources local.
KV_TEST(an_overlay_source_is_never_local) {
    std::vector<OnLinkPrefix> covering(2);
    covering[0].net = ip("100.64.0.0");
    covering[0].length = 10;
    covering[1].net = ip("fd7a:115c:a1e0::");
    covering[1].length = 48;
    for (const char* overlay : {"100.64.0.1", "100.100.1.2", "100.127.255.254", "::ffff:100.100.1.2",
                                "fd7a:115c:a1e0::1", "fd7a:115c:a1e0:ab12:3456::9"}) {
        KV_CHECK_MSG(isOverlayAddress(ip(overlay)), overlay);
        KV_CHECK_MSG(!isLocalSource(ip(overlay), {}), overlay);
        KV_CHECK_MSG(!isLocalSource(ip(overlay), covering), overlay);
    }
    // Just outside the blocks: not overlay. The rest of fc00::/7 is home when a home prefix holds it.
    for (const char* other : {"100.63.255.255", "100.128.0.1", "fd7a:115c:a1e1::1", "fd12:3456::1"})
        KV_CHECK_MSG(!isOverlayAddress(ip(other)), other);
    std::vector<OnLinkPrefix> home(2);
    home[0].net = ip("fd12:3456::");
    home[0].length = 64;
    home[1].net = ip("fd7a:115c:a1e1::");
    home[1].length = 64;
    KV_CHECK(isLocalSource(ip("fd12:3456::1"), home));
    KV_CHECK(isLocalSource(ip("fd7a:115c:a1e1::1"), home));
}

// The overlay adapter's prefixes are not the home network's, so an on-link global IPv6 there isn't local; another
// interface's still are. And the router's own carrier-NAT check (checkWanAddress) is untouched by any of this.
KV_TEST(an_overlay_interface_gives_no_on_link_prefix) {
    const std::vector<std::vector<InterfaceAddress>> interfaces = {
        {{ip("192.168.7.20"), 24}, {ip("2001:db8:aa:bb::20"), 64}},
        {{ip("100.100.1.2"), 32}, {ip("fd7a:115c:a1e0::2"), 128}, {ip("2001:db8:cc:dd::2"), 64}},
    };
    const auto onLink = onLinkPrefixesOf(interfaces);
    KV_CHECK(onLink.size() == 2);
    KV_CHECK(isLocalSource(ip("2001:db8:aa:bb::99"), onLink));
    KV_CHECK(!isLocalSource(ip("2001:db8:cc:dd::99"), onLink));
    KV_CHECK(checkWanAddress("100.64.1.2") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("100.100.1.2") == WanCheck::CarrierNat);
    KV_CHECK(!isUsablePublicAddress("100.100.1.2"));
}

KV_TEST(sources_are_the_ipv4_address_or_the_ipv6_slash_64) {
    // Two mapped IPv4 sources get keys of their own (the limiter's buckets, v3 §8.5).
    KV_CHECK(sourceKey(ip("::ffff:203.0.113.1")) == "203.0.113.1");
    KV_CHECK(sourceKey(ip("::ffff:203.0.113.2")) == "203.0.113.2");
    KV_CHECK(sourceKey(ip("::ffff:203.0.113.1")) == sourceKey(ip("203.0.113.1")));
    // One host rotating through its /64 is one source.
    KV_CHECK(sourceKey(ip("2001:db8:1:2:aaaa::1")) == "2001:db8:1:2::/64");
    KV_CHECK(sourceKey(ip("2001:db8:1:2:ffff:ffff:ffff:fffe")) == "2001:db8:1:2::/64");
    KV_CHECK(sourceKey(ip("2001:db8:1:3::1")) != sourceKey(ip("2001:db8:1:2::1")));
}

KV_TEST(the_wan_address_says_whether_outside_can_work) {
    KV_CHECK(checkWanAddress("") == WanCheck::NotConnected);
    KV_CHECK(checkWanAddress("0.0.0.0") == WanCheck::NotConnected);
    KV_CHECK(checkWanAddress("100.64.1.2") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("100.127.255.254") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("10.0.0.2") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("172.20.1.1") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("192.168.0.2") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("169.254.1.1") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("not-an-ip") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("2001:db8::1") == WanCheck::CarrierNat);
    KV_CHECK(checkWanAddress("100.128.0.1") == WanCheck::Public);
    // The samples are documentation addresses, public only under a test program's own policy (AddressPolicy).
    const AddressPolicy samples{true};
    KV_CHECK(checkWanAddress("203.0.113.160", samples) == WanCheck::Public);
    KV_CHECK(checkWanAddress(" 203.0.113.160\r\n", samples) == WanCheck::Public);
    KV_CHECK(wanCheckSentence(WanCheck::NotConnected, "0.0.0.0") == "The router isn't connected to the internet.");
    const std::string cgn = wanCheckSentence(WanCheck::CarrierNat, "100.64.1.2");
    KV_CHECK(cgn.find("100.64.1.2") != std::string::npos);
    KV_CHECK(cgn.find("It can't work without a relay, and this app doesn't use one.") != std::string::npos);
    KV_CHECK(wanCheckSentence(WanCheck::Public, "203.0.113.160").empty());
}

KV_TEST(outside_settings_are_off_with_a_port_picked_once) {
    const OutsideSettings s;
    KV_CHECK(!s.allowOutside);
    KV_CHECK(s.port == 0);
    // 20000-44999: clear of Windows' dynamic range (49152 up) and of all but the first 375 ports of a block this PC
    // can't bind on 127.0.0.1 (44625-48724); the host's probe skips any port that doesn't bind (v3 §8.7).
    for (int i = 0; i < 200; ++i) {
        const uint16_t p = pickDefaultOutsidePort();
        KV_CHECK_MSG(p >= 20000 && p <= 44999, std::to_string(p));
    }
    KV_CHECK(isValidOutsidePort(1024) && isValidOutsidePort(65535));
    KV_CHECK(!isValidOutsidePort(1023) && !isValidOutsidePort(0) && !isValidOutsidePort(65536));
    KV_CHECK(isUsablePublicAddress("203.0.113.160", AddressPolicy{true}));
    KV_CHECK(!isUsablePublicAddress("192.168.1.1") && !isUsablePublicAddress("example.com"));
}

// The product's policy, the default, never calls a documentation address public: not the router's WAN address,
// not a typed one. A test program's own policy opens those three blocks and nothing else.
KV_TEST(the_products_policy_refuses_the_documentation_blocks) {
    KV_CHECK(!AddressPolicy{}.documentationIsPublic);
    for (const char* doc : {"192.0.2.1", "192.0.2.255", "198.51.100.7", "203.0.113.160", " 203.0.113.142\r\n"}) {
        KV_CHECK_MSG(checkWanAddress(doc) == WanCheck::CarrierNat, doc);
        KV_CHECK_MSG(!isUsablePublicAddress(doc), doc);
        KV_CHECK_MSG(!wanCheckSentence(checkWanAddress(doc), doc).empty(), doc);
        KV_CHECK_MSG(checkWanAddress(doc, AddressPolicy{true}) == WanCheck::Public, doc);
        KV_CHECK_MSG(isUsablePublicAddress(doc, AddressPolicy{true}), doc);
    }
    // Their neighbours are public either way: the blocks are exactly the three /24s.
    for (const char* near : {"192.0.3.1", "198.51.101.7", "203.0.112.255", "203.0.114.1"}) {
        KV_CHECK_MSG(checkWanAddress(near) == WanCheck::Public, near);
    }
    for (const char* other : {"10.0.0.2", "100.64.1.2", "172.20.1.1", "192.168.0.2", "127.0.0.1", "169.254.1.1",
                              "192.0.0.9", "198.18.0.1", "224.0.0.1", "not-an-ip", "2001:db8::1"}) {
        KV_CHECK_MSG(checkWanAddress(other, AddressPolicy{true}) == WanCheck::CarrierNat, other);
    }
    KV_CHECK(checkWanAddress("0.0.0.0", AddressPolicy{true}) == WanCheck::NotConnected);
    KV_CHECK(checkWanAddress("", AddressPolicy{true}) == WanCheck::NotConnected);
}

namespace {

std::string readSource(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// The C++ sources under dir (not recursive): .cpp, .hpp, .h, .c.
std::vector<std::filesystem::path> sourcesIn(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".c")) out.push_back(e.path());
    }
    return out;
}

} // namespace

// No switch to a test program's address policy is reachable from the shipping
// binary, and the viewer's own --test-outside self-test, which runs inside it, keeps exactly its two public-looking
// samples (viewer_app.hpp, TestRouterScript::wan, says why). Read from the source tree this test was built from.
KV_TEST(no_shipping_source_reaches_the_test_policy_and_the_viewer_keeps_two_samples) {
    const std::filesystem::path windows = KV_WINDOWS_SOURCE_DIR;
    const std::filesystem::path core = windows.parent_path() / "core";
    size_t files = 0;
    // The viewer, the CLI and the core never name the policy or the option that carries it.
    for (const auto& dir : {windows / "viewer", windows / "cli", core / "src", core / "include"}) {
        for (const auto& p : sourcesIn(dir)) {
            ++files;
            const std::string text = readSource(p);
            for (const char* name : {"AddressPolicy", "documentationIsPublic", "address_policy_for_test", "addressPolicy"}) {
                KV_CHECK_MSG(text.find(name) == std::string::npos, p.filename().string() + " names " + name);
            }
        }
    }
    // In the host, the switch is named only where it is declared and read, the option only by the server that reads
    // it, and no policy is ever made with a value: a shipping AddressPolicy is always the product's.
    const std::regex made_with_a_value(R"(AddressPolicy\s*(\w+\s*)?[\{(]\s*[^\s\})])");
    for (const auto& p : sourcesIn(windows / "host")) {
        ++files;
        const std::string name = p.filename().string();
        const std::string text = readSource(p);
        if (name != "outside_address.hpp" && name != "outside_address.cpp") {
            KV_CHECK_MSG(text.find("documentationIsPublic") == std::string::npos, name + " names documentationIsPublic");
        }
        if (name != "stream_server.hpp" && name != "stream_server.cpp") {
            KV_CHECK_MSG(text.find("address_policy_for_test") == std::string::npos, name + " names address_policy_for_test");
        }
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            KV_CHECK_MSG(!std::regex_search(line, made_with_a_value), name + ": " + line);
        }
    }
    KV_CHECK_MSG(files > 40, std::to_string(files) + " source files read"); // the tree was found
    // The viewer's IPv4 literals the product calls public: exactly MaxMind's two test addresses.
    const std::regex ipv4(R"((?:^|[^0-9.])(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})(?![0-9.]))");
    std::set<std::string> publicInViewer;
    for (const auto& p : sourcesIn(windows / "viewer")) {
        std::istringstream lines(readSource(p));
        std::string line;
        while (std::getline(lines, line)) {
            for (std::sregex_iterator it(line.begin(), line.end(), ipv4), end; it != end; ++it) {
                const std::string a = (*it)[1].str();
                // A manifest's version='6.0.0.0' (Common Controls) is a version, not an address.
                const size_t at = static_cast<size_t>(it->position(1));
                if (at >= 9 && line.compare(at - 9, 8, "version=") == 0) continue;
                if (checkWanAddress(a) == WanCheck::Public) publicInViewer.insert(a);
            }
        }
    }
    std::string found;
    for (const auto& a : publicInViewer) found += a + " ";
    KV_CHECK_MSG((publicInViewer == std::set<std::string>{"81.2.69.160", "81.2.69.161"}), found);
}