// The keep-running watchdog and the crash recorder (a host that died with Windows Error Reporting off left nothing to
// go on). Synthetic only: the "viewers" here are cmd.exe children or
// child copies of this test exe, the named objects carry this run's own suffix, and the crash records go into new
// temp folders. Nothing here opens, signals or names a viewer it didn't start.
#include "kv_test.hpp"

#include "crash_recorder.hpp"
#include "keep_running.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace kvhost;

namespace {

HANDLE startCmd(const wchar_t* args) {
    std::wstring cmd = std::wstring(L"cmd.exe /c ") + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// This exe again, as a child: `args` after it. Its process handle (full access: the watchdog reads its memory).
HANDLE startSelf(const std::wstring& args) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

struct Recorder {
    std::mutex m;
    std::vector<std::string> lines;
    void add(const std::string& l) {
        std::lock_guard<std::mutex> g(m);
        lines.push_back(l);
    }
    bool has(const std::string& part) {
        std::lock_guard<std::mutex> g(m);
        for (const auto& l : lines)
            if (l.find(part) != std::string::npos) return true;
        return false;
    }
    std::string all() {
        std::lock_guard<std::mutex> g(m);
        std::string s;
        for (const auto& l : lines) s += "    | " + l + "\n";
        return s;
    }
};

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

RestartPolicy fast(int max = 5) {
    RestartPolicy p;
    p.maxRestarts = max;
    p.delaysMs = {10, 10, 10, 10, 10};
    p.dumpTimeoutMs = 2000;
    p.endGraceMs = 2000;
    return p;
}

std::wstring tempDir(const wchar_t* tag) {
    static std::atomic<int> n{0};
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    const std::wstring d = std::wstring(base) + L"kv-keep-running-" + tag + L"-" + std::to_wstring(GetCurrentProcessId()) +
                           L"-" + std::to_wstring(++n);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

int countFiles(const std::wstring& dir, const wchar_t* pattern) {
    int n = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do ++n;
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

std::string readText(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void removeDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n != L"." && n != L"..") DeleteFileW((dir + L"\\" + n).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// A child copy that installs the recorder into `dir` (no watchdog) and raises `kind`: its exit code and how long
// it took to end.
DWORD crashChild(const std::wstring& dir, int kind, uint64_t* tookMs = nullptr) {
    const uint64_t t0 = nowMs();
    HANDLE h = startSelf(L"--crash-child \"" + dir + L"\" " + std::to_wstring(kind));
    if (!h) return 0xFFFFFFFF;
    WaitForSingleObject(h, 60000);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (tookMs) *tookMs = nowMs() - t0;
    return code;
}

// A restarted "viewer" that runs a moment and stops cleanly: its clean-stop event is set, and kept in held until
// the test ends so the watchdog sees it.
HANDLE cleanlyStopping(const KeepRunningNames& names, std::vector<HANDLE>& held) {
    HANDLE h = startCmd(L"ping -n 2 127.0.0.1 >nul");
    if (h) held.push_back(CreateEventW(nullptr, TRUE, TRUE, names.cleanStop(GetProcessId(h)).c_str()));
    return h;
}

WatchdogIo ioFor(Recorder& log, std::function<HANDLE(const std::wstring&)> relaunch,
                 std::function<std::string(HANDLE, DWORD, const FaultRecord&)> write = {}) {
    WatchdogIo io;
    io.relaunch = std::move(relaunch);
    io.log = [&log](const std::string& l) { log.add(l); };
    io.nowMs = nowMs;
    io.writeFault = std::move(write);
    return io;
}

} // namespace

KV_TEST(restarts_back_off_and_give_up_after_five_in_ten_minutes) {
    const RestartPolicy p;
    KV_CHECK(nextRestartDelayMs({}, 1000000, p) == 2000u);
    KV_CHECK(nextRestartDelayMs({1000000}, 1000001, p) == 5000u);
    std::vector<uint64_t> five = {1000000, 1010000, 1020000, 1030000, 1040000};
    KV_CHECK(!nextRestartDelayMs(five, 1050000, p).has_value());
    // Ten minutes after the first, it has aged out: one more is allowed, after the longest wait.
    KV_CHECK(nextRestartDelayMs(five, 1000000 + 600000, p) == 60000u);
    // Restarts long ago don't count at all.
    KV_CHECK(nextRestartDelayMs({1, 2, 3, 4, 5}, 10000000, p) == 2000u);
}

KV_TEST(a_test_runs_named_objects_are_its_own) {
    const KeepRunningNames real = keepRunningNames(), a = keepRunningNames(keepRunningTestSuffix()),
                           b = keepRunningNames(keepRunningTestSuffix());
    KV_CHECK(real.running() == L"Local\\KinectViewer-KeepRunning-Running");
    KV_CHECK(real.standDown(42) == L"Local\\KinectViewer-KeepRunning-StandDown-42"); // install.ps1's name
    for (const auto* n : {&a, &b}) {
        KV_CHECK(n->running() != real.running() && n->watch(42) != real.watch(42) && n->cleanStop(42) != real.cleanStop(42) &&
                 n->block(42) != real.block(42) && n->fault(42) != real.fault(42) && n->standDown(42) != real.standDown(42));
        KV_CHECK(n->prefix.rfind(L"Local\\", 0) == 0); // this user session only
    }
    KV_CHECK(a.running() != b.running());
}

// A restart keeps the launch's state. The launch's own arguments, with the sensor as it was last
// asked: off adds --no-autostart, on takes a launch's --no-autostart away.
KV_TEST(a_restart_keeps_the_launchs_arguments_and_the_sensors_choice) {
    KV_CHECK(relaunchArguments(L"", false) == L"");
    KV_CHECK(relaunchArguments(L"", true) == L"--no-autostart");
    KV_CHECK(relaunchArguments(L"--mode ir --state-dir \"D:\\a b\\state\"", false) == L"--mode ir --state-dir \"D:\\a b\\state\"");
    KV_CHECK(relaunchArguments(L"--mode ir --state-dir \"D:\\a b\\state\"", true) ==
             L"--mode ir --state-dir \"D:\\a b\\state\" --no-autostart");
    KV_CHECK(relaunchArguments(L"--no-autostart --mode ir", false) == L"--mode ir");
    KV_CHECK(relaunchArguments(L"--no-autostart --mode ir", true) == L"--mode ir --no-autostart");
}

KV_TEST(a_viewer_that_ends_without_a_clean_stop_is_started_again_as_launched_and_as_it_was) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    std::vector<std::wstring> asked;
    HANDLE killed = startCmd(L"ping -n 2 127.0.0.1 >nul & exit 3"); // abort()'s exit code
    KV_REQUIRE(killed != nullptr);
    // The viewer's side, made for that process as a viewer makes it for itself: launched with --mode ir, outside
    // viewing on, the sensor asked off.
    KeepRunning kr(names, GetProcessId(killed), L"--mode ir", false, [] { return true; });
    kr.set(true);
    kr.setSensorOff(true);
    HANDLE ev_second = nullptr;
    auto io = ioFor(log, [&](const std::wstring& args) -> HANDLE {
        asked.push_back(args);
        HANDLE h = startCmd(L"ping -n 2 127.0.0.1 >nul");
        if (h) {
            // The restarted "viewer" stops cleanly.
            ev_second = CreateEventW(nullptr, TRUE, FALSE, names.cleanStop(GetProcessId(h)).c_str());
            SetEvent(ev_second);
        }
        return h;
    });
    const WatchdogEnd e = runWatchdog(killed, names, io, fast());
    KV_CHECK_MSG(e == WatchdogEnd::StoodDown, log.all());
    KV_REQUIRE(asked.size() == 1);
    KV_CHECK_MSG(asked[0] == L"--mode ir --no-autostart", std::string(asked[0].begin(), asked[0].end()));
    KV_CHECK_MSG(log.has("without a clean stop (exit code 3 (0x00000003))") && log.has("with the sensor off as it was"), log.all());
    KV_CHECK_MSG(log.has("started the viewer again") && log.has("stopped cleanly"), log.all());
    if (ev_second) CloseHandle(ev_second);
}

KV_TEST(a_crash_with_outside_viewing_off_is_never_started_again) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    HANDLE v = startCmd(L"ping -n 2 127.0.0.1 >nul & exit 5");
    KV_REQUIRE(v != nullptr);
    KeepRunning kr(names, GetProcessId(v), L"", false, [] { return true; });
    kr.set(true);
    kr.set(false); // the person turned outside viewing off; then the viewer died
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return nullptr;
    });
    KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::StoodDown);
    KV_CHECK(relaunched == 0);
    KV_CHECK_MSG(log.has("stopped cleanly"), log.all());
}

