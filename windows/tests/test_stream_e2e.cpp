// End-to-end loopback tests: synthetic engine -> HostController (NV12, hardware H.264, AVCC) ->
// HostStreamServer (pinned mutual TLS 1.3) -> FakePhone (Swift client logic) -> H.264 decode -> barcode.
// Loopback only: listeners bind 127.0.0.1, nothing is advertised. This is NOT the iOS app.
#include "kv_desk.hpp"
#include "kv_test.hpp"

#include "fake_phone.hpp"
#include "host_controller.hpp"
#include "host_power.hpp"
#include "kinect_core_c.h"
#include "pairing_payload.hpp"
#include "state_files.hpp"
#include "test_h264_decoder.hpp"
#include "tls_conn.hpp" // TcpListener, tcpConnect
#include "viewer_model.hpp"

#include <cstdlib>
#include <objbase.h>
#include <openssl/x509.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace kvhost;
using kvtest::FakePhone;

namespace {

std::wstring freshStateDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"kv-e2e-" + tag + L"-" + std::to_wstring(GetCurrentProcessId()) + L"\\.host-state";
    for (const wchar_t* f : {L"\\pairing.json", L"\\devices.json", L"\\pet-camera.json", L"\\.kinect-viewer-host-state",
                             L"\\privacy-on.pending", L"\\connection-log.jsonl"}) {
        DeleteFileW((d + f).c_str());
    }
    // Every kept .bad copy (keepBadCopy numbers them) and a .bad.tmp left by a copy that failed.
    for (const wchar_t* f : {L"\\pairing.json", L"\\devices.json", L"\\pet-camera.json"}) {
        DeleteFileW((d + f + L".bad").c_str());
        DeleteFileW((d + f + L".bad.tmp").c_str());
        for (int i = 1; i < kBadCopiesKept; ++i) DeleteFileW((d + f + L".bad." + std::to_wstring(i)).c_str());
    }
    return d;
}

uint64_t nowNs() { return kv_monotonic_ns(); }

// The connection log (v3 §8.6) has a line that `match` takes. The server records its queued lines within about
// 100 ms, so this waits for one.
bool logHasLine(const HostStreamServer& srv, const std::function<bool(const outside::LogLine&)>& match, int ms = 4000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        for (const auto& l : srv.connectionLog(30)) {
            if (match(l)) return true;
        }
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
bool logHasLine(const HostStreamServer& srv, const char* event, const char* path, const std::string& detail_part) {
    return logHasLine(srv, [&](const outside::LogLine& l) {
        return l.event == event && l.path == path && l.detail.find(detail_part) != std::string::npos;
    });
}

// Synthetic engine + feeder thread, like the viewer's frame loop.
struct SyntheticHost {
    kv_engine_t* eng = nullptr;
    HostController ctl;
    std::thread feeder;
    std::atomic<bool> stop{false};
    std::atomic<int> mode{KV_MODE_IR};
    std::atomic<int> sbs{KV_SBS_IR_DEPTH};
    std::atomic<int> requested_mode{-1};
    std::atomic<int> requested_sbs{-1};

    bool start(const std::wstring& dir, std::string* err) {
        eng = kv_engine_create(nullptr);
        kv_display_params_t p;
        kv_default_display_params(&p);
        p.clock_overlay = true;
        char e[256] = {0};
        if (!kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, true, e, sizeof(e))) {
            *err = e;
            return false;
        }
        HostController::Options o;
        o.server.state_dir = dir;
        o.server.loopback_only = true;
        o.server.advertise = false;
        ctl.onClientRequestedMode = [this](int m, int s) {
            requested_mode = m;
            requested_sbs = s;
            setMode(m, s);
        };
        if (!ctl.start(o, err)) return false;
        feeder = std::thread([this] {
            std::vector<uint8_t> buf(1920 * 1080 * 4);
            uint64_t last = 0;
            while (!stop) {
                size_t req = 0;
                kv_mapped_frame_t m{};
                if (kv_engine_copy_latest_frame(eng, last, buf.data(), buf.size(), &req, &m)) {
                    last = m.frame_seq;
                    ctl.submitFrame(buf.data(), m, static_cast<int>(m.mode), sbs.load(), true);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        return true;
    }

    void setMode(int m, int s) {
        kv_display_params_t p;
        kv_engine_get_display_params(eng, &p);
        p.mode = static_cast<kv_view_mode_t>(m);
        p.sbs_layout = static_cast<kv_sbs_layout_t>(s);
        kv_engine_set_display_params(eng, &p);
        mode = m;
        sbs = s;
        ctl.notifyModeChanged(m, s, 0, 0);
    }

    ~SyntheticHost() {
        stop = true;
        if (feeder.joinable()) feeder.join();
        ctl.stop();
        if (eng) kv_engine_destroy(eng);
    }
};

// The synthetic engine behind the protocol v2 power model, wired as the viewer wires it: phone
// requests (and "turn off on disconnect") go into one FIFO that a single worker drains, so they run
// in arrival order; each confirmed state goes to HostController, which tells a v2 phone.
struct PoweredHost final : kvhost::SensorBackend {
    kv_engine_t* eng = nullptr;
    HostController ctl;
    std::unique_ptr<HostPowerModel> power;
    std::thread feeder, worker;
    std::atomic<bool> stop{false};
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<bool> q;
    std::atomic<int> disconnects{0};
    std::atomic<int> commands{0};
    std::atomic<int> keyframe_commands{0};
    std::atomic<int> processed{0};
    std::atomic<int> orbits{0};
    std::atomic<int> mode_requests{0};
    std::mutex cm; // what came through, for the checks
    std::vector<RemoteControlCommandMessage> got_commands;
    std::vector<Orbit3DGestureMessage> got_orbits;
    // The host's own clock (kv_monotonic_ns) for its power-off promise: the first "off" request reaching the host,
    // and the power model reaching Off after it. 0 until each happens.
    std::atomic<uint64_t> off_requested_ns{0}, off_reached_ns{0};
    bool offReached() const { return off_reached_ns.load() != 0; }
    // Request to Off on the host, in ms; -1 until both have happened.
    int hostOffMs() const {
        const uint64_t a = off_requested_ns.load(), b = off_reached_ns.load();
        return a != 0 && b >= a ? static_cast<int>((b - a) / 1000000ULL) : -1;
    }

    std::optional<std::string> externalLockHolder() override { return std::nullopt; } // synthetic: no lock
    OpenResult open() override {
        kv_display_params_t p;
        kv_default_display_params(&p);
        p.clock_overlay = true;
        char e[256] = {0};
        OpenResult r;
        r.ok = kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, false, e, sizeof(e));
        r.error = e;
        return r;
    }
    void close() override { kv_engine_stop(eng); }
    bool isOpen() const override { return kv_engine_is_running(eng); }

    void post(bool on) {
        std::lock_guard<std::mutex> lk(qm);
        q.push_back(on);
        qcv.notify_all();
    }
    // Waits until every posted request has been handled.
    bool drain(int timeout_ms = 3000) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < until) {
            {
                std::lock_guard<std::mutex> lk(qm);
                if (q.empty() && !busy) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool busy = false; // guarded by qm

    std::atomic<bool> frozen{false}; // the view is frozen: the feeder submits nothing (as the viewer)

    bool start(const std::wstring& dir, std::string* err, int ping_timeout_ms = 5000,
               const std::function<void(HostController::Options&)>& tweak = {}) {
        eng = kv_engine_create(nullptr);
        power = std::make_unique<HostPowerModel>(*this, [this](const HostPowerModel::Snapshot& s) {
            uint64_t none = 0;
            if (s.state == SensorPowerState::Off && off_requested_ns.load() != 0) off_reached_ns.compare_exchange_strong(none, nowNs());
            ctl.setPowerSnapshot(s);
        });
        HostController::Options o;
        o.server.state_dir = dir;
        o.server.loopback_only = true;
        o.server.advertise = false;
        o.server.client_ping_timeout_ms = ping_timeout_ms;
        if (tweak) tweak(o);
        ctl.onClientSensorPowerRequest = [this](const SensorPowerControlMessage& r) {
            uint64_t none = 0;
            if (!r.turnOn) off_requested_ns.compare_exchange_strong(none, nowNs());
            post(r.turnOn);
        };
        ctl.onPhoneDisconnected = [this](bool turn_off) {
            ++disconnects;
            if (turn_off) post(false);
        };
        ctl.onClientRemoteCommand = [this](const RemoteControlCommandMessage& c) {
            ++commands;
            if (c.action == RemoteControlAction::RequestKeyframe) ++keyframe_commands;
            {
                std::lock_guard<std::mutex> lk(cm);
                got_commands.push_back(c);
            }
            ctl.pushHostState(); // a 0x07 after every command
        };
        ctl.onClientRequestedMode = [this](int, int) { ++mode_requests; };
        ctl.onClientOrbit3DGesture = [this](const Orbit3DGestureMessage& g) {
            ++orbits;
            std::lock_guard<std::mutex> lk(cm);
            got_orbits.push_back(g);
        };
        if (!ctl.start(o, err)) return false;
        HostViewState v;
        v.canRecordCurrentMode = false;
        v.recordingDisabledReason = "test host";
        ctl.setViewState(v);
        worker = std::thread([this] {
            while (true) {
                bool on = false;
                {
                    std::unique_lock<std::mutex> lk(qm);
                    qcv.wait(lk, [&] { return stop.load() || !q.empty(); });
                    if (stop && q.empty()) break;
                    on = q.front();
                    q.pop_front();
                    busy = true;
                }
                power->request(on);
                ++processed;
                std::lock_guard<std::mutex> lk(qm);
                busy = false;
            }
        });
        post(true);
        if (!drain()) return false;
        feeder = std::thread([this] {
            std::vector<uint8_t> buf(1920 * 1080 * 4);
            uint64_t last = 0;
            while (!stop) {
                size_t req = 0;
                kv_mapped_frame_t m{};
                if (!frozen && kv_engine_copy_latest_frame(eng, last, buf.data(), buf.size(), &req, &m)) {
                    last = m.frame_seq;
                    ctl.submitFrame(buf.data(), m, static_cast<int>(m.mode), 0, true);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        return power->snapshot().state == SensorPowerState::On;
    }

    ~PoweredHost() override {
        stop = true;
        qcv.notify_all();
        if (feeder.joinable()) feeder.join();
        if (worker.joinable()) worker.join();
        ctl.stop();
        if (eng) {
            kv_engine_stop(eng);
            kv_engine_destroy(eng);
        }
    }
};

// Pairs a phone with the QR token and connects its stream; false if either fails. These tests are
// about the owner's own phone, so it pairs as an owner unless a role is given (the PC's window defaults to
// viewer, protocol v3 D1).
bool pairAndConnect(HostStreamServer& srv, FakePhone& phone, FakePhone::PairResult& pr, PetRole role = PetRole::Owner) {
    std::string err;
    ActivePairingOffer offer;
    if (!srv.beginPairingWindow(120, offer, &err, role)) return false;
    pr = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    if (!pr.ok) return false;
    return phone.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &err);
}

int countIn(const std::vector<int>& v, int from_ms, int to_ms) {
    return static_cast<int>(std::count_if(v.begin(), v.end(), [&](int t) { return t >= from_ms && t < to_ms; }));
}

bool seqNeverGoesDown(const std::vector<HostStateSyncMessage>& s) {
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i].stateSeq < s[i - 1].stateSeq) return false;
    }
    return true;
}

struct StreamStats {
    bool got_hello = false;
    HelloServerMessage hello;
    int configs = 0;
    StreamConfigMessage last_cfg;
    int aus = 0;
    int keyframes = 0;
    bool first_au_key = false;
    int decoded = 0;
    int barcode_ok = 0;
    int pongs = 0;
    std::vector<double> latency_ms; // host capture -> decoded on the "phone" (same clock)
    std::vector<double> rtt_ms;
    std::vector<HelloServerMessage> hellos;
    // Protocol v2
    std::vector<HostStateSyncMessage> states;
    std::vector<int> state_ms;      // when each 0x07 arrived (ms from the start)
    std::vector<int> au_ms;         // when each access unit arrived
    std::vector<int> pong_ms;
    int first_ping_ms = -1;
    bool closed_by_host = false;
};

// Mirrors PocketStreamClientCore: hello, config (format), AUs (epoch must match), pings every 250 ms
// (each one saying v2 from a v2 phone; none at all with send_pings false).
// hello_seen: this connection already got its HelloServer in an earlier run, so pings start at once.
// done/max_ms (runStreamUntil): after duration_ms, keeps reading until done(stats) holds or max_ms has passed.
StreamStats runStream(FakePhone& phone, int duration_ms, const std::function<void(int)>& at_ms = {}, bool send_pings = true,
                      bool hello_seen = false, const std::function<bool(const StreamStats&)>& done = {}, int max_ms = 0) {
    StreamStats st;
    bool hello = hello_seen;
    kvtest::TestH264Decoder dec;
    dec.open();
    uint32_t epoch = 0;
    bool have_cfg = false;
    const uint64_t start = nowNs();
    uint64_t next_ping = start;
    int last_cb = -1;
    while (true) {
        const uint64_t now = nowNs();
        const int elapsed = static_cast<int>((now - start) / 1000000ULL);
        if (elapsed >= duration_ms && (!done || elapsed >= max_ms || done(st))) break;
        if (at_ms && elapsed / 500 != last_cb) {
            last_cb = elapsed / 500;
            at_ms(elapsed);
        }
        if (send_pings && (st.got_hello || hello) && now >= next_ping) {
            PingMessage ping;
            ping.clientSendNs = now;
            ping.lastDecodedSeq = static_cast<uint64_t>(st.decoded);
            ping.lastBarcodeLatencyUs = st.latency_ms.empty() ? 0u : static_cast<uint32_t>(st.latency_ms.back() * 1000.0);
            ping.protocolVersion = phone.pingVersion();
            phone.sendFrame(MsgType::Ping, toJson(ping));
            if (st.first_ping_ms < 0) st.first_ping_ms = elapsed;
            next_ping = now + 250000000ULL;
        }
        MsgType t;
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        if (!phone.readFrame(t, flags, payload, 300)) {
            if (phone.violated()) break; // a v1 phone drops the connection here
            if (phone.lastReadWasClose()) {
                st.closed_by_host = true;
                break;
            }
            if (!st.got_hello && !hello) break; // rejected or closed
            continue;
        }
        if (t == MsgType::HostStateSync) {
            HostStateSyncMessage s;
            if (fromJson(payload, s)) { // the phone drops one it can't decode, without a word
                st.states.push_back(s);
                st.state_ms.push_back(static_cast<int>((nowNs() - start) / 1000000ULL));
            }
            continue;
        }
        if (t == MsgType::HelloServer) {
            HelloServerMessage h;
            if (fromJson(payload, h)) {
                st.got_hello = true;
                st.hello = h;
                st.hellos.push_back(h);
            }
        } else if (t == MsgType::StreamConfig) {
            StreamConfigMessage c;
            if (fromJson(payload, c) && c.parameterSets.size() >= 2) {
                ++st.configs;
                st.last_cfg = c;
                epoch = c.streamEpoch;
                have_cfg = true;
            }
        } else if (t == MsgType::VideoAccessUnit) {
            VideoAccessUnitHeader h;
            const uint8_t* avcc = nullptr;
            size_t n = 0;
            if (!VideoAccessUnitHeader::decode(payload.data(), payload.size(), h, &avcc, &n)) continue;
            if (!have_cfg || h.streamEpoch != epoch) continue; // same rule as the phone
            ++st.aus;
            st.au_ms.push_back(static_cast<int>((nowNs() - start) / 1000000ULL));
            if (st.aus == 1) st.first_au_key = h.isKeyframe();
            if (h.isKeyframe()) ++st.keyframes;
            const auto annexb = kvtest::avccToAnnexB(avcc, n, h.isKeyframe() ? st.last_cfg.parameterSets
                                                                             : std::vector<std::vector<uint8_t>>{});
            if (dec.decode(annexb.data(), annexb.size())) {
                ++st.decoded;
                const uint64_t done = nowNs();
                uint32_t seq24 = 0, ts = 0;
                if (h.hasClockOverlay() &&
                    kv_decode_clock_barcode(dec.luma().data(), dec.width(), dec.height(), dec.width(), 1, &seq24, &ts)) {
                    const uint32_t now_us = static_cast<uint32_t>((done / 1000ULL) & 0xFFFFFFFFULL);
                    const uint32_t diff = now_us - ts;
                    if (diff < 2000000u && seq24 == static_cast<uint32_t>(h.frameSeq & 0xFFFFFF)) {
                        ++st.barcode_ok;
                        st.latency_ms.push_back(diff / 1000.0);
                    }
                }
            }
        } else if (t == MsgType::Pong) {
            PongMessage p;
            if (fromJson(payload, p)) {
                ++st.pongs;
                st.pong_ms.push_back(static_cast<int>((nowNs() - start) / 1000000ULL));
                st.rtt_ms.push_back(static_cast<double>(nowNs() - p.clientSendNs - (p.serverSendNs - p.serverRecvNs)) / 1e6);
            }
        }
    }
    return st;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

} // namespace

// A stream that must carry at least so many AUs: it runs the scripted duration_ms (its at_ms steps happen as before),
// then until the count is reached, up to kStreamDeadlineMs. A fixed duration raced the encoder: under GPU load the
// hardware encoder can take most of a second to open, so a count that is reached in time on a quiet PC fell short.
// What is checked is unchanged; only when the check is made.
constexpr int kStreamDeadlineMs = 15000;
StreamStats runStreamUntil(FakePhone& phone, int duration_ms, const std::function<bool(const StreamStats&)>& done,
                           const std::function<void(int)>& at_ms = {}) {
    return runStream(phone, duration_ms, at_ms, true, false, done, kStreamDeadlineMs);
}
std::function<bool(const StreamStats&)> helloAndAus(int n) {
    return [n](const StreamStats& st) { return st.got_hello && st.aus >= n; };
}
// The long deadline above is for the encoder's slow open only. The frame rate is still checked: the counts
// must arrive within the original windows, measured from the first AU (au_ms.front()), not from the start.
int ausWithin(const StreamStats& st, int window_ms) {
    if (st.au_ms.empty()) return 0;
    int n = 0;
    for (int ms : st.au_ms) n += ms - st.au_ms.front() <= window_ms ? 1 : 0;
    return n;
}
int pongsWithin(const StreamStats& st, int window_ms) {
    if (st.au_ms.empty()) return 0;
    int n = 0;
    for (int ms : st.pong_ms) n += ms >= st.au_ms.front() && ms - st.au_ms.front() <= window_ms ? 1 : 0;
    return n;
}

KV_TEST(unpaired_host_rejects_any_stream_client_inside_the_handshake) {
    SyntheticHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"unpaired"), &err));
    KV_CHECK(!host.ctl.server().isPhonePaired());
    FakePhone phone;
    std::string cerr;
    // The client accepts any server here, so only the host can refuse: it has no pinned phone.
    const bool tls = phone.connectStream("127.0.0.1", host.ctl.server().streamPort(), std::nullopt, &cerr);
    StreamStats st;
    if (tls) st = runStream(phone, 1500);
    KV_CHECK(!st.got_hello);
    KV_CHECK(host.ctl.server().rejectedHandshakeCount() >= 1);
    std::printf("  client tls=%d err=\"%s\" host_rejected=%llu\n", tls ? 1 : 0, cerr.c_str(),
                static_cast<unsigned long long>(host.ctl.server().rejectedHandshakeCount()));
    // v3 §8.6: the refusal on the LAN stream listener is in the connection log, as on the outside listener.
    KV_CHECK(logHasLine(host.ctl.server(), "refused_unknown_cert", "home", "a certificate that matches no device"));
}

KV_TEST(qr_pairing_then_pinned_stream_with_barcode_latency_and_mode_switch) {
    SyntheticHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"qr"), &err));
    HostStreamServer& srv = host.ctl.server();

    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    QRPairingPayload qr;
    KV_REQUIRE(QRPairingPayload::parse(offer.qrURI, qr));
    KV_CHECK(qr.hostIP == "127.0.0.1" && qr.pairPort == offer.port && qr.serviceId == srv.serviceId());

    FakePhone phone;
    const auto pr = phone.pair(qr.hostIP, qr.pairPort, qr.serverCertSHA256, PairingMethod::QrToken, qr.oneTimeToken);
    KV_REQUIRE(pr.ok);
    KV_CHECK(pr.serviceId == srv.serviceId());
    KV_CHECK(pr.streamPort == srv.streamPort());
    KV_CHECK(srv.isPhonePaired());
    KV_CHECK(!srv.currentPairingOffer().has_value()); // window closes on success
    KV_CHECK(srv.pinnedClientFingerprintShort() == hexLower(phone.identity().certificateSHA256().data(), 6));

    std::string cerr;
    KV_REQUIRE(phone.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &cerr));
    bool switched = false;
    const StreamStats st = runStreamUntil(phone, 4000, helloAndAus(100), [&](int ms) {
        if (ms >= 2000 && !switched) {
            switched = true;
            SelectModeMessage sel;
            sel.modeRaw = KV_MODE_DEPTH;
            sel.sbsLayoutRaw = 0;
            phone.sendFrame(MsgType::SelectModeReq, toJson(sel));
        }
    });
    std::printf("  encoder: %s\n", host.ctl.encoderName().c_str());
    std::printf("  hello=%d %dx%d mode=%d configs=%d aus=%d keyframes=%d decoded=%d barcode_ok=%d pongs=%d\n",
                st.got_hello ? 1 : 0, st.hello.width, st.hello.height, st.hello.modeRaw, st.configs, st.aus, st.keyframes,
                st.decoded, st.barcode_ok, st.pongs);
    std::printf("  capture->decoded latency (loopback, same clock): p50=%.1f ms p95=%.1f ms max=%.1f ms; rtt p50=%.2f ms\n",
                pct(st.latency_ms, 0.5), pct(st.latency_ms, 0.95), pct(st.latency_ms, 1.0), pct(st.rtt_ms, 0.5));
    KV_CHECK(st.got_hello);
    // The host says v3 (re-scoped from 2 in W6, v3 §10); this phone is a v1 phone, never reads the
    // version, and still streams.
    KV_CHECK(st.hellos.front().protocolVersion == 3 && st.hellos.front().codecRaw == 0 && st.hellos.front().allowPhoneModeSwitch);
    KV_CHECK(!phone.violated() && st.states.empty());
    KV_CHECK(st.configs >= 2); // initial + after the mode switch
    KV_CHECK(st.first_au_key);
    KV_CHECK_MSG(ausWithin(st, 4000) >= 100, std::to_string(ausWithin(st, 4000)));
    KV_CHECK(st.decoded >= st.aus - 2);
    KV_CHECK(st.barcode_ok >= st.decoded * 9 / 10);
    KV_CHECK(pct(st.latency_ms, 0.95) < 150.0);
    KV_CHECK_MSG(pongsWithin(st, 4000) >= 8, std::to_string(pongsWithin(st, 4000)));
    KV_CHECK(host.requested_mode.load() == KV_MODE_DEPTH);
    KV_CHECK(st.last_cfg.modeRaw == KV_MODE_DEPTH && st.last_cfg.width == 1024 && st.last_cfg.height == 848);
    bool saw_depth_hello = false;
    for (const auto& h : st.hellos) saw_depth_hello = saw_depth_hello || h.modeRaw == KV_MODE_DEPTH;
    KV_CHECK(saw_depth_hello);
    KV_CHECK(st.last_cfg.parameterSets.size() == 2 && (st.last_cfg.parameterSets[0][0] & 0x1F) == 7);
    phone.close();

    // A different phone (never paired) is rejected by the pin.
    FakePhone stranger;
    StreamStats s2;
    if (stranger.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &cerr)) s2 = runStream(stranger, 1500);
    KV_CHECK(!s2.got_hello);
    KV_CHECK(srv.rejectedHandshakeCount() >= 1);

    // The phone refuses a host whose certificate does not match its pin.
    FakePhone wrongpin;
    KV_CHECK(!wrongpin.connectStream("127.0.0.1", pr.streamPort, Bytes(32, 0x5A), &cerr));
}

KV_TEST(colour_mode_streams_1920x1080_and_phone_switch_can_be_disabled) {
    SyntheticHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"colour"), &err));
    HostStreamServer& srv = host.ctl.server();
    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    KV_REQUIRE(srv.allowShortCodeForPairing()); // the PC user chose "Use a code"
    FakePhone phone;
    const Bytes code(offer.shortCodeNormalized.begin(), offer.shortCodeNormalized.end());
    const auto pr = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::ShortCode, code);
    KV_REQUIRE(pr.ok);
    // Short-code path: the phone learnt the host's fingerprint from TLS, bound by the server proof.
    KV_CHECK(hexLower(pr.serverHash.data(), pr.serverHash.size()) == srv.serverFingerprintHex());

    host.setMode(KV_MODE_COLOR, 0);
    srv.setAllowPhoneModeSwitch(false);
    std::string cerr;
    KV_REQUIRE(phone.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &cerr));
    const StreamStats st = runStreamUntil(phone, 2500, helloAndAus(50), [&](int ms) {
        if (ms >= 1000 && ms < 1500) {
            SelectModeMessage sel;
            sel.modeRaw = KV_MODE_IR;
            phone.sendFrame(MsgType::SelectModeReq, toJson(sel));
        }
    });
    std::printf("  colour: cfg %dx%d mode=%d aus=%d decoded=%d barcode_ok=%d\n", st.last_cfg.width, st.last_cfg.height,
                st.last_cfg.modeRaw, st.aus, st.decoded, st.barcode_ok);
    KV_CHECK(st.got_hello && !st.hello.allowPhoneModeSwitch);
    KV_CHECK(st.last_cfg.width == 1920 && st.last_cfg.height == 1080 && st.last_cfg.modeRaw == KV_MODE_COLOR);
    KV_CHECK_MSG(ausWithin(st, 2500) >= 50, std::to_string(ausWithin(st, 2500)));
    KV_CHECK(st.barcode_ok >= st.decoded * 9 / 10);
    KV_CHECK(host.requested_mode.load() == -1); // ignored while switching is disallowed
}

KV_TEST(wrong_short_code_burns_the_one_try_window) {
    SyntheticHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"wrongcode"), &err));
    HostStreamServer& srv = host.ctl.server();
    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err));
    KV_REQUIRE(srv.allowShortCodeForPairing()); // the PC user chose "Use a code"
    std::string wrong = offer.shortCodeNormalized;
    wrong[0] = wrong[0] == '9' ? '0' : static_cast<char>(wrong[0] + 1);
    FakePhone phone;
    const auto bad = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::ShortCode, Bytes(wrong.begin(), wrong.end()));
    KV_CHECK(!bad.ok);
    KV_CHECK_MSG(bad.error == "Invalid pairing code or token; pairing window closed.", bad.error);
    KV_CHECK(!srv.currentPairingOffer().has_value());
    KV_CHECK(!srv.isPhonePaired());
    KV_CHECK(logHasLine(srv, "pairing_burned", "home", "wrong proof")); // v3 §8.6
    const auto retry = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::ShortCode,
                                  Bytes(offer.shortCodeNormalized.begin(), offer.shortCodeNormalized.end()));
    KV_CHECK(!retry.ok); // the right code no longer works: the window is gone
    KV_CHECK(!srv.isPhonePaired());
}

// The pairing listener pairs the TLS leaf, never the certificate a request carries. A leaf that can't be read back
// (the seam stands for one that can't be re-encoded) fails closed: closed unread, nothing paired, the window open.
KV_TEST(a_pairing_whose_tls_certificate_cant_be_read_fails_closed) {
    SyntheticHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"pair-noleaf"), &err));
    HostStreamServer& srv = host.ctl.server();
    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err));
    TlsConnection::failPeerCertReadForTest(1);
    FakePhone phone;
    const auto r = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    TlsConnection::failPeerCertReadForTest(0);
    KV_CHECK_MSG(!r.ok, "paired on the certificate the request carried");
    KV_CHECK(!srv.isPhonePaired());
    KV_CHECK_MSG(srv.currentPairingOffer().has_value(), "a leaf the host couldn't read burned the window");
    KV_CHECK(logHasLine(srv, "pairing_failed", "home", "its TLS certificate couldn't be read"));
    // The next attempt, whose leaf reads back, pairs in the same window.
    FakePhone again;
    KV_CHECK(again.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken).ok);
    KV_CHECK(srv.isPhonePaired());
}

