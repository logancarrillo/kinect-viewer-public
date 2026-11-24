// The camera roll's media layer: pre-roll ring, pass-through writer, reader and leftover probe
// (pet-camera.md §6.1, §6.3; protocol v3 §5.9). See roll_media.hpp for the rules and the threading.
#include "roll_media.hpp"

#include "mf_platform.hpp"

#include "video_convert.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace kvhost {

namespace {

const uint8_t kStartCode[4] = {0, 0, 0, 1};

void putNal(std::vector<uint8_t>& out, const std::vector<uint8_t>& nal) {
    out.insert(out.end(), kStartCode, kStartCode + 4);
    out.insert(out.end(), nal.begin(), nal.end());
}

uint8_t nalType(const std::vector<uint8_t>& nal) { return nal.empty() ? 0 : static_cast<uint8_t>(nal[0] & 0x1F); }

std::string hrText(const char* what, HRESULT hr) {
    char b[160];
    std::snprintf(b, sizeof(b), "%s failed (hr=0x%08lx)", what, static_cast<unsigned long>(hr));
    return b;
}

// 100 ns ticks between 1601-01-01 (FILETIME) and 1970-01-01 (Unix).
constexpr int64_t kFileTimeUnixEpoch = 116444736000000000LL;
// Seconds between 1904-01-01 (MP4 'mvhd') and 1970-01-01.
constexpr uint64_t kMp4UnixEpochSec = 2082844800ULL;

FILETIME unixMsToFileTime(int64_t unix_ms) {
    const uint64_t t = static_cast<uint64_t>(unix_ms * 10000 + kFileTimeUnixEpoch);
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(t & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
    return ft;
}

int64_t fileTimeToUnixMs(const FILETIME& ft) {
    const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return (t - kFileTimeUnixEpoch) / 10000;
}

bool setCreationTime(const std::wstring& path, int64_t unix_ms) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const FILETIME ct = unixMsToFileTime(unix_ms);
    const bool ok = SetFileTime(h, &ct, nullptr, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

// ---- MP4 boxes: just enough to find moov/mvhd for its creation time --------------------------------

bool readAt(HANDLE h, uint64_t pos, void* dst, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(pos);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, dst, n, &got, nullptr) && got == n;
}

bool writeAt(HANDLE h, uint64_t pos, const void* src, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(pos);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD put = 0;
    return WriteFile(h, src, n, &put, nullptr) && put == n;
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
void putBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
void putBe64(uint8_t* p, uint64_t v) {
    putBe32(p, static_cast<uint32_t>(v >> 32));
    putBe32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
}

// Finds the first box of `type` among the boxes in [begin, end). payload is where its contents start.
bool findBox(HANDLE h, uint64_t begin, uint64_t end, const char* type, uint64_t& payload, uint64_t& box_end) {
    uint64_t pos = begin;
    while (pos + 8 <= end) {
        uint8_t hdr[16];
        if (!readAt(h, pos, hdr, 8)) return false;
        uint64_t size = be32(hdr);
        uint64_t header = 8;
        if (size == 1) {
            if (pos + 16 > end || !readAt(h, pos + 8, hdr + 8, 8)) return false;
            size = be64(hdr + 8);
            header = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < header || pos + size > end) return false;
        if (std::memcmp(hdr + 4, type, 4) == 0) {
            payload = pos + header;
            box_end = pos + size;
            return true;
        }
        pos += size;
    }
    return false;
}

bool fileSize(HANDLE h, uint64_t& size) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return false;
    size = static_cast<uint64_t>(li.QuadPart);
    return true;
}

// The mvhd payload's offset and version.
bool findMvhd(HANDLE h, uint64_t& mvhd, uint8_t& version) {
    uint64_t size = 0, moov = 0, moov_end = 0, mvhd_end = 0;
    if (!fileSize(h, size) || !findBox(h, 0, size, "moov", moov, moov_end)) return false;
    if (!findBox(h, moov, moov_end, "mvhd", mvhd, mvhd_end)) return false;
    if (mvhd_end - mvhd < 20 || !readAt(h, mvhd, &version, 1)) return false;
    return version <= 1 && (version == 0 || mvhd_end - mvhd >= 28);
}

// The whole fragments the file holds now (a moof, then an mdat that ends inside the file), read through a handle
// of its own as a reader would see it while the writer still has it open. A box still being written ends the count.
size_t wholeFragmentsOnDisk(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size_t n = 0;
    uint64_t size = 0;
    if (fileSize(h, size)) {
        bool moof = false;
        uint64_t pos = 0;
        while (pos + 8 <= size) {
            uint8_t hdr[16];
            if (!readAt(h, pos, hdr, 8)) break;
            uint64_t box = be32(hdr);
            uint64_t header = 8;
            if (box == 1) {
                if (pos + 16 > size || !readAt(h, pos + 8, hdr + 8, 8)) break;
                box = be64(hdr + 8);
                header = 16;
            }
            if (box < header || pos + box > size) break; // 0 ("to the end") included: not whole yet
            if (std::memcmp(hdr + 4, "moof", 4) == 0) {
                moof = true;
            } else if (std::memcmp(hdr + 4, "mdat", 4) == 0) {
                if (moof) ++n;
                moof = false;
            }
            pos += box;
        }
    }
    CloseHandle(h);
    return n;
}

// Writes the clip start into mvhd's creation time and its end into the modification time.
bool patchMvhdTimes(const std::wstring& path, int64_t start_unix_ms, int64_t end_unix_ms) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint64_t mvhd = 0;
    uint8_t version = 0;
    bool ok = findMvhd(h, mvhd, version);
    if (ok) {
        const uint64_t c = static_cast<uint64_t>(start_unix_ms / 1000) + kMp4UnixEpochSec;
        const uint64_t m = static_cast<uint64_t>(end_unix_ms / 1000) + kMp4UnixEpochSec;
        uint8_t b[16];
        if (version == 1) {
            putBe64(b, c);
            putBe64(b + 8, m);
            ok = writeAt(h, mvhd + 4, b, 16);
        } else {
            putBe32(b, static_cast<uint32_t>(c));
            putBe32(b + 4, static_cast<uint32_t>(m));
            ok = writeAt(h, mvhd + 4, b, 8);
        }
    }
    CloseHandle(h);
    return ok;
}

// Parses MF_MT_MPEG_SEQUENCE_HEADER: Annex-B (what the MP4 source gives and the sink takes) or an
// avcC record (configurationVersion 1), in case a source hands that over instead.
void parseSequenceHeader(const uint8_t* p, size_t n, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    if (n >= 7 && p[0] == 1) {
        size_t i = 5;
        const unsigned nsps = p[i++] & 0x1F;
        for (unsigned k = 0; k < nsps && i + 2 <= n; ++k) {
            const size_t len = (static_cast<size_t>(p[i]) << 8) | p[i + 1];
            i += 2;
            if (i + len > n) return;
            if (sps.empty()) sps.assign(p + i, p + i + len);
            i += len;
        }
        if (i >= n) return;
        const unsigned npps = p[i++];
        for (unsigned k = 0; k < npps && i + 2 <= n; ++k) {
            const size_t len = (static_cast<size_t>(p[i]) << 8) | p[i + 1];
            i += 2;
            if (i + len > n) return;
            if (pps.empty()) pps.assign(p + i, p + i + len);
            i += len;
        }
        return;
    }
    for (const auto& nal : splitAnnexB(p, n)) {
        if (nalType(nal) == 7 && sps.empty()) sps = nal;
        if (nalType(nal) == 8 && pps.empty()) pps = nal;
    }
}

bool startsWithStartCode(const uint8_t* p, size_t n) {
    return (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) || (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1);
}

// A native sample to annexBToAvcc's form. The MPEG-4 source hands H.264 samples over in Annex-B (the
// form its decoder takes); a 4-byte length-prefixed sample is accepted too, with SPS/PPS/AUD removed.
bool sampleToAvcc(const uint8_t* p, size_t n, AvccAccessUnit& out) {
    if (startsWithStartCode(p, n)) return annexBToAvcc(p, n, out);
    out = AvccAccessUnit{};
    size_t i = 0;
    while (i + 4 <= n) {
        const size_t len = be32(p + i);
        i += 4;
        if (len == 0 || i + len > n) return false;
        const uint8_t type = p[i] & 0x1F;
        if (type == 7) out.sps.assign(p + i, p + i + len);
        else if (type == 8) out.pps.assign(p + i, p + i + len);
        else if (type != 9) {
            if (type == 5) out.idr = true;
            out.avcc.insert(out.avcc.end(), p + i - 4, p + i + len);
            ++out.nal_count;
        }
        i += len;
    }
    return i == n && out.nal_count > 0;
}

} // namespace

