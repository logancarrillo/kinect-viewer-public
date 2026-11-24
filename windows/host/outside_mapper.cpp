#include "outside_mapper.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace kvhost::outside {

namespace {

enum class Cmd { Open, Close, Acknowledge, Recheck, RetryRemovals, RemoveNow, Remap, RemoveLeftover, VerifyGateway, Exit };

struct Job {
    Cmd cmd = Cmd::Recheck;
    uint16_t port = 0;
    uint64_t id = 0; // Open: the open's id
    bool map = true; // Open: false while the PC user has the forward removed
    bool pinAnew = false; // Open: the PC user turned outside viewing on
    std::string wan;      // Acknowledge: the address the PC user was shown
};

// The private log: a local debugger only; nothing is written to disk or sent anywhere (as the encoder's).
void privateLog(const std::string& line) {
    const std::string s = "[kv-outside] " + line + "\n";
    OutputDebugStringA(s.c_str());
}

const char* kNoLanAddress = "This PC has no home-network address the router can forward to.";

// The manual steps, in PortMapper's words, for a host that has no router to use at all.
std::string manualSentence(const std::string& why, uint16_t port, const std::string& lan) {
    return why + " Forward TCP port " + std::to_string(port) + " to " + lan +
           " in your router's settings, then type your public address here if you know it.";
}

bool holdsForward(OutsideStatus s) { return s == OutsideStatus::Mapped || s == OutsideStatus::AddressChanged; }

} // namespace

struct OutsideMapper::Shared {
    Config cfg;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> queue;
    MapperState state;
    uint64_t openSeq = 0;
    bool stopped = false; // shutdown() was called: Exit is queued, nothing more is taken
    // Held while onChange runs, so shutdown() can clear it and know it is never called again.
    std::mutex cb_mu;
    std::function<void(const MapperState&)> onChange;

    void post(Cmd c, uint16_t port = 0, uint64_t id = 0, bool map = true, bool pinAnew = false, std::string wan = {}) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stopped) return;
            queue.push_back(Job{c, port, id, map, pinAnew, std::move(wan)});
        }
        cv.notify_all();
    }

    // Applies f to the state under mu, then tells the host (outside mu).
    template <class F> void update(F&& f) {
        MapperState snap;
        {
            std::lock_guard<std::mutex> lk(mu);
            f(state);
            ++state.generation;
            snap = state;
        }
        std::lock_guard<std::mutex> cb(cb_mu);
        if (onChange) onChange(snap);
    }

    static void run(const std::shared_ptr<Shared>& self);
};

