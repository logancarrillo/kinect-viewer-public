#include "crash_recorder.hpp"

#include "keep_running.hpp"

// dbghelp after windows.h
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

namespace kvhost {

namespace {

std::mutex g_dir_m;
std::wstring g_dir;
std::atomic<bool> g_recording{false}; // one record per process: a fault while recording just ends it

std::wstring dirNow() {
    std::lock_guard<std::mutex> g(g_dir_m);
    return g_dir;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(c) : '?';
    return s;
}

// The module an address is in, and the offset into it: "kinect-viewer.exe+0x1a2b3c". Written into `out`.
void whereIs(const void* addr, char* out, size_t n) {
    HMODULE m = nullptr;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     static_cast<LPCWSTR>(addr), &m) || !m) {
        std::snprintf(out, n, "an unknown module");
        return;
    }
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    std::snprintf(out, n, "%s+0x%llx", name,
                  static_cast<unsigned long long>(reinterpret_cast<const char*>(addr) - reinterpret_cast<const char*>(m)));
}

struct Job {
    EXCEPTION_POINTERS* ep = nullptr;
    DWORD thread = 0;
    const char* what = "";
};

DWORD WINAPI writeHere(LPVOID p) {
    const Job& job = *static_cast<const Job*>(p);
    const std::wstring dir = dirNow();
    if (dir.empty()) return 0;
    CreateDirectoryW(dir.c_str(), nullptr);
    pruneCrashDumps(dir);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             static_cast<unsigned long>(GetCurrentProcessId()));
    const std::wstring name = std::wstring(L"crash-") + stamp + L".dmp";
    bool dumped = false;
    HANDLE f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = job.thread;
        mei.ExceptionPointers = job.ep;
        mei.ClientPointers = FALSE;
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                   static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
                                   job.ep ? &mei : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(f);
        if (!dumped) DeleteFileW((dir + L"\\" + name).c_str());
    }
    appendCrashLine(dir, GetCurrentProcessId(), job.thread, job.what, dumped ? narrow(name) : std::string());
    return 0;
}

// Records once, then ends the process with `code`. A watching watchdog writes the dump from outside (its own
// process can't be held up by this one's state); otherwise a thread of this one does. Each wait is bounded.
[[noreturn]] void recordAndEnd(EXCEPTION_POINTERS* ep, const char* what, DWORD code) {
    if (g_recording.exchange(true)) {
        // A second thread faulting while the first records: it waits for the first to end the process, bounded, so
        // the first's record is never cut short.
        Sleep(kRecordedFaultWaitMs + 5000);
    } else {
        bool told = false;
        // Nothing here allocates or builds a name: the handles and the watch name were made with the KeepRunning.
        KeepRunning* kr = keepRunningOfThisProcess();
        KeepRunningBlock* b = kr ? kr->block() : nullptr;
        if (b) {
            HANDLE watched = OpenMutexW(SYNCHRONIZE, FALSE, kr->watchName());
            HANDLE fault = kr->faultEvent();
            HANDLE done = kr->faultDoneEvent();
            if (watched && fault && done) {
                b->thread = GetCurrentThreadId();
                b->code = code;
                b->exceptionPointers = reinterpret_cast<uintptr_t>(ep);
                std::snprintf(b->what, sizeof b->what, "%s", what);
                InterlockedExchange(&b->faulted, 1);
                SetEvent(fault);
                WaitForSingleObject(done, kRecordedFaultWaitMs);
                told = true;
            }
            if (watched) CloseHandle(watched);
        }
        if (!told) {
            static Job job; // static: nothing on this thread's stack is relied on after the wait
            job.ep = ep;
            job.thread = GetCurrentThreadId();
            job.what = what;
            HANDLE t = CreateThread(nullptr, 256 * 1024, writeHere, &job, 0, nullptr);
            if (t) {
                WaitForSingleObject(t, kInProcessDumpWaitMs);
                CloseHandle(t);
            }
        }
    }
    // Never on to a just-in-time debugger, Windows' error dialog, or anything else: ended here.
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);
}

// A context captured here, for a fault that isn't an SEH exception (terminate, abort): static, so the watchdog
// can read it from this process's memory after this function has handed it over.
EXCEPTION_RECORD g_here_record;
CONTEXT g_here_context;
EXCEPTION_POINTERS g_here_pointers;