// ---- Access units and parameter sets ---------------------------------------------------------------

RollAccessUnit rollAccessUnitFromAnnexB(const uint8_t* annexb, size_t n, int64_t capture_hns, uint32_t width,
                                        uint32_t height) {
    RollAccessUnit au;
    au.annexb.assign(annexb, annexb + n);
    au.capture_hns = capture_hns;
    au.width = width;
    au.height = height;
    for (const auto& nal : splitAnnexB(annexb, n)) {
        if (nalType(nal) == 5) au.idr = true;
    }
    return au;
}

RollAccessUnit rollAccessUnitFromAvcc(const std::vector<uint8_t>& avcc, const std::vector<uint8_t>& sps,
                                      const std::vector<uint8_t>& pps, bool idr, int64_t capture_hns,
                                      uint32_t width, uint32_t height) {
    RollAccessUnit au;
    au.capture_hns = capture_hns;
    au.width = width;
    au.height = height;
    if (idr && !sps.empty() && !pps.empty()) {
        putNal(au.annexb, sps);
        putNal(au.annexb, pps);
    }
    size_t i = 0;
    while (i + 4 <= avcc.size()) {
        const size_t len = be32(avcc.data() + i);
        i += 4;
        if (len == 0 || i + len > avcc.size()) break;
        if ((avcc[i] & 0x1F) == 5) au.idr = true;
        au.annexb.insert(au.annexb.end(), kStartCode, kStartCode + 4);
        au.annexb.insert(au.annexb.end(), avcc.begin() + static_cast<std::ptrdiff_t>(i),
                         avcc.begin() + static_cast<std::ptrdiff_t>(i + len));
        i += len;
    }
    return au;
}

