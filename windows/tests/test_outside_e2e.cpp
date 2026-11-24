// Outside the home, end to end on loopback (protocol v3 §8, §10): the host's LAN listeners take
// local sources only, the outside listener admits paired devices and redeems invites, the limiter decides at
// accept, the router mapping runs on its own worker, and turning it off closes everything. Synthetic only:
// listeners bind 127.0.0.1, every state folder is new and removed, the router is a fake behind the RouterSession
// seam (no test ever reaches NATUPnP), and "outside" sources are stood in through the source seam
// (HostStreamServer::Options::test_source_address). This is NOT the iOS app.
#include "kv_test.hpp"

#include "fake_phone.hpp"
#include "host_controller.hpp"
#include "json_lite.hpp"
#include "outside_settings.hpp"
#include "outside_test_dir.hpp"
#include "remote_invite.hpp"
#include "state_files.hpp"

#include <cstdlib>
#include <objbase.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <cstring>
#include <deque>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace kvhost;
using kvtest::FakePhone;

namespace {

uint64_t unixMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

// Waits for a condition with a generous deadline (never a fixed sleep and a count).
bool waitFor(const std::function<bool()>& cond, int ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

// A port nothing listens on now, on loopback (the outside port the test picks).
uint16_t freeLoopbackPort() {
    winsockInit();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    int len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    closesocket(s);
    return ntohs(a.sin_port);
}

// The source seam: what each listener is told its peer is. The outside listener takes the queued sources in
// accept order (tests connect one at a time and wait for each to be taken), then the default.
struct Sources {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> outside_queue;
    std::string outside_default = "203.0.113.200";
    std::string stream, pairing; // "" keeps the real (loopback) address
    int outside_taken = 0;
    int stream_taken = 0;

    std::string pick(const char* listener, const std::string&) {
        std::lock_guard<std::mutex> lk(m);
        if (std::strcmp(listener, "stream") == 0) {
            ++stream_taken;
            cv.notify_all();
        }
        if (std::strcmp(listener, "outside") == 0) {
            ++outside_taken;
            cv.notify_all();
            if (!outside_queue.empty()) {
                std::string s = outside_queue.front();
                outside_queue.pop_front();
                return s;
            }
            return outside_default;
        }
        if (std::strcmp(listener, "stream") == 0) return stream;
        if (std::strcmp(listener, "pairing") == 0) return pairing;
        return {};
    }
    int taken() {
        std::lock_guard<std::mutex> lk(m);
        return outside_taken;
    }
    int streamTaken() {
        std::lock_guard<std::mutex> lk(m);
        return stream_taken;
    }
    void push(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        outside_queue.push_back(s);
    }
    bool waitTaken(int n, int ms = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return outside_taken >= n; });
    }
    void setStream(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        stream = s;
    }
    void setPairing(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        pairing = s;
    }
    void setOutsideDefault(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        outside_default = s;
    }
};

// A fake IGD behind the RouterSession seam. Every call can be held (blocked) to stand for a router that
// doesn't answer.
struct FakeRouter : std::enable_shared_from_this<FakeRouter> {
    std::mutex m;
    std::condition_variable cv;
    std::map<uint16_t, outside::RouterMapping> maps;
    std::string wan = "203.0.113.142";
    bool present = true;
    bool blocked = false;
    std::set<uint16_t> hold_add; // an Add for these ports waits (a router that doesn't answer it)
    bool hold_remove = false;    // every Remove waits
    int opens = 0, adds = 0, removes = 0;
    int adds_entered = 0, removes_entered = 0;

    struct Session final : outside::RouterSession {
        std::shared_ptr<FakeRouter> r;
        explicit Session(std::shared_ptr<FakeRouter> rr) : r(std::move(rr)) {}
        std::optional<outside::RouterMapping> find(uint16_t port) override {
            std::unique_lock<std::mutex> lk(r->m);
            r->cv.wait(lk, [&] { return !r->blocked; });
            auto it = r->maps.find(port);
            if (it == r->maps.end()) return std::nullopt;
            outside::RouterMapping mm = it->second;
            mm.externalAddress = r->wan;
            return mm;
        }
        bool add(uint16_t port, const std::string& client, const std::string& description) override {
            std::unique_lock<std::mutex> lk(r->m);
            ++r->adds_entered;
            r->cv.notify_all();
            r->cv.wait(lk, [&] { return !r->blocked && !r->hold_add.count(port); });
            ++r->adds;
            r->maps[port] = outside::RouterMapping{client, port, true, std::string(), description};
            return true;
        }
        bool remove(uint16_t port) override {
            std::unique_lock<std::mutex> lk(r->m);
            ++r->removes_entered;
            r->cv.notify_all();
            r->cv.wait(lk, [&] { return !r->blocked && !r->hold_remove; });
            ++r->removes;
            return r->maps.erase(port) > 0;
        }
    };

    outside::RouterOpener opener() {
        std::shared_ptr<FakeRouter> self = shared_from_this();
        return [self]() -> std::unique_ptr<outside::RouterSession> {
            std::unique_lock<std::mutex> lk(self->m);
            self->cv.wait(lk, [&] { return !self->blocked; });
            ++self->opens;
            if (!self->present) return nullptr;
            return std::make_unique<Session>(self);
        };
    }
    void block(bool b) {
        std::lock_guard<std::mutex> lk(m);
        blocked = b;
        cv.notify_all();
    }
    void setWan(const std::string& w) {
        std::lock_guard<std::mutex> lk(m);
        wan = w;
    }
    void setPresent(bool p) {
        std::lock_guard<std::mutex> lk(m);
        present = p;
    }
    void holdAdd(uint16_t port, bool hold) {
        std::lock_guard<std::mutex> lk(m);
        if (hold) hold_add.insert(port);
        else hold_add.erase(port);
        cv.notify_all();
    }
    void holdRemove(bool hold) {
        std::lock_guard<std::mutex> lk(m);
        hold_remove = hold;
        cv.notify_all();
    }
    int addsEntered() {
        std::lock_guard<std::mutex> lk(m);
        return adds_entered;
    }
    int removesEntered() {
        std::lock_guard<std::mutex> lk(m);
        return removes_entered;
    }
    bool has(uint16_t port) {
        std::lock_guard<std::mutex> lk(m);
        return maps.count(port) == 1;
    }
    size_t mappings() {
        std::lock_guard<std::mutex> lk(m);
        return maps.size();
    }
    int removed() {
        std::lock_guard<std::mutex> lk(m);
        return removes;
    }
    int added() {
        std::lock_guard<std::mutex> lk(m);
        return adds;
    }
    int opened() {
        std::lock_guard<std::mutex> lk(m);
        return opens;
    }
};

// The home network the rows stand in (v3 §8.0): 192.168.1.0/24 and fd12:3456:789a:1::/64. A private address outside
// them (another network, a VPN) isn't home.
outside::OnLinkPrefix prefixOf(const char* net, int length) {
    outside::OnLinkPrefix p;
    p.net = *outside::parseIp(net);
    p.length = length;
    return p;
}
std::vector<outside::OnLinkPrefix> testHomePrefixes() {
    return {prefixOf("192.168.1.0", 24), prefixOf("fd12:3456:789a:1::", 64)};
}

// One host on loopback with a fake router, a fixed LAN address and the source seam.
struct Rig {
    kvtest::OutsideTempState state;
    std::shared_ptr<Sources> src = std::make_shared<Sources>();
    std::shared_ptr<FakeRouter> router = std::make_shared<FakeRouter>();
    HostStreamServer srv;
    uint16_t outsidePort = 0;

    explicit Rig(const wchar_t* tag) : state(tag) {}
    ~Rig() {
        router->block(false); // a router a test held must not outlive it held
        {
            std::lock_guard<std::mutex> lk(router->m);
            router->hold_add.clear();
            router->hold_remove = false;
            router->cv.notify_all();
        }
        srv.shutdown();
    }

    HostStreamServer::Options options() {
        HostStreamServer::Options o;
        o.state_dir = state.dir;
        o.loopback_only = true;
        o.advertise = false;
        o.router_opener = router->opener();
        o.outside_lan_address = [] { return std::optional<std::string>(kTestLanIPv4); }; // this PC (v3 §8.7)
        o.router_timing.findRouterForMs = 300;
        o.router_timing.retryEveryMs = 50;
        o.router_timing.callDeadlineMs = 3000;
        // The samples (the router's WAN address, typed addresses) are documentation addresses, public only
        // under a test program's own policy. A row that wants the product's puts the default back.
        o.address_policy_for_test.documentationIsPublic = true;
        std::shared_ptr<Sources> s = src;
        o.test_source_address = [s](const char* l, const std::string& p) { return s->pick(l, p); };
        o.on_link_prefixes = testHomePrefixes(); // the test's home network, never this PC's adapters: deterministic
        return o;
    }
    bool start(const std::function<void(HostStreamServer::Options&)>& tweak = {}) {
        HostStreamServer::Options o = options();
        if (tweak) tweak(o);
        std::string err;
        if (!srv.start(o, &err)) {
            std::printf("  start failed: %s\n", err.c_str());
            return false;
        }
        return true;
    }
    // Sharing on, a free port, then outside listening on (the PC's switches, in that order). A port picked here
    // can be taken by something else between the pick and the listen: another one is tried, up to five.
    bool enableOutside(uint16_t port = 0) {
        srv.setSharingEnabled(true);
        for (int attempt = 0; attempt < 5; ++attempt) {
            outsidePort = port ? port : freeLoopbackPort();
            std::string e;
            if (!srv.setOutsidePort(outsidePort, &e) || !srv.setAllowOutside(true, &e)) {
                std::printf("  enable outside failed: %s\n", e.c_str());
                return false;
            }
            if (srv.outsideListeningPort() == outsidePort) return true;
            if (port) break; // the test's own port: nothing else to try
            std::printf("  port %u was taken; trying another\n", static_cast<unsigned>(outsidePort));
        }
        return false;
    }
    bool waitStatus(outside::OutsideStatus st, int ms = 6000) {
        return waitFor([&] {
            const OutsideView v = srv.outsideView();
            return !v.checking && v.status == st;
        }, ms);
    }
    // A raw TCP connection to the outside listener from `source` (no TLS at all), once the listener has taken it.
    SOCKET rawFrom(const std::string& source) {
        const int n = src->taken();
        src->push(source);
        std::string err;
        SOCKET s = tcpConnect("127.0.0.1", outsidePort, 2000, &err);
        src->waitTaken(n + 1);
        return s;
    }
    // A TLS client from `source` that sends its ClientHello and then says nothing more: it never answers the host's
    // flight. The host has read a whole ClientHello, so only the handshake deadline applies.
    struct Stalled {
        SOCKET s = INVALID_SOCKET;
        SSL_CTX* ctx = nullptr;
        SSL* ssl = nullptr;
    };
    Stalled helloFrom(const std::string& source) {
        Stalled h;
        h.s = rawFrom(source);
        if (h.s == INVALID_SOCKET) return h;
        u_long nb = 1;
        ioctlsocket(h.s, FIONBIO, &nb);
        h.ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(h.ctx, TLS1_3_VERSION);
        h.ssl = SSL_new(h.ctx);
        SSL_set_fd(h.ssl, static_cast<int>(h.s));
        SSL_connect(h.ssl); // writes the ClientHello, then would wait for the host: never called again
        return h;
    }
    static void drop(Stalled& h) {
        if (h.ssl) SSL_free(h.ssl);
        if (h.ctx) SSL_CTX_free(h.ctx);
        if (h.s != INVALID_SOCKET) closesocket(h.s);
        h = Stalled{};
    }
    bool connectOutside(FakePhone& phone, const std::string& source, const std::optional<Bytes>& pin) {
        const int n = src->taken();
        src->push(source);
        std::string err;
        const bool ok = phone.connectStream("127.0.0.1", outsidePort, pin, &err);
        src->waitTaken(n + 1);
        return ok;
    }
    bool logHas(const char* event, const std::string& path, const std::string& addr = {}, int ms = 4000) {
        return waitFor([&] {
            for (const auto& l : srv.connectionLog(30)) {
                if (l.event == event && l.path == path && (addr.empty() || l.addr == addr)) return true;
            }
            return false;
        }, ms);
    }
    // A line whose detail starts with `detail`.
    bool logHasDetail(const char* event, const std::string& addr, const std::string& detail, int ms = 4000) {
        return waitFor([&] {
            for (const auto& l : srv.connectionLog(30)) {
                if (l.event == event && l.path == "home" && l.addr == addr && l.detail.rfind(detail, 0) == 0) return true;
            }
            return false;
        }, ms);
    }
};

// The LAN window, as the PC opens it (protocol v3 D1: the role is chosen there).
FakePhone::PairResult pairAtHome(HostStreamServer& srv, FakePhone& phone, PetRole role) {
    std::string err;
    ActivePairingOffer offer;
    if (!srv.beginPairingWindow(120, offer, &err, role)) {
        FakePhone::PairResult r;
        r.error = err;
        return r;
    }
    return phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
}

// What a fake v3 device has read so far. pump() reads for up to ms (pinging as v3 every 250 ms once it has its
// hello), and stops as soon as done() holds.
struct Seen {
    std::vector<HelloServerMessage> hellos;
    std::vector<PetCameraStateMessage> states;
    std::vector<SessionNoticeMessage> notices;
    std::vector<CommandResultMessage> results;
    std::vector<MsgType> order;
    bool closed = false;
    int frames = 0;
};

bool pump(FakePhone& p, Seen& seen, int ms, const std::function<bool(const Seen&)>& done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    auto next_ping = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < until) {
        if (done && done(seen)) return true;
        if (seen.closed) return done ? done(seen) : false;
        if (!seen.hellos.empty() && std::chrono::steady_clock::now() >= next_ping) {
            PingMessage ping;
            ping.clientSendNs = 1;
            ping.protocolVersion = p.version();
            p.sendFrame(MsgType::Ping, toJson(ping));
            next_ping = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        MsgType t;
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        // 300 ms: FakePhone counts a failed read well inside its timeout as the host closing.
        if (!p.readFrame(t, flags, payload, 300)) {
            if (p.lastReadWasClose() || p.violated()) {
                seen.closed = true;
                std::printf("  (closed after %d frame(s)%s%s)\n", seen.frames, p.violated() ? ", violation: " : "",
                            p.violated() ? p.violation().c_str() : "");
            }
            continue;
        }
        ++seen.frames;
        seen.order.push_back(t);
        if (t == MsgType::HelloServer) {
            HelloServerMessage h;
            if (fromJson(payload, h)) seen.hellos.push_back(h);
        } else if (t == MsgType::PetCameraState) {
            PetCameraStateMessage m;
            if (fromJson(payload, m)) seen.states.push_back(m);
        } else if (t == MsgType::SessionNotice) {
            SessionNoticeMessage m;
            if (fromJson(payload, m)) seen.notices.push_back(m);
        } else if (t == MsgType::CommandResult) {
            CommandResultMessage m;
            if (fromJson(payload, m)) seen.results.push_back(m);
        }
    }
    return done ? done(seen) : true;
}

bool hasNotice(const Seen& s, const char* reason) {
    for (const auto& n : s.notices) {
        if (n.reason == reason) return true;
    }
    return false;
}

// A pairRequest for an invite on a connection already open to the outside listener (methodRaw 2, §4.4), and
// the one pairResponse it gets; nullopt when the host closed without a reply.
std::optional<PairResponseMessage> redeemOn(FakePhone& p, const Bytes& token, uint8_t method = 2) {
    PairRequestMessage req;
    req.methodRaw = method;
    req.clientCertDER = p.identity().certificateDER();
    req.clientProofHMAC = computePairingProof(token, p.capturedServerHash(), p.identity().certificateSHA256(), "kv2-pair-client");
    req.protocolVersion = 3;
    req.deviceKind = "iphone";
    if (!p.sendFrame(MsgType::PairRequest, toJson(req))) return std::nullopt;
    MsgType t;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
    PairResponseMessage resp;
    if (!p.readFrame(t, flags, payload, 5000) || t != MsgType::PairResponse || !fromJson(payload, resp)) return std::nullopt;
    return resp;
}

bool refusedSoon(SOCKET s, int ms = 1500) {
    if (s == INVALID_SOCKET) return false; // never connected: that says nothing about the host
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    char b = 0;
    const int n = recv(s, &b, 1, 0);
    return n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT);
}

// How many handshakes the listener evicted for newcomers, by the connection log.
int evictionsLogged(HostStreamServer& srv) {
    int n = 0;
    for (const auto& l : srv.connectionLog(200)) n += l.detail.find("dropped for a newer connection") != std::string::npos;
    return n;
}

HostDeviceInfo deviceNamed(HostStreamServer& srv, const std::string& name) {
    for (const auto& d : srv.devices()) {
        if (d.name == name) return d;
    }
    return HostDeviceInfo{};
}

} // namespace

// ---- §8.0, §8.1: the LAN listeners take local sources only ----------------------------------------------

KV_TEST(outside_lan_stream_listener_and_pairing_window_refuse_a_non_local_source_before_tls) {
    Rig r(L"lan-local");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;

    // An IPv4-mapped home address is unwrapped and local: a session.
    r.src->setStream("::ffff:192.168.1.5");
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen home;
    KV_CHECK(pump(owner, home, 4000, [](const Seen& s) { return !s.hellos.empty(); }));
    owner.close();
    KV_CHECK(r.logHas("connected", "home", "192.168.1.5"));

    // A public source, mapped or not, is closed before TLS: no handshake, nothing read or written.
    for (const char* outside : {"203.0.113.200", "::ffff:203.0.113.201"}) {
        r.src->setStream(outside);
        Seen s;
        if (owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err)) {
            pump(owner, s, 1500, [](const Seen& x) { return x.frames > 0; });
        }
        KV_CHECK_MSG(s.frames == 0, outside);
        owner.close();
    }
    KV_CHECK(r.logHas("refused_local_only", "home", "203.0.113.200"));
    KV_CHECK(r.logHas("refused_local_only", "home", "203.0.113.201"));

    // The pairing window too: a pairing from the internet goes only through an invite.
    r.src->setPairing("203.0.113.202");
    FakePhone stranger(3);
    const auto sp = pairAtHome(r.srv, stranger, PetRole::Viewer);
    KV_CHECK(!sp.ok);
    KV_CHECK(r.srv.devices().size() == 1);
    KV_CHECK(r.logHas("refused_local_only", "home", "203.0.113.202"));
}

// A source on an overlay network (Tailscale's IPv4 or IPv6 block) is refused on the home
// stream listener and the pairing window, closed before TLS, even when an on-link prefix covers it. It may come in
// only through the outside listener, under its rules.
KV_TEST(outside_a_tailnet_source_is_refused_on_the_home_listener) {
    Rig r(L"tailnet-home");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        std::vector<outside::OnLinkPrefix> covering(2);
        covering[0].net = *outside::parseIp("100.64.0.0");
        covering[0].length = 10;
        covering[1].net = *outside::parseIp("fd7a:115c:a1e0::");
        covering[1].length = 48;
        o.on_link_prefixes = covering;
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    for (const char* tailnet : {"100.100.1.2", "fd7a:115c:a1e0::5"}) {
        r.src->setStream(tailnet);
        Seen s;
        if (owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err)) {
            pump(owner, s, 1500, [](const Seen& x) { return x.frames > 0; });
        }
        KV_CHECK_MSG(s.frames == 0 && s.hellos.empty(), tailnet);
        owner.close();
        KV_CHECK_MSG(r.logHas("refused_local_only", "home", tailnet), tailnet);
    }
    for (const char* tailnet : {"100.100.1.3", "fd7a:115c:a1e0:1::6"}) { // another /64: the log counts one per /64
        r.src->setPairing(tailnet);
        FakePhone stranger(3);
        KV_CHECK_MSG(!pairAtHome(r.srv, stranger, PetRole::Viewer).ok, tailnet);
        KV_CHECK_MSG(r.logHas("refused_local_only", "home", tailnet), tailnet);
    }
    KV_CHECK(r.srv.devices().size() == 1);
}

