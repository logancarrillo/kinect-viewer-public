// W8 Phase A tests: a state folder that is new for each test (never a real folder) and removed after it.
#pragma once

#include "host_state.hpp"
#include "kv_test.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kvtest {

inline void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

struct OutsideTempState {
    std::wstring dir;
    bool fresh = false;
    explicit OutsideTempState(const wchar_t* tag) {
        static std::atomic<int> counter{0};
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        dir = std::wstring(tmp) + L"kv-outside-" + tag + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
              std::to_wstring(++counter);
        // New, or the run stops: CreateDirectoryW fails on a folder that is already there.
        fresh = CreateDirectoryW(dir.c_str(), nullptr) != 0;
        if (!fresh) {
            // Never a folder that was already there: the run stops here rather than touch it.
            std::fprintf(stderr, "FATAL: test folder %ls was not new (Win32 error %lu)\n", dir.c_str(), GetLastError());
            std::fflush(stderr);
            std::exit(2);
        }
        kvhost::ensureOwnerOnlyDirectory(dir);
    }
    ~OutsideTempState() {
        if (fresh) removeTree(dir);
    }
    std::wstring file(const wchar_t* name) const { return dir + L"\\" + name; }
};

inline bool writeRaw(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
    return ok && n == text.size();
}

} // namespace kvtest
