#include "recorder.hpp"

#include "folder_protection.hpp" // isUnderOneDrive, protectPrivateFolder
#include "host_state.hpp"    // applyOwnerOnlyAcl
#include "recordings.hpp"    // sessionMetaJson
#include "wire_protocol.hpp" // text::kColourLiveOnly

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

using Microsoft::WRL::ComPtr;

namespace kvrec {

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n) : std::wstring();
}

const char* kCsvHeader = "frame_index,engine_seq,device_ticks,arrival_mach_ns\n";

} // namespace

std::wstring defaultRecordingsDir() {
    const std::wstring home = envVar(L"USERPROFILE");
    return (home.empty() ? std::wstring(L".") : home) + L"\\kinect-viewer-recordings";
}

// The OneDrive check and the folder protections moved to host/folder_protection.cpp unchanged, so the
// camera roll shares them (pet-camera.md §6.5). The recorder keeps its own refusal message.
bool isUnderOneDrive(const std::wstring& path, const std::vector<std::wstring>& extra_roots) {
    return kvhost::isUnderOneDrive(path, extra_roots);
}

bool ensureRecordingsDir(const std::wstring& dir, std::string* err) {
    if (isUnderOneDrive(dir)) {
        if (err) *err = "refusing to record inside OneDrive: " + narrow(dir);
        return false;
    }
    return kvhost::protectPrivateFolder(dir, err);
}

namespace {
// A full path: "C:\..." (a drive, then a separator) or "\\server\share\...". Not ".", "bin", "\bin" or "C:bin".
bool isFullPath(const std::wstring& p) {
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return true;
    return p.size() >= 3 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/');
}
} // namespace

std::wstring resolveFfmpegFrom(const std::wstring& kv_ffmpeg, const std::wstring& path_var, const std::wstring& local_app_data,
                               const std::function<bool(const std::wstring&)>& is_file) {
    if (isFullPath(kv_ffmpeg) && is_file(kv_ffmpeg)) return kv_ffmpeg;
    size_t at = 0;
    while (at <= path_var.size()) {
        const size_t end = std::min(path_var.find(L';', at), path_var.size());
        std::wstring dir = path_var.substr(at, end - at);
        at = end + 1;
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (!isFullPath(dir)) continue; // empty, ".", a relative folder or an unexpanded %VAR%: never searched
        if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        const std::wstring exe = dir + L"ffmpeg.exe";
        if (is_file(exe)) return exe;
    }
    if (isFullPath(local_app_data)) {
        const std::wstring links = local_app_data + L"\\Microsoft\\WinGet\\Links\\ffmpeg.exe";
        if (is_file(links)) return links;
    }
    return {};
}

std::wstring resolveFfmpeg() {
    return resolveFfmpegFrom(envVar(L"KV_FFMPEG"), envVar(L"PATH"), envVar(L"LOCALAPPDATA"), [](const std::wstring& p) {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    });
}

bool startChildWithOnly(const std::wstring& cmd, void* std_in, void* std_out, void* std_err, void** process, void** thread,
                        unsigned long* error) {
    *process = *thread = nullptr;
    HANDLE list[3];
    size_t n = 0;
    for (HANDLE h : {static_cast<HANDLE>(std_in), static_cast<HANDLE>(std_out), static_cast<HANDLE>(std_err)}) {
        if (h && h != INVALID_HANDLE_VALUE && std::find(list, list + n, h) == list + n) list[n++] = h; // each once
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> storage(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (n == 0 || !InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
        if (error) *error = n == 0 ? ERROR_INVALID_PARAMETER : GetLastError();
        return false;
    }
    DWORD e = 0;
    bool ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, n * sizeof(HANDLE), nullptr, nullptr) != 0;
    if (ok) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = std_in;
        si.StartupInfo.hStdOutput = std_out;
        si.StartupInfo.hStdError = std_err;
        si.lpAttributeList = attrs;
        std::wstring line = cmd; // CreateProcessW may write to it
        PROCESS_INFORMATION pi{};
        ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &si.StartupInfo, &pi) != 0;
        if (ok) {
            *process = pi.hProcess;
            *thread = pi.hThread;
        }
    }
    if (!ok) e = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!ok && error) *error = e;
    return ok;
}