// v3 §8.0, home by adapter, through the adapter seam (the same homePrefixesOf the real read goes through). A VPN that
// holds the default route doesn't change home: a phone on the Wi-Fi card's network still reaches the LAN listeners.
// A source on a virtual adapter's network (a virtual switch's vEthernet) or the VPN's own is refused before TLS, and
// so is a private address no adapter holds.
KV_TEST(outside_home_is_the_physical_adapters_networks_whatever_holds_the_default_route) {
    using Kind = outside::AdapterInfo::Kind;
    auto adapter = [](Kind kind, bool hardware, bool tunnel, bool gateway, const char* addr, int len) {
        outside::AdapterInfo a;
        a.kind = kind;
        a.hardware = hardware;
        a.tunnel = tunnel;
        a.hasGateway = gateway;
        a.addresses.push_back(outside::InterfaceAddress{*outside::parseIp(addr), len});
        return a;
    };
    Rig r(L"home-adapters");
    KV_REQUIRE(r.start([&](HostStreamServer::Options& o) {
        o.on_link_prefixes.reset(); // the adapters decide here, not a fixed list
        o.adapters_for_test = [adapter]() -> std::optional<std::vector<outside::AdapterInfo>> {
            return std::vector<outside::AdapterInfo>{
                adapter(Kind::WiFi, true, false, true, "192.168.1.20", 24),     // the home Wi-Fi
                adapter(Kind::Other, false, true, true, "10.8.0.2", 24),        // a VPN with the default route
                adapter(Kind::Ethernet, false, false, true, "172.20.0.1", 20),  // a virtual switch's vEthernet
            };
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    // The home phone, on the Wi-Fi card's network: a session.
    r.src->setStream("192.168.1.40");
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen home;
    KV_CHECK(pump(owner, home, 4000, [](const Seen& s) { return !s.hellos.empty(); }));
    owner.close();
    KV_CHECK(r.logHas("connected", "home", "192.168.1.40"));
    // The VPN's network, the virtual adapter's, and a private address no adapter holds: closed before TLS.
    for (const char* away : {"10.8.0.9", "172.20.3.4", "192.168.50.7"}) {
        r.src->setStream(away);
        Seen s;
        if (owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err)) {
            pump(owner, s, 1500, [](const Seen& x) { return x.frames > 0; });
        }
        KV_CHECK_MSG(s.frames == 0 && s.hellos.empty(), away);
        owner.close();
        KV_CHECK_MSG(r.logHas("refused_local_only", "home", away), away);
    }
    // The pairing window too: the virtual adapter's network can't pair.
    r.src->setPairing("172.20.3.5");
    FakePhone stranger(3);
    KV_CHECK(!pairAtHome(r.srv, stranger, PetRole::Viewer).ok);
    KV_CHECK(r.logHas("refused_local_only", "home", "172.20.3.5"));
    KV_CHECK(r.srv.devices().size() == 1);
}

// A failed adapter read keeps the last list it read: a passing failure never turns the home phones away.
KV_TEST(outside_a_failed_adapter_read_keeps_the_home_network_it_had) {
    auto fail = std::make_shared<std::atomic<bool>>(false);
    Rig r(L"home-read-fails");
    KV_REQUIRE(r.start([fail](HostStreamServer::Options& o) {
        o.on_link_prefixes.reset();
        o.adapters_for_test = [fail]() -> std::optional<std::vector<outside::AdapterInfo>> {
            if (fail->load()) return std::nullopt;
            outside::AdapterInfo wifi;
            wifi.kind = outside::AdapterInfo::Kind::WiFi;
            wifi.hardware = true;
            wifi.hasGateway = true;
            wifi.addresses.push_back(outside::InterfaceAddress{*outside::parseIp("192.168.1.20"), 24});
            return std::vector<outside::AdapterInfo>{wifi};
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    r.src->setStream("192.168.1.41");
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err)); // reads the adapters: home
    Seen first;
    KV_CHECK(pump(owner, first, 4000, [](const Seen& s) { return !s.hellos.empty(); }));
    owner.close();
    // Every read fails from now on. A stranger's address makes the host read again (after 2 s), and that read
    // fails: the home network it had stays, so the home phone still connects.
    fail->store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    r.src->setStream("192.168.50.8");
    if (owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err)) {
        Seen s;
        pump(owner, s, 1500, [](const Seen& x) { return x.frames > 0; });
        KV_CHECK(s.hellos.empty());
    }
    owner.close();
    KV_CHECK(r.logHas("refused_local_only", "home", "192.168.50.8"));
    r.src->setStream("192.168.1.41");
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen again;
    KV_CHECK_MSG(pump(owner, again, 4000, [](const Seen& s) { return !s.hellos.empty(); }),
                 "a failed adapter read turned the home phone away");
}

// ---- the LAN listeners' caps, deadlines and failures ----------------------------------------------------------

namespace {
// A plain TCP connection to a LAN listener, sending nothing.
SOCKET rawTo(uint16_t port) {
    std::string err;
    return tcpConnect("127.0.0.1", port, 2000, &err);
}
} // namespace

// A thread that can't be started (a PC short of memory) turns that one connection away; nothing throws on the accept
// thread, and the host goes on taking connections. Before, std::system_error went unhandled and the host ended.
KV_TEST(lan_a_thread_that_cant_start_turns_that_connection_away_and_the_host_stays_up) {
    Rig r(L"lan-thread");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    r.src->setStream("192.168.1.40");
    TlsConnection::failThreadStartsForTest(2);
    std::string err;
    for (int i = 0; i < 2; ++i) {
        FakePhone p(3, owner.identity());
        KV_CHECK(!p.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    }
    KV_CHECK(r.logHas("refused_unavailable", "home", "192.168.1.40"));
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
}

// Anything a frame's handling throws (std::bad_alloc on a short-of-memory PC) closes that connection only, never the
// host: the connection's own thread catches it.
KV_TEST(lan_a_handler_that_throws_closes_that_connection_only) {
    auto armed = std::make_shared<std::atomic<bool>>(true);
    Rig r(L"lan-throw");
    KV_REQUIRE(r.start([armed](HostStreamServer::Options& o) {
        o.client_frame_hook_for_test = [armed](MsgType t) {
            if (t == MsgType::Ping && armed->exchange(false)) throw std::bad_alloc();
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen first;
    pump(owner, first, 4000, [](const Seen& x) { return x.closed; });
    KV_CHECK_MSG(first.closed, "the connection whose handler threw was not closed");
    owner.close();
    FakePhone again(3, owner.identity());
    KV_REQUIRE(again.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen s;
    KV_CHECK(pump(again, s, 4000, [](const Seen& x) { return !x.hellos.empty() && !x.states.empty(); }));
}

// The home stream listener holds at most so many connections in their TLS handshake, per source and in all; past
// that a new one is closed before TLS and logged. Once they end, there is room again.
KV_TEST(lan_the_stream_listener_caps_handshakes_per_source_and_in_all) {
    Rig r(L"lan-cap");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.lan_handshakes_per_source = 2;
        o.lan_handshakes_total = 3;
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    r.src->setStream("192.168.1.20");
    SOCKET a = rawTo(pr.streamPort), b = rawTo(pr.streamPort);
    Sleep(300);
    SOCKET c = rawTo(pr.streamPort);
    KV_CHECK_MSG(refusedSoon(c), "a third handshake from one source was let in");
    KV_CHECK(r.logHas("refused_too_many", "home", "192.168.1.20"));
    r.src->setStream("192.168.1.21");
    SOCKET d = rawTo(pr.streamPort);
    Sleep(300);
    r.src->setStream("192.168.1.22");
    SOCKET e = rawTo(pr.streamPort);
    KV_CHECK_MSG(refusedSoon(e), "a fourth handshake in all was let in");
    // A paired device is not held up for long: once the stalled ones go, it gets in.
    for (SOCKET s : {a, b, c, d, e}) closesocket(s);
    Sleep(500);
    r.src->setStream("192.168.1.20");
    std::string err;
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
}

// Anything that throws between a LAN connection's admission and its connection taking over gives
// the slot and the socket back. Before, each one leaked a slot, and after a few the listener was wedged until a restart.
KV_TEST(lan_a_throw_after_admission_gives_the_slot_back) {
    auto throws = std::make_shared<std::atomic<int>>(0);
    Rig r(L"lan-leak");
    KV_REQUIRE(r.start([throws](HostStreamServer::Options& o) {
        o.lan_handshakes_per_source = 2;
        o.pairing_conns_per_source = 1;
        o.after_lan_admit_for_test = [throws] {
            if (throws->load() > 0 && throws->fetch_sub(1) > 0) throw std::bad_alloc();
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    r.src->setStream("192.168.1.50");
    *throws = 6;
    for (int i = 0; i < 6; ++i) {
        SOCKET s = rawTo(pr.streamPort);
        KV_CHECK_MSG(refusedSoon(s), "an admission that threw kept its socket");
        closesocket(s);
    }
    KV_CHECK(r.logHas("refused_unavailable", "home", "192.168.1.50"));
    // Six throws past a cap of 2: had any slot leaked, the phone would be refused now.
    std::string err;
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
    // The pairing listener too.
    ActivePairingOffer offer;
    KV_REQUIRE(r.srv.beginPairingWindow(120, offer, &err, PetRole::Viewer));
    r.src->setPairing("192.168.1.51");
    *throws = 3;
    for (int i = 0; i < 3; ++i) {
        FakePhone p(3);
        KV_CHECK(!p.connectStream("127.0.0.1", offer.port, std::nullopt, &err));
    }
    r.srv.cancelPairingWindow();
    FakePhone viewer(3);
    KV_CHECK_MSG(pairAtHome(r.srv, viewer, PetRole::Viewer).ok, "a pairing slot leaked");
}

// A throw AFTER start() (keeping the connection in the list) must not close a socket the
// connection owns from outside it, nor give its slot back twice. Before, the catch did both. Such a
// connection, left out of the list, is closed through its own close (so shutdown can't miss it), giving its slot back
// once: the cap then still holds.
KV_TEST(lan_a_throw_after_start_closes_that_connection_once_and_gives_its_slot_back_once) {
    auto throws = std::make_shared<std::atomic<int>>(0);
    auto reasons = std::make_shared<std::vector<std::string>>();
    auto reasons_m = std::make_shared<std::mutex>();
    Rig r(L"lan-after-start");
    KV_REQUIRE(r.start([throws, reasons, reasons_m](HostStreamServer::Options& o) {
        o.lan_handshakes_per_source = 2;
        o.after_lan_start_for_test = [throws] {
            if (throws->load() > 0 && throws->fetch_sub(1) > 0) throw std::bad_alloc();
        };
        o.lan_closed_for_test = [reasons, reasons_m](const std::string& why) {
            std::lock_guard<std::mutex> g(*reasons_m);
            reasons->push_back(why);
        };
    }));
    auto seen = [&] {
        std::lock_guard<std::mutex> g(*reasons_m);
        return *reasons;
    };
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    r.src->setStream("192.168.1.60");
    SOCKET a = rawTo(pr.streamPort);
    Sleep(300);
    *throws = 1;
    SOCKET b = rawTo(pr.streamPort); // its start() returns, then the throw
    // Not in the list, so it is closed, through its own close: its slot comes back once.
    KV_CHECK_MSG(refusedSoon(b, 3000), "a connection left out of the list was not closed");
    // It ended through its own close(), once, not from its socket closed under it (that
    // would end it as a failed handshake).
    KV_CHECK(waitFor([&] { return seen().size() == 1; }));
    KV_CHECK_MSG(seen().size() == 1 && seen()[0] == "closed", seen().empty() ? std::string("none") : seen()[0]);
    Sleep(300);
    SOCKET c = rawTo(pr.streamPort); // a and c: the cap of 2
    Sleep(300);
    SOCKET d = rawTo(pr.streamPort);
    KV_CHECK_MSG(!refusedSoon(c, 300), "a slot was still held for the closed connection");
    KV_CHECK_MSG(refusedSoon(d), "a third was let in: a slot was given back twice");
    for (SOCKET s : {a, b, c, d}) closesocket(s);
    // The phone still gets in once they go.
    Sleep(500);
    std::string err;
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
}
// SSL_new and WSACreateEvent return null on a PC short of memory. That connection is turned away and
// gives its slot back; the next one gets in.
KV_TEST(lan_a_tls_setup_that_fails_turns_that_connection_away) {
    Rig r(L"lan-ssl");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.lan_handshakes_per_source = 1; }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    r.src->setStream("192.168.1.52");
    TlsConnection::failTlsSetupForTest(2);
    std::string err;
    for (int i = 0; i < 2; ++i) {
        FakePhone p(3, owner.identity());
        KV_CHECK(!p.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    }
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
}

// The LAN caps count each address, IPv6 too: a household's devices share one /64, and one device's
// handshakes must not hold up another's.
KV_TEST(lan_each_ipv6_address_has_its_own_lan_cap) {
    Rig r(L"lan-v6");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.lan_handshakes_per_source = 1; }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    r.src->setStream("fd12:3456:789a:1::10");
    SOCKET a = rawTo(pr.streamPort);
    Sleep(300);
    r.src->setStream("fd12:3456:789a:1::11"); // the same /64, another device
    SOCKET b = rawTo(pr.streamPort);
    KV_CHECK_MSG(!refusedSoon(b, 800), "another address in the same /64 was refused");
    r.src->setStream("fd12:3456:789a:1::10");
    SOCKET c = rawTo(pr.streamPort);
    KV_CHECK_MSG(refusedSoon(c), "a second handshake from one address was let in");
    for (SOCKET s : {a, b, c}) closesocket(s);
}
// A pairing connection must send its request within its deadline, and one that hasn't is closed when the window
// closes: a connection kept from one window can't speak into a later one. Pairing connections are capped per source.
// Each that ends before its request is logged as pairing_failed, with how it ended (§8.6).
KV_TEST(lan_pairing_connections_have_a_deadline_close_with_the_window_and_are_capped) {
    Rig r(L"lan-pairing");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.pairing_frame_timeout_ms = 500;
        o.pairing_conns_per_source = 1;
    }));
    std::string err;
    ActivePairingOffer offer;
    KV_REQUIRE(r.srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    r.src->setPairing("192.168.1.30");
    // Says nothing after its handshake: closed at the deadline.
    FakePhone idle(3);
    KV_REQUIRE(idle.connectStream("127.0.0.1", offer.port, std::nullopt, &err));
    const auto t0 = std::chrono::steady_clock::now();
    MsgType t;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
    bool closed = false;
    while (!closed && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4)) {
        if (!idle.readFrame(t, flags, payload, 300) && idle.lastReadWasClose()) closed = true;
    }
    KV_CHECK_MSG(closed, "a pairing connection that said nothing was not closed at its deadline");
    idle.close();
    // §8.6: logged as a failed attempt, with why.
    KV_CHECK(r.logHasDetail("pairing_failed", "192.168.1.30", "no pairRequest in time"));
    // One per source: a second at once from the same source is refused before TLS.
    r.srv.cancelPairingWindow();
    KV_REQUIRE(r.srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    FakePhone held(3), second(3);
    KV_REQUIRE(held.connectStream("127.0.0.1", offer.port, std::nullopt, &err));
    Sleep(200);
    KV_CHECK_MSG(!second.connectStream("127.0.0.1", offer.port, std::nullopt, &err), "a second pairing connection from one source got in");
    KV_CHECK(r.logHas("refused_too_many", "home", "192.168.1.30"));
    // The window closes: the held connection, which said nothing, is closed with it.
    r.srv.cancelPairingWindow();
    closed = false;
    const auto t1 = std::chrono::steady_clock::now();
    while (!closed && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(2)) {
        if (!held.readFrame(t, flags, payload, 300) && held.lastReadWasClose()) closed = true;
    }
    KV_CHECK_MSG(closed, "a pairing connection kept open was not closed with its window");
    KV_CHECK(r.logHasDetail("pairing_failed", "192.168.1.30", "the window closed before its request came"));
    // A connection that closes before TLS is logged too, with how it ended.
    KV_REQUIRE(r.srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    r.src->setPairing("192.168.1.31");
    SOCKET raw = rawTo(offer.port);
    Sleep(200); // taken, and in its handshake
    closesocket(raw);
    KV_CHECK(r.logHasDetail("pairing_failed", "192.168.1.31", "closed before its request: "));
    r.srv.cancelPairingWindow();
    // And a real pairing in a new window still works, with no failed attempt logged for it.
    r.src->setPairing("192.168.1.32");
    FakePhone phone(3);
    KV_CHECK(pairAtHome(r.srv, phone, PetRole::Viewer).ok);
    KV_CHECK(r.logHas("paired", "home", "192.168.1.32"));
    KV_CHECK_MSG(!r.logHas("pairing_failed", "home", "192.168.1.32", 500), "a pairing that paired was logged as failed");
}
// ---- §8.1: the outside listener ---------------------------------------------------------------------------

KV_TEST(outside_session_for_a_paired_device_has_path_outside) {
    Rig r(L"outside-session");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.77", pr.serverHash));
    Seen s;
    KV_CHECK(pump(owner, s, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_REQUIRE(!s.hellos.empty() && !s.states.empty());
    KV_CHECK(s.hellos[0].petCamera && s.hellos[0].petCamera->outside);
    const PetCameraStateMessage& st = s.states.back();
    KV_CHECK(st.path == PetPath::Outside);
    KV_CHECK(st.outsideEnabled);
    KV_REQUIRE(st.outsideEndpoint.has_value());
    KV_CHECK(st.outsideEndpoint->host == "203.0.113.142" && st.outsideEndpoint->port == r.outsidePort &&
             st.outsideEndpoint->verified);
    KV_CHECK(st.outsideStatus == std::string("mapped"));
    const auto lim = r.srv.outsideLimiterForTest();
    KV_CHECK(waitFor([&] { return lim->inFlight() == 0 && lim->inFlightFor("203.0.113.77") == 0; }));
    bool listed_outside = false;
    for (const auto& d : r.srv.devices()) listed_outside = listed_outside || (d.connected && d.path == "outside");
    KV_CHECK(listed_outside);
    KV_CHECK(r.logHas("connected", "outside", "203.0.113.77"));
    owner.close();
    KV_CHECK(r.logHas("disconnected", "outside", "203.0.113.77"));
    // The pool is bounded, sized to the limiter's caps: 8 plus the reserve's workers, never one per socket.
    KV_CHECK(r.srv.outsideHandshakeWorkersForTest() == 12);
}

KV_TEST(outside_an_unknown_certificate_is_refused_in_the_handshake_with_no_application_byte) {
    Rig r(L"outside-unknown");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    const uint64_t before = r.srv.rejectedHandshakeCount();
    FakePhone stranger(3);
    Seen s;
    if (r.connectOutside(stranger, "203.0.113.90", std::nullopt)) {
        pump(stranger, s, 2000, [](const Seen& x) { return x.frames > 0; });
    }
    KV_CHECK(s.frames == 0); // nothing written to it, ever
    KV_CHECK(r.srv.rejectedHandshakeCount() > before);
    KV_CHECK(r.logHas("refused_unknown_cert", "outside", "203.0.113.90"));
    const auto lim = r.srv.outsideLimiterForTest();
    KV_CHECK(waitFor([&] { return lim->inFlight() == 0; }));
}

// ---- §8.4: invites -----------------------------------------------------------------------------------------

KV_TEST(outside_an_invite_adds_the_device_with_the_invites_role_once) {
    Rig r(L"invite-ok");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    const auto inv = r.srv.createOutsideInvite(PetRole::Owner, &err);
    if (!inv) std::printf("  invite refused: %s\n", err.c_str());
    KV_REQUIRE(inv.has_value());
    outside::InviteLink link;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, link));
    KV_CHECK(link.port == r.outsidePort && link.host == "203.0.113.142" && link.lanHost == "127.0.0.1");
    KV_CHECK(link.role == PetRole::Owner && link.serviceId == r.srv.serviceId());
    KV_CHECK(link.expUnix == inv->expiresUnix && link.expUnix * 1000 > unixMs() && link.expUnix * 1000 <= unixMs() + 600000);
    KV_CHECK(r.srv.currentOutsideInvite().has_value());

    FakePhone dev(3);
    dev.setDeviceName("Second iPad");
    r.src->push("203.0.113.120");
    const auto res = dev.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, link.token);
    KV_CHECK_MSG(res.ok, res.error);
    KV_CHECK(res.role == std::string("owner"));
    KV_CHECK(res.streamPort == r.srv.streamPort()); // §4.4: the LAN stream port, for every method
    KV_CHECK(deviceNamed(r.srv, "Second iPad").role == PetRole::Owner);
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
    KV_CHECK(r.logHas("invite_redeemed", "outside", "203.0.113.120"));

    // Used once: with no invite open, a second device is refused inside the handshake.
    FakePhone late(3);
    const auto again = late.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, link.token);
    KV_CHECK(!again.ok);
    KV_CHECK(r.srv.devices().size() == 1);

    // The redeemed device is a paired device on the outside path now.
    KV_REQUIRE(r.connectOutside(dev, "203.0.113.120", link.hostCertSHA256));
    Seen s;
    KV_CHECK(pump(dev, s, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_CHECK(!s.states.empty() && s.states.back().path == PetPath::Outside && s.states.back().role == PetRole::Owner);

    // The role comes from the host's invite record: a viewer invite pairs a viewer.
    const auto inv2 = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv2.has_value());
    outside::InviteLink link2;
    KV_REQUIRE(outside::InviteLink::parse(inv2->uri, link2));
    FakePhone viewer(3);
    viewer.setDeviceName("Viewer phone");
    const auto vres = viewer.pair("127.0.0.1", r.outsidePort, link2.hostCertSHA256, PairingMethod::Invite, link2.token);
    KV_CHECK(vres.ok && vres.role == std::string("viewer"));
    KV_CHECK(deviceNamed(r.srv, "Viewer phone").role == PetRole::Viewer);
}

KV_TEST(outside_an_invite_used_meanwhile_gets_the_expired_or_used_sentence) {
    Rig r(L"invite-race");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    const auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    outside::InviteLink link;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, link));
    // Both pass the handshake while the invite is open (redemption-only: no hello, nothing first).
    FakePhone a(3), b(3);
    KV_REQUIRE(r.connectOutside(a, "203.0.113.121", link.hostCertSHA256));
    KV_REQUIRE(r.connectOutside(b, "203.0.113.122", link.hostCertSHA256));
    // The client's side of a TLS 1.3 handshake ends before the host's: wait until the host has classified both.
    KV_REQUIRE(waitFor([&] { return r.srv.outsideRedemptionsWaitingForTest() == 2; }));
    const auto ra = redeemOn(a, link.token);
    KV_REQUIRE(ra.has_value());
    KV_CHECK(ra->accepted);
    const auto rb = redeemOn(b, link.token);
    KV_REQUIRE(rb.has_value());
    KV_CHECK(!rb->accepted && rb->message == text::kInviteExpiredOrUsed);
    KV_CHECK(r.srv.devices().size() == 1);
    KV_CHECK(r.logHas("invite_failed", "outside", "203.0.113.122"));
}

// One wrong proof from outside spoils nothing: it is told the invite didn't match, counted as a failure for its
// source, and the invite stays open for the person it was sent to. The tenth wrong attempt burns it. (One source is
// blocked after five failures, so the ten come from ten sources.)
KV_TEST(outside_a_wrong_proof_is_counted_and_the_tenth_burns_the_invite) {
    Rig r(L"invite-wrong");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    outside::InviteLink link;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, link));
    Bytes wrong = link.token;
    wrong[0] ^= 0x5A;
    r.src->push("203.0.113.61");
    FakePhone guess(3);
    const auto res = guess.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, wrong);
    KV_CHECK(!res.ok && res.error == text::kInviteDidntMatch);
    KV_CHECK(r.srv.currentOutsideInvite().has_value());
    KV_CHECK(r.logHas("invite_failed", "outside", "203.0.113.61"));
    // The person it was sent to still gets in.
    r.src->push("203.0.113.62");
    FakePhone right(3);
    const auto ok = right.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, link.token);
    KV_CHECK_MSG(ok.ok, ok.error);
    KV_CHECK(r.srv.devices().size() == 1);
    // A new invite, and ten wrong attempts from ten sources: the tenth burns it.
    inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, link));
    wrong = link.token;
    wrong[0] ^= 0x5A;
    for (int i = 1; i <= outside::kInviteWrongAttemptsToBurn; ++i) {
        r.src->push("203.0.113." + std::to_string(70 + i));
        FakePhone g(3);
        const auto x = g.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, wrong);
        KV_CHECK(!x.ok);
        if (i < outside::kInviteWrongAttemptsToBurn) {
            KV_CHECK_MSG(x.error == text::kInviteDidntMatch && r.srv.currentOutsideInvite().has_value(), std::to_string(i));
        } else {
            KV_CHECK(x.error == text::kInviteWrongCode); // the tenth: burned
        }
    }
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
    KV_CHECK(r.logHas("invite_burned", "outside"));
    // Burned: the right token no longer gets past the handshake.
    r.src->push("203.0.113.90");
    FakePhone late(3);
    KV_CHECK(!late.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, link.token).ok);
    KV_CHECK(r.srv.devices().size() == 1);
}

KV_TEST(outside_a_certificate_mismatch_is_counted_and_gets_no_reply) {
    Rig r(L"invite-mismatch");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    const auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    outside::InviteLink link;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, link));
    FakePhone other(3), dev(3);
    dev.setClientCertOverride(other.identity().certificateDER());
    const auto res = dev.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, link.token);
    KV_CHECK(!res.ok && res.closedWithoutReply);
    KV_CHECK(r.srv.currentOutsideInvite().has_value()); // counted with the wrong proofs, not burned
    KV_CHECK(r.srv.devices().empty());
    KV_CHECK(r.logHas("invite_failed", "outside"));
}

KV_TEST(outside_another_method_on_the_outside_listener_counts_a_failure_and_burns_nothing) {
    Rig r(L"invite-method");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    const auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    outside::InviteLink link;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, link));
    const auto lim = r.srv.outsideLimiterForTest();
    for (uint8_t m : {uint8_t(0), uint8_t(1)}) {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, "203.0.113.130", link.hostCertSHA256));
        const auto resp = redeemOn(p, link.token, m);
        KV_CHECK_MSG(!resp.has_value(), "methodRaw " + std::to_string(m) + " got a reply");
    }
    KV_CHECK(r.srv.currentOutsideInvite().has_value()); // not burned
    KV_CHECK(r.logHas("invite_failed", "outside", "203.0.113.130"));
    // §8.5: each counted as a failure for its source.
    KV_CHECK(waitFor([&] { return lim->failuresFor("203.0.113.130") == 2; }));
    // Something that isn't a pairRequest at all is refused the same way.
    FakePhone junk(3);
    KV_REQUIRE(r.connectOutside(junk, "203.0.113.131", link.hostCertSHA256));
    PingMessage ping;
    junk.sendFrame(MsgType::Ping, toJson(ping));
    KV_CHECK(junk.waitForHostClose(4000));
    KV_CHECK(r.srv.currentOutsideInvite().has_value());
    KV_CHECK(waitFor([&] { return lim->failuresFor("203.0.113.131") == 1; }));
    // And it still works for the right device.
    FakePhone dev(3);
    const auto ok = dev.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, link.token);
    KV_CHECK_MSG(ok.ok, ok.error);
}

KV_TEST(outside_an_expired_invite_is_refused) {
    Rig r(L"invite-expired");
    // The invites' clock is the test's (Options::invite_clock_for_test), so expiry needs no real wait.
    auto clock = std::make_shared<std::atomic<uint64_t>>(unixMs());
    KV_REQUIRE(r.start([clock](HostStreamServer::Options& o) { o.invite_clock_for_test = [clock] { return clock->load(); }; }));
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    const auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    outside::InviteLink link;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, link));
    KV_CHECK(link.expUnix == (clock->load() + outside::kInviteLifetimeMs) / 1000);
    FakePhone p(3);
    KV_REQUIRE(r.connectOutside(p, "203.0.113.140", link.hostCertSHA256)); // in while it is open
    KV_REQUIRE(waitFor([&] { return r.srv.outsideRedemptionsWaitingForTest() == 1; }));
    // One millisecond short of 10 minutes it is still open; at 10 minutes it isn't.
    clock->store(clock->load() + outside::kInviteLifetimeMs - 1);
    KV_CHECK(r.srv.currentOutsideInvite().has_value());
    clock->store(clock->load() + 1);
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
    KV_CHECK_MSG(r.srv.outsideView().inviteEnded == "expired", r.srv.outsideView().inviteEnded);
    const auto resp = redeemOn(p, link.token);
    KV_REQUIRE(resp.has_value());
    KV_CHECK(!resp->accepted && resp->message == text::kInviteExpiredOrUsed);
    KV_CHECK(r.srv.devices().empty());
    // And a new connection is refused inside the handshake: no invite is open for an unknown certificate.
    FakePhone late(3);
    const auto again = late.pair("127.0.0.1", r.outsidePort, link.hostCertSHA256, PairingMethod::Invite, link.token);
    KV_CHECK(!again.ok);
}

// ---- §8.5: the limiter, at accept ------------------------------------------------------------------------

// An outside connection that sends no TLS record within its first-record deadline is dropped, holding its slot
// no longer than that, counting nothing; a phone that speaks at once is unaffected.
KV_TEST(outside_a_connection_that_says_nothing_is_dropped_at_its_first_record_deadline) {
    Rig r(L"outside-first");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_handshake_timeout_ms = 15000;
        o.outside_first_record_ms = 500;
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    const auto t0 = std::chrono::steady_clock::now();
    SOCKET quiet = r.rawFrom("203.0.113.120");
    KV_CHECK(refusedSoon(quiet, 3000));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    KV_CHECK_MSG(took < 3000, std::to_string(took)); // well inside the 15 s handshake deadline
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.120") == 0; }));
    KV_CHECK(lim->failuresFor("203.0.113.120") == 0);
    closesocket(quiet);
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.121", pr.serverHash));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
}

// The per-source 2 is refused before TLS; a full global 8 evicts the oldest stranger that has shown no certificate
// (counting nothing) and lets the newcomer in, so a handful of addresses can't keep everyone else out.
KV_TEST(outside_per_source_two_is_refused_and_a_full_global_eight_evicts_the_oldest_stranger) {
    Rig r(L"limits");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_handshake_timeout_ms = 15000;
        o.outside_first_record_ms = 15000; // these raw sockets stall on purpose
    }));
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    std::vector<SOCKET> held;
    held.push_back(r.rawFrom("203.0.113.10"));
    held.push_back(r.rawFrom("203.0.113.10"));
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.10") == 2; }));
    SOCKET third = r.rawFrom("203.0.113.10");
    KV_CHECK(refusedSoon(third)); // per source: 2
    closesocket(third);
    for (int i = 0; i < 6; ++i) held.push_back(r.rawFrom("203.0.113." + std::to_string(20 + i)));
    KV_CHECK(waitFor([&] { return lim->inFlight() == 8; }));
    SOCKET ninth = r.rawFrom("203.0.113.40");
    KV_CHECK_MSG(!refusedSoon(ninth, 800), "a ninth was refused, not let in over the oldest stranger"); // evicts
    KV_CHECK(refusedSoon(held.front())); // the oldest (203.0.113.10's first) was the one dropped
    KV_CHECK(r.logHas("refused_too_many", "outside", "203.0.113.10")); // its third, refused as before
    KV_CHECK(r.logHas("disconnected", "outside", "203.0.113.10"));
    KV_CHECK(lim->failuresFor("203.0.113.10") == 0);
    KV_CHECK(lim->inFlight() == 8);
    closesocket(ninth);
    for (SOCKET s : held) closesocket(s);
    KV_CHECK(waitFor([&] { return lim->inFlight() == 0; }, 8000));
}

KV_TEST(outside_the_per_source_bucket_refuses_the_fifth_quick_connection) {
    Rig r(L"bucket");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_handshake_timeout_ms = 15000;
        o.outside_limits.inFlightPerSource = 10; // only the bucket is in play here
    }));
    KV_REQUIRE(r.enableOutside());
    std::vector<SOCKET> held;
    for (int i = 0; i < 4; ++i) held.push_back(r.rawFrom("203.0.113.11"));
    const auto lim = r.srv.outsideLimiterForTest();
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.11") == 4; }));
    SOCKET fifth = r.rawFrom("203.0.113.11");
    KV_CHECK(refusedSoon(fifth));
    closesocket(fifth);
    KV_CHECK(r.logHas("refused_rate_limit", "outside", "203.0.113.11"));
    for (SOCKET s : held) closesocket(s);
}

// Five failures block a source for the hour, and a paired device's success clears
// nothing: a phone on a shared carrier address can't keep strangers' guesses from it fresh. A blocked source is
// still let in, but only a listed device's certificate gets past the TLS verify step for it.
KV_TEST(outside_five_failures_block_a_source_and_a_paired_handshake_clears_nothing) {
    Rig r(L"block");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.outside_limits.burst = 30; }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    auto failFrom = [&](const std::string& src) {
        FakePhone stranger(3);
        Seen s;
        if (r.connectOutside(stranger, src, std::nullopt)) pump(stranger, s, 1500, [](const Seen& x) { return x.closed; });
        stranger.close();
        return waitFor([&] { return lim->inFlightFor(src) == 0; });
    };
    for (int i = 0; i < 5; ++i) KV_CHECK(failFrom("203.0.113.50"));
    KV_CHECK(lim->isBlocked("203.0.113.50"));
    // A stranger's certificate from the blocked source: refused as blocked, and nothing is counted.
    KV_CHECK(failFrom("203.0.113.50"));
    KV_CHECK(r.logHas("refused_blocked", "outside", "203.0.113.50"));
    KV_CHECK(lim->isBlocked("203.0.113.50") && lim->failuresFor("203.0.113.50") == 0);

    // 4 failures, then the owner's phone, then 1 more: the fifth blocks (the phone cleared nothing).
    for (int i = 0; i < 4; ++i) KV_CHECK(failFrom("203.0.113.60"));
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.60", pr.serverHash));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
    owner.close();
    KV_CHECK(lim->failuresFor("203.0.113.60") == 4);
    KV_CHECK(failFrom("203.0.113.60"));
    KV_CHECK(lim->isBlocked("203.0.113.60"));
}

