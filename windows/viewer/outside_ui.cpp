#include "outside_ui.hpp"

#include "capture_exclusion.hpp"

#include "outside_panel_model.hpp"
#include "qr_draw.hpp"
#include "remote_invite.hpp" // kvhost::outside::EndpointLink: the re-share link's host

#include <commctrl.h>
#include <netlistmgr.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace kvwin {

namespace {

OutsideUi* g_outui = nullptr;

constexpr UINT_PTR kInviteTimer = 1;

enum : int {
    kInvCopy = 100, kInvCancel,
    kRsCopy = 200, kRsDone,
    kLogList = 300, kLogRefresh, kLogClose,
    kPromptEdit = 400, kPromptViewer, kPromptOwner,
};

std::wstring widenU8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

uint64_t unixMsNow() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000ull;
}

std::wstring localTime(uint64_t unixMs) {
    const uint64_t t = unixMs * 10000ull + 116444736000000000ull;
    FILETIME ft{static_cast<DWORD>(t & 0xFFFFFFFFu), static_cast<DWORD>(t >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"?";
    wchar_t b[40];
    swprintf(b, 40, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
             local.wSecond);
    return b;
}

std::wstring windowText(HWND h) {
    wchar_t b[2048] = {0};
    if (h) GetWindowTextW(h, b, 2048);
    return b;
}

void registerClass(HINSTANCE inst, const wchar_t* name, WNDPROC proc, HBRUSH background) {
    WNDCLASSEXW wc{sizeof(wc)};
    if (GetClassInfoExW(inst, name, &wc)) return;
    wc = WNDCLASSEXW{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

LRESULT CALLBACK InviteProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_outui) return g_outui->inviteProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK ReshareProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_outui) return g_outui->reshareProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_outui) return g_outui->logProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The two small prompts (the invite's role, the port), modal through the viewer's own loop.
struct Prompt {
    HWND edit = nullptr, viewer = nullptr, owner = nullptr, error = nullptr;
    bool port = false; // the port prompt: OK checks the number first
    bool done = false, ok = false;
    bool owner_checked = false;
    uint16_t port_value = 0;
};
Prompt* g_prompt = nullptr;

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (!g_prompt) break;
            if (LOWORD(wp) == IDOK) {
                if (g_prompt->port) {
                    const auto p = parseOutsidePort(windowText(g_prompt->edit));
                    if (!p) {
                        SetWindowTextW(g_prompt->error, L"Use a number from 1024 to 65535.");
                        return 0;
                    }
                    g_prompt->port_value = *p;
                } else {
                    g_prompt->owner_checked = SendMessageW(g_prompt->owner, BM_GETCHECK, 0, 0) == BST_CHECKED;
                }
                g_prompt->ok = true;
                g_prompt->done = true;
                DestroyWindow(hwnd);
            } else if (LOWORD(wp) == IDCANCEL) {
                g_prompt->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            if (g_prompt) g_prompt->done = true;
            DestroyWindow(hwnd);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

OutsideUi::OutsideUi() { g_outui = this; }

OutsideUi::~OutsideUi() {
    stopProfileWatch();
    closeAll();
    if (g_outui == this) g_outui = nullptr;
}

void OutsideUi::init(const Env& env) {
    env_ = env;
    startProfileWatch();
}

namespace {

// pet-camera.md §7: 1 when any connected network's category is Public, 0 when none is, -1 when it can't be read. Read
// only (INetworkListManager, Windows' own API): the profile is never changed here.
int readNetworkPublic() {
    INetworkListManager* nlm = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, __uuidof(INetworkListManager),
                                reinterpret_cast<void**>(&nlm))) ||
        !nlm)
        return -1;
    int result = -1;
    IEnumNetworks* nets = nullptr;
    if (SUCCEEDED(nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &nets)) && nets) {
        result = 0;
        INetwork* net = nullptr;
        ULONG got = 0;
        while (nets->Next(1, &net, &got) == S_OK && got == 1 && net) {
            NLM_NETWORK_CATEGORY cat = NLM_NETWORK_CATEGORY_PRIVATE;
            if (SUCCEEDED(net->GetCategory(&cat)) && cat == NLM_NETWORK_CATEGORY_PUBLIC) result = 1;
            net->Release();
            net = nullptr;
        }
        nets->Release();
    }
    nlm->Release();
    return result;
}

} // namespace

