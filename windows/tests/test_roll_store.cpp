// The camera roll's files and folders (pet-camera.md §6.3-§6.5, protocol v3 §5.2, §5.6-§5.8, §7):
// names and the "-n" suffix, clip id / day / cursor validation (path traversal, tampering), the
// sidecar, index.json (rebuilt by the host's passes only; listing writes nothing), paging, the star
// rules and revoke, deletion and its log, leftovers, retention, the disk floor and folder protection.
// Pure filesystem work in unique temp folders with fake .mp4 bytes: no video, no real roll folder.
#include "kv_test.hpp"

#include "host_state.hpp" // isOwnerOnly
#include "json_lite.hpp"
#include "roll_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace kvhost;
using namespace kvhost::roll;

namespace {

constexpr int64_t kT0 = 1795028602000LL; // 2026-11-18T19:03:22Z, 14:03:22 at UTC-05:00
constexpr int64_t kDayMs = 86400000LL;
constexpr int kFakeOffsetMinutes = -300;

// A fixed UTC-05:00 zone, so names don't depend on the machine running the test.
LocalTime fakeLocal(int64_t unixMs) {
    const int64_t secs = unixMs / 1000 + kFakeOffsetMinutes * 60;
    int64_t days = secs / 86400, sod = secs % 86400;
    if (sod < 0) {
        sod += 86400;
        --days;
    }
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    LocalTime t;
    t.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    t.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    t.year = static_cast<int>(yoe + era * 400 + (t.month <= 2 ? 1 : 0));
    t.hour = static_cast<int>(sod / 3600);
    t.minute = static_cast<int>(sod / 60 % 60);
    t.second = static_cast<int>(sod % 60);
    t.offsetMinutes = kFakeOffsetMinutes;
    return t;
}

std::wstring freshDir(const wchar_t* tag) {
    static std::atomic<int> counter{0};
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"kv-roll-" + tag + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(++counter);
}

// Removes a temp tree. A junction is removed itself, never followed.
void removeTree(const std::wstring& dir) {
    const DWORD top = GetFileAttributesW(dir.c_str());
    if (top == INVALID_FILE_ATTRIBUTES) return;
    if (top & FILE_ATTRIBUTE_REPARSE_POINT) {
        RemoveDirectoryW(dir.c_str());
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(p.c_str());
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void writeBytes(const std::wstring& p, size_t n, char fill) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    const std::string s(n, fill);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

std::string readText(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeText(const std::wstring& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

uint64_t sizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

uint64_t writeTimeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

void setWriteTime(const std::wstring& p, uint64_t t) {
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(t & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
    KV_CHECK(SetFileTime(h, nullptr, nullptr, &ft));
    CloseHandle(h);
}

uint64_t systemFileTimeNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Every file and folder below dir with its size and write time, to prove a call wrote nothing.
using TreeState = std::map<std::wstring, std::pair<uint64_t, uint64_t>>;
void collectTree(const std::wstring& dir, const std::wstring& rel, TreeState& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const uint64_t wt = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out[rel + n + (isDir ? L"\\" : L"")] = {isDir ? 0 : size, isDir ? 0 : wt};
        if (isDir) collectTree(dir + L"\\" + n, rel + n + L"\\", out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
TreeState treeState(const std::wstring& dir) {
    TreeState s;
    collectTree(dir, L"", s);
    return s;
}

std::vector<std::string> logLines(const std::wstring& rollDir) {
    std::vector<std::string> lines;
    std::istringstream in(readText(rollDir + L"\\deletion-log.txt"));
    std::string l;
    while (std::getline(in, l)) lines.push_back(l);
    return lines;
}

// A roll in a unique temp folder, with an injected clock, local zone, free space and probe.
struct Fixture {
    std::wstring base, dir, recordings;
    int64_t nowMs = kT0 + 60000;
    std::optional<uint64_t> freeSpace = 100'000'000'000ULL;
    ProbeFn probe;
    int probeCalls = 0;
    bool withProbe = true;
    std::unique_ptr<RollStore> store;

    explicit Fixture(const wchar_t* tag) {
        base = freshDir(tag);
        CreateDirectoryW(base.c_str(), nullptr);
        dir = base + L"\\roll";
        recordings = base + L"\\recordings";
        std::string err;
        KV_CHECK_MSG(prepareRollFolder(dir, recordings, &err), err);
        open();
    }
    ~Fixture() {
        store.reset();
        removeTree(base);
    }
    void open() {
        RollConfig c;
        c.dir = dir;
        c.nowUnixMs = [this] { return nowMs; };
        c.toLocal = fakeLocal;
        c.freeBytes = [this](const std::wstring&) { return freeSpace; };
        if (withProbe) {
            c.probe = [this](const std::wstring& p) -> std::optional<ProbedClip> {
                ++probeCalls;
                if (!probe) return std::nullopt;
                return probe(p);
            };
        }
        store = std::make_unique<RollStore>(c);
    }
    std::wstring path(const std::string& clipId, const wchar_t* ext) const {
        std::wstring w(clipId.begin(), clipId.end());
        std::replace(w.begin(), w.end(), L'/', L'\\');
        return dir + L"\\" + w + ext;
    }
};

std::string addClip(Fixture& f, int64_t startMs, ClipTrigger trig, size_t mp4Bytes = 4096, bool jpg = true,
                    std::vector<StarEntry> stars = {}, std::optional<StartedBy> by = std::nullopt, double dur = 12.5) {
    std::string err;
    const auto p = f.store->reserveClip(startMs, trig, &err);
    KV_CHECK_MSG(p.has_value(), err);
    if (!p) return {};
    writeBytes(p->mp4, mp4Bytes, 'M'); // fake .mp4 bytes; nothing here reads video
    if (jpg) writeBytes(p->thumbnail, 512, 'J');
    ClipSidecar s;
    s.startUnixMs = static_cast<uint64_t>(startMs);
    s.durationSec = dur;
    s.width = 1280;
    s.height = 720;
    s.modeRaw = 1;
    s.preRollSec = 5.0;
    s.trigger = trig;
    s.starredBy = std::move(stars);
    s.startedBy = std::move(by);
    KV_CHECK_MSG(f.store->finalizeClip(*p, s, &err), err);
    return p->clipId;
}

ClipSidecar sidecarOf(const Fixture& f, const std::string& id) {
    ClipSidecar s;
    KV_CHECK(parseSidecar(readText(f.path(id, L".json")), s));
    return s;
}

std::string b64url(const std::string& raw) { // a test-side encoder, to forge cursors
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : raw) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += a[(acc >> bits) & 63];
        }
    }
    if (bits > 0) out += a[(acc << (6 - bits)) & 63];
    return out;
}

} // namespace

// ---- names, ids, days, cursors -----------------------------------------------------------------------

KV_TEST(names_use_pc_local_time_and_take_a_suffix_when_taken) {
    const LocalTime t = fakeLocal(kT0);
    KV_CHECK(dayString(t) == "2026-11-18");
    KV_CHECK(isoLocalString(t) == "2026-11-18T14:03:22-05:00");
    KV_CHECK(clipName(t, ClipTrigger::Motion) == "14-03-22_motion");
    KV_CHECK(clipName(t, ClipTrigger::Manual, 2) == "14-03-22_manual-2");
    KV_CHECK(clipName(t, ClipTrigger::Unknown).empty());
    LocalTime e = t;
    e.offsetMinutes = 330;
    KV_CHECK(isoLocalString(e) == "2026-11-18T14:03:22+05:30");

    // The PC's own zone gives a valid day and a whole-quarter-hour offset.
    const LocalTime pc = pcLocalTime(kT0);
    KV_CHECK(isValidDay(dayString(pc)));
    KV_CHECK(pc.offsetMinutes % 15 == 0 && pc.offsetMinutes >= -14 * 60 && pc.offsetMinutes <= 14 * 60);

    Fixture f(L"names");
    std::string err;
    const auto a = f.store->reserveClip(kT0, ClipTrigger::Motion, &err);
    const auto b = f.store->reserveClip(kT0 + 400, ClipTrigger::Motion, &err); // same second
    const auto c = f.store->reserveClip(kT0, ClipTrigger::Motion, &err);
    const auto m = f.store->reserveClip(kT0, ClipTrigger::Manual, &err); // another trigger, its own name
    KV_REQUIRE(a && b && c && m);
    KV_CHECK(a->clipId == "2026-11-18/14-03-22_motion");
    KV_CHECK(b->clipId == "2026-11-18/14-03-22_motion-2");
    KV_CHECK(c->clipId == "2026-11-18/14-03-22_motion-3");
    KV_CHECK(m->clipId == "2026-11-18/14-03-22_manual");
    KV_CHECK(a->mp4 == f.dir + L"\\2026-11-18\\14-03-22_motion.mp4");
    KV_CHECK(a->sidecar == f.dir + L"\\2026-11-18\\14-03-22_motion.json");
    KV_CHECK(a->thumbnail == f.dir + L"\\2026-11-18\\14-03-22_motion.jpg");
    KV_CHECK(exists(a->mp4) && sizeOf(a->mp4) == 0); // the reservation holds the name
    // A leftover thumbnail also holds a name.
    writeBytes(f.dir + L"\\2026-11-18\\14-03-22_motion-4.jpg", 10, 'J');
    const auto d = f.store->reserveClip(kT0, ClipTrigger::Motion, &err);
    KV_REQUIRE(d.has_value());
    KV_CHECK(d->clipId == "2026-11-18/14-03-22_motion-5");
    // A local midnight crossing goes to the next day's folder: 23:59:59 and 00:00:00 local.
    const auto late = f.store->reserveClip(kT0 + (9 * 3600 + 56 * 60 + 37) * 1000LL, ClipTrigger::Schedule, &err);
    const auto early = f.store->reserveClip(kT0 + (9 * 3600 + 56 * 60 + 38) * 1000LL, ClipTrigger::Schedule, &err);
    KV_REQUIRE(late && early);
    KV_CHECK(late->clipId == "2026-11-18/23-59-59_schedule");
    KV_CHECK(early->clipId == "2026-11-19/00-00-00_schedule");
    KV_CHECK(!f.store->reserveClip(kT0, ClipTrigger::Unknown, &err));
    // A reserved clip isn't finalized, so it isn't listed or found.
    KV_CHECK(!f.store->findClip(a->clipId));
    KV_CHECK(!f.store->mp4Path(a->clipId));
    const auto list = f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "");
    KV_CHECK(list.error == RollError::None && list.clips.empty());
}

KV_TEST(clip_ids_days_and_traversal_are_validated_exactly) {
    for (const char* ok : {"2026-11-18/14-03-22_motion", "2026-11-18/14-03-22_manual", "2026-11-18/00-00-00_schedule",
                           "2026-11-18/23-59-59_motion-2", "2024-02-29/14-03-22_manual-999", "2026-11-18/14-03-22_motion-0"}) {
        KV_CHECK_MSG(isValidClipId(ok), ok);
    }
    const std::vector<std::string> bad = {
        "", "2026-11-18", "2026-11-18/", "/2026-11-18/14-03-22_motion", "2026-11-18/14-03-22_motion/",
        "2026-11-18\\14-03-22_motion", "2026-11-18/../14-03-22_motion", "../2026-11-18/14-03-22_motion",
        "2026-11-18/14-03-22_motion/..", "2026-11-18/..", "..", "../../../Windows/win.ini",
        "C:/2026-11-18/14-03-22_motion", "C:\\x", "2026-11-18//14-03-22_motion", "2026-11-18/14-03-22_motion.mp4",
        "2026-11-18/14-03-22_motion.json", "2026-11-18/14-03-22_colour", "2026-11-18/14-03-22_MOTION",
        "2026-11-18/14-03-22_motion-1000", "2026-11-18/14-03-22_motion-", "2026-11-18/14-03-22_motion-a",
        "2026-11-18/14-03-22_motion--2", "2026-11-18/24-00-00_motion", "2026-11-18/14-60-00_motion",
        "2026-11-18/14-03-60_motion", "2026-02-30/14-03-22_motion", "2025-02-29/14-03-22_motion",
        "2026-13-01/14-03-22_motion", "0000-01-01/14-03-22_motion", "2026-11-18/14-03-22_motion ",
        " 2026-11-18/14-03-22_motion", "2026-11-18/14-03-22_motion\n", "2026-11-18/14:03:22_motion",
        "2026-11-18/14-03-22_motion%2F..", "2026-11-18/14-03-22_motionx", "20261118/14-03-22_motion",
        "2026-11-18/1-03-22_motion", std::string("2026-11-18/14-03-22_motion\0../x", 31),
        "2026-11-18/14-03-22_motion-\xd9\xa1", "\xd9\xa1\xd9\xa1\xd9\xa1\xd9\xa1-11-18/14-03-22_motion"};
    for (const auto& b : bad) KV_CHECK_MSG(!isValidClipId(b), b);
    KV_CHECK(triggerOfClipId("2026-11-18/14-03-22_schedule-3") == ClipTrigger::Schedule);
    KV_CHECK(triggerOfClipId("2026-11-18/14-03-22_manual") == ClipTrigger::Manual);
    KV_CHECK(triggerOfClipId("../x") == ClipTrigger::Unknown);

    KV_CHECK(isValidDay("2026-11-18") && isValidDay("2024-02-29") && isValidDay("2000-02-29"));
    for (const char* d : {"2026-02-29", "1900-02-29", "2026-04-31", "2026-00-10", "2026-11-00", "2026-11-1",
                          "2026/11/18", "2026-11-18/", "..", "2026-11-18\\..", "0000-01-01",
                          "\xef\xbc\x92\xef\xbc\x90\xef\xbc\x92\xef\xbc\x96-11-18"}) { // full-width digits
        KV_CHECK_MSG(!isValidDay(d), d);
    }

    // A device's id is never joined into a path unchecked: every traversal attempt is not_found and
    // touches nothing, even when a file of that name exists outside the roll.
    Fixture f(L"traversal");
    writeBytes(f.base + L"\\victim.mp4", 100, 'V');
    writeBytes(f.base + L"\\victim.json", 100, 'V');
    writeBytes(f.base + L"\\victim.jpg", 100, 'V');
    addClip(f, kT0, ClipTrigger::Motion);
    const TreeState before = treeState(f.base);
    for (const std::string id : {"../victim", "2026-11-18/../../victim", "..\\victim", "2026-11-18/..\\..\\victim",
                                 "2026-11-18/14-03-22_motion/../../../victim", "C:\\Windows\\win.ini"}) {
        KV_CHECK(!f.store->findClip(id));
        KV_CHECK(!f.store->mp4Path(id));
        KV_CHECK(!f.store->thumbnailPath(id));
        KV_CHECK(f.store->deleteClip(id) == RollError::NotFound);
        KV_CHECK(f.store->setStar(id, "dev", PetRole::Owner, true) == RollError::NotFound);
        KV_CHECK(f.store->listClips(id, std::nullopt, std::nullopt, "").error == RollError::NotFound);
    }
    KV_CHECK(f.store->listClips("..", std::nullopt, std::nullopt, "").error == RollError::NotFound);
    KV_CHECK(f.store->listClips("2026-11-17", std::nullopt, std::nullopt, "").error == RollError::NotFound);
    KV_CHECK(f.store->listClips("2026-02-30", std::nullopt, std::nullopt, "").error == RollError::NotFound);
    KV_CHECK(treeState(f.base) == before);
}

KV_TEST(cursors_round_trip_and_a_tampered_cursor_is_invalid) {
    const std::string c = encodeClipCursor(1795028602000ULL, "2026-11-18/14-03-22_motion-2");
    for (char ch : c) KV_CHECK(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_');
    const auto d = decodeClipCursor(c);
    KV_REQUIRE(d.has_value());
    KV_CHECK(d->startUnixMs == 1795028602000ULL && d->clipId == "2026-11-18/14-03-22_motion-2");
    KV_CHECK(b64url("1795028602000:2026-11-18/14-03-22_motion-2") == c);
    const auto day = decodeDayCursor(encodeDayCursor("2026-11-18"));
    KV_CHECK(day && *day == "2026-11-18");

    // Tampering: other alphabets, padding, a changed character, a cut, and well-formed base64url of
    // a payload of the wrong shape.
    std::vector<std::string> bad = {"", "=", c + "=", c + "==", c.substr(0, c.size() - 1), c + "A",
                                    "!!!!", std::string(300, 'A'), "MTc5NTAyODYwMjAwMDoyMDI2LTExLTE4LzE0LTAzLTIyX21vdGlvbi0y+/"};
    std::string flipped = c;
    flipped[5] = flipped[5] == 'A' ? 'B' : 'A';
    bad.push_back(flipped);
    for (const std::string raw : {"1:../../victim", "1:2026-11-18/../x", "1:2026-11-18\\14-03-22_motion",
                                  "x:2026-11-18/14-03-22_motion", ":2026-11-18/14-03-22_motion",
                                  "-1:2026-11-18/14-03-22_motion", "01:2026-11-18/14-03-22_motion",
                                  "18446744073709551616:2026-11-18/14-03-22_motion",
                                  "1 :2026-11-18/14-03-22_motion", "1:2026-11-18/14-03-22_motion:",
                                  "2026-11-18", "1:2026-02-30/14-03-22_motion"}) {
        bad.push_back(b64url(raw));
    }
    for (const auto& b : bad) KV_CHECK_MSG(!decodeClipCursor(b), b);
    KV_CHECK(decodeClipCursor(b64url("18446744073709551615:2026-11-18/14-03-22_motion")).has_value());
    for (const std::string raw : {"2026-02-30", "../2026-11-18", "2026-11-18/", "1:2026-11-18/14-03-22_motion"}) {
        KV_CHECK_MSG(!decodeDayCursor(b64url(raw)), raw);
    }
    KV_CHECK(!decodeDayCursor(encodeDayCursor("2026-11-18") + "="));
}

// ---- the sidecar and finalizing -------------------------------------------------------------------

KV_TEST(sidecar_has_the_exact_fields_and_reads_leniently) {
    ClipSidecar s;
    s.clipId = "2026-11-18/14-03-22_manual";
    s.startUnixMs = 1795028597000ULL;
    s.endUnixMs = 1795028609500ULL;
    s.startLocal = "2026-11-18T14:03:17-05:00";
    s.durationSec = 12.5;
    s.trigger = ClipTrigger::Manual;
    s.startedBy = StartedBy{std::string("dev-a"), "Owner's iPhone"};
    s.starredBy = {{"dev-a", PetRole::Owner}, {"pc", PetRole::Owner}, {"dev-v", PetRole::Viewer}};
    s.width = 1280;
    s.height = 720;
    s.modeRaw = 1;
    s.sbsLayoutRaw = 0;
    s.bytes = 4096;
    s.preRollSec = 5.0;
    const std::string want =
        "{\"version\":1,\"clipId\":\"2026-11-18\\/14-03-22_manual\",\"startUnixMs\":1795028597000,"
        "\"endUnixMs\":1795028609500,\"startLocal\":\"2026-11-18T14:03:17-05:00\",\"durationSec\":12.5,"
        "\"trigger\":\"manual\",\"startedBy\":{\"deviceId\":\"dev-a\",\"deviceName\":\"Owner's iPhone\"},"
        "\"starred\":true,\"starredBy\":[{\"deviceId\":\"dev-a\",\"role\":\"owner\"},{\"deviceId\":\"pc\",\"role\":\"owner\"},"
        "{\"deviceId\":\"dev-v\",\"role\":\"viewer\"}],\"width\":1280,\"height\":720,\"modeRaw\":1,\"sbsLayoutRaw\":0,"
        "\"bytes\":4096,\"codec\":\"h264\",\"preRollSec\":5}";
    KV_CHECK_MSG(sidecarJson(s) == want, sidecarJson(s));
    ClipSidecar back;
    KV_REQUIRE(parseSidecar(want, back));
    KV_CHECK(back.clipId == s.clipId && back.startUnixMs == s.startUnixMs && back.endUnixMs == s.endUnixMs);
    KV_CHECK(back.startLocal == s.startLocal && back.durationSec == 12.5 && back.trigger == ClipTrigger::Manual);
    KV_CHECK(back.startedBy && back.startedBy->deviceId == std::optional<std::string>("dev-a") &&
             back.startedBy->deviceName == "Owner's iPhone");
    KV_CHECK(back.starredBy.size() == 3 && back.starredBy[2].role == PetRole::Viewer && back.starred());
    KV_CHECK(back.width == 1280 && back.height == 720 && back.modeRaw == 1 && back.bytes == 4096 && back.preRollSec == 5.0);

    // The PC's startedBy has no deviceId; motion and schedule clips never carry one.
    s.startedBy = StartedBy{std::nullopt, kPcDeviceName};
    s.starredBy.clear();
    KV_CHECK(sidecarJson(s).find("\"startedBy\":{\"deviceName\":\"This PC\"},\"starred\":false,\"starredBy\":[]") !=
             std::string::npos);
    s.trigger = ClipTrigger::Motion;
    KV_CHECK(sidecarJson(s).find("startedBy") == std::string::npos);

    // Lenient: unknown keys, wrong types and missing fields keep defaults; a star is never lost.
    ClipSidecar l;
    KV_REQUIRE(parseSidecar("{\"startUnixMs\":5,\"future\":[1,{\"x\":2}],\"width\":\"wide\",\"trigger\":\"teleport\","
                            "\"starredBy\":[{\"deviceId\":\"x\",\"role\":\"admin\"},{\"role\":\"owner\"},7,"
                            "{\"deviceId\":\"x\",\"role\":\"owner\"},{\"deviceId\":\"\",\"role\":\"owner\"}]}",
                            l));
    KV_CHECK(l.startUnixMs == 5 && l.width == 0 && l.trigger == ClipTrigger::Unknown && l.codec == "h264");
    KV_CHECK(l.starredBy.size() == 1 && l.starredBy[0].deviceId == "x" && l.starredBy[0].role == PetRole::Viewer);
    ClipSidecar st;
    KV_REQUIRE(parseSidecar("{\"starred\":true}", st));
    KV_CHECK(st.starred() && st.starredBy[0].deviceId == "pc" && st.starredBy[0].role == PetRole::Owner);
    ClipSidecar junk;
    KV_CHECK(!parseSidecar("not json", junk));
    KV_CHECK(!parseSidecar("[1,2]", junk));
    KV_CHECK(!parseSidecar("{\"version\":1", junk));
}

KV_TEST(finalize_writes_the_sidecar_last_and_atomically) {
    Fixture f(L"finalize");
    std::string err;
    const auto p = f.store->reserveClip(kT0 - 5000, ClipTrigger::Motion, &err);
    KV_REQUIRE(p.has_value());
    writeBytes(p->mp4, 7777, 'M');
    ClipSidecar s;
    s.startUnixMs = static_cast<uint64_t>(kT0 - 5000);
    s.durationSec = 20.25;
    s.trigger = ClipTrigger::Manual;                    // the name says motion: the name wins
    s.startedBy = StartedBy{std::string("d"), "Phone"}; // dropped for a motion clip
    s.width = 1024;
    s.height = 848;
    s.clipId = "../elsewhere";                           // ignored: the reservation names the clip
    KV_REQUIRE(f.store->finalizeClip(*p, s, &err));
    KV_CHECK(!exists(p->sidecar + L".tmp"));
    const ClipSidecar back = sidecarOf(f, p->clipId);
    KV_CHECK(back.clipId == "2026-11-18/14-03-17_motion");
    KV_CHECK(back.trigger == ClipTrigger::Motion && !back.startedBy);
    KV_CHECK(back.bytes == 7777 && back.codec == "h264" && back.version == 1);
    KV_CHECK(back.startLocal == "2026-11-18T14:03:17-05:00");
    KV_CHECK(back.endUnixMs == static_cast<uint64_t>(kT0 - 5000 + 20250));
    KV_CHECK(f.store->findClip(p->clipId).has_value());
    KV_CHECK(f.store->mp4Path(p->clipId) == std::optional<std::wstring>(p->mp4));
    KV_CHECK(!f.store->thumbnailPath(p->clipId)); // no .jpg was written

    // A reservation whose .mp4 vanished can't be finalized; an abandoned one leaves nothing.
    const auto q = f.store->reserveClip(kT0, ClipTrigger::Manual, &err);
    KV_REQUIRE(q.has_value());
    DeleteFileW(q->mp4.c_str());
    KV_CHECK(!f.store->finalizeClip(*q, ClipSidecar{}, &err));
    const auto r = f.store->reserveClip(kT0 + 1000, ClipTrigger::Manual, &err);
    KV_REQUIRE(r.has_value());
    f.store->abandonClip(*r);
    KV_CHECK(!exists(r->mp4) && !exists(r->sidecar));
    KV_CHECK(logLines(f.dir).empty()); // an empty reservation held nothing to log
}

// ---- listing ----------------------------------------------------------------------------------------

KV_TEST(clips_page_newest_first_with_cursors_and_limits) {
    Fixture f(L"pages");
    std::vector<std::pair<uint64_t, std::string>> all;
    for (int i = 0; i < 115; ++i) {
        const int64_t start = kT0 - 3600000 + i * 20000LL;
        const std::string id = addClip(f, start, i % 3 == 0 ? ClipTrigger::Motion : ClipTrigger::Manual, 64, false);
        all.push_back({static_cast<uint64_t>(start), id});
    }
    for (int i = 0; i < 5; ++i) { // the same millisecond five times: ties are ordered by id
        const std::string id = addClip(f, kT0, ClipTrigger::Schedule, 64, false);
        all.push_back({static_cast<uint64_t>(kT0), id});
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });

    std::vector<std::string> seen;
    std::optional<std::string> cursor;
    std::vector<size_t> pageSizes;
    do {
        const auto r = f.store->listClips("2026-11-18", cursor, std::nullopt, "");
        KV_REQUIRE(r.error == RollError::None);
        pageSizes.push_back(r.clips.size());
        for (const auto& c : r.clips) seen.push_back(c.clipId);
        cursor = r.nextCursor;
    } while (cursor && pageSizes.size() < 10);
    KV_CHECK(pageSizes == std::vector<size_t>({50, 50, 20}));
    KV_REQUIRE(seen.size() == all.size());
    for (size_t i = 0; i < all.size(); ++i) KV_CHECK_MSG(seen[i] == all[i].second, seen[i]);

    const auto big = f.store->listClips("2026-11-18", std::nullopt, 100, "");
    KV_CHECK(big.clips.size() == 100 && big.nextCursor);
    const auto one = f.store->listClips("2026-11-18", std::nullopt, 1, "");
    KV_CHECK(one.clips.size() == 1 && one.clips[0].clipId == all[0].second && one.nextCursor);
    const auto last = f.store->listClips("2026-11-18", std::nullopt, 100, "");
    const auto rest = f.store->listClips("2026-11-18", last.nextCursor, 100, "");
    KV_CHECK(rest.clips.size() == 20 && !rest.nextCursor);
    for (int bad : {0, -1, 101, 1000}) {
        KV_CHECK(f.store->listClips("2026-11-18", std::nullopt, bad, "").error == RollError::Invalid);
        KV_CHECK(f.store->listDays(std::nullopt, bad).error == RollError::Invalid);
    }

    // The anchor doesn't have to exist: delete the last clip of page 1, and page 2 still follows it.
    const auto p1 = f.store->listClips("2026-11-18", std::nullopt, 10, "");
    KV_REQUIRE(p1.nextCursor.has_value());
    KV_CHECK(f.store->deleteClip(p1.clips.back().clipId) == RollError::None);
    const auto p2 = f.store->listClips("2026-11-18", p1.nextCursor, 10, "");
    KV_REQUIRE(p2.error == RollError::None && !p2.clips.empty());
    KV_CHECK(p2.clips[0].clipId == all[10].second);

    // Cursors of the wrong kind or tampered are invalid.
    KV_CHECK(f.store->listClips("2026-11-18", encodeDayCursor("2026-11-18"), std::nullopt, "").error == RollError::Invalid);
    KV_CHECK(f.store->listDays(p1.nextCursor, std::nullopt).error == RollError::Invalid);
    std::string t = *p1.nextCursor;
    t.back() = t.back() == '_' ? '-' : '_'; // the last character: a non-canonical form, or a bad id
    KV_CHECK(f.store->listClips("2026-11-18", t, std::nullopt, "").error == RollError::Invalid);
    KV_CHECK(f.store->listClips("2026-11-18", b64url("1:../../victim"), std::nullopt, "").error == RollError::Invalid);
    KV_CHECK(f.store->listClips("2026-11-18", std::string("not a cursor"), std::nullopt, "").error == RollError::Invalid);
}

KV_TEST(days_and_clip_fields_list_as_the_wire_needs_them) {
    Fixture f(L"days");
    const std::string a = addClip(f, kT0 - 2 * kDayMs, ClipTrigger::Motion, 1000);
    const std::string b = addClip(f, kT0 - kDayMs, ClipTrigger::Manual, 2000, true, {{"dev-v", PetRole::Viewer}},
                                  StartedBy{std::string("dev-v"), "Viewer Phone"});
    const std::string c = addClip(f, kT0 - kDayMs + 1000, ClipTrigger::Manual, 3000, false, {}, StartedBy{std::nullopt, kPcDeviceName});
    const std::string d = addClip(f, kT0, ClipTrigger::Schedule, 4000, true, {{"pc", PetRole::Owner}});
    CreateDirectoryW((f.dir + L"\\2026-11-10").c_str(), nullptr); // an empty day isn't listed

    const auto p1 = f.store->listDays(std::nullopt, 2);
    KV_REQUIRE(p1.error == RollError::None && p1.days.size() == 2 && p1.nextCursor);
    KV_CHECK(p1.days[0].day == "2026-11-18" && p1.days[1].day == "2026-11-17");
    KV_CHECK(p1.days[1].clipCount == 2 && p1.days[1].starredCount == 1 && p1.days[1].bytes == 5000);
    KV_CHECK(p1.days[0].clipCount == 1 && p1.days[0].starredCount == 1 && p1.days[0].bytes == 4000);
    const auto p2 = f.store->listDays(p1.nextCursor, 2);
    KV_REQUIRE(p2.days.size() == 1);
    KV_CHECK(p2.days[0].day == "2026-11-16" && !p2.nextCursor);
    KV_CHECK(f.store->listDays(std::nullopt, std::nullopt).days.size() == 3);
    KV_CHECK(f.store->listDays(std::string("%%%"), std::nullopt).error == RollError::Invalid);
    KV_CHECK(f.store->listDays(b64url("../.."), std::nullopt).error == RollError::Invalid);
    // An empty day folder lists as an empty day rather than not_found.
    const auto empty = f.store->listClips("2026-11-10", std::nullopt, std::nullopt, "");
    KV_CHECK(empty.error == RollError::None && empty.clips.empty() && !empty.nextCursor);

    const auto viewer = f.store->listClips("2026-11-17", std::nullopt, std::nullopt, "dev-v");
    KV_REQUIRE(viewer.clips.size() == 2);
    const RollClipItem& pcClip = viewer.clips[0];
    const RollClipItem& vClip = viewer.clips[1];
    KV_CHECK(pcClip.clipId == c && vClip.clipId == b);
    KV_CHECK(pcClip.startedBy == std::optional<std::string>("This PC") && !pcClip.starred && !pcClip.starredByMe);
    KV_CHECK(!pcClip.hasThumbnail && pcClip.bytes == 3000);
    KV_CHECK(vClip.startedBy == std::optional<std::string>("Viewer Phone") && vClip.starred && vClip.starredByMe);
    KV_CHECK(vClip.hasThumbnail && vClip.width == 1280 && vClip.height == 720 && vClip.durationSec == 12.5);
    KV_CHECK(vClip.startLocal == "2026-11-17T14:03:22-05:00" && vClip.startUnixMs == static_cast<uint64_t>(kT0 - kDayMs));
    const auto other = f.store->listClips("2026-11-17", std::nullopt, std::nullopt, "dev-o");
    KV_CHECK(other.clips[1].starred && !other.clips[1].starredByMe);
    const auto motion = f.store->listClips("2026-11-16", std::nullopt, std::nullopt, "");
    KV_CHECK(motion.clips.size() == 1 && motion.clips[0].clipId == a && !motion.clips[0].startedBy &&
             motion.clips[0].trigger == ClipTrigger::Motion);
    KV_CHECK(f.store->thumbnailPath(d) == std::optional<std::wstring>(f.path(d, L".jpg")));

    const RollStorageInfo s = f.store->storage();
    KV_CHECK(s.capBytes == kDefaultCapBytes && s.keepDays == 30 && s.floorBytes == 20'000'000'000ULL);
    KV_CHECK(s.freeBytes == 100'000'000'000ULL && s.usedBytes >= 10000);
}

KV_TEST(listing_never_writes_and_reads_a_stale_or_broken_index_through_the_sidecars) {
    Fixture f(L"nowrite");
    const std::string a = addClip(f, kT0 - 60000, ClipTrigger::Motion);
    const std::string b = addClip(f, kT0, ClipTrigger::Manual);
    const std::wstring index = f.dir + L"\\2026-11-18\\index.json";
    const auto listEverything = [&] {
        f.store->listDays(std::nullopt, std::nullopt);
        f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "dev");
        f.store->listClips("2026-11-19", std::nullopt, std::nullopt, "dev");
        f.store->storage();
        f.store->findClip(a);
        f.store->thumbnailPath(b);
    };
    // No index at all: listing works and creates none.
    KV_CHECK(!exists(index));
    TreeState before = treeState(f.dir);
    listEverything();
    KV_CHECK(f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "").clips.size() == 2);
    KV_CHECK(treeState(f.dir) == before);
    KV_CHECK(!exists(index));

    // A broken index: still listed from the sidecars, still not rewritten.
    writeText(index, "{\"version\":1,\"day\":\"2026-11-18\",\"clips\":[{\"sidecar\":");
    before = treeState(f.dir);
    listEverything();
    KV_CHECK(f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "").clips.size() == 2);
    KV_CHECK(treeState(f.dir) == before);

    // An index naming a clip that isn't there, or missing one that is, is stale.
    writeText(index, "{\"version\":1,\"day\":\"2026-11-18\",\"clips\":[]}");
    setWriteTime(index, systemFileTimeNow() + 36000000000ULL); // an hour ahead: only the set differs
    before = treeState(f.dir);
    KV_CHECK(f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "").clips.size() == 2);
    KV_CHECK(treeState(f.dir) == before);
}

KV_TEST(index_is_rebuilt_by_the_passes_and_used_only_while_fresh) {
    Fixture f(L"index");
    const std::string a = addClip(f, kT0, ClipTrigger::Motion);
    addClip(f, kT0 - 7 * kDayMs, ClipTrigger::Motion);
    const std::wstring sidecar = f.path(a, L".json");
    const std::wstring index = f.dir + L"\\2026-11-18\\index.json";
    // Sidecars written an hour ago, so the index the pass writes now is strictly newer.
    const uint64_t past = systemFileTimeNow() - 36000000000ULL;
    setWriteTime(sidecar, past);
    setWriteTime(f.path(addClip(f, kT0 - 3 * kDayMs, ClipTrigger::Manual), L".json"), past);
    setWriteTime(f.dir + L"\\2026-11-11\\" + L"14-03-22_motion.json", past);

    PassReport r = f.store->retentionPass();
    KV_CHECK(r.indexesRebuilt == 3 && r.deleted.empty());
    KV_REQUIRE(exists(index));
    KV_CHECK(!exists(index + L".tmp"));
    KV_CHECK(f.store->retentionPass().indexesRebuilt == 0); // fresh indexes are left alone
    KV_CHECK(readText(index).find("\"sidecarWriteTime\":") != std::string::npos);

    // Change the sidecar behind the index's back, keeping its size and write time: listing serves the
    // index (the cache is really used).
    std::string text = readText(sidecar);
    const size_t at = text.find("\"durationSec\":12.5");
    KV_REQUIRE(at != std::string::npos);
    text.replace(at, 18, "\"durationSec\":99.5");
    writeText(sidecar, text);
    setWriteTime(sidecar, past);
    KV_CHECK(f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "").clips[0].durationSec == 12.5);

