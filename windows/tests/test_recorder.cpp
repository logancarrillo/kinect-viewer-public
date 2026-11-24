// Recorder tests on the synthetic source, in a temp folder (never the real recordings folder, never kept).
// FFV1 must be bit-exact: the .mkv is decoded back with ffmpeg and compared with the frames that were fed.
#include "kv_test.hpp"

#include "host_state.hpp"
#include "kinect_core_c.h"
#include "recorder.hpp"
#include "viewer_model.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <timeapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace kvrec;

namespace {

std::wstring tempBase(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"kv-rec-test-" + tag + L"-" + std::to_wstring(GetCurrentProcessId());
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

std::vector<std::wstring> listFiles(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

int countLines(const std::wstring& path) {
    std::ifstream f(path);
    int n = 0;
    std::string line;
    while (std::getline(f, line)) ++n;
    return n;
}

// Runs a command line, capturing stdout into out (binary).
bool runCapture(const std::wstring& cmdline, std::vector<uint8_t>& out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 1 << 20);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    uint8_t buf[65536];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.insert(out.end(), buf, buf + n);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return code == 0;
}

// Streams raw16 frames from a synthetic engine into the recorder; returns the frames fed.
std::vector<std::vector<uint16_t>> feedRaw16(Recorder& rec, bool depth, int count) {
    std::vector<std::vector<uint16_t>> fed;
    kv_engine_t* eng = kv_engine_create(nullptr);
    kv_display_params_t p;
    kv_default_display_params(&p);
    p.mode = depth ? KV_MODE_DEPTH : KV_MODE_IR;
    char err[256] = {0};
    if (!kv_engine_start(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, err, sizeof(err))) {
        kv_engine_destroy(eng);
        return fed;
    }
    std::vector<uint16_t> buf(512 * 424);
    uint64_t last = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (static_cast<int>(fed.size()) < count && std::chrono::steady_clock::now() < deadline) {
        uint64_t seq = 0, arr = 0;
        uint32_t ticks = 0, w = 0, h = 0;
        if (kv_engine_copy_latest_raw16(eng, depth, last, buf.data(), buf.size(), &seq, &arr, &ticks, &w, &h)) {
            last = seq;
            if (rec.pushRaw16(buf.data(), w, h, seq, ticks, arr)) fed.push_back(buf);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    kv_engine_destroy(eng);
    return fed;
}

} // namespace

KV_TEST(recordings_dir_is_owner_only_not_indexed_and_never_onedrive) {
    const std::wstring def = defaultRecordingsDir();
    KV_CHECK(def.size() > 25 && def.substr(def.size() - 25) == L"\\kinect-viewer-recordings");
    KV_CHECK(!isUnderOneDrive(def));
    KV_CHECK(isUnderOneDrive(L"C:\\Users\\x\\OneDrive\\stuff", {L"C:\\Users\\x\\OneDrive"}));
    KV_CHECK(!isUnderOneDrive(L"C:\\Users\\x\\OneDriveX\\stuff", {L"C:\\Users\\x\\OneDrive"}));

    const std::wstring base = tempBase(L"dir");
    removeTree(base);
    std::string err;
    KV_REQUIRE(ensureRecordingsDir(base, &err));
    KV_CHECK(kvhost::isOwnerOnly(base));
    KV_CHECK((GetFileAttributesW(base.c_str()) & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0);
    KV_CHECK(GetFileAttributesW((base + L"\\.nosync").c_str()) != INVALID_FILE_ATTRIBUTES);
    KV_CHECK(GetFileAttributesW((base + L"\\.metadata_never_index").c_str()) != INVALID_FILE_ATTRIBUTES);
    removeTree(base);
}

KV_TEST(ffv1_ir_and_depth_are_bit_exact_with_timestamps_csv) {
    const std::wstring ff = resolveFfmpeg();
    KV_REQUIRE(!ff.empty());
    for (bool depth : {false, true}) {
        const std::wstring base = tempBase(depth ? L"ffv1d" : L"ffv1i");
        removeTree(base);
        Recorder rec;
        RecorderOptions o;
        o.base_dir = base;
        o.lossless = Lossless16::Ffv1Mkv;
        std::string err;
        KV_REQUIRE(rec.start(depth ? KV_MODE_DEPTH : KV_MODE_IR, 512, 424, o, &err));
        const auto fed = feedRaw16(rec, depth, 45);
        rec.stop();
        const RecorderStatus st = rec.status();
        std::printf("  %s: fed=%zu written=%llu dropped=%llu bytes=%llu (%s)\n", depth ? "depth" : "ir", fed.size(),
                    static_cast<unsigned long long>(st.frames_written), static_cast<unsigned long long>(st.frames_dropped),
                    static_cast<unsigned long long>(st.bytes), st.summary.c_str());
        KV_REQUIRE(fed.size() == 45);
        KV_CHECK(st.frames_written == 45 && st.frames_dropped == 0);
        const std::wstring stem = depth ? L"depth16mm" : L"ir16";
        const std::wstring mkv = st.session_dir + L"\\" + stem + L"_lossless.mkv";
        KV_CHECK(countLines(st.session_dir + L"\\" + stem + L"_timestamps.csv") == 46);
        KV_CHECK(kvhost::isOwnerOnly(st.session_dir));
        KV_CHECK((GetFileAttributesW(mkv.c_str()) & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0);
        KV_CHECK(st.session_dir.find(L".nosync") != std::wstring::npos);

        std::vector<uint8_t> raw;
        KV_REQUIRE(runCapture(L"\"" + ff + L"\" -nostdin -hide_banner -loglevel error -i \"" + mkv +
                                  L"\" -f rawvideo -pix_fmt gray16le pipe:1",
                              raw));
        const size_t frame_bytes = 512 * 424 * 2;
        KV_CHECK(raw.size() == frame_bytes * fed.size());
        int max_abs_diff = 0;
        for (size_t i = 0; i < fed.size() && (i + 1) * frame_bytes <= raw.size(); ++i) {
            const uint16_t* d = reinterpret_cast<const uint16_t*>(raw.data() + i * frame_bytes);
            for (size_t k = 0; k < 512 * 424; ++k) max_abs_diff = std::max(max_abs_diff, std::abs(int(d[k]) - int(fed[i][k])));
        }
        std::printf("  max_abs_diff=%d\n", max_abs_diff);
        KV_CHECK(max_abs_diff == 0);
        removeTree(base);
    }
}

KV_TEST(png16_sequence_is_bit_exact) {
    const std::wstring base = tempBase(L"png");
    removeTree(base);
    Recorder rec;
    RecorderOptions o;
    o.base_dir = base;
    o.lossless = Lossless16::Png16Sequence;
    std::string err;
    KV_REQUIRE(rec.start(KV_MODE_IR, 512, 424, o, &err));
    const auto fed = feedRaw16(rec, false, 5);
    rec.stop();
    const RecorderStatus st = rec.status();
    KV_REQUIRE(fed.size() == 5 && st.frames_written == 5);
    Microsoft::WRL::ComPtr<IWICImagingFactory> f;
    KV_REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))));
    for (int i = 0; i < 5; ++i) {
        wchar_t name[64];
        swprintf(name, 64, L"\\ir16_%06d.png", i);
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        KV_REQUIRE(SUCCEEDED(f->CreateDecoderFromFilename((st.session_dir + name).c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &dec)));
        dec->GetFrame(0, &frame);
        WICPixelFormatGUID fmt{};
        frame->GetPixelFormat(&fmt);
        KV_CHECK(fmt == GUID_WICPixelFormat16bppGray);
        std::vector<uint16_t> px(512 * 424);
        frame->CopyPixels(nullptr, 512 * 2, static_cast<UINT>(px.size() * 2), reinterpret_cast<BYTE*>(px.data()));
        KV_CHECK(px == fed[i]);
    }
    removeTree(base);
}

// Colour is live only. Colour and every Side-by-Side layout are refused with the exact
// sentence, and nothing is created: not a session folder, not even the recordings folder.
// The viewer refuses the recordings folder for the camera roll by its path alone (a test's --camera-roll-dir,
// the PC's "Change..."), from kv_viewer_model, which can't link the recorder: the two must name the same folder.
KV_TEST(the_viewer_knows_the_recordings_folder_the_recorder_uses) {
    KV_CHECK(kvwin::knownRecordingsDir() == kvrec::defaultRecordingsDir());
}

KV_TEST(colour_and_side_by_side_are_refused_and_nothing_is_written) {
    KV_CHECK(Recorder::canRecord(KV_MODE_IR));
    KV_CHECK(Recorder::canRecord(KV_MODE_DEPTH));
    KV_CHECK(!Recorder::canRecord(KV_MODE_COLOR));
    KV_CHECK(!Recorder::canRecord(KV_MODE_SIDE_BY_SIDE));
    KV_CHECK(!Recorder::canRecord(static_cast<kv_view_mode_t>(4))); // the 3D mode
    // The viewer's rule (what greys out Record and what the phone is told) is the recorder's.
    for (int m = 0; m <= 4; ++m) {
        const auto mode = static_cast<kv_view_mode_t>(m);
        KV_CHECK(Recorder::canRecord(mode) == kvwin::recordingAllowedInMode(mode));
    }
    const std::wstring base = tempBase(L"colour");
    removeTree(base);
    for (const kv_view_mode_t m : {KV_MODE_COLOR, KV_MODE_SIDE_BY_SIDE}) {
        for (const auto lossless : {Lossless16::Ffv1Mkv, Lossless16::Png16Sequence}) {
            Recorder rec;
            RecorderOptions o;
            o.base_dir = base;
            o.lossless = lossless;
            std::string err;
            KV_CHECK(!rec.start(m, 1920, 1080, o, &err));
            KV_CHECK_MSG(err == "Colour is live only. Switch to IR or Depth to record.", err);
            KV_CHECK(!rec.isRecording());
            KV_CHECK(!rec.pushRaw16(nullptr, 0, 0, 0, 0, 0));
        }
    }
    KV_CHECK(GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES); // not even the folder
}
// Where ffmpeg is found. %KV_FFMPEG% only as a full path to a file, then the PATH entries that are full paths,
// in order, then winget's Links folder. Never a relative entry: "." (the current folder), "bin", an unexpanded %VAR%.
KV_TEST(ffmpeg_is_found_only_through_full_paths) {
    std::set<std::wstring> files;
    auto isFile = [&](const std::wstring& p) { return files.count(p) > 0; };
    const std::wstring path = L".;bin\\ff;;%TOOLS%;\"C:\\Tools\\ff\";C:\\Other\\";
    const std::wstring local = L"C:\\Users\\someone\\AppData\\Local";
    const std::wstring links = local + L"\\Microsoft\\WinGet\\Links\\ffmpeg.exe";
    files = {L".\\ffmpeg.exe", L"bin\\ff\\ffmpeg.exe", L"C:\\Tools\\ff\\ffmpeg.exe", L"C:\\Other\\ffmpeg.exe", links};
    KV_CHECK(resolveFfmpegFrom(L"", path, local, isFile) == L"C:\\Tools\\ff\\ffmpeg.exe"); // the first full-path entry
    files.insert(L"D:\\ff\\ffmpeg.exe");
    KV_CHECK(resolveFfmpegFrom(L"D:\\ff\\ffmpeg.exe", path, local, isFile) == L"D:\\ff\\ffmpeg.exe"); // the setting first
    KV_CHECK(resolveFfmpegFrom(L"ffmpeg.exe", path, local, isFile) == L"C:\\Tools\\ff\\ffmpeg.exe");  // not a full path
    KV_CHECK(resolveFfmpegFrom(L"D:\\gone\\ffmpeg.exe", path, local, isFile) == L"C:\\Tools\\ff\\ffmpeg.exe"); // no file
    KV_CHECK(resolveFfmpegFrom(L"", L".;bin;%TOOLS%", local, isFile) == links); // relative entries only: the Links folder
    files = {L".\\ffmpeg.exe", L"bin\\ffmpeg.exe"};
    KV_CHECK(resolveFfmpegFrom(L"", L".;bin", local, isFile).empty()); // and never the current folder's
    KV_CHECK(resolveFfmpegFrom(L"", L"\\\\server\\tools", L"", [](const std::wstring& p) {
                 return p == L"\\\\server\\tools\\ffmpeg.exe";
             }) == L"\\\\server\\tools\\ffmpeg.exe");
}

// A planted ffmpeg.exe in the current folder is never the one found, even with "." first on PATH. (SearchPathW
// looked in the application's folder and then the current folder before PATH.)
KV_TEST(a_planted_ffmpeg_in_the_current_folder_is_never_found) {
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"kv-planted-ffmpeg-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring planted = dir + L"\\ffmpeg.exe";
    { std::ofstream(planted) << "not ffmpeg"; }
    wchar_t cwd[MAX_PATH] = {0};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    std::vector<wchar_t> was(32768, 0);
    const DWORD n = GetEnvironmentVariableW(L"PATH", was.data(), static_cast<DWORD>(was.size()));
    const std::wstring old_path(was.data(), n);
    SetCurrentDirectoryW(dir.c_str());
    SetEnvironmentVariableW(L"PATH", (L".;" + old_path).c_str());
    const std::wstring found = resolveFfmpeg();
    SetEnvironmentVariableW(L"PATH", old_path.c_str());
    SetCurrentDirectoryW(cwd);
    KV_CHECK(found.find(L"kv-planted-ffmpeg-") == std::wstring::npos && found != L".\\ffmpeg.exe");
    DeleteFileW(planted.c_str());
    RemoveDirectoryW(dir.c_str());
}

// A child started with startChildWithOnly gets its three handles and nothing else of this process. The probe: a
// pipe whose write end is inheritable but not given to the child. With this process's copy closed, a read on the pipe
// ends at once, unless a child holds a copy: then it ends only when that child does. The control starts the same
// child the old way (inheritance on, no list, as ffmpeg was started), which must hold it, so the probe is shown to work.
KV_TEST(a_child_gets_only_the_handles_it_is_given) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    KV_REQUIRE(nul != INVALID_HANDLE_VALUE);
    wchar_t sys[MAX_PATH] = {0};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring cmd = L"\"" + std::wstring(sys) + L"\\cmd.exe\" /d /c ping -n 4 127.0.0.1 >nul"; // about 3 s
    auto heldMs = [&](bool every_handle) -> long long {
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE proc = nullptr, thread = nullptr;
        bool started = false;
        if (every_handle) {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = si.hStdOutput = si.hStdError = nul;
            PROCESS_INFORMATION pi{};
            std::wstring line = cmd;
            started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
            proc = pi.hProcess;
            thread = pi.hThread;
        } else {
            started = startChildWithOnly(cmd, nul, nul, nul, &proc, &thread, nullptr);
        }
        CloseHandle(wr); // this process's copy
        const auto t0 = std::chrono::steady_clock::now();
        char b = 0;
        DWORD got = 0;
        ReadFile(rd, &b, 1, &got, nullptr); // no writer left: at once; a child's copy: when the child ends
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CloseHandle(rd);
        if (proc) {
            WaitForSingleObject(proc, 10000);
            CloseHandle(proc);
        }
        if (thread) CloseHandle(thread);
        return started ? ms : -1;
    };
    const long long control = heldMs(true);
    const long long only = heldMs(false);
    std::printf("  the pipe was held: every handle %lld ms, only its own %lld ms\n", control, only);
    KV_CHECK_MSG(control >= 1500, "the probe didn't see the old way's inherited handle: " + std::to_string(control) + " ms");
    KV_CHECK_MSG(only >= 0 && only < 500, std::to_string(only) + " ms");
    CloseHandle(nul);
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return kvtest::runAll(argc, argv);
}
