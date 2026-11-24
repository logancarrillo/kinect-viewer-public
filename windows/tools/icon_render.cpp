#include "icon_render.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1_3.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace kvicon {

namespace {

constexpr UINT kMaster = 1024; // the SVG's own viewBox

std::string hr(const char* what, HRESULT h) {
    char b[160];
    std::snprintf(b, sizeof(b), "%s failed (0x%08lX)", what, static_cast<unsigned long>(h));
    return b;
}

// One size: scale the master (premultiplied) with WIC's high-quality filter, then straight alpha.
bool scaleAndEncode(IWICImagingFactory* wic, IWICBitmap* master, int size, IconImage& img, std::string* err) {
    ComPtr<IWICBitmapScaler> scaler;
    HRESULT h = wic->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(h)) h = scaler->Initialize(master, size, size, WICBitmapInterpolationModeHighQualityCubic);
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(h)) h = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(h)) {
        h = conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
    }
    if (FAILED(h)) {
        if (err) *err = hr("scaling the icon", h);
        return false;
    }
    img.size = size;
    img.bgra.resize(static_cast<size_t>(size) * size * 4);
    h = conv->CopyPixels(nullptr, static_cast<UINT>(size) * 4, static_cast<UINT>(img.bgra.size()), img.bgra.data());
    if (FAILED(h)) {
        if (err) *err = hr("reading the icon pixels", h);
        return false;
    }
    // PNG through WIC, into memory.
    ComPtr<IStream> mem;
    h = CreateStreamOnHGlobal(nullptr, TRUE, &mem);
    ComPtr<IWICBitmapEncoder> enc;
    if (SUCCEEDED(h)) h = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(h)) h = enc->Initialize(mem.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(h)) h = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(h)) h = frame->Initialize(nullptr);
    if (SUCCEEDED(h)) h = frame->SetSize(size, size);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(h)) h = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(h) && fmt != GUID_WICPixelFormat32bppBGRA) h = E_FAIL;
    if (SUCCEEDED(h)) {
        h = frame->WritePixels(size, static_cast<UINT>(size) * 4, static_cast<UINT>(img.bgra.size()), img.bgra.data());
    }
    if (SUCCEEDED(h)) h = frame->Commit();
    if (SUCCEEDED(h)) h = enc->Commit();
    HGLOBAL g = nullptr;
    if (SUCCEEDED(h)) h = GetHGlobalFromStream(mem.Get(), &g);
    STATSTG st{};
    if (SUCCEEDED(h)) h = mem->Stat(&st, STATFLAG_NONAME);
    if (FAILED(h)) {
        if (err) *err = hr("encoding the icon PNG", h);
        return false;
    }
    const auto* p = static_cast<const uint8_t*>(GlobalLock(g));
    img.png.assign(p, p + static_cast<size_t>(st.cbSize.QuadPart));
    GlobalUnlock(g);
    return true;
}

} // namespace

bool isLogoPath(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p) c = (c == L'/') ? L'\\' : static_cast<wchar_t>(towlower(c));
    const std::wstring tail = L"\\design\\logo.svg";
    return p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0;
}

const std::vector<int>& iconSizes() {
    static const std::vector<int> s = {256, 128, 96, 64, 48, 40, 32, 24, 20, 16};
    return s;
}

bool renderSvgIcons(const std::wstring& svg_path, std::vector<IconImage>& out, std::string* err) {
    out.clear();
    if (!isLogoPath(svg_path)) {
        if (err) *err = "refused: only design/logo.svg is rendered";
        return false;
    }
    ComPtr<IWICImagingFactory> wic;
    HRESULT h = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(h)) {
        if (err) *err = hr("WIC", h);
        return false;
    }
    ComPtr<IWICBitmap> master;
    h = wic->CreateBitmap(kMaster, kMaster, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &master);
    ComPtr<ID2D1Factory> d2d;
    if (SUCCEEDED(h)) h = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    ComPtr<ID2D1RenderTarget> rt;
    if (SUCCEEDED(h)) {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        h = d2d->CreateWicBitmapRenderTarget(master.Get(), props, &rt);
    }
    if (FAILED(h)) {
        if (err) *err = hr("the Direct2D target", h);
        return false;
    }
    ComPtr<ID2D1DeviceContext5> dc; // SVG documents: Windows 10 1703 and later
    if (FAILED(rt.As(&dc))) {
        if (err) *err = "this Windows has no Direct2D SVG renderer (ID2D1DeviceContext5)";
        return false;
    }
    ComPtr<IStream> file;
    h = SHCreateStreamOnFileEx(svg_path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &file);
    if (FAILED(h)) {
        if (err) *err = hr("opening the SVG", h);
        return false;
    }
    ComPtr<ID2D1SvgDocument> doc;
    h = dc->CreateSvgDocument(file.Get(), D2D1::SizeF(static_cast<float>(kMaster), static_cast<float>(kMaster)), &doc);
    if (FAILED(h)) {
        if (err) *err = hr("parsing the SVG", h);
        return false;
    }
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent outside the rounded square
    dc->DrawSvgDocument(doc.Get());
    h = dc->EndDraw();
    if (FAILED(h)) {
        if (err) *err = hr("drawing the SVG", h);
        return false;
    }
    for (int size : iconSizes()) {
        IconImage img;
        if (!scaleAndEncode(wic.Get(), master.Get(), size, img, err)) return false;
        out.push_back(std::move(img));
    }
    return true;
}

std::vector<uint8_t> makeIco(const std::vector<IconImage>& images) {
    std::vector<uint8_t> f;
    auto u16 = [&](uint16_t v) {
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        f.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    u16(0); // reserved
    u16(1); // an icon
    u16(static_cast<uint16_t>(images.size()));
    uint32_t offset = 6 + 16 * static_cast<uint32_t>(images.size());
    for (const auto& img : images) {
        f.push_back(static_cast<uint8_t>(img.size >= 256 ? 0 : img.size)); // width (0 is 256)
        f.push_back(static_cast<uint8_t>(img.size >= 256 ? 0 : img.size)); // height
        f.push_back(0);                                                    // no palette
        f.push_back(0);                                                    // reserved
        u16(1);                                                            // planes
        u16(32);                                                           // bits per pixel
        u32(static_cast<uint32_t>(img.png.size()));
        u32(offset);
        offset += static_cast<uint32_t>(img.png.size());
    }
    for (const auto& img : images) f.insert(f.end(), img.png.begin(), img.png.end());
    return f;
}

} // namespace kvicon