namespace {
// Blocks `src` with five strangers' certificates.
bool blockSource(Rig& r, const std::string& src) {
    const auto lim = r.srv.outsideLimiterForTest();
    for (int i = 0; i < 5; ++i) {
        FakePhone stranger(3);
        Seen s;
        if (r.connectOutside(stranger, src, std::nullopt)) pump(stranger, s, 1500, [](const Seen& x) { return x.closed; });
        stranger.close();
        if (!waitFor([&] { return lim->inFlightFor(src) == 0; })) return false;
    }
    return lim->isBlocked(src);
}
} // namespace

// The owner's own paired phone gets in from an address strangers' failures blocked, and the block, its
// clock and the failure count stay exactly as they were.
KV_TEST(outside_a_paired_device_gets_in_from_a_blocked_source_and_the_block_stays) {
    Rig r(L"block-paired");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.outside_limits.burst = 30; }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    KV_REQUIRE(blockSource(r, "203.0.113.81"));
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.81", pr.serverHash));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
    KV_CHECK(!s.hellos.empty());
    owner.close();
    KV_CHECK(lim->isBlocked("203.0.113.81") && lim->failuresFor("203.0.113.81") == 0);
    KV_CHECK(r.logHas("connected", "outside", "203.0.113.81"));
}

// A blocked source never reaches an invite: its redemption is refused in the TLS verify step, before the token is
// read, and the invite stays open and unburned. Even after a paired device from the same source got in.
KV_TEST(outside_an_invite_from_a_blocked_source_is_refused_before_its_token_is_read) {
    Rig r(L"block-invite");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.outside_limits.burst = 30; }));
    r.router->wan = "203.0.113.142";
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const auto lim = r.srv.outsideLimiterForTest();
    KV_REQUIRE(blockSource(r, "203.0.113.82"));
    // The paired phone first: in, and the block unmoved.
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.82", pr.serverHash));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
    owner.close();
    std::string err;
    const auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    outside::InviteLink il;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, il));
    FakePhone guest(3);
    r.src->push("203.0.113.82");
    const auto res = guest.pair("127.0.0.1", r.outsidePort, il.hostCertSHA256, PairingMethod::Invite, il.token);
    KV_CHECK_MSG(!res.ok, "a blocked source redeemed an invite");
    KV_CHECK(r.srv.currentOutsideInvite().has_value());
    KV_CHECK(r.srv.devices().size() == 1);
    KV_CHECK(r.logHas("refused_blocked", "outside", "203.0.113.82"));
    KV_CHECK(lim->isBlocked("203.0.113.82") && lim->failuresFor("203.0.113.82") == 0);
    // The same invite from an address that isn't blocked is still good.
    FakePhone good(3);
    r.src->push("203.0.113.83");
    KV_CHECK(good.pair("127.0.0.1", r.outsidePort, il.hostCertSHA256, PairingMethod::Invite, il.token).ok);
}

// A device removed from the list is a stranger: from a blocked source it is refused like any other.
KV_TEST(outside_a_removed_device_from_a_blocked_source_is_refused) {
    Rig r(L"block-removed");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.outside_limits.burst = 30; }));
    FakePhone owner(3), old(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    const auto po = pairAtHome(r.srv, old, PetRole::Viewer);
    KV_REQUIRE(po.ok);
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    KV_REQUIRE(blockSource(r, "203.0.113.84"));
    std::string removed;
    for (const auto& d : r.srv.devices()) {
        if (d.role == PetRole::Viewer) removed = d.deviceId;
    }
    KV_REQUIRE(!removed.empty() && r.srv.revokeDevice(removed));
    Seen s;
    if (r.connectOutside(old, "203.0.113.84", po.serverHash)) pump(old, s, 2000, [](const Seen& x) { return x.closed; });
    KV_CHECK(s.hellos.empty());
    KV_CHECK(r.logHas("refused_blocked", "outside", "203.0.113.84"));
    KV_CHECK(lim->isBlocked("203.0.113.84") && lim->failuresFor("203.0.113.84") == 0);
}

// A blocked source's handshake that stalls is dropped at the handshake deadline and frees its slot, so a stranger
// can't sit on the source's 2, and blocked sources hold at most their share of the 8.
KV_TEST(outside_a_stalled_blocked_handshake_is_dropped_at_the_deadline) {
    Rig r(L"block-stall");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_limits.burst = 30;
        o.outside_handshake_timeout_ms = 1500;
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    KV_REQUIRE(blockSource(r, "203.0.113.85"));
    SOCKET a = r.rawFrom("203.0.113.85"), b = r.rawFrom("203.0.113.85");
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.85") == 2 && lim->blockedInFlight() == 2; }));
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.85") == 0 && lim->blockedInFlight() == 0; }, 5000));
    closesocket(a);
    closesocket(b);
    KV_CHECK(lim->isBlocked("203.0.113.85") && lim->failuresFor("203.0.113.85") == 0);
}

// §8.2: after the router's error (this port forwarded to another machine) a typed address is not
// told, so devices aren't sent to that other machine; the PC shows the error and offers no invite.
KV_TEST(outside_a_typed_address_is_not_told_after_the_routers_error) {
    Rig r(L"typed-error");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    KV_REQUIRE(r.srv.setOutsidePublicAddress("203.0.113.154", &err));
    // Another machine on the home network holds the port on the router.
    KV_REQUIRE(r.enableOutside());
    {
        std::lock_guard<std::mutex> g(r.router->m);
        r.router->maps[r.outsidePort] = outside::RouterMapping{"192.168.1.99", r.outsidePort, true, std::string(), "another"};
    }
    KV_REQUIRE(r.srv.setAllowOutside(false, &err) && r.srv.setAllowOutside(true, &err));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Error));
    const OutsideView v = r.srv.outsideView();
    KV_CHECK_MSG(!v.endpointKnown && !v.canInvite, v.endpointHost);
}

KV_TEST(outside_a_recently_paired_source_has_its_own_slots_beside_the_global_eight) {
    Rig r(L"reserve");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_handshake_timeout_ms = 15000;
        o.outside_first_record_ms = 15000; // these strangers stall on purpose (the first-record deadline is its own row)
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    // The owner's phone connects once from its carrier address: noteAuthenticated gives that source the reserve.
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.70", pr.serverHash));
    Seen first;
    KV_CHECK(pump(owner, first, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
    owner.close();
    // Strangers take all 8 unauthenticated slots.
    const auto lim = r.srv.outsideLimiterForTest();
    std::vector<SOCKET> held;
    for (int i = 0; i < 8; ++i) held.push_back(r.rawFrom("203.0.113." + std::to_string(150 + i)));
    KV_CHECK(waitFor([&] { return lim->inFlight() == 8; }));
    // A ninth stranger evicts the oldest stranger (none showed a certificate) rather than being refused; the 8
    // stay 8.
    SOCKET ninth = r.rawFrom("203.0.113.170");
    KV_CHECK(!refusedSoon(ninth, 800));
    KV_CHECK(refusedSoon(held.front()));
    for (size_t i = 1; i < held.size(); ++i) KV_CHECK_MSG(!refusedSoon(held[i], 100), "only the oldest stranger gives way");
    KV_CHECK(lim->inFlight() == 8);
    KV_CHECK(evictionsLogged(r.srv) == 1);
    closesocket(ninth);
    // The owner's phone still gets in, through its own reserve, evicting no one (the reserve's proof).
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.70", pr.serverHash));
    Seen again;
    KV_CHECK(pump(owner, again, 4000, [](const Seen& x) { return !x.hellos.empty(); }));
    KV_CHECK(!again.hellos.empty());
    KV_CHECK_MSG(evictionsLogged(r.srv) == 1, "the paired phone evicted a stranger instead of using its reserve");
    for (size_t i = 1; i < held.size(); ++i) KV_CHECK(!refusedSoon(held[i], 50));
    for (SOCKET s : held) closesocket(s);
}

// Host deaths seen in a real run came after outside sessions. The outside
// path's churn, many times over: a paired phone through the outside listener (the handshake pool, the hand-off),
// the same phone again (it replaces the first), then one of them vanishing with a reset, closing without notify,
// or a TLS-only connection that never says hello, and raw sockets that stall past the handshake deadline (each held
// open across the rounds until the host drops it).
// The host stays up, and the phone still gets in at the end. KV_STRESS_ROUNDS sets the count (200 by default).
KV_TEST(outside_session_churn_leaves_the_host_up) {
    Rig r(L"churn");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_limits.burst = 100000;
        o.outside_limits.refillMs = 1;
        o.outside_limits.newPerMinute = 100000;
        o.outside_handshake_timeout_ms = 2000; // a real handshake on a loaded PC fits well inside it
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    owner.close();
    KV_REQUIRE(r.enableOutside());
    int rounds = 200;
    if (const char* e = std::getenv("KV_STRESS_ROUNDS")) rounds = std::max(1, std::atoi(e));
    int hellos = 0;
    // Stalled sockets, from a source of their own: sent nothing, held open, each one dropped by the host at the deadline.
    std::vector<SOCKET> stalls;
    for (int i = 0; i < 2; ++i) stalls.push_back(r.rawFrom("203.0.113.98"));
    const auto lim = r.srv.outsideLimiterForTest();
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.98") == 2; }));
    for (int i = 0; i < rounds; ++i) {
        const std::string src = "203.0.113." + std::to_string(10 + i % 40);
        FakePhone a(3, owner.identity());
        if (r.connectOutside(a, src, pr.serverHash)) {
            Seen s;
            if (pump(a, s, 2000, [](const Seen& x) { return !x.hellos.empty() && !x.states.empty(); })) ++hellos;
        }
        FakePhone b(3, owner.identity());
        const bool b_up = r.connectOutside(b, src, pr.serverHash); // replaces a
        switch (i % 4) {
            case 0: a.vanish(); if (b_up) b.vanish(); break;
            case 1: a.closeWithoutNotify(); if (b_up) b.close(); break;
            case 2: if (b_up) b.closeWithoutNotify(); a.vanish(); break; // b never said hello
            default: {
                Seen sb;
                if (b_up) pump(b, sb, 300, [](const Seen& x) { return !x.hellos.empty(); });
                a.close();
                if (b_up) b.vanish();
            }
        }
    }
    std::printf("  %d rounds, %d got a hello and a 0x0B\n", rounds, hellos);
    // The stalled ones were dropped at the deadline: their slots are free, and the host closed them.
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.98") == 0; }, 6000));
    for (SOCKET s : stalls) {
        char b = 0;
        DWORD to = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        // Closed by the host: an orderly end (0) or a reset. Our own receive timeout is NOT a close.
        const int got = recv(s, &b, 1, 0);
        const int why = got < 0 ? WSAGetLastError() : 0;
        KV_CHECK_MSG(got == 0 || (got < 0 && why != WSAETIMEDOUT), "a stalled socket was not closed by the host: " + std::to_string(got) + " " + std::to_string(why));
        closesocket(s);
    }
    KV_CHECK(hellos >= rounds * 9 / 10);
    FakePhone again(3, owner.identity());
    KV_REQUIRE(r.connectOutside(again, "203.0.113.99", pr.serverHash));
    Seen s;
    KV_CHECK(pump(again, s, 4000, [](const Seen& x) { return !x.hellos.empty() && !x.states.empty(); }));
}
// Reachable from any network for as long as the PC is left running: while viewing from outside the
// home is on the PC is asked not to sleep; turned off, or the host stopping, lets it go. Asked once each way.
KV_TEST(outside_the_pc_is_kept_awake_while_outside_viewing_is_on) {
    Rig r(L"awake");
    auto told = std::make_shared<std::vector<bool>>();
    auto told_m = std::make_shared<std::mutex>();
    KV_REQUIRE(r.start([told, told_m](HostStreamServer::Options& o) {
        o.keep_awake_for_test = [told, told_m](bool awake) {
            std::lock_guard<std::mutex> g(*told_m);
            told->push_back(awake);
        };
    }));
    auto seen = [&] {
        std::lock_guard<std::mutex> g(*told_m);
        return *told;
    };
    KV_CHECK(seen().empty()); // outside viewing off: nothing asked
    KV_REQUIRE(r.enableOutside());
    KV_CHECK(seen() == std::vector<bool>{true});
    std::string err;
    KV_REQUIRE(r.srv.setOutsidePort(freeLoopbackPort(), &err)); // a new port: still on, asked nothing new
    KV_CHECK_MSG(seen().back() == true, std::to_string(seen().size()));
    KV_REQUIRE(r.srv.setAllowOutside(false, &err));
    KV_CHECK(seen().back() == false);
    KV_REQUIRE(r.srv.setAllowOutside(true, &err));
    KV_CHECK(seen().back() == true);
    r.srv.shutdown();
    KV_CHECK(seen().back() == false);
    for (size_t i = 1; i < seen().size(); ++i) KV_CHECK_MSG(seen()[i] != seen()[i - 1], "asked the same way twice");
}
// Outside listening that can't bind its port asks for no keep-awake, and holds none after.
KV_TEST(outside_a_port_that_wont_bind_takes_no_keep_awake) {
    Rig r(L"awake-bind");
    auto told = std::make_shared<std::vector<bool>>();
    auto told_m = std::make_shared<std::mutex>();
    KV_REQUIRE(r.start([told, told_m](HostStreamServer::Options& o) {
        o.keep_awake_for_test = [told, told_m](bool awake) {
            std::lock_guard<std::mutex> g(*told_m);
            told->push_back(awake);
        };
    }));
    // Something else holds the port, exclusively.
    const uint16_t port = freeLoopbackPort();
    SOCKET hold = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;
    setsockopt(hold, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    KV_REQUIRE(bind(hold, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(hold, 1) == 0);
    KV_CHECK(!r.enableOutside(port));
    KV_CHECK(r.srv.outsideListeningPort() == 0);
    {
        std::lock_guard<std::mutex> g(*told_m);
        for (bool b : *told) KV_CHECK_MSG(!b, "keep-awake was asked for a port that never opened");
    }
    closesocket(hold);
}

// A PORT CHANGE that fails to bind lets the keep-awake go (the old listener is gone).
KV_TEST(outside_a_port_change_that_wont_bind_lets_the_keep_awake_go) {
    Rig r(L"awake-move");
    auto told = std::make_shared<std::vector<bool>>();
    auto told_m = std::make_shared<std::mutex>();
    KV_REQUIRE(r.start([told, told_m](HostStreamServer::Options& o) {
        o.keep_awake_for_test = [told, told_m](bool awake) {
            std::lock_guard<std::mutex> g(*told_m);
            told->push_back(awake);
        };
    }));
    KV_REQUIRE(r.enableOutside());
    {
        std::lock_guard<std::mutex> g(*told_m);
        KV_REQUIRE(!told->empty() && told->back());
    }
    const uint16_t port = freeLoopbackPort();
    SOCKET hold = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;
    setsockopt(hold, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    KV_REQUIRE(bind(hold, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(hold, 1) == 0);
    std::string err;
    r.srv.setOutsidePort(port, &err);
    KV_CHECK(r.srv.outsideListeningPort() == 0);
    {
        std::lock_guard<std::mutex> g(*told_m);
        KV_CHECK_MSG(!told->empty() && !told->back(), "the keep-awake was still held with no listener");
    }
    closesocket(hold);
}
// ---- §8.2, §8.3: the router mapping -----------------------------------------------------------------------

// Reachable from any network: an address the PC user typed
// is told to every device when the router has given no checked address, not only in the manual case: a gateway
// with no UPnP, one behind carrier NAT, and one that hasn't answered yet. Not after the router's error (the port
// forwarded to another machine): outside_a_typed_address_is_not_told_after_the_routers_error. Phones paired at home
// then know where to dial from outside. The address stays "entered by you, not checked" (verified false).
static bool typedAddressReachesAPhone(Rig& r, FakePhone& owner, const auto& pr, const std::string& addr) {
    std::string e2;
    if (!owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &e2)) return false;
    Seen s;
    pump(owner, s, 5000, [&](const Seen& x) {
        return !x.states.empty() && x.states.back().outsideEndpoint && x.states.back().outsideEndpoint->host == addr;
    });
    const OutsideView v = r.srv.outsideView();
    return !s.states.empty() && s.states.back().outsideEndpoint && s.states.back().outsideEndpoint->host == addr &&
           !s.states.back().outsideEndpoint->verified && s.states.back().outsideEndpoint->port == r.outsidePort &&
           v.endpointKnown && v.endpointHost == addr && !v.endpointVerified;
}

// outsideCandidates is in every 0x0B, empty while there is nothing to dial, so a
// device clears its stored list. With outside listening on it holds the typed (or router-read) address first, then
// this PC's own overlay (Tailscale) IPv4 address at the outside port, verified false. Only an address on one of the
// PC's own interfaces is told, only one in 100.64.0.0/10, and only while it is there. The typed field's carrier-NAT
// refusal is unchanged: an overlay address can't be typed.
KV_TEST(outside_candidates_are_in_every_0x0B_and_hold_this_pcs_own_overlay_address) {
    Rig r(L"candidates");
    r.router->wan = "100.64.1.2"; // the router's own address is carrier NAT: never a candidate, it isn't this PC's
    auto ifs = std::make_shared<std::vector<std::string>>(
        std::vector<std::string>{"192.168.7.20", "100.100.1.2", "fd7a:115c:a1e0::2", "100.128.0.9"});
    auto ifs_m = std::make_shared<std::mutex>();
    KV_REQUIRE(r.start([ifs, ifs_m](HostStreamServer::Options& o) {
        o.interface_addresses_for_test = [ifs, ifs_m] {
            std::lock_guard<std::mutex> g(*ifs_m);
            return *ifs;
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen s;
    KV_CHECK(pump(owner, s, 4000, [](const Seen& x) { return !x.states.empty(); }));
    // Outside listening off: the list is there, and empty.
    KV_REQUIRE(!s.states.empty());
    KV_CHECK(s.states.back().outsideCandidates && s.states.back().outsideCandidates->empty());

    // The typed field still refuses an overlay address (carrier NAT), and it is not told that way.
    KV_CHECK(!r.srv.setOutsidePublicAddress("100.100.1.2", &err));
    KV_REQUIRE(r.srv.setOutsidePublicAddress("203.0.113.160", &err));
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::CarrierNat));
    auto told = [&](const Seen& x) {
        if (x.states.empty() || !x.states.back().outsideCandidates) return false;
        const auto& c = *x.states.back().outsideCandidates;
        return c.size() == 2 && c[0].host == "203.0.113.160" && c[1].host == "100.100.1.2";
    };
    KV_CHECK(pump(owner, s, 8000, told));
    KV_REQUIRE(told(s));
    const auto& c = *s.states.back().outsideCandidates;
    KV_CHECK(c[0].port == r.outsidePort && !c[0].verified);
    KV_CHECK(c[1].port == r.outsidePort && !c[1].verified);
    KV_CHECK(s.states.back().outsideEndpoint && s.states.back().outsideEndpoint->host == "203.0.113.160");
    // Every 0x0B carries the list, not only the ones where it changed.
    for (const auto& st : s.states) KV_CHECK(st.outsideCandidates.has_value());
    KV_CHECK(r.logHas("outside_endpoint_told", "home"));

    // The overlay address goes away (Tailscale off): the next 0x0B no longer tells it.
    {
        std::lock_guard<std::mutex> g(*ifs_m);
        *ifs = {"192.168.7.20"};
    }
    KV_REQUIRE(r.srv.setOutsidePublicAddress("203.0.113.161", &err)); // anything that sends a 0x0B
    KV_CHECK(pump(owner, s, 8000, [](const Seen& x) {
        return !x.states.empty() && x.states.back().outsideCandidates && x.states.back().outsideCandidates->size() == 1 &&
               x.states.back().outsideCandidates->front().host == "203.0.113.161";
    }));
    // Outside off again: empty, not absent.
    KV_REQUIRE(r.srv.setAllowOutside(false, &err));
    KV_CHECK(pump(owner, s, 8000, [](const Seen& x) {
        return !x.states.empty() && x.states.back().outsideCandidates && x.states.back().outsideCandidates->empty();
    }));
    // None of it ever names an address in the log.
    for (const auto& l : r.srv.connectionLog(50)) {
        KV_CHECK_MSG(l.detail.find("100.100") == std::string::npos && l.detail.find("203.0.113") == std::string::npos, l.detail);
    }
}
KV_TEST(outside_a_typed_address_is_told_behind_carrier_nat) {
    Rig r(L"typed-cgnat");
    r.router->wan = "100.64.1.2"; // the router's own WAN address is carrier NAT
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::CarrierNat));
    KV_CHECK(!r.srv.outsideView().endpointKnown);
    std::string err;
    KV_REQUIRE(r.srv.setOutsidePublicAddress("203.0.113.151", &err));
    KV_CHECK(typedAddressReachesAPhone(r, owner, pr, "203.0.113.151"));
    // The connection log says the device was told an endpoint, and which kind, never the address itself.
    KV_CHECK(r.logHas("outside_endpoint_told", "home"));
    bool sent = false, leaked = false;
    for (const auto& l : r.srv.connectionLog(50)) {
        if (l.event != std::string("outside_endpoint_told")) continue;
        if (l.detail.find("sent") != std::string::npos && l.detail.find("not checked") != std::string::npos) sent = true;
        if (l.detail.find("203.0.113.151") != std::string::npos || l.addr.find("203.0.113.151") != std::string::npos) leaked = true;
    }
    KV_CHECK(sent);
    KV_CHECK_MSG(!leaked, "the typed address was written to the connection log");
}

// A server with the product's policy, the default, as every real launch has. A router reporting a documentation
// address is behind carrier NAT, and the PC user can't type one; a pet-camera.json holding one loads without it.
KV_TEST(outside_the_products_policy_refuses_a_documentation_address_end_to_end) {
    {
        PetSettings saved;
        saved.sharingEnabled = true;
        saved.outsidePublicAddress = "203.0.113.150";
        Rig r(L"product-policy");
        KV_REQUIRE(savePetSettingsAtomically(r.state.dir, saved));
        r.router->wan = "203.0.113.142";
        KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.address_policy_for_test = outside::AddressPolicy{}; }));
        KV_CHECK(r.srv.outsideView().publicAddressEntered.empty()); // the file's address was not taken
        KV_REQUIRE(r.enableOutside());
        KV_REQUIRE(r.waitStatus(outside::OutsideStatus::CarrierNat));
        KV_CHECK(!r.srv.outsideView().endpointKnown);
        std::string err;
        KV_CHECK(!r.srv.setOutsidePublicAddress("203.0.113.151", &err));
        KV_CHECK_MSG(err.find("public IPv4") != std::string::npos, err);
        KV_CHECK(r.srv.outsideView().publicAddressEntered.empty() && !r.srv.outsideView().endpointKnown);
    }
    {
        // The same file and router under the sample policy: the address loads and the forward holds.
        PetSettings saved;
        saved.sharingEnabled = true;
        saved.outsidePublicAddress = "203.0.113.150";
        Rig r(L"sample-policy");
        KV_REQUIRE(savePetSettingsAtomically(r.state.dir, saved));
        r.router->wan = "203.0.113.142";
        KV_REQUIRE(r.start());
        KV_CHECK(r.srv.outsideView().publicAddressEntered == "203.0.113.150");
        KV_REQUIRE(r.enableOutside());
        KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
        std::string err;
        KV_CHECK_MSG(r.srv.setOutsidePublicAddress("203.0.113.151", &err), err);
    }
}

KV_TEST(outside_a_typed_address_is_told_while_the_router_has_not_answered) {
    Rig r(L"typed-unanswered");
    r.router->blocked = true; // a router that never answers: no check finishes
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    std::string err;
    KV_REQUIRE(r.srv.setOutsidePublicAddress("203.0.113.152", &err));
    // At once, while the first check is still waiting on the router: the typed address is known already.
    const OutsideView now = r.srv.outsideView();
    KV_CHECK_MSG(now.endpointKnown && now.endpointHost == "203.0.113.152" && !now.endpointVerified, now.sentence);
    KV_CHECK(typedAddressReachesAPhone(r, owner, pr, "203.0.113.152"));
    {
        std::lock_guard<std::mutex> g(r.router->m);
        r.router->blocked = false;
    }
    r.router->cv.notify_all();
}

KV_TEST(outside_a_router_read_address_still_wins_over_a_typed_one) {
    Rig r(L"typed-mapped");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    KV_REQUIRE(r.srv.setOutsidePublicAddress("203.0.113.153", &err));
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const OutsideView v = r.srv.outsideView();
    KV_CHECK_MSG(v.endpointKnown && v.endpointHost == "203.0.113.142" && v.endpointVerified, v.endpointHost);
}

KV_TEST(outside_manual_status_with_and_without_an_entered_address) {
    Rig r(L"manual");
    r.router->present = false; // no UPnP router answers
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual));
    OutsideView v = r.srv.outsideView();
    KV_CHECK(v.listening && !v.endpointKnown && !v.canInvite && !v.cannotInviteReason.empty());
    KV_CHECK(v.manualSteps == "Forward TCP " + std::to_string(r.outsidePort) + " to " + std::string(kTestLanIPv4) + " in your router's settings.");
    std::string err;
    KV_CHECK(!r.srv.createOutsideInvite(PetRole::Viewer, &err).has_value() && !err.empty());
    KV_CHECK(r.srv.outsideEndpointLink().empty());
    // Only a public IPv4 is taken as the typed address.
    KV_CHECK(!r.srv.setOutsidePublicAddress("192.168.0.1", &err) && !err.empty());
    KV_CHECK(!r.srv.setOutsidePublicAddress("100.64.0.9", &err));
    KV_CHECK(r.srv.setOutsidePublicAddress(" 203.0.113.150 ", &err));
    v = r.srv.outsideView();
    KV_CHECK(v.endpointKnown && v.endpointHost == "203.0.113.150" && !v.endpointVerified && v.canInvite);
    KV_CHECK(v.publicAddressEntered == "203.0.113.150");
    KV_CHECK(r.srv.petSettings().outsidePublicAddress == "203.0.113.150");
    // Devices at home learn it as "not checked", and an owner sees the manual status.
    std::string e2;
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &e2));
    Seen s;
    KV_CHECK(pump(owner, s, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_REQUIRE(!s.states.empty());
    KV_REQUIRE(s.states.back().outsideEndpoint.has_value());
    KV_CHECK(s.states.back().outsideEndpoint->host == "203.0.113.150" && !s.states.back().outsideEndpoint->verified);
    KV_CHECK(s.states.back().outsideStatus == std::string("manual"));
    const std::string link = r.srv.outsideEndpointLink();
    outside::EndpointLink el;
    KV_CHECK(outside::EndpointLink::parse(link, el) && el.host == "203.0.113.150" && el.port == r.outsidePort);
    const auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    outside::InviteLink il;
    KV_REQUIRE(outside::InviteLink::parse(inv->uri, il));
    // Cleared: no endpoint again, so the invite (it carried that address) is closed, not just hidden (§8.4).
    KV_CHECK(r.srv.setOutsidePublicAddress("", &err));
    v = r.srv.outsideView();
    KV_CHECK(!v.endpointKnown);
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
    KV_CHECK_MSG(v.inviteEnded == "address_gone", v.inviteEnded);
    FakePhone late(3);
    const auto res = late.pair("127.0.0.1", r.outsidePort, il.hostCertSHA256, PairingMethod::Invite, il.token);
    KV_CHECK_MSG(!res.ok, "an invite the PC no longer shows was redeemed");
    KV_CHECK(r.srv.devices().size() == 1);
    KV_CHECK(r.logHas("invite_closed", "outside"));
}

KV_TEST(outside_a_port_taken_on_the_router_is_an_error_and_left_alone) {
    Rig r(L"taken");
    const uint16_t port = freeLoopbackPort();
    r.router->maps[port] = outside::RouterMapping{"192.168.1.99", port, true, std::string()};
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside(port));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Error));
    const OutsideView v = r.srv.outsideView();
    KV_CHECK(v.sentence == "That port is already in use on the router. Pick another port.");
    KV_CHECK(!v.endpointKnown && !v.canInvite);
    std::lock_guard<std::mutex> lk(r.router->m);
    KV_CHECK(r.router->maps.size() == 1 && r.router->maps[port].internalClient == "192.168.1.99");
}

