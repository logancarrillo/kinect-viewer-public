#include "roll_ui.hpp"

#include "mf_platform.hpp" // kvhost::ensureMediaFoundation: the only way Media Foundation is started
#include "pet_camera_model.hpp"

#include <commctrl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace kvwin {

namespace {

// The player's worker to its window: a decoded frame is ready; the clip couldn't be opened or decoded.
constexpr UINT kMsgFrame = WM_APP + 1;
constexpr UINT kMsgFailed = WM_APP + 2;
constexpr int kThumbW = 96, kThumbH = 54; // the browser's thumbnails (16:9, the colour clips' shape)

enum : int {
    kPlayerPlay = 1, kPlayerScrub, kPlayerClose,
    kBrDays = 10, kBrClips, kBrPlay, kBrStar, kBrDelete, kBrRefresh,
    kSchedList = 30, kSchedAdd, kSchedRemove, kSchedClose, kSchedStart, kSchedMinutes, kSchedDay0, // ... kSchedDay0 + 6
};

std::wstring widenU8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string hrText(const char* what, HRESULT hr) {
    char b[160];
    std::snprintf(b, sizeof(b), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return b;
}

// Each window keeps its owner object in GWLP_USERDATA, set from CreateWindowEx's parameter.
template <typename T>
T* ownerOf(HWND hwnd, UINT msg, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK PlayerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto* p = ownerOf<RollPlayer>(hwnd, msg, lp)) return p->proc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK BrowserProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto* u = ownerOf<RollUi>(hwnd, msg, lp)) return u->browserProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK RollScheduleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto* u = ownerOf<RollUi>(hwnd, msg, lp)) return u->scheduleProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerClass(HINSTANCE inst, const wchar_t* name, WNDPROC proc, HBRUSH bg) {
    WNDCLASSEXW wc{sizeof(wc)};
    if (GetClassInfoExW(inst, name, &wc)) return;
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = bg;
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

// A clip's .jpg thumbnail (from the roll's own folder) as a kThumbW x kThumbH bitmap for the browser's list,
// fitted and centred on black. Decoded with WIC on the UI thread; nullptr when it can't be read.
HBITMAP thumbnailBitmap(const std::wstring& path) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT sw = 0, sh = 0;
    if (FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0) return nullptr;
    const double k = std::min(static_cast<double>(kThumbW) / sw, static_cast<double>(kThumbH) / sh);
    const UINT dw = std::max<UINT>(1, static_cast<UINT>(sw * k)), dh = std::max<UINT>(1, static_cast<UINT>(sh * k));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), dw, dh, WICBitmapInterpolationModeFant)) ||
        FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    std::vector<uint8_t> px(static_cast<size_t>(dw) * dh * 4);
    if (FAILED(conv->CopyPixels(nullptr, dw * 4, static_cast<UINT>(px.size()), px.data()))) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kThumbW;
    bi.bmiHeader.biHeight = -kThumbH; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) return nullptr;
    auto* out = static_cast<uint32_t*>(bits);
    std::fill(out, out + kThumbW * kThumbH, 0xFF000000u); // opaque black around the picture
    const UINT x0 = (kThumbW - dw) / 2, y0 = (kThumbH - dh) / 2;
    for (UINT y = 0; y < dh; ++y) {
        std::memcpy(out + (y0 + y) * kThumbW + x0, px.data() + static_cast<size_t>(y) * dw * 4, static_cast<size_t>(dw) * 4);
    }
    return hb;
}

// "14:03:22" from a sidecar's startLocal ("2026-11-18T14:03:22-05:00").
std::wstring clipTimeText(const kvhost::roll::RollClipItem& c) {
    if (c.startLocal.size() >= 19) return widenU8(c.startLocal.substr(11, 8));
    return widenU8(c.clipId);
}

} // namespace

// ==== the player ==================================================================================================

// The decoding half, on a thread of its own. The UI thread only asks (play, pause, a seek) and presents what
// the worker hands over; it never waits on a decode, so the viewer's loop (stepView, the live frames to the host)
// keeps running while a clip plays or the owner drags the scrub bar.
struct RollPlayer::Worker {
    std::wstring path;
    HWND notify = nullptr; // the player window: kMsgFrame and kMsgFailed
    std::thread th;
    std::atomic<bool> frame_posted{false};

    // Shared with the UI thread, under m.
    mutable std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    bool playing = true;          // plays from the first frame
    bool restart_clock = true;    // play() or a seek: the play clock starts again from the frame shown
    std::optional<int64_t> seek_to; // the latest position asked for; the ones before it are dropped
    bool seeking = false;         // a seek is being decoded
    uint64_t seeks_done = 0;
    std::vector<uint8_t> ready;   // the newest decoded frame, not presented yet (swapped, never copied)
    bool ready_fresh = false;
    int64_t ready_pts = 0;
    uint32_t w = 0, h = 0;        // the picture (the frames' size)
    int64_t duration = 0;
    uint64_t decoded = 0;
    bool at_end = false;
    bool failed = false;
    std::string error;

    // The worker's own.
    ComPtr<IMFSourceReader> reader;
    uint32_t out_w = 0, out_h = 0; // the decoder's output size (may be padded past the picture)
    LONG stride = 0;               // of the output type; negative means bottom-up
    std::vector<uint8_t> back;     // the frame being decoded into
    int64_t pts = 0;               // of the last frame decoded
    uint64_t play_start_tick = 0;
    int64_t play_start_pts = 0;

