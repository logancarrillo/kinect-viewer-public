// The connection log (v3 §8.6): device, time and address, coalesced per minute, kept 30 days and
// 20 MB, owner-only, never sent anywhere; appended to, with a line budget for unknown sources that never holds
// back a device's lines; a file that can't be read is never written over. New temp folders.
#include "kv_test.hpp"
#include "outside_test_dir.hpp"

#include "connection_log.hpp"
#include "state_files.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace kvhost::outside;
using kvtest::OutsideTempState;

namespace {

constexpr uint64_t kNow = 1'763'500'000'000ull;

LogLine line(const char* event, uint64_t t, const char* addr = "203.0.113.1", const char* path = "outside") {
    LogLine l;
    l.t = t;
    l.event = event;
    l.path = path;
    l.addr = addr;
    return l;
}

std::string readAll(const std::wstring& path) {
    std::string s;
    kvhost::readWholeFile(path, s);
    return s;
}

} // namespace

KV_TEST(a_line_holds_device_time_and_address) {
    LogLine l = line("connected", kNow);
    l.deviceId = "0123456789abcdef";
    l.deviceName = "iPhone";
    KV_CHECK(l.toJson() ==
             "{\"t\":1763500000000,\"event\":\"connected\",\"path\":\"outside\",\"addr\":\"203.0.113.1\","
             "\"deviceId\":\"0123456789abcdef\",\"deviceName\":\"iPhone\"}");
    l.repeat = 3;
    l.detail = "42 s";
    KV_CHECK(l.toJson().find(",\"repeat\":3,\"detail\":\"42 s\"}") != std::string::npos);
}

KV_TEST(only_the_spec_events_and_paths_are_recorded) {
    OutsideTempState t(L"log-events");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    for (const char* e : {"connected", "disconnected", "refused_unknown_cert", "refused_rate_limit", "refused_blocked",
                          "refused_local_only", "refused_too_many", "refused_unavailable", "paired", "invite_redeemed",
                          "invite_failed", "invite_burned", "revoked", "role_changed", "invite_closed",
                          "outside_endpoint_told", "pairing_failed", "pairing_burned", "suppressed"})
        KV_CHECK(log.record(line(e, kNow, "203.0.113.1", "home")));
    KV_CHECK(!log.record(line("frame_seen", kNow)));
    KV_CHECK(!log.record(line("connected", kNow, "203.0.113.1", "cloud")));
    KV_CHECK(log.lines().size() == 19);
}

// A trace from a real run: two attempts from one source with no device, a second apart, that ended
// differently (Safari's, and the app's) were one line with only the first's detail, so the app's was hidden.
KV_TEST(different_details_from_one_source_are_lines_of_their_own) {
    OutsideTempState t(L"log-details");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    LogLine a = line("disconnected", kNow);
    a.detail = "no certificate";
    LogLine b = line("disconnected", kNow + 1000);
    b.detail = "closed during the handshake";
    KV_CHECK(log.record(a));
    KV_CHECK(log.record(b));
    const auto lines = log.lines();
    KV_CHECK_MSG(lines.size() == 2, std::to_string(lines.size()));
    KV_CHECK(lines.size() == 2 && lines[0].detail == "no certificate" && lines[1].detail == "closed during the handshake");
    // The same detail still folds into one line.
    LogLine c = b;
    c.t = kNow + 2000;
    KV_CHECK(log.record(c));
    KV_CHECK(log.lines().size() == 2 && log.lines()[1].repeat == 2);
}