KV_TEST(the_viewers_own_end_is_a_clean_stop_even_with_outside_viewing_on) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    int relaunched = 0;
    Recorder log;
    HANDLE v = startCmd(L"ping -n 2 127.0.0.1 >nul");
    KV_REQUIRE(v != nullptr);
    auto kr = std::make_unique<KeepRunning>(names, GetProcessId(v), L"", false, [] { return true; });
    kr->set(true);
    // The clean-stop event is kept here too, so the mark outlives the hooks whatever the watchdog's timing (a real
    // viewer's handle lives until its process ends, after the mark).
    HANDLE keepClean = CreateEventW(nullptr, TRUE, FALSE, names.cleanStop(GetProcessId(v)).c_str());
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return nullptr;
    });
    std::thread t([&] { KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::StoodDown); });
    // Once it watches (as it does long before a real viewer ends), the viewer's hooks go away on a clean exit.
    for (int i = 0; i < 500; ++i) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, names.watch(GetProcessId(v)).c_str());
        if (m) {
            CloseHandle(m);
            break;
        }
        Sleep(10);
    }
    kr.reset();
    t.join();
    CloseHandle(keepClean);
    KV_CHECK(relaunched == 0);
}

// install.ps1 tells the watchdog to stand down before it copies: a viewer killed for the install is not started
// again under the copy.
KV_TEST(a_watchdog_told_to_stand_down_starts_nothing) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    HANDLE v = startCmd(L"ping -n 30 127.0.0.1 >nul");
    KV_REQUIRE(v != nullptr);
    KeepRunning kr(names, GetProcessId(v), L"", false, [] { return true; });
    kr.set(true);
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return nullptr;
    });
    std::atomic<int> end{-1};
    std::thread t([&] { end = static_cast<int>(runWatchdog(v, names, io, fast())); });
    HANDLE stand = nullptr;
    for (int i = 0; i < 500 && !stand; ++i) {
        stand = OpenEventW(EVENT_MODIFY_STATE, FALSE, names.standDown(GetCurrentProcessId()).c_str());
        if (!stand) Sleep(10);
    }
    KV_REQUIRE(stand != nullptr);
    SetEvent(stand);
    t.join();
    CloseHandle(stand);
    KV_CHECK(end == static_cast<int>(WatchdogEnd::ToldToStandDown));
    KV_CHECK(relaunched == 0);
    KV_CHECK_MSG(log.has("told to stand down"), log.all());
    // The viewer is left as it is: still running, ended here by the test that started it.
    HANDLE v2 = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, GetProcessId(v));
    if (v2) {
        TerminateProcess(v2, 0);
        CloseHandle(v2);
    }
}