void OutsideMapper::Shared::run(const std::shared_ptr<Shared>& self) {
    Shared* const me = self.get();
    Config& cfg = me->cfg;
    std::mutex& mu = me->mu;
    ComScope com; // this worker's own apartment (PortMapper's per-call workers have theirs too)
    PortMapper::Timing timing = cfg.timing;
    if (cfg.lanAddress && !timing.lanAddress) timing.lanAddress = cfg.lanAddress; // one source for the LAN address
    PortMapper mapper(cfg.opener ? cfg.opener : RouterOpener([] { return std::unique_ptr<RouterSession>(); }), timing,
                      cfg.addressPolicy);
    // §8.2: a removal that couldn't run while a router call was stuck runs here once that call returns.
    std::weak_ptr<Shared> weak = self;
    mapper.setOnUnstuck([weak] {
        if (auto sh = weak.lock()) sh->post(Cmd::RetryRemovals);
    });
    const bool haveRouter = static_cast<bool>(cfg.opener);
    bool on = false;
    bool removed = false; // the PC user removed the forward: no check maps it
    uint16_t port = 0;
    uint64_t openId = 0;
    auto nextCheck = std::chrono::steady_clock::now();
    // v3 §8.3: the pinned gateway; held: positive evidence of another router, kept until the PC user acknowledges
    // (or turns outside viewing off and on); verifyAt: when a pass that couldn't read a part is tried again.
    GatewayIdentity pin = cfg.pinned.complete() ? cfg.pinned : GatewayIdentity{};
    std::string lastGood = pin.complete() ? cfg.gatewayLocation : std::string(); // the description that answered last
    bool held = false, contradicted = false;
    bool verifying = false;      // a change is being checked, NATUPnP failures included
    uint32_t failedPasses = 0;   // passes in a row that couldn't read a part
    std::string gatewayAnswer;   // the gateway's own public answer from the last pass that read one
    std::string confirmedWan;    // the held address a pass confirmed (the only one, with a pin, that may be re-shared)
    std::optional<std::chrono::steady_clock::time_point> heldSince;
    std::optional<std::chrono::steady_clock::time_point> checkingSince; // this change's first retry
    std::optional<std::chrono::steady_clock::time_point> verifyAt;
    size_t verifyStep = 0;
    // The address devices were last told: a restart's first read that differs is a change, and is checked.
    if (!cfg.announcedWan.empty()) mapper.seedAnnouncedAddress(cfg.announcedWan);
    // Every report also says which port this PC's forward may still be on (heldPort, §8.7).
    auto update = [&](auto&& f) {
        me->update([&](MapperState& s) {
            f(s);
            s.heldPort = mapper.mayHoldPort();
        });
    };

    // §8.2: map port to this PC's LAN address, then remove whatever a non-mapped result may have left.
    auto openNow = [&](uint16_t p) {
        const std::optional<std::string> lan = cfg.lanAddress ? cfg.lanAddress() : internalClientIPv4();
        MappingReport r;
        std::string client;
        if (!lan || !usableInternalClient(*lan)) {
            r.status = OutsideStatus::Error;
            r.message = kNoLanAddress;
        } else if (!haveRouter) {
            client = *lan;
            r.status = OutsideStatus::Manual;
            r.message = manualSentence("Automatic port forwarding (UPnP) isn't used in a test run.", p, client);
        } else {
            client = *lan;
            r = mapper.open(p, client);
            // No forward is left on the router for any status but mapped. PortMapper
            // already removes one whose WAN check failed; this also covers a forward added but not read back.
            // Only this PC's own forward is ever removed (a port taken by someone else is left alone).
            if (!holdsForward(r.status)) mapper.remove();
        }
        update([&](MapperState& s) {
            s.status = r.status;
            s.port = p;
            s.lanIPv4 = client;
            s.wanAddress = r.wanAddress;
            s.message = r.message;
            s.checking = false;
            s.reported = true;
            s.openId = openId;
            s.removed = false;
        });
    };

    // The forward is removed (pet-camera.md §4) and stays so: manual, with the steps.
    auto reportRemoved = [&]() {
        const std::optional<std::string> lan = cfg.lanAddress ? cfg.lanAddress() : internalClientIPv4();
        const std::string client = lan && usableInternalClient(*lan) ? *lan : std::string();
        update([&](MapperState& s) {
            s.status = OutsideStatus::Manual;
            s.port = port;
            s.wanAddress.clear();
            s.lanIPv4 = client;
            s.message = manualSentence("The port forward was removed from the router.", port,
                                       client.empty() ? std::string("this PC") : client);
            s.checking = false;
            s.reported = true;
            s.openId = openId;
            s.removed = true;
        });
    };

    auto statusNow = [&]() {
        std::lock_guard<std::mutex> lk(mu);
        return me->state.status;
    };
    // This PC's LAN address now (the forward's internal client), "" if there is none.
    auto lanNow = [&]() -> std::string {
        const std::optional<std::string> lan = cfg.lanAddress ? cfg.lanAddress() : internalClientIPv4();
        return lan && usableInternalClient(*lan) ? *lan : std::string();
    };
    auto readGateway = [&](const std::string& lan) {
        return cfg.readGateway && !lan.empty() ? cfg.readGateway(lan) : GatewayIdentity{};
    };
    // The first mapped result with nothing pinned (the PC user just turned outside viewing on, or no gateway could
    // be read before) pins the gateway of the forward's interface. Never while a change is waiting: that moment
    // isn't trusted.
    auto pinIfNone = [&]() {
        if (pin.complete() || statusNow() != OutsideStatus::Mapped) return;
        const GatewayIdentity read = readGateway(lanNow());
        if (!read.complete()) return;
        pin = read;
        update([&](MapperState& s) { s.gatewayPin = pin; });
    };
    // Shows a report of PortMapper's as the state.
    auto applyReport = [&](const MappingReport& r) {
        const std::string lan = lanNow();
        update([&](MapperState& s) {
            s.status = r.status;
            s.wanAddress = r.wanAddress;
            s.message = r.message;
            s.lanIPv4 = lan;
            s.checking = false;
            s.reported = true;
        });
    };
    auto recheck = [&]() {
        MapperState cur;
        {
            std::lock_guard<std::mutex> lk(mu);
            cur = me->state;
        }
        if (!haveRouter || !holdsForward(cur.status)) {
            // Nothing is held: a fresh try, as when the listener opened (UPnP may have been turned on since).
            update([&](MapperState& s) { s.checking = true; });
            openNow(port);
            return;
        }
        update([&](MapperState& s) { s.checking = true; });
        MappingReport r = mapper.refresh();
        if (!holdsForward(r.status)) mapper.remove();
        // refresh() reads this PC's LAN address again and maps a new one in place of the old: show the same.
        applyReport(r);
    };

    // v3 §8.3: one verification pass for a new public address, every part read in it: NATUPnP's address now (a
    // fresh check, so a retry never announces one read earlier), the gateway of the forward's interface, and that
    // gateway's own public answers (only the pinned gateway is asked: a device at any other address has no say).
    //   - Confirmed (one of the gateway's answers is NATUPnP's address): told to devices at once, unless a hold is in
    //     force, in which case the PC user may now re-share it.
    //   - Contradicted (the gateway answers publicly, but not with NATUPnP's address): another device answered as the
    //     router. Held, never acknowledged while it lasts, and checked again until the gateway agrees.
    //   - Another gateway (its address or hardware address differs): held until the PC user re-shares.
    //   - A part that can't be read, NATUPnP included: no evidence. Tried again (gatewayRetryMs) for as long as it
    //     takes; devices keep the address they were told meanwhile.
    //   - Nothing pinned: waits for the PC user.
    auto report = [&](bool checking) {
        update([&](MapperState& s) {
            s.gatewayChecking = checking;
            s.gatewayFailedPasses = failedPasses;
            s.gatewayHeld = held;
            s.gatewayContradicts = contradicted;
            s.gatewayAnswer = gatewayAnswer;
            s.confirmedWan = confirmedWan;
        });
    };
    auto stop = [&] { // nothing more to check for this change
        verifying = false;
        verifyAt.reset();
        verifyStep = 0;
        checkingSince.reset();
        report(false);
    };
    auto again = [&](bool checking) { // read again after the next delay
        uint64_t d =
            cfg.gatewayRetryMs.empty() ? 60'000 : cfg.gatewayRetryMs[std::min(verifyStep, cfg.gatewayRetryMs.size() - 1)];
        // A long check is made less often: a hold, counted from when it began, and a gateway that still can't be read,
        // counted from this change's first retry.
        const auto now = std::chrono::steady_clock::now();
        if (!checkingSince) checkingSince = now;
        if (held && !heldSince) heldSince = now;
        if (now - (held ? *heldSince : *checkingSince) >= std::chrono::milliseconds(cfg.gatewaySlowAfterMs)) {
            d = std::max(d, cfg.gatewaySlowEveryMs);
        }
        ++verifyStep;
        verifyAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(d);
        report(checking);
    };
    auto resolved = [&] { // the change is gone: acknowledged, or the address is the told one again
        held = false;
        heldSince.reset();
        contradicted = false;
        failedPasses = 0;
        gatewayAnswer.clear();
        confirmedWan.clear();
        stop();
    };
    auto verifyChange = [&]() {
        if (!verifying && statusNow() != OutsideStatus::AddressChanged) {
            stop();
            return;
        }
        verifying = true;
        recheck(); // NATUPnP read now: a re-open after a failure, a refresh otherwise
        MapperState now;
        {
            std::lock_guard<std::mutex> lk(mu);
            now = me->state;
        }
        if (now.status == OutsideStatus::Mapped) {
            resolved();
            return;
        }
        if (now.status != OutsideStatus::AddressChanged) {
            // NATUPnP failed this time (no router answer, no public address): read it all again later.
            ++failedPasses;
            again(!held);
            return;
        }
        // A confirmation holds only for the address it confirmed.
        if (confirmedWan != now.wanAddress) confirmedWan.clear();
        const std::string lan = lanNow();
        GatewayReading reading;
        reading.reportedWan = now.wanAddress;
        reading.gateway = readGateway(lan);
        if (pin.complete() && reading.gateway == pin && cfg.askGatewayWan) {
            const std::string before = lastGood;
            for (const std::string& a : cfg.askGatewayWan(pin.ip, lan, lastGood)) {
                if (checkWanAddress(a, cfg.addressPolicy) == WanCheck::Public) reading.gatewayWans.push_back(a);
            }
            // Saved by the host, so a restart asks the same description first.
            if (lastGood != before) update([&](MapperState& s) { s.gatewayLocation = lastGood; });
        }
        if (!reading.gatewayWans.empty()) gatewayAnswer = reading.gatewayWans.front();
        switch (judgeGateway(pin, reading)) {
            case GatewayVerdict::Match:
                contradicted = false;
                failedPasses = 0;
                gatewayAnswer = reading.reportedWan;
                if (held) {
                    // Held before: the gateway agrees now, so the PC user may re-share this address (and only this
                    // one). It isn't told without them, and a later contradiction is still looked for.
                    confirmedWan = reading.reportedWan;
                    privateLog("a held address the gateway now confirms: the PC user may re-share it");
                    again(false);
                    return;
                }
                if (mapper.acknowledgeAddressChange(reading.reportedWan)) {
                    privateLog("a new public address confirmed by the pinned gateway itself: told to devices");
                }
                // One report: mapped, and nothing left being checked.
                held = false;
                heldSince.reset();
                checkingSince.reset();
                contradicted = false;
                failedPasses = 0;
                gatewayAnswer.clear();
                confirmedWan.clear();
                verifying = false;
                verifyAt.reset();
                verifyStep = 0;
                update([&](MapperState& s) {
                    if (s.status == OutsideStatus::AddressChanged && mapper.report().status == OutsideStatus::Mapped) {
                        s.status = OutsideStatus::Mapped;
                    }
                    s.gatewayChecking = false;
                    s.gatewayFailedPasses = 0;
                    s.gatewayHeld = false;
                    s.gatewayContradicts = false;
                    s.gatewayAnswer.clear();
                    s.confirmedWan.clear();
                });
                return;
            case GatewayVerdict::Unreadable:
                ++failedPasses;
                again(!held);
                return;
            case GatewayVerdict::Contradicted:
                held = true;
                contradicted = true;
                confirmedWan.clear();
                failedPasses = 0;
                privateLog("the pinned gateway contradicts the new public address: held, and not to be acknowledged");
                again(false);
                return;
            case GatewayVerdict::OtherGateway:
                held = true;
                contradicted = false;
                confirmedWan.clear();
                failedPasses = 0;
                privateLog("a new public address through another gateway: held for the PC user");
                again(false);
                return;
            case GatewayVerdict::NoPin:
                privateLog("a new public address with no gateway pinned: waits for the PC user");
                stop();
                return;
        }
    };

    for (;;) {
        Job c;
        {
            std::unique_lock<std::mutex> lk(mu);
            for (;;) {
                if (!me->queue.empty()) {
                    c = me->queue.front();
                    me->queue.pop_front();
                    break;
                }
                if (on && std::chrono::steady_clock::now() >= nextCheck) {
                    c = Job{Cmd::Recheck, 0, 0};
                    break;
                }
                if (on && verifyAt && std::chrono::steady_clock::now() >= *verifyAt) {
                    c = Job{Cmd::VerifyGateway, 0, 0};
                    break;
                }
                if (on) me->cv.wait_until(lk, verifyAt ? std::min(nextCheck, *verifyAt) : nextCheck);
                else me->cv.wait(lk);
            }
        }
        switch (c.cmd) {
            case Cmd::Open:
                port = c.port;
                openId = c.id;
                on = true;
                removed = !c.map;
                if (c.pinAnew) {
                    // Turned on by the PC user: the first address read is the one, and the gateway is pinned anew.
                    pin = {};
                    lastGood.clear();
                    mapper.forgetAddress();
                    update([](MapperState& s) {
                        s.gatewayPin = {};
                        s.gatewayLocation.clear();
                    });
                    resolved();
                }
                if (removed) reportRemoved(); // kept removed across the open (a restart, a new port): no router call
                else {
                    openNow(port);
                    pinIfNone();
                    verifyChange();
                }
                nextCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.recheckEveryMs);
                break;
            case Cmd::VerifyGateway:
                verifyAt.reset();
                if (on && !removed) verifyChange();
                break;
            case Cmd::Recheck:
                if (!on) break;
                // While the PC user has the forward removed, the check maps nothing. It doesn't read the public
                // address either: that is read from the mapping (§8.3), and there is none. It is only counted.
                if (!removed) {
                    recheck();
                    pinIfNone();
                    verifyChange();
                }
                update([](MapperState& s) { ++s.rechecks; });
                nextCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.recheckEveryMs);
                break;
            case Cmd::Acknowledge: {
                // Only the address the PC user was shown, only while it is still the one read now, and never one the
                // pinned gateway contradicts (another device answered as the router).
                if (contradicted || (pin.complete() && (confirmedWan.empty() || c.wan != confirmedWan))) {
                    privateLog("an acknowledgement for an address no pass confirmed: refused");
                    update([](MapperState&) {});
                    break;
                }
                if (!mapper.acknowledgeAddressChange(c.wan)) {
                    privateLog("an acknowledgement for an address that isn't the one read now: ignored");
                    update([](MapperState&) {});
                    break;
                }
                // The PC user has seen it: the gateway of the forward's interface is pinned from now on (a gateway
                // that can't be read keeps the old pin).
                const GatewayIdentity read = readGateway(lanNow());
                if (read.complete()) {
                    if (!(read == pin)) lastGood.clear(); // another gateway's description is no use
                    pin = read;
                }
                update([&](MapperState& s) {
                    if (s.status == OutsideStatus::AddressChanged) s.status = OutsideStatus::Mapped;
                    s.gatewayPin = pin;
                    s.gatewayLocation = lastGood;
                });
                resolved();
                break;
            }
            case Cmd::RetryRemovals:
                // The stuck call returned: what turning off (or a non-mapped result) wanted removed goes now.
                if (haveRouter && !mapper.retryPendingRemovals()) privateLog("a kept removal is still waiting on the router");
                update([](MapperState&) {});
                break;
            case Cmd::RemoveNow: {
                if (!on) break;
                if (haveRouter) mapper.remove(); // only this PC's own forward goes; a stuck router keeps it for later
                removed = true;
                reportRemoved();
                nextCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.recheckEveryMs);
                break;
            }
            case Cmd::Remap:
                if (!on) break;
                removed = false;
                update([](MapperState& s) { s.checking = true; });
                openNow(port);
                pinIfNone();
                verifyChange();
                nextCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.recheckEveryMs);
                break;
            case Cmd::RemoveLeftover: {
                // §8.7: never in a test run with the real router (the opener is a test's fake or none there).
                const uint16_t p = c.port;
                const std::optional<std::string> lan = cfg.lanAddress ? cfg.lanAddress() : internalClientIPv4();
                if (!haveRouter || p == 0) {
                    privateLog("start-up: no router to remove a leftover forward from");
                } else if (!lan || !usableInternalClient(*lan)) {
                    privateLog("start-up: no home-network address, so a leftover forward can't be checked as ours");
                } else if (on && p == port) {
                    privateLog("start-up: the leftover forward's port is in use again; kept");
                } else {
                    const PortMapper::Outcome o = mapper.removeLeftover(p, *lan);
                    privateLog(std::string("start-up: leftover forward on port ") + std::to_string(p) + ": " +
                               (o.kind == PortMapper::Outcome::Removed  ? "removed"
                                : o.kind == PortMapper::Outcome::Absent ? "none of ours there, nothing removed"
                                                                        : "the router didn't answer, nothing removed"));
                }
                update([](MapperState&) {}); // heldPort as it is now: the host clears outsideMappedPort
                break;
            }
            case Cmd::Close:
            case Cmd::Exit:
                on = false;
                removed = false;
                if (haveRouter && port != 0) mapper.remove(); // best effort: only this PC's own forward goes
                port = 0;
                openId = 0;
                verifying = false;
                verifyAt.reset();
                verifyStep = 0;
                update([&](MapperState& s) {
                    s = MapperState{.generation = s.generation, .rechecks = s.rechecks, .gatewayPin = pin, .gatewayLocation = lastGood};
                });
                if (c.cmd == Cmd::Exit) return;
                break;
        }
    }
}

