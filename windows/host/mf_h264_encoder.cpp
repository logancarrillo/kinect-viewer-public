#include "mf_h264_encoder.hpp"

#include "mf_platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace kvhost {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string hrText(const char* what, HRESULT hr) {
    char b[160];
    std::snprintf(b, sizeof(b), "%s failed (hr=0x%08lx)", what, static_cast<unsigned long>(hr));
    return b;
}

void setUi4(ICodecAPI* api, const GUID& g, ULONG v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&g, &var);
}

void setBool(ICodecAPI* api, const GUID& g, bool v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&g, &var);
}

void setColorAttrs(IMFMediaType* t) {
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

// A closed encoder's COM objects are parked here and released a few seconds later, never at once. A hardware
// encoder MFT (a GPU vendor's) can still have a work item queued on an MF work queue right after it was
// shut down; releasing the last reference at once let that item run on a freed object (an access violation in
// EnterCriticalSection on an RTWorkQ thread). An earlier fix removed the commonest trigger (MFShutdown right after the last
// close, see mf_platform.cpp), but the release itself still raced under load. So closing now drains the MFT, shuts
// its activation object down, and parks it here: entries are released by a later close, on that closing thread
// (which has COM), only once they have been parked for kParkFor; a newer one is never released early, even when more
// than kParkCap are parked (that is logged). The list is never destroyed, so nothing is released during static
// destruction at exit. Parking, releasing and drain timeouts go to the private debug log only (OutputDebugString).
struct ParkedEncoder {
    ComPtr<IMFTransform> mft;
    ComPtr<ICodecAPI> codec;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFActivate> activate;
    std::string name;
    std::chrono::steady_clock::time_point at;
};
constexpr auto kParkFor = std::chrono::seconds(3);
constexpr size_t kParkCap = 8;
std::mutex g_park_mutex;
EncoderCloseStats g_close_stats; // under g_park_mutex
std::vector<ParkedEncoder>& parked() {
    static auto* v = new std::vector<ParkedEncoder>(); // intentionally never freed (see above)
    return *v;
}

// The private log: a local debugger only; nothing is written to disk or sent anywhere.
void privateLog(const std::string& line) {
    const std::string s = "[kv-encoder] " + line + "@N";
    OutputDebugStringA(s.c_str());
}

void parkEncoder(ParkedEncoder p) {
    std::vector<ParkedEncoder> old; // released after the lock is let go
    {
        std::lock_guard<std::mutex> lk(g_park_mutex);
        auto& v = parked();
        const auto now = std::chrono::steady_clock::now();
        // Oldest first (the list is in parking order); only entries parked for kParkFor or longer.
        while (!v.empty() && now - v.front().at >= kParkFor) {
            old.push_back(std::move(v.front()));
            v.erase(v.begin());
        }
        p.at = now;
        privateLog("parked " + p.name);
        v.push_back(std::move(p));
        ++g_close_stats.parked;
        g_close_stats.released += old.size();
        if (v.size() > kParkCap) {
            ++g_close_stats.overCap;
            privateLog("more than " + std::to_string(kParkCap) + " closed encoders parked (" + std::to_string(v.size()) +
                       "); none is old enough to release yet");
        }
        g_close_stats.parkedNow = v.size();
    }
    if (old.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> names;
    for (auto& o : old) names.push_back(o.name);
    old.clear(); // the releases themselves
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> lk(g_park_mutex);
        if (ms > g_close_stats.releaseMsMax) g_close_stats.releaseMsMax = ms;
    }
    for (auto& n : names) privateLog("released " + n);
}

} // namespace

struct MfH264Encoder::Impl {
    ComPtr<IMFTransform> mft;
    ComPtr<ICodecAPI> codec;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFActivate> activate; // kept so ShutdownObject can be called at close, as hardware MFTs need
    bool async = false;
    bool hardware = false;
    std::string name;
    uint32_t w = 0, h = 0;
    EncoderOptions opts;
    OutputFn out;
    DWORD in_id = 0, out_id = 0;
    MFT_OUTPUT_STREAM_INFO osi{};

    std::mutex m; // serialises MFT calls and the pending slot
    int credits = 0;
    struct Pending {
        std::vector<uint8_t> nv12;
        int64_t pts = 0;
        bool key = false;
        uint64_t token = 0;
        bool valid = false;
    } pending;
    std::map<int64_t, uint64_t> tokens;
    std::thread ev_thread;
    std::atomic<bool> stopping{false};
    // Closing an async MFT: it is drained first (MFT_MESSAGE_COMMAND_DRAIN, then METransformDrainComplete), so it
    // is idle with nothing in flight before it is shut down. Output that comes out meanwhile is dropped.
    std::atomic<bool> draining{false};
    bool drained = false; // under m
    std::condition_variable drain_cv;