    // A sidecar as new as the index makes it stale: listing reads the sidecar, and only the next pass
    // rewrites the index.
    setWriteTime(sidecar, writeTimeOf(index));
    const uint64_t indexTime = writeTimeOf(index);
    KV_CHECK(f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "").clips[0].durationSec == 99.5);
    KV_CHECK(writeTimeOf(index) == indexTime);
    r = f.store->retentionPass();
    KV_CHECK(r.indexesRebuilt == 1);
    KV_CHECK(readText(index).find("99.5") != std::string::npos);

    // A missing index comes back at start-up.
    DeleteFileW(index.c_str());
    KV_CHECK(f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "").clips.size() == 1);
    KV_CHECK(!exists(index));
    r = f.store->startupPass();
    KV_CHECK(r.indexesRebuilt >= 1 && exists(index));
}

// ---- stars, revoke, delete ----------------------------------------------------------------------------

KV_TEST(stars_follow_the_role_rules_and_rewrite_only_the_sidecar) {
    Fixture f(L"stars");
    const std::string id = addClip(f, kT0, ClipTrigger::Motion);
    f.store->retentionPass(); // an index exists
    const std::wstring mp4 = f.path(id, L".mp4"), jpg = f.path(id, L".jpg");
    const std::wstring index = f.dir + L"\\2026-11-18\\index.json";
    const auto others = [&] {
        return std::vector<uint64_t>{sizeOf(mp4), writeTimeOf(mp4), sizeOf(jpg), writeTimeOf(jpg), sizeOf(index),
                                     writeTimeOf(index)};
    };
    const auto untouched = others();
    const auto stars = [&] {
        std::vector<std::string> v;
        for (const auto& e : sidecarOf(f, id).starredBy) v.push_back(e.deviceId + "/" + petRoleString(e.role));
        return v;
    };

    // A viewer with no star of its own may not unstar.
    KV_CHECK(f.store->setStar(id, "viewer-1", PetRole::Viewer, false) == RollError::NotAllowedForRole);
    KV_CHECK(f.store->setStar(id, "viewer-1", PetRole::Viewer, true) == RollError::None);
    KV_CHECK(f.store->setStar(id, "viewer-1", PetRole::Viewer, true) == RollError::None); // once only
    KV_CHECK(f.store->setStar(id, "owner-1", PetRole::Owner, true) == RollError::None);
    KV_CHECK(f.store->setStar(id, "viewer-2", PetRole::Viewer, true) == RollError::None);
    KV_CHECK(stars() == std::vector<std::string>({"viewer-1/viewer", "owner-1/owner", "viewer-2/viewer"}));
    KV_CHECK(sidecarOf(f, id).starred());
    // A viewer removes only its own; the others stay.
    KV_CHECK(f.store->setStar(id, "viewer-2", PetRole::Viewer, false) == RollError::None);
    KV_CHECK(stars() == std::vector<std::string>({"viewer-1/viewer", "owner-1/owner"}));
    KV_CHECK(f.store->setStar(id, "viewer-2", PetRole::Viewer, false) == RollError::NotAllowedForRole);
    KV_CHECK(stars() == std::vector<std::string>({"viewer-1/viewer", "owner-1/owner"}));
    // An owner's unstar clears everyone's, even when it set none itself.
    KV_CHECK(f.store->setStar(id, "owner-2", PetRole::Owner, false) == RollError::None);
    KV_CHECK(stars().empty() && !sidecarOf(f, id).starred());
    KV_CHECK(f.store->setStar(id, "owner-2", PetRole::Owner, false) == RollError::None); // nothing to clear
    // The PC is an owner with deviceId "pc".
    KV_CHECK(f.store->setStar(id, "viewer-1", PetRole::Viewer, true) == RollError::None);
    KV_CHECK(f.store->setStar(id, kPcDeviceId, PetRole::Owner, true) == RollError::None);
    KV_CHECK(stars() == std::vector<std::string>({"viewer-1/viewer", "pc/owner"}));
    KV_CHECK(f.store->setStar(id, kPcDeviceId, PetRole::Owner, false) == RollError::None);
    KV_CHECK(stars().empty());
    // Bad callers.
    KV_CHECK(f.store->setStar(id, "", PetRole::Owner, true) == RollError::Invalid);
    KV_CHECK(f.store->setStar(id, "u", PetRole::Unknown, true) == RollError::NotAllowedForRole);
    KV_CHECK(f.store->setStar("2026-11-18/14-03-23_motion", "o", PetRole::Owner, true) == RollError::NotFound);
    KV_CHECK(rollErrorCode(RollError::NotAllowedForRole) == std::string("not_allowed_for_role"));
    KV_CHECK(rollErrorCode(RollError::NotFound) == std::string("not_found"));
    KV_CHECK(rollErrorCode(RollError::None) == nullptr);

    // Only the sidecar changed: the .mp4, the .jpg and the index are as they were.
    KV_CHECK(others() == untouched);
    KV_CHECK(!exists(f.path(id, L".json.tmp")));
    // And listing sees the current stars at once (the index is now stale).
    KV_CHECK(f.store->setStar(id, "viewer-1", PetRole::Viewer, true) == RollError::None);
    const auto l = f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "viewer-1");
    KV_CHECK(l.clips.size() == 1 && l.clips[0].starred && l.clips[0].starredByMe);
}