    // The output type the source reader's video processing gives (RGB32), and its stride.
    bool readOutputType(std::string* err) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &t);
        if (FAILED(hr)) {
            if (err) *err = hrText("GetCurrentMediaType", hr);
            return false;
        }
        UINT32 ow = 0, oh = 0;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &ow, &oh);
        out_w = ow;
        out_h = oh;
        UINT32 s = 0;
        if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) {
            stride = static_cast<LONG>(s);
        } else {
            LONG computed = 0;
            stride = SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, ow, &computed)) ? computed
                                                                                                       : static_cast<LONG>(ow * 4);
        }
        return out_w > 0 && out_h > 0;
    }

    bool open(std::string* err) {
        // Media Foundation only through the host's helper: started once, never shut down (mf_platform.cpp).
        if (!kvhost::ensureMediaFoundation(err)) return false;
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes(&attrs, 1);
        if (SUCCEEDED(hr)) hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE); // H.264 to RGB32
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader);
        if (FAILED(hr)) {
            if (err) *err = hrText("opening the clip", hr);
            return false;
        }
        IMFSourceReader* r = reader.Get();
        r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        uint32_t pw = 0, ph = 0;
        ComPtr<IMFMediaType> native;
        if (SUCCEEDED(r->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) {
            UINT32 nw = 0, nh = 0;
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
            pw = nw;
            ph = nh;
        }
        ComPtr<IMFMediaType> out;
        hr = MFCreateMediaType(&out);
        if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) hr = r->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out.Get());
        if (FAILED(hr) || !readOutputType(err)) {
            if (err && err->empty()) *err = hrText("decoding the clip to RGB32", hr);
            return false;
        }
        if (pw == 0 || ph == 0 || pw > out_w || ph > out_h) {
            pw = out_w;
            ph = out_h;
        }
        PROPVARIANT v;
        PropVariantInit(&v);
        int64_t dur = 0;
        if (SUCCEEDED(r->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)) &&
            v.vt == VT_UI8) {
            dur = static_cast<int64_t>(v.uhVal.QuadPart);
        }
        PropVariantClear(&v);
        {
            std::lock_guard<std::mutex> lk(m);
            w = pw;
            h = ph;
            duration = dur;
        }
        back.assign(static_cast<size_t>(pw) * ph * 4, 0);
        return true;
    }

    // Decodes the next frame into back; false at the end or on an error (error set then).
    bool readFrame() {
        ComPtr<IMFSample> sample;
        LONGLONG ts = 0;
        for (int guard = 0; guard < 256 && !sample; ++guard) {
            DWORD flags = 0;
            const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags,
                                                  &ts, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                setEnd(hrText("decoding the clip", hr));
                return false;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                std::string e;
                if (!readOutputType(&e)) {
                    setEnd(e);
                    return false;
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                setEnd(std::string());
                return false;
            }
        }
        if (!sample) return false;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
        uint32_t pw = 0, ph = 0;
        {
            std::lock_guard<std::mutex> lk(m);
            pw = w;
            ph = h;
        }
        back.resize(static_cast<size_t>(pw) * ph * 4);
        const uint32_t cw = std::min(pw, out_w), ch = std::min(ph, out_h);
        const size_t row = static_cast<size_t>(cw) * 4;
        bool copied = false;
        ComPtr<IMF2DBuffer> b2;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
            if (static_cast<size_t>(pitch < 0 ? -pitch : pitch) >= row) {
                for (uint32_t y = 0; y < ch; ++y) {
                    std::memcpy(back.data() + y * static_cast<size_t>(pw) * 4, scan0 + static_cast<ptrdiff_t>(y) * pitch, row);
                }
                copied = true;
            }
            b2->Unlock2D();
        } else {
            BYTE* p = nullptr;
            DWORD max_len = 0, len = 0;
            if (SUCCEEDED(buf->Lock(&p, &max_len, &len))) {
                pitch = stride;
                const size_t apitch = static_cast<size_t>(pitch < 0 ? -pitch : pitch);
                if (apitch >= row && len >= apitch * out_h) {
                    scan0 = pitch < 0 ? p + apitch * (out_h - 1) : p;
                    for (uint32_t y = 0; y < ch; ++y) {
                        std::memcpy(back.data() + y * static_cast<size_t>(pw) * 4, scan0 + static_cast<ptrdiff_t>(y) * pitch, row);
                    }
                    copied = true;
                }
                buf->Unlock();
            }
        }
        if (!copied) {
            setEnd("a decoded frame had an unexpected layout");
            return false;
        }
        pts = ts;
        std::lock_guard<std::mutex> lk(m);
        if (pts > duration) duration = pts; // grows as it plays (an fMP4 without its finishing data)
        ++decoded;
        return true;
    }

    void setEnd(const std::string& why) {
        std::lock_guard<std::mutex> lk(m);
        at_end = true;
        playing = false; // stops on the last frame
        if (!why.empty()) error = why;
    }

    // Hands the frame in back to the UI thread (a swap) and wakes it once, however many frames come meanwhile.
    void publish() {
        {
            std::lock_guard<std::mutex> lk(m);
            std::swap(back, ready);
            ready_fresh = true;
            ready_pts = pts;
        }
        if (!frame_posted.exchange(true) && !PostMessageW(notify, kMsgFrame, 0, 0)) frame_posted = false;
    }

    bool newerSeekOrStop() {
        std::lock_guard<std::mutex> lk(m);
        return stop || seek_to.has_value();
    }

    // The keyframe at or before target (shown at once), then forward to the frame there. A newer seek asked for
    // meanwhile takes over: only the latest position is decoded to.
    void doSeek(int64_t target) {
        int64_t dur = 0;
        {
            std::lock_guard<std::mutex> lk(m);
            dur = duration;
        }
        target = std::clamp<int64_t>(target, 0, std::max<int64_t>(0, dur));
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_I8;
        v.hVal.QuadPart = target;
        const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, v);
        PropVariantClear(&v);
        bool complete = true;
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lk(m);
            error = hrText("seeking", hr);
        } else {
            {
                std::lock_guard<std::mutex> lk(m);
                at_end = false;
            }
            bool first = true;
            for (int i = 0; i < 1200; ++i) {
                if (!readFrame()) break;
                if (first) publish(); // the keyframe: something to see while the rest decodes
                first = false;
                if (pts + 166667 > target) break; // within a sixtieth of a second
                if (newerSeekOrStop()) {
                    complete = false;
                    break;
                }
            }
            if (!first) publish();
        }
        std::lock_guard<std::mutex> lk(m);
        if (complete) ++seeks_done;
        seeking = seek_to.has_value();
        restart_clock = true;
    }

    void run() {
        // COM for Media Foundation's source reader on this thread (multithreaded; released when it ends).
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string err;
        if (!open(&err) || !readFrame()) {
            std::lock_guard<std::mutex> lk(m);
            failed = true;
            error = !err.empty() ? err : (error.empty() ? std::string("the clip has no frame to show") : error);
        }
        if (failed) {
            PostMessageW(notify, kMsgFailed, 0, 0);
        } else {
            publish();
            for (;;) {
                std::optional<int64_t> target;
                bool play = false, restart = false;
                {
                    std::unique_lock<std::mutex> lk(m);
                    const auto wait = playing && !at_end ? std::chrono::milliseconds(5) : std::chrono::milliseconds(500);
                    cv.wait_for(lk, wait, [&] { return stop || seek_to.has_value(); });
                    if (stop) break;
                    target = seek_to;
                    seek_to.reset();
                    if (target) seeking = true;
                    play = playing && !at_end;
                    restart = restart_clock;
                    restart_clock = false;
                }
                if (target) {
                    doSeek(*target);
                    continue;
                }
                if (restart) {
                    play_start_tick = GetTickCount64();
                    play_start_pts = pts;
                }
                if (!play) continue;
                const int64_t due = play_start_pts + static_cast<int64_t>(GetTickCount64() - play_start_tick) * 10000;
                bool any = false;
                for (int i = 0; i < 8 && pts < due && !newerSeekOrStop(); ++i) {
                    if (!readFrame()) break;
                    any = true;
                }
                if (any) publish();
            }
        }
        reader.Reset();
        std::fill(back.begin(), back.end(), static_cast<uint8_t>(0)); // nothing of the clip stays in memory
        if (SUCCEEDED(co)) CoUninitialize();
    }
};

