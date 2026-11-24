// W8 Phase A: the outside listener's admission limits (v3 §8.5, with a per-source cap of 2
// unauthenticated connections at once and the IPv6 /64 source). An injected clock; no sockets.
#include "kv_test.hpp"

#include "outside_limiter.hpp"

#include <vector>

using namespace kvhost::outside;

namespace {
uint64_t g_now = 1'000'000;
uint64_t testClock() { return g_now; }

IpAddress ip(const char* s) { return parseIp(s).value_or(IpAddress{}); }

std::shared_ptr<OutsideLimiter> limiter(LimiterConfig cfg = {}) {
    g_now = 1'000'000;
    return OutsideLimiter::create(cfg, &testClock);
}

// Admits and ends at once with r, as a quick handshake would.
Refusal connectAndEnd(OutsideLimiter& l, const char* addr, HandshakeResult r) {
    auto a = l.tryAdmit(ip(addr));
    if (a.refusal == Refusal::None) a.admission.finish(r);
    return a.refusal;
}
} // namespace

KV_TEST(the_per_source_bucket_trips_before_tls) {
    auto l = limiter();
    for (int i = 0; i < 4; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.1", HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "203.0.113.1", HandshakeResult::Timeout) == Refusal::RateLimit);
    KV_CHECK(std::string(refusalLogEvent(Refusal::RateLimit)) == "refused_rate_limit");
    // Another source has its own bucket.
    KV_CHECK(connectAndEnd(*l, "203.0.113.2", HandshakeResult::Timeout) == Refusal::None);
    // One token comes back every 10 s.
    g_now += 9'999;
    KV_CHECK(connectAndEnd(*l, "203.0.113.1", HandshakeResult::Timeout) == Refusal::RateLimit);
    g_now += 1;
    KV_CHECK(connectAndEnd(*l, "203.0.113.1", HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "203.0.113.1", HandshakeResult::Timeout) == Refusal::RateLimit);
}

KV_TEST(two_mapped_ipv4_sources_get_separate_buckets) {
    auto l = limiter();
    for (int i = 0; i < 4; ++i) KV_CHECK(connectAndEnd(*l, "::ffff:203.0.113.1", HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "::ffff:203.0.113.1", HandshakeResult::Timeout) == Refusal::RateLimit);
    // The same address unwrapped is the same source.
    KV_CHECK(connectAndEnd(*l, "203.0.113.1", HandshakeResult::Timeout) == Refusal::RateLimit);
    KV_CHECK(connectAndEnd(*l, "::ffff:203.0.113.2", HandshakeResult::Timeout) == Refusal::None);
}

KV_TEST(an_ipv6_host_rotating_through_its_slash_64_is_one_source) {
    auto l = limiter();
    const char* rotating[] = {"2001:db8:1:2::1", "2001:db8:1:2::2", "2001:db8:1:2:aaaa::3", "2001:db8:1:2:ffff::4"};
    for (const char* a : rotating) KV_CHECK(connectAndEnd(*l, a, HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "2001:db8:1:2:1234::5", HandshakeResult::Timeout) == Refusal::RateLimit);
    KV_CHECK(connectAndEnd(*l, "2001:db8:1:3::1", HandshakeResult::Timeout) == Refusal::None);
    // Its failures count together too: five from five addresses in the /64 block it.
    auto l2 = limiter(LimiterConfig{.burst = 100});
    for (int i = 0; i < 5; ++i) {
        const std::string a = "2001:db8:9:9::" + std::to_string(i + 1);
        KV_CHECK(connectAndEnd(*l2, a.c_str(), HandshakeResult::Failure) == Refusal::None);
    }
    KV_CHECK(l2->isBlocked("2001:db8:9:9::/64"));
    // Any address in the /64 is let in only as blocked (a paired certificate, and nothing else).
    auto b = l2->tryAdmit(ip("2001:db8:9:9:abcd::1"));
    KV_CHECK(b.refusal == Refusal::None && b.admission.blocked());
}

KV_TEST(two_unauthenticated_at_once_per_source_and_eight_in_all) {
    auto l = limiter(LimiterConfig{.burst = 100, .newPerMinute = 1000});
    auto a1 = l->tryAdmit(ip("203.0.113.1"));
    auto a2 = l->tryAdmit(ip("203.0.113.1"));
    KV_CHECK(a1.refusal == Refusal::None && a2.refusal == Refusal::None);
    KV_CHECK(l->tryAdmit(ip("203.0.113.1")).refusal == Refusal::TooManyInFlight);
    KV_CHECK(l->inFlightFor("203.0.113.1") == 2);
    // The handshake ending (any result) frees the slot at once.
    a1.admission.finish(HandshakeResult::Success);
    KV_CHECK(l->inFlightFor("203.0.113.1") == 1);
    auto a3 = l->tryAdmit(ip("203.0.113.1"));
    KV_CHECK(a3.refusal == Refusal::None);

    std::vector<Admission> held;
    held.push_back(std::move(a2.admission));
    held.push_back(std::move(a3.admission));
    for (int i = 0; i < 6; ++i) {
        const std::string a = "198.51.100." + std::to_string(i + 1);
        auto r = l->tryAdmit(ip(a.c_str()));
        KV_CHECK(r.refusal == Refusal::None);
        held.push_back(std::move(r.admission));
    }
    KV_CHECK(l->inFlight() == 8);
    KV_CHECK(l->tryAdmit(ip("192.0.2.77")).refusal == Refusal::TooManyInFlight);
    KV_CHECK(std::string(refusalLogEvent(Refusal::TooManyInFlight)) == "refused_too_many");
    // Dropping a ticket (the socket closed) frees its slot without counting anything.
    held.pop_back();
    KV_CHECK(l->inFlight() == 7);
    KV_CHECK(l->tryAdmit(ip("192.0.2.77")).refusal == Refusal::None);
}

KV_TEST(paired_sessions_never_count_once_their_handshake_ends) {
    auto l = limiter(LimiterConfig{.burst = 100, .newPerMinute = 1000});
    // Ten paired devices behind one carrier address, all connected: none holds a slot.
    for (int i = 0; i < 10; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.50", HandshakeResult::Success) == Refusal::None);
    KV_CHECK(l->inFlight() == 0);
    KV_CHECK(l->inFlightFor("203.0.113.50") == 0);
}

// A /48 holds 65,536 /64 sources, so the per-source 2 alone would let one site take every slot.
// At most 4 unreserved handshakes in flight per IPv6 /48; another /48 and IPv4 are unaffected; it is not the global
// cap, so nothing is evicted for it.
KV_TEST(an_ipv6_48_holds_at_most_four_in_flight) {
    auto l = limiter(LimiterConfig{.burst = 100, .inFlightTotal = 8});
    std::vector<AdmitResult> held;
    for (int i = 0; i < 4; ++i) {
        const std::string a = "2001:db8:aa:" + std::to_string(i + 1) + "::1"; // four /64s in one /48
        held.push_back(l->tryAdmit(ip(a.c_str())));
        KV_CHECK_MSG(held.back().refusal == Refusal::None, a);
    }
    auto fifth = l->tryAdmit(ip("2001:db8:aa:99::1"));
    KV_CHECK(fifth.refusal == Refusal::TooManyInFlight && !fifth.globalFull);
    KV_CHECK(l->tryAdmit(ip("2001:db8:bb:1::1")).refusal == Refusal::None); // another /48
    KV_CHECK(l->tryAdmit(ip("198.51.100.7")).refusal == Refusal::None);     // IPv4
    // One ends: room in that /48 again.
    held.front().admission.finish(HandshakeResult::Timeout);
    KV_CHECK(l->tryAdmit(ip("2001:db8:aa:99::1")).refusal == Refusal::None);
}

// Asking about a /48, or refusing it for another reason, leaves no entry behind; an entry
// lives only while that /48 has handshakes in flight.
KV_TEST(a_48_is_tracked_only_while_it_has_handshakes_in_flight) {
    auto l = limiter(LimiterConfig{.burst = 100, .inFlightTotal = 1});
    auto held = l->tryAdmit(ip("2001:db8:aa:1::1"));
    KV_REQUIRE(held.refusal == Refusal::None);
    KV_CHECK(l->prefixesTracked() == 1);
    for (int i = 0; i < 20; ++i) {
        const std::string a = "2001:db8:" + std::to_string(100 + i) + ":1::1"; // twenty other /48s, all refused (global)
        KV_CHECK(l->tryAdmit(ip(a.c_str())).refusal == Refusal::TooManyInFlight);
    }
    KV_CHECK_MSG(l->prefixesTracked() == 1, std::to_string(l->prefixesTracked()));
    held.admission.finish(HandshakeResult::Timeout);
    KV_CHECK(l->prefixesTracked() == 0);
}

// A refusal only because the global in-flight cap is full says so (the listener then evicts a stalled stranger);
// a source's own cap never does.
KV_TEST(a_full_global_cap_is_told_apart_from_a_sources_own) {
    auto l = limiter(LimiterConfig{.burst = 100, .inFlightTotal = 2});
    auto a = l->tryAdmit(ip("198.51.100.1"));
    auto b = l->tryAdmit(ip("198.51.100.1"));
    auto c = l->tryAdmit(ip("198.51.100.1"));
    KV_CHECK(c.refusal == Refusal::TooManyInFlight && !c.globalFull); // the source's 2
    auto d = l->tryAdmit(ip("198.51.100.2"));
    KV_CHECK(d.refusal == Refusal::TooManyInFlight && d.globalFull); // the global 2
    // Evicting one frees its slot at once, counting nothing; its own end later changes nothing more.
    KV_CHECK(a.admission.evict());
    KV_CHECK(!a.admission.evict());
    a.admission.finish(HandshakeResult::Failure);
    KV_CHECK(l->failuresFor("198.51.100.1") == 0);
    auto e = l->tryAdmit(ip("198.51.100.2"));
    KV_CHECK(e.refusal == Refusal::None);
}

// No per-minute cap by default. Many sources, one connection each, are never refused as too many new.
KV_TEST(no_cap_on_new_connections_a_minute_by_default) {
    auto l = limiter();
    for (int i = 0; i < 200; ++i) {
        const std::string a = "198.51." + std::to_string(100 + i / 250) + "." + std::to_string(i % 250 + 1);
        KV_CHECK_MSG(connectAndEnd(*l, a.c_str(), HandshakeResult::Timeout) == Refusal::None, a);
    }
}

// A per-minute cap can still be set (tests, a future need): it holds as before.
KV_TEST(a_per_minute_cap_when_one_is_set) {
    auto l = limiter(LimiterConfig{.newPerMinute = 30});
    for (int i = 0; i < 30; ++i) {
        const std::string a = "198.51.100." + std::to_string(i + 1);
        KV_CHECK(connectAndEnd(*l, a.c_str(), HandshakeResult::Timeout) == Refusal::None);
    }
    KV_CHECK(connectAndEnd(*l, "192.0.2.1", HandshakeResult::Timeout) == Refusal::TooManyNew);
    g_now += 60'000;
    KV_CHECK(connectAndEnd(*l, "192.0.2.1", HandshakeResult::Timeout) == Refusal::None);
}

KV_TEST(five_failures_in_an_hour_block_and_a_blocked_source_stays_blocked) {
    auto l = limiter(LimiterConfig{.burst = 100});
    for (int i = 0; i < 4; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.7", HandshakeResult::Failure) == Refusal::None);
    KV_CHECK(!l->isBlocked("203.0.113.7"));
    KV_CHECK(connectAndEnd(*l, "203.0.113.7", HandshakeResult::Failure) == Refusal::None);
    KV_CHECK(l->isBlocked("203.0.113.7"));
    // A blocked source is let in, marked so (only a paired device's certificate
    // gets past the TLS verify step for it), and its end changes nothing: not a paired success, not a failure.
    {
        auto a = l->tryAdmit(ip("203.0.113.7"));
        KV_REQUIRE(a.refusal == Refusal::None);
        KV_CHECK(a.admission.blocked() && !a.admission.reserved());
        a.admission.finish(HandshakeResult::Success);
    }
    KV_CHECK(l->isBlocked("203.0.113.7"));
    KV_CHECK(connectAndEnd(*l, "203.0.113.7", HandshakeResult::Failure) == Refusal::None);
    KV_CHECK(std::string(refusalLogEvent(Refusal::Blocked)) == "refused_blocked");
    // The block ends on its own clock, an hour after the fifth failure, whatever happened during it.
    g_now += 3'599'999;
    KV_CHECK(l->isBlocked("203.0.113.7"));
    g_now += 1;
    KV_CHECK(!l->isBlocked("203.0.113.7"));
    {
        auto a = l->tryAdmit(ip("203.0.113.7"));
        KV_CHECK(a.refusal == Refusal::None && !a.admission.blocked());
    }
}

// Attempts during a block, admitted or refused, never extend it or count.
KV_TEST(nothing_during_a_block_extends_it_or_counts) {
    auto l = limiter(LimiterConfig{.burst = 100});
    for (int i = 0; i < 5; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.17", HandshakeResult::Failure) == Refusal::None);
    KV_REQUIRE(l->isBlocked("203.0.113.17"));
    const uint64_t blocked_at = g_now;
    // Fifty minutes of an app knocking once a minute, each attempt let in and failing or timing out.
    for (int i = 0; i < 50; ++i) {
        g_now += 60'000;
        connectAndEnd(*l, "203.0.113.17", i % 2 ? HandshakeResult::Failure : HandshakeResult::Timeout);
    }
    KV_CHECK(l->isBlocked("203.0.113.17"));
    KV_CHECK(l->failuresFor("203.0.113.17") == 0);
    // And a burst refused by its bucket meanwhile.
    for (int i = 0; i < 20; ++i) l->tryAdmit(ip("203.0.113.17"));
    g_now = blocked_at + 3'599'999;
    KV_CHECK(l->isBlocked("203.0.113.17"));
    g_now = blocked_at + 3'600'000;
    KV_CHECK(!l->isBlocked("203.0.113.17"));
}

// Blocked sources together take at most blockedInFlightTotal of the global 8, and never a reserved slot.
KV_TEST(blocked_sources_take_at_most_their_share_and_no_reserve) {
    auto l = limiter(LimiterConfig{.burst = 100, .blockedInFlightTotal = 2});
    const char* srcs[] = {"203.0.113.21", "203.0.113.22", "203.0.113.23"};
    for (const char* s : srcs) {
        for (int i = 0; i < 5; ++i) KV_CHECK(connectAndEnd(*l, s, HandshakeResult::Failure) == Refusal::None);
        KV_REQUIRE(l->isBlocked(s));
    }
    auto a = l->tryAdmit(ip(srcs[0]));
    auto b = l->tryAdmit(ip(srcs[1]));
    KV_CHECK(a.refusal == Refusal::None && b.refusal == Refusal::None && l->blockedInFlight() == 2);
    KV_CHECK(l->tryAdmit(ip(srcs[2])).refusal == Refusal::Blocked);
    // An unblocked source still gets in beside them.
    auto c = l->tryAdmit(ip("203.0.113.30"));
    KV_CHECK(c.refusal == Refusal::None && !c.admission.blocked());
    a.admission.finish(HandshakeResult::Timeout);
    KV_CHECK(l->blockedInFlight() == 1);
    KV_CHECK(l->tryAdmit(ip(srcs[2])).refusal == Refusal::None);
    // A source paired in the last day, blocked since, gets no reserve while blocked.
    auto p = limiter(LimiterConfig{.burst = 100});
    p->noteAuthenticated(ip("203.0.113.40"));
    for (int i = 0; i < 5; ++i) KV_CHECK(connectAndEnd(*p, "203.0.113.40", HandshakeResult::Failure) == Refusal::None);
    auto q = p->tryAdmit(ip("203.0.113.40"));
    KV_CHECK(q.refusal == Refusal::None && q.admission.blocked() && !q.admission.reserved() && p->inFlight() == 1);
}

KV_TEST(timeouts_are_not_failures) {
    auto l = limiter(LimiterConfig{.burst = 100});
    for (int i = 0; i < 20; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.8", HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(!l->isBlocked("203.0.113.8"));
    // Nor is a ticket dropped without a result.
    for (int i = 0; i < 10; ++i) {
        auto a = l->tryAdmit(ip("203.0.113.8"));
        KV_CHECK(a.refusal == Refusal::None);
    }
    KV_CHECK(!l->isBlocked("203.0.113.8"));
}

KV_TEST(failures_older_than_an_hour_drop_out) {
    auto l = limiter(LimiterConfig{.burst = 100});
    for (int i = 0; i < 4; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.9", HandshakeResult::Failure) == Refusal::None);
    g_now += 3'600'000;
    KV_CHECK(connectAndEnd(*l, "203.0.113.9", HandshakeResult::Failure) == Refusal::None);
    KV_CHECK(!l->isBlocked("203.0.113.9"));
}

// A paired device's success refills the bucket and clears no failure, so a phone
// on a shared carrier address can't keep strangers' guesses from it fresh. Failures age out by time only.
KV_TEST(a_paired_handshake_refills_its_source_and_clears_no_failure) {
    auto l = limiter();
    for (int i = 0; i < 4; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.10", HandshakeResult::Failure) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "203.0.113.10", HandshakeResult::Timeout) == Refusal::RateLimit);
    g_now += 10'000;
    KV_CHECK(connectAndEnd(*l, "203.0.113.10", HandshakeResult::Success) == Refusal::None);
    KV_CHECK(l->failuresFor("203.0.113.10") == 4);
    // Refilled to the burst: four connections go in at once. The first failure is the fifth, and blocks.
    KV_CHECK(connectAndEnd(*l, "203.0.113.10", HandshakeResult::Failure) == Refusal::None);
    KV_CHECK(l->isBlocked("203.0.113.10"));
}

KV_TEST(a_refused_attempt_takes_nothing) {
    auto l = limiter();
    for (int i = 0; i < 4; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.11", HandshakeResult::Timeout) == Refusal::None);
    for (int i = 0; i < 50; ++i) KV_CHECK(connectAndEnd(*l, "203.0.113.11", HandshakeResult::Timeout) == Refusal::RateLimit);
    g_now += 10'000;
    KV_CHECK(connectAndEnd(*l, "203.0.113.11", HandshakeResult::Timeout) == Refusal::None);
    // A refused new source leaves no entry behind.
    auto busy = limiter(LimiterConfig{.inFlightTotal = 0});
    for (int i = 0; i < 20; ++i) {
        const std::string a = "198.51.100." + std::to_string(i + 1);
        KV_CHECK(busy->tryAdmit(ip(a.c_str())).refusal == Refusal::TooManyInFlight);
    }
    KV_CHECK(busy->sourcesKnown() == 0);
}

KV_TEST(idle_sources_are_forgotten) {
    auto l = limiter(LimiterConfig{.sweepAbove = 4});
    for (int i = 0; i < 10; ++i) {
        const std::string a = "198.51.100." + std::to_string(i + 1);
        KV_CHECK(connectAndEnd(*l, a.c_str(), HandshakeResult::Timeout) == Refusal::None);
    }
    KV_CHECK(connectAndEnd(*l, "203.0.113.12", HandshakeResult::Failure) == Refusal::None);
    g_now += 60'000; // every bucket full again
    KV_CHECK(connectAndEnd(*l, "192.0.2.200", HandshakeResult::Timeout) == Refusal::None);
    // The source with a failure is kept; the idle ones are gone.
    KV_CHECK(l->sourcesKnown() == 2);
}

KV_TEST(an_admission_ends_once) {
    auto l = limiter();
    auto a = l->tryAdmit(ip("203.0.113.13"));
    KV_CHECK(a.refusal == Refusal::None && static_cast<bool>(a.admission));
    a.admission.finish(HandshakeResult::Failure);
    a.admission.finish(HandshakeResult::Failure);
    KV_CHECK(!static_cast<bool>(a.admission));
    KV_CHECK(l->inFlight() == 0);
    // Moving a ticket moves the slot with it.
    auto b = l->tryAdmit(ip("203.0.113.13"));
    Admission moved = std::move(b.admission);
    KV_CHECK(l->inFlight() == 1);
    moved = Admission{};
    KV_CHECK(l->inFlight() == 0);
}

KV_TEST(sweeping_runs_at_most_once_a_second) {
    // Under a flood of new sources, forgetting idle ones runs once a second, not per attempt.
    auto l = limiter(LimiterConfig{.sweepAbove = 4});
    for (int i = 0; i < 10; ++i) {
        const std::string a = "198.51.100." + std::to_string(i + 1);
        connectAndEnd(*l, a.c_str(), HandshakeResult::Timeout);
    }
    const size_t before = l->sweeps();
    for (int i = 0; i < 300; ++i) {
        const std::string a = "192.0.2." + std::to_string(i % 250 + 1);
        connectAndEnd(*l, a.c_str(), HandshakeResult::Timeout);
    }
    KV_CHECK(l->sweeps() - before <= 1);
    g_now += 1000;
    connectAndEnd(*l, "203.0.113.99", HandshakeResult::Timeout);
    KV_CHECK(l->sweeps() - before <= 2);
}

KV_TEST(a_source_paired_in_the_last_day_has_a_reserve_outside_the_global_caps) {
    // The owner's phone behind a busy address isn't locked out by strangers.
    auto l = limiter(LimiterConfig{.burst = 100, .newPerMinute = 1000});
    l->noteAuthenticated(ip("203.0.113.60"));
    std::vector<Admission> strangers;
    for (int i = 0; i < 8; ++i) {
        const std::string a = "198.51.100." + std::to_string(i + 1);
        auto r = l->tryAdmit(ip(a.c_str()));
        KV_CHECK(r.refusal == Refusal::None);
        strangers.push_back(std::move(r.admission));
    }
    KV_CHECK(l->tryAdmit(ip("192.0.2.9")).refusal == Refusal::TooManyInFlight);
    auto a = l->tryAdmit(ip("203.0.113.60"));
    auto b = l->tryAdmit(ip("203.0.113.60"));
    KV_CHECK(a.refusal == Refusal::None && b.refusal == Refusal::None);
    // Its own per-source cap still holds, and the strangers' slots are as they were.
    KV_CHECK(l->tryAdmit(ip("203.0.113.60")).refusal == Refusal::TooManyInFlight);
    KV_CHECK(l->inFlight() == 8);
    KV_CHECK(l->tryAdmit(ip("192.0.2.9")).refusal == Refusal::TooManyInFlight);
    a.admission.finish(HandshakeResult::Success);
    b.admission.finish(HandshakeResult::Timeout);
    KV_CHECK(l->inFlight() == 8);
    // A day later, with no paired handshake since, the reserve is gone.
    g_now += 24ull * 3600 * 1000 + 1;
    KV_CHECK(l->tryAdmit(ip("203.0.113.60")).refusal == Refusal::TooManyInFlight);
}

KV_TEST(the_reserve_passes_the_per_minute_cap_but_not_its_own_bucket) {
    auto l = limiter(LimiterConfig{.newPerMinute = 2});
    KV_CHECK(connectAndEnd(*l, "198.51.100.1", HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "198.51.100.2", HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "198.51.100.3", HandshakeResult::Timeout) == Refusal::TooManyNew);
    // A paired handshake notes the source too.
    l->noteAuthenticated(ip("2001:db8:5:5::1"));
    for (int i = 0; i < 4; ++i) KV_CHECK(connectAndEnd(*l, "2001:db8:5:5::9", HandshakeResult::Timeout) == Refusal::None);
    KV_CHECK(connectAndEnd(*l, "2001:db8:5:5::9", HandshakeResult::Timeout) == Refusal::RateLimit);
    // A stranger is still held to the cap.
    KV_CHECK(connectAndEnd(*l, "198.51.100.4", HandshakeResult::Timeout) == Refusal::TooManyNew);
}
