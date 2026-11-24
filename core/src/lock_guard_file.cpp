#include "lock_guard_file.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#define kv_getpid _getpid
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define kv_getpid getpid
#endif

namespace kv {

namespace {
std::mutex g_active_lock_mutex;
std::string g_active_lock_path;
int g_active_lock_pid = 0;

std::string formatIsoNow() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm_buf);
    return std::string(buf);
}

std::string readWholeFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

void cleanupAtExit() {
    std::lock_guard<std::mutex> lk(g_active_lock_mutex);
    if (!g_active_lock_path.empty() && g_active_lock_pid == static_cast<int>(kv_getpid())) {
        std::string content = readWholeFile(g_active_lock_path);
        std::string pid_tag = "pid: " + std::to_string(g_active_lock_pid);
        if (content.find(pid_tag) != std::string::npos) {
            std::remove(g_active_lock_path.c_str());
        }
        g_active_lock_path.clear();
    }
}

#ifdef _WIN32
std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrowUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// True only when the lock text names a pid that provably no longer runs on this machine.
// A lock without a parseable pid, or whose pid we cannot query (access denied), counts as live.
bool lockContentNamesDeadPid(const std::string& content) {
    const size_t pos = content.find("pid: ");
    if (pos == std::string::npos) return false;
    size_t i = pos + 5;
    unsigned long long pid = 0;
    size_t digits = 0;
    while (i < content.size() && content[i] >= '0' && content[i] <= '9' && digits < 10) {
        pid = pid * 10ULL + static_cast<unsigned long long>(content[i] - '0');
        ++i;
        ++digits;
    }
    if (digits == 0 || pid == 0 || pid > 0xFFFFFFFFULL) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        // ERROR_INVALID_PARAMETER: no process with that id exists. Anything else (e.g. access denied) = alive.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    DWORD code = 0;
    const BOOL got = GetExitCodeProcess(h, &code);
    bool dead = false;
    if (got && code != STILL_ACTIVE) {
        dead = true;
    } else if (got && code == STILL_ACTIVE) {
        // A process may legitimately exit with 259; disambiguate with a zero-timeout wait.
        dead = (WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
    }
    CloseHandle(h);
    return dead;
}
#endif
} // namespace

std::string KinectLockFile::defaultLockPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) {
        return narrowUtf8(std::wstring(buf, n)) + "\\KINECT-IN-USE";
    }
    return "KINECT-IN-USE";
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/KINECT-IN-USE";
    }
    // No HOME: the current folder's, as Windows does without USERPROFILE (never one person's home).
    return "KINECT-IN-USE";
#endif
}

KinectLockFile::KinectLockFile(std::string lock_path)
    : path_(lock_path.empty() ? defaultLockPath() : std::move(lock_path)) {}

KinectLockFile::~KinectLockFile() {
    release();
}

// The lock's first line names who holds it: this program writes "agent: kinect-viewer". An earlier build wrote
// "agent: <a name> - kinect-viewer", and for one version that form is ours too (with our pid, below), so the
// re-entrancy check reads both. Whole line: "agent: kinect-viewer-helper" is someone else.
static bool agentLineIsOurs(const std::string& content) {
    const std::string agent = "agent: ";
    size_t at = content.rfind(agent, 0) == 0 ? 0 : content.find("\n" + agent);
    if (at == std::string::npos) {
        return false;
    }
    if (at > 0) {
        at += 1;
    }
    const size_t end = content.find('\n', at);
    std::string line = content.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const std::string ours = "kinect-viewer";
    const std::string earlier = " - " + ours;
    return line == agent + ours ||
           (line.size() > agent.size() + earlier.size() && line.compare(line.size() - earlier.size(), earlier.size(),
                                                                         earlier) == 0);
}

