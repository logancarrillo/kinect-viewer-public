#include "tls_conn.hpp"

#include <mstcpip.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <system_error>
#include <typeinfo>

namespace kvhost {

namespace {
// After a close-after-flush: how long the socket waits for the peer to close its side before it is closed anyway.
constexpr DWORD kCloseLingerMs = 2000;
// The most a close-after-flush reads and drops from a peer that keeps sending: past it the socket is closed even
// before kCloseLingerMs, so a flood (or a device stuck sending) can't hold the connection's thread.
constexpr size_t kCloseLingerMaxBytes = 1u << 20;

// The close-after-flush linger, shared by TlsConnection::ioLoop and ServerHandshake::sendAndClose. Called after
// SSL_shutdown and shutdown(SD_SEND), on a non-blocking socket selected on
// sock_event: read and drop what the peer still sends until its FIN (it has read everything before it closes), a
// reset, stopped(), kCloseLingerMs, or kCloseLingerMaxBytes. Nothing is left unread at closesocket when the peer
// behaves, so the close stays a FIN, not a reset; a reset would throw away what the peer hasn't read yet (a 0x0F, an
// accepted pairResponse). The deadline and the byte cap are checked on every pass, after data too, so a peer that
// never stops sending is still closed on time.
void drainBeforeClose(SOCKET sock, WSAEVENT sock_event, HANDLE wake_event, const std::function<bool()>& stopped) {
    HANDLE handles[2] = {sock_event, wake_event};
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCloseLingerMs);
    char drop[4096];
    size_t drained = 0;
    while (!stopped()) {
        if (std::chrono::steady_clock::now() >= until || drained >= kCloseLingerMaxBytes) break;
        const int n = recv(sock, drop, static_cast<int>(sizeof(drop)), 0);
        if (n > 0) {
            drained += static_cast<size_t>(n);
            continue;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) break;
        const auto now = std::chrono::steady_clock::now();
        if (now >= until) break;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count() + 1;
        WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(left < 200 ? left : 200));
        WSANETWORKEVENTS ne{};
        WSAEnumNetworkEvents(sock, sock_event, &ne);
    }
}
} // namespace

bool winsockInit() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA w;
        ok = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    });
    return ok;
}

namespace {

std::string sslErrors() {
    std::string s;
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!s.empty()) s += "; ";
        s += buf;
    }
    return s;
}

Bytes x509Der(X509* x) {
    unsigned char* der = nullptr;
    const int n = i2d_X509(x, &der);
    if (n <= 0) return {};
    Bytes out(der, der + n);
    OPENSSL_free(der);
    return out;
}

int verifyCallback(X509_STORE_CTX* xs, void* arg) {
    auto* self = static_cast<TlsContext*>(arg);
    X509* leaf = X509_STORE_CTX_get0_cert(xs);
    if (!leaf || !self) {
        X509_STORE_CTX_set_error(xs, X509_V_ERR_CERT_REJECTED);
        return 0;
    }
    const Bytes der = x509Der(leaf);
    const Bytes sha = sha256(der);
    if (!self->verifyPeer(sha, der)) {
        X509_STORE_CTX_set_error(xs, X509_V_ERR_CERT_REJECTED);
        return 0;
    }
    return 1;
}

void tuneSocket(SOCKET s) {
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof(one));
    // keepaliveIdle 2 s, keepaliveInterval 1 s, keepaliveCount 3 (the phase 7 Swift NWProtocolTCP
    // options). TCP_KEEPCNT needs Windows 10 1703+; where it is refused, Windows probes 10 times.
    // The stream server's 5 s ping watchdog is what actually notices a dead phone in time.
    tcp_keepalive ka{1, 2000, 1000};
    DWORD ret = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret, nullptr, nullptr);
#ifdef TCP_KEEPCNT
    DWORD probes = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<const char*>(&probes), sizeof(probes));
#endif
#ifdef TCP_MAXRT
    DWORD max_rt_sec = 5; // give up on unacknowledged data after 5 s (connectionDropTime)
    setsockopt(s, IPPROTO_TCP, TCP_MAXRT, reinterpret_cast<const char*>(&max_rt_sec), sizeof(max_rt_sec));