// A pairing window takes QR only until the PC user chooses "Use a code". Before that, a code attempt (even the
// right code) is closed unread and burns nothing, so the window still pairs by QR; after it, the code pairs.
KV_TEST(a_code_pairs_only_after_the_pc_user_chose_use_a_code) {
    SyntheticHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"code-off"), &err));
    HostStreamServer& srv = host.ctl.server();
    KV_CHECK(!srv.allowShortCodeForPairing()); // no window open
    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err));
    KV_CHECK(!offer.shortCodeAllowed && !srv.currentPairingOffer()->shortCodeAllowed); // QR only by default
    const Bytes code(offer.shortCodeNormalized.begin(), offer.shortCodeNormalized.end());
    FakePhone early;
    const auto refused = early.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::ShortCode, code);
    KV_CHECK(!refused.ok);
    KV_CHECK(!srv.isPhonePaired());
    KV_CHECK_MSG(srv.currentPairingOffer().has_value(), "a code attempt before \"Use a code\" burned the window");
    KV_CHECK(logHasLine(srv, "pairing_failed", "home", "the window takes the QR code only")); // v3 §8.6
    // The window still pairs by QR.
    FakePhone byQr;
    const auto qr = byQr.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_CHECK(qr.ok);
    KV_CHECK(srv.devices().size() == 1);
    // A new window: after "Use a code" the same kind of attempt pairs.
    ActivePairingOffer second;
    KV_REQUIRE(srv.beginPairingWindow(120, second, &err));
    KV_REQUIRE(srv.allowShortCodeForPairing());
    KV_CHECK(srv.currentPairingOffer()->shortCodeAllowed);
    FakePhone later;
    const auto ok = later.pair("127.0.0.1", second.port, std::nullopt, PairingMethod::ShortCode,
                               Bytes(second.shortCodeNormalized.begin(), second.shortCodeNormalized.end()));
    KV_CHECK(ok.ok);
    KV_CHECK(srv.devices().size() == 2);
}

// The host's sockets are never inherited by a program it starts (ffmpeg, for a recording): a connection it
// accepts and one it makes are both made non-inheritable (the listener's own socket is made the same way).
KV_TEST(the_hosts_sockets_are_not_inheritable) {
    TcpListener l;
    std::string err;
    std::atomic<int> accepted_flags{-1};
    KV_REQUIRE(l.start(0, true, [&](SOCKET s, const sockaddr*) {
        DWORD f = 0;
        accepted_flags = GetHandleInformation(reinterpret_cast<HANDLE>(s), &f) ? static_cast<int>(f) : -2;
        closesocket(s);
    }, &err));
    SOCKET c = tcpConnect("127.0.0.1", l.port(), 3000, &err);
    if (c == INVALID_SOCKET) std::printf("  tcpConnect: %s\n", err.c_str());
    KV_REQUIRE(c != INVALID_SOCKET);
    DWORD cf = 0;
    KV_CHECK(GetHandleInformation(reinterpret_cast<HANDLE>(c), &cf) && (cf & HANDLE_FLAG_INHERIT) == 0);
    for (int i = 0; i < 300 && accepted_flags.load() == -1; ++i) Sleep(10);
    KV_CHECK_MSG(accepted_flags.load() >= 0 && (accepted_flags.load() & HANDLE_FLAG_INHERIT) == 0,
                 std::to_string(accepted_flags.load()));
    closesocket(c);
    l.stop();
}

// Re-scoped in W6 (v3 §2.4, §10): a second pairing adds a device instead of replacing the only pin.
KV_TEST(pairing_window_expires_and_a_second_pairing_adds_a_device) {
    SyntheticHost host;
    std::string err;
    const std::wstring dir = freshStateDir(L"expiry");
    KV_REQUIRE(host.start(dir, &err));
    HostStreamServer& srv = host.ctl.server();
    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(1, offer, &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));
    KV_CHECK(!srv.currentPairingOffer().has_value());
    FakePhone late;
    KV_CHECK(!late.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken).ok);

    FakePhone first, second;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    const auto p1 = first.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_REQUIRE(p1.ok);
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    const auto p2 = second.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_REQUIRE(p2.ok);
    KV_CHECK(p1.serviceId == p2.serviceId); // pairing never rotates the service id
    std::string cerr;
    StreamStats s1, s2;
    if (first.connectStream("127.0.0.1", p1.streamPort, p1.serverHash, &cerr)) s1 = runStreamUntil(first, 1200, helloAndAus(10));
    KV_CHECK(s1.got_hello && ausWithin(s1, 1200) >= 10); // the first phone is still paired: a second pairing adds a device
    first.close();
    KV_REQUIRE(second.connectStream("127.0.0.1", p2.streamPort, p2.serverHash, &cerr));
    s2 = runStream(second, 1200);
    KV_CHECK(s2.got_hello && s2.aus >= 10);
    second.close();
    const auto devs = srv.devices();
    KV_REQUIRE(devs.size() == 2);
    // Old phones send no name: "iPhone", then "iPhone 2" (v3 §4.4).
    KV_CHECK(devs[0].name == "iPhone" && devs[1].name == "iPhone 2" && devs[0].kind == "iphone");
    KV_CHECK(devs[0].deviceId == hexLower(first.identity().certificateSHA256().data(), 8));

    // Unpair (now: revoke every device) rotates the service id and drops both.
    const std::string old_id = srv.serviceId();
    srv.unpairPhone();
    KV_CHECK(!srv.isPhonePaired());
    KV_CHECK(srv.devices().empty());
    KV_CHECK(srv.serviceId() != old_id && srv.serviceId().size() == 12);
}

KV_TEST(identity_and_pin_persist_across_restart) {
    const std::wstring dir = freshStateDir(L"persist");
    std::string fp, pin, sid;
    {
        HostStreamServer s;
        HostStreamServer::Options o;
        o.state_dir = dir;
        o.loopback_only = true;
        std::string err;
        KV_REQUIRE(s.start(o, &err));
        ActivePairingOffer offer;
        KV_REQUIRE(s.beginPairingWindow(120, offer, &err));
        FakePhone phone;
        KV_REQUIRE(phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken).ok);
        fp = s.serverFingerprintHex();
        pin = s.pinnedClientFingerprintShort();
        sid = s.serviceId();
        s.shutdown();
    }
    HostStreamServer s;
    HostStreamServer::Options o;
    o.state_dir = dir;
    o.loopback_only = true;
    std::string err;
    KV_REQUIRE(s.start(o, &err));
    KV_CHECK(s.serverFingerprintHex() == fp);
    KV_CHECK(s.pinnedClientFingerprintShort() == pin);
    KV_CHECK(s.serviceId() == sid);
    KV_CHECK(isOwnerOnly(dir) && isOwnerOnly(dir + L"\\pairing.json"));
}

// ---- protocol v2 (docs/protocol-changes.md) -------------------------------------------------------

// What a v1 phone must never see is checked in every round of every run: no 0x07, no violation, a v3 hello, the v1
// version seen, and video and pong answers reaching it at all. The pace (20 AUs within 1.5 s of the first, 4 pongs to
// its pings every 250 ms) is a timing floor, judged against the desk each round: a busy PC slows a round's encode and
// loopback with nothing wrong in what the host sends.
KV_TEST(v2_a_v1_phone_never_receives_0x07_across_reconnects) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v1phone"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone phone(1); // strict v1 reader: a 0x07 would be a violation and end the stream
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr));
    for (int round = 0; round < 3; ++round) {
        kvtest::DeskSampler desk;
        if (round > 0) {
            std::string cerr;
            KV_REQUIRE(phone.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &cerr));
        }
        int version_seen = 0;
        const StreamStats st = runStreamUntil(phone, 1500, helloAndAus(20), [&](int ms) {
            if (ms >= 1000) version_seen = srv.activeClientProtocolVersion();
            // State changes on the host while the v1 phone is connected: none may reach it.
            host.ctl.pushHostState();
            srv.setAllowPhoneSensorPowerControl(ms % 1000 < 500);
        });
        const int paced = ausWithin(st, 1500);
        std::printf("  v1 round %d: hello v%d, aus=%d (%d within 1.5 s) pongs=%d states=%zu violated=%d (%s)\n    %s\n",
                    round, st.got_hello ? st.hello.protocolVersion : 0, st.aus, paced, st.pongs, st.states.size(),
                    phone.violated() ? 1 : 0, phone.violation().c_str(), desk.report().line().c_str());
        KV_CHECK(st.got_hello && st.hello.protocolVersion == 3); // the old app reads a v3 hello fine (re-scoped from 2)
        KV_CHECK(!phone.violated());
        KV_CHECK(st.states.empty());
        KV_CHECK(version_seen == 1);
        KV_CHECK_MSG(st.aus >= 20, std::to_string(st.aus)); // video reaches it (within the stream deadline)
        KV_CHECK(st.pongs >= 1);                             // and its pings are answered
        const std::string r = "round " + std::to_string(round) + ": ";
        KV_FLOOR(paced >= 20, desk, r + std::to_string(paced) + " AUs within 1.5 s of the first");
        KV_FLOOR(st.pongs >= 4, desk, r + std::to_string(st.pongs) + " pongs"); // it pings every 250 ms
        phone.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    srv.setAllowPhoneSensorPowerControl(true);
}

KV_TEST(v2_a_v2_phone_receives_0x07_only_after_its_v2_ping) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v2phone"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone phone(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr));
    // Connected, no ping yet: the host is at v1 for this phone and sends no 0x07, even on changes.
    const StreamStats quiet = runStream(phone, 900, [&](int) { host.ctl.pushHostState(); }, /*send_pings=*/false);
    KV_CHECK(quiet.got_hello && quiet.hello.protocolVersion == 3); // re-scoped from 2: v2 phones read ">= 2"
    KV_CHECK(quiet.states.empty());
    KV_CHECK(srv.activeClientProtocolVersion() == 1);
    // Now it pings with protocolVersion 2: one 0x07 comes straight away.
    const StreamStats st = runStream(phone, 1500, {}, true, /*hello_seen=*/true);
    std::printf("  v2: first ping at %d ms, first 0x07 at %d ms, states=%zu\n", st.first_ping_ms,
                st.state_ms.empty() ? -1 : st.state_ms.front(), st.states.size());
    KV_REQUIRE(!st.states.empty());
    KV_CHECK(st.state_ms.front() >= st.first_ping_ms && st.state_ms.front() - st.first_ping_ms < 500);
    const HostStateSyncMessage& s = st.states.front();
    KV_CHECK(s.protocolVersion == 3 && s.stateSeq >= 1 && s.powerState == SensorPowerState::On); // re-scoped from 2
    KV_CHECK(s.allowPhoneModeSwitch && s.allowPhoneSensorPower && !s.sensorPowerDisabledReason);
    KV_CHECK(!s.canRecordCurrentMode && s.recordingDisabledReason == std::string("test host"));
    KV_CHECK(srv.activeClientProtocolVersion() == 2);
    KV_CHECK(seqNeverGoesDown(st.states));

    // A reconnect starts at v1 again: nothing before its own v2 ping.
    phone.close();
    std::string cerr;
    KV_REQUIRE(phone.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &cerr));
    const StreamStats again = runStream(phone, 700, [&](int) { host.ctl.pushHostState(); }, false);
    KV_CHECK(again.got_hello && again.states.empty());
    const StreamStats after = runStream(phone, 800, {}, true, /*hello_seen=*/true);
    KV_REQUIRE(!after.states.empty());
    KV_CHECK(after.states.front().stateSeq > st.states.back().stateSeq); // never goes down
}

// Power off and on from the phone. While Off there is no video and the session stays open (pings are answered); then
// starting, then on, and video again. These are checked in every run. The floor, Off within 1 s of the request, is the
// host's own promise: from the request reaching the host to its power model reaching Off, on the host's clock, judged
// against the desk (KV_FLOOR: skipped, never passed, when the PC was busy). The phone's view of it is printed beside
// it, not gated. "On" is sent 2 s after the host reached Off, so the off window, and the pings answered in it, don't
// depend on how fast it got there.
KV_TEST(v2_power_off_stops_video_keeps_the_session_and_on_brings_it_back) {
    kvtest::DeskSampler desk;
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"power"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone phone(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr));
    int off_sent = -1, off_seen = -1, on_sent = -1;
    const StreamStats st = runStream(phone, 0, [&](int ms) {
        if (ms >= 1500 && off_sent < 0) {
            off_sent = ms;
            SensorPowerControlMessage r;
            r.requestId = 1;
            r.turnOn = false;
            phone.sendFrame(MsgType::SensorPowerReq, toJson(r));
        }
        if (off_sent >= 0 && off_seen < 0 && host.offReached()) off_seen = ms;
        if (off_seen >= 0 && ms >= off_seen + 2000 && on_sent < 0) {
            on_sent = ms;
            SensorPowerControlMessage r;
            r.requestId = 2;
            r.turnOn = true;
            phone.sendFrame(MsgType::SensorPowerReq, toJson(r));
        }
    }, true, false, [&](const StreamStats& x) {
        return on_sent >= 0 && countIn(x.au_ms, on_sent + 300, std::numeric_limits<int>::max()) >= 40; // video again
    }, 20000);
    int off_ms = -1, starting_ms = -1, on_ms = -1;
    for (size_t i = 0; i < st.states.size(); ++i) {
        const auto ps = st.states[i].powerState;
        if (ps == SensorPowerState::Off && off_ms < 0) off_ms = st.state_ms[i];
        if (ps == SensorPowerState::Starting && off_ms >= 0 && starting_ms < 0) starting_ms = st.state_ms[i];
        if (ps == SensorPowerState::On && starting_ms >= 0 && on_ms < 0) on_ms = st.state_ms[i];
    }
    const int end_ms = std::numeric_limits<int>::max();
    const int aus_off = off_ms >= 0 && on_sent >= 0 ? countIn(st.au_ms, off_ms + 300, on_sent) : -1;
    const int aus_after = on_ms >= 0 ? countIn(st.au_ms, on_ms + 300, end_ms) : -1;
    const int pongs_off = off_ms >= 0 && on_sent >= 0 ? countIn(st.pong_ms, off_ms, on_sent) : -1;
    const int host_off_ms = host.hostOffMs();
    std::printf("  host: off %d ms after the request; phone: off sent %d, off at %d; on sent %d, starting at %d, on at %d; "
                "aus while off=%d, after on=%d; pongs while off=%d; states=%zu\n",
                host_off_ms, off_sent, off_ms, on_sent, starting_ms, on_ms, aus_off, aus_after, pongs_off, st.states.size());
    std::printf("  %s\n", desk.report().line().c_str());
    KV_CHECK(off_ms >= off_sent && on_sent > off_ms); // the phone sees Off after asking, and before asking for On
    KV_FLOOR(host_off_ms >= 0 && host_off_ms < 1000, desk,
             "the host reached Off " + std::to_string(host_off_ms) + " ms after the request");
    KV_CHECK(aus_off == 0);       // Off: no 0x03 video
    KV_CHECK(pongs_off >= 4);     // ...and the TLS session stays open
    KV_CHECK(!st.closed_by_host);
    KV_CHECK(starting_ms >= on_sent && on_ms >= starting_ms); // starting, then on, as it happens
    KV_CHECK(aus_after >= 20);    // On: video again
    KV_CHECK(seqNeverGoesDown(st.states));
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On && host.isOpen());
}

KV_TEST(v2_power_requests_are_refused_with_a_reason_when_power_control_is_off) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"nopower"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setAllowPhoneSensorPowerControl(false);
    srv.setAllowPhoneModeSwitch(true); // independent of mode switching
    FakePhone phone(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr));
    bool sent = false;
    const StreamStats st = runStream(phone, 2500, [&](int ms) {
        if (ms >= 1000 && !sent) {
            sent = true;
            SensorPowerControlMessage r;
            r.requestId = 9;
            r.turnOn = false;
            phone.sendFrame(MsgType::SensorPowerReq, toJson(r));
        }
    }, true, false, [](const StreamStats& x) { return x.states.size() >= 2; }, 15000); // waits for the refusal's state
    KV_REQUIRE(st.states.size() >= 2); // the v2 upgrade's, then the refusal's
    for (const auto& s : st.states) {
        KV_CHECK(!s.allowPhoneSensorPower);
        KV_CHECK(s.sensorPowerDisabledReason == std::string("Power control is turned off on the Mac"));
        KV_CHECK(s.allowPhoneModeSwitch);
        KV_CHECK(s.powerState == SensorPowerState::On);
    }
    KV_CHECK(host.processed.load() == 1); // only the start-up On; the phone's Off never reached the model
    KV_CHECK(host.isOpen());
    // Turning it back on here tells the phone at once.
    bool allowed = false;
    // Waits for the state that says so (a fixed 800 ms raced it on a busy PC), with a generous deadline.
    const StreamStats st2 = runStream(phone, 0, [&](int) {
        if (!allowed) {
            allowed = true;
            srv.setAllowPhoneSensorPowerControl(true);
        }
    }, true, /*hello_seen=*/true, [](const StreamStats& x) {
        return !x.states.empty() && x.states.back().allowPhoneSensorPower;
    }, 10000);
    KV_REQUIRE(!st2.states.empty());
    KV_CHECK(st2.states.back().allowPhoneSensorPower && !st2.states.back().sensorPowerDisabledReason);
}

KV_TEST(v2_remote_commands_are_gated_and_answered_and_unknown_types_are_skipped) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"remote"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setAllowPhoneModeSwitch(false);
    FakePhone phone(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr));
    int step = 0;
    const StreamStats st = runStream(phone, 3000, [&](int ms) {
        if (ms >= 800 && step == 0) {
            step = 1;
            RemoteControlCommandMessage sel;
            sel.action = RemoteControlAction::SelectMode;
            sel.modeRaw = 2;
            phone.sendFrame(MsgType::RemoteControlCmd, toJson(sel)); // needs mode switching: dropped
            Orbit3DGestureMessage orbit;
            orbit.deltaYawRadians = 0.1f;
            phone.sendFrame(MsgType::Orbit3DGesture, toJson(orbit)); // likewise
            RemoteControlCommandMessage key;
            phone.sendFrame(MsgType::RemoteControlCmd, toJson(key)); // request_keyframe: always allowed
            // Unknown types are skipped, not a violation. W6 took 0x0B-0x0F, so (re-scoped) the sends
            // use bytes nobody knows: 0x7E (the Swift sentinel) and 0x1A, the first free one (v3 §1 rule 7).
            phone.sendRaw(0x7E, "{\"fromTheFuture\":true}");
            phone.sendRaw(0x1A, "");
            // A v3 command from a v2 connection is ignored, never acted on or answered (v3 §1 rule 3).
            PetCameraCommandMessage priv;
            priv.requestId = 1;
            priv.action = pet_action::kSetPrivacy;
            priv.enabled = true;
            phone.sendFrame(MsgType::PetCameraCmd, toJson(priv));
        }
    });
    std::printf("  commands=%d keyframes=%d states=%zu closed=%d pongs after=%d\n", host.commands.load(),
                host.keyframe_commands.load(), st.states.size(), st.closed_by_host ? 1 : 0, countIn(st.pong_ms, 1000, 3000));
    KV_CHECK(host.commands.load() == 1 && host.keyframe_commands.load() == 1);
    KV_CHECK(host.orbits.load() == 0); // 0x09 is gated with mode switching
    KV_CHECK(!st.closed_by_host);
    KV_CHECK(countIn(st.pong_ms, 1000, 3000) >= 6); // the session carried on after the unknown frames
    KV_CHECK(st.states.size() >= 2);                // the upgrade's, then the keyframe command's
    KV_CHECK(seqNeverGoesDown(st.states));
    KV_CHECK(!srv.petSettings().privacyMode); // the v2 connection's 0x0C changed nothing
    for (const auto& s : st.states) KV_CHECK(s.powerState == SensorPowerState::On);
}

// Stage 5: with mode switching allowed, select_mode 4, set_cutoff (Track me, and off) and 0x09 come
// through with their fields; with it turned off again, the same orbit is dropped.
KV_TEST(v2_select_mode_4_set_cutoff_and_orbit_come_through_when_allowed) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"remote3d"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setAllowPhoneModeSwitch(true);
    FakePhone phone(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr));
    int step = 0;
    const StreamStats st = runStream(phone, 3200, [&](int ms) {
        if (ms >= 800 && step == 0) {
            step = 1;
            RemoteControlCommandMessage sel;
            sel.action = RemoteControlAction::SelectMode;
            sel.modeRaw = 4;
            phone.sendFrame(MsgType::RemoteControlCmd, toJson(sel));
            RemoteControlCommandMessage track;
            track.action = RemoteControlAction::SetCutoff;
            track.applyTrackMePreset = true;
            phone.sendFrame(MsgType::RemoteControlCmd, toJson(track));
            RemoteControlCommandMessage off;
            off.action = RemoteControlAction::SetCutoff;
            off.cutoffEnabled = false;
            phone.sendFrame(MsgType::RemoteControlCmd, toJson(off));
            Orbit3DGestureMessage orbit;
            orbit.deltaYawRadians = 0.25f;
            orbit.zoomScale = 0.5f;
            orbit.panUpMeters = 0.1f;
            phone.sendFrame(MsgType::Orbit3DGesture, toJson(orbit));
            Orbit3DGestureMessage reset;
            reset.resetView = true;
            phone.sendFrame(MsgType::Orbit3DGesture, toJson(reset));
        } else if (ms >= 1800 && step == 1) {
            step = 2;
            srv.setAllowPhoneModeSwitch(false); // turned off on the PC: the next orbit is dropped
            Orbit3DGestureMessage late;
            late.deltaPitchRadians = 0.5f;
            phone.sendFrame(MsgType::Orbit3DGesture, toJson(late));
        }
    });
    std::lock_guard<std::mutex> lk(host.cm);
    std::printf("  commands=%zu orbits=%zu closed=%d\n", host.got_commands.size(), host.got_orbits.size(), st.closed_by_host ? 1 : 0);
    KV_CHECK(!st.closed_by_host);
    KV_REQUIRE(host.got_commands.size() == 3);
    KV_CHECK(host.got_commands[0].action == RemoteControlAction::SelectMode && host.got_commands[0].modeRaw == 4);
    KV_CHECK(host.got_commands[1].action == RemoteControlAction::SetCutoff &&
             host.got_commands[1].applyTrackMePreset.value_or(false));
    KV_CHECK(host.got_commands[2].action == RemoteControlAction::SetCutoff && host.got_commands[2].cutoffEnabled == false &&
             !host.got_commands[2].applyTrackMePreset.value_or(false));
    KV_REQUIRE(host.got_orbits.size() == 2); // the late one, after the setting went off, never arrived
    KV_CHECK(std::fabs(host.got_orbits[0].deltaYawRadians - 0.25f) < 1e-6f && std::fabs(host.got_orbits[0].zoomScale - 0.5f) < 1e-6f &&
             std::fabs(host.got_orbits[0].panUpMeters - 0.1f) < 1e-6f && !host.got_orbits[0].resetView);
    KV_CHECK(host.got_orbits[1].resetView);
    KV_CHECK(seqNeverGoesDown(st.states));
}

KV_TEST(v2_watchdog_closes_a_silent_phone_and_turn_off_on_disconnect_applies) {
    for (const bool turn_off : {false, true}) {
        PoweredHost host;
        std::string err;
        KV_REQUIRE(host.start(freshStateDir(turn_off ? L"wd-off" : L"wd-keep"), &err, /*ping_timeout_ms=*/700));
        HostStreamServer& srv = host.ctl.server();
        srv.setTurnOffKinectOnPhoneDisconnect(turn_off);
        FakePhone phone(2);
        FakePhone::PairResult pr;
        KV_REQUIRE(pairAndConnect(srv, phone, pr));
        // One v2 ping, then silence: the phone reads but sends nothing (a dead app or a lost Wi-Fi).
        const StreamStats alive = runStream(phone, 300);
        KV_CHECK(srv.pairedClientConnected());
        const auto silent_from = std::chrono::steady_clock::now();
        const StreamStats silent = runStream(phone, 3000, {}, /*send_pings=*/false, /*hello_seen=*/true);
        const double closed_after_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - silent_from).count();
        KV_CHECK(host.drain());
        std::printf("  turn_off=%d: closed by host=%d after %.2f s, disconnects=%d, power=%s\n", turn_off ? 1 : 0,
                    silent.closed_by_host ? 1 : 0, closed_after_s, host.disconnects.load(),
                    toWire(host.power->snapshot().state));
        KV_CHECK(silent.closed_by_host);
        KV_CHECK(closed_after_s < 2.5);
        KV_CHECK(!srv.pairedClientConnected());
        KV_CHECK(host.disconnects.load() == 1);
        // The sensor keeps its state when the phone goes, unless the setting is on.
        KV_CHECK(host.power->snapshot().state == (turn_off ? SensorPowerState::Off : SensorPowerState::On));
        KV_CHECK(host.isOpen() == !turn_off);
        (void)alive;
    }
}

KV_TEST(v2_a_phone_that_disconnects_mid_toggle_ends_where_the_setting_says) {
    for (const bool turn_off : {false, true}) {
        PoweredHost host;
        std::string err;
        KV_REQUIRE(host.start(freshStateDir(turn_off ? L"mid-off" : L"mid-keep"), &err));
        HostStreamServer& srv = host.ctl.server();
        srv.setTurnOffKinectOnPhoneDisconnect(turn_off);
        host.post(false); // start from Off
        KV_REQUIRE(host.drain());
        FakePhone phone(2);
        FakePhone::PairResult pr;
        KV_REQUIRE(pairAndConnect(srv, phone, pr));
        runStream(phone, 400);
        SensorPowerControlMessage on;
        on.requestId = 5;
        on.turnOn = true;
        phone.sendFrame(MsgType::SensorPowerReq, toJson(on));
        phone.close(); // gone before the host could answer
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (srv.pairedClientConnected() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        KV_CHECK(host.drain());
        std::printf("  turn_off=%d: power=%s disconnects=%d processed=%d\n", turn_off ? 1 : 0,
                    toWire(host.power->snapshot().state), host.disconnects.load(), host.processed.load());
        // The On came first and is handled first; then the disconnect's Off, only if the setting is on.
        KV_CHECK(host.power->snapshot().state == (turn_off ? SensorPowerState::Off : SensorPowerState::On));
        KV_CHECK(host.disconnects.load() == 1);
    }
}

KV_TEST(v2_a_v2_connection_replaced_by_a_v1_one_with_the_same_cert_gets_no_0x07_and_no_disconnect) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"replace-v1"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setTurnOffKinectOnPhoneDisconnect(true); // a wrongly fired hook would turn the Kinect off
    FakePhone v2(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, v2, pr));
    const StreamStats first = runStream(v2, 600);
    KV_REQUIRE(!first.states.empty());
    KV_REQUIRE(srv.activeClientProtocolVersion() == 2);

    // While the v2 connection is still open, the same certificate connects again as an old app.
    FakePhone v1(1, v2.identity());
    std::string cerr;
    KV_REQUIRE(v1.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &cerr));
    int version_seen = -1;
    bool toggle = false;
    const StreamStats st = runStream(v1, 1500, [&](int ms) {
        // State changes while the v1 connection is the active one: none may reach it.
        host.ctl.pushHostState();
        toggle = !toggle;
        srv.setAllowPhoneSensorPowerControl(toggle);
        if (ms >= 1000) version_seen = srv.activeClientProtocolVersion();
    });
    srv.setAllowPhoneSensorPowerControl(true);
    KV_CHECK(host.drain());
    std::printf("  replaced: v1 hello=%d aus=%d pongs=%d states=%zu violated=%d; disconnects=%d power=%s\n", st.got_hello ? 1 : 0,
                st.aus, st.pongs, st.states.size(), v1.violated() ? 1 : 0, host.disconnects.load(),
                toWire(host.power->snapshot().state));
    KV_CHECK(st.got_hello && st.aus >= 20);
    KV_CHECK(!v1.violated());
    KV_CHECK(st.states.empty());
    KV_CHECK(version_seen == 1);
    KV_CHECK(host.disconnects.load() == 0); // the same phone, back: not a disconnect
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On);
    // The replaced v2 connection was closed by the host.
    MsgType t;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
    bool closed = false;
    for (int i = 0; i < 200 && !closed; ++i) {
        if (!v2.readFrame(t, flags, payload, 300)) closed = v2.lastReadWasClose();
    }
    KV_CHECK(closed);
}

KV_TEST(v2_unpairing_goes_through_the_disconnect_hook) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"unpair-hook"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setTurnOffKinectOnPhoneDisconnect(true);
    FakePhone phone(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr));
    runStream(phone, 500);
    // Waited for, not assumed after a fixed 500 ms: on a busy PC the session may still be coming up.
    for (int i = 0; i < 500 && !srv.pairedClientConnected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    KV_REQUIRE(srv.pairedClientConnected());
    srv.unpairPhone();
    KV_CHECK(host.drain());
    KV_CHECK(host.disconnects.load() == 1);
    KV_CHECK(host.power->snapshot().state == SensorPowerState::Off);
    KV_CHECK(!srv.isPhonePaired());
    // The phone, still holding its old pin, can't get back in.
    std::string cerr;
    StreamStats again;
    if (phone.connectStream("127.0.0.1", pr.streamPort, pr.serverHash, &cerr)) again = runStream(phone, 800);
    KV_CHECK(!again.got_hello);
}

// ---- protocol v3 (W6): many devices, roles, availability, slots, tiers ------------------------------
// docs/protocol-v3-pet-camera.md §10 "W6". Synthetic source, loopback, temp state folders only.

