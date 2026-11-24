// W7 B1 end to end: the camera roll on a fake v3 device over loopback (docs/protocol-v3-pet-camera.md
// §5.1-§5.2, §5.6-§5.10, §6.4, §7; docs/pet-camera.md §6). The synthetic engine feeds HostController, whose
// `medium` tier the roll records; the fake device lists, fetches thumbnails, plays clips and drives the
// clip and star commands, and decodes what it is sent.
//
// Synthetic only (pet-camera.md §1): the synthetic source, synthetic depth arrays, and temp state and roll
// folders of this test's own (v3 §10 "Test isolation"), removed at the end. Nothing here opens the real
// camera roll, the real .host-state or the Kinect. Every wait is on a condition with a deadline, never a
// fixed sleep followed by a count.
#include "kv_test.hpp"

#include "fake_phone.hpp"
#include "host_controller.hpp"
#include "host_power.hpp"
#include "kinect_core_c.h"
#include "pet_settings.hpp"
#include "roll_store.hpp"
#include "state_files.hpp"
#include "test_h264_decoder.hpp"

#include <objbase.h>
#include <timeapi.h>
#include <winioctl.h> // FSCTL_SET_SPARSE: synthetic large clips, no data written

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace kvhost;
using kvtest::FakePhone;

namespace {

uint64_t nowNs() { return kv_monotonic_ns(); }
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool waitUntil(const std::function<bool()>& pred, int timeout_ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        sleepMs(10);
    }
    return pred();
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(p);
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Every file under dir whose name ends with ext.
int countFiles(const std::wstring& dir, const wchar_t* ext) {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            n += countFiles(dir + L"\\" + name, ext);
        } else {
            const std::wstring e = ext;
            if (name.size() >= e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) ++n;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// A temp root of this test's own: <TEMP>\kv-roll-e2e-<tag>-<pid>\ with .host-state and camera-roll in it.
struct Dirs {
    std::wstring root, state, roll, recordings;
    explicit Dirs(const wchar_t* tag) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        root = std::wstring(tmp) + L"kv-roll-e2e-" + tag + L"-" + std::to_wstring(GetCurrentProcessId());
        removeTree(root); // what a killed run with the same PID left
        CreateDirectoryW(root.c_str(), nullptr);
        state = root + L"\\.host-state";
        roll = root + L"\\camera-roll";
        recordings = root + L"\\recordings";
    }
    ~Dirs() { removeTree(root); }
    Dirs(const Dirs&) = delete;
    Dirs& operator=(const Dirs&) = delete;
};

// The trigger set's clock, moved forward by the test (the 10-minute segment cut, motion's 10 s hold)
// without waiting. Thread-safe: the recorder's thread reads it.
class TestRollClock final : public RollClock {
public:
    uint64_t monoMs() const override { return GetTickCount64() + offset_.load(); }
    RollLocalTime localTime() const override { return SystemRollClock().localTime(); }
    void advance(uint64_t ms) { offset_ += ms; }

private:
    std::atomic<uint64_t> offset_{0};
};

// The synthetic engine feeding HostController the way the viewer does, with the sensor reported on.
struct RollHost {
    kv_engine_t* eng = nullptr;
    HostController ctl;
    std::thread feeder;
    std::atomic<bool> stop{false};
    std::atomic<bool> frozen{false}; // as the viewer: no new frames while the view is frozen
    std::atomic<int> disconnects{0};
    std::atomic<int> turn_off_requests{0};

    // with_roll: a camera-roll folder in dirs (the default); without one the host has no roll at all.
    bool start(const Dirs& dirs, std::string* err, bool with_roll = true,
               const std::function<void(HostController::Options&)>& tweak = {}) {
        eng = kv_engine_create(nullptr);
        kv_display_params_t p;
        kv_default_display_params(&p);
        char e[256] = {0};
        if (!kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, false, e, sizeof(e))) {
            *err = e;
            return false;
        }
        HostController::Options o;
        o.server.state_dir = dirs.state;
        o.server.loopback_only = true;
        o.server.advertise = false;
        // A test run, as every serve and viewer test launch is: the roll takes synthetic frames
        // only, and its folder must be new, empty or a test roll (the host marks the one it prepares). A test that
        // needs otherwise sets it in `tweak`, saying why.
        o.test_run = true;
        // `medium` on the software encoder: the roll's tier never waits for a GPU encoder session another
        // process holds (the PC running these is often busy). high keeps today's selection.
        o.tier_hardware_allowed = {true, false, false};
        o.tier_hold = std::chrono::milliseconds(3000);
        if (with_roll) {
            o.roll.dir = dirs.roll;
            o.roll.recordingsDir = dirs.recordings;
        }
        if (tweak) tweak(o);
        ctl.onPhoneDisconnected = [this](bool turn_off) {
            ++disconnects;
            if (turn_off) ++turn_off_requests;
        };
        if (!ctl.start(o, err)) return false;
        setSensor(true);
        feeder = std::thread([this] {
            std::vector<uint8_t> buf(1920 * 1080 * 4);
            uint64_t last = 0;
            while (!stop) {
                size_t req = 0;
                kv_mapped_frame_t m{};
                if (!frozen && kv_engine_copy_latest_frame(eng, last, buf.data(), buf.size(), &req, &m)) {
                    last = m.frame_seq;
                    ctl.submitFrame(buf.data(), m, static_cast<int>(m.mode), 0, false);
                }
                sleepMs(1);
            }
        });
        return true;
    }

    void setSensor(bool on) {
        HostPowerModel::Snapshot s;
        s.state = on ? SensorPowerState::On : SensorPowerState::Off;
        ctl.setPowerSnapshot(s);
    }

    ~RollHost() {
        stop = true;
        if (feeder.joinable()) feeder.join();
        ctl.stop();
        if (eng) {
            kv_engine_stop(eng);
            kv_engine_destroy(eng);
        }
    }
};

// ---- the fake v3 device ---------------------------------------------------------------------------------

struct PlaybackAu {
    uint32_t streamId = 0, epoch = 0;
    uint64_t frameSeq = 0, mediaNs = 0;
    bool key = false;
    bool decoded = false;
};

// Everything one device saw. `order` lists the playback and answer messages as they came, for the ordering
// checks: "17:<state>:<streamId>[:r<requestId>]", "18:<streamId>:<epoch>", "19:<streamId>:<epoch>".
struct RLog {
    bool got_hello = false;
    HelloServerMessage hello;
    std::vector<PetCameraStateMessage> pets;
    std::vector<CommandResultMessage> results;
    std::vector<RollListMessage> lists;
    std::vector<RollThumbMessage> thumbs;
    std::vector<PlaybackStateMessage> pstates;
    std::vector<PlaybackConfigMessage> pconfigs;
    std::vector<PlaybackAu> paus;
    std::vector<SessionNoticeMessage> notices;
    std::vector<std::string> order;
    int live_aus = 0;
    int live_configs = 0;
    int pdecoded = 0;
    int pau_epoch_mismatch = 0; // a 0x19 whose stream or epoch isn't that of the last 0x18 (before any too)
    bool closed = false;

    const PetCameraStateMessage* lastPet() const { return pets.empty() ? nullptr : &pets.back(); }
    const CommandResultMessage* result(uint64_t id) const {
        for (const auto& r : results)
            if (r.requestId == id) return &r;
        return nullptr;
    }
    int resultsFor(uint64_t id) const {
        int n = 0;
        for (const auto& r : results) n += r.requestId == id ? 1 : 0;
        return n;
    }
    const RollListMessage* list(uint64_t id) const {
        for (const auto& l : lists)
            if (l.requestId == id) return &l;
        return nullptr;
    }
    const RollThumbMessage* thumb(uint64_t id) const {
        for (const auto& t : thumbs)
            if (t.requestId == id) return &t;
        return nullptr;
    }
    const PlaybackStateMessage* pstate(uint64_t id) const {
        for (const auto& s : pstates)
            if (s.requestId && *s.requestId == id) return &s;
        return nullptr;
    }
    // A 0x17 without a requestId for this stream in this state.
    const PlaybackStateMessage* hostState(uint32_t stream, const char* state) const {
        for (const auto& s : pstates)
            if (!s.requestId && s.streamId == stream && s.state == state) return &s;
        return nullptr;
    }
    // Index of the first entry in order starting with prefix, or -1.
    int indexOf(const std::string& prefix, int from = 0) const {
        for (int i = std::max(0, from); i < static_cast<int>(order.size()); ++i)
            if (order[i].compare(0, prefix.size(), prefix) == 0) return i;
        return -1;
    }
    int countExact(const std::string& entry) const {
        return static_cast<int>(std::count(order.begin(), order.end(), entry));
    }
    int countPrefix(const std::string& prefix, int from = 0) const {
        int n = 0;
        for (int i = std::max(0, from); i < static_cast<int>(order.size()); ++i)
            n += order[i].compare(0, prefix.size(), prefix) == 0 ? 1 : 0;
        return n;
    }
    bool answered(uint64_t id) const { return result(id) || list(id) || thumb(id) || pstate(id); }
};

std::string idOf(const FakePhone& p) { return hexLower(p.identity().certificateSHA256().data(), 8); }

// Every file and folder under dir, with each file's bytes (the rolls here hold small synthetic files only).
void snapshotTree(const std::wstring& dir, const std::wstring& rel, std::map<std::wstring, std::string>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            out[rel + n + L"\\"] = std::string();
            snapshotTree(p, rel + n + L"\\", out);
        } else {
            std::string bytes;
            HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char buf[65536];
                DWORD got = 0;
                while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) bytes.append(buf, got);
                CloseHandle(f);
            }
            out[rel + n] = bytes;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
std::map<std::wstring, std::string> snapshotTree(const std::wstring& dir) {
    std::map<std::wstring, std::string> out;
    snapshotTree(dir, L"", out);
    return out;
}

class RollPhone {
public:
    explicit RollPhone(FakePhone& p) : phone_(p) {}
    ~RollPhone() { stop(); }
    RollPhone(const RollPhone&) = delete;
    RollPhone& operator=(const RollPhone&) = delete;

    void start() { th_ = std::thread([this] { loop(); }); }
    void stop() {
        stop_ = true;
        if (th_.joinable()) th_.join();
    }
    void sendRaw(MsgType t, const std::string& json) {
        std::lock_guard<std::mutex> lk(m_);
        out_.push_back({t, json});
    }
    // A device that stops reading (it still sends, and pings): what the host sends piles up.
    void pauseReading(bool on) { paused_reading_ = on; }
    // A device that sends a ping after every frame it reads (a phone busy talking while it drains), not every
    // 250 ms: whatever the host does as it closes, the device is sending.
    void pingEveryRead(bool on) { ping_every_read_ = on; }
    RLog log() const {
        std::lock_guard<std::mutex> lk(m_);
        return log_;
    }
    bool waitFor(const std::function<bool(const RLog&)>& pred, int timeout_ms = 10000) const {
        return waitUntil([&] { return pred(log()); }, timeout_ms);
    }

    // ---- requests ----
    uint64_t cmd(const std::string& action, std::optional<bool> enabled = std::nullopt, std::optional<std::string> clip = std::nullopt,
                 std::optional<bool> starred = std::nullopt, std::optional<int> sensitivity = std::nullopt,
                 std::optional<std::string> device = std::nullopt) {
        PetCameraCommandMessage c;
        c.requestId = ++next_id_;
        c.action = action;
        c.enabled = enabled;
        c.clipId = std::move(clip);
        c.starred = starred;
        c.sensitivity = sensitivity;
        c.deviceId = std::move(device);
        sendRaw(MsgType::PetCameraCmd, toJson(c));
        return c.requestId;
    }
    uint64_t list(std::optional<std::string> day = std::nullopt, std::optional<std::string> cursor = std::nullopt,
                  std::optional<int> limit = std::nullopt) {
        RollListRequestMessage r;
        r.requestId = ++next_id_;
        r.day = std::move(day);
        r.cursor = std::move(cursor);
        r.limit = limit;
        sendRaw(MsgType::RollListReq, toJson(r));
        return r.requestId;
    }
    uint64_t thumb(const std::string& clip) {
        RollThumbRequestMessage r;
        r.requestId = ++next_id_;
        r.clipId = clip;
        sendRaw(MsgType::RollThumbReq, toJson(r));
        return r.requestId;
    }
    uint64_t play(const std::string& action, std::optional<std::string> clip = std::nullopt,
                  std::optional<uint32_t> stream = std::nullopt, std::optional<double> pos = std::nullopt) {
        PlaybackRequestMessage r;
        r.requestId = ++next_id_;
        r.action = action;
        r.clipId = std::move(clip);
        r.streamId = stream;
        r.positionSec = pos;
        sendRaw(MsgType::PlaybackReq, toJson(r));
        return r.requestId;
    }
    void watch(bool on) {
        LiveViewRequestMessage r;
        r.watch = on;
        sendRaw(MsgType::LiveViewReq, toJson(r));
    }
    uint64_t nextId() { return ++next_id_; }

    // Waits for the one answer to a request and returns its 0x0D, if it was one.
    std::optional<CommandResultMessage> resultOf(uint64_t id, int timeout_ms = 10000) const {
        if (!waitFor([&](const RLog& l) { return l.answered(id); }, timeout_ms)) return std::nullopt;
        const RLog l = log();
        if (const CommandResultMessage* r = l.result(id)) return *r;
        return std::nullopt;
    }
    bool okOf(uint64_t id) const {
        const auto r = resultOf(id);
        return r && r->ok;
    }
    std::string errorOf(uint64_t id) const {
        const auto r = resultOf(id);
        return r && r->error ? *r->error : std::string("(none)");
    }

private:
    struct Out {
        MsgType t;
        std::string json;
    };
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            std::unique_ptr<kvtest::TestH264Decoder> dec;
            PlaybackConfigMessage cfg;
            bool have_cfg = false;
            uint64_t next_ping = 0;
            const uint64_t t0 = nowNs();
            while (!stop_) {
                std::vector<Out> out;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    out.swap(out_);
                }
                for (const auto& o : out) phone_.sendFrame(o.t, o.json);
                bool hello = false;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    hello = log_.got_hello;
                }
                const uint64_t now = nowNs();
                if (hello && (now >= next_ping || ping_every_read_)) {
                    PingMessage ping;
                    ping.clientSendNs = now;
                    ping.protocolVersion = phone_.pingVersion();
                    phone_.sendFrame(MsgType::Ping, toJson(ping));
                    next_ping = now + 250000000ULL;
                }
                if (paused_reading_) {
                    sleepMs(5);
                    continue;
                }
                MsgType t;
                uint8_t flags = 0;
                std::vector<uint8_t> payload;
                if (!phone_.readFrame(t, flags, payload, 100)) {
                    if (phone_.violated() || phone_.lastReadWasClose()) {
                        std::lock_guard<std::mutex> lk(m_);
                        log_.closed = true;
                        break;
                    }
                    continue;
                }
                (void)t0;
                std::lock_guard<std::mutex> lk(m_);
                switch (t) {
                case MsgType::HelloServer: {
                    HelloServerMessage h;
                    if (fromJson(payload, h)) {
                        log_.got_hello = true;
                        log_.hello = h;
                    }
                    break;
                }
                case MsgType::StreamConfig: ++log_.live_configs; break;
                case MsgType::VideoAccessUnit: ++log_.live_aus; break;
                case MsgType::PetCameraState: {
                    PetCameraStateMessage s;
                    if (fromJson(payload, s)) log_.pets.push_back(s);
                    break;
                }
                case MsgType::CommandResult: {
                    CommandResultMessage r;
                    if (fromJson(payload, r)) {
                        log_.results.push_back(r);
                        log_.order.push_back("0d:r" + std::to_string(r.requestId));
                    }
                    break;
                }
                case MsgType::SessionNotice: {
                    SessionNoticeMessage n;
                    if (fromJson(payload, n)) log_.notices.push_back(n);
                    break;
                }
                case MsgType::RollList: {
                    RollListMessage l;
                    if (fromJson(payload, l)) log_.lists.push_back(l);
                    break;
                }
                case MsgType::RollThumb: {
                    RollThumbMessage th;
                    if (fromJson(payload, th)) log_.thumbs.push_back(th);
                    break;
                }
                case MsgType::PlaybackState: {
                    PlaybackStateMessage s;
                    if (fromJson(payload, s)) {
                        log_.pstates.push_back(s);
                        log_.order.push_back("17:" + s.state + ":" + std::to_string(s.streamId) +
                                             (s.requestId ? ":r" + std::to_string(*s.requestId) : std::string()));
                    }
                    break;
                }
                case MsgType::PlaybackConfig: {
                    PlaybackConfigMessage c;
                    if (fromJson(payload, c)) {
                        log_.pconfigs.push_back(c);
                        log_.order.push_back("18:" + std::to_string(c.streamId) + ":" + std::to_string(c.config.streamEpoch));
                        cfg = c;
                        have_cfg = true;
                        // A second decoder for playback (v3 §5.9), fresh for each config.
                        dec = std::make_unique<kvtest::TestH264Decoder>();
                        dec->open();
                    }
                    break;
                }
                case MsgType::PlaybackAccessUnit: {
                    PlaybackAccessUnitHeader h;
                    const uint8_t* avcc = nullptr;
                    size_t n = 0;
                    if (!PlaybackAccessUnitHeader::decode(payload.data(), payload.size(), h, &avcc, &n)) break;
                    PlaybackAu a;
                    a.streamId = h.streamId;
                    a.epoch = h.au.streamEpoch;
                    a.frameSeq = h.au.frameSeq;
                    a.mediaNs = h.au.hostCaptureNs;
                    a.key = h.au.isKeyframe();
                    log_.order.push_back("19:" + std::to_string(h.streamId) + ":" + std::to_string(h.au.streamEpoch));
                    if (!have_cfg || cfg.streamId != h.streamId || cfg.config.streamEpoch != h.au.streamEpoch) {
                        ++log_.pau_epoch_mismatch; // a device drops it (§5.9)
                    } else if (dec) {
                        const auto annexb = kvtest::avccToAnnexB(avcc, n, a.key ? cfg.config.parameterSets
                                                                                 : std::vector<std::vector<uint8_t>>{});
                        a.decoded = dec->decode(annexb.data(), annexb.size());
                        if (a.decoded) ++log_.pdecoded;
                    }
                    log_.paus.push_back(a);
                    break;
                }
                default: break;
                }
            }
        }
        CoUninitialize();
    }

    FakePhone& phone_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_reading_{false};
    std::atomic<bool> ping_every_read_{false};
    std::atomic<uint64_t> next_id_{1000};
    mutable std::mutex m_;
    std::vector<Out> out_;
    RLog log_;
};

bool pairDevice(HostStreamServer& srv, FakePhone& phone, PetRole role, FakePhone::PairResult& pr) {
    std::string err;
    ActivePairingOffer offer;
    if (!srv.beginPairingWindow(120, offer, &err, role)) return false;
    pr = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    return pr.ok;
}

bool connectDevice(FakePhone& phone, const FakePhone::PairResult& pr) {
    std::string err;
    return phone.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err);
}