#endif
}

} // namespace

// ---- TlsContext ---------------------------------------------------------------------------------

std::unique_ptr<TlsContext> TlsContext::create(bool server, const CryptoIdentity& id, PeerVerifyFn verify, std::string* err) {
    auto t = std::unique_ptr<TlsContext>(new TlsContext());
    t->verify_ = std::move(verify);
    t->ctx_ = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
    if (!t->ctx_) {
        if (err) *err = "SSL_CTX_new: " + sslErrors();
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(t->ctx_, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(t->ctx_, TLS1_3_VERSION);
    const unsigned char* p = id.certificateDER().data();
    X509* x = d2i_X509(nullptr, &p, static_cast<long>(id.certificateDER().size()));
    const bool ok = x && SSL_CTX_use_certificate(t->ctx_, x) == 1 && SSL_CTX_use_PrivateKey(t->ctx_, id.key()) == 1 &&
                    SSL_CTX_check_private_key(t->ctx_) == 1;
    X509_free(x);
    if (!ok) {
        if (err) *err = "TLS identity: " + sslErrors();
        return nullptr;
    }
    SSL_CTX_set_verify(t->ctx_, server ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT) : SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_cert_verify_callback(t->ctx_, verifyCallback, t.get());
    SSL_CTX_set_mode(t->ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (server) {
        SSL_CTX_set_num_tickets(t->ctx_, 0);
        SSL_CTX_set_session_cache_mode(t->ctx_, SSL_SESS_CACHE_OFF);
    }
    return t;
}

TlsContext::~TlsContext() {
    if (ctx_) SSL_CTX_free(ctx_);
}

// ---- TlsConnection ------------------------------------------------------------------------------

namespace {
std::atomic<int> g_fail_thread_starts{0};
std::atomic<int> g_fail_tls_setup{0};
std::atomic<int> g_fail_peer_cert_read{0};

// A thread started, or false when Windows has none to give (std::system_error) or memory is short.
template <typename F>
bool startThread(std::thread& t, F&& f) {
    if (g_fail_thread_starts.load() > 0 && g_fail_thread_starts.fetch_sub(1) > 0) return false;
    try {
        t = std::thread(std::forward<F>(f));
        return true;
    } catch (const std::system_error&) {
        return false;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
} // namespace

void TlsConnection::failThreadStartsForTest(int n) { g_fail_thread_starts = n; }
void TlsConnection::failTlsSetupForTest(int n) { g_fail_tls_setup = n; }
void TlsConnection::failPeerCertReadForTest(int n) { g_fail_peer_cert_read = n; }

void TlsConnection::ioLoopGuarded(int handshake_timeout_ms) {
    std::shared_ptr<TlsConnection> keep_alive = shared_from_this();
    std::string why;
    try {
        ioLoop(handshake_timeout_ms);
        return;
    } catch (const std::exception& e) {
        why = std::string("closed after an error in the host (") + typeid(e).name() + ")";
    } catch (...) {
        why = "closed after an error in the host";
    }
    // As ioLoop's own end would: told once, then everything freed. The callback may throw too; it is not retried.
    try {
        finish(why);
    } catch (...) {
    }
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (sock_ != INVALID_SOCKET) {
        if (sock_event_ != WSA_INVALID_EVENT) WSAEventSelect(sock_, nullptr, 0);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (sock_event_ != WSA_INVALID_EVENT) {
        WSACloseEvent(sock_event_);
        sock_event_ = WSA_INVALID_EVENT;
    }
    video_in_flight_ = false;
}

std::shared_ptr<TlsConnection> TlsConnection::start(SOCKET s, std::shared_ptr<TlsContext> ctx, bool is_server, Callbacks cb,
                                                    int handshake_timeout_ms) {
    // Never throws: whatever fails before the thread runs (an allocation, the event, the thread
    // itself) closes the socket and returns nullptr. Until then the socket is this function's; after, the thread's.
    try {
        auto c = std::shared_ptr<TlsConnection>(new TlsConnection());
        c->ctx_ = std::move(ctx);
        c->is_server_ = is_server;
        c->cb_ = std::move(cb);
        c->wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!c->wake_event_) throw std::bad_alloc();
        c->sock_ = s;
        std::shared_ptr<TlsConnection> self = c;
        if (!startThread(c->io_, [self, handshake_timeout_ms]() { self->ioLoopGuarded(handshake_timeout_ms); })) {
            c->sock_ = INVALID_SOCKET;
            closesocket(s);
            return nullptr;
        }
        return c;
    } catch (...) {
        closesocket(s);
        return nullptr;
    }
}

TlsConnection::~TlsConnection() {
    stop_ = true;
    if (wake_event_) SetEvent(wake_event_);
    if (io_.joinable()) {
        if (io_.get_id() == std::this_thread::get_id()) io_.detach();
        else io_.join();
    }
    if (wake_event_) CloseHandle(wake_event_);
}

void TlsConnection::send(std::vector<uint8_t> bytes, bool is_video, uint8_t drop_class, uint64_t drop_tag) {
    if (closed_ || stop_) return;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (queued_bytes_.load() + bytes.size() > cb_.max_queued_bytes) {
            overflow = true; // v3 §2.2: a session that can't drain 16 MiB is dead
        } else {
            queued_bytes_ += bytes.size();
            Out o;
            o.bytes = std::move(bytes);
            o.video = is_video;
            o.drop_class = drop_class;
            o.drop_tag = drop_tag;
            queue_.push_back(std::move(o));
            if (is_video) video_in_flight_ = true;
        }
    }
    if (overflow) {
        overflowed_ = true;
        close();
        return;
    }
    SetEvent(wake_event_);
}

void TlsConnection::closeAfterFlush() {
    {
        std::lock_guard<std::mutex> lk(m_);
        close_after_flush_ = true;
    }
    SetEvent(wake_event_);
}

void TlsConnection::dropQueuedVideo() {
    std::lock_guard<std::mutex> lk(m_);
    bool more = false;
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->video && !it->started) {
            queued_bytes_ -= it->bytes.size() - it->off;
            it = queue_.erase(it);
        } else {
            more = more || it->video;
            ++it;
        }
    }
    video_in_flight_ = more;
}

std::vector<uint64_t> TlsConnection::dropQueued(uint8_t drop_class) {
    std::vector<uint64_t> tags;
    if (drop_class == 0) return tags;
    std::lock_guard<std::mutex> lk(m_);
    bool more = false;
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->drop_class == drop_class && !it->started) {
            queued_bytes_ -= it->bytes.size() - it->off;
            tags.push_back(it->drop_tag);
            it = queue_.erase(it);
        } else {
            more = more || it->video;
            ++it;
        }
    }
    video_in_flight_ = more;
    return tags;
}

