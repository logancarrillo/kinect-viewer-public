#include "roll_store.hpp"

#include "folder_protection.hpp"
#include "json_lite.hpp"
#include "roll_wire.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace kvhost {
namespace roll {

namespace {

// ---- small helpers -----------------------------------------------------------------------------------

bool isDigit(char c) { return c >= '0' && c <= '9'; } // ASCII only: a multi-byte character never passes

int digitsValue(const std::string& s, size_t pos, size_t n) {
    int v = 0;
    for (size_t i = 0; i < n; ++i) v = v * 10 + (s[pos + i] - '0');
    return v;
}

bool allDigits(const std::string& s, size_t pos, size_t n) {
    if (pos + n > s.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!isDigit(s[pos + i])) return false;
    }
    return true;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

uint64_t fileTimeU64(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

constexpr int64_t kUnixEpochAsFileTimeMs = 11644473600000LL;

int64_t fileTimeToUnixMs(uint64_t ft) { return static_cast<int64_t>(ft / 10000ULL) - kUnixEpochAsFileTimeMs; }

FILETIME unixMsToFileTime(int64_t ms) {
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(std::max<int64_t>(ms, 0) + kUnixEpochAsFileTimeMs) * 10000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

bool isReparse(DWORD attrs) { return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string isoUtcSeconds(int64_t unixSeconds) {
    const FILETIME ft = unixMsToFileTime(unixSeconds * 1000);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return b;
}

std::string lastErrorText(const char* what) {
    return std::string(what) + " (Win32 error " + std::to_string(GetLastError()) + ")";
}

// Writes <dir>\<name>.tmp, flushes it, then replaces <dir>\<name> with MoveFileExW(REPLACE_EXISTING |
// WRITE_THROUGH), the pattern of state_files.cpp. The file inherits the roll folder's owner-only DACL.
bool writeFileAtomically(const std::wstring& dir, const std::wstring& name, const std::string& text, std::string* err) {
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
    if (!wrote) {
        DeleteFileW(tmp_path.c_str());
        if (err) *err = "write failed";
        return false;
    }
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = lastErrorText("MoveFileExW");
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
}

bool readFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return false;
    out = ss.str();
    return true;
}

bool isRealDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !isReparse(a);
}

bool pathExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Every regular file below dir; links and junctions aren't followed.
uint64_t treeBytes(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || isReparse(fd.dwFileAttributes)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += treeBytes(dir + L"\\" + n);
        else total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

uint64_t fileBytes(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

// A file of the clip removed (read-only cleared first). True when it is gone, or was never there.
bool removeFile(const std::wstring& p) {
    if (!pathExists(p)) return true;
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(p.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
}

// ---- base64url without padding (v3 §5.6 cursors) -------------------------------------------------

constexpr const char* kB64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr size_t kMaxCursorChars = 256; // far above any real cursor; bounds the work on a hostile one

std::string base64UrlEncode(const std::string& in) {
    std::string out;
    size_t i = 0;
    const auto byte = [&](size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(in[k])); };
    for (; i + 3 <= in.size(); i += 3) {
        const uint32_t v = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        out += kB64Url[(v >> 18) & 63];
        out += kB64Url[(v >> 12) & 63];
        out += kB64Url[(v >> 6) & 63];
        out += kB64Url[v & 63];
    }
    if (in.size() - i == 1) {
        const uint32_t v = byte(i) << 16;
        out += kB64Url[(v >> 18) & 63];
        out += kB64Url[(v >> 12) & 63];
    } else if (in.size() - i == 2) {
        const uint32_t v = (byte(i) << 16) | (byte(i + 1) << 8);
        out += kB64Url[(v >> 18) & 63];
        out += kB64Url[(v >> 12) & 63];
        out += kB64Url[(v >> 6) & 63];
    }
    return out;
}

int b64UrlValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// Strict: only the url alphabet, no padding, and the canonical form (unused low bits zero), so a
// tampered cursor fails here or in the shape check that follows.
std::optional<std::string> base64UrlDecode(const std::string& in) {
    if (in.empty() || in.size() > kMaxCursorChars || in.size() % 4 == 1) return std::nullopt;
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = b64UrlValue(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) return std::nullopt;
    return out;
}

// ---- clip names ------------------------------------------------------------------------------------

// "HH-MM-SS_<trigger>[-n]", with the time in range and n of 1 to 3 digits (v3 §7.1).
bool isValidClipName(const std::string& name) {
    if (name.size() < 9) return false;
    if (!allDigits(name, 0, 2) || name[2] != '-' || !allDigits(name, 3, 2) || name[5] != '-' || !allDigits(name, 6, 2) ||
        name[8] != '_') {
        return false;
    }
    if (digitsValue(name, 0, 2) > 23 || digitsValue(name, 3, 2) > 59 || digitsValue(name, 6, 2) > 59) return false;
    const std::string tail = name.substr(9);
    for (const char* trig : {"manual", "motion", "schedule"}) {
        const std::string t = trig;
        if (tail == t) return true;
        if (tail.size() > t.size() + 1 && tail.compare(0, t.size(), t) == 0 && tail[t.size()] == '-') {
            const size_t n = tail.size() - t.size() - 1;
            return n >= 1 && n <= 3 && allDigits(tail, t.size() + 1, n);
        }
    }
    return false;
}

json::ObjectWriter sidecarObject(const ClipSidecar& s) {
    json::ObjectWriter w;
    w.field("version", s.version)
        .field("clipId", s.clipId)
        .field("startUnixMs", s.startUnixMs)
        .field("endUnixMs", s.endUnixMs)
        .field("startLocal", s.startLocal)
        .doubleField("durationSec", s.durationSec)
        .field("trigger", toWire(s.trigger));
    // startedBy is left out for motion and schedule clips (§6.3).
    if (s.trigger == ClipTrigger::Manual && s.startedBy) {
        json::ObjectWriter by;
        if (s.startedBy->deviceId) by.field("deviceId", *s.startedBy->deviceId);
        by.field("deviceName", s.startedBy->deviceName);
        w.object("startedBy", by);
    }
    json::ArrayWriter stars;
    for (const auto& e : s.starredBy) {
        json::ObjectWriter o;
        o.field("deviceId", e.deviceId).field("role", petRoleString(e.role));
        stars.object(o);
    }
    w.field("starred", s.starred())
        .array("starredBy", stars)
        .field("width", s.width)
        .field("height", s.height)
        .field("modeRaw", s.modeRaw)
        .field("sbsLayoutRaw", s.sbsLayoutRaw)
        .field("bytes", s.bytes)
        .field("codec", s.codec)
        .doubleField("preRollSec", s.preRollSec);
    return w;
}

bool parseSidecarValue(const json::Value& v, ClipSidecar& out) {
    if (v.type != json::Value::Type::Object) return false;
    ClipSidecar s;
    const auto i32 = [&](const char* key, int& dst) {
        int64_t x = 0;
        const json::Value* p = v.get(key);
        if (p && p->asInt64(x) && x >= INT32_MIN && x <= INT32_MAX) dst = static_cast<int>(x);
    };
    const auto u64 = [&](const char* key, uint64_t& dst) {
        const json::Value* p = v.get(key);
        uint64_t x = 0;
        if (p && p->asUInt64(x)) dst = x;
    };
    const auto dbl = [&](const char* key, double& dst) {
        const json::Value* p = v.get(key);
        double x = 0;
        if (p && p->asDouble(x)) dst = x;
    };
    const auto str = [&](const char* key, std::string& dst) {
        const json::Value* p = v.get(key);
        std::string x;
        if (p && p->asString(x)) dst = x;
    };
    i32("version", s.version);
    str("clipId", s.clipId);
    u64("startUnixMs", s.startUnixMs);
    u64("endUnixMs", s.endUnixMs);
    str("startLocal", s.startLocal);
    dbl("durationSec", s.durationSec);
    std::string trig;
    str("trigger", trig);
    s.trigger = clipTriggerFromWire(trig);
    if (const json::Value* by = v.get("startedBy"); by && by->type == json::Value::Type::Object) {
        StartedBy sb;
        std::string x;
        if (const json::Value* p = by->get("deviceId"); p && p->asString(x)) sb.deviceId = x;
        if (const json::Value* p = by->get("deviceName"); p && p->asString(x)) sb.deviceName = x;
        s.startedBy = sb;
    }
    if (const json::Value* arr = v.get("starredBy"); arr && arr->type == json::Value::Type::Array) {
        for (const auto& item : arr->items) {
            if (item.type != json::Value::Type::Object) continue;
            StarEntry e;
            std::string role;
            if (const json::Value* p = item.get("deviceId"); !p || !p->asString(e.deviceId) || e.deviceId.empty()) continue;
            if (const json::Value* p = item.get("role"); p) p->asString(role);
            // An unknown role is read as a viewer's: the least it could have been.
            e.role = petRoleFromString(role) == PetRole::Owner ? PetRole::Owner : PetRole::Viewer;
            const bool dup = std::any_of(s.starredBy.begin(), s.starredBy.end(),
                                         [&](const StarEntry& o) { return o.deviceId == e.deviceId; });
            if (!dup) s.starredBy.push_back(e);
        }
    }
    // A sidecar that says starred without saying by whom keeps its star, as the PC's: a lenient read
    // never loses a star, so retention never deletes a clip someone starred.
    bool starred = false;
    if (const json::Value* p = v.get("starred"); p && p->asBool(starred) && starred && s.starredBy.empty()) {
        s.starredBy.push_back({kPcDeviceId, PetRole::Owner});
    }
    i32("width", s.width);
    i32("height", s.height);
    i32("modeRaw", s.modeRaw);
    i32("sbsLayoutRaw", s.sbsLayoutRaw);
    u64("bytes", s.bytes);
    str("codec", s.codec);
    dbl("preRollSec", s.preRollSec);
    out = std::move(s);
    return true;
}

} // namespace

// ---- names, ids, days, cursors ----------------------------------------------------------------------

LocalTime pcLocalTime(int64_t unixMs) {
    const FILETIME ft = unixMsToFileTime(unixMs);
    SYSTEMTIME utc{}, loc{};
    FileTimeToSystemTime(&ft, &utc);
    LocalTime t;
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) loc = utc;
    FILETIME lft;
    SystemTimeToFileTime(&loc, &lft);
    const int64_t diff = static_cast<int64_t>(fileTimeU64(lft)) - static_cast<int64_t>(fileTimeU64(ft));
    t.year = loc.wYear;
    t.month = loc.wMonth;
    t.day = loc.wDay;
    t.hour = loc.wHour;
    t.minute = loc.wMinute;
    t.second = loc.wSecond;
    t.offsetMinutes = static_cast<int>(diff / 600000000LL);
    return t;
}

std::string dayString(const LocalTime& t) {
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", t.year, t.month, t.day);
    return b;
}

std::string isoLocalString(const LocalTime& t) {
    const int off = t.offsetMinutes < 0 ? -t.offsetMinutes : t.offsetMinutes;
    char b[40];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", t.year, t.month, t.day, t.hour, t.minute,
                  t.second, t.offsetMinutes < 0 ? '-' : '+', off / 60, off % 60);
    return b;
}

std::string clipName(const LocalTime& t, ClipTrigger trigger, int n) {
    if (trigger == ClipTrigger::Unknown || n < 1 || n > kMaxNameSuffix) return {};
    char b[48];
    std::snprintf(b, sizeof(b), "%02d-%02d-%02d_%s", t.hour, t.minute, t.second, toWire(trigger));
    std::string s = b;
    if (n >= 2) s += "-" + std::to_string(n);
    return s;
}

// One rule for days and clip ids, shared with what the wire layer checks on every device request
// (roll_wire.cpp, v3 §7.1): a real date, and the §7.1 pattern exactly.
bool isValidDay(const std::string& day) { return kvhost::isValidRollDay(day); }

bool isValidClipId(const std::string& clipId) { return kvhost::isValidClipId(clipId); }

ClipTrigger triggerOfClipId(const std::string& clipId) {
    if (!isValidClipId(clipId)) return ClipTrigger::Unknown;
    const std::string tail = clipId.substr(20); // after "YYYY-MM-DD/HH-MM-SS_"
    for (ClipTrigger t : {ClipTrigger::Manual, ClipTrigger::Motion, ClipTrigger::Schedule}) {
        const std::string w = toWire(t);
        if (tail.compare(0, w.size(), w) == 0) return t;
    }
    return ClipTrigger::Unknown;
}

std::string encodeClipCursor(uint64_t startUnixMs, const std::string& clipId) {
    return base64UrlEncode(std::to_string(startUnixMs) + ":" + clipId);
}

std::string encodeDayCursor(const std::string& day) { return base64UrlEncode(day); }

std::optional<ClipCursor> decodeClipCursor(const std::string& cursor) {
    const auto raw = base64UrlDecode(cursor);
    if (!raw) return std::nullopt;
    const size_t colon = raw->find(':');
    if (colon == std::string::npos || colon == 0 || colon > 20 || !allDigits(*raw, 0, colon)) return std::nullopt;
    if (colon > 1 && (*raw)[0] == '0') return std::nullopt; // only the form encodeClipCursor writes
    uint64_t v = 0;
    for (size_t i = 0; i < colon; ++i) {
        const uint64_t d = static_cast<uint64_t>((*raw)[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return std::nullopt;
        v = v * 10 + d;
    }
    ClipCursor c;
    c.startUnixMs = v;
    c.clipId = raw->substr(colon + 1);
    if (!isValidClipId(c.clipId)) return std::nullopt;
    return c;
}

std::optional<std::string> decodeDayCursor(const std::string& cursor) {
    const auto raw = base64UrlDecode(cursor);
    if (!raw || !isValidDay(*raw)) return std::nullopt;
    return raw;
}

// ---- the sidecar ------------------------------------------------------------------------------------

std::string sidecarJson(const ClipSidecar& s) { return sidecarObject(s).finish(); }

bool parseSidecar(const std::string& text, ClipSidecar& out) {
    json::Value v;
    if (!json::parse(text, v)) return false;
    return parseSidecarValue(v, out);
}

const char* rollErrorCode(RollError e) {
    switch (e) {
        case RollError::None: return nullptr;
        case RollError::NotFound: return pet_error::kNotFound;
        case RollError::Invalid: return pet_error::kInvalid;
        case RollError::NotAllowedForRole: return pet_error::kNotAllowedForRole;
        case RollError::Internal: break;
    }
    return pet_error::kInternal;
}

// ---- the folder (pet-camera.md §6.5) ----------------------------------------------------------------

namespace {

std::wstring lowerTrim(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool isInsideOrSame(const std::wstring& p, const std::wstring& base) {
    if (base.empty()) return false;
    return p == base || (p.size() > base.size() && p.compare(0, base.size(), base) == 0 && p[base.size()] == L'\\');
}

// The path with its existing part resolved through links and junctions (GetFinalPathNameByHandleW),
// lower-cased. A folder that doesn't exist yet resolves through its nearest existing parent.
std::wstring resolvedLower(const std::wstring& path) {
    std::wstring full = fullPathLower(path);
    std::wstring head = full, tail;
    while (!head.empty() && !pathExists(head)) {
        const size_t cut = head.find_last_of(L'\\');
        if (cut == std::wstring::npos) return full;
        tail = head.substr(cut) + tail;
        head = head.substr(0, cut);
    }
    if (head.size() == 2 && head[1] == L':') head += L'\\';
    HANDLE h = CreateFileW(head.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return full;
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 2, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= MAX_PATH * 2) return full;
    std::wstring r(buf, n);
    if (r.compare(0, 4, L"\\\\?\\") == 0) r = r.substr(4);
    return lowerTrim(r + tail);
}

bool isAllowedRootEntry(const std::wstring& name, bool isDir) {
    const std::wstring l = lowerTrim(name);
    if (isDir) return isValidDay(narrow(name));
    // Explorer's own files may appear in any folder; they are never content.
    return l == L".nosync" || l == L".metadata_never_index" || l == L"deletion-log.txt" || l == L"desktop.ini" ||
           l == L"thumbs.db" || l == kTestRollMarker;
}

} // namespace

namespace {
bool checkRollFolderFull(const std::wstring& dir, const std::wstring& recordingsDir, std::string* err,
                         const FolderCheckOptions& opts, std::wstring* fullOut) {
    const auto refuse = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    if (dir.empty()) return refuse("no camera-roll folder");
    // A UNC path (\\server\share, \\?\UNC\...) or any other \\ form is a network or device path.
    if (dir.size() >= 2 && (dir[0] == L'\\' || dir[0] == L'/') && (dir[1] == L'\\' || dir[1] == L'/')) {
        return refuse("refusing a network path for the camera roll: " + narrow(dir));
    }
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(dir.c_str(), MAX_PATH * 2, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 2) return refuse("cannot resolve the camera-roll folder: " + narrow(dir));
    std::wstring full(buf, n);
    while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) full.pop_back();
    if (full.size() < 3 || full[1] != L':' || full[2] != L'\\') {
        return refuse("the camera roll needs a folder on a local drive: " + narrow(dir));
    }
    const std::wstring root = full.substr(0, 3);
    const unsigned type = opts.driveType ? opts.driveType(root) : GetDriveTypeW(root.c_str());
    if (type == DRIVE_REMOTE) return refuse("refusing a network drive for the camera roll: " + narrow(dir));
    if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) return refuse("no such drive: " + narrow(dir));
    if (isUnderOneDrive(full, opts.extraOneDriveRoots)) {
        return refuse("refusing to keep the camera roll inside OneDrive: " + narrow(dir));
    }
    // The depth/IR folder's purge deletes anything in it that isn't an IR or depth session (§6.5), and
    // the roll's own owner-only DACL would spread over the recordings if they were inside it.
    if (!recordingsDir.empty()) {
        const std::wstring p = fullPathLower(full), r = fullPathLower(recordingsDir);
        const std::wstring pr = resolvedLower(full), rr = resolvedLower(recordingsDir);
        if (isInsideOrSame(p, r) || isInsideOrSame(pr, rr)) {
            return refuse("refusing the depth/IR recordings folder, or a folder inside it, for the camera roll: " +
                          narrow(dir));
        }
        if (isInsideOrSame(r, p) || isInsideOrSame(rr, pr)) {
            return refuse("refusing a camera-roll folder that holds the depth/IR recordings folder: " + narrow(dir));
        }
    }
    const DWORD attrs = GetFileAttributesW(full.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return refuse("not a folder: " + narrow(dir));
        if (isReparse(attrs)) return refuse("refusing a link or junction for the camera roll: " + narrow(dir));
        // Making the folder owner-only reaches every file already in it, and retention deletes clips;
        // so a folder that holds anything else (a home folder, Documents) is refused.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((full + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            bool foreign = false;
            do {
                const std::wstring e = fd.cFileName;
                if (e == L"." || e == L"..") continue;
                const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (isReparse(fd.dwFileAttributes) || !isAllowedRootEntry(e, isDir)) foreign = true;
            } while (!foreign && FindNextFileW(h, &fd));
            FindClose(h);
            if (foreign) return refuse("this folder holds other files; choose an empty folder for the camera roll: " +
                                       narrow(dir));
        }
    }
    if (fullOut) *fullOut = full;
    return true;
}
} // namespace

namespace {
bool notFound(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }

// The folder as the file system resolves it (GetFinalPathNameByHandleW: junctions, links, subst drives and 8.3
// names), through the nearest part of the path that is there; the parts not made yet follow it. Only "not found"
// moves up a level: any other failure is nullopt, with the reason in why.
std::optional<std::wstring> resolvedTestFolder(const std::wstring& dir, std::string* why) {
    std::wstring head(MAX_PATH * 2, L'\0');
    const DWORD fn = GetFullPathNameW(dir.c_str(), static_cast<DWORD>(head.size()), head.data(), nullptr);
    if (fn == 0 || fn >= head.size()) {
        *why = "its full path couldn't be made (Win32 error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    head.resize(fn);
    std::wstring tail;
    while (head.size() > 3 && (head.back() == L'\\' || head.back() == L'/')) head.pop_back();
    for (;;) {
        if (GetFileAttributesW(head.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        const DWORD e = GetLastError();
        const size_t cut = head.find_last_of(L"\\/");
        if (!notFound(e) || cut == std::wstring::npos || cut < 2) {
            *why = "its attributes couldn't be read (Win32 error " + std::to_string(e) + ")";
            return std::nullopt;
        }
        tail = head.substr(cut + 1) + (tail.empty() ? L"" : L"\\" + tail);
        head = head.substr(0, cut);
        if (head.size() == 2 && head[1] == L':') head += L'\\'; // "c:" is the drive's current folder; "c:\" its root
    }
    HANDLE h = CreateFileW(head.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = "it couldn't be opened to resolve it (Win32 error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    std::wstring buf(1024, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buf.size()) {
        buf.assign(static_cast<size_t>(n) + 1, L'\0');
        n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    const DWORD e = GetLastError();
    CloseHandle(h);
    if (n == 0 || n >= buf.size()) {
        *why = "it couldn't be resolved (Win32 error " + std::to_string(e) + ")";
        return std::nullopt;
    }
    buf.resize(n);
    // "\\?\C:\x" is "C:\x"; "\\?\UNC\server\share" is "\\server\share".
    if (buf.rfind(L"\\\\?\\UNC\\", 0) == 0) buf = L"\\\\" + buf.substr(8);
    else if (buf.rfind(L"\\\\?\\", 0) == 0) buf = buf.substr(4);
    if (!tail.empty()) buf += (buf.back() == L'\\' ? L"" : L"\\") + tail;
    return buf;
}

// A file in a folder: Yes, No ("not found"), or Unknown (any other failure).
enum class Has { Yes, No, Unknown };
Has hasFile(const std::wstring& folder, const wchar_t* name) {
    const std::wstring p = folder + (folder.back() == L'\\' ? L"" : L"\\") + name;
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return Has::Yes;
    return notFound(GetLastError()) ? Has::No : Has::Unknown;
}
} // namespace

bool checkTestRollFolder(const std::wstring& dir, std::string* err) {
    auto refuse = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    // Only "not found" makes the folder new. Any other failure to read its attributes (no access, a bad name, a
    // device error) may hide a real roll, so it refuses (fail closed).
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    const DWORD attrs_error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    if (attrs == INVALID_FILE_ATTRIBUTES && !notFound(attrs_error)) {
        return refuse("a test run's camera-roll folder must be one whose attributes can be read (Win32 error " +
                      std::to_string(attrs_error) + "); refusing " + narrow(dir));
    }
    // Never inside a camera roll that isn't a test roll, whatever name the folder is given: on the path as the file
    // system resolves it (a junction, a subst drive or an 8.3 name can reach into a real roll), no folder above it
    // may hold the roll's markers (pet-camera.md §6.5: .nosync, .metadata_never_index) without the test marker.
    // Those three names are all that is looked up there.
    std::string why;
    const std::optional<std::wstring> real = resolvedTestFolder(dir, &why);
    if (!real) return refuse("a test run's camera-roll folder couldn't be checked: " + why + "; refusing " + narrow(dir));
    std::wstring up = *real;
    for (;;) {
        const size_t cut = up.find_last_of(L'\\');
        if (cut == std::wstring::npos || cut < 2) break;
        up = up.substr(0, cut);
        const std::wstring folder = (up.size() == 2 && up[1] == L':') ? up + L"\\" : up;
        const Has nosync = hasFile(folder, L".nosync"), noindex = hasFile(folder, L".metadata_never_index");
        if (nosync == Has::No && noindex == Has::No) continue;
        const Has marker = hasFile(folder, kTestRollMarker);
        if (marker == Has::Yes && nosync != Has::Unknown && noindex != Has::Unknown) continue; // inside a test roll
        return refuse("a test run never records inside a camera roll that isn't a test roll: " + narrow(folder) +
                      " holds the roll's markers and no " + narrow(kTestRollMarker) + "; refusing " + narrow(dir) +
                      (_wcsicmp(real->c_str(), dir.c_str()) != 0 ? " (it resolves to " + narrow(*real) + ")" : ""));
    }
    if (attrs == INVALID_FILE_ATTRIBUTES) return true; // new: the host makes it, and marks it
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return refuse("not a folder: " + narrow(dir));
    if (pathExists(dir + L"\\" + kTestRollMarker)) return true; // a test roll this host (or an earlier test) made
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    bool empty = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n != L"." && n != L"..") empty = false;
        } while (empty && FindNextFileW(h, &fd));
        FindClose(h);
    } else {
        const DWORD e = GetLastError();
        if (!notFound(e)) {
            return refuse("a test run's camera-roll folder couldn't be listed (Win32 error " + std::to_string(e) +
                          "); refusing " + narrow(dir));
        }
    }
    if (empty) return true;
    return refuse("a test run records only into a new or empty folder, or a test roll (one holding " +
                  narrow(kTestRollMarker) + "); refusing " + narrow(dir));
}

bool writeTestRollMarker(const std::wstring& dir, std::string* err) {
    HANDLE h = CreateFileW((dir + L"\\" + kTestRollMarker).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = lastErrorText("can't mark the test roll");
        return false;
    }
    CloseHandle(h);
    return true;
}

bool checkRollFolder(const std::wstring& dir, const std::wstring& recordingsDir, std::string* err,
                     const FolderCheckOptions& opts) {
    return checkRollFolderFull(dir, recordingsDir, err, opts, nullptr);
}

bool prepareRollFolder(const std::wstring& dir, const std::wstring& recordingsDir, std::string* err,
                       const FolderCheckOptions& opts) {
    std::wstring full;
    if (!checkRollFolderFull(dir, recordingsDir, err, opts, &full)) return false;
    return protectPrivateFolder(full, err);
}

// ---- the store ----------------------------------------------------------------------------------------

struct RollStore::ClipFiles {
    bool mp4 = false, json = false, jpg = false;
    uint64_t mp4Bytes = 0, jsonBytes = 0, jpgBytes = 0;
    uint64_t jsonWriteTime = 0; // FILETIME
    uint64_t mp4CreateTime = 0; // FILETIME
    bool complete() const { return mp4 && json; } // a sidecar is written last (§6.3)
    uint64_t total() const { return mp4Bytes + jsonBytes + jpgBytes; }
};

struct RollStore::DaySnapshot {
    std::string day;
    std::wstring dir;
    std::map<std::string, ClipFiles> clips; // by name; only names that pass the §7.1 pattern
    bool hasIndex = false;
    uint64_t indexWriteTime = 0;
    std::vector<std::wstring> tmpFiles;
};

RollStore::RollStore(RollConfig cfg) : cfg_(std::move(cfg)) {
    while (cfg_.dir.size() > 3 && (cfg_.dir.back() == L'\\' || cfg_.dir.back() == L'/')) cfg_.dir.pop_back();
    if (cfg_.keepDays < 1) cfg_.keepDays = 1;
    counts_.capBytes = cfg_.capBytes;
    counts_.keepDays = cfg_.keepDays;
}

void RollStore::setLimits(uint64_t capBytes, int keepDays) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.capBytes = capBytes;
    cfg_.keepDays = keepDays < 1 ? 1 : keepDays;
    limitsSet_ = true;
    std::lock_guard<std::mutex> c(countsM_);
    counts_.capBytes = cfg_.capBytes;
    counts_.keepDays = cfg_.keepDays;
    counts_.limitsSet = true;
}

RollStorageCounts RollStore::counts() const {
    std::lock_guard<std::mutex> c(countsM_);
    return counts_;
}

void RollStore::recount() {
    std::lock_guard<std::mutex> lk(m_);
    recountLocked();
}

void RollStore::recountLocked() {
    uint64_t starred = 0;
    for (const auto& day : dayFolders()) {
        const DaySnapshot snap = snapshotDay(day);
        for (const auto& s : clipsOfDay(snap, nullptr)) {
            if (s.starred()) starred += snap.clips.at(s.clipId.substr(11)).total();
        }
    }
    const uint64_t used = treeBytes(cfg_.dir);
    const std::optional<uint64_t> free = readFreeBytes();
    std::lock_guard<std::mutex> c(countsM_);
    counts_.counted = true;
    counts_.usedBytes = used;
    counts_.starredBytes = starred;
    if (free) counts_.freeBytes = free;
    counts_.walkedAtMs = GetTickCount64();
}

void RollStore::addUsedBytes(int64_t delta) {
    std::lock_guard<std::mutex> c(countsM_);
    if (delta < 0) {
        const uint64_t d = static_cast<uint64_t>(-delta);
        counts_.usedBytes = d > counts_.usedBytes ? 0 : counts_.usedBytes - d;
    } else {
        counts_.usedBytes += static_cast<uint64_t>(delta);
    }
}

int64_t RollStore::now() const {
    if (cfg_.nowUnixMs) return cfg_.nowUnixMs();
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return fileTimeToUnixMs(fileTimeU64(ft));
}

LocalTime RollStore::local(int64_t unixMs) const { return cfg_.toLocal ? cfg_.toLocal(unixMs) : pcLocalTime(unixMs); }

std::vector<std::string> RollStore::dayFolders() const {
    std::vector<std::string> days;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((cfg_.dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return days;
    do {
        // A junction or link named like a day is never followed: retention would delete through it.
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || isReparse(fd.dwFileAttributes)) continue;
        const std::string n = narrow(fd.cFileName);
        if (isValidDay(n)) days.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(days.begin(), days.end());
    return days;
}

RollStore::DaySnapshot RollStore::snapshotDay(const std::string& day) const {
    DaySnapshot s;
    s.day = day;
    s.dir = cfg_.dir + L"\\" + widen(day);
    // A day that is a junction or link is never read through: it could lead anywhere (§6.5).
    if (!isRealDir(s.dir)) return s;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((s.dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return s;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || isReparse(fd.dwFileAttributes)) continue;
        const std::wstring wn = fd.cFileName;
        const std::string n = narrow(wn);
        const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (n == "index.json") {
            s.hasIndex = true;
            s.indexWriteTime = fileTimeU64(fd.ftLastWriteTime);
            continue;
        }
        if (endsWith(n, ".tmp")) {
            s.tmpFiles.push_back(s.dir + L"\\" + wn);
            continue;
        }
        for (const char* ext : {".mp4", ".json", ".jpg"}) {
            if (!endsWith(n, ext)) continue;
            const std::string stem = n.substr(0, n.size() - std::char_traits<char>::length(ext));
            if (!isValidClipName(stem)) break;
            ClipFiles& f = s.clips[stem];
            if (ext[1] == 'm') {
                f.mp4 = true;
                f.mp4Bytes = size;
                f.mp4CreateTime = fileTimeU64(fd.ftCreationTime);
            } else if (ext[2] == 's') {
                f.json = true;
                f.jsonBytes = size;
                f.jsonWriteTime = fileTimeU64(fd.ftLastWriteTime);
            } else {
                f.jpg = true;
                f.jpgBytes = size;
            }
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return s;
}

// The day's finalized clips, from index.json when it is fresh, otherwise from the sidecars (§6.3). The
// index is fresh only when it is strictly newer than every sidecar and records each sidecar's write
// time and size exactly, for exactly the clips on disk; so a star, a delete, a clip added or a clock
// change all make it stale. Nothing is written here.
std::vector<ClipSidecar> RollStore::clipsOfDay(const DaySnapshot& snap, bool* indexFresh,
                                              std::set<std::string>* unreadable) const {
    if (indexFresh) *indexFresh = false;
    std::set<std::string> complete;
    uint64_t newest = 0;
    for (const auto& [name, f] : snap.clips) {
        if (!f.complete()) continue;
        complete.insert(name);
        newest = std::max(newest, f.jsonWriteTime);
    }
    std::vector<ClipSidecar> out;
    if (snap.hasIndex && snap.indexWriteTime > newest) {
        std::string text;
        json::Value v;
        int64_t ver = 0;
        std::string day;
        const json::Value* arr = nullptr;
        // Our own file, read with the raised container limit: a day may hold more than 1024 clips.
        if (readFile(snap.dir + L"\\" + kIndexName, text) && json::parse(text.data(), text.size(), v, json::kLocalIndexLimits) &&
            v.type == json::Value::Type::Object && v.get("version") && v.get("version")->asInt64(ver) && ver == 1 &&
            json::getString(v, "day", day) && day == snap.day && (arr = v.get("clips")) != nullptr &&
            arr->type == json::Value::Type::Array) {
            std::set<std::string> seen;
            bool ok = true;
            for (const auto& item : arr->items) {
                uint64_t wt = 0, bytes = 0;
                const json::Value* sc = nullptr;
                ClipSidecar s;
                if (item.type != json::Value::Type::Object || !json::getU64(item, "sidecarWriteTime", wt) ||
                    !json::getU64(item, "sidecarBytes", bytes) || !json::getObject(item, "sidecar", sc) ||
                    !parseSidecarValue(*sc, s) || !isValidClipId(s.clipId) || s.clipId.compare(0, 10, snap.day) != 0) {
                    ok = false;
                    break;
                }
                const std::string name = s.clipId.substr(11);
                const auto it = snap.clips.find(name);
                if (!complete.count(name) || !seen.insert(name).second || it == snap.clips.end() ||
                    it->second.jsonWriteTime != wt || it->second.jsonBytes != bytes) {
                    ok = false;
                    break;
                }
                out.push_back(std::move(s));
            }
            if (ok && seen.size() == complete.size()) {
                if (indexFresh) *indexFresh = true;
                return out;
            }
            out.clear();
        }
    }
    for (const auto& name : complete) {
        const ClipFiles& f = snap.clips.at(name);
        ClipSidecar s;
        std::string text;
        if (!readFile(snap.dir + L"\\" + widen(name) + L".json", text)) {
            // A sidecar that can't be READ (another program holds it) says nothing about the clip, its stars
            // least of all. It is listed with what its name gives, but never counted as unstarred: retention
            // skips it, and no index is written from this read.
            if (unreadable) unreadable->insert(name);
            s = ClipSidecar{};
        } else if (!parseSidecar(text, s)) {
            s = ClipSidecar{};
        }
        s.clipId = snap.day + "/" + name;
        // A sidecar too damaged to give its start falls back to the .mp4's creation time, the clip's
        // start (§6.1), so it still sorts and still ages out.
        if (s.startUnixMs == 0) s.startUnixMs = static_cast<uint64_t>(std::max<int64_t>(0, fileTimeToUnixMs(f.mp4CreateTime)));
        s.trigger = triggerOfClipId(s.clipId); // the name is the record (finalizeClip keeps them equal)
        out.push_back(std::move(s));
    }
    return out;
}

bool RollStore::writeIndex(const DaySnapshot& snap, const std::vector<ClipSidecar>& clips, std::string* err) const {
    json::ArrayWriter arr;
    for (const auto& c : clips) {
        const auto it = snap.clips.find(c.clipId.substr(11));
        if (it == snap.clips.end()) continue;
        json::ObjectWriter e;
        e.field("sidecarWriteTime", it->second.jsonWriteTime)
            .field("sidecarBytes", it->second.jsonBytes)
            .object("sidecar", sidecarObject(c));
        arr.object(e);
    }
    json::ObjectWriter w;
    w.field("version", 1).field("day", snap.day).array("clips", arr);
    return writeFileAtomically(snap.dir, kIndexName, w.finish(), err);
}

bool RollStore::writeSidecar(const std::wstring& dayDir, const std::string& name, const ClipSidecar& s,
                             std::string* err) const {
    return writeFileAtomically(dayDir, widen(name) + L".json", sidecarJson(s), err);
}

// The .mp4 first, then the thumbnail, then the sidecar. An .mp4 that can't be removed (a player holds it)
// stops the delete with nothing gone: the clip stays whole, thumbnail included, rather than half-deleted. A delete
// cut short after the .mp4 leaves a sidecar without its .mp4, which start-up removes, and never an .mp4 without a
// sidecar, which it would bring back.
uint64_t RollStore::removeClipFiles(const std::wstring& dayDir, const std::string& name, bool* complete) const {
    uint64_t removed = 0;
    bool all = true;
    for (const wchar_t* ext : {L".mp4", L".jpg", L".json"}) {
        const std::wstring p = dayDir + L"\\" + widen(name) + ext;
        const uint64_t size = fileBytes(p);
        if (removeFile(p)) removed += size;
        else all = false;
        if (!all) break;
    }
    if (complete) *complete = all;
    return removed;
}

// One line per deletion in the roll's deletion-log.txt, in the depth/IR log's format
// (recordings.cpp deletionLogLine): "<ISO-8601 UTC>\t<clip id>\t<N> bytes\t<automatic|user>", with
// "\tincomplete" when only part of the clip could be removed. Nothing about the content.
void RollStore::logDeletion(const std::string& clipId, uint64_t bytes, bool user, bool complete) const {
    std::string line = isoUtcSeconds(now() / 1000) + "\t" + clipId + "\t" + std::to_string(bytes) + " bytes\t" +
                       (user ? "user" : "automatic");
    if (!complete) line += "\tincomplete";
    line += "\n";
    std::ofstream(cfg_.dir + L"\\" + kDeletionLogName, std::ios::binary | std::ios::app) << line;
}

std::optional<std::pair<std::string, std::string>> RollStore::splitValidId(const std::string& clipId) const {
    if (!isValidClipId(clipId)) return std::nullopt;
    return std::make_pair(clipId.substr(0, 10), clipId.substr(11));
}

// ---- writing a clip -------------------------------------------------------------------------------------

std::optional<ClipPaths> RollStore::reserveClip(int64_t startUnixMs, ClipTrigger trigger, std::string* err) {
    std::lock_guard<std::mutex> lk(m_);
    if (trigger == ClipTrigger::Unknown) {
        if (err) *err = "unknown trigger";
        return std::nullopt;
    }
    const LocalTime t = local(startUnixMs);
    const std::string day = dayString(t);
    if (!isValidDay(day)) {
        if (err) *err = "no valid day for that time";
        return std::nullopt;
    }
    const std::wstring dayDir = cfg_.dir + L"\\" + widen(day);
    if (!CreateDirectoryW(dayDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (err) *err = lastErrorText("CreateDirectoryW day folder");
        return std::nullopt;
    }
    if (!isRealDir(dayDir)) {
        if (err) *err = "the day folder is not a plain folder";
        return std::nullopt;
    }
    for (int n = 1; n <= kMaxNameSuffix; ++n) {
        const std::string name = clipName(t, trigger, n);
        ClipPaths p;
        p.clipId = day + "/" + name;
        if (!isValidClipId(p.clipId)) break;
        p.dayDir = dayDir;
        p.mp4 = dayDir + L"\\" + widen(name) + L".mp4";
        p.sidecar = dayDir + L"\\" + widen(name) + L".json";
        p.thumbnail = dayDir + L"\\" + widen(name) + L".jpg";
        if (pathExists(p.sidecar) || pathExists(p.thumbnail)) continue;
        // CREATE_NEW takes the name atomically; the writer then opens the file over it.
        HANDLE h = CreateFileW(p.mp4.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return p;
        }
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) {
            if (err) *err = lastErrorText("CreateFileW .mp4");
            return std::nullopt;
        }
    }
    if (err) *err = "every name for that second is taken";
    return std::nullopt;
}

bool RollStore::finalizeClip(const ClipPaths& paths, ClipSidecar sidecar, std::string* err) {
    std::lock_guard<std::mutex> lk(m_);
    const auto parts = splitValidId(paths.clipId);
    if (!parts) {
        if (err) *err = "invalid clip id";
        return false;
    }
    const std::wstring dayDir = cfg_.dir + L"\\" + widen(parts->first);
    const std::wstring mp4 = dayDir + L"\\" + widen(parts->second) + L".mp4";
    if (!pathExists(mp4)) {
        if (err) *err = "the clip's .mp4 is missing";
        return false;
    }
    sidecar.version = 1;
    sidecar.clipId = paths.clipId;
    sidecar.trigger = triggerOfClipId(paths.clipId); // the name and the field always agree
    sidecar.startLocal = isoLocalString(local(static_cast<int64_t>(sidecar.startUnixMs)));
    sidecar.bytes = fileBytes(mp4);
    sidecar.codec = "h264";
    if (sidecar.endUnixMs == 0) {
        sidecar.endUnixMs = sidecar.startUnixMs + static_cast<uint64_t>(std::max(0.0, sidecar.durationSec) * 1000.0);
    }
    if (sidecar.trigger != ClipTrigger::Manual) sidecar.startedBy.reset();
    if (!writeSidecar(dayDir, parts->second, sidecar, err)) return false;
    // The clip's files join the cached size (its thumbnail was written before the sidecar).
    addUsedBytes(static_cast<int64_t>(sidecar.bytes + fileBytes(dayDir + L"\\" + widen(parts->second) + L".json") +
                                      fileBytes(dayDir + L"\\" + widen(parts->second) + L".jpg")));
    return true;
}

void RollStore::abandonClip(const ClipPaths& paths) {
    std::lock_guard<std::mutex> lk(m_);
    const auto parts = splitValidId(paths.clipId);
    if (!parts) return;
    bool complete = true;
    const uint64_t removed = removeClipFiles(cfg_.dir + L"\\" + widen(parts->first), parts->second, &complete);
    // An empty reservation held nothing; anything written is logged like every other deletion.
    if (removed > 0 || !complete) logDeletion(paths.clipId, removed, false, complete);
}

// ---- passes -----------------------------------------------------------------------------------------

PassReport RollStore::startupPass() {
    std::lock_guard<std::mutex> lk(m_);
    PassReport report;
    for (const auto& day : dayFolders()) {
        DaySnapshot snap = snapshotDay(day);
        for (const auto& t : snap.tmpFiles) removeFile(t); // an atomic write cut short
        for (const auto& [name, f] : snap.clips) {
            const std::string id = day + "/" + name;
            if (f.json && !f.mp4) {
                // A delete cut short (removeClipFiles' order): finish it and log it.
                bool complete = true;
                const uint64_t removed = removeClipFiles(snap.dir, name, &complete);
                logDeletion(id, removed, false, complete);
                if (complete) report.deleted.push_back(id);
            } else if (!f.json && !f.mp4 && f.jpg) {
                bool complete = true;
                const uint64_t removed = removeClipFiles(snap.dir, name, &complete);
                logDeletion(id, removed, false, complete);
                if (complete) report.deleted.push_back(id);
            } else if (f.mp4 && !f.json) {
                // An interrupted clip (§6.3): its sidecar is rebuilt if roll_media can read it.
                const std::wstring mp4 = snap.dir + L"\\" + widen(name) + L".mp4";
                std::optional<ProbedClip> probed;
                if (cfg_.probe && f.mp4Bytes > 0) probed = cfg_.probe(mp4);
                if (probed) {
                    ClipSidecar s;
                    s.clipId = id;
                    s.trigger = triggerOfClipId(id);
                    s.startUnixMs = probed->startUnixMs
                                        ? *probed->startUnixMs
                                        : static_cast<uint64_t>(std::max<int64_t>(0, fileTimeToUnixMs(f.mp4CreateTime)));
                    s.durationSec = probed->durationSec;
                    s.endUnixMs = s.startUnixMs + static_cast<uint64_t>(std::max(0.0, probed->durationSec) * 1000.0);
                    s.startLocal = isoLocalString(local(static_cast<int64_t>(s.startUnixMs)));
                    s.width = probed->width;
                    s.height = probed->height;
                    s.modeRaw = probed->modeRaw;
                    s.sbsLayoutRaw = probed->sbsLayoutRaw;
                    s.bytes = f.mp4Bytes;
                    if (writeSidecar(snap.dir, name, s, nullptr)) {
                        report.recovered.push_back(id);
                        continue;
                    }
                }
                bool complete = true;
                const uint64_t removed = removeClipFiles(snap.dir, name, &complete);
                logDeletion(id, removed, false, complete);
                if (complete) report.deleted.push_back(id);
            }
        }
    }
    // Recovery and indexing only, never retention: at start-up the user's keep days and cap aren't known
    // yet (pet-camera.json is read after the roll starts, and may be unreadable), and the defaults must
    // never delete a clip (pet-camera.md §5 fail closed). The host runs retentionPass once it has them.
    tidyDaysLocked(report);
    recountLocked(); // the cached counts start from what the pass left
    return report;
}

PassReport RollStore::retentionPass() {
    std::lock_guard<std::mutex> lk(m_);
    PassReport report;
    runRetentionLocked(report);
    return report;
}

void RollStore::runRetentionLocked(PassReport& report) {
    struct Item {
        std::string id, name;
        std::wstring dayDir;
        uint64_t start = 0;
        bool starred = false;
        uint64_t bytes = 0;
    };
    std::vector<Item> items;
    for (const auto& day : dayFolders()) {
        const DaySnapshot snap = snapshotDay(day);
        std::set<std::string> unreadable;
        for (const auto& s : clipsOfDay(snap, nullptr, &unreadable)) {
            // A clip whose sidecar can't be read may be starred: it is never deleted on a guess.
            if (unreadable.count(s.clipId.substr(11))) continue;
            Item it;
            it.id = s.clipId;
            it.name = s.clipId.substr(11);
            it.dayDir = snap.dir;
            it.start = s.startUnixMs;
            it.starred = s.starred();
            it.bytes = snap.clips.at(it.name).total();
            items.push_back(std::move(it));
        }
    }
    // Oldest first; within a millisecond, the id decides, so the order is stable.
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.start != b.start ? a.start < b.start : a.id < b.id; });
    const auto drop = [&](Item& it) {
        bool complete = true;
        const uint64_t removed = removeClipFiles(it.dayDir, it.name, &complete);
        logDeletion(it.id, removed, false, complete);
        if (complete) report.deleted.push_back(it.id);
        return removed;
    };

    // 1. Unstarred clips older than keep days. Starred clips are never deleted automatically (D11).
    const int64_t nowMs = now();
    const int64_t keepMs = static_cast<int64_t>(cfg_.keepDays) * 86400000LL;
    std::vector<Item> kept;
    for (auto& it : items) {
        // A clip being played is skipped, never deleted under its player; the next pass tries again.
        if (!it.starred && nowMs >= static_cast<int64_t>(it.start) + keepMs && !pinnedLocked(it.id)) drop(it);
        else kept.push_back(std::move(it));
    }
    // 2. While the roll is over the cap, the oldest unstarred clips. The disk floor plays no part here
    // (§6.4): it never deletes beyond the cap and the day limit.
    uint64_t used = treeBytes(cfg_.dir);
    for (auto& it : kept) {
        if (used <= cfg_.capBytes) break;
        if (it.starred || pinnedLocked(it.id)) continue;
        const uint64_t removed = drop(it);
        used = removed > used ? 0 : used - removed;
        it.bytes = 0;
        it.id.clear();
    }
    // 3. Starred clips alone over the cap stop recording (storage_full).
    uint64_t starredBytes = 0;
    for (const auto& it : kept) {
        if (it.starred && !it.id.empty()) starredBytes += it.bytes;
    }
    storageFull_ = limitsSet_ && starredBytes > cfg_.capBytes;
    ++flagsGen_;
    report.storageFull = storageFull_;

    tidyDaysLocked(report);
    {
        std::lock_guard<std::mutex> c(countsM_);
        counts_.usedBytes = used;
        counts_.starredBytes = starredBytes;
    }
}

void RollStore::tidyDaysLocked(PassReport& report) {
    // 4. Empty day folders go; 5. every day index left stale is rebuilt from its sidecars.
    for (const auto& day : dayFolders()) {
        DaySnapshot snap = snapshotDay(day);
        if (snap.clips.empty()) {
            removeFile(snap.dir + L"\\" + kIndexName);
            for (const auto& t : snap.tmpFiles) removeFile(t);
            if (RemoveDirectoryW(snap.dir.c_str())) ++report.dayFoldersRemoved;
            continue;
        }
        bool fresh = false;
        std::set<std::string> unreadable;
        const auto clips = clipsOfDay(snap, &fresh, &unreadable);
        // An index is never written from a sidecar that couldn't be read; the old one stays until a pass
        // can read them all.
        if (!fresh && unreadable.empty() && writeIndex(snap, clips, nullptr)) ++report.indexesRebuilt;
    }
}

void RollStore::refreshStorageFullLocked() {
    uint64_t starredBytes = 0;
    for (const auto& day : dayFolders()) {
        const DaySnapshot snap = snapshotDay(day);
        for (const auto& s : clipsOfDay(snap, nullptr)) {
            if (s.starred()) starredBytes += snap.clips.at(s.clipId.substr(11)).total();
        }
    }
    // pet-camera.md §6.4: storage_full is measured against the owner's cap only. Until the limits are known
    // the default cap is nobody's choice, and only the disk floor stops recording.
    storageFull_ = limitsSet_ && starredBytes > cfg_.capBytes;
    ++flagsGen_;
    std::lock_guard<std::mutex> c(countsM_);
    counts_.starredBytes = starredBytes;
}

// ---- listing ----------------------------------------------------------------------------------------

DayListResult RollStore::listDays(const std::optional<std::string>& cursor, std::optional<int> limit) const {
    DayListResult r;
    if (limit && (*limit < 1 || *limit > kListMaxLimit)) {
        r.error = RollError::Invalid;
        return r;
    }
    std::optional<std::string> after;
    if (cursor) {
        after = decodeDayCursor(*cursor);
        if (!after) {
            r.error = RollError::Invalid;
            return r;
        }
    }
    const size_t want = static_cast<size_t>(limit.value_or(kListDefaultLimit));
    std::lock_guard<std::mutex> lk(m_);
    auto days = dayFolders();
    std::reverse(days.begin(), days.end()); // newest first
    for (const auto& day : days) {
        if (after && day >= *after) continue; // paging continues after the anchor, which may be gone
        const DaySnapshot snap = snapshotDay(day);
        const auto clips = clipsOfDay(snap, nullptr);
        if (clips.empty()) continue;
        if (r.days.size() == want) {
            r.nextCursor = encodeDayCursor(r.days.back().day);
            break;
        }
        RollDayItem d;
        d.day = day;
        d.clipCount = static_cast<int>(clips.size());
        for (const auto& c : clips) {
            if (c.starred()) ++d.starredCount;
            d.bytes += snap.clips.at(c.clipId.substr(11)).mp4Bytes;
        }
        r.days.push_back(std::move(d));
    }
    return r;
}

ClipListResult RollStore::listClips(const std::string& day, const std::optional<std::string>& cursor,
                                    std::optional<int> limit, const std::string& requesterDeviceId) const {
    ClipListResult r;
    if (!isValidDay(day)) {
        r.error = RollError::NotFound;
        return r;
    }
    if (limit && (*limit < 1 || *limit > kListMaxLimit)) {
        r.error = RollError::Invalid;
        return r;
    }
    std::optional<ClipCursor> after;
    if (cursor) {
        after = decodeClipCursor(*cursor);
        if (!after) {
            r.error = RollError::Invalid;
            return r;
        }
    }
    const size_t want = static_cast<size_t>(limit.value_or(kListDefaultLimit));
    std::lock_guard<std::mutex> lk(m_);
    const std::wstring dayDir = cfg_.dir + L"\\" + widen(day);
    if (!isRealDir(dayDir)) {
        r.error = RollError::NotFound;
        return r;
    }
    const DaySnapshot snap = snapshotDay(day);
    auto clips = clipsOfDay(snap, nullptr);
    std::sort(clips.begin(), clips.end(), [](const ClipSidecar& a, const ClipSidecar& b) {
        return a.startUnixMs != b.startUnixMs ? a.startUnixMs > b.startUnixMs : a.clipId > b.clipId;
    });
    for (const auto& c : clips) {
        if (after && !(c.startUnixMs < after->startUnixMs ||
                       (c.startUnixMs == after->startUnixMs && c.clipId < after->clipId))) {
            continue;
        }
        if (r.clips.size() == want) {
            r.nextCursor = encodeClipCursor(r.clips.back().startUnixMs, r.clips.back().clipId);
            break;
        }
        const ClipFiles& f = snap.clips.at(c.clipId.substr(11));
        RollClipItem it;
        it.clipId = c.clipId;
        it.startUnixMs = c.startUnixMs;
        it.startLocal = c.startLocal.empty() ? isoLocalString(local(static_cast<int64_t>(c.startUnixMs))) : c.startLocal;
        it.durationSec = c.durationSec;
        it.trigger = c.trigger;
        if (c.trigger == ClipTrigger::Manual && c.startedBy) it.startedBy = c.startedBy->deviceName;
        it.starred = c.starred();
        it.starredByMe = !requesterDeviceId.empty() &&
                         std::any_of(c.starredBy.begin(), c.starredBy.end(),
                                     [&](const StarEntry& e) { return e.deviceId == requesterDeviceId; });
        it.width = c.width;
        it.height = c.height;
        it.bytes = f.mp4Bytes;
        it.hasThumbnail = f.jpg;
        r.clips.push_back(std::move(it));
    }
    return r;
}

RollStorageInfo RollStore::storage() const {
    // From the cached counts, never a walk of the tree (a phone's 0x12 asks for this on every page) and
    // never the store's lock. The free space is read afresh: one call, no walk.
    const std::optional<uint64_t> free = readFreeBytes();
    std::lock_guard<std::mutex> c(countsM_);
    if (free) counts_.freeBytes = free;
    RollStorageInfo s;
    s.usedBytes = counts_.usedBytes;
    s.capBytes = counts_.capBytes;
    s.freeBytes = counts_.freeBytes.value_or(0);
    s.floorBytes = kDiskFloorBytes;
    s.keepDays = counts_.keepDays;
    return s;
}

// ---- one clip ---------------------------------------------------------------------------------------

std::optional<ClipSidecar> RollStore::findClip(const std::string& clipId) const {
    const auto parts = splitValidId(clipId);
    if (!parts) return std::nullopt;
    std::lock_guard<std::mutex> lk(m_);
    const DaySnapshot snap = snapshotDay(parts->first);
    const auto it = snap.clips.find(parts->second);
    if (it == snap.clips.end() || !it->second.complete()) return std::nullopt;
    for (auto& s : clipsOfDay(snap, nullptr)) {
        if (s.clipId == clipId) {
            s.bytes = it->second.mp4Bytes;
            return s;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> RollStore::mp4Path(const std::string& clipId) const {
    const auto parts = splitValidId(clipId);
    if (!parts) return std::nullopt;
    std::lock_guard<std::mutex> lk(m_);
    const DaySnapshot snap = snapshotDay(parts->first);
    const auto it = snap.clips.find(parts->second);
    if (it == snap.clips.end() || !it->second.complete()) return std::nullopt;
    return snap.dir + L"\\" + widen(parts->second) + L".mp4";
}

std::optional<std::wstring> RollStore::thumbnailPath(const std::string& clipId) const {
    const auto parts = splitValidId(clipId);
    if (!parts) return std::nullopt;
    std::lock_guard<std::mutex> lk(m_);
    const DaySnapshot snap = snapshotDay(parts->first);
    const auto it = snap.clips.find(parts->second);
    if (it == snap.clips.end() || !it->second.complete() || !it->second.jpg) return std::nullopt;
    return snap.dir + L"\\" + widen(parts->second) + L".jpg";
}

// ---- stars and deletion -----------------------------------------------------------------------------

RollError RollStore::setStar(const std::string& clipId, const std::string& deviceId, PetRole role, bool starred) {
    const auto parts = splitValidId(clipId);
    if (!parts) return RollError::NotFound;
    if (deviceId.empty()) return RollError::Invalid;
    if (role != PetRole::Owner && role != PetRole::Viewer) return RollError::NotAllowedForRole;
    std::lock_guard<std::mutex> lk(m_);
    const DaySnapshot snap = snapshotDay(parts->first);
    const auto f = snap.clips.find(parts->second);
    if (f == snap.clips.end() || !f->second.complete()) return RollError::NotFound;
    // The sidecar itself, never the index: a star must act on what is on disk now.
    std::string text;
    ClipSidecar s;
    if (!readFile(snap.dir + L"\\" + widen(parts->second) + L".json", text) || !parseSidecar(text, s)) {
        return RollError::Internal; // a damaged sidecar isn't rewritten from defaults
    }
    s.clipId = clipId;
    auto& stars = s.starredBy;
    const auto mine = [&](const StarEntry& e) { return e.deviceId == deviceId; };
    if (starred) {
        if (std::any_of(stars.begin(), stars.end(), mine)) return RollError::None;
        stars.push_back({deviceId, role});
    } else if (role == PetRole::Owner) {
        // An owner's (or the PC's) unstar clears every star on the clip (v3 §5.2).
        if (stars.empty()) return RollError::None;
        stars.clear();
    } else {
        // A viewer removes only its own star; without one it isn't allowed.
        const auto it = std::remove_if(stars.begin(), stars.end(), mine);
        if (it == stars.end()) return RollError::NotAllowedForRole;
        stars.erase(it, stars.end());
    }
    if (!writeSidecar(snap.dir, parts->second, s, nullptr)) return RollError::Internal;
    refreshStorageFullLocked();
    return RollError::None;
}

RollError RollStore::deleteClip(const std::string& clipId) {
    const auto parts = splitValidId(clipId);
    if (!parts) return RollError::NotFound;
    std::lock_guard<std::mutex> lk(m_);
    const DaySnapshot snap = snapshotDay(parts->first);
    const auto f = snap.clips.find(parts->second);
    if (f == snap.clips.end() || !f->second.complete()) return RollError::NotFound;
    bool complete = true;
    const uint64_t removed = removeClipFiles(snap.dir, parts->second, &complete);
    logDeletion(clipId, removed, true, complete);
    addUsedBytes(-static_cast<int64_t>(removed));
    refreshStorageFullLocked();
    return complete ? RollError::None : RollError::Internal;
}

int RollStore::removeStarsByDevice(const std::string& deviceId) {
    if (deviceId.empty() || deviceId == kPcDeviceId) return 0;
    std::lock_guard<std::mutex> lk(m_);
    return removeStarsIfLocked([&](const std::string& id) { return id == deviceId; });
}

int RollStore::removeStarsOfDevices(const std::set<std::string>& revoked) {
    if (revoked.empty()) return 0;
    std::lock_guard<std::mutex> lk(m_);
    return removeStarsIfLocked([&](const std::string& id) { return id != kPcDeviceId && revoked.count(id) != 0; });
}

int RollStore::removeStarsIfLocked(const std::function<bool(const std::string& deviceId)>& drop) {
    int changed = 0;
    for (const auto& day : dayFolders()) {
        const DaySnapshot snap = snapshotDay(day);
        for (const auto& [name, f] : snap.clips) {
            if (!f.complete()) continue;
            std::string text;
            ClipSidecar s;
            if (!readFile(snap.dir + L"\\" + widen(name) + L".json", text) || !parseSidecar(text, s)) continue;
            const auto it = std::remove_if(s.starredBy.begin(), s.starredBy.end(),
                                           [&](const StarEntry& e) { return drop(e.deviceId); });
            if (it == s.starredBy.end()) continue;
            s.starredBy.erase(it, s.starredBy.end());
            s.clipId = day + "/" + name;
            if (writeSidecar(snap.dir, name, s, nullptr)) ++changed;
        }
    }
    if (changed > 0) refreshStorageFullLocked();
    return changed;
}

void RollStore::pinClip(const std::string& clipId) {
    std::lock_guard<std::mutex> lk(pinM_);
    ++pins_[clipId];
}

void RollStore::unpinClip(const std::string& clipId) {
    std::lock_guard<std::mutex> lk(pinM_);
    const auto it = pins_.find(clipId);
    if (it == pins_.end()) return;
    if (--it->second <= 0) pins_.erase(it);
}

bool RollStore::pinnedLocked(const std::string& clipId) const {
    std::lock_guard<std::mutex> lk(pinM_);
    return pins_.count(clipId) != 0;
}

// ---- the disk floor ---------------------------------------------------------------------------------

// Free space on the roll's volume, through the seam when one is set (tests never fill a disk, §6.4).
std::optional<uint64_t> RollStore::readFreeBytes() const {
    if (cfg_.freeBytes) return cfg_.freeBytes(cfg_.dir);
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(cfg_.dir.c_str(), &avail, nullptr, nullptr)) return std::nullopt;
    return avail.QuadPart;
}

bool RollStore::checkDiskFloor() {
    std::lock_guard<std::mutex> lk(m_);
    const std::optional<uint64_t> free = readFreeBytes();
    {
        std::lock_guard<std::mutex> c(countsM_);
        if (free) counts_.freeBytes = free;
    }
    // Below 20 GB stops; only above 21 GB resumes, so a disk hovering at the line doesn't flap.
    if (!free) floorBlocked_ = true;
    else if (floorBlocked_) floorBlocked_ = !(*free > kDiskResumeBytes);
    else floorBlocked_ = *free < kDiskFloorBytes;
    ++flagsGen_;
    return !floorBlocked_;
}

RollStore::StorageFlags RollStore::flags() const {
    std::lock_guard<std::mutex> lk(m_);
    StorageFlags f;
    f.storageFull = storageFull_;
    f.floorBlocked = floorBlocked_;
    f.gen = flagsGen_;
    return f;
}

bool RollStore::diskFloorBlocked() const {
    std::lock_guard<std::mutex> lk(m_);
    return floorBlocked_;
}

bool RollStore::storageFull() const {
    std::lock_guard<std::mutex> lk(m_);
    return storageFull_;
}

} // namespace roll
} // namespace kvhost