// Pairs and connects a v3 device and starts its runner; true once its first 0x0B is in.
bool joinDevice(HostStreamServer& srv, FakePhone& phone, PetRole role, RollPhone& runner) {
    FakePhone::PairResult pr;
    if (!pairDevice(srv, phone, role, pr) || !connectDevice(phone, pr)) return false;
    runner.start();
    return runner.waitFor([](const RLog& l) { return l.got_hello && !l.pets.empty(); }, 10000);
}

// The newest 0x0B says so (or one arrives that does).
bool petSays(const RollPhone& r, const std::function<bool(const PetCameraStateMessage&)>& pred, int timeout_ms = 10000) {
    return r.waitFor([&](const RLog& l) { return l.lastPet() && pred(*l.lastPet()); }, timeout_ms);
}

// Turns the roll on from an owner device and waits until it is armed (0x0B: on, nothing blocks).
bool enableRoll(RollPhone& owner) {
    const uint64_t id = owner.cmd(pet_action::kSetRoll, true);
    if (!owner.okOf(id)) return false;
    return petSays(owner, [](const PetCameraStateMessage& p) { return p.rollEnabled && !p.rollBlockedReason; });
}

// Records one manual clip of at least `seconds` (by the 0x0B's clipElapsedSec) and returns its id from the
// listing; empty on failure. The roll must be on.
std::string recordClip(RollHost& host, RollPhone& owner, int seconds) {
    const uint64_t before = host.ctl.rollStatus().clipsFinalized;
    // The ring needs a GOP or so of `medium` before the pre-roll has anything in it; a clip starts at its first
    // IDR either way.
    if (!owner.okOf(owner.cmd(pet_action::kStartClip))) return {};
    if (!petSays(owner, [&](const PetCameraStateMessage& p) {
            return p.clipRecording && p.clipElapsedSec >= seconds;
        }, 30000)) {
        return {};
    }
    if (!owner.okOf(owner.cmd(pet_action::kStopClip))) return {};
    if (!waitUntil([&] { return host.ctl.rollStatus().clipsFinalized > before; }, 20000)) return {};
    return host.ctl.rollStatus().lastClipId;
}

std::string dayOf(const std::string& clipId) { return clipId.substr(0, 10); }

// The clips of a day, as this device lists them (one page of up to 100).
std::vector<RollClip> clipsOf(RollPhone& r, const std::string& day) {
    const uint64_t id = r.list(day, std::nullopt, 100);
    if (!r.waitFor([&](const RLog& l) { return l.answered(id); })) return {};
    const RLog l = r.log();
    const RollListMessage* m = l.list(id);
    if (!m || !m->clips) return {};
    return *m->clips;
}

const RollClip* findClip(const std::vector<RollClip>& clips, const std::string& id) {
    for (const auto& c : clips)
        if (c.clipId == id) return &c;
    return nullptr;
}

// One clip as this device lists it (from its own day), if it is listed.
std::optional<RollClip> clipOf(RollPhone& r, const std::string& id) {
    const std::vector<RollClip> clips = clipsOf(r, dayOf(id));
    const RollClip* c = findClip(clips, id);
    return c ? std::optional<RollClip>(*c) : std::nullopt;
}

bool starredFor(RollPhone& r, const std::string& id, bool* by_me = nullptr) {
    const std::optional<RollClip> c = clipOf(r, id);
    if (by_me) *by_me = c && c->starredByMe;
    return c && c->starred;
}

// Holds the recorder's thread at the top of its next turn (RollRecorder::Options::threadHook) while `hold` is
// set; `held` says it is there. Declared before the host, so it outlives the recorder's thread.
struct ThreadHold {
    std::atomic<bool> hold{false};
    std::atomic<bool> held{false};
    std::function<void()> hook() {
        return [this] {
            while (hold) {
                held = true;
                sleepMs(2);
            }
            held = false;
        };
    }
    bool engage() {
        hold = true;
        return waitUntil([&] { return held.load(); }, 10000);
    }
    void release() { hold = false; }
};

// The `medium` tier has run for about 7 s since this was called, so the pre-roll ring holds its 5 s (GOPs
// are 1 s, so the pre-roll starts at an IDR 4-5 s back).
bool ringFilled(RollHost& host) {
    auto mediumFrames = [&]() -> uint64_t {
        for (const auto& t : host.ctl.tierStatus())
            if (t.tier == liveTierName(LiveTier::Medium)) return t.framesOut;
        return 0;
    };
    const uint64_t start = mediumFrames();
    return waitUntil([&] { return mediumFrames() >= start + 200; }, 30000);
}

} // namespace

// ============================================================================================================
// No roll folder: the roll is off, and the host looks like W6 (v3 §5.1 "Before W7", §10 "Test isolation")
// ============================================================================================================

KV_TEST(roll_is_off_without_a_roll_folder_and_answers_as_w6) {
    Dirs dirs(L"off");
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, /*with_roll=*/false));
    KV_CHECK(!host.ctl.rollAvailable() && !host.ctl.rollStore());
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    const RLog l0 = ra.log();
    KV_REQUIRE(l0.hello.petCamera);
    KV_CHECK(!l0.hello.petCamera->cameraRoll);
    const PetCameraStateMessage& p = *l0.lastPet();
    KV_CHECK(!p.rollAvailable && !p.playbackAvailable && !p.rollEnabled && !p.motionEnabled && !p.clipRecording &&
             !p.rollBlockedReason && !p.rollBlockedText && p.motionSensitivity == 5);
    // Every roll request is answered unsupported, and nothing is recorded anywhere.
    for (uint64_t id : {ra.cmd(pet_action::kSetRoll, true), ra.cmd(pet_action::kStartClip), ra.cmd(pet_action::kStopClip),
                        ra.cmd(pet_action::kSetMotion, true), ra.list(), ra.thumb("2026-11-18/14-03-22_motion"),
                        ra.play(playback_action::kStart, std::string("2026-11-18/14-03-22_motion"))}) {
        KV_CHECK_MSG(ra.errorOf(id) == pet_error::kUnsupported, ra.errorOf(id));
    }
    KV_CHECK(!exists(dirs.roll));
    KV_CHECK(!srv.petSettings().rollEnabled);
}

KV_TEST(roll_folder_that_holds_other_files_is_refused_and_left_alone) {
    Dirs dirs(L"refused");
    CreateDirectoryW(dirs.roll.c_str(), nullptr);
    const std::wstring other = dirs.roll + L"\\notes.txt";
    HANDLE h = CreateFileW(other.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD w = 0;
    WriteFile(h, "keep", 4, &w, nullptr);
    CloseHandle(h);
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    // pet-camera.md §6.5: a folder holding anything else is refused; the host runs without a roll.
    KV_CHECK(!host.ctl.rollAvailable() && !host.ctl.rollError().empty());
    KV_CHECK(exists(other) && !exists(dirs.roll + L"\\.nosync"));
    // The depth/IR recordings folder itself is refused too.
    Dirs dirs2(L"refused2");
    RollHost host2;
    KV_REQUIRE(host2.start(dirs2, &err, true, [&](HostController::Options& o) { o.roll.recordingsDir = dirs2.roll; }));
    KV_CHECK(!host2.ctl.rollAvailable() && !host2.ctl.rollError().empty());
}

// ============================================================================================================
// Recording, listing, thumbnails and playback
// ============================================================================================================

KV_TEST(roll_a_manual_clip_records_lists_thumbnails_and_plays_while_live_stops) {
    Dirs dirs(L"manual");
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    KV_REQUIRE(host.ctl.rollAvailable());
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    a.setDeviceName("Owner's iPhone");
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    {
        const RLog l = ra.log();
        KV_REQUIRE(l.hello.petCamera);
        KV_CHECK(l.hello.petCamera->cameraRoll); // W7 (v3 §4.1)
        const PetCameraStateMessage& p = *l.lastPet();
        // Off by default (pet-camera.md §1): the master switch is the block.
        KV_CHECK(p.rollAvailable && !p.rollEnabled && p.rollBlockedReason == std::string(roll_blocked::kRollDisabled) &&
                 p.rollBlockedText == std::string(text::kRollDisabled) && p.playbackAvailable);
    }
    // D5: off means nothing is recorded, manual included; stop_clip with nothing recording is a no-op ok.
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kRollDisabled);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
    KV_REQUIRE(enableRoll(ra));
    KV_CHECK(srv.petSettings().rollEnabled);
    // Every RollHost is a test run, so the roll it prepared carries the test marker.
    KV_CHECK(exists(dirs.roll + L"\\" + roll::kTestRollMarker));
    // Live video flows to the owner (it wants live by default).
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.live_aus >= 20; }, 15000));

    const std::string clip = recordClip(host, ra, 3);
    KV_REQUIRE(!clip.empty());

    // Listing (v3 §5.6-§5.7): one day, one clip, newest first.
    const uint64_t days_id = ra.list();
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.list(days_id) != nullptr; }));
    {
        const RLog l = ra.log();
        const RollListMessage& m = *l.list(days_id);
        KV_REQUIRE(m.days && m.days->size() == 1);
        KV_CHECK(m.days->front().day == dayOf(clip) && m.days->front().clipCount == 1 && m.days->front().bytes > 0);
        KV_CHECK(!m.clips && !m.nextCursor && !m.day);
        KV_CHECK(m.storage.capBytes == 200000000000ULL && m.storage.floorBytes == 20000000000ULL && m.storage.keepDays == 30 &&
                 m.storage.usedBytes > 0 && m.storage.freeBytes > 0);
    }
    const std::vector<RollClip> clips = clipsOf(ra, dayOf(clip));
    KV_REQUIRE(clips.size() == 1);
    const RollClip& c = clips.front();
    std::printf("  clip %s: %.2f s, %dx%d, %llu bytes, thumbnail %d\n", c.clipId.c_str(), c.durationSec, c.width, c.height,
                static_cast<unsigned long long>(c.bytes), c.hasThumbnail ? 1 : 0);
    KV_CHECK(c.clipId == clip && c.trigger == "manual" && c.startedBy == std::string("Owner's iPhone"));
    KV_CHECK(c.width == 1024 && c.height == 848 && c.bytes > 0 && c.durationSec >= 2.5 && c.hasThumbnail);
    KV_CHECK(!c.starred && !c.starredByMe && c.startUnixMs > 0 && c.startLocal.size() == 25);
    // The thumbnail (§5.8): a JPEG of at most 64 KiB.
    const uint64_t th = ra.thumb(clip);
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(th); }));
    {
        const RLog l = ra.log();
        const RollThumbMessage* t = l.thumb(th);
        KV_REQUIRE(t);
        KV_CHECK(t->clipId == clip && t->jpeg.size() > 100 && t->jpeg.size() <= 64 * 1024 && t->jpeg[0] == 0xFF && t->jpeg[1] == 0xD8);
    }

    // Playback (§5.9): live stops, the slot is kept, 0x18 comes before the first 0x19, every frame decodes.
    const int live_before = ra.log().live_aus;
    const uint64_t start = ra.play(playback_action::kStart, clip);
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.hostState(1, "ended") != nullptr; }, 30000));
    const RLog l = ra.log();
    const PlaybackStateMessage* starting = l.pstate(start);
    KV_REQUIRE(starting);
    KV_CHECK(starting->state == "starting" && starting->streamId == 1 && starting->clipId == clip &&
             std::abs(starting->durationSec - c.durationSec) < 0.01);
    const int i_start = l.indexOf("17:starting:1:r" + std::to_string(start));
    const int i_cfg = l.indexOf("18:1:1");
    const int i_au = l.indexOf("19:1:");
    const int i_playing = l.indexOf("17:playing:1");
    const int i_ended = l.indexOf("17:ended:1");
    KV_CHECK(i_start >= 0 && i_cfg > i_start && i_au > i_cfg && i_playing > i_cfg && i_ended > i_au);
    KV_CHECK(l.countPrefix("19:1:1") == static_cast<int>(l.paus.size()) && l.pau_epoch_mismatch == 0);
    KV_REQUIRE(!l.pconfigs.empty());
    const PlaybackConfigMessage& cfg = l.pconfigs.front();
    KV_CHECK(cfg.streamId == 1 && cfg.clipId == clip && cfg.config.streamEpoch == 1 && cfg.config.width == 1024 &&
             cfg.config.height == 848 && cfg.config.nalLengthHeaderBytes == 4 && cfg.config.parameterSets.size() == 2 &&
             cfg.config.codecRaw == 0 && cfg.config.modeRaw == KV_MODE_IR);
    KV_REQUIRE(!l.paus.empty());
    KV_CHECK(l.paus.front().key && l.paus.front().frameSeq == 0 && l.paus.front().mediaNs < 1000000000ULL);
    bool increasing = true;
    int keys = 0;
    for (size_t i = 0; i < l.paus.size(); ++i) {
        keys += l.paus[i].key ? 1 : 0;
        if (i > 0 && (l.paus[i].mediaNs <= l.paus[i - 1].mediaNs || l.paus[i].frameSeq != l.paus[i - 1].frameSeq + 1)) increasing = false;
    }
    std::printf("  played %zu frames (%d keyframes), decoded %d, live AUs during playback %d\n", l.paus.size(), keys, l.pdecoded,
                l.live_aus - live_before);
    KV_CHECK(increasing && keys >= 2 && static_cast<double>(l.paus.size()) >= c.durationSec * 20.0);
    KV_CHECK(l.pdecoded >= static_cast<int>(l.paus.size()) - 4);
    // One video stream per session: watching false while it plays, and the slot shows as playback.
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.watching; }));
    const std::vector<PetWatcher> watchers = srv.watchingNow();
    KV_CHECK(watchers.size() == 1 && watchers.front().activity == watcher_activity::kPlayback);
    {
        // No live 0x03 once the playback started (a few already queued may still land): it stays flat.
        const int settled = ra.log().live_aus;
        sleepMs(700);
        KV_CHECK_MSG(ra.log().live_aus - settled == 0, std::to_string(ra.log().live_aus - settled));
        KV_CHECK(ra.log().live_aus - live_before < 30);
    }
    // After it stops, live comes back only on 0x0E watch:true (§5.9).
    const uint64_t stop = ra.play(playback_action::kStop, std::nullopt, 1u);
    KV_REQUIRE(ra.waitFor([&](const RLog& l2) { return l2.pstate(stop) != nullptr; }));
    KV_CHECK(ra.log().pstate(stop)->state == "stopped");
    KV_CHECK(srv.watchingNow().empty());
    const int after_stop = ra.log().live_aus;
    ra.watch(true);
    KV_CHECK(ra.waitFor([&](const RLog& l2) { return l2.live_aus >= after_stop + 10; }, 15000));

    // A playing clip that is deleted is stopped first (0x17 stopped, no requestId, with a message), then
    // deleted (§5.2), and it is gone from the listing and the disk.
    const uint64_t replay = ra.play(playback_action::kStart, clip);
    KV_REQUIRE(ra.waitFor([&](const RLog& l2) { return l2.hostState(2, "playing") != nullptr; }, 15000));
    (void)replay;
    const uint64_t del = ra.cmd(pet_action::kDeleteClip, std::nullopt, clip);
    KV_CHECK(ra.okOf(del));
    {
        const RLog l2 = ra.log();
        const PlaybackStateMessage* stopped = l2.hostState(2, "stopped");
        KV_REQUIRE(stopped);
        KV_CHECK(stopped->message == std::string(text::kClipDeleted));
        KV_CHECK(l2.indexOf("17:stopped:2") < l2.indexOf("0d:r" + std::to_string(del)));
    }
    KV_CHECK(clipsOf(ra, dayOf(clip)).empty());
    KV_CHECK(countFiles(dirs.roll, L".mp4") == 0 && countFiles(dirs.roll, L".jpg") == 0);
    // Deleting it again: not_found.
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kDeleteClip, std::nullopt, clip)) == pet_error::kNotFound);
}

