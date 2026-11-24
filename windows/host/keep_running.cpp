#include "keep_running.hpp"

#include "crash_recorder.hpp"

// dbghelp after windows.h
#include <dbghelp.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

namespace kvhost {

namespace {

std::atomic<KeepRunning*> g_this_process{nullptr};

std::string exitText(DWORD code) {
    char b[64];
    std::snprintf(b, sizeof b, "exit code %lu (0x%08lX)", static_cast<unsigned long>(code), static_cast<unsigned long>(code));
    return b;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

std::wstring exePath() {
    std::wstring p(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
        if (n == 0) return {};
        if (n < p.size()) {
            p.resize(n);
            return p;
        }
        p.resize(p.size() * 2);
    }
}

bool exists(const std::wstring& mutexName) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str());
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// One argument quoted the way CommandLineToArgvW reads it back.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == a.size()) {
            q.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            q.append(slashes * 2 + 1, L'\\');
            q += L'"';
        } else {
            q.append(slashes, L'\\');
            q += a[i];
        }
    }
    return q + L"\"";
}

// A block mapped for this process: made if no one has made it yet.
KeepRunningBlock* mapBlock(const std::wstring& name, HANDLE* mapping) {
    *mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(KeepRunningBlock), name.c_str());
    if (!*mapping) return nullptr;
    void* v = MapViewOfFile(*mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(KeepRunningBlock));
    if (!v) {
        CloseHandle(*mapping);
        *mapping = nullptr;
    }
    return static_cast<KeepRunningBlock*>(v);
}

} // namespace

std::wstring KeepRunningNames::watch(DWORD pid) const { return prefix + L"-Watch-" + std::to_wstring(pid); }
std::wstring KeepRunningNames::cleanStop(DWORD pid) const { return prefix + L"-CleanStop-" + std::to_wstring(pid); }
std::wstring KeepRunningNames::block(DWORD pid) const { return prefix + L"-Block-" + std::to_wstring(pid); }
std::wstring KeepRunningNames::fault(DWORD pid) const { return prefix + L"-Fault-" + std::to_wstring(pid); }
std::wstring KeepRunningNames::faultDone(DWORD pid) const { return prefix + L"-FaultDone-" + std::to_wstring(pid); }
std::wstring KeepRunningNames::standDown(DWORD pid) const { return prefix + L"-StandDown-" + std::to_wstring(pid); }
std::wstring KeepRunningNames::running() const { return prefix + L"-Running"; }
std::wstring KeepRunningNames::starts() const { return prefix + L"-Starts"; }

namespace {
// The shared count of real viewers started: read by a watchdog before and after its wait.
volatile LONG* mapStarts(const KeepRunningNames& names, HANDLE* mapping) {
    *mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), names.starts().c_str());
    if (!*mapping) return nullptr;
    void* v = MapViewOfFile(*mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));
    if (!v) {
        CloseHandle(*mapping);
        *mapping = nullptr;
    }
    return static_cast<volatile LONG*>(v);
}
} // namespace

KeepRunningNames keepRunningNames(const std::wstring& suffix) {
    return KeepRunningNames{L"Local\\KinectViewer-KeepRunning" + suffix};
}

std::wstring keepRunningTestSuffix() {
    static std::atomic<int> n{0};
    return L"-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n);
}

std::wstring relaunchArguments(const std::wstring& launchArgs, bool sensorOff) {
    std::wstring out;
    if (!launchArgs.empty()) {
        // CommandLineToArgvW reads the first token as the program: give it one.
        const std::wstring line = L"x " + launchArgs;
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(line.c_str(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--no-autostart") continue;
            if (!out.empty()) out += L' ';
            out += quoteArg(argv[i]);
        }
        if (argv) LocalFree(argv);
    }
    if (sensorOff) out += out.empty() ? L"--no-autostart" : L" --no-autostart";
    return out;
}

std::wstring ownArguments() { return launchArgumentsOf(GetCommandLineW()); }

std::wstring launchArgumentsOf(const wchar_t* commandLine) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(commandLine, &argc);
    std::wstring out;
    bool folderNext = false;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (folderNext) {
            // A restart runs from the exe's folder, so a relative folder is made absolute now.
            wchar_t full[MAX_PATH * 4];
            const DWORD n = GetFullPathNameW(a.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
            if (n > 0 && n < std::size(full)) a = full;
        }
        folderNext = a == L"--state-dir" || a == L"--camera-roll-dir";
        if (!out.empty()) out += L' ';
        out += quoteArg(a);
    }
    if (argv) LocalFree(argv);
    return out;
}