    ~Impl() { shutdown(); }

    void shutdown() {
        const auto t_close = std::chrono::steady_clock::now();
        if (mft && async && ev_thread.joinable()) {
            const auto t_drain = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lk(m);
            draining = true;
            pending.valid = false;
            drained = false;
            if (SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)) &&
                !drain_cv.wait_for(lk, std::chrono::milliseconds(1000), [&] { return drained; })) {
                // Still shut down and parked as usual; never released at once.
                privateLog("drain of " + name + " didn't complete within 1 s");
                std::lock_guard<std::mutex> plk(g_park_mutex);
                ++g_close_stats.drainTimeouts;
            }
            const double dms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_drain).count();
            std::lock_guard<std::mutex> plk(g_park_mutex);
            if (dms > g_close_stats.drainMsMax) g_close_stats.drainMsMax = dms;
        }
        stopping = true;
        if (mft) {
            {
                std::lock_guard<std::mutex> lk(m); // never while the event thread is inside ProcessInput/Output
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            }
            ComPtr<IMFShutdown> sd;
            if (SUCCEEDED(mft.As(&sd))) sd->Shutdown(); // unblocks GetEvent with MF_E_SHUTDOWN
        }
        if (ev_thread.joinable()) ev_thread.join();
        if (activate) activate->ShutdownObject();
        if (mft || codec || events || activate) {
            // Released a few seconds from now, not here (see parkEncoder).
            ParkedEncoder p;
            p.mft = std::move(mft);
            p.codec = std::move(codec);
            p.events = std::move(events);
            p.activate = std::move(activate);
            p.name = name;
            parkEncoder(std::move(p));
        }
        const double cms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_close).count();
        std::lock_guard<std::mutex> plk(g_park_mutex);
        if (cms > g_close_stats.closeMsMax) g_close_stats.closeMsMax = cms;
    }

    bool configure(std::string* err) {
        ComPtr<IMFAttributes> attrs;
        UINT32 is_async = 0;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
            if (is_async) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        async = is_async != 0;

        mft.As(&codec);
        setUi4(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setUi4(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, opts.bitrate);
        setBool(codec.Get(), CODECAPI_AVLowLatencyMode, true);
        setUi4(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setUi4(codec.Get(), CODECAPI_AVEncMPVGOPSize, opts.gop);

        DWORD nin = 0, nout = 0;
        if (SUCCEEDED(mft->GetStreamCount(&nin, &nout)) && nin == 1 && nout == 1) {
            if (mft->GetStreamIDs(1, &in_id, 1, &out_id) == E_NOTIMPL) {
                in_id = 0;
                out_id = 0;
            }
        }

        ComPtr<IMFMediaType> ot;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, opts.bitrate);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, opts.fps, 1);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        setColorAttrs(ot.Get());
        HRESULT hr = mft->SetOutputType(out_id, ot.Get(), 0);
        if (FAILED(hr)) {
            if (err) *err = hrText("SetOutputType(H264)", hr);
            return false;
        }

        ComPtr<IMFMediaType> it;
        bool set_in = false;
        for (DWORD i = 0; !set_in; ++i) {
            ComPtr<IMFMediaType> cand;
            if (FAILED(mft->GetInputAvailableType(in_id, i, &cand))) break;
            GUID sub{};
            cand->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            MFSetAttributeSize(cand.Get(), MF_MT_FRAME_SIZE, w, h);
            MFSetAttributeRatio(cand.Get(), MF_MT_FRAME_RATE, opts.fps, 1);
            MFSetAttributeRatio(cand.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            cand->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            setColorAttrs(cand.Get());
            set_in = SUCCEEDED(mft->SetInputType(in_id, cand.Get(), 0));
        }
        if (!set_in) {
            MFCreateMediaType(&it);
            it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h);
            MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, opts.fps, 1);
            MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            setColorAttrs(it.Get());
            hr = mft->SetInputType(in_id, it.Get(), 0);
            if (FAILED(hr)) {
                if (err) *err = hrText("SetInputType(NV12)", hr);
                return false;
            }
        }
        // Some encoders only honour these after the types are set.
        setBool(codec.Get(), CODECAPI_AVLowLatencyMode, true);
        setUi4(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setUi4(codec.Get(), CODECAPI_AVEncMPVGOPSize, opts.gop);

        mft->GetOutputStreamInfo(out_id, &osi);
        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) {
            if (err) *err = hrText("start streaming", hr);
            return false;
        }
        if (async) {
            if (FAILED(mft.As(&events))) {
                if (err) *err = "async MFT without IMFMediaEventGenerator";
                return false;
            }
            stopping = false;
            ev_thread = std::thread([this] { eventLoop(); });
        }
        return true;
    }

    ComPtr<IMFSample> makeSample(const uint8_t* nv12, size_t n, int64_t pts) {
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> s;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(n), &buf))) return nullptr;
        BYTE* p = nullptr;
        if (FAILED(buf->Lock(&p, nullptr, nullptr))) return nullptr;
        std::memcpy(p, nv12, n);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(n));
        if (FAILED(MFCreateSample(&s))) return nullptr;
        s->AddBuffer(buf.Get());
        s->SetSampleTime(pts);
        s->SetSampleDuration(10000000LL / std::max<uint32_t>(1, opts.fps));
        return s;
    }

    // Caller holds m.
    HRESULT submitLocked(const uint8_t* nv12, size_t n, int64_t pts, bool key, uint64_t token) {
        ComPtr<IMFSample> s = makeSample(nv12, n, pts);
        if (!s) return E_OUTOFMEMORY;
        if (key) setUi4(codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        tokens[pts] = token;
        while (tokens.size() > 64) tokens.erase(tokens.begin());
        return mft->ProcessInput(in_id, s.Get(), 0);
    }

    enum class OutResult { Got, NeedMore, Changed, Error };

    OutResult pullOne(std::vector<uint8_t>& data, int64_t& pts, uint64_t& token) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = out_id;
        ComPtr<IMFSample> own;
        const bool mft_provides = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!mft_provides) {
            ComPtr<IMFMediaBuffer> buf;
            const DWORD cap = std::max<DWORD>(osi.cbSize, w * h * 3 / 2 + 4096);
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(cap, &buf))) return OutResult::Error;
            own->AddBuffer(buf.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) {
            ob.pEvents->Release();
            ob.pEvents = nullptr;
        }
        ComPtr<IMFSample> sample;
        if (ob.pSample) {
            sample = ob.pSample;
            if (mft_provides) ob.pSample->Release(); // we own the MFT-allocated sample's reference
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return OutResult::NeedMore;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft->GetOutputAvailableType(out_id, 0, &t))) mft->SetOutputType(out_id, t.Get(), 0);
            mft->GetOutputStreamInfo(out_id, &osi);
            return OutResult::Changed;
        }
        if (FAILED(hr) || !sample) return OutResult::Error;
        ComPtr<IMFMediaBuffer> cb;
        if (FAILED(sample->ConvertToContiguousBuffer(&cb))) return OutResult::Error;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(cb->Lock(&p, nullptr, &len))) return OutResult::Error;
        data.assign(p, p + len);
        cb->Unlock();
        pts = 0;
        sample->GetSampleTime(&pts);
        token = 0;
        auto itok = tokens.find(pts);
        if (itok != tokens.end()) {
            token = itok->second;
            tokens.erase(tokens.begin(), std::next(itok));
        }
        return OutResult::Got;
    }

    void deliver(const std::vector<uint8_t>& data, int64_t pts, uint64_t token) {
        if (out && !data.empty()) out(data.data(), data.size(), pts, token);
    }

    // Sync MFT: drain every ready output.
    void drainSync() {
        for (int guard = 0; guard < 16; ++guard) {
            std::vector<uint8_t> data;
            int64_t pts = 0;
            uint64_t token = 0;
            OutResult r;
            {
                std::lock_guard<std::mutex> lk(m);
                r = pullOne(data, pts, token);
            }
            if (r == OutResult::Got) deliver(data, pts, token);
            else if (r == OutResult::Changed) continue;
            else break;
        }
    }

    void eventLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stopping) {
            ComPtr<IMFMediaEvent> ev;
            if (FAILED(events->GetEvent(0, &ev)) || !ev) break;
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == METransformDrainComplete) {
                std::lock_guard<std::mutex> lk(m);
                drained = true;
                drain_cv.notify_all();
            } else if (type == METransformNeedInput) {
                std::lock_guard<std::mutex> lk(m);
                if (draining) {
                    // Closing: no more input.
                } else if (pending.valid) {
                    pending.valid = false;
                    submitLocked(pending.nv12.data(), pending.nv12.size(), pending.pts, pending.key, pending.token);
                } else {
                    ++credits;
                }
            } else if (type == METransformHaveOutput) {
                std::vector<uint8_t> data;
                int64_t pts = 0;
                uint64_t token = 0;
                OutResult r;
                {
                    std::lock_guard<std::mutex> lk(m);
                    r = pullOne(data, pts, token);
                }
                if (r == OutResult::Got && !draining) deliver(data, pts, token);
            }
        }
        CoUninitialize();
    }
};

