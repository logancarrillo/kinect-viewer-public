// OpenSSL 3 TLS 1.3 over Winsock: mutual authentication with a leaf-pinning verify callback
// (port of CryptoIdentity.makeTLSParameters), and a connection with its own I/O thread.
#pragma once

#include "crypto_identity.hpp"
#include "wire_protocol.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace kvhost {

bool winsockInit();

// v3 §2.2: a session whose queued, unsent bytes would pass this is closed as dead. Before v3 the queue
// was unbounded.
constexpr size_t kMaxSendQueueBytes = 16u * 1024u * 1024u;

// (peer leaf SHA-256, peer leaf DER) -> accept?
using PeerVerifyFn = std::function<bool(const Bytes& sha256, const Bytes& der)>;

class TlsContext {
public:
    // TLS 1.3 only. Server contexts require a client certificate. The verify callback replaces chain
    // validation entirely (self-signed leaves), exactly like sec_protocol_options_set_verify_block.
    static std::unique_ptr<TlsContext> create(bool server, const CryptoIdentity& identity, PeerVerifyFn verify,
                                              std::string* err);
    ~TlsContext();
    SSL_CTX* get() const { return ctx_; }
    bool verifyPeer(const Bytes& sha, const Bytes& der) const { return verify_ ? verify_(sha, der) : false; }

private:
    SSL_CTX* ctx_ = nullptr;
    PeerVerifyFn verify_;
};

// The classes of roll messages a host-initiated stop drops from a session's queue.
constexpr uint8_t kDropPlaybackFrames = 1; // 0x19
constexpr uint8_t kDropRollThumbs = 2;     // 0x15, tagged with its requestId

class TlsConnection : public std::enable_shared_from_this<TlsConnection> {
public:
    struct Callbacks {
        std::function<void(TlsConnection&)> on_ready;                                    // handshake done
        std::function<void(TlsConnection&, MsgType, uint8_t, std::vector<uint8_t>&)> on_frame;
        std::function<void(TlsConnection&)> on_activity; // bytes arrived (the dead-phone watchdog)
        std::function<void(TlsConnection&, const std::string&)> on_closed;               // once, any reason
        // v2's stream connection skips a frame of a type it doesn't know (after the magic and
        // length checks); the pairing connection, like v1, drops the connection instead.
        bool skip_unknown_types = false;
        // Queued bytes that would pass this close the connection as dead (on_closed says why).
        size_t max_queued_bytes = kMaxSendQueueBytes;
        // A received frame whose header announces more than this closes the connection, before its
        // payload is waited for (the host reads devices with kMaxClientPayload).
        uint32_t max_inbound_payload = kWireMaxPayload;
    };

    // Takes ownership of the connected socket and starts the I/O thread.
    // nullptr (the socket closed) when its thread can't be started: a host short of memory turns one connection away,
    // and nothing throws on the caller's thread (an accept thread with no handler would end the host).
    static std::shared_ptr<TlsConnection> start(SOCKET s, std::shared_ptr<TlsContext> ctx, bool is_server,
                                                Callbacks cb, int handshake_timeout_ms = 10000);
    ~TlsConnection();

    // Queues bytes for sending. Video frames are tracked for back-pressure (see videoInFlight()). Bytes
    // that would take the queue past Callbacks::max_queued_bytes close the connection instead. drop_class (not 0)
    // marks a roll message a host-initiated stop may drop while it is still queued (dropQueued), with drop_tag
    // (a request id) handed back so its request can still be answered. Control messages never carry one.
    void send(std::vector<uint8_t> bytes, bool is_video = false, uint8_t drop_class = 0, uint64_t drop_tag = 0);
    bool videoInFlight() const { return video_in_flight_.load(); }
    size_t queuedBytes() const { return queued_bytes_.load(); }
    bool sendQueueOverflowed() const { return overflowed_.load(); }
    void closeAfterFlush();
    // Drops every queued video frame not yet started on the wire (a frame part written stays, or the
    // stream would break mid-frame). For a session being told it is closed (0x0F): the notice goes next.
    void dropQueuedVideo();
    // Drops every queued message of that class not yet started on the wire, and returns their tags in order.
    std::vector<uint64_t> dropQueued(uint8_t drop_class);
    void close(); // asynchronous; on_closed fires from the I/O thread
    void join();  // waits for the I/O thread (never call from its own callbacks)

