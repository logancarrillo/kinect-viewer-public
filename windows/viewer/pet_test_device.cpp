#include "tls_conn.hpp" // winsock2 before windows.h

#include "pet_test_device.hpp"

#include "wire_protocol.hpp"

#include <chrono>
#include <condition_variable>

namespace kvwin {

using namespace kvhost;

void PetTestDevice::start(const std::string& host, uint16_t pairPort, const Bytes& qrToken, uint16_t streamPort,
                          const std::string& name) {
    stop();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        st_ = TestDeviceStatus{};
        st_.started = true;
        st_.name = name;
    }
    th_ = std::thread([this, host, pairPort, qrToken, streamPort, name] { run(host, pairPort, qrToken, streamPort, name); });
}

void PetTestDevice::stop() {
    stop_ = true;
    std::shared_ptr<TlsConnection> c;
    {
        std::lock_guard<std::mutex> lk(m_);
        c = conn_;
    }
    if (c) c->close();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(m_);
    conn_.reset();
}

TestDeviceStatus PetTestDevice::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

void PetTestDevice::fail(const std::string& why) {
    std::lock_guard<std::mutex> lk(m_);
    if (st_.error.empty()) st_.error = why;
}

void PetTestDevice::run(std::string host, uint16_t pairPort, Bytes qrToken, uint16_t streamPort, std::string name) {
    std::string err;
    identity_ = CryptoIdentity::generate("kv2-pocket", &err);
    if (!identity_) {
        fail("identity: " + err);
        return;
    }
    if (!pair(host, pairPort, qrToken, name) || stop_) return;
    stream(host, streamPort);
}

// The phone's pairing handshake (PocketStreamClientCore): one pairRequest, one pairResponse, and the
// server's proof checked against the certificate the handshake showed.
bool PetTestDevice::pair(const std::string& host, uint16_t port, const Bytes& token, const std::string& name) {
    std::string err;
    auto captured = std::make_shared<Bytes>();
    auto captured_m = std::make_shared<std::mutex>();
    std::shared_ptr<TlsContext> ctx(TlsContext::create(
        false, *identity_,
        [captured, captured_m](const Bytes& h, const Bytes&) {
            std::lock_guard<std::mutex> lk(*captured_m);
            *captured = h;
            return true;
        },
        &err));
    if (!ctx) {
        fail("pairing TLS context: " + err);
        return false;
    }
    SOCKET s = tcpConnect(host, port, 3000, &err);
    if (s == INVALID_SOCKET) {
        fail("pairing connect: " + err);
        return false;
    }
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool got = false;
        PairResponseMessage resp;
    };
    auto sh = std::make_shared<Shared>();
    const Bytes my_hash = identity_->certificateSHA256();
    const Bytes my_der = identity_->certificateDER();
    TlsConnection::Callbacks cb;
    cb.on_ready = [=](TlsConnection& c) {
        Bytes server;
        {
            std::lock_guard<std::mutex> lk(*captured_m);
            server = *captured;
        }
        PairRequestMessage req;
        req.methodRaw = static_cast<uint8_t>(PairingMethod::QrToken);
        req.clientCertDER = my_der;
        req.clientProofHMAC = computePairingProof(token, server, my_hash, "kv2-pair-client");
        req.protocolVersion = kProtocolV3;
        req.deviceName = name;
        req.deviceKind = device_kind::kIphone;
        c.send(packFrame(MsgType::PairRequest, 0, toJson(req)));
    };
    cb.on_frame = [sh](TlsConnection& c, MsgType t, uint8_t, std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lk(sh->m);
        if (t == MsgType::PairResponse && fromJson(payload, sh->resp)) sh->got = true;
        sh->done = true;
        sh->cv.notify_all();
        c.close();
    };
    cb.on_closed = [sh](TlsConnection&, const std::string&) {
        std::lock_guard<std::mutex> lk(sh->m);
        sh->done = true;
        sh->cv.notify_all();
    };
    auto conn = TlsConnection::start(s, ctx, false, cb, 5000);
    {
        std::unique_lock<std::mutex> lk(sh->m);
        sh->cv.wait_for(lk, std::chrono::seconds(8), [&] { return sh->done || stop_.load(); });
    }
    conn->close();
    conn->join();
    std::lock_guard<std::mutex> lk(sh->m);
    if (!sh->got) {
        fail("no pairing response");
        return false;
    }
    if (!sh->resp.accepted) {
        fail("pairing refused: " + sh->resp.message);
        return false;
    }
    {
        std::lock_guard<std::mutex> clk(*captured_m);
        server_hash_ = *captured;
    }
    const Bytes expected = computePairingProof(token, server_hash_, my_hash, "kv2-pair-server");
    if (!constantTimeEqual(sh->resp.serverProofHMAC, expected)) {
        fail("server proof mismatch");
        return false;
    }
    std::lock_guard<std::mutex> slk(m_);
    st_.paired = true;
    st_.deviceId = sh->resp.deviceId.value_or(hexLower(my_hash.data(), 8));
    st_.role = sh->resp.role.value_or("");
    return true;
}