namespace {

// Everything one device saw, as a v3 PocketStreamClientCore would track it.
struct V3Log {
    bool got_hello = false;
    std::vector<HelloServerMessage> hellos;
    std::vector<StreamConfigMessage> configs;
    std::vector<int> cfg_ms;
    std::vector<std::string> cfg_tier; // the tier the last 0x0B named when each 0x02 came ("high" before any)
    int aus = 0;
    int keyframes = 0;
    int decoded = 0;
    bool first_au_key = false;
    int au_epoch_mismatch = 0; // an AU before any 0x02, or of another epoch than the last 0x02's
    int not_idr_after_cfg = 0; // the first AU after a 0x02 of a new epoch wasn't an IDR
    int cfg_epoch_went_down = 0;
    std::vector<int> au_ms;
    std::vector<int> key_ms;
    std::vector<uint32_t> au_epochs;
    std::vector<HostStateSyncMessage> states;
    std::vector<int> state_ms;
    std::vector<PetCameraStateMessage> pets;
    std::vector<int> pet_ms;
    std::vector<CommandResultMessage> results;
    std::vector<SessionNoticeMessage> notices;
    std::vector<int> notice_ms;
    int pongs = 0;
    int first_ping_ms = -1;
    bool closed_by_host = false;
    int closed_ms = -1;
    bool violated = false;

    int ausFrom(int ms) const { return countIn(au_ms, ms, 1 << 30); }
    const PetCameraStateMessage* lastPet() const { return pets.empty() ? nullptr : &pets.back(); }
    const CommandResultMessage* result(uint64_t id) const {
        for (const auto& r : results)
            if (r.requestId == id) return &r;
        return nullptr;
    }
    bool hasNotice(const char* reason) const {
        for (const auto& n : notices)
            if (n.reason == reason) return true;
        return false;
    }
};

// Runs one device's stream on its own thread (like the phone's queue): reads, decodes, pings every
// 250 ms after hello, and sends what the test queues. The FakePhone is not touched by anyone else
// while it runs.
class PhoneRunner {
public:
    explicit PhoneRunner(FakePhone& p, bool send_pings = true) : phone_(p), pings_(send_pings) {}
    ~PhoneRunner() { stop(); }
    PhoneRunner(const PhoneRunner&) = delete;
    PhoneRunner& operator=(const PhoneRunner&) = delete;

    void start() {
        t0_ = nowNs();
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        stop_ = true;
        if (th_.joinable()) th_.join();
    }
    void send(MsgType t, const std::string& json) {
        std::lock_guard<std::mutex> lk(m_);
        out_.push_back({t, json});
    }
    int ms() const { return static_cast<int>((nowNs() - t0_) / 1000000ULL); }
    // A device that stops reading for a while (a congested link): nothing is read, sent or pinged.
    void setPaused(bool on) { paused_ = on; }
    // Drains the phone's decoder on its own thread (TestH264Decoder::drain): frames it still holds count as decoded.
    void drainDecoder() { drain_ = true; }
    V3Log log() const {
        std::lock_guard<std::mutex> lk(m_);
        return log_;
    }
    bool waitFor(const std::function<bool(const V3Log&)>& pred, int timeout_ms) const {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < until) {
            if (pred(log())) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred(log());
    }

private:
    struct Out {
        MsgType t;
        std::string json;
    };
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            auto dec = std::make_unique<kvtest::TestH264Decoder>();
            dec->open();
            uint32_t epoch = 0;
            bool have_cfg = false, expect_idr = false;
            StreamConfigMessage cfg;
            uint64_t next_ping = 0;
            while (!stop_) {
                if (paused_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (drain_.exchange(false)) {
                    const int n = dec->drain();
                    std::lock_guard<std::mutex> lk(m_);
                    log_.decoded += n;
                }
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
                if (pings_ && hello && now >= next_ping) {
                    PingMessage ping;
                    ping.clientSendNs = now;
                    ping.protocolVersion = phone_.pingVersion();
                    phone_.sendFrame(MsgType::Ping, toJson(ping));
                    next_ping = now + 250000000ULL;
                    std::lock_guard<std::mutex> lk(m_);
                    if (log_.first_ping_ms < 0) log_.first_ping_ms = ms();
                }
                MsgType t;
                uint8_t flags = 0;
                std::vector<uint8_t> payload;
                if (!phone_.readFrame(t, flags, payload, 200)) {
                    std::lock_guard<std::mutex> lk(m_);
                    if (phone_.violated()) {
                        log_.violated = true;
                        break;
                    }
                    if (phone_.lastReadWasClose()) {
                        log_.closed_by_host = true;
                        log_.closed_ms = ms();
                        break;
                    }
                    continue;
                }
                const int at = ms();
                std::lock_guard<std::mutex> lk(m_);
                switch (t) {
                    case MsgType::HelloServer: {
                        HelloServerMessage h;
                        if (fromJson(payload, h)) {
                            log_.got_hello = true;
                            log_.hellos.push_back(h);
                        }
                        break;
                    }
                    case MsgType::StreamConfig: {
                        StreamConfigMessage c;
                        if (!fromJson(payload, c) || c.parameterSets.size() < 2) break;
                        if (have_cfg && c.streamEpoch < epoch) ++log_.cfg_epoch_went_down;
                        if (have_cfg && (c.width != cfg.width || c.height != cfg.height)) {
                            dec = std::make_unique<kvtest::TestH264Decoder>(); // a new format: a new decoder
                            dec->open();
                        }
                        if (!have_cfg || c.streamEpoch != epoch) expect_idr = true;
                        cfg = c;
                        epoch = c.streamEpoch;
                        have_cfg = true;
                        log_.configs.push_back(c);
                        log_.cfg_ms.push_back(at);
                        log_.cfg_tier.push_back(log_.pets.empty() ? std::string("high") : log_.pets.back().liveTier);
                        break;
                    }
                    case MsgType::VideoAccessUnit: {
                        VideoAccessUnitHeader h;
                        const uint8_t* avcc = nullptr;
                        size_t n = 0;
                        if (!VideoAccessUnitHeader::decode(payload.data(), payload.size(), h, &avcc, &n)) break;
                        if (!have_cfg || h.streamEpoch != epoch) {
                            ++log_.au_epoch_mismatch; // the phone would drop it
                            break;
                        }
                        if (expect_idr && !h.isKeyframe()) ++log_.not_idr_after_cfg;
                        expect_idr = false;
                        ++log_.aus;
                        if (log_.aus == 1) log_.first_au_key = h.isKeyframe();
                        log_.au_ms.push_back(at);
                        log_.au_epochs.push_back(h.streamEpoch);
                        if (h.isKeyframe()) {
                            ++log_.keyframes;
                            log_.key_ms.push_back(at);
                        }
                        const auto annexb = kvtest::avccToAnnexB(avcc, n, h.isKeyframe() ? cfg.parameterSets
                                                                                         : std::vector<std::vector<uint8_t>>{});
                        if (dec->decode(annexb.data(), annexb.size())) ++log_.decoded;
                        break;
                    }
                    case MsgType::HostStateSync: {
                        HostStateSyncMessage s;
                        if (fromJson(payload, s)) {
                            log_.states.push_back(s);
                            log_.state_ms.push_back(at);
                        }
                        break;
                    }
                    case MsgType::PetCameraState: {
                        PetCameraStateMessage s;
                        if (fromJson(payload, s)) {
                            log_.pets.push_back(s);
                            log_.pet_ms.push_back(at);
                        }
                        break;
                    }
                    case MsgType::CommandResult: {
                        CommandResultMessage r;
                        if (fromJson(payload, r)) log_.results.push_back(r);
                        break;
                    }
                    case MsgType::SessionNotice: {
                        SessionNoticeMessage nm;
                        if (fromJson(payload, nm)) {
                            log_.notices.push_back(nm);
                            log_.notice_ms.push_back(at);
                        }
                        break;
                    }
                    case MsgType::Pong: ++log_.pongs; break;
                    default: break;
                }
            }
        }
        CoUninitialize();
    }

    FakePhone& phone_;
    bool pings_ = true;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> drain_{false};
    uint64_t t0_ = 0;
    mutable std::mutex m_;
    std::vector<Out> out_;
    V3Log log_;
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
    const std::optional<Bytes> pin = pr.serverHash.empty() ? std::nullopt : std::optional<Bytes>(pr.serverHash);
    return phone.connectStream("127.0.0.1", pr.streamPort, pin, &err);
}

std::string idOf(const FakePhone& p) { return hexLower(p.identity().certificateSHA256().data(), 8); }

std::string cmdJson(uint64_t id, const std::string& action, std::optional<bool> enabled = std::nullopt,
                    std::optional<std::string> device = std::nullopt) {
    PetCameraCommandMessage c;
    c.requestId = id;
    c.action = action;
    c.enabled = enabled;
    c.deviceId = std::move(device);
    return toJson(c);
}

std::string watchJson(bool watch, std::optional<std::string> quality = std::nullopt) {
    LiveViewRequestMessage r;
    r.watch = watch;
    r.quality = std::move(quality);
    return toJson(r);
}

bool petSeqNeverGoesDown(const V3Log& l) {
    for (size_t i = 1; i < l.pets.size(); ++i)
        if (l.pets[i].stateSeq < l.pets[i - 1].stateSeq) return false;
    return true;
}

bool waitUntil(const std::function<bool()>& pred, int timeout_ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

KV_TEST(v3_two_v3_devices_and_a_v1_phone_stream_side_by_side) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-mixed"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone a(3), b(3), c(1);
    a.setDeviceName("Owner's iPhone");
    b.setDeviceName("  Second\tiPad  ");
    b.setDeviceKind("ipad");
    FakePhone::PairResult pa, pb, pc;
    KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pa));
    KV_REQUIRE(pairDevice(srv, b, PetRole::Viewer, pb));
    KV_REQUIRE(pairDevice(srv, c, PetRole::Owner, pc));
    // pairResponse's v3 keys (v3 §4.4); a v1 phone ignores them.
    KV_CHECK(pa.role == std::string("owner") && pa.deviceName == std::string("Owner's iPhone") && pa.protocolVersion == 3 &&
             pa.deviceId == idOf(a));
    KV_CHECK(pb.role == std::string("viewer") && pb.deviceName == std::string("Second iPad"));
    const auto devs = srv.devices();
    KV_REQUIRE(devs.size() == 3);
    KV_CHECK(devs[0].lastProtocolVersion == 3 && devs[1].kind == "ipad" && devs[2].name == "iPhone" && devs[2].kind == "iphone");

    KV_REQUIRE(connectDevice(a, pa) && connectDevice(b, pb) && connectDevice(c, pc));
    PhoneRunner ra(a), rb(b), rc(c);
    ra.start();
    rb.start();
    rc.start();
    sleepMs(3000);
    ra.stop();
    rb.stop();
    rc.stop();
    const V3Log la = ra.log(), lb = rb.log(), lc = rc.log();
    std::printf("  owner v3: aus=%d decoded=%d pets=%zu states=%zu | viewer v3: aus=%d decoded=%d pets=%zu | v1: aus=%d "
                "decoded=%d states=%zu violated=%d\n",
                la.aus, la.decoded, la.pets.size(), la.states.size(), lb.aus, lb.decoded, lb.pets.size(), lc.aus, lc.decoded,
                lc.states.size(), lc.violated ? 1 : 0);
    // The v1 phone: no 0x07 and no v3 type (its strict reader would have refused either), and it streams.
    KV_CHECK(!lc.violated && lc.states.empty() && lc.pets.empty() && !lc.closed_by_host);
    KV_CHECK(lc.aus >= 30 && lc.decoded >= lc.aus - 2 && lc.first_au_key);
    KV_CHECK(lc.got_hello && lc.hellos.front().protocolVersion == 3);
    // Both v3 devices: hello with petCamera, 0x07 and 0x0B, and frames.
    for (const V3Log* l : {&la, &lb}) {
        KV_REQUIRE(l->got_hello && l->hellos.front().petCamera);
        const PetCameraHello& pc3 = *l->hellos.front().petCamera;
        // v3 §4.1: petCamera.outside is true from W8 (re-scoped from W6's false); outside listening itself is off
        // by default, which 0x0B's outsideEnabled says (checked below).
        KV_CHECK(pc3.specVersion == 1 && pc3.maxWatchers == 4 && !pc3.cameraRoll && pc3.outside && pc3.qualities.size() == 4);
        KV_CHECK(!l->pets.empty() && !l->states.empty());
        KV_CHECK(l->aus >= 30 && l->decoded >= l->aus - 2 && l->first_au_key && l->au_epoch_mismatch == 0);
        KV_CHECK(petSeqNeverGoesDown(*l) && seqNeverGoesDown(l->states));
        for (const auto& s : l->states) KV_CHECK(s.protocolVersion == 3);
        // The first 0x0B comes straight after the connection becomes v3 (its first ping).
        KV_CHECK(l->pet_ms.front() >= l->first_ping_ms && l->pet_ms.front() - l->first_ping_ms < 600);
    }
    // 0x0B by role: the owner's lists every device and every watcher; the viewer's leaves them out.
    const PetCameraStateMessage* oa = la.lastPet();
    const PetCameraStateMessage* ob = lb.lastPet();
    KV_REQUIRE(oa && ob);
    KV_CHECK(oa->role == PetRole::Owner && oa->deviceId == idOf(a) && oa->deviceName == "Owner's iPhone" && oa->liveAllowed &&
             oa->watching && oa->liveTier == "high" && oa->liveQuality == "auto" && oa->sharingEnabled && !oa->privacyMode &&
             !oa->outsideEnabled && !oa->rollAvailable && oa->motionSensitivity == 5);
    KV_REQUIRE(oa->devices && oa->watchingNow);
    KV_CHECK(oa->devices->size() == 3 && oa->watchingNow->size() == 3);
    for (const auto& d : *oa->devices) KV_CHECK(d.connected && d.lastSeenUnixMs.has_value());
    KV_CHECK(ob->role == PetRole::Viewer && ob->liveAllowed && ob->watching && !ob->devices && !ob->watchingNow);
    // Per-session 0x07: the owner's allows what the PC allows; the viewer's says why it can't.
    KV_CHECK(la.states.back().allowPhoneModeSwitch && la.states.back().allowPhoneSensorPower);
    const HostStateSyncMessage& vs = lb.states.back();
    KV_CHECK(!vs.allowPhoneModeSwitch && !vs.allowPhoneSensorPower && vs.sensorPowerDisabledReason == std::string(text::kViewerPower) &&
             !vs.canRecordCurrentMode && vs.recordingDisabledReason == std::string(text::kViewerRecording) &&
             vs.powerState == SensorPowerState::On);
    KV_CHECK(!lb.hellos.front().allowPhoneModeSwitch && la.hellos.front().allowPhoneModeSwitch);
    KV_CHECK(srv.watchingNow().size() == 3);
}

KV_TEST(v3_a_viewer_v2_phone_cant_switch_power_or_record) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-viewer-v2"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    srv.setAllowPhoneModeSwitch(true); // the PC allows it, for owners only
    FakePhone phone(2);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairAndConnect(srv, phone, pr, PetRole::Viewer));
    PhoneRunner r(phone);
    r.start();
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return !l.states.empty() && l.aus >= 5; }, 4000));
    const int sent_ms = r.ms();
    SelectModeMessage sel;
    sel.modeRaw = KV_MODE_DEPTH;
    r.send(MsgType::SelectModeReq, toJson(sel));
    RemoteControlCommandMessage rc;
    rc.action = RemoteControlAction::SelectMode;
    rc.modeRaw = KV_MODE_DEPTH;
    r.send(MsgType::RemoteControlCmd, toJson(rc));
    RemoteControlCommandMessage rec;
    rec.action = RemoteControlAction::StartRecording;
    r.send(MsgType::RemoteControlCmd, toJson(rec));
    Orbit3DGestureMessage orbit;
    orbit.deltaYawRadians = 0.2f;
    r.send(MsgType::Orbit3DGesture, toJson(orbit));
    SensorPowerControlMessage off;
    off.requestId = 3;
    off.turnOn = false;
    r.send(MsgType::SensorPowerReq, toJson(off));
    RemoteControlCommandMessage key; // request_keyframe: allowed for every role
    r.send(MsgType::RemoteControlCmd, toJson(key));
    sleepMs(1500);
    r.stop();
    KV_CHECK(host.drain());
    const V3Log l = r.log();
    std::printf("  viewer v2: states=%zu aus=%d commands=%d keyframes=%d orbits=%d processed=%d\n", l.states.size(), l.aus,
                host.commands.load(), host.keyframe_commands.load(), host.orbits.load(), host.processed.load());
    KV_REQUIRE(l.got_hello);
    KV_CHECK(!l.hellos.front().allowPhoneModeSwitch); // v1 and v2 phones hide the picker from hello
    for (const auto& s : l.states) {
        KV_CHECK(!s.allowPhoneModeSwitch && !s.allowPhoneSensorPower);
        KV_CHECK(s.sensorPowerDisabledReason == std::string(text::kViewerPower));
        KV_CHECK(!s.canRecordCurrentMode && s.recordingDisabledReason == std::string(text::kViewerRecording));
        KV_CHECK(s.powerState == SensorPowerState::On);
    }
    KV_CHECK(countIn(l.state_ms, sent_ms, 1 << 30) >= 1); // the 0x0A was refused with a 0x07
    KV_CHECK(host.commands.load() == 1 && host.keyframe_commands.load() == 1); // only request_keyframe came through
    KV_CHECK(host.orbits.load() == 0);
    KV_CHECK(host.processed.load() == 1); // only the start-up On reached the power model
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On);
    KV_CHECK(host.mode_requests.load() == 0);
    KV_CHECK(l.aus >= 20 && !l.closed_by_host);
}

// A close after a flush reads and drops what the device still sends, so a reset can't eat its 0x0F. But a device that
// never stops sending must still be closed on time: at kCloseLingerMs (2 s) at the latest, and as soon as
// kCloseLingerMaxBytes (1 MiB) was drained. The phone reads its 0x0F and the host's close_notify (the host is then in
// its linger), and from there floods 64 KiB writes as fast as it can: over 1 MiB in a few milliseconds, so the cap
// closes it well within 1 s. The first version of the drain had no cap (it closed at 2 s here) and skipped its deadline check
// whenever data had arrived; on loopback the reads still find the buffer empty now and then, so that half is shown by
// the code (the check now runs on every pass) rather than by this test.
KV_TEST(v3_a_device_that_keeps_sending_after_its_notice_is_closed_at_the_drain_cap) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-linger-flood"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone phone(3);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairDevice(srv, phone, PetRole::Viewer, pr));
    KV_REQUIRE(connectDevice(phone, pr));
    // A v3 session: its ping says so (the host sends a 0x0F only to v3 sessions), then wait for its first 0x0B.
    PingMessage ping;
    ping.clientSendNs = nowNs();
    ping.protocolVersion = phone.pingVersion();
    KV_REQUIRE(phone.sendFrame(MsgType::Ping, toJson(ping)));
    bool pet = false;
    const auto pet_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pet && std::chrono::steady_clock::now() < pet_until) {
        MsgType t;
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        if (phone.readFrame(t, flags, payload, 200) && t == MsgType::PetCameraState) pet = true;
    }
    KV_REQUIRE(pet);
    KV_REQUIRE(srv.revokeDevice(idOf(phone)));
    bool notice = false, closed = false;
    const auto read_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!closed && std::chrono::steady_clock::now() < read_until) {
        MsgType t;
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        if (phone.readFrame(t, flags, payload, 200)) {
            if (t == MsgType::SessionNotice) notice = true;
        } else if (phone.lastReadWasClose()) {
            closed = true; // the host's close_notify: it is in its close-after-flush linger now
        }
    }
    KV_CHECK_MSG(notice && closed, "no 0x0F and close_notify before the flood");
    KV_REQUIRE(notice && closed);
    const std::vector<uint8_t> chunk(64 * 1024, 0x5A);
    const auto t0 = std::chrono::steady_clock::now();
    const auto give_up = t0 + std::chrono::seconds(10);
    uint64_t sent = 0;
    while (std::chrono::steady_clock::now() < give_up && phone.sendBytes(chunk)) sent += chunk.size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  the host closed a flooding device after %.0f ms (%llu bytes sent)\n", ms, static_cast<unsigned long long>(sent));
    KV_CHECK_MSG(sent >= (1u << 20), "the flood didn't reach the cap: " + std::to_string(sent) + " bytes");
    KV_CHECK_MSG(ms < 1000.0, "a device that kept sending held the connection for " + std::to_string(ms) + " ms");
}

KV_TEST(v3_revoke_closes_with_a_notice_refuses_the_handshake_and_wins_against_a_held_one) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-revoke"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone owner(3), viewer(3), other(3), held(3);
    FakePhone::PairResult po, pv, pw, ph;
    KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
    KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
    KV_REQUIRE(pairDevice(srv, other, PetRole::Viewer, pw));
    KV_REQUIRE(pairDevice(srv, held, PetRole::Viewer, ph));
    const std::string sid = srv.serviceId();
    KV_REQUIRE(connectDevice(owner, po) && connectDevice(viewer, pv) && connectDevice(other, pw));
    PhoneRunner ro(owner), rv(viewer), rw(other);
    ro.start();
    rv.start();
    rw.start();
    for (PhoneRunner* r : {&ro, &rv, &rw}) KV_REQUIRE(r->waitFor([](const V3Log& l) { return !l.pets.empty(); }, 4000));

    // From the PC's list: 0x0F revoked, then closed.
    KV_REQUIRE(srv.revokeDevice(idOf(viewer)));
    KV_CHECK(rv.waitFor([](const V3Log& l) { return l.closed_by_host; }, 3000));
    {
        const V3Log l = rv.log();
        KV_REQUIRE(!l.notices.empty());
        KV_CHECK(l.notices.back().reason == "revoked" && l.notices.back().closing &&
                 l.notices.back().message == std::string(text::kRevoked));
    }
    KV_CHECK(srv.serviceId() == sid && srv.isPhonePaired()); // devices remain: no rotation, still advertised
    KV_CHECK(ro.waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->devices && l.lastPet()->devices->size() == 3; }, 2000));
    // It is refused in the handshake from now on.
    rv.stop();
    const uint64_t rejected = srv.rejectedHandshakeCount();
    StreamStats again;
    if (connectDevice(viewer, pv)) again = runStream(viewer, 800);
    KV_CHECK(!again.got_hello && srv.rejectedHandshakeCount() > rejected);
    KV_CHECK(logHasLine(srv, "refused_unknown_cert", "home", "a certificate that matches no device")); // v3 §8.6

    // From an owner: 0x0C revoke_device, answered before the close; itself and strangers are refused.
    ro.send(MsgType::PetCameraCmd, cmdJson(1, pet_action::kRevokeDevice, std::nullopt, idOf(owner)));
    ro.send(MsgType::PetCameraCmd, cmdJson(2, pet_action::kRevokeDevice, std::nullopt, std::string("0123456789abcdef")));
    ro.send(MsgType::PetCameraCmd, cmdJson(3, pet_action::kRevokeDevice, std::nullopt, idOf(other)));
    ro.send(MsgType::PetCameraCmd, cmdJson(4, pet_action::kRevokeDevice)); // no deviceId
    KV_CHECK(ro.waitFor([](const V3Log& l) { return l.result(1) && l.result(2) && l.result(3) && l.result(4); }, 3000));
    KV_CHECK(rw.waitFor([](const V3Log& l) { return l.closed_by_host; }, 3000));
    {
        const V3Log l = ro.log();
        KV_CHECK(l.result(1) && !l.result(1)->ok && l.result(1)->error == std::string("invalid"));
        KV_CHECK(l.result(2) && !l.result(2)->ok && l.result(2)->error == std::string("not_found"));
        KV_CHECK(l.result(3) && l.result(3)->ok);
        KV_CHECK(l.result(4) && !l.result(4)->ok && l.result(4)->error == std::string("invalid"));
        KV_CHECK(!l.closed_by_host);
        KV_CHECK(rw.log().hasNotice("revoked"));
    }
    // v3 §3.2 step 4: both revokes are logged, the PC's and the owner device's, each naming who did it.
    KV_CHECK(logHasLine(srv, [&](const outside::LogLine& l) {
        return l.event == "revoked" && l.deviceId == idOf(viewer) && l.detail == "on the PC";
    }));
    KV_CHECK(logHasLine(srv, [&](const outside::LogLine& l) {
        return l.event == "revoked" && l.deviceId == idOf(other) && l.path == "home" && !l.addr.empty() &&
               l.detail == "by the owner device " + idOf(owner);
    }));

    // A handshake held between the verify callback and on_ready while revoke runs never becomes a session.
    std::mutex hm;
    std::condition_variable hcv;
    bool armed = true, in_hook = false, release = false;
    srv.testHookBeforeSessionReady = [&] {
        std::unique_lock<std::mutex> lk(hm);
        if (!armed) return;
        armed = false;
        in_hook = true;
        hcv.notify_all();
        hcv.wait_for(lk, std::chrono::seconds(5), [&] { return release; });
    };
    StreamStats hs;
    bool tls = false;
    std::thread t([&] {
        tls = connectDevice(held, ph);
        if (tls) hs = runStream(held, 1500);
    });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lk(hm);
        entered = hcv.wait_for(lk, std::chrono::seconds(5), [&] { return in_hook; });
    }
    KV_CHECK(entered); // the leaf passed verify (it was listed) and on_ready hasn't run yet
    KV_CHECK(srv.revokeDevice(idOf(held)));
    {
        std::lock_guard<std::mutex> lk(hm);
        release = true;
    }
    hcv.notify_all();
    t.join();
    srv.testHookBeforeSessionReady = nullptr;
    std::printf("  held handshake: tls=%d hello=%d; sessions now %d\n", tls ? 1 : 0, hs.got_hello ? 1 : 0, srv.sessionCount());
    KV_CHECK(!hs.got_hello);
    KV_CHECK(srv.sessionCount() == 1); // only the owner
    KV_CHECK(logHasLine(srv, "refused_unknown_cert", "home", "its device was removed during the handshake"));
    ro.stop();
}

// v3 §6.2: a session's 0x08 commands are held to a bucket of 8 that refills at 4 a second. A viewer flooding
// request_keyframe is held to that, and is never closed for it; one asking now and then never meets it.
KV_TEST(v3_a_flood_of_keyframe_requests_is_held_to_the_command_bucket) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-cmd-bucket"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone viewer(3);
    FakePhone::PairResult pv;
    KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
    KV_REQUIRE(connectDevice(viewer, pv));
    PhoneRunner rv(viewer);
    rv.start();
    KV_REQUIRE(rv.waitFor([](const V3Log& l) { return !l.pets.empty() && l.aus >= 3; }, 4000));
    const std::string key = toJson(RemoteControlCommandMessage{}); // request_keyframe is its default action
    const uint64_t dropped0 = srv.droppedCommandCount();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40; ++i) rv.send(MsgType::RemoteControlCmd, key);
    const int pongs = rv.log().pongs;
    // Pinged after them, and answered: the host has read them all.
    KV_REQUIRE(rv.waitFor([&](const V3Log& l) { return l.pongs >= pongs + 2; }, 4000));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const uint64_t dropped = srv.droppedCommandCount() - dropped0;
    std::printf("  40 requests: %llu dropped, read within %.2f s\n", static_cast<unsigned long long>(dropped), secs);
    // The burst of 8 always goes through, then at most what refilled meanwhile; the rest are dropped.
    KV_CHECK(dropped <= 32);
    KV_CHECK_MSG(static_cast<double>(dropped) >= 32.0 - 4.0 * secs - 1.0, std::to_string(dropped));
    // A second later the bucket has room again: a few requests all go through.
    sleepMs(1100);
    const uint64_t before = srv.droppedCommandCount();
    for (int i = 0; i < 4; ++i) rv.send(MsgType::RemoteControlCmd, key);
    const int p2 = rv.log().pongs;
    KV_REQUIRE(rv.waitFor([&](const V3Log& l) { return l.pongs >= p2 + 2; }, 4000));
    KV_CHECK(srv.droppedCommandCount() == before);
    KV_CHECK(!rv.log().closed_by_host);
    rv.stop();
}

// v3 §6.2: the bucket is the device's, kept by its certificate's hash, so a reconnect doesn't bring a fresh burst.
// With the refill turned off (the test seam) it is exact: 8 commands in all, however many sessions they come over.
KV_TEST(v3_a_reconnect_doesnt_refill_the_command_bucket) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-cmd-reconnect"), &err, 5000,
                          [](HostController::Options& o) { o.server.command_refill_per_second_for_test = 0; }));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone viewer(3);
    FakePhone::PairResult pv;
    KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
    const std::string key = toJson(RemoteControlCommandMessage{}); // request_keyframe
    for (int round = 0; round < 3; ++round) {
        KV_REQUIRE(connectDevice(viewer, pv));
        PhoneRunner rv(viewer);
        rv.start();
        KV_REQUIRE(rv.waitFor([](const V3Log& l) { return !l.pets.empty(); }, 4000));
        for (int i = 0; i < 6; ++i) rv.send(MsgType::RemoteControlCmd, key);
        const int pongs = rv.log().pongs;
        KV_REQUIRE(rv.waitFor([&](const V3Log& l) { return l.pongs >= pongs + 2; }, 4000)); // all six read
        rv.stop();
        viewer.close();
    }
    std::printf("  18 requests over 3 sessions: %d handled, %llu dropped\n", host.commands.load(),
                static_cast<unsigned long long>(srv.droppedCommandCount()));
    KV_CHECK_MSG(host.commands.load() == 8, std::to_string(host.commands.load()));
    KV_CHECK(srv.droppedCommandCount() == 10);
}