KV_TEST(it_gives_up_after_too_many_restarts) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    // Each "viewer" crashes (exit 3) after keeping its record, as a real one does.
    std::vector<std::unique_ptr<KeepRunning>> records;
    auto crashing = [&]() -> HANDLE {
        HANDLE h = startCmd(L"ping -n 1 127.0.0.1 >nul & exit 3");
        if (h) records.push_back(std::make_unique<KeepRunning>(names, GetProcessId(h), L"", false, nullptr));
        return h;
    };
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return crashing();
    });
    HANDLE v = crashing();
    KV_REQUIRE(v != nullptr);
    KV_CHECK(runWatchdog(v, names, io, fast(3)) == WatchdogEnd::GaveUp);
    KV_CHECK(relaunched == 3);
    KV_CHECK_MSG(log.has("giving up"), log.all());
}

KV_TEST(it_never_starts_a_second_viewer) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return nullptr;
    });
    // Another viewer (the person started one by hand) is running by the time the wait is over.
    HANDLE other = CreateMutexW(nullptr, FALSE, names.running().c_str());
    HANDLE v = startCmd(L"ping -n 1 127.0.0.1 >nul & exit 3");
    KV_REQUIRE(v != nullptr && other != nullptr);
    KeepRunning record(names, GetProcessId(v), L"", false, nullptr);
    KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::StoodDown);
    KV_CHECK(relaunched == 0);
    KV_CHECK_MSG(log.has("started meanwhile"), log.all());
    CloseHandle(other);
}

KV_TEST(one_watchdog_per_viewer) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    HANDLE v = startCmd(L"ping -n 3 127.0.0.1 >nul");
    KV_REQUIRE(v != nullptr);
    const DWORD pid = GetProcessId(v);
    int spawned = 0;
    KeepRunning kr(names, pid, L"", false, [&] {
        ++spawned;
        return true;
    });
    kr.set(true);
    KV_CHECK(spawned == 1); // none watched it yet
    HANDLE held = CreateMutexW(nullptr, TRUE, names.watch(pid).c_str()); // a watchdog now watches it
    kr.set(false);
    kr.set(true);
    KV_CHECK(spawned == 1);
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), v, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    Recorder log;
    auto io = ioFor(log, [](const std::wstring&) -> HANDLE { return nullptr; });
    KV_CHECK(runWatchdog(dup, names, io, fast()) == WatchdogEnd::AnotherIsWatching);
    ReleaseMutex(held);
    CloseHandle(held);
    kr.set(false);
    WaitForSingleObject(v, 10000);
    CloseHandle(v);
}

