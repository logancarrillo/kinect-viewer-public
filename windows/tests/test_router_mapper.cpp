// The router port mapping (v3 §8.2) and the public address it reads (§8.3), against a fake router
// behind the RouterSession seam. No test reaches NATUPnP or the real router, not even to read.
#include "kv_test.hpp"

#include "router_mapper.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using namespace kvhost::outside;

namespace {

// The routers' sample WAN addresses are documentation addresses: public only under a test program's own policy.
const AddressPolicy kSamples{true};

// A router: its mappings by external port, the WAN address it reports, and what it refuses.
struct FakeRouter {
    std::map<uint16_t, RouterMapping> mappings;
    std::string wan = "203.0.113.160";
    bool present = true; // false: the collection is NULL (no IGD, or UPnP off)
    int nullTimes = 0;   // the first n opens find no collection yet
    bool refuseAdd = false;
    bool dropOnAdd = false; // Add succeeds but nothing reads back
    int opens = 0, adds = 0, removes = 0;
};

class FakeSession : public RouterSession {
public:
    explicit FakeSession(FakeRouter& r) : r_(r) {}
    std::optional<RouterMapping> find(uint16_t port) override {
        auto it = r_.mappings.find(port);
        if (it == r_.mappings.end()) return std::nullopt;
        RouterMapping m = it->second;
        m.externalAddress = r_.wan;
        return m;
    }
    bool add(uint16_t port, const std::string& client, const std::string& description) override {
        ++r_.adds;
        if (r_.refuseAdd || description != kMappingDescription) return false;
        if (!r_.dropOnAdd) r_.mappings[port] = RouterMapping{client, port, true, {}, description};
        return true;
    }
    bool remove(uint16_t port) override {
        ++r_.removes;
        return r_.mappings.erase(port) == 1;
    }

private:
    FakeRouter& r_;
};

RouterOpener opener(FakeRouter& r) {
    return [&r]() -> std::unique_ptr<RouterSession> {
        ++r.opens;
        if (!r.present || r.nullTimes > 0) {
            if (r.nullTimes > 0) --r.nullTimes;
            return nullptr;
        }
        return std::make_unique<FakeSession>(r);
    };
}

// A clock that moves only when the mapper sleeps.
struct FakeTime {
    uint64_t now = 0;
    uint64_t slept = 0;
    PortMapper::Timing timing() {
        PortMapper::Timing t;
        t.nowMs = [this] { return now; };
        t.sleepMs = [this](uint64_t ms) {
            now += ms;
            slept += ms;
        };
        t.lanAddress = [] { return std::optional<std::string>("192.168.1.20"); };
        return t;
    }
};

constexpr uint16_t kPort = 41234;
const std::string kPc = "192.168.1.20";

} // namespace

KV_TEST(a_new_mapping_is_added_and_read_back) {
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    const MappingReport rep = m.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::Mapped);
    KV_CHECK(rep.wanAddress == "203.0.113.160" && rep.message.empty());
    KV_CHECK(r.adds == 1 && r.mappings.count(kPort) == 1);
    KV_CHECK(r.mappings[kPort].internalClient == kPc && r.mappings[kPort].internalPort == kPort);
    KV_CHECK(std::string(outsideStatusName(rep.status)) == "mapped");
}

KV_TEST(this_pcs_leftover_mapping_is_reused) {
    FakeRouter r;
    r.mappings[kPort] = RouterMapping{kPc, kPort, true, {}, kMappingDescription}; // ours: this PC and our description
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    KV_CHECK(r.adds == 0);
}

KV_TEST(a_port_taken_on_the_router_is_an_error) {
    FakeRouter r;
    r.mappings[kPort] = RouterMapping{"192.168.1.99", kPort, true, {}};
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    const MappingReport rep = m.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::Error);
    KV_CHECK(rep.message == "That port is already in use on the router. Pick another port.");
    KV_CHECK(r.adds == 0 && r.mappings[kPort].internalClient == "192.168.1.99");
    // Turning off never removes someone else's mapping.
    m.remove();
    KV_CHECK(r.removes == 0 && r.mappings.count(kPort) == 1);
    // This PC on another internal port is someone else too.
    FakeRouter r2;
    r2.mappings[kPort] = RouterMapping{kPc, 8080, true, {}};
    PortMapper m2(opener(r2), t.timing(), kSamples);
    KV_CHECK(m2.open(kPort, kPc).status == OutsideStatus::Error);
}

KV_TEST(no_router_is_looked_for_about_ten_seconds_then_manual) {
    FakeRouter r;
    r.present = false;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    const MappingReport rep = m.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::Manual);
    KV_CHECK(t.slept >= 10'000 && t.slept <= 11'000);
    KV_CHECK(rep.message.find("Forward TCP port 41234 to 192.168.1.20") != std::string::npos);
    KV_CHECK(rep.wanAddress.empty());
}

