#include "connection_log.hpp"

#include "json_lite.hpp"
#include "outside_address.hpp"
#include "state_files.hpp"

#include <algorithm>
#include <optional>

namespace kvhost::outside {

namespace {

const char* const kEvents[] = {"connected",          "disconnected",       "refused_unknown_cert", "refused_rate_limit",
                               "refused_blocked",    "refused_local_only", "refused_too_many",     "refused_unavailable",
                               "paired",             "invite_redeemed",    "invite_failed",        "invite_burned",
                               "revoked",            "role_changed",       "invite_closed",        "outside_endpoint_told",
                               "pairing_failed",     "pairing_burned",     "suppressed"};

// History even when no device is named: a stranger can't make these at will (each needs an open window or
// invite, an owner, or the PC).
const char* const kHistoryEvents[] = {"paired",        "pairing_burned", "invite_redeemed",      "invite_burned",
                                      "invite_closed", "revoked",        "role_changed",         "outside_endpoint_told"};

constexpr const char* kSuppressed = "suppressed";
constexpr uint64_t kMinuteMs = 60'000;

// The coalescing key: the event, the device or certificate, the source as the limiter counts it (§8.0: the IPv4
// address, or the IPv6 /64), so a host rotating through its /64 is one source here too, and the detail: attempts
// that ended differently (a browser's, and the app's, from one carrier address) are lines of their own.
// The detail as the coalescing key reads it. A session's length ("42 s", "replaced, 42 s" on a disconnected line) is
// left out, so a device's reconnect loop is one line a minute however long each session lasted; the line keeps the
// first one's length.
std::string coalescingDetail(const LogLine& l) {
    if (l.event != "disconnected") return l.detail;
    const std::string& d = l.detail;
    if (d.size() < 3 || d.compare(d.size() - 2, 2, " s") != 0) return d;
    size_t digits = d.size() - 2;
    while (digits > 0 && d[digits - 1] >= '0' && d[digits - 1] <= '9') --digits;
    if (digits == d.size() - 2) return d; // no number before " s"
    if (digits == 0) return std::string();
    if (digits >= 2 && d.compare(digits - 2, 2, ", ") == 0) return d.substr(0, digits - 2);
    return d;
}

std::string keyOf(const LogLine& l, const std::string& who) {
    const auto ip = parseIp(l.addr);
    const std::string source = ip ? sourceKey(*ip) : l.addr;
    return l.event + '\x1f' + who + '\x1f' + source + '\x1f' + l.path + '\x1f' + coalescingDetail(l);
}

bool fromJson(const std::string& text, LogLine& l) {
    json::Value v;
    if (!json::parse(text, v) || v.type != json::Value::Type::Object) return false;
    std::optional<std::string> id, name, detail;
    std::optional<uint32_t> repeat;
    if (!json::getU64(v, "t", l.t) || !json::getString(v, "event", l.event) || !json::getString(v, "path", l.path) ||
        !json::getString(v, "addr", l.addr) || !json::getOptString(v, "deviceId", id) ||
        !json::getOptString(v, "deviceName", name) || !json::getOptU32(v, "repeat", repeat) ||
        !json::getOptString(v, "detail", detail))
        return false;
    if (l.path != "home" && l.path != "outside") return false;
    l.deviceId = id.value_or("");
    l.deviceName = name.value_or("");
    l.repeat = repeat.value_or(0);
    l.detail = detail.value_or("");
    return true;
}

uint32_t countOf(const LogLine& l) { return l.repeat > 1 ? l.repeat : 1u; }

} // namespace

bool isConnectionLogEvent(const std::string& event) {
    return std::find(std::begin(kEvents), std::end(kEvents), event) != std::end(kEvents);
}

bool isHistoryLine(const LogLine& line) {
    return !line.deviceId.empty() ||
           std::find(std::begin(kHistoryEvents), std::end(kHistoryEvents), line.event) != std::end(kHistoryEvents);
}

std::string LogLine::toJson() const {
    json::ObjectWriter w;
    w.field("t", t).field("event", event).field("path", path).field("addr", addr);
    if (!deviceId.empty()) w.field("deviceId", deviceId);
    if (!deviceName.empty()) w.field("deviceName", deviceName);
    if (repeat > 1) w.field("repeat", repeat);
    if (!detail.empty()) w.field("detail", detail);
    return w.finish();
}

ConnectionLog::ConnectionLog(std::wstring stateDir, uint64_t flushEveryMs, size_t maxBytes)
    : dir_(std::move(stateDir)), flushEveryMs_(flushEveryMs), maxBytes_(maxBytes) {}

bool ConnectionLog::readable() const {
    std::lock_guard<std::mutex> lock(mu_);
    return readable_;
}

bool ConnectionLog::load(uint64_t nowMs, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::wstring path = dir_ + L"\\" + kConnectionLogFile;
    unsigned long win32 = 0;
    const FileState fs = fileState(path, &win32);
    std::deque<LogLine> fromFile;
    std::deque<std::string> fileKeys;
    size_t fileSize = 0;
    bool bad = false;
    bool torn = false; // the last line, with no newline after it, didn't parse: a write the host didn't finish
    if (fs == FileState::Unknown) {
        readable_ = false;
        if (err) *err = "connection-log.jsonl can't be checked (Win32 error " + std::to_string(win32) + ")";
        return false;
    }
    if (fs == FileState::Present) {
        std::string text;
        if (!readWholeFile(path, text)) {
            readable_ = false;
            if (err) *err = "connection-log.jsonl can't be read";
            return false;
        }
        fileSize = text.size();
        // A line written again with a larger count (its count grew after it was first written) folds into the first:
        // the same line at the same time, with a count of two or more. The larger count is kept.
        std::unordered_map<std::string, size_t> at;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string one = text.substr(start, end - start);
            if (!one.empty() && one.back() == '\r') one.pop_back();
            if (!one.empty()) {
                LogLine l;
                if (fromJson(one, l)) {
                    // Lines read back coalesce by device, else by source (the hash isn't written).
                    std::string key = keyOf(l, l.deviceId);
                    const std::string when = key + '\x1f' + std::to_string(l.t);
                    const auto it = at.find(when);
                    if (it != at.end() && l.repeat > 1) {
                        LogLine& first = fromFile[it->second];
                        first.repeat = std::max(first.repeat, l.repeat);
                    } else {
                        at[when] = fromFile.size();
                        fileKeys.push_back(std::move(key));
                        fromFile.push_back(std::move(l));
                    }
                } else if (end == text.size() && text.back() != '\n') {
                    // Appends end every line with its newline, so a last line without one that doesn't parse is a
                    // write cut short (the host stopped mid-append). It is dropped, and the file written whole
                    // without it: no .bad copy, since nothing in it is anyone's to look at.
                    torn = true;
                } else bad = true;
            }
            start = end + 1;
        }
    }
    if (bad) {
        std::string what;
        if (!keepBadCopy(dir_, kConnectionLogFile, &what)) {
            readable_ = false;
            if (err) *err = "connection-log.jsonl has lines that didn't parse, and " + what;
            return false;
        }
        if (err) *err = "connection-log.jsonl had lines that didn't parse; " + what;
    } else if (torn && err) {
        *err = "connection-log.jsonl's last line was cut short (the host stopped while writing it), so it was dropped";
    }
    std::deque<uint32_t> onDisk;
    for (const auto& l : fromFile) onDisk.push_back(countOf(l));
    // The file's lines first, then any recorded while it couldn't be read (none of them is in the file). A load
    // while it could be read replaces what is in memory (that came from the file, or was already written to it).
    const bool hadPending = !readable_ && !lines_.empty();
    if (hadPending) {
        fromFile.insert(fromFile.end(), lines_.begin(), lines_.end());
        fileKeys.insert(fileKeys.end(), keys_.begin(), keys_.end());
        onDisk.insert(onDisk.end(), lines_.size(), 0u);
    }
    lines_ = std::move(fromFile);
    keys_ = std::move(fileKeys);
    on_disk_ = std::move(onDisk);
    sizes_.clear();
    bytes_ = 0;
    for (const auto& l : lines_) {
        sizes_.push_back(l.toJson().size() + 1);
        bytes_ += sizes_.back();
    }
    reindexLocked();
    readable_ = true;
    file_bytes_ = fileSize;
    // A missing file is made whole (owner-only), never appended to; one with bad lines is written whole once kept, and
    // one with a torn last line is written whole without it.
    rewrite_ = bad || torn || fs != FileState::Present;
    dirty_ = bad || torn || hadPending;
    pruneLocked(nowMs);
    return !bad;
}