// With a watchdog watching, the fault is written from the watchdog's process (the dump read out of
// the faulted viewer's memory), the viewer ends itself after, and it is started again.
KV_TEST(a_watched_fault_is_dumped_from_outside_and_the_viewer_started_again) {
    const std::wstring suffix = keepRunningTestSuffix();
    const KeepRunningNames names = keepRunningNames(suffix);
    const std::wstring dir = tempDir(L"outside");
    std::vector<HANDLE> held;
    Recorder log;
    int relaunched = 0;
    auto io = ioFor(
        log,
        [&](const std::wstring&) -> HANDLE {
            ++relaunched;
            return cleanlyStopping(names, held);
        },
        [&](HANDLE p, DWORD pid, const FaultRecord& f) { return writeFaultDump(dir, p, pid, f); });
    HANDLE child = startSelf(L"--crash-watched \"" + dir + L"\" 0 " + suffix);
    KV_REQUIRE(child != nullptr);
    HANDLE mine = nullptr;
    DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS);
    const uint64_t t0 = nowMs();
    runWatchdog(child, names, io, fast());
    DWORD code = 0;
    GetExitCodeProcess(mine, &code);
    CloseHandle(mine);
    const std::string line = readText(dir + L"\\crash.log");
    KV_CHECK_MSG(code == 0xC0000005, std::to_string(code));
    KV_CHECK_MSG(line.find("unhandled exception 0xC0000005 in test_keep_running.exe+0x") != std::string::npos &&
                     line.find("; dump crash-") != std::string::npos, line);
    KV_CHECK(countFiles(dir, L"*.dmp") == 1);
    KV_CHECK_MSG(log.has("the viewer faulted (unhandled exception 0xC0000005") && log.has("; dump crash-"), log.all());
    KV_CHECK(relaunched == 1);
    KV_CHECK(nowMs() - t0 < 20000);
    for (HANDLE h : held) CloseHandle(h);
    removeDir(dir);
}

// No fault turns into a hang: a dump that never finishes is given up, and a faulted viewer that doesn't end is
// ended; either way it is started again.
KV_TEST(a_recorder_that_never_finishes_still_ends_with_a_restart) {
    for (int variant = 0; variant < 2; ++variant) {
        const std::wstring suffix = keepRunningTestSuffix();
        const KeepRunningNames names = keepRunningNames(suffix);
        std::vector<HANDLE> held;
        Recorder log;
        int relaunched = 0;
        RestartPolicy p = fast();
        p.dumpTimeoutMs = 500;
        p.endGraceMs = 500;
        auto io = ioFor(
            log,
            [&](const std::wstring&) -> HANDLE {
                ++relaunched;
                return cleanlyStopping(names, held);
            },
            [variant](HANDLE, DWORD, const FaultRecord&) {
                if (variant == 0) Sleep(30000); // a dump that doesn't finish
                return std::string("crash-test.dmp");
            });
        // Variant 0: the child's recorder waits for the watchdog as a viewer's does. Variant 1: the child records the
        // fault and then never ends (a recorder stuck in the faulting process).
        HANDLE child = startSelf((variant == 0 ? L"--crash-watched \"" + tempDir(L"stuck") + L"\" 0 " : L"--fault-and-hang ") + suffix);
        KV_REQUIRE(child != nullptr);
        HANDLE mine = nullptr;
        DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS);
        const uint64_t t0 = nowMs();
        runWatchdog(child, names, io, p);
        const uint64_t took = nowMs() - t0;
        const bool ended = WaitForSingleObject(mine, 0) == WAIT_OBJECT_0;
        CloseHandle(mine);
        KV_CHECK_MSG(ended, log.all());
        KV_CHECK(relaunched == 1);
        KV_CHECK_MSG(took < 15000, std::to_string(took));
        if (variant == 0) KV_CHECK_MSG(log.has("its dump didn't finish"), log.all());
        else KV_CHECK_MSG(log.has("it didn't end by itself and was ended"), log.all());
        for (HANDLE h : held) CloseHandle(h);
    }
}

