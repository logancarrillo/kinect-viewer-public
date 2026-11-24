// Recordings inside the app (as on the Mac): the list, session_meta.json,
// the 7-day deletion and its log, deletion by the user, the purge of old colour sessions, the reminder
// policy, the reader (PNG16 and FFV1 chunks, bit exact at any index, any order) and playback through
// the core's replay pipeline. Temp folders only; the frames are made up (each one's index is in its
// pixels), never sensor data.
#include "kv_test.hpp"

#include "kinect_core_c.h"
#include "recorder.hpp"
#include "recordings.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace kvrec;

namespace {

std::wstring tempBase(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"kv-recordings-" + tag + L"-" + std::to_wstring(GetCurrentProcessId());
}

std::string readAll(const std::wstring& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::wstring& p, const std::string& body) {
    std::ofstream(p, std::ios::binary | std::ios::trunc) << body;
}

// Frame i: every pixel is (i * 31 + x + y) & 0xFFFF; depth-like values, all distinct per frame.
std::vector<uint16_t> madeUpFrame(int i) {
    std::vector<uint16_t> f(512 * 424);
    for (int y = 0; y < 424; ++y)
        for (int x = 0; x < 512; ++x) f[static_cast<size_t>(y) * 512 + x] = static_cast<uint16_t>((i * 31 + x + y) & 0xFFFF);
    return f;
}

kv_ir_intrinsics_t testIntrinsics() {
    kv_ir_intrinsics_t i{};
    i.valid = true;
    i.fx = 365.5f;
    i.fy = 365.25f;
    i.cx = 257.0f;
    i.cy = 206.5f;
    i.k1 = 0.09f;
    return i;
}

// Records `count` made-up frames as a session in base; returns its folder.
std::wstring recordSession(const std::wstring& base, kv_view_mode_t mode, Lossless16 fmt, int count) {
    Recorder rec;
    RecorderOptions o;
    o.base_dir = base;
    o.lossless = fmt;
    o.max_queue = 4096;
    o.ir_intrinsics = testIntrinsics();
    std::string err;
    if (!rec.start(mode, 512, 424, o, &err)) return {};
    uint64_t arrival = 1000000000ULL;
    for (int i = 0; i < count; ++i) {
        const auto f = madeUpFrame(i);
        rec.pushRaw16(f.data(), 512, 424, 1000 + static_cast<uint64_t>(i), static_cast<uint32_t>(i * 267), arrival);
        arrival += 33333333ULL;
    }
    rec.stop();
    return rec.status().session_dir;
}

std::wstring nameOf(const std::wstring& folder) { return folder.substr(folder.find_last_of(L'\\') + 1); }

std::string ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
    return s;
}

} // namespace

