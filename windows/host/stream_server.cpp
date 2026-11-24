#include "stream_server.hpp"

#include "outside_settings.hpp"
#include "pairing_payload.hpp"
#include "roll_wire.hpp"
#include "state_files.hpp"

#include <algorithm>
#include <map>
#include <type_traits>
#include <utility>

namespace kvhost {

namespace {

// A 0x0D for a request: ok, or the code with its user-facing sentence, v3 §9 (defined with the roll's helpers below).
void sendCommandResult(const std::shared_ptr<TlsConnection>& conn, uint64_t requestId, const char* error);

uint64_t monotonicNs() {
    // Same clock as kv_monotonic_ns() on Windows (std::chrono::steady_clock / QPC).
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t unixMsNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kNotifiedCloseGraceNs = 2000ULL * kMs; // a 0x0F gets this long to flush
constexpr uint64_t kTouchFlushNs = 10ULL * 60ULL * 1000ULL * kMs; // pet-camera.md §5: at most every 10 min
constexpr uint64_t kAutoDropWindowNs = 5000ULL * kMs; // v3 §6.1: 3 drops within 5 s step auto down
constexpr int kAutoDropsToStepDown = 3;
constexpr uint64_t kAutoStepUpNs = 30000ULL * kMs; // ...and 30 s without a drop step it back up
constexpr const wchar_t* kPrivacyMarkerFile = L"privacy-on.pending";
constexpr uint64_t kFirstRetryMs = 100;  // a failed state-file read or write is tried again after this,
constexpr uint64_t kMaxRetryMs = 5000;   // doubling up to this, and back to the first on success
constexpr uint64_t kLogPruneEveryNs = 24ULL * 3600ULL * 1000ULL * kMs; // v3 §8.6: pruned at start-up and daily

// v3 §8.7: why the outside verify callback admitted the leaf of the
// handshake running on this thread. It runs inside SSL_do_handshake, on the pool thread that runs that handshake
// (runOutsideHandshake), so this tells that one handshake, and no other, the serial of the invite that was open
// when its certificate was taken (Invite::serial), or 0 when it was taken as a listed device's.
thread_local uint64_t t_outside_admitted_invite = 0;

// The handshake running on this thread is from a source the limiter has blocked.
// The verify callback then takes only a certificate the device list holds at that moment (proved by the TLS
// handshake, never a fingerprint the client states): no invite is ever looked at, and anything else is refused
// inside the verify step, before any application byte.
thread_local bool t_outside_blocked = false;

LiveTier tierForQuality(LiveQuality q) {
    switch (q) {
        case LiveQuality::Medium: return LiveTier::Medium;
        case LiveQuality::Low: return LiveTier::Low;
        default: return LiveTier::High;
    }
}

} // namespace

struct HostStreamServer::Session {
    std::shared_ptr<TlsConnection> conn;
    uint64_t id = 0; // arrival order
    Bytes hash;      // the TLS leaf's SHA-256: the device it speaks for
    PetPath path = PetPath::Home;
    std::string addr;         // the source address, unwrapped (v3 §8.0), for the connection log
    uint64_t started_ms = 0;  // unix ms, for the log's session length
    int version = 1; // v3 §1 rule 1: raised once by the first ping that says 2 or more
    bool pinged = false;
    // This connection's device had other sessions when it became one. They are
    // replaced only at this one's first ping (its hello), so a connection that closes without one (the loser of
    // a phone's racing dials, a TLS-only probe) never ends the live session.
    bool replaces_on_hello = false;
    uint64_t last_activity_ns = 0;
    // Live video (v3 §2.3, §5.4, §6).
    bool wants_live = true; // 0x0E watch; v1/v2 always want live
    LiveQuality quality = LiveQuality::Auto;
    LiveTier auto_tier = LiveTier::High; // where `auto` is now (home starts at high)
    bool watching = false;               // wants its tier and receives its AUs
    LiveTier tier = LiveTier::High;      // the tier it wants while watching
    bool holds_slot = false;
    uint64_t slot_seq = 0; // when it took its slot (pre-emption takes the newest viewer slot)
    bool needs_config = true; // the next AU it gets must be an IDR, after its 0x02
    // The epoch it was shown (its last 0x02's, and every 0x03 it gets carries it) and the tier epoch that
    // stands for. They differ only when the session joined a tier whose epoch was not above one it had
    // already seen (v3 §6.2: higher than any it saw): it then got a fresh number of its own from the
    // host-wide counter, so the tier never starts a new epoch for everyone (v1/v2 on high keep theirs).
    uint32_t last_epoch = 0;
    uint32_t src_epoch = 0;
    bool has_last_tier = false;
    LiveTier last_tier = LiveTier::High;
    bool wait_idr = false; // dropped a frame: nothing until its tier's next IDR
    std::vector<uint64_t> drops_ns;
    uint64_t last_drop_ns = 0;
    uint64_t last_step_ns = 0;
    // Presentation.
    bool available = true;
    std::string reason; // live_unavailable::* when not available
    bool presented = false; // a 0x07 went out (v2+)
    bool presented_available = true;
    std::string presented_reason;
    std::string last_0b; // the last 0x0B without its stateSeq
    // The outside endpoint this device was last told in a 0x0B (host:port, or empty for none), so the connection
    // log says each time that changes: whether a phone was ever told where to dial from outside.
    std::string endpoint_told;
    bool endpoint_logged = false;
    bool force_0b = false;
    bool resend_hello = false; // role change: hello, 0x07 and 0x0B again
    // Camera roll playback (v3 §5.9): at most one stream per session; it holds the session's slot while allocated.
    std::shared_ptr<RollPlayback> playback;
    uint32_t last_stream_id = 0; // stream ids are unique per session, from 1
};

namespace {

// v3 §6.2: a device's 0x08 commands (request_keyframe from any role, an owner's controls) are held to a bucket of
// 8 that refills at 4 a second, kept per device (its certificate's hash), so a reconnect doesn't bring a fresh burst.
// Each one costs a post to the UI thread and may send every session a state message, so a device flooding them is
// held to that, and a person tapping never meets it.
constexpr double kCommandBurst = 8.0;
constexpr double kCommandsPerSecond = 4.0;

} // namespace

struct HostStreamServer::Pending {
    std::shared_ptr<TlsConnection> conn;
    uint64_t deadline_ns = 0;
    bool notice_only = false; // v3 §2.2: hello sent; waits for a v3 ping to send 0x0F too_many_sessions
    Bytes hash;
    PetPath path = PetPath::Home;
};

HostStreamServer::HostStreamServer() = default;

HostStreamServer::~HostStreamServer() {
    shutdown();
}

bool HostStreamServer::start(const Options& opts, std::string* err, bool* identity_unusable) {
    std::lock_guard<std::recursive_mutex> lk(m_);
    if (identity_unusable) *identity_unusable = false;
    if (started_) return true;
    opts_ = opts;
    if (opts_.state_dir.empty()) opts_.state_dir = defaultHostStateDir();
    if (opts_.loopback_only) opts_.advertise = false;
    if (!winsockInit()) {
        if (err) *err = "WSAStartup failed";
        return false;
    }
    if (!ensureOwnerOnlyDirectory(opts_.state_dir, err)) return false;
    // The marker that says this folder is the host's (a --state-dir check reads it). Not fatal when it
    // can't be written: the folder is then still known by its pairing.json.
    if (!fileExists(opts_.state_dir + L"\\" + kHostStateMarkerFile)) {
        writeOwnerOnlyFileAtomically(opts_.state_dir, kHostStateMarkerFile, "kinect-viewer host state\n");
    }

    // pairing.json is this PC's identity (its key, certificate and serviceId), which every paired device pins.
    // Only a missing file is a first run. One that is there but can't be read or parsed fails closed: it is
    // kept as pairing.json.bad, nothing is written, renamed or deleted, and
    // nothing serves (no listener, no advertisement, no pairing window). A new identity comes only from the
    // owner's explicit resetHostIdentity, never from here.
    identity_.reset();
    // What an interrupted write left goes first: a pairing.json.prev with no pairing.json is put back, so it is
    // never read as a first run (pet-camera.md §5).
    const std::string leftover_note = recoverPairingLeftovers(opts_.state_dir);
    HostPersistedState loaded;
    std::string unusable_why;
    std::string key_note; // what wrapping a plain key did, for the reconcile log
    bool passing = false;  // the wrapped key wouldn't unwrap for a reason that may pass: try again, don't reset
    const HostStateLoad load = loadHostStateChecked(opts_.state_dir, loaded, &unusable_why, &passing);
    if (load == HostStateLoad::Loaded) {
        std::string id_err;
        identity_ = CryptoIdentity::fromRecord(loaded.privateKeyRaw, loaded.certificateDER, &id_err);
        if (identity_) {
            state_ = loaded;
            // A pairing.json from before the wrap holds the key plain: wrapped now, the plain key removed only
            // once the wrapped one reads back. A failure keeps the plain key and the host starts with it as
            // before (reachability first); the next start tries again. One already wrapped is left as it is.
            migrateHostKeyToWrapped(opts_.state_dir, &key_note);
        } else {
            unusable_why = "pairing.json (this PC's identity) holds a key and certificate that can't be used" +
                           (id_err.empty() ? std::string() : " (" + id_err + ")");
        }
    }
    if (load != HostStateLoad::Missing && !identity_) {
        std::string kept;
        if (fileState(opts_.state_dir + L"\\pairing.json") == FileState::Missing) {
            kept = "pairing.json.prev is left as it is"; // the identity that couldn't be put back: nothing to copy
        } else {
            keepPairingJsonAsBad(opts_.state_dir, &kept);
        }
        if (err) {
            *err = unusable_why + "; " + kept +
                   (passing ? ". The streaming host is off and makes no new identity, so no paired device is lost: "
                              "try again in a moment."
                            : ". The streaming host is off and makes no new identity, so no paired device is lost: "
                              "try again once the file can be read, or reset this PC's identity (every device must "
                              "pair again).");
        }
        if (identity_unusable) *identity_unusable = true;
        return false;
    }
    if (!identity_) {
        // No pairing.json: a first run.
        identity_ = CryptoIdentity::generate(newHostCommonName(), err);
        if (!identity_) return false;
        state_ = HostPersistedState{};
        state_.privateKeyRaw = identity_->privateKeyRaw();
        state_.certificateDER = identity_->certificateDER();
        state_.serviceId = randomHexId(6);
        state_.allowPhoneModeSwitch = true;
        if (!saveHostStateAtomically(opts_.state_dir, state_, err)) return false;
    }
    // pet-camera.md §5: the device list, reconciled with pairing.json (migration, interrupted saves,
    // a downgraded build's changes). It also repairs both files when they disagree.
    store_ = DeviceStore(opts_.state_dir);
    const auto rec = store_.loadAndReconcile(state_, unixMsNow());
    reconcile_log_ = rec.log;
    if (!leftover_note.empty()) reconcile_log_.push_back(leftover_note);
    if (!key_note.empty()) reconcile_log_.push_back(key_note);
    // devices.json there but unreadable (another program holds it): not corruption. The list stays empty
    // (nobody is admitted), nothing is written over it, and the maintenance loop reads it again.
    devices_unread_ = rec.readFailed;
    devices_rebuilt_ = rec.which == DeviceStore::ReconcileCase::Migrated;
    devices_read_error_ = rec.readError;
    read_retry_ms_ = kFirstRetryMs;
    next_read_retry_ns_ = monotonicNs() + read_retry_ms_ * kMs;
    // A repair write that failed: the list in memory is right, the files catch up on a retry.
    devices_dirty_ = !rec.saveError.empty();
    devices_save_error_ = rec.saveError;
    settings_dirty_ = false;
    settings_save_error_.clear();
    // A pet-camera.json that exists but can't be read or parsed starts with privacy mode on (fail
    // closed); the PC's status line says why until the settings are saved again.
    // One that exists but can't be READ is not corruption either (as devices.json): it is never written
    // over, setting changes that would need it written are refused, and the maintenance loop reads it again.
    bool settings_existed = false;
    settings_ = loadPetSettings(opts_.state_dir, &settings_existed, &settings_load_error_, &settings_unread_,
                                opts_.address_policy_for_test);
    // The roll's limits are the owner's only when the file was read and held them (settings_.rollLimitsKnown);
    // any file that couldn't be used gave the defaults, which leave them unknown.
    if (settings_unread_ || !settings_load_error_.empty()) settings_.rollLimitsKnown = false;
    // v3 §8.3: the address devices were last told, kept across restarts: a different first address is checked.
    announced_wan_ = settings_.outsideAnnouncedAddress;
    privacy_on_asked_while_unread_ = false;
    privacy_marker_write_due_ = false;
    privacy_marker_delete_due_ = false;
    privacy_marker_error_.clear();
    // A privacy-on asked for while pet-camera.json couldn't be read, in an earlier run: it wins over the file.
    const FileState marker_state = fileState(opts_.state_dir + L"\\" + kPrivacyMarkerFile);
    privacy_marker_may_exist_ = marker_state != FileState::Missing;
    privacy_marker_on_disk_ = marker_state == FileState::Present;
    if (privacy_marker_may_exist_) applyPrivacyMarkerLocked();
    settings_read_retry_ms_ = kFirstRetryMs;
    next_settings_read_ns_ = monotonicNs() + settings_read_retry_ms_ * kMs;
    // The outside port (pet-camera.md §4, v3 §8.1 "picked once and saved") is picked once, the first of up to
    // 20 random ports in 20000-44999 that binds (§8.7), and written as soon as pet-camera.json can be written (the
    // start's own flush below), so every later start keeps it. Never over a file that couldn't be read; one that
    // couldn't be parsed (kept as .bad, privacy on) gets it with its next save, so its error stays on the status
    // line until then. None bound: nothing is saved, and it is picked again when outside viewing is turned on.
    if (!settings_unread_ && !outside::isValidOutsidePort(settings_.outsidePort)) {
        settings_.outsidePort = pickOutsidePortLocked();
        if (outside::isValidOutsidePort(settings_.outsidePort) && settings_load_error_.empty()) markSettingsDirtyLocked();
    }
    last_schedule_off_ = scheduleOffNowLocked();
    // The recorder records only once it knows the settings (privacy first: a pet-camera.json that couldn't
    // be read has privacy on, so nothing records).
    roll_store_ = roll_ ? roll_->store() : nullptr;
    pushRollSettingsLocked();
    star_reconcile_due_ = roll_prepared_; // the maintenance loop does it, once the device list is read

    // Stream listener: reject inside the TLS 1.3 handshake unless the leaf is a listed device
    // (compared in constant time against every listed hash).
    auto sctx = TlsContext::create(true, *identity_,
                                   [this](const Bytes& peer_hash, const Bytes&) {
                                       std::lock_guard<std::recursive_mutex> lk2(m_);
                                       const bool ok = store_.findByHash(peer_hash) != nullptr;
                                       if (!ok) ++rejected_handshakes_;
                                       return ok;
                                   },
                                   err);
    // Pairing listener: accept the self-signed client leaf; the proof is the transcript-bound HMAC.
    auto pctx = TlsContext::create(true, *identity_, [](const Bytes&, const Bytes&) { return true; }, err);
    // The outside listener (v3 §8.1): its own context and verify callback. A listed device's hash, or any
    // leaf while an invite is open (that connection can then only redeem it); anything else fails inside the
    // handshake, before any application byte. The LAN contexts above never admit an unknown certificate.
    auto octx = TlsContext::create(true, *identity_,
                                   [this](const Bytes& peer_hash, const Bytes&) {
                                       std::lock_guard<std::recursive_mutex> lk2(m_);
                                       t_outside_admitted_invite = 0;
                                       if (store_.findByHash(peer_hash) != nullptr) return true;
                                       if (t_outside_blocked) return false; // a blocked source: paired devices only
                                       if (invites_) {
                                           if (const auto inv = invites_->open(inviteNowMs())) {
                                               t_outside_admitted_invite = inv->serial; // admitted for this invite
                                               return true;
                                           }
                                       }
                                       ++rejected_handshakes_;
                                       return false;
                                   },
                                   err);
    if (!sctx || !pctx || !octx) return false;
    stream_ctx_ = std::move(sctx);
    pairing_ctx_ = std::move(pctx);
    outside_ctx_ = std::move(octx);
    // §8.5: limits live in memory and reset on restart.
    limiter_ = outside::OutsideLimiter::create(opts_.outside_limits, opts_.outside_limiter_clock);
    // §8.4: the open invite (invite.json), failing closed when it can't be read.
    invites_ = std::make_unique<outside::InviteStore>(opts_.state_dir);
    invite_error_.clear();
    invites_->load(&invite_error_);
    // An invite still open from the last run: its endpoint is taken from the first check that finds one, and it
    // closes if that check finds none (syncInviteLocked).
    invite_live_ = invites_->open(inviteNowMs()).has_value();
    invite_host_.clear();
    invite_port_ = 0;
    invite_end_.clear();
    // §8.6: the connection log, pruned at start-up (and daily, by the maintenance loop).
    if (!log_) log_ = std::make_unique<outside::ConnectionLog>(opts_.state_dir, opts_.connection_log_flush_ms);
    log_->setWriteHookForTest(opts_.connection_log_write_hook);
    log_->load(unixMsNow());
    last_log_prune_ns_ = monotonicNs();
    // §8.2: the router's own worker. Only the opener the options give is ever used: none means no router.
    {
        outside::OutsideMapper::Config mc;
        mc.opener = opts_.router_opener;
        mc.lanAddress = opts_.outside_lan_address;
        mc.recheckEveryMs = opts_.outside_recheck_ms;
        mc.timing = opts_.router_timing;
        mc.addressPolicy = opts_.address_policy_for_test;
        mc.readGateway = opts_.read_gateway;
        mc.askGatewayWan = opts_.ask_gateway_wan;
        mc.pinned = outside::GatewayIdentity{settings_.outsideGatewayIP, settings_.outsideGatewayMAC};
        mc.gatewayLocation = settings_.outsideGatewayLocation;
        mc.announcedWan = settings_.outsideAnnouncedCorrupt ? std::string(outside::OutsideMapper::kAnnouncedUnknown)
                                                            : settings_.outsideAnnouncedAddress;
        mc.gatewayRetryMs = opts_.gateway_retry_ms;
        mc.gatewaySlowAfterMs = opts_.gateway_slow_after_ms;
        mc.gatewaySlowEveryMs = opts_.gateway_slow_every_ms;
        mapper_ = std::make_unique<outside::OutsideMapper>(std::move(mc),
                                                           [this](const outside::MapperState& st) { onMapperChanged(st); });
        mapper_state_ = outside::MapperState{};
    }

    if (!stream_listener_.start(opts_.fixed_stream_port, opts_.loopback_only || opts_.lan_listeners_loopback_for_test,
                                [this](SOCKET s, const sockaddr* peer) { handleStreamSocket(s, peer); }, err)) {
        return false;
    }
    stream_port_ = stream_listener_.port();
    shutting_down_ = false;
    expiry_stop_ = false;
    maintenance_stop_ = false;
    last_flush_ns_ = monotonicNs();
    started_ = true;
    updateAdvertisementLocked();
    // §8.7: a forward an earlier run's exit couldn't remove (outsideMappedPort) goes now, best effort on the
    // mapper's worker, unless outside listening takes that very port again. Only this PC's own mapping with our
    // description is removed; the router is the real one only on a real launch (never a test run's).
    // A forward the PC user removed (outsideMappingRemoved) whose removal didn't finish before the exit goes too,
    // even on the port outside listening takes again: it is meant to be off the router.
    if (mapper_ && !settings_unread_ && settings_.outsideMappedPort != 0 &&
        !(outsideWantedLocked() && settings_.outsidePort == settings_.outsideMappedPort &&
          !settings_.outsideMappingRemoved)) {
        mapper_->removeLeftover(settings_.outsideMappedPort);
    }
    applyOutsideLocked(); // §8.2: at start-up, with outside listening on, the listener opens and the mapping is added again
    expiry_thread_ = std::thread([this] { expiryLoop(); });
    maintenance_thread_ = std::thread([this] { maintenanceLoop(); });
    return true;
}

void HostStreamServer::shutdown() {
    std::vector<std::shared_ptr<TlsConnection>> conns;
    std::vector<std::shared_ptr<TlsConnection>> notified;
    std::vector<std::unique_ptr<TcpListener>> retired;
    std::vector<std::unique_ptr<HandshakePool>> pools;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!started_) return;
        started_ = false;
        shutting_down_ = true;
        closePairingWindowLocked();
        stream_listener_.requestStop();
        // The outside listener and its handshakes stop; its sessions get host_shutting_down below, as every
        // v3 session does. The mapping is removed best effort by the mapper's worker (below, never waited on).
        if (outside_listener_) {
            outside_listener_->requestStop();
            retired_listeners_.push_back(std::move(outside_listener_));
        }
        outside_port_ = 0;
        keepAwakeLocked(false);
        if (power_request_) {
            CloseHandle(static_cast<HANDLE>(power_request_));
            power_request_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> g(hs_live_m_);
            ++outside_gen_;
            for (auto& h : hs_live_) h->abort();
        }
        if (hs_pool_) retired_pools_.push_back(std::move(hs_pool_));
        stream_ad_.stop();
        pairing_ad_.stop();
        // A clean exit (v3 §5.5): v3 sessions are told, then closed after the notice is flushed.
        for (auto& s : sessions_) {
            touchDeviceLocked(*s);
            endPlaybackLocked(*s, nullptr, true);
            if (s->watching && live_) live_->releaseTier(s->tier);
            s->watching = false;
            sent_frames_base_ += s->conn->sentVideoFrames();
            if (s->version >= 3) {
                s->conn->send(packFrame(MsgType::SessionNotice, 0, toJson(makeSessionNotice(notice_reason::kHostShuttingDown))));
                s->conn->closeAfterFlush();
                notified.push_back(s->conn);
            } else {
                s->conn->close();
            }
        }
        sessions_.clear();
        for (auto& p : pending_) p->conn->close();
        pending_.clear();
        paired_client_connected_ = false;
        conns = conns_;
        retired.swap(retired_listeners_);
        pools.swap(retired_pools_);
        expiry_stop_ = true;
        expiry_cv_.notify_all();
        maintenance_stop_ = true;
        maintenance_cv_.notify_all();
        pc_pin_cv_.notify_all(); // a delete waiting on the PC's player gives up
    }
    // pet-camera.md §5: last-seen times on every clean exit; this also sets before = written. A save that
    // failed earlier, or a list whose mirror changed since the last save (the device-list-only save
    // refuses that), gets the full save instead, as the maintenance flush does; pet-camera.json and
    // pairing.json too, if theirs failed. Written outside the lock, after any write in flight (io_m_). A
    // crash (the test mode) has no clean exit: nothing is written.
    flushSaves(FlushKind::Exit);
    joinRetiredPlaybacks();
    // §8.2: a clean exit removes the mapping, best effort: the request is posted to the mapper's worker, which
    // is never waited on (a router that doesn't answer can't hold up the exit). Outside m_: a callback from the
    // worker may be waiting for it.
    if (mapper_) mapper_->shutdown();
    // Give the notices a moment to leave, then close everything.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (auto& c : notified) {
        while (!c->isClosed() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto& c : conns) c->close();
    for (auto& c : conns) c->join();
    for (auto& l : retired) l->stop();
    for (auto& pl : pools) pl->stop();
    stream_listener_.stop();
    if (expiry_thread_.joinable()) expiry_thread_.join();
    if (maintenance_thread_.joinable()) maintenance_thread_.join();
    drainLog();
    if (log_) log_->flush(); // §8.6
    std::lock_guard<std::recursive_mutex> lk(m_);
    conns_.clear();
}

// ---- simple getters and the PC's phone settings -----------------------------------------------------

bool HostStreamServer::isPhonePaired() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return !store_.empty();
}

std::string HostStreamServer::serviceId() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return state_.serviceId;
}

std::string HostStreamServer::pinnedClientFingerprintShort() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    const DeviceRecord* latest = nullptr;
    for (const auto& d : store_.devices()) {
        if (!latest || d.pairedAtUnixMs >= latest->pairedAtUnixMs) latest = &d;
    }
    if (!latest) return "None";
    return hexLower(latest->certSHA256.data(), std::min<size_t>(6, latest->certSHA256.size()));
}

std::string HostStreamServer::serverFingerprintHex() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return identity_ ? identity_->fingerprintHex() : std::string();
}

uint64_t HostStreamServer::pairingsCompleted() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return pairings_completed_;
}

std::string HostStreamServer::lastPairingRefusal() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return last_pairing_refusal_;
}

std::vector<std::string> HostStreamServer::reconcileLog() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return reconcile_log_;
}

std::string HostStreamServer::saveError() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    std::string out;
    if (!devices_read_error_.empty()) {
        out = "The paired devices couldn't be loaded: " + devices_read_error_ +
              ". No device can connect or pair until it can be read; retrying.";
    }
    if (!settings_load_error_.empty()) {
        if (!out.empty()) out += ' ';
        out += settings_load_error_ + ".";
    }
    if (!devices_save_error_.empty() || !pairing_save_error_.empty()) {
        if (!out.empty()) out += ' ';
        const std::string& e = devices_save_error_.empty() ? pairing_save_error_ : devices_save_error_;
        out += "Saving the paired devices failed (" + e + "); retrying.";
    }
    if (!settings_save_error_.empty()) {
        if (!out.empty()) out += ' ';
        out += "Saving the pet-camera settings failed (" + settings_save_error_ + "); retrying.";
    }
    if (!privacy_marker_error_.empty()) {
        if (!out.empty()) out += ' ';
        out += "Saving that privacy mode is on failed (" + privacy_marker_error_ + "); it is on now; retrying.";
    }
    return out;
}

bool HostStreamServer::allowPhoneModeSwitch() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return state_.allowPhoneModeSwitch;
}

void HostStreamServer::setAllowPhoneModeSwitch(bool v) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (state_.allowPhoneModeSwitch == v) return;
        state_.allowPhoneModeSwitch = v;
        markPairingDirtyLocked();
        for (auto& s : sessions_) {
            sendHelloLocked(*s);
            sendHostStateLocked(*s);
        }
    }
    flushSaves(FlushKind::Change);
}

bool HostStreamServer::allowPhoneSensorPowerControl() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return state_.allowPhoneSensorPowerControl;
}

void HostStreamServer::setAllowPhoneSensorPowerControl(bool v) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (state_.allowPhoneSensorPowerControl == v) return;
        state_.allowPhoneSensorPowerControl = v;
        markPairingDirtyLocked();
        for (auto& s : sessions_) sendHostStateLocked(*s);
    }
    flushSaves(FlushKind::Change);
}

bool HostStreamServer::turnOffKinectOnPhoneDisconnect() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return state_.turnOffKinectOnPhoneDisconnect;
}

void HostStreamServer::setTurnOffKinectOnPhoneDisconnect(bool v) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (state_.turnOffKinectOnPhoneDisconnect == v) return;
        state_.turnOffKinectOnPhoneDisconnect = v;
        markPairingDirtyLocked();
    }
    flushSaves(FlushKind::Change);
}

int HostStreamServer::activeClientProtocolVersion() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return sessions_.empty() ? 1 : sessions_.back()->version;
}

int HostStreamServer::sessionCount() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return static_cast<int>(sessions_.size());
}

uint64_t HostStreamServer::stateSeq() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return state_seq_;
}

std::string HostStreamServer::advertisementStatus() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return ad_status_;
}

uint64_t HostStreamServer::sentFramesCount() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    uint64_t n = sent_frames_base_.load();
    for (const auto& s : sessions_) n += s->conn->sentVideoFrames();
    return n;
}

void HostStreamServer::setLiveVideo(LiveVideoControl* live) {
    std::lock_guard<std::recursive_mutex> lk(m_);
    live_ = live;
}

