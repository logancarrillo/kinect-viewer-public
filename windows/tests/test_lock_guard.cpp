// Lock-guard tests for the Windows branch of core/src/lock_guard_file.cpp.
// No Kinect is opened; every test works on a lock file in the temp folder.
#include "kv_test.hpp"
#include "lock_guard_file.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string tempLockPath(const char* tag) {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    std::string p = std::string(dir) + "kv-lock-test-" + tag + "-" + std::to_string(GetCurrentProcessId());
    DeleteFileA(p.c_str());
    return p;
}

std::string readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeAll(const std::string& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

bool fileExists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Starts a hidden child process; returns its handle and pid.
HANDLE spawn(const wchar_t* cmdline, DWORD* out_pid) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd(cmdline);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    *out_pid = pi.dwProcessId;
    return pi.hProcess;
}

std::string otherAgentLock(DWORD pid) {
    return "agent: another program - some tool\npid: " + std::to_string(pid) + "\nopened_at: 2026-11-11T00:00:00\n";
}

} // namespace

KV_TEST(acquire_creates_lock_with_our_pid_and_release_removes_it) {
    const std::string p = tempLockPath("fresh");
    {
        kv::KinectLockFile lf(p);
        std::string err;
        KV_REQUIRE(lf.acquire(&err));
        KV_CHECK(lf.isHeld());
        const std::string text = readAll(p);
        KV_CHECK(text.find("pid: " + std::to_string(GetCurrentProcessId()) + "\n") != std::string::npos);
        KV_CHECK(!lf.isLockedByOther());
        lf.release();
        KV_CHECK(!fileExists(p));
    }
}

// The lock says "agent: kinect-viewer". An earlier build's "agent: <a name> - kinect-viewer" with our pid is still
// ours for one version; another program's line with our pid isn't, nor is a longer name that starts the same.
KV_TEST(our_lock_says_kinect_viewer_and_an_earlier_builds_form_is_still_ours) {
    const std::string p = tempLockPath("agent");
    const std::string our_pid = "pid: " + std::to_string(GetCurrentProcessId()) + "\n";
    {
        kv::KinectLockFile lf(p);
        std::string err;
        KV_REQUIRE(lf.acquire(&err));
        const std::string text = readAll(p);
        KV_CHECK_MSG(text.rfind("agent: kinect-viewer\n" + our_pid, 0) == 0, text);
        lf.release();
    }
    const struct {
        const char* agent_line;
        bool ours;
    } cases[] = {
        {"agent: kinect-viewer\n", true},
        {"agent: kinect-viewer\r\n", true},
        {"agent: an earlier build - kinect-viewer\n", true},
        {"agent: another program - some tool\n", false},
        {"agent: kinect-viewer-helper\n", false},
        {"agent: - kinect-viewer\n", false},
        {"no agent line\n", false},
    };
    for (const auto& c : cases) {
        writeAll(p, std::string(c.agent_line) + our_pid + "opened_at: 2026-11-11T00:00:00\n");
        kv::KinectLockFile lf(p);
        KV_CHECK_MSG(lf.isLockedByOther() == !c.ours, c.agent_line);
    }
    DeleteFileA(p.c_str());
}

KV_TEST(lock_held_by_live_process_blocks_and_is_not_overwritten) {
    const std::string p = tempLockPath("live");
    DWORD pid = 0;
    HANDLE child = spawn(L"cmd.exe /c ping -n 30 127.0.0.1 >nul", &pid);
    KV_REQUIRE(child != nullptr);
    const std::string original = otherAgentLock(pid);
    writeAll(p, original);

    kv::KinectLockFile lf(p);
    std::string owner;
    KV_CHECK(lf.isLockedByOther(&owner));
    std::string err;
    KV_CHECK(!lf.acquire(&err));
    KV_CHECK(!lf.isHeld());
    KV_CHECK_MSG(err.find("in use") != std::string::npos, err);
    KV_CHECK(readAll(p) == original);

    TerminateProcess(child, 0);
    WaitForSingleObject(child, 5000);
    CloseHandle(child);
    DeleteFileA(p.c_str());
}

KV_TEST(lock_held_by_protected_system_pid_counts_as_live) {
    // PID 4 is the System process: OpenProcess is denied, which must read as "alive".
    const std::string p = tempLockPath("system");
    writeAll(p, otherAgentLock(4));
    kv::KinectLockFile lf(p);
    KV_CHECK(lf.isLockedByOther());
    KV_CHECK(!lf.acquire());
    DeleteFileA(p.c_str());
}

KV_TEST(stale_lock_from_exited_process_is_reclaimed) {
    const std::string p = tempLockPath("stale");
    DWORD pid = 0;
    HANDLE child = spawn(L"cmd.exe /c exit 0", &pid);
    KV_REQUIRE(child != nullptr);
    WaitForSingleObject(child, 10000);
    CloseHandle(child); // last handle gone: the pid no longer names a process
    writeAll(p, otherAgentLock(pid));

    kv::KinectLockFile lf(p);
    KV_CHECK(!lf.isLockedByOther());
    std::string err;
    KV_CHECK_MSG(lf.acquire(&err), err);
    KV_CHECK(lf.isHeld());
    const std::string text = readAll(p);
    KV_CHECK(text.find("pid: " + std::to_string(GetCurrentProcessId()) + "\n") != std::string::npos);
    lf.release();
    KV_CHECK(!fileExists(p));
}

KV_TEST(lock_without_pid_is_treated_as_live) {
    const std::string p = tempLockPath("nopid");
    writeAll(p, "someone is using the kinect\n");
    kv::KinectLockFile lf(p);
    KV_CHECK(lf.isLockedByOther());
    KV_CHECK(!lf.acquire());
    KV_CHECK(readAll(p) == "someone is using the kinect\n");
    DeleteFileA(p.c_str());
}

KV_TEST(default_lock_path_is_under_userprofile) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    KV_REQUIRE(n > 0);
    const std::string def = kv::KinectLockFile::defaultLockPath();
    KV_CHECK(def.size() > 14);
    KV_CHECK(def.substr(def.size() - 14) == "\\KINECT-IN-USE");
}

KV_TEST_MAIN()