KV_TEST(sessions_carry_session_meta_and_are_listed_newest_first) {
    const std::wstring base = tempBase(L"list");
    removeTree(base);
    const std::wstring ir = recordSession(base, KV_MODE_IR, Lossless16::Png16Sequence, 12);
    Sleep(30); // a later folder stamp
    const std::wstring depth = recordSession(base, KV_MODE_DEPTH, Lossless16::Ffv1Mkv, 40);
    KV_REQUIRE(!ir.empty() && !depth.empty());
    const std::string meta = readAll(depth + L"\\session_meta.json");
    // The Mac's RecordingSessionMetadata keys, in its order, and the intrinsics.
    KV_CHECK_MSG(std::regex_search(meta, std::regex("^\\{\"createdAtISO8601\":\"\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ\","
                                                    "\"modeSlug\":\"depth16mm\",\"container\":\"ffv1\",\"width\":512,\"height\":424,"
                                                    "\"frameCount\":40,\"durationSec\":[0-9.e-]+,\"irIntrinsics\":\\{\"fx\":365.5,"
                                                    "\"fy\":365.25,\"cx\":257,\"cy\":206.5,\"k1\":0.09,")),
                 meta);
    const auto list = listSessions(base);
    KV_REQUIRE(list.size() == 2);
    KV_CHECK(list[0].folder == depth && list[1].folder == ir); // newest first
    const SessionItem& d = list[0];
    KV_CHECK(d.is_depth && d.container == "ffv1" && d.frame_count == 40 && d.width == 512 && d.height == 424);
    KV_REQUIRE(d.ir_intrinsics.has_value());
    KV_CHECK(d.ir_intrinsics->valid && d.ir_intrinsics->fx == 365.5f && d.ir_intrinsics->cy == 206.5f &&
             d.ir_intrinsics->k1 == 0.09f);
    KV_CHECK(!list[1].is_depth && list[1].container == "png16" && list[1].frame_count == 12);
    KV_CHECK(d.size_bytes > 0 && d.modeTitle() == L"Depth" && list[1].modeTitle() == L"IR");
    // The folder's stamp is the creation time (local time in the name, UTC seconds here).
    KV_CHECK(std::llabs(d.created - unixNow()) < 120);
    // The folder of a recording in progress isn't listed.
    KV_CHECK(listSessions(base, depth).size() == 1);
    removeTree(base);
}

KV_TEST(texts_countdown_length_size_and_the_folder_stamp) {
    SessionItem s;
    s.created = 1000000;
    KV_CHECK(s.countdownText(s.created) == L"7d 0h left");
    KV_CHECK(s.countdownText(s.created + kRetentionSeconds - 3600 - 5 * 60) == L"1h 5m left");
    KV_CHECK(s.countdownText(s.created + kRetentionSeconds - 90) == L"1m left");
    KV_CHECK(s.countdownText(s.created + kRetentionSeconds - 10) == L"1m left");
    KV_CHECK(s.countdownText(s.created + kRetentionSeconds) == L"Expired");
    KV_CHECK(!s.isExpired(s.created + kRetentionSeconds - 1) && s.isExpired(s.created + kRetentionSeconds));
    s.duration_sec = 12.34;
    s.frame_count = 370;
    KV_CHECK(s.lengthText() == L"12.3 s (370 f)");
    s.duration_sec = 125.0;
    s.frame_count = 3750;
    KV_CHECK(s.lengthText() == L"2m 05s (3750 f)");
    s.size_bytes = 12345678;
    KV_CHECK(s.sizeText() == L"12.3 MB");
    s.size_bytes = 456000;
    KV_CHECK(s.sizeText() == L"456 KB");
    KV_CHECK(parseFolderTimestamp(L"20261112-204100-123_depth16mm.nosync").has_value());
    KV_CHECK(!parseFolderTimestamp(L"notes.nosync").has_value());
    KV_CHECK(!parseFolderTimestamp(L"2026111x-204100-123_ir16.nosync").has_value());
    // Two stamps a minute apart are a minute apart.
    KV_CHECK(*parseFolderTimestamp(L"20261112-204200-000_ir16.nosync") - *parseFolderTimestamp(L"20261112-204100-000_ir16.nosync") == 60);
}