KV_TEST(v3_a_demoted_owner_is_refused_owner_commands_without_reconnecting) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-role"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone d(3);
    FakePhone::PairResult pd;
    KV_REQUIRE(pairDevice(srv, d, PetRole::Owner, pd));
    KV_REQUIRE(connectDevice(d, pd));
    PhoneRunner r(d);
    r.start();
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return !l.pets.empty() && l.aus >= 3; }, 4000));
    r.send(MsgType::PetCameraCmd, cmdJson(1, pet_action::kSetPrivacy, false));
    r.send(MsgType::PetCameraCmd, cmdJson(2, "teleport"));                 // unknown action
    r.send(MsgType::PetCameraCmd, cmdJson(3, pet_action::kSetRoll, true)); // W7
    r.send(MsgType::PetCameraCmd, cmdJson(4, pet_action::kStartClip));
    r.send(MsgType::PetCameraCmd, "{\"action\":\"set_privacy\",\"enabled\":true}"); // no requestId: not answered
    r.send(MsgType::PetCameraCmd, cmdJson(5, pet_action::kSetSharing));             // no "enabled": invalid
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return l.results.size() >= 5; }, 3000));
    sleepMs(300);
    {
        const V3Log l = r.log();
        KV_CHECK(l.results.size() == 5);
        KV_CHECK(l.result(1) && l.result(1)->ok);
        KV_CHECK(l.result(2) && !l.result(2)->ok && l.result(2)->error == std::string("unsupported"));
        KV_CHECK(l.result(3) && l.result(3)->error == std::string("unsupported"));
        KV_CHECK(l.result(4) && l.result(4)->error == std::string("unsupported"));
        KV_CHECK(l.result(5) && l.result(5)->error == std::string("invalid"));
    }
    KV_CHECK(!srv.petSettings().privacyMode);
    const size_t hellos_before = r.log().hellos.size();
    // Demoted on the PC: a new hello, 0x07 and 0x0B at once, and owner-only keys leave the 0x0B.
    KV_REQUIRE(srv.setDeviceRole(idOf(d), PetRole::Viewer));
    KV_CHECK(r.waitFor([&](const V3Log& l) {
        return l.hellos.size() > hellos_before && l.lastPet() && l.lastPet()->role == PetRole::Viewer;
    }, 2000));
    r.send(MsgType::PetCameraCmd, cmdJson(6, pet_action::kSetPrivacy, true));
    r.send(MsgType::PetCameraCmd, cmdJson(7, pet_action::kRevokeDevice, std::nullopt, std::string("0123456789abcdef")));
    r.send(MsgType::PetCameraCmd, cmdJson(8, pet_action::kSetSharing, true));
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return l.result(6) && l.result(7) && l.result(8); }, 3000));
    r.stop();
    const V3Log l = r.log();
    KV_CHECK(!l.hellos.back().allowPhoneModeSwitch);
    KV_CHECK(!l.states.back().allowPhoneModeSwitch && l.states.back().sensorPowerDisabledReason == std::string(text::kViewerPower));
    const PetCameraStateMessage* p = l.lastPet();
    KV_REQUIRE(p);
    KV_CHECK(p->role == PetRole::Viewer && !p->devices && !p->watchingNow);
    // Sharing is off (the default): a viewer is unavailable, and it says why.
    KV_CHECK(!p->liveAllowed && p->liveUnavailableReason == std::string("sharing_off") && !p->watching);
    for (uint64_t id : {6, 7, 8}) {
        KV_CHECK(l.result(id) && !l.result(id)->ok && l.result(id)->error == std::string("not_allowed_for_role"));
    }
    KV_CHECK(!srv.petSettings().privacyMode && !srv.petSettings().sharingEnabled);
    KV_CHECK(!l.closed_by_host && srv.sessionCount() == 1);
}

KV_TEST(v3_with_four_viewers_watching_an_owner_takes_the_newest_viewer_slot) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-slots"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    std::vector<std::unique_ptr<FakePhone>> viewers;
    std::vector<FakePhone::PairResult> prs;
    for (int i = 0; i < 4; ++i) {
        viewers.push_back(std::make_unique<FakePhone>(3));
        prs.emplace_back();
        KV_REQUIRE(pairDevice(srv, *viewers.back(), PetRole::Viewer, prs.back()));
    }
    FakePhone owner(2);
    FakePhone::PairResult po;
    KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
    std::vector<std::unique_ptr<PhoneRunner>> runners;
    for (int i = 0; i < 4; ++i) {
        KV_REQUIRE(connectDevice(*viewers[i], prs[i]));
        runners.push_back(std::make_unique<PhoneRunner>(*viewers[i]));
        runners.back()->start();
        // One at a time, so the slots are taken in a known order.
        KV_REQUIRE(runners.back()->waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->watching && l.aus >= 2; }, 4000));
    }
    KV_CHECK(srv.watchingNow().size() == 4);
    KV_REQUIRE(connectDevice(owner, po));
    PhoneRunner ro(owner);
    ro.start();
    const int t_owner = runners[3]->ms();
    KV_CHECK(ro.waitFor([](const V3Log& l) { return l.configs.size() >= 1 && l.decoded >= 5; }, 4000));
    KV_CHECK(runners[3]->waitFor([](const V3Log& l) {
        return l.lastPet() && !l.lastPet()->liveAllowed && !l.lastPet()->watching;
    }, 3000));
    sleepMs(800);
    const V3Log lo = ro.log();
    const V3Log l4 = runners[3]->log();
    std::printf("  owner v2: configs=%zu first_key=%d decoded=%d; newest viewer: reason=%s aus after +400 ms=%d\n",
                lo.configs.size(), lo.first_au_key ? 1 : 0, lo.decoded, l4.lastPet() && l4.lastPet()->liveUnavailableReason
                    ? l4.lastPet()->liveUnavailableReason->c_str() : "-", l4.ausFrom(t_owner + 400));
    KV_CHECK(lo.first_au_key && lo.not_idr_after_cfg == 0 && lo.au_epoch_mismatch == 0);
    KV_CHECK(l4.lastPet() && l4.lastPet()->liveUnavailableReason == std::string("too_many_viewers"));
    KV_CHECK(l4.ausFrom(t_owner + 600) == 0);
    for (int i = 0; i < 3; ++i) {
        const V3Log l = runners[i]->log();
        KV_CHECK(l.lastPet() && l.lastPet()->watching && l.lastPet()->liveAllowed);
    }
    const auto watching = srv.watchingNow();
    KV_CHECK(watching.size() == 4);
    bool owner_watching = false;
    for (const auto& w : watching) owner_watching = owner_watching || (w.deviceId == idOf(owner) && w.role == PetRole::Owner);
    KV_CHECK(owner_watching);
    // The owner leaves: the viewer gets its slot back, with a 0x02 and an IDR.
    const size_t cfgs = l4.configs.size();
    ro.stop();
    owner.close();
    KV_CHECK(runners[3]->waitFor([&](const V3Log& l) {
        return l.lastPet() && l.lastPet()->watching && l.configs.size() > cfgs && l.not_idr_after_cfg == 0;
    }, 4000));
    for (auto& r : runners) r->stop();
}

KV_TEST(v3_eight_sessions_owner_takes_the_newest_viewers_place_and_the_ninth_is_turned_away) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-eight"), &err, 8000));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    std::vector<std::unique_ptr<FakePhone>> phones; // 0-3 v2 viewers, 4-7 v3 viewers
    std::vector<FakePhone::PairResult> prs(8);
    for (int i = 0; i < 8; ++i) {
        phones.push_back(std::make_unique<FakePhone>(i < 4 ? 2 : 3));
        KV_REQUIRE(pairDevice(srv, *phones.back(), PetRole::Viewer, prs[static_cast<size_t>(i)]));
    }
    FakePhone ninth_v2(2), ninth_v3(3), owner(2);
    FakePhone::PairResult p9a, p9b, po;
    KV_REQUIRE(pairDevice(srv, ninth_v2, PetRole::Viewer, p9a)); // lastProtocolVersion: none
    KV_REQUIRE(pairDevice(srv, ninth_v3, PetRole::Viewer, p9b)); // paired by a v3 app: lastProtocolVersion 3
    KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
    std::vector<std::unique_ptr<PhoneRunner>> runners;
    for (int i = 0; i < 8; ++i) {
        KV_REQUIRE(connectDevice(*phones[static_cast<size_t>(i)], prs[static_cast<size_t>(i)]));
        runners.push_back(std::make_unique<PhoneRunner>(*phones[static_cast<size_t>(i)]));
        runners.back()->start();
        KV_REQUIRE(runners.back()->waitFor([](const V3Log& l) { return l.got_hello && !l.states.empty(); }, 4000));
    }
    KV_REQUIRE(waitUntil([&] { return srv.sessionCount() == 8; }, 2000));

    // A 9th session from a device without lastProtocolVersion 3: closed before hello.
    KV_REQUIRE(connectDevice(ninth_v2, p9a));
    PhoneRunner r9a(ninth_v2);
    r9a.start();
    KV_CHECK(r9a.waitFor([](const V3Log& l) { return l.closed_by_host; }, 3000));
    r9a.stop();
    KV_CHECK(!r9a.log().got_hello);
    // A 9th v3 device with lastProtocolVersion 3: the notice-only connection.
    KV_REQUIRE(connectDevice(ninth_v3, p9b));
    PhoneRunner r9b(ninth_v3);
    r9b.start();
    KV_CHECK(r9b.waitFor([](const V3Log& l) { return l.closed_by_host; }, 3000));
    r9b.stop();
    {
        const V3Log l = r9b.log();
        KV_CHECK(l.got_hello && l.hellos.front().petCamera);
        KV_REQUIRE(l.notices.size() == 1);
        KV_CHECK(l.notices[0].reason == "too_many_sessions" && l.notices[0].closing &&
                 l.notices[0].message == std::string(text::kTooManySessions));
        KV_CHECK(l.configs.empty() && l.aus == 0 && l.states.empty() && l.pets.empty());
    }
    // ...and without a ping it is closed after 1 s.
    KV_REQUIRE(connectDevice(ninth_v3, p9b));
    PhoneRunner r9c(ninth_v3, /*send_pings=*/false);
    r9c.start();
    KV_CHECK(r9c.waitFor([](const V3Log& l) { return l.closed_by_host; }, 3000));
    r9c.stop();
    {
        const V3Log l = r9c.log();
        KV_CHECK(l.got_hello && l.notices.empty() && l.closed_ms >= 700 && l.closed_ms < 2500);
    }
    KV_CHECK(srv.sessionCount() == 8);

    // An owner's v2 phone takes the place of the most recently connected viewer, and streams.
    KV_REQUIRE(connectDevice(owner, po));
    PhoneRunner ro(owner);
    ro.start();
    KV_CHECK(ro.waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    KV_CHECK(runners[7]->waitFor([](const V3Log& l) { return l.closed_by_host; }, 3000));
    const V3Log l8 = runners[7]->log();
    KV_CHECK(l8.hasNotice("too_many_sessions"));
    for (int i = 0; i < 7; ++i) KV_CHECK(!runners[static_cast<size_t>(i)]->log().closed_by_host);
    KV_CHECK(srv.sessionCount() == 8);
    const V3Log lo = ro.log();
    std::printf("  owner: first_key=%d decoded=%d; sessions=%d\n", lo.first_au_key ? 1 : 0, lo.decoded, srv.sessionCount());
    KV_CHECK(lo.first_au_key && lo.not_idr_after_cfg == 0 && !lo.states.empty() && lo.states.back().powerState == SensorPowerState::On);
    ro.stop();
    for (auto& r : runners) r->stop();
}

KV_TEST(v3_privacy_mode_stops_video_closes_v1_shows_v2_off_and_keeps_the_sensor_on) {
    PoweredHost host;
    std::string err;
    const std::wstring dir = freshStateDir(L"v3-privacy");
    KV_REQUIRE(host.start(dir, &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setTurnOffKinectOnPhoneDisconnect(true);
    FakePhone p1(1), p2(2), p3(3);
    FakePhone::PairResult r1, r2, r3;
    KV_REQUIRE(pairDevice(srv, p1, PetRole::Owner, r1));
    KV_REQUIRE(pairDevice(srv, p2, PetRole::Owner, r2));
    KV_REQUIRE(pairDevice(srv, p3, PetRole::Owner, r3));
    KV_REQUIRE(connectDevice(p1, r1) && connectDevice(p2, r2) && connectDevice(p3, r3));
    PhoneRunner a(p1), b(p2), c(p3);
    a.start();
    b.start();
    c.start();
    for (PhoneRunner* r : {&a, &b, &c}) KV_REQUIRE(r->waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    const int t_b = b.ms(), t_c = c.ms();
    srv.setPrivacyMode(true);
    KV_CHECK(loadPetSettings(dir).privacyMode); // saved across restarts
    KV_CHECK(a.waitFor([](const V3Log& l) { return l.closed_by_host; }, 2000)); // v1: closed
    KV_CHECK(b.waitFor([](const V3Log& l) {
        return !l.states.empty() && l.states.back().powerState == SensorPowerState::Off;
    }, 2000));
    KV_CHECK(c.waitFor([](const V3Log& l) { return l.lastPet() && !l.lastPet()->liveAllowed; }, 2000));
    sleepMs(1200);
    {
        const V3Log lb = b.log(), lc = c.log();
        const HostStateSyncMessage& s = lb.states.back();
        KV_CHECK(s.powerState == SensorPowerState::Off && !s.allowPhoneSensorPower &&
                 s.sensorPowerDisabledReason == std::string(text::kPrivacyMode));
        const PetCameraStateMessage* p = lc.lastPet();
        KV_CHECK(p && p->privacyMode && !p->watching && p->liveUnavailableReason == std::string("privacy_mode"));
        std::printf("  privacy on: v2 aus after +300 ms=%d, v3 aus after +300 ms=%d, v1 closed=%d\n", lb.ausFrom(t_b + 300),
                    lc.ausFrom(t_c + 300), a.log().closed_by_host ? 1 : 0);
        KV_CHECK(lb.ausFrom(t_b + 300) == 0 && lc.ausFrom(t_c + 300) == 0);
        KV_CHECK(!lb.closed_by_host && !lc.closed_by_host); // v2 and v3 stay connected
    }
    KV_CHECK(host.drain());
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On && host.isOpen()); // the sensor stays on
    KV_CHECK(host.disconnects.load() == 0);
    // Off again: the v2 phone gets its real state, then a 0x02 and a keyframe; the v3 device too.
    const size_t cfg_b = b.log().configs.size(), cfg_c = c.log().configs.size();
    srv.setPrivacyMode(false);
    KV_CHECK(b.waitFor([&](const V3Log& l) {
        return l.states.back().powerState == SensorPowerState::On && l.configs.size() > cfg_b && l.decoded >= 10;
    }, 4000));
    KV_CHECK(c.waitFor([&](const V3Log& l) { return l.lastPet()->watching && l.configs.size() > cfg_c; }, 4000));
    KV_CHECK(b.log().not_idr_after_cfg == 0 && c.log().not_idr_after_cfg == 0);
    // On again, then the unavailable sessions end: "turn off when the last session ends" doesn't fire.
    srv.setPrivacyMode(true);
    KV_CHECK(b.waitFor([](const V3Log& l) { return l.states.back().powerState == SensorPowerState::Off; }, 2000));
    a.stop();
    b.stop();
    c.stop();
    p2.close();
    p3.close();
    KV_CHECK(waitUntil([&] { return srv.sessionCount() == 0; }, 3000));
    sleepMs(200);
    KV_CHECK(host.drain());
    KV_CHECK(host.disconnects.load() == 0);
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On && host.isOpen());
}

KV_TEST(v3_the_same_device_reconnecting_replaces_only_its_own_session) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-replace"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setTurnOffKinectOnPhoneDisconnect(true); // a wrongly fired hook would turn the Kinect off
    FakePhone a(3), b(3);
    FakePhone::PairResult pa, pb;
    KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pa));
    KV_REQUIRE(pairDevice(srv, b, PetRole::Owner, pb));
    KV_REQUIRE(connectDevice(a, pa) && connectDevice(b, pb));
    PhoneRunner ra(a), rb(b);
    ra.start();
    rb.start();
    KV_REQUIRE(ra.waitFor([](const V3Log& l) { return l.decoded >= 5 && !l.pets.empty(); }, 4000));
    KV_REQUIRE(rb.waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    FakePhone a2(3, a.identity());
    KV_REQUIRE(connectDevice(a2, pa));
    PhoneRunner ra2(a2);
    ra2.start();
    const int t_b = rb.ms();
    KV_CHECK(ra.waitFor([](const V3Log& l) { return l.closed_by_host; }, 3000));
    KV_CHECK(ra2.waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    sleepMs(800);
    const V3Log la = ra.log(), lb = rb.log();
    KV_CHECK(la.hasNotice("replaced"));
    KV_CHECK(!la.notices.empty() && la.notices.back().closing && la.notices.back().message == std::string(text::kReplaced));
    KV_CHECK(!lb.closed_by_host && lb.ausFrom(t_b) >= 20 && lb.notices.empty()); // the other device carries on
    KV_CHECK(srv.sessionCount() == 2);
    ra.stop();
    rb.stop();
    ra2.stop();
    KV_CHECK(host.drain());
    KV_CHECK(host.disconnects.load() == 0); // a replacement is not a disconnect
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On);
}

KV_TEST(v3_tiers_config_before_idr_unique_epochs_and_clean_switches) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-tiers"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone h(3), m(3), lw(3);
    FakePhone::PairResult ph, pm, pl;
    KV_REQUIRE(pairDevice(srv, h, PetRole::Owner, ph));
    KV_REQUIRE(pairDevice(srv, m, PetRole::Owner, pm));
    KV_REQUIRE(pairDevice(srv, lw, PetRole::Owner, pl));
    KV_REQUIRE(connectDevice(h, ph) && connectDevice(m, pm) && connectDevice(lw, pl));
    PhoneRunner rh(h), rm(m), rl(lw);
    rh.start();
    rm.start();
    rl.start();
    for (PhoneRunner* r : {&rh, &rm, &rl}) KV_REQUIRE(r->waitFor([](const V3Log& l) { return l.decoded >= 5 && !l.pets.empty(); }, 4000));
    // Opening low (and medium) doesn't stall high.
    const int t_open = rh.ms();
    rl.send(MsgType::LiveViewReq, watchJson(true, std::string("low")));
    rm.send(MsgType::LiveViewReq, watchJson(true, std::string("medium")));
    KV_CHECK(rl.waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->liveTier == "low" && l.decoded >= 10; }, 5000));
    KV_CHECK(rm.waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->liveTier == "medium" && l.decoded >= 10; }, 5000));
    sleepMs(800);
    {
        const V3Log l = rh.log();
        int max_gap = 0;
        int prev = -1;
        for (int t : l.au_ms) {
            if (t < t_open - 200) continue;
            if (prev >= 0 && t - prev > max_gap) max_gap = t - prev;
            prev = t;
            if (t > t_open + 2000) break;
        }
        std::printf("  high's longest gap while low and medium opened: %d ms\n", max_gap);
        KV_CHECK(max_gap < 400);
    }
    // high switches with frames in flight: low, medium, then auto (back to high).
    const char* steps[] = {"low", "medium", "auto"};
    for (const char* q : steps) {
        rh.send(MsgType::LiveViewReq, watchJson(true, std::string(q)));
        const std::string want = std::string(q) == "auto" ? "high" : q;
        KV_CHECK(rh.waitFor([&](const V3Log& l) { return l.lastPet() && l.lastPet()->liveTier == want; }, 3000));
        sleepMs(900);
    }
    sleepMs(500);
    rh.stop();
    rm.stop();
    rl.stop();
    const V3Log lh = rh.log(), lm = rm.log(), ll = rl.log();
    std::printf("  high-path device: configs=%zu aus=%d decoded=%d mismatched=%d not_idr=%d; medium aus=%d; low aus=%d\n",
                lh.configs.size(), lh.aus, lh.decoded, lh.au_epoch_mismatch, lh.not_idr_after_cfg, lm.aus, ll.aus);
    for (const V3Log* l : {&lh, &lm, &ll}) {
        KV_CHECK(l->au_epoch_mismatch == 0); // no AU of an old tier after the new tier's 0x02
        KV_CHECK(l->not_idr_after_cfg == 0); // each tier's 0x02 comes before its IDR
        KV_CHECK(l->cfg_epoch_went_down == 0);
        KV_CHECK(l->decoded >= l->aus - 4 && l->aus >= 10);
        KV_CHECK(petSeqNeverGoesDown(*l));
    }
    KV_CHECK(lm.configs.back().width <= 1280 && ll.configs.back().width <= 640 && ll.configs.back().height <= 640);
    KV_CHECK(ll.configs.back().width < lh.configs.back().width);
    // Epochs are unique across tiers: one epoch always means one tier's size.
    std::map<uint32_t, std::pair<int, int>> sizes;
    bool unique = true;
    for (const V3Log* l : {&lh, &lm, &ll}) {
        for (const auto& c : l->configs) {
            auto it = sizes.find(c.streamEpoch);
            if (it == sizes.end()) sizes[c.streamEpoch] = {c.width, c.height};
            else unique = unique && it->second == std::make_pair(c.width, c.height);
        }
    }
    KV_CHECK(unique);
    // ...and one tier: the same epoch never reached two sessions on different tiers.
    std::map<uint32_t, std::string> tier_of;
    std::set<std::string> tiers_seen;
    bool one_tier = true;
    for (const V3Log* l : {&lh, &lm, &ll}) {
        for (size_t i = 0; i < l->configs.size(); ++i) {
            const uint32_t e = l->configs[i].streamEpoch;
            tiers_seen.insert(l->cfg_tier[i]);
            auto it = tier_of.find(e);
            if (it == tier_of.end()) tier_of[e] = l->cfg_tier[i];
            else one_tier = one_tier && it->second == l->cfg_tier[i];
        }
    }
    std::printf("  %zu epochs over %zu tiers\n", tier_of.size(), tiers_seen.size());
    KV_CHECK(one_tier && tiers_seen.size() == 3);
    const auto diag = host.ctl.tierStatus();
    KV_REQUIRE(diag.size() == 3);
    KV_CHECK(diag[0].open && diag[1].open && diag[2].open && !diag[2].encoderName.empty());
    KV_CHECK(diag[0].epoch != diag[1].epoch && diag[1].epoch != diag[2].epoch && diag[0].epoch != diag[2].epoch);
}

KV_TEST(v3_a_slow_session_doesnt_force_keyframes_on_others_faster_than_every_500_ms) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-slow"), &err, 30000));
    HostStreamServer& srv = host.ctl.server();
    FakePhone normal(3), slow(1);
    slow.setSmallReceiveBuffer(true);
    FakePhone::PairResult pn, ps;
    KV_REQUIRE(pairDevice(srv, normal, PetRole::Owner, pn));
    KV_REQUIRE(pairDevice(srv, slow, PetRole::Owner, ps));
    KV_REQUIRE(connectDevice(normal, pn) && connectDevice(slow, ps)); // slow never reads after the handshake
    PhoneRunner rn(normal);
    rn.start();
    KV_REQUIRE(rn.waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    sleepMs(1500); // the slow device's buffers fill
    const uint64_t dropped0 = srv.droppedCongestionFrames();
    const uint64_t forced0 = host.ctl.tierStatus()[0].forcedKeyframes;
    const int t0 = rn.ms();
    sleepMs(4000);
    const int t1 = rn.ms();
    const uint64_t dropped = srv.droppedCongestionFrames() - dropped0;
    const uint64_t forced = host.ctl.tierStatus()[0].forcedKeyframes - forced0;
    rn.stop();
    const V3Log l = rn.log();
    const int keys = countIn(l.key_ms, t0, t1);
    std::printf("  over %d ms: slow-session drops=%llu, forced IDRs on high=%llu, IDRs at the normal session=%d, its aus=%d\n",
                t1 - t0, static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(forced), keys,
                countIn(l.au_ms, t0, t1));
    // The slow session really was dropping: after each drop it waits for its tier's next IDR (v3 §6.3), so
    // it drops about once per IDR, and asks for one each time.
    KV_CHECK(dropped >= 4);
    KV_CHECK(forced <= static_cast<uint64_t>((t1 - t0) / 500 + 2)); // coalesced: at most one per 500 ms
    KV_CHECK(keys <= (t1 - t0) / 500 + (t1 - t0) / 1000 + 3);        // forced ones plus the 1 s GOP
    KV_CHECK(countIn(l.au_ms, t0, t1) >= 80 && l.au_epoch_mismatch == 0 && !l.closed_by_host);
    slow.close();
}

KV_TEST(v3_auto_quality_steps_down_after_drops_and_the_new_tier_decodes) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-auto"), &err, 30000));
    HostStreamServer& srv = host.ctl.server();
    FakePhone d(3);
    d.setSmallReceiveBuffer(true);
    FakePhone::PairResult pd;
    KV_REQUIRE(pairDevice(srv, d, PetRole::Owner, pd));
    KV_REQUIRE(connectDevice(d, pd));
    PhoneRunner r(d);
    r.start();
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return l.decoded >= 5 && l.lastPet() && l.lastPet()->liveTier == "high"; }, 4000));
    const uint64_t dropped0 = srv.droppedCongestionFrames();
    r.setPaused(true); // congested: the host's sends back up and it drops for this session
    const bool stepped = waitUntil([&] { return srv.sessionsOnTier(LiveTier::High) == 0; }, 8000);
    r.setPaused(false);
    KV_CHECK(stepped);
    KV_CHECK(r.waitFor([](const V3Log& l) {
        return l.lastPet() && l.lastPet()->liveQuality == "auto" && l.lastPet()->liveTier != "high";
    }, 4000));
    const size_t cfgs = r.log().configs.size();
    const int t = r.ms();
    KV_CHECK(r.waitFor([&](const V3Log& l) { return l.configs.size() >= cfgs && l.ausFrom(t) >= 10; }, 4000));
    r.stop();
    const V3Log l = r.log();
    std::printf("  auto: drops=%llu, now on %s, configs=%zu, mismatched=%d not_idr=%d\n",
                static_cast<unsigned long long>(srv.droppedCongestionFrames() - dropped0),
                l.lastPet() ? l.lastPet()->liveTier.c_str() : "-", l.configs.size(), l.au_epoch_mismatch, l.not_idr_after_cfg);
    KV_CHECK(srv.droppedCongestionFrames() - dropped0 >= 3);
    KV_CHECK(l.not_idr_after_cfg == 0 && l.cfg_epoch_went_down == 0);
    KV_CHECK(l.configs.back().width < l.configs.front().width || l.lastPet()->liveTier == "medium");
}

KV_TEST(v3_a_session_whose_queue_passes_the_cap_is_closed_as_dead) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-cap"), &err, 30000, [](HostController::Options& o) {
        o.server.send_queue_cap = 256 * 1024; // 16 MiB in the app; small here so it fills in seconds
    }));
    HostStreamServer& srv = host.ctl.server();
    FakePhone normal(2), slow(1);
    slow.setSmallReceiveBuffer(true);
    FakePhone::PairResult pn, ps;
    KV_REQUIRE(pairDevice(srv, normal, PetRole::Owner, pn));
    KV_REQUIRE(pairDevice(srv, slow, PetRole::Owner, ps));
    KV_REQUIRE(connectDevice(normal, pn) && connectDevice(slow, ps));
    PhoneRunner rn(normal);
    rn.start();
    KV_REQUIRE(waitUntil([&] { return srv.sessionCount() == 2; }, 3000));
    const bool closed = waitUntil([&] { return srv.sessionCount() == 1; }, 20000);
    const int t = rn.ms();
    sleepMs(1000);
    rn.stop();
    const V3Log l = rn.log();
    std::printf("  slow session closed=%d; the normal one carried on: aus after=%d\n", closed ? 1 : 0, l.ausFrom(t));
    KV_CHECK(closed);
    KV_CHECK(!l.closed_by_host && l.ausFrom(t) >= 15);
    slow.close();
}

KV_TEST(v3_while_frozen_a_new_v3_session_gets_its_config_and_an_idr) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-freeze"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone a(3), b(3);
    FakePhone::PairResult pa, pb;
    KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pa));
    KV_REQUIRE(pairDevice(srv, b, PetRole::Owner, pb));
    KV_REQUIRE(connectDevice(a, pa));
    PhoneRunner ra(a);
    ra.start();
    KV_REQUIRE(ra.waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    host.frozen = true; // the frozen view feeds no new frames
    sleepMs(700);
    KV_REQUIRE(connectDevice(b, pb));
    PhoneRunner rb(b);
    rb.start();
    const bool got = rb.waitFor([](const V3Log& l) { return !l.configs.empty() && l.aus >= 1; }, 4000);
    // The frozen view sends that one IDR and nothing after it, and the decoder may hold it until more input comes
    // (decoded=0 in one loaded run): drain it, so the check is whether the IDR decodes, not whether it came out yet.
    rb.drainDecoder();
    rb.waitFor([](const V3Log& l) { return l.decoded >= 1; }, 2000);
    rb.stop();
    ra.stop();
    host.frozen = false;
    const V3Log l = rb.log();
    std::printf("  frozen: configs=%zu aus=%d first_key=%d decoded=%d\n", l.configs.size(), l.aus, l.first_au_key ? 1 : 0, l.decoded);
    KV_CHECK(got);
    KV_CHECK(l.first_au_key && l.not_idr_after_cfg == 0 && l.au_epoch_mismatch == 0 && l.decoded >= 1);
}