KV_TEST(revoking_a_device_drops_its_stars_across_the_roll) {
    Fixture f(L"revoke");
    const std::string a = addClip(f, kT0 - 3 * kDayMs, ClipTrigger::Motion, 100, true, {{"gone", PetRole::Viewer}});
    const std::string b = addClip(f, kT0, ClipTrigger::Motion, 100, true,
                                  {{"keep", PetRole::Owner}, {"gone", PetRole::Viewer}, {"pc", PetRole::Owner}});
    const std::string c = addClip(f, kT0 + 1000, ClipTrigger::Manual, 100, true, {{"keep", PetRole::Viewer}});
    const uint64_t cTime = writeTimeOf(f.path(c, L".json"));
    KV_CHECK(f.store->removeStarsByDevice("gone") == 2);
    KV_CHECK(!sidecarOf(f, a).starred());
    const auto bStars = sidecarOf(f, b).starredBy;
    KV_CHECK(bStars.size() == 2 && bStars[0].deviceId == "keep" && bStars[1].deviceId == "pc");
    KV_CHECK(sidecarOf(f, c).starredBy.size() == 1);
    KV_CHECK(writeTimeOf(f.path(c, L".json")) == cTime); // a clip without its star isn't rewritten
    KV_CHECK(f.store->removeStarsByDevice("gone") == 0);
    KV_CHECK(f.store->removeStarsByDevice(kPcDeviceId) == 0); // the PC is never revoked
    KV_CHECK(f.store->removeStarsByDevice("") == 0);
    KV_CHECK(sidecarOf(f, b).starredBy.size() == 2);
}