// Every row of the v3 §5.9 state table, on one recorded clip.
KV_TEST(roll_playback_follows_every_row_of_the_state_table) {
    Dirs dirs(L"table");
    std::atomic<bool> hold_open{false}; // holds each new stream in `starting`, before it opens the clip
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) {
        o.server.roll_test_hook = [&](const char* where) {
            if (std::string(where) != "playback_open") return;
            while (hold_open) sleepMs(2);
        };
    }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false); // only playback here
    const std::string clip = recordClip(host, ra, 4);
    KV_REQUIRE(!clip.empty());
    const std::vector<RollClip> clips = clipsOf(ra, dayOf(clip));
    KV_REQUIRE(clips.size() == 1);
    const double dur = clips.front().durationSec;
    std::printf("  clip %.2f s\n", dur);
    auto stateOf = [&](uint64_t id) -> std::string {
        if (!ra.waitFor([&](const RLog& l) { return l.answered(id); })) return "(no answer)";
        const RLog l = ra.log();
        if (const PlaybackStateMessage* s = l.pstate(id)) return s->state;
        if (const CommandResultMessage* r = l.result(id)) return "0d:" + (r->error ? *r->error : std::string("ok"));
        return "(other)";
    };

    // Row "none": every action but start is not_found, whatever the stream id.
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 7u)) == "0d:not_found");
    KV_CHECK(stateOf(ra.play(playback_action::kResume, std::nullopt, 7u)) == "0d:not_found");
    KV_CHECK(stateOf(ra.play(playback_action::kSeek, std::nullopt, 7u, 1.0)) == "0d:not_found");
    KV_CHECK(stateOf(ra.play(playback_action::kStop, std::nullopt, 7u)) == "0d:not_found");
    // Start errors and §5.10: an id not in the index, missing keys, an unknown action; a malformed request
    // gets no answer at all.
    KV_CHECK(stateOf(ra.play(playback_action::kStart, std::string("2001-01-01/01-02-03_manual"))) == "0d:not_found");
    KV_CHECK(stateOf(ra.play(playback_action::kStart, std::string("../../pairing"))) == "0d:not_found");
    KV_CHECK(stateOf(ra.play(playback_action::kStart)) == "0d:invalid");
    KV_CHECK(stateOf(ra.play(playback_action::kPause)) == "0d:invalid");
    KV_CHECK(stateOf(ra.play("rewind", std::nullopt, 1u)) == "0d:unsupported");
    {
        const size_t before = ra.log().results.size() + ra.log().pstates.size();
        ra.sendRaw(MsgType::PlaybackReq, R"({"action":"start","clipId":"x"})");          // no requestId
        ra.sendRaw(MsgType::PlaybackReq, R"({"requestId":"7","action":"start"})");       // wrong type
        ra.sendRaw(MsgType::PlaybackReq, R"({"requestId":9,"action":"start",)");          // bad JSON
        const uint64_t probe = ra.play(playback_action::kPause, std::nullopt, 7u);
        KV_CHECK(stateOf(probe) == "0d:not_found");
        KV_CHECK(ra.log().results.size() + ra.log().pstates.size() == before + 1);
    }

    // Row "starting / playing".
    const uint64_t s1 = ra.play(playback_action::kStart, clip);
    KV_CHECK(stateOf(s1) == "starting");
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(1, "playing") != nullptr; }));
    KV_CHECK(stateOf(ra.play(playback_action::kResume, std::nullopt, 1u)) == "playing"); // no change
    // seek -> playing, from the keyframe at or before the position, after a fresh 0x18 with a higher epoch.
    const uint64_t seek1 = ra.play(playback_action::kSeek, std::nullopt, 1u, 2.5);
    KV_CHECK(stateOf(seek1) == "playing");
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.indexOf("19:1:2") >= 0; }));
    {
        const RLog l = ra.log();
        KV_CHECK(l.indexOf("18:1:2") >= 0 && l.indexOf("18:1:2") < l.indexOf("19:1:2"));
        KV_CHECK(l.indexOf("17:playing:1:r" + std::to_string(seek1)) < l.indexOf("18:1:2"));
        const PlaybackAu* first = nullptr;
        for (const auto& au : l.paus)
            if (au.epoch == 2 && !first) first = &au;
        KV_REQUIRE(first);
        // GOPs are 1 s: the keyframe at or before 2.5 s, so between 1.4 s and 2.5 s.
        KV_CHECK_MSG(first->key && first->frameSeq == 0 && first->mediaNs <= 2500000000ULL && first->mediaNs >= 1400000000ULL,
                     std::to_string(first->mediaNs));
        KV_CHECK(l.pau_epoch_mismatch == 0);
    }
    // pause -> paused, and nothing more is sent.
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 1u)) == "paused");

    // Row "paused".
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 1u)) == "paused"); // no change
    {
        // A frame that was in flight when it paused has landed once the device holds every frame this stream
        // sent (the device's own count against the host's), not after a guessed wait. Then nothing more comes over
        // a window.
        KV_REQUIRE(waitUntil([&] {
            const auto p = srv.playbackProbes();
            size_t mine = 0;
            for (const auto& au : ra.log().paus) mine += au.streamId == 1 ? 1 : 0;
            return !p.empty() && p.front().streamId == 1 && mine == p.front().framesSent;
        }, 10000));
        const size_t n = ra.log().paus.size();
        sleepMs(700); // the window in which nothing may arrive
        KV_CHECK_MSG(ra.log().paus.size() == n, "frames while paused");
    }
    // seek -> stays paused, with a new 0x18 and exactly that one keyframe.
    const uint64_t seek2 = ra.play(playback_action::kSeek, std::nullopt, 1u, 1.2);
    KV_CHECK(stateOf(seek2) == "paused");
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.indexOf("19:1:3") >= 0; }));
    sleepMs(800);
    {
        const RLog l = ra.log();
        KV_CHECK(l.indexOf("18:1:3") >= 0 && l.indexOf("18:1:3") < l.indexOf("19:1:3"));
        KV_CHECK_MSG(l.countPrefix("19:1:3") == 1, std::to_string(l.countPrefix("19:1:3")));
        for (const auto& au : l.paus)
            if (au.epoch == 3) KV_CHECK(au.key && au.mediaNs <= 1200000000ULL);
    }
    // resume -> playing, on from there to the end.
    KV_CHECK(stateOf(ra.play(playback_action::kResume, std::nullopt, 1u)) == "playing");
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(1, "ended") != nullptr; }, 20000));

    // Row "ended": pause no change; seek -> playing; resume -> playing from 0.
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 1u)) == "ended");
    const uint64_t seek3 = ra.play(playback_action::kSeek, std::nullopt, 1u, dur - 1.5);
    KV_CHECK(stateOf(seek3) == "playing");
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.indexOf("19:1:4") >= 0; }));
    // The host's own "ended" (no requestId), the second one: the pause answer above says ended too.
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.countExact("17:ended:1") >= 2; }, 20000));
    const uint64_t res0 = ra.play(playback_action::kResume, std::nullopt, 1u);
    KV_CHECK(stateOf(res0) == "playing");
    KV_CHECK(ra.log().pstate(res0)->positionSec == 0.0);
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.indexOf("19:1:5") >= 0; }));
    {
        const RLog l = ra.log();
        for (const auto& au : l.paus) {
            if (au.epoch == 5 && au.frameSeq == 0) KV_CHECK(au.key && au.mediaNs < 100000000ULL); // from 0
        }
    }
    // starting/playing + start: the old stream -> stopped (no requestId), the new one -> starting (with it).
    const uint64_t s2 = ra.play(playback_action::kStart, clip);
    KV_CHECK(stateOf(s2) == "starting");
    {
        const RLog l = ra.log();
        const int stopped1 = l.indexOf("17:stopped:1");
        KV_CHECK(stopped1 >= 0 && l.hostState(1, "stopped") && stopped1 < l.indexOf("17:starting:2:r" + std::to_string(s2)));
        KV_CHECK(l.indexOf("19:1:", stopped1 + 1) < 0); // nothing of stream 1 after its stop
    }
    // The old stream is freed: not_found. starting/playing + stop -> stopped, then that stream is freed too.
    KV_CHECK(stateOf(ra.play(playback_action::kSeek, std::nullopt, 1u, 1.0)) == "0d:not_found");
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(2, "playing") != nullptr; }));
    KV_CHECK(stateOf(ra.play(playback_action::kStop, std::nullopt, 2u)) == "stopped");
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 2u)) == "0d:not_found");

    // paused + start, and paused + stop.
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip)) == "starting"); // stream 3
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 3u)) == "paused");
    const uint64_t s4 = ra.play(playback_action::kStart, clip);
    KV_CHECK(stateOf(s4) == "starting");
    KV_CHECK(ra.log().hostState(3, "stopped") != nullptr);
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 4u)) == "paused");
    KV_CHECK(stateOf(ra.play(playback_action::kStop, std::nullopt, 4u)) == "stopped");

    // ended + start, and ended + stop (a start near the end).
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip, std::nullopt, dur - 0.5)) == "starting"); // stream 5
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(5, "ended") != nullptr; }, 15000));
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip, std::nullopt, dur - 0.5)) == "starting"); // stream 6
    KV_CHECK(ra.log().hostState(5, "stopped") != nullptr);
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(6, "ended") != nullptr; }, 15000));
    KV_CHECK(stateOf(ra.play(playback_action::kStop, std::nullopt, 6u)) == "stopped");
    // A position past the end is clamped (§5.9): it starts, plays the last keyframe on, and ends.
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip, std::nullopt, dur + 100.0)) == "starting"); // stream 7
    const bool ended7 = ra.waitFor([](const RLog& l) { return l.hostState(7, "ended") != nullptr; }, 15000);
    if (!ended7) {
        for (const auto& o : ra.log().order)
            if (o.find(":7") != std::string::npos) std::printf("  stream 7: %s\n", o.c_str());
    }
    KV_REQUIRE(ended7);
    {
        const RLog l = ra.log();
        KV_CHECK(l.pau_epoch_mismatch == 0);
        KV_CHECK(srv.playbackCount() == 1);
    }

    // Row "starting", every action: each stream is held before it opens the clip, so it stays starting.
    hold_open = true;
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip)) == "starting"); // stream 8 (7 -> stopped)
    KV_CHECK(ra.log().hostState(7, "stopped") != nullptr);
    KV_CHECK(stateOf(ra.play(playback_action::kPause, std::nullopt, 8u)) == "paused");
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip)) == "starting"); // stream 9 (8 -> stopped)
    KV_CHECK(ra.log().hostState(8, "stopped") != nullptr);
    KV_CHECK(stateOf(ra.play(playback_action::kSeek, std::nullopt, 9u, 1.0)) == "playing");
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip)) == "starting"); // stream 10
    KV_CHECK(stateOf(ra.play(playback_action::kStop, std::nullopt, 10u)) == "stopped");
    KV_CHECK(stateOf(ra.play(playback_action::kStart, clip)) == "starting"); // stream 11
    // resume while starting: no change, and it plays once ready.
    KV_CHECK(stateOf(ra.play(playback_action::kResume, std::nullopt, 11u)) == "starting");
    hold_open = false;
    KV_CHECK(ra.waitFor([](const RLog& l) { return l.hostState(11, "playing") != nullptr; }, 15000));
    // Nothing of the streams stopped while starting was ever sent.
    {
        const RLog l = ra.log();
        for (const char* s : {"18:8:", "19:8:", "18:9:", "19:9:", "18:10:", "19:10:"}) KV_CHECK_MSG(l.indexOf(s) < 0, s);
    }
    KV_CHECK(stateOf(ra.play(playback_action::kStop, std::nullopt, 11u)) == "stopped");
}

// ============================================================================================================
// Listing, cursors, thumbnails, stars and deletion by role
// ============================================================================================================

namespace {

int64_t unixMsNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

void writeFileBytes(const std::wstring& path, size_t n, char fill) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<char> b(n, fill);
    DWORD w = 0;
    WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &w, nullptr);
    CloseHandle(h);
}

// Clips written straight into the temp roll before the host starts (listing reads only sidecars and the index;
// nothing here plays them). Returns their ids, oldest first. The roll is marked as a test roll, as the host marks the
// one it prepares in a test run (every RollHost is one); test_marker false leaves it looking like a real roll.
std::vector<std::string> seedRoll(const Dirs& dirs, const std::vector<int64_t>& ages_ms, bool jpg_on_odd = true,
                                  int keep_days = roll::kDefaultKeepDays, bool test_marker = true) {
    std::string err;
    std::vector<std::string> ids;
    if (!roll::prepareRollFolder(dirs.roll, dirs.recordings, &err)) {
        std::printf("  prepareRollFolder: %s\n", err.c_str());
        return ids;
    }
    if (test_marker && !roll::writeTestRollMarker(dirs.roll, &err)) {
        std::printf("  writeTestRollMarker: %s\n", err.c_str());
        return ids;
    }
    roll::RollConfig cfg;
    cfg.dir = dirs.roll;
    cfg.keepDays = keep_days;
    roll::RollStore store(cfg);
    const int64_t now = unixMsNow();
    int i = 0;
    for (int64_t age : ages_ms) {
        const int64_t start = now - age;
        const auto p = store.reserveClip(start, ClipTrigger::Motion, &err);
        if (!p) {
            std::printf("  reserveClip: %s\n", err.c_str());
            return {};
        }
        writeFileBytes(p->mp4, 2048, 'M');
        const bool jpg = !jpg_on_odd || (i % 2 == 1);
        if (jpg) {
            std::vector<char> j(300, 'J');
            j[0] = static_cast<char>(0xFF);
            j[1] = static_cast<char>(0xD8);
            HANDLE h = CreateFileW(p->thumbnail.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0;
            WriteFile(h, j.data(), static_cast<DWORD>(j.size()), &w, nullptr);
            CloseHandle(h);
        }
        roll::ClipSidecar sc;
        sc.startUnixMs = static_cast<uint64_t>(start);
        sc.endUnixMs = sc.startUnixMs + 4000;
        sc.durationSec = 4.0;
        sc.trigger = ClipTrigger::Motion;
        sc.width = 1024;
        sc.height = 848;
        if (!store.finalizeClip(*p, sc, &err)) {
            std::printf("  finalizeClip: %s\n", err.c_str());
            return {};
        }
        ids.push_back(p->clipId);
        ++i;
    }
    store.retentionPass(); // the index, as the host's own passes keep it
    return ids;
}

constexpr int64_t kMinMs = 60000;
constexpr int64_t kDayMs = 24 * 60 * kMinMs;

// pet-camera.json in the test's own state folder, before the host starts.
bool writePetJson(const Dirs& dirs, const char* body) {
    CreateDirectoryW(dirs.state.c_str(), nullptr);
    const std::wstring file = dirs.state + L"\\pet-camera.json";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, body, static_cast<DWORD>(std::strlen(body)), &w, nullptr) && w == std::strlen(body);
    CloseHandle(h);
    return ok;
}

} // namespace

// The start-up pass never runs retention, and retention runs only with the user's own limits. Keep days
// 90, clips 40 days old: a start (and a restart) keeps them; the old start-up pass deleted them with the
// default 30 days before pet-camera.json was even read.
KV_TEST(roll_a_restart_keeps_clips_the_users_keep_days_keep) {
    Dirs dirs(L"keep90");
    const std::vector<std::string> ids = seedRoll(dirs, {41 * kDayMs, 40 * kDayMs, kMinMs}, true, 90);
    KV_REQUIRE(ids.size() == 3);
    // The roll on (retention runs only on a prepared folder, and the folder is prepared once the roll is on).
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true,"rollKeepDays":90})"));
    for (int run = 0; run < 2; ++run) {
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err));
        KV_REQUIRE(host.ctl.rollAvailable());
        // The user's limits are applied, and retention has run with them.
        KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().limitsApplied; }, 10000));
        KV_CHECK_MSG(countFiles(dirs.roll, L".mp4") == 3, std::to_string(countFiles(dirs.roll, L".mp4")));
        const std::shared_ptr<roll::RollStore> store = host.ctl.rollStore();
        KV_REQUIRE(store);
        KV_CHECK(store->storage().keepDays == 90);
        for (const auto& id : ids) KV_CHECK(store->findClip(id).has_value());
    }
}

// While pet-camera.json can't be read, retention doesn't run at all, and
// defaults never delete a clip; once it is read, its own limits apply.
KV_TEST(roll_nothing_is_deleted_while_pet_camera_json_is_unread) {
    Dirs dirs(L"keep-unread");
    const std::vector<std::string> ids = seedRoll(dirs, {45 * kDayMs, 40 * kDayMs, kMinMs}, true, 90);
    KV_REQUIRE(ids.size() == 3);
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true,"rollKeepDays":90})"));
    const std::wstring file = dirs.state + L"\\pet-camera.json";
    HANDLE hold = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_REQUIRE(hold != INVALID_HANDLE_VALUE);
    {
        RollHost host;
        std::string err;
        const bool started = host.start(dirs, &err);
        KV_CHECK_MSG(started, err);
        if (!started) {
            CloseHandle(hold);
            return;
        }
        KV_CHECK(countFiles(dirs.roll, L".mp4") == 3);
        // An owner lists and the host runs a while (its retention checks included): still nothing deleted.
        FakePhone a(3);
        RollPhone ra(a);
        KV_REQUIRE(joinDevice(host.ctl.server(), a, PetRole::Owner, ra));
        KV_CHECK(clipOf(ra, ids[0]).has_value() && clipOf(ra, ids[1]).has_value());
        KV_CHECK(!host.ctl.rollStatus().limitsApplied);
        KV_CHECK(countFiles(dirs.roll, L".mp4") == 3);
        // Read at last: its 90 days apply, and both old clips stay.
        CloseHandle(hold);
        hold = INVALID_HANDLE_VALUE;
        KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().limitsApplied; }, 30000));
        KV_CHECK(countFiles(dirs.roll, L".mp4") == 3);
        KV_CHECK(host.ctl.rollStore()->storage().keepDays == 90);
    }
    if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
    // No pet-camera.json at all: the defaults (30 days) are not the user's, and delete nothing either.
    Dirs fresh(L"keep-missing");
    KV_REQUIRE(seedRoll(fresh, {40 * kDayMs}, true, 90).size() == 1);
    RollHost host2;
    std::string err2;
    KV_REQUIRE(host2.start(fresh, &err2));
    KV_REQUIRE(host2.ctl.rollAvailable());
    FakePhone b(3);
    RollPhone rb(b);
    KV_REQUIRE(joinDevice(host2.ctl.server(), b, PetRole::Owner, rb));
    KV_CHECK(countFiles(fresh.roll, L".mp4") == 1 && !host2.ctl.rollStatus().limitsApplied);
}

namespace {

// A clip's .mp4 made sparse and this long (no data is written: a synthetic size, so a 1 GB cap can be passed
// without filling a disk). The listing still reads only its sidecar; nothing here plays it.
bool makeSparse(const std::wstring& path, uint64_t bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr) != 0;
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(bytes);
    ok = ok && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

std::wstring clipPath(const Dirs& dirs, const std::string& clipId, const wchar_t* ext) {
    std::wstring w(clipId.begin(), clipId.end());
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return dirs.roll + L"\\" + w + ext;
}

// Blocks a roll_test_hook point until released; `at` says a request is held there.
struct HookGate {
    std::string where;
    std::atomic<bool> armed{false};
    std::atomic<bool> at{false};
    std::atomic<bool> open{false};
    void hit(const char* w) {
        if (!armed || where != w) return;
        armed = false; // once
        at = true;
        while (!open) sleepMs(2);
        at = false;
    }
};

} // namespace

// A device revoked while the host had no roll keeps no star: the next start with a roll drops the
// stars of every device devices.json records as revoked (v3 §3.2 step 5). A listed device's star and the PC's stay.
KV_TEST(roll_stars_of_a_device_revoked_without_a_roll_are_dropped_when_a_roll_starts) {
    Dirs dirs(L"stars-noroll");
    const std::vector<std::string> ids = seedRoll(dirs, {3 * kMinMs, 2 * kMinMs}, false);
    KV_REQUIRE(ids.size() == 2);
    FakePhone a(3), v(3);
    {
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err, /*with_roll=*/false));
        HostStreamServer& srv = host.ctl.server();
        FakePhone::PairResult pa, pv;
        KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pa));
        KV_REQUIRE(pairDevice(srv, v, PetRole::Viewer, pv));
        // Their stars, as an earlier run with a roll left them.
        roll::RollConfig cfg;
        cfg.dir = dirs.roll;
        roll::RollStore store(cfg);
        KV_REQUIRE(store.setStar(ids[0], idOf(v), PetRole::Viewer, true) == roll::RollError::None);
        KV_REQUIRE(store.setStar(ids[1], idOf(v), PetRole::Viewer, true) == roll::RollError::None);
        KV_REQUIRE(store.setStar(ids[1], idOf(a), PetRole::Owner, true) == roll::RollError::None);
        KV_REQUIRE(store.setStar(ids[0], roll::kPcDeviceId, PetRole::Owner, true) == roll::RollError::None);
        // Revoked while there is no roll: its stars can't be dropped now.
        std::string rerr;
        KV_REQUIRE(srv.revokeDevice(idOf(v), &rerr));
    }
    // The roll on, so its folder is prepared, which is when the stars are reconciled.
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true})"));
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    KV_REQUIRE(host.ctl.rollAvailable());
    const std::shared_ptr<roll::RollStore> store = host.ctl.rollStore();
    auto starredBy = [&](const std::string& id, const std::string& dev) {
        const std::optional<roll::ClipSidecar> s = store->findClip(id);
        if (!s) return false;
        for (const auto& e : s->starredBy)
            if (e.deviceId == dev) return true;
        return false;
    };
    KV_CHECK(waitUntil([&] { return !starredBy(ids[0], idOf(v)) && !starredBy(ids[1], idOf(v)); }, 10000));
    KV_CHECK(starredBy(ids[1], idOf(a)) && starredBy(ids[0], roll::kPcDeviceId));
}

