#include "d3d_preview.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace kvwin {

namespace {

// Full-screen triangle from SV_VertexID; the viewport does the aspect fit.
const char kShaderSrc[] = R"(
Texture2D tex0 : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
float4 ps_main(VSOut i) : SV_Target {
    return float4(tex0.Sample(smp, i.uv).rgb, 1.0);
}
)";

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string* err) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "kv_preview", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
    if (FAILED(hr)) {
        if (err) {
            *err = std::string("shader compile failed: ") +
                   (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
        }
        return false;
    }
    return true;
}

} // namespace

bool D3DPreview::init(HWND hwnd, std::string* out_error) {
    hwnd_ = hwnd;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    }
    if (FAILED(hr)) {
        if (out_error) *out_error = "D3D11CreateDevice failed";
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_dev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    device_.As(&dxgi_dev);
    dxgi_dev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = std::max<LONG>(1, rc.right - rc.left);
    sd.Height = std::max<LONG>(1, rc.bottom - rc.top);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        if (out_error) *out_error = "CreateSwapChainForHwnd failed";
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3DBlob> vsb, psb;
    if (!compile("vs_main", "vs_5_0", vsb, out_error) || !compile("ps_main", "ps_5_0", psb, out_error)) {
        return false;
    }
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);

    D3D11_SAMPLER_DESC smp{};
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    device_->CreateSamplerState(&smp, &point_);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device_->CreateSamplerState(&smp, &linear_);

    return createTargets();
}

bool D3DPreview::createTargets() {
    rtv_.Reset();
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    bb_w_ = d.Width;
    bb_h_ = d.Height;
    return SUCCEEDED(device_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_));
}

void D3DPreview::shutdown() {
    if (ctx_) ctx_->ClearState();
    srv_.Reset();
    tex_.Reset();
    rtv_.Reset();
    swap_.Reset();
    vs_.Reset();
    ps_.Reset();
    point_.Reset();
    linear_.Reset();
    ctx_.Reset();
    device_.Reset();
}

void D3DPreview::resize() {
    if (!swap_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT w = std::max<LONG>(1, rc.right - rc.left);
    const UINT h = std::max<LONG>(1, rc.bottom - rc.top);
    if (w == bb_w_ && h == bb_h_) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    createTargets();
}

void D3DPreview::upload(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride) {
    if (!device_ || !bgra || width == 0 || height == 0) return;
    if (!tex_ || width != tex_w_ || height != tex_h_) {
        srv_.Reset();
        tex_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex_))) return;
        device_->CreateShaderResourceView(tex_.Get(), nullptr, &srv_);
        tex_w_ = width;
        tex_h_ = height;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(tex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    const size_t row = static_cast<size_t>(width) * 4;
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, bgra + static_cast<size_t>(y) * stride, row);
    }
    ctx_->Unmap(tex_.Get(), 0);
    has_frame_ = true;
}

void D3DPreview::clearFrame() {
    has_frame_ = false;
}

void D3DPreview::render(bool nearest, const float* image_rect) {
    if (!swap_ || !rtv_) return;
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx_->ClearRenderTargetView(rtv_.Get(), bg_);

    if (has_frame_ && srv_ && tex_w_ > 0 && tex_h_ > 0) {
        D3D11_VIEWPORT vp{};
        if (image_rect && image_rect[2] > 0.0f && image_rect[3] > 0.0f) {
            // Zoomed/panned: the viewport may extend past the target (it's clipped); keep it within
            // D3D11's viewport bounds.
            vp.TopLeftX = std::max(-32768.0f, image_rect[0]);
            vp.TopLeftY = std::max(-32768.0f, image_rect[1]);
            vp.Width = std::min(32767.0f, image_rect[2]);
            vp.Height = std::min(32767.0f, image_rect[3]);
        } else {
            const float sx = static_cast<float>(bb_w_) / static_cast<float>(tex_w_);
            const float sy = static_cast<float>(bb_h_) / static_cast<float>(tex_h_);
            const float s = std::min(sx, sy);
            vp.Width = static_cast<float>(tex_w_) * s;
            vp.Height = static_cast<float>(tex_h_) * s;
            vp.TopLeftX = (static_cast<float>(bb_w_) - vp.Width) * 0.5f;
            vp.TopLeftY = (static_cast<float>(bb_h_) - vp.Height) * 0.5f;
        }
        vp.MaxDepth = 1.0f;
        ctx_->RSSetViewports(1, &vp);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, srv_.GetAddressOf());
        ID3D11SamplerState* smp = nearest ? point_.Get() : linear_.Get();
        ctx_->PSSetSamplers(0, 1, &smp);
        ctx_->Draw(3, 0);
    }
    swap_->Present(sync_, 0);
}

} // namespace kvwin