// With no watchdog, every kind of fault writes its dump and its line from inside, then the process ends itself at
// once with the fault's code: no debugger, no dialog, no hang. Each kind in a crash folder of its own, so one kind's
// line can't stand for another's; then at most three dumps in one folder.
KV_TEST(a_fault_leaves_a_dump_and_a_line_and_the_process_ends_itself) {
    struct Kind {
        TestFault f;
        DWORD code;
        const char* line;
    };
    const Kind kinds[] = {
        {TestFault::AccessViolation, 0xC0000005, "unhandled exception 0xC0000005 in test_keep_running.exe+0x"},
        // MSVC: a thread started after install has the default terminate handler, so it arrives as abort().
        {TestFault::UncaughtException, 3, "abort() from std::terminate with an uncaught class std::runtime_error"},
        {TestFault::Terminate, 3, "std::terminate with no exception"},
        {TestFault::Abort, 3, ": abort(); dump crash-"},
        {TestFault::PureCall, 3, "a pure virtual call"},
    };
    for (const auto& k : kinds) {
        const std::wstring dir = tempDir(L"kind");
        uint64_t took = 0;
        const DWORD code = crashChild(dir, static_cast<int>(k.f), &took);
        char msg[96];
        std::snprintf(msg, sizeof msg, "kind %d exit 0x%08lX in %llu ms", static_cast<int>(k.f), static_cast<unsigned long>(code),
                      static_cast<unsigned long long>(took));
        KV_CHECK_MSG(code == k.code, msg);
        KV_CHECK_MSG(took < 20000, msg);
        const std::string log = readText(dir + L"\\crash.log");
        KV_CHECK_MSG(log.find(k.line) != std::string::npos, log);
        KV_CHECK_MSG(std::count(log.begin(), log.end(), '\n') == 1, log); // this kind's line alone
        KV_CHECK_MSG(countFiles(dir, L"*.dmp") == 1, msg);
        KV_CHECK(log.find("synthetic") == std::string::npos); // the exception's type, never its message
        removeDir(dir);
    }
    const std::wstring dir = tempDir(L"cap");
    for (int i = 0; i < 5; ++i) crashChild(dir, static_cast<int>(TestFault::AccessViolation));
    KV_CHECK(countFiles(dir, L"*.dmp") == kCrashDumpsKept);
    KV_CHECK(readText(dir + L"\\crash.log").find("no dump could be written") == std::string::npos);
    removeDir(dir);
}

// The sensor's choice is never lost. A restarted viewer that dies before filling its
// own block changes nothing the watchdog knew, and a --no-autostart from the original command line stays.
KV_TEST(a_restarted_viewer_that_dies_before_its_record_is_not_restarted_and_the_restart_kept_the_sensor_off) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    std::vector<std::wstring> asked;
    std::vector<HANDLE> held;
    HANDLE first = startCmd(L"ping -n 2 127.0.0.1 >nul & exit 3");
    KV_REQUIRE(first != nullptr);
    KeepRunning kr(names, GetProcessId(first), L"--no-autostart --mode ir", true, [] { return true; });
    kr.set(true);
    auto io = ioFor(log, [&](const std::wstring& args) -> HANDLE {
        asked.push_back(args);
        // The restart dies at once, before it could keep a record of its own: that one isn't started again.
        return asked.size() == 1 ? startCmd(L"exit 3") : cleanlyStopping(names, held);
    });
    KV_CHECK(runWatchdog(first, names, io, fast()) == WatchdogEnd::StoodDown);
    KV_REQUIRE(asked.size() == 1);
    KV_CHECK_MSG(asked[0] == L"--mode ir --no-autostart", std::string(asked[0].begin(), asked[0].end()));
    KV_CHECK_MSG(log.has("with no record of its own read"), log.all());
    for (HANDLE h : held) CloseHandle(h);
}

// Clean is decided by the clean-stop mark alone: a SIGNALLED fault after the person turned outside viewing off (or
// closed the window) is recorded, and never restarted.
KV_TEST(a_signalled_fault_after_a_clean_stop_is_recorded_and_never_restarted) {
    const std::wstring suffix = keepRunningTestSuffix();
    const KeepRunningNames names = keepRunningNames(suffix);
    const std::wstring dir = tempDir(L"after-clean");
    Recorder log;
    int relaunched = 0;
    auto io = ioFor(
        log,
        [&](const std::wstring&) -> HANDLE {
            ++relaunched;
            return nullptr;
        },
        [&](HANDLE p, DWORD pid, const FaultRecord& f) { return writeFaultDump(dir, p, pid, f); });
    HANDLE child = startSelf(L"--crash-after-clean-stop \"" + dir + L"\" " + suffix);
    KV_REQUIRE(child != nullptr);
    KV_CHECK(runWatchdog(child, names, io, fast()) == WatchdogEnd::StoodDown);
    KV_CHECK(relaunched == 0);
    KV_CHECK_MSG(log.has("the viewer faulted (unhandled exception 0xC0000005") && log.has("then faulted while closing"), log.all());
    KV_CHECK(countFiles(dir, L"*.dmp") == 1);
    removeDir(dir);
}

