// The camera roll's media layer (pet-camera.md §6.1, §6.3 "Leftovers"; protocol v3 §5.9) on synthetic
// frames only: the parameter-set rules, the pre-roll ring (IDR first, 6 s, the byte cap, one size), the
// 5 s pre-roll, the pass-through writer (one size, skipping to the first IDR), and the container
// experiment: a finished fragmented MP4 and a plain MP4 read back through IMFSourceReader with every
// frame, its time and exactly the IDRs as clean points, seeks land on the IDR at or before the position,
// and an fMP4 never finalized (a crash) stays readable up to its last fragment, where a plain MP4 is lost.
// Frames are generated patterns encoded by the software H.264 MFT; files go to a temp folder only.
#include "kv_test.hpp"

#include "mf_h264_encoder.hpp"
#include "roll_media.hpp"
#include "test_h264_decoder.hpp"
#include "video_convert.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

using namespace kvhost;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr uint32_t kW = 640, kH = 360;
constexpr int64_t kFrame = 333333;           // 30 fps in 100 ns
constexpr int64_t kBase = 123456789000LL;    // an arbitrary capture-clock origin: the file must start at 0
constexpr int kFrames = 300;                 // 10 s, an IDR every 30 frames
constexpr int64_t kStartUnixMs = 1795000000000LL; // a fixed clip start (2026-11-18), for the file times

// Capture times 1/30 s apart with up to +/-2 ms of jitter, so the file must carry real times.
int64_t captureOf(int i) { return kBase + static_cast<int64_t>(i) * kFrame + static_cast<int64_t>(((i * 7919) % 41) - 20) * 1000; }

std::wstring freshDir() {
    static std::atomic<int> counter{0};
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"kv-roll-media-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++counter);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

struct TempDir {
    std::wstring dir = freshDir();
    ~TempDir() { removeTree(dir); }
    std::wstring file(const wchar_t* name) const { return dir + L"\\" + name; }
};

bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Copies a file that another handle is still writing, as a crash would leave it.
bool copyOpenFile(const std::wstring& src, const std::wstring& dst, std::string& why) {
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        why = "open for reading failed, error " + std::to_string(GetLastError());
        return false;
    }
    HANDLE out = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        why = "create failed";
        return false;
    }
    std::vector<char> buf(1 << 16);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        DWORD put = 0;
        if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got) {
            ok = false;
            break;
        }
    }
    // The file a crash leaves keeps its creation time; so does this copy of it.
    FILETIME ct;
    if (ok && GetFileTime(in, &ct, nullptr, nullptr)) SetFileTime(out, &ct, nullptr, nullptr);
    CloseHandle(in);
    CloseHandle(out);
    if (!ok) why = "copy failed";
    return ok;
}

// ---- Synthetic NAL units for the pure tests ----

const Bytes kSps = {0x67, 0x42, 0xC0, 0x1E, 0xDA, 0x02, 0x80};
const Bytes kPps = {0x68, 0xCE, 0x3C, 0x80};
const Bytes kAud = {0x09, 0xF0};

Bytes nalIdr(uint8_t tag) { return {0x65, 0x88, 0x84, tag, 0x21}; }
Bytes nalP(uint8_t tag) { return {0x41, 0x9A, 0x02, tag}; }

Bytes annexB(const std::vector<Bytes>& nals) {
    Bytes out;
    for (const auto& n : nals) {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}

std::vector<uint8_t> nalTypes(const Bytes& annexb) {
    std::vector<uint8_t> t;
    for (const auto& n : splitAnnexB(annexb.data(), annexb.size())) t.push_back(static_cast<uint8_t>(n[0] & 0x1F));
    return t;
}

// One fake AU of a 30 fps stream with an IDR every `gop` frames. Only frame 0 carries SPS/PPS, as an
// encoder that sends them once would; padding makes its size `bytes`.
RollAccessUnit fakeAu(int i, int gop, uint32_t w = kW, uint32_t h = kH, size_t bytes = 0) {
    const bool idr = i % gop == 0;
    std::vector<Bytes> nals;
    if (i == 0) {
        nals.push_back(kSps);
        nals.push_back(kPps);
    }
    Bytes slice = idr ? nalIdr(static_cast<uint8_t>(i)) : nalP(static_cast<uint8_t>(i));
    if (bytes > slice.size() + 4) slice.resize(bytes - 4, 0x5A);
    nals.push_back(slice);
    const Bytes b = annexB(nals);
    return rollAccessUnitFromAnnexB(b.data(), b.size(), static_cast<int64_t>(i) * kFrame, w, h);
}

bool idrHasParams(const RollAccessUnit& au) {
    const auto t = nalTypes(au.annexb);
    return std::find(t.begin(), t.end(), 7) != t.end() && std::find(t.begin(), t.end(), 8) != t.end();
}

// ---- Real H.264 from the software encoder, made once ----

void makeNv12(int i, Bytes& f) {
    f.assign(static_cast<size_t>(kW) * kH * 3 / 2, 128);
    const uint32_t bx = static_cast<uint32_t>((i * 7) % static_cast<int>(kW - 96));
    const uint32_t by = static_cast<uint32_t>((i * 3) % static_cast<int>(kH - 96));
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            uint8_t v = static_cast<uint8_t>(((x / 16 + y / 16) & 1) ? 70 : 170);
            if (x >= bx && x < bx + 96 && y >= by && y < by + 96) v = 235;
            v = static_cast<uint8_t>(v + ((x * 13 + y * 7 + static_cast<uint32_t>(i) * 31) % 9));
            f[static_cast<size_t>(y) * kW + x] = v;
        }
    }
    uint8_t* uv = f.data() + static_cast<size_t>(kW) * kH;
    for (uint32_t y = 0; y < kH / 2; ++y) {
        for (uint32_t x = 0; x < kW / 2; ++x) {
            uv[static_cast<size_t>(y) * kW + x * 2] = static_cast<uint8_t>(96 + (x + static_cast<uint32_t>(i)) % 64);
            uv[static_cast<size_t>(y) * kW + x * 2 + 1] = static_cast<uint8_t>(96 + (y * 2) % 64);
        }
    }
}