void TlsConnection::close() {
    stop_ = true;
    if (wake_event_) SetEvent(wake_event_);
}

void TlsConnection::join() {
    if (io_.joinable() && io_.get_id() != std::this_thread::get_id()) io_.join();
}

Bytes TlsConnection::peerCertDer() const {
    std::lock_guard<std::mutex> lk(m_);
    return peer_der_;
}

bool TlsConnection::flushLocked(std::string* why) {
    while (!queue_.empty()) {
        Out& o = queue_.front();
        const size_t left = o.bytes.size() - o.off;
        if (left == 0) {
            queue_.pop_front();
            continue;
        }
        o.started = true;
        const int n = SSL_write(ssl_, o.bytes.data() + o.off, static_cast<int>(left));
        if (n > 0) {
            o.off += static_cast<size_t>(n);
            sent_bytes_ += static_cast<uint64_t>(n);
            queued_bytes_ -= static_cast<size_t>(n);
            if (o.off == o.bytes.size()) {
                const bool was_video = o.video;
                queue_.pop_front();
                if (was_video) {
                    ++sent_video_;
                    bool more = false;
                    for (const auto& q : queue_) more = more || q.video;
                    video_in_flight_ = more;
                }
            }
            continue;
        }
        const int e = SSL_get_error(ssl_, n);
        if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) return true;
        if (why) *why = "write failed: " + sslErrors();
        return false;
    }
    return true;
}

void TlsConnection::finish(const std::string& why) {
    if (closed_.exchange(true)) return;
    if (cb_.on_closed) cb_.on_closed(*this, why);
}

