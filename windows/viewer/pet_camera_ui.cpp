#include "pet_camera_ui.hpp"

#include "pet_camera_model.hpp"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>

namespace kvwin {

namespace {

PetCameraUi* g_petui = nullptr;

enum : int {
    kDevList = 100, kDevRename, kDevRole, kDevRePair, kDevRevoke, kDevRevokeAll, kDevPair, kDevClose,
    kSchedList = 200, kSchedAdd, kSchedRemove, kSchedClose, kSchedFrom, kSchedTo, kSchedDay0, // ... kSchedDay0 + 6
};

std::wstring widenU8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrowU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

uint64_t unixMsNow() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000ull;
}

std::wstring localDate(uint64_t unixMs) {
    if (unixMs == 0) return L"—";
    const uint64_t t = unixMs * 10000ull + 116444736000000000ull;
    FILETIME ft{static_cast<DWORD>(t & 0xFFFFFFFFu), static_cast<DWORD>(t >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"?";
    wchar_t b[32];
    swprintf(b, 32, L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    return b;
}

LRESULT CALLBACK DevicesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_petui) return g_petui->devicesProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ScheduleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_petui) return g_petui->scheduleProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The rename prompt (a small modal window, like the preset name prompt).
struct NamePrompt {
    HWND edit = nullptr;
    bool done = false;
    bool ok = false;
    std::wstring text;
};
NamePrompt* g_name_prompt = nullptr;

LRESULT CALLBACK NamePromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (g_name_prompt && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
                g_name_prompt->ok = LOWORD(wp) == IDOK;
                if (g_name_prompt->ok) {
                    wchar_t b[128] = {0};
                    GetWindowTextW(g_name_prompt->edit, b, 128);
                    g_name_prompt->text = b;
                }
                g_name_prompt->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            if (g_name_prompt) g_name_prompt->done = true;
            DestroyWindow(hwnd);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerClass(HINSTANCE inst, const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof(wc)};
    if (GetClassInfoExW(inst, name, &wc)) return;
    wc = WNDCLASSEXW{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

} // namespace

PetCameraUi::PetCameraUi() { g_petui = this; }

PetCameraUi::~PetCameraUi() {
    closeAll();
    if (g_petui == this) g_petui = nullptr;
}

void PetCameraUi::init(const Env& env) { env_ = env; }

void PetCameraUi::closeAll() {
    if (dev_wnd_) DestroyWindow(dev_wnd_);
    if (sched_wnd_) DestroyWindow(sched_wnd_);
}

HWND PetCameraUi::child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), env_.inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(env_.font), TRUE);
    return c;
}

void PetCameraUi::changed() {
    if (env_.on_changed) env_.on_changed();
}

// ---- Devices... -----------------------------------------------------------------------------------------

