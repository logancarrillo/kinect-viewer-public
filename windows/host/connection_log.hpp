// W8 (outside the home), Phase A: the connection log (protocol v3 §8.6, pet-camera.md §5). connection-log.jsonl
// in the host state folder, one JSON object a line: device, time, address. Kept 30 days and at most 20 MB, on the
// PC only: it is never sent anywhere, owners included. Repeats of the same (event, device or certificate, source:
// the IPv4 address or IPv6 /64, detail but a disconnected line's session length) within a minute become one line
// with a repeat count.
//
// A scanner can't flush the history out:
// - Lines that name no device and aren't the PC's own (refusals, failed attempts, handshakes that ended early) are
//   at most kLogUnknownLinesPerMinute new lines a minute. Past that, one "suppressed" line counts the rest.
// - When the log would pass its size, the oldest lines go until it is at three quarters of it: those lines first,
//   and a device's or the PC's own lines only once none of them is left.
//
// Appended, not rewritten: each write adds the lines recorded since the last one. A line whose count grows after it
// was written is written again once its minute is over (or by flush()), the same line with the larger count; load()
// keeps the larger. The whole file is written (through writeOwnerOnlyFileAtomically) only for its first write, after
// a trim or a prune, after lines that didn't parse, and after a write that failed.
//
// Fails closed: a file that is there but can't be read is never written over (lines are kept in memory, and load()
// is tried again); one that was read but has bad lines is kept as connection-log.jsonl.bad first, and only then
// rewritten. A torn last line (no newline, doesn't parse: an append the host didn't finish) is dropped without a
// copy. An append never creates the file, and a failed flush counts as a failed write.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kvhost::outside {

constexpr const wchar_t* kConnectionLogFile = L"connection-log.jsonl";
constexpr uint64_t kLogKeepMs = 30ull * 24 * 60 * 60 * 1000;
constexpr size_t kLogMaxBytes = 20u * 1024 * 1024;
constexpr uint64_t kLogCoalesceMs = 60'000;
constexpr uint32_t kLogUnknownLinesPerMinute = 60;

// v3 §8.6 events.
bool isConnectionLogEvent(const std::string& event);

struct LogLine {
    uint64_t t = 0;         // unix ms
    std::string event;      // one of §8.6's events
    std::string path;       // "home" or "outside"
    std::string addr;       // source IP, unwrapped, no port
    std::string deviceId;   // optional
    std::string deviceName; // optional
    uint32_t repeat = 0;    // optional: how many times in all, when more than once
    std::string detail;     // optional
    std::string toJson() const;
};

// A line of the devices' or the PC's own history: it names a device, or it is a pairing, revoke, role change,
// invite event or endpoint notice. Never held back by the budget, and the last to go when the log is full.
bool isHistoryLine(const LogLine& line);

class ConnectionLog {
public:
    // maxBytes: 20 MB (a test passes less).
    explicit ConnectionLog(std::wstring stateDir, uint64_t flushEveryMs = 10'000, size_t maxBytes = kLogMaxBytes);

    // Reads the file and prunes it (nowMs: unix ms). Missing: an empty log. There but unreadable: false,
    // readable() stays false and nothing is written until a later load() succeeds; lines recorded meanwhile
    // are kept and written after the file's own.
    bool load(uint64_t nowMs, std::string* err = nullptr);
    bool readable() const;

    // who: the deviceId when known, else the certificate hash (hex), else empty; it is part of the coalescing
    // key only and is not written. False for an event that isn't §8.6's or a path other than home/outside.
    // A new line past the minute's budget is counted on the "suppressed" line instead (still true).
    bool record(LogLine line, const std::string& who = {});

    // Drops lines older than 30 days (the whole file is written next).
    void prune(uint64_t nowMs);
    // flush: writes everything not yet in the file, grown counts included (at exit). flushIfDue: at most every
    // flushEveryMs; a grown count waits until its line's minute is over by nowMs (unix ms).
    bool flush(std::string* err = nullptr);
    bool flushIfDue(uint64_t monotonicMs, uint64_t nowMs, std::string* err = nullptr);

    std::vector<LogLine> lines() const; // for the PC's "Connection log…", oldest first

    // Test seam: runs on the writing thread just before the file is written, with no lock of the log's held
    // except the write lock (a test makes the write slow here and checks nothing else waits on it).
    void setWriteHookForTest(std::function<void()> hook);

private:
    bool recordLocked(LogLine line, const std::string& who, bool budgeted);
    void reindexLocked();
    // Keeps the lines `keep` says, in order, and rebuilds the index; the whole file is written next.
    void keepOnlyLocked(const std::vector<bool>& keep);
    // Drops the oldest lines until the log is at most `target` bytes: unknown sources' first, then any.
    void trimLocked(size_t target);
    size_t trimTarget() const { return maxBytes_ - maxBytes_ / 4; }
    void pruneLocked(uint64_t nowMs);
    // Takes write_mu_ then mu_: the text is built under mu_, and written with only write_mu_ held, so a
    // slow disk never holds up record() or lines(). Must not be called with mu_ held.
    bool writeOut(bool all, uint64_t nowMs, std::string* err);

    std::wstring dir_;
    uint64_t flushEveryMs_;
    size_t maxBytes_;
    uint64_t lastFlush_ = 0;
    bool flushedOnce_ = false;
    mutable std::mutex mu_;
    // Serialises the file writes (taken before mu_, never while mu_ is held), so an older snapshot is never
    // written over a newer one.
    std::mutex write_mu_;
    std::function<void()> write_hook_;
    bool readable_ = true;
    bool dirty_ = false;    // something may be waiting to be written
    bool rewrite_ = true;   // the next write writes the whole file
    std::deque<LogLine> lines_;
    std::deque<std::string> keys_;  // coalescing key of each line
    std::deque<size_t> sizes_;      // bytes each line takes in the file, newline included
    std::deque<uint32_t> on_disk_;  // the count the file holds for each line: 0 when it isn't in the file yet
    size_t bytes_ = 0;              // the lines in memory, each once
    size_t file_bytes_ = 0;         // the file as this log last wrote or read it (repeated lines included)
    std::unordered_map<std::string, size_t> last_; // key -> index of its newest line
    uint64_t budget_minute_ = 0;    // the minute (unix ms / 60 000) the budget counts
    uint32_t budget_used_ = 0;      // unknown sources' new lines in that minute
};

} // namespace kvhost::outside