void TlsConnection::ioLoop(int handshake_timeout_ms) {
    std::shared_ptr<TlsConnection> keep_alive = shared_from_this();
    if (!adopted_) {
        ssl_ = g_fail_tls_setup.load() > 0 && g_fail_tls_setup.fetch_sub(1) > 0 ? nullptr : SSL_new(ctx_->get());
        sock_event_ = WSACreateEvent();
        // Both return null on a PC short of memory (they don't throw). This connection is turned away;
        // used unchecked they would end the host.
        if (!ssl_ || sock_event_ == WSA_INVALID_EVENT) throw std::bad_alloc(); // ioLoopGuarded closes it and frees both
        WSAEventSelect(sock_, sock_event_, FD_READ | FD_WRITE | FD_CLOSE); // also makes the socket non-blocking
        SSL_set_fd(ssl_, static_cast<int>(sock_));
        if (is_server_) SSL_set_accept_state(ssl_);
        else SSL_set_connect_state(ssl_);
    }
    HANDLE handles[2] = {sock_event_, wake_event_};

    // Handshake (already done for a connection a ServerHandshake handed over).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(handshake_timeout_ms);
    bool hs_ok = adopted_;
    std::string why;
    while (!stop_ && !adopted_) {
        ERR_clear_error();
        const int r = SSL_do_handshake(ssl_);
        if (r == 1) {
            hs_ok = true;
            break;
        }
        const int e = SSL_get_error(ssl_, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            const std::string detail = sslErrors();
            why = "TLS handshake failed" + (detail.empty() ? std::string() : (": " + detail));
            cert_refused_ = SSL_get_verify_result(ssl_) == X509_V_ERR_CERT_REJECTED;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            why = "TLS handshake timed out";
            break;
        }
        const DWORD wait_ms = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        WaitForMultipleObjects(2, handles, FALSE, std::min<DWORD>(wait_ms, 200));
        WSANETWORKEVENTS ne{};
        WSAEnumNetworkEvents(sock_, sock_event_, &ne);
        if ((ne.lNetworkEvents & FD_CLOSE) && !(ne.lNetworkEvents & FD_READ)) {
            // keep looping once more: SSL_do_handshake will see EOF and fail with a reason
        }
    }
    if (stop_ && !hs_ok && why.empty()) why = "closed";
    bool linger = false; // closed after a flush: the peer gets to read it all before the socket goes

    if (hs_ok) {
        if (X509* pc = adopted_ ? nullptr : SSL_get0_peer_certificate(ssl_)) {
            const bool fail = g_fail_peer_cert_read.load() > 0 && g_fail_peer_cert_read.fetch_sub(1) > 0;
            std::lock_guard<std::mutex> lk(m_);
            peer_der_ = fail ? Bytes() : x509Der(pc);
        }
        ready_ = true;
        if (cb_.on_ready) cb_.on_ready(*this);

        FrameReader reader(kHostProtocolVersion, cb_.skip_unknown_types, cb_.max_inbound_payload);
        std::vector<uint8_t> buf(64 * 1024);
        bool peer_closed = false;
        while (!stop_ && !closed_) {
            // Read everything available.
            bool read_failed = false;
            while (!closed_) {
                ERR_clear_error();
                const int n = SSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
                if (n > 0) {
                    if (cb_.on_activity) cb_.on_activity(*this); // every byte counts (the dead-phone watchdog)
                    reader.append(buf.data(), static_cast<size_t>(n));
                    while (true) {
                        MsgType t;
                        uint8_t flags = 0;
                        std::vector<uint8_t> payload;
                        std::string pv;
                        const auto res = reader.next(t, flags, payload, &pv);
                        if (res == FrameReader::Result::NeedMore) break;
                        if (res == FrameReader::Result::Violation) {
                            why = "protocol violation: " + pv;
                            read_failed = true;
                            break;
                        }
                        if (res == FrameReader::Result::Unknown) continue;
                        if (cb_.on_frame) cb_.on_frame(*this, t, flags, payload);
                    }
                    if (read_failed) break;
                    continue;
                }
                const int e = SSL_get_error(ssl_, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
                if (e == SSL_ERROR_ZERO_RETURN) why = "peer closed";
                else if (e == SSL_ERROR_SYSCALL) why = "peer closed (connection reset)";
                else why = "read failed: " + sslErrors();
                read_failed = true;
                break;
            }
            if (read_failed || closed_) break;

            bool flush_close = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (!flushLocked(&why)) {
                    read_failed = true;
                } else if (close_after_flush_ && queue_.empty()) {
                    flush_close = true;
                }
            }
            if (read_failed) break;
            if (flush_close) {
                SSL_shutdown(ssl_);
                // Then the FIN, after everything sent. The socket itself is closed only once the peer has closed
                // its side (below). Closed earlier, anything the peer still sends (a ping, while it reads what is
                // on its way) meets a closed socket and draws a reset, as does a byte left unread at closesocket;
                // and the reset throws away what the peer hasn't read yet, a 0x0F included.
                shutdown(sock_, SD_SEND);
                linger = true;
                why = "closed after flush";
                break;
            }
            if (peer_closed) {
                why = "peer closed";
                break;
            }
            WaitForMultipleObjects(2, handles, FALSE, 200);
            WSANETWORKEVENTS ne{};
            WSAEnumNetworkEvents(sock_, sock_event_, &ne);
            if (ne.lNetworkEvents & FD_CLOSE) peer_closed = true; // drain reads once more, then stop
        }
        if (why.empty()) why = "closed";
    }
    if (overflowed_) why = "send queue over " + std::to_string(cb_.max_queued_bytes) + " bytes (dead)";

    finish(why);
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (linger) {
        // Until the peer's FIN, a reset, close(), kCloseLingerMs or kCloseLingerMaxBytes (drainBeforeClose).
        drainBeforeClose(sock_, sock_event_, wake_event_, [this] { return static_cast<bool>(stop_); });
    }
    WSAEventSelect(sock_, nullptr, 0);
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
    WSACloseEvent(sock_event_);
    sock_event_ = WSA_INVALID_EVENT;
    video_in_flight_ = false;
}

