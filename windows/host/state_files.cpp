#include "state_files.hpp"

#include "host_state.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <fstream>
#include <sstream>

namespace kvhost {

namespace {

std::string lastErrorText(const char* what) {
    return std::string(what) + " (Win32 error " + std::to_string(GetLastError()) + ")";
}

std::atomic<FileAttributesProbe> g_probe{nullptr};

// The state files' names are plain ASCII.
std::string narrowName(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (const wchar_t c : w) out.push_back(static_cast<char>(c));
    return out;
}

std::wstring badCopyName(const std::wstring& name, int i) {
    return i == 0 ? name + L".bad" : name + L".bad." + std::to_wstring(i);
}

bool isNotFound(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }

DWORD attributesOf(const std::wstring& path, DWORD* err) {
    if (const FileAttributesProbe probe = g_probe.load()) return probe(path, err);
    const DWORD a = GetFileAttributesW(path.c_str());
    *err = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    return a;
}

} // namespace

bool writeOwnerOnlyFileAtomically(const std::wstring& dir, const std::wstring& name, const std::string& text,
                                  std::string* err) {
    if (!ensureOwnerOnlyDirectory(dir, err)) return false;
    const std::wstring final_path = dir + L"\\" + name;
    const std::wstring tmp_path = final_path + L".tmp";
    HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = lastErrorText("CreateFileW .tmp");
        return false;
    }
    DWORD written = 0;
    const BOOL wrote =
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!wrote || !applyOwnerOnlyAcl(tmp_path, false, err)) {
        DeleteFileW(tmp_path.c_str());
        if (err && err->empty()) *err = "write failed";
        return false;
    }
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = lastErrorText("MoveFileExW");
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
}

bool appendToExistingFile(const std::wstring& path, const std::string& text, std::string* err) {
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = lastErrorText("CreateFileW (append)");
        return false;
    }
    DWORD written = 0;
    const BOOL wrote =
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    if (!wrote && err) *err = lastErrorText("WriteFile (append)");
    // A flush that fails counts as a failed write: the caller then writes the file whole next time.
    const BOOL flushed = FlushFileBuffers(h);
    if (wrote && !flushed && err) *err = lastErrorText("FlushFileBuffers (append)");
    CloseHandle(h);
    return wrote && flushed;
}

bool readWholeFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return false;
    out = ss.str();
    return true;
}

FileState fileState(const std::wstring& path, unsigned long* win32err) {
    DWORD e = 0;
    const DWORD a = attributesOf(path, &e);
    FileState st = FileState::Present;
    if (a == INVALID_FILE_ATTRIBUTES) {
        st = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FileState::Missing : FileState::Unknown;
    } else if (a & FILE_ATTRIBUTE_DIRECTORY) {
        e = ERROR_DIRECTORY; // a folder where the file should be: not missing, and not a file that can be read
        st = FileState::Unknown;
    }
    if (win32err) *win32err = e;
    return st;
}

bool keepBadCopy(const std::wstring& dir, const std::wstring& name, std::string* what) {
    const std::wstring src = dir + L"\\" + name;
    std::string current;
    if (readWholeFile(src, current)) {
        for (int i = 0; i < kBadCopiesKept; ++i) {
            std::string kept;
            if (readWholeFile(dir + L"\\" + badCopyName(name, i), kept) && kept == current) {
                if (what) *what = narrowName(badCopyName(name, i)) + " already held it";
                return true;
            }
        }
    }
    const std::wstring tmp = dir + L"\\" + name + L".bad.tmp";
    auto fail = [&](DWORD e) {
        DeleteFileW(tmp.c_str());
        if (what) *what = "keeping a copy as " + narrowName(name) + ".bad failed (Win32 error " + std::to_string(e) + ")";
        return false;
    };
    if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE)) return fail(GetLastError());
    // Room for it: the oldest goes, then each copy moves up one (a gap is skipped). A move that fails stops here
    // with every copy still under some name.
    if (!DeleteFileW((dir + L"\\" + badCopyName(name, kBadCopiesKept - 1)).c_str()) && !isNotFound(GetLastError())) {
        return fail(GetLastError());
    }
    for (int i = kBadCopiesKept - 2; i >= 0; --i) {
        const std::wstring from = dir + L"\\" + badCopyName(name, i);
        const std::wstring to = dir + L"\\" + badCopyName(name, i + 1);
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) && !isNotFound(GetLastError())) {
            return fail(GetLastError());
        }
    }
    if (!MoveFileExW(tmp.c_str(), (dir + L"\\" + badCopyName(name, 0)).c_str(), MOVEFILE_WRITE_THROUGH)) {
        return fail(GetLastError());
    }
    if (what) *what = "kept as " + narrowName(name) + ".bad";
    return true;
}

bool fileExists(const std::wstring& path) {
    return fileState(path) == FileState::Present;
}

void setFileAttributesProbeForTest(FileAttributesProbe probe) { g_probe = probe; }

} // namespace kvhost