void HostStreamServer::setLocalTimeForTest(const std::optional<LocalTime>& t) {
    // Only the clock: the schedule check in maintenanceLoop notices a change within a second, as it
    // would notice the real clock crossing a row's edge.
    std::lock_guard<std::recursive_mutex> lk(m_);
    test_time_ = t;
}

void HostStreamServer::setCrashBetweenDeviceWritesForTest(bool on) {
    std::lock_guard<std::recursive_mutex> lk(m_);
    crash_test_ = on;
    store_.setCrashBetweenWritesForTest(on);
}

// ---- saving: snapshot under the lock, write outside it ---------------------------------------------------

void HostStreamServer::markPairingDirtyLocked() {
    // The phone settings only: serviceId and the pin (the downgrade mirror) change only through the full
    // device save (DeviceStore::save), and state_ already holds what is on disk for both.
    pairing_dirty_ = true;
    ++pairing_gen_;
}

void HostStreamServer::markDevicesDirtyLocked() {
    devices_dirty_ = true; // devices.json, then pairing.json with the mirror
    ++devices_gen_;
}

void HostStreamServer::markSettingsDirtyLocked() {
    settings_dirty_ = true;
    ++settings_gen_;
}

void HostStreamServer::markPrivacyPendingLocked() {
    // Already on disk, or a write is due (or failing and retried): that covers this request too.
    if (privacy_marker_on_disk_ || privacy_marker_write_due_) return;
    privacy_marker_write_due_ = true;
    privacy_marker_may_exist_ = true;
    ++privacy_marker_gen_;
}

void HostStreamServer::applyPrivacyMarkerLocked() {
    settings_.privacyMode = true;
    if (settings_unread_) {
        privacy_on_asked_while_unread_ = true; // still on once the file is read (retrySettingsReadLocked)
    } else {
        markSettingsDirtyLocked(); // pet-camera.json gets privacy on; that write removes the marker
    }
}

std::string HostStreamServer::stateErrorsLocked() const {
    return devices_read_error_ + "\n" + settings_load_error_ + "\n" + devices_save_error_ + "\n" + pairing_save_error_ +
           "\n" + settings_save_error_ + "\n" + privacy_marker_error_;
}

bool HostStreamServer::flushSaves(FlushKind kind) {
    // io_m_ first, then m_ (never the other way round): every state-file write happens in the order its
    // snapshot was taken, so an older snapshot can never land on top of a newer one.
    std::lock_guard<std::mutex> io(io_m_);
    DeviceStore dev{std::wstring()};
    HostPersistedState st;
    PetSettings set;
    std::wstring dir;
    bool full = false, list = false, pairing = false, settings = false, marker_write = false, marker_delete = false;
    bool marker_may_exist = false;
    uint64_t dev_gen = 0, pair_gen = 0, set_gen = 0, marker_gen = 0;
    std::string before;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        // The crash test mode stands for a host that died after its failed write: no retry, no exit save.
        if (crash_test_ && kind != FlushKind::Change) return false;
        before = stateErrorsLocked();
        const bool devices_ok = !devices_unread_; // never over a devices.json that couldn't be read
        full = devices_ok && devices_dirty_;
        list = devices_ok && !full &&
               (kind == FlushKind::Exit || (kind == FlushKind::TouchFlush && store_.hasUnsavedTouches()));
        pairing = pairing_dirty_ && !full; // a full save writes pairing.json too
        settings = settings_dirty_ && !settings_unread_; // never over a pet-camera.json that couldn't be read
        marker_write = privacy_marker_write_due_;
        marker_delete = !marker_write && privacy_marker_delete_due_;
        marker_may_exist = privacy_marker_may_exist_;
        marker_gen = privacy_marker_gen_;
        if (!full && !list && !pairing && !settings && !marker_write && !marker_delete) return false;
        if (full || list) dev = store_;
        st = state_;
        if (settings) set = settings_;
        dir = opts_.state_dir;
        dev_gen = devices_gen_;
        pair_gen = pairing_gen_;
        set_gen = settings_gen_;
    }

    // The writes, outside the server's lock: the fan-out and every session carry on meanwhile.
    bool dev_ok = true, pair_ok = true, set_ok = true, mk_ok = true, del_ok = true;
    std::string dev_err, pair_err, set_err, mk_err;
    DWORD del_error = 0;
    // The pending privacy-on first: durable before anything else is tried.
    if (marker_write) {
        ++save_attempts_;
        mk_ok = writeOwnerOnlyFileAtomically(dir, kPrivacyMarkerFile,
                                             "privacy mode was turned on while pet-camera.json couldn't be read\n", &mk_err);
    }
    if (list) {
        ++save_attempts_;
        if (!dev.saveDeviceListOnly(&dev_err)) {
            full = true; // the mirror changed since the last save (or the write failed): the full save
            dev_err.clear();
        }
    }
    if (full) {
        ++save_attempts_;
        dev_ok = dev.save(st, &dev_err);
    }
    if (pairing) {
        ++save_attempts_;
        pair_ok = saveHostStateAtomically(dir, st, &pair_err);
    }
    if (settings) {
        ++save_attempts_;
        set_ok = savePetSettingsAtomically(dir, set, &set_err);
    }
    // pet-camera.json was written: the marker has done its job. A settings write happens only once the file has
    // been read, and the marker was applied to settings_ then, so this snapshot already reflects it; if it says
    // privacy off, that is the owner's later choice, which a leftover marker must not undo at the next start.
    if (settings && set_ok && (marker_may_exist || marker_write)) marker_delete = true;
    if (marker_delete) {
        const std::wstring path = dir + L"\\" + kPrivacyMarkerFile;
        if (!DeleteFileW(path.c_str())) {
            del_error = GetLastError();
            del_ok = del_error == ERROR_FILE_NOT_FOUND || del_error == ERROR_PATH_NOT_FOUND;
        }
    }

    std::lock_guard<std::recursive_mutex> lk(m_);
    auto text = [](const std::string& e) { return e.empty() ? std::string("write failed") : e; };
    if (full || list) {
        if (dev_ok) {
            store_.adoptSave(dev);
            // What pairing.json holds now (a full save wrote the mirror into it).
            if (full) state_.pinnedClientCertSHA256 = st.pinnedClientCertSHA256;
        }
        if (full) {
            // A change made while this was written is still due: its own flush (waiting on io_m_) or the
            // retry writes it.
            if (dev_ok && devices_gen_ == dev_gen) devices_dirty_ = false;
            if (!dev_ok) devices_dirty_ = true;
            devices_save_error_ = dev_ok ? std::string() : text(dev_err);
            if (dev_ok && pairing_gen_ == pair_gen) {
                pairing_dirty_ = false; // pairing.json was written from this snapshot too
                pairing_save_error_.clear();
            }
        }
    }
    if (pairing) {
        if (pair_ok && pairing_gen_ == pair_gen) pairing_dirty_ = false;
        pairing_save_error_ = pair_ok ? std::string() : "pairing.json: " + text(pair_err);
    }
    if (settings) {
        if (set_ok) settings_load_error_.clear(); // the file holds what is in force again
        if (set_ok && settings_gen_ == set_gen) settings_dirty_ = false;
        settings_save_error_ = set_ok ? std::string() : "pet-camera.json: " + text(set_err);
    }
    if (marker_write) {
        if (mk_ok) privacy_marker_on_disk_ = true;
        if (mk_ok && privacy_marker_gen_ == marker_gen) privacy_marker_write_due_ = false;
        privacy_marker_error_ = mk_ok ? std::string() : "privacy-on.pending: " + text(mk_err);
    }
    if (marker_delete) {
        if (privacy_marker_gen_ == marker_gen) {
            // Removed (and the file now holds what is in force, so a marker write still due isn't needed either).
            privacy_marker_write_due_ = false;
            privacy_marker_delete_due_ = !del_ok;
            if (del_ok) {
                privacy_marker_may_exist_ = false;
                privacy_marker_on_disk_ = false;
                privacy_marker_error_.clear();
            } else {
                privacy_marker_error_ = "removing privacy-on.pending: Win32 error " + std::to_string(del_error);
            }
        }
    }
    // The retry backs off while a write keeps failing (100 ms doubling to 5 s) and starts over on success.
    const bool failed = !dev_ok || !pair_ok || !set_ok || !mk_ok || !del_ok;
    if (failed) {
        next_save_retry_ns_ = monotonicNs() + save_retry_ms_ * kMs;
        save_retry_ms_ = std::min(save_retry_ms_ * 2, kMaxRetryMs);
    } else {
        save_retry_ms_ = kFirstRetryMs;
        next_save_retry_ns_ = 0;
    }
    return stateErrorsLocked() != before;
}
void HostStreamServer::notifyPairingChanged() {
    if (onPairingStateChanged) onPairingStateChanged();
}

void HostStreamServer::notifyPetStateChanged() {
    if (onPetStateChanged) onPetStateChanged();
}

void HostStreamServer::updateAdvertisementLocked() {
    if (!opts_.advertise) {
        ad_status_ = "mDNS off";
        return;
    }
    // v3 §2.1: advertised while any device is listed.
    if (!store_.empty() && started_) {
        const std::string want = state_.serviceId + "." + kStreamServiceType + ".local";
        if (!stream_ad_.active() || stream_ad_.fullName() != want) {
            stream_ad_.stop();
            std::string err;
            if (stream_ad_.start(state_.serviceId, kStreamServiceType, stream_port_, primaryLanIPv4(), &err)) {
                ad_status_ = "advertising _kv2s._tcp";
            } else {
                ad_status_ = "mDNS failed: " + err;
            }
        }
    } else {
        stream_ad_.stop();
        ad_status_ = "not advertised (unpaired)";
    }
}

void HostStreamServer::updateConnectedFlagLocked() {
    paired_client_connected_ = !sessions_.empty();
}

// ---- lookups ------------------------------------------------------------------------------------------

HostStreamServer::Session* HostStreamServer::findSessionLocked(const TlsConnection& conn) {
    for (auto& s : sessions_) {
        if (s->conn.get() == &conn) return s.get();
    }
    return nullptr;
}

const DeviceRecord* HostStreamServer::deviceOfLocked(const Session& s) const {
    return store_.findByHash(s.hash);
}

// v3 §2.1: every permission check reads the device's current role, never a cached copy.
PetRole HostStreamServer::roleOfLocked(const Session& s) const {
    const DeviceRecord* d = deviceOfLocked(s);
    return d ? d->role : PetRole::Viewer; // a device that is gone is treated as least able
}

bool HostStreamServer::scheduleOffNowLocked() const {
    return isSharingScheduledOff(settings_.sharingSchedule, test_time_ ? *test_time_ : localTimeNow());
}

bool HostStreamServer::sharingScheduledOffNow() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return scheduleOffNowLocked();
}

bool HostStreamServer::anyAvailableLocked() const {
    for (const auto& s : sessions_) {
        if (s->available) return true;
    }
    return false;
}

void HostStreamServer::touchDeviceLocked(const Session& s) {
    // Last seen always; the version only once the session has said it (its first ping), so a session
    // that ends before pinging doesn't turn a v3 device's lastProtocolVersion into 1.
    store_.touch(s.hash, unixMsNow(), s.pinged ? std::optional<int>(s.version) : std::nullopt);
}

// ---- messages to one session ------------------------------------------------------------------------

HelloServerMessage HostStreamServer::helloForLocked(const Session& s) {
    HelloServerMessage h = onHelloSnapshot ? onHelloSnapshot() : HelloServerMessage{};
    h.protocolVersion = kHostProtocolVersion;
    // v3 §2.4: a viewer-role device hides its mode picker (v1 phones read this from hello).
    h.allowPhoneModeSwitch = roleOfLocked(s) == PetRole::Owner && state_.allowPhoneModeSwitch;
    h.codecRaw = static_cast<uint8_t>(CodecKind::H264);
    h.petCamera = PetCameraHello{};
    // The roll is offered when this host has one (a roll folder). This host can listen outside the home
    // (v3 §4.1: true whether or not it is switched on; 0x0B's outsideEnabled says that).
    h.petCamera->cameraRoll = rollConfiguredLocked();
    h.petCamera->outside = true;
    return h;
}

void HostStreamServer::sendHelloLocked(Session& s) {
    s.conn->send(packFrame(MsgType::HelloServer, 0, toJson(helloForLocked(s))));
}

// The only place 0x07 is built for a session: never to one that hasn't said v2 (v3 §4.3).
void HostStreamServer::sendHostStateLocked(Session& s) {
    if (s.version < 2 || !onClientConnectedInitialState) return;
    HostStateSyncMessage m = onClientConnectedInitialState();
    m.protocolVersion = kHostProtocolVersion;
    if (roleOfLocked(s) == PetRole::Owner) {
        m.allowPhoneModeSwitch = state_.allowPhoneModeSwitch;
        m.allowPhoneSensorPower = state_.allowPhoneSensorPowerControl;
        m.sensorPowerDisabledReason.reset();
        if (!state_.allowPhoneSensorPowerControl) m.sensorPowerDisabledReason = text::kSensorPowerDisabledOnHost;
    } else {
        // v3 §2.4: a viewer can't switch, power or record, and is told why.
        m.allowPhoneModeSwitch = false;
        m.allowPhoneSensorPower = false;
        m.sensorPowerDisabledReason = text::kViewerPower;
        m.canRecordCurrentMode = false;
        m.recordingDisabledReason = text::kViewerRecording;
    }
    if (s.version == 2 && !s.available) {
        // v3 §2.3: an unavailable v2 phone sees its own "off", with the reason under the switch. The
        // real sensor state is unchanged and other sessions see the truth.
        m.powerState = SensorPowerState::Off;
        m.inUseHolder.reset();
        m.errorMessage.reset();
        m.allowPhoneSensorPower = false;
        const char* why = text::sentenceFor(s.reason);
        m.sensorPowerDisabledReason = why ? std::string(why) : s.reason;
    }
    m.stateSeq = ++state_seq_; // host-wide; never goes down within a session
    s.conn->send(packFrame(MsgType::HostStateSync, 0, toJson(m)));
    s.presented = true;
    s.presented_available = s.available;
    s.presented_reason = s.reason;
}

PetCameraStateMessage HostStreamServer::petStateForLocked(const Session& s, bool sched_off) {
    PetCameraStateMessage m;
    const DeviceRecord* d = deviceOfLocked(s);
    const PetRole role = d ? d->role : PetRole::Viewer;
    m.deviceId = d ? d->deviceId : deviceIdFromHash(s.hash);
    m.deviceName = d ? d->name : std::string();
    m.role = role;
    m.path = s.path;
    m.sharingEnabled = settings_.sharingEnabled;
    m.sharingScheduleOff = sched_off;
    // v3 §5.1, §8.3: outsideEnabled and outsideEndpoint for both roles; the status for owners below.
    m.outsideEnabled = outsideListeningLocked();
    m.outsideEndpoint = outsideEndpointLocked();
    m.outsideCandidates = outsideCandidatesLocked(m.outsideEndpoint); // every 0x0B, empty when none
    m.privacyMode = settings_.privacyMode;
    m.liveAllowed = s.available;
    if (!s.available) m.liveUnavailableReason = s.reason;
    m.watching = s.watching;
    m.liveQuality = toWire(s.quality);
    m.liveTier = s.watching ? toWire(s.tier) : "high";
    // The roll keys (v3 §5.1). A host without a roll keeps their defaults.
    if (rollConfiguredLocked()) {
        const RollRecorderStatus st = roll_->status();
        m.rollEnabled = settings_.rollEnabled;
        m.motionEnabled = settings_.motionEnabled;
        m.motionAvailable = st.motionAvailable;
        m.motionSensitivity = settings_.motionSensitivity;
        m.scheduleEnabled = settings_.recordScheduleEnabled;
        if (!settings_.recordSchedule.empty()) m.scheduleSummary = recordScheduleSummary(settings_.recordSchedule);
        m.clipRecording = st.recording;
        if (st.recording && st.clipTrigger) m.clipTrigger = std::string(toWire(*st.clipTrigger));
        m.clipManualActive = st.manualActive;
        m.clipElapsedSec = st.recording ? st.clipElapsedSec : 0.0;
        if (st.block != RollBlock::None) setRollBlocked(m, std::string(rollBlockCode(st.block)));
        else setRollBlocked(m, std::nullopt);
        m.rollAvailable = rollAccessLocked(s, role) == nullptr;
        // Thumbnails need rollAvailable and privacy off; a playback also a slot (§5.1).
        m.playbackAvailable = m.rollAvailable && !settings_.privacyMode && canTakeSlotLocked(s, role);
    } else if (const char* block = rollFolderBlockLocked()) {
        // A roll folder is set but can't be used: still no roll (rollAvailable false), but the switch and the
        // reason, so the owner learns why nothing records (v3 §5.1).
        m.rollEnabled = settings_.rollEnabled;
        setRollBlocked(m, std::string(block));
    }
    if (role == PetRole::Owner) {
        std::vector<PetWatcher> watchers;
        for (const auto& o : sessions_) {
            const bool playing = playingLocked(*o) && o->holds_slot;
            if (!o->watching && !playing) continue;
            const DeviceRecord* od = deviceOfLocked(*o);
            PetWatcher w;
            w.deviceId = od ? od->deviceId : deviceIdFromHash(o->hash);
            w.deviceName = od ? od->name : std::string();
            w.role = od ? od->role : PetRole::Viewer;
            w.path = o->path;
            // One video stream per session (§5.9), so one activity.
            w.activity = playing ? watcher_activity::kPlayback : watcher_activity::kLive;
            watchers.push_back(std::move(w));
        }
        m.watchingNow = std::move(watchers);
        std::vector<PetDevice> list;
        for (const auto& dev : store_.devices()) {
            PetDevice pd;
            pd.deviceId = dev.deviceId;
            pd.name = dev.name;
            pd.role = dev.role;
            pd.kind = deviceKindString(dev.kind);
            pd.pairedAtUnixMs = dev.pairedAtUnixMs;
            pd.lastSeenUnixMs = dev.lastSeenUnixMs;
            for (const auto& o : sessions_) pd.connected = pd.connected || o->hash == dev.certSHA256;
            list.push_back(std::move(pd));
        }
        m.devices = std::move(list);
        // Owners only: the status and the PC's sentence for it ("mapping" until the first check finishes).
        if (outside_listener_ && !mapper_state_.reported) m.outsideStatus = std::string(outside_status::kMapping);
        else m.outsideStatus = std::string(outside::outsideStatusName(outsideStatusLocked()));
        m.outsideStatusText = outsideSentenceLocked();
    }
    return m;
}

// v3 §5.1: sent on becoming v3, whenever a field of this session's message changes, and (forced) in
// answer to every 0x0E. Never to a session below v3.
void HostStreamServer::sendPetStateLocked(Session& s, bool force) {
    if (s.version < 3) return;
    PetCameraStateMessage m = petStateForLocked(s, last_schedule_off_);
    m.stateSeq = 0;
    std::string key = toJson(m);
    if (!force && key == s.last_0b) return;
    s.last_0b = std::move(key);
    m.stateSeq = ++state_seq_;
    s.conn->send(packFrame(MsgType::PetCameraState, 0, toJson(m)));
    // The connection log: each time the outside endpoint this device is told changes (and the first time),
    // that it was sent and which kind, by device. Never the address itself.
    std::string told;
    for (const auto& c : *m.outsideCandidates) told += c.host + ":" + std::to_string(c.port) + " ";
    const bool overlay_told = m.outsideCandidates->size() > (m.outsideEndpoint ? 1u : 0u);
    if (!s.endpoint_logged || told != s.endpoint_told) {
        s.endpoint_logged = true;
        s.endpoint_told = told;
        const DeviceRecord* d = deviceOfLocked(s);
        std::string kind = !m.outsideEndpoint            ? "no outside endpoint"
                           : m.outsideEndpoint->verified ? "outside endpoint sent (read from the router)"
                                                         : "outside endpoint sent (entered on the PC, not checked)";
        if (overlay_told) kind += ", and this PC's overlay address";
        logLocked("outside_endpoint_told", s.path, std::string(), d ? d->deviceId : deviceIdFromHash(s.hash),
                  d ? d->name : std::string(), kind);
    }
}

void HostStreamServer::pushHostState() {
    std::lock_guard<std::recursive_mutex> lk(m_);
    for (auto& s : sessions_) sendHostStateLocked(*s); // built (stateSeq raised) and queued under m_
}

void HostStreamServer::notifyModeChanged(const HelloServerMessage&) {
    // The hello is built per session (role), from onHelloSnapshot, which already has the new mode.
    std::lock_guard<std::recursive_mutex> lk(m_);
    for (auto& s : sessions_) sendHelloLocked(*s);
}

// ---- video --------------------------------------------------------------------------------------------

LiveTier HostStreamServer::desiredTierLocked(const Session& s) const {
    if (s.version < 3) return LiveTier::High; // v3 §6.1: v1 and v2 sessions are always on high
    if (s.quality == LiveQuality::Auto) return s.auto_tier;
    return tierForQuality(s.quality);
}

void HostStreamServer::startVideoLocked(Session& s, LiveTier t) {
    s.watching = true;
    s.tier = t;
    s.needs_config = true; // v3 §6.2: its tier's 0x02 before its first AU, and that AU an IDR
    s.wait_idr = false;
    if (live_) {
        live_->wantTier(t);
        // Only an IDR: the epoch it is shown is picked at that IDR (epochShownLocked), so another tier's
        // higher epoch never makes this tier start a new one for the sessions already on it.
        live_->requestTierKeyframe(t);
    }
}

// The epoch a session needing a 0x02 is shown at an IDR of its tier: the same one again for the stream it
// already has; the tier's own when that is above every epoch it saw; otherwise a fresh number from the
// host-wide counter, for this session alone. 0 when none can be had (no live video control).
uint32_t HostStreamServer::epochShownLocked(const Session& s, const TierAccessUnit& au) {
    if (s.has_last_tier && s.last_tier == au.tier && s.src_epoch == au.epoch) return s.last_epoch;
    if (au.epoch > s.last_epoch) return au.epoch;
    return live_ ? live_->nextLiveEpoch() : 0;
}

void HostStreamServer::stopVideoLocked(Session& s) {
    if (!s.watching) return;
    s.watching = false;
    if (live_) live_->releaseTier(s.tier);
}

// v3 §6.3: drops feed `auto`. Called under m_ from the fan-out.
void HostStreamServer::noteDropLocked(Session& s, uint64_t now_ns, std::vector<Session*>& retier) {
    s.last_drop_ns = now_ns;
    if (s.version < 3 || s.quality != LiveQuality::Auto) return;
    s.drops_ns.push_back(now_ns);
    s.drops_ns.erase(std::remove_if(s.drops_ns.begin(), s.drops_ns.end(),
                                    [&](uint64_t t) { return now_ns - t > kAutoDropWindowNs; }),
                     s.drops_ns.end());
    if (static_cast<int>(s.drops_ns.size()) >= kAutoDropsToStepDown && s.auto_tier != LiveTier::Low) {
        s.auto_tier = s.auto_tier == LiveTier::High ? LiveTier::Medium : LiveTier::Low;
        s.drops_ns.clear();
        s.last_step_ns = now_ns;
        retier.push_back(&s);
    }
}

void HostStreamServer::deliverLiveAccessUnit(const TierAccessUnit& au) {
    std::lock_guard<std::recursive_mutex> lk(m_); // the fan-out lock: tier changes happen under it too
    if (!started_ || shutting_down_) return;
    std::vector<uint8_t> cfg_frame;
    std::vector<uint8_t> au_frame;
    auto cfg = [&]() -> const std::vector<uint8_t>& {
        if (cfg_frame.empty()) cfg_frame = packFrame(MsgType::StreamConfig, 0, toJson(streamConfigFor(au)));
        return cfg_frame;
    };
    auto video = [&]() -> const std::vector<uint8_t>& {
        if (au_frame.empty()) {
            std::vector<uint8_t> payload;
            const uint8_t flags = videoPayloadFor(au, payload);
            au_frame = packFrame(MsgType::VideoAccessUnit, flags, payload);
        }
        return au_frame;
    };
    // A session shown another number than the tier's epoch gets the same 0x02 and 0x03 with that number
    // in place of the tier's (the 0x03 header's first field); built once per number.
    struct Renumbered {
        std::vector<uint8_t> cfg, au;
    };
    std::map<uint32_t, Renumbered> renumbered;
    auto cfgFor = [&](uint32_t shown) -> const std::vector<uint8_t>& {
        if (shown == au.epoch) return cfg();
        Renumbered& r = renumbered[shown];
        if (r.cfg.empty()) {
            StreamConfigMessage c = streamConfigFor(au);
            c.streamEpoch = shown;
            r.cfg = packFrame(MsgType::StreamConfig, 0, toJson(c));
        }
        return r.cfg;
    };
    auto videoFor = [&](uint32_t shown) -> const std::vector<uint8_t>& {
        if (shown == au.epoch) return video();
        Renumbered& r = renumbered[shown];
        if (r.au.empty()) {
            r.au = video();
            r.au[kWireHeaderSize + 0] = static_cast<uint8_t>(shown >> 24); // VideoAccessUnitHeader::streamEpoch
            r.au[kWireHeaderSize + 1] = static_cast<uint8_t>(shown >> 16);
            r.au[kWireHeaderSize + 2] = static_cast<uint8_t>(shown >> 8);
            r.au[kWireHeaderSize + 3] = static_cast<uint8_t>(shown);
        }
        return r.au;
    };
    const uint64_t now = monotonicNs();
    uint64_t sent_bytes = 0;
    std::vector<Session*> retier;
    for (auto& sp : sessions_) {
        Session& s = *sp;
        if (!s.watching || s.tier != au.tier) continue;
        if (s.needs_config) {
            // Joining, becoming available, watching again or a new tier: this tier's 0x02, then an IDR.
            if (!au.idr) continue;
            const uint32_t shown = epochShownLocked(s, au);
            if (shown == 0) continue;
            s.conn->send(cfgFor(shown));
            s.needs_config = false;
            s.wait_idr = false;
            s.last_epoch = shown;
            s.src_epoch = au.epoch;
            s.last_tier = au.tier;
            s.has_last_tier = true;
            s.conn->send(videoFor(shown), true);
            sent_bytes += videoFor(shown).size();
            continue;
        }
        if (au.epoch != s.src_epoch) {
            // The tier started a new epoch (open, mode or size change): every session on it gets its 0x02
            // at the epoch's IDR, config_due or not (a phone drops every 0x03 whose epoch isn't its last
            // 0x02's, v3 §6.2). An AU of it before that IDR means nothing to this session.
            if (!au.idr) continue;
            const uint32_t shown = au.epoch > s.last_epoch ? au.epoch : (live_ ? live_->nextLiveEpoch() : 0);
            if (shown == 0) continue;
            s.conn->send(cfgFor(shown));
            s.last_epoch = shown;
            s.src_epoch = au.epoch;
        } else if (au.config_due) {
            // The same epoch's config again (the host asked for a keyframe with its config).
            s.conn->send(cfgFor(s.last_epoch));
        }
        if (s.wait_idr && !au.idr) continue;
        if (!au.idr && s.conn->videoInFlight()) {
            // Back-pressure, per session: it waits for the next IDR, requested under the 500 ms rule,
            // so a slow device never forces keyframes on the others faster than that.
            ++dropped_congestion_;
            s.wait_idr = true;
            noteDropLocked(s, now, retier);
            if (live_) live_->requestTierKeyframe(au.tier);
            continue;
        }
        s.wait_idr = false;
        s.conn->send(videoFor(s.last_epoch), true);
        sent_bytes += video().size();
    }
    for (Session* s : retier) {
        // auto stepped down: the new tier's 0x02 (higher epoch) and an IDR, nothing of the old tier after it.
        const LiveTier want = desiredTierLocked(*s);
        if (s->watching && want != s->tier) {
            stopVideoLocked(*s);
            startVideoLocked(*s, want);
        }
        sendPetStateLocked(*s, false);
    }
    if (sent_bytes > 0) {
        sent_bytes_window_ += sent_bytes;
        if (last_bitrate_sample_ns_ == 0) {
            last_bitrate_sample_ns_ = now;
        } else if (now - last_bitrate_sample_ns_ >= 1000000000ULL) {
            const double dt = static_cast<double>(now - last_bitrate_sample_ns_) / 1e9;
            bitrate_mbps_ = (static_cast<double>(sent_bytes_window_) * 8.0) / (dt * 1e6);
            sent_bytes_window_ = 0;
            last_bitrate_sample_ns_ = now;
        }
    }
}