// ---- ServerHandshake ------------------------------------------------------------------------------

ServerHandshake::ServerHandshake(SOCKET s, std::shared_ptr<TlsContext> ctx) : sock_(s), ctx_(std::move(ctx)) {
    wake_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ssl_ = SSL_new(ctx_->get());
    sock_event_ = WSACreateEvent();
    WSAEventSelect(sock_, sock_event_, FD_READ | FD_WRITE | FD_CLOSE); // also makes the socket non-blocking
    if (ssl_) {
        SSL_set_fd(ssl_, static_cast<int>(sock_));
        SSL_set_accept_state(ssl_);
    }
}

ServerHandshake::~ServerHandshake() {
    if (ssl_) SSL_free(ssl_);
    if (sock_ != INVALID_SOCKET) {
        WSAEventSelect(sock_, nullptr, 0);
        closesocket(sock_);
    }
    if (sock_event_ != WSA_INVALID_EVENT) WSACloseEvent(sock_event_);
    if (wake_event_) CloseHandle(wake_event_);
}

void ServerHandshake::abort() {
    if (committed_) return;
    aborted_ = true;
    if (wake_event_) SetEvent(wake_event_);
}

void ServerHandshake::commit() {
    committed_ = true;
    aborted_ = false;
    if (wake_event_) ResetEvent(wake_event_);
}

bool ServerHandshake::waitIo(Clock::time_point deadline) {
    if (aborted_) return false;
    const auto now = Clock::now();
    const long long left =
        deadline > now ? static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()) : 0;
    HANDLE handles[2] = {sock_event_, wake_event_};
    WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(std::min<long long>(left, 200)));
    WSANETWORKEVENTS ne{};
    WSAEnumNetworkEvents(sock_, sock_event_, &ne);
    return !aborted_;
}

ServerHandshake::Result ServerHandshake::run(Clock::time_point deadline, std::string* why, Clock::time_point firstRecordBy) {
    // Settled on every way out but Aborted, by compare-and-swap: certified on Ok, done on any
    // other end. Either way it can no longer be evicted, so a refused certificate is always counted by its finish().
    // If the listener evicted it first, it lost its slot: it ends as Aborted, counting nothing.
    const Result r = runSteps(deadline, why, firstRecordBy);
    if (r == Result::Aborted) return r;
    int open = 0;
    if (!state_.compare_exchange_strong(open, r == Result::Ok ? 1 : 3)) return Result::Aborted;
    return r;
}