KV_TEST(a_router_found_late_is_still_mapped) {
    FakeRouter r;
    r.nullTimes = 3;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    KV_CHECK(r.opens == 4 && t.slept == 3'000);
}

KV_TEST(a_refused_or_unreadable_add_is_manual) {
    FakeTime t;
    FakeRouter refusing;
    refusing.refuseAdd = true;
    PortMapper a(opener(refusing), t.timing(), kSamples);
    KV_CHECK(a.open(kPort, kPc).status == OutsideStatus::Manual);
    FakeRouter dropping;
    dropping.dropOnAdd = true;
    PortMapper b(opener(dropping), t.timing(), kSamples);
    KV_CHECK(b.open(kPort, kPc).status == OutsideStatus::Manual);
}

KV_TEST(no_usable_internal_address_is_an_error_without_asking_the_router) {
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    for (const char* bad : {"127.0.0.1", "203.0.113.160", "", "fe80::1", "100.64.0.5"}) {
        KV_CHECK(m.open(kPort, bad).status == OutsideStatus::Error);
    }
    KV_CHECK(r.opens == 0 && r.adds == 0);
    KV_CHECK(usableInternalClient("10.0.0.5") && usableInternalClient("172.16.4.4") && usableInternalClient(kPc));
}

KV_TEST(a_wan_of_zero_is_an_error_and_100_64_is_carrier_nat) {
    FakeTime t;
    FakeRouter zero;
    zero.wan = "0.0.0.0";
    PortMapper a(opener(zero), t.timing(), kSamples);
    MappingReport rep = a.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::Error);
    KV_CHECK(rep.message == "The router isn't connected to the internet.");
    FakeRouter cgn;
    cgn.wan = "100.64.12.34";
    PortMapper b(opener(cgn), t.timing(), kSamples);
    rep = b.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::CarrierNat);
    KV_CHECK(std::string(outsideStatusName(rep.status)) == "carrier_nat");
    KV_CHECK(rep.message.find("100.64.12.34") != std::string::npos);
    FakeRouter doubleNat;
    doubleNat.wan = "192.168.0.2";
    PortMapper c(opener(doubleNat), t.timing(), kSamples);
    KV_CHECK(c.open(kPort, kPc).status == OutsideStatus::CarrierNat);
}

// Under the product's policy, the default, a router reporting a documentation address is behind carrier NAT
// like any other address that isn't public, and its forward is removed at once; the sample policy alone maps it.
KV_TEST(a_documentation_wan_is_carrier_nat_under_the_products_policy) {
    FakeTime t;
    FakeRouter r;
    r.wan = "203.0.113.160";
    PortMapper product(opener(r), t.timing());
    const MappingReport rep = product.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::CarrierNat);
    KV_CHECK(rep.message.find("203.0.113.160") != std::string::npos);
    KV_CHECK(r.mappings.empty()); // nothing left open on the router
    FakeRouter s;
    s.wan = "203.0.113.160";
    PortMapper samples(opener(s), t.timing(), kSamples);
    KV_CHECK(samples.open(kPort, kPc).status == OutsideStatus::Mapped);
    KV_CHECK(s.mappings.size() == 1);
}

KV_TEST(a_mapping_that_disappears_is_added_again_on_the_check) {
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    r.mappings.clear(); // the router rebooted
    const int opensBefore = r.opens;
    KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
    KV_CHECK(r.opens == opensBefore + 1); // a fresh session each check
    KV_CHECK(r.adds == 2 && r.mappings.count(kPort) == 1);
    // A mapping switched off on the router is put back as it should be.
    r.mappings[kPort].enabled = false;
    KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
    KV_CHECK(r.mappings[kPort].enabled);
}

// Only a mapping with our description and this PC as its internal client is
// ours. One on our port to this PC with another description is someone else's (another app on this PC): "port in
// use", never adopted, and never removed on any path (off, a check, Remove mapping now, the exit).
KV_TEST(a_mapping_to_this_pc_with_another_description_is_taken_and_never_removed) {
    FakeRouter r;
    r.mappings[kPort] = RouterMapping{kPc, kPort, true, {}, "Another app"};
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    const MappingReport rep = m.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::Error);
    KV_CHECK(rep.message == "That port is already in use on the router. Pick another port.");
    KV_CHECK(r.adds == 0 && r.removes == 0 && r.mappings[kPort].description == "Another app");
    KV_CHECK(m.mayHoldPort() == 0);
    KV_CHECK(m.refresh().status == OutsideStatus::Error); // the check finds it taken too
    m.remove();
    KV_CHECK(r.removes == 0 && r.mappings.count(kPort) == 1 && r.mappings[kPort].description == "Another app");
    // Switched off on the router, it is still someone else's: not removed and put back as ours.
    FakeRouter off;
    off.mappings[kPort] = RouterMapping{kPc, kPort, false, {}, "Another app"};
    PortMapper m2(opener(off), t.timing(), kSamples);
    KV_CHECK(m2.open(kPort, kPc).status == OutsideStatus::Error);
    m2.remove();
    KV_CHECK(off.removes == 0 && off.adds == 0 && off.mappings[kPort].description == "Another app");
    // Ours (our description) is still adopted, and removed as before.
    FakeRouter mine;
    mine.mappings[kPort] = RouterMapping{kPc, kPort, true, {}, kMappingDescription};
    PortMapper m3(opener(mine), t.timing(), kSamples);
    KV_CHECK(m3.open(kPort, kPc).status == OutsideStatus::Mapped);
    KV_CHECK(mine.adds == 0);
    KV_CHECK(m3.remove() && mine.mappings.empty() && mine.removes == 1);
    // A check that finds our mapping's description changed on the router: not ours any more, and left alone.
    FakeRouter changed;
    PortMapper m4(opener(changed), t.timing(), kSamples);
    KV_CHECK(m4.open(kPort, kPc).status == OutsideStatus::Mapped);
    changed.mappings[kPort].description = "Renamed by someone";
    KV_CHECK(m4.refresh().status == OutsideStatus::Error);
    m4.remove();
    KV_CHECK(changed.mappings.count(kPort) == 1 && changed.removes == 0);
}