std::optional<uint64_t> nextRestartDelayMs(const std::vector<uint64_t>& restartsMs, uint64_t nowMs,
                                           const RestartPolicy& p) {
    int recent = 0;
    for (uint64_t t : restartsMs)
        if (nowMs < t + p.windowMs) ++recent;
    if (recent >= p.maxRestarts) return std::nullopt;
    if (p.delaysMs.empty()) return 0;
    return p.delaysMs[std::min<size_t>(static_cast<size_t>(recent), p.delaysMs.size() - 1)];
}

KeepRunning::KeepRunning(KeepRunningNames names, DWORD pid, const std::wstring& launchArgs, bool sensorOff, Spawn spawn)
    : names_(std::move(names)), pid_(pid), spawn_(std::move(spawn)) {
    watch_name_ = names_.watch(pid_);
    clean_stop_ = CreateEventW(nullptr, TRUE, FALSE, names_.cleanStop(pid_).c_str());
    // "A viewer is running" is held by the viewer process itself (a test may make one for a child it started), and
    // each real viewer adds one to the shared starts count, so a watchdog waiting to restart sees it was started.
    if (pid_ == GetCurrentProcessId()) {
        running_ = CreateMutexW(nullptr, FALSE, names_.running().c_str());
        HANDLE m = nullptr;
        if (volatile LONG* n = mapStarts(names_, &m)) {
            InterlockedIncrement(n);
            UnmapViewOfFile(const_cast<LONG*>(n));
        }
        starts_mapping_ = m; // kept open: the count lives while any viewer or watchdog holds it
    }
    fault_ = CreateEventW(nullptr, FALSE, FALSE, names_.fault(pid_).c_str());
    fault_done_ = CreateEventW(nullptr, TRUE, FALSE, names_.faultDone(pid_).c_str());
    block_ = mapBlock(names_.block(pid_), &mapping_);
    if (block_) {
        // Whole or not at all: a command line cut short could start the viewer as it never was launched.
        if (launchArgs.size() <= kKeepRunningArgsMax) {
            std::memcpy(block_->args, launchArgs.data(), launchArgs.size() * sizeof(wchar_t));
            block_->args[launchArgs.size()] = L'\0';
        } else {
            block_->args[0] = L'\0';
            InterlockedExchange(&block_->argsRefused, 1);
        }
        InterlockedExchange(&block_->sensorOff, sensorOff ? 1 : 0);
        InterlockedExchange(&block_->filled, 1); // last: the watchdog trusts the rest from here
    }
    if (pid_ == GetCurrentProcessId()) g_this_process = this;
}

KeepRunning::~KeepRunning() {
    KeepRunning* self = this;
    g_this_process.compare_exchange_strong(self, nullptr);
    if (clean_stop_) {
        SetEvent(clean_stop_);
        CloseHandle(clean_stop_);
    }
    if (running_) CloseHandle(running_);
    if (starts_mapping_) CloseHandle(starts_mapping_);
    if (fault_) CloseHandle(fault_);
    if (fault_done_) CloseHandle(fault_done_);
    if (block_) UnmapViewOfFile(block_);
    if (mapping_) CloseHandle(mapping_);
}

void KeepRunning::stopForGood() {
    stopped_for_good_ = true;
    if (clean_stop_) SetEvent(clean_stop_);
}

void KeepRunning::set(bool on) {
    if (!clean_stop_) return; // best effort: without the event a watchdog could never tell a clean stop
    if (!on || stopped_for_good_) {
        SetEvent(clean_stop_);
        return;
    }
    ResetEvent(clean_stop_);
    if (!exists(names_.watch(pid_)) && spawn_) spawn_();
}

