// Test-only stand-in for the iPhone's PocketStreamClientCore, written from the Swift: same TLS 1.3
// client settings, same pairing messages and proofs, same stream handling. It is NOT the iOS app.
#pragma once

#include "crypto_identity.hpp"
#include "pairing_payload.hpp"
#include "tls_conn.hpp"
#include "wire_protocol.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kvtest {

using kvhost::Bytes;

class FakePhone {
public:
    // protocol_version 1 is the old app: its reader treats 0x07 and up as a protocol violation (the
    // real one then drops the connection and reconnects in a loop), and its pings carry no
    // protocolVersion. 2 is the v2 app: it says v2 in every ping and skips unknown types. 3 is a v3
    // pet-camera device (protocol-v3-pet-camera.md): it pings with 3, reads 0x0B-0x0F, and its
    // pairRequest carries protocolVersion 3 with its name and kind.
    explicit FakePhone(int protocol_version = 1)
        : version_(protocol_version), reader_(protocol_version, protocol_version >= 2) {
        identity_ = kvhost::CryptoIdentity::generate("kv2-pocket");
    }
    // The same phone's certificate in another app (an old build on the same paired phone): the host
    // can't tell them apart by the pin, only by what each one says.
    FakePhone(int protocol_version, const kvhost::CryptoIdentity& same_identity)
        : version_(protocol_version), reader_(protocol_version, protocol_version >= 2) {
        identity_ = kvhost::CryptoIdentity::fromRecord(same_identity.privateKeyRaw(), same_identity.certificateDER());
    }
    ~FakePhone() { close(); }

    const kvhost::CryptoIdentity& identity() const { return *identity_; }
    int version() const { return version_; }
    std::optional<int> pingVersion() const { return version_ >= 2 ? std::optional<int>(version_) : std::nullopt; }
    // v3 pairRequest keys (sent only by a v3 phone; an empty name is left out, so the host names it).
    void setDeviceName(const std::string& n) { device_name_ = n; }
    void setDeviceKind(const std::string& k) { device_kind_ = k; }
    // A slow device: a tiny socket receive buffer, so the host's sends back up as soon as it stops reading.
    void setSmallReceiveBuffer(bool on) { small_rcvbuf_ = on; }
    // The pairRequest's methodRaw (tests send 2, an invite, to the LAN pairing listener).
    void setPairMethodOverride(std::optional<uint8_t> m) { method_override_ = m; }
    // The pairRequest's clientCertDER (and the proof's client hash) from another certificate than the TLS leaf
    // (a W8 invite test: the host burns the invite and closes without a reply).
    void setClientCertOverride(std::optional<Bytes> der) { cert_override_ = std::move(der); }
    // A frame this phone's reader refused (for a v1 phone: a v2 message reached it).
    bool violated() const { return violated_; }
    std::string violation() const { return violation_; }

    struct PairResult {
        bool ok = false;
        std::string error;
        std::string serviceId;
        uint16_t streamPort = 0;
        Bytes serverHash;
        // v3 §4.4 pairResponse keys (absent from a v2 host)
        std::optional<std::string> role;
        std::optional<std::string> deviceId;
        std::optional<std::string> deviceName;
        std::optional<int> protocolVersion;
        bool closedWithoutReply = false;
    };