RollPlayer::RollPlayer() = default;

RollPlayer::~RollPlayer() {
    close();
    stopWorker();
}

bool RollPlayer::open(HINSTANCE inst, HWND owner, HFONT font, int dpi, const std::wstring& mp4_path,
                      const std::wstring& title, std::string* err) {
    close();
    last_error_.clear();
    font_ = font;
    dpi_ = dpi;
    have_frame_ = false;
    shown_pts_ = 0;
    width_ = height_ = 0;
    frames_presented_ = 0;
    seeks_requested_ = 0;
    registerClass(inst, L"KinectViewerRollPlayer", PlayerProc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int cw = S(800), ch = S(500);
    wnd_ = CreateWindowExW(0, L"KinectViewerRollPlayer", title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           orc.left + S(300), orc.top + S(80), cw, ch, owner, nullptr, inst, this);
    if (!wnd_) {
        if (err) *err = "the player window couldn't open";
        return false;
    }
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, wnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return c;
    };
    btn_play_ = make(L"BUTTON", L"Pause", BS_PUSHBUTTON | WS_TABSTOP, kPlayerPlay);
    scrub_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, kPlayerScrub);
    SendMessageW(scrub_, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(scrub_, TBM_SETRANGEMAX, TRUE, 1000);
    lbl_time_ = make(L"STATIC", L"", SS_CENTERIMAGE | SS_RIGHT, 0);
    make(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, kPlayerClose);
    SendMessageW(wnd_, WM_SIZE, 0, 0);
    ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    // The clip is opened and decoded on the worker; its first frame arrives as kMsgFrame.
    worker_ = std::make_unique<Worker>();
    worker_->path = mp4_path;
    worker_->notify = wnd_;
    Worker* w = worker_.get();
    worker_->th = std::thread([w]() { w->run(); });
    updateControls();
    return true;
}

void RollPlayer::close() {
    if (wnd_) DestroyWindow(wnd_); // WM_DESTROY stops the worker
    stopWorker();
}

void RollPlayer::stopWorker() {
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lk(worker_->m);
        worker_->stop = true;
    }
    worker_->cv.notify_all();
    if (worker_->th.joinable()) worker_->th.join();
    {
        std::lock_guard<std::mutex> lk(worker_->m);
        std::fill(worker_->ready.begin(), worker_->ready.end(), static_cast<uint8_t>(0));
    }
    worker_.reset();
}

void RollPlayer::present() {
    if (!worker_) return;
    worker_->frame_posted = false; // a frame from now on posts again
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lk(worker_->m);
        if (worker_->ready_fresh) {
            std::swap(worker_->ready, frame_);
            worker_->ready_fresh = false;
            width_ = worker_->w;
            height_ = worker_->h;
            shown_pts_ = worker_->ready_pts;
            fresh = true;
        }
    }
    if (fresh) {
        have_frame_ = true;
        ++frames_presented_;
        if (wnd_) InvalidateRect(wnd_, nullptr, FALSE);
    }
    updateControls();
}