KV_TEST(delete_removes_all_three_files_and_logs_like_the_depth_ir_log) {
    Fixture f(L"delete");
    const std::string id = addClip(f, kT0, ClipTrigger::Manual, 4096, true);
    const std::string keep = addClip(f, kT0 + 1000, ClipTrigger::Manual, 4096, true);
    const uint64_t bytes = sizeOf(f.path(id, L".mp4")) + sizeOf(f.path(id, L".json")) + sizeOf(f.path(id, L".jpg"));
    KV_CHECK(f.store->deleteClip(id) == RollError::None);
    KV_CHECK(!exists(f.path(id, L".mp4")) && !exists(f.path(id, L".json")) && !exists(f.path(id, L".jpg")));
    KV_CHECK(exists(f.path(keep, L".mp4")));
    // "<ISO-8601 UTC>\t<name>\t<N> bytes\t<trigger>", as recordings.cpp deletionLogLine writes it.
    const auto lines = logLines(f.dir);
    KV_REQUIRE(lines.size() == 1);
    KV_CHECK_MSG(lines[0] == "2026-11-18T19:04:22Z\t2026-11-18/14-03-22_manual\t" + std::to_string(bytes) + " bytes\tuser",
                 lines[0]);
    KV_CHECK(f.store->deleteClip(id) == RollError::NotFound);
    KV_CHECK(!f.store->findClip(id));
    KV_CHECK(logLines(f.dir).size() == 1);
}