KV_TEST(outside_carrier_nat_removes_the_forward_and_allows_no_invite) {
    Rig r(L"cgnat");
    r.router->wan = "100.64.3.4";
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::CarrierNat));
    const OutsideView v = r.srv.outsideView();
    KV_CHECK(v.wanAddress == "100.64.3.4" && !v.endpointKnown && !v.canInvite);
    KV_CHECK(v.sentence.find("carrier NAT") != std::string::npos);
    KV_CHECK(waitFor([&] { return r.router->mappings() == 0; })); // nothing left open on the router
    // A router with no internet is an error.
    r.router->setWan("0.0.0.0");
    r.srv.outsideRecheckNowForTest();
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Error));
}

KV_TEST(outside_the_mapping_is_removed_when_outside_listening_turns_off) {
    Rig r(L"remove");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(r.router->mappings() == 1);
    std::string err;
    KV_CHECK(r.srv.setAllowOutside(false, &err));
    KV_CHECK(waitFor([&] { return r.router->mappings() == 0 && r.router->removed() >= 1; }));
    const OutsideView v = r.srv.outsideView();
    KV_CHECK(!v.listening && !v.allowed && v.status == outside::OutsideStatus::Off);
    KV_CHECK(r.srv.outsideListeningPort() == 0);
}

KV_TEST(outside_an_address_change_tells_every_v3_session_and_closes_the_invite) {
    Rig r(L"address");
    KV_REQUIRE(r.start());
    FakePhone owner(3), viewer(3);
    const auto po = pairAtHome(r.srv, owner, PetRole::Owner);
    const auto pv = pairAtHome(r.srv, viewer, PetRole::Viewer);
    KV_REQUIRE(po.ok && pv.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    KV_REQUIRE(owner.connectStream("127.0.0.1", po.streamPort, po.serverHash, &err));
    KV_REQUIRE(viewer.connectStream("127.0.0.1", pv.streamPort, pv.serverHash, &err));
    Seen so, sv;
    KV_REQUIRE(pump(owner, so, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_REQUIRE(pump(viewer, sv, 5000, [](const Seen& x) { return !x.states.empty(); }));
    // Viewers get the endpoint but never the status (owners only).
    KV_CHECK(sv.states.back().outsideEndpoint.has_value() && !sv.states.back().outsideStatus);
    KV_CHECK(r.srv.createOutsideInvite(PetRole::Viewer, &err).has_value());

    r.router->setWan("203.0.113.143");
    r.srv.outsideRecheckNowForTest();
    KV_CHECK(r.waitStatus(outside::OutsideStatus::AddressChanged));
    // Not told yet. A device on the home network answering as a fake router could have reported this address,
    // so devices keep the one they were told, with no notice, until the PC user re-shares. Owners see the status.
    KV_CHECK(pump(owner, so, 5000, [](const Seen& x) {
        return !x.states.empty() && x.states.back().outsideStatus == std::string("address_changed");
    }));
    pump(viewer, sv, 1500, [](const Seen&) { return false; }); // anything the viewer would be sent arrives meanwhile
    for (const Seen* s : {&so, &sv}) {
        KV_CHECK(!hasNotice(*s, notice_reason::kAddressChanged));
        for (const auto& st : s->states) KV_CHECK(!st.outsideEndpoint || st.outsideEndpoint->host == "203.0.113.142");
        KV_CHECK(!s->closed);
    }
    KV_CHECK(!r.srv.currentOutsideInvite().has_value()); // it carried the old address
    KV_CHECK(!r.srv.createOutsideInvite(PetRole::Viewer, &err).has_value()); // the re-share comes first
    KV_CHECK_MSG(err.find("Re-share it first") != std::string::npos, err);
    // The re-share shows the PC user the new address...
    outside::EndpointLink el;
    KV_CHECK(outside::EndpointLink::parse(r.srv.outsideEndpointLink(), el) && el.host == "203.0.113.143");
    // ...and closing it acknowledges it: now every v3 session is told, with the notice.
    r.srv.acknowledgeAddressChange("203.0.113.143");
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Mapped));
    auto told = [](const Seen& x) {
        if (!hasNotice(x, notice_reason::kAddressChanged)) return false;
        for (const auto& st : x.states) {
            if (st.outsideEndpoint && st.outsideEndpoint->host == "203.0.113.143") return true;
        }
        return false;
    };
    KV_CHECK(pump(owner, so, 5000, told));
    KV_CHECK(pump(viewer, sv, 5000, told));
    for (const Seen* s : {&so, &sv}) {
        for (const auto& n : s->notices) {
            if (n.reason == notice_reason::kAddressChanged) KV_CHECK(!n.closing && n.message == std::string(text::kAddressChanged));
        }
        KV_CHECK(!s->closed);
    }
    // A new invite now carries the new address.
    const auto inv = r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_REQUIRE(inv.has_value());
    KV_CHECK(inv->uri.find("h=203.0.113.143") != std::string::npos);
}

KV_TEST(outside_a_test_run_never_builds_the_real_router) {
    // The decision itself, with nothing started: test_run alone (even off loopback) means no real router; only
    // a host that is neither a test run nor loopback-only, with no router of the caller's, uses NATUPnP.
    {
        HostController::Options o;
        o.test_run = true;
        KV_CHECK(!HostController::usesRealRouter(o));
        o.test_run = false;
        o.server.loopback_only = true;
        KV_CHECK(!HostController::usesRealRouter(o));
        o.server.loopback_only = false;
        o.router_factory = [] { return std::unique_ptr<outside::RouterSession>(); };
        KV_CHECK(!HostController::usesRealRouter(o));
        o.router_factory = nullptr;
        KV_CHECK(HostController::usesRealRouter(o)); // the PC app's own run (never started here)
    }
    kvtest::OutsideTempState state(L"testrun");
    {
        HostController ctl;
        HostController::Options o;
        o.server.state_dir = state.dir;
        o.server.loopback_only = true; // no test listener beyond loopback
        o.server.advertise = false;
        o.server.outside_lan_address = [] { return std::optional<std::string>(kTestLanIPv4); };
        o.test_run = true; // and no router_factory: no router at all, so manual with the steps
        std::string err;
        if (!ctl.start(o, &err)) std::printf("  start: %s\n", err.c_str());
        KV_REQUIRE(ctl.server().streamPort() != 0);
        ctl.server().setSharingEnabled(true);
        KV_REQUIRE(ctl.setOutsidePort(freeLoopbackPort(), &err));
        KV_CHECK_MSG(ctl.setAllowOutside(true, &err), err);
        KV_CHECK(waitFor([&] {
            const OutsideView v = ctl.outsideView();
            return !v.checking && v.status == outside::OutsideStatus::Manual;
        }));
        const OutsideView v = ctl.outsideView();
        KV_CHECK(v.sentence.find("isn't used in a test run") != std::string::npos);
        KV_CHECK(!v.manualSteps.empty());
        ctl.stop();
    }
    {
        // A test's fake through router_factory is the only router used.
        auto router = std::make_shared<FakeRouter>();
        kvtest::OutsideTempState s2(L"testrun-fake");
        HostController ctl;
        HostController::Options o;
        o.server.state_dir = s2.dir;
        o.server.loopback_only = true;
        o.server.advertise = false;
        o.server.outside_lan_address = [] { return std::optional<std::string>(kTestLanIPv4); };
        o.server.address_policy_for_test.documentationIsPublic = true; // the fake router's sample WAN
        o.test_run = true;
        o.router_factory = router->opener();
        std::string err;
        if (!ctl.start(o, &err)) std::printf("  start: %s\n", err.c_str());
        KV_REQUIRE(ctl.server().streamPort() != 0);
        ctl.server().setSharingEnabled(true);
        KV_REQUIRE(ctl.setOutsidePort(freeLoopbackPort(), &err));
        KV_REQUIRE(ctl.setAllowOutside(true, &err));
        KV_CHECK(waitFor([&] { return ctl.outsideView().status == outside::OutsideStatus::Mapped; }));
        KV_CHECK(router->opened() > 0);
        ctl.stop();
    }
}

// ---- §8.1: turning it off, and privacy mode ----------------------------------------------------------------

KV_TEST(outside_turning_outside_off_tells_outside_sessions_and_closes_the_invite) {
    Rig r(L"off");
    KV_REQUIRE(r.start());
    FakePhone owner(3), home(3);
    const auto po = pairAtHome(r.srv, owner, PetRole::Owner);
    const auto ph = pairAtHome(r.srv, home, PetRole::Viewer);
    KV_REQUIRE(po.ok && ph.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.80", po.serverHash));
    std::string err;
    KV_REQUIRE(home.connectStream("127.0.0.1", ph.streamPort, ph.serverHash, &err));
    Seen so, sh;
    KV_REQUIRE(pump(owner, so, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_REQUIRE(pump(home, sh, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_CHECK(r.srv.createOutsideInvite(PetRole::Viewer, &err).has_value());

    KV_CHECK(r.srv.setAllowOutside(false, &err));
    KV_CHECK(pump(owner, so, 5000, [](const Seen& x) { return hasNotice(x, notice_reason::kOutsideDisabled) && x.closed; }));
    KV_CHECK(hasNotice(so, notice_reason::kOutsideDisabled));
    // The home session stays, with outside off in its 0x0B.
    KV_CHECK(pump(home, sh, 3000, [](const Seen& x) { return !x.states.back().outsideEnabled; }));
    KV_CHECK(!sh.closed && !hasNotice(sh, notice_reason::kOutsideDisabled));
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
    KV_CHECK(r.srv.outsideListeningPort() == 0);
    FakePhone late(3);
    KV_CHECK(!late.connectStream("127.0.0.1", r.outsidePort, po.serverHash, &err)); // the listener is closed
}

KV_TEST(outside_an_owner_turning_sharing_off_from_outside_gets_its_answer_first) {
    Rig r(L"sharing-off");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto po = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(po.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.81", po.serverHash));
    Seen s;
    KV_REQUIRE(pump(owner, s, 5000, [](const Seen& x) { return !x.states.empty(); }));
    PetCameraCommandMessage cmd;
    cmd.requestId = 7;
    cmd.action = pet_action::kSetSharing;
    cmd.enabled = false;
    KV_REQUIRE(owner.sendFrame(MsgType::PetCameraCmd, toJson(cmd)));
    KV_CHECK(pump(owner, s, 5000, [](const Seen& x) { return x.closed; }));
    // v3 §5.2: the 0x0D goes out before the 0x0F outside_disabled and the close.
    size_t answer = SIZE_MAX, notice = SIZE_MAX;
    for (size_t i = 0; i < s.order.size(); ++i) {
        if (s.order[i] == MsgType::CommandResult && answer == SIZE_MAX) answer = i;
        if (s.order[i] == MsgType::SessionNotice && notice == SIZE_MAX) notice = i;
    }
    KV_CHECK(answer != SIZE_MAX && notice != SIZE_MAX && answer < notice);
    KV_CHECK(!s.results.empty() && s.results[0].requestId == 7 && s.results[0].ok);
    KV_CHECK(hasNotice(s, notice_reason::kOutsideDisabled));
    const PetSettings ps = r.srv.petSettings();
    KV_CHECK(!ps.sharingEnabled && !ps.allowOutside); // the setting is cleared too (D9)
    KV_CHECK(waitFor([&] { return r.router->mappings() == 0; }));
    // Sharing on again doesn't bring outside listening back: only the PC does.
    r.srv.setSharingEnabled(true);
    KV_CHECK(r.srv.outsideListeningPort() == 0 && !r.srv.outsideView().allowed);
}

KV_TEST(outside_sharing_off_on_the_pc_turns_outside_off_and_privacy_keeps_it_up) {
    Rig r(L"privacy");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto po = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(po.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.82", po.serverHash));
    Seen s;
    KV_REQUIRE(pump(owner, s, 5000, [](const Seen& x) { return !x.states.empty(); }));
    // Privacy mode keeps the listener and the outside session up (§8.1), so the owner can turn it off.
    r.srv.setPrivacyMode(true);
    KV_CHECK(pump(owner, s, 3000, [](const Seen& x) { return x.states.back().privacyMode; }));
    KV_CHECK(!s.closed && !hasNotice(s, notice_reason::kOutsideDisabled));
    KV_CHECK(r.srv.outsideListeningPort() == r.outsidePort);
    // Sharing off on the PC: outside off, the setting cleared, the outside session told.
    r.srv.setSharingEnabled(false);
    KV_CHECK(pump(owner, s, 5000, [](const Seen& x) { return hasNotice(x, notice_reason::kOutsideDisabled); }));
    KV_CHECK(!r.srv.petSettings().allowOutside && r.srv.outsideListeningPort() == 0);
    std::string err;
    KV_CHECK(!r.srv.setAllowOutside(true, &err) && !err.empty()); // needs sharing on
}

// ---- pet-camera.json fails closed --------------------------------------------------------------------------

KV_TEST(outside_an_unreadable_pet_camera_json_keeps_outside_off_and_refuses_changes) {
    Rig r(L"failclosed");
    const uint16_t port = freeLoopbackPort();
    const std::string json = "{\"sharingEnabled\":true,\"privacyMode\":false,\"allowOutside\":true,\"outsidePort\":" +
                             std::to_string(port) + "}";
    KV_REQUIRE(kvtest::writeRaw(r.state.file(L"pet-camera.json"), json));
    // Held open with no sharing: the host can't read it.
    HANDLE h = CreateFileW(r.state.file(L"pet-camera.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    KV_REQUIRE(r.start());
    KV_CHECK(r.srv.outsideListeningPort() == 0);
    std::string err;
    KV_CHECK(!r.srv.setAllowOutside(true, &err) && err.find("pet-camera.json") != std::string::npos);
    KV_CHECK(!r.srv.setOutsidePort(port, &err) && !err.empty());
    KV_CHECK(!r.srv.setOutsidePublicAddress("203.0.113.150", &err) && !err.empty());
    KV_CHECK(r.srv.outsideView().sentence.find("pet-camera.json") != std::string::npos);
    // Once it can be read, outside listening follows what it says.
    CloseHandle(h);
    KV_CHECK(waitFor([&] { return r.srv.outsideListeningPort() == port; }, 8000));
}

KV_TEST(outside_a_pet_camera_json_with_outside_on_but_sharing_off_doesnt_listen) {
    Rig r(L"needs-sharing");
    const uint16_t port = freeLoopbackPort();
    KV_REQUIRE(kvtest::writeRaw(r.state.file(L"pet-camera.json"),
                                "{\"sharingEnabled\":false,\"allowOutside\":true,\"outsidePort\":" + std::to_string(port) + "}"));
    KV_REQUIRE(r.start());
    KV_CHECK(r.srv.outsideListeningPort() == 0);
    KV_CHECK(!r.srv.outsideView().listening);
}

// ---- shutdown never waits on the router -----------------------------------------------------------------------

KV_TEST(outside_shutdown_does_not_wait_on_a_router_that_doesnt_answer) {
    Rig r(L"stuck");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.router_timing.callDeadlineMs = 60000; }));
    r.router->block(true);
    KV_REQUIRE(r.enableOutside()); // the PC's switch returns at once; the worker is stuck on the router
    const auto t0 = std::chrono::steady_clock::now();
    const OutsideView v = r.srv.outsideView();
    KV_CHECK(v.listening && v.checking);
    KV_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    const auto t1 = std::chrono::steady_clock::now();
    r.srv.shutdown();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
    std::printf("  shutdown took %lld ms with the router held\n", static_cast<long long>(took));
    KV_CHECK(took < 3000);
    r.router->block(false);
}

// ---- §8.6: the log -------------------------------------------------------------------------------------------

KV_TEST(outside_the_connection_log_is_newest_first_and_kept_on_the_pc) {
    Rig r(L"log");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    for (int i = 0; i < 3; ++i) {
        SOCKET s = r.rawFrom("203.0.113." + std::to_string(30 + i));
        closesocket(s);
        KV_CHECK(waitFor([&] { return r.srv.outsideLimiterForTest()->inFlight() == 0; }));
    }
    std::vector<outside::LogLine> lines;
    KV_CHECK(waitFor([&] {
        lines = r.srv.connectionLog(30);
        return lines.size() >= 3;
    }));
    for (size_t i = 1; i < lines.size(); ++i) KV_CHECK(lines[i - 1].t >= lines[i].t);
    // Written to the state folder (owner-only), never anywhere else.
    r.srv.shutdown();
    std::string text;
    KV_CHECK(readWholeFile(r.state.file(outside::kConnectionLogFile), text) && text.find("\"path\":\"outside\"") != std::string::npos);
}

// ---- Races, the invite following the endpoint, the log, failures, the mapping ----------------------------------

namespace {

// A test-held gate for the host's seams: the host's thread waits in pass() until the test opens it (or 10 s pass, so
// a broken test can't hang the run). Shared, so a host thread still in it can't outlive it.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    int entered = 0;
    bool open = false;
    void pass() {
        std::unique_lock<std::mutex> lk(m);
        ++entered;
        cv.notify_all();
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return open; });
    }
    bool waitEntered(int n = 1, int ms = 5000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return entered >= n; });
    }
    void release() {
        std::lock_guard<std::mutex> lk(m);
        open = true;
        cv.notify_all();
    }
};

// Two different free loopback ports.
std::pair<uint16_t, uint16_t> twoFreePorts() {
    const uint16_t a = freeLoopbackPort();
    uint16_t b = freeLoopbackPort();
    for (int i = 0; i < 10 && b == a; ++i) b = freeLoopbackPort();
    return {a, b};
}

// Waits until the host has closed a raw socket (EOF or reset); false at the deadline.
bool closedWithin(SOCKET s, int ms) {
    if (s == INVALID_SOCKET) return false;
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    char b[256];
    for (;;) {
        const int n = recv(s, b, sizeof(b), 0);
        if (n == 0) return true;
        if (n < 0) return WSAGetLastError() != WSAETIMEDOUT;
    }
}

void closeWithReset(SOCKET s) {
    linger l{1, 0}; // an abortive close: the host reads a reset
    setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
    closesocket(s);
}

std::optional<outside::InviteLink> inviteOf(HostStreamServer& srv, PetRole role) {
    std::string err;
    const auto inv = srv.createOutsideInvite(role, &err);
    if (!inv) {
        std::printf("  invite refused: %s\n", err.c_str());
        return std::nullopt;
    }
    outside::InviteLink link;
    if (!outside::InviteLink::parse(inv->uri, link)) return std::nullopt;
    return link;
}

} // namespace

// A connection the outside pool handed off just before outside listening turned off never becomes a session.
KV_TEST(outside_a_connection_handed_off_as_outside_turns_off_never_becomes_a_session) {
    Rig r(L"h1-race");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    auto gate = std::make_shared<Gate>();
    r.srv.testHookBeforeSessionReady = [gate] { gate->pass(); };
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.91", pr.serverHash));
    // The handshake is done and the connection handed off; its on_ready is held before it takes the server lock.
    KV_REQUIRE(gate->waitEntered());
    std::string err;
    KV_CHECK(r.srv.setAllowOutside(false, &err));
    gate->release();
    Seen s;
    pump(owner, s, 3000, [](const Seen& x) { return x.closed || !x.hellos.empty(); });
    KV_CHECK_MSG(s.hellos.empty(), "a connection from outside got a hello after outside listening turned off");
    KV_CHECK(waitFor([&] { return s.closed || owner.waitForHostClose(500); }, 4000));
    KV_CHECK(r.srv.sessionCount() == 0);
    for (const auto& d : r.srv.devices()) KV_CHECK(!d.connected);
}

// An invite closes when the router check stops giving an endpoint, and can't be redeemed after.
KV_TEST(outside_an_invite_closes_when_a_check_reads_manual) {
    Rig r(L"h2-manual");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const auto link = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link.has_value());
    r.router->setPresent(false); // the router stops answering UPnP
    r.srv.outsideRecheckNowForTest();
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual));
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
    KV_CHECK_MSG(r.srv.outsideView().inviteEnded == "address_gone", r.srv.outsideView().inviteEnded);
    FakePhone dev(3);
    const auto res = dev.pair("127.0.0.1", r.outsidePort, link->hostCertSHA256, PairingMethod::Invite, link->token);
    KV_CHECK_MSG(!res.ok, "an invite the PC no longer shows was redeemed");
    KV_CHECK(r.srv.devices().empty());
}

// Why the invite ended, as the PC's window says it.
KV_TEST(outside_the_pc_learns_why_an_invite_ended) {
    Rig r(L"h2-why");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(r.srv.outsideView().inviteEnded.empty());
    KV_REQUIRE(inviteOf(r.srv, PetRole::Viewer).has_value());
    r.srv.cancelOutsideInvite();
    KV_CHECK(r.srv.outsideView().inviteEnded == "cancelled");
    const auto link = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link.has_value());
    KV_CHECK(r.srv.outsideView().inviteEnded.empty());
    FakePhone dev(3);
    const auto res = dev.pair("127.0.0.1", r.outsidePort, link->hostCertSHA256, PairingMethod::Invite, link->token);
    KV_CHECK_MSG(res.ok, res.error);
    KV_CHECK(r.srv.outsideView().inviteEnded == "used");
    const auto link2 = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link2.has_value());
    Bytes wrong = link2->token;
    wrong[3] ^= 0x11;
    for (int i = 1; i <= outside::kInviteWrongAttemptsToBurn; ++i) { // the tenth wrong attempt burns it
        r.src->push("203.0.113." + std::to_string(100 + i));
        FakePhone guess(3);
        KV_CHECK(!guess.pair("127.0.0.1", r.outsidePort, link2->hostCertSHA256, PairingMethod::Invite, wrong).ok);
    }
    KV_CHECK(r.srv.outsideView().inviteEnded == "burned");
    KV_REQUIRE(inviteOf(r.srv, PetRole::Viewer).has_value());
    std::string err;
    KV_CHECK(r.srv.setAllowOutside(false, &err));
    KV_CHECK(r.srv.outsideView().inviteEnded == "outside_off");
}

// An address change after a failed check is still an address change (§8.3), with its notices.
KV_TEST(outside_an_address_change_after_a_failed_check_is_still_address_changed) {
    Rig r(L"h3-addr");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto po = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(po.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped)); // at 203.0.113.142
    std::string err;
    KV_REQUIRE(owner.connectStream("127.0.0.1", po.streamPort, po.serverHash, &err));
    Seen so;
    KV_REQUIRE(pump(owner, so, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_REQUIRE(inviteOf(r.srv, PetRole::Viewer).has_value());
    r.router->setWan("0.0.0.0"); // the router lost its internet connection for a while
    r.srv.outsideRecheckNowForTest();
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Error));
    KV_CHECK(!r.srv.currentOutsideInvite().has_value()); // no endpoint, so it closed
    r.router->setWan("203.0.113.143"); // back, with a new address
    r.srv.outsideRecheckNowForTest();
    KV_CHECK(r.waitStatus(outside::OutsideStatus::AddressChanged));
    // Against the address devices were last told (.142), so the failed check in between can't hide it; and, as for
    // any change, not told until the PC user re-shares: the owner sees the status and keeps .142.
    KV_CHECK(pump(owner, so, 5000, [](const Seen& x) {
        return !x.states.empty() && x.states.back().outsideStatus == std::string("address_changed");
    }));
    KV_CHECK(!so.closed && !hasNotice(so, notice_reason::kAddressChanged));
    KV_CHECK(so.states.back().outsideEndpoint && so.states.back().outsideEndpoint->host == "203.0.113.142");
    // It stays address_changed through another failed check, until the PC user acknowledges it.
    r.router->setWan("0.0.0.0");
    r.srv.outsideRecheckNowForTest();
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Error));
    r.router->setWan("203.0.113.143");
    r.srv.outsideRecheckNowForTest();
    KV_CHECK(r.waitStatus(outside::OutsideStatus::AddressChanged));
    r.srv.acknowledgeAddressChange("203.0.113.143");
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(pump(owner, so, 5000, [](const Seen& x) { // now told, with the notice
        if (!hasNotice(x, notice_reason::kAddressChanged)) return false;
        for (const auto& st : x.states) {
            if (st.outsideEndpoint && st.outsideEndpoint->host == "203.0.113.143") return true;
        }
        return false;
    }));
}

// The default port is picked once and saved (§8.1), so the next start keeps it.
KV_TEST(outside_the_default_outside_port_is_saved_and_kept_across_restarts) {
    Rig r(L"h4-port");
    KV_REQUIRE(r.start());
    const uint16_t port = r.srv.petSettings().outsidePort;
    KV_REQUIRE(outside::isValidOutsidePort(port));
    // Written as soon as pet-camera.json can be written, not only with a later change.
    KV_CHECK(waitFor([&] {
        std::string text;
        return readWholeFile(r.state.file(L"pet-camera.json"), text) &&
               text.find("\"outsidePort\":" + std::to_string(port)) != std::string::npos;
    }));
    r.srv.shutdown();
    HostStreamServer again;
    std::string err;
    KV_REQUIRE(again.start(r.options(), &err));
    KV_CHECK_MSG(again.petSettings().outsidePort == port, "the default port changed across a restart");
    again.shutdown();
}