KV_TEST(v3_during_a_pc_replay_viewers_are_unavailable_and_owners_keep_the_frames) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-replay"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone v3(3), v2(2), owner(2);
    FakePhone::PairResult p3, p2, po;
    KV_REQUIRE(pairDevice(srv, v3, PetRole::Viewer, p3));
    KV_REQUIRE(pairDevice(srv, v2, PetRole::Viewer, p2));
    KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
    KV_REQUIRE(connectDevice(v3, p3) && connectDevice(v2, p2) && connectDevice(owner, po));
    PhoneRunner r3(v3), r2(v2), ro(owner);
    r3.start();
    r2.start();
    ro.start();
    for (PhoneRunner* r : {&r3, &r2, &ro}) KV_REQUIRE(r->waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    HostViewState v = host.ctl.viewState();
    v.isReplayingOnHost = true;
    const int t3 = r3.ms(), t2 = r2.ms(), to = ro.ms();
    host.ctl.setViewState(v);
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.lastPet() && !l.lastPet()->liveAllowed; }, 2000));
    KV_CHECK(r2.waitFor([](const V3Log& l) { return l.states.back().powerState == SensorPowerState::Off; }, 2000));
    sleepMs(1200);
    {
        const V3Log l3 = r3.log(), l2 = r2.log(), lo = ro.log();
        std::printf("  replay: viewer v3 aus after=%d, viewer v2 aus after=%d, owner aus after=%d\n", l3.ausFrom(t3 + 300),
                    l2.ausFrom(t2 + 300), lo.ausFrom(to + 300));
        KV_CHECK(l3.lastPet()->liveUnavailableReason == std::string("pc_replaying"));
        KV_CHECK(l2.states.back().sensorPowerDisabledReason == std::string(text::kPcReplaying));
        KV_CHECK(l3.ausFrom(t3 + 300) == 0 && l2.ausFrom(t2 + 300) == 0);
        KV_CHECK(lo.ausFrom(to + 300) >= 20); // the owner still gets the replay, as v2 phones do today
        KV_CHECK(lo.states.back().isReplayingOnHost && lo.states.back().powerState == SensorPowerState::On);
    }
    v.isReplayingOnHost = false;
    host.ctl.setViewState(v);
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.lastPet()->watching; }, 3000));
    KV_CHECK(r2.waitFor([](const V3Log& l) { return l.states.back().powerState == SensorPowerState::On; }, 3000));
    r3.stop();
    r2.stop();
    ro.stop();
}

KV_TEST(v3_sharing_and_its_schedule_gate_viewers_only) {
    PoweredHost host;
    std::string err;
    const std::wstring dir = freshStateDir(L"v3-sharing");
    KV_REQUIRE(host.start(dir, &err));
    HostStreamServer& srv = host.ctl.server();
    // Wednesday 12:30, inside a "sharing off" row from 12:00 to 13:00 on Wednesdays.
    srv.setLocalTimeForTest(LocalTime{3, 12, 30});
    FakePhone viewer(3), owner(3);
    FakePhone::PairResult pv, po;
    KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
    KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
    KV_REQUIRE(connectDevice(viewer, pv) && connectDevice(owner, po));
    PhoneRunner rv(viewer), ro(owner);
    rv.start();
    ro.start();
    // Sharing off (the default): the viewer is unavailable; the owner watches at home (D2).
    KV_CHECK(rv.waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->liveUnavailableReason == std::string("sharing_off"); }, 3000));
    KV_CHECK(ro.waitFor([](const V3Log& l) { return l.decoded >= 5; }, 4000));
    // An owner turns sharing on: answered first, then the viewer watches.
    ro.send(MsgType::PetCameraCmd, cmdJson(1, pet_action::kSetSharing, true));
    KV_CHECK(ro.waitFor([](const V3Log& l) { return l.result(1) && l.result(1)->ok; }, 2000));
    KV_CHECK(rv.waitFor([](const V3Log& l) { return l.lastPet()->watching && l.decoded >= 3; }, 4000));
    KV_CHECK(srv.petSettings().sharingEnabled && loadPetSettings(dir).sharingEnabled);
    SharingScheduleRow row;
    row.days = 1u << 3;
    row.fromMinute = 12 * 60;
    row.toMinute = 13 * 60;
    const int t_o = ro.ms();
    srv.setSharingSchedule({row});
    KV_CHECK(rv.waitFor([](const V3Log& l) {
        return !l.lastPet()->liveAllowed && l.lastPet()->liveUnavailableReason == std::string("sharing_schedule") &&
               l.lastPet()->sharingScheduleOff;
    }, 2000));
    sleepMs(600);
    // The clock moves out of the row: the once-a-second schedule check lets the viewer watch again.
    srv.setLocalTimeForTest(LocalTime{3, 13, 5});
    KV_CHECK(rv.waitFor([](const V3Log& l) { return l.lastPet()->watching && !l.lastPet()->sharingScheduleOff; }, 3000));
    KV_CHECK(ro.log().ausFrom(t_o) >= 10); // the owner was never affected
    // 0x0E: watch false frees the slot; every 0x0E is answered with a 0x0B, even one that changes nothing.
    const size_t pets_before = rv.log().pets.size();
    rv.send(MsgType::LiveViewReq, watchJson(false));
    KV_CHECK(rv.waitFor([&](const V3Log& l) { return l.pets.size() > pets_before && !l.lastPet()->watching; }, 2000));
    KV_CHECK(srv.watchingNow().size() == 1);
    const size_t pets_mid = rv.log().pets.size();
    rv.send(MsgType::LiveViewReq, watchJson(false));
    KV_CHECK(rv.waitFor([&](const V3Log& l) { return l.pets.size() > pets_mid; }, 2000));
    rv.stop();
    ro.stop();
    KV_CHECK(petSeqNeverGoesDown(rv.log()));
}

KV_TEST(v3_pairing_rules_methods_sixteen_devices_and_re_pair) {
    HostStreamServer srv;
    HostStreamServer::Options o;
    o.state_dir = freshStateDir(L"v3-pairing");
    o.loopback_only = true;
    std::string err;
    KV_REQUIRE(srv.start(o, &err));
    // methodRaw 2 (an invite) on the LAN pairing listener: closed without a proof check, window kept.
    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err));
    FakePhone invite(3);
    invite.setPairMethodOverride(static_cast<uint8_t>(PairingMethod::Invite));
    const auto bad = invite.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_CHECK(!bad.ok && bad.closedWithoutReply);
    KV_CHECK(srv.currentPairingOffer().has_value());
    std::vector<std::unique_ptr<FakePhone>> phones;
    phones.push_back(std::make_unique<FakePhone>(1));
    const auto first = phones.back()->pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_REQUIRE(first.ok); // the same window still pairs
    KV_CHECK(srv.devices().size() == 1 && srv.devices()[0].role == PetRole::Viewer); // D1: viewer by default
    for (int i = 1; i < 16; ++i) {
        phones.push_back(std::make_unique<FakePhone>(1));
        FakePhone::PairResult pr;
        KV_REQUIRE(pairDevice(srv, *phones.back(), PetRole::Viewer, pr));
    }
    KV_CHECK(srv.devices().size() == 16 && srv.devicesFull());
    KV_CHECK(srv.devices()[15].name == "iPhone 16");
    FakePhone seventeenth(1);
    FakePhone::PairResult p17;
    KV_CHECK(!pairDevice(srv, seventeenth, PetRole::Viewer, p17));
    KV_CHECK_MSG(p17.error == text::kTooManyPairedDevices, p17.error);
    KV_CHECK(srv.devices().size() == 16);
    // Pairing a listed device again keeps its name and role.
    FakePhone::PairResult again;
    KV_CHECK(pairDevice(srv, *phones[2], PetRole::Owner, again));
    KV_CHECK(srv.devices()[2].role == PetRole::Viewer && srv.devices()[2].name == "iPhone 3");
    // Re-pair on a row: the new certificate takes the record, with its name and role; the old one is refused.
    KV_REQUIRE(srv.setDeviceRole(idOf(*phones[0]), PetRole::Owner));
    KV_REQUIRE(srv.renameDevice(idOf(*phones[0]), "Owner's phone"));
    FakePhone reinstalled(3);
    reinstalled.setDeviceName("New name it sends");
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Viewer, idOf(*phones[0])));
    const auto rp = reinstalled.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_REQUIRE(rp.ok);
    KV_CHECK(rp.role == std::string("owner") && rp.deviceName == std::string("Owner's phone") && rp.deviceId == idOf(reinstalled));
    KV_CHECK(srv.devices().size() == 16 && srv.devices()[0].deviceId == idOf(reinstalled) && srv.devices()[0].name == "Owner's phone");
    StreamStats old_one, new_one;
    if (connectDevice(*phones[0], first)) old_one = runStream(*phones[0], 600);
    KV_CHECK(!old_one.got_hello);
    FakePhone::PairResult rp2 = first;
    KV_REQUIRE(connectDevice(reinstalled, rp2));
    new_one = runStream(reinstalled, 600);
    KV_CHECK(new_one.got_hello && new_one.hello.allowPhoneModeSwitch); // an owner again
    srv.shutdown();
}

namespace {
// A pairing.json as a v2 build left it: the host identity, a service id and the one pin.
bool writeV2PairingState(const std::wstring& dir, const Bytes& pin, std::string& serviceId) {
    std::string err;
    auto id = CryptoIdentity::generate("kv2-host", &err);
    if (!id || !ensureOwnerOnlyDirectory(dir, &err)) return false;
    HostPersistedState st;
    st.privateKeyRaw = id->privateKeyRaw();
    st.certificateDER = id->certificateDER();
    st.serviceId = randomHexId(6);
    st.pinnedClientCertSHA256 = pin;
    serviceId = st.serviceId;
    return saveHostStateAtomically(dir, st, &err);
}

bool startServer(HostStreamServer& srv, const std::wstring& dir) {
    HostStreamServer::Options o;
    o.state_dir = dir;
    o.loopback_only = true;
    std::string err;
    return srv.start(o, &err);
}

// Connects with what the phone stored at pairing (the stream port changes per start, so it is given).
bool helloFrom(FakePhone& phone, HostStreamServer& srv) {
    FakePhone::PairResult pr;
    pr.streamPort = srv.streamPort();
    std::string err;
    if (!phone.connectStream("127.0.0.1", pr.streamPort, std::nullopt, &err)) return false;
    const StreamStats st = runStream(phone, 600);
    phone.close();
    return st.got_hello;
}
} // namespace

KV_TEST(v3_migration_the_pin_becomes_owner_iphone_and_the_downgrade_cases_hold_end_to_end) {
    // The pin of a v2 build becomes owner "iPhone", and streams at home with sharing off.
    const std::wstring dir = freshStateDir(L"v3-migrate");
    FakePhone phone(2);
    std::string sid;
    KV_REQUIRE(writeV2PairingState(dir, phone.identity().certificateSHA256(), sid));
    {
        PoweredHost host;
        std::string err;
        KV_REQUIRE(host.start(dir, &err));
        HostStreamServer& srv = host.ctl.server();
        const auto devs = srv.devices();
        KV_REQUIRE(devs.size() == 1);
        KV_CHECK(devs[0].name == "iPhone" && devs[0].role == PetRole::Owner && devs[0].kind == "iphone" &&
                 devs[0].deviceId == idOf(phone));
        KV_CHECK(srv.serviceId() == sid && !srv.reconcileLog().empty());
        KV_CHECK(!srv.petSettings().sharingEnabled);
        FakePhone::PairResult pr;
        pr.streamPort = srv.streamPort();
        KV_REQUIRE(connectDevice(phone, pr));
        const StreamStats st = runStreamUntil(phone, 1500, [](const StreamStats& x) {
            return x.got_hello && x.aus >= 20 && !x.states.empty();
        });
        KV_CHECK(st.got_hello && st.hello.allowPhoneModeSwitch && ausWithin(st, 1500) >= 20 && !st.states.empty() &&
                 st.states.back().allowPhoneSensorPower);
        phone.close();
        // After the owner is revoked, a remaining viewer still connects and the host is still advertised.
        srv.setSharingEnabled(true);
        FakePhone viewer(1);
        FakePhone::PairResult pv;
        KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
        KV_REQUIRE(srv.revokeDevice(idOf(phone)));
        KV_CHECK(srv.serviceId() == sid);
        KV_CHECK(srv.isPhonePaired()); // the _kv2s advertisement condition (mDNS itself is off in tests)
        StreamStats refused;
        if (connectDevice(phone, pr)) refused = runStream(phone, 600);
        KV_CHECK(!refused.got_hello);
        KV_REQUIRE(connectDevice(viewer, pv));
        const StreamStats sv = runStreamUntil(viewer, 1200, helloAndAus(10));
        KV_CHECK(sv.got_hello && ausWithin(sv, 1200) >= 10);
    }
    // An unpair on a downgraded (v2) build, then an upgrade: that device stays refused.
    {
        const std::wstring d2 = freshStateDir(L"v3-downgrade");
        FakePhone p(2);
        {
            HostStreamServer srv;
            KV_REQUIRE(startServer(srv, d2));
            FakePhone::PairResult pr;
            KV_REQUIRE(pairDevice(srv, p, PetRole::Owner, pr));
            KV_CHECK(helloFrom(p, srv));
            srv.shutdown();
        }
        HostPersistedState st;
        KV_REQUIRE(loadHostState(d2, st));
        KV_CHECK(st.pinnedClientCertSHA256 == p.identity().certificateSHA256()); // the mirror
        st.pinnedClientCertSHA256.reset(); // what the old build's unpair does
        st.serviceId = randomHexId(6);
        KV_REQUIRE(saveHostStateAtomically(d2, st));
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, d2));
        KV_CHECK(srv.devices().empty() && !srv.isPhonePaired());
        KV_CHECK(!helloFrom(p, srv));
        srv.shutdown();
    }
    // A crash between the two writes: revoking the last device (serviceId rotates), revoking all, and
    // pairing the first owner. Nothing revoked comes back, nothing listed is lost.
    for (int which = 0; which < 3; ++which) {
        const std::wstring d3 = freshStateDir(which == 0 ? L"v3-crash-last" : which == 1 ? L"v3-crash-all" : L"v3-crash-pair");
        FakePhone p(2), q(2);
        std::string rotated;
        {
            HostStreamServer srv;
            KV_REQUIRE(startServer(srv, d3));
            FakePhone::PairResult pr, qr;
            if (which == 2) srv.setCrashBetweenDeviceWritesForTest(true);
            KV_REQUIRE(pairDevice(srv, p, PetRole::Owner, pr));
            if (which == 1) KV_REQUIRE(pairDevice(srv, q, PetRole::Viewer, qr));
            if (which == 0) {
                srv.setCrashBetweenDeviceWritesForTest(true);
                KV_REQUIRE(srv.revokeDevice(idOf(p)));
                rotated = srv.serviceId();
            } else if (which == 1) {
                srv.setCrashBetweenDeviceWritesForTest(true);
                KV_CHECK(srv.revokeAllDevices() == 2);
            }
            srv.shutdown();
        }
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, d3));
        if (which == 2) {
            KV_CHECK(srv.devices().size() == 1 && srv.devices()[0].role == PetRole::Owner);
            KV_CHECK(helloFrom(p, srv));
        } else {
            KV_CHECK(srv.devices().empty());
            KV_CHECK(!helloFrom(p, srv));
            if (which == 1) KV_CHECK(!helloFrom(q, srv));
            if (which == 0) KV_CHECK(srv.serviceId() == rotated);
        }
        srv.shutdown();
    }
}

KV_TEST(v3_clean_shutdown_tells_v3_sessions) {
    FakePhone a(3);
    FakePhone::PairResult pa;
    V3Log l;
    {
        PoweredHost host;
        std::string err;
        KV_REQUIRE(host.start(freshStateDir(L"v3-shutdown"), &err));
        KV_REQUIRE(pairDevice(host.ctl.server(), a, PetRole::Owner, pa));
        KV_REQUIRE(connectDevice(a, pa));
        PhoneRunner r(a);
        r.start();
        KV_REQUIRE(r.waitFor([](const V3Log& x) { return !x.pets.empty(); }, 4000));
        host.ctl.stop();
        KV_CHECK(r.waitFor([](const V3Log& x) { return x.closed_by_host; }, 3000));
        r.stop();
        l = r.log();
    }
    KV_CHECK(l.hasNotice("host_shutting_down"));
    KV_CHECK(!l.notices.empty() && l.notices.back().message == std::string(text::kHostShuttingDown));
}

// pet-camera.md §3: the role is chosen in the pairing window and can be changed there until a device
// pairs; a Re-pair window keeps the record's role and takes no change.
KV_TEST(v3_the_pairing_window_role_can_change_until_a_device_pairs) {
    HostStreamServer srv;
    HostStreamServer::Options o;
    o.state_dir = freshStateDir(L"v3-pair-role");
    o.loopback_only = true;
    std::string err;
    KV_REQUIRE(srv.start(o, &err));
    KV_CHECK(!srv.setPairingRole(PetRole::Owner)); // no window open
    ActivePairingOffer offer;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err)); // Viewer by default (D1)
    KV_CHECK(srv.currentPairingOffer()->role == PetRole::Viewer);
    KV_CHECK(!srv.setPairingRole(PetRole::Unknown));
    KV_CHECK(srv.setPairingRole(PetRole::Owner));
    KV_CHECK(srv.currentPairingOffer()->role == PetRole::Owner);
    FakePhone phone(3);
    phone.setDeviceName("Garden iPad");
    const auto pr = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_REQUIRE(pr.ok);
    KV_CHECK(pr.role == std::string("owner"));
    KV_REQUIRE(srv.devices().size() == 1);
    KV_CHECK(srv.devices()[0].role == PetRole::Owner && srv.devices()[0].name == "Garden iPad");
    KV_CHECK(!srv.setPairingRole(PetRole::Viewer)); // the window closed with the pairing
    // Back to Viewer before a pairing: the next device is a viewer.
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    KV_CHECK(srv.setPairingRole(PetRole::Viewer));
    FakePhone second(3);
    const auto p2 = second.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_REQUIRE(p2.ok);
    KV_CHECK(p2.role == std::string("viewer") && srv.devices().size() == 2 && srv.devices()[1].role == PetRole::Viewer);
    // Re-pair keeps the record's role: the window takes no role change.
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Viewer, idOf(phone)));
    KV_CHECK(!srv.setPairingRole(PetRole::Viewer));
    srv.cancelPairingWindow();
    srv.shutdown();
}

// ---- save failures fail closed (pet-camera.md §5, v3 §3.2, §6.4) ------------------------------------

namespace {
// Another process holding a state file open without FILE_SHARE_DELETE (an indexer, AV, a sync tool):
// the atomic replace (MoveFileExW onto it) fails until it lets go.
struct FileHold {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit FileHold(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~FileHold() { release(); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    void release() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
};
} // namespace

// A revoke (of the mirror owner, so the mirror changes) and privacy on, each with its write failing:
// both hold in memory at once, the PC is told, the revoked device is refused, the maintenance loop's
// retry saves them once the file is free (with nothing else touched), and a restart keeps both.
KV_TEST(v3_a_failed_save_fails_closed_is_shown_retried_and_holds_after_a_restart) {
    const std::wstring dir = freshStateDir(L"v3-save-fails");
    FakePhone owner(3), other(3);
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        FakePhone::PairResult po, pw;
        KV_REQUIRE(pairDevice(srv, other, PetRole::Owner, pw));
        KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po)); // the newest owner: the downgrade mirror
        srv.setSharingEnabled(true);                            // pet-camera.json exists
        KV_CHECK(srv.saveError().empty());
        KV_CHECK(helloFrom(owner, srv));
        {
            FileHold hold(dir + L"\\devices.json");
            KV_REQUIRE(hold.ok());
            KV_CHECK(srv.revokeDevice(idOf(owner)));
            KV_CHECK(srv.devices().size() == 1);
            KV_CHECK_MSG(!srv.saveError().empty(), "a failed revoke save isn't shown");
            KV_CHECK(!helloFrom(owner, srv)); // refused at once, from memory
            sleepMs(400);                     // retries fail while the file is held
            KV_CHECK(!srv.saveError().empty());
            KV_CHECK(!helloFrom(owner, srv));
        }
        KV_CHECK_MSG(waitUntil([&] { return srv.saveError().empty(); }, 3000), srv.saveError());
        {
            FileHold hold(dir + L"\\pet-camera.json");
            KV_REQUIRE(hold.ok());
            srv.setPrivacyMode(true);
            KV_CHECK(srv.petSettings().privacyMode); // on at once: live video already stopped
            KV_CHECK_MSG(!srv.saveError().empty(), "a failed privacy save isn't shown");
            KV_CHECK(!loadPetSettings(dir).privacyMode);
            sleepMs(300);
            KV_CHECK(!srv.saveError().empty());
        }
        KV_CHECK_MSG(waitUntil([&] { return srv.saveError().empty(); }, 3000), srv.saveError());
        KV_CHECK(loadPetSettings(dir).privacyMode);
        srv.shutdown();
    }
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    std::printf("  after the restart: %zu device(s), privacy=%d\n", srv.devices().size(), srv.petSettings().privacyMode ? 1 : 0);
    KV_CHECK(srv.devices().size() == 1 && srv.devices()[0].deviceId == idOf(other));
    KV_CHECK(!helloFrom(owner, srv)); // never admitted again
    KV_CHECK(helloFrom(other, srv));
    KV_CHECK(srv.petSettings().privacyMode);
    srv.shutdown();
}

// A failed save still pending at a clean exit: shutdown() falls back to the full save (devices.json and
// pairing.json) when the device-list-only one is refused because the mirror changed, and saves
// pet-camera.json, so the restart sees the revoke and privacy mode.
KV_TEST(v3_a_save_still_failing_at_exit_is_written_by_shutdown) {
    const std::wstring dir = freshStateDir(L"v3-save-exit");
    FakePhone owner(3), other(3);
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        FakePhone::PairResult po, pw;
        KV_REQUIRE(pairDevice(srv, other, PetRole::Viewer, pw));
        KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po)); // the only owner: the mirror
        srv.setSharingEnabled(true);
        FileHold dev(dir + L"\\devices.json");
        FileHold pet(dir + L"\\pet-camera.json");
        KV_REQUIRE(dev.ok() && pet.ok());
        KV_CHECK(srv.revokeDevice(idOf(owner)));
        srv.setPrivacyMode(true);
        KV_CHECK(!srv.saveError().empty());
        // Let go and exit at once: shutdown's save (or a last retry just before it) writes both.
        dev.release();
        pet.release();
        srv.shutdown();
    }
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK(srv.devices().size() == 1 && srv.devices()[0].deviceId == idOf(other));
    KV_CHECK(!helloFrom(owner, srv));
    KV_CHECK(srv.petSettings().privacyMode);
    HostPersistedState st;
    KV_CHECK(loadHostState(dir, st) && !st.pinnedClientCertSHA256); // no owner left: no mirror
    srv.shutdown();
}

// ---- unavailable sessions: what each version is told, and what an unavailable owner keeps ----------

namespace {
std::string powerJson(uint64_t id, bool on) {
    SensorPowerControlMessage r;
    r.requestId = id;
    r.turnOn = on;
    return toJson(r);
}
bool everyStateSaysV3(const V3Log& l) {
    for (const auto& s : l.states)
        if (s.protocolVersion != 3) return false;
    return true;
}
} // namespace

// v3 §2.3: a v3 session the camera is unavailable to gets 0x0B liveAllowed false with the reason, and
// its 0x07 keeps the real power state (never the v2 "off" presentation); a v2 session gets the "off"
// 0x07 with the §9 sentence. Privacy mode for every role, sharing off for viewers. §4.3: every 0x07,
// to v2 and v3 sessions alike, carries protocolVersion 3, the host's version.
KV_TEST(v3_unavailable_v3_sessions_get_0x0b_and_v2_sessions_get_the_off_0x07) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-unavailable"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone o3(3), o2(2), v3(3), v2(2);
    FakePhone::PairResult po3, po2, pv3, pv2;
    KV_REQUIRE(pairDevice(srv, o3, PetRole::Owner, po3));
    KV_REQUIRE(pairDevice(srv, o2, PetRole::Owner, po2));
    KV_REQUIRE(pairDevice(srv, v3, PetRole::Viewer, pv3));
    KV_REQUIRE(pairDevice(srv, v2, PetRole::Viewer, pv2));
    KV_REQUIRE(connectDevice(o3, po3) && connectDevice(o2, po2) && connectDevice(v3, pv3) && connectDevice(v2, pv2));
    PhoneRunner ro3(o3), ro2(o2), rv3(v3), rv2(v2);
    for (PhoneRunner* r : {&ro3, &ro2, &rv3, &rv2}) r->start();
    // Sharing off (the default): the viewers are unavailable, the owners watch.
    KV_CHECK(rv3.waitFor([](const V3Log& l) {
        return l.lastPet() && !l.lastPet()->liveAllowed && l.lastPet()->liveUnavailableReason == std::string("sharing_off");
    }, 3000));
    KV_CHECK(rv2.waitFor([](const V3Log& l) {
        return !l.states.empty() && l.states.back().powerState == SensorPowerState::Off &&
               l.states.back().sensorPowerDisabledReason == std::string(text::kSharingOff);
    }, 3000));
    KV_CHECK(ro3.waitFor([](const V3Log& l) { return l.decoded >= 3; }, 4000));
    KV_CHECK(ro2.waitFor([](const V3Log& l) { return l.decoded >= 3; }, 4000));
    srv.setPrivacyMode(true);
    KV_CHECK(ro3.waitFor([](const V3Log& l) {
        return l.lastPet() && !l.lastPet()->liveAllowed && l.lastPet()->liveUnavailableReason == std::string("privacy_mode");
    }, 3000));
    KV_CHECK(ro2.waitFor([](const V3Log& l) {
        return l.states.back().powerState == SensorPowerState::Off &&
               l.states.back().sensorPowerDisabledReason == std::string(text::kPrivacyMode);
    }, 3000));
    KV_CHECK(rv3.waitFor([](const V3Log& l) { return l.lastPet()->liveUnavailableReason == std::string("privacy_mode"); }, 3000));
    KV_CHECK(rv2.waitFor([](const V3Log& l) {
        return l.states.back().sensorPowerDisabledReason == std::string(text::kPrivacyMode);
    }, 3000));
    sleepMs(300);
    for (PhoneRunner* r : {&ro3, &ro2, &rv3, &rv2}) r->stop();
    const V3Log lo3 = ro3.log(), lo2 = ro2.log(), lv3 = rv3.log(), lv2 = rv2.log();
    std::printf("  0x07s: owner v3 %zu, owner v2 %zu, viewer v3 %zu, viewer v2 %zu; 0x0Bs to v2: %zu %zu\n", lo3.states.size(),
                lo2.states.size(), lv3.states.size(), lv2.states.size(), lo2.pets.size(), lv2.pets.size());
    // v3: the real state in every 0x07; the owner's switch stays enabled, the viewer's says only the role.
    KV_REQUIRE(!lo3.states.empty() && !lv3.states.empty());
    for (const auto& s : lo3.states) {
        KV_CHECK(s.powerState == SensorPowerState::On && s.allowPhoneSensorPower && !s.sensorPowerDisabledReason);
    }
    for (const auto& s : lv3.states) {
        KV_CHECK(s.powerState == SensorPowerState::On && !s.allowPhoneSensorPower &&
                 s.sensorPowerDisabledReason == std::string(text::kViewerPower));
    }
    // v2: no 0x0B ever, the "off" presentation with the reason instead.
    KV_CHECK(lo2.pets.empty() && lv2.pets.empty());
    KV_CHECK(!lo2.states.back().allowPhoneSensorPower && !lv2.states.back().allowPhoneSensorPower);
    // §4.3: protocolVersion 3 in every 0x07, whatever the session's version.
    KV_CHECK(everyStateSaysV3(lo3) && everyStateSaysV3(lo2) && everyStateSaysV3(lv3) && everyStateSaysV3(lv2));
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On); // privacy mode never touches the sensor
}