ServerHandshake::Result ServerHandshake::runSteps(Clock::time_point deadline, std::string* why, Clock::time_point firstRecordBy) {
    if (!ssl_ || sock_event_ == WSA_INVALID_EVENT || !wake_event_) {
        if (why) *why = "TLS setup failed";
        return Result::Failed;
    }
    while (true) {
        if (aborted_) return Result::Aborted;
        ERR_clear_error();
        const int r = SSL_do_handshake(ssl_);
        if (r == 1) break;
        const int e = SSL_get_error(ssl_, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            // The verify callback's refusal is recorded as the verify result (X509_V_ERR_CERT_REJECTED); it is
            // X509_V_OK for every failure that came before a certificate was checked.
            const bool rejected = SSL_get_verify_result(ssl_) == X509_V_ERR_CERT_REJECTED;
            const std::string detail = sslErrors();
            if (why) {
                *why = std::string(rejected ? "certificate refused" : "TLS handshake failed") +
                       (detail.empty() ? std::string() : (": " + detail));
            }
            return rejected ? Result::Rejected : Result::Failed;
        }
        if (Clock::now() >= deadline) {
            if (why) *why = "TLS handshake timed out";
            return Result::TimedOut;
        }
        // Still reading the ClientHello (TLS_ST_BEFORE, or TLS_ST_SR_CLNT_HELLO once its header is in) past its own,
        // shorter deadline. Only then does that deadline count, for the drop and for the wait: once the ClientHello
        // is read, waiting on it would wake at once and spin a core until the handshake deadline.
        const OSSL_HANDSHAKE_STATE st = SSL_get_state(ssl_);
        const bool hello_pending = st == TLS_ST_BEFORE || st == TLS_ST_SR_CLNT_HELLO;
        if (hello_pending && Clock::now() >= firstRecordBy) {
            if (why) *why = "no ClientHello in time";
            return Result::TimedOut;
        }
        if (!waitIo(hello_pending ? std::min(deadline, firstRecordBy) : deadline)) return Result::Aborted;
    }
    if (X509* pc = SSL_get0_peer_certificate(ssl_)) peer_der_ = x509Der(pc);
    if (peer_der_.empty()) {
        if (why) *why = "no client certificate";
        return Result::Failed;
    }
    return Result::Ok; // certified by run()
}

ServerHandshake::Read ServerHandshake::readOneFrame(Clock::time_point deadline, uint32_t max_payload, MsgType& type,
                                                    std::vector<uint8_t>& payload, std::string* why) {
    FrameReader reader(kHostProtocolVersion, false, max_payload);
    std::vector<uint8_t> buf(16 * 1024);
    while (true) {
        if (aborted_) return Read::Aborted;
        uint8_t flags = 0;
        std::string pv;
        const auto res = reader.next(type, flags, payload, &pv);
        if (res == FrameReader::Result::Frame) return Read::Frame;
        if (res != FrameReader::Result::NeedMore) {
            if (why) *why = "not one frame: " + pv;
            return Read::Invalid;
        }
        ERR_clear_error();
        const int n = SSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
        if (n > 0) {
            reader.append(buf.data(), static_cast<size_t>(n));
            continue;
        }
        const int e = SSL_get_error(ssl_, n);
        if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) {
            if (why) *why = "peer closed";
            return Read::Closed;
        }
        // OpenSSL 3 reports a TCP close with no close_notify (a link that just
        // drops, an app killed) as SSL_ERROR_SSL, "unexpected eof while reading". That is the peer going away, not a
        // malformed redemption, so it is Closed (v3 §8.5: never a failure). Mapped here, for this one read, rather
        // than SSL_OP_IGNORE_UNEXPECTED_EOF on the outside context: the option would also change how the paired
        // sessions handed off from that context read a close, and the frames are length-prefixed, so a cut-off frame
        // is still caught here as an incomplete frame (it never reaches a parser).
        if (e == SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
            ERR_clear_error();
            if (why) *why = "peer closed (no close_notify)";
            return Read::Closed;
        }
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            if (why) *why = "read failed: " + sslErrors();
            return Read::Invalid;
        }
        if (Clock::now() >= deadline) {
            if (why) *why = "no frame in time";
            return Read::TimedOut;
        }
        if (!waitIo(deadline)) return Read::Aborted;
    }
}