// ---- leftovers ----------------------------------------------------------------------------------------

KV_TEST(startup_recovers_or_deletes_leftovers_and_finishes_cut_short_writes) {
    Fixture f(L"leftovers");
    const std::wstring day = f.dir + L"\\2026-11-18";
    CreateDirectoryW(day.c_str(), nullptr);
    // An interrupted clip the probe can read: its sidecar is rebuilt.
    writeBytes(day + L"\\14-00-00_motion.mp4", 9000, 'M');
    writeBytes(day + L"\\14-00-00_motion.jpg", 300, 'J');
    // One it can't read, and one with a thumbnail only: both deleted and logged.
    writeBytes(day + L"\\14-10-00_manual.mp4", 5000, 'M');
    writeBytes(day + L"\\14-10-00_manual.jpg", 200, 'J');
    // An empty reservation from a crash before the first byte: deleted without probing.
    writeBytes(day + L"\\14-20-00_schedule.mp4", 0, 'M');
    // A sidecar whose .mp4 is gone (a delete cut short): finished and logged.
    writeText(day + L"\\14-30-00_motion.json", "{\"version\":1}");
    writeBytes(day + L"\\14-30-00_motion.jpg", 100, 'J');
    // Atomic writes cut short, and a stranger's file that must be left alone.
    writeText(day + L"\\14-40-00_motion.json.tmp", "{");
    writeText(day + L"\\index.json.tmp", "{");
    writeText(day + L"\\notes.txt", "mine");
    writeText(day + L"\\14-50-00_motion.MP4.bak", "x");

    f.probe = [](const std::wstring& p) -> std::optional<ProbedClip> {
        if (p.find(L"14-00-00_motion.mp4") == std::wstring::npos) return std::nullopt;
        ProbedClip c;
        c.durationSec = 31.5;
        c.width = 1024;
        c.height = 848;
        c.startUnixMs = 1795028400000ULL; // 14:00:00 local
        c.modeRaw = 2;
        return c;
    };
    const PassReport r = f.store->startupPass();
    KV_CHECK(f.probeCalls == 2); // the readable one and the unreadable one, not the empty one
    KV_CHECK(r.recovered == std::vector<std::string>({"2026-11-18/14-00-00_motion"}));
    const std::set<std::string> deleted(r.deleted.begin(), r.deleted.end());
    KV_CHECK(deleted == std::set<std::string>({"2026-11-18/14-10-00_manual", "2026-11-18/14-20-00_schedule",
                                               "2026-11-18/14-30-00_motion"}));
    const ClipSidecar s = sidecarOf(f, "2026-11-18/14-00-00_motion");
    KV_CHECK(s.clipId == "2026-11-18/14-00-00_motion" && s.trigger == ClipTrigger::Motion && s.durationSec == 31.5);
    KV_CHECK(s.width == 1024 && s.height == 848 && s.modeRaw == 2 && s.bytes == 9000 && !s.starred());
    KV_CHECK(s.startUnixMs == 1795028400000ULL && s.endUnixMs == 1795028431500ULL);
    KV_CHECK(s.startLocal == "2026-11-18T14:00:00-05:00");
    KV_CHECK(!exists(day + L"\\14-10-00_manual.mp4") && !exists(day + L"\\14-10-00_manual.jpg"));
    KV_CHECK(!exists(day + L"\\14-20-00_schedule.mp4"));
    KV_CHECK(!exists(day + L"\\14-30-00_motion.json") && !exists(day + L"\\14-30-00_motion.jpg"));
    KV_CHECK(!exists(day + L"\\14-40-00_motion.json.tmp") && !exists(day + L"\\index.json.tmp"));
    KV_CHECK(exists(day + L"\\notes.txt") && exists(day + L"\\14-50-00_motion.MP4.bak"));
    const auto lines = logLines(f.dir);
    KV_CHECK(lines.size() == 3);
    bool sawManual = false;
    for (const auto& l : lines) {
        KV_CHECK(l.size() > 21 && l.substr(l.size() - 10) == "\tautomatic");
        if (l == "2026-11-18T19:04:22Z\t2026-11-18/14-10-00_manual\t5200 bytes\tautomatic") sawManual = true;
    }
    KV_CHECK(sawManual);
    const auto list = f.store->listClips("2026-11-18", std::nullopt, std::nullopt, "");
    KV_CHECK(list.clips.size() == 1 && list.clips[0].hasThumbnail);
    KV_CHECK(exists(day + L"\\index.json")); // the pass rebuilds the missing index

    // With no probe at all, an interrupted clip is deleted and logged.
    Fixture g(L"noprobe");
    g.withProbe = false;
    g.open();
    CreateDirectoryW((g.dir + L"\\2026-11-18").c_str(), nullptr);
    writeBytes(g.dir + L"\\2026-11-18\\14-00-00_motion.mp4", 9000, 'M');
    const PassReport gr = g.store->startupPass();
    KV_CHECK(gr.recovered.empty() && gr.deleted.size() == 1 && g.probeCalls == 0);
    KV_CHECK(!exists(g.dir + L"\\2026-11-18")); // its day folder emptied, then removed
    KV_CHECK(logLines(g.dir).size() == 1);
}

// The start-up pass recovers and indexes, and never runs retention. At start the user's keep days and cap
// aren't known yet (pet-camera.json is read after the roll starts, and may be unreadable); the defaults must
// never delete a clip. Only retentionPass, which the host runs once the user's limits are applied, deletes.
KV_TEST(startup_pass_never_runs_retention) {
    Fixture f(L"startup-noretention");
    const int64_t now = f.nowMs;
    const std::string old = addClip(f, now - 40 * kDayMs, ClipTrigger::Motion); // past the default 30 days
    const std::string young = addClip(f, now - 1000, ClipTrigger::Manual);
    f.store->setLimits(0, 1); // even past a 0-byte cap and a 1-day limit
    const PassReport r = f.store->startupPass();
    KV_CHECK(r.deleted.empty() && !r.storageFull);
    KV_CHECK(f.store->findClip(old) && f.store->findClip(young) && logLines(f.dir).empty());
    // The same limits, given to retention: both go.
    const PassReport rr = f.store->retentionPass();
    KV_CHECK(rr.deleted.size() == 2 && !f.store->findClip(old) && !f.store->findClip(young));
}