uint64_t directorySizeBytes(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

using Frame = Recorder::Frame;

// ---- backends --------------------------------------------------------------------------------------

struct Recorder::Backend {
    std::wstring dir;
    std::wstring stem;
    FILE* csv = nullptr;
    uint64_t index = 0;
    virtual ~Backend() { closeCsv(); }
    bool openCsv(std::string* err) {
        csv = _wfopen((dir + L"\\" + stem + L"_timestamps.csv").c_str(), L"wb");
        if (!csv) {
            if (err) *err = "cannot create timestamps CSV";
            return false;
        }
        std::fputs(kCsvHeader, csv);
        return true;
    }
    void csvRow(uint64_t seq, uint32_t ticks, uint64_t arrival) {
        if (csv) {
            std::fprintf(csv, "%llu,%llu,%u,%llu\n", static_cast<unsigned long long>(index), static_cast<unsigned long long>(seq),
                         ticks, static_cast<unsigned long long>(arrival));
        }
    }
    void closeCsv() {
        if (csv) {
            std::fclose(csv);
            csv = nullptr;
        }
    }
    virtual bool write(const Frame& f, std::string* err) = 0;
    virtual void finish() = 0;
};

namespace {

// FFV1 v3 16-bit lossless through `ffmpeg -nostdin` (same arguments as the Mac's FFV1 backend).
struct Ffv1Backend final : Recorder::Backend {
    HANDLE stdin_w = nullptr;
    PROCESS_INFORMATION pi{};
    uint32_t w = 0, h = 0;

    bool open(const std::wstring& ffmpeg, uint32_t width, uint32_t height, std::string* err) {
        w = width;
        h = height;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 4 * 1024 * 1024)) {
            if (err) *err = "CreatePipe failed";
            return false;
        }
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        const std::wstring mkv = dir + L"\\" + stem + L"_lossless.mkv";
        std::wstring cmd = L"\"" + ffmpeg + L"\" -nostdin -hide_banner -loglevel error -y -f rawvideo -pixel_format gray16le "
                           L"-video_size " + std::to_wstring(w) + L"x" + std::to_wstring(h) +
                           L" -framerate 30 -i pipe:0 -c:v ffv1 -level 3 -coder 1 -context 1 -g 1 -slicecrc 1 \"" + mkv + L"\"";
        // Only its own pipe end and NUL: no other handle of this process (a socket of the host's, a file) reaches it.
        unsigned long start_error = 0;
        const bool ok = startChildWithOnly(cmd, rd, nul, nul, &pi.hProcess, &pi.hThread, &start_error);
        CloseHandle(rd);
        CloseHandle(nul);
        if (!ok) {
            CloseHandle(wr);
            if (err) *err = "cannot start ffmpeg (" + std::to_string(start_error) + ")";
            return false;
        }
        stdin_w = wr;
        return openCsv(err);
    }
    bool write(const Frame& f, std::string* err) override {
        DWORD written = 0;
        const DWORD n = static_cast<DWORD>(f.data.size());
        if (!WriteFile(stdin_w, f.data.data(), n, &written, nullptr) || written != n) {
            if (err) *err = "ffmpeg pipe closed";
            return false;
        }
        csvRow(f.seq, f.ticks, f.arrival_ns);
        ++index;
        return true;
    }
    void finish() override {
        if (stdin_w) {
            CloseHandle(stdin_w);
            stdin_w = nullptr;
        }
        if (pi.hProcess) {
            WaitForSingleObject(pi.hProcess, 8000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
        closeCsv();
    }
    ~Ffv1Backend() override { finish(); }
};

// 16-bit grayscale PNG per frame via WIC.
struct Png16Backend final : Recorder::Backend {
    ComPtr<IWICImagingFactory> factory;
    bool open(std::string* err) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            if (err) *err = "WIC unavailable";
            return false;
        }
        return openCsv(err);
    }
    bool write(const Frame& f, std::string* err) override {
        wchar_t name[64];
        swprintf(name, 64, L"_%06llu.png", static_cast<unsigned long long>(index));
        const std::wstring path = dir + L"\\" + stem + name;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(f.w, f.h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat16bppGray;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
        if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat16bppGray) hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            hr = frame->WritePixels(f.h, f.w * 2, static_cast<UINT>(f.data.size()), const_cast<BYTE*>(f.data.data()));
        }
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        if (FAILED(hr)) {
            if (err) *err = "PNG16 write failed";
            return false;
        }
        csvRow(f.seq, f.ticks, f.arrival_ns);
        ++index;
        return true;
    }
    void finish() override { closeCsv(); }
};

std::wstring stampNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[40];
    swprintf(b, 40, L"%04u%02u%02u-%02u%02u%02u-%03u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    return b;
}

} // namespace

// ---- Recorder ---------------------------------------------------------------------------------------

Recorder::Recorder() = default;
Recorder::~Recorder() { stop(); }

bool Recorder::canRecord(kv_view_mode_t mode) {
    return mode == KV_MODE_IR || mode == KV_MODE_DEPTH;
}