// A devices.json that doesn't parse is rebuilt from pairing.json, which lists only the paired
// owner again. A rebuilt list is no record of who was revoked, so the stars of the phones it no longer lists stay,
// and retention (30 days here, the clips are 40 and 41 days old) deletes neither clip. They stay through a second
// restart too, when the rebuilt list reads as consistent. A revoke still drops that device's stars (v3 §3.2 step
// 5). The old code dropped every star of a device the rebuilt list didn't hold.
KV_TEST(roll_a_rebuilt_device_list_keeps_every_star_and_retention_deletes_nothing) {
    Dirs dirs(L"stars-rebuilt");
    const std::vector<std::string> ids = seedRoll(dirs, {41 * kDayMs, 40 * kDayMs}, false, 90);
    KV_REQUIRE(ids.size() == 2);
    FakePhone a(3), v(3);
    {
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err, /*with_roll=*/false));
        HostStreamServer& srv = host.ctl.server();
        FakePhone::PairResult pa, pv;
        KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pa));
        KV_REQUIRE(pairDevice(srv, v, PetRole::Viewer, pv));
    }
    {
        roll::RollConfig cfg;
        cfg.dir = dirs.roll;
        cfg.keepDays = 90;
        roll::RollStore store(cfg);
        KV_REQUIRE(store.setStar(ids[0], idOf(v), PetRole::Viewer, true) == roll::RollError::None);
        KV_REQUIRE(store.setStar(ids[1], idOf(a), PetRole::Owner, true) == roll::RollError::None);
    }
    {
        // Corrupt: it doesn't parse, so it is kept as a .bad copy and rebuilt from pairing.json.
        const std::wstring file = dirs.state + L"\\devices.json";
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        DWORD w = 0;
        WriteFile(h, "{\"devices\":[", 12, &w, nullptr);
        CloseHandle(h);
    }
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true,"rollKeepDays":30})"));
    std::atomic<int> reconciled{0};
    auto withHook = [&](HostController::Options& o) {
        o.server.roll_test_hook = [&](const char* w) {
            if (std::string(w) == "stars_reconciled") ++reconciled;
        };
    };
    for (int run = 0; run < 2; ++run) {
        reconciled = 0;
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err, true, withHook));
        HostStreamServer& srv = host.ctl.server();
        if (run == 0) {
            const std::vector<HostDeviceInfo> listed = srv.devices();
            KV_CHECK_MSG(listed.size() == 1, std::to_string(listed.size())); // only the owner pairing.json pins
        }
        // The stars have been reconciled with the device list, and retention has run with 30 days.
        KV_REQUIRE(waitUntil([&] { return reconciled.load() >= 1 && host.ctl.rollStatus().limitsApplied; }, 20000));
        const std::shared_ptr<roll::RollStore> store = host.ctl.rollStore();
        KV_REQUIRE(store);
        auto starredBy = [&](const std::string& id, const std::string& dev) {
            const std::optional<roll::ClipSidecar> s = store->findClip(id);
            if (!s) return false;
            for (const auto& e : s->starredBy)
                if (e.deviceId == dev) return true;
            return false;
        };
        KV_CHECK_MSG(starredBy(ids[0], idOf(v)), "run " + std::to_string(run));
        KV_CHECK_MSG(starredBy(ids[1], idOf(a)), "run " + std::to_string(run));
        KV_CHECK_MSG(countFiles(dirs.roll, L".mp4") == 2, std::to_string(countFiles(dirs.roll, L".mp4")));
        if (run == 1) {
            // Revoking the owner the rebuilt list holds drops its star, and only that one.
            std::string rerr;
            KV_REQUIRE(srv.revokeDevice(idOf(a), &rerr));
            KV_CHECK(waitUntil([&] { return !starredBy(ids[1], idOf(a)); }, 10000));
            KV_CHECK(starredBy(ids[0], idOf(v)));
        }
    }
}

// A viewer's star_clip whose task ran after its device was revoked used to land, undoing the revoke's
// star removal. Held at the "star" point while the PC revokes the device, it is now refused.
KV_TEST(roll_a_star_racing_a_revoke_never_survives_it) {
    Dirs dirs(L"stars-race");
    const std::vector<std::string> ids = seedRoll(dirs, {2 * kMinMs}, false);
    KV_REQUIRE(ids.size() == 1);
    HookGate gate;
    gate.where = "star";
    std::atomic<int> star_done{0};
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) {
        o.server.roll_test_hook = [&](const char* w) {
            gate.hit(w);
            if (std::string(w) == "star_done") ++star_done;
        };
    }));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone a(3), v(3);
    RollPhone ra(a), rv(v);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(joinDevice(srv, v, PetRole::Viewer, rv));
    gate.armed = true;
    rv.cmd(pet_action::kStarClip, std::nullopt, ids[0], true);
    KV_REQUIRE(waitUntil([&] { return gate.at.load(); }, 10000));
    std::string rerr;
    KV_REQUIRE(srv.revokeDevice(idOf(v), &rerr)); // its stars are dropped now (there are none yet)
    gate.open = true;
    KV_CHECK(waitUntil([&] { return !gate.at.load(); }, 10000));
    // The held star either lands before the drop (and is dropped) or is refused: never left behind.
    const std::shared_ptr<roll::RollStore> store = host.ctl.rollStore();
    KV_CHECK(waitUntil([&] { return rv.log().closed; }, 10000));
    // The held task has decided (refused, or written and then dropped): its "star_done" point has been passed,
    // and the maintenance thread has dropped what a revoke leaves.
    KV_REQUIRE(waitUntil([&] { return star_done.load() >= 1; }, 10000));
    KV_CHECK(waitUntil([&] {
        const std::optional<roll::ClipSidecar> s = store->findClip(ids[0]);
        return s && s->starredBy.empty();
    }, 10000));
    const std::optional<roll::ClipSidecar> s = store->findClip(ids[0]);
    KV_REQUIRE(s.has_value());
    KV_CHECK_MSG(s->starredBy.empty(), std::to_string(s->starredBy.size()));
}

// storage_full follows a star, an unstar and a delete at once (it used to wait for the next clip or the
// hourly retention). A starred clip made 1.5 GB (sparse) and a 1 GB cap reach it for real.
KV_TEST(roll_storage_full_follows_stars_and_deletes_at_once) {
    Dirs dirs(L"storage-full");
    const std::vector<std::string> ids = seedRoll(dirs, {2 * kMinMs}, false);
    KV_REQUIRE(ids.size() == 1);
    KV_REQUIRE(makeSparse(clipPath(dirs, ids[0], L".mp4"), 1500000000ULL));
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true,)"
                                  R"("rollKeepDays":30,"rollCapBytes":1000000000})"));
    {
        roll::RollConfig cfg;
        cfg.dir = dirs.roll;
        roll::RollStore store(cfg);
        KV_REQUIRE(store.setStar(ids[0], roll::kPcDeviceId, PetRole::Owner, true) == roll::RollError::None);
    }
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    ra.watch(false);
    auto blockedBy = [&](const char* code) {
        return petSays(ra, [&](const PetCameraStateMessage& p) {
            return code ? p.rollBlockedReason == std::string(code) : !p.rollBlockedReason;
        }, 10000);
    };
    // The starred clip alone is over the 1 GB cap (§6.4 step 3).
    KV_REQUIRE(blockedBy(roll_blocked::kStorageFull));
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kStorageFull);
    // The owner's unstar clears it at once: 0x0B drops storage_full and start_clip works.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStarClip, std::nullopt, ids[0], false)));
    KV_CHECK(blockedBy(nullptr));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    // Starred again (before any retention could delete it, now unstarred over the cap): storage_full again at
    // once, which ends that clip as any block does.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStarClip, std::nullopt, ids[0], true)));
    KV_CHECK(blockedBy(roll_blocked::kStorageFull));
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kStorageFull);
    // Deleted: cleared at once.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kDeleteClip, std::nullopt, ids[0])));
    KV_CHECK(blockedBy(nullptr));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
}

// A delete and a playback start of the same clip, in both orders, held at the race points. Whichever
// comes first, no stream is ever left playing a deleted clip, and a stream that was playing gets 0x17 stopped
// with "This clip was deleted.".
KV_TEST(roll_a_delete_and_a_playback_start_race_in_both_orders) {
    Dirs dirs(L"delete-race");
    HookGate gate;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) {
        o.server.roll_test_hook = [&](const char* w) { gate.hit(w); };
    }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3), b(3);
    RollPhone ra(a), rb(b);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(joinDevice(srv, b, PetRole::Owner, rb));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    rb.watch(false);
    const std::string c1 = recordClip(host, ra, 3);
    const std::string c2 = recordClip(host, ra, 3);
    KV_REQUIRE(!c1.empty() && !c2.empty());

    // 1. The delete first: b plays c1; a deletes it and is held before the files go. b's stream got its
    //    0x17 stopped "This clip was deleted." already; a start of c1 now (the clip still on disk) is not_found.
    rb.play(playback_action::kStart, c1);
    KV_REQUIRE(rb.waitFor([](const RLog& l) { return l.hostState(1, "playing") != nullptr; }, 15000));
    gate.where = "delete_before_remove";
    gate.armed = true;
    const uint64_t del1 = ra.cmd(pet_action::kDeleteClip, std::nullopt, c1);
    KV_REQUIRE(waitUntil([&] { return gate.at.load(); }, 10000));
    KV_REQUIRE(rb.waitFor([](const RLog& l) { return l.hostState(1, "stopped") != nullptr; }));
    KV_CHECK(rb.log().hostState(1, "stopped")->message == std::string(text::kClipDeleted));
    KV_CHECK(exists(clipPath(dirs, c1, L".mp4")));
    const uint64_t late = rb.play(playback_action::kStart, c1);
    KV_CHECK(rb.errorOf(late) == pet_error::kNotFound);
    KV_CHECK(srv.playbackCount() == 0);
    gate.open = true;
    KV_CHECK(ra.okOf(del1));
    KV_CHECK(!exists(clipPath(dirs, c1, L".mp4")));
    gate.open = false;

    // 2. The start first: b's start of c2 is held after the clip was looked up; a deletes c2 meanwhile, all
    //    the way. Released, the start is not_found: no stream on a deleted clip.
    gate.where = "play_after_find";
    gate.armed = true;
    const uint64_t s2 = rb.play(playback_action::kStart, c2);
    KV_REQUIRE(waitUntil([&] { return gate.at.load(); }, 10000));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kDeleteClip, std::nullopt, c2)));
    KV_CHECK(!exists(clipPath(dirs, c2, L".mp4")));
    gate.open = true;
    KV_CHECK(rb.errorOf(s2) == pet_error::kNotFound);
    KV_CHECK(srv.playbackCount() == 0);
    const RLog l = rb.log();
    for (const auto& s : l.pstates) {
        KV_CHECK_MSG(s.clipId != c2, s.state); // nothing was ever started on c2
    }
}

// Retention skips a clip that is playing; it goes at a later pass, once nobody plays it.
// With the roll off the host doesn't create or change the roll folder. It is
// prepared (created, protected, marked) the first time the roll is on with pet-camera.json read and privacy mode
// off. Until then the roll is on the wire as an empty one.
KV_TEST(roll_the_roll_folder_is_left_alone_until_the_roll_is_turned_on) {
    {
        Dirs dirs(L"lazy-folder");
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err)); // no pet-camera.json: the roll is off
        KV_REQUIRE(host.ctl.rollAvailable());
        HostStreamServer& srv = host.ctl.server();
        FakePhone a(3);
        RollPhone ra(a);
        KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
        {
            const RLog l = ra.log();
            KV_REQUIRE(l.hello.petCamera);
            KV_CHECK(l.hello.petCamera->cameraRoll);
            KV_CHECK(l.lastPet()->rollAvailable && !l.lastPet()->rollEnabled &&
                     l.lastPet()->rollBlockedReason == std::string(roll_blocked::kRollDisabled));
        }
        const uint64_t days = ra.list();
        KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(days); }));
        KV_CHECK(ra.log().list(days) && ra.log().list(days)->days && ra.log().list(days)->days->empty());
        KV_CHECK(!exists(dirs.roll));
        // On, but behind privacy mode: still left alone.
        KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetPrivacy, true)));
        KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetRoll, true)));
        KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) {
            return p.rollEnabled && p.rollBlockedReason == std::string(roll_blocked::kPrivacyMode);
        }));
        sleepMs(500); // several recorder turns
        KV_CHECK(!exists(dirs.roll));
        // Privacy off: now it is prepared, and recording works.
        KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetPrivacy, false)));
        KV_CHECK(waitUntil([&] { return exists(dirs.roll + L"\\.nosync") && exists(dirs.roll + L"\\.metadata_never_index"); }, 10000));
        KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return p.rollEnabled && !p.rollBlockedReason; }));
        ra.watch(false);
        const std::string clip = recordClip(host, ra, 2);
        KV_CHECK(!clip.empty() && clipOf(ra, clip).has_value());
    }
    {
        // On in pet-camera.json, but the file is held unreadable at start: left alone until it is read.
        Dirs dirs(L"lazy-unread");
        KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true})"));
        const std::wstring file = dirs.state + L"\\pet-camera.json";
        HANDLE hold = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(hold != INVALID_HANDLE_VALUE);
        RollHost host;
        std::string err;
        const bool started = host.start(dirs, &err);
        KV_CHECK_MSG(started, err);
        sleepMs(500);
        KV_CHECK(!exists(dirs.roll));
        CloseHandle(hold);
        if (!started) return;
        KV_CHECK(waitUntil([&] { return exists(dirs.roll + L"\\.nosync"); }, 20000));
    }
}

KV_TEST(roll_retention_skips_a_clip_that_is_playing) {
    Dirs dirs(L"retention-playing");
    // A starred 1.5 GB (sparse) clip keeps the roll over a 1 GB cap, so retention wants every unstarred clip.
    const std::vector<std::string> ids = seedRoll(dirs, {40 * kDayMs}, false, 90);
    KV_REQUIRE(ids.size() == 1);
    KV_REQUIRE(makeSparse(clipPath(dirs, ids[0], L".mp4"), 1500000000ULL));
    {
        roll::RollConfig cfg;
        cfg.dir = dirs.roll;
        roll::RollStore store(cfg);
        KV_REQUIRE(store.setStar(ids[0], roll::kPcDeviceId, PetRole::Owner, true) == roll::RollError::None);
    }
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true,"rollKeepDays":90})"));
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(petSays(ra, [](const PetCameraStateMessage& p) { return p.rollEnabled && !p.rollBlockedReason; }));
    ra.watch(false);
    const std::string clip = recordClip(host, ra, 3);
    KV_REQUIRE(!clip.empty());
    // It plays, and is paused there (still allocated, still playing as far as retention is concerned).
    ra.play(playback_action::kStart, clip);
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(1, "playing") != nullptr; }, 15000));
    KV_CHECK(ra.log().pstates.back().state != "error");
    const uint64_t p = ra.play(playback_action::kPause, std::nullopt, 1u);
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.pstate(p) != nullptr; }));
    // A 1 GB cap: retention runs at once (storage_full says it did) and skips the playing clip.
    KV_REQUIRE(srv.setRollRetention(90, 1000000000ULL));
    KV_REQUIRE(petSays(ra, [](const PetCameraStateMessage& p2) { return p2.rollBlockedReason == std::string(roll_blocked::kStorageFull); }));
    KV_CHECK(host.ctl.rollStore()->findClip(clip).has_value() && exists(clipPath(dirs, clip, L".mp4")));
    // Stopped: the next pass (the limits applied again) deletes it.
    const uint64_t st = ra.play(playback_action::kStop, std::nullopt, 1u);
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.pstate(st) != nullptr; }));
    KV_REQUIRE(waitUntil([&] { return srv.playbackCount() == 0; }, 5000));
    int days = 89;
    KV_REQUIRE(waitUntil([&] {
        srv.setRollRetention(days, 1000000000ULL); // each change of the limits runs retention again
        days = days == 89 ? 88 : 89;
        sleepMs(200);
        return !host.ctl.rollStore()->findClip(clip).has_value();
    }, 15000));
    KV_CHECK(!exists(clipPath(dirs, clip, L".mp4")) && host.ctl.rollStore()->findClip(ids[0]).has_value());
}

// A paused or ended playback keeps its slot only for 60 s without a command
// (2 s here, through the seam); then the host stops it with 0x17 stopped and its sentence, and a fifth device
// gets the slot.
KV_TEST(roll_a_paused_or_ended_playback_frees_its_slot_after_a_minute_without_a_command) {
    Dirs dirs(L"idle");
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.server.playback_idle_ms = 2000; }));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    std::string clip;
    {
        FakePhone o(3);
        RollPhone ro(o);
        KV_REQUIRE(joinDevice(srv, o, PetRole::Owner, ro));
        KV_REQUIRE(enableRoll(ro));
        clip = recordClip(host, ro, 3);
        ro.stop();
        o.close();
    }
    KV_REQUIRE(!clip.empty());
    KV_REQUIRE(waitUntil([&] { return srv.sessionCount() == 0; }, 10000));
    FakePhone v1(3), v2(3), v3(3), v4(3), v5(3);
    RollPhone r1(v1), r2(v2), r3(v3), r4(v4), r5(v5);
    KV_REQUIRE(joinDevice(srv, v1, PetRole::Viewer, r1));
    KV_REQUIRE(joinDevice(srv, v2, PetRole::Viewer, r2));
    KV_REQUIRE(joinDevice(srv, v3, PetRole::Viewer, r3));
    KV_REQUIRE(joinDevice(srv, v4, PetRole::Viewer, r4));
    // v4 plays and pauses: still holding its slot.
    r4.play(playback_action::kStart, clip);
    KV_REQUIRE(r4.waitFor([](const RLog& l) { return l.hostState(1, "playing") != nullptr; }));
    const uint64_t pause = r4.play(playback_action::kPause, std::nullopt, 1u);
    KV_REQUIRE(r4.waitFor([&](const RLog& l) { return l.pstate(pause) != nullptr; }));
    KV_REQUIRE(waitUntil([&] { return srv.watchingNow().size() == 4; }, 10000));
    KV_REQUIRE(joinDevice(srv, v5, PetRole::Viewer, r5));
    KV_CHECK(petSays(r5, [](const PetCameraStateMessage& p) { return p.rollAvailable && !p.playbackAvailable; }));
    // 2 s without a command: stopped, with the sentence, and the slot is free.
    KV_REQUIRE(r4.waitFor([](const RLog& l) { return l.hostState(1, "stopped") != nullptr; }, 10000));
    KV_CHECK(r4.log().hostState(1, "stopped")->message == std::string(text::kPlaybackIdle));
    KV_CHECK(petSays(r5, [](const PetCameraStateMessage& p) { return p.playbackAvailable; }));
    // The fifth device plays it to the end; ended, it too is stopped after 2 s without a command.
    KV_CHECK(r5.play(playback_action::kStart, clip) > 0);
    KV_REQUIRE(r5.waitFor([](const RLog& l) { return l.hostState(1, "ended") != nullptr; }, 20000));
    KV_REQUIRE(r5.waitFor([](const RLog& l) { return l.hostState(1, "stopped") != nullptr; }, 10000));
    KV_CHECK(r5.log().hostState(1, "stopped")->message == std::string(text::kPlaybackIdle));
    KV_CHECK(waitUntil([&] { return srv.playbackCount() == 0; }, 5000));
    // A stopped stream is freed: an action on it is not_found.
    KV_CHECK(r5.errorOf(r5.play(playback_action::kResume, std::nullopt, 1u)) == pet_error::kNotFound);
}