void OutsideUi::startProfileWatch() {
    if (profile_thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(profile_mu_);
        profile_stop_ = false;
    }
    // Off the UI thread, with its own COM apartment: the query is quick, but never makes the panel wait.
    profile_thread_ = std::thread([this] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            profile_public_ = readNetworkPublic();
            std::unique_lock<std::mutex> lk(profile_mu_);
            if (profile_cv_.wait_for(lk, std::chrono::seconds(15), [this] { return profile_stop_; })) break;
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    });
}

void OutsideUi::stopProfileWatch() {
    {
        std::lock_guard<std::mutex> lk(profile_mu_);
        profile_stop_ = true;
    }
    profile_cv_.notify_all();
    if (profile_thread_.joinable()) profile_thread_.join();
}

std::optional<bool> OutsideUi::networkPublic() const {
    const int v = profile_public_.load();
    if (v < 0) return std::nullopt;
    return v == 1;
}

void OutsideUi::closeAll() {
    if (inv_wnd_) DestroyWindow(inv_wnd_);
    if (rs_wnd_) DestroyWindow(rs_wnd_);
    if (log_wnd_) DestroyWindow(log_wnd_);
}

HWND OutsideUi::child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), env_.inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(env_.font), TRUE);
    return c;
}

void OutsideUi::changed(const std::wstring& note) {
    if (env_.on_changed) env_.on_changed(note);
}

void OutsideUi::copyToClipboard(HWND owner, const std::string& text, bool private_data) {
    const std::wstring w = widenU8(text);
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, w.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    if (private_data) {
        // An invite link holds a live token (§8.4: "Send this only to the person you're inviting"): kept out of
        // clipboard history, the cloud clipboard and clipboard monitors, as Windows documents for passwords.
        for (const std::wstring& name : privateClipboardFormats()) {
            const UINT fmt = RegisterClipboardFormatW(name.c_str());
            if (!fmt) continue;
            if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
                if (void* p = GlobalLock(mem)) {
                    *static_cast<DWORD*>(p) = 0;
                    GlobalUnlock(mem);
                    if (!SetClipboardData(fmt, mem)) GlobalFree(mem);
                } else {
                    GlobalFree(mem);
                }
            }
        }
    }
    CloseClipboard();
}

// ---- Invite from outside... ---------------------------------------------------------------------------------------

std::optional<kvhost::PetRole> OutsideUi::askInviteRole(HWND owner) {
    registerClass(env_.inst, L"KinectViewerOutsidePrompt", PromptProc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    Prompt st;
    g_prompt = &st;
    RECT orc{};
    GetWindowRect(owner, &orc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"KinectViewerOutsidePrompt", L"Invite from outside the home",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + S(320), orc.top + S(120), S(340), S(200), owner,
                               nullptr, env_.inst, nullptr);
    child(dlg, L"STATIC", L"The invited device gets this role (it can be changed later in Devices…):", SS_LEFT, 12, 10, 310,
          32, 0);
    st.viewer = child(dlg, L"BUTTON", L"Viewer (watch only)", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 20, 46, 160, 22,
                      kPromptViewer);
    st.owner = child(dlg, L"BUTTON", L"Owner", BS_AUTORADIOBUTTON, 190, 46, 120, 22, kPromptOwner);
    SendMessageW(st.viewer, BM_SETCHECK, BST_CHECKED, 0); // v3 D1: Viewer by default
    child(dlg, L"STATIC", L"A new invite closes any invite that is still open.", SS_LEFT, 12, 76, 310, 20, 0);
    child(dlg, L"BUTTON", L"Create invite", BS_DEFPUSHBUTTON | WS_TABSTOP, 120, 112, 100, 28, IDOK);
    child(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 226, 112, 96, 28, IDCANCEL);
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.viewer);
    if (env_.run_modal) env_.run_modal(dlg, [&] { return st.done; });
    if (IsWindow(dlg)) DestroyWindow(dlg);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_prompt = nullptr;
    if (!st.ok) return std::nullopt;
    return st.owner_checked ? kvhost::PetRole::Owner : kvhost::PetRole::Viewer;
}