bool Recorder::start(kv_view_mode_t mode, uint32_t width, uint32_t height, const RecorderOptions& opts, std::string* err) {
    stop();
    // Colour is live only: refused before anything, a folder included, is created.
    if (!canRecord(mode)) {
        if (err) *err = kvhost::text::kColourLiveOnly;
        return false;
    }
    const std::wstring base = opts.base_dir.empty() ? defaultRecordingsDir() : opts.base_dir;
    if (!ensureRecordingsDir(base, err)) return false;
    const wchar_t* slug = mode == KV_MODE_IR ? L"ir16" : L"depth16mm";
    const std::wstring dir = base + L"\\" + stampNow() + L"_" + slug + L".nosync";
    if (!kvhost::ensureOwnerOnlyDirectory(dir, err)) return false;
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);

    std::unique_ptr<Backend> b;
    {
        std::wstring ff = opts.ffmpeg_path.empty() ? resolveFfmpeg() : opts.ffmpeg_path;
        if (opts.lossless == Lossless16::Ffv1Mkv) {
            if (ff.empty()) {
                if (err) *err = "FFV1 needs ffmpeg (not found); choose the PNG16 sequence instead";
                return false;
            }
            auto f = std::make_unique<Ffv1Backend>();
            f->dir = dir;
            f->stem = slug;
            if (!f->open(ff, width, height, err)) return false;
            summary_ = mode == KV_MODE_IR ? "16-bit IR FFV1 lossless (.mkv)" : "16-bit Depth (mm) FFV1 lossless (.mkv)";
            b = std::move(f);
        } else {
            auto p = std::make_unique<Png16Backend>();
            p->dir = dir;
            p->stem = slug;
            if (!p->open(err)) return false;
            summary_ = mode == KV_MODE_IR ? "16-bit IR PNG sequence" : "16-bit Depth (mm) PNG sequence";
            b = std::move(p);
        }
    }
    backend_ = std::move(b);
    session_dir_ = dir;
    meta_slug_ = mode == KV_MODE_IR ? "ir16" : "depth16mm";
    meta_container_ = opts.lossless == Lossless16::Png16Sequence ? "png16" : "ffv1";
    meta_created_ = isoUtc(unixNow());
    meta_w_ = width;
    meta_h_ = height;
    meta_intrinsics_ = opts.ir_intrinsics;
    error_.clear();
    max_queue_ = std::max<size_t>(4, opts.max_queue);
    written_ = 0;
    dropped_ = 0;
    start_ns_ = nowNs();
    stop_ns_ = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = false;
        queue_.clear();
    }
    recording_ = true;
    writer_ = std::thread([this] { writerLoop(); });
    return true;
}

void Recorder::stop() {
    if (!writer_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        cv_.notify_all();
    }
    writer_.join();
    if (backend_) backend_->finish();
    backend_.reset();
    recording_ = false;
    stop_ns_ = nowNs();
    cached_at_ns_ = 0;
    writeSessionMeta();
}

// session_meta.json beside the frames (the Mac's RecordingSessionMetadata): what the recordings list
// reads, and the intrinsics a playback's ruler needs.
void Recorder::writeSessionMeta() {
    if (session_dir_.empty() || meta_slug_.empty()) return;
    const double secs = stop_ns_ > start_ns_ ? static_cast<double>(stop_ns_ - start_ns_) / 1e9 : 0.0;
    const std::string body = sessionMetaJson(meta_created_, meta_slug_, meta_container_, static_cast<int>(meta_w_),
                                             static_cast<int>(meta_h_), static_cast<int>(written_.load()), secs, meta_intrinsics_);
    const std::wstring path = session_dir_ + L"\\session_meta.json";
    if (FILE* fm = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(body.data(), 1, body.size(), fm);
        std::fclose(fm);
    }
    meta_slug_.clear(); // once per session
}

bool Recorder::pushRaw16(const uint16_t* px, uint32_t w, uint32_t h, uint64_t seq, uint32_t ticks, uint64_t arrival_ns) {
    if (!recording_) return false;
    Frame f;
    f.data.assign(reinterpret_cast<const uint8_t*>(px), reinterpret_cast<const uint8_t*>(px) + static_cast<size_t>(w) * h * 2);
    f.w = w;
    f.h = h;
    f.seq = seq;
    f.ticks = ticks;
    f.arrival_ns = arrival_ns;
    std::lock_guard<std::mutex> lk(m_);
    if (queue_.size() >= max_queue_) {
        ++dropped_;
        return false;
    }
    queue_.push_back(std::move(f));
    cv_.notify_one();
    return true;
}

void Recorder::writerLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (true) {
        Frame f;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) break; // stop requested and drained
            f = std::move(queue_.front());
            queue_.pop_front();
        }
        std::string err;
        if (backend_->write(f, &err)) {
            ++written_;
        } else {
            std::lock_guard<std::mutex> lk(m_);
            error_ = err;
        }
    }
    CoUninitialize();
}

RecorderStatus Recorder::status() const {
    RecorderStatus st;
    st.recording = recording_;
    st.frames_written = written_;
    st.frames_dropped = dropped_;
    st.session_dir = session_dir_;
    st.summary = summary_;
    {
        std::lock_guard<std::mutex> lk(m_);
        st.error = error_;
    }
    const uint64_t now = nowNs();
    if (!session_dir_.empty() && (cached_at_ns_ == 0 || now - cached_at_ns_ > 1000000000ULL)) {
        cached_bytes_ = directorySizeBytes(session_dir_);
        cached_at_ns_ = now;
    }
    st.bytes = cached_bytes_;
    const uint64_t end = recording_ ? now : (stop_ns_ ? stop_ns_ : now);
    st.seconds = start_ns_ ? static_cast<double>(end - start_ns_) / 1e9 : 0.0;
    st.mb_per_min = st.seconds > 0.5 ? (static_cast<double>(st.bytes) / 1e6) / (st.seconds / 60.0) : 0.0;
    return st;
}

} // namespace kvrec