// ---- §8.7: the default port is the first of up to 20 random ports that binds ---------------------------------

namespace {

// The outside_port_probe seam: records every port it is asked about and answers by the test's rule. Nothing binds.
struct FakeProbe {
    std::mutex m;
    std::vector<uint16_t> asked;
    int reject_first = 0;    // the first N ports asked about don't bind
    bool reject_all = false; // no port binds

    static std::function<bool(uint16_t)> of(const std::shared_ptr<FakeProbe>& p) {
        return [p](uint16_t port) {
            std::lock_guard<std::mutex> lk(p->m);
            p->asked.push_back(port);
            if (p->reject_all) return false;
            return static_cast<int>(p->asked.size()) > p->reject_first;
        };
    }
    std::vector<uint16_t> seen() {
        std::lock_guard<std::mutex> lk(m);
        return asked;
    }
    void restart(bool reject_everything) {
        std::lock_guard<std::mutex> lk(m);
        asked.clear();
        reject_first = 0;
        reject_all = reject_everything;
    }
};

bool inDefaultRange(uint16_t p) { return p >= 20000 && p <= 44999; }

bool petJsonHas(const kvtest::OutsideTempState& state, const std::string& needle) {
    std::string text;
    return readWholeFile(state.file(L"pet-camera.json"), text) && text.find(needle) != std::string::npos;
}

// The listener couldn't open on a port the fake probe let through, for a reason of this PC's (in use, or one it
// won't bind on loopback): not what these tests check, so the test runs again with new random ports.
bool realBindRefused(HostStreamServer& srv) {
    const OutsideView v = srv.outsideView();
    if (v.listening || v.status != outside::OutsideStatus::Error ||
        v.sentence.find("can't be opened on this PC") == std::string::npos) {
        return false;
    }
    std::printf("  port %u can't be bound on this PC (%s); again with other ports\n", static_cast<unsigned>(v.port),
                v.sentence.c_str());
    return true;
}

const char* const kNoBindablePortSentence =
    "Couldn't find a port this PC can open for viewing from outside the home. Enter a port below.";

} // namespace

// (a) The probe refuses the first three ports: the fourth is the default, saved in pet-camera.json, and the
// outside listener opens on it when outside viewing turns on.
KV_TEST(outside_the_default_port_is_the_first_random_port_that_binds) {
    for (int run = 0; run < 3; ++run) {
        auto probe = std::make_shared<FakeProbe>();
        probe->reject_first = 3;
        Rig r(L"probe-first");
        KV_REQUIRE(r.start([probe](HostStreamServer::Options& o) { o.outside_port_probe = FakeProbe::of(probe); }));
        r.srv.setSharingEnabled(true);
        std::string err;
        KV_REQUIRE(r.srv.setAllowOutside(true, &err));
        const std::vector<uint16_t> asked = probe->seen();
        KV_REQUIRE(asked.size() == 4); // asked once each, the fourth bound; turning it on asked nothing more
        const uint16_t chosen = asked[3];
        KV_CHECK(inDefaultRange(chosen));
        KV_CHECK_MSG(r.srv.petSettings().outsidePort == chosen, std::to_string(r.srv.petSettings().outsidePort));
        for (size_t i = 0; i < 3; ++i) KV_CHECK(asked[i] != chosen); // a refused port is never the one saved
        KV_CHECK(waitFor([&] { return petJsonHas(r.state, "\"outsidePort\":" + std::to_string(chosen)); }));
        if (r.srv.outsideListeningPort() != chosen && realBindRefused(r.srv)) continue;
        KV_CHECK(r.srv.outsideListeningPort() == chosen);
        const OutsideView v = r.srv.outsideView();
        KV_CHECK(v.listening && v.port == chosen);
        return;
    }
    KV_CHECK_MSG(false, "the listener couldn't open on three runs' ports");
}

// (b) No port binds: 20 tries, then an error on the PC's status line, nothing listening and no port saved. The
// next turn-on tries again, and a port that binds is then picked, saved and listened on.
KV_TEST(outside_no_port_that_binds_is_an_error_saves_none_and_is_tried_again) {
    for (int run = 0; run < 3; ++run) {
        auto probe = std::make_shared<FakeProbe>();
        probe->reject_all = true;
        Rig r(L"probe-none");
        KV_REQUIRE(r.start([probe](HostStreamServer::Options& o) { o.outside_port_probe = FakeProbe::of(probe); }));
        // The start's own pick (no port saved yet) found none either: 20 tries, nothing picked.
        KV_CHECK_MSG(probe->seen().size() == 20, std::to_string(probe->seen().size()));
        KV_CHECK(r.srv.petSettings().outsidePort == 0);
        r.srv.setSharingEnabled(true);
        probe->restart(true);
        std::string err;
        KV_REQUIRE(r.srv.setAllowOutside(true, &err));
        KV_CHECK_MSG(probe->seen().size() == 20, std::to_string(probe->seen().size()));
        for (uint16_t p : probe->seen()) KV_CHECK(inDefaultRange(p));
        {
            const OutsideView v = r.srv.outsideView();
            KV_CHECK(v.allowed && !v.listening);
            KV_CHECK(v.status == outside::OutsideStatus::Error);
            KV_CHECK_MSG(v.sentence == kNoBindablePortSentence, v.sentence);
            KV_CHECK(r.srv.outsideListeningPort() == 0);
            KV_CHECK(r.srv.petSettings().outsidePort == 0);
        }
        // Saved with outside on, and no port at all.
        KV_CHECK(waitFor([&] { return petJsonHas(r.state, "\"allowOutside\":true"); }));
        KV_CHECK(!petJsonHas(r.state, "\"outsidePort\""));
        {
            const OutsideView v = r.srv.outsideView(); // still shown once the save is done (not wiped)
            KV_CHECK(v.status == outside::OutsideStatus::Error && v.sentence == kNoBindablePortSentence);
        }
        // Off, then on again: a port that binds now is picked, saved and listened on.
        probe->restart(false);
        KV_REQUIRE(r.srv.setAllowOutside(false, &err));
        KV_CHECK(r.srv.outsideView().status == outside::OutsideStatus::Off);
        KV_REQUIRE(r.srv.setAllowOutside(true, &err));
        const std::vector<uint16_t> asked = probe->seen();
        KV_REQUIRE(asked.size() == 1);
        const uint16_t chosen = asked[0];
        KV_CHECK(inDefaultRange(chosen));
        KV_CHECK(r.srv.petSettings().outsidePort == chosen);
        KV_CHECK(waitFor([&] { return petJsonHas(r.state, "\"outsidePort\":" + std::to_string(chosen)); }));
        if (r.srv.outsideListeningPort() != chosen && realBindRefused(r.srv)) continue;
        KV_CHECK(r.srv.outsideListeningPort() == chosen);
        KV_CHECK(r.srv.outsideView().listening);
        return;
    }
    KV_CHECK_MSG(false, "the listener couldn't open on three runs' ports");
}

// (c) Every port the picker asks about is in 20000-44999, and none twice.
KV_TEST(outside_the_default_port_picker_asks_only_about_ports_in_its_range) {
    auto probe = std::make_shared<FakeProbe>();
    probe->reject_first = 15;
    Rig r(L"probe-range");
    KV_REQUIRE(r.start([probe](HostStreamServer::Options& o) { o.outside_port_probe = FakeProbe::of(probe); }));
    const std::vector<uint16_t> asked = probe->seen();
    KV_CHECK_MSG(asked.size() == 16, std::to_string(asked.size()));
    for (uint16_t p : asked) KV_CHECK_MSG(inDefaultRange(p), std::to_string(p));
    KV_CHECK(std::set<uint16_t>(asked.begin(), asked.end()).size() == asked.size());
    KV_CHECK(!asked.empty() && r.srv.petSettings().outsidePort == asked.back());
}

// (d) A port the PC user chose is taken as given (the probe isn't asked), and one that can't be bound gets the
// existing sentence.
KV_TEST(outside_a_chosen_port_that_cant_be_bound_gets_the_existing_sentence) {
    winsockInit();
    SOCKET holder = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); // holds a loopback port, so it can't be bound
    KV_REQUIRE(holder != INVALID_SOCKET);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(a);
    const bool held = bind(holder, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(holder, 1) == 0 &&
                      getsockname(holder, reinterpret_cast<sockaddr*>(&a), &len) == 0;
    const uint16_t port = ntohs(a.sin_port);
    if (!held) {
        closesocket(holder);
        KV_REQUIRE(held);
    }
    {
        auto probe = std::make_shared<FakeProbe>();
        Rig r(L"probe-chosen");
        if (r.start([probe](HostStreamServer::Options& o) { o.outside_port_probe = FakeProbe::of(probe); })) {
            const size_t before = probe->seen().size();
            r.srv.setSharingEnabled(true);
            std::string err;
            KV_CHECK(r.srv.setOutsidePort(port, &err));
            KV_CHECK(r.srv.setAllowOutside(true, &err));
            KV_CHECK(probe->seen().size() == before); // a chosen port is never probed
            const OutsideView v = r.srv.outsideView();
            KV_CHECK(!v.listening && v.status == outside::OutsideStatus::Error && v.port == port);
            const std::string head = "Port " + std::to_string(port) + " can't be opened on this PC (";
            KV_CHECK_MSG(v.sentence.rfind(head, 0) == 0, v.sentence);
            const std::string tail = "). Pick another port.";
            KV_CHECK_MSG(v.sentence.size() > tail.size() &&
                             v.sentence.compare(v.sentence.size() - tail.size(), tail.size(), tail) == 0,
                         v.sentence);
            KV_CHECK(r.srv.petSettings().outsidePort == port); // kept as given
        } else {
            KV_CHECK_MSG(false, "the server didn't start");
        }
    }
    closesocket(holder);
}

// A slow connection log write holds up nothing else.
KV_TEST(outside_a_slow_connection_log_write_never_holds_up_the_server) {
    Rig r(L"h5-log");
    auto gate = std::make_shared<Gate>();
    KV_REQUIRE(r.start([gate](HostStreamServer::Options& o) {
        o.connection_log_flush_ms = 20;
        o.connection_log_write_hook = [gate] { gate->pass(); }; // every write waits here until released
    }));
    // A refused LAN connection makes a line; the maintenance loop starts writing it, and the write is held.
    r.src->setStream("203.0.113.201");
    std::string cerr;
    SOCKET a = tcpConnect("127.0.0.1", r.srv.streamPort(), 2000, &cerr);
    KV_REQUIRE(gate->waitEntered());
    // While it is held, more refused connections are logged on the accept thread...
    for (int i = 0; i < 3; ++i) {
        const int n = r.src->streamTaken();
        SOCKET b = tcpConnect("127.0.0.1", r.srv.streamPort(), 2000, &cerr);
        KV_CHECK(waitFor([&] { return r.src->streamTaken() > n; }));
        KV_CHECK(closedWithin(b, 3000)); // closed before TLS, so the accept thread is past its log line
        closesocket(b);
    }
    // ...and the server's lock is free: the PC's view and the session count answer at once.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([&r, done] {
        for (int i = 0; i < 10; ++i) {
            (void)r.srv.outsideView();
            (void)r.srv.sessionCount();
        }
        done->store(true);
    });
    const bool prompt = waitFor([&] { return done->load(); }, 2000);
    KV_CHECK_MSG(prompt, "the server waited on the connection log's file write");
    gate->release();
    t.join();
    closesocket(a);
    KV_CHECK(r.logHas("refused_local_only", "home", "203.0.113.201"));
}

// A flaky link never blocks a source; five unknown certificates do.
KV_TEST(outside_a_flaky_link_never_blocks_a_source_and_five_unknown_certificates_do) {
    Rig r(L"flaky-link");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_handshake_timeout_ms = 400; // the stall case, quickly
        o.outside_limits.burst = 100;         // only the failures are in play here
        o.outside_limits.newPerMinute = 1000;
    }));
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    const std::string src = "203.0.113.55";
    auto settled = [&] { return waitFor([&] { return lim->inFlightFor(src) == 0; }, 5000); };
    // The first bytes of a ClientHello: a TLS record header announcing 512 bytes, then a few of them.
    std::vector<char> part = {0x16, 0x03, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, static_cast<char>(0xfc), 0x03, 0x03};
    part.resize(64, 0x2a);
    for (int round = 0; round < 3; ++round) {
        // Dropped before the ClientHello.
        closesocket(r.rawFrom(src));
        KV_CHECK(settled());
        // Dropped mid-handshake (part of the ClientHello, then a close).
        SOCKET s = r.rawFrom(src);
        send(s, part.data(), static_cast<int>(part.size()), 0);
        closesocket(s);
        KV_CHECK(settled());
        // A reset mid-handshake.
        s = r.rawFrom(src);
        send(s, part.data(), static_cast<int>(part.size()), 0);
        closeWithReset(s);
        KV_CHECK(settled());
        // A stall past the handshake deadline.
        s = r.rawFrom(src);
        send(s, part.data(), 20, 0);
        KV_CHECK(closedWithin(s, 5000));
        closesocket(s);
        KV_CHECK(settled());
        // Bytes that aren't TLS at all (a protocol error before any certificate).
        s = r.rawFrom(src);
        const char http[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        send(s, http, static_cast<int>(sizeof(http) - 1), 0);
        KV_CHECK(closedWithin(s, 3000));
        closesocket(s);
        KV_CHECK(settled());
    }
    KV_CHECK_MSG(lim->failuresFor(src) == 0, "a dropped or stalled handshake counted as a failure");
    KV_CHECK(!lim->isBlocked(src));
    KV_CHECK(r.logHas("disconnected", "outside", src));
    // Five real handshakes whose certificate matches no device (no invite open) block it.
    for (int i = 0; i < 5; ++i) {
        FakePhone stranger(3);
        Seen s;
        if (r.connectOutside(stranger, src, std::nullopt)) pump(stranger, s, 1500, [](const Seen& x) { return x.closed; });
        stranger.close();
        KV_CHECK(settled());
    }
    KV_CHECK(lim->failuresFor(src) == 5 || lim->isBlocked(src));
    KV_CHECK(lim->isBlocked(src));
    KV_CHECK(r.logHas("refused_unknown_cert", "outside", src));
}

// The outside handshake deadline is 5 s, and a timeout isn't a failure: five slow sockets don't block a source.
KV_TEST(outside_the_outside_handshake_deadline_is_five_seconds_and_not_a_failure) {
    Rig r(L"t1-deadline");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        KV_CHECK(o.outside_handshake_timeout_ms == 5000); // the default, unchanged here
        KV_CHECK(o.outside_first_record_ms == 1500);       // the first-record deadline's default
        o.outside_first_record_ms = 15000; // silent sockets here wait for the handshake deadline, not the first record's
        o.outside_limits.burst = 10;
        o.outside_limits.inFlightPerSource = 5; // all five at once, so the test waits 5 s, not 15
    }));
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    const std::string src = "203.0.113.56";
    std::vector<SOCKET> slow;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) slow.push_back(r.rawFrom(src));
    KV_CHECK(waitFor([&] { return lim->inFlightFor(src) == 5; }));
    for (SOCKET s : slow) KV_CHECK(closedWithin(s, 9000));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  five silent handshakes closed after %lld ms\n", static_cast<long long>(took));
    KV_CHECK(took >= 4500 && took <= 8000);
    for (SOCKET s : slow) closesocket(s);
    KV_CHECK(waitFor([&] { return lim->inFlightFor(src) == 0; }));
    KV_CHECK(lim->failuresFor(src) == 0 && !lim->isBlocked(src));
    KV_CHECK(r.logHas("disconnected", "outside", src));
}

namespace {
// The host closed a (non-blocking) socket within `ms`: an orderly end or a reset. Data the host sends is read and
// thrown away; our own wait running out is not a close.
bool hostClosed(SOCKET s, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    char buf[4096];
    while (std::chrono::steady_clock::now() < until) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n == 0) return true;
        if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) return true;
        if (n < 0) Sleep(20);
    }
    return false;
}

// This process's CPU time so far, in ms (the host runs in it).
uint64_t cpuMs() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](FILETIME f) { return ((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000; };
    return ms(k) + ms(u);
}

} // namespace

// At default settings: a ClientHello and then a stall is held to the 5 s handshake deadline, and
// waiting on it doesn't spin a core. Before, once the first-record deadline had passed, every wait was 0 ms: about
// 3.5 s of one core per such connection.
KV_TEST(outside_a_clienthello_then_a_stall_holds_no_core_and_ends_at_the_handshake_deadline) {
    Rig r(L"outside-hello-stall");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        KV_CHECK(o.outside_handshake_timeout_ms == 5000 && o.outside_first_record_ms == 1500); // the defaults
    }));
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    Sleep(500);
    const uint64_t cpu0 = cpuMs();
    const auto t0 = std::chrono::steady_clock::now();
    auto h = r.helloFrom("203.0.113.130");
    KV_REQUIRE(h.ssl != nullptr);
    KV_CHECK(hostClosed(h.s, 9000));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const uint64_t spent = cpuMs() - cpu0;
    std::printf("  closed after %lld ms, %llu ms of CPU\n", static_cast<long long>(took), static_cast<unsigned long long>(spent));
    KV_CHECK_MSG(took >= 4500 && took <= 8000, std::to_string(took)); // the handshake deadline, not the first record's
    KV_CHECK_MSG(spent < 1500, std::to_string(spent) + " ms of CPU: the wait spun");
    KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.130") == 0; }));
    KV_CHECK(lim->failuresFor("203.0.113.130") == 0);
    Rig::drop(h);
}

// Handshakes that haven't completed (here, a ClientHello and a stall) are evicted one each for
// newcomers, each a different one: an entry already evicted is never picked again (that refused the newcomer).
KV_TEST(outside_each_newcomer_evicts_a_different_unfinished_handshake) {
    Rig r(L"outside-evict-each");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.outside_handshake_timeout_ms = 15000;
        // Evicted handshakes stay in the list a while, so each later newcomer meets them there and must pass them
        // over (without this the row couldn't go red).
        o.aborted_linger_ms_for_test = 1500;
    }));
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    std::vector<Rig::Stalled> stalled;
    for (int i = 0; i < 8; ++i) stalled.push_back(r.helloFrom("203.0.113." + std::to_string(140 + i)));
    KV_REQUIRE(waitFor([&] { return lim->inFlight() == 8; }));
    std::vector<SOCKET> newcomers;
    for (int i = 0; i < 3; ++i) newcomers.push_back(r.rawFrom("203.0.113." + std::to_string(160 + i)));
    for (SOCKET n : newcomers) KV_CHECK_MSG(!refusedSoon(n, 300), "a newcomer was refused, not let in over an unfinished handshake");
    KV_CHECK(waitFor([&] { return evictionsLogged(r.srv) == 3; }));
    for (int i = 0; i < 3; ++i) KV_CHECK(!r.logHas("refused_too_many", "outside", "203.0.113." + std::to_string(160 + i), 200));
    int closed = 0;
    for (auto& h : stalled) closed += hostClosed(h.s, 200) ? 1 : 0;
    KV_CHECK_MSG(closed == 3, std::to_string(closed) + " of the stalled were dropped");
    for (SOCKET n : newcomers) closesocket(n);
    for (auto& h : stalled) Rig::drop(h);
}

// Eviction takes from the most-represented IPv6 /48 first, so one site's many /64s give way before
// single addresses that came earlier.
KV_TEST(outside_eviction_takes_from_the_most_represented_48_first) {
    Rig r(L"outside-evict-48");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.outside_handshake_timeout_ms = 15000; }));
    KV_REQUIRE(r.enableOutside());
    const auto lim = r.srv.outsideLimiterForTest();
    std::vector<Rig::Stalled> v4, v6;
    for (int i = 0; i < 4; ++i) v4.push_back(r.helloFrom("203.0.113." + std::to_string(170 + i))); // first, so oldest
    for (int i = 0; i < 4; ++i) v6.push_back(r.helloFrom("2001:db8:cc:" + std::to_string(i + 1) + "::7")); // one /48
    KV_REQUIRE(waitFor([&] { return lim->inFlight() == 8; }));
    SOCKET n = r.rawFrom("203.0.113.180");
    KV_CHECK(!refusedSoon(n, 300));
    KV_CHECK(waitFor([&] { return evictionsLogged(r.srv) == 1; }));
    int v4closed = 0, v6closed = 0;
    for (auto& h : v4) v4closed += hostClosed(h.s, 200) ? 1 : 0;
    for (auto& h : v6) v6closed += hostClosed(h.s, 200) ? 1 : 0;
    KV_CHECK_MSG(v4closed == 0 && v6closed == 1, "v4 " + std::to_string(v4closed) + ", v6 " + std::to_string(v6closed));
    closesocket(n);
    for (auto& h : v4) Rig::drop(h);
    for (auto& h : v6) Rig::drop(h);
}

// A late report for the old port never shows the new port as mapped.
KV_TEST(outside_a_late_report_for_the_old_port_never_shows_the_new_one_as_mapped) {
    Rig r(L"h7-stale");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.router_timing.callDeadlineMs = 20000; }));
    const auto ports = twoFreePorts();
    r.router->holdAdd(ports.first, true);
    KV_REQUIRE(r.enableOutside(ports.first));
    KV_REQUIRE(waitFor([&] { return r.router->addsEntered() >= 1; })); // the worker is in Add
    r.router->holdRemove(true);
    std::string err;
    KV_REQUIRE(r.srv.setOutsidePort(ports.second, &err));
    KV_CHECK(r.srv.outsideListeningPort() == ports.second);
    r.outsidePort = ports.second;
    r.router->holdAdd(ports.first, false); // P1's Add answers now: a report for P1 comes back
    KV_REQUIRE(waitFor([&] { return r.router->removesEntered() >= 1; })); // then the close removes P1 (held)
    OutsideView v = r.srv.outsideView();
    KV_CHECK_MSG(!(v.endpointKnown && v.endpointVerified), "P2 shown as reachable while only P1 was mapped");
    KV_CHECK(v.status != outside::OutsideStatus::Mapped);
    r.router->holdRemove(false);
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    v = r.srv.outsideView();
    KV_CHECK(v.endpointKnown && v.endpointVerified && v.port == ports.second);
    KV_CHECK(r.router->has(ports.second) && !r.router->has(ports.first));
}

// A port change while listening moves the listener and the mapping; sessions stay; the invite closes.
KV_TEST(outside_a_port_change_while_listening_keeps_sessions_and_closes_the_invite) {
    Rig r(L"t6-port");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto po = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(po.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const uint16_t old_port = r.outsidePort;
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.92", po.serverHash));
    Seen so;
    KV_REQUIRE(pump(owner, so, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_REQUIRE(inviteOf(r.srv, PetRole::Viewer).has_value());
    uint16_t new_port = freeLoopbackPort();
    for (int i = 0; i < 10 && new_port == old_port; ++i) new_port = freeLoopbackPort();
    std::string err;
    KV_REQUIRE(r.srv.setOutsidePort(new_port, &err));
    KV_CHECK(r.srv.outsideListeningPort() == new_port);
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
    KV_CHECK(r.srv.outsideView().inviteEnded == "port_changed");
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(pump(owner, so, 5000, [new_port](const Seen& x) {
        return !x.states.empty() && x.states.back().outsideEndpoint && x.states.back().outsideEndpoint->port == new_port;
    }));
    KV_CHECK(!so.closed && !hasNotice(so, notice_reason::kOutsideDisabled));
    KV_CHECK(r.router->has(new_port) && !r.router->has(old_port));
    std::string cerr;
    SOCKET old_sock = tcpConnect("127.0.0.1", old_port, 1000, &cerr);
    KV_CHECK(old_sock == INVALID_SOCKET); // nothing listens on the old port
    if (old_sock != INVALID_SOCKET) closesocket(old_sock);
}

// A forward added while the router was stuck is removed once the router answers, after outside turned off.
KV_TEST(outside_a_forward_added_while_the_router_was_stuck_is_removed_once_it_answers) {
    Rig r(L"h8-stuck");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.router_timing.callDeadlineMs = 300; }));
    const uint16_t port = freeLoopbackPort();
    r.router->holdAdd(port, true);
    KV_REQUIRE(r.enableOutside(port));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual)); // the Add was given up at the deadline
    std::string err;
    KV_CHECK(r.srv.setAllowOutside(false, &err)); // the removal can't run yet: the Add still holds the router
    r.router->holdAdd(port, false); // the router answers the Add at last: the forward is made...
    KV_CHECK_MSG(waitFor([&] { return !r.router->has(port) && r.router->removed() >= 1; }, 6000),
                 "the forward stayed on the router");
    KV_CHECK(waitFor([&] { return r.srv.petSettings().outsideMappedPort == 0; }));
}

// A committed redemption gets its reply, even when outside turns off meanwhile.
KV_TEST(outside_a_committed_redemption_gets_its_reply_while_outside_turns_off) {
    Rig r(L"h9-commit");
    HostStreamServer* srv = &r.srv;
    auto gate = std::make_shared<Gate>();
    KV_REQUIRE(r.start([gate, srv](HostStreamServer::Options& o) {
        o.outside_test_hook = [gate, srv](const char* where) {
            if (std::strcmp(where, "redeem_committed") != 0) return;
            {
                std::lock_guard<std::mutex> lk(gate->m);
                ++gate->entered;
                gate->cv.notify_all();
            }
            // Held until the turn-off has closed the listener and aborted every handshake.
            waitFor([srv] { return srv->outsideListeningPort() == 0; }, 5000);
        };
    }));
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const auto link = inviteOf(r.srv, PetRole::Owner);
    KV_REQUIRE(link.has_value());
    std::thread off([&r, gate] {
        if (!gate->waitEntered(1, 10000)) return;
        std::string err;
        r.srv.setAllowOutside(false, &err);
    });
    FakePhone dev(3);
    dev.setDeviceName("Late reply phone");
    const auto res = dev.pair("127.0.0.1", r.outsidePort, link->hostCertSHA256, PairingMethod::Invite, link->token);
    off.join();
    KV_CHECK_MSG(res.ok, "the committed redemption lost its reply: " + res.error);
    KV_CHECK(deviceNamed(r.srv, "Late reply phone").role == PetRole::Owner);
    KV_CHECK(r.srv.outsideListeningPort() == 0);
}