bool KinectLockFile::isOwnedByCurrentProcess(const std::string& content) const {
    if (!agentLineIsOurs(content)) {
        return false;
    }
    std::string pid_tag = "pid: " + std::to_string(static_cast<int>(kv_getpid())) + "\n";
    return content.find(pid_tag) != std::string::npos;
}

bool KinectLockFile::isLockedByOther(std::string* out_owner_info) const {
    std::ifstream ifs(path_);
    if (!ifs.good()) {
        return false;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    if (isOwnedByCurrentProcess(content)) {
        return false;
    }
#ifdef _WIN32
    if (lockContentNamesDeadPid(content)) {
        return false; // Stale lock left by a process that no longer exists; acquire() reclaims it.
    }
#endif
    if (out_owner_info) {
        // Collapse newlines for compact status display
        std::string clean = content;
        for (char& c : clean) {
            if (c == '\n' || c == '\r') c = ' ';
        }
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        *out_owner_info = clean.empty() ? "unknown owner (empty KINECT-IN-USE)" : clean;
    }
    return true;
}

bool KinectLockFile::acquire(std::string* out_error) {
    if (held_) {
        return true;
    }

    std::string other;
    if (isLockedByOther(&other)) {
        if (out_error) {
            *out_error = "Kinect is in use (" + path_ + ": " + other + "). Waiting without opening device.";
        }
        return false;
    }

    int pid = static_cast<int>(kv_getpid());
    std::ostringstream payload;
    payload << "agent: kinect-viewer\n"
            << "pid: " << pid << "\n"
            << "opened_at: " << formatIsoNow() << "\n";
    const std::string text = payload.str();

#ifdef _WIN32
    const std::wstring wpath = widenUtf8(path_);
    HANDLE fh = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        fh = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh != INVALID_HANDLE_VALUE) break;
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
            if (out_error) *out_error = "Failed to create lock file at " + path_ + " (Win32 error " + std::to_string(err) + ")";
            return false;
        }
        const std::string existing = readWholeFile(path_);
        if (isOwnedByCurrentProcess(existing)) {
            held_ = true;
            return true;
        }
        if (attempt == 0 && lockContentNamesDeadPid(existing)) {
            DeleteFileW(wpath.c_str()); // Reclaim a stale lock once, then retry the atomic create.
            continue;
        }
        if (out_error) {
            std::string owner_desc;
            isLockedByOther(&owner_desc);
            *out_error = "Kinect is in use (" + path_ + ": " + owner_desc + "). Waiting without opening device.";
        }
        return false;
    }
    if (fh == INVALID_HANDLE_VALUE) {
        if (out_error) *out_error = "Failed to create lock file at " + path_;
        return false;
    }
    DWORD written = 0;
    WriteFile(fh, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(fh);
#else
    int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        // Check if already owned by our own PID
        std::string existing = readWholeFile(path_);
        if (isOwnedByCurrentProcess(existing)) {
            held_ = true;
            return true;
        }
        if (out_error) {
            std::string owner_desc;
            isLockedByOther(&owner_desc);
            *out_error = "Kinect is in use (" + path_ + ": " + owner_desc + "). Waiting without opening device.";
        }
        return false;
    }
    ssize_t written = ::write(fd, text.data(), text.size());
    (void)written;
    ::close(fd);
#endif

    held_ = true;
    {
        static std::once_flag once;
        std::call_once(once, []() { std::atexit(cleanupAtExit); });
        std::lock_guard<std::mutex> lk(g_active_lock_mutex);
        g_active_lock_path = path_;
        g_active_lock_pid = pid;
    }
    return true;
}

void KinectLockFile::release() {
    if (!held_) {
        return;
    }
    std::string content = readWholeFile(path_);
    if (isOwnedByCurrentProcess(content)) {
        std::remove(path_.c_str());
    }
    held_ = false;
    {
        std::lock_guard<std::mutex> lk(g_active_lock_mutex);
        if (g_active_lock_path == path_) {
            g_active_lock_path.clear();
        }
    }
}

} // namespace kv