bool OutsideUi::openInvite(kvhost::PetRole role, std::string* err) {
    if (!env_.hooks) {
        if (err) *err = "no streaming host";
        return false;
    }
    if (inv_wnd_) {
        inv_closed_because_.clear();
        DestroyWindow(inv_wnd_); // the host closes the older invite when it makes the new one
    }
    const auto made = env_.hooks->createOutsideInvite(role, err);
    if (!made) return false;
    showInvite(*made);
    return true;
}

void OutsideUi::showInvite(const OutsideInvitePanelView& invite) {
    if (inv_wnd_) {
        inv_closed_because_.clear();
        DestroyWindow(inv_wnd_);
    }
    inv_ = invite;
    inv_masked_ = env_.mask_tokens;
    inv_shown_ = inv_masked_ ? maskInviteToken(inv_.uri) : inv_.uri; // v3 §10: a test run never shows a live token
    inv_closed_because_.clear();

    registerClass(env_.inst, L"KinectViewerOutsideInvite", InviteProc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    RECT mr{};
    GetWindowRect(env_.main, &mr);
    inv_wnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, L"KinectViewerOutsideInvite", L"Invite from outside the home",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, mr.left + S(320), mr.top + S(60), S(436), S(640), env_.main,
                               nullptr, env_.inst, nullptr);
    kvwin::excludeFromCapture(inv_wnd_); // its link and QR code are a live invite: never in a screen share or screenshot
    inv_role_ = child(inv_wnd_, L"STATIC", inviteRoleText(inv_.role).c_str(), SS_CENTER, 16, 10, 388, 20, 0);
    SendMessageW(inv_role_, WM_SETFONT, reinterpret_cast<WPARAM>(env_.bold), TRUE);
    child(inv_wnd_, L"STATIC",
          L"Scan this on the invited device, or copy the link and send it there. It works once, within 10 minutes.",
          SS_CENTER, 16, 32, 388, 34, 0);
    // The QR code is drawn at y 72, 260 px square (WM_PAINT).
    inv_text_ = child(inv_wnd_, L"EDIT", widenU8(inv_shown_).c_str(),
                      WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 16, 340, 388, 70, 0);
    child(inv_wnd_, L"BUTTON", L"Copy link", BS_PUSHBUTTON | WS_TABSTOP, 16, 418, 100, 26, kInvCopy);
    inv_countdown_ = child(inv_wnd_, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 128, 418, 276, 26, 0);
    inv_warning_ = child(inv_wnd_, L"STATIC", kOutsideInviteWarning, SS_CENTER, 16, 454, 388, 22, 0);
    SendMessageW(inv_warning_, WM_SETFONT, reinterpret_cast<WPARAM>(env_.bold), TRUE);
    child(inv_wnd_, L"STATIC",
          L"Anyone holding it can use it once until it runs out, and a wrong code burns it; then make a new one. Closing "
          L"this window keeps the invite open; Cancel closes it.",
          SS_CENTER, 16, 480, 388, 50, 0);
    child(inv_wnd_, L"BUTTON", L"Cancel invite", BS_PUSHBUTTON | WS_TABSTOP, 150, 548, 120, 30, kInvCancel);
    if (inv_masked_) EnableWindow(GetDlgItem(inv_wnd_, kInvCopy), FALSE); // nothing of a test run's token to copy
    updateInviteCountdown();
    SetTimer(inv_wnd_, kInviteTimer, 500, nullptr);
    ShowWindow(inv_wnd_, SW_SHOWNOACTIVATE);
    changed(std::wstring());
}