EncoderCloseStats encoderCloseStats() {
    std::lock_guard<std::mutex> lk(g_park_mutex);
    return g_close_stats;
}

MfH264Encoder::MfH264Encoder() = default;
MfH264Encoder::~MfH264Encoder() { close(); }

bool MfH264Encoder::isOpen() const { return impl_ && impl_->mft; }
std::string MfH264Encoder::name() const { return impl_ ? impl_->name : std::string(); }
bool MfH264Encoder::isHardware() const { return impl_ && impl_->hardware; }

void MfH264Encoder::close() {
    if (impl_) {
        impl_->shutdown();
        impl_.reset();
    }
}

bool MfH264Encoder::open(uint32_t width, uint32_t height, const EncoderOptions& opts, OutputFn on_output, std::string* err) {
    close();
    if ((width & 1) || (height & 1) || width == 0 || height == 0) {
        if (err) *err = "H.264 needs even, non-zero dimensions";
        return false;
    }
    std::string attempts;
    auto tryCategory = [&](UINT32 flags, bool hw) -> bool {
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &count))) return false;
        bool ok = false;
        for (UINT32 i = 0; i < count; ++i) {
            if (!ok) {
                auto impl = std::make_unique<Impl>();
                impl->w = width;
                impl->h = height;
                impl->opts = opts;
                impl->out = on_output;
                impl->hardware = hw;
                LPWSTR fname = nullptr;
                UINT32 flen = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &flen))) {
                    impl->name = narrow(fname);
                    CoTaskMemFree(fname);
                }
                std::string why;
                impl->activate = acts[i]; // AddRef: ShutdownObject is called when the encoder closes
                if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&impl->mft))) && impl->configure(&why)) {
                    impl_ = std::move(impl);
                    ok = true;
                } else {
                    attempts += (attempts.empty() ? "" : "; ") + impl->name + ": " + (why.empty() ? "activate failed" : why);
                    impl->shutdown(); // calls ShutdownObject too
                }
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
        return ok;
    };

    if (!ensureMediaFoundation(err)) return false; // started once, never shut down (mf_platform.cpp says why)
    bool ok = false;
    if (opts.prefer_hardware) ok = tryCategory(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true);
    if (!ok && opts.allow_software) ok = tryCategory(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, false);
    if (!ok && err) *err = "no H.264 encoder MFT accepted the settings" + (attempts.empty() ? std::string() : " (" + attempts + ")");
    return ok;
}