KV_TEST(repeats_within_a_minute_are_one_line) {
    OutsideTempState t(L"log-coalesce");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    for (int i = 0; i < 100; ++i) KV_CHECK(log.record(line("refused_unknown_cert", kNow + i * 500), "aa11"));
    auto lines = log.lines();
    KV_CHECK(lines.size() == 1 && lines[0].repeat == 100);
    // Another certificate, another source or another event is a line of its own.
    KV_CHECK(log.record(line("refused_unknown_cert", kNow + 1000), "bb22"));
    KV_CHECK(log.record(line("refused_unknown_cert", kNow + 1000, "203.0.113.2"), "aa11"));
    KV_CHECK(log.record(line("refused_rate_limit", kNow + 1000), "aa11"));
    KV_CHECK(log.lines().size() == 4);
    // A minute after the line began, a new line starts.
    KV_CHECK(log.record(line("refused_unknown_cert", kNow + 60'000), "aa11"));
    lines = log.lines();
    KV_CHECK(lines.size() == 5 && lines.back().repeat == 0);
}

KV_TEST(the_log_is_written_owner_only_and_read_back) {
    OutsideTempState t(L"log-roundtrip");
    {
        ConnectionLog log(t.dir);
        KV_CHECK(log.load(kNow));
        LogLine a = line("paired", kNow, "192.168.1.30", "home");
        a.deviceId = "0123456789abcdef";
        a.deviceName = "iPad";
        KV_CHECK(log.record(a, a.deviceId));
        KV_CHECK(log.record(line("refused_blocked", kNow + 5)));
        KV_CHECK(log.record(line("refused_blocked", kNow + 6)));
        KV_CHECK(log.flush());
    }
    const std::string text = readAll(t.file(kConnectionLogFile));
    KV_CHECK(std::count(text.begin(), text.end(), '\n') == 2);
    KV_CHECK(text.find("\"repeat\":2") != std::string::npos);
    ConnectionLog again(t.dir);
    KV_CHECK(again.load(kNow + 10));
    const auto lines = again.lines();
    KV_CHECK(lines.size() == 2);
    KV_CHECK(lines[0].deviceName == "iPad" && lines[0].path == "home" && lines[0].addr == "192.168.1.30");
    KV_CHECK(lines[1].event == "refused_blocked" && lines[1].repeat == 2);
}

KV_TEST(lines_older_than_30_days_are_dropped) {
    OutsideTempState t(L"log-30days");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    const uint64_t day = 24ull * 60 * 60 * 1000;
    KV_CHECK(log.record(line("connected", kNow - 31 * day)));
    KV_CHECK(log.record(line("connected", kNow - 29 * day, "203.0.113.2")));
    KV_CHECK(log.flush());
    ConnectionLog again(t.dir);
    KV_CHECK(again.load(kNow)); // pruned at start-up
    KV_CHECK(again.lines().size() == 1 && again.lines()[0].addr == "203.0.113.2");
    again.prune(kNow + 2 * day); // and daily
    KV_CHECK(again.lines().empty());
}

// Past its size the oldest lines go until it is at three quarters of it, so a full log trims (and is written
// whole) once per quarter of its size, not once a line. Lines of one size: ten fit, and a trim keeps seven.
KV_TEST(the_log_keeps_its_newest_lines_within_its_size) {
    OutsideTempState t(L"log-cap");
    const size_t oneLine = line("connected", kNow, "203.0.113.100").toJson().size() + 1;
    ConnectionLog log(t.dir, 10'000, oneLine * 10);
    KV_CHECK(log.load(kNow));
    for (int i = 0; i < 25; ++i) {
        const std::string a = "203.0.113." + std::to_string(100 + i);
        KV_CHECK(log.record(line("connected", kNow + i, a.c_str())));
        KV_CHECK(log.lines().size() <= 10);
    }
    // The 11th, 15th, 19th and 23rd lines each trimmed it to seven: .116 to .122 then, and two since.
    const auto lines = log.lines();
    KV_CHECK_MSG(lines.size() == 9, std::to_string(lines.size()));
    KV_CHECK(lines.size() == 9 && lines.front().addr == "203.0.113.116" && lines.back().addr == "203.0.113.124");
    for (size_t i = 1; i < lines.size(); ++i) KV_CHECK(lines[i].t == lines[i - 1].t + 1); // the newest, in order
    KV_CHECK(log.flush());
    KV_CHECK(readAll(t.file(kConnectionLogFile)).size() <= oneLine * 10);
}

// The budget and the cap together: however long a scan from new sources goes on, the devices' history stays, and
// the lines dropped are the scan's own, oldest first.
KV_TEST(the_history_outlasts_a_long_scan_from_new_sources) {
    OutsideTempState t(L"log-history");
    ConnectionLog log(t.dir, 10'000, 16 * 1024);
    KV_CHECK(log.load(kNow));
    LogLine paired = line("paired", kNow - 1000, "192.168.1.30", "home");
    paired.deviceId = "0123456789abcdef";
    paired.deviceName = "iPad";
    KV_CHECK(log.record(paired, paired.deviceId));
    LogLine revoked = line("revoked", kNow - 500, "", "home"); // the PC's own: no address, no device left
    revoked.detail = "on the PC";
    KV_CHECK(log.record(revoked));
    for (int m = 0; m < 30; ++m) {
        for (int i = 0; i < 60; ++i) {
            char a[64];
            std::snprintf(a, sizeof a, "2001:db8:%x:%x::1", m + 1, i + 1); // a new /64 each time
            KV_CHECK(log.record(line("refused_unknown_cert", kNow + m * 60'000ull + i * 100, a)));
        }
    }
    auto lines = log.lines();
    KV_REQUIRE(lines.size() > 2);
    KV_CHECK(lines[0].event == "paired" && lines[0].deviceName == "iPad");
    KV_CHECK(lines[1].event == "revoked" && lines[1].detail == "on the PC");
    KV_CHECK(lines.back().addr == "2001:db8:1e:3c::1"); // the newest of the scan
    KV_CHECK(lines[2].addr != "2001:db8:1:1::1");      // its oldest went
    size_t bytes = 0;
    for (const auto& l : lines) bytes += l.toJson().size() + 1;
    KV_CHECK_MSG(bytes <= 16 * 1024, std::to_string(bytes));
    KV_CHECK(log.flush());
    ConnectionLog again(t.dir, 10'000, 16 * 1024);
    KV_CHECK(again.load(kNow + 31 * 60'000ull));
    lines = again.lines();
    KV_CHECK(lines.size() > 2 && lines[0].event == "paired" && lines[1].event == "revoked");
}

// Sources no device is known for add at most 60 new lines a minute; the rest are counted on one "suppressed" line.
// A device's line is never held back, and the next minute has a budget of its own.
KV_TEST(a_flood_from_many_sources_is_held_to_a_line_budget) {
    OutsideTempState t(L"log-budget");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    const uint64_t minute = kNow - kNow % 60'000;
    for (int i = 0; i < 500; ++i) {
        char a[64];
        std::snprintf(a, sizeof a, "2001:db8:%x::1", i + 1);
        KV_CHECK(log.record(line("refused_unknown_cert", minute + 1000 + i * 10, a)));
    }
    auto lines = log.lines();
    KV_CHECK_MSG(lines.size() == 61, std::to_string(lines.size()));
    KV_REQUIRE(!lines.empty());
    KV_CHECK(lines.back().event == "suppressed" && lines.back().repeat == 440 && lines.back().addr.empty());
    KV_CHECK_MSG(lines.back().detail.find("past 60 a minute") != std::string::npos, lines.back().detail);
    // A repeat of a line already in the log isn't a new line, so the budget doesn't hold it back.
    KV_CHECK(log.record(line("refused_unknown_cert", minute + 6000, "2001:db8:1::1")));
    KV_CHECK(log.lines().size() == 61 && log.lines()[0].repeat == 2);
    LogLine dev = line("connected", minute + 6000, "2001:db8:ffff::1");
    dev.deviceId = "0123456789abcdef";
    KV_CHECK(log.record(dev, dev.deviceId));
    lines = log.lines();
    KV_CHECK(lines.size() == 62 && lines.back().deviceId == "0123456789abcdef");
    KV_CHECK(log.record(line("refused_unknown_cert", minute + 60'000, "2001:db8:1234::1")));
    lines = log.lines();
    KV_CHECK(lines.size() == 63 && lines.back().event == "refused_unknown_cert");
}

namespace {

// The file's identity (its NTFS file index): an append keeps it, a write through a new file and a rename doesn't.
uint64_t fileIndex(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    return ok ? (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow : 0;
}

} // namespace

KV_TEST(a_flush_appends_and_never_rewrites_what_is_written) {
    OutsideTempState t(L"log-append");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    KV_CHECK(log.record(line("connected", kNow)));
    KV_CHECK(log.flush());
    const std::string first = readAll(t.file(kConnectionLogFile));
    const uint64_t id = fileIndex(t.file(kConnectionLogFile));
    KV_REQUIRE(!first.empty() && id != 0);
    const LogLine next = line("connected", kNow + 1, "203.0.113.2");
    KV_CHECK(log.record(next));
    KV_CHECK(log.flush());
    const std::string second = readAll(t.file(kConnectionLogFile));
    KV_CHECK(second.size() > first.size() && second.compare(0, first.size(), first) == 0);
    KV_CHECK(second.substr(std::min(first.size(), second.size())) == next.toJson() + "\n");
    KV_CHECK_MSG(fileIndex(t.file(kConnectionLogFile)) == id, "the flush wrote a new file in place of the old one");
}

KV_TEST(a_count_that_grows_after_its_line_is_written_is_written_again_once_its_minute_is_over) {
    OutsideTempState t(L"log-grow");
    ConnectionLog log(t.dir, 0);
    KV_CHECK(log.load(kNow));
    KV_CHECK(log.record(line("refused_blocked", kNow)));
    KV_CHECK(log.flushIfDue(1, kNow));
    const std::string once = readAll(t.file(kConnectionLogFile));
    KV_CHECK(std::count(once.begin(), once.end(), '\n') == 1 && once.find("repeat") == std::string::npos);
    for (int i = 1; i <= 3; ++i) KV_CHECK(log.record(line("refused_blocked", kNow + i * 1000)));
    KV_CHECK(log.flushIfDue(2, kNow + 30'000));
    KV_CHECK_MSG(readAll(t.file(kConnectionLogFile)) == once, "a count was written while its minute could still grow it");
    KV_CHECK(log.flushIfDue(3, kNow + 60'000));
    const std::string text = readAll(t.file(kConnectionLogFile));
    KV_CHECK(std::count(text.begin(), text.end(), '\n') == 2 && text.find("\"repeat\":4") != std::string::npos);
    KV_CHECK(text.compare(0, once.size(), once) == 0);
    // Read back, the two are one line with the larger count.
    ConnectionLog again(t.dir);
    KV_CHECK(again.load(kNow + 61'000));
    const auto lines = again.lines();
    KV_CHECK(lines.size() == 1 && lines[0].repeat == 4);
    // At exit, flush() writes a count that is still growing.
    KV_CHECK(again.record(line("refused_blocked", kNow + 61'000)));
    KV_CHECK(again.record(line("refused_blocked", kNow + 62'000)));
    KV_CHECK(again.flush());
    ConnectionLog third(t.dir);
    KV_CHECK(third.load(kNow + 63'000));
    KV_CHECK(third.lines().size() == 2 && third.lines()[1].repeat == 2);
}

// A paired device's reconnect loop is a line a minute however long each session lasted: the session length on a
// disconnected line isn't part of the coalescing key (the line keeps the first one's). A reason with a length is
// keyed by its reason.
KV_TEST(a_devices_reconnect_loop_is_a_line_a_minute_whatever_its_sessions_lasted) {
    OutsideTempState t(L"log-loop");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    const uint64_t minute = kNow - kNow % 60'000;
    auto gone = [&](uint64_t at, const std::string& detail) {
        LogLine l = line("disconnected", at, "192.168.1.30", "home");
        l.deviceId = "0123456789abcdef";
        l.deviceName = "iPad";
        l.detail = detail;
        return log.record(l, l.deviceId);
    };
    for (int i = 0; i < 60; ++i) KV_CHECK(gone(minute + i * 900, std::to_string(100 + i * 7) + " s"));
    auto lines = log.lines();
    KV_CHECK_MSG(lines.size() == 1, std::to_string(lines.size()));
    KV_CHECK(!lines.empty() && lines[0].repeat == 60 && lines[0].detail == "100 s");
    for (int i = 0; i < 5; ++i) KV_CHECK(gone(minute + 54'000 + i * 100, "replaced, " + std::to_string(i + 1) + " s"));
    lines = log.lines();
    KV_CHECK(lines.size() == 2 && lines[1].repeat == 5 && lines[1].detail == "replaced, 1 s");
    // Two more minutes of the loop: a line each.
    for (int m = 1; m <= 2; ++m)
        for (int i = 0; i < 20; ++i) KV_CHECK(gone(minute + m * 60'000ull + i * 1000, std::to_string(i + 1) + " s"));
    lines = log.lines();
    KV_CHECK_MSG(lines.size() == 4, std::to_string(lines.size()));
    // Other events keep their details apart, as before.
    KV_CHECK(log.record(line("refused_blocked", minute + 1000, "203.0.113.9")));
    LogLine other = line("refused_blocked", minute + 2000, "203.0.113.9");
    other.detail = "5 s";
    KV_CHECK(log.record(other));
    KV_CHECK(log.lines().size() == 6);
}

// Appends end every line with its newline, so a last line without one that doesn't parse is a write cut short
// (the host stopped mid-append). It is dropped and the file written whole without it, with no .bad copy; a bad
// line anywhere else is still kept as .bad first (a_log_with_bad_lines_is_kept_before_it_is_rewritten).
KV_TEST(a_torn_last_line_is_dropped_without_a_bad_copy) {
    OutsideTempState t(L"log-torn");
    const std::string good = line("connected", kNow).toJson();
    const std::string next = line("connected", kNow + 1, "203.0.113.2").toJson();
    KV_REQUIRE(kvtest::writeRaw(t.file(kConnectionLogFile), good + "\n" + next.substr(0, next.size() / 2)));
    ConnectionLog log(t.dir);
    std::string err;
    KV_CHECK(log.load(kNow + 2, &err));
    KV_CHECK_MSG(err.find("cut short") != std::string::npos, err);
    KV_CHECK(!kvhost::fileExists(t.file(L"connection-log.jsonl.bad")));
    KV_CHECK(log.lines().size() == 1);
    KV_CHECK(log.flush());
    KV_CHECK(readAll(t.file(kConnectionLogFile)) == good + "\n");
    // Appends carry on after it as usual.
    KV_CHECK(log.record(line("connected", kNow + 3, "203.0.113.3")));
    KV_CHECK(log.flush());
    KV_CHECK(readAll(t.file(kConnectionLogFile)) == good + "\n" + line("connected", kNow + 3, "203.0.113.3").toJson() + "\n");
}

// An append never makes a file (only the owner-only writer does): one taken away while the host runs is written
// whole again, every line in it.
KV_TEST(a_log_file_taken_away_is_written_whole_again) {
    OutsideTempState t(L"log-gone");
    ConnectionLog log(t.dir);
    KV_CHECK(log.load(kNow));
    KV_CHECK(log.record(line("connected", kNow)));
    KV_CHECK(log.flush());
    KV_REQUIRE(DeleteFileW(t.file(kConnectionLogFile).c_str()) != 0);
    KV_CHECK(log.record(line("connected", kNow + 1, "203.0.113.2")));
    std::string err;
    KV_CHECK(!log.flush(&err));
    KV_CHECK(!err.empty() && !kvhost::fileExists(t.file(kConnectionLogFile)));
    KV_CHECK(log.flush());
    const std::string text = readAll(t.file(kConnectionLogFile));
    KV_CHECK(std::count(text.begin(), text.end(), '\n') == 2);
    KV_CHECK(text.find("203.0.113.1") != std::string::npos && text.find("203.0.113.2") != std::string::npos);
}

KV_TEST(writes_are_at_most_every_ten_seconds) {
    OutsideTempState t(L"log-flush");
    ConnectionLog log(t.dir, 10'000);
    KV_CHECK(log.load(kNow));
    KV_CHECK(log.record(line("connected", kNow)));
    KV_CHECK(log.flushIfDue(5'000, kNow));
    KV_CHECK(readAll(t.file(kConnectionLogFile)).find("203.0.113.1") != std::string::npos);
    KV_CHECK(log.record(line("connected", kNow + 1, "203.0.113.2")));
    KV_CHECK(log.flushIfDue(14'999, kNow + 9'999));
    KV_CHECK(readAll(t.file(kConnectionLogFile)).find("203.0.113.2") == std::string::npos);
    KV_CHECK(log.flushIfDue(15'000, kNow + 10'000));
    KV_CHECK(readAll(t.file(kConnectionLogFile)).find("203.0.113.2") != std::string::npos);
}

KV_TEST(a_log_that_cant_be_read_is_never_written_over) {
    OutsideTempState t(L"log-unreadable");
    KV_CHECK(CreateDirectoryW(t.file(kConnectionLogFile).c_str(), nullptr) != 0);
    ConnectionLog log(t.dir);
    std::string err;
    KV_CHECK(!log.load(kNow, &err));
    KV_CHECK(!err.empty() && !log.readable());
    KV_CHECK(log.record(line("connected", kNow)));
    KV_CHECK(!log.flush(&err));
    // The folder is still there, untouched, and the line waits in memory.
    KV_CHECK(GetFileAttributesW(t.file(kConnectionLogFile).c_str()) & FILE_ATTRIBUTE_DIRECTORY);
    KV_CHECK(log.lines().size() == 1);
    // Once it can be read, the waiting line follows the file's own.
    KV_CHECK(RemoveDirectoryW(t.file(kConnectionLogFile).c_str()) != 0);
    KV_CHECK(kvtest::writeRaw(t.file(kConnectionLogFile),
                              line("revoked", kNow - 5, "192.168.1.30", "home").toJson() + "\n"));
    KV_CHECK(log.load(kNow));
    KV_CHECK(log.flush());
    const auto lines = log.lines();
    KV_CHECK(lines.size() == 2 && lines[0].event == "revoked" && lines[1].event == "connected");
}

KV_TEST(a_log_with_bad_lines_is_kept_before_it_is_rewritten) {
    OutsideTempState t(L"log-bad");
    const std::string good = line("connected", kNow).toJson();
    KV_CHECK(kvtest::writeRaw(t.file(kConnectionLogFile), good + "\n{\"t\":broken\n"));
    ConnectionLog log(t.dir);
    std::string err;
    KV_CHECK(!log.load(kNow, &err));
    KV_CHECK(log.readable());
    KV_CHECK(kvhost::fileExists(t.file(L"connection-log.jsonl.bad")));
    KV_CHECK(readAll(t.file(L"connection-log.jsonl.bad")).find("broken") != std::string::npos);
    KV_CHECK(log.lines().size() == 1);
    KV_CHECK(log.flush());
    KV_CHECK(readAll(t.file(kConnectionLogFile)) == good + "\n");
}

KV_TEST(one_slash_64_rotating_its_addresses_is_one_line_and_history_survives) {
    // Repeats merge by source (the IPv4 address, or the IPv6 /64), not by the full address,
    // so a host rotating through its /64 can't flush 30 days of history out.
    OutsideTempState t(L"log-rotate");
    ConnectionLog log(t.dir, 10'000, 64 * 1024);
    KV_CHECK(log.load(kNow));
    LogLine old = line("paired", kNow - 1000, "192.168.1.30", "home");
    old.deviceName = "iPhone";
    KV_CHECK(log.record(old, "0123456789abcdef"));
    for (int i = 0; i < 10'000; ++i) {
        char a[64];
        std::snprintf(a, sizeof a, "2001:db8:7:7:%x:%x::1", i / 256 + 1, i % 256 + 1);
        KV_CHECK(log.record(line("refused_unknown_cert", kNow + i % 50, a)));
    }
    const auto lines = log.lines();
    KV_CHECK(lines.size() == 2);
    KV_CHECK(lines[0].event == "paired" && lines[0].deviceName == "iPhone");
    KV_CHECK(lines[1].repeat == 10'000);
}

KV_TEST(a_slow_file_write_never_holds_up_record_or_lines) {
    // The text is built under the log's lock, and the file is written after it is released, so a
    // slow disk (up to 20 MB, flushed) never stalls a caller that records a line or reads the log meanwhile.
    OutsideTempState t(L"log-slow");
    ConnectionLog log(t.dir, 0);
    KV_CHECK(log.load(kNow));
    std::mutex m;
    std::condition_variable cv;
    bool in_write = false, release = false;
    log.setWriteHookForTest([&] {
        std::unique_lock<std::mutex> lk(m);
        in_write = true;
        cv.notify_all();
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return release; });
    });
    KV_CHECK(log.record(line("refused_blocked", kNow, "203.0.113.7")));
    std::thread writer([&] { log.flush(); });
    {
        std::unique_lock<std::mutex> lk(m);
        KV_REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return in_write; }));
    }
    std::atomic<bool> done{false};
    std::thread caller([&] {
        log.record(line("refused_blocked", kNow + 70'000, "203.0.113.8"));
        (void)log.lines();
        done = true;
    });
    for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    KV_CHECK_MSG(done.load(), "record() waited on the file write");
    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    writer.join();
    caller.join();
    // The line recorded during the write is written by the next flush (the write it raced left the log dirty).
    log.setWriteHookForTest(nullptr);
    KV_CHECK(log.flush());
    KV_CHECK(readAll(t.file(kConnectionLogFile)).find("203.0.113.8") != std::string::npos);
}