void KeepRunning::setSensorOff(bool off) {
    if (block_) InterlockedExchange(&block_->sensorOff, off ? 1 : 0);
}

KeepRunning* keepRunningOfThisProcess() { return g_this_process.load(); }

std::string writeFaultDump(const std::wstring& dir, HANDLE proc, DWORD pid, const FaultRecord& f) {
    CreateDirectoryW(dir.c_str(), nullptr);
    pruneCrashDumps(dir);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             static_cast<unsigned long>(pid));
    const std::wstring name = std::wstring(L"crash-") + stamp + L".dmp";
    bool dumped = false;
    HANDLE file = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = f.thread;
        mei.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(static_cast<uintptr_t>(f.exceptionPointers));
        mei.ClientPointers = TRUE; // the pointers are in the viewer's memory
        dumped = MiniDumpWriteDump(proc, pid, file, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
                                   f.exceptionPointers ? &mei : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(file);
        if (!dumped) DeleteFileW((dir + L"\\" + name).c_str());
    }
    appendCrashLine(dir, pid, f.thread, f.what, dumped ? narrow(name) : std::string());
    return dumped ? narrow(name) : std::string();
}

WatchdogEnd runWatchdog(HANDLE proc, const KeepRunningNames& names, const WatchdogIo& io, const RestartPolicy& p) {
    std::vector<uint64_t> restarts;
    // What the watchdog knows of the launch, from the first viewer's filled block, and of the sensor, from the last
    // filled block it read (a restarted viewer that dies before filling its own changes nothing).
    std::wstring launchArgs;
    bool haveArgs = false;
    bool argsRefused = false;
    bool sensorOff = false;
    // The shared count of real viewers started, to see one started by hand during a restart's wait.
    HANDLE startsMapping = nullptr;
    volatile LONG* starts = mapStarts(names, &startsMapping);
    struct MapCloser {
        HANDLE h;
        volatile LONG* v;
        ~MapCloser() {
            if (v) UnmapViewOfFile(const_cast<LONG*>(v));
            if (h) CloseHandle(h);
        }
    } startsCloser{startsMapping, starts};
    HANDLE stand = CreateEventW(nullptr, TRUE, FALSE, names.standDown(GetCurrentProcessId()).c_str());
    struct Closer {
        HANDLE h;
        ~Closer() {
            if (h) CloseHandle(h);
        }
    } standCloser{stand};
    bool first = true;
    for (;;) {
        const DWORD pid = GetProcessId(proc);
        HANDLE watch = CreateMutexW(nullptr, TRUE, names.watch(pid).c_str());
        if (!watch || GetLastError() == ERROR_ALREADY_EXISTS) {
            // The viewer started its own watchdog first (after a restart), or this one was started twice.
            if (watch) CloseHandle(watch);
            CloseHandle(proc);
            if (first) return watch ? WatchdogEnd::AnotherIsWatching : WatchdogEnd::CantStart;
            io.log("another watchdog watches the restarted viewer; standing down");
            return WatchdogEnd::AnotherIsWatching;
        }
        HANDLE clean = CreateEventW(nullptr, TRUE, FALSE, names.cleanStop(pid).c_str());
        HANDLE fault = CreateEventW(nullptr, FALSE, FALSE, names.fault(pid).c_str());
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, names.faultDone(pid).c_str());
        HANDLE mapping = nullptr;
        KeepRunningBlock* block = mapBlock(names.block(pid), &mapping);
        // A filled block's launch and sensor: read now (the first viewer filled it before starting this watchdog) and
        // again when the viewer ends, since the sensor's choice changes while it runs.
        bool filledSeen = false; // a filled block read from this very process
        auto readBlock = [&] {
            if (!block || block->filled == 0) return;
            filledSeen = true;
            if (!haveArgs) {
                launchArgs = block->args; // the first viewer's own: every restart starts from these
                argsRefused = block->argsRefused != 0;
                haveArgs = true;
            }
            sensorOff = block->sensorOff != 0;
        };
        readBlock();
        if (first) io.log("watching the viewer (pid " + std::to_string(pid) + ")");
        first = false;
        auto release = [&] {
            if (block) UnmapViewOfFile(block);
            if (mapping) CloseHandle(mapping);
            for (HANDLE h : {clean, fault, done}) {
                if (h) CloseHandle(h);
            }
            ReleaseMutex(watch);
            CloseHandle(watch);
        };
        HANDLE waits[3] = {proc, stand, fault};
        const DWORD n = fault ? 3 : 2;
        DWORD w = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
        // A fault signal counts only with the block's mark set; anything else is ignored.
        while (w == WAIT_OBJECT_0 + 2 && !(block && block->faulted)) w = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 1) {
            io.log("told to stand down (an install); the viewer is left as it is");
            CloseHandle(proc);
            release();
            return WatchdogEnd::ToldToStandDown;
        }
        std::string faultSaid;
        if (w == WAIT_OBJECT_0 + 2) {
            // A fault the viewer recorded: written down from here, then the viewer is ended if it hasn't ended.
            FaultRecord f;
            f.thread = block->thread;
            f.code = block->code;
            f.exceptionPointers = block->exceptionPointers;
            f.what.assign(block->what, strnlen(block->what, sizeof block->what));
            auto result = std::make_shared<std::string>();
            auto finished = std::make_shared<std::atomic<bool>>(false);
            HANDLE dup = nullptr;
            DuplicateHandle(GetCurrentProcess(), proc, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
            std::thread writer([io, dup, pid, f, result, finished] {
                if (io.writeFault) *result = io.writeFault(dup, pid, f);
                finished->store(true);
                if (dup) CloseHandle(dup);
            });
            const uint64_t until = io.nowMs() + p.dumpTimeoutMs;
            while (!finished->load() && io.nowMs() < until) Sleep(20);
            if (finished->load()) {
                writer.join();
                faultSaid = "the viewer faulted (" + f.what + "); " +
                            (result->empty() ? std::string("no dump could be written") : "dump " + *result);
            } else {
                writer.detach(); // it finishes or not on its own; nothing waits on it again
                faultSaid = "the viewer faulted (" + f.what + "); its dump didn't finish in " +
                            std::to_string(p.dumpTimeoutMs / 1000) + " s and was given up";
            }
            if (done) SetEvent(done);
            if (WaitForSingleObject(proc, static_cast<DWORD>(p.endGraceMs)) == WAIT_TIMEOUT) {
                TerminateProcess(proc, f.code ? f.code : 3);
                WaitForSingleObject(proc, 5000);
                faultSaid += "; it didn't end by itself and was ended";
            }
            io.log(faultSaid);
        } else {
            WaitForSingleObject(proc, INFINITE);
        }
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        proc = nullptr;
        // Clean is decided by the clean-stop mark alone: a fault during teardown, after the person
        // closed the window or turned outside viewing off, is recorded above and never restarted.
        const bool was_clean = clean && WaitForSingleObject(clean, 0) == WAIT_OBJECT_0;
        readBlock();
        release();
        if (was_clean) {
            io.log("the viewer stopped cleanly (" + exitText(code) + ")" +
                   (faultSaid.empty() ? std::string() : std::string(", then faulted while closing")) + "; standing down");
            return WatchdogEnd::StoodDown;
        }
        // A restart only for a viewer that kept its record and ended as a crash does: a recorded fault,
        // or a crash's exit code. End task, taskkill and anything else with no fault ended it on purpose.
        if (!filledSeen) {
            io.log("the viewer ended (" + exitText(code) + ") with no record of its own read; not starting it again");
            return WatchdogEnd::StoodDown;
        }
        if (faultSaid.empty() && !isCrashExit(code)) {
            char said[96];
            std::snprintf(said, sizeof said, "the viewer ended with 0x%08lX, not a crash code, not started again",
                          static_cast<unsigned long>(code));
            io.log(said);
            return WatchdogEnd::StoodDown;
        }
        if (argsRefused) {
            io.log("the viewer ended without a clean stop (" + exitText(code) +
                   "), but its command line was too long to keep whole; not starting it again");
            return WatchdogEnd::GaveUp;
        }
        const LONG startsAtEnd = starts ? *starts : 0;
        for (;;) {
            const auto delay = nextRestartDelayMs(restarts, io.nowMs(), p);
            if (!delay) {
                io.log("the viewer ended without a clean stop (" + exitText(code) + ") after " +
                       std::to_string(p.maxRestarts) + " restarts in " + std::to_string(p.windowMs / 60000) +
                       " minutes; giving up");
                return WatchdogEnd::GaveUp;
            }
            io.log("the viewer ended without a clean stop (" + exitText(code) + "); starting it again in " +
                   std::to_string(*delay / 1000) + "." + std::to_string(*delay % 1000 / 100) + " s" +
                   (sensorOff ? ", with the sensor off as it was" : ""));
            if (WaitForSingleObject(stand, static_cast<DWORD>(*delay)) == WAIT_OBJECT_0) {
                io.log("told to stand down (an install) before the restart");
                return WatchdogEnd::ToldToStandDown;
            }
            if (exists(names.running()) || (starts && *starts != startsAtEnd)) {
                // Someone started a viewer during the wait, even one that has closed again: their launch, their choices.
                io.log("a viewer was started meanwhile; standing down");
                return WatchdogEnd::StoodDown;
            }
            restarts.push_back(io.nowMs());
            proc = io.relaunch ? io.relaunch(relaunchArguments(launchArgs, sensorOff)) : nullptr;
            if (proc) break;
            io.log("the viewer couldn't be started again");
        }
        io.log("started the viewer again (pid " + std::to_string(GetProcessId(proc)) + ")");
    }
}