int HostStreamServer::sessionsOnTier(LiveTier t) const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    int n = 0;
    for (const auto& s : sessions_) n += (s->watching && s->tier == t) ? 1 : 0;
    return n;
}

// ---- availability, slots and presentation (v3 §2.3) ----------------------------------------------------

void HostStreamServer::reevaluateLocked() {
    if (shutting_down_) return;
    syncInviteLocked(); // §8.4: the invite follows the endpoint the PC shows
    const bool sched_off = scheduleOffNowLocked();
    last_schedule_off_ = sched_off;

    struct Eval {
        std::shared_ptr<Session> s;
        PetRole role = PetRole::Viewer;
        std::string base; // conditions 1-3
        bool wants = true; // condition 4
    };
    std::vector<Eval> ev;
    ev.reserve(sessions_.size());
    for (auto& sp : sessions_) {
        Eval e;
        e.s = sp;
        e.role = roleOfLocked(*sp);
        // A v3 session holds a slot while it wants live video or plays a clip (one video stream, §5.9).
        e.wants = sp->version < 3 ? true : (sp->wants_live || playingLocked(*sp));
        // A connection waiting to replace its device's session (no hello yet) wants no slot until
        // its hello, so it never takes a viewer's picture away (an owner's would) only to close a moment later.
        if (sp->replaces_on_hello) e.wants = false;
        const bool viewer = e.role != PetRole::Owner;
        if (settings_.privacyMode) e.base = live_unavailable::kPrivacyMode;
        else if (viewer && pc_replaying_) e.base = live_unavailable::kPcReplaying;
        else if (viewer && !settings_.sharingEnabled) e.base = live_unavailable::kSharingOff;
        else if (viewer && sched_off) e.base = live_unavailable::kSharingSchedule;
        ev.push_back(std::move(e));
    }

    // Condition 5, the watcher slots: holders keep theirs, free slots go in arrival order, an owner
    // takes the slot of the viewer that took one most recently.
    int holders = 0;
    for (auto& e : ev) {
        if (e.s->holds_slot && !(e.base.empty() && e.wants)) e.s->holds_slot = false;
        if (e.s->holds_slot) ++holders;
    }
    for (auto& e : ev) {
        Session& s = *e.s;
        if (s.holds_slot || !e.base.empty() || !e.wants) continue;
        if (holders < kMaxWatcherSlots) {
            s.holds_slot = true;
            s.slot_seq = ++slot_counter_;
            ++holders;
            continue;
        }
        if (e.role != PetRole::Owner) continue;
        Eval* victim = nullptr;
        for (auto& o : ev) {
            if (o.s->holds_slot && o.role != PetRole::Owner && (!victim || o.s->slot_seq > victim->s->slot_seq)) victim = &o;
        }
        if (victim) {
            victim->s->holds_slot = false;
            s.holds_slot = true;
            s.slot_seq = ++slot_counter_;
        }
    }
    // Host-initiated playback stops (§5.9): privacy mode, a viewer losing roll access (sharing off, its
    // schedule, a PC replay), or an owner taking the slot. The stream ends with 0x17 stopped and the reason's
    // sentence, and the session wants live video only if it asks again (0x0E).
    for (auto& e : ev) {
        Session& s = *e.s;
        // A session that may not see the roll now (privacy mode; for a viewer, sharing off, its schedule or a PC
        // replay) gets no thumbnail that was still queued for it. Each such request is answered with the reason.
        if (!e.base.empty() && s.version >= kProtocolV3) {
            for (uint64_t rid : s.conn->dropQueued(kDropRollThumbs)) sendCommandResult(s.conn, rid, e.base.c_str());
        }
        if (!playingLocked(s)) continue;
        if (!e.base.empty()) {
            endPlaybackLocked(s, e.base.c_str(), false);
        } else if (!s.holds_slot) {
            endPlaybackLocked(s, live_unavailable::kTooManyViewers, false);
        } else {
            continue;
        }
        e.wants = s.version < 3 ? true : s.wants_live;
    }
    bool viewer_holds = false;
    for (const auto& e : ev) viewer_holds = viewer_holds || (e.s->holds_slot && e.role != PetRole::Owner);
    for (auto& e : ev) {
        Session& s = *e.s;
        std::string reason = e.base;
        if (reason.empty() && !s.holds_slot) {
            if (e.wants) {
                reason = live_unavailable::kTooManyViewers;
            } else {
                // Not watching (0x0E watch:false): could it get a slot if it asked?
                const bool could = holders < kMaxWatcherSlots || (e.role == PetRole::Owner && viewer_holds);
                if (!could) reason = live_unavailable::kTooManyViewers;
            }
        }
        s.available = reason.empty();
        s.reason = std::move(reason);
    }

    // Video: start, stop or change tier; v1 sessions that are unavailable are closed.
    std::vector<std::shared_ptr<Session>> v1_close;
    for (auto& e : ev) {
        Session& s = *e.s;
        if (s.version < 2 && !s.available && s.pinged) {
            v1_close.push_back(e.s); // v3 §2.3: a v1 phone can't be told why
            continue;
        }
        // Live video: not while the session plays a clip (§5.9: playback keeps the slot, live stops).
        const bool watch = s.available && e.wants && s.holds_slot && !playingLocked(s);
        if (watch) {
            const LiveTier want = desiredTierLocked(s);
            if (!s.watching) {
                startVideoLocked(s, want);
            } else if (s.tier != want) {
                stopVideoLocked(s);
                startVideoLocked(s, want);
            }
        } else {
            stopVideoLocked(s);
        }
    }
    ev.clear();
    for (auto& s : v1_close) {
        s->holds_slot = false;
        dropSessionLocked(*s, nullptr);
    }

    // Presentation: 0x07 for v2+ (first one, a role change, or the v2 "off" presentation changing),
    // then 0x0B for v3 when its content changed (or forced).
    for (auto& sp : sessions_) {
        Session& s = *sp;
        if (s.resend_hello) {
            s.resend_hello = false;
            sendHelloLocked(s);
            sendHostStateLocked(s);
            s.force_0b = true;
        } else if (s.version >= 2 && (!s.presented || (s.version == 2 && (s.presented_available != s.available ||
                                                                            s.presented_reason != s.reason)))) {
            sendHostStateLocked(s);
        }
        sendPetStateLocked(s, s.force_0b);
        s.force_0b = false;
    }
    updateConnectedFlagLocked();
}

// ---- closing sessions -------------------------------------------------------------------------------

void HostStreamServer::dropSessionLocked(Session& s, const char* notice_reason) {
    std::shared_ptr<Session> keep;
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->get() == &s) {
            keep = *it;
            sessions_.erase(it);
            break;
        }
    }
    if (!keep) return;
    // An outside session the host ends (outside off, revoked, replaced, too
    // many) is out of sessions_ from here, so onStreamClosed won't log it: it is logged here, once, with why.
    if (s.path == PetPath::Outside) {
        const DeviceRecord* d = deviceOfLocked(s);
        const uint64_t secs = (unixMsNow() - std::min(unixMsNow(), s.started_ms)) / 1000;
        logLocked("disconnected", s.path, s.addr, d ? d->deviceId : deviceIdFromHash(s.hash), d ? d->name : std::string(),
                  std::string(notice_reason ? notice_reason : "closed by the host") + ", " + std::to_string(secs) + " s");
    }
    stopVideoLocked(s);
    endPlaybackLocked(s, nullptr, true); // §5.9: the session ends, and so does its playback
    s.holds_slot = false;
    sent_frames_base_ += s.conn->sentVideoFrames();
    touchDeviceLocked(s);
    if (notice_reason && s.version >= 3) {
        // Out of the live list now, so the video still queued for it is dropped (a frame part sent stays,
        // to keep the stream whole): a revoked device gets its 0x0F next, not more pictures first. That covers
        // the camera roll's queued 0x15 thumbnails too (dropped, as for any stop the host makes): they go unanswered, as the session ends.
        s.conn->dropQueuedVideo();
        s.conn->dropQueued(kDropRollThumbs);
        s.conn->send(packFrame(MsgType::SessionNotice, 0, toJson(makeSessionNotice(notice_reason))));
        s.conn->closeAfterFlush();
        auto p = std::make_shared<Pending>();
        p->conn = s.conn;
        p->deadline_ns = monotonicNs() + kNotifiedCloseGraceNs;
        p->hash = s.hash;
        pending_.push_back(std::move(p));
    } else {
        s.conn->close();
    }
}

void HostStreamServer::closeSessionsOfHashLocked(const Bytes& hash, const char* notice_reason) {
    std::vector<std::shared_ptr<Session>> victims;
    for (auto& s : sessions_) {
        if (s->hash == hash) victims.push_back(s);
    }
    for (auto& v : victims) dropSessionLocked(*v, notice_reason);
    for (auto& p : pending_) {
        if (p->notice_only && p->hash == hash) p->conn->close();
    }
}

void HostStreamServer::disconnectActiveClient() {
    bool fire = false;
    bool shutting_down = false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        shutting_down = shutting_down_;
        fire = anyAvailableLocked();
        std::vector<std::shared_ptr<Session>> all = sessions_;
        for (auto& s : all) dropSessionLocked(*s, nullptr);
        reevaluateLocked();
    }
    if (fire && !shutting_down) {
        if (onPairedClientDisconnected) onPairedClientDisconnected();
    }
    notifyPairingChanged();
    notifyPetStateChanged();
}

// ---- devices ------------------------------------------------------------------------------------------

std::vector<HostDeviceInfo> HostStreamServer::devices() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    std::vector<HostDeviceInfo> out;
    for (const auto& d : store_.devices()) {
        HostDeviceInfo i;
        i.deviceId = d.deviceId;
        i.name = d.name;
        i.role = d.role;
        i.kind = deviceKindString(d.kind);
        i.pairedAtUnixMs = d.pairedAtUnixMs;
        i.lastSeenUnixMs = d.lastSeenUnixMs;
        i.lastProtocolVersion = d.lastProtocolVersion;
        for (const auto& s : sessions_) {
            if (s->hash != d.certSHA256) continue;
            i.connected = true;
            i.path = toWire(s->path);
            i.sessionVersion = s->version;
            i.watching = s->watching;
            i.available = s->available;
            i.unavailableReason = s->reason;
        }
        out.push_back(std::move(i));
    }
    return out;
}

bool HostStreamServer::devicesFull() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return store_.full();
}

bool HostStreamServer::refuseDeviceChangeLocked(std::string* err) const {
    if (!devices_unread_) return false;
    // The list in memory is empty and devices.json must not be written over: nothing can be changed (or
    // revoked) until it is read, and the PC says why rather than reporting nothing done.
    if (err) *err = "Can't change devices until the device list can be read. " + devices_read_error_ + ".";
    return true;
}

bool HostStreamServer::renameDevice(const std::string& deviceId, const std::string& name, std::string* err) {
    if (err) err->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (refuseDeviceChangeLocked(err)) return false;
        if (!store_.findByDeviceId(deviceId)) {
            if (err) *err = "That device is no longer on the list.";
            return false;
        }
        if (!store_.rename(deviceId, name)) {
            if (err) *err = "That name is empty.";
            return false;
        }
        markDevicesDirtyLocked();
        reevaluateLocked(); // its own deviceName and owners' device lists change
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    return true;
}

bool HostStreamServer::setDeviceRole(const std::string& deviceId, PetRole role, std::string* err) {
    if (err) err->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (refuseDeviceChangeLocked(err)) return false;
        const DeviceRecord* d = store_.findByDeviceId(deviceId);
        if (!d) {
            if (err) *err = "That device is no longer on the list.";
            return false;
        }
        const bool changed = d->role != role;
        if (!store_.setRole(deviceId, role)) {
            if (err) *err = "A device is an owner or a viewer.";
            return false;
        }
        if (changed) {
            markDevicesDirtyLocked(); // the mirror can change (pet-camera.md §5)
            d = store_.findByDeviceId(deviceId);
            if (d) logLocked("role_changed", PetPath::Home, std::string(), d->deviceId, d->name, petRoleString(role));
            // v3 §3.2: at once, a new hello and 0x07 (as setAllowPhoneModeSwitch does) and a 0x0B.
            for (auto& s : sessions_) {
                if (d && s->hash == d->certSHA256) s->resend_hello = true;
            }
            reevaluateLocked();
        }
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    return true;
}

bool HostStreamServer::revokeDevice(const std::string& deviceId, std::string* err) {
    if (err) err->clear();
    {
        // v3 §3.2: under the lock on_ready takes, so a handshake in flight can't slip through.
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (refuseDeviceChangeLocked(err)) return false;
        const DeviceStore::RevokeResult r = store_.revoke(deviceId);
        if (!r.found || !r.removed) {
            if (err) *err = "That device is no longer on the list.";
            return false;
        }
        if (r.nowEmpty) state_.serviceId = randomHexId(6); // as today's unpair: old phones go quiet
        star_drops_.push_back(r.removed->deviceId); // v3 §3.2 step 5, below, outside the lock
        logLocked("revoked", PetPath::Home, std::string(), r.removed->deviceId, r.removed->name, "on the PC");
        // devices.json (with the revoked hash), then pairing.json (mirror), written just below, outside
        // the lock. Fail closed: if that write fails, the revoke still holds (admission reads this list in
        // memory), the PC's status line says so, and the maintenance loop retries until the disk has it.
        markDevicesDirtyLocked();
        closeSessionsOfHashLocked(r.removed->certSHA256, notice_reason::kRevoked);
        updateAdvertisementLocked();
        reevaluateLocked();
    }
    flushSaves(FlushKind::Change);
    maintenance_cv_.notify_all(); // its stars are dropped on the maintenance thread, never here (the UI thread)
    joinRetiredPlaybacks();
    notifyPairingChanged();
    notifyPetStateChanged();
    return true;
}

size_t HostStreamServer::revokeAllDevices(std::string* err) {
    if (err) err->clear();
    size_t n = 0;
    bool fire = false;
    bool shutting_down = false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        // An unread device list has no device in it to revoke and must not be written over (nor may
        // serviceId rotate away from what devices.json records): refused, with the reason, until it is read.
        if (refuseDeviceChangeLocked(err)) return 0;
        shutting_down = shutting_down_;
        // The old whole-PC unpair: it still ends the last available session, so the disconnect rule
        // applies as before; a single revoke doesn't fire it (v3 §2.3).
        fire = anyAvailableLocked();
        for (const auto& d : store_.devices()) star_drops_.push_back(d.deviceId); // v3 §3.2 step 5
        n = store_.revokeAll();
        state_.serviceId = randomHexId(6); // no device is left: rotate, as today's unpair
        markDevicesDirtyLocked(); // a failure holds from memory and is retried (as revokeDevice)
        std::vector<std::shared_ptr<Session>> all = sessions_;
        for (auto& s : all) dropSessionLocked(*s, notice_reason::kRevoked);
        for (auto& p : pending_) {
            if (p->notice_only) p->conn->close();
        }
        updateAdvertisementLocked();
        reevaluateLocked();
    }
    flushSaves(FlushKind::Change);
    maintenance_cv_.notify_all(); // the stars go on the maintenance thread
    joinRetiredPlaybacks();
    if (fire && !shutting_down && onPairedClientDisconnected) onPairedClientDisconnected();
    notifyPairingChanged();
    notifyPetStateChanged();
    return n;
}

void HostStreamServer::unpairPhone() {
    revokeAllDevices();
}

// ---- settings ----------------------------------------------------------------------------------------

PetSettings HostStreamServer::petSettings() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return settings_;
}

void HostStreamServer::setSharingEnabled(bool on) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) return; // refused: it would be saved over a file not read yet (status line says so)
        if (settings_.sharingEnabled == on) return;
        settings_.sharingEnabled = on;
        // v3 §8.1, §5.2: turning sharing off also turns outside listening off and clears that setting; it comes
        // back only when it is turned on again on the PC.
        if (!on) {
            settings_.allowOutside = false;
            settings_.outsideMappingRemoved = false; // outside viewing turned off, so the next turn-on maps
        }
        // Sharing on with the outside switch already on (as pet-camera.json had it) turns outside viewing on too:
        // a default port that binds is tried for then as well, if none was found before (v3 §8.7).
        if (on && settings_.allowOutside && !outside::isValidOutsidePort(settings_.outsidePort)) {
            settings_.outsidePort = pickOutsidePortLocked();
        }
        markSettingsDirtyLocked(); // a failure holds from memory and is retried
        applyOutsideLocked();
        reevaluateLocked();
    }
    flushSaves(FlushKind::Change);
    joinRetiredOutside();
    notifyPetStateChanged();
    notifyOutsideChanged();
}

void HostStreamServer::setSharingSchedule(const std::vector<SharingScheduleRow>& rows) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) return; // refused, as setSharingEnabled
        settings_.sharingSchedule = rows;
        if (settings_.sharingSchedule.size() > kMaxSharingScheduleRows) settings_.sharingSchedule.resize(kMaxSharingScheduleRows);
        markSettingsDirtyLocked();
        reevaluateLocked();
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
}

void HostStreamServer::setPrivacyMode(bool on) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) {
            // Privacy is on already (fail closed). Off is refused, as setSharingEnabled; on is kept, so it
            // still wins over what the file says once it is read, and made durable (privacy-on.pending), so it
            // wins after a restart too.
            // A marker that couldn't be checked at start also set privacy_on_asked_while_unread_, but nothing
            // was written then: this request writes it unless it is known to be on disk.
            if (!on) return;
            privacy_on_asked_while_unread_ = true;
            markPrivacyPendingLocked();
        } else {
            if (settings_.privacyMode == on) return;
            settings_.privacyMode = on; // v3 §6.4: all live video stops at once; the sensor is untouched
            // Fail closed: privacy mode is on from now whether or not pet-camera.json could be written; a
            // failed write is shown on the PC and retried until a restart would find it on too.
            markSettingsDirtyLocked();
            pushRollSettingsLocked(); // §6.4: a clip in progress is finalized, and no trigger fires
            reevaluateLocked();       // live video and playback stop

        }
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
}

void HostStreamServer::setPcReplaying(bool replaying) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (pc_replaying_ == replaying) return;
        pc_replaying_ = replaying;
        pushRollSettingsLocked(); // pet-camera.md §6.2: replaying blocks the roll
        reevaluateLocked();
    }
    notifyPetStateChanged();
}

std::vector<PetWatcher> HostStreamServer::watchingNow() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    std::vector<PetWatcher> out;
    for (const auto& s : sessions_) {
        const bool playing = playingLocked(*s) && s->holds_slot;
        if (!s->watching && !playing) continue;
        const DeviceRecord* d = deviceOfLocked(*s);
        PetWatcher w;
        w.deviceId = d ? d->deviceId : deviceIdFromHash(s->hash);
        w.deviceName = d ? d->name : std::string();
        w.role = d ? d->role : PetRole::Viewer;
        w.path = s->path;
        w.activity = playing ? watcher_activity::kPlayback : watcher_activity::kLive;
        out.push_back(std::move(w));
    }
    return out;
}

// ---- stream connections -------------------------------------------------------------------------------

TlsConnection::Callbacks HostStreamServer::streamCallbacksLocked(PetPath path, const std::string& addr, uint64_t outside_gen) {
    TlsConnection::Callbacks cb;
    cb.skip_unknown_types = true; // v2: a future message type is skipped, not a violation
    cb.max_queued_bytes = opts_.send_queue_cap;
    cb.max_inbound_payload = kMaxClientPayload; // a device sends small JSON only
    cb.on_ready = [this, path, addr, outside_gen](TlsConnection& conn) { onStreamReady(conn, path, addr, outside_gen); };
    cb.on_activity = [this](TlsConnection& conn) {
        std::lock_guard<std::recursive_mutex> lk2(m_);
        if (Session* s = findSessionLocked(conn)) s->last_activity_ns = monotonicNs();
    };
    cb.on_frame = [this](TlsConnection& conn, MsgType t, uint8_t, std::vector<uint8_t>& payload) {
        handleClientFrame(conn, t, payload);
    };
    cb.on_closed = [this](TlsConnection& conn, const std::string&) { onStreamClosed(conn); };
    return cb;
}

void HostStreamServer::handleStreamSocket(SOCKET s, const sockaddr* peer) {
    const outside::IpAddress src = sourceOf(peer, "stream");
    const bool local = isLocal(src); // before m_: it may read the adapters
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (shutting_down_) {
            closesocket(s);
            return;
        }
        // v3 §8.1: the LAN stream listener takes local sources only (§8.0), so every connection from the internet
        // goes through the outside listener and its limits. Anything else is closed before TLS, and logged.
        if (local) {
            // Connections still in their TLS handshake, capped in all and per source. A session (or a refusal
            // inside the handshake) ends the count; a paired device's session is never counted.
            // Keyed by the full address, IPv6 too: a household's devices share one /64.
            const std::string key = outside::toText(src);
            if (!lanAdmitLocked(lan_handshakes_, key, opts_.lan_handshakes_total, opts_.lan_handshakes_per_source)) {
                closesocket(s);
                logLocked("refused_too_many", PetPath::Home, outside::toText(src));
            } else {
                // From here the slot and the socket are this branch's until the connection takes them over.
                // `handed`: start() has the socket (it closes it itself when it can't run). The slot is given back
                // once (`counted`), whatever throws, and nothing after start() can close a socket the connection owns.
                bool handed = false;
                std::shared_ptr<std::atomic<bool>> counted;
                try {
                    if (opts_.after_lan_admit_for_test) opts_.after_lan_admit_for_test();
                    counted = std::make_shared<std::atomic<bool>>(true);
                    auto release = [this, key, counted] {
                        if (!counted->exchange(false)) return;
                        std::lock_guard<std::recursive_mutex> lk2(m_);
                        lanReleaseLocked(lan_handshakes_, key);
                    };
                    TlsConnection::Callbacks cb = streamCallbacksLocked(PetPath::Home, outside::toText(src));
                    auto ready = std::move(cb.on_ready);
                    cb.on_ready = [release, ready](TlsConnection& c) {
                        release();
                        if (ready) ready(c);
                    };
                    auto closed = std::move(cb.on_closed);
                    cb.on_closed = [this, release, closed, key](TlsConnection& c, const std::string& why) {
                        release();
                        if (c.certRefused()) {
                            // §8.6: a certificate refused inside the handshake is logged here as on the outside
                            // listener. Coalesced by source: a scanner trying certificates is a line a minute.
                            {
                                std::lock_guard<std::recursive_mutex> lk2(m_);
                                logLocked("refused_unknown_cert", PetPath::Home, key, {}, {},
                                          "a certificate that matches no device");
                            }
                            drainLog();
                        }
                        if (opts_.lan_closed_for_test) opts_.lan_closed_for_test(why);
                        if (closed) closed(c, why);
                    };
                    handed = true;
                    if (auto c = TlsConnection::start(s, stream_ctx_, true, std::move(cb))) {
                        try {
                            if (opts_.after_lan_start_for_test) opts_.after_lan_start_for_test();
                            conns_.push_back(c);
                        } catch (...) {
                            // Not in the list, so nothing would close it at shutdown: it is closed now, and gives its
                            // slot back as it ends.
                            c->close();
                        }
                        return;
                    }
                } catch (...) {
                }
                // Turned away (no thread for it, or a throw before start): the socket closed once, here only if start()
                // never had it, and the slot given back once.
                if (!handed) closesocket(s);
                if (!counted || counted->exchange(false)) lanReleaseLocked(lan_handshakes_, key);
                try {
                    logLocked("refused_unavailable", PetPath::Home, outside::toText(src));
                } catch (...) {
                }
            }
        } else {
            closesocket(s);
            logLocked("refused_local_only", PetPath::Home, outside::toText(src));
        }
    }
    drainLog(); // §8.6, with m_ released
}

void HostStreamServer::onStreamReady(TlsConnection& conn, PetPath path, const std::string& addr, uint64_t outside_gen) {
    // Only reached after the client certificate passed the device list inside the handshake.
    if (testHookBeforeSessionReady) testHookBeforeSessionReady();
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (shutting_down_) {
            conn.close();
            return;
        }
        // v3 §8.1 "Turning it off": a connection the outside pool handed off just before outside listening
        // turned off (the PC's switch, sharing off, an owner's set_sharing) isn't in sessions_ or pending_ yet,
        // so the turn-off couldn't close it. It is refused here, under the same lock, and never becomes a
        // session. By the outside generation it was accepted under, not by
        // "listening" alone: an off and an on again before this runs leave outside listening on, on a new listener,
        // and the connection still came from the old one. A port change moves the generation too, so a connection
        // still being handed off from the old port is dropped as well (it reconnects, as a device does after any
        // close); sessions already established stay across a port change, since this runs once, at ready.
        if (path == PetPath::Outside && (!outsideListeningLocked() || outside_gen != outside_gen_.load())) {
            conn.close();
            logLocked("disconnected", path, addr, {}, {},
                      outsideListeningLocked() ? "outside listening was reopened since it connected"
                                               : "viewing from outside the home turned off");
            return;
        }
        // v3 §2.1: look the leaf up again under the lock revoke holds; a device revoked since the
        // handshake's verify never becomes a session.
        const Bytes der = conn.peerCertDer();
        const Bytes hash = der.empty() ? Bytes() : sha256(der);
        const DeviceRecord* dev = hash.empty() ? nullptr : store_.findByHash(hash);
        if (!dev) {
            ++rejected_handshakes_;
            conn.close();
            logLocked("refused_unknown_cert", path, addr, {}, {}, "its device was removed during the handshake");
            return;
        }
        const PetRole role = dev->role;
        const std::optional<int> last_version = dev->lastProtocolVersion;
        const std::string dev_id = dev->deviceId, dev_name = dev->name; // for the log, before the list is touched
        // Replacement (v3 §2.1): the same device back replaces its older session. Not a disconnect,
        // never against the limit; a v3 one is told "replaced". Only once this one has said hello (its first
        // ping, below), so a connection that closes without one never ends the live session (a
        // phone races dials, and the loser's handshake may land just after the winner's).
        size_t same = 0;
        for (auto& s : sessions_) {
            if (s->hash == hash) ++same;
        }
        // The 8-session limit (v3 §2.2), not counting the sessions this one will replace.
        if (static_cast<int>(sessions_.size() - same) >= kMaxSessions) {
            std::shared_ptr<Session> victim;
            if (role == PetRole::Owner) {
                for (auto& s : sessions_) {
                    if (roleOfLocked(*s) != PetRole::Owner && (!victim || s->id > victim->id)) victim = s;
                }
            }
            if (victim) {
                dropSessionLocked(*victim, notice_reason::kTooManySessions);
            } else if (last_version && *last_version >= kProtocolV3) {
                // Notice-only: hello, then on its v3 ping a 0x0F too_many_sessions and a close, or a
                // close after 1 s with no ping. Never counts, never holds a slot, gets nothing else.
                auto p = std::make_shared<Pending>();
                p->conn = conn.shared_from_this();
                p->notice_only = true;
                p->hash = hash;
                p->path = path;
                p->deadline_ns = monotonicNs() + static_cast<uint64_t>(std::max(100, opts_.notice_only_timeout_ms)) * kMs;
                Session tmp;
                tmp.hash = hash;
                tmp.conn = p->conn;
                sendHelloLocked(tmp);
                pending_.push_back(std::move(p));
                return;
            } else {
                conn.close(); // turned away before hello: an old phone never sees connect then disconnect
                return;
            }
        }
        auto s = std::make_shared<Session>();
        s->conn = conn.shared_from_this();
        s->id = ++next_session_id_;
        s->hash = hash;
        s->path = path; // v3 §8.1: "outside" for a listed device on the outside listener
        // v3 §6.1: auto starts at high at home and at medium outside.
        if (path == PetPath::Outside) s->auto_tier = LiveTier::Medium;
        s->addr = addr;
        s->replaces_on_hello = same > 0;
        s->started_ms = unixMsNow();
        s->last_activity_ns = monotonicNs();
        sessions_.push_back(s);
        touchDeviceLocked(*s); // last seen; the version comes with its first ping
        logLocked("connected", path, addr, dev_id, dev_name); // §8.6
        // Every connection starts at v1, a replacing one too: no 0x07 until its own ping says v2.
        sendHelloLocked(*s);
        reevaluateLocked();
    }
    notifyPairingChanged();
    notifyPetStateChanged();
}

