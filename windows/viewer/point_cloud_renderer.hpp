// The 3D point cloud on Direct3D 11 (the Mac's MetalPointCloudRenderer, macos/Rendering3D):
// every pixel of the 512x424 undistorted depth is a point, placed in the shader with the
// depth camera's own intrinsics, x = (col + 0.5 - cx) / fx * z and y = -(row + 0.5 - cy) / fy * z,
// coloured from the core's BGRA point colours, drawn as a square (round from 2.5 px) of the chosen
// size with a depth test, on black. It renders offscreen at 960x720 and reads the picture back:
// the viewer shows it, and the phone gets the same picture encoded (as from the Mac).
//
// Memory only: nothing it draws or reads goes to disk.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "kinect_core_c.h"
#include "point_cloud_camera.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kvwin {

class PointCloudRenderer {
public:
    static constexpr uint32_t kWidth = 960, kHeight = 720;     // the Mac's default render size
    static constexpr uint32_t kDepthW = 512, kDepthH = 424;

    // Its own device (hardware; WARP when there's no hardware device). False with a reason.
    bool init(std::string* out_error);
    void shutdown();
    bool ready() const { return device_ != nullptr; }
    bool usingWarp() const { return warp_; }

    // depth_mm: 512x424 undistorted depth (0 = no point); point_bgra: 512x424 BGRA8. out: width x
    // height BGRA8, stride width * 4. False, with why, when the intrinsics aren't usable (nothing is
    // drawn then) or the GPU failed.
    bool render(const float* depth_mm, const uint8_t* point_bgra, const kv_ir_intrinsics_t& intr, const PointCloudCamera& cam,
                float point_size_px, bool mirror, bool rotate180, uint32_t width, uint32_t height, std::vector<uint8_t>& out,
                std::wstring* why);
    double lastRenderMs() const { return last_ms_; }
    // Times the renderer started again after losing its device (a driver update or reset, a removed
    // GPU: DXGI_ERROR_DEVICE_REMOVED and friends), or after a shutdown.
    int recoveries() const { return recoveries_; }

private:
    bool ensureTargets(uint32_t w, uint32_t h);
    // A removed or missing device: shutdown(), then init() again (at most once a second when it keeps
    // failing). True when there's a working device afterwards.
    bool recover(std::wstring* why);
    bool deviceLost() const;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_in_, color_in_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_srv_, color_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_, ds_, staging_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> dss_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_;
    uint32_t rt_w_ = 0, rt_h_ = 0;
    bool warp_ = false;
    double last_ms_ = 0.0;
    bool ever_ready_ = false;
    int recoveries_ = 0;
    uint64_t last_failed_ms_ = 0; // GetTickCount64 of the last init that failed

};

} // namespace kvwin
