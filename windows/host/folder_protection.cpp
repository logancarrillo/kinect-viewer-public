#include "folder_protection.hpp"

#include "host_state.hpp" // ensureOwnerOnlyDirectory

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwctype>

namespace kvhost {

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n) : std::wstring();
}

} // namespace

std::wstring fullPathLower(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, buf, nullptr);
    std::wstring s = (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n) : p;
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool isUnderOneDrive(const std::wstring& path, const std::vector<std::wstring>& extra_roots) {
    std::vector<std::wstring> roots = extra_roots;
    for (const wchar_t* v : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
        const std::wstring r = envVar(v);
        if (!r.empty()) roots.push_back(r);
    }
    const std::wstring p = fullPathLower(path);
    for (const auto& r : roots) {
        const std::wstring rl = fullPathLower(r);
        if (rl.empty()) continue;
        if (p == rl || (p.size() > rl.size() && p.compare(0, rl.size(), rl) == 0 && p[rl.size()] == L'\\')) return true;
    }
    return false;
}

bool protectPrivateFolder(const std::wstring& dir, std::string* err) {
    if (!ensureOwnerOnlyDirectory(dir, err)) return false;
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(dir.c_str(), (attrs & ~FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)) {
        if (err) *err = "cannot set NOT_CONTENT_INDEXED on " + narrow(dir);
        return false;
    }
    for (const wchar_t* marker : {L"\\.nosync", L"\\.metadata_never_index"}) {
        HANDLE h = CreateFileW((dir + marker).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    return true;
}

} // namespace kvhost
