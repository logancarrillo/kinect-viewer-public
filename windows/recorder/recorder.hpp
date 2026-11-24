// Host-only recorder (port of macos/Recording/HostRecorder.swift), Windows edition.
//   IR / Depth only: 16-bit lossless. FFV1 .mkv via `ffmpeg -nostdin` (gray16le), or a 16-bit PNG
//   sequence via WIC. Both write <stem>_timestamps.csv.
//   Colour is live only (phase 0): Colour and every Side-by-Side layout are refused with
//   "Colour is live only. Switch to IR or Depth to record." before anything is created. There is no
//   colour writer at all, and nothing colour-derived is ever written.
// Files go to %USERPROFILE%\kinect-viewer-recordings\<stamp>_<mode>.nosync\ only: never OneDrive, owner-only
// NTFS ACL, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED (Windows Search). Recordings never leave this PC.
#pragma once

#include "kinect_core_c.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kvrec {

enum class Lossless16 { Ffv1Mkv = 0, Png16Sequence = 1 };

std::wstring defaultRecordingsDir(); // %USERPROFILE%\kinect-viewer-recordings
// True when path is inside one of the OneDrive roots (%OneDrive%, %OneDriveConsumer%, %OneDriveCommercial%,
// or the ones passed in, for tests).
bool isUnderOneDrive(const std::wstring& path, const std::vector<std::wstring>& extra_roots = {});
// Creates the folder (refusing OneDrive), applies the owner-only DACL and NOT_CONTENT_INDEXED, and writes
// the .nosync / .metadata_never_index markers for parity with the Mac.
bool ensureRecordingsDir(const std::wstring& dir, std::string* err);
// The ffmpeg to run: %KV_FFMPEG% when it is a full path to a file, then each PATH entry that is a full path, in
// order, then winget's Links folder; empty if none. Never the application's folder or the current folder, where a
// planted ffmpeg.exe would run as the user (SearchPathW looked there first).
std::wstring resolveFfmpeg();
// The same from given values (tests). is_file says whether a path is an existing file.
std::wstring resolveFfmpegFrom(const std::wstring& kv_ffmpeg, const std::wstring& path_var, const std::wstring& local_app_data,
                               const std::function<bool(const std::wstring&)>& is_file);
// Starts cmd (CREATE_NO_WINDOW) with std_in, std_out and std_err as its standard handles and no other handle of this
// process (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): the sockets, pipes and files the process holds stay its own. The three
// must be inheritable (a value may repeat). process and thread: the child's handles, for the caller to close;
// on failure false, with the Win32 error. (void* is HANDLE, so this header needs no windows.h.)
bool startChildWithOnly(const std::wstring& cmd, void* std_in, void* std_out, void* std_err, void** process, void** thread,
                        unsigned long* error);

struct RecorderOptions {
    std::wstring base_dir;      // empty = defaultRecordingsDir()
    Lossless16 lossless = Lossless16::Ffv1Mkv;
    std::wstring ffmpeg_path;   // empty = resolveFfmpeg()
    size_t max_queue = 90;      // frames buffered for the writer thread; more are dropped and counted
    // The depth camera's intrinsics (kv_engine_get_ir_intrinsics), kept in session_meta.json so the
    // ruler works on playback. Without them a playback's ruler says "Intrinsics unavailable".
    std::optional<kv_ir_intrinsics_t> ir_intrinsics;
};

struct RecorderStatus {
    bool recording = false;
    uint64_t frames_written = 0;
    uint64_t frames_dropped = 0;
    uint64_t bytes = 0;
    double mb_per_min = 0.0;
    double seconds = 0.0;
    std::wstring session_dir;
    std::string summary;
    std::string error;
};

class Recorder {
public:
    Recorder();
    ~Recorder();

    // IR and Depth only; anything else (Colour, Side by Side, later modes) carries colour or isn't raw.
    static bool canRecord(kv_view_mode_t mode);
    // width/height are the raw IR/Depth frame size. Any other mode fails with text::kColourLiveOnly
    // and creates nothing.
    bool start(kv_view_mode_t mode, uint32_t width, uint32_t height, const RecorderOptions& opts, std::string* err);
    void stop();
    bool isRecording() const { return recording_.load(); }

    // Queue a raw 16-bit frame (copied). Returns false if the queue is full (frame dropped) or not recording.
    bool pushRaw16(const uint16_t* px, uint32_t w, uint32_t h, uint64_t seq, uint32_t ticks, uint64_t arrival_ns);

    RecorderStatus status() const;

    struct Frame {
        std::vector<uint8_t> data;
        uint32_t w = 0, h = 0;
        uint64_t seq = 0;
        uint32_t ticks = 0;
        uint64_t arrival_ns = 0;
    };
    struct Backend;

private:
    void writerLoop();

    std::unique_ptr<Backend> backend_;
    std::thread writer_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Frame> queue_;
    bool stop_ = false;
    std::atomic<bool> recording_{false};
    size_t max_queue_ = 90;
    uint64_t start_ns_ = 0;
    uint64_t stop_ns_ = 0;
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::wstring session_dir_;
    std::string meta_slug_, meta_container_, meta_created_;
    uint32_t meta_w_ = 0, meta_h_ = 0;
    std::optional<kv_ir_intrinsics_t> meta_intrinsics_;
    void writeSessionMeta();
    std::string summary_;
    std::string error_;
    mutable uint64_t cached_bytes_ = 0;
    mutable uint64_t cached_at_ns_ = 0;
};

uint64_t directorySizeBytes(const std::wstring& dir);

} // namespace kvrec