    Bytes peerCertDer() const;
    bool isReady() const { return ready_.load(); }
    bool isClosed() const { return closed_.load(); }
    // The handshake failed because the context's verify callback refused the peer's certificate, told apart from
    // every other failure by the verify result, as ServerHandshake::run does. Set before on_closed runs.
    bool certRefused() const { return cert_refused_.load(); }
    uint64_t sentVideoFrames() const { return sent_video_.load(); }
    uint64_t sentBytes() const { return sent_bytes_.load(); }

private:
    friend class ServerHandshake;
    TlsConnection() = default;
    void ioLoop(int handshake_timeout_ms);
    // ioLoop, with anything a callback throws (std::bad_alloc on a short-of-memory PC) closing this connection only:
    // never std::terminate on the connection's thread.
    void ioLoopGuarded(int handshake_timeout_ms);

public:
    // Test seam: the next n thread starts fail as if Windows had none to give (start() and handOff() return nullptr).
    static void failThreadStartsForTest(int n);
    // Test seam: the next n connections' SSL_new returns null, as on a PC short of memory.
    static void failTlsSetupForTest(int n);
    // Test seam: the next n completed handshakes' peer certificate reads back empty (peerCertDer), as when it can't
    // be re-encoded on a PC short of memory.
    static void failPeerCertReadForTest(int n);

private:
    bool flushLocked(std::string* why);
    void finish(const std::string& why);

    SOCKET sock_ = INVALID_SOCKET;
    SSL* ssl_ = nullptr;
    std::shared_ptr<TlsContext> ctx_;
    bool is_server_ = true;
    bool adopted_ = false; // handed over by a ServerHandshake: the handshake is done, ssl_ is set
    Callbacks cb_;
    WSAEVENT sock_event_ = WSA_INVALID_EVENT;
    HANDLE wake_event_ = nullptr;
    std::thread io_;

    mutable std::mutex m_;
    struct Out {
        std::vector<uint8_t> bytes;
        size_t off = 0;
        bool video = false;
        uint8_t drop_class = 0;
        uint64_t drop_tag = 0;
        bool started = false; // SSL_write was given it: it must be finished (partial writes, retries)
    };
    std::deque<Out> queue_;
    bool close_after_flush_ = false;
    Bytes peer_der_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> cert_refused_{false};
    std::atomic<bool> video_in_flight_{false};
    std::atomic<size_t> queued_bytes_{0};
    std::atomic<bool> overflowed_{false};
    std::atomic<uint64_t> sent_video_{0};
    std::atomic<uint64_t> sent_bytes_{0};
};

// A server TLS 1.3 handshake run on the calling thread, for the outside listener's bounded pool (v3 §8.1: no
// thread per connection before it is authenticated), and the redemption-only exchange that may follow it. A
// paired device's connection is then handed to a TlsConnection with its own I/O thread (handOff), as a LAN
// session is. Nothing is read from or written to the peer but the handshake until the caller reads or sends.
class ServerHandshake {
public:
    using Clock = std::chrono::steady_clock;
    // Rejected: the client's certificate arrived and the context's verify callback refused it (v3 §8.5: the
    // only handshake outcome that counts toward a block). Failed: anything else that went wrong first (the peer
    // closing or resetting, bytes that aren't TLS, an alert, no certificate at all).
    enum class Result { Ok, Rejected, Failed, TimedOut, Aborted };
    enum class Read { Frame, TimedOut, Closed, Invalid, Aborted };

    ServerHandshake(SOCKET s, std::shared_ptr<TlsContext> ctx); // takes ownership of the socket
    ~ServerHandshake();                                         // closes it, unless it was handed off
    ServerHandshake(const ServerHandshake&) = delete;
    ServerHandshake& operator=(const ServerHandshake&) = delete;