bool ConnectionLog::record(LogLine line, const std::string& who) {
    if (!isConnectionLogEvent(line.event) || (line.path != "home" && line.path != "outside")) return false;
    std::lock_guard<std::mutex> lock(mu_);
    return recordLocked(std::move(line), who, true);
}

bool ConnectionLog::recordLocked(LogLine line, const std::string& who, bool budgeted) {
    const std::string key = keyOf(line, who);
    auto it = last_.find(key);
    if (it != last_.end()) {
        const size_t at = it->second;
        LogLine& prev = lines_[at];
        if (line.t >= prev.t && line.t - prev.t < kLogCoalesceMs) {
            prev.repeat = (prev.repeat < 2 ? 1u : prev.repeat) + 1u;
            const size_t size = prev.toJson().size() + 1;
            bytes_ = bytes_ - sizes_[at] + size;
            sizes_[at] = size;
            dirty_ = true;
            if (bytes_ > maxBytes_) trimLocked(trimTarget());
            return true;
        }
    }
    // A new line from a source no device is known for counts against its minute's budget. Past it, the line is
    // counted on the minute's "suppressed" line (coalesced like any other), so a flood from many sources adds one
    // line a minute after the first ones.
    if (budgeted && !isHistoryLine(line) && line.event != kSuppressed) {
        const uint64_t minute = line.t / kMinuteMs;
        if (minute != budget_minute_) {
            budget_minute_ = minute;
            budget_used_ = 0;
        }
        if (budget_used_ >= kLogUnknownLinesPerMinute) {
            LogLine s;
            s.t = line.t;
            s.event = kSuppressed;
            s.path = line.path;
            s.detail = "new lines from sources no device is known for, past " +
                       std::to_string(kLogUnknownLinesPerMinute) + " a minute";
            return recordLocked(std::move(s), std::string(), false);
        }
        ++budget_used_;
    }
    line.repeat = 0;
    sizes_.push_back(line.toJson().size() + 1);
    bytes_ += sizes_.back();
    lines_.push_back(std::move(line));
    keys_.push_back(key);
    on_disk_.push_back(0);
    last_[key] = lines_.size() - 1;
    dirty_ = true;
    if (bytes_ > maxBytes_) trimLocked(trimTarget());
    return true;
}