// The 0x0A ruling (v3 §3.1, §6.4): an owner keeps sensor power while it is unavailable, if the PC
// allows phone power. A v3 owner in privacy mode turns the sensor off and on and is told why it gets
// no video by its 0x0B; a v2 owner in the same state is refused, because its "off" 0x07 already
// disables the switch and says why. The v3 owner's owner commands keep working: sharing on and off,
// and privacy mode off (each answered with an ok 0x0D).
KV_TEST(v3_an_owner_in_privacy_mode_keeps_power_and_owner_commands_a_v2_owner_is_refused) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-privacy-power"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone o3(3), o2(2);
    FakePhone::PairResult p3, p2;
    KV_REQUIRE(pairDevice(srv, o3, PetRole::Owner, p3));
    KV_REQUIRE(pairDevice(srv, o2, PetRole::Owner, p2));
    KV_REQUIRE(connectDevice(o3, p3) && connectDevice(o2, p2));
    PhoneRunner r3(o3), r2(o2);
    r3.start();
    r2.start();
    KV_REQUIRE(r3.waitFor([](const V3Log& l) { return l.decoded >= 3 && !l.pets.empty(); }, 4000));
    KV_REQUIRE(r2.waitFor([](const V3Log& l) { return l.decoded >= 3; }, 4000));
    srv.setPrivacyMode(true);
    KV_REQUIRE(r3.waitFor([](const V3Log& l) { return l.lastPet()->liveUnavailableReason == std::string("privacy_mode"); }, 3000));
    KV_REQUIRE(r2.waitFor([](const V3Log& l) { return l.states.back().powerState == SensorPowerState::Off; }, 3000));
    KV_REQUIRE(host.drain());
    const int processed0 = host.processed.load();

    // The v2 owner: refused, answered with its "off" 0x07 and the reason.
    const size_t states2 = r2.log().states.size();
    r2.send(MsgType::SensorPowerReq, powerJson(1, false));
    KV_CHECK(r2.waitFor([&](const V3Log& l) { return l.states.size() > states2; }, 2000));
    sleepMs(300);
    KV_CHECK(host.drain());
    KV_CHECK(host.processed.load() == processed0);
    KV_CHECK(host.power->snapshot().state == SensorPowerState::On);
    {
        const HostStateSyncMessage s = r2.log().states.back();
        KV_CHECK(s.powerState == SensorPowerState::Off && !s.allowPhoneSensorPower &&
                 s.sensorPowerDisabledReason == std::string(text::kPrivacyMode));
    }

    // The v3 owner: off, then on again, both acted on.
    r3.send(MsgType::SensorPowerReq, powerJson(2, false));
    KV_CHECK(waitUntil([&] { return host.power->snapshot().state == SensorPowerState::Off; }, 3000));
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.states.back().powerState == SensorPowerState::Off; }, 2000));
    r3.send(MsgType::SensorPowerReq, powerJson(3, true));
    KV_CHECK(waitUntil([&] { return host.power->snapshot().state == SensorPowerState::On; }, 4000));
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.states.back().powerState == SensorPowerState::On; }, 3000));
    KV_CHECK(host.drain());
    std::printf("  privacy on: power requests processed %d (v2's refused), sensor %s\n", host.processed.load() - processed0,
                toWire(host.power->snapshot().state));
    KV_CHECK(host.processed.load() == processed0 + 2);
    {
        // ...and told why there is no video: the 0x0B, unchanged by the power changes.
        const V3Log l = r3.log();
        KV_CHECK(!l.lastPet()->liveAllowed && l.lastPet()->liveUnavailableReason == std::string("privacy_mode") &&
                 !l.lastPet()->watching);
        KV_CHECK(l.states.back().allowPhoneSensorPower && !l.states.back().sensorPowerDisabledReason);
    }
    // Owner commands in privacy mode: sharing on and off, then privacy off.
    r3.send(MsgType::PetCameraCmd, cmdJson(10, pet_action::kSetSharing, true));
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.result(10) != nullptr; }, 2000));
    KV_CHECK(srv.petSettings().sharingEnabled);
    r3.send(MsgType::PetCameraCmd, cmdJson(11, pet_action::kSetSharing, false));
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.result(11) != nullptr; }, 2000));
    KV_CHECK(!srv.petSettings().sharingEnabled);
    r3.send(MsgType::PetCameraCmd, cmdJson(12, pet_action::kSetPrivacy, false));
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.result(12) != nullptr; }, 2000));
    KV_CHECK(!srv.petSettings().privacyMode);
    KV_CHECK(r3.waitFor([](const V3Log& l) { return l.lastPet()->liveAllowed && l.lastPet()->watching; }, 3000));
    r3.stop();
    r2.stop();
    const V3Log l = r3.log();
    for (uint64_t id : {10, 11, 12}) KV_CHECK(l.result(id) && l.result(id)->ok);
}

// The same for an owner unavailable with too_many_viewers because four owners hold the slots: it
// powers the sensor off and on, its 0x0B says too_many_viewers, and its owner commands work.
KV_TEST(v3_an_owner_refused_a_slot_by_four_owners_keeps_power_and_owner_commands) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-fifth-owner"), &err, 8000));
    HostStreamServer& srv = host.ctl.server();
    std::vector<std::unique_ptr<FakePhone>> owners;
    std::vector<FakePhone::PairResult> prs(5);
    for (int i = 0; i < 5; ++i) {
        owners.push_back(std::make_unique<FakePhone>(3));
        KV_REQUIRE(pairDevice(srv, *owners.back(), PetRole::Owner, prs[static_cast<size_t>(i)]));
    }
    std::vector<std::unique_ptr<PhoneRunner>> runners;
    for (int i = 0; i < 4; ++i) {
        KV_REQUIRE(connectDevice(*owners[static_cast<size_t>(i)], prs[static_cast<size_t>(i)]));
        runners.push_back(std::make_unique<PhoneRunner>(*owners[static_cast<size_t>(i)]));
        runners.back()->start();
        KV_REQUIRE(runners.back()->waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->watching; }, 4000));
    }
    KV_REQUIRE(connectDevice(*owners[4], prs[4]));
    PhoneRunner r5(*owners[4]);
    r5.start();
    KV_REQUIRE(r5.waitFor([](const V3Log& l) {
        return l.lastPet() && !l.lastPet()->liveAllowed && l.lastPet()->liveUnavailableReason == std::string("too_many_viewers");
    }, 4000));
    KV_REQUIRE(host.drain());
    const int processed0 = host.processed.load();
    r5.send(MsgType::SensorPowerReq, powerJson(1, false));
    KV_CHECK(waitUntil([&] { return host.power->snapshot().state == SensorPowerState::Off; }, 3000));
    r5.send(MsgType::SensorPowerReq, powerJson(2, true));
    KV_CHECK(waitUntil([&] { return host.power->snapshot().state == SensorPowerState::On; }, 4000));
    KV_CHECK(host.drain());
    KV_CHECK(host.processed.load() == processed0 + 2);
    r5.send(MsgType::PetCameraCmd, cmdJson(10, pet_action::kSetSharing, true));
    r5.send(MsgType::PetCameraCmd, cmdJson(11, pet_action::kSetSharing, false));
    r5.send(MsgType::PetCameraCmd, cmdJson(12, pet_action::kSetPrivacy, true));
    r5.send(MsgType::PetCameraCmd, cmdJson(13, pet_action::kSetPrivacy, false));
    KV_CHECK(r5.waitFor([](const V3Log& l) { return l.result(10) && l.result(11) && l.result(12) && l.result(13); }, 3000));
    sleepMs(300);
    r5.stop();
    for (auto& r : runners) r->stop();
    const V3Log l = r5.log();
    std::printf("  fifth owner: power requests processed %d, reason %s, results %zu\n", host.processed.load() - processed0,
                l.lastPet() && l.lastPet()->liveUnavailableReason ? l.lastPet()->liveUnavailableReason->c_str() : "-",
                l.results.size());
    for (uint64_t id : {10, 11, 12, 13}) KV_CHECK(l.result(id) && l.result(id)->ok);
    KV_CHECK(!srv.petSettings().sharingEnabled && !srv.petSettings().privacyMode);
    KV_CHECK(!l.lastPet()->liveAllowed && l.lastPet()->liveUnavailableReason == std::string("too_many_viewers"));
    for (const auto& s : l.states) KV_CHECK(s.allowPhoneSensorPower && !s.sensorPowerDisabledReason);
}

// ---- v3 §6.2: every session on a tier gets each new epoch's 0x02 ----------------------------------------

namespace {
TierAccessUnit fakeAu(uint32_t epoch, bool idr, bool config_due, uint64_t seq) {
    TierAccessUnit au;
    au.tier = LiveTier::High;
    au.epoch = epoch;
    au.idr = idr;
    au.config_due = config_due;
    au.sps = {0x67, 0x42, 0xC0, 0x1E};
    au.pps = {0x68, 0xCE, 0x3C, 0x80};
    au.avcc = {0x00, 0x00, 0x00, 0x02, static_cast<uint8_t>(idr ? 0x65 : 0x41), 0x88};
    au.width = 64;
    au.height = 48;
    au.frame_seq = seq;
    au.capture_ns = nowNs();
    au.encode_done_ns = au.capture_ns;
    return au;
}
} // namespace

// The server-side defence: an IDR of an epoch a watching session wasn't configured for gets that
// tier's 0x02 first, even when the encoder's config_due flag was used up by an IDR of the old epoch
// still in an async encoder. Without it the phone drops every 0x03 of the new epoch (a frozen view).
KV_TEST(v3_an_idr_of_a_new_epoch_without_config_due_still_gets_its_0x02) {
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, freshStateDir(L"v3-epoch-defence")));
    FakePhone a(3), b(2);
    FakePhone::PairResult pa, pb;
    KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pa));
    KV_REQUIRE(pairDevice(srv, b, PetRole::Owner, pb));
    KV_REQUIRE(connectDevice(a, pa) && connectDevice(b, pb));
    PhoneRunner ra(a), rb(b);
    ra.start();
    rb.start();
    KV_REQUIRE(ra.waitFor([](const V3Log& l) { return !l.pets.empty(); }, 3000));
    KV_REQUIRE(rb.waitFor([](const V3Log& l) { return l.got_hello && l.first_ping_ms >= 0; }, 3000)); // no 0x07 here: no app state
    KV_REQUIRE(waitUntil([&] { return srv.sessionsOnTier(LiveTier::High) == 2; }, 2000));
    // Epoch 7 opens (config_due), a P-frame; then epoch 8's IDR arrives without config_due, and a P-frame.
    const TierAccessUnit seq[] = {fakeAu(7, true, true, 1), fakeAu(7, false, false, 2), fakeAu(8, true, false, 3),
                                  fakeAu(8, false, false, 4)};
    int sent = 0;
    for (const auto& au : seq) {
        srv.deliverLiveAccessUnit(au);
        ++sent;
        // One at a time, so per-session back-pressure never drops a P-frame here.
        KV_CHECK(ra.waitFor([&](const V3Log& l) { return l.aus + l.au_epoch_mismatch >= sent; }, 2000));
        KV_CHECK(rb.waitFor([&](const V3Log& l) { return l.aus + l.au_epoch_mismatch >= sent; }, 2000));
    }
    ra.stop();
    rb.stop();
    for (const V3Log& l : {ra.log(), rb.log()}) {
        std::printf("  configs:");
        for (const auto& c : l.configs) std::printf(" %u", c.streamEpoch);
        std::printf("; aus=%d mismatched=%d not_idr=%d\n", l.aus, l.au_epoch_mismatch, l.not_idr_after_cfg);
        KV_REQUIRE(l.configs.size() == 2);
        KV_CHECK(l.configs[0].streamEpoch == 7 && l.configs[1].streamEpoch == 8);
        KV_CHECK(l.aus == 4 && l.au_epoch_mismatch == 0 && l.not_idr_after_cfg == 0);
    }
    srv.shutdown();
}

// ---- what a device may send: 64 KiB per frame on the stream and pairing connections ---------------------

namespace {
// A frame header that announces `len` payload bytes, and the first `body` of them.
std::vector<uint8_t> announcedFrame(MsgType t, uint32_t len, size_t body) {
    std::vector<uint8_t> f = packFrame(t, 0, std::vector<uint8_t>(len, '0'));
    f.resize(kWireHeaderSize + std::min<size_t>(body, len));
    return f;
}

// A ping whose JSON payload is exactly `size` bytes (the padding is a key the host ignores).
std::string pingOfSize(size_t size) {
    PingMessage p;
    p.clientSendNs = 1;
    p.protocolVersion = 3;
    std::string j = toJson(p);
    const std::string head = j.substr(0, j.size() - 1) + ",\"pad\":\"";
    const size_t fixed = head.size() + 2; // the closing quote and brace
    return head + std::string(size > fixed ? size - fixed : 0, 'x') + "\"}";
}

bool readHello(FakePhone& phone) {
    MsgType t;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
    return phone.readFrame(t, flags, payload, 3000) && t == MsgType::HelloServer;
}

// Reads until a pong comes (true) or the connection ends or 3 s pass (false).
bool readPong(FakePhone& phone) {
    const ULONGLONG until = GetTickCount64() + 3000;
    while (GetTickCount64() < until) {
        MsgType t;
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        if (!phone.readFrame(t, flags, payload, 500)) {
            if (phone.lastReadWasClose()) return false;
            continue;
        }
        if (t == MsgType::Pong) return true;
    }
    return false;
}
} // namespace

// A paired device's stream connection: a frame that announces more than 64 KiB closes the connection at
// its header, before a byte of the payload is waited for or parsed (so it never reaches the JSON parser
// under the server's lock). 64 KiB itself is fine; the other sessions carry on.
KV_TEST(v3_a_stream_frame_over_64_kib_closes_the_connection_at_its_header) {
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, freshStateDir(L"v3-big-frame")));
    FakePhone a(3), b(3);
    FakePhone::PairResult pa, pb;
    KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pa));
    KV_REQUIRE(pairDevice(srv, b, PetRole::Owner, pb));

    // Exactly 64 KiB: read and answered.
    KV_REQUIRE(connectDevice(a, pa) && readHello(a));
    const std::string ok = pingOfSize(64 * 1024);
    KV_REQUIRE(ok.size() == 64 * 1024);
    KV_CHECK(a.sendFrame(MsgType::Ping, ok));
    KV_CHECK_MSG(readPong(a), "a 64 KiB ping was not answered");

    // A header that announces 64 KiB + 1, then a little of it: closed without waiting for the rest.
    KV_REQUIRE(connectDevice(b, pb) && readHello(b));
    KV_CHECK(b.sendBytes(announcedFrame(MsgType::Ping, 64 * 1024 + 1, 1000)));
    KV_CHECK_MSG(b.waitForHostClose(2000), "a frame announcing 64 KiB + 1 kept the connection open");
    // The whole attack in one frame, just under the old 4 MiB cap: closed too (the same device, connected
    // again). The send may fail part way once the host has closed; only the close matters.
    KV_REQUIRE(connectDevice(b, pb) && readHello(b));
    std::string arr = "[";
    while (arr.size() < 4u * 1024u * 1024u - 64) arr += "0,";
    arr += "0]";
    b.sendBytes(packFrame(MsgType::Ping, 0, arr));
    KV_CHECK_MSG(b.waitForHostClose(5000), "a 4 MiB frame kept the connection open");
    b.close();
    // The well-behaved session is untouched.
    KV_CHECK(a.sendFrame(MsgType::Ping, pingOfSize(100)));
    KV_CHECK(readPong(a));
    KV_CHECK(srv.sessionCount() == 1);
    a.close();
    srv.shutdown();
}

// The pairing connection (open to anyone on the LAN while the window is open) has the same 64 KiB limit:
// an oversized frame closes the connection at its header, and the window stays open for the real phone.
KV_TEST(v3_a_pairing_frame_over_64_kib_closes_the_connection_at_its_header) {
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, freshStateDir(L"v3-big-pair")));
    ActivePairingOffer offer;
    std::string err;
    KV_REQUIRE(srv.beginPairingWindow(120, offer, &err, PetRole::Owner));
    FakePhone stranger(3);
    KV_REQUIRE(stranger.connectPairing("127.0.0.1", offer.port, &err));
    KV_CHECK(stranger.sendBytes(announcedFrame(MsgType::PairRequest, 64 * 1024 + 1, 1000)));
    KV_CHECK_MSG(stranger.waitForHostClose(2000), "a pairing frame announcing 64 KiB + 1 kept the connection open");
    stranger.close();
    // The one try wasn't used up: the window is still open and the real phone pairs through it.
    KV_CHECK(srv.currentPairingOffer().has_value());
    FakePhone phone(3);
    const auto pr = phone.pair("127.0.0.1", offer.port, std::nullopt, PairingMethod::QrToken, offer.qrToken);
    KV_CHECK_MSG(pr.ok, pr.error);
    srv.shutdown();
}

// ---- the state folder: the host marks it as its own --------------------------------------------------------

// The host writes its marker into a state folder it creates or uses, so a later --state-dir check knows the
// folder by more than a file name. An existing install (pairing.json, no marker yet) is still the host's,
// and gets the marker on its first start.
KV_TEST(v3_the_host_marks_its_state_folder_and_an_existing_install_stays_accepted) {
    const std::wstring fresh = freshStateDir(L"v3-marker");
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, fresh));
        srv.shutdown();
    }
    KV_CHECK_MSG(GetFileAttributesW((fresh + L"\\.kinect-viewer-host-state").c_str()) != INVALID_FILE_ATTRIBUTES,
                 "no marker in a state folder the host created");
    std::wstring why;
    KV_CHECK(kvwin::stateDirAcceptable(fresh, &why));

    // A copy of an install from before the marker: pairing.json (and devices.json) only.
    const std::wstring old = freshStateDir(L"v3-marker-old");
    DeleteFileW((old + L"\\.kinect-viewer-host-state").c_str());
    std::string sid;
    FakePhone phone(2);
    KV_REQUIRE(writeV2PairingState(old, phone.identity().certificateSHA256(), sid));
    KV_CHECK_MSG(kvwin::stateDirAcceptable(old, &why), "an existing install's state folder was refused");
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, old));
        KV_CHECK(srv.devices().size() == 1 && srv.serviceId() == sid); // its pairing carried over
        srv.shutdown();
    }
    KV_CHECK(GetFileAttributesW((old + L"\\.kinect-viewer-host-state").c_str()) != INVALID_FILE_ATTRIBUTES);
    KV_CHECK(kvwin::stateDirAcceptable(old, &why));
}

// ---- pet-camera.json that can't be read: privacy on, and the PC says why ---------------------------------

KV_TEST(v3_an_unparsable_pet_camera_json_starts_the_host_in_privacy_mode_and_says_so) {
    const std::wstring dir = freshStateDir(L"v3-bad-settings");
    DeleteFileW((dir + L"\\pet-camera.json.bad").c_str());
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        srv.setSharingEnabled(true);
        srv.shutdown();
    }
    {
        HANDLE h = CreateFileW((dir + L"\\pet-camera.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        KV_REQUIRE(h != INVALID_HANDLE_VALUE);
        DWORD n = 0;
        WriteFile(h, "{\"sharingEnabled\":true,\"privacyMo", 33, &n, nullptr); // cut short
        CloseHandle(h);
    }
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK_MSG(srv.petSettings().privacyMode, "privacy mode is off after an unparsable pet-camera.json");
    const std::string why = srv.saveError();
    std::printf("  status line: %s\n", why.c_str());
    KV_CHECK_MSG(why.find("pet-camera.json") != std::string::npos, "the PC isn't told pet-camera.json couldn't be read");
    KV_CHECK(GetFileAttributesW((dir + L"\\pet-camera.json.bad").c_str()) != INVALID_FILE_ATTRIBUTES);
    // Turning privacy off on the PC saves the file again and clears the line.
    srv.setPrivacyMode(false);
    KV_CHECK(!srv.petSettings().privacyMode && srv.saveError().empty());
    KV_CHECK(!loadPetSettings(dir).privacyMode);
    srv.shutdown();
}

// ---- devices.json that can't be read at start: fail closed, don't overwrite, read it again ----------------

KV_TEST(v3_a_devices_json_held_at_start_admits_nobody_isnt_overwritten_and_is_read_once_free) {
    const std::wstring dir = freshStateDir(L"v3-devices-held");
    FakePhone owner(3), viewer(3);
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        FakePhone::PairResult po, pv;
        KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
        KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
        srv.setSharingEnabled(true);
        srv.shutdown();
    }
    std::string before;
    KV_REQUIRE(readWholeFile(dir + L"\\devices.json", before));
    // Another process holds devices.json without read sharing (and without delete sharing).
    HANDLE h = CreateFileW((dir + L"\\devices.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK_MSG(srv.devices().empty(), "devices were listed from a devices.json that couldn't be read");
    KV_CHECK(!srv.saveError().empty());
    std::printf("  status line: %s\n", srv.saveError().c_str());
    ActivePairingOffer offer;
    std::string err;
    KV_CHECK_MSG(!srv.beginPairingWindow(120, offer, &err), "a pairing window opened over an unread device list");
    KV_CHECK(!helloFrom(owner, srv)); // nobody is admitted, the pinned owner included
    KV_CHECK(!helloFrom(viewer, srv));
    sleepMs(500); // retries happen while it is held
    CloseHandle(h);
    // Once it can be read: the real list, both devices, and nothing was written over it.
    KV_CHECK_MSG(waitUntil([&] { return srv.devices().size() == 2; }, 8000), "the real list was never read");
    KV_CHECK(waitUntil([&] { return srv.saveError().empty(); }, 3000));
    std::string after;
    KV_REQUIRE(readWholeFile(dir + L"\\devices.json", after));
    KV_CHECK_MSG(after == before, "devices.json was overwritten");
    KV_CHECK(helloFrom(owner, srv) && helloFrom(viewer, srv));
    srv.shutdown();
}

// ---- v3 §6.1: high is today's stream for v1 and v2 phones, whatever v3 sessions do ----------------------

// A v3 session moving onto high (auto stepping back up, here by 0x0E, which takes the same path) and a v3
// session joining high get their own 0x02 and an IDR; a v1 and a v2 phone already on high get no new 0x02
// and their video keeps flowing.
KV_TEST(v3_sessions_moving_onto_high_send_no_new_0x02_to_v1_and_v2_phones) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-epoch-churn"), &err));
    HostStreamServer& srv = host.ctl.server();
    FakePhone p1(1), p2(2), d(3), late(3);
    FakePhone::PairResult r1p, r2p, dp, lp;
    KV_REQUIRE(pairDevice(srv, p1, PetRole::Owner, r1p));
    KV_REQUIRE(pairDevice(srv, p2, PetRole::Owner, r2p));
    KV_REQUIRE(pairDevice(srv, d, PetRole::Owner, dp));
    KV_REQUIRE(pairDevice(srv, late, PetRole::Owner, lp));
    KV_REQUIRE(connectDevice(p1, r1p) && connectDevice(p2, r2p) && connectDevice(d, dp));
    PhoneRunner r1(p1), r2(p2), rd(d);
    r1.start();
    r2.start();
    rd.start();
    for (PhoneRunner* r : {&r1, &r2, &rd}) KV_REQUIRE(r->waitFor([](const V3Log& l) { return l.decoded >= 10; }, 5000));
    // The v3 device goes to medium and stays there a while.
    rd.send(MsgType::LiveViewReq, watchJson(true, std::string("medium")));
    KV_REQUIRE(rd.waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->liveTier == "medium"; }, 3000));
    sleepMs(1200);
    const size_t c1 = r1.log().configs.size(), c2 = r2.log().configs.size();
    const int t1 = r1.ms(), t2 = r2.ms(), td = rd.ms();
    // Back onto high (auto starts at high at home), then a new v3 session joins high.
    rd.send(MsgType::LiveViewReq, watchJson(true, std::string("auto")));
    KV_CHECK(rd.waitFor([&](const V3Log& l) {
        return l.lastPet() && l.lastPet()->liveTier == "high" && countIn(l.cfg_ms, td, 1 << 30) >= 1 && l.ausFrom(td) >= 10;
    }, 4000));
    KV_REQUIRE(connectDevice(late, lp));
    PhoneRunner rl(late);
    rl.start();
    KV_CHECK(rl.waitFor([](const V3Log& l) { return l.decoded >= 10; }, 5000));
    sleepMs(1000);
    for (PhoneRunner* r : {&r1, &r2, &rd, &rl}) r->stop();
    const V3Log l1 = r1.log(), l2 = r2.log(), ld = rd.log(), ll = rl.log();
    std::printf("  v1: configs %zu -> %zu, aus after=%d; v2: configs %zu -> %zu, aus after=%d; v3 configs=%zu went_down=%d "
                "mismatched=%d; joiner configs=%zu decoded=%d\n",
                c1, l1.configs.size(), l1.ausFrom(t1), c2, l2.configs.size(), l2.ausFrom(t2), ld.configs.size(),
                ld.cfg_epoch_went_down, ld.au_epoch_mismatch, ll.configs.size(), ll.decoded);
    KV_CHECK_MSG(l1.configs.size() == c1, "the v1 phone got a new 0x02");
    KV_CHECK_MSG(l2.configs.size() == c2, "the v2 phone got a new 0x02");
    KV_CHECK(!l1.violated && !l1.closed_by_host && l1.ausFrom(t1) >= 40 && l1.au_epoch_mismatch == 0);
    KV_CHECK(!l2.closed_by_host && l2.ausFrom(t2) >= 40 && l2.au_epoch_mismatch == 0);
    // The v3 sessions still follow §6.2: a higher epoch on every tier change, its 0x02 before its IDR.
    for (const V3Log* l : {&ld, &ll}) {
        KV_CHECK(l->cfg_epoch_went_down == 0 && l->not_idr_after_cfg == 0 && l->au_epoch_mismatch == 0);
        KV_CHECK(l->decoded >= l->aus - 4);
    }
    KV_CHECK(ld.ausFrom(td) >= 20 && ld.lastPet() && ld.lastPet()->liveTier == "high");
}

// ---- saves: a file that keeps failing is retried with a back-off, never every tick -----------------------

KV_TEST(v3_a_save_that_keeps_failing_is_retried_with_a_back_off) {
    const std::wstring dir = freshStateDir(L"v3-save-backoff");
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    srv.setSharingEnabled(true); // pet-camera.json exists
    KV_REQUIRE(srv.saveError().empty());
    {
        FileHold hold(dir + L"\\pet-camera.json");
        KV_REQUIRE(hold.ok());
        const uint64_t a0 = srv.saveAttemptsForTest();
        srv.setPrivacyMode(true);
        KV_CHECK(srv.petSettings().privacyMode && !srv.saveError().empty()); // fail closed, and shown
        sleepMs(2500);
        const uint64_t tries = srv.saveAttemptsForTest() - a0;
        std::printf("  writes tried over 2.5 s with the file held: %llu\n", static_cast<unsigned long long>(tries));
        // 100 ms doubling: the change itself, then about 100, 300, 700 and 1500 ms after it.
        KV_CHECK_MSG(tries >= 3 && tries <= 6, "retried every tick, not backed off");
        KV_CHECK(!srv.saveError().empty() && srv.petSettings().privacyMode);
    }
    // Free again: the next retry (at most 5 s away) writes it, and the back-off starts over.
    KV_CHECK_MSG(waitUntil([&] { return srv.saveError().empty(); }, 7000), srv.saveError());
    KV_CHECK(loadPetSettings(dir).privacyMode);
    srv.shutdown();
}

// ---- v3 §3.2: a revoked session gets its 0x0F, not the video still queued for it -------------------------

// The host's own count of video frames written into the session's socket (sentFramesCount, with no other
// session) is taken at the revoke: those may still arrive, plus one frame part-written then. Anything more
// was still in the host's queue, which the revoke must drop. On the old code the queue (several IDRs of a
// device that stopped reading) was flushed ahead of the 0x0F.
KV_TEST(v3_a_revoked_session_gets_its_notice_without_the_video_queued_before_it) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-revoke-queue"), &err, 30000));
    HostStreamServer& srv = host.ctl.server();
    FakePhone victim(3);
    victim.setSmallReceiveBuffer(true);
    FakePhone::PairResult pv;
    KV_REQUIRE(pairDevice(srv, victim, PetRole::Owner, pv));
    KV_REQUIRE(connectDevice(victim, pv));
    PhoneRunner rv(victim);
    rv.start();
    KV_REQUIRE(rv.waitFor([](const V3Log& l) { return l.decoded >= 5 && !l.pets.empty(); }, 5000));
    rv.send(MsgType::LiveViewReq, watchJson(true, std::string("high"))); // no auto step-down while it is congested
    KV_REQUIRE(rv.waitFor([](const V3Log& l) { return l.lastPet() && l.lastPet()->liveQuality == "high"; }, 2000));
    rv.setPaused(true); // it stops reading: its IDRs queue up on the host
    sleepMs(4000);
    KV_REQUIRE(srv.revokeDevice(idOf(victim)));
    const uint64_t written = srv.sentFramesCount(); // the session's, frozen when it left the live list
    rv.setPaused(false);
    KV_CHECK(rv.waitFor([](const V3Log& l) { return l.closed_by_host; }, 5000));
    rv.stop();
    const V3Log l = rv.log();
    const uint64_t got = static_cast<uint64_t>(l.aus + l.au_epoch_mismatch);
    const int last_au = l.au_ms.empty() ? -1 : l.au_ms.back();
    std::printf("  0x03s written to the socket by the revoke: %llu; read in all: %llu; notice at %d ms, last 0x03 at %d ms\n",
                static_cast<unsigned long long>(written), static_cast<unsigned long long>(got),
                l.notice_ms.empty() ? -1 : l.notice_ms.back(), last_au);
    KV_REQUIRE(l.hasNotice("revoked"));
    KV_CHECK(l.notices.back().closing);
    KV_CHECK_MSG(got <= written + 1, "video still queued at the revoke was delivered before the notice");
    KV_CHECK(last_au <= l.notice_ms.back()); // nothing after the notice
}
// ---- pet-camera.json that can't be READ at start: never written over, read again once free ---------------

