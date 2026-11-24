// The desk a timing row runs on: how busy the rest of this PC was while the row ran. A timing floor (a time the
// product promises, checked with KV_FLOOR) that is missed while the desk was busy is reported as skipped (the runner
// exits kSkipReturnCode, which ctest's SKIP_RETURN_CODE shows as "Not Run"), never as passed. A floor missed on a quiet
// desk fails as any check does, and a floor met on a busy desk passes, since it held anyway. Correctness checks (order,
// counts, what must never happen) stay KV_CHECK and fail in every run: only a floor goes through KV_FLOOR, and nothing
// is retried, stretched or lowered. No row makes load to test this but test_desk_floor's one, which prints its window.
// The merge rule: a PR whose ctest shows timing rows skipped has those rows run on a quiet desk at its head before it
// merges.
// KV_TEST_DESK_FORCE (busy or quiet) forces the desk's outcome, and only in a binary built with KV_DESK_FORCE_ALLOWED:
// test_desk_floor alone, to show what the runner does with each. Every other binary ignores it, so no environment can
// turn a missed floor into a skip (test_desk_unforced checks that).
#pragma once

#include "kv_test.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kvtest {

// The desk was busy when more than kDeskBusyShare of the row's samples had other processes' load at or above the busy
// line: deskLineLps(N) logical processors' worth, half of this PC's N and at least kDeskBusyMinLps (never more than N).
// Counted in logical processors, not as a share tuned to one PC, so it holds on a 4-core laptop as on 16 cores. Set
// from traces on 16 logical processors, 100 ms samples: an idle desk (other load p50 16 to 18 %, spikes to 87 %) had
// about 5 % of its samples at or above 8 processors' worth, a two-job Rust gate 1 to 3 % (29 % in its burst minute),
// and a real build about 40 to 50 %. Under the line, or at it for no more than a quarter of the row, the desk is quiet
// and a missed floor fails: one missed beside a light build fails and wants a quiet rerun, the honest side to err on.
constexpr double kDeskBusyShare = 0.25;
constexpr unsigned kDeskBusyMinLps = 3;
inline unsigned deskLineLps(unsigned logicalProcessors) {
    return std::min(logicalProcessors, std::max(kDeskBusyMinLps, logicalProcessors / 2));
}
inline unsigned deskLogicalProcessors() { return std::max(1u, std::thread::hardware_concurrency()); }

// Whether this binary honours KV_TEST_DESK_FORCE (built with KV_DESK_FORCE_ALLOWED: test_desk_floor alone).
#ifdef KV_DESK_FORCE_ALLOWED
constexpr bool kDeskForceAllowed = true;
#else
constexpr bool kDeskForceAllowed = false;
#endif

struct DeskReport {
    int samples = 0;
    unsigned lps = 0;        // this PC's logical processors
    unsigned lineLps = 0;    // the busy line, in logical processors' worth of other load
    double p50 = 0, max = 0; // other processes' load, % of the whole PC
    double busyShare = 0;    // the share of samples at or above the line
    bool busy = false;
    const char* forced = nullptr; // "busy" or "quiet" when test_desk_floor forced it (KV_TEST_DESK_FORCE), else null
    double p50Lps() const { return p50 * lps / 100.0; }
    std::string line() const {
        char buf[240];
        std::snprintf(buf, sizeof(buf),
                      "desk: other load p50 %.1f of %u logical processors (%.0f %%), max %.0f %%; "
                      "at or over %u in %.0f %% of %d samples (%s%s)",
                      p50Lps(), lps, p50, max, lineLps, busyShare * 100.0, samples, busy ? "busy" : "quiet",
                      forced ? ", forced" : "");
        return buf;
    }
};

// The report for these samples (each other processes' load, % of the whole PC) on a PC of this many logical processors.
inline DeskReport deskReportFrom(std::vector<double> samples, unsigned logicalProcessors = deskLogicalProcessors()) {
    DeskReport r;
    r.lps = std::max(1u, logicalProcessors);
    r.lineLps = deskLineLps(r.lps);
    r.samples = static_cast<int>(samples.size());
    if (samples.empty()) return r;
    std::sort(samples.begin(), samples.end());
    r.p50 = samples[samples.size() / 2];
    r.max = samples.back();
    const double atOrOver = 100.0 * r.lineLps; // a sample s (in %) is at or over the line when s * lps >= this
    const auto n = std::count_if(samples.begin(), samples.end(), [&](double s) { return s * r.lps >= atOrOver; });
    r.busyShare = static_cast<double>(n) / static_cast<double>(samples.size());
    r.busy = r.busyShare > kDeskBusyShare;
    return r;
}