bool MfH264Encoder::encode(const uint8_t* nv12, size_t n, int64_t pts, bool key, uint64_t token, std::string* err) {
    if (!impl_ || !impl_->mft) {
        if (err) *err = "encoder not open";
        return false;
    }
    Impl& I = *impl_;
    if (n < static_cast<size_t>(I.w) * I.h * 3 / 2) {
        if (err) *err = "short NV12 frame";
        return false;
    }
    if (I.async) {
        std::lock_guard<std::mutex> lk(I.m);
        if (I.credits > 0) {
            --I.credits;
            const HRESULT hr = I.submitLocked(nv12, n, pts, key, token);
            if (FAILED(hr)) {
                if (err) *err = hrText("ProcessInput", hr);
                return false;
            }
        } else {
            // Latest frame wins; keep a pending keyframe request.
            I.pending.key = I.pending.key && I.pending.valid ? true : key;
            I.pending.nv12.assign(nv12, nv12 + n);
            I.pending.pts = pts;
            I.pending.token = token;
            I.pending.valid = true;
        }
        return true;
    }
    HRESULT hr;
    {
        std::lock_guard<std::mutex> lk(I.m);
        hr = I.submitLocked(nv12, n, pts, key, token);
    }
    if (hr == MF_E_NOTACCEPTING) {
        I.drainSync();
        std::lock_guard<std::mutex> lk(I.m);
        hr = I.submitLocked(nv12, n, pts, key, token);
    }
    if (FAILED(hr)) {
        if (err) *err = hrText("ProcessInput", hr);
        return false;
    }
    I.drainSync();
    return true;
}

std::vector<uint8_t> MfH264Encoder::sequenceHeader() const {
    if (!impl_ || !impl_->mft) return {};
    std::lock_guard<std::mutex> lk(impl_->m);
    ComPtr<IMFMediaType> t;
    if (FAILED(impl_->mft->GetOutputCurrentType(impl_->out_id, &t))) return {};
    UINT32 size = 0;
    if (FAILED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return {};
    std::vector<uint8_t> blob(size);
    if (FAILED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), size, &size))) return {};
    blob.resize(size);
    return blob;
}

} // namespace kvhost
