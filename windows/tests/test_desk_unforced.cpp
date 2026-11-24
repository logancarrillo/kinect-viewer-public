// A test binary built without KV_DESK_FORCE_ALLOWED, as every timing row's is, ignores KV_TEST_DESK_FORCE: set in the
// environment it starts with, the variable can't turn a missed floor into a skip. Run as
// "test_desk_unforced --report" it only prints its desk report and exits 10 if the desk was forced, else 0.
#include "kv_desk.hpp"
#include "kv_test.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr int kForcedExit = 10;

// This program again with --report, KV_TEST_DESK_FORCE set to force for it alone. Its exit code, or -1 when it
// couldn't start.
int runReport(const wchar_t* force) {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --report";
    SetEnvironmentVariableW(L"KV_TEST_DESK_FORCE", force);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableW(L"KV_TEST_DESK_FORCE", nullptr);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 60'000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

} // namespace

KV_TEST(a_binary_without_the_test_define_ignores_the_desk_force_variable) {
    KV_CHECK(!kvtest::kDeskForceAllowed);
    KV_CHECK(runReport(L"busy") == 0);
    KV_CHECK(runReport(L"quiet") == 0);
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--report") == 0) {
        kvtest::DeskSampler desk(10);
        const kvtest::DeskReport r = desk.report();
        std::printf("  child: %s\n", r.line().c_str());
        return r.forced != nullptr ? kForcedExit : 0;
    }
    return kvtest::runAll(argc, argv);
}
