#include "point_cloud_renderer.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace kvwin {

namespace {

// One instance per depth pixel, six vertices (two triangles) per instance: the point's square.
// A pixel without depth is put outside the clip volume (z/w = 2), so it draws nothing.
const char kShaderSrc[] = R"(
Texture2D<float> depthTex : register(t0);
Texture2D<float4> colorTex : register(t1);
cbuffer Params : register(b0) {
    float4x4 mvp;   // column-major, as the C++ side lays it out
    float4 intr;    // fx, fy, cx, cy
    float4 opts;    // x sign, y sign, point size (px), round (1/0)
    float4 vp;      // render width, height (px)
};
struct VSOut {
    float4 pos : SV_Position;
    float4 col : COLOR0;
    float2 corner : TEXCOORD0;
};
static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(-1, 1),
    float2(-1, 1), float2(1, -1), float2(1, 1)
};
VSOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    VSOut o;
    const uint col = iid % 512u;
    const uint row = iid / 512u;
    const float d = depthTex.Load(int3(col, row, 0));
    const float2 c = kCorners[vid];
    o.corner = c;
    o.col = float4(0, 0, 0, 1);
    if (!(d > 0.0)) {
        o.pos = float4(0, 0, 2, 1);
        return o;
    }
    const float z = d * 0.001;
    const float x = ((float(col) + 0.5 - intr.z) / intr.x) * z * opts.x;
    const float y = -((float(row) + 0.5 - intr.w) / intr.y) * z * opts.y;
    float4 p = mul(mvp, float4(x, y, -z, 1.0));
    // Half the point size in NDC is size / viewport (one pixel is 2 / viewport); times w so it stays
    // that many pixels after the divide.
    p.xy += c * (opts.z / vp.xy) * p.w;
    o.pos = p;
    o.col = float4(colorTex.Load(int3(col, row, 0)).rgb, 1.0);
    return o;
}
float4 ps_main(VSOut i) : SV_Target {
    if (opts.w > 0.5 && dot(i.corner, i.corner) > 1.0) discard;
    return i.col;
}
)";

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string* err) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "kv_point_cloud", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
    if (FAILED(hr)) {
        if (err) {
            *err = std::string("point cloud shader compile failed: ") +
                   (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
        }
        return false;
    }
    return true;
}

struct alignas(16) Params {
    float mvp[16];
    float intr[4];
    float opts[4];
    float vp[4];
};

bool dynamicTexture(ID3D11Device* dev, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = PointCloudRenderer::kDepthW;
    td.Height = PointCloudRenderer::kDepthH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv));
}

bool uploadRows(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const void* src, size_t row_bytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    const auto* s = static_cast<const uint8_t*>(src);
    auto* d = static_cast<uint8_t*>(m.pData);
    for (uint32_t y = 0; y < PointCloudRenderer::kDepthH; ++y) {
        std::memcpy(d + static_cast<size_t>(y) * m.RowPitch, s + static_cast<size_t>(y) * row_bytes, row_bytes);
    }
    ctx->Unmap(tex, 0);
    return true;
}

} // namespace

bool PointCloudRenderer::init(std::string* out_error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    warp_ = false;
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, static_cast<UINT>(std::size(levels)),
                               D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
        warp_ = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
        if (out_error) *out_error = "D3D11CreateDevice (feature level 11) failed for the 3D view";
        return false;
    }
    ComPtr<ID3DBlob> vsb, psb;
    if (!compile("vs_main", "vs_5_0", vsb, out_error) || !compile("ps_main", "ps_5_0", psb, out_error)) {
        shutdown();
        return false;
    }
    bool ok = SUCCEEDED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) &&
              SUCCEEDED(device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device_->CreateBuffer(&bd, nullptr, &cb_));
    ok = ok && dynamicTexture(device_.Get(), DXGI_FORMAT_R32_FLOAT, depth_in_, depth_srv_);
    ok = ok && dynamicTexture(device_.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, color_in_, color_srv_);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    ok = ok && SUCCEEDED(device_->CreateDepthStencilState(&dsd, &dss_));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device_->CreateRasterizerState(&rd, &rs_));
    if (!ok) {
        if (out_error) *out_error = "creating the 3D view's GPU resources failed";
        shutdown();
        return false;
    }
    ever_ready_ = true;
    return true;
}

bool PointCloudRenderer::deviceLost() const {
    return device_ && FAILED(device_->GetDeviceRemovedReason());
}

bool PointCloudRenderer::recover(std::wstring* why) {
    const uint64_t now = GetTickCount64();
    if (last_failed_ms_ != 0 && now - last_failed_ms_ < 1000) { // still failing: not every frame
        if (why) *why = L"The 3D view's GPU device was lost; trying again.";
        return false;
    }
    const bool had = device_ != nullptr || ever_ready_;
    shutdown();
    std::string err;
    if (!init(&err)) {
        last_failed_ms_ = now;
        if (why) *why = L"The 3D view's GPU device was lost and couldn't be made again: " + std::wstring(err.begin(), err.end());
        return false;
    }
    last_failed_ms_ = 0;
    if (had) ++recoveries_;
    return true;
}