KV_TEST(old_colour_sessions_are_purged_and_logged_and_never_listed_as_ir) {
    const std::wstring base = tempBase(L"purge");
    removeTree(base);
    const std::wstring keep = recordSession(base, KV_MODE_IR, Lossless16::Png16Sequence, 3);
    KV_REQUIRE(!keep.empty());
    // Colour sessions (an HEVC .mp4 and its CSV), a side-by-side one, and an unknown folder.
    const std::vector<std::wstring> bad = {L"20261101-100000-000_color1080p.nosync", L"20261101-100001-000_sidebyside.nosync",
                                           L"20261101-100002-000_mystery.nosync"};
    for (const auto& n : bad) {
        CreateDirectoryW((base + L"\\" + n).c_str(), nullptr);
        writeFile(base + L"\\" + n + L"\\color1080p_hevc.mp4", std::string(1000, 'x'));
        writeFile(base + L"\\" + n + L"\\color1080p_timestamps.csv", "frame_index,engine_seq,device_ticks,arrival_mach_ns\n0,1,2,3\n");
    }
    // An IR session without session_meta.json still counts (explicit ir16 markers).
    const std::wstring legacy = base + L"\\20261101-100003-000_ir16.nosync";
    CreateDirectoryW(legacy.c_str(), nullptr);
    writeFile(legacy + L"\\ir16_timestamps.csv", "frame_index,engine_seq,device_ticks,arrival_mach_ns\n0,1,2,1000000000\n1,2,3,1033333333\n");
    const auto listed = listSessions(base);
    KV_CHECK(listed.size() == 2); // the new IR and the legacy IR, no colour
    for (const auto& s : listed) KV_CHECK(s.folder_name.find(L"color") == std::wstring::npos && s.folder_name.find(L"side") == std::wstring::npos);

    const int64_t now = unixNow();
    const auto deleted = purgeDisallowedSessions(base, now);
    KV_CHECK(deleted.size() == 3);
    for (const auto& n : bad) KV_CHECK(GetFileAttributesW((base + L"\\" + n).c_str()) == INVALID_FILE_ATTRIBUTES);
    KV_CHECK(GetFileAttributesW(keep.c_str()) != INVALID_FILE_ATTRIBUTES);
    KV_CHECK(GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES);
    const std::string log = readAll(base + L"\\deletion-log.txt");
    for (const auto& n : bad) {
        KV_CHECK_MSG(log.find("\t" + ascii(n) + "\t") != std::string::npos, log);
    }
    // Only name, size, time and trigger: "<ISO>\t<name>\t<N> bytes\tautomatic".
    std::istringstream lines(log);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        ++count;
        KV_CHECK_MSG(std::regex_match(line, std::regex("\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ\\t[^\\t]+\\.nosync\\t\\d+ bytes\\tautomatic")), line);
    }
    KV_CHECK(count == 3);
    removeTree(base);
}