// A fault signal without the block's mark is not a fault.
KV_TEST(a_fault_signal_without_the_mark_is_ignored) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    int wrote = 0;
    HANDLE v = startCmd(L"ping -n 2 127.0.0.1 >nul");
    KV_REQUIRE(v != nullptr);
    KeepRunning kr(names, GetProcessId(v), L"", false, [] { return true; });
    kr.set(true);
    auto io = ioFor(
        log,
        [&](const std::wstring&) -> HANDLE {
            ++relaunched;
            return nullptr;
        },
        [&](HANDLE, DWORD, const FaultRecord&) {
            ++wrote;
            return std::string();
        });
    std::thread t([&] { KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::StoodDown); });
    for (int i = 0; i < 500 && !OpenMutexW(SYNCHRONIZE, FALSE, names.watch(GetProcessId(v)).c_str()); ++i) Sleep(10);
    SetEvent(kr.faultEvent()); // no mark in the block
    Sleep(200);
    kr.set(false);
    t.join();
    KV_CHECK(wrote == 0 && relaunched == 0);
    KV_CHECK_MSG(!log.has("faulted"), log.all());
}

// A command line too long to keep whole is refused, never cut: no restart from half of it.
KV_TEST(a_command_line_too_long_to_keep_is_refused_not_cut) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    HANDLE v = startCmd(L"exit 3");
    KV_REQUIRE(v != nullptr);
    const std::wstring longArgs = L"--state-dir " + std::wstring(kKeepRunningArgsMax, L'x');
    KeepRunning kr(names, GetProcessId(v), longArgs, false, [] { return true; });
    kr.set(true);
    KV_CHECK(kr.block()->argsRefused == 1 && kr.block()->args[0] == L'\0');
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return nullptr;
    });
    KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::GaveUp);
    KV_CHECK(relaunched == 0);
    KV_CHECK_MSG(log.has("too long to keep whole"), log.all());
    // Exactly the longest kept is kept whole.
    HANDLE w = startCmd(L"exit 0");
    KeepRunning fits(keepRunningNames(keepRunningTestSuffix()), GetProcessId(w), std::wstring(kKeepRunningArgsMax, L'y'), false, nullptr);
    KV_CHECK(fits.block()->argsRefused == 0 && std::wstring(fits.block()->args).size() == kKeepRunningArgsMax);
    CloseHandle(w);
}

// A restart runs from the exe's folder: --state-dir's and --camera-roll-dir's folders are made absolute when kept.
KV_TEST(the_kept_folders_are_absolute) {
    wchar_t full[MAX_PATH];
    GetFullPathNameW(L"rel\\state", MAX_PATH, full, nullptr);
    wchar_t roll[MAX_PATH];
    GetFullPathNameW(L"roll", MAX_PATH, roll, nullptr);
    const std::wstring got = launchArgumentsOf(L"kinect-viewer.exe --state-dir rel\\state --mode ir --camera-roll-dir roll");
    const std::wstring want = L"--state-dir " + std::wstring(full) + L" --mode ir --camera-roll-dir " + std::wstring(roll);
    KV_CHECK_MSG(got == want, std::string(got.begin(), got.end()));
    KV_CHECK(launchArgumentsOf(L"kinect-viewer.exe") == L"");
}

// A second thread faulting while the first records waits for it: one line, one dump, the first fault's code.
KV_TEST(a_second_fault_waits_for_the_first_record) {
    const std::wstring dir = tempDir(L"two");
    HANDLE h = startSelf(L"--two-faults \"" + dir + L"\"");
    KV_REQUIRE(h != nullptr);
    WaitForSingleObject(h, 90000);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    const std::string log = readText(dir + L"\\crash.log");
    KV_CHECK_MSG(code == 0xC0000005, std::to_string(code));
    KV_CHECK_MSG(std::count(log.begin(), log.end(), '\n') == 1 && log.find("; dump crash-") != std::string::npos, log);
    KV_CHECK(countFiles(dir, L"*.dmp") == 1);
    removeDir(dir);
}