void HostStreamServer::onStreamClosed(TlsConnection& conn) {
    bool fire = false;
    bool was_session = false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [&](const auto& c) { return c.get() == &conn; }), conns_.end());
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const auto& p) { return p->conn.get() == &conn; }),
                       pending_.end());
        std::shared_ptr<Session> gone;
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if ((*it)->conn.get() == &conn) {
                gone = *it;
                sessions_.erase(it);
                break;
            }
        }
        if (gone) {
            was_session = true;
            const bool was_available = gone->available;
            {
                const DeviceRecord* d = deviceOfLocked(*gone);
                const uint64_t secs = (unixMsNow() - std::min(unixMsNow(), gone->started_ms)) / 1000;
                logLocked("disconnected", gone->path, gone->addr, d ? d->deviceId : deviceIdFromHash(gone->hash),
                          d ? d->name : std::string(), std::to_string(secs) + " s");
            }
            stopVideoLocked(*gone);
            endPlaybackLocked(*gone, nullptr, true); // §5.9: "the session ends"
            gone->holds_slot = false;
            sent_frames_base_ += conn.sentVideoFrames();
            touchDeviceLocked(*gone);
            // v3 §2.3: "turn off when the iPhone disconnects" means the last available session ended.
            fire = was_available && !shutting_down_ && !anyAvailableLocked();
            reevaluateLocked(); // a freed slot may make a waiting session available
        }
    }
    if (fire && onPairedClientDisconnected) onPairedClientDisconnected();
    if (was_session) {
        notifyPairingChanged();
        notifyPetStateChanged();
    }
    // Not joined here: this runs on the closing connection's own thread, which a playback may be waiting
    // on. The maintenance loop joins it.
}

bool HostStreamServer::takeCommandTokenLocked(Session& s) {
    const uint64_t now = monotonicNs();
    const double per_second = opts_.command_refill_per_second_for_test >= 0 ? opts_.command_refill_per_second_for_test
                                                                           : kCommandsPerSecond;
    auto it = command_buckets_.find(s.hash);
    if (it == command_buckets_.end()) {
        // Bounded by the paired devices: buckets of devices no longer listed go first.
        if (command_buckets_.size() >= 2 * kMaxPairedDevices) {
            for (auto b = command_buckets_.begin(); b != command_buckets_.end();) {
                b = store_.findByHash(b->first) ? std::next(b) : command_buckets_.erase(b);
            }
        }
        it = command_buckets_.emplace(s.hash, CommandBucket{kCommandBurst, now}).first; // a new device starts full
    } else {
        CommandBucket& b = it->second;
        b.tokens = std::min(kCommandBurst, b.tokens + static_cast<double>(now - b.refill_ns) / 1e9 * per_second);
        b.refill_ns = now;
    }
    if (it->second.tokens < 1.0) return false;
    it->second.tokens -= 1.0;
    return true;
}

void HostStreamServer::handleClientFrame(TlsConnection& conn, MsgType type, std::vector<uint8_t>& payload) {
    if (opts_.client_frame_hook_for_test) opts_.client_frame_hook_for_test(type);
    // The JSON is parsed here, before the server's lock: a device's frame (at most 64 KiB, within the
    // parser's caps) never holds up the fan-out or another session while it is decoded.
    const uint64_t recv_ns = monotonicNs();
    std::optional<PingMessage> ping_msg;
    std::optional<SelectModeMessage> select_msg;
    std::optional<RemoteControlCommandMessage> remote_msg;
    std::optional<Orbit3DGestureMessage> orbit_msg;
    std::optional<SensorPowerControlMessage> power_msg;
    std::optional<PetCameraCommandMessage> pet_msg;
    std::optional<LiveViewRequestMessage> live_msg;
    std::optional<RollListRequestMessage> list_msg;
    std::optional<RollThumbRequestMessage> thumb_msg;
    std::optional<PlaybackRequestMessage> playback_msg;
    auto decode = [&](auto& out) {
        typename std::decay_t<decltype(out)>::value_type m;
        if (fromJson(payload, m)) out = std::move(m);
    };
    switch (type) {
        case MsgType::Ping: decode(ping_msg); break;
        case MsgType::SelectModeReq: decode(select_msg); break;
        case MsgType::RemoteControlCmd: decode(remote_msg); break;
        case MsgType::Orbit3DGesture: decode(orbit_msg); break;
        case MsgType::SensorPowerReq: decode(power_msg); break;
        case MsgType::PetCameraCmd: decode(pet_msg); break;
        case MsgType::LiveViewReq: decode(live_msg); break;
        case MsgType::RollListReq: decode(list_msg); break;
        case MsgType::RollThumbReq: decode(thumb_msg); break;
        case MsgType::PlaybackReq: decode(playback_msg); break;
        default: break;
    }
    // Decided under the lock; the callbacks out to the app and the state-file writes run after it is
    // released.
    std::function<void()> after;
    std::function<void()> roll_task; // the roll's file work, after the lock (handleRoll*Locked)
    bool pet_changed = false;
    bool save_due = false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        Session* sp = findSessionLocked(conn);
        if (!sp) {
            // A notice-only connection's v3 ping gets the notice; nothing else is read from it. A
            // replaced or closing connection no longer speaks for its device.
            for (auto& p : pending_) {
                if (p->conn.get() != &conn || !p->notice_only) continue;
                if (ping_msg) {
                    if (ping_msg->protocolVersion && *ping_msg->protocolVersion >= kProtocolV3) {
                        conn.send(packFrame(MsgType::SessionNotice, 0,
                                            toJson(makeSessionNotice(notice_reason::kTooManySessions))));
                        conn.closeAfterFlush();
                        p->notice_only = false;
                        p->deadline_ns = monotonicNs() + kNotifiedCloseGraceNs;
                    } else {
                        conn.close();
                    }
                }
                break;
            }
            return;
        }
        Session& s = *sp;
        const PetRole role = roleOfLocked(s);
        const bool owner = role == PetRole::Owner;
        switch (type) {
            case MsgType::Ping: {
                if (!ping_msg) return;
                const PingMessage& ping = *ping_msg;
                const bool first = !s.pinged;
                s.pinged = true;
                if (first && s.replaces_on_hello) {
                    // Its hello: now it replaces the device's older sessions (v3 §2.1).
                    s.replaces_on_hello = false;
                    std::vector<std::shared_ptr<Session>> old;
                    for (auto& o : sessions_) {
                        if (o.get() != &s && o->hash == s.hash) old.push_back(o);
                    }
                    for (auto& o : old) dropSessionLocked(*o, notice_reason::kReplaced);
                }
                bool raised = false;
                if (ping.protocolVersion && *ping.protocolVersion >= 2 && s.version < 2) {
                    // v3 §1 rule 1: raised once, to min(ping, 3); never changes after that.
                    s.version = std::min(*ping.protocolVersion, kHostProtocolVersion);
                    raised = true;
                    touchDeviceLocked(s);
                    if (s.version >= 3) s.force_0b = true; // 0x0B straight after becoming v3
                }
                if (first || raised) {
                    reevaluateLocked(); // the first 0x07 (and 0x0B); a v1 phone unavailable at its first ping is closed
                    pet_changed = true;
                }
                if (!findSessionLocked(conn)) break; // closed just now
                if (ping.lastHostToDisplayLatencyUs > 0) last_display_latency_ms_ = ping.lastHostToDisplayLatencyUs / 1000.0;
                if (ping.lastBarcodeLatencyUs > 0) last_barcode_latency_ms_ = ping.lastBarcodeLatencyUs / 1000.0;
                PongMessage pong;
                pong.clientSendNs = ping.clientSendNs;
                pong.serverRecvNs = recv_ns;
                pong.serverSendNs = monotonicNs();
                conn.send(packFrame(MsgType::Pong, 0, toJson(pong)));
                break;
            }
            case MsgType::SelectModeReq: {
                if (!owner || !state_.allowPhoneModeSwitch || !select_msg) return;
                const SelectModeMessage req = *select_msg;
                if (onClientRequestedMode) after = [this, req] { onClientRequestedMode(req.modeRaw, req.sbsLayoutRaw); };
                break;
            }
            case MsgType::RemoteControlCmd: {
                if (!remote_msg) return;
                if (!takeCommandTokenLocked(s)) {
                    ++commands_dropped_; // past the bucket (v3 §6.2): dropped, as 0x08 has no answer
                    return;
                }
                const RemoteControlCommandMessage cmd = *remote_msg;
                if (cmd.action == RemoteControlAction::RequestKeyframe) {
                    // Any role: this session gets its tier's 0x02 and the next IDR (coalesced per tier).
                    if (s.watching) {
                        s.needs_config = true; // the same epoch again (epochShownLocked), with the next IDR
                        if (live_) live_->requestTierKeyframe(s.tier);
                    }
                } else if (!owner || !state_.allowPhoneModeSwitch) {
                    return; // v3 §2.4: a viewer's commands are ignored; an owner's need mode switching
                }
                if (onClientRemoteCommand) after = [this, cmd] { onClientRemoteCommand(cmd); };
                break;
            }
            case MsgType::Orbit3DGesture: {
                if (!owner || !state_.allowPhoneModeSwitch || !orbit_msg) return;
                const Orbit3DGestureMessage orbit = *orbit_msg;
                if (onClientOrbit3DGesture) after = [this, orbit] { onClientOrbit3DGesture(orbit); };
                break;
            }
            case MsgType::SensorPowerReq: {
                if (!power_msg) return;
                const SensorPowerControlMessage req = *power_msg;
                // v3 §3.1, §6.4: an owner may power the sensor whenever the PC allows phone power, also
                // while it is unavailable (privacy mode, or four owners on the slots); its 0x0B already
                // says why it gets no video. An unavailable session below v3 sees the "off" presentation,
                // whose 0x07 disables the switch with the reason, so it is refused. (v1 has no 0x0A; a v2
                // phone is version 1 here until its first ping, and is held to the v2 rule.)
                if (!owner || !state_.allowPhoneSensorPowerControl || (s.version < kProtocolV3 && !s.available)) {
                    // Refused: the device gets its state, which says why the switch is disabled.
                    sendHostStateLocked(s);
                    return;
                }
                if (onClientSensorPowerRequest) after = [this, req] { onClientSensorPowerRequest(req); };
                break;
            }
            case MsgType::PetCameraCmd: {
                if (s.version < kProtocolV3) return; // v3 §1 rule 3: v3 types from older sessions are ignored
                if (!pet_msg) return; // malformed: not answered (§5.10)
                // Written below, outside the lock, only when the command changed something that is saved: a
                // refused, unsupported or no-op command must not force a write past the retry's back-off.
                const uint64_t gens = devices_gen_ + pairing_gen_ + settings_gen_ + privacy_marker_gen_;
                if (isRollAction(pet_msg->action)) roll_task = handleRollCommandLocked(s, *pet_msg);
                else handlePetCommandLocked(s, *pet_msg);
                pet_changed = true;
                save_due = devices_gen_ + pairing_gen_ + settings_gen_ + privacy_marker_gen_ != gens;
                break;
            }
            case MsgType::LiveViewReq: {
                if (s.version < kProtocolV3) return;
                if (!live_msg) return;
                const LiveViewRequestMessage& req = *live_msg;
                s.wants_live = req.watch;
                if (req.quality) {
                    const LiveQuality q = liveQualityFromWire(*req.quality);
                    if (q != LiveQuality::Unknown) { // an unknown value is ignored (§1 rule 6)
                        if (q == LiveQuality::Auto && s.quality != LiveQuality::Auto) {
                            s.auto_tier = s.path == PetPath::Outside ? LiveTier::Medium : LiveTier::High;
                            s.drops_ns.clear();
                            s.last_step_ns = monotonicNs();
                        }
                        s.quality = q;
                    }
                }
                s.force_0b = true; // the answer is always the next 0x0B, even when nothing changed
                reevaluateLocked();
                pet_changed = true;
                break;
            }
            // The camera roll: v3 types, read only from a v3 session (§1 rule 3), and a malformed one is
            // ignored without an answer (§5.10).
            case MsgType::RollListReq: {
                if (s.version < kProtocolV3 || !list_msg) return;
                roll_task = handleRollListLocked(s, *list_msg);
                break;
            }
            case MsgType::RollThumbReq: {
                if (s.version < kProtocolV3 || !thumb_msg) return;
                roll_task = handleRollThumbLocked(s, *thumb_msg);
                break;
            }
            case MsgType::PlaybackReq: {
                if (s.version < kProtocolV3 || !playback_msg) return;
                roll_task = handlePlaybackLocked(s, *playback_msg);
                pet_changed = true; // "Watching now" (playback) on the PC
                break;
            }
            default:
                break; // host -> device types, and nothing else exists: no recording download or file commands
        }
    }
    if (save_due) flushSaves(FlushKind::Change);
    // An owner's set_sharing off may have closed the outside listener: its listener and pool stop here, outside m_.
    if (type == MsgType::PetCameraCmd) joinRetiredOutside();
    if (roll_task) roll_task();
    // A revoke from this device's command leaves its star drop to the maintenance thread, never this one.
    if (!star_drops_.empty()) maintenance_cv_.notify_all();
    if (after) after();
    if (pet_changed) notifyPetStateChanged();
}

void HostStreamServer::handlePetCommandLocked(Session& s, const PetCameraCommandMessage& cmd) {
    auto answer = [&](bool ok, const char* error) {
        CommandResultMessage r;
        r.requestId = cmd.requestId;
        r.ok = ok;
        if (error) {
            r.error = std::string(error);
            if (const char* sentence = text::sentenceFor(error)) r.message = std::string(sentence);
        }
        s.conn->send(packFrame(MsgType::CommandResult, 0, toJson(r)));
    };
    const bool owner = roleOfLocked(s) == PetRole::Owner;
    const std::string& a = cmd.action;
    if (a == pet_action::kSetSharing || a == pet_action::kSetPrivacy) {
        if (!owner) return answer(false, pet_error::kNotAllowedForRole);
        if (!cmd.enabled) return answer(false, pet_error::kInvalid);
        if (settings_unread_) {
            // pet-camera.json couldn't be read: privacy is on (fail closed), and a change that would have to
            // be written over the unread file is refused. Privacy on is already so, and wins after the read.
            if (a == pet_action::kSetPrivacy && *cmd.enabled) {
                answer(true, nullptr); // privacy is on in memory already, whether or not the marker is written
                privacy_on_asked_while_unread_ = true;
                markPrivacyPendingLocked(); // durable across a restart (written by the caller's flush), unless already so
                return;
            }
            return answer(false, pet_error::kInternal);
        }
        answer(true, nullptr); // the answer goes out before any effect (v3 §5.2)
        if (a == pet_action::kSetSharing) {
            if (settings_.sharingEnabled == *cmd.enabled) return;
            settings_.sharingEnabled = *cmd.enabled;
            if (!*cmd.enabled) {
                // v3 §5.2: off also turns outside listening off and clears the setting (D9: only the PC turns it
                // on again). The 0x0D above is already queued, so an owner outside the home reads it before its
                // own 0x0F outside_disabled.
                settings_.allowOutside = false;
                settings_.outsideMappingRemoved = false; // off, so the next turn-on maps again
                applyOutsideLocked();
            }
        } else {
            if (settings_.privacyMode == *cmd.enabled) return;
            settings_.privacyMode = *cmd.enabled;
        }
        markSettingsDirtyLocked(); // in force already; a failed write is shown on the PC and retried
        pushRollSettingsLocked();  // v3 §6.4: privacy mode also finalizes a clip in progress and stops every trigger
        reevaluateLocked(); // the next 0x0B (and the v2 presentation) carry the change
        return;
    }
    if (a == pet_action::kRevokeDevice) {
        if (!owner) return answer(false, pet_error::kNotAllowedForRole);
        if (!cmd.deviceId) return answer(false, pet_error::kInvalid);
        // Defensive: no session is admitted while devices.json can't be read, and the unread list must not be
        // written over. No test reaches this: a phone can't be connected to send it while the list is unread.
        if (devices_unread_) return answer(false, pet_error::kInternal);
        const DeviceRecord* target = store_.findByDeviceId(*cmd.deviceId);
        if (!target) return answer(false, pet_error::kNotFound);
        if (target->certSHA256 == s.hash) return answer(false, pet_error::kInvalid); // not itself
        answer(true, nullptr);
        const std::string id = *cmd.deviceId;
        // The owner's own id, read before the list changes (a removal may move the records).
        const DeviceRecord* by = deviceOfLocked(s);
        const std::string by_id = by ? by->deviceId : deviceIdFromHash(s.hash);
        const DeviceStore::RevokeResult r = store_.revoke(id);
        if (!r.found || !r.removed) return;
        if (r.nowEmpty) state_.serviceId = randomHexId(6);
        star_drops_.push_back(r.removed->deviceId); // v3 §3.2 step 5, after the lock (handleClientFrame)
        // v3 §3.2 step 4: from the owner's session, its path and address, naming the owner's device.
        logLocked("revoked", s.path, s.addr, r.removed->deviceId, r.removed->name, "by the owner device " + by_id);
        markDevicesDirtyLocked(); // as revokeDevice: holds from memory if the write fails, and is retried
        closeSessionsOfHashLocked(r.removed->certSHA256, notice_reason::kRevoked);
        updateAdvertisementLocked();
        reevaluateLocked();
        return;
    }
    // The roll actions go to handleRollCommandLocked; anything else is unknown (§1 rule 6).
    answer(false, pet_error::kUnsupported);
}

void HostStreamServer::maintenanceLoop() {
    std::unique_lock<std::recursive_mutex> lk(m_);
    uint64_t last_auto_check = 0;
    while (!maintenance_stop_) {
        // Woken at once for a retired playback or a revoked device's stars, else every 100 ms.
        maintenance_cv_.wait_for(lk, std::chrono::milliseconds(100), [&] {
            return maintenance_stop_ || !retired_playbacks_.empty() || (!star_drops_.empty() && roll_store_);
        });
        if (maintenance_stop_) break;
        const uint64_t now = monotonicNs();
        // Dead sessions (v3 §2.2: the watchdog, per session). Closed from here; their on_closed reports them.
        const uint64_t limit = static_cast<uint64_t>(std::max(100, opts_.client_ping_timeout_ms)) * kMs;
        for (auto& s : sessions_) {
            if (now > s->last_activity_ns && now - s->last_activity_ns > limit) {
                s->conn->close();
                s->last_activity_ns = now; // one close per silence, not one per tick
            }
        }
        // Notice-only connections without a ping, and notified ones that didn't flush in time.
        for (auto& p : pending_) {
            if (now >= p->deadline_ns) p->conn->close();
        }
        // A pairing connection that hasn't sent its request by its deadline.
        for (auto& pc : pairing_conns_) {
            if (pc->spoke || pc->deadline_ns == 0 || now < pc->deadline_ns) continue;
            if (!pc->closed_by_host) pc->closed_by_host = "no pairRequest in time"; // the first reason
            if (auto c = pc->conn.lock()) c->close();
            pc->deadline_ns = 0; // one close each
        }
        // §8.6: the connection log is written (at most every flush interval) and pruned daily, outside m_.
        if (log_) {
            const bool prune = now - last_log_prune_ns_ >= kLogPruneEveryNs;
            if (prune) last_log_prune_ns_ = now;
            lk.unlock();
            drainLog();
            if (prune) log_->prune(unixMsNow());
            log_->flushIfDue(now / kMs, unixMsNow());
            lk.lock();
            if (maintenance_stop_) break;
        }
        // Playback streams that ended: their threads are joined here, outside the lock.
        if (!retired_playbacks_.empty()) {
            lk.unlock();
            joinRetiredPlaybacks();
            lk.lock();
            continue;
        }
        // Stars of revoked devices, and of devices that are no longer listed (v3 §3.2), once the list has been read.
        // Only here, on the maintenance thread, so the full scan under roll_star_m_ never runs on the UI thread or
        // a device's connection thread. A revoke still wins: a revoked device's star_clip is refused (its device is gone
        // from the list, checked under roll_star_m_), and one that landed before the revoke is dropped here.
        if ((!star_drops_.empty() && roll_store_) || (star_reconcile_due_ && !devices_unread_ && roll_store_)) {
            lk.unlock();
            dropRevokedStars();
            lk.lock();
            continue;
        }
        // A device list that couldn't be read at start: read again, backing off.
        // pet-camera.json that couldn't be read at start: read again, backing off (io_m_ then m_, as there).
        if (settings_unread_ && now >= next_settings_read_ns_) {
            lk.unlock();
            bool changed = false;
            {
                std::lock_guard<std::mutex> io(io_m_);
                std::lock_guard<std::recursive_mutex> lk2(m_);
                if (settings_unread_ && !maintenance_stop_) {
                    const std::string before = settings_load_error_;
                    changed = retrySettingsReadLocked(monotonicNs()) || settings_load_error_ != before;
                }
            }
            if (changed) notifyPetStateChanged(); // the panel and the status line
            lk.lock();
            continue;
        }
        if (devices_unread_ && now >= next_read_retry_ns_) {
            // Under io_m_ as well, taken before m_ (the order every write takes them in): the reconcile may
            // write, and no other write may run beside it.
            lk.unlock();
            bool loaded = false, changed = false;
            {
                std::lock_guard<std::mutex> io(io_m_);
                std::lock_guard<std::recursive_mutex> lk2(m_);
                if (devices_unread_ && !maintenance_stop_) {
                    const std::string before = devices_read_error_;
                    loaded = retryDeviceReadLocked(monotonicNs());
                    changed = loaded || devices_read_error_ != before;
                }
            }
            if (loaded) notifyPairingChanged();
            if (changed) notifyPetStateChanged(); // the PC's list and status line
            lk.lock();
            continue;
        }
        // The sharing schedule (pet-camera.md §4), evaluated every second.
        if (now - last_schedule_check_ns_ >= 1000ULL * kMs) {
            last_schedule_check_ns_ = now;
            if (scheduleOffNowLocked() != last_schedule_off_) {
                reevaluateLocked();
                lk.unlock();
                notifyPetStateChanged();
                lk.lock();
                continue;
            }
        }
        // auto quality steps back up after 30 s without a drop (v3 §6.1).
        if (now - last_auto_check >= 1000ULL * kMs) {
            last_auto_check = now;
            bool changed = false;
            for (auto& s : sessions_) {
                if (s->version < 3 || s->quality != LiveQuality::Auto || s->auto_tier == LiveTier::High) continue;
                const uint64_t quiet_since = std::max(s->last_drop_ns, s->last_step_ns);
                if (now - quiet_since < kAutoStepUpNs) continue;
                s->auto_tier = s->auto_tier == LiveTier::Low ? LiveTier::Medium : LiveTier::High;
                s->last_step_ns = now;
                changed = true;
            }
            if (changed) reevaluateLocked();
        }
        // A save that failed is retried, whether or not anything changed since: memory already holds the
        // change (fail closed), and the disk must catch up before a restart could read it. The retry backs
        // off while it keeps failing (100 ms doubling to 5 s, flushSaves), and it is written outside the
        // lock, so a file that can't be written never holds up the video. Not in the crash test mode, which
        // stands for a host that died after its failed write.
        if (!crash_test_ &&
            (devices_dirty_ || pairing_dirty_ || settings_dirty_ || privacy_marker_write_due_ || privacy_marker_delete_due_) &&
            now >= next_save_retry_ns_) {
            lk.unlock();
            const bool changed = flushSaves(FlushKind::Retry);
            if (changed) notifyPetStateChanged(); // the PC's status line
            lk.lock();
            continue;
        }
        // Last-seen flush, at most every 10 minutes (pet-camera.md §5). While a full save is pending the
        // retry above carries the touches too.
        if (!devices_dirty_ && !devices_unread_ && store_.hasUnsavedTouches() && now - last_flush_ns_ >= kTouchFlushNs) {
            last_flush_ns_ = now;
            lk.unlock();
            const bool changed = flushSaves(FlushKind::TouchFlush); // a failure is shown and retried from now
            if (changed) notifyPetStateChanged();
            lk.lock();
        }
    }
}

bool HostStreamServer::retrySettingsReadLocked(uint64_t now_ns) {
    bool existed = false, unread = false;
    std::string err;
    PetSettings s = loadPetSettings(opts_.state_dir, &existed, &err, &unread, opts_.address_policy_for_test);
    if (unread) {
        settings_load_error_ = err;
        settings_read_retry_ms_ = std::min(settings_read_retry_ms_ * 2, kMaxRetryMs);
        next_settings_read_ns_ = now_ns + settings_read_retry_ms_ * kMs;
        return false;
    }
    if (!existed) {
        // It was there at start and is gone now: nothing is known of it, so privacy stays on.
        s = PetSettings{};
        s.privacyMode = true;
        err = "pet-camera.json went missing; privacy mode is on until the settings are saved again";
    }
    // Read (or kept as .bad): its values are in force from now. A privacy-on asked for meanwhile wins, and is
    // then saved; nothing else changed since start (every other change was refused). So does privacy-on.pending
    // (or one whose existence can't be checked): privacy is on whatever the file says. The next successful
    // write of the file (privacy on, or a later off the owner chose) then removes the marker.
    if (fileState(opts_.state_dir + L"\\" + kPrivacyMarkerFile) != FileState::Missing) privacy_marker_may_exist_ = true;
    const bool privacy_wins = privacy_on_asked_while_unread_ || privacy_marker_may_exist_ || privacy_marker_write_due_;
    if (privacy_wins) s.privacyMode = true;
    settings_ = std::move(s);
    settings_unread_ = false;
    // Read and parsed, and holding the roll's limits: they are the owner's. Otherwise unknown.
    if (!existed || !err.empty()) settings_.rollLimitsKnown = false;
    privacy_on_asked_while_unread_ = false;
    settings_load_error_ = err;
    settings_read_retry_ms_ = kFirstRetryMs;
    if (privacy_wins) markSettingsDirtyLocked(); // the next retry tick writes it
    if (!outside::isValidOutsidePort(settings_.outsidePort)) {
        settings_.outsidePort = pickOutsidePortLocked(); // v3 §8.7: one that binds, or none (nothing saved)
        if (outside::isValidOutsidePort(settings_.outsidePort) && settings_load_error_.empty()) {
            markSettingsDirtyLocked(); // picked once and saved (v3 §8.1)
        }
    }
    pushRollSettingsLocked();
    applyOutsideLocked(); // read at last: outside listening as the file says
    reevaluateLocked();
    return true;
}

bool HostStreamServer::retryDeviceReadLocked(uint64_t now_ns) {
    // If the list is read now, the roll's stars are reconciled with it (dropRevokedStars checks it was read).
    star_reconcile_due_ = star_reconcile_due_ || roll_prepared_;
    DeviceStore fresh(opts_.state_dir);
    fresh.setCrashBetweenWritesForTest(crash_test_);
    HostPersistedState pairing = state_;
    const auto rec = fresh.loadAndReconcile(pairing, unixMsNow());
    reconcile_log_.insert(reconcile_log_.end(), rec.log.begin(), rec.log.end());
    if (rec.readFailed) {
        devices_read_error_ = rec.readError;
        read_retry_ms_ = std::min(read_retry_ms_ * 2, kMaxRetryMs);
        next_read_retry_ns_ = now_ns + read_retry_ms_ * kMs;
        return false;
    }
    // Read: the list (reconciled with pairing.json, as at a start) is in force from now.
    store_ = std::move(fresh);
    state_ = pairing;
    devices_unread_ = false;
    devices_rebuilt_ = rec.which == DeviceStore::ReconcileCase::Migrated;
    devices_read_error_.clear();
    read_retry_ms_ = kFirstRetryMs;
    devices_dirty_ = !rec.saveError.empty();
    devices_save_error_ = rec.saveError;
    updateAdvertisementLocked();
    reevaluateLocked();
    return true;
}