KV_TEST(sessions_are_deleted_after_seven_days_and_by_the_user_with_the_log) {
    const std::wstring base = tempBase(L"expire");
    removeTree(base);
    const std::wstring a = recordSession(base, KV_MODE_DEPTH, Lossless16::Png16Sequence, 2);
    Sleep(30);
    const std::wstring b = recordSession(base, KV_MODE_IR, Lossless16::Png16Sequence, 2);
    KV_REQUIRE(!a.empty() && !b.empty());
    const auto list = listSessions(base);
    KV_REQUIRE(list.size() == 2);
    const int64_t created = list[1].created; // a, the older
    // A second before seven days: nothing goes.
    KV_CHECK(purgeExpiredSessions(base, created + kRetentionSeconds - 1).empty());
    // Seven days on: a goes (b was made up to a few ms later, so pick its own cut-off for it).
    const auto gone = purgeExpiredSessions(base, created + kRetentionSeconds, b);
    KV_CHECK(gone.size() == 1 && gone[0] == nameOf(a));
    KV_CHECK(GetFileAttributesW(a.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(b.c_str()) != INVALID_FILE_ATTRIBUTES);
    // The active (excluded) folder survives even when old.
    KV_CHECK(purgeExpiredSessions(base, created + 30 * 86400, b).empty());
    // The user deletes b.
    const auto rest = listSessions(base);
    KV_REQUIRE(rest.size() == 1);
    KV_CHECK(deleteSessionByUser(rest[0], base, created + 100));
    KV_CHECK(listSessions(base).empty());
    const std::string log = readAll(base + L"\\deletion-log.txt");
    KV_CHECK(log.find(ascii(nameOf(a)) + "\t") != std::string::npos &&
             log.find("\tautomatic\n") != std::string::npos);
    KV_CHECK(log.find(ascii(nameOf(b)) + "\t") != std::string::npos &&
             log.find("\tuser\n") != std::string::npos);
    KV_CHECK(deletionLogLine(L"x.nosync", 42, 0, DeletionTrigger::User) == "1970-01-01T00:00:00Z\tx.nosync\t42 bytes\tuser\n");
    removeTree(base);
}

namespace {
// mklink /J: a directory junction (no administrator needed).
bool makeJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    const DWORD a = GetFileAttributesW(link.c_str());
    return code == 0 && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
} // namespace

KV_TEST(junctions_are_never_followed_out_of_the_recordings_folder) {
    const std::wstring base = tempBase(L"junction");
    const std::wstring outside = tempBase(L"junction-target");
    removeTree(base);
    removeTree(outside);
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(outside.c_str(), nullptr);
    writeFile(outside + L"\\precious.txt", "keep me");
    // Junctions named like sessions: one "disallowed" (colour), one looking like IR.
    const std::wstring j_colour = base + L"\\20261101-100000-000_color1080p.nosync";
    const std::wstring j_ir = base + L"\\20261101-100001-000_ir16.nosync";
    KV_REQUIRE(makeJunction(j_colour, outside));
    KV_REQUIRE(makeJunction(j_ir, outside));
    // A real session holding a junction out, and one that loops back to the session itself.
    const std::wstring real = recordSession(base, KV_MODE_DEPTH, Lossless16::Png16Sequence, 2);
    KV_REQUIRE(!real.empty());
    KV_REQUIRE(makeJunction(real + L"\\out", outside));
    KV_REQUIRE(makeJunction(real + L"\\loop", real));

    const auto list = listSessions(base); // finishes (the loop isn't followed) and lists only the real one
    KV_REQUIRE(list.size() == 1);
    KV_CHECK(list[0].folder == real);
    KV_CHECK(list[0].size_bytes < 2 * 512 * 424 * 2 + 100000); // its own files only, not the loop's

    // A forged delete: the junction as the "session", and a folder outside base. Both refused.
    SessionItem forged = list[0];
    forged.folder = j_ir;
    forged.folder_name = L"20261101-100001-000_ir16.nosync";
    KV_CHECK(!deleteSessionByUser(forged, base, unixNow()));
    forged.folder = outside;
    forged.folder_name = L"..";
    KV_CHECK(!deleteSessionByUser(forged, base, unixNow()));
    // base\.. and base\. pass the slash and path checks by themselves: refused by name.
    for (const wchar_t* dots : {L"..", L"."}) {
        forged.folder_name = dots;
        forged.folder = base + L"\\" + dots;
        KV_CHECK(!deleteSessionByUser(forged, base, unixNow()));
    }
    KV_CHECK(GetFileAttributesW(base.c_str()) != INVALID_FILE_ATTRIBUTES);

    // The purge, days later: the junctions in base are skipped; the real session goes, taking its
    // inner junctions as links only.
    const auto gone = purgeExpiredSessions(base, unixNow() + 30 * 86400);
    KV_CHECK(gone.size() == 1);
    KV_CHECK(GetFileAttributesW(real.c_str()) == INVALID_FILE_ATTRIBUTES);
    KV_CHECK(readAll(outside + L"\\precious.txt") == "keep me"); // the target survived all of it
    KV_CHECK(GetFileAttributesW(j_colour.c_str()) != INVALID_FILE_ATTRIBUTES); // not ours to delete
    // removeTree on a junction removes the link, not the target.
    KV_CHECK(removeTree(j_ir));
    KV_CHECK(GetFileAttributesW(j_ir.c_str()) == INVALID_FILE_ATTRIBUTES && readAll(outside + L"\\precious.txt") == "keep me");
    RemoveDirectoryW(j_colour.c_str());
    removeTree(base);
    removeTree(outside);
}

KV_TEST(bad_metadata_future_stamps_and_a_partial_delete) {
    const std::wstring base = tempBase(L"bounds");
    removeTree(base);
    const std::wstring a = recordSession(base, KV_MODE_DEPTH, Lossless16::Png16Sequence, 2);
    KV_REQUIRE(!a.empty());
    // Only 512x424: a session_meta.json saying 1920x1080 isn't a session (and the purge removes it).
    std::string meta = readAll(a + L"\\session_meta.json");
    meta.replace(meta.find("\"width\":512"), 11, "\"width\":1920");
    writeFile(a + L"\\session_meta.json", meta);
    KV_CHECK(listSessions(base).empty());
    // A huge frame count is capped.
    meta.replace(meta.find("\"width\":1920"), 12, "\"width\":512");
    meta.replace(meta.find("\"frameCount\":2"), 14, "\"frameCount\":2000000000");
    writeFile(a + L"\\session_meta.json", meta);
    auto list = listSessions(base);
    KV_REQUIRE(list.size() == 1);
    KV_CHECK(list[0].frame_count == kMaxSessionFrames);
    // A stamp in the future (a wrong clock): the age counts from now at the latest.
    const std::wstring future = base + L"\\29991231-235959-000_ir16.nosync";
    CreateDirectoryW(future.c_str(), nullptr);
    writeFile(future + L"\\ir16_timestamps.csv", "frame_index,engine_seq,device_ticks,arrival_mach_ns\n0,1,2,3\n");
    list = listSessions(base);
    for (const auto& s : list) {
        KV_CHECK(s.created <= unixNow());
        if (s.folder == future) KV_CHECK(s.isExpired(unixNow() + kRetentionSeconds + 1));
    }
    // A delete that fails partway (a file held open) is logged as incomplete, with what went.
    const std::wstring held = a + L"\\held.bin";
    writeFile(held, std::string(4096, 'h'));
    HANDLE h = CreateFileW(held.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr); // no sharing: can't delete
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    list = listSessions(base);
    const auto it = std::find_if(list.begin(), list.end(), [&](const SessionItem& s) { return s.folder == a; });
    KV_REQUIRE(it != list.end());
    KV_CHECK(!deleteSessionByUser(*it, base, 5000));
    CloseHandle(h);
    const std::string log = readAll(base + L"\\deletion-log.txt");
    KV_CHECK_MSG(log.find("\tuser\tincomplete\n") != std::string::npos, log);
    removeTree(base);
}

KV_TEST(the_reminder_snoozes_for_one_hour) {
    ReminderPolicy r;
    KV_CHECK(r.needed(1000));
    r.snoozeOneHour(1000);
    KV_CHECK(!r.needed(1000) && !r.needed(1000 + 3599) && r.needed(1000 + 3600));
}

KV_TEST(the_reader_is_bit_exact_at_any_index_in_any_order) {
    const std::wstring base = tempBase(L"reader");
    removeTree(base);
    const int n = 130; // three FFV1 chunks of 60
    const std::wstring ffv1 = recordSession(base, KV_MODE_DEPTH, Lossless16::Ffv1Mkv, n);
    Sleep(30);
    const std::wstring png = recordSession(base, KV_MODE_IR, Lossless16::Png16Sequence, 8);
    KV_REQUIRE(!ffv1.empty() && !png.empty());
    const auto list = listSessions(base);
    KV_REQUIRE(list.size() == 2);
    const SessionItem& sp = list[0].container == "png16" ? list[0] : list[1];
    const SessionItem& sf = list[0].container == "ffv1" ? list[0] : list[1];
    KV_CHECK(sf.frame_count == n);
    {
        SessionReader r(sf, 60);
        KV_CHECK(r.totalFrames() == n);
        KV_CHECK(r.deviceTicks(10) == 2670u && std::fabs(r.frameIntervalSeconds(3) - 1.0 / 30.0) < 1e-3);
        std::vector<uint16_t> px;
        for (int i : {0, 59, 60, 61, 129, 5, 120, 64}) { // across chunk edges, backwards too
            KV_REQUIRE(r.readFrame(i, px));
            KV_CHECK_MSG(px == madeUpFrame(i), "ffv1 frame " + std::to_string(i));
        }
        KV_CHECK(!r.readFrame(n, px) && !r.readFrame(-1, px));
    }
    {
        SessionReader r(sp);
        std::vector<uint16_t> px;
        for (int i : {7, 0, 3}) {
            KV_REQUIRE(r.readFrame(i, px));
            KV_CHECK_MSG(px == madeUpFrame(i), "png frame " + std::to_string(i));
        }
    }
    // The decode worker: off this thread; the newest request wins.
    {
        ReplayDecoder dec(sf);
        KV_CHECK(dec.totalFrames() == n);
        dec.request(100);
        std::optional<ReplayDecoder::Frame> f;
        for (int t = 0; t < 400 && !f; ++t) {
            f = dec.take();
            if (!f) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        KV_REQUIRE(f.has_value());
        KV_CHECK(f->ok && f->index == 100 && f->pixels == madeUpFrame(100) && f->ticks == 26700u);
    }
    // Stopping a playback mid-decode kills ffmpeg rather than waiting for the chunk.
    {
        const auto t0 = std::chrono::steady_clock::now();
        {
            ReplayDecoder dec(sf);
            dec.request(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // the chunk decode has started
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  decoder stopped mid-chunk after %.3f s\n", s);
        KV_CHECK(s < 2.0);
    }
    removeTree(base);
}

KV_TEST(playback_goes_through_the_live_pipeline) {
    const std::wstring base = tempBase(L"replay");
    removeTree(base);
    const std::wstring dir = recordSession(base, KV_MODE_DEPTH, Lossless16::Png16Sequence, 5);
    KV_REQUIRE(!dir.empty());
    const auto list = listSessions(base);
    KV_REQUIRE(list.size() == 1 && list[0].ir_intrinsics.has_value());
    SessionReader r(list[0]);
    kv_engine_t* eng = kv_engine_create(nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = KV_MODE_DEPTH;
    KV_REQUIRE(kv_engine_start_replay(eng, &p, &*list[0].ir_intrinsics));
    std::vector<uint16_t> px;
    KV_REQUIRE(r.readFrame(2, px));
    KV_REQUIRE(kv_engine_push_replay_frame16(eng, true, px.data(), 512, 424, 2, r.deviceTicks(2)));
    // The mapped frame comes out as in live (512x424 depth colours), and the inspection buffers carry
    // the recording's depth and its intrinsics, so the ruler works on it.
    std::vector<uint8_t> bgra(1920 * 1080 * 4);
    size_t req = 0;
    kv_mapped_frame_t m{};
    bool got = false;
    for (int t = 0; t < 200 && !got; ++t) {
        got = kv_engine_copy_latest_frame(eng, 0, bgra.data(), bgra.size(), &req, &m);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    KV_REQUIRE(got);
    KV_CHECK(m.width == 512 && m.height == 424);
    std::vector<float> depth(512 * 424), ir(512 * 424);
    uint32_t w = 0, h = 0;
    kv_ir_intrinsics_t intr{};
    KV_REQUIRE(kv_engine_copy_inspection_buffers(eng, depth.data(), depth.size(), ir.data(), ir.size(), &w, &h, &intr));
    KV_CHECK(intr.valid && intr.fx == 365.5f);
    const size_t at = static_cast<size_t>(100) * 512 + 200;
    KV_CHECK_MSG(depth[at] == static_cast<float>(px[at]) || depth[at] == 0.0f, std::to_string(depth[at]));
    kv_engine_stop(eng);
    kv_engine_destroy(eng);
    removeTree(base);
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return kvtest::runAll(argc, argv);
}