// A viewer ENDED ON PURPOSE (End task, taskkill: exit code 1, no fault recorded) is
// never started again, whether it ended by itself or was ended from outside.
KV_TEST(a_viewer_ended_on_purpose_is_never_started_again) {
    for (int how = 0; how < 2; ++how) {
        const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
        Recorder log;
        int relaunched = 0;
        HANDLE v = startCmd(how == 0 ? L"ping -n 2 127.0.0.1 >nul & exit 1" : L"ping -n 30 127.0.0.1 >nul");
        KV_REQUIRE(v != nullptr);
        KeepRunning kr(names, GetProcessId(v), L"", false, [] { return true; });
        kr.set(true);
        auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
            ++relaunched;
            return nullptr;
        });
        std::thread t([&] { KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::StoodDown); });
        if (how == 1) {
            // Task Manager's End task: TerminateProcess with 1, from outside.
            for (int i = 0; i < 500 && !OpenMutexW(SYNCHRONIZE, FALSE, names.watch(GetProcessId(v)).c_str()); ++i) Sleep(10);
            HANDLE k = OpenProcess(PROCESS_TERMINATE, FALSE, GetProcessId(v));
            if (k) {
                TerminateProcess(k, 1);
                CloseHandle(k);
            }
        }
        t.join();
        KV_CHECK(relaunched == 0);
        KV_CHECK_MSG(log.has("not a crash code, not started again"), log.all());
    }
}

// Never a restart unless a filled record was read from that very process: no block at all, or one never filled.
KV_TEST(a_viewer_with_no_record_of_its_own_is_never_started_again) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    HANDLE v = startCmd(L"ping -n 1 127.0.0.1 >nul & exit 3"); // a crash's code, but nothing kept for it
    KV_REQUIRE(v != nullptr);
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return nullptr;
    });
    KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::StoodDown);
    KV_CHECK(relaunched == 0);
    KV_CHECK_MSG(log.has("with no record of its own read"), log.all());
}

// A viewer started by hand during the restart's wait (even one that has closed again: the person started it, turned
// the sensor off and closed it) means the watchdog stands down.
KV_TEST(a_viewer_started_during_the_wait_makes_it_stand_down) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    Recorder log;
    int relaunched = 0;
    HANDLE v = startCmd(L"ping -n 1 127.0.0.1 >nul & exit 3");
    KV_REQUIRE(v != nullptr);
    KeepRunning record(names, GetProcessId(v), L"", false, nullptr);
    auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
        ++relaunched;
        return nullptr;
    });
    RestartPolicy p = fast();
    p.delaysMs = {1500};
    std::thread t([&] { KV_CHECK(runWatchdog(v, names, io, p) == WatchdogEnd::StoodDown); });
    // Once the viewer has ended and the wait has begun: a viewer of this process starts and closes again.
    for (int i = 0; i < 500 && !log.has("starting it again in"); ++i) Sleep(10);
    {
        KeepRunning byHand(names, GetCurrentProcessId(), L"", true, nullptr);
    }
    t.join();
    KV_CHECK(relaunched == 0);
    KV_CHECK_MSG(log.has("started meanwhile"), log.all());
}

// The window's close handler marks the stop first and for good: nothing in the teardown can take it back.
KV_TEST(a_close_marks_the_stop_for_good) {
    const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
    KeepRunning kr(names, GetCurrentProcessId(), L"", false, nullptr);
    kr.set(true);
    HANDLE clean = OpenEventW(SYNCHRONIZE, FALSE, names.cleanStop(GetCurrentProcessId()).c_str());
    KV_REQUIRE(clean != nullptr);
    KV_CHECK(WaitForSingleObject(clean, 0) == WAIT_TIMEOUT);
    kr.stopForGood();
    kr.set(true); // outside listening reopened during the teardown: ignored
    KV_CHECK(WaitForSingleObject(clean, 0) == WAIT_OBJECT_0);
    CloseHandle(clean);
}