KV_TEST(a_check_that_finds_the_port_taken_reports_it) {
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    r.mappings[kPort].internalClient = "192.168.1.99";
    KV_CHECK(m.refresh().status == OutsideStatus::Error);
}

KV_TEST(a_router_that_stops_answering_reads_manual_until_it_answers) {
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    r.present = false;
    KV_CHECK(m.refresh().status == OutsideStatus::Manual);
    r.present = true;
    KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
}

KV_TEST(an_address_change_reads_changed_until_acknowledged) {
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
    r.wan = "203.0.113.161";
    MappingReport rep = m.refresh();
    KV_CHECK(rep.status == OutsideStatus::AddressChanged && rep.wanAddress == "203.0.113.161");
    KV_CHECK(std::string(outsideStatusName(rep.status)) == "address_changed");
    KV_CHECK(m.refresh().status == OutsideStatus::AddressChanged);
    // v3 §8.3: only the address read now is acknowledged (the one the PC user was shown, or the gateway confirmed).
    KV_CHECK(!m.acknowledgeAddressChange("203.0.113.160"));
    KV_CHECK(!m.acknowledgeAddressChange(""));
    KV_CHECK(m.report().status == OutsideStatus::AddressChanged);
    KV_CHECK(m.acknowledgeAddressChange("203.0.113.161"));
    KV_CHECK(m.report().status == OutsideStatus::Mapped);
    KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
    KV_CHECK(!m.acknowledgeAddressChange("203.0.113.161")); // nothing waits any more
}

// v3 §8.3: the address devices were last told is saved and seeded at start, so a first read that differs is a change
// (checked, not told at once) and one that matches isn't; a corrupt saved one ("unknown") makes any first read a
// change. forgetAddress (the PC user turned outside viewing on) takes the first address as it is.
KV_TEST(a_seeded_announced_address_makes_a_different_first_read_a_change) {
    FakeRouter r; // reports 203.0.113.160
    FakeTime t;
    {
        PortMapper m(opener(r), t.timing(), kSamples);
        m.seedAnnouncedAddress("203.0.113.160");
        KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    }
    for (const char* seed : {"203.0.113.150", "unknown"}) {
        PortMapper m(opener(r), t.timing(), kSamples);
        m.seedAnnouncedAddress(seed);
        const MappingReport rep = m.open(kPort, kPc);
        KV_CHECK_MSG(rep.status == OutsideStatus::AddressChanged && rep.wanAddress == "203.0.113.160", seed);
        KV_CHECK(m.acknowledgeAddressChange("203.0.113.160"));
        KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
    }
    PortMapper m(opener(r), t.timing(), kSamples);
    m.seedAnnouncedAddress("203.0.113.150");
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::AddressChanged);
    m.forgetAddress();
    KV_CHECK(m.report().status == OutsideStatus::Mapped);
    KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
}

KV_TEST(turning_outside_off_removes_the_mapping) {
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    KV_CHECK(m.remove());
    KV_CHECK(r.mappings.empty() && r.removes == 1);
    KV_CHECK(m.report().status == OutsideStatus::Off);
    // Off stays off: the 10-minute check doesn't put it back.
    KV_CHECK(m.refresh().status == OutsideStatus::Off);
    KV_CHECK(r.mappings.empty() && r.adds == 1);
}

KV_TEST(a_forward_that_fails_the_wan_check_is_removed_and_not_put_back) {
    // Carrier NAT or no internet leaves no forward open on the router, and the
    // 10-minute check doesn't add it again while it isn't mapped.
    for (const char* wan : {"100.64.12.34", "0.0.0.0"}) {
        FakeRouter r;
        r.wan = wan;
        FakeTime t;
        PortMapper m(opener(r), t.timing(), kSamples);
        const OutsideStatus s = m.open(kPort, kPc).status;
        KV_CHECK(s == OutsideStatus::CarrierNat || s == OutsideStatus::Error);
        KV_CHECK(r.mappings.empty());
        const int adds = r.adds;
        KV_CHECK(m.refresh().status == s);
        KV_CHECK(r.adds == adds && r.mappings.empty());
    }
}

