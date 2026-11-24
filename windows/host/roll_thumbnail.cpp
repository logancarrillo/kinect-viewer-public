#include "roll_thumbnail.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace kvhost {

namespace {

constexpr uint32_t kMaxSourceSide = 16384; // far above any view; keeps stride * height well inside 32 bits

void setErr(std::string* err, const char* what, HRESULT hr) {
    if (!err) return;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
    *err = buf;
}

// Balances CoInitializeEx when this call was the one that initialised COM on the thread.
struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr); // S_FALSE (already initialised) also needs its CoUninitialize
    }
    ~ComScope() {
        if (owned) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

// One JPEG of the 24-bit BGR pixels at `quality`.
HRESULT encodeJpeg(IWICImagingFactory* factory, const std::vector<uint8_t>& bgr, uint32_t w, uint32_t h,
                   uint32_t stride, float quality, std::vector<uint8_t>& out) {
    ComPtr<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    ComPtr<IWICBitmapEncoder> enc;
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt = {};
        wchar_t name[] = L"ImageQuality";
        opt.pstrName = name;
        VARIANT v = {}; // VT_EMPTY; zeroing it avoids VariantInit and oleaut32
        v.vt = VT_R4;
        v.fltVal = quality;
        hr = props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(h, stride, static_cast<UINT>(bgr.size()), const_cast<BYTE*>(bgr.data()));
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    STATSTG st = {};
    if (SUCCEEDED(hr)) hr = stream->Stat(&st, STATFLAG_NONAME);
    HGLOBAL mem = nullptr;
    if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.Get(), &mem);
    if (SUCCEEDED(hr)) {
        const size_t n = static_cast<size_t>(st.cbSize.QuadPart);
        const void* p = GlobalLock(mem);
        if (!p) return E_OUTOFMEMORY;
        out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        GlobalUnlock(mem);
    }
    return hr;
}

} // namespace

const std::vector<float>& rollThumbQualitySteps() {
    static const std::vector<float> steps = {0.75f, 0.65f, 0.55f, 0.45f, 0.35f, 0.25f, 0.15f, 0.10f, 0.05f};
    return steps;
}

void rollThumbSize(uint32_t width, uint32_t height, uint32_t* outWidth, uint32_t* outHeight) {
    uint32_t w = width, h = height;
    if (width > kRollThumbMaxWidth) {
        w = kRollThumbMaxWidth;
        h = static_cast<uint32_t>((static_cast<uint64_t>(height) * kRollThumbMaxWidth + width / 2) / width);
        if (h == 0) h = 1;
    }
    if (outWidth) *outWidth = w;
    if (outHeight) *outHeight = h;
}

bool makeRollThumbnail(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t strideBytes,
                       RollThumbnail& out, std::string* err) {
    out = RollThumbnail{};
    if (!bgra || width == 0 || height == 0 || width > kMaxSourceSide || height > kMaxSourceSide ||
        strideBytes < width * 4 || static_cast<uint64_t>(strideBytes) * height > 0x7FFFFFFFull) {
        if (err) *err = "not a BGRA frame";
        return false;
    }
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        setErr(err, "WIC unavailable", hr);
        return false;
    }

    // Scale once (Fant: an area average, right for shrinking) and convert to the 24-bit BGR the JPEG
    // encoder takes, dropping alpha; every quality step then encodes the same pixels.
    ComPtr<IWICBitmap> src;
    hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, strideBytes,
                                         strideBytes * height, const_cast<BYTE*>(bgra), &src);
    uint32_t tw = 0, th = 0;
    rollThumbSize(width, height, &tw, &th);
    ComPtr<IWICBitmapSource> scaled;
    if (SUCCEEDED(hr)) {
        if (tw == width && th == height) {
            scaled = src;
        } else {
            ComPtr<IWICBitmapScaler> scaler;
            hr = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hr)) hr = scaler->Initialize(src.Get(), tw, th, WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr)) scaled = scaler;
        }
    }
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) {
        hr = conv->Initialize(scaled.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
    }
    const uint32_t bgrStride = (tw * 3 + 3) & ~3u;
    std::vector<uint8_t> bgr(static_cast<size_t>(bgrStride) * th);
    if (SUCCEEDED(hr)) hr = conv->CopyPixels(nullptr, bgrStride, static_cast<UINT>(bgr.size()), bgr.data());
    if (FAILED(hr)) {
        setErr(err, "scaling the frame failed", hr);
        return false;
    }

    // §6.1: quality 0.75, lowered until the JPEG is at most 64 KiB.
    std::vector<uint8_t> jpeg;
    for (const float q : rollThumbQualitySteps()) {
        hr = encodeJpeg(factory.Get(), bgr, tw, th, bgrStride, q, jpeg);
        if (FAILED(hr)) {
            setErr(err, "JPEG encoding failed", hr);
            return false;
        }
        if (jpeg.size() <= kRollThumbMaxBytes) {
            out.jpeg = std::move(jpeg);
            out.width = tw;
            out.height = th;
            out.quality = q;
            return true;
        }
    }
    if (err) *err = "the thumbnail is over 64 KiB even at the lowest quality";
    return false;
}

} // namespace kvhost