void RollParameterSets::setSequenceHeader(const uint8_t* annexb, size_t n, uint32_t width, uint32_t height) {
    if (!annexb || n == 0) return;
    std::vector<uint8_t> sps, pps;
    parseSequenceHeader(annexb, n, sps, pps);
    if (sps.empty() || pps.empty()) return;
    sps_ = std::move(sps);
    pps_ = std::move(pps);
    width_ = width;
    height_ = height;
}

std::vector<uint8_t> RollParameterSets::sequenceHeader() const {
    std::vector<uint8_t> out;
    if (!known()) return out;
    putNal(out, sps_);
    putNal(out, pps_);
    return out;
}

bool RollParameterSets::normalize(RollAccessUnit& au) {
    const auto nals = splitAnnexB(au.annexb.data(), au.annexb.size());
    bool has_sps = false, has_pps = false;
    au.idr = false;
    for (const auto& nal : nals) {
        const uint8_t t = nalType(nal);
        if (t == 7) {
            sps_ = nal;
            has_sps = true;
        } else if (t == 8) {
            pps_ = nal;
            has_pps = true;
        } else if (t == 5) {
            au.idr = true;
        }
    }
    if (has_sps || has_pps) {
        width_ = au.width;
        height_ = au.height;
    }
    if (!au.idr) return true;
    if (has_sps && has_pps) return true;
    if (!known()) return false;
    if ((width_ != 0 || height_ != 0) && (width_ != au.width || height_ != au.height)) return false;
    // Rebuild: [AUD], SPS, PPS, then the rest, so the IDR decodes on its own (pet-camera.md §6.1).
    std::vector<uint8_t> out;
    out.reserve(au.annexb.size() + sps_.size() + pps_.size() + 16);
    size_t k = 0;
    if (!nals.empty() && nalType(nals[0]) == 9) {
        putNal(out, nals[0]);
        k = 1;
    }
    putNal(out, sps_);
    putNal(out, pps_);
    for (; k < nals.size(); ++k) {
        const uint8_t t = nalType(nals[k]);
        if (t == 7 || t == 8) continue;
        putNal(out, nals[k]);
    }
    au.annexb.swap(out);
    return true;
}

// ---- The pre-roll ring -------------------------------------------------------------------------------

RollPreRollRing::RollPreRollRing() = default;
RollPreRollRing::RollPreRollRing(const Options& opts) : opts_(opts) {}

void RollPreRollRing::setSequenceHeader(const uint8_t* annexb, size_t n, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lk(m_);
    params_.setSequenceHeader(annexb, n, width, height);
}

bool RollPreRollRing::push(RollAccessUnit au) {
    std::lock_guard<std::mutex> lk(m_);
    if (!params_.normalize(au)) return false;
    // A clip has one size (pet-camera.md §6.1), so the pre-roll never spans a size change.
    if (!aus_.empty() && (au.width != aus_.front().width || au.height != aus_.front().height)) {
        aus_.clear();
        bytes_ = 0;
    }
    if (aus_.empty() && !au.idr) return false; // the ring always starts at an IDR
    bytes_ += au.annexb.size();
    aus_.push_back(std::move(au));
    trimLocked();
    return !aus_.empty();
}

void RollPreRollRing::trimLocked() {
    while (!aus_.empty() &&
           (aus_.back().capture_hns - aus_.front().capture_hns > opts_.span_hns || bytes_ > opts_.byte_cap)) {
        // Drop the front GOP: everything before the next IDR (all of it when there is none).
        size_t k = 1;
        while (k < aus_.size() && !aus_[k].idr) ++k;
        for (size_t i = 0; i < k; ++i) {
            bytes_ -= aus_.front().annexb.size();
            aus_.pop_front();
        }
    }
}

std::vector<RollAccessUnit> RollPreRollRing::preRoll(int64_t max_hns) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<RollAccessUnit> out;
    if (aus_.empty()) return out;
    const int64_t cutoff = aus_.back().capture_hns - std::max<int64_t>(0, max_hns);
    for (size_t i = 0; i < aus_.size(); ++i) {
        if (aus_[i].idr && aus_[i].capture_hns >= cutoff) {
            out.assign(aus_.begin() + static_cast<std::ptrdiff_t>(i), aus_.end());
            break;
        }
    }
    return out;
}

void RollPreRollRing::clear() {
    std::lock_guard<std::mutex> lk(m_);
    aus_.clear();
    bytes_ = 0;
}

std::vector<int64_t> RollPreRollRing::captureTimes() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<int64_t> out;
    for (const auto& a : aus_) out.push_back(a.capture_hns);
    return out;
}