// ---- retention ------------------------------------------------------------------------------------------

KV_TEST(retention_deletes_unstarred_clips_past_keep_days_and_empty_days) {
    Fixture f(L"keepdays");
    const int64_t now = f.nowMs;
    const std::string old = addClip(f, now - 31 * kDayMs, ClipTrigger::Motion);
    const std::string edge = addClip(f, now - 30 * kDayMs, ClipTrigger::Motion);           // exactly 30 days: goes
    const std::string young = addClip(f, now - 30 * kDayMs + 1, ClipTrigger::Manual);      // a millisecond younger: stays
    const std::string starredOld = addClip(f, now - 90 * kDayMs, ClipTrigger::Motion, 4096, true, {{"v", PetRole::Viewer}});
    const std::string today = addClip(f, now - 1000, ClipTrigger::Manual);
    writeText(f.dir + L"\\2026-10-18\\index.json", "{}"); // an index in a day that empties
    const PassReport r = f.store->retentionPass();
    const std::set<std::string> deleted(r.deleted.begin(), r.deleted.end());
    KV_CHECK(deleted == std::set<std::string>({old, edge}));
    KV_CHECK(f.store->findClip(young) && f.store->findClip(starredOld) && f.store->findClip(today));
    KV_CHECK(!exists(f.dir + L"\\2026-10-18")); // emptied, index and all
    KV_CHECK(r.dayFoldersRemoved == 1);
    KV_CHECK(exists(f.dir + L"\\2026-08-20")); // the starred clip's day stays
    KV_CHECK(!r.storageFull);
    const auto lines = logLines(f.dir);
    KV_CHECK(lines.size() == 2);
    for (const auto& l : lines) KV_CHECK(l.find("\tautomatic") != std::string::npos);

    // keep days come from the PC's settings.
    f.store->setLimits(kDefaultCapBytes, 7);
    const PassReport r2 = f.store->retentionPass();
    KV_CHECK(r2.deleted == std::vector<std::string>({young}));
    KV_CHECK(f.store->storage().keepDays == 7);

    // A day folder that still holds a stranger's file stays.
    CreateDirectoryW((f.dir + L"\\2026-01-02").c_str(), nullptr);
    writeText(f.dir + L"\\2026-01-02\\notes.txt", "mine");
    f.store->retentionPass();
    KV_CHECK(exists(f.dir + L"\\2026-01-02\\notes.txt"));
}

KV_TEST(retention_over_the_cap_deletes_oldest_unstarred_first_and_never_starred) {
    Fixture f(L"cap");
    constexpr size_t kClip = 100000;
    std::vector<std::string> ids;
    for (int i = 0; i < 6; ++i) {
        std::vector<StarEntry> stars;
        if (i == 0 || i == 3) stars.push_back({"o", PetRole::Owner}); // the oldest is starred
        ids.push_back(addClip(f, f.nowMs - (10 - i) * 3600000LL, ClipTrigger::Motion, kClip, true, stars));
    }
    const uint64_t used = f.store->storage().usedBytes;
    // Room for all but about 2.5 clips: three of the oldest unstarred go (1, 2, 4), never 0 or 3.
    f.store->setLimits(used - kClip * 5 / 2, 30);
    const PassReport r = f.store->retentionPass();
    KV_CHECK(r.deleted == std::vector<std::string>({ids[1], ids[2], ids[4]}));
    KV_CHECK(f.store->findClip(ids[0]) && f.store->findClip(ids[3]) && f.store->findClip(ids[5]));
    KV_CHECK(!r.storageFull && !f.store->storageFull());
    KV_CHECK(f.store->storage().usedBytes <= used - kClip * 5 / 2);

    // Starred clips alone over the cap: everything unstarred goes, starred stays, and storage_full.
    f.store->setLimits(kClip + kClip / 2, 30);
    const PassReport full = f.store->retentionPass();
    KV_CHECK(full.deleted == std::vector<std::string>({ids[5]}));
    KV_CHECK(full.storageFull && f.store->storageFull());
    KV_CHECK(f.store->findClip(ids[0]) && f.store->findClip(ids[3]));
    // Unstarring one brings the starred total under the cap at once.
    KV_CHECK(f.store->setStar(ids[3], "o", PetRole::Owner, false) == RollError::None);
    KV_CHECK(!f.store->storageFull());
    KV_CHECK(f.store->setStar(ids[3], "v", PetRole::Viewer, true) == RollError::None);
    KV_CHECK(f.store->storageFull());
    KV_CHECK(f.store->removeStarsByDevice("v") == 1);
    KV_CHECK(!f.store->storageFull());
}

// ---- the disk floor ---------------------------------------------------------------------------------------