bool ServerHandshake::sendAndClose(const std::vector<uint8_t>& bytes, Clock::time_point deadline) {
    size_t off = 0;
    while (off < bytes.size()) {
        if (aborted_) return false;
        ERR_clear_error();
        const int n = SSL_write(ssl_, bytes.data() + off, static_cast<int>(bytes.size() - off));
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        const int e = SSL_get_error(ssl_, n);
        if ((e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_WANT_READ) || Clock::now() >= deadline) return false;
        if (!waitIo(deadline)) return false;
    }
    // The same lingering close as TlsConnection::closeAfterFlush, not a bare one. close_notify, then the FIN after everything
    // sent, then read and drop until the peer closes (drainBeforeClose). Closed at once, a byte the device sent after
    // its pairRequest (a ping while it waits) is left unread, so closesocket sends a reset, and the reset throws away
    // the reply the device hasn't read yet: a committed accepted: true included. An abort still ends the wait, except
    // once committed (abort() is ignored then), when it is bounded by kCloseLingerMs.
    SSL_shutdown(ssl_);
    shutdown(sock_, SD_SEND);
    drainBeforeClose(sock_, sock_event_, wake_event_, [this] { return aborted_.load(); });
    return true;
}

std::shared_ptr<TlsConnection> ServerHandshake::handOff(std::shared_ptr<TlsContext> ctx, TlsConnection::Callbacks cb) {
    auto c = std::shared_ptr<TlsConnection>(new TlsConnection());
    c->sock_ = sock_;
    c->ssl_ = ssl_;
    c->sock_event_ = sock_event_;
    c->ctx_ = std::move(ctx);
    c->is_server_ = true;
    c->adopted_ = true;
    c->peer_der_ = peer_der_;
    c->cb_ = std::move(cb);
    c->wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sock_ = INVALID_SOCKET;
    ssl_ = nullptr;
    sock_event_ = WSA_INVALID_EVENT;
    std::shared_ptr<TlsConnection> self = c;
    // No wake event (out of handles) or no thread: the connection is closed here, as start() does for both.
    if (!c->wake_event_ || !startThread(c->io_, [self]() { self->ioLoopGuarded(0); })) {
        if (c->ssl_) SSL_free(c->ssl_);
        c->ssl_ = nullptr;
        WSAEventSelect(c->sock_, nullptr, 0);
        closesocket(c->sock_);
        c->sock_ = INVALID_SOCKET;
        WSACloseEvent(c->sock_event_);
        c->sock_event_ = WSA_INVALID_EVENT;
        return nullptr;
    }
    return c;
}

// ---- HandshakePool --------------------------------------------------------------------------------

HandshakePool::HandshakePool(int workers) {
    for (int i = 0; i < std::max(1, workers); ++i) {
        threads_.emplace_back([this] {
            while (true) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
                    if (stop_) return;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                job();
            }
        });
    }
}

HandshakePool::~HandshakePool() { stop(); }

bool HandshakePool::post(std::function<void()> job, bool first) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (stop_) return false;
        if (first) jobs_.push_front(std::move(job));
        else jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void HandshakePool::stop() {
    std::deque<std::function<void()>> dropped;
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        dropped.swap(jobs_);
    }
    cv_.notify_all();
    dropped.clear(); // outside the lock: what they hold (sockets, admission tickets) is released here
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

// ---- TcpListener --------------------------------------------------------------------------------