[[noreturn]] void recordHere(const char* what, DWORD code) {
    RtlCaptureContext(&g_here_context);
    std::memset(&g_here_record, 0, sizeof g_here_record);
    g_here_record.ExceptionCode = 0xE0000001; // "recorded here": not a real exception
    g_here_record.ExceptionAddress = reinterpret_cast<void*>(g_here_context.Rip);
    g_here_pointers.ExceptionRecord = &g_here_record;
    g_here_pointers.ContextRecord = &g_here_context;
    recordAndEnd(&g_here_pointers, what, code);
}

char g_what[256];

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* at = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    char where[160];
    whereIs(at, where, sizeof where);
    std::snprintf(g_what, sizeof g_what, "unhandled exception 0x%08lX in %s", static_cast<unsigned long>(code), where);
    recordAndEnd(ep, g_what, code);
}

// What is in flight on this thread: " with an uncaught <type>", or nothing. The type only, never its message.
void inFlight(char* out, size_t n) {
    out[0] = '\0';
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            std::snprintf(out, n, " with an uncaught %s", typeid(x).name());
        } catch (...) {
            std::snprintf(out, n, " with an uncaught exception of an unknown type");
        }
    }
}

void onTerminate() {
    char flying[200];
    inFlight(flying, sizeof flying);
    std::snprintf(g_what, sizeof g_what, "std::terminate%s", flying[0] ? flying
                  : " with no exception (a joinable std::thread destroyed, or a noexcept function that threw)");
    recordHere(g_what, 3);
}

void onAbortSignal(int) {
    // MSVC keeps the terminate handler per thread, so std::terminate on a thread started after the recorder was
    // installed (a session's, a pool's) reaches the default one, which calls abort(): say what was in flight.
    char flying[200];
    inFlight(flying, sizeof flying);
    if (flying[0]) std::snprintf(g_what, sizeof g_what, "abort() from std::terminate%s", flying);
    else std::snprintf(g_what, sizeof g_what, "abort()");
    recordHere(g_what, 3);
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    recordHere("a CRT invalid parameter", 3);
}

void onPureCall() { recordHere("a pure virtual call", 3); }

struct Base {
    Base() { callIt(); }
    virtual ~Base() = default;
    virtual void pure() = 0;
    void callIt() { pure(); }
};
struct Derived : Base {
    void pure() override {}
};

} // namespace

void pruneCrashDumps(const std::wstring& dir) {
    struct Dump {
        std::wstring path;
        FILETIME t;
    };
    std::vector<Dump> dumps;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        dumps.push_back({dir + L"\\" + fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(dumps.begin(), dumps.end(), [](const Dump& a, const Dump& b) { return CompareFileTime(&a.t, &b.t) < 0; });
    while (dumps.size() >= static_cast<size_t>(kCrashDumpsKept)) {
        DeleteFileW(dumps.front().path.c_str());
        dumps.erase(dumps.begin());
    }
}

void appendCrashLine(const std::wstring& dir, DWORD pid, DWORD thread, const std::string& what, const std::string& dump) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[600];
    std::snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u pid %lu thread %lu: %s; %s\r\n", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond, static_cast<unsigned long>(pid),
                  static_cast<unsigned long>(thread), what.c_str(),
                  dump.empty() ? "no dump could be written" : ("dump " + dump).c_str());
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE lf = CreateFileW((dir + L"\\crash.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lf == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(lf, &size) && size.QuadPart < 256 * 1024) {
        DWORD n = 0;
        WriteFile(lf, line, static_cast<DWORD>(std::strlen(line)), &n, nullptr);
    }
    CloseHandle(lf);
}

void installCrashRecorder(const std::wstring& dir) {
    {
        std::lock_guard<std::mutex> g(g_dir_m);
        g_dir = dir;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        // No error dialog of Windows' own for a fault, and none for abort().
        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        SetUnhandledExceptionFilter(onUnhandled);
        std::set_terminate(onTerminate);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort() raises SIGABRT, not a fastfail
        std::signal(SIGABRT, onAbortSignal);
        _set_invalid_parameter_handler(onInvalidParameter);
        _set_purecall_handler(onPureCall);
    });
}

void raiseTestFault(TestFault f) {
    switch (f) {
        case TestFault::AccessViolation: {
            volatile int* p = nullptr;
            *p = 1;
            break;
        }
        case TestFault::UncaughtException:
            // Thrown on a std::thread and never caught: std::terminate, with the exception current.
            std::thread([] { throw std::runtime_error("synthetic"); }).join();
            break;
        case TestFault::Terminate: std::terminate();
        case TestFault::Abort: std::abort();
        case TestFault::PureCall: {
            Derived d;
            (void)d;
            break;
        }
    }
    std::_Exit(99); // not reached
}

} // namespace kvhost