// Like devices.json: an I/O failure is not corruption. Privacy is on meanwhile; a change that would
// need pet-camera.json written (privacy off, sharing, the schedule) is refused, from the PC and from a
// device's 0x0C (internal), rather than writing defaults over the real file; once the file can be read its
// values are adopted, and the file is byte for byte as it was.
KV_TEST(v3_a_pet_camera_json_held_at_start_is_never_written_over_and_is_read_once_free) {
    const std::wstring dir = freshStateDir(L"v3-settings-held");
    FakePhone owner(3);
    FakePhone::PairResult po;
    SharingScheduleRow row;
    row.days = 0x22;
    row.fromMinute = 21 * 60;
    row.toMinute = 7 * 60 + 30;
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
        srv.setSharingEnabled(true);
        srv.setSharingSchedule({row});
        KV_REQUIRE(srv.saveError().empty());
        srv.shutdown();
    }
    std::string before;
    KV_REQUIRE(readWholeFile(dir + L"\\pet-camera.json", before));
    HANDLE h = CreateFileW((dir + L"\\pet-camera.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK(srv.petSettings().privacyMode); // fail closed
    KV_CHECK(!srv.saveError().empty());
    // From the PC: privacy off and a sharing change are refused, not saved as defaults later.
    srv.setPrivacyMode(false);
    srv.setSharingEnabled(false);
    KV_CHECK_MSG(srv.petSettings().privacyMode, "privacy went off while pet-camera.json couldn't be read");
    // From an owner's device: answered, not acted on.
    FakePhone::PairResult pc = po;
    pc.streamPort = srv.streamPort();
    KV_REQUIRE(connectDevice(owner, pc));
    PhoneRunner r(owner);
    r.start();
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return !l.pets.empty(); }, 4000));
    r.send(MsgType::PetCameraCmd, cmdJson(1, pet_action::kSetPrivacy, false));
    KV_CHECK(r.waitFor([](const V3Log& l) { return l.result(1) != nullptr; }, 3000));
    {
        const V3Log l = r.log();
        KV_CHECK_MSG(l.result(1) && !l.result(1)->ok && l.result(1)->error == std::string(pet_error::kInternal),
                     "a 0x0C privacy off was not refused while pet-camera.json couldn't be read");
    }
    KV_CHECK(srv.petSettings().privacyMode);
    sleepMs(300);
    CloseHandle(h);
    // Free again: the file's own values come back, and nothing was written over it.
    KV_CHECK_MSG(waitUntil([&] { return !srv.petSettings().privacyMode; }, 8000), "pet-camera.json was never read again");
    const PetSettings got = srv.petSettings();
    KV_CHECK(got.sharingEnabled && got.sharingSchedule.size() == 1 && got.sharingSchedule[0] == row);
    sleepMs(500); // any write that was going to happen has happened
    std::string after;
    KV_REQUIRE(readWholeFile(dir + L"\\pet-camera.json", after));
    KV_CHECK_MSG(after == before, "pet-camera.json was written over");
    KV_CHECK(waitUntil([&] { return srv.saveError().empty(); }, 3000));
    r.stop();
    srv.shutdown();
    std::string at_exit;
    KV_REQUIRE(readWholeFile(dir + L"\\pet-camera.json", at_exit));
    KV_CHECK(at_exit == before);
}

// A viewer's refused or unsupported 0x0C changes nothing, so it writes nothing: a burst of them while a
// save keeps failing must not get around the retry's back-off with one write attempt per frame.
KV_TEST(v3_refused_0x0c_commands_add_no_write_attempts_while_a_save_is_backed_off) {
    const std::wstring dir = freshStateDir(L"v3-0x0c-burst");
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    srv.setSharingEnabled(true); // pet-camera.json exists, and the viewer is available
    FakePhone viewer(3);
    FakePhone::PairResult pv;
    KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
    KV_REQUIRE(connectDevice(viewer, pv));
    PhoneRunner r(viewer);
    r.start();
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return !l.pets.empty(); }, 4000));
    FileHold hold(dir + L"\\pet-camera.json");
    KV_REQUIRE(hold.ok());
    srv.setPrivacyMode(true); // its write fails and is retried, backing off
    KV_REQUIRE(!srv.saveError().empty());
    sleepMs(1700); // the next retry is now about 1.6 s away
    const uint64_t a0 = srv.saveAttemptsForTest();
    for (uint64_t i = 0; i < 50; ++i) {
        if (i % 2) r.send(MsgType::PetCameraCmd, cmdJson(100 + i, pet_action::kSetPrivacy, false)); // not for a viewer
        else r.send(MsgType::PetCameraCmd, cmdJson(100 + i, "teleport"));                          // unsupported
    }
    KV_CHECK(r.waitFor([](const V3Log& l) { return l.results.size() >= 50; }, 3000));
    const uint64_t tries = srv.saveAttemptsForTest() - a0;
    std::printf("  write attempts during 50 refused 0x0C: %llu\n", static_cast<unsigned long long>(tries));
    KV_CHECK_MSG(tries <= 1, "refused 0x0C commands forced write attempts"); // at most the retry's own
    for (const auto& res : r.log().results) KV_CHECK(!res.ok);
    r.stop();
    hold.release();
    KV_CHECK(waitUntil([&] { return srv.saveError().empty(); }, 7000));
    srv.shutdown();
}

// ---- pairing.json fails closed: the PC's identity is never replaced by a start ------------------------------

namespace {
std::string fileBytes(const std::wstring& path) {
    std::string s;
    return readWholeFile(path, s) ? s : std::string("<unreadable>");
}

bool writeBytes(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
    return ok && n == text.size();
}

// No pairing.json.tmp* left in dir: neither an earlier build's name nor a process's pairing.json.tmp.<pid>.
bool noPairingTmpIn(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\pairing.json.tmp*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    FindClose(h);
    return false;
}

// A plain TCP connect to 127.0.0.1:port: true when anything accepts it.
bool somethingListensOn(uint16_t port) {
    winsockInit();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

HostStreamServer::Options loopbackOptions(const std::wstring& dir, uint16_t port = 0) {
    HostStreamServer::Options o;
    o.state_dir = dir;
    o.loopback_only = true;
    o.fixed_stream_port = port;
    return o;
}

// A host in dir with one paired owner (pairing.json, devices.json and pet-camera.json all written). Returns
// the stream port it used, so a later start can be pinned to it.
uint16_t pairedHost(const std::wstring& dir, FakePhone& owner, FakePhone::PairResult& po) {
    HostStreamServer srv;
    if (!startServer(srv, dir)) return 0;
    if (!pairDevice(srv, owner, PetRole::Owner, po)) return 0;
    srv.setSharingEnabled(true);
    const uint16_t port = srv.streamPort();
    srv.shutdown();
    return port;
}
} // namespace

// pairing.json exists but doesn't parse, or parses but its key and certificate don't load, or holds a wrapped
// key that can't be unwrapped on this PC (damaged, or wrapped on another PC or Windows install): no start
// makes a new identity. start() fails and says the identity is unusable; pairing.json is byte for byte as it
// was and kept as pairing.json.bad; nothing listens and no pairing window opens; a second start (the
// server's, and the controller's that the viewer and kinect-cli serve use) fails the same way. On the old code
// the first start generated a new identity and saved it over pairing.json, and every paired device was
// dropped. The key that won't unwrap is told as such, with the owner's way out: reset this PC's identity.
KV_TEST(v3_pairing_json_that_cant_be_parsed_fails_closed_and_is_never_replaced) {
    HostPersistedState mismatched;
    {
        std::string e;
        auto a = CryptoIdentity::generate("kv2-host", &e);
        auto b = CryptoIdentity::generate("kv2-host", &e);
        KV_REQUIRE(a && b);
        mismatched.privateKeyRaw = a->privateKeyRaw();
        mismatched.certificateDER = b->certificateDER(); // another key's certificate: fromRecord refuses it
        mismatched.serviceId = randomHexId(6);
    }
    HostPersistedState unwrappable = mismatched; // its key wrapped, but no wrap this PC can open
    unwrappable.privateKeyRaw.clear();
    unwrappable.privateKeyWrapped = std::vector<uint8_t>(64, 0x5A);
    const std::string variants[] = {"{\"serverIdentity\":{\"privateKeyRaw\":\"AAAA\"", toJson(mismatched),
                                    toJson(unwrappable)};
    const wchar_t* const names[] = {L"pj-unparsable", L"pj-wrong-key", L"pj-wont-unwrap"};
    int which = 0;
    for (const std::string& bad : variants) {
        const int index = which++;
        const std::wstring dir = freshStateDir(names[index]);
        DeleteFileW((dir + L"\\pairing.json.bad").c_str());
        FakePhone owner(3);
        FakePhone::PairResult po;
        const uint16_t port = pairedHost(dir, owner, po);
        KV_REQUIRE(port != 0);
        KV_REQUIRE(writeBytes(dir + L"\\pairing.json", bad));
        for (int attempt = 0; attempt < 2; ++attempt) {
            HostStreamServer srv;
            bool unusable = false;
            std::string err;
            const bool started = srv.start(loopbackOptions(dir, port), &err, &unusable);
            std::printf("  start %d: started=%d unusable=%d: %s\n", attempt, started ? 1 : 0, unusable ? 1 : 0, err.c_str());
            KV_CHECK_MSG(!started, "the host started over a pairing.json it couldn't use");
            KV_CHECK_MSG(unusable, "start didn't say the identity is unusable");
            KV_CHECK_MSG(err.find("pairing.json") != std::string::npos && err.find("pairing.json.bad") != std::string::npos, err);
            if (index == 2) {
                KV_CHECK_MSG(err.find("can't be unwrapped") != std::string::npos &&
                                 err.find("Reset this PC's identity") != std::string::npos,
                             err);
            }
            KV_CHECK_MSG(fileBytes(dir + L"\\pairing.json") == bad, "pairing.json was changed");
            KV_CHECK_MSG(fileBytes(dir + L"\\pairing.json.bad") == bad, "no pairing.json.bad with the original bytes");
            KV_CHECK(noPairingTmpIn(dir));
            KV_CHECK_MSG(!somethingListensOn(port), "something listens on the stream port");
            KV_CHECK(!connectDevice(owner, po));
            ActivePairingOffer offer;
            std::string perr;
            KV_CHECK_MSG(!srv.beginPairingWindow(120, offer, &perr), "a pairing window opened");
            KV_CHECK(!srv.currentPairingOffer());
            srv.shutdown();
        }
        HostController ctl;
        HostController::Options co;
        co.server = loopbackOptions(dir, port);
        bool unusable = false;
        std::string err;
        KV_CHECK(!ctl.start(co, &err, &unusable));
        KV_CHECK_MSG(unusable, "HostController::start didn't say the identity is unusable");
        ctl.stop();
        KV_CHECK_MSG(fileBytes(dir + L"\\pairing.json") == bad, "pairing.json was changed by the controller's start");
        KV_CHECK(!somethingListensOn(port));
    }
}

// The host key at rest: a start over a pairing.json from before the wrap (the key plain) wraps it, and serves with
// the very same identity: the fingerprint every paired device pinned, and the serviceId, are unchanged. The file
// then holds the key wrapped and never plain, the reconcile log says so, and the next start serves with it as it is.
KV_TEST(v3_a_start_wraps_a_plain_key_and_keeps_the_fingerprint) {
    const std::wstring dir = freshStateDir(L"pj-plain-wraps");
    std::string err;
    auto id = CryptoIdentity::generate(newHostCommonName(), &err);
    KV_REQUIRE(id);
    HostPersistedState plain;
    plain.privateKeyRaw = id->privateKeyRaw();
    plain.certificateDER = id->certificateDER();
    plain.serviceId = randomHexId(6);
    KV_REQUIRE(ensureOwnerOnlyDirectory(dir));
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json", toJson(plain))); // no privateKeyWrapped: written plain
    KV_REQUIRE(fileBytes(dir + L"\\pairing.json").find("\"privateKeyRaw\":") != std::string::npos);
    {
        HostStreamServer srv;
        const bool started = srv.start(loopbackOptions(dir), &err);
        KV_CHECK_MSG(started, err);
        KV_REQUIRE(started);
        KV_CHECK(srv.serverFingerprintHex() == id->fingerprintHex());
        KV_CHECK(srv.serviceId() == plain.serviceId);
        bool told = false;
        for (const auto& line : srv.reconcileLog()) told = told || line.find("now wrapped") != std::string::npos;
        KV_CHECK_MSG(told, "the reconcile log doesn't say the key was wrapped");
        srv.shutdown();
    }
    const std::string after = fileBytes(dir + L"\\pairing.json");
    KV_CHECK_MSG(after.find("privateKeyRaw") == std::string::npos, "pairing.json still holds the key plain");
    KV_CHECK(after.find("\"privateKeyWrapped\":") != std::string::npos);
    HostStreamServer again;
    KV_REQUIRE(again.start(loopbackOptions(dir), &err));
    KV_CHECK(again.serverFingerprintHex() == id->fingerprintHex());
    again.shutdown();
    KV_CHECK(fileBytes(dir + L"\\pairing.json").find("privateKeyRaw") == std::string::npos);
}

// A plain save whose replace failed midway can leave this PC's identity as pairing.json.prev with no pairing.json.
// A start puts it back before it loads and serves with the same identity: never a first run, so no paired device
// is lost. Beside a pairing.json, a leftover .prev is removed, and so are temp files of writes that never finished.
KV_TEST(v3_a_start_puts_an_interrupted_saves_prev_back_and_keeps_the_identity) {
    const std::wstring dir = freshStateDir(L"pj-prev-back");
    std::string err;
    std::string fingerprint, service;
    {
        HostStreamServer srv;
        KV_REQUIRE(srv.start(loopbackOptions(dir), &err));
        fingerprint = srv.serverFingerprintHex();
        service = srv.serviceId();
        srv.shutdown();
    }
    const std::string identity = fileBytes(dir + L"\\pairing.json");
    KV_REQUIRE(identity.find("\"privateKeyWrapped\":") != std::string::npos);
    KV_REQUIRE(MoveFileExW((dir + L"\\pairing.json").c_str(), (dir + L"\\pairing.json.prev").c_str(), 0));
    // Temp files of writes that never finished: an earlier build's name, and one whose process is gone.
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json.tmp", "{}"));
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json.tmp.4000000000", "{}"));
    {
        HostStreamServer srv;
        const bool started = srv.start(loopbackOptions(dir), &err);
        KV_CHECK_MSG(started, err);
        KV_REQUIRE(started);
        KV_CHECK_MSG(srv.serverFingerprintHex() == fingerprint, "a start over pairing.json.prev made a new identity");
        KV_CHECK(srv.serviceId() == service);
        bool told = false;
        for (const auto& line : srv.reconcileLog()) told = told || line.find("pairing.json again") != std::string::npos;
        KV_CHECK_MSG(told, "the reconcile log doesn't say pairing.json.prev was put back");
        srv.shutdown();
    }
    KV_CHECK(fileBytes(dir + L"\\pairing.json") == identity);
    KV_CHECK(!fileExists(dir + L"\\pairing.json.prev"));
    KV_CHECK(noPairingTmpIn(dir));
    // Beside a pairing.json, a leftover .prev of the same identity is removed, and the identity is pairing.json's.
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json.prev", identity));
    {
        HostStreamServer srv;
        KV_REQUIRE(srv.start(loopbackOptions(dir), &err));
        KV_CHECK(srv.serverFingerprintHex() == fingerprint);
        srv.shutdown();
    }
    KV_CHECK(!fileExists(dir + L"\\pairing.json.prev"));
}

// An interrupted save's pairing.json.prev that a start can't put back (another program holds it, as the one behind
// the failed replace may) fails the start closed: no new identity over the missing pairing.json, nothing serves,
// the error says to try again, and a reset is refused with nothing written. Once it lets go, the next start puts it
// back and serves with the same identity.
KV_TEST(v3_a_prev_that_cant_be_put_back_fails_closed_and_a_later_start_keeps_the_identity) {
    const std::wstring dir = freshStateDir(L"pj-prev-held");
    std::string err;
    std::string fingerprint;
    {
        HostStreamServer srv;
        KV_REQUIRE(srv.start(loopbackOptions(dir), &err));
        fingerprint = srv.serverFingerprintHex();
        srv.shutdown();
    }
    const std::string identity = fileBytes(dir + L"\\pairing.json");
    const std::wstring prev = dir + L"\\pairing.json.prev";
    KV_REQUIRE(MoveFileExW((dir + L"\\pairing.json").c_str(), prev.c_str(), 0));
    // Held without FILE_SHARE_DELETE: it can't be moved back while this handle is open.
    HANDLE h = CreateFileW(prev.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        HostStreamServer srv;
        bool unusable = false;
        const bool started = srv.start(loopbackOptions(dir), &err, &unusable);
        std::printf("  held .prev: started=%d unusable=%d: %s\n", started ? 1 : 0, unusable ? 1 : 0, err.c_str());
        KV_CHECK_MSG(!started, "a start over a pairing.json.prev it couldn't put back served");
        KV_CHECK(unusable);
        KV_CHECK_MSG(err.find("pairing.json.prev") != std::string::npos && err.find("try again") != std::string::npos, err);
        srv.shutdown();
    }
    KV_CHECK_MSG(!fileExists(dir + L"\\pairing.json"), "a new pairing.json was written beside the held .prev");
    std::string reset_err, note;
    KV_CHECK(resetHostIdentity(dir, &reset_err, &note) == IdentityReset::Refused);
    KV_CHECK_MSG(reset_err.find("couldn't be put back") != std::string::npos, reset_err);
    KV_CHECK_MSG(!fileExists(dir + L"\\pairing.json"), "the reset made a new identity beside the held .prev");
    CloseHandle(h);
    KV_CHECK(fileBytes(prev) == identity);
    {
        HostStreamServer srv;
        const bool started = srv.start(loopbackOptions(dir), &err);
        KV_CHECK_MSG(started, err);
        KV_REQUIRE(started);
        KV_CHECK_MSG(srv.serverFingerprintHex() == fingerprint, "the start after it let go made a new identity");
        srv.shutdown();
    }
    KV_CHECK(fileBytes(dir + L"\\pairing.json") == identity);
    KV_CHECK(!fileExists(prev));
}

// pairing.json held open by another program (no sharing): start fails, reports the unusable identity and says
// the .bad copy failed, and nothing is written. Once it lets go, a new start serves with the OLD identity and
// the paired owner connects. On the old code the start tried to write a new identity over it.
KV_TEST(v3_pairing_json_that_cant_be_read_fails_closed_and_a_later_start_keeps_the_identity) {
    const std::wstring dir = freshStateDir(L"pj-held");
    DeleteFileW((dir + L"\\pairing.json.bad").c_str());
    FakePhone owner(3);
    FakePhone::PairResult po;
    KV_REQUIRE(pairedHost(dir, owner, po) != 0);
    const std::string before = fileBytes(dir + L"\\pairing.json");
    HostPersistedState st;
    KV_REQUIRE(loadHostState(dir, st));
    HANDLE h = CreateFileW((dir + L"\\pairing.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        HostStreamServer srv;
        bool unusable = false;
        std::string err;
        const bool started = srv.start(loopbackOptions(dir), &err, &unusable);
        std::printf("  held: started=%d unusable=%d: %s\n", started ? 1 : 0, unusable ? 1 : 0, err.c_str());
        KV_CHECK(!started);
        KV_CHECK_MSG(unusable, "start didn't say the identity is unusable");
        KV_CHECK_MSG(err.find("couldn't be read") != std::string::npos && err.find("Win32 error") != std::string::npos, err);
        KV_CHECK_MSG(err.find("keeping a copy") != std::string::npos, err); // the .bad copy failed too, and it says so
        srv.shutdown();
    }
    CloseHandle(h);
    KV_CHECK_MSG(fileBytes(dir + L"\\pairing.json") == before, "pairing.json was written");
    KV_CHECK(noPairingTmpIn(dir));
    HostStreamServer srv;
    bool unusable = true;
    std::string err;
    KV_REQUIRE(srv.start(loopbackOptions(dir), &err, &unusable));
    KV_CHECK(!unusable);
    KV_CHECK(srv.serverFingerprintHex() == CryptoIdentity::fromRecord(st.privateKeyRaw, st.certificateDER)->fingerprintHex());
    KV_CHECK(srv.serviceId() == st.serviceId && srv.devices().size() == 1);
    FakePhone::PairResult again = po;
    again.streamPort = srv.streamPort();
    KV_CHECK_MSG(connectDevice(owner, again), "the owner paired with the old identity can't connect"); // pinned
    owner.close();
    srv.shutdown();
    KV_CHECK(fileBytes(dir + L"\\pairing.json") == before);
}

// An unparsable pairing.json whose .bad copy can't be made (pairing.json.bad.tmp, where the copy is made first, is
// a folder): the error says the copy failed, with the Win32 error, and the host still doesn't serve or touch
// pairing.json.
KV_TEST(v3_pairing_json_whose_bad_copy_fails_still_fails_closed) {
    const std::wstring dir = freshStateDir(L"pj-nocopy");
    FakePhone owner(3);
    FakePhone::PairResult po;
    const uint16_t port = pairedHost(dir, owner, po);
    KV_REQUIRE(port != 0);
    const std::string bad = "not json at all";
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json", bad));
    RemoveDirectoryW((dir + L"\\pairing.json.bad.tmp").c_str());
    KV_REQUIRE(CreateDirectoryW((dir + L"\\pairing.json.bad.tmp").c_str(), nullptr));
    HostStreamServer srv;
    bool unusable = false;
    std::string err;
    const bool started = srv.start(loopbackOptions(dir, port), &err, &unusable);
    std::printf("  started=%d unusable=%d: %s\n", started ? 1 : 0, unusable ? 1 : 0, err.c_str());
    KV_CHECK_MSG(!started, "the host started");
    KV_CHECK(unusable);
    KV_CHECK_MSG(err.find("keeping a copy") != std::string::npos && err.find("failed (Win32 error") != std::string::npos, err);
    KV_CHECK(fileBytes(dir + L"\\pairing.json") == bad);
    KV_CHECK(!somethingListensOn(port));
    KV_CHECK(!fileExists(dir + L"\\pairing.json.bad"));
    srv.shutdown();
    RemoveDirectoryW((dir + L"\\pairing.json.bad.tmp").c_str());
}

// No pairing.json at all is a first run, as before: an identity is made and saved, and start reports nothing
// unusable. Starting again (server or controller) keeps that identity: no start path changes it.
KV_TEST(v3_pairing_json_missing_is_a_first_run_and_no_start_changes_the_identity) {
    const std::wstring dir = freshStateDir(L"pj-first-run");
    DeleteFileW((dir + L"\\pairing.json.bad").c_str());
    std::string first;
    {
        HostStreamServer srv;
        bool unusable = true;
        std::string err;
        KV_REQUIRE(srv.start(loopbackOptions(dir), &err, &unusable));
        KV_CHECK(!unusable);
        first = fileBytes(dir + L"\\pairing.json");
        HostPersistedState st;
        KV_REQUIRE(loadHostState(dir, st));
        KV_CHECK(st.allowPhoneModeSwitch && st.serviceId.size() == 12);
        KV_CHECK(srv.serverFingerprintHex() == CryptoIdentity::fromRecord(st.privateKeyRaw, st.certificateDER)->fingerprintHex());
        srv.shutdown();
    }
    KV_CHECK(!fileExists(dir + L"\\pairing.json.bad"));
    for (int i = 0; i < 2; ++i) {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        srv.shutdown();
        HostController ctl;
        HostController::Options co;
        co.server = loopbackOptions(dir);
        std::string err;
        KV_REQUIRE(ctl.start(co, &err));
        ctl.stop();
    }
    KV_CHECK_MSG(fileBytes(dir + L"\\pairing.json") == first, "a start changed pairing.json");
}

// A new identity's certificate has a random common name, never a fixed one an internet scan could look for. A later
// start keeps the identity, and so the certificate every device pinned; a reset makes another random one.
KV_TEST(a_new_identitys_certificate_has_a_random_name_and_later_starts_keep_it) {
    const auto cnOf = [](const Bytes& der) {
        const unsigned char* p = der.data();
        X509* cert = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
        char cn[64] = {0};
        if (cert) {
            X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn));
            X509_free(cert);
        }
        return std::string(cn);
    };
    const auto randomName = [](const std::string& cn) {
        return cn.size() == 16 && cn.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    const std::wstring dir = freshStateDir(L"cert-cn");
    Bytes first;
    std::string first_cn;
    for (int run = 0; run < 2; ++run) {
        HostStreamServer srv;
        std::string err;
        KV_REQUIRE(srv.start(loopbackOptions(dir), &err));
        srv.shutdown();
        HostPersistedState st;
        KV_REQUIRE(loadHostState(dir, st));
        const std::string cn = cnOf(st.certificateDER);
        KV_CHECK_MSG(randomName(cn), cn);
        if (run == 0) {
            first = st.certificateDER;
            first_cn = cn;
        } else {
            KV_CHECK_MSG(st.certificateDER == first, "a later start changed the identity's certificate");
        }
    }
    const std::wstring other = freshStateDir(L"cert-cn-reset");
    std::string err;
    KV_REQUIRE(resetHostIdentity(other, &err) == IdentityReset::Done); // no pairing.json: a new identity
    HostPersistedState st;
    KV_REQUIRE(loadHostState(other, st));
    const std::string cn = cnOf(st.certificateDER);
    KV_CHECK_MSG(randomName(cn) && cn != first_cn, cn);
}

// Even an unusable identity read through the existence-check seam (fileState Unknown) fails closed: the start
// doesn't take the file for missing and make a first run's identity over it.
KV_TEST(v3_pairing_json_whose_existence_cant_be_checked_fails_closed) {
    const std::wstring dir = freshStateDir(L"pj-unknown");
    FakePhone owner(3);
    FakePhone::PairResult po;
    KV_REQUIRE(pairedHost(dir, owner, po) != 0);
    const std::string before = fileBytes(dir + L"\\pairing.json");
    setFileAttributesProbeForTest([](const std::wstring& path, unsigned long* e) -> unsigned long {
        const std::wstring name = L"\\pairing.json";
        if (path.size() >= name.size() && _wcsicmp(path.c_str() + path.size() - name.size(), name.c_str()) == 0) {
            *e = ERROR_ACCESS_DENIED;
            return INVALID_FILE_ATTRIBUTES;
        }
        const DWORD a = GetFileAttributesW(path.c_str());
        *e = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        return a;
    });
    HostStreamServer srv;
    bool unusable = false;
    std::string err;
    const bool started = srv.start(loopbackOptions(dir), &err, &unusable);
    setFileAttributesProbeForTest(nullptr);
    std::printf("  started=%d unusable=%d: %s\n", started ? 1 : 0, unusable ? 1 : 0, err.c_str());
    KV_CHECK(!started && unusable);
    KV_CHECK_MSG(err.find("couldn't be checked (Win32 error 5)") != std::string::npos, err);
    KV_CHECK(fileBytes(dir + L"\\pairing.json") == before);
    srv.shutdown();
}

// The owner's reset is the only way to a new identity: it keeps the old pairing.json as .bad, writes a new
// identity (another certificate and serviceId, owner-only), leaves pet-camera.json alone, and the next start
// serves with it. Every device listed before is gone and a device paired before is refused; a new pairing works.
KV_TEST(v3_pairing_json_reset_makes_a_new_identity_and_every_device_must_pair_again) {
    const std::wstring dir = freshStateDir(L"pj-reset");
    FakePhone owner(3), viewer(3);
    FakePhone::PairResult po, pv;
    std::string old_cert_fp, old_sid;
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
        KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
        srv.setSharingEnabled(true);
        srv.setPrivacyMode(true);
        old_cert_fp = srv.serverFingerprintHex();
        old_sid = srv.serviceId();
        srv.shutdown();
    }
    const std::string pet_before = fileBytes(dir + L"\\pet-camera.json");
    const std::string bad = "{\"serverIdentity\":";
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json", bad));
    {
        HostStreamServer srv;
        bool unusable = false;
        std::string err;
        KV_REQUIRE(!srv.start(loopbackOptions(dir), &err, &unusable));
        KV_CHECK(unusable);
        srv.shutdown();
    }
    std::string err, note;
    KV_REQUIRE(resetHostIdentity(dir, &err, &note) == IdentityReset::Done);
    std::printf("  reset: %s\n", note.c_str());
    KV_CHECK_MSG(note == "pairing.json.bad already held it", note); // the failed start kept it
    KV_CHECK(fileBytes(dir + L"\\pairing.json.bad") == bad);
    HostPersistedState st;
    KV_REQUIRE(loadHostState(dir, st));
    auto id = CryptoIdentity::fromRecord(st.privateKeyRaw, st.certificateDER);
    KV_REQUIRE(id != nullptr);
    KV_CHECK_MSG(id->fingerprintHex() != old_cert_fp, "the reset kept the old certificate");
    KV_CHECK(st.serviceId.size() == 12 && st.serviceId != old_sid);
    KV_CHECK(st.allowPhoneModeSwitch && st.allowPhoneSensorPowerControl && !st.turnOffKinectOnPhoneDisconnect);
    KV_CHECK(!st.pinnedClientCertSHA256);
    KV_CHECK(isOwnerOnly(dir + L"\\pairing.json"));
    KV_CHECK_MSG(fileBytes(dir + L"\\pet-camera.json") == pet_before, "the reset touched pet-camera.json");