struct Encoded {
    bool ok = false;
    std::string encoder;
    Bytes seq_header;
    std::vector<RollAccessUnit> aus; // in output order; capture_hns = captureOf(frame)
    std::vector<int> frame;          // the frame index of each AU
};

const Encoded& encoded() {
    static Encoded e = [] {
        Encoded r;
        MfH264Encoder enc;
        EncoderOptions o;
        o.prefer_hardware = false; // the software MFT: synchronous, one AU per frame, the same on every PC
        o.allow_software = true;
        o.fps = 30;
        o.gop = 30;
        o.bitrate = 2500000;
        std::string err;
        const bool opened = enc.open(kW, kH, o,
                                     [&](const uint8_t* d, size_t n, int64_t pts, uint64_t token) {
                                         r.aus.push_back(rollAccessUnitFromAnnexB(d, n, pts, kW, kH));
                                         r.frame.push_back(static_cast<int>(token));
                                     },
                                     &err);
        if (!opened) {
            std::printf("  encoder open failed: %s\n", err.c_str());
            return r;
        }
        r.encoder = enc.name();
        Bytes nv12;
        for (int i = 0; i < kFrames; ++i) {
            makeNv12(i, nv12);
            if (!enc.encode(nv12.data(), nv12.size(), captureOf(i), i % 30 == 0, static_cast<uint64_t>(i), &err)) {
                std::printf("  encode %d failed: %s\n", i, err.c_str());
                return r;
            }
        }
        r.seq_header = enc.sequenceHeader();
        enc.close();
        r.ok = !r.aus.empty() && r.aus.front().idr;
        std::printf("  encoder: %s, %zu AUs of %d frames, sequence header %zu B\n", r.encoder.c_str(), r.aus.size(),
                    kFrames, r.seq_header.size());
        return r;
    }();
    return e;
}

// The index of the first AU at or after frame f.
size_t auAtFrame(const Encoded& e, int f) {
    for (size_t k = 0; k < e.frame.size(); ++k)
        if (e.frame[k] >= f) return k;
    return e.frame.size();
}

// The fMP4's own sync-sample flags, sample by sample: each moof/traf/trun's per-sample flags, its
// first-sample flags, or the tfhd/trex default (ISO/IEC 14496-12 8.8). A sample is sync when
// sample_is_non_sync_sample (bit 16) is clear. This reads the file, not what IMFSourceReader says.
uint32_t rd32(const Bytes& b, size_t i) {
    return (static_cast<uint32_t>(b[i]) << 24) | (static_cast<uint32_t>(b[i + 1]) << 16) |
           (static_cast<uint32_t>(b[i + 2]) << 8) | b[i + 3];
}

bool readWholeFile(const std::wstring& path, Bytes& b) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    b.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(h, b.data(), static_cast<DWORD>(b.size()), &got, nullptr) && got == b.size();
    CloseHandle(h);
    return ok;
}

// Calls f(type, payload_begin, box_end) for each box in [pos, end); false on a malformed box.
template <typename F>
bool eachBox(const Bytes& b, size_t pos, size_t end, F f) {
    while (pos + 8 <= end) {
        const size_t size = rd32(b, pos);
        if (size < 8 || pos + size > end) return false;
        f(std::string(reinterpret_cast<const char*>(&b[pos + 4]), 4), pos + 8, pos + size);
        pos += size;
    }
    return pos == end;
}

bool fmp4SyncFlags(const std::wstring& path, std::vector<bool>& sync, size_t& fragments) {
    Bytes b;
    if (!readWholeFile(path, b)) return false;
    uint32_t trex_flags = 0;
    bool ok = true;
    sync.clear();
    fragments = 0;
    ok &= eachBox(b, 0, b.size(), [&](const std::string& t, size_t p, size_t e) {
        if (t == "moov") {
            ok &= eachBox(b, p, e, [&](const std::string& t2, size_t p2, size_t e2) {
                if (t2 != "mvex") return;
                ok &= eachBox(b, p2, e2, [&](const std::string& t3, size_t p3, size_t e3) {
                    if (t3 == "trex" && e3 - p3 >= 24) trex_flags = rd32(b, p3 + 20);
                });
            });
        }
        if (t != "moof") return;
        ++fragments;
        ok &= eachBox(b, p, e, [&](const std::string& t2, size_t p2, size_t e2) {
            if (t2 != "traf") return;
            uint32_t def = trex_flags;
            ok &= eachBox(b, p2, e2, [&](const std::string& t3, size_t p3, size_t e3) {
                if (t3 == "tfhd") {
                    const uint32_t f = rd32(b, p3) & 0xFFFFFF;
                    size_t q = p3 + 8; // version/flags, track_ID
                    if (f & 0x1) q += 8;
                    if (f & 0x2) q += 4;
                    if (f & 0x8) q += 4;
                    if (f & 0x10) q += 4;
                    if ((f & 0x20) && q + 4 <= e3) def = rd32(b, q);
                } else if (t3 == "trun") {
                    const uint32_t f = rd32(b, p3) & 0xFFFFFF;
                    const uint32_t count = rd32(b, p3 + 4);
                    size_t q = p3 + 8;
                    if (f & 0x1) q += 4;
                    uint32_t first = def;
                    const bool has_first = (f & 0x4) != 0;
                    if (has_first) {
                        first = rd32(b, q);
                        q += 4;
                    }
                    for (uint32_t k = 0; k < count; ++k) {
                        uint32_t flags = (k == 0 && has_first) ? first : def;
                        if (f & 0x100) q += 4;
                        if (f & 0x200) q += 4;
                        if (f & 0x400) {
                            if (q + 4 > e3) {
                                ok = false;
                                return;
                            }
                            flags = rd32(b, q);
                            q += 4;
                        }
                        if (f & 0x800) q += 4;
                        sync.push_back((flags & 0x10000) == 0);
                    }
                }
            });
        });
    });
    return ok;
}