size_t RollPreRollRing::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return aus_.size();
}

size_t RollPreRollRing::bytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return bytes_;
}

int64_t RollPreRollRing::spanHns() const {
    std::lock_guard<std::mutex> lk(m_);
    return aus_.size() < 2 ? 0 : aus_.back().capture_hns - aus_.front().capture_hns;
}

std::vector<uint8_t> RollPreRollRing::sequenceHeader() const {
    std::lock_guard<std::mutex> lk(m_);
    return params_.sequenceHeader();
}

// ---- The writer ----------------------------------------------------------------------------------------

struct RollClipWriter::Impl {
    RollWriterOptions opts;

    // Shared with the writer thread, under m.
    std::mutex m;
    std::condition_variable cv;      // work queued, or a stop
    std::condition_variable idle_cv; // the queue drained
    std::deque<RollAccessUnit> q;
    size_t q_bytes = 0;
    enum class Stop { None, Finalize, Abandon } stop = Stop::None;
    bool failed = false;     // MF failed: nothing more is taken or written
    bool overflowed = false; // the queue overflowed: nothing more is taken; what came before is kept
    bool busy = false;       // the thread is writing an AU it took off the queue
    std::string error;
    std::thread th;
    std::atomic<uint64_t> written{0};
    RollClipResult result; // set by the thread before it exits

    // The writer thread's own state.
    ComPtr<IMFByteStream> stream;
    ComPtr<IMFSinkWriter> sink;
    DWORD stream_index = 0;
    RollParameterSets params;
    bool started = false;
    RollAccessUnit held; // one AU held back until the next gives its duration
    bool have_held = false;
    int64_t held_t = 0;
    int64_t first_capture = 0;
    int64_t last_delta = 0;
    int64_t end_t = 0;

    void fail(const std::string& why) {
        std::lock_guard<std::mutex> lk(m);
        if (!failed) {
            failed = true;
            error = why;
        }
        q.clear();
        q_bytes = 0;
    }