    HostStreamServer srv;
    bool unusable = true;
    KV_REQUIRE(srv.start(loopbackOptions(dir), &err, &unusable));
    KV_CHECK(!unusable);
    KV_CHECK(srv.serverFingerprintHex() == id->fingerprintHex() && srv.serviceId() == st.serviceId);
    KV_CHECK_MSG(srv.devices().empty(), "devices paired with the old identity are still listed");
    KV_CHECK(srv.petSettings().privacyMode && srv.petSettings().sharingEnabled); // pet-camera.json as it was
    KV_CHECK_MSG(!helloFrom(owner, srv), "a device paired before the reset was admitted");
    KV_CHECK(!helloFrom(viewer, srv));
    FakePhone::PairResult again;
    KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, again)); // pairs again, with the new identity pinned
    KV_CHECK(again.serverHash == id->certificateSHA256());
    KV_CHECK(connectDevice(owner, again));
    owner.close();
    srv.shutdown();

    // A reset of a pairing.json that can be used is not needed: nothing is written or copied, and the caller just
    // starts with it. On the old code the reset trusted the earlier "unusable" and replaced a good identity.
    const std::string current = fileBytes(dir + L"\\pairing.json");
    KV_CHECK(resetHostIdentity(dir, &err, &note) == IdentityReset::NotNeeded);
    KV_CHECK_MSG(note == "pairing.json can be read now, so this PC's identity was kept", note);
    KV_CHECK(fileBytes(dir + L"\\pairing.json") == current);
    KV_CHECK(fileBytes(dir + L"\\pairing.json.bad") == bad && !fileExists(dir + L"\\pairing.json.bad.1"));
}

// The "unusable" the owner saw is stale: another program let go, or a human put the good file back, between the
// failed start and the reset. The reset loads it again, refuses, and a start serves with the OLD identity, so no
// device has to pair again.
KV_TEST(v3_pairing_json_reset_after_the_file_is_good_again_keeps_the_identity) {
    const std::wstring dir = freshStateDir(L"pj-reset-stale");
    FakePhone owner(3);
    FakePhone::PairResult po;
    const uint16_t port = pairedHost(dir, owner, po);
    KV_REQUIRE(port != 0);
    const std::string good = fileBytes(dir + L"\\pairing.json");
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json", "{\"serverIdentity\":"));
    {
        HostStreamServer srv;
        bool unusable = false;
        std::string err;
        KV_REQUIRE(!srv.start(loopbackOptions(dir, port), &err, &unusable));
        KV_CHECK(unusable);
        srv.shutdown();
    }
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json", good)); // put back
    std::string err, note;
    KV_CHECK(resetHostIdentity(dir, &err, &note) == IdentityReset::NotNeeded);
    KV_CHECK_MSG(fileBytes(dir + L"\\pairing.json") == good, "the reset replaced an identity that loads again");
    HostStreamServer srv;
    KV_REQUIRE(srv.start(loopbackOptions(dir, port), &err));
    FakePhone::PairResult pc = po;
    pc.streamPort = srv.streamPort();
    KV_CHECK_MSG(connectDevice(owner, pc), "the device paired before can't connect: the identity changed");
    owner.close();
    srv.shutdown();
}

// A reset whose .bad copy fails while pairing.json can still be read is refused: the old identity is never written
// over without a copy of it. On the old code the copy's failure was only noted and the file was replaced.
KV_TEST(v3_pairing_json_reset_is_refused_when_the_copy_fails_and_the_file_can_be_read) {
    const std::wstring dir = freshStateDir(L"pj-reset-nocopy");
    const std::string bad = "{\"serverIdentity\": not this";
    KV_REQUIRE(ensureOwnerOnlyDirectory(dir));
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json", bad));
    RemoveDirectoryW((dir + L"\\pairing.json.bad.tmp").c_str());
    KV_REQUIRE(CreateDirectoryW((dir + L"\\pairing.json.bad.tmp").c_str(), nullptr));
    std::string err, note;
    const IdentityReset r = resetHostIdentity(dir, &err, &note);
    std::printf("  reset: %s / %s\n", err.c_str(), note.c_str());
    KV_CHECK(r == IdentityReset::Refused);
    KV_CHECK_MSG(err.find("couldn't be kept") != std::string::npos && err.find("Win32 error") != std::string::npos, err);
    KV_CHECK_MSG(fileBytes(dir + L"\\pairing.json") == bad, "the reset wrote over the only copy of the old identity");
    KV_CHECK(!fileExists(dir + L"\\pairing.json.bad"));
    // The copy works again: the reset goes ahead and keeps it.
    RemoveDirectoryW((dir + L"\\pairing.json.bad.tmp").c_str());
    KV_CHECK(resetHostIdentity(dir, &err, &note) == IdentityReset::Done);
    KV_CHECK_MSG(note == "kept as pairing.json.bad", note);
    KV_CHECK(fileBytes(dir + L"\\pairing.json.bad") == bad && fileBytes(dir + L"\\pairing.json") != bad);
}

// The reset while devices.json can't be read: the list is dropped once it is finally read (row 4a, the new
// serviceId), not admitted from the old file.
KV_TEST(v3_pairing_json_reset_drops_the_list_also_when_devices_json_is_read_later) {
    const std::wstring dir = freshStateDir(L"pj-reset-held");
    FakePhone owner(3);
    FakePhone::PairResult po;
    KV_REQUIRE(pairedHost(dir, owner, po) != 0);
    KV_REQUIRE(writeBytes(dir + L"\\pairing.json", "not json")); // a reset is only for an identity that can't be used
    std::string err;
    KV_REQUIRE(resetHostIdentity(dir, &err) == IdentityReset::Done);
    HANDLE h = CreateFileW((dir + L"\\devices.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK(srv.devices().empty() && !srv.saveError().empty()); // not read yet
    sleepMs(300);
    CloseHandle(h);
    KV_CHECK_MSG(waitUntil([&] { return srv.saveError().empty(); }, 8000), srv.saveError());
    KV_CHECK_MSG(srv.devices().empty(), "the device list came back after the reset");
    KV_CHECK(!helloFrom(owner, srv));
    std::string list;
    KV_REQUIRE(readWholeFile(dir + L"\\devices.json", list));
    KV_CHECK_MSG(list.find("\"devices\":[]") != std::string::npos, list); // saved without them
    srv.shutdown();
}

// ---- a privacy-on asked for while pet-camera.json can't be read survives a restart ----------------------------

namespace {
HANDLE holdExclusively(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
} // namespace

// Privacy on from the PC while pet-camera.json is held (unread): it is on at once, and the durable
// privacy-on.pending marker is written. The host exits with the file still held, so pet-camera.json still says
// off. The next start reads it fine (off) but the marker wins: privacy is on, pet-camera.json is rewritten with
// it, and the marker goes. On the old code the request lived only in memory and the restart turned privacy off.
KV_TEST(v3_privacy_on_while_pet_camera_json_is_held_survives_a_restart) {
    const std::wstring dir = freshStateDir(L"privacy-pending");
    const std::wstring marker = dir + L"\\privacy-on.pending";
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        srv.setSharingEnabled(true); // pet-camera.json: sharing on, privacy off
        srv.shutdown();
    }
    const std::string off = fileBytes(dir + L"\\pet-camera.json");
    HANDLE h = holdExclusively(dir + L"\\pet-camera.json");
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        srv.setPrivacyMode(true);
        KV_CHECK(srv.petSettings().privacyMode);
        KV_CHECK_MSG(waitUntil([&] { return fileExists(marker); }, 3000), "no privacy-on.pending was written");
        srv.shutdown(); // pet-camera.json is still held: it can't be written at exit either
    }
    CloseHandle(h);
    KV_REQUIRE(fileBytes(dir + L"\\pet-camera.json") == off);
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        KV_CHECK_MSG(srv.petSettings().privacyMode, "privacy is off after the restart: the privacy-on was lost");
        KV_CHECK(srv.petSettings().sharingEnabled); // the file's own values otherwise
        KV_CHECK_MSG(waitUntil([&] { return loadPetSettings(dir).privacyMode; }, 3000), "pet-camera.json wasn't rewritten with privacy on");
        KV_CHECK_MSG(waitUntil([&] { return !fileExists(marker); }, 3000), "privacy-on.pending is still there");
        KV_CHECK(waitUntil([&] { return srv.saveError().empty(); }, 3000));
        srv.shutdown();
    }
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK(srv.petSettings().privacyMode && srv.petSettings().sharingEnabled);
    srv.shutdown();
}

// The same through a v3 owner's 0x0C set_privacy on: answered ok, kept across the restart.
KV_TEST(v3_privacy_on_by_0x0c_while_pet_camera_json_is_held_survives_a_restart) {
    const std::wstring dir = freshStateDir(L"privacy-pending-0x0c");
    const std::wstring marker = dir + L"\\privacy-on.pending";
    FakePhone owner(3);
    FakePhone::PairResult po;
    KV_REQUIRE(pairedHost(dir, owner, po) != 0);
    HANDLE h = holdExclusively(dir + L"\\pet-camera.json");
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        FakePhone::PairResult pc = po;
        pc.streamPort = srv.streamPort();
        KV_REQUIRE(connectDevice(owner, pc));
        PhoneRunner r(owner);
        r.start();
        KV_REQUIRE(r.waitFor([](const V3Log& l) { return !l.pets.empty(); }, 4000));
        r.send(MsgType::PetCameraCmd, cmdJson(7, pet_action::kSetPrivacy, true));
        KV_CHECK(r.waitFor([](const V3Log& l) { return l.result(7) != nullptr; }, 3000));
        KV_CHECK(r.log().result(7) && r.log().result(7)->ok);
        KV_CHECK_MSG(waitUntil([&] { return fileExists(marker); }, 3000), "no privacy-on.pending was written");
        r.stop();
        srv.shutdown();
    }
    CloseHandle(h);
    KV_REQUIRE(!loadPetSettings(dir).privacyMode);
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK_MSG(srv.petSettings().privacyMode, "privacy is off after the restart: the 0x0C privacy-on was lost");
    KV_CHECK(waitUntil([&] { return loadPetSettings(dir).privacyMode && !fileExists(marker); }, 3000));
    srv.shutdown();
}

// A marker whose existence can't be checked (fileState Unknown, through the seam) counts as there: privacy is
// on whatever pet-camera.json says, and the file is then saved with privacy on.
KV_TEST(v3_privacy_on_when_the_marker_cant_be_checked) {
    const std::wstring dir = freshStateDir(L"privacy-pending-unknown");
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        srv.setSharingEnabled(true); // privacy off on disk
        srv.shutdown();
    }
    setFileAttributesProbeForTest([](const std::wstring& path, unsigned long* e) -> unsigned long {
        const std::wstring name = L"\\privacy-on.pending";
        if (path.size() >= name.size() && _wcsicmp(path.c_str() + path.size() - name.size(), name.c_str()) == 0) {
            *e = ERROR_ACCESS_DENIED;
            return INVALID_FILE_ATTRIBUTES;
        }
        const DWORD a = GetFileAttributesW(path.c_str());
        *e = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        return a;
    });
    HostStreamServer srv;
    const bool started = startServer(srv, dir);
    setFileAttributesProbeForTest(nullptr);
    KV_REQUIRE(started);
    KV_CHECK_MSG(srv.petSettings().privacyMode, "privacy is off though privacy-on.pending couldn't be checked");
    KV_CHECK(waitUntil([&] { return loadPetSettings(dir).privacyMode; }, 3000));
    KV_CHECK(waitUntil([&] { return srv.saveError().empty(); }, 3000));
    srv.shutdown();
}

// A marker that couldn't be checked at start, while pet-camera.json is held (unread): privacy is on, but nothing
// is on disk to keep it. A privacy-on from the PC then writes the marker, so a restart that finds no marker and
// an "off" in pet-camera.json still comes up on. On the old code the unchecked marker counted as "privacy already
// asked", the PC's request returned early, nothing was written, and the restart came up off.
KV_TEST(v3_privacy_on_writes_the_marker_when_it_couldnt_be_checked_at_start) {
    const std::wstring dir = freshStateDir(L"privacy-pending-unknown-ask");
    const std::wstring marker = dir + L"\\privacy-on.pending";
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        srv.setSharingEnabled(true); // pet-camera.json: sharing on, privacy off
        srv.shutdown();
    }
    const std::string off = fileBytes(dir + L"\\pet-camera.json");
    HANDLE h = holdExclusively(dir + L"\\pet-camera.json");
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    {
        setFileAttributesProbeForTest([](const std::wstring& path, unsigned long* e) -> unsigned long {
            const std::wstring name = L"\\privacy-on.pending";
            if (path.size() >= name.size() && _wcsicmp(path.c_str() + path.size() - name.size(), name.c_str()) == 0) {
                *e = ERROR_ACCESS_DENIED;
                return INVALID_FILE_ATTRIBUTES;
            }
            const DWORD a = GetFileAttributesW(path.c_str());
            *e = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
            return a;
        });
        HostStreamServer srv;
        const bool started = startServer(srv, dir);
        setFileAttributesProbeForTest(nullptr);
        KV_REQUIRE(started);
        KV_CHECK(srv.petSettings().privacyMode); // unread and unchecked: on (fail closed)
        KV_CHECK(!fileExists(marker));
        srv.setPrivacyMode(true);
        KV_CHECK_MSG(waitUntil([&] { return fileExists(marker); }, 3000), "no privacy-on.pending was written");
        srv.shutdown(); // pet-camera.json is still held: it can't be written at exit either
    }
    CloseHandle(h);
    KV_REQUIRE(fileBytes(dir + L"\\pet-camera.json") == off);
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK_MSG(srv.petSettings().privacyMode, "privacy is off after the restart: the privacy-on was lost");
    KV_CHECK(waitUntil([&] { return loadPetSettings(dir).privacyMode && !fileExists(marker); }, 3000));
    srv.shutdown();
}

// A privacy-off the owner chooses after the marker forced privacy on is kept: the marker goes with ANY successful
// pet-camera.json write (every write already reflects it), so a restart doesn't turn privacy back on. pet-camera.json
// is held (readable, not replaceable) at start, so the forced-on write can't land before the PC's off: the write that
// finally goes through has privacy off. On the old code the marker survived that write (only a privacy-on write
// removed it) and the next restart came up with privacy on.
KV_TEST(v3_privacy_on_marker_is_removed_by_an_off_write_and_the_off_survives_a_restart) {
    const std::wstring dir = freshStateDir(L"privacy-pending-off");
    const std::wstring marker = dir + L"\\privacy-on.pending";
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        srv.setSharingEnabled(true); // pet-camera.json: sharing on, privacy off
        srv.shutdown();
    }
    KV_REQUIRE(writeBytes(marker, "left by an earlier run\n")); // a privacy-on asked for while the file was unread
    {
        FileHold hold(dir + L"\\pet-camera.json"); // readable, but the atomic replace fails until it lets go
        KV_REQUIRE(hold.ok());
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        KV_CHECK_MSG(srv.petSettings().privacyMode, "the marker didn't force privacy on");
        srv.setPrivacyMode(false); // the owner's choice, after privacy was shown on
        KV_CHECK(!srv.petSettings().privacyMode);
        sleepMs(300);
        KV_CHECK(fileExists(marker)); // nothing written yet
        hold.release();
        KV_CHECK_MSG(waitUntil([&] { return srv.saveError().empty(); }, 7000), srv.saveError());
        KV_CHECK(!loadPetSettings(dir).privacyMode && loadPetSettings(dir).sharingEnabled);
        KV_CHECK_MSG(!fileExists(marker), "privacy-on.pending is still there after pet-camera.json was written");
        srv.shutdown();
    }
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_CHECK_MSG(!srv.petSettings().privacyMode, "the restart turned privacy back on over the owner's off");
    srv.shutdown();
}

// A marker that can't be written (privacy-on.pending is a folder): the status line says so and the write is
// retried, while privacy is on in memory, so the 0x0C still answers ok.
KV_TEST(v3_privacy_on_marker_that_cant_be_written_is_shown_and_privacy_stays_on) {
    const std::wstring dir = freshStateDir(L"privacy-pending-fails");
    const std::wstring marker = dir + L"\\privacy-on.pending";
    RemoveDirectoryW(marker.c_str());
    FakePhone owner(3);
    FakePhone::PairResult po;
    KV_REQUIRE(pairedHost(dir, owner, po) != 0);
    HANDLE h = holdExclusively(dir + L"\\pet-camera.json");
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    KV_REQUIRE(CreateDirectoryW(marker.c_str(), nullptr));
    FakePhone::PairResult pc = po;
    pc.streamPort = srv.streamPort();
    KV_REQUIRE(connectDevice(owner, pc));
    PhoneRunner r(owner);
    r.start();
    KV_REQUIRE(r.waitFor([](const V3Log& l) { return !l.pets.empty(); }, 4000));
    r.send(MsgType::PetCameraCmd, cmdJson(9, pet_action::kSetPrivacy, true));
    KV_CHECK(r.waitFor([](const V3Log& l) { return l.result(9) != nullptr; }, 3000));
    KV_CHECK_MSG(r.log().result(9) && r.log().result(9)->ok, "the 0x0C privacy on wasn't answered ok");
    KV_CHECK(srv.petSettings().privacyMode);
    const bool shown = waitUntil([&] { return srv.saveError().find("privacy-on.pending") != std::string::npos; }, 2000);
    std::printf("  status line: %s\n", srv.saveError().c_str());
    KV_CHECK_MSG(shown, "a privacy-on.pending that couldn't be written isn't on the status line");
    // Writable again: the retry writes it and the line clears (pet-camera.json is still held: its own line stays).
    RemoveDirectoryW(marker.c_str());
    KV_CHECK_MSG(waitUntil([&] { return fileExists(marker); }, 7000), "the marker write wasn't retried");
    KV_CHECK(waitUntil([&] { return srv.saveError().find("privacy-on.pending") == std::string::npos; }, 3000));
    r.stop();
    CloseHandle(h);
    srv.shutdown();
}

// ---- device changes while devices.json can't be read are refused with a reason -------------------------------

// Revoke all, a single revoke, a rename and a role change, from the PC, while devices.json is held unreadable:
// each is refused with "Can't change devices until the device list can be read." and the read error, nothing is
// written over devices.json, and once it can be read both devices are back. On the old code revokeAllDevices
// returned 0 with no reason (the PC said "0 devices revoked"), and the others failed as "not found".
KV_TEST(v3_device_changes_while_devices_json_cant_be_read_are_refused_with_a_reason) {
    const std::wstring dir = freshStateDir(L"devices-held-revoke");
    FakePhone owner(3), viewer(3);
    {
        HostStreamServer srv;
        KV_REQUIRE(startServer(srv, dir));
        FakePhone::PairResult po, pv;
        KV_REQUIRE(pairDevice(srv, owner, PetRole::Owner, po));
        KV_REQUIRE(pairDevice(srv, viewer, PetRole::Viewer, pv));
        srv.shutdown();
    }
    const std::string before = fileBytes(dir + L"\\devices.json");
    HANDLE h = holdExclusively(dir + L"\\devices.json");
    KV_REQUIRE(h != INVALID_HANDLE_VALUE);
    HostStreamServer srv;
    KV_REQUIRE(startServer(srv, dir));
    const std::string sid = srv.serviceId();
    const std::string refusal = "Can't change devices until the device list can be read.";
    auto refused = [&](const std::string& err) {
        return err.find(refusal) != std::string::npos && err.find("devices.json") != std::string::npos;
    };
    std::string err;
    KV_CHECK(srv.revokeAllDevices(&err) == 0);
    std::printf("  revoke all: %s\n", err.c_str());
    KV_CHECK_MSG(refused(err), "revoke all wasn't refused with the reason: " + err);
    err.clear();
    KV_CHECK(!srv.revokeDevice(idOf(owner), &err));
    KV_CHECK_MSG(refused(err), "a revoke wasn't refused with the reason: " + err);
    err.clear();
    KV_CHECK(!srv.renameDevice(idOf(owner), "Kitchen", &err));
    KV_CHECK_MSG(refused(err), "a rename wasn't refused with the reason: " + err);
    err.clear();
    KV_CHECK(!srv.setDeviceRole(idOf(viewer), PetRole::Owner, &err));
    KV_CHECK_MSG(refused(err), "a role change wasn't refused with the reason: " + err);
    KV_CHECK(srv.serviceId() == sid); // nothing rotated
    sleepMs(300);
    CloseHandle(h);
    KV_CHECK_MSG(waitUntil([&] { return srv.devices().size() == 2; }, 8000), "the list was never read");
    KV_CHECK_MSG(fileBytes(dir + L"\\devices.json") == before, "devices.json was written over");
    // Readable: the same calls work, with no error.
    err.clear();
    KV_CHECK(srv.renameDevice(idOf(owner), "Kitchen", &err) && err.empty());
    KV_CHECK(srv.revokeDevice(idOf(viewer), &err) && err.empty());
    KV_CHECK(srv.revokeAllDevices(&err) == 1 && err.empty());
    srv.shutdown();
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return kvtest::runAll(argc, argv);
}

// Host deaths seen in a real run came after outside sessions, with "replaced" lines in the log and no fault
// recorded. Session churn, many times over: a device streaming live, the
// same device again mid-stream (it replaces the first), then the second one vanishing with a reset, closing
// cleanly, or a TLS-only connection from it that never says hello. The host must stay up and a fresh device must
// still stream at the end. KV_STRESS_ROUNDS sets the count (200 by default).
KV_TEST(v3_session_churn_replace_mid_stream_and_vanish_leaves_the_host_up) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-churn"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone first(3);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairDevice(srv, first, PetRole::Owner, pr));
    first.close();
    int rounds = 200;
    if (const char* e = std::getenv("KV_STRESS_ROUNDS")) rounds = std::max(1, std::atoi(e));
    int streamed = 0;
    for (int i = 0; i < rounds; ++i) {
        FakePhone a(3, first.identity());
        if (!connectDevice(a, pr)) continue;
        PhoneRunner ra(a);
        ra.start();
        ra.send(MsgType::LiveViewReq, watchJson(true, std::string(i % 2 ? "low" : "medium")));
        if (ra.waitFor([](const V3Log& l) { return l.aus > 0; }, 3000)) ++streamed;
        FakePhone b(3, first.identity());
        const bool b_up = connectDevice(b, pr); // replaces a, mid-stream
        if (i % 4 == 3) {
            // A TLS-only connection from the same device that never says hello.
            if (b_up) b.closeWithoutNotify();
            ra.stop();
            a.vanish();
            continue;
        }
        PhoneRunner rb(b);
        if (b_up) {
            rb.start();
            rb.send(MsgType::LiveViewReq, watchJson(true, std::string("low")));
            rb.waitFor([](const V3Log& l) { return l.aus > 0; }, i % 3 == 0 ? 50 : 1500);
        }
        ra.stop();
        if (i % 2) a.vanish(); else a.close();
        rb.stop();
        if (i % 3 == 0) b.vanish(); else if (i % 3 == 1) b.closeWithoutNotify(); else b.close();
    }
    std::printf("  %d rounds, %d streamed before the replace\n", rounds, streamed);
    KV_CHECK(streamed >= rounds / 2);
    // Still up: a fresh connection from the device streams.
    FakePhone again(3, first.identity());
    KV_REQUIRE(connectDevice(again, pr));
    PhoneRunner rg(again);
    rg.start();
    rg.send(MsgType::LiveViewReq, watchJson(true, std::string("low")));
    KV_CHECK(rg.waitFor([](const V3Log& l) { return l.got_hello && l.aus >= 10; }, 8000));
    rg.stop();
    again.close();
}

// Only a session that has said hello (its first ping) replaces an older one from the
// same device. A phone races dials, and the loser's TLS-only connection may land just after the winner's: it must
// never end the live stream. One that does say hello still replaces it (v3 §2.1).
KV_TEST(v3_a_tls_only_connection_from_the_same_device_never_ends_its_live_stream) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-hello-replace"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone a(3);
    FakePhone::PairResult pr;
    KV_REQUIRE(pairDevice(srv, a, PetRole::Owner, pr));
    KV_REQUIRE(connectDevice(a, pr));
    PhoneRunner ra(a);
    ra.start();
    ra.send(MsgType::LiveViewReq, watchJson(true, std::string("low")));
    KV_REQUIRE(ra.waitFor([](const V3Log& l) { return l.aus >= 5; }, 5000));
    // Three TLS-only connections from the same device, each closed without a hello: one without notify, one
    // cleanly, one held open past a second.
    for (int i = 0; i < 3; ++i) {
        FakePhone loser(3, a.identity());
        KV_REQUIRE(connectDevice(loser, pr));
        if (i == 2) sleepMs(1200);
        if (i == 0) loser.closeWithoutNotify(); else loser.close();
    }
    const int before = ra.log().aus;
    KV_CHECK(ra.waitFor([before](const V3Log& l) { return l.aus >= before + 10; }, 5000));
    const V3Log la = ra.log();
    KV_CHECK_MSG(!la.closed_by_host, "the live stream was ended by a connection that never said hello");
    // One that says hello replaces it.
    FakePhone winner(3, a.identity());
    KV_REQUIRE(connectDevice(winner, pr));
    PhoneRunner rw(winner);
    rw.start();
    KV_CHECK(ra.waitFor([](const V3Log& l) { return l.closed_by_host; }, 5000));
    rw.stop();
    ra.stop();
    winner.close();
    a.close();
}

// A connection waiting to replace its device's session (no hello yet) takes no watcher slot. With
// all four held (an owner and three viewers watching), the owner's second connection that never says hello must not
// take a viewer's slot: no viewer's picture stops.
KV_TEST(v3_a_connection_waiting_to_replace_takes_no_watcher_slot) {
    PoweredHost host;
    std::string err;
    KV_REQUIRE(host.start(freshStateDir(L"v3-replace-slot"), &err));
    HostStreamServer& srv = host.ctl.server();
    srv.setSharingEnabled(true);
    FakePhone o(3), v1(3), v2(3), v3(3);
    FakePhone::PairResult po, p1, p2, p3;
    KV_REQUIRE(pairDevice(srv, o, PetRole::Owner, po));
    KV_REQUIRE(pairDevice(srv, v1, PetRole::Viewer, p1));
    KV_REQUIRE(pairDevice(srv, v2, PetRole::Viewer, p2));
    KV_REQUIRE(pairDevice(srv, v3, PetRole::Viewer, p3));
    KV_REQUIRE(connectDevice(o, po) && connectDevice(v1, p1) && connectDevice(v2, p2) && connectDevice(v3, p3));
    PhoneRunner ro(o), r1(v1), r2(v2), r3(v3);
    for (PhoneRunner* r : {&ro, &r1, &r2, &r3}) {
        r->start();
        r->send(MsgType::LiveViewReq, watchJson(true, std::string("low")));
    }
    for (PhoneRunner* r : {&ro, &r1, &r2, &r3}) KV_REQUIRE(r->waitFor([](const V3Log& l) { return l.aus >= 3; }, 8000));
    // The owner's second connection: TLS done, no hello, held open for a while.
    FakePhone second(3, o.identity());
    KV_REQUIRE(connectDevice(second, po));
    sleepMs(1500);
    const int a1 = r1.log().aus, a2 = r2.log().aus, a3 = r3.log().aus;
    KV_CHECK(r1.waitFor([a1](const V3Log& l) { return l.aus >= a1 + 5; }, 4000));
    KV_CHECK(r2.waitFor([a2](const V3Log& l) { return l.aus >= a2 + 5; }, 4000));
    KV_CHECK(r3.waitFor([a3](const V3Log& l) { return l.aus >= a3 + 5; }, 4000));
    // Never, not even for a moment: the host drops a connection that never pings after a while, and the viewer would
    // get its slot back then, so the last state alone proves nothing.
    for (PhoneRunner* r : {&r1, &r2, &r3}) {
        const V3Log l = r->log();
        KV_CHECK(!l.closed_by_host);
        bool lost = false;
        for (const auto& p : l.pets) lost = lost || (p.liveUnavailableReason && *p.liveUnavailableReason == live_unavailable::kTooManyViewers);
        KV_CHECK_MSG(!lost, "a viewer was told too_many_viewers while the owner's second connection had not said hello");
    }
    second.close();
    for (PhoneRunner* r : {&ro, &r1, &r2, &r3}) r->stop();
}