KV_TEST(disk_floor_stops_below_20gb_and_resumes_above_21gb_without_deleting) {
    Fixture f(L"floor");
    const std::string a = addClip(f, kT0 - kDayMs, ClipTrigger::Motion);
    const std::string b = addClip(f, kT0, ClipTrigger::Manual);
    f.freeSpace = 25'000'000'000ULL;
    KV_CHECK(f.store->checkDiskFloor() && !f.store->diskFloorBlocked());
    f.freeSpace = 20'000'000'000ULL; // not below the floor
    KV_CHECK(f.store->checkDiskFloor());
    f.freeSpace = 19'999'999'999ULL;
    KV_CHECK(!f.store->checkDiskFloor() && f.store->diskFloorBlocked());
    f.freeSpace = 20'500'000'000ULL; // between the two: still stopped
    KV_CHECK(!f.store->checkDiskFloor());
    f.freeSpace = 21'000'000'000ULL; // not above the resume line
    KV_CHECK(!f.store->checkDiskFloor());
    f.freeSpace = 21'000'000'001ULL;
    KV_CHECK(f.store->checkDiskFloor() && !f.store->diskFloorBlocked());
    f.freeSpace = 20'500'000'000ULL; // between the two after resuming: still running
    KV_CHECK(f.store->checkDiskFloor());
    f.freeSpace = std::nullopt; // the free space can't be read: fail closed
    KV_CHECK(!f.store->checkDiskFloor());
    f.freeSpace = 1'000'000'000ULL;
    KV_CHECK(!f.store->checkDiskFloor());
    // The floor never deletes beyond the cap and the day limit.
    const PassReport r = f.store->retentionPass();
    KV_CHECK(r.deleted.empty());
    KV_CHECK(f.store->findClip(a) && f.store->findClip(b));
    KV_CHECK(f.store->storage().freeBytes == 1'000'000'000ULL);
}

// ---- folder protection ------------------------------------------------------------------------------------

KV_TEST(roll_folder_is_protected_and_refuses_unsafe_places) {
    const std::wstring base = freshDir(L"folder");
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring recordings = base + L"\\recordings";
    CreateDirectoryW(recordings.c_str(), nullptr);
    writeText(recordings + L"\\deletion-log.txt", "");
    std::string err;

    const std::wstring good = base + L"\\roll";
    KV_REQUIRE(prepareRollFolder(good, recordings, &err));
    KV_CHECK(isOwnerOnly(good));
    KV_CHECK((GetFileAttributesW(good.c_str()) & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0);
    KV_CHECK(exists(good + L"\\.nosync") && exists(good + L"\\.metadata_never_index"));
    // A roll with days and its log is prepared again at every start.
    CreateDirectoryW((good + L"\\2026-11-18").c_str(), nullptr);
    writeText(good + L"\\deletion-log.txt", "x\n");
    writeText(good + L"\\desktop.ini", "");
    KV_CHECK_MSG(prepareRollFolder(good + L"\\", recordings, &err), err);

    // The depth/IR recordings folder, anything inside it, and anything holding it.
    for (const std::wstring& p : {recordings, recordings + L"\\", recordings + L"\\roll", recordings + L"\\a\\b",
                                  base + L"\\RECORDINGS\\Roll", base + L"\\recordings\\..\\recordings\\x"}) {
        err.clear();
        KV_CHECK(!prepareRollFolder(p, recordings, &err));
        KV_CHECK(err.find("recordings") != std::string::npos);
    }
    KV_CHECK(!exists(recordings + L"\\roll"));
    KV_CHECK(!isOwnerOnly(recordings));
    KV_CHECK(!prepareRollFolder(base, recordings, &err));
    // A recordings folder reached through a junction is the same folder.
    const std::wstring alias = base + L"\\alias";
    {
        CreateDirectoryW(alias.c_str(), nullptr);
        HANDLE h = CreateFileW(alias.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        // A mount-point reparse buffer (the layout of REPARSE_DATA_BUFFER's MountPointReparseBuffer).
        const std::wstring target = L"\\??\\" + recordings;
        const USHORT subBytes = static_cast<USHORT>(target.size() * sizeof(wchar_t));
        std::vector<BYTE> buf(8 + 8 + subBytes + 2 * sizeof(wchar_t), 0);
        auto put16 = [&](size_t at, USHORT v) { memcpy(buf.data() + at, &v, 2); };
        const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        memcpy(buf.data(), &tag, 4);
        put16(4, static_cast<USHORT>(buf.size() - 8)); // ReparseDataLength
        put16(8, 0);                                   // SubstituteNameOffset
        put16(10, subBytes);                           // SubstituteNameLength
        put16(12, static_cast<USHORT>(subBytes + 2));  // PrintNameOffset
        put16(14, 0);                                  // PrintNameLength
        memcpy(buf.data() + 16, target.c_str(), subBytes);
        DWORD ret = 0;
        const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr,
                                        0, &ret, nullptr);
        CloseHandle(h);
        KV_REQUIRE(ok);
    }
    KV_CHECK(!prepareRollFolder(alias + L"\\roll", recordings, &err));
    KV_CHECK(!prepareRollFolder(alias, recordings, &err));
    KV_CHECK(!exists(recordings + L"\\roll"));

    // OneDrive, network paths and network drives.
    FolderCheckOptions od;
    od.extraOneDriveRoots = {base + L"\\OneDrive"};
    KV_CHECK(!prepareRollFolder(base + L"\\OneDrive\\roll", recordings, &err, od));
    KV_CHECK(err.find("OneDrive") != std::string::npos);
    for (const std::wstring p : {L"\\\\server\\share\\roll", L"\\\\?\\UNC\\server\\share\\roll", L"//server/share/roll"}) {
        KV_CHECK(!prepareRollFolder(p, recordings, &err));
    }
    // device-path forms of a folder in this test's own temp tree, never of a drive root, so a regression that
    // accepted them would create a folder here and nowhere else (and the check below would catch it).
    for (const std::wstring p : {L"\\\\?\\" + base + L"\\devroll", L"\\\\.\\" + base + L"\\devroll"}) {
        KV_CHECK(!prepareRollFolder(p, recordings, &err));
    }
    KV_CHECK(!exists(base + L"\\devroll"));
    FolderCheckOptions net;
    net.driveType = [](const std::wstring&) { return static_cast<unsigned>(DRIVE_REMOTE); };
    KV_CHECK(!prepareRollFolder(base + L"\\netroll", recordings, &err, net));
    KV_CHECK(err.find("network") != std::string::npos);
    KV_CHECK(!exists(base + L"\\netroll"));

    // A folder of other things is never made owner-only or used.
    const std::wstring docs = base + L"\\docs";
    CreateDirectoryW(docs.c_str(), nullptr);
    writeText(docs + L"\\letter.txt", "dear");
    KV_CHECK(!prepareRollFolder(docs, recordings, &err));
    KV_CHECK(!isOwnerOnly(docs));
    const std::wstring sub = base + L"\\sub";
    CreateDirectoryW(sub.c_str(), nullptr);
    CreateDirectoryW((sub + L"\\photos").c_str(), nullptr);
    KV_CHECK(!prepareRollFolder(sub, recordings, &err));
    writeText(base + L"\\file", "x");
    KV_CHECK(!prepareRollFolder(base + L"\\file", recordings, &err));
    KV_CHECK(!prepareRollFolder(L"", recordings, &err));

    removeTree(base);
}

KV_TEST(a_junction_named_like_a_day_is_never_followed) {
    Fixture f(L"junction");
    const std::wstring outside = f.base + L"\\outside";
    CreateDirectoryW(outside.c_str(), nullptr);
    writeBytes(outside + L"\\14-03-22_motion.mp4", 100, 'M');
    writeText(outside + L"\\14-03-22_motion.json", "{\"version\":1,\"startUnixMs\":1000}");
    const std::wstring link = f.dir + L"\\2020-01-01";
    CreateDirectoryW(link.c_str(), nullptr);
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    const std::wstring target = L"\\??\\" + outside;
    const USHORT subBytes = static_cast<USHORT>(target.size() * sizeof(wchar_t));
    std::vector<BYTE> buf(16 + subBytes + 2 * sizeof(wchar_t), 0);
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    const USHORT len = static_cast<USHORT>(buf.size() - 8), printOff = static_cast<USHORT>(subBytes + 2);
    memcpy(buf.data(), &tag, 4);
    memcpy(buf.data() + 4, &len, 2);
    memcpy(buf.data() + 10, &subBytes, 2);
    memcpy(buf.data() + 12, &printOff, 2);
    memcpy(buf.data() + 16, target.c_str(), subBytes);
    DWORD ret = 0;
    const BOOL ok =
        DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &ret, nullptr);
    CloseHandle(h);
    KV_REQUIRE(ok);
    KV_REQUIRE(exists(link + L"\\14-03-22_motion.mp4")); // the junction works

    f.store->setLimits(0, 1); // everything unstarred is past the cap and the day limit
    const PassReport r = f.store->startupPass();
    KV_CHECK(r.deleted.empty());
    const PassReport rr = f.store->retentionPass(); // retention doesn't follow the junction either
    KV_CHECK(rr.deleted.empty());
    KV_CHECK(exists(outside + L"\\14-03-22_motion.mp4") && exists(outside + L"\\14-03-22_motion.json"));
    KV_CHECK(!exists(outside + L"\\index.json"));
    KV_CHECK(f.store->listDays(std::nullopt, std::nullopt).days.empty());
    KV_CHECK(f.store->listClips("2020-01-01", std::nullopt, std::nullopt, "").error == RollError::NotFound);
    KV_CHECK(!f.store->findClip("2020-01-01/14-03-22_motion"));
    KV_CHECK(f.store->deleteClip("2020-01-01/14-03-22_motion") == RollError::NotFound);
    KV_CHECK(f.store->setStar("2020-01-01/14-03-22_motion", "o", PetRole::Owner, true) == RollError::NotFound);
}

// ---- Storage cap, retention, deletes, the index and test-roll folders ----------------------------------------

// storage_full is measured against the owner's cap only. A store whose cap was never set (the default, which
// nobody chose) never raises it, however much is starred; once the owner's cap is set, it does.
KV_TEST(storage_full_is_never_raised_against_a_cap_the_owner_didnt_set) {
    Fixture f(L"s3-cap");
    f.store.reset();
    RollConfig c;
    c.dir = f.dir;
    c.nowUnixMs = [&f] { return f.nowMs; };
    c.toLocal = fakeLocal;
    c.freeBytes = [&f](const std::wstring&) { return f.freeSpace; };
    c.capBytes = 1000; // stands in for the default cap: tiny, so a test can pass it
    f.store = std::make_unique<RollStore>(c);
    const std::string id = addClip(f, f.nowMs - 1000, ClipTrigger::Manual, 50000, true, {{"o", PetRole::Owner}});
    KV_CHECK(f.store->setStar(id, "v", PetRole::Viewer, true) == RollError::None); // a star change re-measures
    KV_CHECK(!f.store->storageFull());
    KV_CHECK(!f.store->retentionPass().storageFull);
    KV_CHECK(!f.store->storageFull() && f.store->findClip(id));
    // The owner's cap, the same size: now the starred clip alone is over it.
    f.store->setLimits(1000, 30);
    KV_CHECK(f.store->retentionPass().storageFull && f.store->storageFull());
}

// A sidecar that can't be READ (another program holds it) never counts as unstarred. Retention skips the clip,
// and no index is written from the failed read. The old code read it as defaults (unstarred) and deleted it.
KV_TEST(retention_skips_a_clip_whose_sidecar_cant_be_read) {
    Fixture f(L"m1-held");
    const int64_t now = f.nowMs;
    const std::string starredOld = addClip(f, now - 40 * kDayMs, ClipTrigger::Motion, 4096, true, {{"o", PetRole::Owner}});
    const std::string young = addClip(f, now - 1000, ClipTrigger::Manual);
    const std::wstring dayDir = f.dir + L"\\" + std::wstring(starredOld.begin(), starredOld.begin() + 10);
    DeleteFileW((dayDir + L"\\index.json").c_str()); // no index: the pass must read the sidecars
    HANDLE held = CreateFileW(f.path(starredOld, L".json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_REQUIRE(held != INVALID_HANDLE_VALUE);
    f.store->setLimits(kDefaultCapBytes, 30);
    const PassReport r = f.store->retentionPass();
    KV_CHECK(r.deleted.empty());
    KV_CHECK(exists(f.path(starredOld, L".mp4")) && exists(f.path(starredOld, L".jpg")));
    KV_CHECK(!exists(dayDir + L"\\index.json")); // nothing written from a read that failed
    CloseHandle(held);
    // Readable again: it is starred, so it stays; and now the day's index is written.
    const PassReport r2 = f.store->retentionPass();
    KV_CHECK(r2.deleted.empty() && f.store->findClip(starredOld) && f.store->findClip(young));
    KV_CHECK(exists(dayDir + L"\\index.json"));
}

// An .mp4 that can't be deleted (a player holds it) leaves the clip whole, thumbnail and all: the .mp4 goes
// first. The old order removed the .jpg first and left a clip without its thumbnail. Retention skips a pinned clip.
KV_TEST(a_delete_with_a_held_mp4_leaves_the_clip_whole_and_retention_skips_a_pinned_clip) {
    Fixture f(L"m2-held");
    const std::string id = addClip(f, f.nowMs - 1000, ClipTrigger::Manual, 4096, true);
    HANDLE held = CreateFileW(f.path(id, L".mp4").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_REQUIRE(held != INVALID_HANDLE_VALUE);
    KV_CHECK(f.store->deleteClip(id) == RollError::Internal);
    KV_CHECK(exists(f.path(id, L".mp4")) && exists(f.path(id, L".jpg")) && exists(f.path(id, L".json")));
    KV_CHECK(f.store->findClip(id).has_value());
    CloseHandle(held);

    const std::string old = addClip(f, f.nowMs - 40 * kDayMs, ClipTrigger::Motion);
    f.store->setLimits(kDefaultCapBytes, 30);
    f.store->pinClip(old);
    KV_CHECK(f.store->retentionPass().deleted.empty() && f.store->findClip(old));
    f.store->unpinClip(old);
    KV_CHECK(f.store->retentionPass().deleted == std::vector<std::string>({old}));
}

// A day with more than 1024 clips keeps an index that reads back. json_lite's container limit (1024) is for
// what devices send; the host's own index is parsed with a raised one. The old code couldn't parse the index it
// wrote, so the day was read from its sidecars every time and the index rewritten on every pass.
KV_TEST(an_index_of_more_than_1024_clips_stays_fresh) {
    Fixture f(L"m5-index");
    const std::wstring day = f.dir + L"\\2026-11-18";
    CreateDirectoryW(day.c_str(), nullptr);
    // 1100 synthetic sidecars (no video): 14:00:00 onward, one a second, each with an empty .mp4 beside it.
    for (int i = 0; i < 1100; ++i) {
        const int sod = 14 * 3600 + i;
        char name[32];
        std::snprintf(name, sizeof(name), "%02d-%02d-%02d_motion", sod / 3600, sod / 60 % 60, sod % 60);
        const std::wstring stem = day + L"\\" + std::wstring(name, name + std::strlen(name));
        writeBytes(stem + L".mp4", 16, 'M');
        ClipSidecar s;
        s.clipId = std::string("2026-11-18/") + name;
        s.startUnixMs = 1795028400000ULL + static_cast<uint64_t>(i) * 1000;
        s.trigger = ClipTrigger::Motion;
        writeText(stem + L".json", sidecarJson(s));
    }
    const PassReport first = f.store->startupPass();
    KV_CHECK(first.indexesRebuilt == 1);
    KV_REQUIRE(exists(day + L"\\index.json"));
    const uint64_t written = writeTimeOf(day + L"\\index.json");
    // Fresh and readable: the next passes don't rewrite it, and listing is served from it.
    const PassReport second = f.store->startupPass();
    KV_CHECK_MSG(second.indexesRebuilt == 0, std::to_string(second.indexesRebuilt));
    KV_CHECK(writeTimeOf(day + L"\\index.json") == written);
    const auto days = f.store->listDays(std::nullopt, std::nullopt);
    KV_REQUIRE(days.days.size() == 1);
    KV_CHECK(days.days[0].clipCount == 1100);
    // Devices' limits are unchanged: the same document is refused by the default parse.
    json::Value v;
    KV_CHECK(!json::parse(readText(day + L"\\index.json"), v));
    KV_CHECK(json::parse(readText(day + L"\\index.json").data(), readText(day + L"\\index.json").size(), v,
                         json::kLocalIndexLimits));
}

// storage_full and the disk floor are read together with a generation that grows at each measurement, so the
// recorder (which applies them later, under its own lock) can tell an older reading from a newer one.
KV_TEST(storage_flags_carry_a_generation_that_grows_with_each_measurement) {
    Fixture f(L"k5-gen");
    const RollStore::StorageFlags a = f.store->flags();
    f.store->checkDiskFloor();
    const RollStore::StorageFlags b = f.store->flags();
    KV_CHECK(b.gen > a.gen && !b.floorBlocked);
    f.freeSpace = 1000;
    f.store->checkDiskFloor();
    const RollStore::StorageFlags c = f.store->flags();
    KV_CHECK(c.gen > b.gen && c.floorBlocked);
    const std::string id = addClip(f, f.nowMs - 1000, ClipTrigger::Manual, 50000, true);
    f.store->setLimits(1000, 30);
    KV_CHECK(f.store->setStar(id, "o", PetRole::Owner, true) == RollError::None);
    const RollStore::StorageFlags d = f.store->flags();
    KV_CHECK(d.gen > c.gen && d.storageFull && d.floorBlocked);
    f.store->retentionPass();
    KV_CHECK(f.store->flags().gen > d.gen);
}

// The cached counts follow finalize, delete and retention without a walk, and a full recount agrees.
KV_TEST(cached_storage_counts_follow_the_store_without_a_walk) {
    Fixture f(L"k2-counts");
    f.store->recount();
    const RollStorageCounts empty = f.store->counts();
    KV_CHECK(empty.counted && empty.usedBytes == 0 && !empty.limitsSet);
    const std::string a = addClip(f, f.nowMs - 2000, ClipTrigger::Manual, 10000, true, {{"o", PetRole::Owner}});
    const std::string b = addClip(f, f.nowMs - 1000, ClipTrigger::Motion, 20000, false);
    const uint64_t aBytes = sizeOf(f.path(a, L".mp4")) + sizeOf(f.path(a, L".json")) + sizeOf(f.path(a, L".jpg"));
    const uint64_t bBytes = sizeOf(f.path(b, L".mp4")) + sizeOf(f.path(b, L".json"));
    KV_CHECK_MSG(f.store->counts().usedBytes == aBytes + bBytes, std::to_string(f.store->counts().usedBytes));
    KV_CHECK(f.store->storage().usedBytes == aBytes + bBytes); // 0x12's storage: the cache, no walk
    KV_CHECK(f.store->deleteClip(b) == RollError::None);
    KV_CHECK(f.store->counts().usedBytes == aBytes);
    KV_CHECK(f.store->setStar(a, "v", PetRole::Viewer, true) == RollError::None);
    KV_CHECK(f.store->counts().starredBytes == sizeOf(f.path(a, L".mp4")) + sizeOf(f.path(a, L".json")) +
                                                   sizeOf(f.path(a, L".jpg")));
    f.store->setLimits(5'000'000'000ULL, 9);
    const RollStorageCounts set = f.store->counts();
    KV_CHECK(set.limitsSet && set.capBytes == 5'000'000'000ULL && set.keepDays == 9);
    f.store->recount();
    const RollStorageCounts walked = f.store->counts();
    KV_CHECK(walked.counted && walked.usedBytes >= aBytes && walked.freeBytes == f.freeSpace);
}

// ---- test runs and the camera roll -------------------------------------------------------------------------

namespace {
// mklink /J: a directory junction (no administrator needed), made with FSCTL_SET_REPARSE_POINT as above.
bool makeJunction(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::wstring sub = L"\\??\\" + target;
    const USHORT subBytes = static_cast<USHORT>(sub.size() * sizeof(wchar_t));
    std::vector<BYTE> buf(16 + subBytes + 2 * sizeof(wchar_t), 0);
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    const USHORT len = static_cast<USHORT>(buf.size() - 8), printOff = static_cast<USHORT>(subBytes + 2);
    memcpy(buf.data(), &tag, 4);
    memcpy(buf.data() + 4, &len, 2);
    memcpy(buf.data() + 10, &subBytes, 2);
    memcpy(buf.data() + 12, &printOff, 2);
    memcpy(buf.data() + 16, sub.c_str(), subBytes);
    DWORD ret = 0;
    const BOOL ok =
        DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &ret, nullptr);
    CloseHandle(h);
    return ok != 0;
}
} // namespace

// A test run never records inside a camera roll that isn't a test roll, whatever name the
// folder is given: a plain new subfolder of it or of one of its days, a junction to the roll or to a day, its 8.3
// name, or a subst drive. The fixture is a temp folder made to look like a real roll (a day folder with a clip's
// sidecar, .nosync, .metadata_never_index and the deletion log; no test marker). And only "not found" makes a
// folder new: any other failure to read its attributes refuses. The old check took every such folder as new.
KV_TEST(a_test_roll_folder_inside_a_roll_that_isnt_a_test_roll_is_refused_by_any_name) {
    const std::wstring base = freshDir(L"p4-inside");
    KV_REQUIRE(CreateDirectoryW(base.c_str(), nullptr));
    const std::wstring fake = base + L"\\fake-real-roll", day = fake + L"\\2026-11-18";
    KV_REQUIRE(CreateDirectoryW(fake.c_str(), nullptr));
    KV_REQUIRE(CreateDirectoryW(day.c_str(), nullptr));
    writeText(fake + L"\\.nosync", "");
    writeText(fake + L"\\.metadata_never_index", "");
    writeText(fake + L"\\deletion-log.txt", "");
    writeText(day + L"\\14-03-22_motion.json", "{\"version\":1}");
    std::string err;
    // The rules that held already: a new folder beside it is fine, the roll itself is refused.
    KV_CHECK_MSG(checkTestRollFolder(base + L"\\new", &err), err);
    KV_CHECK(!checkTestRollFolder(fake, &err));
    auto refused = [&](const std::wstring& dir) {
        err.clear();
        const bool ok = checkTestRollFolder(dir, &err);
        std::printf("  %ls: %s\n", dir.c_str(), ok ? "accepted" : err.c_str());
        return !ok && err.find("test run") != std::string::npos;
    };
    // Inside it, by its own path.
    KV_CHECK(refused(fake + L"\\newsub"));
    KV_CHECK(refused(day + L"\\newsub"));
    KV_CHECK(refused(fake + L"\\a\\b"));
    // Through a junction to the roll, and to one of its days.
    const std::wstring link = base + L"\\link", day_link = base + L"\\day-link";
    KV_REQUIRE(makeJunction(link, fake));
    KV_REQUIRE(makeJunction(day_link, day));
    KV_REQUIRE(exists(day_link + L"\\14-03-22_motion.json")); // the junctions work
    KV_CHECK(refused(link + L"\\newsub"));
    KV_CHECK(refused(day_link + L"\\newsub"));
    // Its 8.3 name, where the volume makes them.
    {
        wchar_t shortName[MAX_PATH] = {0};
        const DWORD n = GetShortPathNameW(fake.c_str(), shortName, MAX_PATH);
        if (n > 0 && n < MAX_PATH && _wcsicmp(shortName, fake.c_str()) != 0) {
            KV_CHECK(refused(std::wstring(shortName) + L"\\newsub"));
        } else {
            std::printf("  this volume makes no 8.3 names: that case is skipped\n");
        }
    }
    // A subst drive (DefineDosDevice, as subst does) on a free letter, removed straight after. Letters are taken
    // from H up, as the missing-drive test in test_roll_e2e takes them from Z down.
    {
        std::wstring letter;
        const DWORD drives = GetLogicalDrives();
        for (wchar_t c = L'H'; c <= L'Y' && letter.empty(); ++c) {
            if (!(drives & (1u << (c - L'A')))) letter = std::wstring(1, c) + L":";
        }
        if (!letter.empty() && DefineDosDeviceW(DDD_NO_BROADCAST_SYSTEM, letter.c_str(), fake.c_str())) {
            KV_CHECK(exists(letter + L"\\2026-11-18\\14-03-22_motion.json")); // the drive works
            KV_CHECK(refused(letter + L"\\newsub"));
            DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM, letter.c_str(),
                             fake.c_str());
        } else {
            std::printf("  no subst drive could be made: that case is skipped\n");
        }
    }
    // Attributes that can't be read for a reason other than "not found" (here an invalid name): refused.
    KV_CHECK(!checkTestRollFolder(base + L"\\bad|name", &err));
    // A test roll, and a folder inside one, stay acceptable.
    const std::wstring test_roll = base + L"\\test-roll";
    KV_REQUIRE(CreateDirectoryW(test_roll.c_str(), nullptr));
    writeText(test_roll + L"\\.nosync", "");
    writeText(test_roll + L"\\" + kTestRollMarker, "");
    KV_CHECK_MSG(checkTestRollFolder(test_roll, &err), err);
    KV_CHECK_MSG(checkTestRollFolder(test_roll + L"\\newsub", &err), err);
    // The check reads only: nothing was made in the fixture.
    KV_CHECK(!exists(fake + L"\\newsub") && !exists(day + L"\\newsub") && !exists(fake + L"\\a"));
    removeTree(base);
}

KV_TEST_MAIN()