bool RollPlayer::playing() const {
    if (!worker_) return false;
    std::lock_guard<std::mutex> lk(worker_->m);
    return worker_->playing;
}

void RollPlayer::play() {
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lk(worker_->m);
        if (worker_->at_end && !worker_->seek_to) {
            worker_->seek_to = 0;
            worker_->seeking = true;
        }
        worker_->playing = true;
        worker_->restart_clock = true;
    }
    worker_->cv.notify_all();
    updateControls();
}

void RollPlayer::pause() {
    if (!worker_) return;
    {
        std::lock_guard<std::mutex> lk(worker_->m);
        worker_->playing = false;
    }
    updateControls();
}

bool RollPlayer::seek(int64_t pos_hns) {
    if (!worker_) return false;
    {
        std::lock_guard<std::mutex> lk(worker_->m);
        if (worker_->failed) return false;
        worker_->seek_to = std::max<int64_t>(0, pos_hns); // replaces one not started yet (coalesced)
        worker_->seeking = true;
    }
    ++seeks_requested_;
    worker_->cv.notify_all();
    return true;
}

bool RollPlayer::seekPending() const {
    if (!worker_) return false;
    std::lock_guard<std::mutex> lk(worker_->m);
    return worker_->seeking || worker_->seek_to.has_value();
}

uint64_t RollPlayer::seeksCompleted() const {
    if (!worker_) return 0;
    std::lock_guard<std::mutex> lk(worker_->m);
    return worker_->seeks_done;
}

int64_t RollPlayer::durationHns() const {
    if (!worker_) return 0;
    std::lock_guard<std::mutex> lk(worker_->m);
    return worker_->duration;
}

uint64_t RollPlayer::framesDecoded() const {
    if (!worker_) return 0;
    std::lock_guard<std::mutex> lk(worker_->m);
    return worker_->decoded;
}

std::string RollPlayer::lastError() const {
    if (!worker_) return last_error_;
    std::lock_guard<std::mutex> lk(worker_->m);
    return worker_->error;
}

void RollPlayer::updateControls() {
    if (!wnd_) return;
    SetWindowTextW(btn_play_, playing() ? L"Pause" : L"Play");
    const int64_t dur = durationHns();
    const int pos = dur > 0 ? static_cast<int>(shown_pts_ * 1000 / dur) : 0;
    if (GetCapture() != scrub_) SendMessageW(scrub_, TBM_SETPOS, TRUE, pos);
    const std::wstring t = clipDurationText(static_cast<double>(shown_pts_) / 1e7) + L" / " +
                           clipDurationText(static_cast<double>(dur) / 1e7);
    wchar_t cur[64] = {0};
    GetWindowTextW(lbl_time_, cur, 64);
    if (t != cur) SetWindowTextW(lbl_time_, t.c_str());
}

void RollPlayer::paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int bar = S(40);
    RECT area{0, 0, cr.right, std::max<LONG>(0, cr.bottom - bar)};
    if (!have_frame_ || width_ == 0 || height_ == 0 || frame_.size() < static_cast<size_t>(width_) * height_ * 4) {
        FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    } else {
        const double k = std::min(static_cast<double>(area.right) / width_, static_cast<double>(area.bottom) / height_);
        const int dw = static_cast<int>(width_ * k), dh = static_cast<int>(height_ * k);
        const int x = (area.right - dw) / 2, y = (area.bottom - dh) / 2;
        // Black around the picture only, so the picture itself never flickers.
        RECT bands[4] = {{0, 0, area.right, y}, {0, y + dh, area.right, area.bottom}, {0, y, x, y + dh}, {x + dw, y, area.right, y + dh}};
        for (const RECT& b : bands) {
            if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(width_);
        bi.bmiHeader.biHeight = -static_cast<LONG>(height_); // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, x, y, dw, dh, 0, 0, static_cast<int>(width_), static_cast<int>(height_), frame_.data(), &bi,
                      DIB_RGB_COLORS, SRCCOPY);
    }
    RECT strip{0, area.bottom, cr.right, cr.bottom};
    FillRect(dc, &strip, GetSysColorBrush(COLOR_BTNFACE));
    EndPaint(hwnd, &ps);
}