KV_TEST(a_leftover_forward_that_fails_the_wan_check_is_removed_too) {
    FakeRouter r;
    r.wan = "100.64.12.34";
    r.mappings[kPort] = RouterMapping{kPc, kPort, true, {}, kMappingDescription};
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::CarrierNat);
    KV_CHECK(r.mappings.empty());
}

namespace {
// A router that never answers until released (no NATUPnP call may hang the host).
struct Hang {
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> entered{0}, left{0};
    void waitHere() {
        ++entered;
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [this] { return release; });
        ++left;
    }
};
class HangingSession : public RouterSession {
public:
    explicit HangingSession(Hang& h) : h_(h) {}
    std::optional<RouterMapping> find(uint16_t) override {
        h_.waitHere();
        return std::nullopt;
    }
    bool add(uint16_t, const std::string&, const std::string&) override { return false; }
    bool remove(uint16_t) override { return false; }

private:
    Hang& h_;
};
} // namespace

KV_TEST(a_router_that_never_answers_gives_up_at_the_deadline) {
    static Hang hang; // outlives the abandoned worker
    FakeTime t;
    PortMapper::Timing timing = t.timing();
    timing.callDeadlineMs = 200;
    PortMapper m([] { return std::unique_ptr<RouterSession>(std::make_unique<HangingSession>(hang)); }, timing, kSamples);
    const auto start = std::chrono::steady_clock::now();
    const MappingReport rep = m.open(kPort, kPc);
    KV_CHECK(rep.status == OutsideStatus::Manual);
    KV_CHECK(rep.message.find("didn't answer") != std::string::npos);
    // While that call is stuck, the next ones give up at once and start no other.
    KV_CHECK(m.refresh().status == OutsideStatus::Manual);
    m.remove();
    KV_CHECK(m.report().status == OutsideStatus::Off);
    const auto took = std::chrono::steady_clock::now() - start;
    KV_CHECK(took < std::chrono::seconds(3));
    KV_CHECK(hang.entered == 1);
    {
        std::lock_guard<std::mutex> lock(hang.mu);
        hang.release = true;
    }
    hang.cv.notify_all();
    for (int i = 0; i < 200 && hang.left != hang.entered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    KV_CHECK(hang.left == hang.entered);
}

KV_TEST(the_check_follows_a_new_lan_address) {
    // refresh() reads this PC's address again and maps to the new one.
    FakeRouter r;
    FakeTime t;
    std::optional<std::string> lan = kPc;
    PortMapper::Timing timing = t.timing();
    timing.lanAddress = [&lan] { return lan; };
    PortMapper m(opener(r), timing, kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    lan = "192.168.1.21";
    KV_CHECK(m.refresh().status == OutsideStatus::Mapped);
    KV_CHECK(r.mappings.size() == 1 && r.mappings[kPort].internalClient == "192.168.1.21");
    // No usable address: an error, and the old forward goes.
    lan = std::nullopt;
    KV_CHECK(m.refresh().status == OutsideStatus::Error);
    KV_CHECK(r.mappings.empty());
}

KV_TEST(an_address_change_survives_a_failed_check_and_a_reopen) {
    // §8.3: the last public address is kept across a failed check, remove() and open(), so a new one is
    // still address_changed; a failed check never drops an unacknowledged change.
    FakeRouter r;
    FakeTime t;
    PortMapper m(opener(r), t.timing(), kSamples);
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
    r.wan = "0.0.0.0";
    KV_CHECK(m.refresh().status == OutsideStatus::Error);
    r.wan = "203.0.113.161";
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::AddressChanged); // the 10-minute check opens again
    m.remove();
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::AddressChanged); // turned off and on: still unacknowledged
    r.wan = "100.64.1.2";
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::CarrierNat);
    r.wan = "203.0.113.161";
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::AddressChanged);
    KV_CHECK(m.acknowledgeAddressChange("203.0.113.161"));
    KV_CHECK(m.report().status == OutsideStatus::Mapped);
    m.remove();
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Mapped);
}

KV_TEST(a_leftover_forward_is_removed_only_with_our_description_and_client) {
    FakeTime t;
    {
        FakeRouter r;
        r.mappings[kPort] = RouterMapping{kPc, kPort, true, {}, kMappingDescription};
        PortMapper m(opener(r), t.timing(), kSamples);
        KV_CHECK(m.removeLeftover(kPort, kPc).kind == PortMapper::Outcome::Removed);
        KV_CHECK(r.mappings.empty());
    }
    {
        FakeRouter r;
        r.mappings[kPort] = RouterMapping{kPc, kPort, true, {}, "Another app"};
        PortMapper m(opener(r), t.timing(), kSamples);
        KV_CHECK(m.removeLeftover(kPort, kPc).kind == PortMapper::Outcome::Absent);
        KV_CHECK(r.mappings.size() == 1 && r.removes == 0);
    }
    {
        FakeRouter r;
        r.mappings[kPort] = RouterMapping{"192.168.1.99", kPort, true, {}, kMappingDescription};
        PortMapper m(opener(r), t.timing(), kSamples);
        KV_CHECK(m.removeLeftover(kPort, kPc).kind == PortMapper::Outcome::Absent);
        KV_CHECK(r.mappings.size() == 1 && r.removes == 0);
    }
}