// A viewer ended with 0xCFFFFFFF (a hung app the person ended with "close the program"), 0xC000013A
// (Ctrl+C, a console closed), 0xC0000142 or 0xC0000135 (it could not load: a restart would loop) or 0x40010004 (a
// debugger) is never started again, and the log says the code.
KV_TEST(a_viewer_closed_as_hung_by_ctrl_c_or_that_cant_load_is_never_started_again) {
    for (const wchar_t* how : {L"ping -n 1 127.0.0.1 >nul & exit -805306369", L"ping -n 1 127.0.0.1 >nul & exit -1073741510",
                             L"ping -n 1 127.0.0.1 >nul & exit -1073741502", L"ping -n 1 127.0.0.1 >nul & exit -1073741515",
                             L"ping -n 1 127.0.0.1 >nul & exit 1073807364"}) {
        const KeepRunningNames names = keepRunningNames(keepRunningTestSuffix());
        Recorder log;
        int relaunched = 0;
        HANDLE v = startCmd(how);
        KV_REQUIRE(v != nullptr);
        KeepRunning record(names, GetProcessId(v), L"", false, nullptr);
        auto io = ioFor(log, [&](const std::wstring&) -> HANDLE {
            ++relaunched;
            return nullptr;
        });
        KV_CHECK(runWatchdog(v, names, io, fast()) == WatchdogEnd::StoodDown);
        KV_CHECK(relaunched == 0);
        KV_CHECK_MSG(log.has("not a crash code, not started again") && (log.has("0xCFFFFFFF") || log.has("0xC000013A") || log.has("0xC0000142") || log.has("0xC0000135") || log.has("0x40010004")), log.all());
    }
}
// Crash exit codes, and the ones that aren't.
KV_TEST(crash_exit_codes_are_told_from_ended_on_purpose) {
    for (DWORD c : {3u, 0xC0000005u, 0xC0000409u, 0xC0000374u, 0xC00000FDu, 0xC000001Du, 0xC0000094u, 0xC0000095u, 0xC0000096u,
                    0xC0000420u, 0xC0000602u, 0xC000041Du, 0xC0000006u})
        KV_CHECK(isCrashExit(c));
    // A hung app the person ended ("close the program"), Ctrl+C or a closed console, End task, a clean exit: not crashes.
    for (DWORD c : {0u, 1u, 2u, 4u, 0xCFFFFFFFu, 0xC000013Au, 0xC0000001u, 0x40010004u, 0x80000003u, 0xC0000142u, 0xC0000135u}) KV_CHECK(!isCrashExit(c));
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--crash-child") {
        const std::string d = argv[2];
        installCrashRecorder(std::wstring(d.begin(), d.end()));
        raiseTestFault(static_cast<TestFault>(std::atoi(argv[3])));
    }
    if (argc >= 5 && std::string(argv[1]) == "--crash-watched") {
        // A viewer as a real launch makes it: its KeepRunning (this run's names), the recorder; then, once its watchdog
        // watches, the fault.
        const std::string d = argv[2], s = argv[4];
        const KeepRunningNames names = keepRunningNames(std::wstring(s.begin(), s.end()));
        static KeepRunning kr(names, GetCurrentProcessId(), L"--mode ir", false, nullptr);
        installCrashRecorder(std::wstring(d.begin(), d.end()));
        for (int i = 0; i < 1000 && !OpenMutexW(SYNCHRONIZE, FALSE, names.watch(GetCurrentProcessId()).c_str()); ++i) Sleep(10);
        raiseTestFault(static_cast<TestFault>(std::atoi(argv[3])));
    }
    if (argc >= 4 && std::string(argv[1]) == "--crash-after-clean-stop") {
        // Outside viewing on, a watchdog watching, then turned off (a clean stop), then a fault while closing.
        const std::string d = argv[2], s = argv[3];
        const KeepRunningNames names = keepRunningNames(std::wstring(s.begin(), s.end()));
        static KeepRunning kr(names, GetCurrentProcessId(), L"", false, nullptr);
        installCrashRecorder(std::wstring(d.begin(), d.end()));
        for (int i = 0; i < 1000 && !OpenMutexW(SYNCHRONIZE, FALSE, names.watch(GetCurrentProcessId()).c_str()); ++i) Sleep(10);
        kr.set(true);
        kr.set(false);
        raiseTestFault(TestFault::AccessViolation);
    }
    if (argc >= 3 && std::string(argv[1]) == "--two-faults") {
        // No watchdog: two threads fault, the second while the first records.
        const std::string d = argv[2];
        installCrashRecorder(std::wstring(d.begin(), d.end()));
        std::thread([] {
            Sleep(30);
            raiseTestFault(TestFault::AccessViolation);
        }).detach();
        raiseTestFault(TestFault::AccessViolation);
    }
    if (argc >= 3 && std::string(argv[1]) == "--fault-and-hang") {
        // A fault recorded in the block and signalled, and then a process that never ends.
        const std::string s = argv[2];
        const KeepRunningNames names = keepRunningNames(std::wstring(s.begin(), s.end()));
        static KeepRunning kr(names, GetCurrentProcessId(), L"", false, nullptr);
        for (int i = 0; i < 1000 && !OpenMutexW(SYNCHRONIZE, FALSE, names.watch(GetCurrentProcessId()).c_str()); ++i) Sleep(10);
        KeepRunningBlock* b = kr.block();
        b->thread = GetCurrentThreadId();
        b->code = 0xC0000005;
        InterlockedExchange(&b->faulted, 1);
        std::snprintf(b->what, sizeof b->what, "a fault whose recorder never finishes");
        HANDLE fault = OpenEventW(EVENT_MODIFY_STATE, FALSE, names.fault(GetCurrentProcessId()).c_str());
        if (fault) SetEvent(fault);
        for (;;) Sleep(INFINITE);
    }
    return ::kvtest::runAll(argc, argv);
}