// A forward an earlier run left (outsideMappedPort) is removed at the next start, only if it is ours.
KV_TEST(outside_a_forward_left_by_an_earlier_run_is_removed_at_start_only_if_ours) {
    for (const bool ours : {true, false}) {
        Rig r(ours ? L"h10-ours" : L"h10-theirs");
        const uint16_t port = freeLoopbackPort();
        KV_REQUIRE(kvtest::writeRaw(r.state.file(L"pet-camera.json"),
                                    "{\"sharingEnabled\":true,\"allowOutside\":false,\"outsidePort\":" + std::to_string(port) +
                                        ",\"outsideMappedPort\":" + std::to_string(port) + "}"));
        r.router->maps[port] = outside::RouterMapping{kTestLanIPv4, port, true, std::string(),
                                                      ours ? outside::kMappingDescription : "Someone else's game"};
        KV_REQUIRE(r.start());
        KV_CHECK(waitFor([&] { return r.srv.petSettings().outsideMappedPort == 0; }));
        if (ours) {
            KV_CHECK_MSG(waitFor([&] { return !r.router->has(port); }), "the leftover forward wasn't removed");
        } else {
            KV_CHECK(r.router->has(port) && r.router->removed() == 0); // never someone else's
        }
        KV_CHECK(waitFor([&] {
            std::string text;
            return readWholeFile(r.state.file(L"pet-camera.json"), text) && text.find("outsideMappedPort") == std::string::npos;
        }));
    }
    // Written when mapped, cleared when removed.
    Rig r(L"h10-key");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(waitFor([&] { return r.srv.petSettings().outsideMappedPort == r.outsidePort; }));
    KV_CHECK(waitFor([&] {
        std::string text;
        return readWholeFile(r.state.file(L"pet-camera.json"), text) &&
               text.find("\"outsideMappedPort\":" + std::to_string(r.outsidePort)) != std::string::npos;
    }));
    std::string err;
    KV_CHECK(r.srv.setAllowOutside(false, &err));
    KV_CHECK(waitFor([&] { return r.srv.petSettings().outsideMappedPort == 0 && r.router->mappings() == 0; }));
}

// "Remove mapping now" removes the forward and keeps the listener. This replaces
// "the next check maps it again while on": it stays removed, and the 10-minute check doesn't add it back.
KV_TEST(outside_remove_mapping_now_removes_the_forward_and_keeps_listening) {
    Rig r(L"v3-remove");
    KV_REQUIRE(r.start());
    std::string err;
    KV_CHECK(!r.srv.removeOutsideMappingNow(&err) && !err.empty()); // nothing to remove while off
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(r.router->has(r.outsidePort));
    KV_CHECK_MSG(r.srv.removeOutsideMappingNow(&err), err);
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual));
    KV_CHECK(!r.router->has(r.outsidePort));
    const OutsideView v = r.srv.outsideView();
    KV_CHECK(v.listening && r.srv.outsideListeningPort() == r.outsidePort);
    KV_CHECK(!v.manualSteps.empty() && !v.endpointKnown);
    KV_CHECK(!r.srv.removeOutsideMappingNow(&err)); // nothing left to remove
    KV_CHECK(waitFor([&] { return r.srv.petSettings().outsideMappedPort == 0; })); // the removal is confirmed
    const int adds = r.router->added();
    const uint64_t checks = r.srv.outsideRechecksForTest();
    r.srv.outsideRecheckNowForTest(); // the 10-minute check, while outside listening is still on
    KV_CHECK(waitFor([&] { return r.srv.outsideRechecksForTest() > checks; })); // it has been through the worker
    KV_CHECK_MSG(!waitFor([&] { return r.router->added() != adds || r.router->has(r.outsidePort); }, 3000),
                 "the 10-minute check added back a mapping the owner removed");
    KV_CHECK(r.router->added() == adds && !r.router->has(r.outsidePort));
    const OutsideView after = r.srv.outsideView();
    KV_CHECK_MSG(after.status == outside::OutsideStatus::Manual && after.listening && !after.endpointKnown,
                 std::string(outside::outsideStatusName(after.status)));
    KV_CHECK(after.mappingRemoved && !after.manualSteps.empty());
    KV_CHECK(r.srv.petSettings().outsideMappedPort == 0);
    KV_CHECK(r.srv.petSettings().outsideMappingRemoved);
}

// ---- "Remove mapping now" stays removed --------------------------------------------------------------------

// "Map the port again" maps it now, sets outsideMappedPort again and clears the flag; refused when nothing was removed.
KV_TEST(outside_map_the_port_again_maps_a_removed_forward) {
    Rig r(L"r2-remap");
    KV_REQUIRE(r.start());
    std::string err;
    KV_CHECK(!r.srv.remapOutsideNow(&err) && !err.empty()); // off: nothing to map again
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(!r.srv.remapOutsideNow(&err) && !err.empty()); // not removed: nothing to map again
    KV_REQUIRE(r.srv.removeOutsideMappingNow(&err));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual));
    KV_REQUIRE(waitFor([&] { return r.srv.petSettings().outsideMappedPort == 0; }));
    KV_CHECK(waitFor([&] { return petJsonHas(r.state, "\"outsideMappingRemoved\":true"); }));
    const int adds = r.router->added();
    KV_CHECK_MSG(r.srv.remapOutsideNow(&err), err);
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(r.router->added() == adds + 1 && r.router->has(r.outsidePort));
    KV_CHECK(waitFor([&] { return r.srv.petSettings().outsideMappedPort == r.outsidePort; }));
    KV_CHECK(!r.srv.petSettings().outsideMappingRemoved && !r.srv.outsideView().mappingRemoved);
    KV_CHECK(waitFor([&] { return petJsonHas(r.state, "\"outsideMappingRemoved\":false"); }));
    // Mapped again, the checks keep it as before (a router reboot that lost it gets it back).
    {
        std::lock_guard<std::mutex> lk(r.router->m);
        r.router->maps.clear();
    }
    r.srv.outsideRecheckNowForTest();
    KV_CHECK(waitFor([&] { return r.router->has(r.outsidePort); }));
}

// Turning outside viewing off clears the flag, so turning it on again maps.
KV_TEST(outside_turning_outside_off_and_on_maps_a_removed_forward_again) {
    Rig r(L"r2-offon");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    KV_REQUIRE(r.srv.removeOutsideMappingNow(&err));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual));
    KV_REQUIRE(r.srv.petSettings().outsideMappingRemoved);
    KV_REQUIRE(r.srv.setAllowOutside(false, &err));
    KV_CHECK(!r.srv.petSettings().outsideMappingRemoved);
    KV_CHECK(waitFor([&] { return petJsonHas(r.state, "\"outsideMappingRemoved\":false"); }));
    const int adds = r.router->added();
    KV_REQUIRE(r.srv.setAllowOutside(true, &err));
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(r.router->added() == adds + 1 && r.router->has(r.outsidePort));
    KV_CHECK(waitFor([&] { return r.srv.petSettings().outsideMappedPort == r.outsidePort; }));
    // Sharing off turns outside viewing off too, and clears it the same way.
    KV_REQUIRE(r.srv.removeOutsideMappingNow(&err));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual));
    r.srv.setSharingEnabled(false);
    KV_CHECK(!r.srv.petSettings().outsideMappingRemoved && !r.srv.petSettings().allowOutside);
}

// Across a restart it stays removed: with outside viewing on and the flag set, the listener opens and nothing is
// mapped. A value that isn't a bool reads as removed (never map on a bad value); a real false maps.
KV_TEST(outside_a_removed_forward_stays_removed_across_a_restart) {
    struct Case {
        const wchar_t* tag;
        const char* value;
        bool removed;
    };
    for (const Case c : {Case{L"r2-restart-true", "true", true}, Case{L"r2-restart-str", "\"no\"", true},
                         Case{L"r2-restart-num", "0", true}, Case{L"r2-restart-null", "null", true},
                         Case{L"r2-restart-false", "false", false}}) {
        Rig r(c.tag);
        const uint16_t port = freeLoopbackPort();
        KV_REQUIRE(kvtest::writeRaw(r.state.file(L"pet-camera.json"),
                                    std::string("{\"sharingEnabled\":true,\"allowOutside\":true,\"outsidePort\":") +
                                        std::to_string(port) + ",\"outsideMappingRemoved\":" + c.value + "}"));
        KV_REQUIRE(r.start());
        r.outsidePort = port;
        KV_CHECK_MSG(r.srv.outsideListeningPort() == port, c.value);
        KV_CHECK_MSG(r.srv.petSettings().outsideMappingRemoved == c.removed, c.value);
        if (c.removed) {
            KV_CHECK_MSG(r.waitStatus(outside::OutsideStatus::Manual), c.value);
            const uint64_t checks = r.srv.outsideRechecksForTest();
            r.srv.outsideRecheckNowForTest();
            KV_CHECK(waitFor([&] { return r.srv.outsideRechecksForTest() > checks; }));
            KV_CHECK_MSG(r.router->added() == 0 && r.router->mappings() == 0, c.value);
            const OutsideView v = r.srv.outsideView();
            KV_CHECK(v.listening && v.mappingRemoved && !v.endpointKnown && !v.manualSteps.empty());
            KV_CHECK(r.srv.petSettings().outsideMappedPort == 0);
            std::string err;
            KV_CHECK(!r.srv.removeOutsideMappingNow(&err)); // nothing on the router to remove
            KV_CHECK_MSG(r.srv.remapOutsideNow(&err), err);  // and it can be mapped again
            KV_CHECK(r.waitStatus(outside::OutsideStatus::Mapped));
            KV_CHECK(r.router->added() == 1 && r.router->has(port));
        } else {
            KV_CHECK_MSG(r.waitStatus(outside::OutsideStatus::Mapped), c.value);
            KV_CHECK(r.router->added() == 1 && !r.srv.outsideView().mappingRemoved);
        }
    }
}

// A port change while removed keeps it removed (the listener moves; nothing is mapped on the new port).
KV_TEST(outside_a_port_change_keeps_a_removed_forward_removed) {
    Rig r(L"r2-port");
    KV_REQUIRE(r.start());
    const auto ports = twoFreePorts();
    KV_REQUIRE(r.enableOutside(ports.first));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    std::string err;
    KV_REQUIRE(r.srv.removeOutsideMappingNow(&err));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Manual));
    const int adds = r.router->added();
    KV_REQUIRE(r.srv.setOutsidePort(ports.second, &err));
    KV_CHECK(r.srv.outsideListeningPort() == ports.second);
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Manual));
    KV_CHECK(r.router->added() == adds && r.router->mappings() == 0);
    KV_CHECK(r.srv.outsideView().mappingRemoved);
}

// test_run alone (no loopback_only) binds the outside listener to 127.0.0.1, read from the socket, and uses
// the fixed test LAN address instead of this PC's adapters.
KV_TEST(outside_a_test_run_alone_binds_the_outside_listener_to_loopback) {
    kvtest::OutsideTempState state(L"t2-bind");
    auto router = std::make_shared<FakeRouter>();
    HostController ctl;
    HostController::Options o;
    o.server.state_dir = state.dir;
    o.server.loopback_only = false;
    o.server.lan_listeners_loopback_for_test = true; // the LAN listeners stay on loopback (no firewall prompt)
    o.server.advertise = false;
    o.server.address_policy_for_test.documentationIsPublic = true; // the fake router's sample WAN
    o.test_run = true;
    o.router_factory = router->opener();
    std::string err;
    KV_REQUIRE(ctl.start(o, &err));
    ctl.server().setSharingEnabled(true);
    KV_REQUIRE(ctl.setOutsidePort(freeLoopbackPort(), &err));
    KV_REQUIRE(ctl.setAllowOutside(true, &err));
    KV_CHECK_MSG(ctl.server().outsideListenerAddress() == "127.0.0.1", ctl.server().outsideListenerAddress());
    KV_CHECK(waitFor([&] { return ctl.outsideView().status == outside::OutsideStatus::Mapped; }));
    KV_CHECK_MSG(ctl.outsideView().lanIPv4 == kTestLanIPv4, ctl.outsideView().lanIPv4);
    {
        std::lock_guard<std::mutex> lk(router->m);
        for (const auto& m : router->maps) KV_CHECK(m.second.internalClient == kTestLanIPv4);
    }
    ctl.stop();
}

// The LAN stream listener refuses an unknown certificate even while an invite is open.
KV_TEST(outside_the_lan_listener_refuses_an_unknown_certificate_while_an_invite_is_open) {
    Rig r(L"t3-lan");
    KV_REQUIRE(r.start());
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const auto link = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link.has_value());
    const uint64_t before = r.srv.rejectedHandshakeCount();
    FakePhone stranger(3);
    std::string err;
    Seen s;
    if (stranger.connectStream("127.0.0.1", r.srv.streamPort(), link->hostCertSHA256, &err)) {
        pump(stranger, s, 2000, [](const Seen& x) { return x.frames > 0 || x.closed; });
    }
    KV_CHECK(s.frames == 0);
    KV_CHECK(r.srv.rejectedHandshakeCount() > before);
    KV_CHECK(r.srv.sessionCount() == 0 && r.srv.devices().empty());
    KV_CHECK(r.srv.currentOutsideInvite().has_value()); // untouched
}

// Redemption failures each count, and burn nothing.
KV_TEST(outside_redemption_failures_count_and_burn_nothing) {
    Rig r(L"t4-redeem");
    auto gate = std::make_shared<Gate>();
    auto closing = std::make_shared<std::atomic<bool>>(false);
    HostStreamServer* srv = &r.srv;
    KV_REQUIRE(r.start([gate, closing, srv](HostStreamServer::Options& o) {
        o.outside_redeem_timeout_ms = 600; // the deadline's own default (10 s) is checked below
        o.outside_limits.burst = 30;
        o.outside_test_hook = [closing, srv](const char* where) {
            // "The invite closed during the handshake": closed here, after the handshake, before classifying.
            if (std::strcmp(where, "before_classify") == 0 && closing->load()) srv->cancelOutsideInvite();
        };
    }));
    KV_CHECK(HostStreamServer::Options{}.outside_redeem_timeout_ms == 10000);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const auto link = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link.has_value());
    const auto lim = r.srv.outsideLimiterForTest();
    // Too slow: nothing within the redemption deadline.
    {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, "203.0.113.141", link->hostCertSHA256));
        KV_CHECK(p.waitForHostClose(5000));
        KV_CHECK(waitFor([&] { return lim->failuresFor("203.0.113.141") == 1; }));
    }
    // Over 16 KiB: a header announcing 16 KiB + 1 is closed before its payload is waited for.
    {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, "203.0.113.142", link->hostCertSHA256));
        KV_REQUIRE(waitFor([&] { return r.srv.outsideRedemptionsWaitingForTest() == 1; }));
        p.sendRaw(static_cast<uint8_t>(MsgType::PairRequest), std::string(16 * 1024 + 1, ' '));
        KV_CHECK(p.waitForHostClose(4000));
        KV_CHECK(waitFor([&] { return lim->failuresFor("203.0.113.142") == 1; }));
    }
    // Bad JSON.
    {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, "203.0.113.143", link->hostCertSHA256));
        KV_REQUIRE(waitFor([&] { return r.srv.outsideRedemptionsWaitingForTest() == 1; }));
        p.sendFrame(MsgType::PairRequest, "{\"methodRaw\":2,");
        KV_CHECK(p.waitForHostClose(4000));
        KV_CHECK(waitFor([&] { return lim->failuresFor("203.0.113.143") == 1; }));
    }
    KV_CHECK(r.srv.currentOutsideInvite().has_value()); // none of them burned it
    KV_CHECK(r.logHas("invite_failed", "outside", "203.0.113.143"));
    // The PC closed the invite during the handshake (this replaces
    // "closed and counted"): not the device's failure. Its one frame is answered "This invite was closed", nothing
    // is counted, and the log says invite_closed, not refused_unknown_cert.
    closing->store(true);
    {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, "203.0.113.144", link->hostCertSHA256));
        const auto resp = redeemOn(p, link->token);
        KV_CHECK_MSG(resp.has_value(), "the device got no answer after the invite closed during its handshake");
        if (resp) {
            KV_CHECK(!resp->accepted);
            KV_CHECK_MSG(resp->message == "This invite was closed. Ask for a new one.", resp->message);
        }
        KV_CHECK(waitFor([&] { return lim->inFlightFor("203.0.113.144") == 0; }));
        KV_CHECK_MSG(lim->failuresFor("203.0.113.144") == 0, "an invite the PC closed counted against the device");
        KV_CHECK(r.logHas("invite_closed", "outside", "203.0.113.144"));
    }
    KV_CHECK(!r.srv.currentOutsideInvite().has_value());
}

namespace {

bool logHasExactly(HostStreamServer& srv, const char* event, const std::string& addr) {
    for (const auto& l : srv.connectionLog(30)) {
        if (l.event == event && l.path == "outside" && l.addr == addr) return true;
    }
    return false;
}

} // namespace

// An invite the PC closes while a device is mid-handshake isn't
// that device's failure. Only a certificate the verify callback took because an invite was open counts: one that
// arrives while no invite is open is still refused inside the handshake and counted.
KV_TEST(outside_an_invite_closed_while_a_device_connects_is_not_its_failure) {
    Rig r(L"r1-closed");
    auto closing = std::make_shared<std::atomic<bool>>(false);
    HostStreamServer* srv = &r.srv;
    KV_REQUIRE(r.start([closing, srv](HostStreamServer::Options& o) {
        o.outside_limits.burst = 30;
        o.outside_test_hook = [closing, srv](const char* where) {
            // The handshake is done (the verify callback took the leaf for the open invite); the PC cancels it
            // before the connection is classified.
            if (std::strcmp(where, "before_classify") == 0 && closing->exchange(false)) srv->cancelOutsideInvite();
        };
    }));
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const auto lim = r.srv.outsideLimiterForTest();
    const auto link = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link.has_value());
    const uint64_t rejected_before = r.srv.rejectedHandshakeCount();

    // 1. Closed between the handshake and its classification.
    const std::string a = "203.0.113.150";
    closing->store(true);
    {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, a, link->hostCertSHA256));
        const auto resp = redeemOn(p, link->token);
        KV_CHECK_MSG(resp.has_value(), "the device got no answer: it can't tell the invite was closed");
        if (resp) {
            KV_CHECK(!resp->accepted);
            KV_CHECK_MSG(resp->message == "This invite was closed. Ask for a new one.", resp->message);
        }
    }
    KV_CHECK(!closing->load()); // the hook ran: the invite was cancelled mid-handshake
    KV_CHECK(waitFor([&] { return lim->inFlightFor(a) == 0; }));
    KV_CHECK_MSG(lim->failuresFor(a) == 0, "the PC's cancel counted as the device's failure");
    KV_CHECK(!lim->isBlocked(a));
    KV_CHECK(r.logHas("invite_closed", "outside", a));
    KV_CHECK_MSG(!logHasExactly(r.srv, "refused_unknown_cert", a), "logged as an unknown certificate");
    KV_CHECK(r.srv.rejectedHandshakeCount() == rejected_before);
    KV_CHECK(r.srv.devices().empty());

    // 2. Closed while its redemption frame was on the way (classified, waiting for the frame): a new invite in its
    //    place must not be redeemed (or burned) by the old invite's device either.
    const auto link2 = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link2.has_value());
    const std::string b = "203.0.113.151";
    {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, b, link2->hostCertSHA256));
        KV_REQUIRE(waitFor([&] { return r.srv.outsideRedemptionsWaitingForTest() == 1; }));
        const auto link3 = inviteOf(r.srv, PetRole::Viewer); // replaces (closes) link2's invite
        KV_REQUIRE(link3.has_value());
        const auto resp = redeemOn(p, link2->token);
        KV_CHECK_MSG(resp.has_value(), "the device got no answer after its invite was replaced");
        if (resp) {
            KV_CHECK(!resp->accepted);
            KV_CHECK_MSG(resp->message == "This invite was closed. Ask for a new one.", resp->message);
        }
        KV_CHECK_MSG(r.srv.currentOutsideInvite().has_value(), "the new invite was burned by the old one's device");
    }
    KV_CHECK(waitFor([&] { return lim->inFlightFor(b) == 0; }));
    KV_CHECK_MSG(lim->failuresFor(b) == 0, "a replaced invite counted as the device's failure");
    KV_CHECK(r.logHas("invite_closed", "outside", b));
    KV_CHECK(r.srv.devices().empty());

    // 3. The opposite case: a certificate that arrives while no invite is open is still refused inside the
    //    handshake (no application byte), and counted.
    r.srv.cancelOutsideInvite();
    KV_REQUIRE(!r.srv.currentOutsideInvite().has_value());
    const std::string c = "203.0.113.152";
    {
        FakePhone stranger(3);
        Seen s;
        if (r.connectOutside(stranger, c, link->hostCertSHA256)) pump(stranger, s, 1500, [](const Seen& x) { return x.closed; });
        KV_CHECK(s.frames == 0);
    }
    KV_CHECK(waitFor([&] { return lim->failuresFor(c) == 1; }));
    KV_CHECK(r.logHas("refused_unknown_cert", "outside", c));
    KV_CHECK(r.srv.rejectedHandshakeCount() > rejected_before);
}

// ---- outside redemptions: closes, reconnects and the log ----------------------------------------------------

namespace {

// How many times the log says `event` for `addr` on the outside path, coalesced repeats included.
int logCount(HostStreamServer& srv, const char* event, const std::string& addr, std::string* detail = nullptr) {
    int n = 0;
    for (const auto& l : srv.connectionLog(30)) {
        if (l.event == event && l.path == "outside" && l.addr == addr) {
            n += l.repeat > 1 ? static_cast<int>(l.repeat) : 1;
            if (detail) *detail = l.detail;
        }
    }
    return n;
}

} // namespace

// A device that completes the handshake and then just drops its TCP connection (no close_notify) before its
// redemption frame is a disconnect, never a failure (a weak mobile link never blocks the owner's own phone).
KV_TEST(outside_a_plain_tcp_close_before_the_redemption_frame_is_not_a_failure) {
    Rig r(L"eof-drop");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) { o.outside_limits.burst = 30; }));
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const auto link = inviteOf(r.srv, PetRole::Viewer);
    KV_REQUIRE(link.has_value());
    const auto lim = r.srv.outsideLimiterForTest();
    const std::string src = "203.0.113.161";
    for (int i = 0; i < 3; ++i) {
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, src, link->hostCertSHA256));
        KV_REQUIRE(waitFor([&] { return r.srv.outsideRedemptionsWaitingForTest() == 1; })); // classified, reading
        p.closeWithoutNotify();
        KV_CHECK(waitFor([&] { return r.srv.outsideRedemptionsWaitingForTest() == 0 && lim->inFlightFor(src) == 0; }));
    }
    KV_CHECK_MSG(lim->failuresFor(src) == 0, "a TCP close with no close_notify counted as a failure");
    KV_CHECK(!lim->isBlocked(src));
    KV_CHECK(r.logHas("disconnected", "outside", src));
    KV_CHECK_MSG(logCount(r.srv, "invite_failed", src) == 0, "logged as a failed redemption");
    KV_CHECK(r.srv.currentOutsideInvite().has_value()); // nothing burned
}

// The redemption reply's close drains as a session's does (SSL_shutdown, the FIN, then read and drop until the peer
// closes), so a device still sending after its pairRequest (pings while it waits) gets the whole accepted reply,
// not a reset that throws it away.
KV_TEST(outside_a_device_that_keeps_sending_after_its_pair_request_still_reads_the_reply) {
    Rig r(L"reply-drain");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        // Held after the commit, before the reply goes out: the device's pings land unread meanwhile.
        o.outside_test_hook = [](const char* where) {
            if (std::strcmp(where, "redeem_committed") == 0) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        };
    }));
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    for (int round = 0; round < 3; ++round) {
        const auto link = inviteOf(r.srv, PetRole::Viewer);
        KV_REQUIRE(link.has_value());
        FakePhone p(3);
        KV_REQUIRE(r.connectOutside(p, "203.0.113.162", link->hostCertSHA256));
        PairRequestMessage req;
        req.methodRaw = 2;
        req.clientCertDER = p.identity().certificateDER();
        req.clientProofHMAC =
            computePairingProof(link->token, p.capturedServerHash(), p.identity().certificateSHA256(), "kv2-pair-client");
        req.protocolVersion = 3;
        req.deviceName = "Drain phone " + std::to_string(round);
        req.deviceKind = "iphone";
        KV_REQUIRE(p.sendFrame(MsgType::PairRequest, toJson(req)));
        // Pings for 600 ms, past the host's reply and its close; a send the host refuses is ignored.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        while (std::chrono::steady_clock::now() < until) {
            PingMessage ping;
            ping.clientSendNs = 1;
            ping.protocolVersion = 3;
            p.sendFrame(MsgType::Ping, toJson(ping));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        MsgType t;
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        PairResponseMessage resp;
        const bool got = p.readFrame(t, flags, payload, 5000) && t == MsgType::PairResponse && fromJson(payload, resp);
        KV_CHECK_MSG(got && resp.accepted, "round " + std::to_string(round) + ": the accepted reply was lost");
        KV_CHECK(deviceNamed(r.srv, "Drain phone " + std::to_string(round)).role == PetRole::Viewer);
    }
}

// A connection the outside pool handed off, then outside turned off AND on again before its on_ready runs,
// never becomes a session: it was accepted by a listener that is gone (the outside generation moved).
KV_TEST(outside_a_connection_handed_off_before_off_then_on_never_becomes_a_session) {
    Rig r(L"off-on-handoff");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    auto gate = std::make_shared<Gate>();
    r.srv.testHookBeforeSessionReady = [gate] { gate->pass(); };
    KV_REQUIRE(r.connectOutside(owner, "203.0.113.163", pr.serverHash));
    KV_REQUIRE(gate->waitEntered()); // handed off; its on_ready is held before it takes the server lock
    std::string err;
    KV_CHECK(r.srv.setAllowOutside(false, &err));
    KV_CHECK(r.srv.setAllowOutside(true, &err));
    KV_CHECK(r.srv.outsideListeningPort() != 0); // listening again, on a new listener
    gate->release();
    Seen s;
    pump(owner, s, 3000, [](const Seen& x) { return x.closed || !x.hellos.empty(); });
    KV_CHECK_MSG(s.hellos.empty(), "a connection handed off before the off got a hello after the on");
    KV_CHECK(r.srv.sessionCount() == 0);
    for (const auto& d : r.srv.devices()) KV_CHECK(!d.connected);
}