namespace {
// A router whose Add can be held past the deadline and then answer (thread-safe: the abandoned call runs on).
struct SlowAdd {
    std::mutex mu;
    std::condition_variable cv;
    std::map<uint16_t, RouterMapping> mappings;
    bool holdAdd = true;
    int removes = 0;
};
class SlowAddSession : public RouterSession {
public:
    explicit SlowAddSession(std::shared_ptr<SlowAdd> r) : r_(std::move(r)) {}
    std::optional<RouterMapping> find(uint16_t port) override {
        std::lock_guard<std::mutex> lk(r_->mu);
        auto it = r_->mappings.find(port);
        if (it == r_->mappings.end()) return std::nullopt;
        RouterMapping m = it->second;
        m.externalAddress = "203.0.113.160";
        return m;
    }
    bool add(uint16_t port, const std::string& client, const std::string& description) override {
        std::unique_lock<std::mutex> lk(r_->mu);
        r_->cv.wait(lk, [&] { return !r_->holdAdd; });
        r_->mappings[port] = RouterMapping{client, port, true, {}, description};
        return true;
    }
    bool remove(uint16_t port) override {
        std::lock_guard<std::mutex> lk(r_->mu);
        ++r_->removes;
        return r_->mappings.erase(port) == 1;
    }

private:
    std::shared_ptr<SlowAdd> r_;
};
} // namespace