const char* containerName(RollContainer c) { return c == RollContainer::FragmentedMp4 ? "fMP4" : "MP4"; }

// What a clip written from AU `from` to the end should read back as.
struct Expected {
    std::vector<int64_t> media;  // media time of each AU
    std::vector<bool> idr;
    std::vector<Bytes> avcc;     // annexBToAvcc of each AU
    Bytes sps, pps;
};

Expected expectedFrom(const Encoded& e, size_t from) {
    Expected x;
    RollParameterSets ps;
    ps.setSequenceHeader(e.seq_header.data(), e.seq_header.size());
    for (size_t k = from; k < e.aus.size(); ++k) {
        RollAccessUnit au = e.aus[k];
        ps.normalize(au);
        x.media.push_back(au.capture_hns - e.aus[from].capture_hns);
        x.idr.push_back(au.idr);
        AvccAccessUnit a;
        annexBToAvcc(au.annexb.data(), au.annexb.size(), a);
        x.avcc.push_back(a.avcc);
    }
    x.sps = ps.sps();
    x.pps = ps.pps();
    return x;
}

bool avccHasIdr(const Bytes& avcc) {
    size_t i = 0;
    while (i + 4 <= avcc.size()) {
        const size_t len = (static_cast<size_t>(avcc[i]) << 24) | (static_cast<size_t>(avcc[i + 1]) << 16) |
                           (static_cast<size_t>(avcc[i + 2]) << 8) | avcc[i + 3];
        i += 4;
        if (i + len > avcc.size() || len == 0) return false;
        if ((avcc[i] & 0x1F) == 5) return true;
        i += len;
    }
    return false;
}

// Writes a clip the way the roll recorder does: the ring is fed frames 0..209, the clip starts with its pre-roll
// (which must begin at the IDR of frame 60) and then takes the live frames 210..299.
struct WrittenClip {
    bool ok = false;
    size_t from = 0; // the first AU in the clip
    RollClipResult result;
};

WrittenClip writeClip(const Encoded& e, const std::wstring& path, RollContainer c) {
    WrittenClip w;
    RollPreRollRing ring;
    ring.setSequenceHeader(e.seq_header.data(), e.seq_header.size(), kW, kH);
    const size_t live = auAtFrame(e, 210);
    for (size_t k = 0; k < live; ++k) ring.push(e.aus[k]);
    const std::vector<RollAccessUnit> pre = ring.preRoll();
    KV_CHECK(!pre.empty() && pre.front().idr);
    if (pre.empty()) return w;
    w.from = auAtFrame(e, 60);
    KV_CHECK_MSG(pre.front().capture_hns == e.aus[w.from].capture_hns, "the pre-roll starts at frame 60's IDR");
    KV_CHECK(pre.size() == live - w.from);

    RollClipWriter writer;
    RollWriterOptions o;
    o.path = path;
    o.container = c;
    o.width = kW;
    o.height = kH;
    o.start_unix_ms = kStartUnixMs;
    o.sequence_header = ring.sequenceHeader();
    std::string err;
    KV_CHECK_MSG(writer.open(o, &err), err);
    KV_CHECK_MSG(writer.appendAll(pre, &err), err);
    for (size_t k = live; k < e.aus.size(); ++k) KV_CHECK_MSG(writer.append(e.aus[k], &err), err);
    w.result = writer.finalize();
    KV_CHECK_MSG(w.result.ok, w.result.error);
    w.ok = w.result.ok;
    return w;
}

