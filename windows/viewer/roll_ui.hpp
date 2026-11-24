// The camera roll's windows on the PC (pet-camera.md §4, §6): the roll browser (days, then each day's clips
// with thumbnail, time, trigger, duration and star; Play, Star, Delete), the scheduled-recording editor, and the
// player, a window of its own that decodes a clip locally.
//
// This is part of the colour exception (pet-camera.md §1, §6.6): the browser's thumbnails and the player are the
// only places the PC shows colour from the roll. They show it on this PC's screen and nowhere else:
// - The player (RollPlayer) is given nothing but a file path. It has no StreamHooks, no encoder and no view, so
//   nothing it decodes can reach hooks->onFrame, the live tiers, the encoder, the phones or the camera roll.
// - While the player is open or the browser shows thumbnails, the screenshot hook refuses (screenshotAllowed).
// Media Foundation is started only through kvhost::ensureMediaFoundation(), never MFStartup or MFShutdown here.
#pragma once

#include "viewer_app.hpp"

#include <commctrl.h> // HIMAGELIST

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kvwin {

// The player window: one clip, decoded with IMFSourceReader (H.264 to RGB32) on a worker thread of its own and
// shown with GDI. The UI thread never decodes: it asks (open, play, pause, seek) and presents the frames the
// worker hands over, so the viewer's own loop (stepView, the live frames to the host) never waits on a decode,
// even while the owner drags the scrub bar. Seeks coalesce: only the latest position asked for is decoded to (the
// keyframe at or before it, then forward). Media Foundation is used only on the worker, after
// kvhost::ensureMediaFoundation(). Every call is from the UI thread.
class RollPlayer {
public:
    RollPlayer();
    ~RollPlayer();
    RollPlayer(const RollPlayer&) = delete;
    RollPlayer& operator=(const RollPlayer&) = delete;

    // Opens the window (closing any clip open before) and starts the worker on the clip at mp4_path; it plays once
    // the first frame is decoded. False with err only when the window can't open. A clip that can't be opened or
    // decoded closes the window later, with lastError() saying why and onFailed called.
    bool open(HINSTANCE inst, HWND owner, HFONT font, int dpi, const std::wstring& mp4_path, const std::wstring& title,
              std::string* err);
    void close();
    bool isOpen() const { return wnd_ != nullptr; }
    HWND window() const { return wnd_; }

    void play();
    void pause();
    bool playing() const;
    // Asks for the frame at pos_hns (100 ns units). Returns at once; the worker decodes to the latest position
    // asked for and drops the ones it hadn't started.
    bool seek(int64_t pos_hns);
    // A seek is asked for or being decoded.
    bool seekPending() const;
    uint64_t seeksRequested() const { return seeks_requested_; }
    uint64_t seeksCompleted() const;

    int64_t durationHns() const;
    int64_t positionHns() const { return shown_pts_; } // of the frame on screen
    uint64_t framesDecoded() const;                    // by the worker since the clip opened
    uint64_t framesPresented() const { return frames_presented_; }
    uint32_t frameWidth() const { return width_; }
    uint32_t frameHeight() const { return height_; }
    HWND scrubBar() const { return scrub_; } // the test hook drags it as the owner would
    // A frame is on screen (something was decoded since the clip opened).
    bool showingFrame() const { return have_frame_; }
    std::string lastError() const;
    // Called when the window closes (by the user or close()).
    std::function<void()> onClosed;
    // Called (UI thread) when the clip couldn't be opened or decoded, just before the window closes.
    std::function<void(const std::string& err)> onFailed;

    LRESULT proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Worker;
    void present();     // the worker's newest frame onto the screen (WM_APP from the worker)
    void stopWorker();  // joins it; the buffers are cleared
    void paint(HWND hwnd);
    void updateControls();
    int S(int v) const { return MulDiv(v, dpi_, 96); }

    std::unique_ptr<Worker> worker_;
    HWND wnd_ = nullptr, btn_play_ = nullptr, scrub_ = nullptr, lbl_time_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    std::vector<uint8_t> frame_; // the frame on screen: top-down BGRX, width_ x height_ (swapped in from the worker)
    uint32_t width_ = 0, height_ = 0;
    int64_t shown_pts_ = 0;
    bool have_frame_ = false;
    uint64_t frames_presented_ = 0;
    uint64_t seeks_requested_ = 0;
    std::string last_error_; // why the last clip failed, kept once the worker is gone
};

class RollUi {
public:
    struct Env {
        HINSTANCE inst = nullptr;
        HWND main = nullptr;
        HFONT font = nullptr;
        HFONT bold = nullptr;
        int dpi = 96;
        StreamHooks* hooks = nullptr;
        // Something changed here (a star, a delete, a schedule row, the player opening or closing): the panel
        // refreshes and the screenshot rule is looked at again.
        std::function<void()> on_changed;
    };

    RollUi();
    ~RollUi();
    RollUi(const RollUi&) = delete;
    RollUi& operator=(const RollUi&) = delete;

    void init(const Env& env);
    void closeAll();

    // ---- the browser ("Camera roll...") ----------------------------------------------------------------------
    void openBrowser();
    void closeBrowser();
    HWND browserWindow() const { return br_wnd_; }
    // The days again, and the selected day's clips (keeps the selections by day and clip id).
    void refreshBrowser();
    int dayRows() const { return static_cast<int>(days_.size()); }
    int clipRows() const { return static_cast<int>(clips_.size()); }
    bool selectDay(int index);
    bool selectClip(const std::string& clipId);
    const std::vector<kvhost::roll::RollClipItem>& clips() const { return clips_; }
    // How many of the shown clips have a thumbnail decoded into the list.
    int thumbnailsShown() const { return thumbs_shown_; }
    // The browser shows clips' thumbnails now (colour from the roll on screen).
    bool showingThumbnails() const { return br_wnd_ && thumbs_shown_ > 0; }
    // The browser's buttons, also used by the --test-camera-roll hook. ask: the delete's confirm dialog.
    bool starSelected();
    bool deleteSelected(bool ask);
    bool playSelected();
    const std::string& lastError() const { return last_error_; }

    // ---- the player --------------------------------------------------------------------------------------------
    RollPlayer& player() { return player_; }
    bool playerOpen() const { return player_.isOpen(); }
    const std::string& playingClip() const { return playing_clip_; }
    // A phone owner is deleting clipId. If the player has it open, the player closes (and so unpins it)
    // and the browser says "This clip was deleted."; true then.
    bool closePlayerForDeletedClip(const std::string& clipId);
    // What the browser last told the owner ("This clip was deleted."), for the test hook.
    const std::wstring& notice() const { return notice_; }

    // ---- "Scheduled recording" rows ------------------------------------------------------------------------------
    void openSchedule();
    HWND scheduleWindow() const { return sched_wnd_; }
    void refreshSchedule();
    // The editor's Add and Remove with its own fields (the test hook fills them first). False with the reason
    // shown in the editor.
    bool addScheduleRowFromEditor();
    bool removeSelectedScheduleRow();
    void setScheduleEditorFields(uint8_t days, const std::wstring& start, const std::wstring& minutes);
    bool selectScheduleRow(int index);

    LRESULT browserProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT scheduleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    int S(int v) const { return MulDiv(v, env_.dpi, 96); }
    HWND child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id);
    void loadDays();
    void loadClips();
    int selectedDay() const;
    int selectedClip() const;
    void updateBrowserButtons();
    void changed();

    Env env_;
    RollPlayer player_;
    std::string playing_clip_; // the clip the player has open
    uint64_t player_pin_ = 0;  // the host's pin on it while the player has it (0: none)
    std::wstring notice_;
    void releasePlayerPin();

    HWND br_wnd_ = nullptr, br_days_ = nullptr, br_clips_ = nullptr;
    HWND br_play_ = nullptr, br_star_ = nullptr, br_delete_ = nullptr, br_refresh_ = nullptr, br_info_ = nullptr;
    HIMAGELIST br_images_ = nullptr;
    std::vector<kvhost::roll::RollDayItem> days_;
    std::vector<kvhost::roll::RollClipItem> clips_;
    std::string shown_day_;
    int thumbs_shown_ = 0;
    std::string last_error_;

    HWND sched_wnd_ = nullptr, sched_list_ = nullptr, sched_start_ = nullptr, sched_minutes_ = nullptr, sched_note_ = nullptr;
    HWND sched_days_[7] = {};
    std::vector<kvhost::RecordScheduleRow> sched_rows_;
};

} // namespace kvwin