// A roll folder that is set but can't be used (a folder §6.5 refuses, or a
// drive that isn't there). The host serves without a roll (cameraRoll false), the PC says why, and 0x0B
// carries folder_unavailable, which start_clip is refused with (privacy mode and the switch come first).
KV_TEST(roll_a_roll_folder_that_cant_be_used_is_reported_and_start_clip_is_refused_with_it) {
    for (int k = 0; k < 2; ++k) {
        Dirs dirs(k == 0 ? L"folder-refused" : L"folder-missing");
        KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true})"));
        std::wstring folder;
        if (k == 0) {
            CreateDirectoryW(dirs.roll.c_str(), nullptr);
            HANDLE h = CreateFileW((dirs.roll + L"\\notes.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            KV_REQUIRE(h != INVALID_HANDLE_VALUE);
            CloseHandle(h);
            folder = dirs.roll;
        } else {
            const DWORD drives = GetLogicalDrives();
            for (wchar_t c = L'Z'; c >= L'G' && folder.empty(); --c) {
                if (!(drives & (1u << (c - L'A')))) folder = std::wstring(1, c) + L":\\kv-roll-e2e-missing";
            }
            if (folder.empty()) {
                std::printf("  every drive letter is taken: the missing-drive case is skipped\n");
                continue;
            }
        }
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.roll.dir = folder; }));
        KV_CHECK(!host.ctl.rollAvailable() && !host.ctl.rollError().empty());
        const RollRecorderStatus st = host.ctl.rollStatus();
        std::printf("  %s: %s\n", k == 0 ? "refused" : "missing drive", st.lastError.c_str());
        KV_CHECK(st.folderUnavailable && st.lastError == host.ctl.rollError());
        HostStreamServer& srv = host.ctl.server();
        FakePhone a(3);
        RollPhone ra(a);
        KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
        {
            const RLog l = ra.log();
            KV_REQUIRE(l.hello.petCamera);
            KV_CHECK(!l.hello.petCamera->cameraRoll);
        }
        KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) {
            return p.rollEnabled && !p.rollAvailable && !p.playbackAvailable &&
                   p.rollBlockedReason == std::string(roll_blocked::kFolderUnavailable) &&
                   p.rollBlockedText == std::string(text::kFolderUnavailable);
        }));
        KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kFolderUnavailable);
        KV_CHECK(ra.errorOf(ra.list()) == pet_error::kUnsupported); // still no roll to list
        // Privacy mode comes first in the §5.1 order.
        KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetPrivacy, true)));
        KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) {
            return p.rollBlockedReason == std::string(roll_blocked::kPrivacyMode);
        }));
        KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kPrivacyMode);
        KV_CHECK(!exists(folder + L"\\.nosync"));
    }
}

// Test gap: §5.9 "Pacing". A device that stops reading: the host stops reading the clip once about the backlog
// is queued for it (128 KiB here, through the seam; 2 MiB by default), and goes on, skipping nothing, when the
// device reads again.
KV_TEST(roll_playback_waits_at_the_backlog_and_goes_on_when_the_device_reads) {
    KV_CHECK(kPlaybackBacklogBytes == 2u * 1024u * 1024u);
    Dirs dirs(L"backlog");
    constexpr size_t kBacklog = 128 * 1024;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.server.playback_backlog_bytes = kBacklog; }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    a.setSmallReceiveBuffer(true); // a fixed small TCP window, so the host's own queue fills
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    const std::string clip = recordClip(host, ra, 8);
    KV_REQUIRE(!clip.empty());
    const std::optional<RollClip> c = clipOf(ra, clip);
    KV_REQUIRE(c.has_value());
    std::printf("  clip %.2f s, %llu bytes\n", c->durationSec, static_cast<unsigned long long>(c->bytes));
    KV_REQUIRE(c->bytes >= 4 * kBacklog);
    ra.pauseReading(true);
    ra.play(playback_action::kStart, clip);
    auto probe = [&]() -> std::optional<HostStreamServer::PlaybackProbe> {
        const auto p = srv.playbackProbes();
        if (p.empty()) return std::nullopt;
        return p.front();
    };
    KV_REQUIRE(waitUntil([&] { const auto p = probe(); return p && p->queuedBytes > kBacklog; }, 20000));
    // It has stopped reading: no frame goes out for a second, with the queue at about the backlog (one frame over
    // at most).
    const uint64_t sent = probe()->framesSent;
    sleepMs(1000);
    const auto held = probe();
    KV_REQUIRE(held.has_value());
    std::printf("  waiting at %zu bytes queued after %llu frames\n", held->queuedBytes,
                static_cast<unsigned long long>(held->framesSent));
    KV_CHECK(held->framesSent == sent && held->queuedBytes > kBacklog && held->queuedBytes < 2 * kBacklog + 512 * 1024);
    KV_CHECK(static_cast<double>(held->framesSent) < c->durationSec * 25.0); // not at the end
    // Reading again: it goes on to the end, every frame in order.
    ra.pauseReading(false);
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(1, "ended") != nullptr; }, 30000));
    const RLog l = ra.log();
    bool in_order = !l.paus.empty() && l.paus.front().frameSeq == 0;
    for (size_t i = 1; i < l.paus.size(); ++i) in_order = in_order && l.paus[i].frameSeq == l.paus[i - 1].frameSeq + 1;
    KV_CHECK(in_order && l.pau_epoch_mismatch == 0);
    KV_CHECK(static_cast<double>(l.paus.size()) >= c->durationSec * 20.0);
    KV_CHECK(l.pdecoded >= static_cast<int>(l.paus.size()) - 4);
}

// A writer that fails mid-clip, after fragments reached the disk, keeps what reads back: the clip is finalized
// as recovered, with a sidecar, and lastError says it was cut short. One that fails before its first fragment
// leaves nothing readable, and its reservation goes, as before. The old code abandoned both.
// The first failure comes after 90 samples and once 3 whole fragments (about a second) are on the disk: the
// sink's writes land in their own time, and on a busy PC none had landed after 90 samples, so the seam's
// failure came "before the first fragment" and the clip was rightly abandoned (the flake this fixes).
KV_TEST(roll_a_writer_that_fails_after_fragments_keeps_what_was_written) {
    {
        Dirs dirs(L"m4-recover");
        KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true})"));
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err, true, [](HostController::Options& o) {
            o.roll.writerFailWriteAfter = 90;
            o.roll.writerFailAfterFragments = 3;
        }));
        KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().armed; }, 20000));
        KV_REQUIRE(ringFilled(host));
        KV_REQUIRE(host.ctl.startManualClip().ok);
        KV_REQUIRE(waitUntil([&] {
            const RollRecorderStatus s = host.ctl.rollStatus();
            return s.clipsFinalized + s.clipsFailed >= 1 && !s.recording;
        }, 30000));
        const RollRecorderStatus st = host.ctl.rollStatus();
        std::printf("  finalized %llu, failed %llu: %s\n", static_cast<unsigned long long>(st.clipsFinalized),
                    static_cast<unsigned long long>(st.clipsFailed), st.lastError.c_str());
        KV_CHECK(st.clipsFinalized == 1 && st.clipsFailed == 0);
        KV_CHECK_MSG(st.lastError.find("cut short") != std::string::npos, st.lastError);
        const std::optional<roll::ClipSidecar> sc = host.ctl.rollStore()->findClip(st.lastClipId);
        KV_REQUIRE(sc.has_value());
        KV_CHECK(sc->durationSec > 0.5 && sc->durationSec < 4.0);
        KV_CHECK(exists(clipPath(dirs, st.lastClipId, L".mp4")));
    }
    {
        Dirs dirs(L"m4-early");
        KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true})"));
        RollHost host;
        std::string err;
        KV_REQUIRE(host.start(dirs, &err, true, [](HostController::Options& o) { o.roll.writerFailWriteAfter = 1; }));
        KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().armed; }, 20000));
        KV_REQUIRE(host.ctl.startManualClip().ok);
        KV_REQUIRE(waitUntil([&] {
            const RollRecorderStatus s = host.ctl.rollStatus();
            return s.clipsFinalized + s.clipsFailed >= 1 && !s.recording;
        }, 30000));
        const RollRecorderStatus st = host.ctl.rollStatus();
        KV_CHECK(st.clipsFinalized == 0 && st.clipsFailed == 1);
        KV_CHECK(waitUntil([&] { return countFiles(dirs.roll, L".mp4") == 0; }, 10000));
    }
}

// A host-initiated stop drops what is still queued for the session. A device that stops reading builds a
// backlog of 0x19 frames and two 0x15 thumbnails behind them; privacy mode goes on; once it reads again, none of
// the queued frames or thumbnails reach it (each thumbnail request is answered privacy_mode instead), only what was
// already on its way, then the 0x17. The old code delivered the whole backlog first.
KV_TEST(roll_privacy_drops_queued_playback_frames_and_thumbnails_before_the_stop) {
    Dirs dirs(L"p1-drop");
    constexpr size_t kBacklog = 128 * 1024;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.server.playback_backlog_bytes = kBacklog; }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    a.setSmallReceiveBuffer(true);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    const std::string clip = recordClip(host, ra, 8);
    KV_REQUIRE(!clip.empty());
    const std::optional<RollClip> c = clipOf(ra, clip);
    KV_REQUIRE(c.has_value() && c->bytes >= 4 * kBacklog);
    ra.pauseReading(true);
    ra.play(playback_action::kStart, clip);
    auto probe = [&]() -> std::optional<HostStreamServer::PlaybackProbe> {
        const auto p = srv.playbackProbes();
        if (p.empty()) return std::nullopt;
        return p.front();
    };
    KV_REQUIRE(waitUntil([&] { const auto p = probe(); return p && p->queuedBytes > kBacklog; }, 20000));
    // Two thumbnails queue up behind the frames (the playback waits at the backlog, so only they add to it).
    const size_t q0 = probe()->queuedBytes;
    const uint64_t t1 = ra.thumb(clip);
    const uint64_t t2 = ra.thumb(clip);
    KV_REQUIRE(waitUntil([&] { const auto p = probe(); return p && p->queuedBytes > q0 + 2000; }, 10000));
    const HostStreamServer::PlaybackProbe at = *probe();
    srv.setPrivacyMode(true);
    ra.pauseReading(false);
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(1, "stopped") != nullptr; }, 20000));
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(t1) && l.answered(t2); }, 10000));
    const RLog l = ra.log();
    KV_CHECK(l.thumb(t1) == nullptr && l.thumb(t2) == nullptr);
    KV_CHECK(ra.errorOf(t1) == pet_error::kPrivacyMode && ra.errorOf(t2) == pet_error::kPrivacyMode);
    KV_CHECK(l.hostState(1, "stopped")->message == std::string(text::kPrivacyMode));
    // Nothing of the clip after the stop, and most of what was queued never came: at least half the frames the
    // queued bytes held (by the clip's average frame size) are missing from what the device got.
    const int stop_at = l.indexOf("17:stopped:1");
    KV_CHECK(stop_at >= 0 && l.countPrefix("19:", stop_at) == 0);
    const double frames_in_clip = std::max(1.0, c->durationSec * 25.0);
    const double avg_frame = static_cast<double>(c->bytes) / frames_in_clip;
    const double queued_frames = static_cast<double>(at.queuedBytes) / avg_frame;
    std::printf("  sent %llu frames, %zu bytes queued (~%.0f frames), device got %zu\n",
                static_cast<unsigned long long>(at.framesSent), at.queuedBytes, queued_frames, l.paus.size());
    KV_CHECK(static_cast<double>(l.paus.size()) + queued_frames / 2.0 <= static_cast<double>(at.framesSent));
}

// A revoke drops what is still queued for the device, thumbnails included. As above, a device
// that stops reading has two 0x15 thumbnails queued behind a backlog of 0x19 frames; the PC revokes it; once it
// reads again it gets only what was already on its way and then its 0x0F, never a queued thumbnail. The old code
// dropped only the video, so both thumbnails went out after the revoke.
// The device pings after every frame as it drains. The host used to close its socket as soon as its own queue was
// empty, while the device still had what was on its way to read; a ping that reached the closed socket drew a reset,
// and the reset threw away the unread frames and the 0x0F (the flake at the first waitFor below: no 0x0F, no close
// seen, for 20 s). Pinging every 250 ms hit that now and then; pinging on every read hits it every time.
KV_TEST(roll_a_revoke_drops_queued_thumbnails_before_the_notice) {
    Dirs dirs(L"revoke-thumbs");
    constexpr size_t kBacklog = 128 * 1024;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.server.playback_backlog_bytes = kBacklog; }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3), b(3);
    a.setSmallReceiveBuffer(true);
    RollPhone ra(a), rb(b);
    KV_REQUIRE(joinDevice(srv, b, PetRole::Owner, rb)); // stays listed, so the revoke leaves the list non-empty
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    const std::string clip = recordClip(host, ra, 8);
    KV_REQUIRE(!clip.empty());
    const std::optional<RollClip> c = clipOf(ra, clip);
    KV_REQUIRE(c.has_value() && c->bytes >= 4 * kBacklog);
    ra.pauseReading(true);
    ra.play(playback_action::kStart, clip);
    auto probe = [&]() -> std::optional<HostStreamServer::PlaybackProbe> {
        const auto p = srv.playbackProbes();
        if (p.empty()) return std::nullopt;
        return p.front();
    };
    KV_REQUIRE(waitUntil([&] { const auto p = probe(); return p && p->queuedBytes > kBacklog; }, 20000));
    const size_t q0 = probe()->queuedBytes;
    const uint64_t t1 = ra.thumb(clip);
    const uint64_t t2 = ra.thumb(clip);
    KV_REQUIRE(waitUntil([&] { const auto p = probe(); return p && p->queuedBytes > q0 + 2000; }, 10000));
    std::string rerr;
    KV_REQUIRE(srv.revokeDevice(idOf(a), &rerr));
    ra.pingEveryRead(true);
    ra.pauseReading(false);
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return !l.notices.empty() || l.closed; }, 20000));
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.closed; }, 20000));
    const RLog l = ra.log();
    KV_CHECK_MSG(!l.notices.empty(), "no 0x0F before the close");
    KV_CHECK_MSG(l.thumb(t1) == nullptr && l.thumb(t2) == nullptr, std::to_string(l.thumbs.size()) + " thumbnail(s) after the revoke");
    KV_CHECK(l.thumbs.empty());
}

// Test gap: the disk floor (§6.4) at the recorder and on the wire, through the recorder's free-space seam.
// Below 20 GB recording stops with disk_floor; it comes back only above 21 GB.
KV_TEST(roll_the_disk_floor_stops_recording_and_it_comes_back_above_21_gb) {
    Dirs dirs(L"floor");
    std::atomic<uint64_t> free_bytes{100000000000ULL};
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) {
        o.roll.freeBytes = [&](const std::wstring&) -> std::optional<uint64_t> { return free_bytes.load(); };
        o.roll.floorCheckMs = 200;
    }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_REQUIRE(petSays(ra, [](const PetCameraStateMessage& p) { return p.clipRecording && p.clipElapsedSec >= 1; }, 20000));
    const uint64_t finalized = host.ctl.rollStatus().clipsFinalized;
    free_bytes = 19000000000ULL;
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) {
        return p.rollBlockedReason == std::string(roll_blocked::kDiskFloor) && p.rollBlockedText == std::string(text::kDiskFloor) &&
               !p.clipRecording && !p.clipManualActive;
    }));
    KV_CHECK(host.ctl.rollStatus().diskFloor && host.ctl.rollStatus().block == RollBlock::DiskFloor);
    KV_CHECK(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized > finalized; }, 15000)); // kept
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kDiskFloor);
    // 20.5 GB: over the floor but not over 21 GB, so still stopped (no flapping at the line).
    free_bytes = 20500000000ULL;
    sleepMs(800); // several 200 ms checks
    KV_CHECK(host.ctl.rollStatus().diskFloor);
    KV_CHECK(ra.log().lastPet()->rollBlockedReason == std::string(roll_blocked::kDiskFloor));
    // 21.5 GB: it resumes on its own, and start_clip works.
    free_bytes = 21500000000ULL;
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.rollBlockedReason; }));
    KV_CHECK(!host.ctl.rollStatus().diskFloor);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
}