void checkRoundTrip(RollContainer c) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Encoded& e = encoded();
    KV_REQUIRE(e.ok);
    TempDir tmp;
    const std::wstring path = tmp.file(L"14-03-22_motion.mp4");
    const WrittenClip w = writeClip(e, path, c);
    KV_REQUIRE(w.ok);
    const Expected x = expectedFrom(e, w.from);
    const size_t n = x.media.size();
    const int64_t want_dur = x.media.back() + (x.media.back() - x.media[n - 2]);
    std::printf("  %s: %llu frames, %llu keyframes, %.3f s, %llu bytes\n", containerName(c),
                static_cast<unsigned long long>(w.result.frames), static_cast<unsigned long long>(w.result.keyframes),
                w.result.durationSec(), static_cast<unsigned long long>(w.result.bytes));
    KV_CHECK(w.result.frames == n);
    KV_CHECK(w.result.keyframes == static_cast<uint64_t>(std::count(x.idr.begin(), x.idr.end(), true)));
    KV_CHECK(w.result.skipped_before_idr == 0);
    KV_CHECK(w.result.duration_hns == want_dur);
    KV_CHECK(w.result.first_capture_hns == e.aus[w.from].capture_hns);
    KV_CHECK(w.result.creation_time_set);

    RollClipReader rd;
    std::string err;
    KV_REQUIRE(rd.open(path, &err));
    std::printf("  reader: %ux%u, duration %.3f s (%s)\n", rd.width(), rd.height(), rd.durationSec(),
                rd.durationFromScan() ? "from a scan" : "MF_PD_DURATION");
    KV_CHECK(rd.width() == kW && rd.height() == kH);
    KV_CHECK_MSG(std::llabs(rd.durationHns() - want_dur) <= kFrame, std::to_string(rd.durationHns()));
    KV_CHECK(rd.sps() == x.sps);
    KV_CHECK(rd.pps() == x.pps);
    KV_CHECK(rd.parameterSets().size() == 2);

    size_t got = 0, bad_time = 0, bad_key = 0, bad_clean = 0, bad_bytes = 0, idr_in_non_key = 0;
    int64_t max_err = 0;
    RollSample s;
    for (;;) {
        const RollClipReader::Read r = rd.next(s, &err);
        if (r == RollClipReader::Read::End) break;
        KV_REQUIRE(r == RollClipReader::Read::Sample);
        if (got < n) {
            const int64_t d = std::llabs(s.pts_hns - x.media[got]);
            max_err = std::max(max_err, d);
            if (d > kFrame) ++bad_time;
            if (s.keyframe != x.idr[got]) ++bad_key;
            if (s.clean_point != x.idr[got]) {
                std::printf("  sample %zu at %.3f s: MF clean point %d, IDR %d\n", got,
                            static_cast<double>(s.pts_hns) / kRollHnsPerSecond, s.clean_point ? 1 : 0, x.idr[got] ? 1 : 0);
                ++bad_clean;
            }
            if (avccHasIdr(s.avcc) != s.keyframe) ++idr_in_non_key;
            if (s.avcc != x.avcc[got]) ++bad_bytes;
            if (got == 0) KV_CHECK_MSG(s.pts_hns == 0, "the first sample is at 0");
        }
        ++got;
    }
    std::printf("  read back %zu of %zu; max time error %.3f ms\n", got, n, static_cast<double>(max_err) / 10000.0);
    KV_CHECK(got == n);
    KV_CHECK(bad_time == 0);         // every timestamp within one frame
    KV_CHECK(bad_key == 0);          // every IDR, and only the IDRs, is a keyframe
    if (c == RollContainer::Mp4) {
        KV_CHECK(bad_clean == 0);    // and MF's clean points are exactly the IDRs
    } else {
        // The MPEG-4 source over-reports one clean point on fMP4 (the first sample of the second
        // fragment); the file itself is right, as its sync flags show. The reader decides by NAL type.
        KV_CHECK(bad_clean <= 1);
        std::vector<bool> sync;
        size_t fragments = 0;
        KV_CHECK(fmp4SyncFlags(path, sync, fragments));
        std::printf("  fMP4: %zu fragments; MF clean points that are not IDRs: %zu\n", fragments, bad_clean);
        KV_CHECK(fragments >= 2);
        KV_CHECK_MSG(sync == x.idr, "the file's sync-sample flags are exactly the IDRs");
    }
    KV_CHECK(idr_in_non_key == 0);
    KV_CHECK_MSG(bad_bytes == 0, std::to_string(bad_bytes) + " samples differ from what the encoder made");
    rd.close();

    // The clip start as the file's creation time: mvhd (seconds) and NTFS (ms).
    int64_t mvhd_ms = 0;
    KV_CHECK(rollMp4CreationUnixMs(path, mvhd_ms));
    KV_CHECK_MSG(mvhd_ms == kStartUnixMs / 1000 * 1000, std::to_string(mvhd_ms));
    RollProbeResult pr;
    KV_REQUIRE(probeRollClip(path, pr, &err));
    KV_CHECK(!pr.truncated);
    KV_CHECK(pr.frames == n);
    KV_CHECK(pr.width == kW && pr.height == kH);
    KV_CHECK(std::llabs(pr.duration_hns - want_dur) <= kFrame);
    KV_CHECK(pr.start_unix_ms == kStartUnixMs);
    KV_CHECK(std::llabs(pr.end_unix_ms - (kStartUnixMs + want_dur / 10000)) <= kFrame / 10000 + 1);
    KV_CHECK(pr.bytes == w.result.bytes);
}

void checkSeek(RollContainer c) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Encoded& e = encoded();
    KV_REQUIRE(e.ok);
    TempDir tmp;
    const std::wstring path = tmp.file(L"14-05-40_manual.mp4");
    const WrittenClip w = writeClip(e, path, c);
    KV_REQUIRE(w.ok);
    RollClipReader rd;
    std::string err;
    KV_REQUIRE(rd.open(path, &err));
    const int64_t dur = rd.durationHns();
    // The IDR times as the file stores them (its timescale rounds them by a few microseconds).
    std::vector<int64_t> idr_times;
    {
        RollSample s;
        while (rd.next(s, &err) == RollClipReader::Read::Sample)
            if (s.keyframe) idr_times.push_back(s.pts_hns);
    }
    KV_REQUIRE(idr_times.size() >= 5);
    // Positions: the start, mid-GOP, exactly on an IDR, just before one, near the end, past the end.
    const std::vector<int64_t> positions = {0, kRollHnsPerSecond / 2, idr_times[2], idr_times[3] - 1,
                                            idr_times[1] + kRollHnsPerSecond / 2, dur - kRollHnsPerSecond / 10,
                                            dur + 5 * kRollHnsPerSecond};
    for (const int64_t pos : positions) {
        const int64_t clamped = std::min(pos, dur);
        int64_t want = 0;
        for (const int64_t t : idr_times)
            if (t <= clamped) want = t;
        KV_REQUIRE(rd.seek(pos, &err));
        RollSample s;
        const RollClipReader::Read rr = rd.next(s, &err);
        if (rr != RollClipReader::Read::Sample) std::printf("  seek %.3f: next -> %d %s\n", static_cast<double>(pos) / 1e7, static_cast<int>(rr), err.c_str());
        KV_REQUIRE(rr == RollClipReader::Read::Sample);
        std::printf("  %s seek %.3f s -> %.3f s (%s), want %.3f s\n", containerName(c),
                    static_cast<double>(pos) / kRollHnsPerSecond, static_cast<double>(s.pts_hns) / kRollHnsPerSecond,
                    s.keyframe ? "key" : "not key", static_cast<double>(want) / kRollHnsPerSecond);
        KV_CHECK(s.keyframe);
        KV_CHECK(std::llabs(s.pts_hns - want) < kFrame / 2); // the IDR at or before the position

        // What playback sends decodes from there: AVCC + the 0x18 parameter sets on the keyframe.
        kvtest::TestH264Decoder dec;
        KV_REQUIRE(dec.open());
        int decoded = 0;
        for (int k = 0; k < 10; ++k) {
            const Bytes ab = kvtest::avccToAnnexB(s.avcc.data(), s.avcc.size(),
                                                  s.keyframe ? rd.parameterSets() : std::vector<Bytes>{});
            if (dec.decode(ab.data(), ab.size())) ++decoded;
            if (rd.next(s, &err) != RollClipReader::Read::Sample) break;
        }
        KV_CHECK(decoded >= 1);
        KV_CHECK(dec.width() == kW && dec.height() >= kH);
    }
}

} // namespace

