#include "recordings.hpp"

#include "json_lite.hpp"
#include "recorder.hpp" // resolveFfmpeg

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace kvrec {

namespace json = kvhost::json;

namespace {

int64_t fileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

FILETIME unixToFileTime(int64_t t) {
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(t + 11644473600LL) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool isDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool endsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string readText(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::wstring lowerPath(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

struct CsvStats {
    int frames = 0;
    double duration = 0.0;
};

CsvStats csvStats(const std::wstring& folder, const std::wstring& stem) {
    CsvStats st;
    std::istringstream in(readText(folder + L"\\" + stem + L"_timestamps.csv"));
    std::string line;
    bool header = true;
    uint64_t first = 0, last = 0;
    bool have = false;
    while (std::getline(in, line)) {
        if (header) {
            header = false;
            continue;
        }
        if (line.empty() || line == "\r") continue;
        ++st.frames;
        const auto c = line.rfind(',');
        if (c != std::string::npos) {
            const uint64_t ns = std::strtoull(line.c_str() + c + 1, nullptr, 10);
            if (!have) first = ns;
            last = ns;
            have = true;
        }
    }
    st.duration = (have && last > first) ? static_cast<double>(last - first) / 1e9 + 1.0 / 30.0
                                         : static_cast<double>(st.frames) / 30.0;
    return st;
}

bool getIntrinsics(const json::Value& v, kv_ir_intrinsics_t& out) {
    out = kv_ir_intrinsics_t{};
    return json::getFloat(v, "fx", out.fx) && json::getFloat(v, "fy", out.fy) && json::getFloat(v, "cx", out.cx) &&
           json::getFloat(v, "cy", out.cy) && json::getFloat(v, "k1", out.k1) && json::getFloat(v, "k2", out.k2) &&
           json::getFloat(v, "k3", out.k3) && json::getFloat(v, "p1", out.p1) && json::getFloat(v, "p2", out.p2) &&
           ((out.valid = out.fx > 0.0f && out.fy > 0.0f), true);
}

int64_t folderOrFileTime(const std::wstring& folder) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(folder.c_str(), GetFileExInfoStandard, &fa)) return fileTimeToUnix(fa.ftCreationTime);
    return unixNow();
}

// A session, or nullopt for anything else (never an old colour or side-by-side one as IR).
std::optional<SessionItem> parseSession(const std::wstring& folder, const std::wstring& name) {
    SessionItem s;
    s.folder = folder;
    s.folder_name = name;
    s.size_bytes = treeSizeBytes(folder);
    const std::string meta_text = readText(folder + L"\\session_meta.json");
    json::Value meta;
    if (!meta_text.empty() && json::parse(meta_text, meta) && meta.type == json::Value::Type::Object) {
        std::string slug, container;
        if (!json::getString(meta, "modeSlug", slug) || (slug != "ir16" && slug != "depth16mm")) return std::nullopt;
        s.is_depth = slug == "depth16mm";
        json::getString(meta, "container", container);
        s.container = container == "png16" ? "png16" : "ffv1";
        int w = 0, h = 0, frames = 0;
        double dur = 0.0;
        json::getInt(meta, "width", w);
        json::getInt(meta, "height", h);
        json::getInt(meta, "frameCount", frames);
        json::getDouble(meta, "durationSec", dur);
        // Only the sensor's 512x424 (a damaged or forged file can't size the buffers).
        if ((w != 0 && w != 512) || (h != 0 && h != 424)) return std::nullopt;
        s.width = 512;
        s.height = 424;
        s.frame_count = std::clamp(frames, 0, kMaxSessionFrames);
        s.duration_sec = dur;
        if (const json::Value* iv = meta.get("irIntrinsics"); iv && iv->type == json::Value::Type::Object) {
            kv_ir_intrinsics_t intr{};
            if (getIntrinsics(*iv, intr)) s.ir_intrinsics = intr;
        }
        if (s.frame_count == 0) {
            const CsvStats st = csvStats(folder, s.modeSlug());
            s.frame_count = std::min(st.frames, kMaxSessionFrames);
            if (s.duration_sec <= 0.0) s.duration_sec = st.duration;
        }
    } else {
        // No session_meta.json (a W4 session): only explicit ir16 / depth16mm markers count.
        const bool depth = endsWith(name, L"_depth16mm.nosync") || exists(folder + L"\\depth16mm_timestamps.csv") ||
                           exists(folder + L"\\depth16mm_lossless.mkv") || exists(folder + L"\\depth16mm_000000.png");
        const bool ir = !depth && (endsWith(name, L"_ir16.nosync") || exists(folder + L"\\ir16_timestamps.csv") ||
                                   exists(folder + L"\\ir16_lossless.mkv") || exists(folder + L"\\ir16_000000.png"));
        if (!depth && !ir) return std::nullopt;
        s.is_depth = depth;
        s.container = exists(folder + L"\\" + s.modeSlug() + L"_lossless.mkv") ? "ffv1" : "png16";
        const CsvStats st = csvStats(folder, s.modeSlug());
        s.frame_count = std::min(st.frames, kMaxSessionFrames);
        s.duration_sec = st.duration;
    }
    // The earliest of the folder's stamp, its creation time and now: a clock set wrong (or a stamp in
    // the future) can't keep a session past 7 days.
    s.created = std::min(folderOrFileTime(folder), unixNow());
    if (const auto stamp = parseFolderTimestamp(name)) s.created = std::min(s.created, *stamp);
    return s;
}

const char* triggerName(DeletionTrigger t) { return t == DeletionTrigger::User ? "user" : "automatic"; }

} // namespace

int64_t unixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return fileTimeToUnix(ft);
}

std::string isoUtc(int64_t t) {
    const FILETIME ft = unixToFileTime(t);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return b;
}

std::optional<int64_t> parseFolderTimestamp(const std::wstring& name) {
    // yyyyMMdd-HHmmss-SSS_ (local time, as Recorder::start stamps it)
    if (name.size() < 20 || name[8] != L'-' || name[15] != L'-' || name[19] != L'_') return std::nullopt;
    for (size_t i : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 11u, 12u, 13u, 14u, 16u, 17u, 18u}) {
        if (name[i] < L'0' || name[i] > L'9') return std::nullopt;
    }
    auto num = [&](size_t at, size_t n) { return static_cast<WORD>(std::stoi(name.substr(at, n))); };
    SYSTEMTIME local{};
    local.wYear = num(0, 4);
    local.wMonth = num(4, 2);
    local.wDay = num(6, 2);
    local.wHour = num(9, 2);
    local.wMinute = num(11, 2);
    local.wSecond = num(13, 2);
    local.wMilliseconds = num(16, 3);
    SYSTEMTIME utc{};
    FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return std::nullopt;
    return fileTimeToUnix(ft);
}