LRESULT RollPlayer::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(hwnd); return 0;
        case WM_SIZE: {
            RECT cr{};
            GetClientRect(hwnd, &cr);
            const int y = cr.bottom - S(34);
            if (btn_play_) MoveWindow(btn_play_, S(8), y, S(70), S(26), TRUE);
            if (scrub_) MoveWindow(scrub_, S(84), y, std::max<int>(S(40), cr.right - S(84) - S(200)), S(26), TRUE);
            if (lbl_time_) MoveWindow(lbl_time_, cr.right - S(196), y, S(110), S(26), TRUE);
            if (HWND c = GetDlgItem(hwnd, kPlayerClose)) MoveWindow(c, cr.right - S(78), y, S(70), S(26), TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case kMsgFrame: present(); return 0;
        case kMsgFailed:
            last_error_ = lastError();
            if (onFailed) onFailed(last_error_);
            DestroyWindow(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kPlayerPlay) {
                if (playing()) pause();
                else play();
            } else if (LOWORD(wp) == kPlayerClose) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_HSCROLL:
            // A drag sends many of these; each only asks the worker for the position (the latest wins).
            if (reinterpret_cast<HWND>(lp) == scrub_) {
                const int code = LOWORD(wp);
                if (code == TB_THUMBTRACK || code == TB_PAGEUP || code == TB_PAGEDOWN || code == TB_LINEUP ||
                    code == TB_LINEDOWN || code == TB_THUMBPOSITION) {
                    const int pos = code == TB_THUMBTRACK || code == TB_THUMBPOSITION
                                        ? static_cast<int>(static_cast<short>(HIWORD(wp)))
                                        : static_cast<int>(SendMessageW(scrub_, TBM_GETPOS, 0, 0));
                    seek(durationHns() * std::clamp(pos, 0, 1000) / 1000);
                }
            }
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            if (worker_) {
                const std::string e = lastError();
                if (!e.empty()) last_error_ = e;
            }
            stopWorker();
            wnd_ = nullptr;
            btn_play_ = scrub_ = lbl_time_ = nullptr;
            have_frame_ = false;
            std::fill(frame_.begin(), frame_.end(), static_cast<uint8_t>(0)); // nothing of the clip stays in memory
            if (onClosed) onClosed();
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// ==== the browser and the schedule editor ====================================================================

RollUi::RollUi() = default;

RollUi::~RollUi() = default;

void RollUi::init(const Env& env) {
    env_ = env;
    player_.onClosed = [this]() {
        releasePlayerPin(); // the player let go of the clip
        playing_clip_.clear();
        changed();
    };
    player_.onFailed = [this](const std::string& err) {
        last_error_ = err;
        notice_ = L"The clip can't be played: " + widenU8(err);
        if (br_info_) SetWindowTextW(br_info_, notice_.c_str());
    };
}

void RollUi::releasePlayerPin() {
    if (player_pin_ && env_.hooks) env_.hooks->unpinRollClipForPc(player_pin_);
    player_pin_ = 0;
}

bool RollUi::closePlayerForDeletedClip(const std::string& clipId) {
    if (clipId.empty() || playing_clip_ != clipId || !player_.isOpen()) return false;
    player_.close(); // unpins (onClosed), so the host's delete can go ahead
    notice_ = L"This clip was deleted.";
    if (br_info_) SetWindowTextW(br_info_, notice_.c_str());
    return true;
}

void RollUi::closeAll() {
    player_.close();
    if (sched_wnd_) DestroyWindow(sched_wnd_);
    if (br_wnd_) DestroyWindow(br_wnd_);
}

void RollUi::changed() {
    if (env_.on_changed) env_.on_changed();
}

HWND RollUi::child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), env_.inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(env_.font), TRUE);
    return c;
}

void RollUi::openBrowser() {
    if (br_wnd_) {
        refreshBrowser();
        SetForegroundWindow(br_wnd_);
        return;
    }
    registerClass(env_.inst, L"KinectViewerRollBrowser", BrowserProc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    RECT mr{};
    GetWindowRect(env_.main, &mr);
    br_wnd_ = CreateWindowExW(0, L"KinectViewerRollBrowser", L"Camera roll (on this PC)",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, mr.left + S(320), mr.top + S(60), S(840),
                              S(560), env_.main, nullptr, env_.inst, this);
    br_days_ = child(br_wnd_, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 10,
                     10, 210, 430, kBrDays);
    br_clips_ = child(br_wnd_, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 228,
                      10, 586, 430, kBrClips);
    ListView_SetExtendedListViewStyle(br_days_, LVS_EX_FULLROWSELECT);
    ListView_SetExtendedListViewStyle(br_clips_, LVS_EX_FULLROWSELECT);
    auto columns = [&](HWND list, std::initializer_list<std::pair<const wchar_t*, int>> cols) {
        int i = 0;
        for (const auto& c : cols) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(c.first);
            col.cx = S(c.second);
            ListView_InsertColumn(list, i++, &col);
        }
    };
    columns(br_days_, {{L"Day", 100}, {L"Clips", 46}, {L"Starred", 56}});
    columns(br_clips_, {{L"Time", 180}, {L"Trigger", 70}, {L"Length", 60}, {L"Star", 44}, {L"Started by", 120}, {L"Size", 80}});
    br_images_ = ImageList_Create(kThumbW, kThumbH, ILC_COLOR32, 8, 8);
    ListView_SetImageList(br_clips_, br_images_, LVSIL_SMALL);
    br_play_ = child(br_wnd_, L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP, 228, 448, 90, 26, kBrPlay);
    br_star_ = child(br_wnd_, L"BUTTON", L"Star", BS_PUSHBUTTON | WS_TABSTOP, 324, 448, 90, 26, kBrStar);
    br_delete_ = child(br_wnd_, L"BUTTON", L"Delete…", BS_PUSHBUTTON | WS_TABSTOP, 420, 448, 90, 26, kBrDelete);
    br_refresh_ = child(br_wnd_, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 10, 448, 90, 26, kBrRefresh);
    br_info_ = child(br_wnd_, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 10, 482, 804, 40, 0);
    refreshBrowser();
    ShowWindow(br_wnd_, SW_SHOWNOACTIVATE);
    changed();
}

void RollUi::closeBrowser() {
    if (br_wnd_) DestroyWindow(br_wnd_);
}

int RollUi::selectedDay() const { return br_days_ ? ListView_GetNextItem(br_days_, -1, LVNI_SELECTED) : -1; }

int RollUi::selectedClip() const { return br_clips_ ? ListView_GetNextItem(br_clips_, -1, LVNI_SELECTED) : -1; }

void RollUi::refreshBrowser() {
    if (!br_wnd_) return;
    loadDays();
    loadClips();
}

