// Recordings inside the app (the Mac's phase 2 and phase 7 fixes 6 and 12; RecordingLibraryAndPlayer.swift):
// the list of sessions in %USERPROFILE%\kinect-viewer-recordings (metadata only,
// never a thumbnail or a preview), the 7-day automatic deletion with the deletion log, deletion by the
// user, the purge of old colour sessions, the reminder before recording, and a reader that plays a
// 16-bit session back through the live pipeline.
//
// Times are Unix seconds (UTC). Folder names keep the recorder's local-time stamp
// (yyyyMMdd-HHmmss-SSS_<mode>.nosync), as on the Mac.
#pragma once

#include "kinect_core_c.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kvrec {

constexpr int64_t kRetentionSeconds = 7 * 86400;
constexpr const wchar_t* kDeletionLogName = L"deletion-log.txt";
constexpr int kMaxSessionFrames = 30 * 86400 * 7; // a week at 30 fps: more is a damaged file

int64_t unixNow();

struct SessionItem {
    std::wstring folder;      // full path
    std::wstring folder_name; // e.g. 20261112-204100-123_depth16mm.nosync
    int64_t created = 0;      // Unix seconds
    bool is_depth = false;
    std::string container; // "ffv1" or "png16"
    int width = 512, height = 424;
    int frame_count = 0;
    double duration_sec = 0.0;
    uint64_t size_bytes = 0;
    std::optional<kv_ir_intrinsics_t> ir_intrinsics; // from session_meta.json; none in an older session

    std::wstring modeSlug() const { return is_depth ? L"depth16mm" : L"ir16"; }
    std::wstring modeTitle() const { return is_depth ? L"Depth" : L"IR"; }
    int64_t expiresAt() const { return created + kRetentionSeconds; }
    bool isExpired(int64_t now) const { return now >= expiresAt(); }
    std::wstring countdownText(int64_t now) const; // "6d 23h left", "4h 12m left", "3m left", "Expired"
    std::wstring lengthText() const;               // "12.3 s (369 f)", "2m 05s (3750 f)"
    std::wstring sizeText() const;                 // "12.3 MB", "456 KB"
    std::wstring dateText() const;                 // local time, "2026-11-12 20:41"
};

// The local-time stamp at the front of a folder name, as Unix seconds; nullopt if it isn't one.
std::optional<int64_t> parseFolderTimestamp(const std::wstring& folder_name);

// Every IR and Depth session in base, newest first. A folder that isn't one (an old colour or
// side-by-side session, anything unknown) is never listed, and never as IR. exclude: the folder of
// a recording in progress.
std::vector<SessionItem> listSessions(const std::wstring& base, const std::wstring& exclude = {});

enum class DeletionTrigger { Automatic, User };
// One line per deletion, nothing about the content: "<ISO-8601 UTC>\t<folder>\t<N> bytes\t<trigger>".
void appendDeletionLog(const std::wstring& base, const std::wstring& name, uint64_t size_bytes, int64_t when,
                       DeletionTrigger trigger);
std::string deletionLogLine(const std::wstring& name, uint64_t size_bytes, int64_t when, DeletionTrigger trigger);

// Deletes every .nosync folder that isn't an IR or Depth session (old *_color1080p / *_sidebyside
// HEVC ones included), logging each as automatic. Returns the names deleted.
std::vector<std::wstring> purgeDisallowedSessions(const std::wstring& base, int64_t now, const std::wstring& exclude = {});
// purgeDisallowedSessions, then every session 7 days old or more. At launch and once an hour.
std::vector<std::wstring> purgeExpiredSessions(const std::wstring& base, int64_t now, const std::wstring& exclude = {});
// The user's Delete; logged as user. False if the folder couldn't be removed.
bool deleteSessionByUser(const SessionItem& s, const std::wstring& base, int64_t now);

uint64_t treeSizeBytes(const std::wstring& dir);
bool removeTree(const std::wstring& dir);

// The reminder before every recording (shared text in wire_protocol.hpp, text::kRecordingReminder),
// with "don't remind me for 1 hour".
class ReminderPolicy {
public:
    bool needed(int64_t now) const { return now >= suppressed_until_; }
    void snoozeOneHour(int64_t now);
    int64_t suppressedUntil() const { return suppressed_until_; }

private:
    int64_t suppressed_until_ = 0;
};

// session_meta.json, as the Mac writes it (RecordingSessionMetadata): created, mode, container,
// size, frame count, duration and the IR intrinsics when known.
std::string sessionMetaJson(const std::string& created_iso, const std::string& mode_slug, const std::string& container,
                            int width, int height, int frame_count, double duration_sec,
                            const std::optional<kv_ir_intrinsics_t>& intrinsics);
std::string isoUtc(int64_t unix_seconds); // "2026-11-12T20:41:00Z"

// Reads a session's 16-bit frames by index: PNG16 through WIC, FFV1 through `ffmpeg -nostdin -ss
// <t> -i <mkv> -frames:v <n> -f rawvideo -pix_fmt gray16le` in chunks (all-intra, so a chunk starts
// without decoding from frame 0). Call from one thread (the decode worker below); it needs COM there.
class SessionReader {
public:
    explicit SessionReader(SessionItem s, int chunk = 60);
    const SessionItem& session() const { return s_; }
    int totalFrames() const { return total_; }
    uint32_t deviceTicks(int index) const;
    double frameIntervalSeconds(int index) const; // from the CSV, clamped to [1/120, 0.25]; 1/30 without
    bool readFrame(int index, std::vector<uint16_t>& out);
    // From another thread: kills a running ffmpeg chunk decode, and any later read fails (playback stop).
    void cancel();

private:
    bool readPng(int index, std::vector<uint16_t>& out);
    bool decodeChunk(int chunk_index);
    SessionItem s_;
    int chunk_;
    int total_ = 0;
    std::vector<uint64_t> ts_ns_;
    std::vector<uint32_t> ticks_;
    int cached_chunk_ = -1;
    std::vector<std::vector<uint16_t>> cached_;
    std::mutex proc_m_;
    void* proc_ = nullptr; // the running ffmpeg (a HANDLE), while a chunk decodes
    std::atomic<bool> cancelled_{false};
};

// Decodes off the UI thread (phase 7 fix 12): request(index) queues the latest wanted frame (older
// requests are dropped), take() hands back a decoded one if there is one. The worker owns the reader.
class ReplayDecoder {
public:
    struct Frame {
        int index = -1;
        std::vector<uint16_t> pixels;
        uint32_t ticks = 0;
        bool ok = false;
    };
    explicit ReplayDecoder(SessionItem s);
    ~ReplayDecoder();
    void request(int index);
    std::optional<Frame> take();
    int totalFrames() const { return total_; }
    double frameIntervalSeconds(int index) const;

private:
    void loop();
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    int wanted_ = -1;
    std::optional<Frame> ready_;
    int total_ = 0;
    std::vector<double> intervals_;
    SessionItem s_;
    std::unique_ptr<SessionReader> reader_; // used by the worker; cancelled from the destructor
    std::thread th_;
};

} // namespace kvrec