// What a floor comes to: met (passes on any desk), missed on a quiet desk (fails), missed on a busy desk (skipped).
enum class Floor { Met, Failed, Skipped };
inline Floor judgeFloor(bool met, bool busy) { return met ? Floor::Met : busy ? Floor::Skipped : Floor::Failed; }

// A desk report kept as it was (DeskSampler::snapshot); KV_FLOOR takes one as it takes a sampler.
struct DeskSnapshot {
    DeskReport r;
    DeskReport report() const { return r; }
};

// Samples other processes' CPU every everyMs from construction: the whole PC's busy time (GetSystemTimes) less this
// process's own (GetProcessTimes), over the whole PC's time. Start one when a row starts, before what it times.
class DeskSampler {
public:
    explicit DeskSampler(int everyMs = 100) : everyMs_(everyMs) {
        read(lastBusy_, lastTotal_, lastSelf_);
        t_ = std::thread([this] {
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(everyMs_));
                sample();
            }
        });
    }
    ~DeskSampler() {
        stop_.store(true);
        if (t_.joinable()) t_.join();
    }
    DeskSampler(const DeskSampler&) = delete;
    DeskSampler& operator=(const DeskSampler&) = delete;

    // The desk from the start to now, kept: a row takes one when its timed part ends, so a floor is judged against the
    // stretch it timed, not against the checking that follows it.
    DeskSnapshot snapshot() { return DeskSnapshot{report()}; }

    // The desk from the start to now (one more sample is taken now). It keeps sampling.
    DeskReport report() {
        sample();
        std::vector<double> copy;
        {
            std::lock_guard<std::mutex> lk(m_);
            copy = samples_;
        }
        DeskReport r = deskReportFrom(std::move(copy));
#ifdef KV_DESK_FORCE_ALLOWED
        // test_desk_floor only: a row forces the outcome to show what the runner does with it.
        if (const char* f = std::getenv("KV_TEST_DESK_FORCE")) {
            const std::string force = f;
            if (force == "busy" || force == "quiet") {
                r.busy = force == "busy";
                r.forced = r.busy ? "busy" : "quiet";
            }
        }
#endif
        return r;
    }

private:
    static uint64_t ticks(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
    static void read(uint64_t& busy, uint64_t& total, uint64_t& self) {
        FILETIME idle{}, kernel{}, user{}, created{}, exited{}, pk{}, pu{};
        GetSystemTimes(&idle, &kernel, &user); // kernel includes idle
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &pk, &pu);
        total = ticks(kernel) + ticks(user);
        busy = total - ticks(idle);
        self = ticks(pk) + ticks(pu);
    }
    void sample() {
        std::lock_guard<std::mutex> lk(m_);
        uint64_t busy = 0, total = 0, self = 0;
        read(busy, total, self);
        const double dt = static_cast<double>(total - lastTotal_);
        if (dt > 0) {
            const double other = (static_cast<double>(busy - lastBusy_) - static_cast<double>(self - lastSelf_)) / dt * 100.0;
            samples_.push_back(std::clamp(other, 0.0, 100.0));
        }
        lastBusy_ = busy;
        lastTotal_ = total;
        lastSelf_ = self;
    }

    int everyMs_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::vector<double> samples_;
    uint64_t lastBusy_ = 0, lastTotal_ = 0, lastSelf_ = 0;
    std::thread t_;
};

} // namespace kvtest

// A timing floor: met passes; missed on a busy desk is skipped (and printed with the desk's numbers); missed on a quiet
// desk fails. desk is a DeskSampler started before what the row times.
#define KV_FLOOR(cond, desk, what)                                                                                     \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            const ::kvtest::DeskReport kv_desk_ = (desk).report();                                                     \
            if (::kvtest::judgeFloor(false, kv_desk_.busy) == ::kvtest::Floor::Skipped) {                              \
                std::printf("  FLOOR SKIPPED %s:%d: %s (%s); %s\n", __FILE__, __LINE__, #cond, std::string(what).c_str(), \
                            kv_desk_.line().c_str());                                                                  \
                ++::kvtest::skips();                                                                                   \
            } else {                                                                                                   \
                std::printf("  FLOOR FAILED %s:%d: %s (%s); %s\n", __FILE__, __LINE__, #cond, std::string(what).c_str(),  \
                            kv_desk_.line().c_str());                                                                  \
                ++::kvtest::failures();                                                                                \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)