KV_TEST(roll_list_pages_with_cursors_and_checks_what_the_device_sends) {
    Dirs dirs(L"paging");
    // Five clips today (10 s apart, so they share a day unless this runs in the first minute after midnight),
    // one yesterday, one the day before: three days.
    const std::vector<std::string> ids =
        seedRoll(dirs, {2 * kDayMs + 50000, kDayMs + 50000, 50000, 40000, 30000, 20000, 10000});
    KV_REQUIRE(ids.size() == 7);
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    KV_REQUIRE(host.ctl.rollAvailable());
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    // §5.6: a list request never writes. Nothing else writes here either (no pet-camera.json, so no retention),
    // so the roll's files must stay byte for byte as they are, index.json included, through every list below.
    const auto before_lists = snapshotTree(dirs.roll);
    KV_REQUIRE(before_lists.size() > 10);

    std::set<std::string> seeded_days;
    for (const auto& id : ids) seeded_days.insert(dayOf(id)); // three, or four if this runs just after midnight
    // Days, newest first, two to a page.
    std::vector<std::string> days;
    std::optional<std::string> cursor;
    for (int page = 0; page < 5; ++page) {
        const uint64_t id = ra.list(std::nullopt, cursor, 2);
        KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(id); }));
        const RLog l = ra.log();
        const RollListMessage* m = l.list(id);
        KV_REQUIRE(m && m->days);
        KV_CHECK(m->days->size() <= 2);
        for (const auto& d : *m->days) days.push_back(d.day);
        cursor = m->nextCursor;
        if (!cursor) break;
    }
    KV_CHECK(days.size() == seeded_days.size());
    KV_CHECK(std::is_sorted(days.rbegin(), days.rend()) && std::set<std::string>(days.begin(), days.end()) == seeded_days);
    // Today's clips, two to a page, newest first, none twice, none missing; the day is echoed.
    const std::string today = dayOf(ids.back());
    std::vector<RollClip> got;
    std::optional<std::string> first_cursor;
    cursor.reset();
    int pages = 0;
    for (; pages < 10; ++pages) {
        const uint64_t id = ra.list(today, cursor, 2);
        KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(id); }));
        const RLog l = ra.log();
        const RollListMessage* m = l.list(id);
        KV_REQUIRE(m && m->clips && m->day == today && !m->days);
        got.insert(got.end(), m->clips->begin(), m->clips->end());
        cursor = m->nextCursor;
        if (pages == 0) first_cursor = cursor;
        if (!cursor) break;
    }
    int today_count = 0;
    for (const auto& id : ids) today_count += dayOf(id) == today ? 1 : 0;
    KV_CHECK_MSG(static_cast<int>(got.size()) == today_count, std::to_string(got.size()));
    KV_CHECK(pages == (today_count + 1) / 2 - 1);
    for (size_t i = 1; i < got.size(); ++i) KV_CHECK(got[i].startUnixMs < got[i - 1].startUnixMs);
    std::set<std::string> seen;
    for (const auto& c : got) {
        seen.insert(c.clipId);
        KV_CHECK(c.trigger == "motion" && !c.startedBy && c.width == 1024);
    }
    KV_CHECK(seen.size() == got.size());
    // What the device sends is checked (§5.6, §7.1): a malformed cursor is invalid, a day that isn't real or
    // isn't in the index is not_found, a limit outside 1-100 is invalid.
    KV_CHECK(ra.errorOf(ra.list(today, std::string("!!not-base64!!"))) == pet_error::kInvalid);
    KV_CHECK(ra.errorOf(ra.list(today, std::string("Z2FyYmFnZQ"))) == pet_error::kInvalid); // "garbage"
    KV_CHECK(ra.errorOf(ra.list(std::string("2026-13-01"))) == pet_error::kNotFound);
    KV_CHECK(ra.errorOf(ra.list(std::string("2001-01-01"))) == pet_error::kNotFound);
    KV_CHECK(ra.errorOf(ra.list(std::string("..\\..\\x"))) == pet_error::kNotFound);
    KV_CHECK(ra.errorOf(ra.list(today, std::nullopt, 0)) == pet_error::kInvalid);
    KV_CHECK(ra.errorOf(ra.list(today, std::nullopt, 101)) == pet_error::kInvalid);
    KV_CHECK(snapshotTree(dirs.roll) == before_lists); // every list and error above wrote nothing
    // A cursor whose anchor is gone still pages on (§5.6): the first page ended at got[1]; delete it, and the
    // next page still starts at got[2].
    KV_REQUIRE(got.size() >= 4 && first_cursor);
    const uint64_t del = ra.cmd(pet_action::kDeleteClip, std::nullopt, got[1].clipId);
    KV_CHECK(ra.okOf(del));
    const auto after_delete = snapshotTree(dirs.roll);
    {
        const uint64_t id = ra.list(today, first_cursor, 2);
        KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(id); }));
        const RLog l = ra.log();
        const RollListMessage* m = l.list(id);
        KV_REQUIRE(m && m->clips && m->clips->size() == 2);
        KV_CHECK(m->clips->at(0).clipId == got[2].clipId && m->clips->at(1).clipId == got[3].clipId);
    }
    // Thumbnails: one with a .jpg, one without (not_found), an id that isn't valid (not_found).
    const RollClip* with = nullptr;
    const RollClip* without = nullptr;
    for (const auto& c : got) {
        if (c.clipId == got[1].clipId) continue;
        if (c.hasThumbnail && !with) with = &c;
        if (!c.hasThumbnail && !without) without = &c;
    }
    KV_REQUIRE(with && without);
    const uint64_t t1 = ra.thumb(with->clipId);
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(t1); }));
    KV_CHECK(ra.log().thumb(t1) && ra.log().thumb(t1)->jpeg.size() == 300);
    KV_CHECK(ra.errorOf(ra.thumb(without->clipId)) == pet_error::kNotFound);
    KV_CHECK(ra.errorOf(ra.thumb("2026-11-18/14-03-22_motion\\..")) == pet_error::kNotFound);
    // A list request never writes (§5.6): since the delete, the paging and thumbnail requests changed no byte.
    KV_CHECK(snapshotTree(dirs.roll) == after_delete);
    KV_CHECK(countFiles(dirs.roll, L".tmp") == 0);
}

KV_TEST(roll_stars_by_role_viewer_cant_delete_and_revoke_drops_stars) {
    Dirs dirs(L"stars");
    const std::vector<std::string> ids = seedRoll(dirs, {3 * kMinMs, 2 * kMinMs}, false);
    KV_REQUIRE(ids.size() == 2);
    const std::string c1 = ids[0], c2 = ids[1];
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3), v(3);
    RollPhone ra(a), rv(v);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(joinDevice(srv, v, PetRole::Viewer, rv));
    // A viewer while sharing is off: no roll (§3.1, §7.3).
    KV_CHECK(petSays(rv, [](const PetCameraStateMessage& p) { return !p.rollAvailable && !p.playbackAvailable; }));
    KV_CHECK(rv.errorOf(rv.list()) == pet_error::kSharingOff);
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kStarClip, std::nullopt, c1, true)) == pet_error::kSharingOff);
    KV_CHECK(rv.errorOf(rv.thumb(c1)) == pet_error::kSharingOff);
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kStartClip)) == pet_error::kSharingOff);
    srv.setSharingEnabled(true);
    KV_CHECK(petSays(rv, [](const PetCameraStateMessage& p) { return p.rollAvailable; }));
    // The viewer stars c1: starred for everyone, starredByMe only for it.
    KV_CHECK(rv.okOf(rv.cmd(pet_action::kStarClip, std::nullopt, c1, true)));
    {
        bool v_mine = false, a_mine = true;
        KV_CHECK(starredFor(rv, c1, &v_mine) && v_mine);
        KV_CHECK(starredFor(ra, c1, &a_mine) && !a_mine);
    }
    // A viewer may unstar only its own star (§5.2).
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kStarClip, std::nullopt, c2, false)) == pet_error::kNotAllowedForRole);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStarClip, std::nullopt, c2, true)));
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kStarClip, std::nullopt, c2, false)) == pet_error::kNotAllowedForRole);
    KV_CHECK(starredFor(ra, c2));
    // A viewer can't delete (§3.1); nothing is deleted.
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kDeleteClip, std::nullopt, c2)) == pet_error::kNotAllowedForRole);
    KV_CHECK(clipOf(ra, c1) && clipOf(ra, c2));
    // An owner's unstar clears every star on the clip.
    KV_CHECK(rv.okOf(rv.cmd(pet_action::kStarClip, std::nullopt, c2, true)));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStarClip, std::nullopt, c2, false)));
    KV_CHECK(clipOf(rv, c2) && !starredFor(rv, c2));
    // Bad ids and missing keys (§5.2, §7.1).
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStarClip, std::nullopt, std::string("../c1"), true)) == pet_error::kNotFound);
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStarClip, std::nullopt, std::string("2001-01-01/01-01-01_manual"), true)) ==
             pet_error::kNotFound);
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStarClip, std::nullopt, c1)) == pet_error::kInvalid);
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kDeleteClip)) == pet_error::kInvalid);
    KV_CHECK(ra.errorOf(ra.cmd("set_roll")) == pet_error::kInvalid);
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kSetRoll, true)) == pet_error::kNotAllowedForRole);
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kSetMotion, true)) == pet_error::kNotAllowedForRole);
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kSetMotion, std::nullopt, std::nullopt, std::nullopt, 11)) == pet_error::kInvalid);

    // Revoking the viewer drops the stars it set (v3 §3.2 step 5), and only those.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStarClip, std::nullopt, c2, true)));
    KV_CHECK(rv.okOf(rv.cmd(pet_action::kStarClip, std::nullopt, c1, true))); // c1: the viewer's own star
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kRevokeDevice, std::nullopt, std::nullopt, std::nullopt, std::nullopt, idOf(v))));
    KV_CHECK(rv.waitFor([](const RLog& l) { return l.closed; }));
    KV_CHECK(waitUntil([&] { return clipOf(ra, c1) && !starredFor(ra, c1); }, 10000));
    KV_CHECK(starredFor(ra, c2));
    // The owner deletes c2: gone from the list and from the disk.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kDeleteClip, std::nullopt, c2)));
    KV_CHECK(!clipOf(ra, c2) && clipOf(ra, c1));
    KV_CHECK(countFiles(dirs.roll, L".mp4") == 1);
    // The PC's own star and delete (the PC is an owner, "pc").
    KV_CHECK(srv.setRollClipStarByPc(c1, true) == roll::RollError::None);
    KV_CHECK(starredFor(ra, c1));
    KV_CHECK(srv.deleteRollClip(c1) == roll::RollError::None);
    KV_CHECK(!clipOf(ra, c1));
}

// ============================================================================================================
// Privacy mode, the other blocks, and D6
// ============================================================================================================

KV_TEST(roll_privacy_stops_recording_and_playback_and_refuses_thumbnails) {
    Dirs dirs(L"privacy");
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    const std::string clip = recordClip(host, ra, 3);
    KV_REQUIRE(!clip.empty());
    // A clip recording and a playback running when privacy mode comes on.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_REQUIRE(petSays(ra, [](const PetCameraStateMessage& p) { return p.clipRecording && p.clipElapsedSec >= 1; }, 20000));
    const uint64_t finalized = host.ctl.rollStatus().clipsFinalized;
    ra.play(playback_action::kStart, clip);
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(1, "playing") != nullptr; }));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetPrivacy, true)));
    // §6.4: the playback stops at once (0x17 stopped with the privacy sentence), the clip is finalized.
    KV_REQUIRE(ra.waitFor([](const RLog& l) { return l.hostState(1, "stopped") != nullptr; }));
    {
        const RLog l = ra.log();
        const PlaybackStateMessage* s = l.hostState(1, "stopped");
        KV_CHECK(s->message == std::string(text::kPrivacyMode));
        const int i = l.indexOf("17:stopped:1");
        KV_CHECK(l.indexOf("19:1:", i + 1) < 0);
    }
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) {
        return p.privacyMode && !p.clipRecording && !p.clipManualActive && p.rollBlockedReason == std::string(roll_blocked::kPrivacyMode) &&
               !p.playbackAvailable && p.rollAvailable;
    }));
    KV_CHECK(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized > finalized; }, 15000));
    // While on: thumbnails, playback and start_clip are refused; listing, stop_clip and stars keep working.
    KV_CHECK(ra.errorOf(ra.thumb(clip)) == pet_error::kPrivacyMode);
    KV_CHECK(ra.errorOf(ra.play(playback_action::kStart, clip)) == pet_error::kPrivacyMode);
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kPrivacyMode);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
    KV_CHECK(clipOf(ra, clip).has_value());
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStarClip, std::nullopt, clip, true)));
    // A viewer gets privacy_mode for start_clip too (the §5.1 order puts privacy first), even with sharing off;
    // and so for a thumbnail and a playback start (0x14 used to answer sharing_off here).
    FakePhone v(3);
    RollPhone rv(v);
    KV_REQUIRE(joinDevice(srv, v, PetRole::Viewer, rv));
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kStartClip)) == pet_error::kPrivacyMode);
    KV_CHECK_MSG(rv.errorOf(rv.thumb(clip)) == pet_error::kPrivacyMode, rv.errorOf(rv.thumb(clip)));
    KV_CHECK(rv.errorOf(rv.play(playback_action::kStart, clip)) == pet_error::kPrivacyMode);
    // Off again: nothing restarts on its own (the manual trigger was dropped), and thumbnails come back.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetPrivacy, false)));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.privacyMode && !p.rollBlockedReason; }));
    sleepMs(500);
    KV_CHECK(!host.ctl.rollStatus().recording);
    const uint64_t th = ra.thumb(clip);
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(th); }));
    KV_CHECK(ra.log().thumb(th) != nullptr);
}

KV_TEST(roll_blocks_follow_the_view_the_sensor_and_a_pc_replay_and_d6_skips_turn_off) {
    Dirs dirs(L"blocks");
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone a(3), v(3);
    RollPhone ra(a), rv(v);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(joinDevice(srv, v, PetRole::Viewer, rv));
    KV_REQUIRE(enableRoll(ra));
    auto blocked = [&](const char* code) {
        return petSays(ra, [&](const PetCameraStateMessage& p) {
            return code ? p.rollBlockedReason == std::string(code) && p.rollBlockedText.has_value() : !p.rollBlockedReason;
        });
    };
    // A frozen view: a clip in progress is finalized, start_clip refused with view_frozen.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_REQUIRE(petSays(ra, [](const PetCameraStateMessage& p) { return p.clipRecording && p.clipElapsedSec >= 1; }, 20000));
    const uint64_t finalized = host.ctl.rollStatus().clipsFinalized;
    HostViewState vs;
    vs.isHostFrozen = true;
    host.frozen = true;
    host.ctl.setViewState(vs);
    KV_CHECK(blocked(roll_blocked::kViewFrozen));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.clipRecording; }));
    KV_CHECK(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized > finalized; }, 15000));
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kViewFrozen);
    // Privacy comes first in the order (§5.1), over a frozen view.
    srv.setPrivacyMode(true);
    KV_CHECK(blocked(roll_blocked::kPrivacyMode));
    srv.setPrivacyMode(false);
    KV_CHECK(blocked(roll_blocked::kViewFrozen));
    vs.isHostFrozen = false;
    host.frozen = false;
    host.ctl.setViewState(vs);
    KV_CHECK(blocked(nullptr));
    // The sensor off.
    host.setSensor(false);
    KV_CHECK(blocked(roll_blocked::kSensorOff));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.motionAvailable; }));
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kStartClip)) == pet_error::kSensorOff);
    host.setSensor(true);
    KV_CHECK(blocked(nullptr));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return p.motionAvailable; }));
    // A PC replay: blocked for everyone, and a viewer loses the roll (pc_replaying) while an owner keeps it.
    vs.isReplayingOnHost = true;
    host.ctl.setViewState(vs);
    KV_CHECK(blocked(roll_blocked::kPcReplaying));
    KV_CHECK(petSays(rv, [](const PetCameraStateMessage& p) { return !p.rollAvailable; }));
    KV_CHECK(rv.errorOf(rv.list()) == pet_error::kPcReplaying);
    KV_CHECK(rv.errorOf(rv.cmd(pet_action::kStartClip)) == pet_error::kPcReplaying);
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kPcReplaying);
    const uint64_t owner_list = ra.list();
    KV_CHECK(ra.waitFor([&](const RLog& l) { return l.list(owner_list) != nullptr; }));
    vs.isReplayingOnHost = false;
    host.ctl.setViewState(vs);
    KV_CHECK(blocked(nullptr));
    // The disk floor, through the store's free-space seam, is covered in roll_store; here the PC's own manual
    // clip: startedBy "This PC".
    KV_CHECK(host.ctl.startManualClip().ok);
    KV_REQUIRE(petSays(ra, [](const PetCameraStateMessage& p) { return p.clipRecording && p.clipManualActive; }, 20000));
    KV_CHECK(host.ctl.stopManualClip() == StopClipResult::Ok);
    KV_REQUIRE(waitUntil([&] { return !host.ctl.rollStatus().recording; }, 10000));

    // D6: "Turn the Kinect off when the iPhone disconnects" is skipped while the roll is on.
    srv.setTurnOffKinectOnPhoneDisconnect(true);
    rv.stop();
    v.close();
    ra.stop();
    a.close();
    KV_CHECK(waitUntil([&] { return host.disconnects.load() >= 1; }, 10000));
    KV_CHECK(host.turn_off_requests.load() == 0);
    // ...and applies again with the roll off.
    srv.setRollEnabled(false);
    FakePhone b(3);
    RollPhone rb(b);
    KV_REQUIRE(joinDevice(srv, b, PetRole::Owner, rb));
    const int before = host.disconnects.load();
    rb.stop();
    b.close();
    KV_CHECK(waitUntil([&] { return host.disconnects.load() > before; }, 10000));
    KV_CHECK(host.turn_off_requests.load() == 1);
}

// Host-initiated stops (§5.9): an owner takes a playing viewer's slot; a viewer loses roll access.
KV_TEST(roll_playback_is_stopped_when_an_owner_takes_the_slot_or_sharing_goes_off) {
    Dirs dirs(L"slots");
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    // A clip to play, recorded by an owner that then leaves.
    std::string clip;
    {
        FakePhone o(3);
        RollPhone ro(o);
        KV_REQUIRE(joinDevice(srv, o, PetRole::Owner, ro));
        KV_REQUIRE(enableRoll(ro));
        clip = recordClip(host, ro, 3);
        ro.stop();
        o.close();
    }
    KV_REQUIRE(!clip.empty());
    KV_REQUIRE(waitUntil([&] { return srv.sessionCount() == 0; }, 10000));
    // Three viewers watching live, then a fourth that plays the clip (the newest slot).
    FakePhone v1(3), v2(3), v3(3), v4(3);
    RollPhone r1(v1), r2(v2), r3(v3), r4(v4);
    KV_REQUIRE(joinDevice(srv, v1, PetRole::Viewer, r1));
    KV_REQUIRE(joinDevice(srv, v2, PetRole::Viewer, r2));
    KV_REQUIRE(joinDevice(srv, v3, PetRole::Viewer, r3));
    KV_REQUIRE(joinDevice(srv, v4, PetRole::Viewer, r4));
    KV_REQUIRE(waitUntil([&] { return srv.watchingNow().size() == 4; }, 10000));
    const uint64_t p4 = r4.play(playback_action::kStart, clip);
    KV_REQUIRE(r4.waitFor([](const RLog& l) { return l.hostState(1, "playing") != nullptr; }));
    (void)p4;
    // A fifth viewer can't start a playback: no slot (too_many_viewers).
    FakePhone v5(3);
    RollPhone r5(v5);
    KV_REQUIRE(joinDevice(srv, v5, PetRole::Viewer, r5));
    KV_CHECK(petSays(r5, [](const PetCameraStateMessage& p) { return p.rollAvailable && !p.playbackAvailable; }));
    KV_CHECK(r5.errorOf(r5.play(playback_action::kStart, clip)) == pet_error::kTooManyViewers);
    r5.stop();
    v5.close();
    // An owner arrives: it takes the newest viewer slot, the playing one, whose playback stops.
    FakePhone ow(3);
    RollPhone row(ow);
    KV_REQUIRE(joinDevice(srv, ow, PetRole::Owner, row));
    KV_REQUIRE(r4.waitFor([](const RLog& l) { return l.hostState(1, "stopped") != nullptr; }));
    KV_CHECK(r4.log().hostState(1, "stopped")->message == std::string(text::kTooManyViewers));
    KV_CHECK(petSays(r4, [](const PetCameraStateMessage& p) {
        return p.liveUnavailableReason == std::string(live_unavailable::kTooManyViewers);
    }));
    {
        const RLog l4 = r4.log();
        KV_REQUIRE(l4.lastPet());
        KV_CHECK(!l4.lastPet()->liveAllowed);
        KV_CHECK(!l4.lastPet()->playbackAvailable); // no slot, so no playback either
    }
    // A viewer that holds a slot plays, then sharing goes off: its playback stops with sharing_off.
    r3.play(playback_action::kStart, clip);
    KV_REQUIRE(r3.waitFor([](const RLog& l) { return l.hostState(1, "playing") != nullptr; }));
    {
        const std::vector<PetWatcher> w = srv.watchingNow();
        int playing = 0;
        for (const auto& x : w) playing += x.activity == watcher_activity::kPlayback ? 1 : 0;
        KV_CHECK(playing == 1 && w.size() == 4);
    }
    srv.setSharingEnabled(false);
    KV_REQUIRE(r3.waitFor([](const RLog& l) { return l.hostState(1, "stopped") != nullptr; }));
    KV_CHECK(r3.log().hostState(1, "stopped")->message == std::string(text::kSharingOff));
    KV_CHECK(waitUntil([&] { return srv.playbackCount() == 0; }, 5000));
}