// ---- pairing ------------------------------------------------------------------------------------------

bool HostStreamServer::beginPairingWindow(int duration_sec, ActivePairingOffer& out, std::string* err, PetRole role,
                                          const std::optional<std::string>& rePairDeviceId) {
    std::vector<std::unique_ptr<TcpListener>> retired;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!started_) {
            if (err) *err = "server not started";
            return false;
        }
        if (role != PetRole::Owner && role != PetRole::Viewer) {
            if (err) *err = "a pairing needs the owner or viewer role";
            return false;
        }
        if (devices_unread_) {
            // Fail closed: a pairing would be saved over the list on disk, which may hold other devices.
            if (err) *err = "the paired-device list couldn't be read (" + devices_read_error_ + "); no device can pair until it can";
            return false;
        }
        if (rePairDeviceId && !store_.findByDeviceId(*rePairDeviceId)) {
            if (err) *err = "no such device to re-pair";
            return false;
        }
        closePairingWindowLocked();
        retired.swap(retired_listeners_);

        ActivePairingOffer offer;
        offer.role = role;
        offer.rePairDeviceId = rePairDeviceId;
        offer.qrToken.assign(32, 0);
        if (!randomBytes(offer.qrToken.data(), offer.qrToken.size()) ||
            !generateShortCode(offer.shortCodeFormatted, offer.shortCodeNormalized)) {
            if (err) *err = "random generation failed";
            return false;
        }
        auto listener = std::make_unique<TcpListener>();
        if (!listener->start(0, opts_.loopback_only || opts_.lan_listeners_loopback_for_test, [this](SOCKET s, const sockaddr* peer) { handlePairingSocket(s, peer); },
                             err)) {
            return false;
        }
        offer.port = listener->port();
        offer.hostIP = opts_.loopback_only ? "127.0.0.1" : primaryLanIPv4();
        QRPairingPayload qr;
        qr.serviceId = state_.serviceId;
        qr.serverCertSHA256 = identity_->certificateSHA256();
        qr.oneTimeToken = offer.qrToken;
        qr.hostIP = offer.hostIP;
        qr.pairPort = offer.port;
        offer.qrURI = qr.toURIString();
        offer.expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
        pairing_listener_ = std::move(listener);
        offer_ = offer;
        last_pairing_refusal_.clear();
        if (opts_.advertise) {
            std::string aerr;
            if (pairing_ad_.start(state_.serviceId, kPairingServiceType, offer.port, offer.hostIP, &aerr)) {
                ad_status_ = "advertising _kv2p._tcp (pairing)";
            } else {
                ad_status_ = "pairing mDNS failed: " + aerr;
            }
        }
        out = offer;
        expiry_cv_.notify_all();
    }
    for (auto& l : retired) l->stop();
    notifyPairingChanged();
    return true;
}

void HostStreamServer::cancelPairingWindow() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        closePairingWindowLocked();
    }
    notifyPairingChanged();
}

bool HostStreamServer::setPairingRole(PetRole role) {
    if (role != PetRole::Owner && role != PetRole::Viewer) return false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!offer_ || offer_->rePairDeviceId) return false;
        offer_->role = role; // read when the pairRequest arrives (processPairRequest copies the offer)
    }
    notifyPairingChanged();
    return true;
}

bool HostStreamServer::allowShortCodeForPairing() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!offer_ || std::chrono::steady_clock::now() >= offer_->expiresAt) return false;
        offer_->shortCodeAllowed = true;
    }
    notifyPairingChanged();
    return true;
}

std::optional<ActivePairingOffer> HostStreamServer::currentPairingOffer() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return offer_;
}

void HostStreamServer::closePairingWindowLocked() {
    offer_.reset();
    // A connection from this window that hasn't sent its request is closed with it, so it can't speak into a
    // later window. One that has spoken is being answered and finishes on its own.
    for (auto& pc : pairing_conns_) {
        if (pc->spoke) continue;
        if (!pc->closed_by_host) pc->closed_by_host = "the window closed before its request came"; // the first reason
        if (auto c = pc->conn.lock()) c->close();
    }
    pairing_ad_.stop();
    if (pairing_listener_) {
        pairing_listener_->requestStop();
        retired_listeners_.push_back(std::move(pairing_listener_));
    }
    if (opts_.advertise) {
        ad_status_ = !store_.empty() ? (stream_ad_.active() ? "advertising _kv2s._tcp" : ad_status_) : "not advertised (unpaired)";
    }
}

void HostStreamServer::expiryLoop() {
    std::unique_lock<std::recursive_mutex> lk(m_);
    while (!expiry_stop_) {
        if (offer_) {
            const auto deadline = offer_->expiresAt;
            expiry_cv_.wait_until(lk, deadline);
            if (!expiry_stop_ && offer_ && std::chrono::steady_clock::now() >= offer_->expiresAt) {
                closePairingWindowLocked();
                lk.unlock();
                notifyPairingChanged();
                lk.lock();
            }
        } else {
            expiry_cv_.wait(lk);
        }
    }
}

void HostStreamServer::handlePairingSocket(SOCKET s, const sockaddr* peer) {
    const outside::IpAddress src = sourceOf(peer, "pairing");
    const bool local = isLocal(src); // before m_: it may read the adapters
    std::unique_lock<std::recursive_mutex> lk(m_);
    if (shutting_down_ || !offer_) {
        closesocket(s);
        return;
    }
    // v3 §8.1: the pairing window takes local sources only, closed before TLS (pairing over the internet
    // happens only through an invite, on the outside listener).
    if (!local) {
        closesocket(s);
        logLocked("refused_local_only", PetPath::Home, outside::toText(src));
        lk.unlock();
        drainLog(); // §8.6, with m_ released
        return;
    }
    const std::string addr = outside::toText(src);
    // Pairing connections open at once, capped in all and per source; each must send its request within
    // pairing_frame_timeout_ms of its handshake, and one that hasn't is closed when the window closes.
    const std::string key = addr; // the full address, as on the stream listener
    if (!lanAdmitLocked(pairing_open_, key, opts_.pairing_conns_total, opts_.pairing_conns_per_source)) {
        closesocket(s);
        logLocked("refused_too_many", PetPath::Home, addr);
        lk.unlock();
        drainLog();
        return;
    }
    // From here the slot and the socket are this function's until the connection takes them over: anything that throws
    // before then gives both back. `handed`: start() has the socket (it closes it itself when it can't
    // run); `released`: the slot was given back. Each happens once, whatever throws, and nothing after start() closes
    // a socket the connection owns.
    std::shared_ptr<TlsConnection> c;
    bool handed = false, released = false;
    try {
        if (opts_.after_lan_admit_for_test) opts_.after_lan_admit_for_test();
        auto pc = std::make_shared<PairingConn>();
        TlsConnection::Callbacks cb;
        cb.max_inbound_payload = kMaxClientPayload; // anyone on the LAN can reach it while the window is open
        cb.on_ready = [this, pc](TlsConnection&) {
            std::lock_guard<std::recursive_mutex> lk2(m_);
            pc->deadline_ns = monotonicNs() + static_cast<uint64_t>(std::max(100, opts_.pairing_frame_timeout_ms)) * kMs;
        };
        cb.on_frame = [this, addr, pc](TlsConnection& conn, MsgType t, uint8_t, std::vector<uint8_t>& payload) {
            // Parsed before the server's lock, like a session's frames.
            PairRequestMessage req;
            const bool parsed = t == MsgType::PairRequest && fromJson(payload, req);
            std::vector<uint8_t> accepted;
            {
                std::lock_guard<std::recursive_mutex> lk2(m_);
                pc->spoke = true;
                // §8.6: every attempt that doesn't pair is logged, as pairing_failed (the window stays open) or
                // pairing_burned (it closed). processPairRequest logs the ones it refuses.
                if (!parsed || !offer_ || std::chrono::steady_clock::now() >= offer_->expiresAt) {
                    conn.close();
                    logLocked("pairing_failed", PetPath::Home, addr, {}, {},
                              !parsed ? "not a pairRequest" : "the pairing window had closed");
                    return;
                }
                // v3 §4.4: the LAN pairing listener takes QR token (0) or short code (1) only. Anything else
                // (an invite, 2, belongs to the outside listener) is closed without checking a proof and
                // without burning the window.
                if (req.methodRaw != static_cast<uint8_t>(PairingMethod::QrToken) &&
                    req.methodRaw != static_cast<uint8_t>(PairingMethod::ShortCode)) {
                    conn.close();
                    logLocked("pairing_failed", PetPath::Home, addr, {}, {}, "a method this listener doesn't take");
                    return;
                }
                // The code only once the PC user chose "Use a code" for this window (QR only by default): before
                // that a code attempt is closed the same way, unread, and burns nothing.
                if (req.methodRaw == static_cast<uint8_t>(PairingMethod::ShortCode) && !offer_->shortCodeAllowed) {
                    conn.close();
                    logLocked("pairing_failed", PetPath::Home, addr, {}, {}, "a code, while the window takes the QR code only");
                    return;
                }
                accepted = processPairRequest(conn, req, addr);
                if (!accepted.empty()) {
                    const DeviceRecord* d = store_.findByHash(sha256(req.clientCertDER));
                    logLocked("paired", PetPath::Home, addr, d ? d->deviceId : std::string(), d ? d->name : std::string());
                }
            }
            if (accepted.empty()) return;
            // devices.json first, then pairing.json with the mirror (pet-camera.md §5), written outside the
            // lock and before the phone is told. A failed write doesn't undo the pairing (the device is
            // listed in memory and the phone is told so); the PC shows the error and the retry (or
            // shutdown) writes it once the disk takes it.
            flushSaves(FlushKind::Change);
            conn.send(std::move(accepted));
            conn.closeAfterFlush();
        };
        cb.on_closed = [this, key, addr, pc](TlsConnection& conn, const std::string& why) {
            {
                std::lock_guard<std::recursive_mutex> lk2(m_);
                conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [&](const auto& c) { return c.get() == &conn; }), conns_.end());
                pairing_conns_.erase(std::remove(pairing_conns_.begin(), pairing_conns_.end(), pc), pairing_conns_.end());
                lanReleaseLocked(pairing_open_, key);
                // §8.6: an attempt that never sent its request is logged too, as pairing_failed (it burns nothing),
                // with how it ended: the window closed, its deadline passed, or the connection itself ended (a peer
                // that closed, a handshake that failed). One that spoke was logged when it was answered.
                if (!pc->spoke) {
                    logLocked("pairing_failed", PetPath::Home, addr, {}, {},
                              pc->closed_by_host ? std::string(pc->closed_by_host) : "closed before its request: " + why);
                }
            }
            drainLog(); // with m_ released
        };
        handed = true;
        c = TlsConnection::start(s, pairing_ctx_, true, std::move(cb));
        if (!c) {
            lanReleaseLocked(pairing_open_, key); // no thread for it: that connection only is turned away
            released = true;
            logLocked("refused_unavailable", PetPath::Home, addr);
            lk.unlock();
            drainLog();
            return;
        }
        pc->conn = c;
        if (opts_.after_lan_start_for_test) opts_.after_lan_start_for_test();
        pairing_conns_.push_back(pc);
        conns_.push_back(c);
    } catch (...) {
        if (!c) {
            // Turned away. The socket is closed here only if start() never had it; the slot is given back once. The
            // server's lock may already be let go (a throw from drainLog after it): then nothing more is done here.
            if (!handed) closesocket(s);
            if (lk.owns_lock()) {
                if (!released) lanReleaseLocked(pairing_open_, key);
                try {
                    logLocked("refused_unavailable", PetPath::Home, addr);
                } catch (...) {
                }
            }
        } else {
            // It runs, but it may not be in the lists: closed now, and its on_closed gives the slot back.
            c->close();
        }
        if (lk.owns_lock()) lk.unlock();
        drainLog();
    }
}

bool HostStreamServer::lanAdmitLocked(LanCount& c, const std::string& key, int total, int perSource) {
    int& mine = c.bySource[key];
    if (c.total >= total || mine >= perSource) {
        if (mine == 0) c.bySource.erase(key);
        return false;
    }
    ++mine;
    ++c.total;
    return true;
}

void HostStreamServer::lanReleaseLocked(LanCount& c, const std::string& key) {
    auto it = c.bySource.find(key);
    if (it == c.bySource.end()) return;
    if (--it->second <= 0) c.bySource.erase(it);
    if (c.total > 0) --c.total;
}

std::vector<uint8_t> HostStreamServer::processPairRequest(TlsConnection& conn, const PairRequestMessage& req,
                                                          const std::string& addr) {
    // m_ is held. Use the TLS-authenticated leaf, never what the request claims: the pairing context requires a
    // client certificate, so an empty one means it couldn't be read back (out of memory). That fails closed, closed
    // unread like an attempt the window doesn't take, and burns nothing (it isn't the device's doing).
    const Bytes verified = conn.peerCertDer();
    if (verified.empty()) {
        conn.close();
        logLocked("pairing_failed", PetPath::Home, addr, {}, {}, "its TLS certificate couldn't be read");
        return {};
    }
    if (verified != req.clientCertDER) {
        closePairingWindowLocked();
        conn.close();
        logLocked("pairing_burned", PetPath::Home, addr, {}, {}, "the TLS certificate isn't clientCertDER");
        notifyPairingChanged();
        return {};
    }
    const ActivePairingOffer offer = *offer_;
    const Bytes client_hash = sha256(verified);
    const Bytes secret = (req.methodRaw == static_cast<uint8_t>(PairingMethod::QrToken))
                             ? offer.qrToken
                             : Bytes(offer.shortCodeNormalized.begin(), offer.shortCodeNormalized.end());
    const Bytes expected = computePairingProof(secret, identity_->certificateSHA256(), client_hash, "kv2-pair-client");
    // Every refusal closes the window. The log says pairing_burned for a wrong proof, and pairing_failed for one
    // refused after its proof checked (the window closes then too).
    auto refuse = [&](const std::string& message, const char* event, const char* detail) {
        PairResponseMessage reject; // today's shape: no v3 keys on a refusal
        reject.message = message;
        conn.send(packFrame(MsgType::PairResponse, 0, toJson(reject)));
        conn.closeAfterFlush();
        last_pairing_refusal_ = message;
        closePairingWindowLocked();
        logLocked(event, PetPath::Home, addr, {}, {}, detail);
        notifyPairingChanged();
    };
    if (!constantTimeEqual(req.clientProofHMAC, expected)) {
        // Single attempt: a wrong guess burns it.
        refuse("Invalid pairing code or token; pairing window closed.", "pairing_burned", "wrong proof");
        return {};
    }
    const uint64_t now_ms = unixMsNow();
    const DeviceRecord* rec = nullptr;
    if (offer.rePairDeviceId) {
        // Re-pair: that record gets this certificate and keeps its name and role (pet-camera.md §3).
        const DeviceRecord* before = store_.findByDeviceId(*offer.rePairDeviceId);
        const Bytes old_hash = before ? before->certSHA256 : Bytes();
        const auto r = store_.rePair(*offer.rePairDeviceId, client_hash, now_ms, req.protocolVersion, req.deviceKind);
        if (r != DeviceStore::RePairResult::Replaced) {
            const bool in_use = r == DeviceStore::RePairResult::HashInUse;
            refuse(in_use ? "This device is already paired as another device."
                          : "The device to re-pair is no longer on the list.",
                   "pairing_failed",
                   in_use ? "already paired as another device" : "the device to re-pair is no longer on the list");
            return {};
        }
        if (!old_hash.empty() && old_hash != client_hash) closeSessionsOfHashLocked(old_hash, nullptr);
        rec = store_.findByHash(client_hash);
    } else {
        DeviceStore::NewDevice nd;
        nd.certSHA256 = client_hash;
        nd.role = offer.role;
        nd.deviceName = req.deviceName;
        nd.deviceKind = req.deviceKind;
        nd.protocolVersion = req.protocolVersion;
        nd.nowUnixMs = now_ms;
        const auto r = store_.addPairedDevice(nd, &rec);
        if (r == DeviceStore::AddResult::TooManyDevices) {
            refuse(text::kTooManyPairedDevices, "pairing_failed", "16 devices are paired"); // v3 §2.1: a 17th pairing
            return {};
        }
        if (r == DeviceStore::AddResult::Invalid || !rec) {
            refuse("Pairing failed.", "pairing_failed", "the device couldn't be added");
            return {};
        }
        rec = store_.findByHash(client_hash); // AlreadyPaired keeps its name and role
    }
    markDevicesDirtyLocked(); // written by the caller, outside the lock, before the response goes
    updateAdvertisementLocked();
    PairResponseMessage resp;
    resp.accepted = true;
    resp.serviceId = state_.serviceId;
    resp.streamPort = stream_port_;
    resp.serverProofHMAC = computePairingProof(secret, identity_->certificateSHA256(), client_hash, "kv2-pair-server");
    resp.message = "Paired";
    if (rec) {
        // v3 §4.4: on acceptance only; old phones ignore the extra keys.
        resp.role = std::string(petRoleString(rec->role));
        resp.deviceId = rec->deviceId;
        resp.deviceName = rec->name;
        resp.protocolVersion = kProtocolV3;
    }
    ++pairings_completed_;
    last_pairing_refusal_.clear();
    closePairingWindowLocked();
    reevaluateLocked(); // owners' device lists
    notifyPairingChanged();
    notifyPetStateChanged();
    return packFrame(MsgType::PairResponse, 0, toJson(resp));
}

// ---- outside the home (v3 §8) --------------------------------------------------------------------------

namespace {

constexpr uint32_t kRedeemMaxPayload = 16u * 1024u; // §8.1: the one redemption frame, at most 16 KiB
// Beside the limiter's global 8: workers for a recently paired source's own slots (§8.5's reserve), so a
// stranger's handshakes never queue the owner's phone. The pool never grows past this.
constexpr int kReserveHandshakeWorkers = 4;
constexpr uint64_t kOnLinkRefreshNs = 30000ULL * kMs;
constexpr uint64_t kHomeRecheckNs = 2000ULL * kMs; // a source the list doesn't hold reads it again, at most this often
constexpr int kRedeemReplyWaitMs = 2000;
constexpr size_t kLogQueueMax = 10000; // connection log lines waiting for drainLog

std::string hexOf(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

} // namespace

outside::IpAddress HostStreamServer::sourceOf(const sockaddr* peer, const char* listener) const {
    outside::IpAddress a; // 0.0.0.0: an address that can't be read is never local, and is its own source
    if (peer) {
        if (const auto p = outside::fromSockaddr(peer)) a = outside::unwrapMapped(*p); // §8.0: unwrap first
    }
    if (opts_.test_source_address) {
        const std::string over = opts_.test_source_address(listener, outside::toText(a));
        if (!over.empty()) {
            if (const auto p = outside::parseIp(over)) a = outside::unwrapMapped(*p);
        }
    }
    return a;
}

void HostStreamServer::readHomePrefixes(uint64_t asked_ns) {
    // One read at a time. A read that finished after this one was asked for already answers it.
    std::lock_guard<std::mutex> one(home_read_m_);
    {
        std::lock_guard<std::mutex> h(home_m_);
        if (on_link_read_ && on_link_read_ns_ >= asked_ns) return;
    }
    // §8.0: the home network is the physical Ethernet and Wi-Fi adapters' prefixes. GetAdaptersAddresses and a
    // GetIfEntry2 per adapter can take tens of milliseconds, so the read runs with neither m_ nor home_m_ held: a
    // stranger's connection (one read every 2 s at most) never holds up a session's callbacks. A read that fails
    // keeps the last list, so a passing failure never turns the home phones away.
    std::optional<std::vector<outside::AdapterInfo>> adapters =
        opts_.adapters_for_test ? opts_.adapters_for_test() : outside::adaptersOfThisPc();
    std::optional<std::vector<outside::OnLinkPrefix>> prefixes;
    if (adapters) prefixes = outside::homePrefixesOf(*adapters);
    std::lock_guard<std::mutex> h(home_m_);
    const uint64_t done = monotonicNs();
    if (prefixes) {
        on_link_ = std::move(*prefixes);
        on_link_good_ns_ = done;
    } else if (done - on_link_good_ns_ >= static_cast<uint64_t>(opts_.home_list_keep_ms) * kMs) {
        // Reads have failed for longer than a list is kept: it may be another network's now. Home is loopback
        // alone until a read works again (fail closed).
        on_link_.clear();
    }
    on_link_read_ns_ = done;
    on_link_read_ = true;
}

bool HostStreamServer::isLocal(const outside::IpAddress& a) {
    if (opts_.on_link_prefixes) return outside::isLocalSource(a, *opts_.on_link_prefixes);
    if (outside::isLocalSource(a, {})) return true; // loopback needs no adapter read
    const uint64_t now = monotonicNs();
    {
        std::lock_guard<std::mutex> h(home_m_);
        if (on_link_read_ && now - on_link_read_ns_ < kOnLinkRefreshNs) {
            if (outside::isLocalSource(a, on_link_)) return true;
            // Not in a list less than 2 s old: not home. Older, it is read again now, once (the PC may just have
            // joined the network or changed its address): a stranger's connections make at most one read every 2 s.
            if (now - on_link_read_ns_ < kHomeRecheckNs) return false;
        }
    }
    readHomePrefixes(now);
    std::lock_guard<std::mutex> h(home_m_);
    return outside::isLocalSource(a, on_link_);
}

void HostStreamServer::logLocked(const char* event, PetPath path, const std::string& addr, const std::string& deviceId,
                                 const std::string& deviceName, const std::string& detail, const std::string& who) {
    if (!log_) return;
    outside::LogLine l;
    l.t = unixMsNow();
    l.event = event;
    l.path = toWire(path);
    l.addr = addr;
    l.deviceId = deviceId;
    l.deviceName = deviceName;
    l.detail = detail;
    // v3 §8.6: only queued here. ConnectionLog::record takes the log's own lock, so it is called after m_ is
    // released (drainLog), and nothing holding m_ ever waits on the log.
    std::lock_guard<std::mutex> q(log_q_m_);
    // Bounded: past this (a flood from many sources between two drains) further lines are dropped until the next
    // drain. The accept threads drain after every connection, and the maintenance loop every 100 ms.
    if (log_q_.size() >= kLogQueueMax) return;
    log_q_.emplace_back(std::move(l), who.empty() ? deviceId : who);
}

void HostStreamServer::drainLog() const {
    if (!log_) return;
    // One drainer at a time, so lines are recorded in the order they were queued (the log's time order).
    std::lock_guard<std::mutex> d(log_drain_m_);
    std::vector<std::pair<outside::LogLine, std::string>> lines;
    {
        std::lock_guard<std::mutex> q(log_q_m_);
        lines.swap(log_q_);
    }
    for (auto& l : lines) log_->record(std::move(l.first), l.second);
}

void HostStreamServer::notifyOutsideChanged() {
    if (onOutsideChanged) onOutsideChanged();
}

bool HostStreamServer::outsideWantedLocked() const {
    return outsideTurnedOnLocked() && outside::isValidOutsidePort(settings_.outsidePort);
}

bool HostStreamServer::outsideTurnedOnLocked() const {
    // §8.1: it needs "Share the camera" on; and a pet-camera.json that couldn't be read keeps it off (fail closed).
    return started_ && !shutting_down_ && !settings_unread_ && settings_.sharingEnabled && settings_.allowOutside;
}

bool HostStreamServer::outsideNoPortLocked() const {
    // Every path that turns it on (or reads it on) picks a default when there is no port, so no port here means
    // none of the tries bound. Derived from the settings, so no close or re-apply can wipe it while it holds.
    return outsideTurnedOnLocked() && !outside::isValidOutsidePort(settings_.outsidePort);
}

uint16_t HostStreamServer::pickOutsidePortLocked() const {
    if (opts_.outside_port_probe) return outside::pickBindableOutsidePort(opts_.outside_port_probe);
    // The real probe binds the address the outside listener will (openOutsideLocked): 127.0.0.1 when loopback-only
    // (every test run), otherwise [::] dual-stack. Bind only, never listen, closed at once.
    const bool loop = opts_.loopback_only || opts_.outside_loopback_only;
    return outside::pickBindableOutsidePort([loop](uint16_t port) { return tcpListenerPortBinds(port, loop); });
}

void HostStreamServer::applyOutsideLocked() {
    if (!outsideWantedLocked()) {
        if (outside_listener_ || !outside_error_.empty()) closeOutsideLocked();
        // §8.7: turned on with no port that binds: owners hear the error status too (outsideStatusLocked).
        else if (outsideNoPortLocked()) reevaluateLocked();
        return;
    }
    if (outside_listener_ && outside_port_ == settings_.outsidePort) return;
    if (outside_listener_) {
        // A new port: the listener and the mapping move. Sessions already connected stay (they learn the new
        // endpoint from 0x0B); an open invite carries the old port, so it closes.
        outside_listener_->requestStop();
        retired_listeners_.push_back(std::move(outside_listener_));
        {
            std::lock_guard<std::mutex> g(hs_live_m_);
            ++outside_gen_;
            for (auto& h : hs_live_) h->abort();
        }
        if (hs_pool_) retired_pools_.push_back(std::move(hs_pool_));
        if (mapper_) mapper_->close();
        closeInviteLocked("port_changed", "the outside port changed");
        outside_port_ = 0;
    }
    openOutsideLocked();
}

void HostStreamServer::openOutsideLocked() {
    outside_error_.clear();
    auto l = std::make_unique<TcpListener>();
    std::string err;
    const bool loop = opts_.loopback_only || opts_.outside_loopback_only;
    const uint16_t port = settings_.outsidePort;
    if (!l->start(port, loop, [this](SOCKET s, const sockaddr* p) { handleOutsideSocket(s, p); }, &err)) {
        outside_error_ = "Port " + std::to_string(port) + " can't be opened on this PC (" + err + "). Pick another port.";
        // Nothing listens now, so nothing keeps the PC awake (a port change that fails to bind lets
        // the old listener's hold go here).
        keepAwakeLocked(false);
        reevaluateLocked();
        return;
    }
    outside_listener_ = std::move(l);
    outside_port_ = outside_listener_->port();
    keepAwakeLocked(true);
    // No thread per connection: a fixed pool sized to the limiter's caps.
    hs_pool_ = std::make_unique<HandshakePool>(opts_.outside_limits.inFlightTotal + kReserveHandshakeWorkers);
    if (mapper_) {
        // A forward the PC user removed stays removed across a restart, a re-apply or a new port.
        mapper_open_id_ = mapper_->open(outside_port_, !settings_.outsideMappingRemoved, std::exchange(outside_pin_anew_, false));
        mapper_state_ = mapper_->state();
    }
    reevaluateLocked(); // outsideEnabled, and owners' outsideStatus
}

void HostStreamServer::closeOutsideLocked() {
    outside_error_.clear();
    // 1. The listener, and every handshake or redemption still running on it.
    if (outside_listener_) {
        outside_listener_->requestStop();
        retired_listeners_.push_back(std::move(outside_listener_));
    }
    outside_port_ = 0;
    {
        std::lock_guard<std::mutex> g(hs_live_m_);
        ++outside_gen_;
        for (auto& h : hs_live_) h->abort();
    }
    if (hs_pool_) retired_pools_.push_back(std::move(hs_pool_));
    // 2. Outside sessions get 0x0F outside_disabled (a v3 one; older ones are closed) and are closed.
    std::vector<std::shared_ptr<Session>> victims;
    for (auto& s : sessions_) {
        if (s->path == PetPath::Outside) victims.push_back(s);
    }
    for (auto& v : victims) dropSessionLocked(*v, notice_reason::kOutsideDisabled);
    for (auto& p : pending_) {
        if (p->path == PetPath::Outside && p->notice_only) p->conn->close();
    }
    // 3. The router mapping (on the mapper's worker; nothing waits for it).
    if (mapper_) {
        mapper_->close();
        mapper_state_ = outside::MapperState{};
    }
    // 4. Any open invite.
    closeInviteLocked("outside_off", "viewing from outside the home turned off");
    // 5. The PC may sleep again.
    keepAwakeLocked(false);
    reevaluateLocked();
}

void HostStreamServer::keepAwakeLocked(bool awake) {
    if (awake == awake_held_) return;
    awake_held_ = awake;
    if (opts_.on_outside_listening) opts_.on_outside_listening(awake);
    if (opts_.keep_awake_for_test) {
        opts_.keep_awake_for_test(awake);
        return;
    }
    if (!opts_.keep_awake) return;
    if (awake && !power_request_) {
        REASON_CONTEXT why{};
        why.Version = POWER_REQUEST_CONTEXT_VERSION;
        why.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        why.Reason.SimpleReasonString = const_cast<LPWSTR>(L"Kinect Viewer: viewing from outside the home is on");
        const HANDLE h = PowerCreateRequest(&why);
        power_request_ = h == INVALID_HANDLE_VALUE ? nullptr : h;
    }
    if (!power_request_) return; // best effort: a PC that can't be asked still serves while it is awake
    if (awake) PowerSetRequest(static_cast<HANDLE>(power_request_), PowerRequestSystemRequired);
    else PowerClearRequest(static_cast<HANDLE>(power_request_), PowerRequestSystemRequired);
}

void HostStreamServer::joinRetiredOutside() {
    std::vector<std::unique_ptr<TcpListener>> listeners;
    std::vector<std::unique_ptr<HandshakePool>> pools;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        listeners.swap(retired_listeners_);
        pools.swap(retired_pools_);
    }
    for (auto& l : listeners) l->stop();
    for (auto& p : pools) p->stop(); // their jobs were aborted: they end within a moment
}