    bool openSink(std::string* err) {
        const std::vector<uint8_t> seq = params.sequenceHeader();
        HRESULT hr = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_RESET_IF_EXIST, MF_FILEFLAGS_NONE,
                                  opts.path.c_str(), &stream);
        if (FAILED(hr)) {
            *err = hrText("MFCreateFile", hr);
            return false;
        }
        // Set the clip start again now that MF's file stream holds the file, in case opening it reset
        // the time (attribute-only access passes the stream's share mode), so an interrupted clip keeps
        // it (pet-camera.md §6.3 "Leftovers").
        if (opts.start_unix_ms > 0) setCreationTime(opts.path, opts.start_unix_ms);
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 2);
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, opts.container == RollContainer::FragmentedMp4
                                                       ? MFTranscodeContainerType_FMPEG4
                                                       : MFTranscodeContainerType_MPEG4);
        hr = MFCreateSinkWriterFromURL(opts.path.c_str(), stream.Get(), attrs.Get(), &sink);
        if (FAILED(hr)) {
            *err = hrText("MFCreateSinkWriterFromURL", hr);
            return false;
        }
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, opts.width, opts.height);
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, std::max<uint32_t>(1, opts.fps), 1);
        MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        t->SetUINT32(MF_MT_AVG_BITRATE, opts.bitrate);
        t->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seq.data(), static_cast<UINT32>(seq.size()));
        hr = sink->AddStream(t.Get(), &stream_index);
        if (FAILED(hr)) {
            *err = hrText("IMFSinkWriter::AddStream", hr);
            return false;
        }
        // The same type in and out: pass-through, no encoder (pet-camera.md §6.1).
        hr = sink->SetInputMediaType(stream_index, t.Get(), nullptr);
        if (FAILED(hr)) {
            *err = hrText("IMFSinkWriter::SetInputMediaType", hr);
            return false;
        }
        hr = sink->BeginWriting();
        if (FAILED(hr)) {
            *err = hrText("IMFSinkWriter::BeginWriting", hr);
            return false;
        }
        return true;
    }

    // The test seam's wait (RollWriterOptions::test_fail_after_fragments): until the file holds that many whole
    // fragments, the clip is abandoned, or 10 s have gone.
    void waitForTestFragments() {
        if (opts.test_fail_after_fragments == 0) return;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (wholeFragmentsOnDisk(opts.path) < opts.test_fail_after_fragments &&
               std::chrono::steady_clock::now() < until) {
            std::unique_lock<std::mutex> lk(m);
            if (stop == Stop::Abandon) return;
            cv.wait_for(lk, std::chrono::milliseconds(10));
        }
    }

    bool writeHeld(int64_t duration, std::string* err) {
        ComPtr<IMFMediaBuffer> buf;
        const DWORD n = static_cast<DWORD>(held.annexb.size());
        HRESULT hr = MFCreateMemoryBuffer(n, &buf);
        if (FAILED(hr)) {
            *err = hrText("MFCreateMemoryBuffer", hr);
            return false;
        }
        BYTE* p = nullptr;
        if (FAILED(buf->Lock(&p, nullptr, nullptr))) {
            *err = "IMFMediaBuffer::Lock failed";
            return false;
        }
        std::memcpy(p, held.annexb.data(), n);
        buf->Unlock();
        buf->SetCurrentLength(n);
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(held_t);
        s->SetSampleDuration(duration);
        // Sync samples give the file its keyframe table for seeks (pet-camera.md §6.1).
        s->SetUINT32(MFSampleExtension_CleanPoint, held.idr ? 1 : 0);
        if (opts.test_fail_write_after > 0 && result.frames >= opts.test_fail_write_after) {
            waitForTestFragments();
            *err = "IMFSinkWriter::WriteSample failed (test seam)";
            return false;
        }
        hr = sink->WriteSample(stream_index, s.Get());
        if (FAILED(hr)) {
            *err = hrText("IMFSinkWriter::WriteSample", hr);
            return false;
        }
        ++result.frames;
        if (held.idr) ++result.keyframes;
        if (result.frames == 1) result.first_capture_hns = held.capture_hns;
        result.last_capture_hns = held.capture_hns;
        end_t = held_t + duration;
        written.fetch_add(1);
        return true;
    }

    void process(RollAccessUnit& au) {
        std::string err;
        if (!params.normalize(au) || (!started && !au.idr)) {
            ++result.skipped_before_idr;
            return;
        }
        if (!started) {
            if (!openSink(&err)) {
                fail(err);
                return;
            }
            started = true;
            first_capture = au.capture_hns;
        }
        // Real times from the capture clock, the first at 0, kept strictly increasing.
        int64_t t = au.capture_hns - first_capture;
        if (have_held && t <= held_t) t = held_t + 1;
        if (t < 0) t = 0;
        if (have_held) {
            const int64_t d = t - held_t;
            if (!writeHeld(d, &err)) {
                fail(err);
                return;
            }
            last_delta = d;
        }
        held = std::move(au);
        held_t = t;
        have_held = true;
    }

    void finish(bool finalize) {
        if (!finalize) {
            // As a crash would: no Finalize, so the sink never writes what it writes at the end.
            sink.Reset();
            if (stream) stream->Close();
            stream.Reset();
            result.ok = false;
            result.error = "abandoned without finalizing";
            return;
        }
        std::string err;
        bool ok = !failed;
        if (ok && started && have_held) {
            const int64_t d = last_delta > 0 ? last_delta : kRollHnsPerSecond / std::max<uint32_t>(1, opts.fps);
            if (!writeHeld(d, &err)) {
                fail(err);
                ok = false;
            }
            have_held = false;
        }
        if (ok && started) {
            const HRESULT hr = sink->Finalize();
            if (FAILED(hr)) {
                fail(hrText("IMFSinkWriter::Finalize", hr));
                ok = false;
            }
        }
        sink.Reset();
        if (stream) stream->Close();
        stream.Reset();
        if (!started) {
            DeleteFileW(opts.path.c_str()); // nothing to show: no IDR ever came
            result.ok = false;
            result.error = failed ? error : "no IDR reached the writer; nothing was recorded";
            return;
        }
        result.duration_hns = end_t;
        if (ok && opts.start_unix_ms > 0) {
            const int64_t end_ms = opts.start_unix_ms + end_t / 10000;
            const bool mvhd = patchMvhdTimes(opts.path, opts.start_unix_ms, end_ms);
            const bool ntfs = setCreationTime(opts.path, opts.start_unix_ms);
            result.creation_time_set = mvhd && ntfs;
        }
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(opts.path.c_str(), GetFileExInfoStandard, &fa)) {
            result.bytes = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        }
        std::lock_guard<std::mutex> lk(m);
        result.ok = ok;
        if (!ok) result.error = error;
        else if (overflowed) result.error = error; // finished, but cut short
    }

    void threadMain() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // MF is started once for the whole host and never shut down (mf_platform.cpp says why).
        std::string mf_err;
        const bool mf = ensureMediaFoundation(&mf_err);
        if (!mf) fail(mf_err.empty() ? std::string("MFStartup failed") : mf_err);
        bool finalize = true;
        for (;;) {
            RollAccessUnit au;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return !q.empty() || stop != Stop::None; });
                if (stop == Stop::Abandon) {
                    finalize = false;
                    q.clear();
                    q_bytes = 0;
                    break;
                }
                if (q.empty()) break; // Finalize with everything written
                au = std::move(q.front());
                q.pop_front();
                q_bytes -= au.annexb.size();
                busy = true;
            }
            if (mf) process(au);
            {
                std::lock_guard<std::mutex> lk(m);
                busy = false;
            }
            idle_cv.notify_all();
        }
        finish(finalize);
        if (SUCCEEDED(co)) CoUninitialize();
        idle_cv.notify_all();
    }

    bool enqueue(RollAccessUnit&& au, std::string* err) {
        std::lock_guard<std::mutex> lk(m);
        auto refuse = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (stop != Stop::None) return refuse("the clip is already finishing");
        if (failed) return refuse(error);
        if (overflowed) return refuse(error);
        if (au.width != opts.width || au.height != opts.height) {
            return refuse("a clip has one size: " + std::to_string(au.width) + "x" + std::to_string(au.height) +
                          " is not " + std::to_string(opts.width) + "x" + std::to_string(opts.height));
        }
        if (q_bytes + au.annexb.size() > opts.max_queued_bytes) {
            overflowed = true;
            error = "the writer fell behind; the clip was cut short";
            return refuse(error);
        }
        q_bytes += au.annexb.size();
        q.push_back(std::move(au));
        cv.notify_one();
        return true;
    }

    void stopThread(Stop how) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (stop == Stop::None) stop = how;
        }
        cv.notify_one();
        if (th.joinable()) th.join();
    }
};

