// Direct3D 11 preview surface for a child HWND: uploads BGRA8 frames and draws them aspect-fit.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace kvwin {

class D3DPreview {
public:
    bool init(HWND hwnd, std::string* out_error);
    void shutdown();

    // Uploads a new BGRA8 frame (stride in bytes). Recreates the texture on size change.
    void upload(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride);
    void clearFrame();

    // Resizes the swap chain to the window's client size.
    void resize();

    // Draws the current frame (or a dark background) and presents. nearest=true keeps pixels crisp.
    // image_rect (x, y, w, h in client pixels, may reach past the window: zoom and pan) places the
    // frame; null draws it aspect-fit.
    void render(bool nearest, const float* image_rect = nullptr);
    // What's drawn around (or instead of) the frame; dark grey by default. The second display uses black.
    void setBackground(float r, float g, float b) { bg_[0] = r; bg_[1] = g; bg_[2] = b; }
    // Present's sync interval: 1 (vsync, the default) or 0 (the second display, so its presents never
    // wait on another monitor's refresh inside the UI loop; flip-model windows don't tear).
    void setSyncInterval(UINT n) { sync_ = n; }
    bool hasFrame() const { return has_frame_; }
    // Shows the last uploaded frame again after clearFrame (the second display's black-out).
    void restoreFrame() { has_frame_ = tex_ != nullptr; }
    UINT clientWidth() const { return bb_w_; }
    UINT clientHeight() const { return bb_h_; }


    uint32_t frameWidth() const { return tex_w_; }
    uint32_t frameHeight() const { return tex_h_; }

private:
    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> point_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linear_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t tex_w_ = 0;
    uint32_t tex_h_ = 0;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    bool has_frame_ = false;
    float bg_[4] = {0.07f, 0.07f, 0.08f, 1.0f};
    UINT sync_ = 1;

    bool createTargets();
};

} // namespace kvwin