// ---- Parameter sets ----

KV_TEST(parameter_sets_go_in_front_of_every_idr) {
    RollParameterSets ps;
    RollAccessUnit lone;
    {
        const Bytes b = annexB({nalIdr(1)});
        lone = rollAccessUnitFromAnnexB(b.data(), b.size(), 0, kW, kH);
    }
    KV_CHECK(lone.idr);
    RollAccessUnit copy = lone;
    KV_CHECK_MSG(!ps.normalize(copy), "an IDR before any SPS/PPS can't decode on its own");

    const Bytes full = annexB({kSps, kPps, nalIdr(2)});
    RollAccessUnit a = rollAccessUnitFromAnnexB(full.data(), full.size(), 0, kW, kH);
    KV_CHECK(ps.normalize(a));
    KV_CHECK(a.annexb == full); // already self-contained: untouched
    KV_CHECK(ps.sps() == kSps && ps.pps() == kPps);
    KV_CHECK(ps.sequenceHeader() == annexB({kSps, kPps}));

    const Bytes p = annexB({nalP(3)});
    RollAccessUnit b = rollAccessUnitFromAnnexB(p.data(), p.size(), 0, kW, kH);
    KV_CHECK(ps.normalize(b) && !b.idr && b.annexb == p);

    // An IDR without them gets them, after its access unit delimiter.
    const Bytes aud_idr = annexB({kAud, nalIdr(4)});
    RollAccessUnit c = rollAccessUnitFromAnnexB(aud_idr.data(), aud_idr.size(), 0, kW, kH);
    KV_CHECK(ps.normalize(c));
    KV_CHECK(c.annexb == annexB({kAud, kSps, kPps, nalIdr(4)}));

    // Parameter sets seen at one size are never put on an IDR of another.
    RollAccessUnit big = rollAccessUnitFromAnnexB(aud_idr.data(), aud_idr.size(), 0, 1280, 720);
    KV_CHECK(!ps.normalize(big));

    // TierAccessUnit's form back to Annex-B.
    AvccAccessUnit av;
    KV_REQUIRE(annexBToAvcc(full.data(), full.size(), av));
    const RollAccessUnit r = rollAccessUnitFromAvcc(av.avcc, av.sps, av.pps, true, 77, kW, kH);
    KV_CHECK(r.idr && r.capture_hns == 77 && r.annexb == full);

    // An avcC-form sequence header is read too.
    Bytes avcc_rec = {1, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0, static_cast<uint8_t>(kSps.size())};
    avcc_rec.insert(avcc_rec.end(), kSps.begin(), kSps.end());
    avcc_rec.push_back(1);
    avcc_rec.push_back(0);
    avcc_rec.push_back(static_cast<uint8_t>(kPps.size()));
    avcc_rec.insert(avcc_rec.end(), kPps.begin(), kPps.end());
    RollParameterSets ps2;
    ps2.setSequenceHeader(avcc_rec.data(), avcc_rec.size());
    KV_CHECK(ps2.known() && ps2.sps() == kSps && ps2.pps() == kPps);
}

// ---- The pre-roll ring ----

KV_TEST(ring_starts_at_an_idr_and_holds_at_most_6s) {
    RollPreRollRing ring;
    // A stream joined mid-GOP: nothing is kept before an IDR, nor an IDR whose SPS/PPS are unknown.
    for (int i = 10; i < 30; ++i) KV_CHECK(!ring.push(fakeAu(i, 30)));
    KV_CHECK(!ring.push(fakeAu(30, 30)));
    KV_CHECK(ring.size() == 0);
    RollPreRollRing ring2;
    int64_t max_span = 0;
    bool always_idr_first = true, always_params = true;
    for (int i = 0; i < 20 * 30; ++i) {
        KV_CHECK(ring2.push(fakeAu(i, 30)));
        max_span = std::max(max_span, ring2.spanHns());
        const auto all = ring2.preRoll(kRollRingSpanHns);
        if (all.empty() || !all.front().idr) always_idr_first = false;
        for (const auto& au : all)
            if (au.idr && !idrHasParams(au)) always_params = false;
        if (i >= 8 * 30) KV_CHECK(ring2.spanHns() >= 5 * kRollHnsPerSecond); // whole 1 s GOPs: 5..6 s held
    }
    std::printf("  max span %.3f s, now %zu AUs, %zu bytes\n", static_cast<double>(max_span) / kRollHnsPerSecond,
                ring2.size(), ring2.bytes());
    KV_CHECK(max_span <= kRollRingSpanHns);
    KV_CHECK(always_idr_first);
    KV_CHECK_MSG(always_params, "every IDR in the ring carries SPS/PPS");

    // A GOP longer than the span can't be cut inside: the ring empties and restarts at the next IDR.
    RollPreRollRing::Options o;
    o.span_hns = kRollHnsPerSecond;
    RollPreRollRing longgop(o);
    for (int i = 0; i < 6 * 30; ++i) {
        longgop.push(fakeAu(i, 60));
        KV_CHECK(longgop.spanHns() <= kRollHnsPerSecond);
        const auto all = longgop.preRoll(kRollRingSpanHns);
        if (!all.empty()) KV_CHECK(all.front().idr);
    }
}

