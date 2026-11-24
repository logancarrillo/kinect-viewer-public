#include "second_display.hpp"

namespace kvwin {

namespace {

constexpr wchar_t kClass[] = L"KinectViewerSecondDisplay";

LRESULT CALLBACK OutputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SecondDisplayOutput*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;     // the mouse goes through (the Mac's ignoresMouseEvents)
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // and it never takes the keyboard
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (self) self->present();
            return 0;
        }
        case WM_SIZE:
            if (self) self->present();
            return 0;
        case WM_CLOSE: return 0; // closed from the main window only
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

uint64_t frameHash(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride) {
    uint64_t h = 1469598103934665603ull; // FNV-1a over the visible rows
    if (!bgra) return 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
        for (size_t i = 0; i < static_cast<size_t>(width) * 4; ++i) {
            h ^= row[i];
            h *= 1099511628211ull;
        }
    }
    return h;
}

bool SecondDisplayOutput::clickThrough() const {
    if (!hwnd_) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    return (ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT);
}

bool SecondDisplayOutput::start(HINSTANCE inst, const RECT& r, std::string* err) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        if (err) *err = "that display has no area";
        return false;
    }
    if (!hwnd_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = OutputProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            registered = true;
        }
        // No caption, no border, not in the taskbar or Alt-Tab, never activated. Layered + transparent
        // (fully opaque) makes it click-through for every window under it, any program's, as the Mac's
        // ignoresMouseEvents: a click on a projector showing it over another app reaches that app.
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT, kClass,
                                L"Kinect viewer output", WS_POPUP, r.left, r.top, w, h, nullptr, nullptr, inst, nullptr);
        if (!hwnd_) {
            if (err) *err = "CreateWindowEx failed";
            return false;
        }
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (!d3d_.init(hwnd_, err)) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return false;
        }
        d3d_.setBackground(0.0f, 0.0f, 0.0f); // black around the picture, and when blacked out
        d3d_.setSyncInterval(0);
    }
    // Above the taskbar on that display; the main window keeps the focus.
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    d3d_.resize();
    present();
    return true;
}

void SecondDisplayOutput::stop() {
    if (!hwnd_) return;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    d3d_.shutdown();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void SecondDisplayOutput::showFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride, bool nearest) {
    if (!hwnd_) return;
    d3d_.upload(bgra, width, height, stride);
    last_hash_ = frameHash(bgra, width, height, stride);
    nearest_ = nearest;
    if (!blackout_) {
        ++frames_;
        present();
    }
}

void SecondDisplayOutput::clearFrame() {
    if (!hwnd_) return;
    d3d_.clearFrame();
    present();
}

void SecondDisplayOutput::setBlackout(bool on) {
    blackout_ = on;
    present();
}

void SecondDisplayOutput::present() {
    if (!hwnd_) return;
    d3d_.resize();
    if (blackout_) {
        // Black, whatever frame it holds: the frame stays uploaded for the moment B comes back.
        const bool had = d3d_.hasFrame();
        d3d_.clearFrame();
        d3d_.render(false);
        if (had) d3d_.restoreFrame();
        ++blacks_;
        return;
    }
    d3d_.render(nearest_);
}

} // namespace kvwin
