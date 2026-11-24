#include "window_capture.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace kvwin {

bool captureWindowToPng(HWND hwnd, const std::wstring& path, std::string* out_error, size_t* out_lit, size_t* out_total) {
    if (out_lit) *out_lit = 0;
    if (out_total) *out_total = 0;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        if (out_error) *out_error = "window has no size";
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    // PW_RENDERFULLCONTENT (2) asks DWM for the composed content, which includes the D3D swap chain.
    const BOOL ok = PrintWindow(hwnd, mem, 2);
    GdiFlush();

    bool saved = false;
    if (ok && bits) {
        // Force opaque alpha so viewers do not show the DIB's undefined alpha.
        auto* px = static_cast<uint8_t*>(bits);
        size_t lit = 0;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            px[i * 4 + 3] = 255;
            lit += (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) != 0;
        }
        if (out_lit) *out_lit = lit;
        if (out_total) *out_total = static_cast<size_t>(w) * h;

        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
        if (SUCCEEDED(hr)) hr = frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w) * 4,
                                                   static_cast<UINT>(w) * 4 * static_cast<UINT>(h), static_cast<BYTE*>(bits));
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        saved = SUCCEEDED(hr);
        if (!saved && out_error) *out_error = "WIC PNG encode failed";
    } else if (out_error) {
        *out_error = "PrintWindow failed";
    }

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return saved;
}

} // namespace kvwin