KV_TEST(ring_byte_cap_drops_whole_gops) {
    RollPreRollRing::Options o;
    o.byte_cap = 45 * 1000; // 1.5 GOPs of 1000-byte AUs
    RollPreRollRing ring(o);
    for (int i = 0; i < 10 * 30; ++i) {
        ring.push(fakeAu(i, 30, kW, kH, 1000));
        KV_CHECK(ring.bytes() <= o.byte_cap);
        const auto all = ring.preRoll(kRollRingSpanHns);
        KV_CHECK(!all.empty() && all.front().idr);
    }
    KV_CHECK(ring.size() >= 15 && ring.size() <= 45);
    // One GOP bigger than the cap: the ring holds nothing rather than a cut GOP.
    o.byte_cap = 10 * 1000;
    RollPreRollRing tiny(o);
    for (int i = 0; i < 90; ++i) tiny.push(fakeAu(i, 30, kW, kH, 1000));
    KV_CHECK(tiny.bytes() <= o.byte_cap);
    const auto all = tiny.preRoll(kRollRingSpanHns);
    if (!all.empty()) KV_CHECK(all.front().idr);
}

KV_TEST(ring_restarts_on_a_size_change) {
    RollPreRollRing ring;
    for (int i = 0; i < 75; ++i) ring.push(fakeAu(i, 30));
    KV_CHECK(ring.size() > 0);
    // The view changed: a P frame of the new size empties the ring and is not kept.
    RollAccessUnit p = fakeAu(75, 30, 1280, 720);
    KV_CHECK(!ring.push(p));
    KV_CHECK(ring.size() == 0);
    // The new encoder's first IDR carries its own SPS/PPS; from then on the ring is all 1280x720.
    const Bytes b = annexB({kSps, kPps, nalIdr(9)});
    KV_CHECK(ring.push(rollAccessUnitFromAnnexB(b.data(), b.size(), 90 * kFrame, 1280, 720)));
    KV_CHECK(ring.push(fakeAu(91, 30, 1280, 720)));
    for (const auto& au : ring.preRoll(kRollRingSpanHns)) KV_CHECK(au.width == 1280 && au.height == 720);
    KV_CHECK(ring.size() == 2);
}

KV_TEST(preroll_is_at_most_5s_and_starts_at_an_idr) {
    RollPreRollRing ring;
    for (int i = 0; i < 8 * 30 + 20; ++i) ring.push(fakeAu(i, 30)); // the newest is 19 frames into a GOP
    const size_t before = ring.size();
    const auto pre = ring.preRoll();
    KV_REQUIRE(!pre.empty());
    const int64_t span = pre.back().capture_hns - pre.front().capture_hns;
    std::printf("  pre-roll %.3f s, %zu AUs; ring %.3f s\n", static_cast<double>(span) / kRollHnsPerSecond, pre.size(),
                static_cast<double>(ring.spanHns()) / kRollHnsPerSecond);
    KV_CHECK(pre.front().idr && idrHasParams(pre.front()));
    KV_CHECK(span <= kRollPreRollMaxHns);
    KV_CHECK(span >= 4 * kRollHnsPerSecond);
    KV_CHECK(ring.size() == before); // a copy: the ring keeps running for the next clip
    // The newest IDR is 19 frames (0.63 s) old: a 0.5 s pre-roll has none to start at, a 1 s one has.
    KV_CHECK(ring.preRoll(kRollHnsPerSecond / 2).empty());
    const auto one = ring.preRoll(kRollHnsPerSecond);
    KV_CHECK(one.size() == 20 && one.front().idr);
}

// ---- The writer ----

KV_TEST(writer_takes_one_size_and_starts_at_the_first_idr) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Encoded& e = encoded();
    KV_REQUIRE(e.ok);
    TempDir tmp;
    const std::wstring path = tmp.file(L"14-07-00_manual.mp4");
    RollClipWriter w;
    RollWriterOptions o;
    o.path = path;
    o.width = kW;
    o.height = kH;
    o.sequence_header = e.seq_header;
    std::string err;
    KV_REQUIRE(w.open(o, &err));
    // Started without pre-roll mid-GOP: AUs before the next IDR are skipped.
    for (size_t k = 20; k < 70; ++k) KV_CHECK(w.append(e.aus[k], &err));
    // A frame of another size is refused: a clip has one size (pet-camera.md §6.1).
    RollAccessUnit other = e.aus[70];
    other.width = 1280;
    other.height = 720;
    KV_CHECK(!w.append(other, &err));
    KV_CHECK(err.find("one size") != std::string::npos);
    // The writer goes on taking the clip's own size.
    KV_CHECK(w.append(e.aus[70], &err));
    // A second writer can't take the same file: a clip is never written over.
    RollClipWriter w2;
    KV_CHECK(!w2.open(o, &err));
    const RollClipResult r = w.finalize();
    KV_CHECK_MSG(r.ok, r.error);
    KV_CHECK(r.skipped_before_idr == auAtFrame(e, 30) - 20);
    KV_CHECK(r.frames == 71 - auAtFrame(e, 30));
    KV_CHECK(r.keyframes == 2);
    KV_CHECK(!r.creation_time_set); // no start time given
    RollProbeResult pr;
    KV_CHECK_MSG(probeRollClip(path, pr, &err), err);
    KV_CHECK(pr.frames == r.frames && pr.keyframes == 2);
    KV_CHECK(!w.append(e.aus[71], &err)); // finalized: closed
}

KV_TEST(writer_with_no_idr_leaves_no_file) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Encoded& e = encoded();
    KV_REQUIRE(e.ok);
    TempDir tmp;
    const std::wstring path = tmp.file(L"14-08-00_motion.mp4");
    RollClipWriter w;
    RollWriterOptions o;
    o.path = path;
    o.width = kW;
    o.height = kH;
    std::string err;
    KV_REQUIRE(w.open(o, &err));
    for (size_t k = 1; k < 20; ++k) KV_CHECK(w.append(e.aus[k], &err));
    const RollClipResult r = w.finalize();
    KV_CHECK(!r.ok && r.frames == 0 && r.skipped_before_idr == 19);
    KV_CHECK(!fileExists(path));
}