void PointCloudRenderer::shutdown() {
    if (ctx_) ctx_->ClearState();
    rtv_.Reset();
    dsv_.Reset();
    rt_.Reset();
    ds_.Reset();
    staging_.Reset();
    depth_srv_.Reset();
    color_srv_.Reset();
    depth_in_.Reset();
    color_in_.Reset();
    cb_.Reset();
    dss_.Reset();
    rs_.Reset();
    vs_.Reset();
    ps_.Reset();
    ctx_.Reset();
    device_.Reset();
    rt_w_ = rt_h_ = 0;
}

bool PointCloudRenderer::ensureTargets(uint32_t w, uint32_t h) {
    if (rt_ && rt_w_ == w && rt_h_ == h) return true;
    rtv_.Reset();
    dsv_.Reset();
    rt_.Reset();
    ds_.Reset();
    staging_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &rt_)) || FAILED(device_->CreateRenderTargetView(rt_.Get(), nullptr, &rtv_))) {
        return false;
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) return false;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    td.CPUAccessFlags = 0;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &ds_)) || FAILED(device_->CreateDepthStencilView(ds_.Get(), nullptr, &dsv_))) {
        return false;
    }
    rt_w_ = w;
    rt_h_ = h;
    return true;
}

bool PointCloudRenderer::render(const float* depth_mm, const uint8_t* point_bgra, const kv_ir_intrinsics_t& intr,
                                const PointCloudCamera& cam, float point_size_px, bool mirror, bool rotate180, uint32_t width,
                                uint32_t height, std::vector<uint8_t>& out, std::wstring* why) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!pointCloudIntrinsicsUsable(intr)) { // no fallback values
        if (why) *why = kNo3DIntrinsics;
        return false;
    }
    if (!depth_mm || !point_bgra || width == 0 || height == 0) {
        if (why) *why = L"The 3D view got no input.";
        return false;
    }
    // A device that was removed (driver reset or update, GPU gone) or shut down: make it again.
    if ((!ready() || deviceLost()) && !recover(why)) return false;
    // After a GPU call fails: if the device went, start again for the next frame.
    auto failed = [&](const wchar_t* what) {
        if (deviceLost()) {
            recover(nullptr);
            if (why) *why = L"The 3D view's GPU device was reset; it starts again with the next frame.";
        } else if (why) {
            *why = what;
        }
        return false;
    };
    if (!ensureTargets(width, height) || !uploadRows(ctx_.Get(), depth_in_.Get(), depth_mm, kDepthW * sizeof(float)) ||
        !uploadRows(ctx_.Get(), color_in_.Get(), point_bgra, kDepthW * 4u)) {
        return failed(L"The GPU refused the 3D view's buffers.");
    }

    Params prm{};
    const auto m = cam.viewProjection(static_cast<float>(width) / static_cast<float>(height));
    std::copy(m.begin(), m.end(), prm.mvp);
    prm.intr[0] = intr.fx;
    prm.intr[1] = intr.fy;
    prm.intr[2] = intr.cx;
    prm.intr[3] = intr.cy;
    const float size = clampPointSizePx(point_size_px);
    prm.opts[0] = pointCloudFlipX(mirror, rotate180) ? -1.0f : 1.0f;
    prm.opts[1] = pointCloudFlipY(mirror, rotate180) ? -1.0f : 1.0f;
    prm.opts[2] = size;
    prm.opts[3] = size >= kRoundPointsFromPx ? 1.0f : 0.0f;
    prm.vp[0] = static_cast<float>(width);
    prm.vp[1] = static_cast<float>(height);
    D3D11_MAPPED_SUBRESOURCE cm{};
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        return failed(L"The GPU refused the 3D view's parameters.");
    }
    std::memcpy(cm.pData, &prm, sizeof(prm));
    ctx_->Unmap(cb_.Get(), 0);

    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx_->ClearRenderTargetView(rtv_.Get(), black);
    ctx_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    ctx_->OMSetRenderTargets(1, rtvs, dsv_.Get());
    ctx_->OMSetDepthStencilState(dss_.Get(), 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->RSSetState(rs_.Get());
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {cb_.Get()};
    ctx_->VSSetConstantBuffers(0, 1, cbs);
    ctx_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[] = {depth_srv_.Get(), color_srv_.Get()};
    ctx_->VSSetShaderResources(0, 2, srvs);
    ctx_->DrawInstanced(6, kDepthW * kDepthH, 0, 0);
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx_->VSSetShaderResources(0, 2, none);

    ctx_->CopyResource(staging_.Get(), rt_.Get());
    D3D11_MAPPED_SUBRESOURCE rm{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &rm))) {
        return failed(L"Reading the 3D view back from the GPU failed.");
    }
    const size_t row = static_cast<size_t>(width) * 4u;
    out.resize(row * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + y * row, static_cast<const uint8_t*>(rm.pData) + static_cast<size_t>(y) * rm.RowPitch, row);
    }
    ctx_->Unmap(staging_.Get(), 0);
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace kvwin