OutsideMapper::OutsideMapper(Config cfg, std::function<void(const MapperState&)> onChange) : sh_(std::make_shared<Shared>()) {
    sh_->cfg = std::move(cfg);
    sh_->state.gatewayPin = sh_->cfg.pinned; // what the host saved, until the worker says otherwise
    sh_->state.gatewayLocation = sh_->cfg.pinned.complete() ? sh_->cfg.gatewayLocation : std::string();
    sh_->onChange = std::move(onChange);
    std::shared_ptr<Shared> sh = sh_;
    // Detached at once: the worker holds what it uses (sh), and nothing ever joins it (shutdown never waits).
    std::thread([sh] { Shared::run(sh); }).detach();
}

OutsideMapper::~OutsideMapper() { shutdown(); }

uint64_t OutsideMapper::open(uint16_t port, bool map, bool pinAnew) {
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(sh_->mu);
        if (sh_->stopped) return 0;
        id = ++sh_->openSeq;
        // Shown as being worked on at once, so the PC and owners see "mapping" rather than a stale result.
        sh_->state.checking = true;
        sh_->state.reported = false;
        sh_->state.port = port;
        sh_->state.openId = id;
        ++sh_->state.generation;
    }
    sh_->post(Cmd::Open, port, id, map, pinAnew);
    return id;
}

