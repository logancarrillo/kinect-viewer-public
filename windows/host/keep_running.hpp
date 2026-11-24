// Keeping the viewer running while viewing from outside the home is on: it must stay reachable from any network for
// as long as the PC is left running. No admin, no service: the viewer starts a small watchdog, a second
// copy of its own exe run as `kinect-viewer.exe --keep-running <pid> <state folder>`, which waits on the viewer's
// process.
//
//   - A clean stop (outside viewing turned off, the window closed, a logoff, the host shutting down) tells it to
//     stand down. So does an install (install.ps1 sets the watchdog's own stand-down event before it copies).
//   - A fault the viewer can see is recorded by the watchdog, out of the faulting process: the
//     viewer fills a small shared block (thread, exception pointers, what happened) and signals; the watchdog
//     writes the minidump with MiniDumpWriteDump on the viewer's handle, a line to crash\crash.log, and tells the
//     viewer it is done, and the viewer ends itself. A dump that doesn't finish, or a viewer that doesn't end, is
//     cut off: no fault turns into a hang.
//   - Any end without a clean stop is written to keep-running.log with its exit code, and the viewer is started
//     again with the command line it was launched with, keeping the person's choices: if the sensor was last asked
//     off, it starts with --no-autostart. Outside viewing turned off is a clean stop, so it is never reopened.
//     It backs off, gives up after too many restarts in a short time, and never starts a second viewer.
//
// The named objects are per user session ("Local\") and per process id. A test names its own with a suffix, and
// the viewer makes the real ones only on a real launch (never a test run), so a test never signals a viewer it
// didn't start.
#pragma once