std::wstring SessionItem::countdownText(int64_t now) const {
    const int64_t remaining = expiresAt() - now;
    if (remaining <= 0) return L"Expired";
    const int64_t total_min = remaining / 60;
    const int64_t days = total_min / (24 * 60), hours = (total_min % (24 * 60)) / 60, mins = total_min % 60;
    wchar_t b[48];
    if (days > 0) swprintf(b, 48, L"%lldd %lldh left", static_cast<long long>(days), static_cast<long long>(hours));
    else if (hours > 0) swprintf(b, 48, L"%lldh %lldm left", static_cast<long long>(hours), static_cast<long long>(mins));
    else swprintf(b, 48, L"%lldm left", static_cast<long long>(std::max<int64_t>(1, mins)));
    return b;
}

std::wstring SessionItem::lengthText() const {
    wchar_t b[64];
    if (duration_sec >= 60.0) {
        const int secs_total = static_cast<int>(std::lround(duration_sec));
        swprintf(b, 64, L"%dm %02ds (%d f)", static_cast<int>(duration_sec) / 60, secs_total % 60, frame_count);
    } else {
        swprintf(b, 64, L"%.1f s (%d f)", duration_sec, frame_count);
    }
    return b;
}

std::wstring SessionItem::sizeText() const {
    wchar_t b[32];
    if (size_bytes >= 1000000) swprintf(b, 32, L"%.1f MB", static_cast<double>(size_bytes) / 1e6);
    else swprintf(b, 32, L"%.0f KB", static_cast<double>(size_bytes) / 1e3);
    return b;
}