int runKeepRunningMain(DWORD pid, const std::wstring& stateDir) {
    // The dump is written from here, so it needs to read the viewer's memory; ending a viewer that doesn't end
    // after a fault needs PROCESS_TERMINATE.
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE, FALSE, pid);
    if (!proc) proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    if (!proc) return 2;
    const std::wstring logPath = stateDir.empty() ? std::wstring() : stateDir + L"\\keep-running.log";
    const std::wstring crashDir = stateDir.empty() ? std::wstring() : stateDir + L"\\crash";
    WatchdogIo io;
    io.nowMs = [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    };
    io.log = [logPath](const std::string& line) {
        if (logPath.empty()) return;
        // Kept small: past 256 KiB the file starts again.
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const bool big = GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fa) &&
                         (fa.nFileSizeHigh != 0 || fa.nFileSizeLow > 256 * 1024);
        std::ofstream f(logPath, big ? std::ios::trunc : std::ios::app);
        if (!f) return;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char ts[32];
        std::snprintf(ts, sizeof ts, "%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay, st.wHour,
                      st.wMinute, st.wSecond);
        f << ts << line << "\n";
    };
    io.writeFault = [crashDir](HANDLE p, DWORD id, const FaultRecord& f) {
        return crashDir.empty() ? std::string() : writeFaultDump(crashDir, p, id, f);
    };
    const std::wstring exe = exePath();
    io.relaunch = [exe](const std::wstring& args) -> HANDLE {
        if (exe.empty()) return nullptr;
        std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? std::wstring() : L" " + args);
        std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
            return nullptr;
        CloseHandle(pi.hThread);
        return pi.hProcess;
    };
    const WatchdogEnd e = runWatchdog(proc, keepRunningNames(), io, RestartPolicy{});
    return e == WatchdogEnd::GaveUp ? 1 : 0;
}

bool spawnKeepRunning(const std::wstring& stateDir) {
    const std::wstring exe = exePath();
    if (exe.empty()) return false;
    std::wstring cmd = L"\"" + exe + L"\" --keep-running " + std::to_wstring(GetCurrentProcessId()) + L" " + quoteArg(stateDir);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    const DWORD base = CREATE_NO_WINDOW | DETACHED_PROCESS;
    // Outside the viewer's job if it is in one, so ending the job tree doesn't end the watchdog with it.
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, base | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                        nullptr, &si, &pi) &&
        !CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, base, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace kvhost
