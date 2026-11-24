// Test-only H.264 decoder (Microsoft's software MFT). It plays the iPhone's part in the tests: decode
// what the host encoded, then read the luma plane the way PocketStreamClientCore does.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

namespace kvtest {

class TestH264Decoder {
public:
    ~TestH264Decoder() { close(); }

    bool open() {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
        started_ = true;
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)))) return false;
        Microsoft::WRL::ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft_.As(&api))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 1;
            api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }
        Microsoft::WRL::ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(mft_->SetInputType(0, it.Get(), 0))) return false;
        if (!setNv12Output()) return false;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void close() {
        mft_.Reset();
        if (started_) {
            MFShutdown();
            started_ = false;
        }
    }

    // Feeds one Annex-B access unit; returns true when a decoded frame is available in luma().
    bool decode(const uint8_t* annexb, size_t n) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        Microsoft::WRL::ComPtr<IMFSample> s;
        MFCreateMemoryBuffer(static_cast<DWORD>(n), &buf);
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        std::memcpy(p, annexb, n);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(n));
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(pts_);
        s->SetSampleDuration(333333);
        pts_ += 333333;
        if (FAILED(mft_->ProcessInput(0, s.Get(), 0))) return false;
        bool got = false;
        for (int i = 0; i < 4; ++i) {
            const int r = pull();
            if (r == 1) got = true;
            else if (r == 2) continue;
            else break;
        }
        return got;
    }

    // End of stream: asks for every frame the decoder still holds. It may hold even a lone IDR until more input
    // comes, and a frozen view sends no more. Returns how many frames came out; the last is in luma(). The
    // decoder takes new input again afterwards.
    int drain() {
        if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) return 0;
        int got = 0;
        for (int i = 0; i < 8; ++i) {
            const int r = pull();
            if (r == 1) ++got;
            else if (r == 2) continue;
            else break;
        }
        return got;
    }

    const std::vector<uint8_t>& luma() const { return y_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

private:
    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    bool started_ = false;
    int64_t pts_ = 0;
    std::vector<uint8_t> y_;
    uint32_t w_ = 0, h_ = 0;

    bool setNv12Output() {
        for (DWORD i = 0;; ++i) {
            Microsoft::WRL::ComPtr<IMFMediaType> t;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                if (FAILED(mft_->SetOutputType(0, t.Get(), 0))) return false;
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                w_ = w;
                h_ = h;
                return true;
            }
        }
    }

    // 1 = frame, 0 = need more input, 2 = format changed, -1 = error
    int pull() {
        MFT_OUTPUT_STREAM_INFO osi{};
        mft_->GetOutputStreamInfo(0, &osi);
        Microsoft::WRL::ComPtr<IMFSample> s;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> b;
        MFCreateSample(&s);
        MFCreateMemoryBuffer(osi.cbSize ? osi.cbSize : 4 * 1024 * 1024, &b);
        s->AddBuffer(b.Get());
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.pSample = s.Get();
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return 0;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) return setNv12Output() ? 2 : -1;
        if (FAILED(hr)) return -1;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> cb;
        s->ConvertToContiguousBuffer(&cb);
        Microsoft::WRL::ComPtr<IMF2DBuffer> b2;
        BYTE* p = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        bool locked2d = SUCCEEDED(cb.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch));
        if (!locked2d) {
            if (FAILED(cb->Lock(&p, nullptr, &len))) return -1;
            pitch = static_cast<LONG>(w_);
        }
        y_.resize(static_cast<size_t>(w_) * h_);
        for (uint32_t r = 0; r < h_; ++r) std::memcpy(&y_[static_cast<size_t>(r) * w_], p + static_cast<size_t>(r) * pitch, w_);
        if (locked2d) b2->Unlock2D();
        else cb->Unlock();
        return 1;
    }
};

// AVCC (4-byte lengths) + parameter sets -> Annex-B, as the phone's format description does implicitly.
inline std::vector<uint8_t> avccToAnnexB(const uint8_t* avcc, size_t n, const std::vector<std::vector<uint8_t>>& param_sets) {
    std::vector<uint8_t> out;
    const uint8_t sc[4] = {0, 0, 0, 1};
    for (const auto& ps : param_sets) {
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), ps.begin(), ps.end());
    }
    size_t i = 0;
    while (i + 4 <= n) {
        const uint32_t len = (static_cast<uint32_t>(avcc[i]) << 24) | (static_cast<uint32_t>(avcc[i + 1]) << 16) |
                             (static_cast<uint32_t>(avcc[i + 2]) << 8) | avcc[i + 3];
        i += 4;
        if (i + len > n) break;
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), avcc + i, avcc + i + len);
        i += len;
    }
    return out;
}

} // namespace kvtest