void OutsideMapper::removeNow() { sh_->post(Cmd::RemoveNow); }

void OutsideMapper::remapNow() { sh_->post(Cmd::Remap); }

void OutsideMapper::removeLeftover(uint16_t port) { sh_->post(Cmd::RemoveLeftover, port); }

void OutsideMapper::close() { sh_->post(Cmd::Close); }

void OutsideMapper::acknowledgeAddressChange(const std::string& shownWan) {
    sh_->post(Cmd::Acknowledge, 0, 0, true, false, shownWan);
}

void OutsideMapper::recheckNow() { sh_->post(Cmd::Recheck); }

MapperState OutsideMapper::state() const {
    std::lock_guard<std::mutex> lk(sh_->mu);
    return sh_->state;
}

void OutsideMapper::shutdown() {
    {
        std::lock_guard<std::mutex> lk(sh_->mu);
        if (sh_->stopped) return;
        // Whatever was still queued is dropped: the removal is the last thing the worker does.
        sh_->queue.clear();
        sh_->queue.push_back(Job{Cmd::Exit, 0, 0});
        sh_->stopped = true;
    }
    sh_->cv.notify_all();
    // Waits only for a callback already running (it takes the host's lock briefly), never for the router.
    std::lock_guard<std::mutex> cb(sh_->cb_mu);
    sh_->onChange = nullptr;
}

} // namespace kvhost::outside