void ConnectionLog::reindexLocked() {
    last_.clear();
    for (size_t i = 0; i < keys_.size(); ++i) last_[keys_[i]] = i;
}

void ConnectionLog::keepOnlyLocked(const std::vector<bool>& keep) {
    std::deque<LogLine> lines;
    std::deque<std::string> keys;
    std::deque<size_t> sizes;
    std::deque<uint32_t> onDisk;
    size_t bytes = 0;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (!keep[i]) continue;
        lines.push_back(std::move(lines_[i]));
        keys.push_back(std::move(keys_[i]));
        sizes.push_back(sizes_[i]);
        onDisk.push_back(on_disk_[i]);
        bytes += sizes_[i];
    }
    lines_ = std::move(lines);
    keys_ = std::move(keys);
    sizes_ = std::move(sizes);
    on_disk_ = std::move(onDisk);
    bytes_ = bytes;
    reindexLocked();
    // The file still holds the lines dropped here: it is written whole next.
    rewrite_ = true;
    dirty_ = true;
}

void ConnectionLog::trimLocked(size_t target) {
    if (bytes_ <= target) return;
    // Oldest first: lines that aren't history, then (only if that wasn't enough) any. One rebuild, so a full log
    // trims once per quarter of its size, not once a line.
    std::vector<bool> keep(lines_.size(), true);
    size_t bytes = bytes_;
    for (int pass = 0; pass < 2 && bytes > target; ++pass) {
        for (size_t i = 0; i < lines_.size() && bytes > target; ++i) {
            if (!keep[i] || (pass == 0 && isHistoryLine(lines_[i]))) continue;
            keep[i] = false;
            bytes -= sizes_[i];
        }
    }
    keepOnlyLocked(keep);
}