    // executePairingHandshakeOnQueue
    PairResult pair(const std::string& host, uint16_t port, const std::optional<Bytes>& expected_server_hash,
                    kvhost::PairingMethod method, const Bytes& secret) {
        PairResult r;
        if (!open(host, port, expected_server_hash, &r.error)) return r;
        r.serverHash = captured_server_hash_;
        kvhost::PairRequestMessage req;
        req.methodRaw = method_override_ ? *method_override_ : static_cast<uint8_t>(method);
        req.clientCertDER = cert_override_ ? *cert_override_ : identity_->certificateDER();
        req.clientProofHMAC = kvhost::computePairingProof(secret, r.serverHash, kvhost::sha256(req.clientCertDER), "kv2-pair-client");
        if (version_ >= 3) {
            req.protocolVersion = version_;
            if (!device_name_.empty()) req.deviceName = device_name_;
            req.deviceKind = device_kind_;
        }
        if (!sendFrame(kvhost::MsgType::PairRequest, kvhost::toJson(req))) {
            r.error = "send failed";
            close();
            return r;
        }
        kvhost::MsgType t;
        uint8_t flags = 0;
        std::vector<uint8_t> payload;
        kvhost::PairResponseMessage resp;
        if (!readFrame(t, flags, payload, 5000) || t != kvhost::MsgType::PairResponse || !kvhost::fromJson(payload, resp)) {
            r.error = "Invalid pairing response.";
            r.closedWithoutReply = lastReadWasClose();
            close();
            return r;
        }
        close();
        if (!resp.accepted) {
            r.error = resp.message;
            return r;
        }
        const Bytes expected = kvhost::computePairingProof(secret, r.serverHash, identity_->certificateSHA256(), "kv2-pair-server");
        if (!kvhost::constantTimeEqual(resp.serverProofHMAC, expected)) {
            r.error = "Server authentication proof mismatch.";
            return r;
        }
        r.ok = true;
        r.serviceId = resp.serviceId;
        r.streamPort = resp.streamPort;
        r.role = resp.role;
        r.deviceId = resp.deviceId;
        r.deviceName = resp.deviceName;
        r.protocolVersion = resp.protocolVersion;
        return r;
    }

    // connectToStreamEndpointOnQueue: TLS with the pinned server hash. Note: in TLS 1.3 the client's
    // handshake can complete before the server has checked the client certificate, so a host-side
    // rejection shows up as a failed first read (no HelloServer), as on the phone.
    bool connectStream(const std::string& host, uint16_t port, const std::optional<Bytes>& pinned_server_hash, std::string* err) {
        return open(host, port, pinned_server_hash, err);
    }

    // The pairing connection on its own (no pairRequest), for tests that send it something else.
    bool connectPairing(const std::string& host, uint16_t port, std::string* err) {
        return open(host, port, std::nullopt, err);
    }

    bool sendFrame(kvhost::MsgType t, const std::string& json) { return sendRaw(static_cast<uint8_t>(t), json); }

