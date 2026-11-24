#include "outside_limiter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <iterator>

namespace kvhost::outside {

namespace {
uint64_t steadyMs() { return GetTickCount64(); }
} // namespace

const char* refusalLogEvent(Refusal r) {
    switch (r) {
        case Refusal::RateLimit: return "refused_rate_limit";
        case Refusal::Blocked: return "refused_blocked";
        case Refusal::TooManyInFlight:
        case Refusal::TooManyNew: return "refused_too_many";
        case Refusal::None: break;
    }
    return "";
}

// --- Admission ---

Admission::Admission(Admission&& o) noexcept
    : limiter_(std::move(o.limiter_)), source_(std::move(o.source_)), reserved_(o.reserved_), blocked_(o.blocked_) {
    o.limiter_.reset();
}

Admission& Admission::operator=(Admission&& o) noexcept {
    if (this != &o) {
        release();
        limiter_ = std::move(o.limiter_);
        source_ = std::move(o.source_);
        reserved_ = o.reserved_;
        blocked_ = o.blocked_;
        o.limiter_.reset();
    }
    return *this;
}

Admission::~Admission() { release(); }

namespace {
// Ends an admission once, whichever thread gets there first (the handshake's own, or the listener evicting it).
std::mutex g_admission_end_m;
} // namespace

void Admission::finish(HandshakeResult r) {
    std::shared_ptr<OutsideLimiter> l;
    {
        std::lock_guard<std::mutex> g(g_admission_end_m);
        l.swap(limiter_);
    }
    if (l) l->ended(source_, r, true, reserved_, blocked_);
}

bool Admission::ended() const {
    std::lock_guard<std::mutex> g(g_admission_end_m);
    return limiter_ == nullptr;
}

bool Admission::evict() {
    std::shared_ptr<OutsideLimiter> l;
    {
        std::lock_guard<std::mutex> g(g_admission_end_m);
        l.swap(limiter_);
    }
    if (!l) return false;
    l->ended(source_, HandshakeResult::Timeout, false, reserved_, blocked_);
    return true;
}

void Admission::release() {
    std::shared_ptr<OutsideLimiter> l;
    {
        std::lock_guard<std::mutex> g(g_admission_end_m);
        l.swap(limiter_);
    }
    // Dropped without a result (the socket closed, the host stopped): frees the slot, counts nothing.
    if (l) l->ended(source_, HandshakeResult::Timeout, false, reserved_, blocked_);
}

// --- OutsideLimiter ---

std::shared_ptr<OutsideLimiter> OutsideLimiter::create(LimiterConfig cfg, Clock clock) {
    return std::make_shared<OutsideLimiter>(cfg, clock);
}

OutsideLimiter::OutsideLimiter(LimiterConfig cfg, Clock clock) : cfg_(cfg), clock_(clock ? clock : &steadyMs) {}

uint64_t OutsideLimiter::now() const { return clock_(); }

void OutsideLimiter::refill(Source& s, uint64_t t) const {
    if (t <= s.refilledAt) return;
    s.credit = std::min(full(), s.credit + (t - s.refilledAt));
    s.refilledAt = t;
}

int OutsideLimiter::prefixInFlight(const std::string& prefix) const {
    // find(), never [], so a refused or a merely asked /48 leaves no entry behind
    const auto it = inFlightByPrefix_.find(prefix);
    return it == inFlightByPrefix_.end() ? 0 : it->second;
}

AdmitResult OutsideLimiter::tryAdmit(const IpAddress& peer) {
    const std::string key = sourceKey(peer);
    const std::string prefix = prefix48Key(peer); // "" for IPv4
    std::lock_guard<std::mutex> lock(mu_);
    const uint64_t t = now();
    // Forgetting idle sources walks them all: at most once a second, so a flood can't make every attempt pay it.
    if (sources_.size() > cfg_.sweepAbove && (sweeps_ == 0 || t - lastSweep_ >= 1'000)) sweep(t);

    AdmitResult r;
    auto it = sources_.find(key);
    Source fresh;
    fresh.credit = full();
    fresh.refilledAt = t;
    Source& s = it != sources_.end() ? it->second : fresh;
    refill(s, t);

    // A source with a paired handshake in the last day has its own slots outside the global caps, so
    // strangers can't lock the owner's phone out. Its own caps still hold.
    // A blocked source is let in only to show a paired device's certificate:
    // under its bucket and its 2 as any source, inside the global 8 and the per-minute cap, never a reserved slot,
    // and with every blocked source's handshakes together at most blockedInFlightTotal. Past those it is refused
    // as blocked. Before, it was refused before TLS for the hour, and a paired phone behind the same carrier
    // address (strangers' failures, or its own browser's) couldn't get in at all.
    const bool blocked = s.blockedUntil > t;
    const bool reserved = !blocked && s.paired && t - s.pairedAt < cfg_.reserveForMs;
    if (s.credit < cfg_.refillMs) r.refusal = Refusal::RateLimit;
    else if (s.inFlight >= cfg_.inFlightPerSource) r.refusal = blocked ? Refusal::Blocked : Refusal::TooManyInFlight;
    else if (blocked && blockedInFlight_ >= cfg_.blockedInFlightTotal) r.refusal = Refusal::Blocked;
    else if (!reserved && !prefix.empty() && prefixInFlight(prefix) >= cfg_.inFlightPerPrefix48) {
        r.refusal = Refusal::TooManyInFlight; // its /48's share (not the global cap: nothing is evicted for it)
    }
    else if (reserved) {
        // Outside the global 8 and the per-minute cap.
    } else if (inFlight_ >= cfg_.inFlightTotal) {
        r.refusal = Refusal::TooManyInFlight;
        r.globalFull = true;
    } else if (cfg_.newPerMinute > 0) {
        while (!recentNew_.empty() && t - recentNew_.front() >= 60'000) recentNew_.pop_front();
        if (static_cast<int>(recentNew_.size()) >= cfg_.newPerMinute) r.refusal = Refusal::TooManyNew;
    }
    if (r.refusal != Refusal::None) return r; // a refused attempt takes nothing and leaves no new entry

    if (it == sources_.end()) it = sources_.emplace(key, fresh).first;
    Source& kept = it->second;
    kept.credit -= cfg_.refillMs;
    ++kept.inFlight;
    if (reserved) ++reservedInFlight_;
    else {
        ++inFlight_;
        if (cfg_.newPerMinute > 0) recentNew_.push_back(t); // kept only while there is such a cap
    }
    if (blocked) ++blockedInFlight_;
    if (!reserved && !prefix.empty()) ++inFlightByPrefix_[prefix];
    r.admission = Admission(shared_from_this(), key, reserved, blocked);
    return r;
}

void OutsideLimiter::noteAuthenticated(const IpAddress& peer) {
    const std::string key = sourceKey(peer);
    std::lock_guard<std::mutex> lock(mu_);
    const uint64_t t = now();
    auto it = sources_.find(key);
    if (it == sources_.end()) {
        Source fresh;
        fresh.credit = full();
        fresh.refilledAt = t;
        it = sources_.emplace(key, fresh).first;
    }
    it->second.paired = true;
    it->second.pairedAt = t;
}

void OutsideLimiter::ended(const std::string& key, HandshakeResult result, bool countResult, bool reserved, bool blocked) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint64_t t = now();
    int& count = reserved ? reservedInFlight_ : inFlight_;
    if (count > 0) --count;
    if (!reserved) {
        // the /48 its source key (an IPv6 /64) lies in
        if (key.size() > 3 && key.compare(key.size() - 3, 3, "/64") == 0) {
            if (const auto ip = parseIp(key.substr(0, key.size() - 3))) {
                const auto pit = inFlightByPrefix_.find(prefix48Key(*ip));
                if (pit != inFlightByPrefix_.end() && --pit->second <= 0) inFlightByPrefix_.erase(pit);
            }
        }
    }
    if (blocked && blockedInFlight_ > 0) --blockedInFlight_;
    auto it = sources_.find(key);
    if (it == sources_.end()) return;
    Source& s = it->second;
    if (s.inFlight > 0) --s.inFlight;
    // A blocked source's end changes nothing: the block, its clock and the failures stay as they were.
    if (!countResult || blocked) return;
    // A paired device's success refills the bucket and clears no failure: a phone
    // must not keep strangers' guesses from the same carrier address fresh. Failures age out by time only.
    if (result == HandshakeResult::Success) {
        s.credit = full();
        s.refilledAt = t;
    } else if (result == HandshakeResult::Failure) {
        while (!s.failures.empty() && t - s.failures.front() >= cfg_.failureWindowMs) s.failures.pop_front();
        s.failures.push_back(t);
        if (static_cast<int>(s.failures.size()) >= cfg_.failuresToBlock) {
            s.blockedUntil = t + cfg_.blockMs;
            s.failures.clear();
        }
    }
}

void OutsideLimiter::sweep(uint64_t t) {
    ++sweeps_;
    lastSweep_ = t;
    for (auto it = sources_.begin(); it != sources_.end();) {
        Source& s = it->second;
        refill(s, t);
        while (!s.failures.empty() && t - s.failures.front() >= cfg_.failureWindowMs) s.failures.pop_front();
        const bool recent = s.paired && t - s.pairedAt < cfg_.reserveForMs;
        const bool idle = s.inFlight == 0 && s.failures.empty() && s.blockedUntil <= t && s.credit >= full() && !recent;
        it = idle ? sources_.erase(it) : std::next(it);
    }
}

size_t OutsideLimiter::sweeps() const {
    std::lock_guard<std::mutex> lock(mu_);
    return sweeps_;
}

int OutsideLimiter::inFlight() const {
    std::lock_guard<std::mutex> lock(mu_);
    return inFlight_;
}

int OutsideLimiter::blockedInFlight() const {
    std::lock_guard<std::mutex> lock(mu_);
    return blockedInFlight_;
}

int OutsideLimiter::inFlightFor(const std::string& source) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sources_.find(source);
    return it == sources_.end() ? 0 : it->second.inFlight;
}

bool OutsideLimiter::isBlocked(const std::string& source) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sources_.find(source);
    return it != sources_.end() && it->second.blockedUntil > now();
}

int OutsideLimiter::failuresFor(const std::string& source) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sources_.find(source);
    return it == sources_.end() ? 0 : static_cast<int>(it->second.failures.size());
}

size_t OutsideLimiter::sourcesKnown() const {
    std::lock_guard<std::mutex> lock(mu_);
    return sources_.size();
}

} // namespace kvhost::outside