// An outside session the host ends (outside turned off here) is logged as disconnected exactly once, with why.
KV_TEST(outside_an_outside_session_the_host_ends_is_logged_once_as_disconnected) {
    Rig r(L"end-log");
    KV_REQUIRE(r.start());
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_REQUIRE(r.enableOutside());
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    const std::string src = "203.0.113.164";
    KV_REQUIRE(r.connectOutside(owner, src, pr.serverHash));
    Seen s;
    KV_REQUIRE(pump(owner, s, 5000, [](const Seen& x) { return !x.states.empty(); }));
    KV_CHECK(r.logHas("connected", "outside", src));
    std::string err;
    KV_REQUIRE(r.srv.setAllowOutside(false, &err));
    KV_CHECK(pump(owner, s, 5000, [](const Seen& x) { return x.closed; }));
    KV_CHECK(waitFor([&] { return logCount(r.srv, "disconnected", src) >= 1; }));
    // The connection's own close has run by now (its thread ended): still exactly one line.
    KV_CHECK(waitFor([&] { return r.srv.sessionCount() == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::string detail;
    KV_CHECK_MSG(logCount(r.srv, "disconnected", src, &detail) == 1,
                 "disconnected lines: " + std::to_string(logCount(r.srv, "disconnected", src)));
    KV_CHECK_MSG(detail.find(notice_reason::kOutsideDisabled) != std::string::npos, detail);
}

// A mapping on our port to this PC's address with someone else's description is never adopted (the port is in use
// on the router) and never removed: not by turning off, not by Remove mapping now, not by a port change or the exit.
KV_TEST(outside_a_mapping_to_this_pc_with_another_description_is_never_adopted_or_removed) {
    Rig r(L"foreign-mapping");
    const uint16_t port = freeLoopbackPort();
    r.router->maps[port] = outside::RouterMapping{kTestLanIPv4, port, true, std::string(), "Someone else's game"};
    KV_REQUIRE(r.start()); // the Rig's LAN address is kTestLanIPv4: this PC
    KV_REQUIRE(r.enableOutside(port));
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Error));
    const OutsideView v = r.srv.outsideView();
    KV_CHECK_MSG(v.sentence == "That port is already in use on the router. Pick another port.", v.sentence);
    KV_CHECK(!v.endpointKnown && r.router->added() == 0);
    KV_CHECK(r.srv.petSettings().outsideMappedPort == 0);
    std::string err;
    KV_CHECK(!r.srv.removeOutsideMappingNow(&err)); // not ours: nothing to remove
    KV_CHECK_MSG(!waitFor([&] { return !r.router->has(port); }, 1000), "Remove mapping now removed someone else's mapping");
    auto foreignIntact = [&] {
        std::lock_guard<std::mutex> lk(r.router->m);
        const auto it = r.router->maps.find(port);
        return it != r.router->maps.end() && it->second.description == "Someone else's game";
    };
    // A port change: the old port's mapping is left as it is.
    uint16_t other = freeLoopbackPort();
    for (int i = 0; i < 10 && other == port; ++i) other = freeLoopbackPort();
    KV_REQUIRE(r.srv.setOutsidePort(other, &err));
    KV_REQUIRE(r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(r.router->has(other));
    KV_CHECK_MSG(!waitFor([&] { return !foreignIntact(); }, 1000), "a port change removed someone else's mapping");
    // Back on that port, then off: never removed.
    KV_REQUIRE(r.srv.setOutsidePort(port, &err));
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Error));
    KV_REQUIRE(r.srv.setAllowOutside(false, &err));
    KV_CHECK_MSG(!waitFor([&] { return !foreignIntact(); }, 1500), "turning off removed someone else's mapping");
    KV_CHECK(waitFor([&] { return !r.router->has(other); })); // ours on the other port went, as before
    // On again and the exit: still there.
    KV_REQUIRE(r.srv.setAllowOutside(true, &err));
    KV_CHECK(r.waitStatus(outside::OutsideStatus::Error));
    r.srv.shutdown();
    KV_CHECK_MSG(!waitFor([&] { return !foreignIntact(); }, 1500), "the exit removed someone else's mapping");
}

// ---- v3 §8.3: pin the gateway and ask it directly ----------------------------------------------------------------

namespace {

// The gateway as the host reads it (the read_gateway and ask_gateway_wan seams), set by the row: the gateway of the
// forward's interface, and what that gateway answers itself for the public address (one answer per WAN service; none
// while its UPnP doesn't answer). Synthetic only: a private LAN address, hardware addresses from the documentation
// range (00-00-5e-00-53-xx) and documentation public addresses; no test reads this PC's network.
struct FakeGateway {
    std::mutex m;
    outside::GatewayIdentity gateway;
    std::vector<std::string> own;
    int reads = 0, asks = 0;
    std::string lastLan, lastAsked;
    void set(outside::GatewayIdentity g, std::vector<std::string> answers) {
        std::lock_guard<std::mutex> lk(m);
        gateway = std::move(g);
        own = std::move(answers);
    }
    void set(outside::GatewayIdentity g, const std::string& answer) {
        set(std::move(g), answer.empty() ? std::vector<std::string>{} : std::vector<std::string>{answer});
    }
    int askCount() {
        std::lock_guard<std::mutex> lk(m);
        return asks;
    }
};

std::function<outside::GatewayIdentity(const std::string&)> gatewayReader(const std::shared_ptr<FakeGateway>& g) {
    return [g](const std::string& lan) {
        std::lock_guard<std::mutex> lk(g->m);
        ++g->reads;
        g->lastLan = lan;
        return g->gateway;
    };
}

std::function<std::vector<std::string>(const std::string&, const std::string&, std::string&)> gatewayAsker(
    const std::shared_ptr<FakeGateway>& g) {
    return [g](const std::string& ip, const std::string&, std::string&) {
        std::lock_guard<std::mutex> lk(g->m);
        ++g->asks;
        g->lastAsked = ip;
        return g->own;
    };
}

const outside::GatewayIdentity kHomeGateway{"192.168.1.1", "00-00-5e-00-53-01"};
// A device on the home network that has become the gateway (another hardware address at the router's address).
const outside::GatewayIdentity kOtherGateway{"192.168.1.1", "00-00-5e-00-53-66"};
const std::string kPinned = "address and hardware address";

// An owner connected at home to a host whose router forwards the port at .142, with the gateway pinned at turn-on.
struct GatewayRig {
    Rig r;
    std::shared_ptr<FakeGateway> gw = std::make_shared<FakeGateway>();
    FakePhone owner{3};
    Seen so;
    explicit GatewayRig(const wchar_t* tag) : r(tag) {}
    void seams(HostStreamServer::Options& o, std::vector<uint64_t> retryMs) {
        o.read_gateway = gatewayReader(gw);
        o.ask_gateway_wan = gatewayAsker(gw);
        o.gateway_retry_ms = std::move(retryMs);
    }
    bool up(outside::GatewayIdentity gateway = kHomeGateway, std::vector<uint64_t> retryMs = {100},
            const std::function<void(HostStreamServer::Options&)>& more = {}) {
        gw->set(gateway, "203.0.113.142");
        if (!r.start([&](HostStreamServer::Options& o) {
                seams(o, retryMs);
                if (more) more(o);
            }))
            return false;
        const auto po = pairAtHome(r.srv, owner, PetRole::Owner);
        if (!po.ok || !r.enableOutside() || !r.waitStatus(outside::OutsideStatus::Mapped)) return false;
        if (gateway.complete() && !waitFor([&] { return r.srv.outsideView().gatewayPinKind == kPinned; })) return false;
        std::string err;
        if (!owner.connectStream("127.0.0.1", po.streamPort, po.serverHash, &err)) return false;
        return pump(owner, so, 5000, [](const Seen& x) { return !x.states.empty(); });
    }
    // The router (NATUPnP's side) reports a new public address, and the host looks now.
    void newAddress(const char* wan) {
        r.router->setWan(wan);
        r.srv.outsideRecheckNowForTest();
    }
    void routerAnswers(bool yes) { r.router->setPresent(yes); } // NATUPnP finds the router, or doesn't
    // The owner is told `host`, with the address-changed notice.
    bool told(const std::string& host, int ms = 5000) {
        return pump(owner, so, ms, [&](const Seen& x) {
            if (!hasNotice(x, notice_reason::kAddressChanged)) return false;
            for (const auto& st : x.states) {
                if (st.outsideEndpoint && st.outsideEndpoint->host == host) return true;
            }
            return false;
        });
    }
    // Over `ms`, the owner is told nothing new: no notice, and every endpoint it was told is still `host`.
    bool keeps(const std::string& host, int ms = 1500) {
        pump(owner, so, ms, [](const Seen&) { return false; });
        if (hasNotice(so, notice_reason::kAddressChanged) || so.closed) return false;
        for (const auto& st : so.states) {
            if (st.outsideEndpoint && st.outsideEndpoint->host != host) return false;
        }
        return true;
    }
    bool held(int ms = 6000) {
        return waitFor([&] {
            const OutsideView v = r.srv.outsideView();
            return !v.checking && v.status == outside::OutsideStatus::AddressChanged && v.gatewayHeld && !v.gatewayChecking;
        }, ms);
    }
    bool contradicted(bool yes, int ms = 6000) {
        return waitFor([&] {
            const OutsideView v = r.srv.outsideView();
            return v.status == outside::OutsideStatus::AddressChanged && v.gatewayHeld && v.gatewayContradicts == yes;
        }, ms);
    }
    bool checking(int ms = 6000) {
        return waitFor([&] {
            const OutsideView v = r.srv.outsideView();
            return v.status == outside::OutsideStatus::AddressChanged && v.gatewayChecking && !v.gatewayHeld;
        }, ms);
    }
    bool inviteRefused() {
        std::string err;
        return !r.srv.createOutsideInvite(PetRole::Viewer, &err).has_value() && err.find("Re-share it first") != std::string::npos;
    }
    // The host of the PC's re-share link, "" when none is offered.
    std::string reshareHost() {
        outside::EndpointLink el;
        return outside::EndpointLink::parse(r.srv.outsideEndpointLink(), el) ? el.host : std::string();
    }
    std::string saved() {
        std::string text;
        return readWholeFile(r.state.file(L"pet-camera.json"), text) ? text : std::string();
    }
};

// A gateway's UPnP on 127.0.0.1, for the real askGatewayForWanAt: its description at /rootDesc.xml with a
// WANIPConnection:1 service (control URL as `control` says) and, when asked for, a WANPPPConnection:1 service at
// /ctl/PPPConn; GetExternalIPAddress over SOAP answers `ipWan` and `pppWan` in each service's own namespace. It counts
// the requests it serves.
struct LoopbackIgd {
    static constexpr const char* kIp = "urn:schemas-upnp-org:service:WANIPConnection:1";
    static constexpr const char* kPpp = "urn:schemas-upnp-org:service:WANPPPConnection:1";
    SOCKET ls = INVALID_SOCKET;
    uint16_t port = 0;
    std::mutex m;
    std::string ipWan = "203.0.113.143", pppWan;
    bool withPpp = false;
    std::string control = "/ctl/IPConn";
    int requests = 0;
    int soapHangMs = 0; // > 0: a SOAP request is held this long, then closed with no answer (a port gone dead)
    std::thread t;
    LoopbackIgd() {
        WSADATA w{};
        WSAStartup(MAKEWORD(2, 2), &w);
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(ls, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        listen(ls, 8);
        int len = sizeof(a);
        getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        t = std::thread([this] { serve(); });
    }
    ~LoopbackIgd() {
        closesocket(ls); // ends accept()
        if (t.joinable()) t.join();
        WSACleanup();
    }
    std::string location() const { return "http://127.0.0.1:" + std::to_string(port) + "/rootDesc.xml"; }
    void answer(const std::string& a) {
        std::lock_guard<std::mutex> lk(m);
        ipWan = a;
    }
    void answerPpp(const std::string& a) {
        std::lock_guard<std::mutex> lk(m);
        withPpp = true;
        pppWan = a;
    }
    void controlAt(const std::string& c) {
        std::lock_guard<std::mutex> lk(m);
        control = c;
    }
    int count() {
        std::lock_guard<std::mutex> lk(m);
        return requests;
    }
    void hangSoap(int ms) {
        std::lock_guard<std::mutex> lk(m);
        soapHangMs = ms;
    }
    static std::string soap(const char* service, const std::string& wan) {
        return std::string("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                           "<u:GetExternalIPAddressResponse xmlns:u=\"") +
               service + "\"><NewExternalIPAddress>" + wan +
               "</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
    }
    void serve() {
        for (;;) {
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            DWORD tmo = 3000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
            std::string req;
            char buf[4096];
            for (;;) {
                const int n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
                const size_t h = req.find("\r\n\r\n");
                if (h == std::string::npos) continue;
                std::string head = req.substr(0, h);
                for (auto& ch : head) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                const size_t cl = head.find("content-length:");
                const size_t want = cl == std::string::npos ? 0 : static_cast<size_t>(std::atoi(head.c_str() + cl + 15));
                if (req.size() >= h + 4 + want) break;
            }
            std::string body, status = "200 OK";
            int hang = 0;
            {
                std::lock_guard<std::mutex> lk(m);
                ++requests;
                if (soapHangMs > 0 && req.rfind("POST ", 0) == 0) hang = soapHangMs;
                const auto soapAction = [&](const char* service) {
                    return req.find(std::string("SOAPAction: \"") + service + "#GetExternalIPAddress\"") != std::string::npos;
                };
                if (req.rfind("GET /rootDesc.xml ", 0) == 0) {
                    body = std::string("<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
                                       "<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><deviceList><device>"
                                       "<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>"
                                       "<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType><serviceList>"
                                       "<service><serviceType>") +
                           kIp + "</serviceType><controlURL>" + control + "</controlURL></service>" +
                           (withPpp ? std::string("<service><serviceType>") + kPpp +
                                          "</serviceType><controlURL>/ctl/PPPConn</controlURL></service>"
                                    : std::string()) +
                           "</serviceList></device></deviceList></device></deviceList></device></root>";
                } else if (req.rfind("POST /ctl/IPConn ", 0) == 0 && soapAction(kIp)) {
                    body = soap(kIp, ipWan);
                } else if (req.rfind("POST /ctl/PPPConn ", 0) == 0 && soapAction(kPpp)) {
                    body = soap(kPpp, pppWan);
                } else {
                    status = "404 Not Found";
                }
            }
            if (hang > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(hang));
                closesocket(c);
                continue;
            }
            const std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: text/xml\r\nContent-Length: " +
                                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            send(c, resp.data(), static_cast<int>(resp.size()), 0);
            shutdown(c, SD_SEND);
            closesocket(c);
        }
    }
};

// The asker for a LoopbackIgd: the real description and SOAP exchange at its location, with no search (a test never
// searches this PC's network). It records the description it was handed to ask first, and keeps the location that
// answered, as askGatewayForWan does.
std::function<std::vector<std::string>(const std::string&, const std::string&, std::string&)> loopbackAsker(
    const std::string& location, std::shared_ptr<std::vector<std::string>> handed = nullptr) {
    return [location, handed](const std::string& ip, const std::string&, std::string& lastGood) {
        if (handed) handed->push_back(lastGood);
        std::vector<std::string> answers = outside::askGatewayForWanAt(ip, location, 3000);
        if (!answers.empty()) lastGood = location;
        return answers;
    };
}

// A GatewayRig whose gateway is a LoopbackIgd (pinned as 127.0.0.1), asked through the real description and SOAP
// exchange.
const outside::GatewayIdentity kLoopbackGateway{"127.0.0.1", "00-00-5e-00-53-01"};
bool upOnLoopback(GatewayRig& g, const LoopbackIgd& igd) {
    const std::string location = igd.location();
    return g.up(kLoopbackGateway, {100},
                [location](HostStreamServer::Options& o) { o.ask_gateway_wan = loopbackAsker(location); });
}

} // namespace

// A new public address that the pinned gateway itself confirms (one of its own GetExternalIPAddress answers is the
// address NATUPnP read, in the same pass) is told to every device at once, with no click at the PC: a new lease or a
// router restart while the owner is away keeps outside viewing working. Only the pinned gateway is asked, for the
// forward's interface. Retries are once a minute unless a test says otherwise.
KV_TEST(outside_a_new_address_the_pinned_gateway_confirms_is_told_with_no_click) {
    KV_CHECK((HostStreamServer::Options{}.gateway_retry_ms == std::vector<uint64_t>{60'000}));
    GatewayRig g(L"gw-confirmed");
    KV_REQUIRE(g.up());
    KV_CHECK(waitFor([&] { return g.r.srv.petSettings().outsideGatewayMAC == kHomeGateway.mac; })); // saved
    g.gw->set(kHomeGateway, "203.0.113.143");
    g.newAddress("203.0.113.143");
    KV_CHECK(g.told("203.0.113.143"));
    KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
    {
        std::lock_guard<std::mutex> lk(g.gw->m);
        KV_CHECK(g.gw->lastAsked == kHomeGateway.ip && g.gw->lastLan == kTestLanIPv4);
    }
    std::string err;
    const auto inv = g.r.srv.createOutsideInvite(PetRole::Viewer, &err);
    KV_CHECK_MSG(inv.has_value() && inv->uri.find("h=203.0.113.143") != std::string::npos, err);
    KV_CHECK(waitFor([&] { return g.saved().find("\"outsideAnnouncedAddress\":\"203.0.113.143\"") != std::string::npos; }));
}

// Positive evidence that the new address didn't come from the pinned router holds it: the gateway's own answer
// differs from what NATUPnP read (contradicted), or the gateway itself is another device (its hardware address or its
// address differs). Devices keep the address they were told, with no notice; invites wait; and no re-share is offered,
// since no check has confirmed the address.
KV_TEST(outside_a_new_address_the_gateway_doesnt_confirm_is_held) {
    struct Case {
        const wchar_t* tag;
        outside::GatewayIdentity gateway;
        std::string own;
        bool contradicts;
    };
    const Case cases[] = {{L"gw-held-answer", kHomeGateway, "203.0.113.142", true},
                          {L"gw-held-mac", kOtherGateway, "203.0.113.143", false},
                          {L"gw-held-ip", {"192.168.1.254", kHomeGateway.mac}, "203.0.113.143", false}};
    for (const Case& c : cases) {
        GatewayRig g(c.tag);
        KV_REQUIRE(g.up());
        g.gw->set(c.gateway, c.own);
        g.newAddress("203.0.113.143");
        KV_CHECK(g.held());
        KV_CHECK(g.contradicted(c.contradicts));
        KV_CHECK(g.keeps("203.0.113.142"));
        KV_CHECK(g.inviteRefused());
        KV_CHECK(g.reshareHost().empty());
        if (c.contradicts) KV_CHECK(g.r.srv.outsideView().gatewayAnswer == "203.0.113.142");
        KV_CHECK(g.r.srv.petSettings().outsideGatewayMAC == kHomeGateway.mac); // the pin stays
    }
}

// An address the pinned gateway contradicts (another device answered as the router) is never acknowledged: no
// re-share is offered, and an acknowledgement for it is refused. The host keeps checking; once the gateway agrees
// with NATUPnP, the PC user may re-share that address (still not told without them), and then it is told.
KV_TEST(outside_an_address_the_gateway_contradicts_is_never_acknowledged) {
    GatewayRig g(L"gw-contradicted");
    KV_REQUIRE(g.up());
    g.gw->set(kHomeGateway, "203.0.113.142");
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.contradicted(true));
    KV_CHECK(g.reshareHost().empty());
    g.r.srv.acknowledgeAddressChange("203.0.113.143"); // what a re-share of the router's address would send
    KV_CHECK(g.keeps("203.0.113.142"));
    KV_CHECK(g.contradicted(true, 500));
    g.gw->set(kHomeGateway, "203.0.113.143"); // the gateway agrees now
    KV_CHECK(g.contradicted(false));
    KV_CHECK(g.held(500));
    KV_CHECK(g.keeps("203.0.113.142", 500)); // still only with the PC user
    KV_CHECK(g.reshareHost() == "203.0.113.143");
    g.r.srv.acknowledgeAddressChange("203.0.113.143");
    KV_CHECK(g.told("203.0.113.143"));
    KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
}

// A hold on another gateway is sticky: later checks that would confirm the address don't tell it. Only the PC user's
// re-share does (or turning outside viewing off and on).
KV_TEST(outside_a_hold_stays_until_the_pc_user_acknowledges) {
    GatewayRig g(L"gw-sticky");
    KV_REQUIRE(g.up());
    g.gw->set(kOtherGateway, "203.0.113.143");
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.held());
    g.gw->set(kHomeGateway, "203.0.113.143"); // now it would confirm
    for (int i = 0; i < 3; ++i) {
        const uint64_t before = g.r.srv.outsideRechecksForTest();
        g.r.srv.outsideRecheckNowForTest();
        KV_CHECK(waitFor([&] { return g.r.srv.outsideRechecksForTest() > before; }));
    }
    KV_CHECK(g.held(500));
    KV_CHECK(g.keeps("203.0.113.142"));
    g.r.srv.acknowledgeAddressChange("203.0.113.143");
    KV_CHECK(g.told("203.0.113.143"));
    KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
}

// The re-share acknowledges only the address the PC user was shown, and only while it is still the address read now
// and a check confirmed it.
KV_TEST(outside_an_acknowledgement_applies_only_to_the_address_shown) {
    GatewayRig g(L"gw-ack-shown");
    KV_REQUIRE(g.up());
    g.gw->set(kHomeGateway, "203.0.113.142");
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.contradicted(true));
    g.gw->set(kHomeGateway, "203.0.113.143"); // confirmed, while held
    KV_REQUIRE(waitFor([&] { return g.reshareHost() == "203.0.113.143"; }));
    g.r.srv.acknowledgeAddressChange("203.0.113.150"); // not the one shown
    KV_CHECK(g.held(1500) && g.keeps("203.0.113.142", 500));
    g.gw->set(kHomeGateway, "203.0.113.144");
    g.newAddress("203.0.113.144"); // the address moved after the link was shown, and is confirmed in turn
    KV_CHECK(waitFor([&] { return g.r.srv.outsideView().gatewayHeld && g.reshareHost() == "203.0.113.144"; }));
    g.r.srv.acknowledgeAddressChange("203.0.113.143"); // the stale one
    KV_CHECK(g.held(1500) && g.keeps("203.0.113.142", 500));
    g.r.srv.acknowledgeAddressChange("203.0.113.144");
    KV_CHECK(g.told("203.0.113.144"));
}

// v3 §8.3: with a gateway pinned, only an address a check CONFIRMED is ever re-shared or acknowledged. Contradicted,
// then confirmed (re-share offered for that address), then NATUPnP reads another address while the gateway can't be
// read (its UPnP made to look unreadable): that address is neither offered nor acknowledged, and devices keep theirs.
KV_TEST(outside_only_a_confirmed_address_is_ever_re_shared) {
    GatewayRig g(L"gw-confirmed-only");
    KV_REQUIRE(g.up());
    g.gw->set(kHomeGateway, "203.0.113.142");
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.contradicted(true));
    g.gw->set(kHomeGateway, "203.0.113.143");
    KV_REQUIRE(waitFor([&] { return g.reshareHost() == "203.0.113.143"; }));
    g.gw->set(kHomeGateway, ""); // unread from now on
    g.newAddress("203.0.113.144");
    KV_CHECK(waitFor([&] { return g.r.srv.outsideView().wanAddress == "203.0.113.144"; }));
    KV_CHECK(waitFor([&] { return g.reshareHost().empty() && g.r.srv.outsideEndpointLink().empty(); }));
    g.r.srv.acknowledgeAddressChange("203.0.113.144");
    g.r.srv.acknowledgeAddressChange("203.0.113.143");
    KV_CHECK(g.keeps("203.0.113.142"));
    KV_CHECK(g.held(500));
    KV_CHECK(g.inviteRefused());
}

// A held change through another gateway (a replaced router, or another device) is never confirmed, so it can't be
// re-shared; turning outside viewing off and on starts afresh with the gateway there now, and its address is told.
KV_TEST(outside_another_gateway_is_accepted_by_turning_outside_viewing_off_and_on) {
    GatewayRig g(L"gw-toggle");
    KV_REQUIRE(g.up());
    g.gw->set(kOtherGateway, "203.0.113.143");
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.held());
    KV_CHECK(g.reshareHost().empty());
    g.r.srv.acknowledgeAddressChange("203.0.113.143");
    KV_CHECK(g.held(1500) && g.keeps("203.0.113.142", 500));
    std::string err;
    KV_REQUIRE(g.r.srv.setAllowOutside(false, &err));
    KV_REQUIRE(g.r.srv.setAllowOutside(true, &err));
    KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(waitFor([&] { return g.r.srv.petSettings().outsideGatewayMAC == kOtherGateway.mac; })); // pinned afresh
    KV_CHECK(g.told("203.0.113.143"));
}

// The address devices were last told is saved, so a restart checks a first address that differs instead of telling
// it at once: held when the gateway's own answer differs, told with no click when it confirms it. The pin comes back
// from pet-camera.json too.
KV_TEST(outside_a_restart_checks_a_first_address_that_differs_from_the_one_announced) {
    for (const bool confirmed : {false, true}) {
        GatewayRig g(confirmed ? L"gw-restart-confirmed" : L"gw-restart-held");
        KV_REQUIRE(g.up());
        KV_REQUIRE(waitFor([&] { return g.saved().find("\"outsideAnnouncedAddress\":\"203.0.113.142\"") != std::string::npos; }));
        g.r.srv.shutdown();
        g.r.router->setWan("203.0.113.143");
        g.gw->set(kHomeGateway, confirmed ? "203.0.113.143" : "203.0.113.142");
        HostStreamServer again;
        HostStreamServer::Options o = g.r.options();
        g.seams(o, {100});
        std::string err;
        KV_REQUIRE(again.start(o, &err));
        KV_CHECK(again.petSettings().outsideGatewayMAC == kHomeGateway.mac);
        const outside::OutsideStatus want = confirmed ? outside::OutsideStatus::Mapped : outside::OutsideStatus::AddressChanged;
        KV_CHECK(waitFor([&] {
            const OutsideView v = again.outsideView();
            return !v.checking && v.status == want && v.gatewayHeld == !confirmed;
        }, 8000));
        std::string e2;
        const auto inv = again.createOutsideInvite(PetRole::Viewer, &e2);
        if (confirmed) KV_CHECK_MSG(inv.has_value() && inv->uri.find("h=203.0.113.143") != std::string::npos, e2);
        else KV_CHECK_MSG(!inv.has_value() && e2.find("Re-share it first") != std::string::npos, e2);
        again.shutdown();
    }
}

// A retry reads everything again: NATUPnP's address included, so the address a pass announces is the one read in
// that pass, never one read earlier.
KV_TEST(outside_a_retry_reads_the_address_again_and_never_announces_an_earlier_one) {
    GatewayRig g(L"gw-retry-fresh");
    KV_REQUIRE(g.up(kHomeGateway, {1500}));
    g.gw->set(kHomeGateway, ""); // its UPnP doesn't answer yet
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.checking());
    g.r.router->setWan("203.0.113.144");
    g.gw->set(kHomeGateway, "203.0.113.144");
    KV_CHECK(g.told("203.0.113.144", 10000));
    for (const auto& st : g.so.states) KV_CHECK(!st.outsideEndpoint || st.outsideEndpoint->host != "203.0.113.143");
    KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
}