std::wstring SessionItem::dateText() const {
    const FILETIME ft = unixToFileTime(created);
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t b[32];
    swprintf(b, 32, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return b;
}

// A junction or symbolic link is never entered: its size isn't counted, and deleting removes the
// link itself, never what it points at (a junction "X.nosync" to Documents must not empty it).
static bool isReparse(DWORD attrs) { return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }

uint64_t treeSizeBytes(const std::wstring& dir) {
    const DWORD top = GetFileAttributesW(dir.c_str());
    if (top == INVALID_FILE_ATTRIBUTES || isReparse(top)) return 0;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (isReparse(fd.dwFileAttributes)) continue; // not followed (a looping one can't recurse forever)
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += treeSizeBytes(dir + L"\\" + n);
        else total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

bool removeTree(const std::wstring& dir) {
    const DWORD top = GetFileAttributesW(dir.c_str());
    if (top == INVALID_FILE_ATTRIBUTES) return false;
    if (isReparse(top)) {
        // The link itself only.
        return (top & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(dir.c_str()) != 0 : DeleteFileW(dir.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (isReparse(fd.dwFileAttributes)) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
                else DeleteFileW(p.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(p);
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(dir.c_str()) != 0;
}

namespace {
// The .nosync folders directly in base that are real folders (no junction, no link).
std::vector<std::wstring> sessionFolderNames(const std::wstring& base) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*.nosync").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        const std::wstring n = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || isReparse(fd.dwFileAttributes) || n == L".nosync") continue;
        names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return names;
}

// Removes a session folder and logs it. A folder only partly removed is logged too, with what went
// and "incomplete", so the log never misses a deletion.
bool removeAndLog(const std::wstring& base, const std::wstring& folder, const std::wstring& name, uint64_t size_before,
                  int64_t now, DeletionTrigger trigger) {
    if (removeTree(folder)) {
        appendDeletionLog(base, name, size_before, now, trigger);
        return true;
    }
    const uint64_t left = treeSizeBytes(folder);
    if (left < size_before) {
        std::string line = deletionLogLine(name, size_before - left, now, trigger);
        line.insert(line.size() - 1, "\tincomplete");
        std::ofstream(base + L"\\" + kDeletionLogName, std::ios::binary | std::ios::app) << line;
    }
    return false;
}
} // namespace

std::vector<SessionItem> listSessions(const std::wstring& base, const std::wstring& exclude) {
    std::vector<SessionItem> out;
    const std::wstring ex = lowerPath(exclude);
    for (const auto& n : sessionFolderNames(base)) {
        const std::wstring p = base + L"\\" + n;
        if (!ex.empty() && lowerPath(p) == ex) continue;
        if (auto s = parseSession(p, n)) out.push_back(std::move(*s));
    }
    // Newest first; within a second, the folder stamp's milliseconds decide.
    std::sort(out.begin(), out.end(), [](const SessionItem& a, const SessionItem& b) {
        return a.created != b.created ? a.created > b.created : a.folder_name > b.folder_name;
    });
    return out;
}

std::string deletionLogLine(const std::wstring& name, uint64_t size_bytes, int64_t when, DeletionTrigger trigger) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, u8.data(), n, nullptr, nullptr);
    return isoUtc(when) + "\t" + u8 + "\t" + std::to_string(size_bytes) + " bytes\t" + triggerName(trigger) + "\n";
}

void appendDeletionLog(const std::wstring& base, const std::wstring& name, uint64_t size_bytes, int64_t when,
                       DeletionTrigger trigger) {
    CreateDirectoryW(base.c_str(), nullptr);
    const std::string line = deletionLogLine(name, size_bytes, when, trigger);
    std::ofstream out(base + L"\\" + kDeletionLogName, std::ios::binary | std::ios::app);
    out << line;
}

std::vector<std::wstring> purgeDisallowedSessions(const std::wstring& base, int64_t now, const std::wstring& exclude) {
    std::vector<std::wstring> deleted;
    const std::wstring ex = lowerPath(exclude);
    for (const auto& n : sessionFolderNames(base)) { // junctions never reach here
        const std::wstring p = base + L"\\" + n;
        if (!ex.empty() && lowerPath(p) == ex) continue;
        if (parseSession(p, n)) continue; // an IR or Depth session stays
        if (removeAndLog(base, p, n, treeSizeBytes(p), now, DeletionTrigger::Automatic)) deleted.push_back(n);
    }
    return deleted;
}

std::vector<std::wstring> purgeExpiredSessions(const std::wstring& base, int64_t now, const std::wstring& exclude) {
    std::vector<std::wstring> deleted = purgeDisallowedSessions(base, now, exclude);
    for (const auto& s : listSessions(base, exclude)) {
        if (!s.isExpired(now)) continue;
        if (removeAndLog(base, s.folder, s.folder_name, s.size_bytes, now, DeletionTrigger::Automatic)) {
            deleted.push_back(s.folder_name);
        }
    }
    return deleted;
}

bool deleteSessionByUser(const SessionItem& s, const std::wstring& base, int64_t now) {
    // Only a real folder directly inside base, named as listed: never a junction, never elsewhere.
    if (s.folder_name.empty() || s.folder_name == L"." || s.folder_name == L".." ||
        s.folder_name.find_first_of(L"\\/") != std::wstring::npos) {
        return false;
    }
    if (lowerPath(s.folder) != lowerPath(base + L"\\" + s.folder_name)) return false;
    const DWORD attrs = GetFileAttributesW(s.folder.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || isReparse(attrs)) return false;
    const uint64_t size = treeSizeBytes(s.folder);
    return removeAndLog(base, s.folder, s.folder_name, size > 0 ? size : s.size_bytes, now, DeletionTrigger::User);
}
void ReminderPolicy::snoozeOneHour(int64_t now) {
    suppressed_until_ = now + 3600;
}

std::string sessionMetaJson(const std::string& created_iso, const std::string& mode_slug, const std::string& container,
                            int width, int height, int frame_count, double duration_sec,
                            const std::optional<kv_ir_intrinsics_t>& intr) {
    json::ObjectWriter w;
    w.field("createdAtISO8601", created_iso)
        .field("modeSlug", mode_slug)
        .field("container", container)
        .field("width", width)
        .field("height", height)
        .field("frameCount", frame_count)
        .doubleField("durationSec", duration_sec);
    if (intr && intr->valid) {
        w.raw("irIntrinsics", json::ObjectWriter()
                                  .floatField("fx", intr->fx)
                                  .floatField("fy", intr->fy)
                                  .floatField("cx", intr->cx)
                                  .floatField("cy", intr->cy)
                                  .floatField("k1", intr->k1)
                                  .floatField("k2", intr->k2)
                                  .floatField("k3", intr->k3)
                                  .floatField("p1", intr->p1)
                                  .floatField("p2", intr->p2)
                                  .finish());
    }
    return w.finish();
}

// ---- reader ----------------------------------------------------------------------------------------

SessionReader::SessionReader(SessionItem s, int chunk) : s_(std::move(s)), chunk_(std::max(1, chunk)) {
    std::istringstream in(readText(s_.folder + L"\\" + s_.modeSlug() + L"_timestamps.csv"));
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) {
            header = false;
            continue;
        }
        if (line.empty() || line == "\r") continue;
        // frame_index,engine_seq,device_ticks,arrival_mach_ns
        unsigned long long idx = 0, seq = 0, arr = 0;
        unsigned ticks = 0;
        if (std::sscanf(line.c_str(), "%llu,%llu,%u,%llu", &idx, &seq, &ticks, &arr) == 4) {
            ticks_.push_back(ticks);
            ts_ns_.push_back(arr);
        }
    }
    total_ = std::min(kMaxSessionFrames, std::max(s_.frame_count, static_cast<int>(ts_ns_.size())));
}

uint32_t SessionReader::deviceTicks(int i) const {
    return (i >= 0 && i < static_cast<int>(ticks_.size())) ? ticks_[static_cast<size_t>(i)] : static_cast<uint32_t>(i * 267);
}

double SessionReader::frameIntervalSeconds(int i) const {
    if (i >= 0 && i + 1 < static_cast<int>(ts_ns_.size())) {
        const uint64_t t0 = ts_ns_[static_cast<size_t>(i)], t1 = ts_ns_[static_cast<size_t>(i) + 1];
        if (t1 > t0) return std::min(0.25, std::max(1.0 / 120.0, static_cast<double>(t1 - t0) / 1e9));
    }
    return 1.0 / 30.0;
}

void SessionReader::cancel() {
    cancelled_ = true;
    std::lock_guard<std::mutex> lk(proc_m_);
    if (proc_) TerminateProcess(static_cast<HANDLE>(proc_), 1); // its pipe closes; the read ends
}

bool SessionReader::readFrame(int index, std::vector<uint16_t>& out) {
    if (cancelled_ || index < 0 || index >= total_) return false;
    if (s_.container == "png16") return readPng(index, out);
    const int c = index / chunk_;
    if (c != cached_chunk_ && !decodeChunk(c)) return false;
    const size_t off = static_cast<size_t>(index - c * chunk_);
    if (off >= cached_.size()) return false;
    out = cached_[off];
    return true;
}

bool SessionReader::readPng(int index, std::vector<uint16_t>& out) {
    wchar_t name[64];
    swprintf(name, 64, L"_%06d.png", index);
    const std::wstring path = s_.folder + L"\\" + s_.modeSlug() + name;
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (static_cast<int>(w) != s_.width || static_cast<int>(h) != s_.height) return false;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat16bppGray, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        return false;
    }
    out.resize(static_cast<size_t>(w) * h);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 2, static_cast<UINT>(out.size() * 2), reinterpret_cast<BYTE*>(out.data())));
}