void HostStreamServer::handleOutsideSocket(SOCKET s, const sockaddr* p) {
    const outside::IpAddress peer = sourceOf(p, "outside");
    const std::string addr = outside::toText(peer);
    // §8.5: admission at accept, before any thread or TLS work. A refused socket is
    // closed at once, with nothing read or written, and logged.
    outside::AdmitResult ar = limiter_->tryAdmit(peer);
    if (ar.refusal == outside::Refusal::TooManyInFlight && ar.globalFull) {
        // The global slots are full. A handshake that hasn't completed (a stranger connecting
        // and stalling, with or without a certificate it hasn't proved) gives its slot up, counting nothing, and this
        // one gets it, so a handful of addresses can't keep a new invitee or a phone on a fresh carrier address out.
        // Taken from the most-represented IPv6 /48 first (one site can't keep many slots), else the oldest. A
        // completed handshake is never taken, nor a recently paired source's reserved slot, nor one already ended or
        // evicted: each candidate is tried in turn until one gives its slot up.
        std::shared_ptr<ServerHandshake> victim;
        std::string vaddr;
        {
            std::lock_guard<std::mutex> g(hs_live_m_);
            struct Candidate {
                std::shared_ptr<ServerHandshake> hs;
                std::shared_ptr<outside::Admission> adm;
                const LiveHandshake* info;
            };
            std::vector<Candidate> open;
            std::map<std::string, int> perPrefix;
            for (const auto& h : hs_live_) {
                const auto it = hs_info_.find(h.get());
                if (it == hs_info_.end() || h->certified()) continue;
                auto a = it->second.admission.lock();
                if (!a || a->reserved() || a->ended()) continue;
                open.push_back(Candidate{h, a, &it->second});
                if (!it->second.prefix.empty()) ++perPrefix[it->second.prefix];
            }
            std::sort(open.begin(), open.end(), [&](const Candidate& x, const Candidate& y) {
                const int nx = x.info->prefix.empty() ? 1 : perPrefix[x.info->prefix];
                const int ny = y.info->prefix.empty() ? 1 : perPrefix[y.info->prefix];
                if (nx != ny) return nx > ny;
                return x.info->accepted_ns < y.info->accepted_ns;
            });
            for (const auto& c : open) {
                if (!c.hs->markEvicted()) continue; // certified, or evicted, since it was looked at
                if (!c.adm->evict()) continue;      // its admission ended meanwhile: the next one
                c.hs->abort();
                victim = c.hs;
                vaddr = c.info->addr;
                break;
            }
        }
        if (victim) {
            {
                std::lock_guard<std::recursive_mutex> lk(m_);
                logLocked("disconnected", PetPath::Outside, vaddr, {}, {}, "dropped for a newer connection: its handshake hadn't completed");
            }
            ar = limiter_->tryAdmit(peer);
        }
    }
    if (ar.refusal != outside::Refusal::None) {
        closesocket(s);
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            logLocked(outside::refusalLogEvent(ar.refusal), PetPath::Outside, addr);
        }
        drainLog(); // §8.6, with m_ released: a flood of refusals never waits on the log's file
        return;
    }
    auto adm = std::make_shared<outside::Admission>(std::move(ar.admission));
    std::lock_guard<std::recursive_mutex> lk(m_);
    if (shutting_down_ || !outside_listener_ || !hs_pool_) {
        closesocket(s); // the ticket goes with adm, counting nothing
        return;
    }
    auto hs = std::make_shared<ServerHandshake>(s, outside_ctx_);
    const auto deadline = ServerHandshake::Clock::now() + std::chrono::milliseconds(opts_.outside_handshake_timeout_ms);
    const auto firstBy = ServerHandshake::Clock::now() + std::chrono::milliseconds(std::max(100, opts_.outside_first_record_ms));
    const uint64_t gen = outside_gen_.load();
    const bool reserved = adm->reserved();
    {
        // Live from its accept (it holds a slot from here), so it can be evicted even before a pool thread runs it.
        std::lock_guard<std::mutex> g(hs_live_m_);
        hs_live_.push_back(hs);
        hs_info_[hs.get()] = LiveHandshake{adm, monotonicNs(), addr, outside::prefix48Key(peer)};
    }
    // Unregistered when the job goes, run or not (the pool stopped, or post() refused it).
    struct LiveToken {
        // It holds the handshake itself, not its address: while it lives, no new handshake can take that address and
        // be erased in its place.
        LiveToken(HostStreamServer* s, std::shared_ptr<ServerHandshake> hs) : self(s), h(std::move(hs)) {}
        LiveToken(const LiveToken&) = delete; // made once, in place: a copy's end would unregister it early
        LiveToken& operator=(const LiveToken&) = delete;
        HostStreamServer* self;
        std::shared_ptr<ServerHandshake> h;
        ~LiveToken() {
            std::lock_guard<std::mutex> g(self->hs_live_m_);
            auto& v = self->hs_live_;
            v.erase(std::remove(v.begin(), v.end(), h), v.end());
            self->hs_info_.erase(h.get());
        }
    };
    auto token = std::make_shared<LiveToken>(this, hs);
    if (!hs_pool_->post([this, hs, adm, peer, deadline, firstBy, gen, token] {
            runOutsideHandshake(hs, adm, peer, deadline, firstBy, gen);
        },
                        reserved)) {
        hs->abort(); // refused: the job (and the token) are gone, and the socket closes with hs
    }
}

void HostStreamServer::runOutsideHandshake(const std::shared_ptr<ServerHandshake>& hs,
                                           const std::shared_ptr<outside::Admission>& adm, const outside::IpAddress& peer,
                                           ServerHandshake::Clock::time_point deadline,
                                           ServerHandshake::Clock::time_point firstBy, uint64_t gen) {
    {
        std::lock_guard<std::mutex> g(hs_live_m_);
        if (gen != outside_gen_.load()) hs->abort(); // turned off (or moved) since it was accepted
    }
    // Unregistered by the job's LiveToken when the job goes (handleOutsideSocket).

    const std::string addr = outside::toText(peer);
    std::string why;
    t_outside_admitted_invite = 0;
    const bool blocked = adm->blocked();
    ServerHandshake::Result r;
    {
        // Cleared on every way out of run(), a throw too, so a pool thread's next handshake never
        // inherits it.
        struct BlockedFlag {
            explicit BlockedFlag(bool on) { t_outside_blocked = on; }
            ~BlockedFlag() { t_outside_blocked = false; }
            BlockedFlag(const BlockedFlag&) = delete;
            BlockedFlag& operator=(const BlockedFlag&) = delete;
        } flag(blocked);
        r = hs->run(deadline, &why, firstBy);
    }
    // The verify callback ran on this thread, inside run(): the invite it admitted this leaf for, if any.
    const uint64_t admitted_invite = t_outside_admitted_invite;
    t_outside_admitted_invite = 0;
    if (r == ServerHandshake::Result::Aborted) {
        // turned off or evicted: the ticket is released, nothing counted. A test can keep an evicted one registered a
        // while, so a later newcomer meets it in the list (so a row can show an evicted one is never picked again).
        if (opts_.aborted_linger_ms_for_test > 0) Sleep(static_cast<DWORD>(opts_.aborted_linger_ms_for_test));
        return;
    }
    if (blocked && r != ServerHandshake::Result::Ok) {
        // A blocked source that showed no listed device's certificate: refused as blocked, counting nothing (the
        // limiter changes nothing for a blocked source's end), and the block keeps its clock.
        adm->finish(r == ServerHandshake::Result::Rejected ? outside::HandshakeResult::Failure
                                                           : outside::HandshakeResult::Timeout);
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            logLocked("refused_blocked", PetPath::Outside, addr, {}, {},
                      r == ServerHandshake::Result::Rejected ? "a certificate that matches no device" : why);
        }
        drainLog();
        return;
    }
    if (r != ServerHandshake::Result::Ok) {
        // §8.5: only a certificate that arrived and matched no listed device (with no invite open) counts as a
        // failure. A peer that closes, resets, stalls past the deadline or sends something that isn't TLS before
        // its certificate is checked never does: a weak mobile link must not block the owner's own phone.
        const bool rejected = r == ServerHandshake::Result::Rejected;
        adm->finish(rejected ? outside::HandshakeResult::Failure : outside::HandshakeResult::Timeout);
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            logLocked(rejected ? "refused_unknown_cert" : "disconnected", PetPath::Outside, addr, {}, {}, why);
        }
        drainLog();
        return;
    }
    if (opts_.outside_test_hook) opts_.outside_test_hook("before_classify");
    // §8.1: classified under the server lock.
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (shutting_down_ || !outside_listener_ || gen != outside_gen_.load()) return;
        const Bytes hash = sha256(hs->peerCertDer());
        if (blocked && !store_.findByHash(hash)) {
            // Removed between the verify step and here: a blocked source never reaches an invite.
            adm->finish(outside::HandshakeResult::Timeout);
            logLocked("refused_blocked", PetPath::Outside, addr, {}, {}, "a certificate that matches no device");
            return;
        }
        if (store_.findByHash(hash)) {
            // A listed device: an ordinary session on the outside path. Its handshake is over, so it no longer
            // counts against the unauthenticated caps; its source gets the reserve for the next 24 h.
            adm->finish(outside::HandshakeResult::Success);
            limiter_->noteAuthenticated(peer);
            if (auto c = hs->handOff(outside_ctx_, streamCallbacksLocked(PetPath::Outside, addr, gen))) {
                conns_.push_back(std::move(c));
            } else {
                logLocked("refused_unavailable", PetPath::Outside, addr); // no thread for it: this one only
            }
            return;
        }
        // A certificate the verify callback admitted because an invite
        // was open, whose invite the PC has closed since (cancelled it, made a new one, or closed it with the
        // endpoint), is not the device's failure. It goes on to the redemption, which answers its one frame with
        // "This invite was closed" and counts nothing. Decided from the invite it was admitted for, never from
        // "no invite now" alone: a leaf taken as a listed device's (revoked since), or for an invite that ran out
        // or was used or burned meanwhile, is closed and counted as before.
        const outside::InviteStore::Fate fate =
            !invites_ ? outside::InviteStore::Fate::Closed
            : admitted_invite != 0
                ? invites_->fateOf(admitted_invite, inviteNowMs())
                : (invites_->open(inviteNowMs()) ? outside::InviteStore::Fate::Open : outside::InviteStore::Fate::Ended);
        const bool closed_by_pc = admitted_invite != 0 && fate == outside::InviteStore::Fate::Closed;
        if (fate != outside::InviteStore::Fate::Open && !closed_by_pc) {
            // No invite it could redeem is open: closed, and counted (§8.5).
            adm->finish(outside::HandshakeResult::Failure);
            ++rejected_handshakes_;
            logLocked("refused_unknown_cert", PetPath::Outside, addr, {}, {}, "no invite is open", hexOf(hash));
            return;
        }
    }
    runRedemption(hs, adm, peer, admitted_invite); // still on this pool thread: unauthenticated until it ends
}

void HostStreamServer::runRedemption(const std::shared_ptr<ServerHandshake>& hs, const std::shared_ptr<outside::Admission>& adm,
                                     const outside::IpAddress& peer, uint64_t admitted_invite) {
    const std::string addr = outside::toText(peer);
    const Bytes leaf = hs->peerCertDer();
    const Bytes client_hash = sha256(leaf);
    const std::string who = hexOf(client_hash);
    // §8.1: no hello and nothing else first; exactly one frame within 10 s, at most 16 KiB, which must be a
    // pairRequest with methodRaw 2. Anything else is closed, counted as a failure and logged; it burns nothing.
    MsgType type{};
    std::vector<uint8_t> payload;
    std::string why;
    const auto deadline = ServerHandshake::Clock::now() + std::chrono::milliseconds(opts_.outside_redeem_timeout_ms);
    ++redemptions_waiting_;
    const ServerHandshake::Read rd = hs->readOneFrame(deadline, kRedeemMaxPayload, type, payload, &why);
    --redemptions_waiting_;
    if (rd == ServerHandshake::Read::Aborted) return;
    if (rd == ServerHandshake::Read::Closed) {
        // §8.5: the peer went away before sending its frame. Not a failure (a dropped link), and nothing burned.
        adm->finish(outside::HandshakeResult::Timeout);
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            logLocked("disconnected", PetPath::Outside, addr, {}, {}, "closed before its pairRequest", who);
        }
        drainLog();
        return;
    }
    // Anything else is a failed or malformed redemption after a completed handshake, and counts (§8.5): a frame
    // too big or not a frame, none within 10 s, not a pairRequest, one that doesn't parse, another method.
    PairRequestMessage req;
    bool well_formed = rd == ServerHandshake::Read::Frame;
    if (well_formed && type != MsgType::PairRequest) {
        well_formed = false;
        why = "a frame other than pairRequest";
    }
    if (well_formed && !fromJson(payload, req)) {
        well_formed = false;
        why = "a pairRequest that doesn't parse";
    }
    if (well_formed && req.methodRaw != static_cast<uint8_t>(PairingMethod::Invite)) {
        well_formed = false; // v3 §4.4: the outside listener takes methodRaw 2 only
        why = "methodRaw " + std::to_string(req.methodRaw) + " on the outside listener";
    }
    if (!well_formed) {
        adm->finish(outside::HandshakeResult::Failure);
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            logLocked("invite_failed", PetPath::Outside, addr, {}, {}, why, who);
        }
        drainLog();
        return;
    }

    std::vector<uint8_t> reply;
    bool accepted = false;
    outside::HandshakeResult result = outside::HandshakeResult::Failure;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (shutting_down_ || !outside_listener_) return;
        auto refuse = [&](const std::string& message) {
            PairResponseMessage r; // today's shape: no v3 keys on a refusal
            r.message = message;
            reply = packFrame(MsgType::PairResponse, 0, toJson(r));
        };
        if (admitted_invite != 0 && (!invites_ || invites_->fateOf(admitted_invite, inviteNowMs()) ==
                                                       outside::InviteStore::Fate::Closed)) {
            // The PC closed the invite this device was admitted for,
            // during its handshake or while its frame was on the way (cancelled, a new one made, closed with the
            // endpoint). Not the device's failure, and nothing is checked or burned: it is told the invite was
            // closed, in today's refusal shape (§4.4, §9).
            refuse(text::kInviteClosed);
            result = outside::HandshakeResult::Timeout;
            logLocked("invite_closed", PetPath::Outside, addr, {}, {}, "closed while this device was redeeming it", who);
        } else if (devices_unread_) {
            // Fail closed: the device list on disk may hold other devices; nothing is added over it. Not the
            // device's fault, so neither a failure nor a burned invite.
            result = outside::HandshakeResult::Timeout;
            logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "the device list can't be read", who);
        } else if (store_.full()) {
            refuse(text::kTooManyPairedDevices); // v3 §2.1; the invite stays open
            result = outside::HandshakeResult::Timeout;
            logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "16 devices are paired", who);
        } else {
            outside::Invite used;
            std::string ierr;
            // §8.4 step 4: the TLS leaf must equal clientCertDER, and the proof must match (constant time).
            bool burned = false;
            const outside::RedeemResult res = invites_->redeem(inviteNowMs(), identity_->certificateSHA256(), client_hash,
                                                               leaf == req.clientCertDER, req.clientProofHMAC, &used, &ierr,
                                                               &burned);
            switch (res) {
                case outside::RedeemResult::Accepted: {
                    DeviceStore::NewDevice nd;
                    nd.certSHA256 = client_hash;
                    nd.role = used.role; // the host's own invite record, never the link's r
                    nd.deviceName = req.deviceName;
                    nd.deviceKind = req.deviceKind;
                    nd.protocolVersion = req.protocolVersion;
                    nd.nowUnixMs = unixMsNow();
                    const DeviceRecord* rec = nullptr;
                    const auto ar = store_.addPairedDevice(nd, &rec);
                    if (ar == DeviceStore::AddResult::TooManyDevices || ar == DeviceStore::AddResult::Invalid) {
                        refuse(ar == DeviceStore::AddResult::TooManyDevices ? text::kTooManyPairedDevices : "Pairing failed.");
                        result = outside::HandshakeResult::Timeout;
                        logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "the device couldn't be added", who);
                        break;
                    }
                    rec = store_.findByHash(client_hash);
                    markDevicesDirtyLocked(); // written below, outside the lock, before the device is told
                    updateAdvertisementLocked();
                    PairResponseMessage resp;
                    resp.accepted = true;
                    resp.serviceId = state_.serviceId;
                    resp.streamPort = stream_port_; // §4.4: the LAN stream listener's port, for every method
                    resp.serverProofHMAC =
                        computePairingProof(used.token, identity_->certificateSHA256(), client_hash, "kv2-pair-server");
                    resp.message = "Paired";
                    if (rec) {
                        resp.role = std::string(petRoleString(rec->role));
                        resp.deviceId = rec->deviceId;
                        resp.deviceName = rec->name;
                        resp.protocolVersion = kProtocolV3;
                    }
                    reply = packFrame(MsgType::PairResponse, 0, toJson(resp));
                    accepted = true;
                    invite_live_ = false;
                    invite_end_ = "used";
                    // §8.7: committed. The device is added and the invite used, so the reply must reach it: no
                    // turn-off, port change or exit aborts this exchange from now (its send is still bounded).
                    {
                        std::lock_guard<std::mutex> g(hs_live_m_);
                        hs_live_.erase(std::remove_if(hs_live_.begin(), hs_live_.end(),
                                                      [&](const auto& x) { return x.get() == hs.get(); }),
                                       hs_live_.end());
                        hs->commit();
                    }
                    result = outside::HandshakeResult::Success;
                    ++pairings_completed_;
                    last_pairing_refusal_.clear();
                    logLocked("invite_redeemed", PetPath::Outside, addr, rec ? rec->deviceId : std::string(),
                              rec ? rec->name : std::string(), std::string("as ") + petRoleString(used.role), who);
                    reevaluateLocked(); // owners' device lists
                    break;
                }
                case outside::RedeemResult::ExpiredOrUsed:
                    refuse(text::kInviteExpiredOrUsed);
                    logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "expired or already used", who);
                    break;
                case outside::RedeemResult::WrongProof:
                    // A failure for this source (the limiter); the invite burns only at the tenth wrong attempt.
                    if (burned) {
                        refuse(text::kInviteWrongCode);
                        invite_live_ = false;
                        invite_end_ = "burned";
                        logLocked("invite_burned", PetPath::Outside, addr, {}, {}, "wrong proof, the tenth", who);
                    } else {
                        refuse(text::kInviteDidntMatch);
                        logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "wrong proof", who);
                    }
                    break;
                case outside::RedeemResult::CertMismatch:
                    // Closed without a reply (§4.4, as the LAN pairing listener does), and counted the same way.
                    if (burned) {
                        invite_live_ = false;
                        invite_end_ = "burned";
                        logLocked("invite_burned", PetPath::Outside, addr, {}, {}, "the TLS certificate isn't clientCertDER, the tenth", who);
                    } else {
                        logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "the TLS certificate isn't clientCertDER", who);
                    }
                    break;
                case outside::RedeemResult::NoInvite:
                    logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "no invite is open", who);
                    break;
                case outside::RedeemResult::NotSaved:
                    // Right, but its use couldn't be saved: refused (it can't come back after a restart either).
                    refuse(text::kInviteExpiredOrUsed);
                    result = outside::HandshakeResult::Timeout;
                    invite_error_ = ierr;
                    invite_live_ = false;
                    invite_end_ = "not_saved";
                    logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "the invite's use couldn't be saved", who);
                    break;
            }
        }
    }
    adm->finish(result);
    if (accepted && opts_.outside_test_hook) opts_.outside_test_hook("redeem_committed");
    if (accepted) flushSaves(FlushKind::Change); // devices.json, then pairing.json, before the device is told
    if (!reply.empty()) {
        const bool sent =
            hs->sendAndClose(reply, ServerHandshake::Clock::now() + std::chrono::milliseconds(kRedeemReplyWaitMs));
        if (!sent && accepted) {
            // It holds a valid pairing, so it stays listed (the PC can revoke it); the log says it wasn't told.
            std::lock_guard<std::recursive_mutex> lk(m_);
            logLocked("invite_failed", PetPath::Outside, addr, {}, {}, "paired, but the reply couldn't be delivered", who);
        }
    }
    drainLog();
    if (accepted) notifyPairingChanged();
    notifyPetStateChanged();
    notifyOutsideChanged();
}

void HostStreamServer::onMapperChanged(const outside::MapperState& st) {
    bool save = false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!started_ || shutting_down_) return;
        // §8.7: the port this PC's forward may still be on is saved (outsideMappedPort), from every report, so a
        // forward an exit couldn't remove is removed at the next start. Never over an unreadable pet-camera.json.
        if (!settings_unread_ && settings_.outsideMappedPort != st.heldPort) {
            settings_.outsideMappedPort = st.heldPort;
            markSettingsDirtyLocked();
            save = true;
        }
        // The pinned gateway, saved so it holds across restarts.
        const outside::GatewayIdentity saved{settings_.outsideGatewayIP, settings_.outsideGatewayMAC};
        if (!settings_unread_ && (!(st.gatewayPin == saved) || st.gatewayLocation != settings_.outsideGatewayLocation)) {
            settings_.outsideGatewayIP = st.gatewayPin.ip;
            settings_.outsideGatewayMAC = st.gatewayPin.mac;
            settings_.outsideGatewayLocation = st.gatewayLocation; // its description that last answered
            markSettingsDirtyLocked();
            save = true;
        }
        // §8.7: a report that answers an earlier open (a port changed since, outside turned off and on) is stale:
        // it would show the old port's mapping as the new one's. Only the open in force is taken.
        const bool current = outside_listener_ ? (st.openId == mapper_open_id_ && st.port == outside_port_) : st.openId == 0;
        if (!current) {
            if (!save) return;
        } else {
            mapper_state_ = st;
        }
        // §8.3: the router's public address changed. A device on the home network answering as a fake router can
        // report any address, so a new one is told to devices only once it is acknowledged: by the mapper, when it
        // was read through the pinned router, or by the PC user's re-share (acknowledgeAddressChange). Either way
        // the mapper then reports mapped with it. While it reads address_changed (being checked, or waiting for the
        // PC user), devices keep the address they were last told and the open invite, which carries that address,
        // closes. Once acknowledged, every v3 session gets a 0x0B with the new endpoint and a 0x0F address_changed
        // (not closing). Compared with the address devices were last told, so a failed check in between can't hide it.
        const bool pending = current && outside_listener_ && st.status == outside::OutsideStatus::AddressChanged &&
                             st.wanAddress != announced_wan_;
        const bool told_new = current && outside_listener_ && st.status == outside::OutsideStatus::Mapped &&
                              !st.wanAddress.empty() && st.wanAddress != announced_wan_;
        const bool changed = told_new && !announced_wan_.empty(); // the first address is no change
        if (told_new) {
            announced_wan_ = st.wanAddress;
            // Saved, so a restart checks a different first address rather than tell it at once.
            if (!settings_unread_ && (settings_.outsideAnnouncedAddress != announced_wan_ || settings_.outsideAnnouncedCorrupt)) {
                settings_.outsideAnnouncedAddress = announced_wan_;
                settings_.outsideAnnouncedCorrupt = false;
                markSettingsDirtyLocked();
                save = true;
            }
        }
        if (pending || changed) closeInviteLocked("address_changed", "the home address changed");
        if (changed) {
            for (auto& s : sessions_) s->force_0b = true;
        }
        reevaluateLocked(); // outsideStatus for owners, outsideEndpoint for everyone
        if (changed) {
            for (auto& s : sessions_) {
                if (s->version < kProtocolV3) continue;
                s->conn->send(packFrame(MsgType::SessionNotice, 0, toJson(makeSessionNotice(notice_reason::kAddressChanged))));
            }
        }
    }
    if (save) flushSaves(FlushKind::Change); // outside m_ (a failure is shown and retried)
    drainLog();
    notifyPetStateChanged();
    notifyOutsideChanged();
}

void HostStreamServer::closeInviteLocked(const char* end, const char* detail) {
    if (!invites_) return;
    const bool was_open = invites_->open(inviteNowMs()).has_value();
    invites_->close(&invite_error_);
    if (was_open) {
        invite_end_ = end;
        logLocked("invite_closed", PetPath::Outside, std::string(), {}, {}, detail); // §8.6
    }
    invite_live_ = false;
}

void HostStreamServer::syncInviteLocked() {
    if (!invite_live_ || !invites_) return;
    if (!invites_->open(inviteNowMs())) {
        invite_live_ = false; // ran out (a use, a burn or a close says so where it happens)
        if (invite_end_.empty()) invite_end_ = "expired";
        return;
    }
    // §8.4, §8.3: an invite is redeemable exactly while the PC shows it. It carries the endpoint it was made
    // with, so it closes as soon as there is no endpoint (a check that reads manual, error or carrier_nat, or an
    // entered address cleared) or the endpoint's address or port is another one. Not while the first check
    // after the listener opened is still running: that is not a result yet.
    if (!outside_listener_) {
        closeInviteLocked("outside_off", "viewing from outside the home turned off");
        return;
    }
    if (!mapper_state_.reported) return;
    const std::optional<PetEndpoint> ep = outsideEndpointLocked();
    if (!ep) {
        closeInviteLocked("address_gone", "the PC no longer knows its home address");
        return;
    }
    if (invite_host_.empty()) { // made before a restart: the first endpoint found is the one it carries
        invite_host_ = ep->host;
        invite_port_ = outside_port_;
        return;
    }
    if (ep->host != invite_host_ || outside_port_ != invite_port_) {
        closeInviteLocked("address_changed", "the home address changed");
    }
}

uint64_t HostStreamServer::inviteNowMs() const {
    return opts_.invite_clock_for_test ? opts_.invite_clock_for_test() : unixMsNow();
}