// The host opens the writer over the empty .mp4 that RollStore::reserveClip created to take the name.
KV_TEST(writer_opens_over_an_empty_reservation_and_never_over_a_clip) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Encoded& e = encoded();
    KV_REQUIRE(e.ok);
    TempDir tmp;
    const std::wstring path = tmp.file(L"14-09-00_manual.mp4");
    RollWriterOptions o;
    o.path = path;
    o.width = kW;
    o.height = kH;
    o.start_unix_ms = kStartUnixMs;
    std::string err;
    {
        // Without the flag the reservation is a file that exists: refused, as before.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        CloseHandle(h);
        RollClipWriter w;
        KV_CHECK(!w.open(o, &err));
    }
    o.reserved_empty_file = true;
    RollClipWriter w;
    const bool opened = w.open(o, &err);
    KV_CHECK_MSG(opened, err);
    KV_REQUIRE(opened);
    for (size_t k = 0; k < 40; ++k) KV_CHECK_MSG(w.append(e.aus[k], &err), err);
    const RollClipResult r = w.finalize();
    KV_CHECK_MSG(r.ok && r.frames == 40, r.error);
    RollProbeResult pr;
    KV_CHECK_MSG(probeRollClip(path, pr, &err), err);
    KV_CHECK(pr.frames == 40);
    // The finished clip is not empty: opening over it is refused, and it is left as it was.
    WIN32_FILE_ATTRIBUTE_DATA before{}, after{};
    KV_REQUIRE(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &before));
    RollClipWriter again;
    KV_CHECK(!again.open(o, &err));
    KV_REQUIRE(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &after));
    KV_CHECK(before.nFileSizeLow == after.nFileSizeLow && before.nFileSizeLow > 0);
    // Opening one that isn't there at all fails too (the reservation must exist).
    o.path = tmp.file(L"14-09-01_manual.mp4");
    RollClipWriter missing;
    KV_CHECK(!missing.open(o, &err));
}

// ---- The container experiment ----

KV_TEST(fmp4_reads_back_every_frame_time_and_keyframe) { checkRoundTrip(RollContainer::FragmentedMp4); }
KV_TEST(mp4_reads_back_every_frame_time_and_keyframe) { checkRoundTrip(RollContainer::Mp4); }
KV_TEST(fmp4_seek_lands_on_the_idr_at_or_before) { checkSeek(RollContainer::FragmentedMp4); }
KV_TEST(mp4_seek_lands_on_the_idr_at_or_before) { checkSeek(RollContainer::Mp4); }