bool SessionReader::decodeChunk(int c) {
    cached_chunk_ = -1;
    cached_.clear();
    const std::wstring ff = resolveFfmpeg();
    const std::wstring mkv = s_.folder + L"\\" + s_.modeSlug() + L"_lossless.mkv";
    if (ff.empty() || !isDir(s_.folder) || !exists(mkv)) return false;
    const int start = c * chunk_;
    const int end = std::min(total_ - 1, start + chunk_ - 1);
    if (start > end) return false;
    const int count = end - start + 1;
    std::wstring cmd = L"\"" + ff + L"\" -nostdin -hide_banner -loglevel error";
    if (start > 0) {
        // Matroska stamps frames in 1 ms ticks (round(frame * 1000 / 30) ms) and FFV1 here is all-intra
        // (-g 1): an input -ss lands on the frame without decoding from frame 0 (the Mac's arguments).
        const double rounded_ms = std::round(start * 1000.0 / 30.0);
        wchar_t ss[64];
        swprintf(ss, 64, L" -ss %.6f", std::max(0.0, rounded_ms / 1000.0 - 0.0004));
        cmd += ss;
    }
    cmd += L" -i \"" + mkv + L"\" -frames:v " + std::to_wstring(count) + L" -f rawvideo -pix_fmt gray16le pipe:1";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    // Only NUL and its own pipe end: no other handle of this process reaches ffmpeg.
    PROCESS_INFORMATION pi{};
    const bool ok = startChildWithOnly(cmd, nul, wr, nul, &pi.hProcess, &pi.hThread, nullptr);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(proc_m_);
        proc_ = pi.hProcess;
        if (cancelled_) TerminateProcess(pi.hProcess, 1);
    }
    // Read the pipe to its end before waiting, so ffmpeg never blocks on a full pipe.
    const size_t px = static_cast<size_t>(s_.width) * s_.height;
    std::vector<uint8_t> raw;
    raw.reserve(px * 2 * static_cast<size_t>(count));
    uint8_t buf[1 << 16];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) raw.insert(raw.end(), buf, buf + n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 10000);
    {
        std::lock_guard<std::mutex> lk(proc_m_);
        proc_ = nullptr;
    }
    CloseHandle(pi.hProcess);
    if (cancelled_) return false;
    CloseHandle(pi.hThread);
    const size_t frames = raw.size() / (px * 2);
    if (frames == 0) return false;
    cached_.resize(frames);
    for (size_t f = 0; f < frames; ++f) {
        cached_[f].resize(px);
        std::memcpy(cached_[f].data(), raw.data() + f * px * 2, px * 2);
    }
    cached_chunk_ = c;
    return true;
}