std::string HostStreamServer::inviteEndLocked() const {
    if (invite_live_ && invites_ && !invites_->open(inviteNowMs())) return "expired";
    return invite_end_;
}

std::vector<PetEndpoint> HostStreamServer::outsideCandidatesLocked(const std::optional<PetEndpoint>& ep) {
    std::vector<PetEndpoint> out;
    if (ep) out.push_back(*ep);
    if (!outside_listener_) return out;
    std::vector<outside::IpAddress> found;
    if (opts_.interface_addresses_for_test) {
        for (const auto& text : opts_.interface_addresses_for_test()) {
            if (const auto a = outside::parseIp(text)) found.push_back(*a);
        }
    } else {
        // Read from the interfaces when it is told, not once when outside listening opened: an address that went
        // away (Tailscale off) is no longer told. At most every 5 s, as 0x0B can go out often.
        const uint64_t now = monotonicNs();
        if (!overlay_read_ || now - overlay_read_ns_ >= 5000ULL * kMs) {
            overlay_ = outside::overlayAddressesOfThisPc();
            overlay_read_ns_ = now;
            overlay_read_ = true;
        }
        found = overlay_;
    }
    for (const auto& raw : found) {
        const outside::IpAddress a = outside::unwrapMapped(raw);
        if (a.v6 || !outside::isOverlayAddress(a)) continue; // this PC's own 100.64.0.0/10 address only
        PetEndpoint c;
        c.host = outside::toText(a);
        c.port = outside_port_;
        c.verified = false;
        bool dup = false;
        for (const auto& x : out) dup = dup || (x.host == c.host && x.port == c.port);
        if (!dup) out.push_back(std::move(c));
    }
    return out;
}
std::optional<PetEndpoint> HostStreamServer::outsideEndpointLocked() const {
    if (!outside_listener_) return std::nullopt;
    const outside::OutsideStatus st = mapper_state_.status;
    // A public address read from the router, for a forward that holds, wins: it is checked. While it reads
    // address_changed, devices keep the one they were last told until it is acknowledged (onMapperChanged).
    if (mapper_state_.reported && (st == outside::OutsideStatus::Mapped || st == outside::OutsideStatus::AddressChanged) &&
        outside::checkWanAddress(mapper_state_.wanAddress, opts_.address_policy_for_test) == outside::WanCheck::Public) {
        if (!addressChangePendingLocked()) return PetEndpoint{mapper_state_.wanAddress, outside_port_, true};
        // An address not acknowledged yet is never told: devices keep the one they were told, if any.
        if (!announced_wan_.empty()) return PetEndpoint{announced_wan_, outside_port_, true};
    }
    // §8.3: otherwise an address the PC user typed, labelled "entered by you, not checked", whatever the router
    // said or hasn't said yet: no UPnP (manual), carrier NAT, or the first check still running, so
    // the camera stays reachable from any network: a phone paired at home learns where to dial from outside. Not
    // after an error: that is the router saying this port leads to another machine (taken), so the typed
    // address would send devices there, and the PC shows the error and offers no invite (§8.2).
    const bool router_refused = mapper_state_.reported && st == outside::OutsideStatus::Error;
    if (!settings_.outsidePublicAddress.empty() && !router_refused) {
        return PetEndpoint{settings_.outsidePublicAddress, outside_port_, false};
    }
    return std::nullopt;
}

bool HostStreamServer::reshareOfferedLocked() const {
    if (!addressChangePendingLocked() || mapper_state_.gatewayContradicts) return false;
    if (!mapper_state_.gatewayPin.complete()) return true; // nothing pinned: nothing can confirm it
    return !mapper_state_.confirmedWan.empty() && mapper_state_.confirmedWan == mapper_state_.wanAddress;
}

bool HostStreamServer::addressChangePendingLocked() const {
    return outside_listener_ && mapper_state_.reported && mapper_state_.status == outside::OutsideStatus::AddressChanged &&
           mapper_state_.wanAddress != announced_wan_;
}

outside::OutsideStatus HostStreamServer::outsideStatusLocked() const {
    if (!outside_listener_) {
        return outside_error_.empty() && !outsideNoPortLocked() ? outside::OutsideStatus::Off : outside::OutsideStatus::Error;
    }
    return mapper_state_.reported ? mapper_state_.status : outside::OutsideStatus::Off;
}

std::string HostStreamServer::outsideSentenceLocked() const {
    if (!outside_listener_) {
        if (!outside_error_.empty()) return outside_error_;
        if (outsideNoPortLocked()) return outside::kNoBindableOutsidePortText;
        if (settings_unread_) return "pet-camera.json can't be read, so viewing from outside the home stays off.";
        if (settings_.allowOutside && !settings_.sharingEnabled) return "Viewing from outside the home needs \"Share the camera\" on.";
        return "Viewing from outside the home is off.";
    }
    if (!mapper_state_.reported) return "Checking the router.";
    const std::string at = mapper_state_.wanAddress + ":" + std::to_string(outside_port_);
    switch (mapper_state_.status) {
        case outside::OutsideStatus::Mapped: return "Devices outside your home can reach this PC at " + at + ".";
        case outside::OutsideStatus::AddressChanged: return "Your home address changed; re-share. It is now " + at + ".";
        case outside::OutsideStatus::Off: return "Viewing from outside the home is off.";
        default: return mapper_state_.message;
    }
}

std::string HostStreamServer::cannotInviteReasonLocked() const {
    if (!outside_listener_) return "Viewing from outside the home is off.";
    if (!outsideEndpointLocked()) {
        if (!mapper_state_.reported) return "The PC is still checking the router.";
        switch (mapper_state_.status) {
            case outside::OutsideStatus::Manual:
                return "The PC couldn't read your public address from the router. Type it in first.";
            case outside::OutsideStatus::CarrierNat:
                return "Your internet provider is using carrier NAT, so devices outside your home can't reach this PC.";
            default: return mapper_state_.message.empty() ? "The PC doesn't know its public address." : mapper_state_.message;
        }
    }
    if (devices_unread_) return "Can't add devices until the device list can be read.";
    if (store_.full()) return text::kTooManyPairedDevices;
    if (invites_ && !invites_->readable()) return "Can't make an invite until invite.json can be read. " + invite_error_;
    return {};
}

std::string HostStreamServer::inviteLanHostLocked() const {
    // lh is used only for redemption, at the outside listener: on loopback it is the loopback address.
    if (opts_.loopback_only || opts_.outside_loopback_only) return "127.0.0.1";
    if (!mapper_state_.lanIPv4.empty()) return mapper_state_.lanIPv4;
    return primaryLanIPv4();
}

std::optional<OutsideInviteView> HostStreamServer::inviteViewLocked(const outside::Invite& inv) const {
    const std::optional<PetEndpoint> ep = outsideEndpointLocked();
    if (!ep || !identity_) return std::nullopt;
    outside::InviteLink link;
    link.serviceId = state_.serviceId;
    link.hostCertSHA256 = identity_->certificateSHA256();
    link.token = inv.token;
    link.host = ep->host;
    link.port = outside_port_;
    link.lanHost = inviteLanHostLocked();
    link.expUnix = inv.expiresMs / 1000;
    link.role = inv.role;
    OutsideInviteView v;
    v.uri = link.toURI();
    v.expiresUnix = link.expUnix;
    v.role = inv.role;
    return v;
}

OutsideView HostStreamServer::outsideView() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    OutsideView v;
    v.allowed = settings_.allowOutside;
    v.listening = outside_listener_ != nullptr;
    v.status = outsideStatusLocked();
    v.port = v.listening ? outside_port_ : settings_.outsidePort;
    v.lanIPv4 = mapper_state_.lanIPv4;
    v.wanAddress = mapper_state_.wanAddress;
    if (const auto ep = outsideEndpointLocked()) {
        v.endpointKnown = true;
        v.endpointHost = ep->host;
        v.endpointVerified = ep->verified;
    }
    v.publicAddressEntered = settings_.outsidePublicAddress;
    v.sentence = outsideSentenceLocked();
    if (v.listening && v.status == outside::OutsideStatus::Manual && !mapper_state_.lanIPv4.empty()) {
        v.manualSteps = "Forward TCP " + std::to_string(outside_port_) + " to " + mapper_state_.lanIPv4 +
                        " in your router's settings.";
    }
    v.checking = v.listening && (mapper_state_.checking || !mapper_state_.reported);
    v.cannotInviteReason = cannotInviteReasonLocked();
    v.canInvite = v.cannotInviteReason.empty();
    v.inviteEnded = inviteEndLocked();
    v.mappingRemoved = v.listening && settings_.outsideMappingRemoved;
    v.gatewayPinKind = outside::gatewayPinKind(mapper_state_.gatewayPin);
    v.gatewayChecking = v.listening && mapper_state_.gatewayChecking;
    v.gatewayHeld = v.listening && mapper_state_.gatewayHeld;
    v.gatewayContradicts = v.listening && mapper_state_.gatewayContradicts;
    v.gatewayAnswer = v.listening ? mapper_state_.gatewayAnswer : std::string();
    v.gatewayFailedPasses = v.listening ? mapper_state_.gatewayFailedPasses : 0;
    v.reshareOffered = reshareOfferedLocked();
    return v;
}

bool HostStreamServer::setAllowOutside(bool on, std::string* err) {
    if (err) err->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) {
            // Nothing is written over a pet-camera.json that couldn't be read, and outside listening
            // stays off meanwhile.
            if (err) *err = "Can't change this until pet-camera.json can be read. " + settings_load_error_;
            return false;
        }
        if (on && !settings_.sharingEnabled) {
            if (err) *err = "Turn on \"Share the camera\" first: viewing from outside the home needs it.";
            return false;
        }
        if (settings_.allowOutside == on) return true;
        settings_.allowOutside = on;
        // Turning outside viewing off ends "Remove mapping now", so the next turn-on maps again.
        if (!on) settings_.outsideMappingRemoved = false;
        // Turned on by the PC user, so the router is pinned again at the first mapped result.
        outside_pin_anew_ = on;
        // v3 §8.7: a default port that binds, tried again on every turn-on while there is none. None bound: no
        // port is saved and the status line says so (outsideNoPortLocked).
        if (on && !outside::isValidOutsidePort(settings_.outsidePort)) settings_.outsidePort = pickOutsidePortLocked();
        markSettingsDirtyLocked(); // in force at once; a failed write is shown and retried
        applyOutsideLocked();
    }
    flushSaves(FlushKind::Change);
    joinRetiredOutside();
    notifyPetStateChanged();
    notifyOutsideChanged();
    return true;
}

bool HostStreamServer::setOutsidePort(uint16_t port, std::string* err) {
    if (err) err->clear();
    if (!outside::isValidOutsidePort(port)) {
        if (err) *err = "Pick a port from 1024 to 65535.";
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) {
            if (err) *err = "Can't change this until pet-camera.json can be read. " + settings_load_error_;
            return false;
        }
        if (settings_.outsidePort == port && (outside_listener_ || !outsideWantedLocked())) return true;
        settings_.outsidePort = port;
        markSettingsDirtyLocked();
        applyOutsideLocked(); // a new port re-opens the listener and the mapping
    }
    flushSaves(FlushKind::Change);
    joinRetiredOutside();
    notifyPetStateChanged();
    notifyOutsideChanged();
    return true;
}

bool HostStreamServer::setOutsidePublicAddress(const std::string& addr, std::string* err) {
    if (err) err->clear();
    const std::string trimmed = outside::trimmedAddress(addr);
    if (!trimmed.empty() && !outside::isUsablePublicAddress(trimmed, opts_.address_policy_for_test)) {
        if (err) *err = "Type your router's public IPv4 address (four numbers with dots), or leave it empty.";
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) {
            if (err) *err = "Can't change this until pet-camera.json can be read. " + settings_load_error_;
            return false;
        }
        if (settings_.outsidePublicAddress == trimmed) return true;
        settings_.outsidePublicAddress = trimmed;
        markSettingsDirtyLocked();
        reevaluateLocked(); // the manual case's outsideEndpoint
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    notifyOutsideChanged();
    return true;
}

bool HostStreamServer::removeOutsideMappingNow(std::string* err) {
    if (err) err->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        const bool holds = mapper_state_.reported && (mapper_state_.status == outside::OutsideStatus::Mapped ||
                                                      mapper_state_.status == outside::OutsideStatus::AddressChanged);
        if (!outside_listener_ || !mapper_ || !holds) {
            if (err) *err = "There is no port forward on the router to remove.";
            return false;
        }
        // It stays removed, across a restart too, until outside viewing is turned off or "Map the port again".
        settings_.outsideMappingRemoved = true;
        markSettingsDirtyLocked(); // in force at once; a failed write is shown and retried
        mapper_->removeNow();
        mapper_state_.checking = true; // shown as being worked on until the worker reports manual
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    notifyOutsideChanged();
    return true;
}

bool HostStreamServer::remapOutsideNow(std::string* err) {
    if (err) err->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) {
            if (err) *err = "Can't change this until pet-camera.json can be read. " + settings_load_error_;
            return false;
        }
        if (!outside_listener_ || !mapper_) {
            if (err) *err = "Viewing from outside the home is off.";
            return false;
        }
        if (!settings_.outsideMappingRemoved) {
            if (err) *err = "The port forward wasn't removed, so there is nothing to map again.";
            return false;
        }
        settings_.outsideMappingRemoved = false;
        markSettingsDirtyLocked();
        mapper_->remapNow();
        mapper_state_.checking = true; // shown as being worked on until the worker reports
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    notifyOutsideChanged();
    return true;
}

uint64_t HostStreamServer::outsideRechecksForTest() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return mapper_ ? mapper_->state().rechecks : 0;
}

std::string HostStreamServer::outsideListenerAddress() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return outside_listener_ ? outside_listener_->boundAddress() : std::string();
}

void HostStreamServer::acknowledgeAddressChange(const std::string& shownHost) {
    std::lock_guard<std::recursive_mutex> lk(m_);
    // Its worker applies it only if shownHost is still the address read now, and reports Mapped back through
    // onMapperChanged.
    if (mapper_) mapper_->acknowledgeAddressChange(shownHost);
}

std::optional<OutsideInviteView> HostStreamServer::createOutsideInvite(PetRole role, std::string* err) {
    if (err) err->clear();
    std::optional<OutsideInviteView> out;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (role != PetRole::Owner && role != PetRole::Viewer) {
            if (err) *err = "An invite is for an owner or a viewer.";
            return std::nullopt;
        }
        // §8.3: a changed address is re-shared first (that acknowledges it), so no invite carries one the PC user
        // hasn't seen, nor the old one devices are still told.
        if (addressChangePendingLocked()) {
            if (err) *err = "Your home address changed. Re-share it first, then make the invite.";
            return std::nullopt;
        }
        const std::string why = cannotInviteReasonLocked();
        if (!why.empty() || !invites_) {
            if (err) *err = why.empty() ? std::string("Invites aren't available.") : why;
            return std::nullopt;
        }
        outside::Invite inv;
        std::string ierr;
        if (!invites_->create(role, inviteNowMs(), inv, &ierr)) { // closes any older one
            invite_error_ = ierr;
            if (err) *err = ierr;
            return std::nullopt;
        }
        out = inviteViewLocked(inv);
        // §8.4: it carries this endpoint; it closes when that goes away or changes (syncInviteLocked).
        const std::optional<PetEndpoint> ep = outsideEndpointLocked();
        invite_live_ = true;
        invite_host_ = ep ? ep->host : std::string();
        invite_port_ = outside_port_;
        invite_end_.clear();
    }
    notifyOutsideChanged();
    return out;
}

void HostStreamServer::cancelOutsideInvite() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        closeInviteLocked("cancelled", "cancelled on the PC");
    }
    drainLog();
    notifyOutsideChanged();
}

std::optional<OutsideInviteView> HostStreamServer::currentOutsideInvite() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    if (!invites_ || !outside_listener_) return std::nullopt;
    const std::optional<outside::Invite> inv = invites_->open(inviteNowMs());
    if (!inv) return std::nullopt;
    return inviteViewLocked(*inv);
}

std::string HostStreamServer::outsideEndpointLink() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    // The PC's re-share (§8.3): the router's address as it reads now, a changed one included. Showing it to the PC
    // user is the point; closing the re-share acknowledges it, and only then are devices told.
    // v3 §8.3: a changed address is re-shared only once a pass confirmed it (with a gateway pinned): never one the
    // gateway contradicts, nor one it hasn't confirmed (its UPnP can be made to look unreadable).
    if (addressChangePendingLocked() && !reshareOfferedLocked()) return {};
    std::optional<PetEndpoint> ep = outsideEndpointLocked();
    if (addressChangePendingLocked()) ep = PetEndpoint{mapper_state_.wanAddress, outside_port_, true};
    if (!ep || !identity_) return {};
    outside::EndpointLink link;
    link.serviceId = state_.serviceId;
    link.hostCertSHA256 = identity_->certificateSHA256();
    link.host = ep->host;
    link.port = ep->port;
    return link.toURI();
}

std::vector<outside::LogLine> HostStreamServer::connectionLog(int days) const {
    std::vector<outside::LogLine> out;
    outside::ConnectionLog* log = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        log = log_.get();
    }
    if (!log) return out;
    drainLog(); // lines queued under m_ are shown at once
    const uint64_t now = unixMsNow();
    const uint64_t span = static_cast<uint64_t>(std::max(0, days)) * 24ull * 3600ull * 1000ull;
    std::vector<outside::LogLine> all = log->lines(); // oldest first
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (span != 0 && now > span && it->t < now - span) break;
        out.push_back(*it);
    }
    return out;
}

uint16_t HostStreamServer::outsideListeningPort() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return outside_listener_ ? outside_port_ : 0;
}

void HostStreamServer::outsideRecheckNowForTest() {
    std::lock_guard<std::recursive_mutex> lk(m_);
    if (mapper_) mapper_->recheckNow();
}

std::shared_ptr<outside::OutsideLimiter> HostStreamServer::outsideLimiterForTest() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return limiter_;
}

int HostStreamServer::outsideHandshakeWorkersForTest() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return hs_pool_ ? hs_pool_->workers() : 0;
}

int HostStreamServer::outsideHandshakesRunningForTest() const {
    std::lock_guard<std::mutex> g(hs_live_m_);
    return static_cast<int>(hs_live_.size());
}

// ---- the camera roll -----------------------------------------------------------------------------------

namespace {

void sendCommandResult(const std::shared_ptr<TlsConnection>& conn, uint64_t requestId, const char* error) {
    CommandResultMessage r;
    if (error) {
        r = makeCommandError(requestId, error); // ok false, the code and its user-facing sentence (v3 §9)
    } else {
        r.requestId = requestId;
        r.ok = true;
    }
    conn->send(packFrame(MsgType::CommandResult, 0, toJson(r)));
}

RollDay toWireDay(const roll::RollDayItem& d) {
    RollDay w;
    w.day = d.day;
    w.clipCount = d.clipCount;
    w.starredCount = d.starredCount;
    w.bytes = d.bytes;
    return w;
}

RollClip toWireClip(const roll::RollClipItem& c) {
    RollClip w;
    w.clipId = c.clipId;
    w.startUnixMs = c.startUnixMs;
    w.startLocal = c.startLocal;
    w.durationSec = c.durationSec;
    w.trigger = toWire(c.trigger);
    w.startedBy = c.startedBy;
    w.starred = c.starred;
    w.starredByMe = c.starredByMe;
    w.width = c.width;
    w.height = c.height;
    w.bytes = c.bytes;
    w.hasThumbnail = c.hasThumbnail;
    return w;
}

RollStorage toWireStorage(const roll::RollStorageInfo& s) {
    RollStorage w;
    w.usedBytes = s.usedBytes;
    w.capBytes = s.capBytes;
    w.freeBytes = s.freeBytes;
    w.floorBytes = s.floorBytes;
    w.keepDays = s.keepDays;
    return w;
}

// The 0x0D code for a roll store error (nullptr for none).
const char* storeErrorCode(roll::RollError e) {
    const char* c = roll::rollErrorCode(e);
    return c;
}

bool readSmallFile(const std::wstring& path, size_t max_bytes, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) <= max_bytes;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

} // namespace

void HostStreamServer::setRollRecorder(RollRecorder* roll) {
    std::lock_guard<std::recursive_mutex> lk(m_);
    roll_ = roll;
    roll_store_ = roll ? roll->store() : nullptr;
    // Nothing in the folder is changed before it is prepared, star clean-up included (rollPrepared).
    roll_prepared_ = false;
    star_reconcile_due_ = false;
    if (started_) {
        pushRollSettingsLocked();
        reevaluateLocked();
    }
}

bool HostStreamServer::rollAvailable() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return rollConfiguredLocked();
}

bool HostStreamServer::rollReadyLocked() const { return roll_ != nullptr && roll_store_ != nullptr; }

bool HostStreamServer::rollConfiguredLocked() const { return roll_ != nullptr && !roll_folder_unavailable_; }

void HostStreamServer::rollPrepared() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!roll_) return;
        roll_store_ = roll_->store();
        roll_prepared_ = true;
        star_reconcile_due_ = true; // the maintenance loop, once the device list is read
        maintenance_cv_.notify_all();
        if (!started_ || shutting_down_) return;
        reevaluateLocked();
        for (auto& s : sessions_) sendPetStateLocked(*s, false);
    }
    notifyPetStateChanged();
}

void HostStreamServer::setRollFolderUnavailable(bool unavailable) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        roll_folder_unavailable_ = unavailable;
        if (!started_ || shutting_down_) return;
        reevaluateLocked();
        for (auto& s : sessions_) sendPetStateLocked(*s, false);
    }
    notifyPetStateChanged();
}

const char* HostStreamServer::rollFolderBlockLocked() const {
    if (!roll_folder_unavailable_) return nullptr;
    if (settings_.privacyMode) return roll_blocked::kPrivacyMode;
    if (!settings_.rollEnabled) return roll_blocked::kRollDisabled;
    return roll_blocked::kFolderUnavailable;
}

void HostStreamServer::pushRollSettingsLocked() {
    if (!roll_) return;
    // pet-camera.md §6.2: what every trigger needs. The recorder keeps the sensor and the frozen view from the
    // PC (HostController); privacy, the PC replaying and the settings come from here.
    RollRecorder::Settings rs;
    rs.enabled = settings_.rollEnabled;
    rs.motionEnabled = settings_.motionEnabled;
    rs.motionSensitivity = settings_.motionSensitivity;
    rs.scheduleEnabled = settings_.recordScheduleEnabled;
    rs.schedule = settings_.recordSchedule;
    rs.keepDays = settings_.rollKeepDays;
    rs.capBytes = settings_.rollCapBytes;
    rs.limitsKnown = settings_.rollLimitsKnown;
    rs.settingsRead = !settings_unread_;
    roll_->setPrivacyMode(settings_.privacyMode); // first: privacy wins over everything (§5.1 order)
    roll_->setPcReplaying(pc_replaying_);
    roll_->setSettings(rs);
}

void HostStreamServer::rollStatusChanged() {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!started_ || shutting_down_) return;
        for (auto& s : sessions_) sendPetStateLocked(*s, false); // only a session whose 0x0B changed
    }
    notifyPetStateChanged();
}

const char* HostStreamServer::rollAccessLocked(const Session&, PetRole role) const {
    if (!rollConfiguredLocked()) return pet_error::kUnsupported; // as a host without a roll (§7.3)
    if (role == PetRole::Owner) return nullptr;
    // §3.1 "While sharing is on" also means the sharing schedule allows it and the PC isn't replaying.
    if (pc_replaying_) return pet_error::kPcReplaying;
    if (!settings_.sharingEnabled) return pet_error::kSharingOff;
    if (last_schedule_off_) return pet_error::kSharingSchedule;
    return nullptr;
}

bool HostStreamServer::canTakeSlotLocked(const Session& s, PetRole role) const {
    if (s.holds_slot) return true;
    int holders = 0;
    bool viewer_holds = false;
    for (const auto& o : sessions_) {
        if (!o->holds_slot) continue;
        ++holders;
        viewer_holds = viewer_holds || roleOfLocked(*o) != PetRole::Owner;
    }
    if (holders < kMaxWatcherSlots) return true;
    return role == PetRole::Owner && viewer_holds; // an owner takes the newest viewer's slot (§2.3)
}

bool HostStreamServer::playingLocked(const Session& s) const { return s.playback && s.playback->allocated(); }

void HostStreamServer::endPlaybackLocked(Session& s, const char* reason, bool silent) {
    if (!s.playback) return;
    if (silent) {
        s.playback->stopSilently();
    } else {
        std::optional<std::string> message;
        if (reason) {
            const char* sentence = text::sentenceFor(reason);
            message = std::string(sentence ? sentence : reason);
        }
        s.playback->stop(std::nullopt, message); // no requestId: a host-initiated stop (§5.9)
    }
    retirePlaybackLocked(std::move(s.playback));
    s.playback.reset();
}

void HostStreamServer::retirePlaybackLocked(std::shared_ptr<RollPlayback> p) {
    // The maintenance loop joins it now, not at its next 100 ms tick, so stopped streams don't pile up while a
    // device starts and stops playbacks quickly (each holds a thread and an open clip until joined).
    retired_playbacks_.push_back(std::move(p));
    maintenance_cv_.notify_all();
}

void HostStreamServer::joinRetiredPlaybacks() {
    std::vector<std::shared_ptr<RollPlayback>> done;
    std::shared_ptr<roll::RollStore> store;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        done.swap(retired_playbacks_);
        store = roll_store_;
    }
    for (auto& p : done) {
        p->join();
        // The reader has let go of the file: retention may delete the clip again (pinned when it started).
        if (store) store->unpinClip(p->clipId());
    }
}

void HostStreamServer::rollTestHook(const char* where) const {
    if (opts_.roll_test_hook) opts_.roll_test_hook(where);
}

std::vector<std::shared_ptr<RollPlayback>> HostStreamServer::stopPlaybacksOfClipLocked(const std::string& clipId) {
    std::vector<std::shared_ptr<RollPlayback>> stopped;
    for (auto& s : sessions_) {
        if (!s->playback || s->playback->clipId() != clipId) continue;
        s->playback->stop(std::nullopt, std::string(text::kClipDeleted)); // stopped first (§5.2), no requestId
        stopped.push_back(s->playback);
        retirePlaybackLocked(std::move(s->playback));
        s->playback.reset();
    }
    if (!stopped.empty()) reevaluateLocked();
    return stopped;
}

roll::RollError HostStreamServer::deleteClipNow(const std::shared_ptr<roll::RollStore>& store, const std::string& clipId,
                                                std::vector<std::shared_ptr<RollPlayback>> stopped) {
    for (const auto& p : stopped) p->join(); // the reader has let go of the file
    rollTestHook("delete_before_remove");
    {
        // A stream made on the clip since (its start was past the not_found check already) is stopped too,
        // with the same 0x17, before the files go.
        std::lock_guard<std::recursive_mutex> lk(m_);
        stopped = stopPlaybacksOfClipLocked(clipId);
    }
    for (const auto& p : stopped) p->join();
    const roll::RollError e = store->deleteClip(clipId);
    RollRecorder* rec = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        const auto it = deleting_clips_.find(clipId);
        if (it != deleting_clips_.end() && --it->second <= 0) deleting_clips_.erase(it);
        rec = roll_;
    }
    if (rec) rec->refreshStorageState(); // storage_full may have cleared (v3 §5.1)
    return e;
}

