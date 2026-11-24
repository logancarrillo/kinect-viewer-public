// The load check for timing floors (kv_load.hpp): a floor missed while the rest of the PC is busy is skipped, never
// passed, and the runner says so with its exit code; a floor missed on a quiet PC fails; a floor met passes. Run as
// "test_load_floor --burn MS" it only keeps one more logical processor than the busy line busy for MS milliseconds:
// another process's load for the one row that makes any, brief, so that a run beside other work disturbs it little.
#include "kv_load.hpp"
#include "kv_test.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

static_assert(kvtest::kLoadForceAllowed, "test_load_floor forces the load: build it with KV_LOAD_FORCE_ALLOWED");

namespace {

using kvtest::LoadReport;
using kvtest::Floor;

// This program again, with args, and these test-only variables set for it alone. Its exit code, or -1 when it couldn't
// start. Waits for it unless wait is false (then it returns 0 and hands back its process handle through *handle).
int runSelf(const std::wstring& args, const std::vector<std::pair<std::wstring, std::wstring>>& env, bool wait = true,
            HANDLE* handle = nullptr) {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" " + args;
    for (const auto& [k, v] : env) SetEnvironmentVariableW(k.c_str(), v.c_str());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    for (const auto& [k, v] : env) SetEnvironmentVariableW(k.c_str(), nullptr);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    if (!wait) {
        *handle = pi.hProcess;
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 60'000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// How many logical processors the burner keeps busy: one more than the busy line, at most all of them.
unsigned burnerLps() {
    const unsigned n = kvtest::loadLogicalProcessors();
    return std::min(n, kvtest::loadLineLps(n) + 1);
}

int burn(int ms) {
    const unsigned n = burnerLps();
    std::atomic<bool> stop{false};
    std::vector<std::thread> spinners;
    for (unsigned i = 0; i < n; ++i) {
        spinners.emplace_back([&] {
            volatile uint64_t x = 0;
            while (!stop.load(std::memory_order_relaxed)) x = x + 1;
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    stop.store(true);
    for (auto& t : spinners) t.join();
    return 0;
}

bool demo() { return std::getenv("KV_LOAD_DEMO") != nullptr; }

// The local wall clock, "HH:MM:SS.mmm", for the load window.
std::string clockNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

} // namespace

// A floor met passes on any PC; missed, it fails on a quiet PC and is skipped on a busy one.
KV_TEST(a_missed_floor_is_skipped_only_on_a_busy_pc) {
    KV_CHECK(kvtest::judgeFloor(true, false) == Floor::Met);
    KV_CHECK(kvtest::judgeFloor(true, true) == Floor::Met);
    KV_CHECK(kvtest::judgeFloor(false, false) == Floor::Failed);
    KV_CHECK(kvtest::judgeFloor(false, true) == Floor::Skipped);
}

// The busy line is half of the PC's logical processors, at least three of them, never more than all of them.
KV_TEST(the_busy_line_is_half_the_logical_processors_and_at_least_three) {
    KV_CHECK(kvtest::loadLineLps(1) == 1);
    KV_CHECK(kvtest::loadLineLps(2) == 2);
    KV_CHECK(kvtest::loadLineLps(4) == 3);  // 75 % of a 4-core PC
    KV_CHECK(kvtest::loadLineLps(6) == 3);  // 50 %
    KV_CHECK(kvtest::loadLineLps(8) == 4);  // 50 %
    KV_CHECK(kvtest::loadLineLps(16) == 8); // 50 %
    KV_CHECK(kvtest::loadLineLps(32) == 16);
}

// The PC was busy when more than a quarter of the row's samples had other load at or above the line.
KV_TEST(the_pc_is_busy_when_more_than_a_quarter_of_the_row_is_at_the_line) {
    KV_CHECK(!kvtest::loadReportFrom({}, 16).busy);
    std::vector<double> s(20, 18.0);
    LoadReport r = kvtest::loadReportFrom(s, 16);
    KV_CHECK(!r.busy && r.samples == 20 && r.p50 == 18.0 && r.max == 18.0 && r.lps == 16 && r.lineLps == 8);
    for (int i = 0; i < 5; ++i) s[i] = 50.0; // exactly a quarter at the line (8 of 16): not more
    r = kvtest::loadReportFrom(s, 16);
    KV_CHECK(!r.busy && r.max == 50.0 && r.busyShare == 0.25);
    s[5] = 49.9; // just under the line: doesn't count
    KV_CHECK(!kvtest::loadReportFrom(s, 16).busy);
    s[5] = 50.0; // six of twenty at the line
    r = kvtest::loadReportFrom(s, 16);
    KV_CHECK(r.busy && r.p50 == 18.0 && r.busyShare == 0.3);
    const std::string line = r.line();
    KV_CHECK_MSG(line.find("PC: other load p50 2.9 of 16 logical processors (18 %), max 50 %; "
                           "at or over 8 in 30 % of 20 samples (busy)") == 0,
                 line);
    // On a 4-core PC the same shares are counted against 3 of 4 (75 %), not half.
    KV_CHECK(!kvtest::loadReportFrom(s, 4).busy);
    for (int i = 0; i < 6; ++i) s[i] = 75.0;
    KV_CHECK(kvtest::loadReportFrom(s, 4).busy);
}

// The traces the line was set from, as sample sets of 100 on 16 logical processors: an idle PC (other load p50
// 18 %, spikes to 87 %) and a light two-job build's usual minute read quiet; its burst minute and a real build read busy.
KV_TEST(an_idle_pc_reads_quiet_and_a_real_build_reads_busy) {
    auto set = [](std::vector<std::pair<int, double>> runs) {
        std::vector<double> s;
        for (const auto& [count, value] : runs) s.insert(s.end(), static_cast<size_t>(count), value);
        return s;
    };
    // Idle: deciles 11 12 14 16 18 21 25 31 41, 8 % over 45 %, max 87 %.
    const auto idle = set({{10, 9}, {10, 11}, {10, 12}, {10, 14}, {10, 16}, {10, 18}, {10, 21}, {10, 25}, {10, 31},
                           {1, 41}, {1, 44}, {1, 46}, {1, 50}, {1, 55}, {1, 60}, {1, 65}, {1, 70}, {1, 80}, {1, 87}});
    // The light build's usual minute: p50 29 %, p90 38 %, 2 % at or over 50 %. Its burst minute: p50 37 %, p90 71 %, 29 %.
    const auto light = set({{20, 22}, {30, 27}, {10, 29}, {20, 31}, {11, 38}, {7, 45}, {2, 63}});
    const auto burst = set({{40, 28}, {20, 37}, {11, 45}, {29, 71}});
    const auto build = set({{30, 40}, {20, 44}, {10, 45}, {30, 52}, {10, 60}}); // p50 45 %, 40 % at or over 50 %
    const LoadReport ri = kvtest::loadReportFrom(idle, 16), rg = kvtest::loadReportFrom(light, 16),
                     rb = kvtest::loadReportFrom(burst, 16), rr = kvtest::loadReportFrom(build, 16);
    std::printf("  idle: %s\n  light: %s\n  burst: %s\n  build: %s\n", ri.line().c_str(), rg.line().c_str(),
                rb.line().c_str(), rr.line().c_str());
    KV_CHECK_MSG(!ri.busy && ri.p50 == 18.0 && ri.busyShare == 0.07, ri.line());
    KV_CHECK_MSG(!rg.busy && rg.busyShare == 0.02, rg.line());
    KV_CHECK_MSG(rb.busy && rb.busyShare == 0.29, rb.line());
    KV_CHECK_MSG(rr.busy && rr.p50 == 45.0 && rr.busyShare == 0.40, rr.line());
}

// A snapshot keeps the load as it was when the timed part ended; the sampler goes on sampling.
KV_TEST(a_snapshot_keeps_the_load_as_it_was) {
    kvtest::LoadSampler load(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const kvtest::LoadSnapshot snap = load.snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    KV_CHECK(snap.r.samples >= 1);
    KV_CHECK(snap.report().samples == snap.r.samples && snap.report().line() == snap.r.line());
    KV_CHECK_MSG(load.report().samples > snap.r.samples, std::to_string(snap.r.samples));
}

// The rows the next row runs in a child, each only when asked (KV_LOAD_DEMO): a floor that is never met, and one
// that always is, with the load sampled for a second.
KV_TEST(demo_a_floor_that_is_never_met) {
    if (!demo()) return;
    kvtest::LoadSampler load;
    std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
    KV_FLOOR(false, load, "a floor no run meets");
}
KV_TEST(demo_a_floor_that_is_always_met) {
    if (!demo()) return;
    kvtest::LoadSampler load;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    KV_FLOOR(true, load, "a floor every run meets");
}

// What the runner exits with, which is what ctest shows (SKIP_RETURN_CODE): a missed floor on a busy PC is skipped
// (kSkipReturnCode, "Not Run"), never passed (0); on a quiet PC it fails (1); a met floor passes even on a busy PC.
// Then with a real busy PC: another process keeps one more logical processor than the busy line busy, the sampler
// here sees it, and the missed floor is skipped again, not passed. This is the only row anywhere that makes load, once,
// for about 2.6 s; it prints its window, so a timing miss elsewhere in it can be told apart.
KV_TEST(a_busy_pc_gives_skipped_never_passed) {
    const std::wstring missed = L"demo_a_floor_that_is_never_met", met = L"demo_a_floor_that_is_always_met";
    KV_CHECK(runSelf(missed, {{L"KV_LOAD_DEMO", L"1"}, {L"KV_TEST_LOAD_FORCE", L"busy"}}) == kvtest::kSkipReturnCode);
    KV_CHECK(runSelf(missed, {{L"KV_LOAD_DEMO", L"1"}, {L"KV_TEST_LOAD_FORCE", L"quiet"}}) == 1);
    KV_CHECK(runSelf(met, {{L"KV_LOAD_DEMO", L"1"}, {L"KV_TEST_LOAD_FORCE", L"busy"}}) == 0);

    HANDLE burner = nullptr;
    const std::string from = clockNow();
    KV_REQUIRE(runSelf(L"--burn 2600", {}, false, &burner) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // its spinners start
    kvtest::LoadSampler load;
    const int code = runSelf(missed, {{L"KV_LOAD_DEMO", L"1"}}); // samples for a second, as the burner runs
    const LoadReport seen = load.report();
    WaitForSingleObject(burner, 10'000);
    CloseHandle(burner);
    std::printf("  load window: %s to %s, %u of %u logical processors\n", from.c_str(), clockNow().c_str(), burnerLps(),
                kvtest::loadLogicalProcessors());
    KV_CHECK_MSG(seen.busy && seen.p50Lps() >= seen.lineLps, seen.line());
    KV_CHECK_MSG(code == kvtest::kSkipReturnCode, "exit " + std::to_string(code));
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--burn") return burn(std::atoi(argv[2]));
    return kvtest::runAll(argc, argv);
}