void PetTestDevice::stream(const std::string& host, uint16_t port) {
    std::string err;
    const Bytes pin = server_hash_;
    std::shared_ptr<TlsContext> ctx(TlsContext::create(
        false, *identity_, [pin](const Bytes& h, const Bytes&) { return constantTimeEqual(h, pin); }, &err));
    if (!ctx) {
        fail("stream TLS context: " + err);
        return;
    }
    SOCKET s = tcpConnect(host, port, 3000, &err);
    if (s == INVALID_SOCKET) {
        fail("stream connect: " + err);
        return;
    }
    TlsConnection::Callbacks cb;
    cb.skip_unknown_types = true; // a v3 device skips types it doesn't know
    cb.on_ready = [this](TlsConnection&) {
        std::lock_guard<std::mutex> lk(m_);
        st_.connected = true;
    };
    cb.on_frame = [this](TlsConnection&, MsgType t, uint8_t, std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lk(m_);
        switch (t) {
            case MsgType::HelloServer: st_.got_hello = true; break;
            case MsgType::StreamConfig: ++st_.configs; break;
            case MsgType::VideoAccessUnit: {
                ++st_.aus;
                VideoAccessUnitHeader h{};
                const uint8_t* body = nullptr;
                size_t body_len = 0;
                if (VideoAccessUnitHeader::decode(payload.data(), payload.size(), h, &body, &body_len) && h.isKeyframe()) {
                    ++st_.keyframes;
                }
                break;
            }
            case MsgType::PetCameraState: {
                PetCameraStateMessage m;
                if (!fromJson(payload, m)) break;
                ++st_.pet_states;
                st_.live_allowed = m.liveAllowed;
                st_.watching = m.watching;
                st_.privacy = m.privacyMode;
                st_.sharing = m.sharingEnabled;
                st_.reason = m.liveUnavailableReason.value_or("");
                st_.device_name = m.deviceName;
                break;
            }
            case MsgType::SessionNotice: {
                SessionNoticeMessage n;
                if (fromJson(payload, n)) st_.notice = n.reason;
                break;
            }
            default: break;
        }
    };
    cb.on_closed = [this](TlsConnection&, const std::string&) {
        std::lock_guard<std::mutex> lk(m_);
        st_.closed = true;
    };
    auto conn = TlsConnection::start(s, ctx, false, cb, 5000);
    {
        std::lock_guard<std::mutex> lk(m_);
        conn_ = conn;
    }
    // Pings with protocolVersion 3 every 250 ms once hello has arrived (the session becomes v3 then).
    while (!stop_) {
        bool hello = false, closed = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            hello = st_.got_hello;
            closed = st_.closed;
        }
        if (closed) break;
        if (hello) {
            PingMessage ping;
            ping.clientSendNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            ping.protocolVersion = kProtocolV3;
            conn->send(packFrame(MsgType::Ping, 0, toJson(ping)));
        }
        for (int i = 0; i < 25 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    conn->close();
    conn->join();
}

} // namespace kvwin