void RollUi::loadDays() {
    const int was = selectedDay();
    const std::string was_day = was >= 0 && was < static_cast<int>(days_.size()) ? days_[static_cast<size_t>(was)].day
                                                                                  : shown_day_;
    days_.clear();
    ListView_DeleteAllItems(br_days_);
    const auto store = env_.hooks ? env_.hooks->rollStore() : nullptr;
    std::wstring info;
    if (!store) {
        info = L"No camera roll: " + widenU8(env_.hooks ? env_.hooks->rollError() : std::string("no streaming host"));
    } else {
        std::optional<std::string> cursor;
        for (int page = 0; page < 100; ++page) { // newest first, a page at a time (v3 §5.6)
            auto r = store->listDays(cursor, kvhost::roll::kListMaxLimit);
            if (r.error != kvhost::roll::RollError::None) break;
            days_.insert(days_.end(), r.days.begin(), r.days.end());
            if (!r.nextCursor) break;
            cursor = r.nextCursor;
        }
        // The host's cached counts; the folder isn't walked for this on the UI thread.
        info = L"Folder: " + env_.hooks->rollDir();
        if (const auto st = env_.hooks->rollStorageSnapshot()) {
            info += L"\n" + rollStorageText(st->usedBytes, st->capBytes, st->freeBytes, st->floorBytes) + L" Starred clips are kept.";
        }
    }
    int select = days_.empty() ? -1 : 0;
    for (size_t i = 0; i < days_.size(); ++i) {
        const auto& d = days_[i];
        std::wstring cols[3] = {widenU8(d.day), std::to_wstring(d.clipCount), std::to_wstring(d.starredCount)};
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = static_cast<int>(i);
        lv.pszText = cols[0].data();
        ListView_InsertItem(br_days_, &lv);
        for (int c = 1; c < 3; ++c) ListView_SetItemText(br_days_, static_cast<int>(i), c, cols[c].data());
        if (!was_day.empty() && d.day == was_day) select = static_cast<int>(i);
    }
    if (select >= 0) ListView_SetItemState(br_days_, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SetWindowTextW(br_info_, info.c_str());
}

void RollUi::loadClips() {
    const int was = selectedClip();
    const std::string was_id = was >= 0 && was < static_cast<int>(clips_.size()) ? clips_[static_cast<size_t>(was)].clipId
                                                                                  : std::string();
    clips_.clear();
    ListView_DeleteAllItems(br_clips_);
    ImageList_RemoveAll(br_images_);
    thumbs_shown_ = 0;
    const int day = selectedDay();
    const auto store = env_.hooks ? env_.hooks->rollStore() : nullptr;
    shown_day_ = day >= 0 && day < static_cast<int>(days_.size()) ? days_[static_cast<size_t>(day)].day : std::string();
    if (store && !shown_day_.empty()) {
        std::optional<std::string> cursor;
        for (int page = 0; page < 100; ++page) {
            auto r = store->listClips(shown_day_, cursor, kvhost::roll::kListMaxLimit, kvhost::roll::kPcDeviceId);
            if (r.error != kvhost::roll::RollError::None) break;
            clips_.insert(clips_.end(), r.clips.begin(), r.clips.end());
            if (!r.nextCursor) break;
            cursor = r.nextCursor;
        }
    }
    int select = -1;
    for (size_t i = 0; i < clips_.size(); ++i) {
        const auto& c = clips_[i];
        int image = I_IMAGENONE;
        if (c.hasThumbnail && store) {
            if (const auto path = store->thumbnailPath(c.clipId)) {
                if (HBITMAP hb = thumbnailBitmap(*path)) {
                    image = ImageList_Add(br_images_, hb, nullptr);
                    DeleteObject(hb);
                    if (image >= 0) ++thumbs_shown_;
                    else image = I_IMAGENONE;
                }
            }
        }
        std::wstring cols[6] = {clipTimeText(c), clipTriggerLabel(c.trigger), clipDurationText(c.durationSec),
                                c.starred ? L"★" : L"", c.startedBy ? widenU8(*c.startedBy) : std::wstring(), bytesText(c.bytes)};
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_IMAGE;
        lv.iItem = static_cast<int>(i);
        lv.iImage = image;
        lv.pszText = cols[0].data();
        ListView_InsertItem(br_clips_, &lv);
        for (int k = 1; k < 6; ++k) ListView_SetItemText(br_clips_, static_cast<int>(i), k, cols[k].data());
        if (!was_id.empty() && c.clipId == was_id) select = static_cast<int>(i);
    }
    if (select < 0 && !clips_.empty()) select = 0;
    if (select >= 0) ListView_SetItemState(br_clips_, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    updateBrowserButtons();
    changed(); // thumbnails on screen or not: the screenshot rule looks again
}

void RollUi::updateBrowserButtons() {
    if (!br_wnd_) return;
    const int sel = selectedClip();
    const bool any = sel >= 0 && sel < static_cast<int>(clips_.size());
    for (HWND h : {br_play_, br_star_, br_delete_}) EnableWindow(h, any);
    SetWindowTextW(br_star_, any && clips_[static_cast<size_t>(sel)].starred ? L"Unstar" : L"Star");
}

bool RollUi::selectDay(int index) {
    if (!br_wnd_ || index < 0 || index >= static_cast<int>(days_.size())) return false;
    ListView_SetItemState(br_days_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    loadClips();
    return true;
}

bool RollUi::selectClip(const std::string& clipId) {
    for (size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i].clipId == clipId) {
            ListView_SetItemState(br_clips_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            updateBrowserButtons();
            return true;
        }
    }
    return false;
}

bool RollUi::starSelected() {
    last_error_.clear();
    const int sel = selectedClip();
    if (!env_.hooks || sel < 0 || sel >= static_cast<int>(clips_.size())) return false;
    const auto& c = clips_[static_cast<size_t>(sel)];
    // The PC stars as an owner; its unstar clears every star on the clip (pet-camera.md §6.3).
    const auto e = env_.hooks->setRollClipStar(c.clipId, !c.starred);
    if (e != kvhost::roll::RollError::None) last_error_ = e == kvhost::roll::RollError::NotFound ? "not_found" : "internal";
    loadDays();
    loadClips();
    changed();
    return last_error_.empty();
}

bool RollUi::deleteSelected(bool ask) {
    last_error_.clear();
    const int sel = selectedClip();
    if (!env_.hooks || sel < 0 || sel >= static_cast<int>(clips_.size())) return false;
    const auto c = clips_[static_cast<size_t>(sel)];
    if (ask) {
        const std::wstring q = L"Delete the " + clipTriggerLabel(c.trigger) + L" clip of " + widenU8(shown_day_) + L" " +
                               clipTimeText(c) + (c.starred ? L"? It is starred." : L"?") +
                               L"\n\nIt is deleted from this PC for every device.";
        if (MessageBoxW(br_wnd_, q.c_str(), L"Delete clip", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return false;
    }
    if (playing_clip_ == c.clipId) player_.close(); // the PC's own player lets go of the file first
    const auto e = env_.hooks->deleteRollClip(c.clipId); // stops any device's playback of it too
    if (e != kvhost::roll::RollError::None) {
        last_error_ = e == kvhost::roll::RollError::NotFound ? "not_found" : "internal";
        if (ask) MessageBoxW(br_wnd_, L"It couldn't be deleted (is the file open?).", L"Delete clip", MB_ICONWARNING);
    }
    loadDays();
    loadClips();
    changed();
    return last_error_.empty();
}

bool RollUi::playSelected() {
    last_error_.clear();
    const int sel = selectedClip();
    const auto store = env_.hooks ? env_.hooks->rollStore() : nullptr;
    if (!store || sel < 0 || sel >= static_cast<int>(clips_.size())) return false;
    const auto c = clips_[static_cast<size_t>(sel)];
    const auto path = store->mp4Path(c.clipId);
    if (!path) {
        last_error_ = "not_found";
        return false;
    }
    // The host holds the clip while the player has it (retention skips it; a phone owner's delete tells the
    // PC first and waits for the unpin).
    const uint64_t pin = env_.hooks->pinRollClipForPc(c.clipId);
    if (pin == 0) {
        last_error_ = "not_found";
        return false;
    }
    // The player gets the file's path and nothing else (no hooks, no encoder): see roll_ui.hpp.
    const std::wstring title = L"Camera roll: " + widenU8(shown_day_) + L" " + clipTimeText(c) + L" (" +
                               clipTriggerLabel(c.trigger) + L")";
    std::string err;
    notice_.clear();
    if (!player_.open(env_.inst, env_.main, env_.font, env_.dpi, *path, title, &err)) {
        env_.hooks->unpinRollClipForPc(pin);
        last_error_ = err;
        if (br_wnd_) MessageBoxW(br_wnd_, (L"The clip can't be played: " + widenU8(err)).c_str(), L"Camera roll", MB_ICONWARNING);
        changed();
        return false;
    }
    playing_clip_ = c.clipId;
    player_pin_ = pin; // after open(): its close() of an earlier clip released that clip's pin
    changed();
    return true;
}

LRESULT RollUi::browserProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kBrPlay: playSelected(); break;
                case kBrStar: starSelected(); break;
                case kBrDelete: deleteSelected(true); break;
                case kBrRefresh: refreshBrowser(); break;
                default: break;
            }
            return 0;
        case WM_NOTIFY: {
            const auto* nm = reinterpret_cast<const NMHDR*>(lp);
            if (nm->hwndFrom == br_days_ && nm->code == LVN_ITEMCHANGED) {
                const auto* lv = reinterpret_cast<const NMLISTVIEW*>(lp);
                if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
                    const int d = lv->iItem;
                    if (d >= 0 && d < static_cast<int>(days_.size()) && days_[static_cast<size_t>(d)].day != shown_day_) loadClips();
                }
            } else if (nm->hwndFrom == br_clips_) {
                if (nm->code == LVN_ITEMCHANGED) updateBrowserButtons();
                if (nm->code == NM_DBLCLK) playSelected();
            }
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            br_wnd_ = br_days_ = br_clips_ = br_play_ = br_star_ = br_delete_ = br_refresh_ = br_info_ = nullptr;
            if (br_images_) ImageList_Destroy(br_images_);
            br_images_ = nullptr;
            thumbs_shown_ = 0;
            days_.clear();
            clips_.clear();
            shown_day_.clear();
            changed();
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- "Scheduled recording" rows ---------------------------------------------------------------------------

void RollUi::openSchedule() {
    if (sched_wnd_) {
        SetForegroundWindow(sched_wnd_);
        return;
    }
    registerClass(env_.inst, L"KinectViewerRollSchedule", RollScheduleProc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    RECT mr{};
    GetWindowRect(env_.main, &mr);
    sched_wnd_ = CreateWindowExW(0, L"KinectViewerRollSchedule", L"Scheduled recording", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 mr.left + S(340), mr.top + S(120), S(420), S(360), env_.main, nullptr, env_.inst, this);
    child(sched_wnd_, L"STATIC", L"Each row records from its start time, for its length (1-60 minutes), on its days. "
                                L"Times are this PC's clock.",
          SS_LEFT, 10, 8, 390, 32, 0);
    sched_list_ = child(sched_wnd_, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 10, 42, 390, 110, kSchedList);
    static const wchar_t* const kNames[7] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    for (int k = 1; k <= 7; ++k) { // Monday first
        const int bit = k % 7;
        sched_days_[bit] = child(sched_wnd_, L"BUTTON", kNames[bit], BS_AUTOCHECKBOX | WS_TABSTOP, 10 + (k - 1) * 56, 160, 54, 22,
                                 kSchedDay0 + bit);
        SendMessageW(sched_days_[bit], BM_SETCHECK, BST_CHECKED, 0);
    }
    child(sched_wnd_, L"STATIC", L"Start (HH:MM)", SS_LEFT | SS_CENTERIMAGE, 10, 190, 90, 24, 0);
    sched_start_ = child(sched_wnd_, L"EDIT", L"08:00", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 102, 190, 60, 24, kSchedStart);
    child(sched_wnd_, L"STATIC", L"Minutes", SS_LEFT | SS_CENTERIMAGE, 176, 190, 54, 24, 0);
    sched_minutes_ = child(sched_wnd_, L"EDIT", L"10", WS_BORDER | ES_NUMBER | WS_TABSTOP, 232, 190, 40, 24, kSchedMinutes);
    child(sched_wnd_, L"BUTTON", L"Add", BS_PUSHBUTTON | WS_TABSTOP, 10, 224, 90, 26, kSchedAdd);
    child(sched_wnd_, L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, 106, 224, 90, 26, kSchedRemove);
    child(sched_wnd_, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 310, 224, 90, 26, kSchedClose);
    sched_note_ = child(sched_wnd_, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 10, 258, 390, 54, 0);
    refreshSchedule();
    ShowWindow(sched_wnd_, SW_SHOWNOACTIVATE);
}

void RollUi::refreshSchedule() {
    if (!sched_wnd_ || !env_.hooks) return;
    sched_rows_ = env_.hooks->petSettings().recordSchedule;
    const int was = static_cast<int>(SendMessageW(sched_list_, LB_GETCURSEL, 0, 0));
    SendMessageW(sched_list_, LB_RESETCONTENT, 0, 0);
    for (const auto& r : sched_rows_) {
        const std::wstring t = recordScheduleRowText(r);
        SendMessageW(sched_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
    }
    if (was >= 0 && was < static_cast<int>(sched_rows_.size())) SendMessageW(sched_list_, LB_SETCURSEL, was, 0);
}

void RollUi::setScheduleEditorFields(uint8_t days, const std::wstring& start, const std::wstring& minutes) {
    if (!sched_wnd_) return;
    for (int b = 0; b < 7; ++b) SendMessageW(sched_days_[b], BM_SETCHECK, (days & (1u << b)) ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(sched_start_, start.c_str());
    SetWindowTextW(sched_minutes_, minutes.c_str());
}

bool RollUi::selectScheduleRow(int index) {
    if (!sched_wnd_ || index < 0 || index >= static_cast<int>(sched_rows_.size())) return false;
    SendMessageW(sched_list_, LB_SETCURSEL, index, 0);
    return true;
}

bool RollUi::addScheduleRowFromEditor() {
    if (!sched_wnd_ || !env_.hooks) return false;
    uint8_t days = 0;
    for (int b = 0; b < 7; ++b) {
        if (SendMessageW(sched_days_[b], BM_GETCHECK, 0, 0) == BST_CHECKED) days |= static_cast<uint8_t>(1u << b);
    }
    wchar_t start[32] = {0}, mins[16] = {0};
    GetWindowTextW(sched_start_, start, 32);
    GetWindowTextW(sched_minutes_, mins, 16);
    const auto at = parseClockText(start);
    const int length = _wtoi(mins);
    std::wstring why;
    if (days == 0) why = L"Pick at least one day.";
    else if (!at) why = L"The start is a time like 08:00.";
    else if (length < 1 || length > 60) why = L"The length is 1 to 60 minutes.";
    else if (sched_rows_.size() >= kvhost::kMaxRecordScheduleRows) why = L"That is as many rows as the schedule holds.";
    if (why.empty()) {
        kvhost::RecordScheduleRow r;
        r.days = days;
        r.startMinute = *at;
        r.lengthMinutes = static_cast<uint16_t>(length);
        auto rows = env_.hooks->petSettings().recordSchedule;
        rows.push_back(r);
        if (!env_.hooks->setRecordSchedule(rows, env_.hooks->petSettings().recordScheduleEnabled)) {
            const std::string e = env_.hooks->petSaveError();
            why = L"Not saved: " + (e.empty() ? std::wstring(L"the row isn't valid.") : widenU8(e));
        }
    }
    SetWindowTextW(sched_note_, why.c_str());
    refreshSchedule();
    changed();
    return why.empty();
}

bool RollUi::removeSelectedScheduleRow() {
    if (!sched_wnd_ || !env_.hooks) return false;
    const int sel = static_cast<int>(SendMessageW(sched_list_, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(sched_rows_.size())) return false;
    auto rows = env_.hooks->petSettings().recordSchedule;
    if (sel >= static_cast<int>(rows.size())) return false;
    rows.erase(rows.begin() + sel);
    std::wstring why;
    if (!env_.hooks->setRecordSchedule(rows, env_.hooks->petSettings().recordScheduleEnabled)) {
        const std::string e = env_.hooks->petSaveError();
        why = L"Not saved: " + (e.empty() ? std::wstring(L"the schedule couldn't be changed.") : widenU8(e));
    }
    SetWindowTextW(sched_note_, why.c_str());
    refreshSchedule();
    changed();
    return why.empty();
}

LRESULT RollUi::scheduleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kSchedAdd: addScheduleRowFromEditor(); break;
                case kSchedRemove: removeSelectedScheduleRow(); break;
                case kSchedClose: DestroyWindow(hwnd); break;
                default: break;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            sched_wnd_ = sched_list_ = sched_start_ = sched_minutes_ = sched_note_ = nullptr;
            for (HWND& h : sched_days_) h = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace kvwin