// pet-camera.md §6.1/§6.3: an fMP4 cut off by a crash is readable up to its last fragment.
KV_TEST(fmp4_interrupted_clip_is_readable_to_its_last_fragment) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Encoded& e = encoded();
    KV_REQUIRE(e.ok);
    TempDir tmp;
    const std::wstring path = tmp.file(L"14-09-00_motion.mp4");
    const std::wstring copy = tmp.file(L"crash-copy.mp4");
    const Expected x = expectedFrom(e, 0);
    RollClipWriter w;
    RollWriterOptions o;
    o.path = path;
    o.container = RollContainer::FragmentedMp4;
    o.width = kW;
    o.height = kH;
    o.start_unix_ms = kStartUnixMs;
    o.sequence_header = e.seq_header;
    std::string err;
    KV_REQUIRE(w.open(o, &err));
    const size_t first = auAtFrame(e, 150) + 1; // 5 GOPs and one frame of the 6th
    for (size_t k = 0; k < first; ++k) KV_CHECK(w.append(e.aus[k], &err));
    KV_CHECK(w.waitIdle(10000));
    // The file stream's queued writes land in their own time. Copy (and probe the copy) until it holds what was
    // handed over, less at most the fragment being filled, with a deadline, rather than after a fixed wait.
    // The power goes off at the copy that holds it: that copy is what the disk holds.
    std::string why;
    bool copied = false;
    const auto copy_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        copied = copyOpenFile(path, copy, why);
        RollProbeResult cp;
        std::string cerr;
        if (copied && probeRollClip(copy, cp, &cerr) && cp.frames + 30 >= first - 1) break;
        if (std::chrono::steady_clock::now() >= copy_until) break;
        Sleep(20);
    }
    if (!copied) std::printf("  copy while the writer is open: %s\n", why.c_str());
    KV_CHECK_MSG(copied, why);
    for (size_t k = first; k < auAtFrame(e, 200); ++k) KV_CHECK(w.append(e.aus[k], &err));
    KV_CHECK(w.waitIdle(10000));
    const uint64_t handed = w.samplesWritten();
    w.abandon(); // and the process ends without Finalize

    auto checkRecovered = [&](const std::wstring& p, uint64_t at_most, const char* what) -> uint64_t {
        RollProbeResult pr;
        std::string perr;
        const bool ok = probeRollClip(p, pr, &perr);
        std::printf("  %s: %s, %llu frames, %llu keyframes, %.3f s%s, %llu bytes\n", what, ok ? "readable" : perr.c_str(),
                    static_cast<unsigned long long>(pr.frames), static_cast<unsigned long long>(pr.keyframes),
                    pr.durationSec(), pr.truncated ? " (truncated)" : "", static_cast<unsigned long long>(pr.bytes));
        KV_CHECK_MSG(ok, perr);
        if (!ok) return 0;
        KV_CHECK(pr.frames <= at_most);
        // Lost: at most the fragment being filled (the sink cuts one every 10 samples), never the clip.
        KV_CHECK(pr.frames + 30 >= at_most);
        KV_CHECK(pr.width == kW && pr.height == kH);
        KV_CHECK(pr.sps == x.sps && pr.pps == x.pps);
        KV_CHECK(pr.start_unix_ms == kStartUnixMs); // set when the file was created
        // Every recovered frame is the one written, at its time, keyframes exactly on the IDRs.
        RollClipReader rd;
        if (!rd.open(p, &perr)) {
            KV_CHECK_MSG(false, perr);
            return 0;
        }
        RollSample s;
        size_t k = 0, bad = 0;
        while (k < pr.frames && rd.next(s, &perr) == RollClipReader::Read::Sample) {
            if (std::llabs(s.pts_hns - x.media[k]) > kFrame || s.keyframe != x.idr[k] || s.avcc != x.avcc[k]) ++bad;
            ++k;
        }
        KV_CHECK(k == pr.frames && bad == 0);
        return pr.frames;
    };
    uint64_t from_copy = 0;
    if (copied) from_copy = checkRecovered(copy, first - 1, "crash copy");
    const uint64_t from_abandoned = checkRecovered(path, handed, "abandoned file");
    KV_CHECK(from_abandoned >= from_copy);

    // A recovered (interrupted) clip plays to its end: the reader gives End after its last whole fragment, so
    // playback goes to ended, and after a seek it reads to End again. The reader now also takes an error there, on
    // a clip its open() found truncated, as the end (an error before that point is still an error). Media Foundation
    // here ends an interrupted fMP4 cleanly, even one cut inside its last moof or mdat (tried while writing this),
    // so this checks the behaviour asked for; the error-at-the-end path itself isn't reproduced.
    for (const std::wstring& p : {copy, path}) {
        if (p == copy && !copied) continue;
        RollClipReader rd;
        std::string rerr;
        KV_REQUIRE(rd.open(p, &rerr));
        RollSample s;
        RollClipReader::Read r = RollClipReader::Read::Sample;
        uint64_t n = 0;
        std::vector<int64_t> all_pts, idr_pts;
        while ((r = rd.next(s, &rerr)) == RollClipReader::Read::Sample) {
            ++n;
            all_pts.push_back(s.pts_hns);
            if (s.keyframe) idr_pts.push_back(s.pts_hns);
        }
        KV_CHECK_MSG(r == RollClipReader::Read::End, rerr);
        KV_CHECK(n > 0);
        KV_REQUIRE(rd.seek(kRollHnsPerSecond, &rerr));
        uint64_t m = 0;
        int64_t first_pts = -1;
        bool first_key = false;
        while ((r = rd.next(s, &rerr)) == RollClipReader::Read::Sample) {
            if (first_pts < 0) {
                first_pts = s.pts_hns;
                first_key = s.keyframe;
            }
            ++m;
        }
        std::printf("  %s: truncated %d, %llu frames; after a seek to 1 s, %llu frames from %.3f s\n",
                    p == copy ? "crash copy" : "abandoned file", rd.truncated() ? 1 : 0, static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(m), static_cast<double>(first_pts) / kRollHnsPerSecond);
        KV_CHECK_MSG(r == RollClipReader::Read::End && m > 0, rerr);
        // v3 §5.9: a seek is keyframe-accurate, on a recovered clip too: it starts at the last IDR at or before the
        // position (the capture times jitter by up to 2 ms, so the IDR near 1 s may fall just after it), and from
        // there every frame follows.
        int64_t want = -1;
        for (int64_t t : idr_pts)
            if (t <= kRollHnsPerSecond) want = t;
        uint64_t want_n = 0;
        for (int64_t t : all_pts)
            if (t >= want) ++want_n;
        KV_CHECK_MSG(first_key, "the first frame after the seek isn't a keyframe");
        KV_CHECK_MSG(first_pts == want, "the seek to 1 s started at " + std::to_string(first_pts) + ", not the IDR at " +
                                            std::to_string(want));
        KV_CHECK_MSG(m == want_n, "frames after the seek: " + std::to_string(m) + ", expected " + std::to_string(want_n));
    }
}

// The other half of the decision: a plain MP4 writes its index (moov) only at Finalize, so the same
// crash leaves nothing readable and the leftover pass has to delete it (pet-camera.md §6.3).
KV_TEST(mp4_interrupted_clip_is_lost) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Encoded& e = encoded();
    KV_REQUIRE(e.ok);
    TempDir tmp;
    const std::wstring path = tmp.file(L"14-10-00_motion.mp4");
    RollClipWriter w;
    RollWriterOptions o;
    o.path = path;
    o.container = RollContainer::Mp4;
    o.width = kW;
    o.height = kH;
    o.sequence_header = e.seq_header;
    std::string err;
    KV_REQUIRE(w.open(o, &err));
    for (size_t k = 0; k < auAtFrame(e, 151); ++k) KV_CHECK(w.append(e.aus[k], &err));
    KV_CHECK(w.waitIdle(10000));
    w.abandon();
    RollProbeResult pr;
    const bool ok = probeRollClip(path, pr, &err);
    std::printf("  plain MP4 after a crash: %s\n", ok ? "readable" : err.c_str());
    KV_CHECK(!ok);
}

KV_TEST(probe_refuses_what_is_not_a_clip) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TempDir tmp;
    RollProbeResult pr;
    std::string err;
    KV_CHECK(!probeRollClip(tmp.file(L"missing.mp4"), pr, &err));
    const std::wstring junk = tmp.file(L"14-11-00_motion.mp4");
    HANDLE h = CreateFileW(junk.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    Bytes noise(64 * 1024);
    uint32_t seed = 12345;
    for (auto& b : noise) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    DWORD put = 0;
    WriteFile(h, noise.data(), static_cast<DWORD>(noise.size()), &put, nullptr);
    CloseHandle(h);
    KV_CHECK(!probeRollClip(junk, pr, &err));
    int64_t ms = 0;
    KV_CHECK(!rollMp4CreationUnixMs(junk, ms));
    RollClipReader rd;
    KV_CHECK(!rd.open(junk, &err));
}

KV_TEST_MAIN()
