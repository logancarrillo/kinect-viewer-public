// The crash recorder (a host that dies with Windows Error Reporting off otherwise leaves nothing to go on). A
// fault the process can still see (an access violation, an uncaught C++
// exception, std::terminate, abort, a CRT invalid parameter or a pure call) is recorded, and then the process ends
// itself at once (TerminateProcess), never passing on to a just-in-time debugger or an error dialog.
//
// Recorded out of the faulting process when a keep-running watchdog watches it: the fault goes
// into the shared block, the watchdog writes the minidump and the line, and the process waits for that at most
// 40 s. With no watchdog, a thread of this process writes them, waited for at most 30 s. Either way no fault turns
// into a hang. __fastfail (0xC0000409) can't be seen from inside; the watchdog records its exit code.
//
// The dump is MiniDumpNormal plus thread info: the threads' stacks and the modules, never the heap, so no colour
// frame or picture is in it. The line holds the time, the pid, the thread id, what happened (the exception code and
// the module and offset it was in, or the C++ exception's type) and the dump's name, never an address of a device
// or a message's text. At most three dumps are kept: the oldest go first. Dumps stay on the PC.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <string>

namespace kvhost {

// Installs the recorder for this process, writing into `dir` (made if missing) when no watchdog takes the fault.
// Called once, on a real launch only; a second call only changes the folder.
void installCrashRecorder(const std::wstring& dir);

// The dump folder's two shared steps, for the recorder and the watchdog alike.
void pruneCrashDumps(const std::wstring& dir); // keeps at most kCrashDumpsKept - 1, for the one about to be written
void appendCrashLine(const std::wstring& dir, DWORD pid, DWORD thread, const std::string& what, const std::string& dump);

// Test seams: the kinds of fault a test child raises once the recorder is installed.
enum class TestFault { AccessViolation, UncaughtException, Terminate, Abort, PureCall };
[[noreturn]] void raiseTestFault(TestFault f);

constexpr int kCrashDumpsKept = 3;
constexpr DWORD kRecordedFaultWaitMs = 40000; // for the watchdog's dump
constexpr DWORD kInProcessDumpWaitMs = 30000; // for this process's own

} // namespace kvhost