// ============================================================================================================
// The 10-minute segment cut (with the test clock), writer failure, and motion by depth
// ============================================================================================================

KV_TEST(roll_a_clip_is_cut_into_10_minute_segments) {
    Dirs dirs(L"segment");
    TestRollClock clock;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.roll.clock = &clock; }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    KV_REQUIRE(ringFilled(host)); // the first segment gets a full pre-roll: 4-5 s (GOPs are 1 s)
    // The test clock also runs in real time: the trigger is somewhere in [t_before, t_after] on it.
    const uint64_t t_before = clock.monoMs();
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    const uint64_t t_after = clock.monoMs();
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().recording; }, 10000));
    sleepMs(2000); // real frames into the first segment
    // Five minutes on, start_clip again: the manual trigger's own 10 minutes restart (§6.2), so it is still
    // active when the clip reaches its 10-minute segment cap.
    clock.advance(5 * 60 * 1000);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    // The cap is the file's 10 minutes, pre-roll included. 10:00 - 6 s after the trigger, the file (with at
    // most 5 s of pre-roll) is still under 10 minutes: no cut.
    clock.advance(t_before + 10 * 60 * 1000 - 6000 - clock.monoMs());
    sleepMs(300); // a few recorder ticks
    KV_CHECK(host.ctl.rollStatus().clipsFinalized == 0);
    // 10:00 - 2.5 s after the trigger, the file with its 4-5 s of pre-roll is past 10 minutes: cut within a
    // tick or two. Measured from the trigger, as before, it would cut only once 2.5 s more had passed.
    clock.advance(t_after + 10 * 60 * 1000 - 2500 - clock.monoMs());
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().clipElapsedSec < 60; }, 1500)); // the new segment's
    // The first segment is finalized and the next starts at once, under the same trigger.
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized == 1; }, 15000));
    const std::string first_id = host.ctl.rollStatus().lastClipId;
    KV_CHECK(host.ctl.rollStatus().recording && host.ctl.rollStatus().manualActive);
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return p.clipRecording && p.clipElapsedSec < 60; }));
    sleepMs(2500); // real frames into the second segment (it starts at its first IDR)
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized == 2; }, 15000));
    const RollRecorderStatus st = host.ctl.rollStatus();
    const std::vector<RollClip> clips = clipsOf(ra, dayOf(st.lastClipId));
    KV_REQUIRE(clips.size() == 2);
    for (const auto& c : clips) {
        std::printf("  segment %s: %.2f s\n", c.clipId.c_str(), c.durationSec);
        KV_CHECK(c.trigger == "manual" && c.durationSec > 0.5);
    }
    KV_CHECK(clips[0].clipId != clips[1].clipId);
    // The first file reached its 10 minutes (on the test clock) with its pre-roll: at least 3 s of it.
    const std::shared_ptr<roll::RollStore> store = host.ctl.rollStore();
    const std::optional<roll::ClipSidecar> s1 = store->findClip(first_id);
    const std::string second_id = clips[0].clipId == first_id ? clips[1].clipId : clips[0].clipId;
    const std::optional<roll::ClipSidecar> s2 = store->findClip(second_id);
    KV_REQUIRE(s1 && s2);
    KV_CHECK_MSG(s1->preRollSec >= 3.0 && s1->preRollSec <= 6.0, std::to_string(s1->preRollSec));
    KV_CHECK(s2->preRollSec == 0.0);
    // The gap at the cut: the host asks for a keyframe at once, so the next segment starts about a frame
    // after the first one's last frame (frames are 33 ms apart; the two start times are each taken from the
    // capture clock, to the millisecond).
    const int64_t gap_ms = static_cast<int64_t>(s2->startUnixMs) - static_cast<int64_t>(s1->endUnixMs);
    std::printf("  gap at the cut: %lld ms (first segment pre-roll %.2f s)\n", static_cast<long long>(gap_ms), s1->preRollSec);
    KV_CHECK_MSG(gap_ms >= -40 && gap_ms <= 150, std::to_string(gap_ms));
}

KV_TEST(roll_a_writer_that_fails_ends_the_clip_cleanly) {
    Dirs dirs(L"writer-fail");
    RollHost host;
    std::string err;
    // One byte of queue: the writer refuses the clip's first frame, as a disk that can't keep up would.
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.roll.writerMaxQueuedBytes = 1; }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().clipsFailed >= 1; }, 20000));
    const RollRecorderStatus st = host.ctl.rollStatus();
    std::printf("  writer failure: %s\n", st.lastError.c_str());
    KV_CHECK(!st.lastError.empty() && st.clipsFinalized == 0);
    // The clip ended and every trigger was dropped: nothing records, and stop_clip is the no-op ok.
    KV_CHECK(waitUntil([&] { return !host.ctl.rollStatus().recording; }, 5000));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.clipRecording && !p.clipManualActive; }));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
    // Nothing is left behind: no reservation, no half-written clip, nothing listed.
    KV_CHECK(waitUntil([&] { return countFiles(dirs.roll, L".mp4") == 0; }, 5000));
    KV_CHECK(countFiles(dirs.roll, L".json") == 0 && countFiles(dirs.roll, L".jpg") == 0);
    const uint64_t id = ra.list();
    KV_REQUIRE(ra.waitFor([&](const RLog& l) { return l.answered(id); }));
    KV_REQUIRE(ra.log().list(id) && ra.log().list(id)->days);
    KV_CHECK(ra.log().list(id)->days->empty());
}


// A writer failing after it opened and took frames used to dereference a null clip (abortLocked was
// given the member it reset). The clip is now cut short and kept, every trigger is dropped, and the host
// keeps running.
KV_TEST(roll_a_writer_that_fails_mid_clip_cuts_the_clip_short_and_keeps_running) {
    Dirs dirs(L"writer-midclip");
    RollHost host;
    std::string err;
    // The writer opens and takes the pre-roll and 15 live frames, then refuses the next one.
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.roll.failAppendAfter = 15; }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized + host.ctl.rollStatus().clipsFailed >= 1; }, 20000));
    const RollRecorderStatus st = host.ctl.rollStatus();
    std::printf("  mid-clip failure: %s (finalized %llu, failed %llu)\n", st.lastError.c_str(),
                static_cast<unsigned long long>(st.clipsFinalized), static_cast<unsigned long long>(st.clipsFailed));
    KV_CHECK(st.lastError.find("couldn't be written") != std::string::npos);
    // What was written before the failure is kept (§6.1: a clip cut short is still a clip).
    KV_CHECK(st.clipsFinalized == 1 && st.clipsFailed == 0);
    KV_CHECK(waitUntil([&] { return !host.ctl.rollStatus().recording; }, 5000));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.clipRecording && !p.clipManualActive; }));
    KV_REQUIRE(!st.lastClipId.empty());
    KV_CHECK(clipOf(ra, st.lastClipId).has_value());
    // The recorder still works: the next clip records (and is cut short the same way).
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_CHECK(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized == 2; }, 20000));
}

// The other abort, a clip whose writer hasn't opened while past what it may hold. The recorder's thread is
// held so the writer can't open; the next AU aborts the clip. It is counted as failed, nothing is left behind,
// and the host keeps running.
KV_TEST(roll_a_clip_whose_writer_doesnt_open_in_time_is_aborted_without_a_crash) {
    Dirs dirs(L"writer-late");
    ThreadHold th;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) {
        o.roll.maxPendingBytes = 1; // the pre-roll alone is over it: the first AU after the start aborts the clip
        o.roll.threadHook = th.hook();
    }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    KV_REQUIRE(ringFilled(host));
    KV_REQUIRE(th.engage());
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    // The abort happens on the encoder's thread at the next AU; the recorder (held) reports it once released.
    KV_CHECK(waitUntil([&] { return !host.ctl.rollStatus().recording; }, 10000));
    th.release();
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().clipsFailed >= 1; }, 20000));
    const RollRecorderStatus st = host.ctl.rollStatus();
    std::printf("  late writer: %s\n", st.lastError.c_str());
    KV_CHECK(st.lastError.find("didn't open in time") != std::string::npos && st.clipsFinalized == 0);
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return !p.clipRecording && !p.clipManualActive; }));
    KV_CHECK(host.ctl.rollStatus().clipsFinalized == 0 && countFiles(dirs.roll, L".mp4") == 0);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
}

// The gate is the time an AU was captured, not the time it arrives. `medium` keeps encoding through its
// hold after a block, and AUs still in the pipeline land after the block clears; one captured before the
// block cleared (during privacy mode, or before it) never reaches the pre-roll ring or a clip. Fed straight
// to a recorder with synthetic AUs (a few made-up bytes; nothing is decoded), its thread held so nothing is
// opened while they are looked at.
KV_TEST(roll_an_au_captured_before_a_block_cleared_never_reaches_the_ring_or_a_clip) {
    Dirs dirs(L"stale-au");
    ThreadHold th;
    RollRecorder rec;
    RollRecorder::Options o;
    o.dir = dirs.roll;
    o.recordingsDir = dirs.recordings;
    o.threadHook = th.hook();
    std::string err;
    const bool started = rec.start(o, &err);
    KV_CHECK_MSG(started, err);
    KV_REQUIRE(started);
    RollRecorder::Settings st;
    st.enabled = true;
    st.settingsRead = true; // the folder is prepared on the recorder's thread, then it arms
    rec.setSettings(st);
    rec.setSensorOn(true);
    KV_REQUIRE(waitUntil([&] { return rec.status().armed; }, 10000));
    KV_REQUIRE(th.engage());
    auto au = [](uint64_t capture_ns, bool idr) {
        TierAccessUnit t;
        t.tier = LiveTier::Medium;
        t.epoch = 1;
        t.idr = idr;
        t.avcc = {0, 0, 0, 2, static_cast<uint8_t>(idr ? 0x65 : 0x41), 0x88};
        t.sps = {0x67, 0x42, 0xC0, 0x1E}; // the ring keeps an IDR only with its parameter sets
        t.pps = {0x68, 0xCE, 0x3C, 0x80};
        t.width = 64;
        t.height = 64;
        t.capture_ns = capture_ns;
        return t;
    };
    auto hns = [](uint64_t ns) { return static_cast<int64_t>(ns / 100); };
    const uint64_t before = kv_monotonic_ns();
    rec.onMediumAccessUnit(au(before, true));
    KV_CHECK(rec.heldForTest().ring.size() == 1);
    // Privacy on: disarmed, and the ring is cleared (on any block).
    rec.setPrivacyMode(true);
    KV_CHECK(!rec.status().armed && rec.heldForTest().ring.empty());
    const uint64_t during = kv_monotonic_ns();
    sleepMs(2); // the block clears strictly after `during`
    rec.setPrivacyMode(false);
    KV_REQUIRE(rec.status().armed);
    // Frames captured during privacy mode, or before it, arriving now (still in the encoder): dropped.
    rec.onMediumAccessUnit(au(during, true));
    rec.onMediumAccessUnit(au(before, false));
    KV_CHECK_MSG(rec.heldForTest().ring.empty(), std::to_string(rec.heldForTest().ring.size()));
    // A clip started now doesn't get them either.
    KV_REQUIRE(rec.startClip(ClipStartedBy::pc()).ok);
    rec.onMediumAccessUnit(au(during, false));
    {
        const RollRecorder::HeldForTest h = rec.heldForTest();
        KV_CHECK_MSG(h.ring.empty() && h.clip.empty(), std::to_string(h.ring.size()) + " " + std::to_string(h.clip.size()));
    }
    // A frame captured after the block cleared goes to both.
    const uint64_t after = kv_monotonic_ns();
    rec.onMediumAccessUnit(au(after, true));
    {
        const RollRecorder::HeldForTest h = rec.heldForTest();
        KV_CHECK(h.ring.size() == 1 && h.clip.size() == 1);
        for (int64_t c : h.ring) KV_CHECK(c == hns(after));
        for (int64_t c : h.clip) KV_CHECK(c == hns(after));
    }
    rec.stopClip();
    th.release();
    rec.stop();
}

// A clip that ends before the busy recorder thread gets to open it is still written (pet-camera.md §6.1:
// the clip in progress is finalized), from its pre-roll; it used to be dropped without a trace.
KV_TEST(roll_a_clip_that_ends_before_the_busy_thread_opens_it_is_still_written) {
    Dirs dirs(L"ended-early");
    ThreadHold th;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.roll.threadHook = th.hook(); }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    KV_REQUIRE(ringFilled(host));
    KV_REQUIRE(th.engage());
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStartClip)));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kStopClip)));
    KV_CHECK(!host.ctl.rollStatus().recording);
    th.release();
    KV_REQUIRE(waitUntil([&] {
        const RollRecorderStatus s = host.ctl.rollStatus();
        return s.clipsFinalized + s.clipsFailed >= 1;
    }, 20000));
    const RollRecorderStatus s = host.ctl.rollStatus();
    std::printf("  finalized %llu, failed %llu, %s\n", static_cast<unsigned long long>(s.clipsFinalized),
                static_cast<unsigned long long>(s.clipsFailed), s.lastError.c_str());
    KV_CHECK(s.clipsFinalized == 1 && s.clipsFailed == 0);
    const std::optional<RollClip> c = clipOf(ra, s.lastClipId);
    KV_REQUIRE(c.has_value());
    KV_CHECK(c->trigger == "manual" && c->durationSec >= 3.0); // the pre-roll (4-5 s)
}

namespace {

// Synthetic depth (pet-camera.md §1): a flat floor at 2000 mm, optional noise, and optionally a 64x64 box at
// 1200 mm whose left edge is at box_x.
std::vector<uint16_t> depthFrame(int box_x, uint32_t seed, int noise_mm) {
    std::vector<uint16_t> d(512 * 424, 2000);
    uint32_t r = seed * 2654435761u + 1;
    for (auto& px : d) {
        if (noise_mm > 0) {
            r = r * 1664525u + 1013904223u;
            px = static_cast<uint16_t>(2000 + static_cast<int>(r >> 16) % (2 * noise_mm + 1) - noise_mm);
        }
    }
    if (box_x >= 0) {
        for (int y = 160; y < 224; ++y)
            for (int x = box_x; x < box_x + 64 && x < 512; ++x) d[static_cast<size_t>(y) * 512 + x] = 1200;
    }
    return d;
}

} // namespace

KV_TEST(roll_motion_by_depth_starts_a_clip_from_the_live_sensor_only) {
    Dirs dirs(L"motion");
    TestRollClock clock;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.roll.clock = &clock; }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra));
    ra.watch(false);
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetMotion, true, std::nullopt, std::nullopt, 5)));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return p.motionEnabled && p.motionSensitivity == 5 && p.motionAvailable; }));
    uint64_t seq = 100;
    uint64_t ts = 1000000000ULL;
    auto feed = [&](int box_x, int noise, bool live, uint64_t seq_step = 1, uint32_t seed = 0) {
        const std::vector<uint16_t> f = depthFrame(box_x, seed ? seed : static_cast<uint32_t>(seq), noise);
        seq += seq_step;
        ts += 33000000ULL;
        host.ctl.submitDepthForMotion(f.data(), 512, 424, seq, ts, live);
    };
    auto noClip = [&] {
        sleepMs(300);
        return !host.ctl.rollStatus().recording;
    };
    // A moving box from a replay (not the live sensor) is never motion.
    for (int i = 0; i < 10; ++i) feed(-1, 0, false);
    for (int i = 0; i < 12; ++i) feed(32 * i, 0, false);
    KV_CHECK(noClip());
    // Live: the background, then noise around it (as the sensor gives), then a brightness or IR change
    // (which depth doesn't see: the depth stays as it was).
    for (int i = 0; i < 5; ++i) feed(-1, 0, true);
    for (int i = 0; i < 30; ++i) feed(-1, 15, true);
    KV_CHECK(noClip());
    // Frames after a dropped one never complete a run (W7): a gap every frame.
    for (int i = 0; i < 12; ++i) feed(32 * i, 0, true, 2);
    KV_CHECK(noClip());
    // A box moving on consecutive frames: motion, a clip named motion. The same frame polled twice (the same
    // sequence number) doesn't break the run.
    for (int i = 0; i < 8; ++i) {
        const std::vector<uint16_t> f = depthFrame(40 * i, 0, 0);
        ++seq;
        ts += 33000000ULL;
        host.ctl.submitDepthForMotion(f.data(), 512, 424, seq, ts, true);
        host.ctl.submitDepthForMotion(f.data(), 512, 424, seq, ts, true);
    }
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().recording; }, 5000));
    KV_CHECK(host.ctl.rollStatus().clipTrigger == ClipTrigger::Motion);
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return p.clipRecording && p.clipTrigger == std::string("motion"); }));
    // A motion clip with no manual trigger: stop_clip is busy (v3 §5.2).
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStopClip)) == pet_error::kBusy);
    sleepMs(1500); // real frames into it
    // 10 s after the last motion, the clip ends (the test clock), and it is kept.
    clock.advance(11000);
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().clipsFinalized == 1; }, 15000));
    KV_CHECK(!host.ctl.rollStatus().recording);
    const std::vector<RollClip> clips = clipsOf(ra, dayOf(host.ctl.rollStatus().lastClipId));
    KV_REQUIRE(clips.size() == 1);
    KV_CHECK(clips.front().trigger == "motion" && !clips.front().startedBy);
    // While blocked (privacy), depth is ignored: no clip.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetPrivacy, true)));
    clock.advance(40000); // past the cooldown
    for (int i = 0; i < 12; ++i) feed(36 * i, 0, true);
    KV_CHECK(noClip());
    // Motion off: ignored too.
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetPrivacy, false)));
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetMotion, false)));
    for (int i = 0; i < 12; ++i) feed(36 * i, 0, true);
    KV_CHECK(noClip());
}

// ============================================================================================================
// Settings: pet-camera.json's roll keys, and fail-closed writes
// ============================================================================================================