void HostStreamServer::dropRevokedStars() {
    std::vector<std::string> ids;
    std::shared_ptr<roll::RollStore> store;
    RollRecorder* rec = nullptr;
    bool reconciled = false;
    std::set<std::string> revoked;
    {
        // Called after every client frame: nothing to do is the usual case, and it must not wait on a star write.
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (star_drops_.empty() && !(star_reconcile_due_ && roll_store_ && !devices_unread_)) return;
    }
    std::lock_guard<std::mutex> stars(roll_star_m_); // no star write lands between a star's check and this drop
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        ids.swap(star_drops_);
        store = roll_store_;
        rec = roll_;
        // Fail closed: nothing is removed against a device list that hasn't been read.
        if (star_reconcile_due_ && store && !devices_unread_) {
            star_reconcile_due_ = false;
            reconciled = true;
            // Only the devices devices.json records as revoked (v3 §3.2 step 5): one revoked while no roll ran, or
            // in an earlier run. Being missing from the list is no proof of a revoke: a list rebuilt from
            // pairing.json (devices.json missing or unparsable) holds only the paired owner, and dropping every
            // other phone's stars would let retention delete their clips. A rebuilt list has no revoked record, so
            // nothing is dropped against it at all.
            if (!devices_rebuilt_) {
                for (const auto& h : store_.revoked()) {
                    std::string id = deviceIdFromHash(h);
                    if (!store_.findByDeviceId(id)) revoked.insert(std::move(id));
                }
            }
        }
    }
    if (!store) return;
    int changed = 0;
    for (const auto& id : ids) changed += store->removeStarsByDevice(id); // v3 §3.2 step 5
    if (!revoked.empty()) changed += store->removeStarsOfDevices(revoked);
    if (changed > 0 && rec) rec->refreshStorageState(); // storage_full may have cleared
    if (reconciled) rollTestHook("stars_reconciled"); // a test waits on this rather than a fixed time
}

std::vector<HostStreamServer::PlaybackProbe> HostStreamServer::playbackProbes() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    std::vector<PlaybackProbe> out;
    for (const auto& s : sessions_) {
        if (!playingLocked(*s)) continue;
        PlaybackProbe p;
        p.streamId = s->playback->streamId();
        p.framesSent = s->playback->framesSent();
        p.queuedBytes = s->conn->queuedBytes();
        out.push_back(p);
    }
    return out;
}

int HostStreamServer::playbackCount() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    int n = 0;
    for (const auto& s : sessions_) n += playingLocked(*s) ? 1 : 0;
    return n;
}

// ---- the PC's roll settings and actions --------------------------------------------------------------------

bool HostStreamServer::setRollEnabled(bool on) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) return false; // fail closed: never written over a pet-camera.json not read yet
        // The PC shows the limits beside the switch, so turning the roll on there confirms them (and writes
        // them), even when it was on already.
        const bool confirm = on && !settings_.rollLimitsKnown;
        if (settings_.rollEnabled == on && !confirm) return true;
        settings_.rollEnabled = on;
        if (confirm) settings_.rollLimitsKnown = true;
        markSettingsDirtyLocked();
        pushRollSettingsLocked();
        reevaluateLocked();
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    return true;
}

bool HostStreamServer::setMotionSettings(bool enabled, int sensitivity) {
    if (sensitivity < DepthMotionDetector::kMinSensitivity || sensitivity > DepthMotionDetector::kMaxSensitivity) return false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) return false;
        if (settings_.motionEnabled == enabled && settings_.motionSensitivity == sensitivity) return true;
        settings_.motionEnabled = enabled;
        settings_.motionSensitivity = sensitivity;
        markSettingsDirtyLocked();
        pushRollSettingsLocked();
        reevaluateLocked();
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    return true;
}

bool HostStreamServer::setRecordSchedule(const std::vector<RecordScheduleRow>& rows, bool enabled) {
    for (const auto& r : rows) {
        if ((r.days & 0x80) || r.startMinute >= 24 * 60 || r.lengthMinutes < 1 || r.lengthMinutes > 60) return false;
    }
    if (rows.size() > kMaxRecordScheduleRows) return false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) return false;
        settings_.recordSchedule = rows;
        settings_.recordScheduleEnabled = enabled;
        markSettingsDirtyLocked();
        pushRollSettingsLocked();
        reevaluateLocked();
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    return true;
}

bool HostStreamServer::setRollRetention(int keepDays, uint64_t capBytes) {
    if (keepDays < kRollMinKeepDays || keepDays > kRollMaxKeepDays || capBytes < kRollMinCapBytes) return false;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) return false;
        settings_.rollKeepDays = keepDays;
        settings_.rollCapBytes = capBytes;
        settings_.rollLimitsKnown = true; // the owner chose them
        markSettingsDirtyLocked();
        pushRollSettingsLocked(); // the recorder applies it and runs retention
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    return true;
}

bool HostStreamServer::rollLimitsKnown() const {
    std::lock_guard<std::recursive_mutex> lk(m_);
    return settings_.rollLimitsKnown;
}

bool HostStreamServer::confirmRollLimits(std::string* err) {
    if (err) err->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) {
            if (err) *err = "pet-camera.json can't be read yet, so the limits can't be saved";
            return false;
        }
        if (!settings_.rollLimitsKnown) {
            // The limits shown (the current settings) become the owner's, and are written with them.
            settings_.rollLimitsKnown = true;
            markSettingsDirtyLocked();
            pushRollSettingsLocked(); // the recorder applies them and runs retention, without a restart
        }
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    std::lock_guard<std::recursive_mutex> lk(m_);
    if (settings_dirty_ && !settings_save_error_.empty()) {
        if (err) *err = settings_save_error_;
        return false; // in force from memory; the maintenance loop retries the write
    }
    return true;
}

uint64_t HostStreamServer::pinRollClipForPc(const std::string& clipId) {
    std::shared_ptr<roll::RollStore> store;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        store = roll_store_;
    }
    if (!store || !store->mp4Path(clipId)) return 0;
    store->pinClip(clipId); // retention skips it while the PC plays it
    std::lock_guard<std::recursive_mutex> lk(m_);
    const uint64_t token = ++next_pc_pin_;
    pc_pins_[token] = PcPin{clipId, store};
    return token;
}

void HostStreamServer::unpinRollClipForPc(uint64_t token) {
    PcPin pin;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        const auto it = pc_pins_.find(token);
        if (it == pc_pins_.end()) return;
        pin = std::move(it->second);
        pc_pins_.erase(it);
    }
    if (pin.store) pin.store->unpinClip(pin.clipId);
    pc_pin_cv_.notify_all();
}

bool HostStreamServer::pcPinnedLocked(const std::string& clipId) const {
    for (const auto& [token, pin] : pc_pins_) {
        if (pin.clipId == clipId) return true;
    }
    return false;
}

bool HostStreamServer::waitForPcUnpin(const std::string& clipId) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!pcPinnedLocked(clipId)) return true;
    }
    // The PC's player has the clip open. It is told (on this thread, no lock held; it posts to its UI
    // thread), closes its player and unpins; the delete waits up to 2 s for that.
    if (onRollClipDeletedForPc) onRollClipDeletedForPc(clipId);
    std::unique_lock<std::recursive_mutex> lk(m_);
    return pc_pin_cv_.wait_for(lk, std::chrono::milliseconds(kPcUnpinWaitMs),
                               [&] { return !pcPinnedLocked(clipId) || shutting_down_; }) &&
           !pcPinnedLocked(clipId);
}

bool HostStreamServer::setRollFolderSetting(const std::string& folderUtf8) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (settings_unread_) return false;
        if (settings_.rollFolder == folderUtf8) return true;
        settings_.rollFolder = folderUtf8;
        markSettingsDirtyLocked();
    }
    flushSaves(FlushKind::Change);
    notifyPetStateChanged();
    return true;
}

roll::RollError HostStreamServer::setRollClipStarByPc(const std::string& clipId, bool starred) {
    std::shared_ptr<roll::RollStore> store;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        store = roll_store_;
    }
    if (!store) return roll::RollError::NotFound;
    roll::RollError e;
    {
        std::lock_guard<std::mutex> stars(roll_star_m_);
        e = store->setStar(clipId, roll::kPcDeviceId, PetRole::Owner, starred);
    }
    RollRecorder* rec = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        rec = roll_;
    }
    if (rec) rec->refreshStorageState(); // v3 §5.1: storage_full follows the stars at once
    notifyPetStateChanged();
    return e;
}

roll::RollError HostStreamServer::deleteRollClip(const std::string& clipId) {
    std::shared_ptr<roll::RollStore> store;
    std::vector<std::shared_ptr<RollPlayback>> stopped;
    {
        std::lock_guard<std::recursive_mutex> lk(m_);
        store = roll_store_;
        if (!store) return roll::RollError::NotFound;
        ++deleting_clips_[clipId]; // from now a 0x16 start for it gets not_found
        stopped = stopPlaybacksOfClipLocked(clipId); // stopped first (§5.2)
    }
    const roll::RollError e = deleteClipNow(store, clipId, std::move(stopped));
    notifyPetStateChanged();
    return e;
}

// ---- the roll on the wire ------------------------------------------------------------------------------------

std::function<void()> HostStreamServer::handleRollCommandLocked(Session& s, const PetCameraCommandMessage& cmd) {
    const std::shared_ptr<TlsConnection> conn = s.conn;
    const uint64_t rid = cmd.requestId;
    if (!rollConfiguredLocked()) {
        // A host without a roll answers unsupported (§5.1); with a roll folder set that can't be used, start_clip is
        // refused with the block instead, so the device can say why (§5.2). A viewer without roll access
        // hears that first, after privacy mode, as it would with a roll.
        const char* block = cmd.action == pet_action::kStartClip ? rollFolderBlockLocked() : nullptr;
        if (block && roleOfLocked(s) != PetRole::Owner && !settings_.privacyMode) {
            if (pc_replaying_) block = pet_error::kPcReplaying;
            else if (!settings_.sharingEnabled) block = pet_error::kSharingOff;
            else if (last_schedule_off_) block = pet_error::kSharingSchedule;
        }
        sendCommandResult(conn, rid, block ? block : pet_error::kUnsupported);
        return {};
    }
    RollCommand rc;
    if (const char* code = parseRollCommand(cmd, rc)) {
        sendCommandResult(conn, rid, code);
        return {};
    }
    const PetRole role = roleOfLocked(s);
    const bool owner = role == PetRole::Owner;
    const char* access = rollAccessLocked(s, role);
    const DeviceRecord* dev = deviceOfLocked(s);
    const std::string device_id = dev ? dev->deviceId : deviceIdFromHash(s.hash);
    const std::string device_name = dev ? dev->name : std::string();
    switch (rc.kind) {
    case RollCommandKind::SetRoll:
    case RollCommandKind::SetMotion: {
        if (!owner) {
            sendCommandResult(conn, rid, pet_error::kNotAllowedForRole);
            return {};
        }
        if (settings_unread_) {
            // pet-camera.json couldn't be read: a change that would have to be written over it is refused
            // (pet-camera.md §5), as set_sharing is.
            sendCommandResult(conn, rid, pet_error::kInternal);
            return {};
        }
        sendCommandResult(conn, rid, nullptr); // the answer goes out before the effect (§5.2)
        bool changed = false;
        if (rc.kind == RollCommandKind::SetRoll) {
            changed = settings_.rollEnabled != *rc.enabled;
            settings_.rollEnabled = *rc.enabled;
        } else {
            if (rc.enabled) {
                changed = changed || settings_.motionEnabled != *rc.enabled;
                settings_.motionEnabled = *rc.enabled;
            }
            if (rc.sensitivity) {
                changed = changed || settings_.motionSensitivity != *rc.sensitivity;
                settings_.motionSensitivity = *rc.sensitivity;
            }
        }
        if (!changed) return {};
        markSettingsDirtyLocked();
        pushRollSettingsLocked();
        reevaluateLocked();
        return {};
    }
    case RollCommandKind::StartClip: {
        // §3.1: an owner, or a viewer while sharing is on. Privacy mode comes first for every role (§5.1
        // order), so a viewer in privacy mode hears privacy_mode, from the recorder's block.
        if (!owner && access && !settings_.privacyMode) {
            sendCommandResult(conn, rid, access);
            return {};
        }
        const StartClipResult r = roll_->startClip(ClipStartedBy::device(device_id, device_name));
        sendCommandResult(conn, rid, r.ok ? nullptr : rollBlockCode(r.refused));
        return {};
    }
    case RollCommandKind::StopClip: {
        if (!owner && access) {
            sendCommandResult(conn, rid, access);
            return {};
        }
        // Ok when it stopped the manual trigger, ok (a no-op) when nothing records,
        // busy only for a motion or schedule clip with no manual trigger. It works in privacy mode (§6.4).
        const StopClipResult r = roll_->stopClip();
        sendCommandResult(conn, rid, r == StopClipResult::Busy ? pet_error::kBusy : nullptr);
        return {};
    }
    case RollCommandKind::Star:
    case RollCommandKind::Unstar: {
        if (!owner && access) {
            sendCommandResult(conn, rid, access);
            return {};
        }
        if (!roll_store_) {
            sendCommandResult(conn, rid, pet_error::kNotFound); // no folder yet: no clip to star
            return {};
        }
        const std::shared_ptr<roll::RollStore> store = roll_store_;
        const bool starred = rc.kind == RollCommandKind::Star;
        const std::string clip = rc.clipId;
        // The sidecar's read-modify-write, after the lock. The store applies the §5.2 rules: an owner's unstar
        // clears every star, a viewer's only its own (not_allowed_for_role when it has none).
        return [this, conn, rid, store, clip, device_id, role, starred] {
            rollTestHook("star");
            RollRecorder* rec = nullptr;
            roll::RollError e = roll::RollError::None;
            bool decided = false;
            {
                // v3 §3.2: a device revoked since the lock was released must not star. Checked again under
                // roll_star_m_, which the revoke's star removal also takes, so the star either lands before
                // that removal (which then removes it) or is refused here.
                std::lock_guard<std::mutex> stars(roll_star_m_);
                bool listed = false;
                {
                    std::lock_guard<std::recursive_mutex> lk(m_);
                    Session* sp = findSessionLocked(*conn);
                    const DeviceRecord* d = sp ? deviceOfLocked(*sp) : nullptr;
                    listed = sp && d && d->deviceId == device_id && !shutting_down_; // else closed: no one to answer
                    rec = roll_;
                }
                if (listed) {
                    e = store->setStar(clip, device_id, role, starred);
                    decided = true;
                }
            }
            rollTestHook("star_done"); // a test waits on this rather than a fixed time
            if (!decided) return;
            sendCommandResult(conn, rid, storeErrorCode(e));
            if (rec) rec->refreshStorageState(); // v3 §5.1: storage_full follows the stars at once
        };
    }
    case RollCommandKind::Delete: {
        if (!owner) {
            sendCommandResult(conn, rid, pet_error::kNotAllowedForRole); // §3.1: a viewer can't delete
            return {};
        }
        if (!roll_store_) {
            sendCommandResult(conn, rid, pet_error::kNotFound); // no folder yet: no clip to delete
            return {};
        }
        // §5.2: a playing clip is stopped first (0x17 stopped), on every session that plays it, and until its
        // files are gone a 0x16 start for it gets not_found.
        ++deleting_clips_[rc.clipId];
        const std::shared_ptr<roll::RollStore> store = roll_store_;
        const std::string clip = rc.clipId;
        return [this, conn, rid, store, clip, device_id] {
            auto release = [&] {
                std::lock_guard<std::recursive_mutex> lk(m_);
                const auto it = deleting_clips_.find(clip);
                if (it != deleting_clips_.end() && --it->second <= 0) deleting_clips_.erase(it);
            };
            // The PC's own player is asked to let go first (up to 2 s). Still holding it, the delete is
            // refused and nothing is touched: no playback stopped, no file removed.
            if (!waitForPcUnpin(clip)) {
                release();
                sendCommandResult(conn, rid, pet_error::kInternal);
                return;
            }
            rollTestHook("delete_after_wait");
            std::vector<std::shared_ptr<RollPlayback>> stopped;
            const char* refused = nullptr;
            bool answer = true;
            {
                std::lock_guard<std::recursive_mutex> lk(m_);
                // v3 §3.1: only an owner deletes. The wait above can take 2 s, so the device is checked again under
                // the lock, as a star is: still connected, still listed as this device, and still an owner. If not,
                // nothing is deleted.
                Session* sp = findSessionLocked(*conn);
                const DeviceRecord* d = sp ? deviceOfLocked(*sp) : nullptr;
                if (!sp || shutting_down_) {
                    refused = pet_error::kNotFound;
                    answer = false; // closed (a revoke closes the session): no one to answer
                } else if (!d || d->deviceId != device_id) {
                    refused = pet_error::kNotFound;
                } else if (d->role != PetRole::Owner) {
                    refused = pet_error::kNotAllowedForRole;
                } else {
                    stopped = stopPlaybacksOfClipLocked(clip);
                }
            }
            if (refused) {
                release();
                if (answer) sendCommandResult(conn, rid, refused);
                rollTestHook("delete_done");
                return;
            }
            const roll::RollError e = deleteClipNow(store, clip, std::move(stopped));
            sendCommandResult(conn, rid, storeErrorCode(e));
            rollTestHook("delete_done");
        };
    }
    }
    sendCommandResult(conn, rid, pet_error::kUnsupported);
    return {};
}

std::function<void()> HostStreamServer::handleRollListLocked(Session& s, const RollListRequestMessage& m) {
    const std::shared_ptr<TlsConnection> conn = s.conn;
    const PetRole role = roleOfLocked(s);
    // §7.3: unsupported without a roll; a viewer's sharing_off, sharing_schedule or pc_replaying. Listing is
    // metadata only and keeps working in privacy mode (§6.4).
    if (const char* code = rollAccessLocked(s, role)) {
        sendCommandResult(conn, m.requestId, code);
        return {};
    }
    if (const char* code = checkRollListRequest(m)) {
        sendCommandResult(conn, m.requestId, code);
        return {};
    }
    const DeviceRecord* dev = deviceOfLocked(s);
    const std::string device_id = dev ? dev->deviceId : deviceIdFromHash(s.hash);
    if (!roll_store_) {
        // No folder yet (it is made the first time the roll is on): an empty roll, no day in it.
        if (m.day) {
            sendCommandResult(conn, m.requestId, pet_error::kNotFound);
            return {};
        }
        RollListMessage out;
        out.requestId = m.requestId;
        out.days = std::vector<RollDay>();
        conn->send(packFrame(MsgType::RollList, 0, toJson(out)));
        return {};
    }
    const std::shared_ptr<roll::RollStore> store = roll_store_;
    // Read only: a list request never creates or rewrites a file (§5.6).
    return [conn, store, m, device_id] {
        RollListMessage out;
        out.requestId = m.requestId;
        if (!m.day) {
            const roll::DayListResult r = store->listDays(m.cursor, m.limit);
            if (r.error != roll::RollError::None) {
                sendCommandResult(conn, m.requestId, storeErrorCode(r.error));
                return;
            }
            std::vector<RollDay> days;
            for (const auto& d : r.days) days.push_back(toWireDay(d));
            out.days = std::move(days);
            out.nextCursor = r.nextCursor;
        } else {
            const roll::ClipListResult r = store->listClips(*m.day, m.cursor, m.limit, device_id);
            if (r.error != roll::RollError::None) {
                sendCommandResult(conn, m.requestId, storeErrorCode(r.error));
                return;
            }
            std::vector<RollClip> clips;
            for (const auto& c : r.clips) clips.push_back(toWireClip(c));
            out.day = m.day;
            out.clips = std::move(clips);
            out.nextCursor = r.nextCursor;
        }
        out.storage = toWireStorage(store->storage());
        conn->send(packFrame(MsgType::RollList, 0, toJson(out)));
    };
}

std::function<void()> HostStreamServer::handleRollThumbLocked(Session& s, const RollThumbRequestMessage& m) {
    const std::shared_ptr<TlsConnection> conn = s.conn;
    const PetRole role = roleOfLocked(s);
    if (!rollConfiguredLocked()) {
        sendCommandResult(conn, m.requestId, pet_error::kUnsupported); // as a host without a roll (§7.3)
        return {};
    }
    // §6.4: privacy mode refuses thumbnails, for every role, and comes first, as for a 0x16 start and
    // start_clip (the §5.1 order).
    if (settings_.privacyMode) {
        sendCommandResult(conn, m.requestId, pet_error::kPrivacyMode);
        return {};
    }
    if (const char* code = rollAccessLocked(s, role)) {
        sendCommandResult(conn, m.requestId, code);
        return {};
    }
    if (const char* code = checkRollThumbRequest(m)) {
        sendCommandResult(conn, m.requestId, code);
        return {};
    }
    if (!roll_store_) {
        sendCommandResult(conn, m.requestId, pet_error::kNotFound); // no folder yet: an empty roll
        return {};
    }
    const std::shared_ptr<roll::RollStore> store = roll_store_;
    return [this, conn, store, m] {
        const std::optional<std::wstring> path = store->thumbnailPath(m.clipId); // validated against the index
        std::vector<uint8_t> jpeg;
        if (!path) {
            sendCommandResult(conn, m.requestId, pet_error::kNotFound); // §5.8: no clip, or no thumbnail
            return;
        }
        if (!readSmallFile(*path, RollThumbMessage::kMaxJpegBytes, jpeg)) {
            sendCommandResult(conn, m.requestId, pet_error::kInternal); // there, but unreadable or over 64 KiB
            return;
        }
        // Checked again as it goes out, under the lock privacy mode is turned on under: a thumbnail read
        // before privacy came on is not sent after it (fail closed).
        std::lock_guard<std::recursive_mutex> lk(m_);
        Session* sp = findSessionLocked(*conn);
        if (!sp) return;
        const char* code = settings_.privacyMode ? pet_error::kPrivacyMode : rollAccessLocked(*sp, roleOfLocked(*sp));
        if (code) {
            sendCommandResult(conn, m.requestId, code);
            return;
        }
        RollThumbMessage out;
        out.requestId = m.requestId;
        out.clipId = m.clipId;
        out.jpeg = std::move(jpeg);
        // Droppable while queued: privacy mode, or a viewer losing roll access, takes it back off the queue.
        conn->send(packFrame(MsgType::RollThumb, 0, toJson(out)), false, kDropRollThumbs, m.requestId);
    };
}

std::function<void()> HostStreamServer::handlePlaybackLocked(Session& s, const PlaybackRequestMessage& m) {
    const std::shared_ptr<TlsConnection> conn = s.conn;
    const uint64_t rid = m.requestId;
    if (!rollConfiguredLocked()) {
        sendCommandResult(conn, rid, pet_error::kUnsupported);
        return {};
    }
    if (const char* code = checkPlaybackRequest(m)) {
        sendCommandResult(conn, rid, code);
        return {};
    }
    const PetRole role = roleOfLocked(s);
    if (m.action != playback_action::kStart) {
        // §5.9: the stream id must be this session's current stream, and still allocated (stopped and error
        // free it); otherwise not_found. Each action sends its one 0x17.
        RollPlayback* pb = s.playback.get();
        if (!pb || pb->streamId() != *m.streamId || !pb->allocated()) {
            sendCommandResult(conn, rid, pet_error::kNotFound);
            return {};
        }
        bool answered = false;
        if (m.action == playback_action::kPause) {
            answered = pb->pause(rid);
        } else if (m.action == playback_action::kResume) {
            answered = pb->resume(rid);
        } else if (m.action == playback_action::kSeek) {
            answered = pb->seek(rid, *m.positionSec);
        } else if (m.action == playback_action::kStop) {
            answered = pb->stop(rid, std::nullopt);
            if (answered) {
                retirePlaybackLocked(std::move(s.playback));
                s.playback.reset();
                reevaluateLocked(); // the slot is free; live video comes back only on 0x0E watch:true
            }
        }
        if (!answered) sendCommandResult(conn, rid, pet_error::kNotFound);
        return {};
    }
    // start (§5.9 "Errors on start"): privacy mode, a viewer without roll access, no slot; the clip itself
    // is looked up after the lock.
    if (settings_.privacyMode) {
        sendCommandResult(conn, rid, pet_error::kPrivacyMode);
        return {};
    }
    if (const char* code = rollAccessLocked(s, role)) {
        sendCommandResult(conn, rid, code);
        return {};
    }
    if (!canTakeSlotLocked(s, role)) {
        sendCommandResult(conn, rid, pet_error::kTooManyViewers);
        return {};
    }
    if (!roll_store_) {
        sendCommandResult(conn, rid, pet_error::kNotFound); // no folder yet: an empty roll
        return {};
    }
    const std::shared_ptr<roll::RollStore> store = roll_store_;
    return [this, conn, store, m, rid] {
        // From the index only: an id that isn't listed is not_found, and nothing the device sent is ever
        // joined into a path (§7.1).
        const std::optional<roll::ClipSidecar> sidecar = store->findClip(*m.clipId);
        const std::optional<std::wstring> path = sidecar ? store->mp4Path(*m.clipId) : std::nullopt;
        if (!sidecar || !path) {
            sendCommandResult(conn, rid, pet_error::kNotFound);
            return;
        }
        rollTestHook("play_after_find");
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            Session* sp = findSessionLocked(*conn);
            if (!sp || shutting_down_) return;
            Session& s2 = *sp;
            const PetRole role2 = roleOfLocked(s2);
            // Checked again now the lock is back (fail closed): privacy, access and the slot may have changed.
            const char* code = settings_.privacyMode ? pet_error::kPrivacyMode : rollAccessLocked(s2, role2);
            if (!code && !canTakeSlotLocked(s2, role2)) code = pet_error::kTooManyViewers;
            // Retention skips a pinned clip, so the clip is pinned before its file is checked: retention can't
            // delete it between that check and the stream's start. Unpinned again if the start is refused; a stream
            // that starts keeps the pin until joinRetiredPlaybacks.
            store->pinClip(*m.clipId);
            // v3 §5.2: a clip being deleted, or deleted since it was looked up, is not_found. A delete that starts
            // after this point stops the stream made here (with its 0x17) before the files go.
            if (!code && (deleting_clips_.count(*m.clipId) != 0 || GetFileAttributesW(path->c_str()) == INVALID_FILE_ATTRIBUTES)) {
                code = pet_error::kNotFound;
            }
            if (code) {
                store->unpinClip(*m.clipId);
                sendCommandResult(conn, rid, code);
                return;
            }
            // A new start replaces the session's playback (§2.2): the old stream -> stopped, with no requestId.
            if (s2.playback) {
                s2.playback->stop(std::nullopt, std::nullopt);
                retirePlaybackLocked(std::move(s2.playback));
                s2.playback.reset();
            }
            RollPlayback::Params p;
            p.conn = conn;
            p.streamId = ++s2.last_stream_id;
            p.clipId = *m.clipId;
            p.path = *path;
            p.durationSec = sidecar->durationSec;
            p.modeRaw = sidecar->modeRaw;
            p.sbsLayoutRaw = sidecar->sbsLayoutRaw;
            p.startSec = m.positionSec.value_or(0.0);
            p.idleMs = opts_.playback_idle_ms;
            p.backlogBytes = opts_.playback_backlog_bytes;
            if (opts_.roll_test_hook) p.beforeOpen = [this] { rollTestHook("playback_open"); };
            // One video stream per session (§5.9): live video stops, and comes back only when the device asks
            // (0x0E watch:true).
            s2.wants_live = false;
            std::weak_ptr<TlsConnection> weak_conn = conn;
            const uint32_t stream_id = p.streamId;
            s2.playback = RollPlayback::start(p, rid, [this, weak_conn, stream_id] {
                // The stream freed itself (it couldn't read the clip, or it was idle): its slot goes back.
                {
                    std::lock_guard<std::recursive_mutex> lk2(m_);
                    const std::shared_ptr<TlsConnection> c = weak_conn.lock();
                    Session* sp2 = c ? findSessionLocked(*c) : nullptr;
                    if (!sp2 || !sp2->playback || sp2->playback->streamId() != stream_id) return;
                    retirePlaybackLocked(std::move(sp2->playback));
                    sp2->playback.reset();
                    reevaluateLocked();
                }
                notifyPetStateChanged();
            });
            reevaluateLocked(); // the playback keeps (or takes) the session's slot; owners' watchingNow says so
        }
    };
}

} // namespace kvhost
