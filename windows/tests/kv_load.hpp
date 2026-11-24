// The load a timing row runs under: how busy the rest of this PC was while the row ran. A timing floor (a time the
// product promises, checked with KV_FLOOR) that is missed while the PC was busy is reported as skipped (the runner
// exits kSkipReturnCode, which ctest's SKIP_RETURN_CODE shows as "Not Run"), never as passed. A floor missed on a quiet
// PC fails as any check does, and a floor met on a busy PC passes, since it held anyway. Correctness checks (order,
// counts, what must never happen) stay KV_CHECK and fail in every run: only a floor goes through KV_FLOOR, and nothing
// is retried, stretched or lowered. No row makes load to test this but test_load_floor's one, which prints its window.
// The merge rule: a PR whose ctest shows timing rows skipped has those rows run on a quiet PC at its head before it
// merges.
// KV_TEST_LOAD_FORCE (busy or quiet) forces the load's outcome, and only in a binary built with KV_LOAD_FORCE_ALLOWED:
// test_load_floor alone, to show what the runner does with each. Every other binary ignores it, so no environment can
// turn a missed floor into a skip (test_load_unforced checks that).
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

// The PC was busy when more than kLoadBusyShare of the row's samples had other processes' load at or above the busy
// line: loadLineLps(N) logical processors' worth, half of this PC's N and at least kLoadBusyMinLps (never more than N).
// Counted in logical processors, not as a share tuned to one PC, so it holds on a 4-core laptop as on 16 cores. Set
// from traces on 16 logical processors, 100 ms samples: an idle PC (other load p50 16 to 18 %, spikes to 87 %) had
// about 5 % of its samples at or above 8 processors' worth, a light two-job build 1 to 3 % (29 % in its burst minute),
// and a real build about 40 to 50 %. Under the line, or at it for no more than a quarter of the row, the PC is quiet
// and a missed floor fails: one missed beside a light build fails and wants a quiet rerun, the honest side to err on.
constexpr double kLoadBusyShare = 0.25;
constexpr unsigned kLoadBusyMinLps = 3;
inline unsigned loadLineLps(unsigned logicalProcessors) {
    return std::min(logicalProcessors, std::max(kLoadBusyMinLps, logicalProcessors / 2));
}
inline unsigned loadLogicalProcessors() { return std::max(1u, std::thread::hardware_concurrency()); }

// Whether this binary honours KV_TEST_LOAD_FORCE (built with KV_LOAD_FORCE_ALLOWED: test_load_floor alone).
#ifdef KV_LOAD_FORCE_ALLOWED
constexpr bool kLoadForceAllowed = true;
#else
constexpr bool kLoadForceAllowed = false;
#endif

struct LoadReport {
    int samples = 0;
    unsigned lps = 0;        // this PC's logical processors
    unsigned lineLps = 0;    // the busy line, in logical processors' worth of other load
    double p50 = 0, max = 0; // other processes' load, % of the whole PC
    double busyShare = 0;    // the share of samples at or above the line
    bool busy = false;
    const char* forced = nullptr; // "busy" or "quiet" when test_load_floor forced it (KV_TEST_LOAD_FORCE), else null
    double p50Lps() const { return p50 * lps / 100.0; }
    std::string line() const {
        char buf[240];
        std::snprintf(buf, sizeof(buf),
                      "PC: other load p50 %.1f of %u logical processors (%.0f %%), max %.0f %%; "
                      "at or over %u in %.0f %% of %d samples (%s%s)",
                      p50Lps(), lps, p50, max, lineLps, busyShare * 100.0, samples, busy ? "busy" : "quiet",
                      forced ? ", forced" : "");
        return buf;
    }
};

// The report for these samples (each other processes' load, % of the whole PC) on a PC of this many logical processors.
inline LoadReport loadReportFrom(std::vector<double> samples, unsigned logicalProcessors = loadLogicalProcessors()) {
    LoadReport r;
    r.lps = std::max(1u, logicalProcessors);
    r.lineLps = loadLineLps(r.lps);
    r.samples = static_cast<int>(samples.size());
    if (samples.empty()) return r;
    std::sort(samples.begin(), samples.end());
    r.p50 = samples[samples.size() / 2];
    r.max = samples.back();
    const double atOrOver = 100.0 * r.lineLps; // a sample s (in %) is at or over the line when s * lps >= this
    const auto n = std::count_if(samples.begin(), samples.end(), [&](double s) { return s * r.lps >= atOrOver; });
    r.busyShare = static_cast<double>(n) / static_cast<double>(samples.size());
    r.busy = r.busyShare > kLoadBusyShare;
    return r;
}

// What a floor comes to: met (passes on any PC), missed on a quiet PC (fails), missed on a busy PC (skipped).
enum class Floor { Met, Failed, Skipped };
inline Floor judgeFloor(bool met, bool busy) { return met ? Floor::Met : busy ? Floor::Skipped : Floor::Failed; }

// A load report kept as it was (LoadSampler::snapshot); KV_FLOOR takes one as it takes a sampler.
struct LoadSnapshot {
    LoadReport r;
    LoadReport report() const { return r; }
};

// Samples other processes' CPU every everyMs from construction: the whole PC's busy time (GetSystemTimes) less this
// process's own (GetProcessTimes), over the whole PC's time. Start one when a row starts, before what it times.
class LoadSampler {
public:
    explicit LoadSampler(int everyMs = 100) : everyMs_(everyMs) {
        read(lastBusy_, lastTotal_, lastSelf_);
        t_ = std::thread([this] {
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(everyMs_));
                sample();
            }
        });
    }
    ~LoadSampler() {
        stop_.store(true);
        if (t_.joinable()) t_.join();
    }
    LoadSampler(const LoadSampler&) = delete;
    LoadSampler& operator=(const LoadSampler&) = delete;

    // The load from the start to now, kept: a row takes one when its timed part ends, so a floor is judged against the
    // stretch it timed, not against the checking that follows it.
    LoadSnapshot snapshot() { return LoadSnapshot{report()}; }

    // The load from the start to now (one more sample is taken now). It keeps sampling.
    LoadReport report() {
        sample();
        std::vector<double> copy;
        {
            std::lock_guard<std::mutex> lk(m_);
            copy = samples_;
        }
        LoadReport r = loadReportFrom(std::move(copy));
#ifdef KV_LOAD_FORCE_ALLOWED
        // test_load_floor only: a row forces the outcome to show what the runner does with it.
        if (const char* f = std::getenv("KV_TEST_LOAD_FORCE")) {
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

// A timing floor: met passes; missed on a busy PC is skipped (and printed with the load's numbers); missed on a quiet
// PC fails. load is a LoadSampler started before what the row times.
#define KV_FLOOR(cond, load, what)                                                                                     \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            const ::kvtest::LoadReport kv_load_ = (load).report();                                                     \
            if (::kvtest::judgeFloor(false, kv_load_.busy) == ::kvtest::Floor::Skipped) {                              \
                std::printf("  FLOOR SKIPPED %s:%d: %s (%s); %s\n", __FILE__, __LINE__, #cond, std::string(what).c_str(), \
                            kv_load_.line().c_str());                                                                  \
                ++::kvtest::skips();                                                                                   \
            } else {                                                                                                   \
                std::printf("  FLOOR FAILED %s:%d: %s (%s); %s\n", __FILE__, __LINE__, #cond, std::string(what).c_str(),  \
                            kv_load_.line().c_str());                                                                  \
                ++::kvtest::failures();                                                                                \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)