std::wstring OutsideUi::inviteShownLink() const { return windowText(inv_text_); }
std::wstring OutsideUi::inviteCountdown() const { return windowText(inv_countdown_); }
std::wstring OutsideUi::inviteWarning() const { return windowText(inv_warning_); }
std::wstring OutsideUi::inviteRoleLine() const { return windowText(inv_role_); }

bool OutsideUi::inviteMatchesHost() const {
    if (!env_.hooks || inv_.uri.empty()) return false;
    const auto cur = env_.hooks->currentOutsideInvite();
    return cur && cur->uri == inv_.uri;
}

void OutsideUi::updateInviteCountdown() {
    if (!inv_countdown_) return;
    const std::wstring t = inviteCountdownText(inv_.expiresUnix, unixMsNow() / 1000);
    if (t != windowText(inv_countdown_)) SetWindowTextW(inv_countdown_, t.c_str());
}

void OutsideUi::cancelInvite() {
    if (env_.hooks) env_.hooks->cancelOutsideInvite();
    inv_closed_because_.clear();
    inv_closed_note_.clear();
    if (inv_wnd_) DestroyWindow(inv_wnd_);
    changed(L"The invite was cancelled.");
}

void OutsideUi::closeInviteWindow(const std::string& because, const std::wstring& note) {
    inv_closed_because_ = because;
    inv_closed_note_ = note;
    if (inv_wnd_) DestroyWindow(inv_wnd_);
    changed(note);
}