RollClipWriter::RollClipWriter() = default;
RollClipWriter::~RollClipWriter() { abandon(); }

bool RollClipWriter::isOpen() const { return impl_ != nullptr; }

bool RollClipWriter::open(const RollWriterOptions& opts, std::string* err) {
    abandon();
    auto refuse = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    if (opts.path.empty()) return refuse("no path");
    if (opts.width == 0 || opts.height == 0 || (opts.width & 1) || (opts.height & 1)) {
        return refuse("H.264 needs even, non-zero dimensions");
    }
    // Create the file now, never over an existing clip, and give it the clip's start as its creation
    // time; MF then opens it without recreating it, so even an interrupted clip keeps that time. A
    // reservation (RollStore::reserveClip) is the empty file the store created to take the name.
    HANDLE h = CreateFileW(opts.path.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr,
                           opts.reserved_empty_file ? OPEN_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return refuse(std::string(opts.reserved_empty_file ? "cannot open the reserved clip file" : "cannot create the clip file") +
                      " (error " + std::to_string(GetLastError()) + ")");
    }
    if (opts.reserved_empty_file) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size) || size.QuadPart != 0) {
            CloseHandle(h);
            return refuse("the reserved clip file isn't empty; a clip is never written over");
        }
    }
    if (opts.start_unix_ms > 0) {
        const FILETIME ct = unixMsToFileTime(opts.start_unix_ms);
        SetFileTime(h, &ct, nullptr, nullptr);
    }
    CloseHandle(h);
    impl_ = std::make_unique<Impl>();
    impl_->opts = opts;
    if (!opts.sequence_header.empty()) {
        impl_->params.setSequenceHeader(opts.sequence_header.data(), opts.sequence_header.size(), opts.width,
                                        opts.height);
    }
    Impl* impl = impl_.get();
    impl_->th = std::thread([impl] { impl->threadMain(); });
    return true;
}

bool RollClipWriter::append(const RollAccessUnit& au, std::string* err) {
    RollAccessUnit copy = au;
    return append(std::move(copy), err);
}

bool RollClipWriter::append(RollAccessUnit&& au, std::string* err) {
    if (!impl_) {
        if (err) *err = "the writer is not open";
        return false;
    }
    return impl_->enqueue(std::move(au), err);
}

bool RollClipWriter::appendAll(const std::vector<RollAccessUnit>& aus, std::string* err) {
    for (const auto& au : aus) {
        if (!append(au, err)) return false;
    }
    return true;
}

RollClipResult RollClipWriter::finalize() {
    if (!impl_) {
        RollClipResult r;
        r.error = "the writer is not open";
        return r;
    }
    impl_->stopThread(Impl::Stop::Finalize);
    RollClipResult r = impl_->result;
    impl_.reset();
    return r;
}

void RollClipWriter::abandon() {
    if (!impl_) return;
    impl_->stopThread(Impl::Stop::Abandon);
    impl_.reset();
}

bool RollClipWriter::waitIdle(int timeout_ms) {
    if (!impl_) return false;
    std::unique_lock<std::mutex> lk(impl_->m);
    return impl_->idle_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                   [&] { return impl_->q.empty() && !impl_->busy; });
}

uint64_t RollClipWriter::samplesWritten() const { return impl_ ? impl_->written.load() : 0; }

// ---- The reader --------------------------------------------------------------------------------------

struct RollClipReader::Impl {
    std::wstring path;
    ComPtr<IMFSourceReader> reader;
    std::deque<RollSample> pending; // read ahead by seek(): the IDR at or before the position, onwards

    ~Impl() { reader.Reset(); }

    bool create(std::string* err) {
        reader.Reset();
        HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
        if (FAILED(hr)) {
            if (err) *err = hrText("MFCreateSourceReaderFromURL", hr);
            return false;
        }
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        if (FAILED(hr)) {
            if (err) *err = "the clip has no video stream";
            return false;
        }
        return true;
    }
};

RollClipReader::RollClipReader() = default;
RollClipReader::~RollClipReader() { close(); }

bool RollClipReader::isOpen() const { return impl_ && impl_->reader; }