void ConnectionLog::prune(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    pruneLocked(nowMs);
}

void ConnectionLog::pruneLocked(uint64_t nowMs) {
    const uint64_t cutoff = nowMs > kLogKeepMs ? nowMs - kLogKeepMs : 0;
    // Lines are in time order but for a clock that went back, so every line is checked.
    if (std::any_of(lines_.begin(), lines_.end(), [cutoff](const LogLine& l) { return l.t < cutoff; })) {
        std::vector<bool> keep(lines_.size());
        for (size_t i = 0; i < lines_.size(); ++i) keep[i] = lines_[i].t >= cutoff;
        keepOnlyLocked(keep);
    }
    if (bytes_ > maxBytes_) trimLocked(trimTarget());
}

bool ConnectionLog::writeOut(bool all, uint64_t nowMs, std::string* err) {
    std::lock_guard<std::mutex> wlock(write_mu_);
    std::string text;
    bool whole = false;
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!readable_) {
            if (err) *err = "connection-log.jsonl can't be read, so it isn't written over";
            return false;
        }
        if (!dirty_) return true;
        // What the file lacks: lines not in it yet, and counts that grew after their line was written (once the
        // line's minute is over, so a line is written at most twice; any, for flush()).
        std::vector<size_t> due;
        size_t add = 0;
        bool waiting = false;
        if (!rewrite_) {
            for (size_t i = 0; i < lines_.size(); ++i) {
                if (on_disk_[i] >= countOf(lines_[i])) continue;
                if (on_disk_[i] != 0 && !all && nowMs < lines_[i].t + kLogCoalesceMs) {
                    waiting = true;
                    continue;
                }
                due.push_back(i);
                add += sizes_[i];
            }
            // The file would pass its size (lines written twice take room too): trimmed, and written whole.
            if (file_bytes_ + add > maxBytes_) {
                trimLocked(trimTarget());
                rewrite_ = true;
            }
        }
        whole = rewrite_;
        if (whole) {
            text.reserve(bytes_);
            for (size_t i = 0; i < lines_.size(); ++i) {
                text += lines_[i].toJson() + "\n";
                on_disk_[i] = countOf(lines_[i]);
            }
            rewrite_ = false;
            dirty_ = false;
        } else {
            text.reserve(add);
            for (const size_t i : due) {
                text += lines_[i].toJson() + "\n";
                on_disk_[i] = countOf(lines_[i]);
            }
            dirty_ = waiting;
        }
        hook = write_hook_;
    }
    // v3 §8.6: written with the log's lock released, so a flood of refused connects (each one a record())
    // never waits on the disk, and neither does anything that holds the server's lock while it records.
    if (hook) hook();
    const bool ok = whole ? writeOwnerOnlyFileAtomically(dir_, kConnectionLogFile, text, err)
                          : (text.empty() || appendToExistingFile(dir_ + L"\\" + kConnectionLogFile, text, err));
    std::lock_guard<std::mutex> lock(mu_);
    if (ok) {
        file_bytes_ = whole ? text.size() : file_bytes_ + text.size();
    } else {
        // How much reached the file isn't known (part of an append, or nothing): the next write makes it whole.
        rewrite_ = true;
        dirty_ = true;
    }
    return ok;
}

bool ConnectionLog::flush(std::string* err) { return writeOut(true, 0, err); }

bool ConnectionLog::flushIfDue(uint64_t monotonicMs, uint64_t nowMs, std::string* err) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (flushedOnce_ && monotonicMs - lastFlush_ < flushEveryMs_) return true;
        flushedOnce_ = true;
        lastFlush_ = monotonicMs;
    }
    return writeOut(false, nowMs, err);
}

void ConnectionLog::setWriteHookForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mu_);
    write_hook_ = std::move(hook);
}

std::vector<LogLine> ConnectionLog::lines() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::vector<LogLine>(lines_.begin(), lines_.end());
}

} // namespace kvhost::outside