LRESULT OutsideUi::inviteProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr{};
            GetClientRect(hwnd, &cr);
            FillRect(dc, &cr, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            if (hwnd == inv_wnd_ && !inv_shown_.empty()) drawQrCode(dc, inv_shown_, cr.right / 2, S(72), S(260));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: // the labels and the read-only link, on the window's white
            SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
            if (reinterpret_cast<HWND>(lp) == inv_warning_) SetTextColor(reinterpret_cast<HDC>(wp), RGB(150, 30, 30));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        case WM_TIMER:
            if (wp == kInviteTimer && hwnd == inv_wnd_) {
                updateInviteCountdown();
                // The window closes itself when the host's invite is no longer open (§8.4), saying why as the host
                // knows it: used, ran out, cancelled, or the home address changed or went away. Never "used" unless
                // it was.
                const auto cur = env_.hooks ? env_.hooks->currentOutsideInvite() : std::nullopt;
                if (!cur || cur->uri != inv_.uri) {
                    const std::string ended = env_.hooks && !cur ? env_.hooks->outsideView().inviteEnded : std::string();
                    closeInviteWindow(ended.empty() ? std::string("closed") : ended, inviteClosedText(ended));
                } else if (unixMsNow() / 1000 >= inv_.expiresUnix) {
                    closeInviteWindow("expired", inviteClosedText("expired"));
                }
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kInvCopy:
                    if (!inv_masked_) copyToClipboard(hwnd, inv_.uri, true);
                    break;
                case kInvCancel: cancelInvite(); break;
                default: break;
            }
            return 0;
        case WM_CLOSE: // the invite stays open (it may have been sent already); the panel says so
            DestroyWindow(hwnd);
            changed(std::wstring());
            return 0;
        case WM_DESTROY:
            if (hwnd == inv_wnd_) {
                KillTimer(hwnd, kInviteTimer);
                inv_wnd_ = inv_text_ = inv_countdown_ = inv_warning_ = inv_role_ = nullptr;
            }
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Re-share address... ------------------------------------------------------------------------------------------

bool OutsideUi::openReshare(bool acknowledge, std::string* err) {
    if (!env_.hooks) {
        if (err) *err = "no streaming host";
        return false;
    }
    const std::string link = env_.hooks->outsideEndpointLink();
    if (link.empty()) {
        if (err) *err = "There's no outside address to share yet.";
        return false;
    }
    showReshare(link, acknowledge);
    return true;
}

void OutsideUi::showReshare(const std::string& link, bool acknowledge) {
    if (rs_wnd_) {
        rs_ack_ = false; // the window is shown again at once: the acknowledgement waits for that one to close
        DestroyWindow(rs_wnd_);
    }
    rs_link_ = link;
    rs_ack_ = acknowledge;
    registerClass(env_.inst, L"KinectViewerOutsideReshare", ReshareProc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    RECT mr{};
    GetWindowRect(env_.main, &mr);
    rs_wnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, L"KinectViewerOutsideReshare", L"Re-share address",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU, mr.left + S(340), mr.top + S(80), S(436), S(580), env_.main,
                              nullptr, env_.inst, nullptr);
    child(rs_wnd_, L"STATIC",
          L"Devices at home learn the new address by themselves. Send this link to paired devices that are away from home: "
          L"each tries the new address and keeps it only once it reaches this PC.",
          SS_CENTER, 16, 10, 388, 52, 0);
    // The QR code is drawn at y 72, 260 px square (WM_PAINT).
    child(rs_wnd_, L"EDIT", widenU8(rs_link_).c_str(), WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
          16, 340, 388, 56, 0);
    child(rs_wnd_, L"BUTTON", L"Copy link", BS_PUSHBUTTON | WS_TABSTOP, 16, 404, 100, 26, kRsCopy);
    child(rs_wnd_, L"STATIC", L"It holds no secret: only this PC's id, its fingerprint and the new address.", SS_LEFT, 128, 404,
          276, 34, 0);
    child(rs_wnd_, L"BUTTON", L"Done", BS_DEFPUSHBUTTON | WS_TABSTOP, 160, 488, 100, 30, kRsDone);
    ShowWindow(rs_wnd_, SW_SHOWNOACTIVATE);
}

void OutsideUi::closeReshare() {
    if (rs_wnd_) DestroyWindow(rs_wnd_);
}

LRESULT OutsideUi::reshareProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr{};
            GetClientRect(hwnd, &cr);
            FillRect(dc, &cr, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            if (hwnd == rs_wnd_ && !rs_link_.empty()) drawQrCode(dc, rs_link_, cr.right / 2, S(72), S(260));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        case WM_COMMAND:
            if (LOWORD(wp) == kRsCopy) copyToClipboard(hwnd, rs_link_, false); // no secret in it (§8.4)
            else if (LOWORD(wp) == kRsDone) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            if (hwnd == rs_wnd_) {
                rs_wnd_ = nullptr;
                // §8.3: shown, so the status goes back to mapped once the PC user has re-shared.
                const bool ack = rs_ack_;
                rs_ack_ = false;
                // The address in the link the PC user was shown: the host acknowledges only that one.
                kvhost::outside::EndpointLink shown;
                if (ack && env_.hooks && kvhost::outside::EndpointLink::parse(rs_link_, shown)) {
                    env_.hooks->acknowledgeAddressChange(shown.host);
                    acknowledged_host_ = shown.host;
                    ++acknowledged_;
                }
                // Neutral: the host applies it only while the shown address is still the one read, and the gateway
                // doesn't contradict it.
                changed(ack ? L"Re-share closed." : L"");
            }
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Connection log... --------------------------------------------------------------------------------------------

void OutsideUi::openLog() {
    if (!env_.hooks) return;
    if (log_wnd_) {
        refreshLog();
        SetForegroundWindow(log_wnd_);
        return;
    }
    registerClass(env_.inst, L"KinectViewerOutsideLog", LogProc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    RECT mr{};
    GetWindowRect(env_.main, &mr);
    log_wnd_ = CreateWindowExW(0, L"KinectViewerOutsideLog", L"Connection log", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               mr.left + S(300), mr.top + S(100), S(800), S(460), env_.main, nullptr, env_.inst, nullptr);
    child(log_wnd_, L"STATIC",
          L"The last 30 days, newest first: every connection to this PC, at home and from outside. It stays on this PC "
          L"and is never sent anywhere, not even to owner devices.",
          SS_LEFT, 10, 8, 764, 34, 0);
    log_list_ = child(log_wnd_, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP, 10, 46, 764, 320, kLogList);
    ListView_SetExtendedListViewStyle(log_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t* title;
        int width;
    } cols[] = {{L"Time", 128}, {L"Event", 170}, {L"Path", 60}, {L"Address", 120}, {L"Device", 120}, {L"Repeats", 60}, {L"Detail", 100}};
    for (int i = 0; i < 7; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(cols[i].title);
        col.cx = S(cols[i].width);
        ListView_InsertColumn(log_list_, i, &col);
    }
    log_summary_ = child(log_wnd_, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 10, 374, 540, 26, 0);
    child(log_wnd_, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 566, 374, 100, 26, kLogRefresh);
    child(log_wnd_, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 674, 374, 100, 26, kLogClose);
    refreshLog();
    ShowWindow(log_wnd_, SW_SHOWNOACTIVATE);
}

void OutsideUi::refreshLog() {
    if (!log_wnd_ || !env_.hooks) return;
    const auto rows = connectionLogRows(env_.hooks->connectionLog(kConnectionLogDays), unixMsNow());
    SendMessageW(log_list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(log_list_);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& l = rows[i];
        std::wstring cols[7] = {localTime(l.t),
                                connectionLogEventLabel(l.event),
                                l.path == "outside" ? L"outside" : L"home",
                                widenU8(l.addr),
                                connectionLogDeviceText(l),
                                l.repeat > 1 ? std::to_wstring(l.repeat) : std::wstring(),
                                widenU8(l.detail)};
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = static_cast<int>(i);
        lv.pszText = cols[0].data();
        ListView_InsertItem(log_list_, &lv);
        for (int c = 1; c < 7; ++c) ListView_SetItemText(log_list_, static_cast<int>(i), c, cols[c].data());
    }
    SendMessageW(log_list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(log_list_, nullptr, TRUE);
    wchar_t b[120];
    swprintf(b, 120, L"%zu line(s). Repeats within a minute are kept as one line.", rows.size());
    SetWindowTextW(log_summary_, b);
}

int OutsideUi::logRows() const { return log_list_ ? ListView_GetItemCount(log_list_) : 0; }

LRESULT OutsideUi::logProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == kLogRefresh) refreshLog();
            else if (LOWORD(wp) == kLogClose) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            if (hwnd == log_wnd_) log_wnd_ = log_list_ = log_summary_ = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Change port... -----------------------------------------------------------------------------------------------

std::optional<uint16_t> OutsideUi::askPort(HWND owner, uint16_t current) {
    registerClass(env_.inst, L"KinectViewerOutsidePrompt", PromptProc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    Prompt st;
    st.port = true;
    g_prompt = &st;
    RECT orc{};
    GetWindowRect(owner, &orc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"KinectViewerOutsidePrompt", L"Outside port", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               orc.left + S(320), orc.top + S(120), S(340), S(196), owner, nullptr, env_.inst, nullptr);
    child(dlg, L"STATIC",
          L"The TCP port devices outside the home connect to (1024 to 65535). The router mapping moves with it.", SS_LEFT, 12,
          10, 310, 34, 0);
    st.edit = child(dlg, L"EDIT", current ? std::to_wstring(current).c_str() : L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                    12, 50, 100, 24, kPromptEdit);
    SendMessageW(st.edit, EM_LIMITTEXT, 5, 0);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    st.error = child(dlg, L"STATIC", L"", SS_LEFT, 12, 80, 310, 20, 0);
    child(dlg, L"BUTTON", L"Change", BS_DEFPUSHBUTTON | WS_TABSTOP, 140, 108, 86, 28, IDOK);
    child(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 234, 108, 88, 28, IDCANCEL);
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit);
    if (env_.run_modal) env_.run_modal(dlg, [&] { return st.done; });
    if (IsWindow(dlg)) DestroyWindow(dlg);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_prompt = nullptr;
    if (!st.ok) return std::nullopt;
    return st.port_value;
}

} // namespace kvwin