namespace {

// A socket no child process inherits (as socket() makes, overlapped, but never inheritable): a program this one
// starts, such as ffmpeg for a recording, gets none of the host's connections or listeners.
SOCKET privateSocket(int af, int type, int protocol) {
    return WSASocketW(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

// The listener's socket, bound and not yet listening: 127.0.0.1 when loopback_only, otherwise [::] dual-stack
// (IPV6_V6ONLY off). INVALID_SOCKET with err when it can't be bound.
SOCKET bindListenerSocket(uint16_t port, bool loopback_only, std::string* err) {
    SOCKET s = INVALID_SOCKET;
    if (loopback_only) {
        s = privateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            if (err) *err = "bind 127.0.0.1 failed (" + std::to_string(WSAGetLastError()) + ")";
            if (s != INVALID_SOCKET) closesocket(s);
            return INVALID_SOCKET;
        }
    } else {
        s = privateSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        DWORD off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        a.sin6_addr = in6addr_any;
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            if (err) *err = "bind [::] failed (" + std::to_string(WSAGetLastError()) + ")";
            if (s != INVALID_SOCKET) closesocket(s);
            return INVALID_SOCKET;
        }
    }
    return s;
}

} // namespace

bool tcpListenerPortBinds(uint16_t port, bool loopback_only) {
    if (!winsockInit()) return false;
    const SOCKET s = bindListenerSocket(port, loopback_only, nullptr);
    if (s == INVALID_SOCKET) return false;
    closesocket(s); // bound only, never listening: no firewall prompt, and nothing to accept
    return true;
}

bool TcpListener::start(uint16_t port, bool loopback_only, std::function<void(SOCKET, const sockaddr*)> on_accept,
                        std::string* err) {
    stop();
    if (!winsockInit()) {
        if (err) *err = "WSAStartup failed";
        return false;
    }
    stop_ = false;
    sock_ = bindListenerSocket(port, loopback_only, err);
    if (sock_ == INVALID_SOCKET) {
        stop();
        return false;
    }
    if (listen(sock_, SOMAXCONN) != 0) {
        if (err) *err = "listen failed";
        stop();
        return false;
    }
    sockaddr_storage ss{};
    int len = sizeof(ss);
    getsockname(sock_, reinterpret_cast<sockaddr*>(&ss), &len);
    port_ = ntohs(ss.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port
                                           : reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    {
        char text[INET6_ADDRSTRLEN] = {0};
        const void* a = ss.ss_family == AF_INET6 ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr)
                                                 : static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(&ss)->sin_addr);
        bound_ = inet_ntop(ss.ss_family, a, text, sizeof(text)) ? text : "";
    }
    th_ = std::thread([this, on_accept]() {
        while (!stop_) {
            // v3 §8.0: the peer address is read here, at accept, for the listener to judge before any TLS.
            sockaddr_storage peer{};
            int peer_len = sizeof(peer);
            SOCKET c = accept(sock_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (c == INVALID_SOCKET) {
                if (stop_) break;
                Sleep(10);
                continue;
            }
            SetHandleInformation(reinterpret_cast<HANDLE>(c), HANDLE_FLAG_INHERIT, 0); // never a child's, as the listener
            tuneSocket(c);
            // Nothing the handler throws ends the accept thread, or the host with it. The handler owns
            // the socket from the call; a PC short of memory gets a moment before the next accept.
            try {
                on_accept(c, reinterpret_cast<const sockaddr*>(&peer));
            } catch (...) {
                Sleep(50);
            }
        }
    });
    return true;
}

void TcpListener::requestStop() {
    stop_ = true;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

void TcpListener::stop() {
    stop_ = true;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (th_.joinable()) th_.join();
    port_ = 0;
    bound_.clear();
}

SOCKET tcpConnect(const std::string& host, uint16_t port, int timeout_ms, std::string* err) {
    if (!winsockInit()) return INVALID_SOCKET;
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        if (err) *err = "getaddrinfo failed";
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai && s == INVALID_SOCKET; ai = ai->ai_next) {
        SOCKET t = privateSocket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (t == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(t, FIONBIO, &nb);
        connect(t, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(t, &wf);
        FD_SET(t, &ef);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(0, nullptr, &wf, &ef, &tv) == 1 && FD_ISSET(t, &wf)) {
            nb = 0;
            ioctlsocket(t, FIONBIO, &nb);
            tuneSocket(t);
            s = t;
        } else {
            closesocket(t);
        }
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET && err) *err = "connect to " + host + ":" + std::to_string(port) + " failed";
    return s;
}

} // namespace kvhost