void PetCameraUi::openDevices() {
    if (!env_.hooks) return;
    if (dev_wnd_) {
        refreshDevices();
        SetForegroundWindow(dev_wnd_);
        return;
    }
    registerClass(env_.inst, L"KinectViewerPetDevices", DevicesProc);
    RECT mr{};
    GetWindowRect(env_.main, &mr);
    dev_wnd_ = CreateWindowExW(0, L"KinectViewerPetDevices", L"Pet camera: devices", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               mr.left + S(320), mr.top + S(80), S(720), S(430), env_.main, nullptr, env_.inst, nullptr);
    dev_list_ = child(dev_wnd_, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 10,
                      10, 684, 232, kDevList);
    ListView_SetExtendedListViewStyle(dev_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct {
        const wchar_t* title;
        int width;
    } cols[] = {{L"Name", 140}, {L"Role", 56}, {L"Kind", 56}, {L"Paired", 84}, {L"Last seen", 150}, {L"Connected", 192}};
    for (int i = 0; i < 6; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(cols[i].title);
        col.cx = S(cols[i].width);
        ListView_InsertColumn(dev_list_, i, &col);
    }
    dev_rename_ = child(dev_wnd_, L"BUTTON", L"Rename…", BS_PUSHBUTTON | WS_TABSTOP, 10, 250, 90, 26, kDevRename);
    dev_role_ = child(dev_wnd_, L"BUTTON", L"Make owner", BS_PUSHBUTTON | WS_TABSTOP, 106, 250, 100, 26, kDevRole);
    dev_repair_ = child(dev_wnd_, L"BUTTON", L"Re-pair…", BS_PUSHBUTTON | WS_TABSTOP, 212, 250, 90, 26, kDevRePair);
    dev_revoke_ = child(dev_wnd_, L"BUTTON", L"Revoke…", BS_PUSHBUTTON | WS_TABSTOP, 308, 250, 90, 26, kDevRevoke);
    dev_revoke_all_ = child(dev_wnd_, L"BUTTON", L"Revoke all…", BS_PUSHBUTTON | WS_TABSTOP, 594, 250, 100, 26, kDevRevokeAll);
    child(dev_wnd_, L"BUTTON", L"Pair a device…", BS_PUSHBUTTON | WS_TABSTOP, 10, 288, 120, 26, kDevPair);
    child(dev_wnd_, L"STATIC", L"Pairing your own phone again? Use Re-pair on its row: it keeps its name and role.",
          SS_LEFT | SS_CENTERIMAGE, 140, 288, 554, 26, 0);
    child(dev_wnd_, L"STATIC",
          L"Highlighted rows are owners not seen for 30 days: revoke them if the device is lost. Owners may change "
          L"sharing and privacy from the device; viewers only watch (while sharing is on).",
          SS_LEFT, 10, 322, 684, 34, 0);
    dev_summary_ = child(dev_wnd_, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 10, 362, 560, 24, 0);
    child(dev_wnd_, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 594, 360, 100, 26, kDevClose);
    refreshDevices();
    ShowWindow(dev_wnd_, SW_SHOWNOACTIVATE);
}

void PetCameraUi::refreshDevices() {
    if (!dev_wnd_ || !env_.hooks) return;
    const std::string keep = selectedDeviceId();
    rows_ = env_.hooks->petDevices();
    const uint64_t now = unixMsNow();
    stale_.assign(rows_.size(), false);
    SendMessageW(dev_list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(dev_list_);
    int connected = 0, owners = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& d = rows_[i];
        stale_[i] = isStaleOwner(d, now);
        connected += d.connected ? 1 : 0;
        owners += d.role == kvhost::PetRole::Owner ? 1 : 0;
        std::wstring seen = lastSeenText(d, now);
        if (stale_[i]) seen += L" (30+ days)";
        std::wstring cols[6] = {widenU8(d.name), roleLabel(d.role), kindLabel(d.kind), localDate(d.pairedAtUnixMs), seen,
                                connectionText(d)};
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = static_cast<int>(i);
        lv.pszText = cols[0].data();
        ListView_InsertItem(dev_list_, &lv);
        for (int c = 1; c < 6; ++c) ListView_SetItemText(dev_list_, static_cast<int>(i), c, cols[c].data());
        if (!keep.empty() && d.deviceId == keep) {
            ListView_SetItemState(dev_list_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    SendMessageW(dev_list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(dev_list_, nullptr, TRUE);
    wchar_t b[200];
    swprintf(b, 200, L"%zu device(s) paired (16 at most): %d owner(s), %d connected now.", rows_.size(), owners, connected);
    SetWindowTextW(dev_summary_, b);
    updateDeviceButtons();
}

int PetCameraUi::deviceRows() const { return dev_list_ ? ListView_GetItemCount(dev_list_) : 0; }

int PetCameraUi::highlightedRows() const {
    return static_cast<int>(std::count(stale_.begin(), stale_.end(), true));
}

std::string PetCameraUi::selectedDeviceId() const {
    if (!dev_list_) return std::string();
    const int sel = ListView_GetNextItem(dev_list_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(rows_.size())) return std::string();
    return rows_[static_cast<size_t>(sel)].deviceId;
}

void PetCameraUi::updateDeviceButtons() {
    if (!dev_wnd_) return;
    const std::string id = selectedDeviceId();
    const bool any = !id.empty();
    for (HWND b : {dev_rename_, dev_role_, dev_repair_, dev_revoke_}) EnableWindow(b, any);
    EnableWindow(dev_revoke_all_, !rows_.empty());
    bool owner = false;
    for (const auto& d : rows_) {
        if (d.deviceId == id) owner = d.role == kvhost::PetRole::Owner;
    }
    SetWindowTextW(dev_role_, owner ? L"Make viewer" : L"Make owner");
}

std::optional<std::wstring> PetCameraUi::askName(HWND owner, const std::wstring& current) {
    registerClass(env_.inst, L"KinectViewerPetName", NamePromptProc);
    NamePrompt st;
    g_name_prompt = &st;
    RECT orc{};
    GetWindowRect(owner, &orc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"KinectViewerPetName", L"Rename device", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               orc.left + S(60), orc.top + S(80), S(300), S(130), owner, nullptr, env_.inst, nullptr);
    child(dlg, L"STATIC", L"Name shown on the PC and on the device:", SS_LEFT, 12, 10, 270, 18, 0);
    st.edit = child(dlg, L"EDIT", current.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 12, 32, 270, 24, 100);
    SendMessageW(st.edit, EM_LIMITTEXT, 64, 0);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    child(dlg, L"BUTTON", L"Rename", BS_DEFPUSHBUTTON | WS_TABSTOP, 120, 66, 76, 26, IDOK);
    child(dlg, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 204, 66, 78, 26, IDCANCEL);
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit);
    if (env_.run_modal) env_.run_modal(dlg, [&] { return st.done; });
    if (IsWindow(dlg)) DestroyWindow(dlg);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_name_prompt = nullptr;
    if (!st.ok) return std::nullopt;
    return st.text;
}

bool PetCameraUi::renameDevice(const std::string& deviceId, const std::wstring& name) {
    if (!env_.hooks) return false;
    const bool ok = env_.hooks->renameDevice(deviceId, narrowU8(name), &last_error_);
    refreshDevices();
    changed();
    return ok;
}

bool PetCameraUi::toggleRole(const std::string& deviceId) {
    if (!env_.hooks) return false;
    for (const auto& d : env_.hooks->petDevices()) {
        if (d.deviceId != deviceId) continue;
        const auto to = d.role == kvhost::PetRole::Owner ? kvhost::PetRole::Viewer : kvhost::PetRole::Owner;
        const bool ok = env_.hooks->setDeviceRole(deviceId, to, &last_error_); // at once, on its open session too (v3 §3.2)
        refreshDevices();
        changed();
        return ok;
    }
    return false;
}

void PetCameraUi::rePair(const std::string& deviceId) {
    if (env_.hooks) env_.hooks->openPairingWindowFor(dev_wnd_ ? dev_wnd_ : env_.main, kvhost::PetRole::Viewer, deviceId);
}

bool PetCameraUi::revokeDevice(const std::string& deviceId, bool ask) {
    if (!env_.hooks) return false;
    std::wstring name;
    for (const auto& d : env_.hooks->petDevices()) {
        if (d.deviceId == deviceId) name = widenU8(d.name);
    }
    if (ask) {
        const std::wstring q = L"Revoke “" + name +
                               L"”? It is disconnected now and can't connect again until it is paired again on this PC.";
        if (MessageBoxW(dev_wnd_ ? dev_wnd_ : env_.main, q.c_str(), L"Revoke device", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) !=
            IDOK) {
            return false;
        }
    }
    const bool ok = env_.hooks->revokeDevice(deviceId, &last_error_);
    if (!ok && ask && !last_error_.empty()) {
        MessageBoxW(dev_wnd_ ? dev_wnd_ : env_.main, widenU8(last_error_).c_str(), L"Revoke device", MB_ICONERROR);
    }
    refreshDevices();
    changed();
    return ok;
}

size_t PetCameraUi::revokeAll(bool ask, HWND owner) {
    if (!env_.hooks) return 0;
    if (ask) {
        HWND o = owner ? owner : (dev_wnd_ ? dev_wnd_ : env_.main);
        if (MessageBoxW(o,
                        L"Revoke every paired device? They are all disconnected now and each one must be paired again "
                        L"on this PC.",
                        L"Revoke all devices", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) {
            return 0;
        }
    }
    const size_t n = env_.hooks->revokeAllDevices(&last_error_);
    if (!last_error_.empty() && ask) {
        // Refused (the device list can't be read): the reason, never "0 devices revoked".
        MessageBoxW(owner ? owner : (dev_wnd_ ? dev_wnd_ : env_.main), widenU8(last_error_).c_str(), L"Revoke all devices",
                    MB_ICONERROR);
    }
    refreshDevices();
    changed();
    return n;
}

void PetCameraUi::pairDevice(HWND owner) {
    if (env_.hooks) env_.hooks->openPairingWindowFor(owner ? owner : env_.main, kvhost::PetRole::Viewer, std::string());
}

LRESULT PetCameraUi::devicesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const std::string id = selectedDeviceId();
            switch (LOWORD(wp)) {
                case kDevRename:
                    if (!id.empty()) {
                        std::wstring current;
                        for (const auto& d : rows_) {
                            if (d.deviceId == id) current = widenU8(d.name);
                        }
                        if (auto name = askName(hwnd, current)) {
                            if (!renameDevice(id, *name)) {
                                MessageBoxW(hwnd, last_error_.empty() ? L"That name is empty." : widenU8(last_error_).c_str(),
                                            L"Rename device", MB_ICONINFORMATION);
                            }
                        }
                    }
                    break;
                case kDevRole:
                    if (!id.empty() && !toggleRole(id) && !last_error_.empty()) {
                        MessageBoxW(hwnd, widenU8(last_error_).c_str(), L"Change role", MB_ICONINFORMATION);
                    }
                    break;
                case kDevRePair: if (!id.empty()) rePair(id); break;
                case kDevRevoke: if (!id.empty()) revokeDevice(id, true); break;
                case kDevRevokeAll: revokeAll(true); break;
                case kDevPair: pairDevice(hwnd); break;
                case kDevClose: DestroyWindow(hwnd); break;
                default: break;
            }
            return 0;
        }
        case WM_NOTIFY: {
            const auto* nm = reinterpret_cast<const NMHDR*>(lp);
            if (nm->hwndFrom != dev_list_) break;
            if (nm->code == LVN_ITEMCHANGED) updateDeviceButtons();
            if (nm->code == NM_CUSTOMDRAW) {
                // pet-camera.md §3: owners not seen for 30 days are highlighted.
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const size_t i = static_cast<size_t>(cd->nmcd.dwItemSpec);
                    if (i < stale_.size() && stale_[i]) {
                        cd->clrTextBk = RGB(255, 226, 170);
                        cd->clrText = RGB(110, 50, 0);
                    }
                    return CDRF_DODEFAULT;
                }
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            if (hwnd == dev_wnd_) {
                dev_wnd_ = dev_list_ = dev_rename_ = dev_role_ = dev_repair_ = dev_revoke_ = dev_revoke_all_ = dev_summary_ = nullptr;
                rows_.clear();
                stale_.clear();
            }
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Sharing schedule... ---------------------------------------------------------------------------------

void PetCameraUi::openSchedule() {
    if (!env_.hooks) return;
    if (sched_wnd_) {
        refreshSchedule();
        SetForegroundWindow(sched_wnd_);
        return;
    }
    registerClass(env_.inst, L"KinectViewerPetSchedule", ScheduleProc);
    RECT mr{};
    GetWindowRect(env_.main, &mr);
    sched_wnd_ = CreateWindowExW(0, L"KinectViewerPetSchedule", L"Sharing schedule", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 mr.left + S(330), mr.top + S(120), S(440), S(392), env_.main, nullptr, env_.inst, nullptr);
    child(sched_wnd_, L"STATIC", L"During these times sharing is off for viewers. Owners aren't affected.", SS_LEFT, 10, 10, 404,
          18, 0);
    sched_list_ = child(sched_wnd_, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 10, 32, 404, 130, kSchedList);
    // Monday first, as the rows read; the mask is SYSTEMTIME's (bit 0 Sunday).
    static const wchar_t* const day_names[7] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};
    for (int k = 0; k < 7; ++k) {
        const int bit = (k + 1) % 7;
        sched_days_[bit] = child(sched_wnd_, L"BUTTON", day_names[k], BS_AUTOCHECKBOX | WS_TABSTOP, 10 + k * 58, 172, 56, 22,
                                 kSchedDay0 + bit);
        SendMessageW(sched_days_[bit], BM_SETCHECK, BST_CHECKED, 0);
    }
    child(sched_wnd_, L"STATIC", L"From", SS_LEFT | SS_CENTERIMAGE, 10, 202, 36, 24, 0);
    sched_from_ = child(sched_wnd_, L"EDIT", L"22:00", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 48, 202, 60, 24, kSchedFrom);
    child(sched_wnd_, L"STATIC", L"to", SS_LEFT | SS_CENTERIMAGE, 116, 202, 20, 24, 0);
    sched_to_ = child(sched_wnd_, L"EDIT", L"07:00", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 138, 202, 60, 24, kSchedTo);
    for (HWND e : {sched_from_, sched_to_}) SendMessageW(e, EM_LIMITTEXT, 5, 0);
    child(sched_wnd_, L"BUTTON", L"Add row", BS_PUSHBUTTON | WS_TABSTOP, 210, 201, 96, 26, kSchedAdd);
    child(sched_wnd_, L"BUTTON", L"Remove selected", BS_PUSHBUTTON | WS_TABSTOP, 312, 201, 102, 26, kSchedRemove);
    child(sched_wnd_, L"STATIC",
          L"Times are HH:MM on this PC's clock. A row that ends before it starts runs past midnight (22:00 to 07:00); "
          L"the same time twice means 24 hours.",
          SS_LEFT, 10, 236, 404, 48, 0);
    sched_note_ = child(sched_wnd_, L"STATIC", L"", SS_LEFT, 10, 288, 404, 36, 0);
    child(sched_wnd_, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 314, 328, 100, 26, kSchedClose);
    refreshSchedule();
    ShowWindow(sched_wnd_, SW_SHOWNOACTIVATE);
}

void PetCameraUi::refreshSchedule() {
    if (!sched_wnd_ || !env_.hooks) return;
    const int sel = static_cast<int>(SendMessageW(sched_list_, LB_GETCURSEL, 0, 0));
    sched_rows_ = env_.hooks->petSettings().sharingSchedule;
    SendMessageW(sched_list_, LB_RESETCONTENT, 0, 0);
    for (const auto& r : sched_rows_) {
        SendMessageW(sched_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(scheduleRowText(r).c_str()));
    }
    if (sel >= 0 && sel < static_cast<int>(sched_rows_.size())) SendMessageW(sched_list_, LB_SETCURSEL, sel, 0);
    if (sched_rows_.empty()) SetWindowTextW(sched_note_, L"No rows: sharing follows “Share the camera” alone.");
    else SetWindowTextW(sched_note_, env_.hooks->sharingScheduledOffNow() ? L"Sharing is off now (schedule)." : L"");
}

void PetCameraUi::setScheduleEditorFields(uint8_t days, const std::wstring& from, const std::wstring& to) {
    if (!sched_wnd_) return;
    for (int bit = 0; bit < 7; ++bit) {
        SendMessageW(sched_days_[bit], BM_SETCHECK, (days & (1u << bit)) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    SetWindowTextW(sched_from_, from.c_str());
    SetWindowTextW(sched_to_, to.c_str());
}

bool PetCameraUi::selectScheduleRow(int index) {
    if (!sched_list_ || index < 0 || index >= static_cast<int>(sched_rows_.size())) return false;
    SendMessageW(sched_list_, LB_SETCURSEL, index, 0);
    return true;
}

bool PetCameraUi::addScheduleRowFromEditor() {
    if (!sched_wnd_ || !env_.hooks) return false;
    uint8_t days = 0;
    for (int bit = 0; bit < 7; ++bit) {
        if (SendMessageW(sched_days_[bit], BM_GETCHECK, 0, 0) == BST_CHECKED) days |= static_cast<uint8_t>(1u << bit);
    }
    wchar_t fb[16] = {0}, tb[16] = {0};
    GetWindowTextW(sched_from_, fb, 16);
    GetWindowTextW(sched_to_, tb, 16);
    const auto from = parseClockText(fb);
    const auto to = parseClockText(tb);
    if (days == 0) {
        SetWindowTextW(sched_note_, L"Pick at least one day.");
        return false;
    }
    if (!from || !to) {
        SetWindowTextW(sched_note_, L"Use HH:MM for both times (00:00 to 23:59).");
        return false;
    }
    auto rows = env_.hooks->petSettings().sharingSchedule;
    if (rows.size() >= kvhost::kMaxSharingScheduleRows) {
        SetWindowTextW(sched_note_, L"That's the most rows the schedule holds.");
        return false;
    }
    kvhost::SharingScheduleRow r;
    r.days = days;
    r.fromMinute = *from;
    r.toMinute = *to;
    rows.push_back(r);
    env_.hooks->setSharingSchedule(rows); // saved and applied at once (pet-camera.json)
    refreshSchedule();
    changed();
    return true;
}

bool PetCameraUi::removeSelectedScheduleRow() {
    if (!sched_wnd_ || !env_.hooks) return false;
    const int sel = static_cast<int>(SendMessageW(sched_list_, LB_GETCURSEL, 0, 0));
    auto rows = env_.hooks->petSettings().sharingSchedule;
    if (sel < 0 || sel >= static_cast<int>(rows.size())) return false;
    rows.erase(rows.begin() + sel);
    env_.hooks->setSharingSchedule(rows);
    refreshSchedule();
    changed();
    return true;
}

LRESULT PetCameraUi::scheduleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
            if (hwnd == sched_wnd_) {
                sched_wnd_ = sched_list_ = sched_from_ = sched_to_ = sched_note_ = nullptr;
                for (HWND& d : sched_days_) d = nullptr;
            }
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace kvwin