// ---- decode worker ---------------------------------------------------------------------------------

ReplayDecoder::ReplayDecoder(SessionItem s) : s_(std::move(s)), reader_(std::make_unique<SessionReader>(s_)) {
    total_ = reader_->totalFrames();
    intervals_.reserve(static_cast<size_t>(total_));
    for (int i = 0; i < total_; ++i) intervals_.push_back(reader_->frameIntervalSeconds(i));
    th_ = std::thread([this] { loop(); });
}

ReplayDecoder::~ReplayDecoder() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    reader_->cancel(); // a chunk decode in progress is killed, not waited for
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

double ReplayDecoder::frameIntervalSeconds(int i) const {
    return (i >= 0 && i < static_cast<int>(intervals_.size())) ? intervals_[static_cast<size_t>(i)] : 1.0 / 30.0;
}

void ReplayDecoder::request(int index) {
    {
        std::lock_guard<std::mutex> lk(m_);
        wanted_ = index; // the newest request wins; older ones aren't decoded
    }
    cv_.notify_all();
}

std::optional<ReplayDecoder::Frame> ReplayDecoder::take() {
    std::lock_guard<std::mutex> lk(m_);
    std::optional<Frame> f = std::move(ready_);
    ready_.reset();
    return f;
}

void ReplayDecoder::loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC for PNG16
    SessionReader& reader = *reader_;
    while (true) {
        int idx = -1;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || wanted_ >= 0; });
            if (stop_) break;
            idx = wanted_;
            wanted_ = -1;
        }
        Frame f;
        f.index = idx;
        f.ok = reader.readFrame(idx, f.pixels);
        f.ticks = reader.deviceTicks(idx);
        std::lock_guard<std::mutex> lk(m_);
        ready_ = std::move(f);
    }
    CoUninitialize();
}

} // namespace kvrec
