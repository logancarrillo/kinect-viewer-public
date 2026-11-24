// W8 (outside the home), Phase A: the outside listener's admission limits (protocol v3 §8.5). It decides at
// accept time, before any thread or TLS work: a refused socket is closed with nothing read or written.
// Every per-source count uses the §8.0 source key (IPv4 address, IPv6 /64). Pure logic with an injected
// clock, safe to call from any thread. Paired sessions never count once their handshake has ended.
#pragma once

#include "outside_address.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kvhost::outside {

struct LimiterConfig {
    int burst = 4;                         // per source: new connections in a burst
    uint64_t refillMs = 10'000;            // ... and one more every 10 s
    int inFlightTotal = 8;                 // unauthenticated (handshake or redemption) at once, all sources
    int inFlightPerSource = 2;             // ... and per source, inside the 8
    // ... and per IPv6 /48 in aggregate, inside the 8: a /48 holds 65,536 /64 sources, so the
    // per-source 2 alone would let one site take every slot. IPv4 has no such aggregate. A reserved slot never counts.
    int inFlightPerPrefix48 = 4;
    // New unauthenticated connections a minute, all sources; 0: no such cap (a cap of 30 let about five
    // addresses spend the whole budget and lock out every new invitee and a phone on a fresh carrier address; the per-
    // source buckets and the in-flight caps hold the load, and a full in-flight cap evicts a stalled stranger).
    int newPerMinute = 0;
    int failuresToBlock = 5;               // failures per source within failureWindowMs ...
    uint64_t failureWindowMs = 3'600'000;  // ... block it
    uint64_t blockMs = 3'600'000;          // ... for an hour
    // A blocked source is still let in to show a paired device's certificate, and nothing else: its
    // handshakes, all blocked sources together, take at most this many of the
    // global 8, never a reserved slot, and each still keeps to its source's bucket and its 2.
    int blockedInFlightTotal = 4;
    size_t sweepAbove = 512;               // forget idle sources once more than this many are known (at most 1/s)
    uint64_t reserveForMs = 86'400'000;    // a source paired within this keeps its own slots outside the global caps
};

enum class Refusal { None, RateLimit, Blocked, TooManyInFlight, TooManyNew };
// The connection log's event for a refusal (v3 §8.6): refused_rate_limit, refused_blocked, refused_too_many.
const char* refusalLogEvent(Refusal r);

enum class HandshakeResult {
    Success, // a paired device's handshake: refills the source's bucket; its failures and any block stay as they were
    // v3 §8.5: a certificate that arrived and matched no listed device while no invite was open, or a failed or
    // malformed redemption after a completed handshake (wrong proof, certificate mismatch, wrong method, too big,
    // bad JSON, too slow, the invite closed meanwhile). Nothing else.
    Failure,
    // Not a failure: a peer closing, resetting or stalling, or bytes that aren't TLS, before its certificate was
    // checked. A weak mobile link must not block the owner's own phone.
    Timeout,
};

class OutsideLimiter;

// Holds one unauthenticated slot (in total and for its source) from admission until the handshake ends or
// the ticket is dropped, whichever comes first. Move-only.
class Admission {
public:
    Admission() = default;
    Admission(Admission&& o) noexcept;
    Admission& operator=(Admission&& o) noexcept;
    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;
    ~Admission();

    explicit operator bool() const { return limiter_ != nullptr; }
    const std::string& source() const { return source_; }
    // It holds one of a recently paired source's own slots (§8.5's reserve), not one of the global 8.
    bool reserved() const { return reserved_; }
    // Its source is blocked: the handshake may end only as a paired device's (its certificate one the device list
    // holds, checked in the TLS verify step). Anything else is refused there and counts nothing; the block, its
    // clock and the source's failures stay exactly as they were, whatever the end.
    bool blocked() const { return blocked_; }
    // The handshake (or redemption) ended. Frees the slot; a Failure counts against the source. Once only, and safe to
    // call from two threads (the handshake's, and the listener's evicting it).
    void finish(HandshakeResult r);
    // Evicted for a newer connection: frees the slot now and counts nothing. False if it had already ended.
    bool evict();
    // It has ended (finished, evicted or released): its slot is free.
    bool ended() const;

private:
    friend class OutsideLimiter;
    Admission(std::shared_ptr<OutsideLimiter> l, std::string source, bool reserved, bool blocked)
        : limiter_(std::move(l)), source_(std::move(source)), reserved_(reserved), blocked_(blocked) {}
    void release();
    std::shared_ptr<OutsideLimiter> limiter_;
    std::string source_;
    bool reserved_ = false; // holds one of a recently paired source's own slots, not one of the global 8
    bool blocked_ = false;  // its source was blocked when it was let in
};

struct AdmitResult {
    Refusal refusal = Refusal::None;
    // Refused only because the global in-flight cap is full (not the source's own caps): the caller may evict a
    // handshake that has shown no certificate and try once more.
    bool globalFull = false;
    Admission admission; // set when refusal == None
};

class OutsideLimiter : public std::enable_shared_from_this<OutsideLimiter> {
public:
    using Clock = uint64_t (*)(); // monotonic ms
    static std::shared_ptr<OutsideLimiter> create(LimiterConfig cfg = {}, Clock clock = nullptr);

    // At accept, before any thread or TLS work.
    AdmitResult tryAdmit(const IpAddress& peer);
    // A paired device's handshake succeeded from peer (Phase B calls it beside finish(Success)). For the next
    // 24 h that source has its own slots outside the global 8 and the per-minute cap, still
    // held to its own bucket and per-source cap.
    void noteAuthenticated(const IpAddress& peer);

    // For tests and the PC's display.
    int inFlight() const; // the global 8's count; a recently paired source's own slots aren't in it
    int blockedInFlight() const; // of those, blocked sources' handshakes
    size_t sweeps() const;
    int inFlightFor(const std::string& source) const;
    bool isBlocked(const std::string& source) const;
    int failuresFor(const std::string& source) const; // counted within the window, not yet cleared
    size_t sourcesKnown() const;

    OutsideLimiter(LimiterConfig cfg, Clock clock);

private:
    friend class Admission;
    struct Source {
        uint64_t credit = 0; // ms of refill earned: one connection costs refillMs, the bucket holds burst of them
        uint64_t refilledAt = 0;
        int inFlight = 0;
        std::deque<uint64_t> failures;
        uint64_t blockedUntil = 0;
        bool paired = false;
        uint64_t pairedAt = 0;
    };
    uint64_t now() const;
    void refill(Source& s, uint64_t t) const;
    uint64_t full() const { return static_cast<uint64_t>(cfg_.burst) * cfg_.refillMs; }
    void ended(const std::string& source, HandshakeResult r, bool countResult, bool reserved, bool blocked);
    void sweep(uint64_t t);

    LimiterConfig cfg_;
    Clock clock_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Source> sources_;
    std::deque<uint64_t> recentNew_;
    int inFlight_ = 0;
    int reservedInFlight_ = 0;
    int blockedInFlight_ = 0;
    std::unordered_map<std::string, int> inFlightByPrefix_; // unreserved, by IPv6 /48; an entry only while above 0
    int prefixInFlight(const std::string& prefix) const;
public:
    // For tests: how many /48s have an entry (one only while it has handshakes in flight).
    size_t prefixesTracked() const {
        std::lock_guard<std::mutex> lock(mu_);
        return inFlightByPrefix_.size();
    }
private:
    size_t sweeps_ = 0;
    uint64_t lastSweep_ = 0;
};

} // namespace kvhost::outside
