// The pet camera's windows on the PC (pet-camera.md §4, protocol v3 W6): the Devices dialog (name, role,
// kind, paired, last seen, connected; Rename, Role, Re-pair, Revoke, Revoke all, Pair a device...) and
// the sharing-schedule editor. Both are modeless and talk to the host only through StreamHooks, on the
// UI thread. The panel section and the "Watching now" banner live in viewer_app.cpp.
#pragma once

#include "viewer_app.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kvwin {

class PetCameraUi {
public:
    struct Env {
        HINSTANCE inst = nullptr;
        HWND main = nullptr;
        HFONT font = nullptr;
        HFONT bold = nullptr;
        int dpi = 96;
        StreamHooks* hooks = nullptr;
        // Runs a modal prompt's loop until done() (the viewer's own loop, so the view and the phones'
        // video keep running while it is up).
        std::function<void(HWND dlg, const std::function<bool()>& done)> run_modal;
        // Something changed here (a rename, a schedule row): the panel refreshes.
        std::function<void()> on_changed;
    };

    PetCameraUi();
    ~PetCameraUi();
    PetCameraUi(const PetCameraUi&) = delete;
    PetCameraUi& operator=(const PetCameraUi&) = delete;

    void init(const Env& env);
    void closeAll();

    // ---- Devices... -----------------------------------------------------------------------------------
    void openDevices();
    HWND devicesWindow() const { return dev_wnd_; }
    void refreshDevices(); // the list again (keeps the selection by device id)
    int deviceRows() const;
    int highlightedRows() const; // stale owners (pet-camera.md §3)
    // The dialog's actions, also used by the --test-pet-camera hook. `ask` shows the confirm dialog, and a
    // refusal's reason in a message box after it. lastError(): the last action's refusal (empty after a success).
    bool renameDevice(const std::string& deviceId, const std::wstring& name);
    bool toggleRole(const std::string& deviceId);
    void rePair(const std::string& deviceId);
    bool revokeDevice(const std::string& deviceId, bool ask);
    size_t revokeAll(bool ask, HWND owner = nullptr);
    const std::string& lastError() const { return last_error_; }
    void pairDevice(HWND owner = nullptr); // the pairing window, with its role choice (Viewer by default)

    // ---- Sharing schedule... --------------------------------------------------------------------------
    void openSchedule();
    HWND scheduleWindow() const { return sched_wnd_; }
    void refreshSchedule();
    // The editor's Add and Remove, with the editor's own fields (the test hook fills them first).
    // days: bit 0 Sunday ... bit 6 Saturday. False with the reason shown in the editor.
    bool addScheduleRowFromEditor();
    bool removeSelectedScheduleRow();
    void setScheduleEditorFields(uint8_t days, const std::wstring& from, const std::wstring& to);
    bool selectScheduleRow(int index);

    // Window procedures (static trampolines call these).
    LRESULT devicesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT scheduleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    int S(int v) const { return MulDiv(v, env_.dpi, 96); }
    HWND child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id);
    std::string selectedDeviceId() const;
    void updateDeviceButtons();
    std::optional<std::wstring> askName(HWND owner, const std::wstring& current);
    void changed();

    Env env_;
    HWND dev_wnd_ = nullptr;
    HWND dev_list_ = nullptr;
    HWND dev_rename_ = nullptr, dev_role_ = nullptr, dev_repair_ = nullptr, dev_revoke_ = nullptr, dev_revoke_all_ = nullptr;
    HWND dev_summary_ = nullptr;
    std::vector<kvhost::HostDeviceInfo> rows_;
    std::vector<bool> stale_;
    std::string last_error_;

    HWND sched_wnd_ = nullptr;
    HWND sched_list_ = nullptr;
    HWND sched_days_[7] = {};
    HWND sched_from_ = nullptr, sched_to_ = nullptr, sched_note_ = nullptr;
    std::vector<kvhost::SharingScheduleRow> sched_rows_;
};

} // namespace kvwin