void RollClipReader::close() {
    impl_.reset();
    width_ = height_ = 0;
    duration_hns_ = 0;
    duration_scanned_ = false;
    truncated_ = false;
    scan_end_ = 0;
    last_end_ = 0;
    sps_.clear();
    pps_.clear();
}

bool RollClipReader::open(const std::wstring& path, std::string* err) {
    close();
    impl_ = std::make_unique<Impl>();
    impl_->path = path;
    if (!ensureMediaFoundation(err)) { // started once, never shut down (mf_platform.cpp)
        impl_.reset();
        return false;
    }
    if (!impl_->create(err) || !readParams(err)) {
        close();
        return false;
    }
    return true;
}

bool RollClipReader::readParams(std::string* err) {
    IMFSourceReader* r = impl_->reader.Get();
    ComPtr<IMFMediaType> t;
    HRESULT hr = r->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &t);
    if (FAILED(hr)) {
        if (err) *err = hrText("GetNativeMediaType", hr);
        return false;
    }
    GUID sub{};
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    if (sub != MFVideoFormat_H264) {
        if (err) *err = "the clip's video is not H.264";
        return false;
    }
    // Keep the native type, so no decoder is ever loaded: playback sends what is in the file.
    r->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t.Get());
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
    width_ = w;
    height_ = h;
    UINT32 size = 0;
    if (SUCCEEDED(t->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
        std::vector<uint8_t> blob(size);
        if (SUCCEEDED(t->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), size, &size))) {
            parseSequenceHeader(blob.data(), size, sps_, pps_);
        }
    }
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(r->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)) &&
        v.vt == VT_UI8) {
        duration_hns_ = static_cast<int64_t>(v.uhVal.QuadPart);
    }
    PropVariantClear(&v);
    if (duration_hns_ > 0 && !sps_.empty() && !pps_.empty() && width_ && height_) return true;

    // No duration (or no parameter sets) from the container: read the clip through once.
    duration_scanned_ = duration_hns_ <= 0;
    int64_t end = 0;
    RollSample s;
    for (;;) {
        const Read rr = readRaw(s, nullptr);
        if (rr == Read::Error) truncated_ = true; // an interrupted clip: it ends after its last whole fragment
        if (rr != Read::Sample) break;
        end = std::max(end, s.pts_hns + s.duration_hns); // next() also takes SPS/PPS from a keyframe
    }
    scan_end_ = end;
    if (duration_scanned_) duration_hns_ = end;
    if (!impl_->create(err)) return false;
    ComPtr<IMFMediaType> t2;
    if (SUCCEEDED(impl_->reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &t2))) {
        impl_->reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t2.Get());
    }
    if (sps_.empty() || pps_.empty()) {
        if (err) *err = "the clip has no SPS/PPS";
        return false;
    }
    if (width_ == 0 || height_ == 0) {
        if (err) *err = "the clip has no frame size";
        return false;
    }
    return true;
}

bool RollClipReader::seek(int64_t pos_hns, std::string* err) {
    if (!isOpen()) {
        if (err) *err = "the reader is not open";
        return false;
    }
    if (pos_hns < 0) pos_hns = 0;
    if (duration_hns_ > 0 && pos_hns > duration_hns_) pos_hns = duration_hns_;
    impl_->pending.clear();
    // SetCurrentPosition lands on a clean point near the position, but not always on the IDR at or
    // before it: on an fMP4 the MPEG-4 source lands one GOP early when the position is exactly an IDR's
    // time, and a clean point it reports need not be an IDR (see readRaw). So land at or before the
    // position, read forward, and keep from the last IDR at or before it (v3 §5.9: seeks are
    // keyframe-accurate). Landing after it means trying again from further back.
    const int64_t backs[] = {0, 2 * kRollHnsPerSecond, 10 * kRollHnsPerSecond, pos_hns};
    for (const int64_t back : backs) {
        const int64_t target = std::max<int64_t>(0, pos_hns - back);
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_I8;
        v.hVal.QuadPart = target;
        const HRESULT hr = impl_->reader->SetCurrentPosition(GUID_NULL, v);
        PropVariantClear(&v);
        if (FAILED(hr)) {
            if (err) *err = hrText("IMFSourceReader::SetCurrentPosition", hr);
            return false;
        }
        std::deque<RollSample> kept;
        bool late = false;
        for (;;) {
            RollSample s;
            if (readRaw(s, nullptr) != Read::Sample) break; // the end (or a truncated end): keep what is held
            if (s.pts_hns > pos_hns) {
                if (kept.empty()) late = true;
                else kept.push_back(std::move(s));
                break;
            }
            if (s.keyframe) {
                kept.clear();
                kept.push_back(std::move(s));
            } else if (!kept.empty()) {
                kept.push_back(std::move(s));
            }
        }
        if (!kept.empty()) {
            impl_->pending = std::move(kept);
            return true;
        }
        // Nothing readable from where it landed. That is the true end only when it landed at the start; landing
        // at (or just before) the end of the clip, e.g. a seek to its duration, reads nothing there either, so
        // that tries again from further back like a landing after the position does.
        if (!late && target == 0) break;
    }
    if (err) *err = "no keyframe at or before the position";
    return false;
}