// A router that stops answering NATUPnP during a check doesn't end it: the host keeps checking (counting the passes
// that couldn't read it), and once the router and the gateway answer and agree, the address is told with no click.
KV_TEST(outside_a_router_failure_during_a_check_keeps_it_checking) {
    GatewayRig g(L"gw-natupnp-failure");
    KV_REQUIRE(g.up(kHomeGateway, {100}));
    g.gw->set(kHomeGateway, ""); // the gateway's UPnP doesn't answer yet
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.checking());
    g.routerAnswers(false); // NATUPnP can't find the router either
    KV_CHECK(waitFor([&] { return g.r.srv.outsideView().gatewayFailedPasses >= 3; }));
    g.routerAnswers(true);
    g.gw->set(kHomeGateway, "203.0.113.143");
    KV_CHECK(g.told("203.0.113.143", 10000));
    KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(g.r.srv.outsideView().gatewayFailedPasses == 0);
}

// A gateway that can't be read is no evidence: the host keeps reading it (here far more passes than any time limit
// would allow), devices keep the address they were told, invites wait, and the panel says it is checking. Once one
// pass confirms the address it is told, with no click.
KV_TEST(outside_a_gateway_unreachable_for_long_then_confirmed_is_told_with_no_click) {
    GatewayRig g(L"gw-unreachable-long");
    KV_REQUIRE(g.up(kHomeGateway, {20}));
    g.gw->set(kHomeGateway, "");
    const int before = g.gw->askCount();
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.checking());
    KV_CHECK(waitFor([&] { return g.gw->askCount() >= before + 40; }, 15000));
    KV_CHECK(g.checking(500));
    KV_CHECK(g.r.srv.outsideView().gatewayFailedPasses >= 30); // the panel now suggests re-sharing
    KV_CHECK(g.inviteRefused());
    KV_CHECK(g.keeps("203.0.113.142", 300));
    g.gw->set(kHomeGateway, "203.0.113.143");
    KV_CHECK(g.told("203.0.113.143"));
    KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
    KV_CHECK(!g.r.srv.outsideView().gatewayChecking);
}

// ...but positive evidence after a stretch of not answering holds it, stickily.
KV_TEST(outside_a_gateway_unreachable_then_answering_differently_is_held) {
    GatewayRig g(L"gw-unreachable-other");
    KV_REQUIRE(g.up(kHomeGateway, {50}));
    g.gw->set(kHomeGateway, "");
    g.newAddress("203.0.113.143");
    KV_REQUIRE(g.checking());
    g.gw->set(kHomeGateway, "203.0.113.150");
    KV_CHECK(g.held());
    g.gw->set(kHomeGateway, "203.0.113.143");
    const uint64_t before = g.r.srv.outsideRechecksForTest();
    g.r.srv.outsideRecheckNowForTest();
    KV_CHECK(waitFor([&] { return g.r.srv.outsideRechecksForTest() > before; }));
    KV_CHECK(g.held(500));
    KV_CHECK(g.keeps("203.0.113.142"));
}

// pet-camera.json's pin fails safe: a key that isn't a valid address or hardware address drops the whole pin, so a
// first address that differs at the next start waits for the PC user (nothing to check it against). A corrupt saved
// address makes every first address a change to check, which the pinned gateway can still confirm.
KV_TEST(outside_corrupt_router_pin_keys_drop_the_pin_and_a_change_waits) {
    {
        GatewayRig g(L"gw-corrupt-pin");
        KV_REQUIRE(g.up());
        KV_REQUIRE(waitFor([&] { return g.saved().find("\"outsideGatewayMAC\":\"00-00-5e-00-53-01\"") != std::string::npos; }));
        g.r.srv.shutdown();
        std::string text = g.saved();
        text.replace(text.find("00-00-5e-00-53-01"), 17, "zz-not-a-mac-addr");
        KV_REQUIRE(kvtest::writeRaw(g.r.state.file(L"pet-camera.json"), text));
        g.r.router->setWan("203.0.113.143");
        g.gw->set(kHomeGateway, "203.0.113.143"); // it would confirm, if anything were pinned
        HostStreamServer again;
        HostStreamServer::Options o = g.r.options();
        g.seams(o, {100});
        std::string err;
        KV_REQUIRE(again.start(o, &err));
        KV_CHECK(again.petSettings().outsideGatewayIP.empty() && again.petSettings().outsideGatewayMAC.empty());
        KV_CHECK(waitFor([&] {
            const OutsideView v = again.outsideView();
            return !v.checking && v.status == outside::OutsideStatus::AddressChanged && v.gatewayPinKind.empty() &&
                   !v.gatewayChecking;
        }, 8000));
        std::string e2;
        KV_CHECK(!again.createOutsideInvite(PetRole::Viewer, &e2).has_value());
        again.acknowledgeAddressChange("203.0.113.143");
        KV_CHECK(waitFor([&] {
            const OutsideView v = again.outsideView();
            return v.status == outside::OutsideStatus::Mapped && v.gatewayPinKind == kPinned;
        }));
        again.shutdown();
    }
    {
        GatewayRig g(L"gw-corrupt-address");
        KV_REQUIRE(g.up());
        KV_REQUIRE(waitFor([&] { return g.saved().find("\"outsideAnnouncedAddress\":\"203.0.113.142\"") != std::string::npos; }));
        g.r.srv.shutdown();
        std::string text = g.saved();
        text.replace(text.find("\"outsideAnnouncedAddress\":\"203.0.113.142\""), 41, "\"outsideAnnouncedAddress\":\"not-an-addr!!\"");
        KV_REQUIRE(kvtest::writeRaw(g.r.state.file(L"pet-camera.json"), text));
        const int asks = g.gw->askCount();
        HostStreamServer again;
        HostStreamServer::Options o = g.r.options();
        g.seams(o, {100});
        std::string err;
        KV_REQUIRE(again.start(o, &err));
        KV_CHECK(again.petSettings().outsideAnnouncedCorrupt);
        KV_CHECK(waitFor([&] {
            const OutsideView v = again.outsideView();
            return !v.checking && v.status == outside::OutsideStatus::Mapped;
        }, 8000));
        KV_CHECK(g.gw->askCount() > asks); // checked, then confirmed by the gateway
        KV_CHECK(waitFor([&] { return !again.petSettings().outsideAnnouncedCorrupt; }));
        again.shutdown();
    }
}

// The real exchange with a gateway (description, then GetExternalIPAddress over SOAP), against one on 127.0.0.1: its
// answer comes back; a description URL anywhere but the gateway, or with user-info, is never fetched; a control URL
// off the gateway is never called. And a host whose gateway is that loopback one tells an address the gateway
// confirms and holds one it doesn't.
KV_TEST(outside_the_pinned_gateway_is_asked_over_soap_and_only_on_its_own_address) {
    LoopbackIgd igd;
    KV_CHECK((outside::askGatewayForWanAt("127.0.0.1", igd.location(), 3000) == std::vector<std::string>{"203.0.113.143"}));
    const int served = igd.count();
    KV_CHECK(served == 2); // the description and one SOAP call
    const std::string port = std::to_string(igd.port);
    KV_CHECK(outside::askGatewayForWanAt("127.0.0.1", "http://127.0.0.2:" + port + "/rootDesc.xml", 3000).empty());
    KV_CHECK(outside::askGatewayForWanAt("127.0.0.1", "http://x@127.0.0.1:" + port + "/rootDesc.xml", 3000).empty());
    KV_CHECK(outside::askGatewayForWanAt("127.0.0.1", "https://127.0.0.1:" + port + "/rootDesc.xml", 3000).empty());
    KV_CHECK(igd.count() == served);
    igd.controlAt("http://127.0.0.2:" + port + "/ctl/IPConn");
    KV_CHECK(outside::askGatewayForWanAt("127.0.0.1", igd.location(), 3000).empty());
    KV_CHECK(igd.count() == served + 1); // the description only
    igd.controlAt("/ctl/IPConn");
    // The real asker tries the description that answered last first, straight to the gateway, before any search.
    std::string lastGood = igd.location();
    const int beforeDirect = igd.count();
    KV_CHECK((outside::askGatewayForWan("127.0.0.1", "127.0.0.1", 3000, lastGood) == std::vector<std::string>{"203.0.113.143"}));
    KV_CHECK(igd.count() == beforeDirect + 2 && lastGood == igd.location());

    GatewayRig g(L"gw-loopback");
    KV_REQUIRE(upOnLoopback(g, igd));
    igd.answer("203.0.113.143");
    g.newAddress("203.0.113.143");
    KV_CHECK(g.told("203.0.113.143"));
    g.so = Seen{}; // from here on, only what the owner is told next
    igd.answer("203.0.113.150");
    g.newAddress("203.0.113.144");
    KV_CHECK(g.held());
    KV_CHECK(g.keeps("203.0.113.143"));
}

// Every WAN connection service of the gateway is asked, and only public answers count: one service answering 0.0.0.0
// or a private address (unread) while another holds the public address confirms it; a public answer that differs
// from NATUPnP's doesn't outweigh one that agrees.
KV_TEST(outside_every_wan_service_is_asked_and_only_public_answers_count) {
    struct Case {
        const wchar_t* tag;
        std::string ipAnswer;
    };
    const Case cases[] = {{L"gw-services-unconnected", "0.0.0.0"},
                          {L"gw-services-private", "192.168.100.1"},
                          {L"gw-services-other-public", "203.0.113.150"}};
    for (const Case& c : cases) {
        LoopbackIgd igd;
        igd.answer("203.0.113.142");
        igd.answerPpp("203.0.113.142");
        GatewayRig g(c.tag);
        KV_REQUIRE(upOnLoopback(g, igd));
        igd.answer(c.ipAnswer);         // WANIPConnection:1, asked first
        igd.answerPpp("203.0.113.143"); // WANPPPConnection:1 holds the public address
        g.newAddress("203.0.113.143");
        KV_CHECK(g.told("203.0.113.143"));
        KV_CHECK(g.r.waitStatus(outside::OutsideStatus::Mapped));
    }
}

// The read-only router probe sees each step of the gateway read (GatewayTrace): the description and its WAN services,
// each service's answer, a URL off the gateway that is reported and never fetched, and a service that gives no answer.
KV_TEST(outside_the_gateway_read_reports_each_step_to_the_router_probe) {
    LoopbackIgd igd;
    igd.answerPpp("0.0.0.0");
    std::vector<std::string> lines;
    const outside::GatewayTrace trace = [&](const std::string& l) { lines.push_back(l); };
    const auto has = [&](const std::string& a, const std::string& b = "") {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
            return l.find(a) != std::string::npos && l.find(b) != std::string::npos;
        });
    };
    std::string lastGood = igd.location();
    KV_CHECK((outside::askGatewayForWan("127.0.0.1", "127.0.0.1", 3000, lastGood, trace) ==
              std::vector<std::string>{"203.0.113.143", "0.0.0.0"}));
    KV_REQUIRE(!lines.empty());
    KV_CHECK(lines.front() == "first, the description that answered last time, straight to the gateway");
    KV_CHECK(has("description " + igd.location() + ": ", " bytes, 2 WAN connection service(s) on the gateway: " +
                                                             std::string(LoopbackIgd::kIp) + ", " + LoopbackIgd::kPpp));
    KV_CHECK(has(std::string(LoopbackIgd::kIp) + " at http://127.0.0.1:" + std::to_string(igd.port) + "/ctl/IPConn: ",
                 "answered 203.0.113.143"));
    KV_CHECK(has(std::string(LoopbackIgd::kPpp) + " at http://127.0.0.1:" + std::to_string(igd.port) + "/ctl/PPPConn: ",
                 "answered 0.0.0.0"));

    lines.clear();
    const int served = igd.count();
    const std::string off = "http://127.0.0.2:" + std::to_string(igd.port) + "/rootDesc.xml";
    KV_CHECK(outside::askGatewayForWanAt("127.0.0.1", off, 3000, trace).empty());
    KV_CHECK(has("description " + off + ": not an http URL on the gateway's own address, not fetched"));
    KV_CHECK(igd.count() == served);

    lines.clear();
    igd.controlAt("/ctl/Gone");
    KV_CHECK((outside::askGatewayForWanAt("127.0.0.1", igd.location(), 3000, trace) == std::vector<std::string>{"0.0.0.0"}));
    KV_CHECK(has(std::string(LoopbackIgd::kIp) + " at http://127.0.0.1:" + std::to_string(igd.port) + "/ctl/Gone: ",
                 "no usable answer"));
}

// The description that gave the gateway's answer is saved with the pin, and a restart asks it first, so a pass after a
// restart needn't search. A saved description anywhere but the pinned gateway is dropped at load, and the pin stands.
KV_TEST(outside_the_description_that_answered_is_kept_across_a_restart) {
    LoopbackIgd igd;
    GatewayRig g(L"gw-location-kept");
    KV_REQUIRE(upOnLoopback(g, igd));
    g.newAddress("203.0.113.143"); // the gateway answers .143 too
    KV_CHECK(g.told("203.0.113.143"));
    const auto asSaved = [](std::string s) { // json_lite writes '/' as "\/"
        for (size_t i = 0; (i = s.find('/', i)) != std::string::npos; i += 2) s.insert(i, 1, '\\');
        return s;
    };
    const std::string key = "\"outsideGatewayLocation\":\"" + asSaved(igd.location()) + "\"";
    KV_REQUIRE(waitFor([&] { return g.saved().find(key) != std::string::npos; }));
    g.r.srv.shutdown();

    g.r.router->setWan("203.0.113.144");
    igd.answer("203.0.113.144");
    auto handed = std::make_shared<std::vector<std::string>>();
    {
        HostStreamServer again;
        HostStreamServer::Options o = g.r.options();
        g.seams(o, {100});
        o.ask_gateway_wan = loopbackAsker(igd.location(), handed);
        std::string err;
        KV_REQUIRE(again.start(o, &err));
        KV_CHECK(again.petSettings().outsideGatewayLocation == igd.location());
        KV_CHECK(waitFor([&] {
            const OutsideView v = again.outsideView();
            return !v.checking && v.status == outside::OutsideStatus::Mapped && !v.gatewayHeld;
        }, 8000));
        KV_REQUIRE(!handed->empty());
        KV_CHECK(handed->front() == igd.location()); // asked first, before any search
        again.shutdown();
    }

    std::string text = g.saved();
    const size_t at = text.find(key);
    KV_REQUIRE(at != std::string::npos);
    const std::string off =
        "\"outsideGatewayLocation\":\"" + asSaved("http://127.0.0.2:" + std::to_string(igd.port) + "/rootDesc.xml") + "\"";
    text.replace(at, key.size(), off);
    KV_REQUIRE(kvtest::writeRaw(g.r.state.file(L"pet-camera.json"), text));
    HostStreamServer third;
    HostStreamServer::Options o = g.r.options();
    g.seams(o, {100});
    std::string err;
    KV_REQUIRE(third.start(o, &err));
    KV_CHECK(third.petSettings().outsideGatewayLocation.empty());
    KV_CHECK(third.petSettings().outsideGatewayIP == kLoopbackGateway.ip &&
             third.petSettings().outsideGatewayMAC == kLoopbackGateway.mac);
    third.shutdown();
}

// A gateway that still can't be read is checked less often after a while, as a hold is (every 5 minutes after 30;
// here every 60 s after 2 s, the retry every 100 ms). The change stays "checking" all along. Waited on by the passes
// themselves, with generous bounds, so a busy PC only makes it slower.
KV_TEST(outside_a_gateway_unreadable_for_long_is_checked_less_often) {
    GatewayRig g(L"gw-slow-unread");
    KV_REQUIRE(g.up(kHomeGateway, {100}, [](HostStreamServer::Options& o) {
        o.gateway_slow_after_ms = 2'000;
        o.gateway_slow_every_ms = 60'000;
    }));
    g.gw->set(kHomeGateway, std::vector<std::string>{}); // its UPnP stops answering
    const int before = g.gw->askCount();
    g.newAddress("203.0.113.143");
    KV_CHECK(g.checking());
    // At first, a pass every 100 ms.
    KV_CHECK(waitFor([&] { return g.gw->askCount() - before >= 3; }, 10'000));
    // Then, once the slow-down starts, a stretch of 1.5 s with no pass comes. Without it, a pass every 100 ms never
    // leaves one.
    bool quiet = false;
    int last = g.gw->askCount();
    auto since = std::chrono::steady_clock::now();
    const auto end = since + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int now = g.gw->askCount();
        if (now != last) {
            last = now;
            since = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - since >= std::chrono::milliseconds(1'500)) {
            quiet = true;
            break;
        }
    }
    KV_CHECK_MSG(quiet, "passes kept coming: " + std::to_string(g.gw->askCount() - before));
    KV_CHECK(g.checking());
}

// The description that answered last time gets at most kLastGoodTryMs of the deadline: an old port that has gone dead
// (here its SOAP answers are held, then dropped, by both services) can't use up the 8 s and leave nothing for the
// search. lanIPv4 isn't an address, so the search sends nothing (no test searches this PC's network).
KV_TEST(outside_a_dead_last_good_description_leaves_time_for_the_search) {
    LoopbackIgd igd;
    igd.answerPpp("203.0.113.143");
    igd.hangSoap(3'500);
    std::vector<std::string> lines;
    const outside::GatewayTrace trace = [&](const std::string& l) { lines.push_back(l); };
    std::string lastGood = igd.location();
    const auto start = std::chrono::steady_clock::now();
    KV_CHECK(outside::askGatewayForWan("127.0.0.1", "", 8'000, lastGood, trace).empty());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    KV_CHECK_MSG(ms < outside::kLastGoodTryMs + 1'500, "the last good description took " + std::to_string(ms) + " ms");
    KV_CHECK(lastGood == igd.location()); // kept for the next pass
    KV_CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l == "search: the gateway's or this PC's address isn't a dotted IPv4, nothing sent";
    }));
}

// ---- home by adapter: the review's fixes ---------------------------------------------------------------------

namespace {
// The adapter seam's home Wi-Fi: 192.168.1.20/24, and fe80::20 on its own link (interface 7).
outside::AdapterInfo homeWifi() {
    outside::AdapterInfo wifi;
    wifi.kind = outside::AdapterInfo::Kind::WiFi;
    wifi.hardware = true;
    wifi.hasGateway = true;
    wifi.addresses.push_back(outside::InterfaceAddress{*outside::parseIp("192.168.1.20"), 24});
    wifi.addresses.push_back(outside::InterfaceAddress{*outside::parseIp("fe80::20%7"), 64});
    return wifi;
}
} // namespace

// fe80::/64 is on every link. A link-local source on the home Wi-Fi's link is home; the same kind of address on
// another link (a virtual switch, a VPN's TAP adapter, a Mobile Hotspot) is closed before TLS, as any other stranger.
KV_TEST(outside_a_link_local_source_is_home_only_on_the_home_adapters_link) {
    Rig r(L"home-link-local");
    KV_REQUIRE(r.start([](HostStreamServer::Options& o) {
        o.on_link_prefixes.reset();
        o.adapters_for_test = []() -> std::optional<std::vector<outside::AdapterInfo>> {
            return std::vector<outside::AdapterInfo>{homeWifi()};
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    std::string err;
    r.src->setStream("fe80::5%7");
    KV_REQUIRE(owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err));
    Seen home;
    KV_CHECK(pump(owner, home, 4000, [](const Seen& s) { return !s.hellos.empty(); }));
    owner.close();
    KV_CHECK(r.logHas("connected", "home", "fe80::5"));
    // On another link, and then on none known. The log coalesces a source as the limiter counts it (an IPv6 /64),
    // so each refusal gets a host of its own, and so its own line.
    for (const char* away : {"fe80::6%12", "fe80::7"}) {
        Rig other(away[7] == '%' ? L"home-link-other" : L"home-link-none");
        KV_REQUIRE(other.start([](HostStreamServer::Options& o) {
            o.on_link_prefixes.reset();
            o.adapters_for_test = []() -> std::optional<std::vector<outside::AdapterInfo>> {
                return std::vector<outside::AdapterInfo>{homeWifi()};
            };
        }));
        FakePhone phone(3);
        const auto paired = pairAtHome(other.srv, phone, PetRole::Owner);
        KV_REQUIRE(paired.ok);
        other.src->setStream(away);
        Seen s;
        if (phone.connectStream("127.0.0.1", paired.streamPort, paired.serverHash, &err)) {
            pump(phone, s, 1500, [](const Seen& x) { return x.frames > 0; });
        }
        KV_CHECK_MSG(s.frames == 0 && s.hellos.empty(), away);
        phone.close();
        const std::string logged = std::string(away).substr(0, std::string(away).find('%'));
        KV_CHECK_MSG(other.logHas("refused_local_only", "home", logged), away);
    }
}

// The adapter read (GetAdaptersAddresses and a GetIfEntry2 per adapter: tens of milliseconds) runs with the server's
// lock released, so the stranger whose connection makes it read holds nothing else up. Here the read is held for as
// long as the row likes; meanwhile a call that takes the server's lock returns at once.
KV_TEST(outside_a_slow_adapter_read_holds_nothing_else_up) {
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool armed = false, entered = false, released = false;
    };
    auto gate = std::make_shared<Gate>();
    Rig r(L"home-read-slow");
    KV_REQUIRE(r.start([gate](HostStreamServer::Options& o) {
        o.on_link_prefixes.reset();
        o.adapters_for_test = [gate]() -> std::optional<std::vector<outside::AdapterInfo>> {
            std::unique_lock<std::mutex> lk(gate->m);
            if (gate->armed) {
                gate->entered = true;
                gate->cv.notify_all();
                gate->cv.wait_for(lk, std::chrono::seconds(10), [&] { return gate->released; });
            }
            return std::vector<outside::AdapterInfo>{homeWifi()};
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner); // from loopback: no adapter read
    KV_REQUIRE(pr.ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    {
        std::lock_guard<std::mutex> lk(gate->m);
        gate->armed = true;
    }
    r.src->setStream("192.168.50.9");
    const SOCKET stranger = rawTo(pr.streamPort); // the accept thread reads the adapters for it, and is held there
    KV_REQUIRE(stranger != INVALID_SOCKET);
    bool entered = false;
    {
        std::unique_lock<std::mutex> lk(gate->m);
        entered = gate->cv.wait_for(lk, std::chrono::seconds(5), [&] { return gate->entered; });
    }
    KV_CHECK_MSG(entered, "the stranger's connection didn't read the adapters");
    auto asked = std::async(std::launch::async, [&] { return r.srv.serviceId(); }); // takes the server's lock
    const bool answered = asked.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    {
        std::lock_guard<std::mutex> lk(gate->m);
        gate->released = true;
        gate->armed = false;
    }
    gate->cv.notify_all();
    KV_CHECK_MSG(answered, "a call needing the server's lock waited on the adapter read");
    KV_CHECK(!asked.get().empty());
    KV_CHECK(closedWithin(stranger, 5000)); // a stranger still: closed before TLS once the read is in
    closesocket(stranger);
    KV_CHECK(r.logHas("refused_local_only", "home", "192.168.50.9"));
}

// A stranger's connections read the adapters again at most once every 2 s, however many there are: within 2 s of a
// read the list answers them, and the first after that reads once more.
KV_TEST(outside_strangers_read_the_adapters_at_most_once_every_2_s) {
    auto reads = std::make_shared<std::atomic<int>>(0);
    Rig r(L"home-read-count");
    KV_REQUIRE(r.start([reads](HostStreamServer::Options& o) {
        o.on_link_prefixes.reset();
        o.adapters_for_test = [reads]() -> std::optional<std::vector<outside::AdapterInfo>> {
            reads->fetch_add(1);
            return std::vector<outside::AdapterInfo>{homeWifi()};
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner);
    KV_REQUIRE(pr.ok);
    KV_CHECK_MSG(reads->load() == 0, "loopback (the pairing here) read the adapters"); // loopback needs no read
    // A burst of strangers, each closed before TLS: as many reads as the 2 s allow, and no more.
    auto burst = [&](int first, int count) {
        for (int i = 0; i < count; ++i) {
            r.src->setStream("192.168.50." + std::to_string(first + i));
            const SOCKET s = rawTo(pr.streamPort);
            KV_CHECK(s != INVALID_SOCKET && closedWithin(s, 3000));
            closesocket(s);
        }
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const int before = reads->load();
    const auto t0 = std::chrono::steady_clock::now();
    burst(10, 12);
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  12 strangers in %lld ms: %d read(s)\n", static_cast<long long>(took), reads->load() - before);
    // The first stranger reads (the list is over 2 s old); each read after it needs 2 s more.
    const int burstReads = reads->load() - before;
    KV_CHECK_MSG(burstReads >= 1 && burstReads <= 1 + static_cast<int>(took / 2000),
                 std::to_string(burstReads) + " reads in " + std::to_string(took) + " ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const int again = reads->load();
    burst(30, 3);
    KV_CHECK_MSG(reads->load() - again == 1, std::to_string(reads->load() - again));
}

// A failed adapter read keeps the last list, but not for ever: once reads have failed for longer than a list is kept
// (home_list_keep_ms, 5 minutes in the product, 3 s here), the list may be another network's, and home is loopback
// alone until a read works again. Then the home phone is home again.
KV_TEST(outside_a_home_list_that_cant_be_read_again_is_kept_only_so_long) {
    auto fail = std::make_shared<std::atomic<bool>>(false);
    Rig r(L"home-read-expires");
    KV_REQUIRE(r.start([fail](HostStreamServer::Options& o) {
        o.on_link_prefixes.reset();
        o.home_list_keep_ms = 3000;
        o.adapters_for_test = [fail]() -> std::optional<std::vector<outside::AdapterInfo>> {
            if (fail->load()) return std::nullopt;
            return std::vector<outside::AdapterInfo>{homeWifi()};
        };
    }));
    FakePhone owner(3);
    const auto pr = pairAtHome(r.srv, owner, PetRole::Owner); // from loopback: no adapter read
    KV_REQUIRE(pr.ok);
    std::string err;
    auto homeConnects = [&]() {
        r.src->setStream("192.168.1.42");
        Seen s;
        if (owner.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err)) {
            pump(owner, s, 3000, [](const Seen& x) { return !x.hellos.empty(); });
        }
        owner.close();
        return !s.hellos.empty();
    };
    auto stranger = [&](const char* addr) {
        r.src->setStream(addr);
        const SOCKET s = rawTo(pr.streamPort);
        KV_CHECK(s != INVALID_SOCKET && closedWithin(s, 3000));
        closesocket(s);
    };
    KV_REQUIRE(homeConnects()); // the read that works: 192.168.1.0/24 is home
    fail->store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    stranger("192.168.50.20"); // a read that fails, 2 s after the good one: the list is kept
    KV_CHECK_MSG(homeConnects(), "a list read 2 s ago wasn't kept");
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    stranger("192.168.50.21"); // a read that fails, over 3 s after the good one: the list goes
    KV_CHECK_MSG(!homeConnects(), "a list past its keep time still let a home phone in");
    KV_CHECK(r.logHas("refused_local_only", "home", "192.168.1.42"));
    fail->store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    KV_CHECK_MSG(homeConnects(), "the home phone wasn't home again once a read worked");
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return kvtest::runAll(argc, argv);
}