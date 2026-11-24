// The second-display (projector) output (the Mac's phase 4.2, SecondDisplayOutputController.swift):
// a borderless window over a whole display showing the current view and nothing
// else, aspect-fit on black, with no controls, cursor readout or tool drawings on it. It never takes
// the keyboard or the mouse, so the main window keeps every control and the B and Esc keys. Black
// out (B) turns it black at once, until B again.
//
// Live only: it draws to the screen; nothing it shows is kept or written anywhere.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "d3d_preview.hpp"

#include <cstdint>
#include <string>

namespace kvwin {

class SecondDisplayOutput {
public:
    ~SecondDisplayOutput() { stop(); }
    // Opens the window over rect (a display's, in virtual-screen pixels), or moves it there.
    bool start(HINSTANCE inst, const RECT& rect, std::string* out_error);
    void stop();
    bool active() const { return hwnd_ != nullptr; }
    HWND window() const { return hwnd_; }

    // The clean frame (BGRA8); drawn at once unless blacked out.
    void showFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride, bool nearest);
    // No picture (the sensor stopped): black.
    void clearFrame();
    // Black out or back, at once (a present right away, not at the next frame).
    void setBlackout(bool on);
    bool blackout() const { return blackout_; }

    uint64_t framesShown() const { return frames_; }    // frames drawn while live
    uint64_t blackPresents() const { return blacks_; }  // presents while blacked out
    uint64_t lastFrameHash() const { return last_hash_; } // of the last frame given (for the tests)
    bool clickThrough() const;                           // layered + transparent: the mouse goes through
    bool hasFrame() const { return d3d_.hasFrame(); }    // a picture to show (black-out aside)

    void present(); // draws what it has now (also on WM_PAINT)

private:
    HWND hwnd_ = nullptr;
    D3DPreview d3d_;
    bool blackout_ = false;
    bool nearest_ = true;
    uint64_t frames_ = 0, blacks_ = 0;
    uint64_t last_hash_ = 0;
};

// FNV-1a over a frame's visible bytes (not the stride padding).
uint64_t frameHash(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride);

} // namespace kvwin