KV_TEST(a_removal_kept_while_the_router_was_stuck_runs_once_it_answers) {
    // §8.2: turning off while an Add is stuck past its deadline must not leave the forward behind.
    auto slow = std::make_shared<SlowAdd>();
    FakeTime t;
    PortMapper::Timing timing = t.timing();
    timing.callDeadlineMs = 150;
    PortMapper m([slow] { return std::unique_ptr<RouterSession>(std::make_unique<SlowAddSession>(slow)); }, timing,
                 kSamples);
    std::atomic<int> unstuck{0};
    m.setOnUnstuck([&unstuck] { ++unstuck; });
    KV_CHECK(m.open(kPort, kPc).status == OutsideStatus::Manual); // given up at the deadline
    KV_CHECK(!m.remove());                                        // can't run yet: kept
    KV_CHECK(m.mayHoldPort() == kPort);
    {
        std::lock_guard<std::mutex> lk(slow->mu);
        slow->holdAdd = false;
    }
    slow->cv.notify_all();
    for (int i = 0; i < 300 && unstuck.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    KV_REQUIRE(unstuck.load() == 1);
    {
        std::lock_guard<std::mutex> lk(slow->mu);
        KV_CHECK(slow->mappings.size() == 1); // the stuck Add made the forward after all
    }
    KV_CHECK(m.retryPendingRemovals());
    KV_CHECK(m.mayHoldPort() == 0);
    std::lock_guard<std::mutex> lk(slow->mu);
    KV_CHECK(slow->mappings.empty() && slow->removes == 1);
}

// v3 §8.3, pin the gateway and ask it directly: one pass's verdict. Another gateway, or the gateway contradicting
// NATUPnP, holds; a part that can't be read is no evidence (read again); the address is told only when one of the
// pinned gateway's own public answers is what NATUPnP read (every WAN service is asked: one may hold the public
// address while another answers 0.0.0.0 or a private one, and those count as unread before the judge).
KV_TEST(gateway_judge_needs_the_pinned_gateway_to_confirm_the_address_itself) {
    const GatewayIdentity pin{"192.168.1.1", "00-00-5e-00-53-01"};
    using Wans = std::vector<std::string>;
    const auto pass = [&](GatewayIdentity gateway, std::string reported, Wans own) {
        return judgeGateway(pin, GatewayReading{std::move(gateway), std::move(reported), std::move(own)});
    };
    KV_CHECK(pass(pin, "203.0.113.9", Wans{"203.0.113.9"}) == GatewayVerdict::Match);
    KV_CHECK(pass(pin, "203.0.113.9", Wans{"203.0.113.50", "203.0.113.9"}) == GatewayVerdict::Match); // any service
    KV_CHECK(pass({"192.168.1.254", pin.mac}, "203.0.113.9", Wans{"203.0.113.9"}) == GatewayVerdict::OtherGateway);
    KV_CHECK(pass({pin.ip, "00-00-5e-00-53-66"}, "203.0.113.9", Wans{"203.0.113.9"}) == GatewayVerdict::OtherGateway);
    KV_CHECK(pass({pin.ip, "00-00-5e-00-53-66"}, "203.0.113.9", Wans{}) == GatewayVerdict::OtherGateway);
    KV_CHECK(pass({"", "00-00-5e-00-53-66"}, "", Wans{}) == GatewayVerdict::OtherGateway);
    KV_CHECK(pass(pin, "203.0.113.66", Wans{"203.0.113.9"}) == GatewayVerdict::Contradicted); // NATUPnP heard someone else
    KV_CHECK(pass(pin, "203.0.113.66", Wans{"203.0.113.9", "203.0.113.50"}) == GatewayVerdict::Contradicted);
    KV_CHECK(pass(pin, "203.0.113.9", Wans{}) == GatewayVerdict::Unreadable); // no public answer from its UPnP
    KV_CHECK(pass({pin.ip, ""}, "203.0.113.9", Wans{"203.0.113.9"}) == GatewayVerdict::Unreadable); // no neighbour entry yet
    KV_CHECK(pass(GatewayIdentity{}, "203.0.113.9", Wans{"203.0.113.9"}) == GatewayVerdict::Unreadable);
    KV_CHECK(pass(pin, "", Wans{"203.0.113.9"}) == GatewayVerdict::Unreadable);
    KV_CHECK(judgeGateway(GatewayIdentity{}, GatewayReading{pin, "203.0.113.9", Wans{"203.0.113.9"}}) == GatewayVerdict::NoPin);
    KV_CHECK(judgeGateway(GatewayIdentity{pin.ip, ""}, GatewayReading{pin, "203.0.113.9", Wans{"203.0.113.9"}}) ==
             GatewayVerdict::NoPin);
    KV_CHECK(std::string(gatewayPinKind(pin)) == "address and hardware address");
    KV_CHECK(std::string(gatewayPinKind(GatewayIdentity{pin.ip, ""})).empty());
}
// The pin's parts as saved: a dotted IPv4 exactly, and a lower-case "aa-bb-cc-dd-ee-ff".
KV_TEST(gateway_pin_parts_are_checked_as_saved) {
    KV_CHECK(validGatewayIp("192.168.1.1") && validGatewayIp("10.0.0.138") && validGatewayIp("127.0.0.1"));
    for (const char* bad : {"", "192.168.1", "192.168.001.001", "0xc0.168.1.1", "192.168.1.1 ", "::1", "router.lan", "256.1.1.1",
                            "3232235777"}) {
        KV_CHECK_MSG(!validGatewayIp(bad), bad);
    }
    KV_CHECK(validGatewayMac("00-00-5e-00-53-01"));
    for (const char* bad : {"", "00-00-5E-00-53-01", "00:00:5e:00:53:01", "00-00-5e-00-53", "00-00-5e-00-53-01-02", "0g-00-5e-00-53-01"}) {
        KV_CHECK_MSG(!validGatewayMac(bad), bad);
    }
}

// The gateway's UPnP URLs, cracked with WinHttpCrackUrl: plain http on the pinned address in dotted form, with no
// user-info. Another device, a name, IPv6, a number in another form, https or a space: refused.
KV_TEST(gateway_urls_are_http_on_the_pinned_gateway_address_only) {
    const std::string gw = "192.168.1.1";
    const auto ok = crackGatewayUrl("http://192.168.1.1:5000/rootDesc.xml", gw);
    KV_REQUIRE(ok.has_value());
    KV_CHECK(ok->host == gw && ok->port == 5000 && ok->path == "/rootDesc.xml");
    const auto query = crackGatewayUrl("http://192.168.1.1/ctl/IPConn?x=1", gw);
    KV_CHECK(query.has_value() && query->port == 80 && query->path == "/ctl/IPConn?x=1");
    const auto bare = crackGatewayUrl("http://192.168.1.1:49152", gw);
    KV_CHECK(bare.has_value() && bare->path == "/");
    for (const char* bad : {"https://192.168.1.1/rootDesc.xml", "http://user@192.168.1.1/rootDesc.xml",
                            "http://user:pw@192.168.1.1/rootDesc.xml", "http://192.168.1.1@192.168.1.66/rootDesc.xml",
                            "http://192.168.1.66:5000/rootDesc.xml", "http://[fe80::1]:5000/rootDesc.xml",
                            "http://router.lan/rootDesc.xml", "http://0xc0.0xa8.1.1/rootDesc.xml",
                            "http://192.168.001.001/rootDesc.xml", "http://3232235777/rootDesc.xml",
                            "http://192.168.1.1 /rootDesc.xml", "ftp://192.168.1.1/rootDesc.xml", "192.168.1.1/rootDesc.xml", ""}) {
        KV_CHECK_MSG(!crackGatewayUrl(bad, gw).has_value(), bad);
    }
    KV_CHECK(!crackGatewayUrl("http://192.168.1.1/x", "not an address").has_value());
}

// An SSDP search reply counts only when it comes from the pinned gateway and points at it: a device at any other
// address answering the search, whatever it points at, is ignored, and so is the gateway's address pointing away.
KV_TEST(an_ssdp_reply_counts_only_from_the_pinned_gateway_about_itself) {
    const std::string gw = "192.168.1.1";
    const std::string fromGateway = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\n"
                                    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                                    "Location: http://192.168.1.1:5000/rootDesc.xml\r\nSERVER: test\r\n\r\n";
    KV_CHECK(ssdpLocation(fromGateway) == "http://192.168.1.1:5000/rootDesc.xml");
    KV_CHECK(gatewayLocationFrom(gw, fromGateway, gw) == "http://192.168.1.1:5000/rootDesc.xml");
    const std::string rogue = "HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.66:5000/rootDesc.xml\r\n\r\n";
    KV_CHECK(gatewayLocationFrom("192.168.1.66", rogue, gw).empty());
    KV_CHECK(gatewayLocationFrom("192.168.1.66", fromGateway, gw).empty());
    KV_CHECK(gatewayLocationFrom(gw, rogue, gw).empty());
    KV_CHECK(gatewayLocationFrom(gw, "HTTP/1.1 200 OK\r\nLOCATION: http://x@192.168.1.1:5000/d.xml\r\n\r\n", gw).empty());
    KV_CHECK(ssdpLocation("NOTIFY * HTTP/1.1\r\nLOCATION: http://192.168.1.1:5000/rootDesc.xml\r\n\r\n").empty());
    KV_CHECK(ssdpLocation("HTTP/1.1 200 OK\nlocation:   http://192.168.1.1/d.xml  \n\n") == "http://192.168.1.1/d.xml");
    KV_CHECK(ssdpLocation("HTTP/1.1 200 OK\r\n\r\nLOCATION: http://192.168.1.1/late.xml\r\n").empty());
}

// The description's WAN connection services, on the gateway only: a control URL (or a URLBase) on another device,
// or with user-info, is dropped; a relative one is resolved against URLBase or the description's own URL. A
// document type definition is refused outright, and so is a document that isn't whole.
KV_TEST(the_gateway_description_gives_its_wan_connection_on_the_gateway_only) {
    const std::string gw = "192.168.1.1", loc = "http://192.168.1.1:5000/desc/root.xml";
    const std::string xml =
        "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major>"
        "<minor>0</minor></specVersion><device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>"
        "<serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
        "<controlURL>/ctl/L3F</controlURL></service></serviceList><deviceList><device>"
        "<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>"
        "<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl/IPConn</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType><controlURL>ppp/ctl</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType>"
        "<controlURL>http://192.168.1.66:5000/ctl/rogue</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>http://u@192.168.1.1:5000/ctl/x</controlURL></service>"
        "</serviceList></device></deviceList></device></deviceList></device></root>";
    const auto s = gatewayServices(xml, loc, gw);
    KV_REQUIRE(s.size() == 2);
    KV_CHECK(s[0].type == "urn:schemas-upnp-org:service:WANIPConnection:1" && s[0].control.port == 5000 &&
             s[0].control.path == "/ctl/IPConn");
    KV_CHECK(s[1].type == "urn:schemas-upnp-org:service:WANPPPConnection:1" && s[1].control.path == "/desc/ppp/ctl");
    const std::string withBase = "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><URLBase>http://192.168.1.1:49000/</URLBase>"
                                 "<device><serviceList><service>"
                                 "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                                 "<controlURL>ctl/ip</controlURL></service></serviceList></device></root>";
    const auto b = gatewayServices(withBase, loc, gw);
    KV_CHECK(b.size() == 1 && b[0].control.port == 49000 && b[0].control.path == "/ctl/ip");
    std::string baseElsewhere = withBase;
    baseElsewhere.replace(baseElsewhere.find("192.168.1.1:49000"), 17, "192.168.1.66:4900");
    KV_CHECK(gatewayServices(baseElsewhere, loc, gw).empty());
    KV_CHECK(gatewayServices("<!DOCTYPE root [<!ENTITY x \"y\">]>" + withBase, loc, gw).empty());
    KV_CHECK(gatewayServices(withBase.substr(0, withBase.size() / 2), loc, gw).empty());
    KV_CHECK(gatewayServices("", loc, gw).empty());
    // Not a UPnP device description (another root, or none in the device namespace): nothing.
    std::string notUpnp = withBase;
    const std::string deviceNs = " xmlns=\"urn:schemas-upnp-org:device-1-0\"";
    notUpnp.replace(notUpnp.find(deviceNs), deviceNs.size(), "");
    KV_CHECK(gatewayServices(notUpnp, loc, gw).empty());
    KV_CHECK(gatewayServices("<html xmlns=\"urn:schemas-upnp-org:device-1-0\"><URLBase>http://192.168.1.1/</URLBase></html>",
                             loc, gw)
                 .empty());
}

// GetExternalIPAddress's answer: NewExternalIPAddress, a dotted IPv4, inside a SOAP envelope's
// GetExternalIPAddressResponse in a WAN connection service's namespace (WANIPConnection or WANPPPConnection, any
// version); nothing otherwise.
KV_TEST(the_gateway_answer_is_a_dotted_ipv4_in_a_soap_answer_or_nothing) {
    const std::string ip = "urn:schemas-upnp-org:service:WANIPConnection:1";
    const auto envelope = [](const std::string& ns, const std::string& inner) {
        return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
               "<u:GetExternalIPAddressResponse xmlns:u=\"" +
               ns + "\">" + inner + "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
    };
    const std::string answer = "<NewExternalIPAddress>203.0.113.9</NewExternalIPAddress>";
    for (const char* ns : {"urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2",
                           "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
        KV_CHECK_MSG(soapExternalAddress(envelope(ns, answer)) == "203.0.113.9", ns);
    }
    for (const char* ns : {"urn:schemas-upnp-org:service:Layer3Forwarding:1", "urn:schemas-upnp-org:service:WANIPConnection:",
                           "urn:schemas-upnp-org:service:WANIPConnection:1x", "urn:example:WANIPConnection:1", ""}) {
        KV_CHECK_MSG(soapExternalAddress(envelope(ns, answer)).empty(), ns);
    }
    KV_CHECK(soapExternalAddress(envelope(ip, "<NewExternalIPAddress> 203.0.113.9 </NewExternalIPAddress>")) == "203.0.113.9");
    KV_CHECK(soapExternalAddress(envelope(ip, "<NewExternalIPAddress></NewExternalIPAddress>")).empty());
    KV_CHECK(soapExternalAddress(envelope(ip, "<NewExternalIPAddress>gw.example</NewExternalIPAddress>")).empty());
    KV_CHECK(soapExternalAddress(envelope(ip, "<NewExternalIPAddress>2001:db8::1</NewExternalIPAddress>")).empty());
    KV_CHECK(soapExternalAddress(envelope(ip, "")).empty());
    KV_CHECK(soapExternalAddress(envelope(ip, answer + "<NewExternalIPAddress>203.0.113.10</NewExternalIPAddress>")).empty());
    // Not a SOAP answer: the address alone, or outside the envelope's Body.
    KV_CHECK(soapExternalAddress(answer).empty());
    KV_CHECK(soapExternalAddress("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                                 "<u:GetExternalIPAddressResponse xmlns:u=\"" +
                                 ip + "\">" + answer + "</u:GetExternalIPAddressResponse></s:Envelope>")
                 .empty());
    KV_CHECK(soapExternalAddress("not xml at all").empty());
}

// The router probe's output can be posted: every dotted IPv4 in a line is masked, by its name when the probe named
// it, else by its kind with a number that stays the same on later lines; 0.0.0.0 is kept; a hardware address in
// either form is masked; numbers that aren't a dotted IPv4 are left as they are.
KV_TEST(a_router_probe_line_masks_every_address) {
    AddressPolicy docs;
    docs.documentationIsPublic = true;
    std::vector<std::pair<std::string, std::string>> names = {{"192.168.100.1", "gateway"},
                                                              {"203.0.113.7", "NATUPnP's public address"}};
    KV_CHECK(maskProbeLine("gateway 192.168.100.1, answered 203.0.113.7.", names, docs) ==
             "gateway [gateway], answered [NATUPnP's public address].");
    KV_CHECK(maskProbeLine("answered 198.51.100.4 and 0.0.0.0", names, docs) == "answered [public address 1] and 0.0.0.0");
    KV_CHECK(maskProbeLine("again 198.51.100.4; then 198.51.100.5", names, docs) ==
             "again [public address 1]; then [public address 2]");
    KV_CHECK(maskProbeLine("from 192.168.100.2 and 100.64.0.1", names, docs) ==
             "from [non-public address 1] and [non-public address 2]");
    KV_CHECK(maskProbeLine("http://192.168.100.1:5000/rootDesc.xml", names, docs) == "http://[gateway]:5000/rootDesc.xml");
    KV_CHECK(maskProbeLine("x198.51.100.4x", names, docs) == "x[public address 1]x");
    KV_CHECK(maskProbeLine("mac 00-00-5e-00-53-01 or 00:00:5E:00:53:02.", names, docs) ==
             "mac [hardware address] or [hardware address].");
    KV_CHECK(maskProbeLine("WANIPConnection:1 at port 49152, 1.2.3 and v2.0", names, docs) ==
             "WANIPConnection:1 at port 49152, 1.2.3 and v2.0");
    KV_CHECK(maskProbeLine("http://192.168.100.1:5000/12345678-9abc-def0-1234-56789ABCDEF0/desc.xml", names, docs) ==
             "http://[gateway]:5000/[uuid]/desc.xml");
    KV_CHECK(maskProbeLine("http://192.168.100.1:5000/dev/00005e005301/desc.xml, serial SN0A1B2C3D4E", names, docs) ==
             "http://[gateway]:5000/dev/[id]/desc.xml, serial SN[id]");
    KV_CHECK(maskProbeLine("pass 1: 8006 ms, 49152 bytes, cafe123", names, docs) == "pass 1: 8006 ms, 49152 bytes, cafe123");
    // Under the product's policy a documentation address isn't public, and it is masked all the same.
    std::vector<std::pair<std::string, std::string>> none;
    KV_CHECK(maskProbeLine("203.0.113.9", none) == "[non-public address 1]");
}