RollClipReader::Read RollClipReader::next(RollSample& out, std::string* err) {
    if (impl_ && !impl_->pending.empty()) {
        out = std::move(impl_->pending.front());
        impl_->pending.pop_front();
        last_end_ = out.pts_hns + out.duration_hns;
        return Read::Sample;
    }
    const Read r = readRaw(out, err);
    if (r == Read::Sample) last_end_ = out.pts_hns + out.duration_hns;
    // The error an interrupted clip gives after its last whole fragment is its end. An error before that point
    // (a clip damaged further since open) is still an error.
    if (r == Read::Error && truncated_ && last_end_ >= scan_end_) return Read::End;
    return r;
}

RollClipReader::Read RollClipReader::readRaw(RollSample& out, std::string* err) {
    if (!isOpen()) {
        if (err) *err = "the reader is not open";
        return Read::Error;
    }
    for (int guard = 0; guard < 64; ++guard) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        const HRESULT hr = impl_->reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                                     nullptr, &flags, &ts, &s);
        if (FAILED(hr)) {
            if (err) *err = hrText("IMFSourceReader::ReadSample", hr);
            return Read::Error;
        }
        if (flags & MF_SOURCE_READERF_ERROR) {
            if (err) *err = "the source reader reported an error";
            return Read::Error;
        }
        if (!s) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return Read::End;
            continue; // a stream tick or a type change: no data
        }
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(s->ConvertToContiguousBuffer(&buf))) {
            if (err) *err = "ConvertToContiguousBuffer failed";
            return Read::Error;
        }
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) {
            if (err) *err = "IMFMediaBuffer::Lock failed";
            return Read::Error;
        }
        AvccAccessUnit au;
        const bool ok = sampleToAvcc(p, len, au);
        buf->Unlock();
        if (!ok) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return Read::End;
            continue; // nothing to show in it (only parameter sets or delimiters)
        }
        if (sps_.empty() && !au.sps.empty()) sps_ = au.sps;
        if (pps_.empty() && !au.pps.empty()) pps_ = au.pps;
        out.avcc = std::move(au.avcc);
        LONGLONG t = ts;
        s->GetSampleTime(&t);
        out.pts_hns = t;
        LONGLONG d = 0;
        s->GetSampleDuration(&d);
        out.duration_hns = d;
        // A keyframe is a sample with an IDR slice. MF's clean-point flag is kept for comparison: on an
        // fMP4 the MPEG-4 source reports the first sample of the second fragment as a clean point,
        // although the file's own sample flags (trun) mark only the IDRs (found in W7).
        out.keyframe = au.idr;
        out.clean_point = MFGetAttributeUINT32(s.Get(), MFSampleExtension_CleanPoint, 0) != 0;
        return Read::Sample;
    }
    if (err) *err = "the source reader returned no samples";
    return Read::Error;
}

// ---- Leftovers ------------------------------------------------------------------------------------------

bool probeRollClip(const std::wstring& path, RollProbeResult& out, std::string* err) {
    out = RollProbeResult{};
    RollClipReader rd;
    if (!rd.open(path, err)) return false;
    out.truncated = rd.truncated(); // next() gives End for it after its last fragment, so ask the reader
    out.width = rd.width();
    out.height = rd.height();
    out.sps = rd.sps();
    out.pps = rd.pps();
    RollSample s;
    int64_t end = 0;
    for (;;) {
        std::string why;
        const RollClipReader::Read r = rd.next(s, &why);
        if (r == RollClipReader::Read::End) break;
        if (r == RollClipReader::Read::Error) {
            out.truncated = true;
            break;
        }
        if (out.frames == 0 && !s.keyframe) {
            if (err) *err = "the clip does not start at a keyframe";
            return false;
        }
        ++out.frames;
        if (s.keyframe) ++out.keyframes;
        end = std::max(end, s.pts_hns + s.duration_hns);
    }
    if (out.frames == 0) {
        if (err) *err = "no readable frames";
        return false;
    }
    out.duration_hns = end;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        out.bytes = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        out.start_unix_ms = fileTimeToUnixMs(fa.ftCreationTime);
        out.end_unix_ms = out.start_unix_ms + end / 10000;
    }
    return true;
}

bool rollMp4CreationUnixMs(const std::wstring& path, int64_t& unix_ms) {
    unix_ms = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint64_t mvhd = 0;
    uint8_t version = 0;
    uint8_t b[8];
    bool ok = findMvhd(h, mvhd, version);
    uint64_t secs = 0;
    if (ok && version == 1) {
        ok = readAt(h, mvhd + 4, b, 8);
        secs = be64(b);
    } else if (ok) {
        ok = readAt(h, mvhd + 4, b, 4);
        secs = be32(b);
    }
    CloseHandle(h);
    if (!ok) return false;
    unix_ms = secs == 0 ? 0 : (static_cast<int64_t>(secs) - static_cast<int64_t>(kMp4UnixEpochSec)) * 1000;
    return true;
}

} // namespace kvhost