    // `firstRecordBy`: a peer whose ClientHello hasn't been read whole by then is dropped as timed out, so a
    // stranger can't sit on a slot by connecting and saying nothing (or sending a few bytes). It counts only while
    // the ClientHello is still being read; after that the handshake deadline alone applies.
    Result run(Clock::time_point deadline, std::string* why, Clock::time_point firstRecordBy = Clock::time_point::max());
    // Open, then certified, evicted or done, once: certified only when the handshake has
    // completed (the client's CertificateVerify has proved its key), never at its Certificate message; evicted only
    // while open, by the listener giving its slot to a newcomer. A certified or done handshake is never evicted.
    bool markEvicted() {
        int open = 0;
        return state_.compare_exchange_strong(open, 2);
    }
    bool certified() const { return state_.load() == 1; }
    // Any thread: a run, read or send in progress returns (Aborted) within a moment, and later ones at once.
    // Ignored once committed.
    void abort();
    // The exchange is committed (v3 §8.1: a redemption that added the device): its reply must go out, so later
    // aborts are ignored and an earlier one is undone. Call it under the same lock the aborts are made under,
    // so none can land after it. The reply is still bounded by sendAndClose's deadline.
    void commit();
    Bytes peerCertDer() const { return peer_der_; } // after Ok
    // One whole frame within the deadline, its header announcing at most max_payload; anything else (a frame
    // too big, bytes that aren't a frame, the peer closing) is reported as such.
    Read readOneFrame(Clock::time_point deadline, uint32_t max_payload, MsgType& type, std::vector<uint8_t>& payload,
                      std::string* why);
    // Writes the bytes, then close_notify. False when they couldn't all be written by the deadline.
    bool sendAndClose(const std::vector<uint8_t>& bytes, Clock::time_point deadline);
    // The paired session: the socket and the TLS state go to a connection of its own (its on_ready runs on
    // its I/O thread, as for a LAN connection). Once only, after Ok.
    // nullptr (the socket and TLS state freed) when the connection's thread can't be started.
    std::shared_ptr<TlsConnection> handOff(std::shared_ptr<TlsContext> ctx, TlsConnection::Callbacks cb);

private:
    bool waitIo(Clock::time_point deadline); // false once aborted
    SOCKET sock_ = INVALID_SOCKET;
    SSL* ssl_ = nullptr;
    WSAEVENT sock_event_ = WSA_INVALID_EVENT;
    HANDLE wake_event_ = nullptr; // manual reset: set by abort()
    std::shared_ptr<TlsContext> ctx_;
    std::atomic<bool> aborted_{false};
    std::atomic<int> state_{0}; // 0 open, 1 certified, 2 evicted, 3 done (ended any other way)
    Result runSteps(Clock::time_point deadline, std::string* why, Clock::time_point firstRecordBy);
    std::atomic<bool> committed_{false};
    Bytes peer_der_;
};

// A fixed number of worker threads for the outside listener's handshakes and redemptions (v3 §8.1): sized to the limiter's caps, so the threads never grow with the sockets. A job
// posted first (a recently paired source's reserved slot) runs before those already queued.
class HandshakePool {
public:
    explicit HandshakePool(int workers);
    ~HandshakePool(); // stop()
    HandshakePool(const HandshakePool&) = delete;
    HandshakePool& operator=(const HandshakePool&) = delete;
    bool post(std::function<void()> job, bool first); // false once stopped (the job is dropped)
    // No more jobs; queued ones are dropped (destroyed, so what they hold closes) and the workers joined. The
    // caller ends the jobs that are running first (ServerHandshake::abort). Never call it from a worker.
    void stop();
    int workers() const { return static_cast<int>(threads_.size()); }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

// Listening socket with an accept thread. Dual-stack (IPv6 + IPv4-mapped) unless loopback_only, which
// binds 127.0.0.1 only (tests; no firewall prompt). on_accept gets the peer's address as accept() read it
// (v3 §8.0: every listener reads it; an IPv4 peer of the dual-stack socket arrives IPv4-mapped).
class TcpListener {
public:
    ~TcpListener() { stop(); }
    bool start(uint16_t port, bool loopback_only, std::function<void(SOCKET, const sockaddr*)> on_accept, std::string* err);
    void stop();        // closes the socket and joins the accept thread
    void requestStop(); // closes the socket only (safe while holding locks the accept callback needs)
    uint16_t port() const { return port_; }
    // The address the socket is bound to, as getsockname read it at start ("127.0.0.1", "::"); "" if not started.
    std::string boundAddress() const { return bound_; }

private:
    SOCKET sock_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::string bound_;
    std::thread th_;
    std::atomic<bool> stop_{false};
};

// Whether a TcpListener could bind this port now, on the address its start(port, loopback_only) binds
// (127.0.0.1, or [::] dual-stack). Bind only, never listen (no firewall prompt); the socket is closed at once.
bool tcpListenerPortBinds(uint16_t port, bool loopback_only);

// TCP connect helper for clients/tests (blocking, with timeout).
SOCKET tcpConnect(const std::string& host, uint16_t port, int timeout_ms, std::string* err);

} // namespace kvhost