KV_TEST(roll_settings_round_trip_read_leniently_and_summarise) {
    PetSettings s;
    s.rollEnabled = true;
    s.rollFolder = "E:\\Roll";
    s.rollKeepDays = 14;
    s.rollCapBytes = 50000000000ULL;
    s.rollLimitsKnown = true;
    s.motionEnabled = true;
    s.motionSensitivity = 8;
    s.recordScheduleEnabled = true;
    s.recordSchedule = {RecordScheduleRow{0x7F, 8 * 60, 10}, RecordScheduleRow{(1 << 1) | (1 << 3), 22 * 60, 30}};
    PetSettings r;
    KV_REQUIRE(fromJson(toJson(s), r));
    KV_CHECK(r.rollEnabled && r.rollFolder == s.rollFolder && r.rollKeepDays == 14 && r.rollCapBytes == 50000000000ULL &&
             r.rollLimitsKnown && r.motionEnabled && r.motionSensitivity == 8 && r.recordScheduleEnabled &&
             r.recordSchedule == s.recordSchedule);
    // Limits nobody chose are never written, so a restart can't take the defaults for the owner's.
    PetSettings u = s;
    u.rollLimitsKnown = false;
    const std::string uj = toJson(u);
    KV_CHECK(uj.find("rollKeepDays") == std::string::npos && uj.find("rollCapBytes") == std::string::npos);
    PetSettings ur;
    KV_REQUIRE(fromJson(uj, ur));
    KV_CHECK(!ur.rollLimitsKnown && ur.rollKeepDays == 30 && ur.rollCapBytes == 200000000000ULL);
    // A W6-era file (no roll keys) doesn't make them known; one holding either key does.
    PetSettings w6;
    KV_REQUIRE(fromJson(R"({"sharingEnabled":true,"privacyMode":false,"sharingSchedule":[]})", w6));
    KV_CHECK(!w6.rollLimitsKnown);
    PetSettings k1;
    KV_REQUIRE(fromJson(R"({"rollEnabled":true,"rollKeepDays":90})", k1));
    KV_CHECK(k1.rollLimitsKnown && k1.rollKeepDays == 90);
    KV_CHECK(recordScheduleSummary(r.recordSchedule) == "Every day 08:00\xE2\x80\x93" "08:10; Mon, Wed 22:00\xE2\x80\x93" "22:30");
    // Defaults record nothing; a bad value keeps its default; a bad row is dropped, never guessed at.
    PetSettings d;
    KV_REQUIRE(fromJson(R"({"privacyMode":false})", d));
    KV_CHECK(!d.rollEnabled && !d.motionEnabled && d.motionSensitivity == 5 && d.rollKeepDays == 30 &&
             d.rollCapBytes == 200000000000ULL && d.recordSchedule.empty() && d.rollFolder.empty());
    PetSettings b;
    KV_REQUIRE(fromJson(R"({"rollEnabled":"yes","motionSensitivity":11,"rollKeepDays":0,"rollCapBytes":5,)"
                        R"("recordSchedule":[{"days":127,"start":"25:00","minutes":10},{"days":1,"start":"07:00","minutes":61},)"
                        R"({"days":2,"start":"07:30","minutes":5},7]})",
                        b));
    KV_CHECK(!b.rollEnabled && b.motionSensitivity == 5 && b.rollKeepDays == 30 && b.rollCapBytes == 200000000000ULL);
    KV_CHECK(!b.rollLimitsKnown); // bad limits are not the owner's
    KV_REQUIRE(b.recordSchedule.size() == 1);
    KV_CHECK(b.recordSchedule.front() == (RecordScheduleRow{2, 7 * 60 + 30, 5}));
}

KV_TEST(roll_changes_are_refused_while_pet_camera_json_cant_be_read) {
    Dirs dirs(L"unread");
    CreateDirectoryW(dirs.state.c_str(), nullptr);
    const std::wstring file = dirs.state + L"\\pet-camera.json";
    {
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        const char* body = R"({"sharingEnabled":true,"privacyMode":false,"rollEnabled":false})";
        DWORD w = 0;
        WriteFile(h, body, static_cast<DWORD>(std::strlen(body)), &w, nullptr);
        CloseHandle(h);
    }
    // Held by another program while the host starts: privacy is on, and roll changes that need a write are
    // refused (pet-camera.md §5).
    HANDLE hold = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    KV_REQUIRE(hold != INVALID_HANDLE_VALUE);
    RollHost host;
    std::string err;
    const bool started = host.start(dirs, &err);
    KV_CHECK_MSG(started, err);
    if (!started) {
        CloseHandle(hold);
        return;
    }
    HostStreamServer& srv = host.ctl.server();
    KV_CHECK(srv.petSettings().privacyMode);
    KV_CHECK(!srv.setRollEnabled(true) && !srv.setMotionSettings(true, 5) && !srv.setRollRetention(10, 10000000000ULL));
    FakePhone a(3);
    RollPhone ra(a);
    const bool joined = joinDevice(srv, a, PetRole::Owner, ra);
    KV_CHECK(joined);
    if (joined) {
        KV_CHECK(ra.errorOf(ra.cmd(pet_action::kSetRoll, true)) == pet_error::kInternal);
        KV_CHECK(ra.errorOf(ra.cmd(pet_action::kSetMotion, true)) == pet_error::kInternal);
        KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) {
            return p.privacyMode && !p.rollEnabled && p.rollBlockedReason == std::string(roll_blocked::kPrivacyMode);
        }));
    }
    KV_CHECK(!srv.petSettings().rollEnabled);
    CloseHandle(hold);
    // Once it can be read again, the change goes through and is saved.
    KV_CHECK(waitUntil([&] { return srv.setRollEnabled(true); }, 10000));
    KV_CHECK(waitUntil([&] {
        PetSettings s;
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        char buf[4096];
        DWORD got = 0;
        ReadFile(h, buf, sizeof(buf), &got, nullptr);
        CloseHandle(h);
        return fromJson(std::string(buf, got), s) && s.rollEnabled;
    }, 10000));
}

// ============================================================================================================
// W7 fix round 2
// ============================================================================================================

namespace {

std::string readWhole(const std::wstring& path) {
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return out;
}

} // namespace

// On a first run (no pet-camera.json) the limits aren't known, and nothing is aged out. Turning the roll on at
// the PC, beside the limits it shows, confirms them: they are written, and retention runs with them at once,
// without a restart. The old code never applied them until a restart read them back.
KV_TEST(roll_first_run_turning_the_roll_on_confirms_the_limits_and_retention_runs_at_once) {
    Dirs dirs(L"s3-first");
    const std::vector<std::string> ids = seedRoll(dirs, {40 * kDayMs, kMinMs}, true, 90);
    KV_REQUIRE(ids.size() == 2);
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    KV_CHECK(!host.ctl.rollLimitsKnown() && !host.ctl.rollStatus().limitsKnown);
    KV_REQUIRE(host.ctl.server().setRollEnabled(true));
    KV_CHECK(host.ctl.rollLimitsKnown());
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().limitsApplied; }, 20000));
    // The 30 days shown (the defaults) are the owner's now: the 40-day-old clip goes, the new one stays.
    KV_CHECK(waitUntil([&] { return countFiles(dirs.roll, L".mp4") == 1; }, 10000));
    KV_CHECK(host.ctl.rollStore()->findClip(ids[1]).has_value());
    const std::string saved = readWhole(dirs.state + L"\\pet-camera.json");
    KV_CHECK_MSG(saved.find("\"rollKeepDays\":30") != std::string::npos, saved);
    const HostController::RollStorageSnapshot snap = host.ctl.rollStorageSnapshot();
    KV_CHECK(snap.available && snap.limitsKnown && snap.keepDays == 30);
}

// A W6-era pet-camera.json (no roll keys) doesn't make the limits known: an owner device turning the roll on
// prepares the folder but ages nothing out, until the limits are confirmed on the PC. The old code took the W6 file
// as holding the owner's limits and deleted the 40-day-old clip with the default 30 days.
KV_TEST(roll_a_w6_era_file_leaves_the_limits_unknown_until_they_are_confirmed) {
    Dirs dirs(L"s3-w6");
    const std::vector<std::string> ids = seedRoll(dirs, {40 * kDayMs}, true, 90);
    KV_REQUIRE(ids.size() == 1);
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"sharingSchedule":[]})"));
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(enableRoll(ra)); // the phone's switch: the folder is prepared, the limits stay unknown
    KV_CHECK(!host.ctl.rollLimitsKnown());
    sleepMs(500); // several recorder turns: nothing may be aged out meanwhile
    KV_CHECK(countFiles(dirs.roll, L".mp4") == 1 && !host.ctl.rollStatus().limitsApplied);
    std::string cerr;
    KV_CHECK_MSG(host.ctl.confirmRollLimits(&cerr), cerr);
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().limitsApplied; }, 20000));
    KV_CHECK(waitUntil([&] { return countFiles(dirs.roll, L".mp4") == 0; }, 10000));
}

// With the roll off, a folder that is already a prepared roll is kept tidy: its start-up pass runs (a leftover
// .tmp goes), and retention with the owner's limits; it stays browsable. Only creating or preparing a
// folder is ruled out. The old code left it alone entirely while the roll was off.
KV_TEST(roll_off_an_already_prepared_folder_is_kept_tidy_and_browsable) {
    Dirs dirs(L"s2-tidy");
    const std::vector<std::string> ids = seedRoll(dirs, {40 * kDayMs, kMinMs}, true, 90);
    KV_REQUIRE(ids.size() == 2);
    const std::wstring tmp = dirs.roll + L"\\" + std::wstring(ids[1].begin(), ids[1].begin() + 10) + L"\\leftover.json.tmp";
    {
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        CloseHandle(h);
    }
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":false,"rollKeepDays":30})"));
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().limitsApplied; }, 20000));
    KV_CHECK(waitUntil([&] { return !exists(tmp); }, 10000));
    KV_CHECK(waitUntil([&] { return countFiles(dirs.roll, L".mp4") == 1; }, 10000));
    const RollRecorderStatus st = host.ctl.rollStatus();
    KV_CHECK(!st.enabled && !st.prepared && st.block == RollBlock::RollDisabled);
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(host.ctl.server(), a, PetRole::Owner, ra));
    KV_CHECK(clipOf(ra, ids[1]).has_value() && !clipOf(ra, ids[0]).has_value());
}

// While the folder is being prepared, 0x0B says roll_preparing (with its §9 sentence, right after roll_disabled
// in the §5.1 order) and start_clip is refused with it. The recorder's thread is held, so preparing lasts.
KV_TEST(roll_preparing_is_reported_and_refuses_start_clip_until_the_folder_is_ready) {
    Dirs dirs(L"s5-preparing");
    ThreadHold th;
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) { o.roll.threadHook = th.hook(); }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(th.engage());
    KV_CHECK(ra.okOf(ra.cmd(pet_action::kSetRoll, true)));
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) {
        return p.rollEnabled && p.rollBlockedReason == std::string(roll_blocked::kRollPreparing) &&
               p.rollBlockedText == std::string(text::kRollPreparing);
    }));
    KV_CHECK(host.ctl.rollStatus().preparing && host.ctl.rollStatus().block == RollBlock::RollPreparing);
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kStartClip)) == pet_error::kRollPreparing);
    th.release();
    KV_CHECK(petSays(ra, [](const PetCameraStateMessage& p) { return p.rollEnabled && !p.rollBlockedReason; }, 20000));
    KV_CHECK(!host.ctl.rollStatus().preparing && host.ctl.rollStatus().prepared);
}

// In a test run, a frame that isn't from the synthetic source stops the roll with the reason, and the clip in
// progress is thrown away: nothing is written. The test roll the host prepared carries its marker.
KV_TEST(roll_a_test_run_refuses_a_frame_that_isnt_synthetic_and_keeps_nothing) {
    Dirs dirs(L"p2-real");
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true})"));
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [](HostController::Options& o) { o.test_run = true; }));
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().armed; }, 20000));
    KV_CHECK(exists(dirs.roll + L"\\" + roll::kTestRollMarker));
    KV_REQUIRE(ringFilled(host));
    KV_REQUIRE(host.ctl.startManualClip().ok);
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStatus().recording; }, 10000));
    // One frame of the synthetic source, as a real one would be marked.
    std::vector<uint8_t> buf(1920 * 1080 * 4);
    size_t req = 0;
    kv_mapped_frame_t m{};
    KV_REQUIRE(waitUntil([&] { return kv_engine_copy_latest_frame(host.eng, 0, buf.data(), buf.size(), &req, &m); }, 5000));
    m.is_synthetic = false;
    host.ctl.submitFrame(buf.data(), m, static_cast<int>(m.mode), 0, false);
    KV_CHECK(!host.ctl.rollError().empty());
    KV_CHECK(waitUntil([&] { return host.ctl.rollStatus().clipsFailed >= 1 && !host.ctl.rollStatus().recording; }, 20000));
    const RollRecorderStatus st = host.ctl.rollStatus();
    KV_CHECK(st.folderUnavailable && st.clipsFinalized == 0 && !host.ctl.turnOffSkippedForRoll());
    KV_CHECK(!host.ctl.startManualClip().ok);
    KV_CHECK(waitUntil([&] { return countFiles(dirs.roll, L".mp4") == 0; }, 10000));
    KV_CHECK(countFiles(dirs.roll, L".jpg") == 0);
}

// A test run accepts only a new or empty folder, or a test roll. A temp folder made to look like a real roll
// (a prepared folder with a clip, no test marker) is refused and left alone.
KV_TEST(roll_a_test_run_refuses_a_folder_that_looks_like_a_real_roll) {
    Dirs dirs(L"p4-fake");
    KV_REQUIRE(seedRoll(dirs, {kMinMs}, true, 90, /*test_marker=*/false).size() == 1);
    const auto before = snapshotTree(dirs.roll);
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [](HostController::Options& o) { o.test_run = true; }));
    KV_CHECK(!host.ctl.rollAvailable());
    KV_CHECK_MSG(host.ctl.rollError().find("test run") != std::string::npos, host.ctl.rollError());
    sleepMs(300);
    KV_CHECK(snapshotTree(dirs.roll) == before);
}

// A phone owner's delete of a clip the PC's player holds tells the PC, waits for the unpin and then deletes;
// if the PC still holds it after 2 s, the delete is answered internal and nothing is touched.
KV_TEST(roll_a_phone_delete_of_a_clip_the_pc_holds_waits_for_the_pc) {
    Dirs dirs(L"m2-pin");
    const std::vector<std::string> ids = seedRoll(dirs, {2 * kMinMs, kMinMs}, true, 90);
    KV_REQUIRE(ids.size() == 2);
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true,"rollKeepDays":90})"));
    RollHost host;
    std::atomic<int> told{0};
    std::mutex told_m;
    std::string told_id;
    host.ctl.onRollClipDeletedForPc = [&](const std::string& id) {
        std::lock_guard<std::mutex> lk(told_m);
        told_id = id;
        ++told;
    };
    std::string err;
    KV_REQUIRE(host.start(dirs, &err));
    FakePhone a(3);
    RollPhone ra(a);
    KV_REQUIRE(joinDevice(host.ctl.server(), a, PetRole::Owner, ra));
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStore() != nullptr; }, 10000));
    KV_CHECK(host.ctl.pinRollClipForPc("2020-01-01/00-00-00_motion") == 0); // no such clip
    // The PC lets go when told: the delete goes through after the unpin.
    const uint64_t tok = host.ctl.pinRollClipForPc(ids[0]);
    KV_REQUIRE(tok != 0);
    const uint64_t del = ra.cmd(pet_action::kDeleteClip, std::nullopt, ids[0]);
    KV_REQUIRE(waitUntil([&] { return told.load() == 1; }, 10000));
    {
        std::lock_guard<std::mutex> lk(told_m);
        KV_CHECK(told_id == ids[0]);
    }
    KV_CHECK(exists(clipPath(dirs, ids[0], L".mp4")) && !ra.log().answered(del)); // waiting for the PC
    host.ctl.unpinRollClipForPc(tok);
    KV_CHECK(ra.okOf(del));
    KV_CHECK(!exists(clipPath(dirs, ids[0], L".mp4")));
    // The PC holds on: internal after about 2 s, and every file stays.
    const uint64_t tok2 = host.ctl.pinRollClipForPc(ids[1]);
    KV_REQUIRE(tok2 != 0);
    const auto t0 = std::chrono::steady_clock::now();
    KV_CHECK(ra.errorOf(ra.cmd(pet_action::kDeleteClip, std::nullopt, ids[1])) == pet_error::kInternal);
    KV_CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(1900));
    KV_CHECK(exists(clipPath(dirs, ids[1], L".mp4")) && exists(clipPath(dirs, ids[1], L".json")));
    KV_CHECK(host.ctl.rollStore()->findClip(ids[1]).has_value());
    host.ctl.unpinRollClipForPc(tok2);
}

// An owner's delete is checked again once the wait for the PC's player is over, as a star is
// (v3 §3.1: only an owner deletes). Held at that point, a device made a viewer meanwhile is answered
// not_allowed_for_role, and one revoked meanwhile gets nothing (its session is closed); either way every file of
// the clip stays. The old code deleted the clip in both cases.
KV_TEST(roll_an_owner_delete_is_checked_again_after_the_wait_for_the_pc) {
    Dirs dirs(L"delete-recheck");
    const std::vector<std::string> ids = seedRoll(dirs, {3 * kMinMs, 2 * kMinMs}, true, 90);
    KV_REQUIRE(ids.size() == 2);
    KV_REQUIRE(writePetJson(dirs, R"({"sharingEnabled":false,"privacyMode":false,"rollEnabled":true,"rollKeepDays":90})"));
    HookGate gate;
    gate.where = "delete_after_wait";
    std::atomic<int> deletes_done{0};
    RollHost host;
    std::string err;
    KV_REQUIRE(host.start(dirs, &err, true, [&](HostController::Options& o) {
        o.server.roll_test_hook = [&](const char* w) {
            gate.hit(w);
            if (std::string(w) == "delete_done") ++deletes_done;
        };
    }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3), b(3), c(3);
    RollPhone ra(a), rb(b), rc(c);
    KV_REQUIRE(joinDevice(srv, a, PetRole::Owner, ra));
    KV_REQUIRE(joinDevice(srv, b, PetRole::Owner, rb));
    KV_REQUIRE(joinDevice(srv, c, PetRole::Owner, rc)); // stays an owner throughout
    KV_REQUIRE(waitUntil([&] { return host.ctl.rollStore() != nullptr; }, 10000));
    auto untouched = [&](const std::string& id) {
        return exists(clipPath(dirs, id, L".mp4")) && exists(clipPath(dirs, id, L".json")) &&
               host.ctl.rollStore()->findClip(id).has_value();
    };
    // Made a viewer while its delete waited.
    gate.armed = true;
    const uint64_t d1 = ra.cmd(pet_action::kDeleteClip, std::nullopt, ids[0]);
    KV_REQUIRE(waitUntil([&] { return gate.at.load(); }, 10000));
    std::string rerr;
    KV_REQUIRE(srv.setDeviceRole(idOf(a), PetRole::Viewer, &rerr));
    gate.open = true;
    KV_CHECK_MSG(ra.errorOf(d1) == pet_error::kNotAllowedForRole, ra.errorOf(d1));
    KV_CHECK(untouched(ids[0]));
    // Revoked while its delete waited.
    KV_REQUIRE(waitUntil([&] { return !gate.at.load() && deletes_done.load() == 1; }, 10000)); // the first has decided
    gate.open = false;
    gate.armed = true;
    const int done_before = deletes_done.load();
    rb.cmd(pet_action::kDeleteClip, std::nullopt, ids[1]);
    KV_REQUIRE(waitUntil([&] { return gate.at.load(); }, 10000));
    KV_REQUIRE(srv.revokeDevice(idOf(b), &rerr));
    gate.open = true;
    KV_REQUIRE(waitUntil([&] { return deletes_done.load() > done_before; }, 10000)); // the held delete has decided
    KV_CHECK(untouched(ids[1]));
    // An owner's delete still works.
    KV_CHECK(rc.okOf(rc.cmd(pet_action::kDeleteClip, std::nullopt, ids[1])));
    KV_CHECK(!exists(clipPath(dirs, ids[1], L".mp4")));
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int rc = kvtest::runAll(argc, argv);
    CoUninitialize();
    timeEndPeriod(1);
    return rc;
}
