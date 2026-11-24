// Outside the home, on the PC (pet-camera.md §3, §4; protocol v3 §8.4, §8.6): the invite window (its QR code,
// the copyable link and the 10-minute countdown), the re-share window (the endpoint link, which holds no secret),
// the connection log window, and the two small prompts the panel asks (the invite's role, the outside port). All
// modeless except the prompts, all on the UI thread, and all talk to the host through StreamHooks only. The panel's
// "Outside the home" section lives in viewer_app.cpp.
//
// v3 §10: in a test run (mask_tokens) the invite window never shows a live token: the link it shows and draws has
// the token's value replaced. The screenshot hook refuses while the invite window is open (inviteOpen), and no hook
// captures it or the re-share window.
#pragma once

#include "viewer_app.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kvwin {

class OutsideUi {
public:
    struct Env {
        HINSTANCE inst = nullptr;
        HWND main = nullptr;
        HFONT font = nullptr;
        HFONT bold = nullptr;
        int dpi = 96;
        StreamHooks* hooks = nullptr;
        bool mask_tokens = false; // a test run: never show a live token (v3 §10)
        // Runs a modal prompt's loop until done() (the viewer's own loop: the view and the phones' video go on).
        std::function<void(HWND dlg, const std::function<bool()>& done)> run_modal;
        // Something changed here, with a line for the panel to show (or empty): the panel refreshes.
        std::function<void(const std::wstring& note)> on_changed;
    };

    OutsideUi();
    ~OutsideUi();
    OutsideUi(const OutsideUi&) = delete;
    OutsideUi& operator=(const OutsideUi&) = delete;

    void init(const Env& env);
    void closeAll();

    // ---- Invite from outside... (§8.4) -----------------------------------------------------------------------
    // The role prompt (Viewer by default, v3 D1); nullopt when cancelled.
    std::optional<kvhost::PetRole> askInviteRole(HWND owner);
    // Makes the invite (closing any older one, as the host does) and shows it. False with err when refused.
    bool openInvite(kvhost::PetRole role, std::string* err);
    // Shows an invite the host made (openInvite's second half; the test hook also hands it a made-up one, with no
    // real token, to check the window in every build).
    void showInvite(const OutsideInvitePanelView& invite);
    HWND inviteWindow() const { return inv_wnd_; }
    bool inviteOpen() const { return inv_wnd_ != nullptr; }
    // The window's Cancel: the host closes the invite, and the window goes.
    void cancelInvite();
    // What the window shows (for the test hook, which writes none of the link itself to its report).
    std::wstring inviteShownLink() const;
    std::wstring inviteCountdown() const;
    std::wstring inviteWarning() const;
    std::wstring inviteRoleLine() const;
    size_t inviteLinkLength() const { return inv_.uri.size(); }
    bool inviteMasked() const { return inv_masked_; }
    // The link the window holds is the host's open invite (the hook's check, without reading the link out).
    bool inviteMatchesHost() const;
    // Why the window last closed itself: "" while open or after Cancel; the host's reason when its invite went
    // (OutsidePanelView::inviteEnded: "used", "expired", "cancelled", "address_gone", ...), "closed" when the host
    // gave none, "expired" when its time ran out here first.
    const std::string& inviteClosedBecause() const { return inv_closed_because_; }
    const std::wstring& inviteClosedNote() const { return inv_closed_note_; } // the sentence it closed with

    // ---- Re-share address... (§8.3, §8.4) ----------------------------------------------------------------------
    // The endpoint link as a QR code and as text. acknowledge: closing the window acknowledges the address change.
    bool openReshare(bool acknowledge, std::string* err);
    void showReshare(const std::string& link, bool acknowledge); // openReshare's second half
    HWND reshareWindow() const { return rs_wnd_; }
    const std::string& reshareLink() const { return rs_link_; }
    void closeReshare(); // as its Done button
    int acknowledgedCount() const { return acknowledged_; }
    // The host the last acknowledgement carried: the address in the re-share link that was shown (v3 §8.3).
    const std::string& acknowledgedHost() const { return acknowledged_host_; }

    // ---- Connection log... (§8.6) --------------------------------------------------------------------------------
    void openLog();
    HWND logWindow() const { return log_wnd_; }
    void refreshLog();
    int logRows() const;

    // ---- pet-camera.md §7: the network profile ----------------------------------------------------------------------
    // Whether Windows treats a connected network as Public (INetworkListManager on a thread of its own, every 15 s;
    // read only, the profile is never changed). nullopt until read, or when it can't be read.
    std::optional<bool> networkPublic() const;

    // ---- Change port... ------------------------------------------------------------------------------------------
    // The prompt: 1024-65535 (v3 §8.1); nullopt when cancelled.
    std::optional<uint16_t> askPort(HWND owner, uint16_t current);

    // Window procedures (static trampolines call these).
    LRESULT inviteProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT reshareProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT logProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    int S(int v) const { return MulDiv(v, env_.dpi, 96); }
    HWND child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id);
    void changed(const std::wstring& note);
    void closeInviteWindow(const std::string& because, const std::wstring& note);
    void updateInviteCountdown();
    // private_data: an invite link (its live token): the clipboard is told to keep it out of clipboard history, the
    // cloud clipboard and clipboard monitors (privateClipboardFormats).
    void copyToClipboard(HWND owner, const std::string& text, bool private_data);
    void startProfileWatch();
    void stopProfileWatch();

    Env env_;
    // The invite window.
    HWND inv_wnd_ = nullptr;
    HWND inv_text_ = nullptr, inv_countdown_ = nullptr, inv_warning_ = nullptr, inv_role_ = nullptr;
    OutsideInvitePanelView inv_;
    std::string inv_shown_; // the link as shown and drawn: inv_.uri, or with the token masked in a test run
    bool inv_masked_ = false;
    std::string inv_closed_because_;
    std::wstring inv_closed_note_;
    // The re-share window.
    HWND rs_wnd_ = nullptr;
    std::string rs_link_;
    bool rs_ack_ = false;
    int acknowledged_ = 0;
    std::string acknowledged_host_;
    // The connection log window.
    HWND log_wnd_ = nullptr, log_list_ = nullptr, log_summary_ = nullptr;
    // The network profile watch (pet-camera.md §7).
    std::thread profile_thread_;
    std::mutex profile_mu_;
    std::condition_variable profile_cv_;
    bool profile_stop_ = false;
    std::atomic<int> profile_public_{-1}; // -1 unknown, 0 not public, 1 public
};

} // namespace kvwin