    // Bytes as they are, framed or not (a header that announces more than follows, for instance).
    bool sendBytes(const std::vector<uint8_t>& bytes) {
        if (!ssl_) return false;
        size_t off = 0;
        while (off < bytes.size()) {
            const int n = SSL_write(ssl_, bytes.data() + off, static_cast<int>(bytes.size() - off));
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    // Waits for the host to close the connection, reading (and dropping) whatever comes first. True
    // when it closed within timeout_ms, false when it was still open then.
    bool waitForHostClose(int timeout_ms) {
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        while (GetTickCount64() < until) {
            kvhost::MsgType t;
            uint8_t flags = 0;
            std::vector<uint8_t> payload;
            const int left = static_cast<int>(until - GetTickCount64());
            if (readFrame(t, flags, payload, std::max(150, std::min(left, 500)))) continue;
            if (lastReadWasClose() || violated()) return true;
        }
        return false;
    }

    // Any type byte, even one no version knows (to check the host skips it).
    bool sendRaw(uint8_t type, const std::string& payload) {
        if (!ssl_) return false;
        const auto pkt = kvhost::packFrame(static_cast<kvhost::MsgType>(type), 0, payload);
        size_t off = 0;
        while (off < pkt.size()) {
            const int n = SSL_write(ssl_, pkt.data() + off, static_cast<int>(pkt.size() - off));
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool readFrame(kvhost::MsgType& t, uint8_t& flags, std::vector<uint8_t>& payload, int timeout_ms) {
        if (!ssl_) return false;
        DWORD tv = static_cast<DWORD>(timeout_ms);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        while (true) {
            std::string why;
            const auto res = reader_.next(t, flags, payload, &why);
            if (res == kvhost::FrameReader::Result::Frame) return true;
            if (res == kvhost::FrameReader::Result::Unknown) continue; // v2: skipped
            if (res == kvhost::FrameReader::Result::Violation) {
                violated_ = true;
                violation_ = why;
                return false;
            }
            uint8_t buf[65536];
            const ULONGLONG t0 = GetTickCount64();
            const int n = SSL_read(ssl_, buf, sizeof(buf));
            if (n <= 0) {
                // A close_notify, or a read that failed well before its timeout (an EOF or a reset:
                // OpenSSL 3 reports those in more than one way), is the host closing the connection.
                const int e = SSL_get_error(ssl_, n);
                const bool early = GetTickCount64() - t0 + 100 < static_cast<ULONGLONG>(timeout_ms);
                last_read_closed_ = e == SSL_ERROR_ZERO_RETURN || early;
                ERR_clear_error();
                return false;
            }
            last_read_closed_ = false;
            reader_.append(buf, static_cast<size_t>(n));
        }
    }

    // The last failed read was the host closing the connection, not a timeout.
    bool lastReadWasClose() const { return last_read_closed_; }

    void close() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        ctx_.reset();
        reader_.reset();
    }

    // A phone that vanishes mid-stream (out of signal, the radio switched): no close_notify and a TCP reset, so the host's
    // next read or write fails at once rather than seeing an orderly end.
    void vanish() {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (sock_ != INVALID_SOCKET) {
            linger lg{1, 0};
            setsockopt(sock_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        ctx_.reset();
        reader_.reset();
    }

    // The TCP socket closed with no TLS close_notify (a link that just drops, an app killed): SSL_free sends nothing,
    // so the host reads a bare FIN after the handshake (OpenSSL 3: "unexpected eof while reading").
    void closeWithoutNotify() {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        ctx_.reset();
        reader_.reset();
    }

    const Bytes& capturedServerHash() const { return captured_server_hash_; }

private:
    int version_ = 1;
    std::unique_ptr<kvhost::CryptoIdentity> identity_;
    std::unique_ptr<kvhost::TlsContext> ctx_;
    SSL* ssl_ = nullptr;
    SOCKET sock_ = INVALID_SOCKET;
    kvhost::FrameReader reader_;
    Bytes captured_server_hash_;
    bool violated_ = false;
    std::string violation_;
    bool last_read_closed_ = false;
    std::string device_name_;
    std::string device_kind_ = "iphone";
    bool small_rcvbuf_ = false;
    std::optional<uint8_t> method_override_;
    std::optional<Bytes> cert_override_;

    bool open(const std::string& host, uint16_t port, const std::optional<Bytes>& expected, std::string* err) {
        close();
        captured_server_hash_.clear();
        ctx_ = kvhost::TlsContext::create(false, *identity_,
                                          [this, expected](const Bytes& h, const Bytes&) {
                                              if (expected && !kvhost::constantTimeEqual(h, *expected)) return false;
                                              captured_server_hash_ = h;
                                              return true;
                                          },
                                          err);
        if (!ctx_) return false;
        sock_ = kvhost::tcpConnect(host, port, 3000, err);
        if (sock_ == INVALID_SOCKET) return false;
        if (small_rcvbuf_) {
            int rcv = 4096; // a fixed small window (and no autotuning) for the slow-device tests
            setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
        }
        ssl_ = SSL_new(ctx_->get());
        SSL_set_fd(ssl_, static_cast<int>(sock_));
        DWORD tv = 5000;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        if (SSL_connect(ssl_) != 1) {
            if (err) *err = "TLS handshake failed";
            ERR_clear_error();
            close();
            return false;
        }
        if (captured_server_hash_.empty()) {
            if (err) *err = "Missing server certificate.";
            close();
            return false;
        }
        return true;
    }
};

} // namespace kvtest