#include <winsock2.h> // before windows.h, as everywhere in the host
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kvhost {

struct KeepRunningNames {
    std::wstring prefix;                       // L"Local\\KinectViewer-KeepRunning", plus a test's suffix
    std::wstring watch(DWORD pid) const;       // mutex: held by the one watchdog watching that viewer
    std::wstring cleanStop(DWORD pid) const;   // manual-reset event: set when that viewer stops cleanly
    std::wstring block(DWORD pid) const;       // file mapping: that viewer's KeepRunningBlock
    std::wstring fault(DWORD pid) const;       // auto-reset event: the viewer recorded a fault in its block
    std::wstring faultDone(DWORD pid) const;   // manual-reset event: the watchdog has written it down
    std::wstring standDown(DWORD watchdogPid) const; // manual-reset event: that watchdog stops (an install)
    std::wstring running() const;              // mutex every real viewer keeps open: one is running
    std::wstring starts() const;               // file mapping: a LONG every real viewer adds one to as it starts
};
// The real names for an empty suffix; a test passes keepRunningTestSuffix().
KeepRunningNames keepRunningNames(const std::wstring& suffix = {});
// A suffix no other process uses: this process's id and a counter.
std::wstring keepRunningTestSuffix();

// Shared between a viewer and its watchdog, one per viewer process.
struct KeepRunningBlock {
    volatile LONG filled;                   // 1 once the viewer has written args and sensorOff: until then the
                                            // watchdog keeps what it knew (a restarted viewer that dies early)
    volatile LONG argsRefused;              // 1 when the launch's command line was too long to keep whole
    volatile LONG sensorOff;                // 1 while the sensor was last asked off (on the PC or a phone)
    volatile LONG faulted;                  // 1 once the fields below hold a fault: a fault signal without it is ignored
    DWORD thread;                           // the faulting thread
    DWORD code;                             // the exit code the viewer ends with
    unsigned long long exceptionPointers;   // an EXCEPTION_POINTERS* in the viewer's memory, or 0
    char what[256];                         // what happened: a code and module+offset, or a C++ type; never text
    wchar_t args[2048];                     // the viewer's command line after its exe, as launched
};

// An exit code that says the viewer crashed. A fault the RECORDER marked restarts whatever the exit
// code (a C++ exception and abort end with 3); this list covers only the deaths the recorder couldn't see. An explicit
// list, never a range: it must leave ended 0xCFFFFFFF (a hung app the person ended with "close the program"),
// 0xC000013A (Ctrl+C or a console closed), 0xC0000142 (DLL init failed: a start during a session's end would loop),
// 0xC0000135 (a DLL not found: a broken install would loop), 0x40010004 (a debugger ended it) and 1 (Task Manager's
// End task, taskkill). Anything not listed: no restart, and one log line saying the code, so the list grows by evidence.
inline bool isCrashExit(DWORD code) {
    switch (code) {
        case 3:          // abort(), std::terminate
        case 0xC0000005: // access violation
        case 0xC0000409: // __fastfail, stack buffer overrun
        case 0xC0000374: // heap corruption
        case 0xC00000FD: // stack overflow
        case 0xC000001D: // illegal instruction
        case 0xC0000094: // integer divide by zero
        case 0xC0000095: // integer overflow
        case 0xC0000096: // privileged instruction
        case 0xC0000420: // assertion failure
        case 0xC0000602: // fail-fast exception
        case 0xC000041D: // fatal exception in a window callback
        case 0xC0000006: // in-page error
            return true;
        default:
            return false;
    }
}

// The arguments to start the viewer again with: the launch's own, with --no-autostart added when the sensor was
// last asked off and taken away when it was last asked on.
std::wstring relaunchArguments(const std::wstring& launchArgs, bool sensorOff);
// This process's command line after its exe (empty when there is none), with --state-dir's and --camera-roll-dir's
// folders made absolute: a restart runs from the exe's folder.
std::wstring ownArguments();
// The same for any command line (its first token is the program).
std::wstring launchArgumentsOf(const wchar_t* commandLine);
// The longest command line a block keeps; a longer one is refused, never cut (no restart then).
constexpr size_t kKeepRunningArgsMax = 2047;

struct RestartPolicy {
    int maxRestarts = 5;                                                 // in any window...
    uint64_t windowMs = 10 * 60 * 1000;                                  // ...this long; past that it gives up
    std::vector<uint64_t> delaysMs = {2000, 5000, 15000, 30000, 60000}; // before the 1st, 2nd... in the window
    uint64_t dumpTimeoutMs = 30000; // a dump not written by then is given up
    uint64_t endGraceMs = 10000;    // a faulted viewer not ended by then (after its dump) is ended
};
// The wait before the next restart, given the times of the earlier ones and now; nullopt: give up.
std::optional<uint64_t> nextRestartDelayMs(const std::vector<uint64_t>& restartsMs, uint64_t nowMs,
                                           const RestartPolicy& p);

// The viewer's side, made only on a real launch, for this process (pid).
class KeepRunning {
public:
    using Spawn = std::function<bool()>; // starts a watchdog for this process; false if it couldn't
    // sensorOff: the sensor as launched (--no-autostart) or as last asked, written before the block is marked filled.
    KeepRunning(KeepRunningNames names, DWORD pid, const std::wstring& launchArgs, bool sensorOff, Spawn spawn);
    ~KeepRunning(); // a clean stop: the event is set
    KeepRunning(const KeepRunning&) = delete;
    KeepRunning& operator=(const KeepRunning&) = delete;
    // On: the event is reset, and a watchdog is started unless one already watches this process. Off: it is set.
    void set(bool on);
    // The person closed the window (or Windows is ending the session): the clean-stop mark is set for good, and no
    // later set(true) during the teardown can take it back. The first thing the close handler does.
    void stopForGood();
    // The sensor's last On/Off request (the PC's or a phone's): a restart keeps it.
    void setSensorOff(bool off);
    const KeepRunningNames& names() const { return names_; }
    KeepRunningBlock* block() const { return block_; }
    // For the crash recorder, which must not allocate or build names on a faulting thread: made in advance.
    HANDLE faultEvent() const { return fault_; }
    HANDLE faultDoneEvent() const { return fault_done_; }
    const wchar_t* watchName() const { return watch_name_.c_str(); }

private:
    KeepRunningNames names_;
    DWORD pid_ = 0;
    Spawn spawn_;
    std::wstring watch_name_;
    std::atomic<bool> stopped_for_good_{false};
    HANDLE clean_stop_ = nullptr;
    HANDLE running_ = nullptr;
    HANDLE starts_mapping_ = nullptr;
    HANDLE mapping_ = nullptr;
    HANDLE fault_ = nullptr;
    HANDLE fault_done_ = nullptr;
    KeepRunningBlock* block_ = nullptr;
};
// The KeepRunning of this process, if one was made (the crash recorder reports through it).
KeepRunning* keepRunningOfThisProcess();

struct FaultRecord {
    DWORD thread = 0;
    DWORD code = 0;
    unsigned long long exceptionPointers = 0;
    std::string what;
};
struct WatchdogIo {
    std::function<HANDLE(const std::wstring& args)> relaunch; // starts the viewer again: its handle, or nullptr
    std::function<void(const std::string&)> log;              // one line, no address in it
    std::function<uint64_t()> nowMs;                          // a monotonic clock
    // Writes the faulted viewer's dump and its crash.log line: the dump's name, or "" if none could be written.
    // Run on a thread of its own and given up after RestartPolicy::dumpTimeoutMs.
    std::function<std::string(HANDLE proc, DWORD pid, const FaultRecord& f)> writeFault;
};
enum class WatchdogEnd { StoodDown, GaveUp, AnotherIsWatching, CantStart, ToldToStandDown };
// Watches `proc` (the watchdog owns the handle) until a clean stop, a stand-down, another viewer or watchdog
// taking over, or giving up. `launchArgs` is used when the viewer's own block has none.
WatchdogEnd runWatchdog(HANDLE proc, const KeepRunningNames& names, const WatchdogIo& io, const RestartPolicy& p);

// The out-of-process dump: MiniDumpWriteDump on `proc` (it needs PROCESS_VM_READ and PROCESS_QUERY_INFORMATION),
// with the fault's thread and exception pointers when it has them, and the line, into `dir` (made if missing),
// keeping at most kCrashDumpsKept dumps. The dump's name, or "" if none could be written.
std::string writeFaultDump(const std::wstring& dir, HANDLE proc, DWORD pid, const FaultRecord& f);

// `kinect-viewer.exe --keep-running <pid> <state folder>`: the real names, this exe, keep-running.log.
int runKeepRunningMain(DWORD pid, const std::wstring& stateDir);
// Starts that watchdog for this process, detached from any job the viewer is in when Windows allows it.
bool spawnKeepRunning(const std::wstring& stateDir);

} // namespace kvhost
