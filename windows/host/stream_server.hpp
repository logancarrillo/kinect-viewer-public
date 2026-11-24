// C++ port of macos/Streaming/HostStreamServer.swift, grown into the protocol v3 pet-camera host
// (docs/protocol-v3-pet-camera.md): the pinned mutual-TLS 1.3 stream listener, now admitting every
// device on the paired-device list (DeviceStore) with many sessions side by side; the 120 s one-try
// pairing listener (QR token or 8-digit code) that adds a device with the role picked on the PC; mDNS;
// per-session negotiation (v1/v2/v3), roles, availability, watcher slots, per-session 0x07 and 0x0B,
// 0x0C commands, 0x0E live-view requests, 0x0F notices; and the per-session live-video fan-out of the
// encoder tiers (LiveTierSet, driven through LiveVideoControl) with per-session back-pressure. It adds
// the camera roll on the wire: the roll fields of 0x0B, the roll actions of 0x0C, listing (0x12/0x13),
// thumbnails (0x14/0x15) and playback (0x16-0x19), with the recorder (RollRecorder) doing the recording.
#pragma once

#include "connection_log.hpp"
#include "crypto_identity.hpp"
#include "device_store.hpp"
#include "host_state.hpp"
#include "live_tiers.hpp"
#include "mdns.hpp"
#include "outside_address.hpp"
#include "outside_limiter.hpp"
#include "outside_mapper.hpp"
#include "remote_invite.hpp"
#include "pet_camera_types.hpp"
#include "pet_settings.hpp"
#include "roll_playback.hpp"
#include "roll_recorder.hpp"
#include "tls_conn.hpp"
#include "wire_protocol.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace kvhost {

constexpr int kMaxSessions = 8;      // v3 §2.2: sessions at once, all devices
constexpr int kMaxWatcherSlots = 4;  // v3 §2.3

struct ActivePairingOffer {
    std::string qrURI;
    std::string shortCodeFormatted;
    std::string shortCodeNormalized;
    Bytes qrToken;
    std::chrono::steady_clock::time_point expiresAt;
    uint16_t port = 0;
    std::string hostIP;
    // v3 D1: the role the PC picked for the device this window pairs (Viewer by default), and the
    // device a Re-pair replaces (it keeps that record's name and role; `role` is then unused).
    PetRole role = PetRole::Viewer;
    std::optional<std::string> rePairDeviceId;
    // The 8-digit code is taken only once the PC user chose "Use a code" for this window
    // (HostStreamServer::allowShortCodeForPairing). Until then the window pairs by QR only: a code attempt is closed
    // unread and burns nothing. The code has 10^8 values and the phone proves it over whatever certificate it saw,
    // so someone on the home network could recover it offline; it is offered only when the person asks for it.
    bool shortCodeAllowed = false;
};

// Outside the home (v3 §8; a fixed interface with the PC app). What the PC panel shows,
// read cheaply and never waiting on the router.
struct OutsideView {
    bool allowed = false;   // the setting (allowOutside)
    bool listening = false; // the outside listener is open
    outside::OutsideStatus status = outside::OutsideStatus::Off; // off/mapped/manual/error/carrier_nat/address_changed
    uint16_t port = 0;
    std::string lanIPv4;    // the internal client (empty if none)
    std::string wanAddress; // as read from the router (empty if not read)
    bool endpointKnown = false; // an outsideEndpoint exists (§8.3)
    std::string endpointHost;
    bool endpointVerified = false; // false = "entered by you, not checked"
    std::string publicAddressEntered;
    std::string sentence;    // the plain-words §8.2/§8.3 sentence for the status (CGNAT text, port taken, ...)
    std::string manualSteps; // "Forward TCP <port> to <LAN IPv4> in your router's settings" when status is manual
    bool checking = false;   // the mapper worker is working on it now
    bool canInvite = false;  // §8.4: listening and an outsideEndpoint exists
    std::string cannotInviteReason;
    // Why the last invite this run made (or found open at start) is no longer open, for the PC's invite window:
    // "" (still open, or none), "used", "expired", "cancelled", "burned" (a wrong code or certificate),
    // "not_saved", "address_changed", "address_gone" (no endpoint any more), "port_changed", "outside_off".
    std::string inviteEnded;
    // The PC user removed the forward ("Remove mapping now") and it stays removed while listening: the panel
    // offers "Map the port again".
    bool mappingRemoved = false;
    // v3 §8.3: what the pinned gateway is known by ("address and hardware address"), or "" when none is pinned (an
    // address change then waits for the PC user). gatewayChecking: a change is being checked (a part couldn't be read
    // yet, so it is read again). gatewayHeld: positive evidence of another router; held until the PC user re-shares.
    std::string gatewayPinKind;
    bool gatewayChecking = false;
    bool gatewayHeld = false;
    // The pinned gateway's own public answer contradicts NATUPnP's address: another device answered as the router.
    // No re-share is offered (outsideEndpointLink is empty) and no acknowledgement applies while it does.
    bool gatewayContradicts = false;
    std::string gatewayAnswer;     // the gateway's own public answer from the last pass that read one
    uint32_t gatewayFailedPasses = 0; // passes in a row that couldn't read a part, for the change being checked
    // A changed address the PC user may re-share now (outsideEndpointLink isn't empty): with a gateway pinned, only
    // one a pass confirmed; with none pinned, the address read.
    bool reshareOffered = false;
};

struct OutsideInviteView {
    std::string uri; // kv2://invite?...
    uint64_t expiresUnix = 0;
    PetRole role = PetRole::Viewer;
};

// What the stream server asks of the live encoders (HostController implements it over LiveTierSet).
// Called with the server's lock held: implementations only set flags and never call back.
class LiveVideoControl {
public:
    virtual ~LiveVideoControl() = default;
    virtual void wantTier(LiveTier t) = 0;            // a session started watching this tier
    virtual void releaseTier(LiveTier t) = 0;         // ...and stopped
    virtual void requestTierKeyframe(LiveTier t) = 0; // coalesced to one per tier per 500 ms
    // A fresh number from the host-wide live epoch counter (v3 §6.2), for a session joining a tier whose
    // current epoch is not above one it already saw. The tier itself keeps its epoch, so the sessions
    // already on it (v1/v2 phones on high) get no new 0x02.
    virtual uint32_t nextLiveEpoch() = 0;
};

class HostStreamServer {
public:
    struct Options {
        std::wstring state_dir;          // default %USERPROFILE%\kinect-viewer\.host-state
        uint16_t fixed_stream_port = 0;  // 0 = ephemeral, like the Mac
        bool loopback_only = false;      // tests: listen on 127.0.0.1 and advertise nothing
        // Test seam: the LAN listeners (stream, pairing) bind 127.0.0.1 and nothing else changes, so a test can
        // check the outside listener's own binding (v3 §10) without loopback_only, and still listen on no LAN.
        bool lan_listeners_loopback_for_test = false;
        bool advertise = true;           // mDNS on/off (off in tests)
        // A session that sends nothing (no ping, no frame) for this long is taken as dead and closed
        // (v3 §2.2, per session). Phones ping every 250 ms. Tests shorten it.
        int client_ping_timeout_ms = 5000;
        // v3 §2.2: a notice-only connection with no ping for this long is closed.
        int notice_only_timeout_ms = 1000;
        // The LAN listeners' caps. Connections still in their TLS handshake on the home stream listener,
        // and connections open on the pairing listener, in all and per source (the full address, IPv6 too: a household
        // shares a /64); past
        // them a new one is closed before TLS and logged as refused_too_many. A pairing connection must send its
        // request within pairing_frame_timeout_ms of its handshake, and is closed when the window closes.
        int lan_handshakes_total = 16;
        int lan_handshakes_per_source = 4;
        int pairing_conns_total = 8;
        int pairing_conns_per_source = 2;
        int pairing_frame_timeout_ms = 10000;
        // v3 §2.2: queued bytes past this close a session as dead. Tests lower it.
        size_t send_queue_cap = kMaxSendQueueBytes;
        // Test seam for the roll's races: called with no lock held at "star" (a device's star_clip, before it
        // is checked and applied), "play_after_find" (a 0x16 start, after the clip was looked up and before the
        // stream is made) and "delete_before_remove" (a delete, after the playbacks stopped and before the
        // files go). A test blocks in it to order two requests.
        std::function<void(const char* where)> roll_test_hook;
        // v3 §2.3, §5.9: a playback paused or ended this long without a command is stopped. Tests shorten it.
        int playback_idle_ms = kPlaybackIdleMs;
        // §5.9 "Pacing": the backlog past which a playback waits. Tests lower it.
        size_t playback_backlog_bytes = kPlaybackBacklogBytes;

        // ---- outside the home (v3 §8) ----
        // The router (§8.2). Empty: no router is ever used, and the status is manual with the steps. Only
        // HostController passes the real NATUPnP session, and never in a test run (v3 §10).
        outside::RouterOpener router_opener;
        // This PC's LAN IPv4, the forward's internal client; empty: internalClientIPv4 (the adapters, no traffic).
        std::function<std::optional<std::string>()> outside_lan_address;
        uint64_t outside_recheck_ms = 10ull * 60 * 1000; // §8.2: every 10 minutes
        outside::PortMapper::Timing router_timing;       // PortMapper's deadlines (tests shorten them)
        // Test seam: which addresses the outside checks call public (outside::AddressPolicy): the router's
        // WAN address, the address the PC user types, and the one pet-camera.json holds. The default is the
        // product's, where the documentation blocks are never public. Only a test program sets it, so that its
        // sample addresses come from those blocks; the viewer never does, not even in a test launch.
        outside::AddressPolicy address_policy_for_test;
        // v3 §8.3: the gateway of the interface that owns the forward's LAN address, to pin it and check a change
        // against it, and that gateway's own answer for the public address. Only HostController passes the real
        // ones (readGatewayOn, askGatewayForWan), and never in a test run; a test passes fakes or its own loopback
        // gateway. Empty: nothing is pinned, and an address change waits for the PC user.
        std::function<outside::GatewayIdentity(const std::string& lanIPv4)> read_gateway;
        // lastGoodLocation: the description URL that last answered (saved in pet-camera.json), asked first; the asker
        // sets it to the one that answers.
        std::function<std::vector<std::string>(const std::string& gatewayIp, const std::string& lanIPv4,
                                               std::string& lastGoodLocation)>
            ask_gateway_wan;
        // How a pass that couldn't read a part is tried again, the last delay repeating: once a minute (tests shorten
        // it).
        std::vector<uint64_t> gateway_retry_ms = {60'000};
        // A change checked this long (held, or a gateway that still can't be read) is checked only every
        // gateway_slow_every_ms from then on: every 5 minutes after 30 (tests shorten it).
        uint64_t gateway_slow_after_ms = 30ull * 60 * 1000;
        uint64_t gateway_slow_every_ms = 5ull * 60 * 1000;
        // The outside listener binds 127.0.0.1 only (loopback_only does too). A test run sets it: no test listener
        // is ever reachable beyond loopback.
        bool outside_loopback_only = false;
        // Test seam (v3 §8.7): whether a default outside port binds on the outside listener's address. The default
        // is the first of up to 20 random ports that does. Empty: the real probe (bind only, never listen, closed
        // at once). A test passes a fake, so nothing binds.
        std::function<bool(uint16_t port)> outside_port_probe;
        int outside_handshake_timeout_ms = 5000; // §8.1, §8.5 (the LAN listeners keep 10 s)
        // An outside connection that hasn't sent its first TLS record by then is dropped, holding no slot longer.
        int outside_first_record_ms = 1500;
        int outside_redeem_timeout_ms = 10000;   // §8.1: the one redemption frame within 10 s
        outside::LimiterConfig outside_limits;   // §8.5 (tests may tighten them)
        outside::OutsideLimiter::Clock outside_limiter_clock = nullptr; // tests: an injected clock
        // Test seam (v3 §10): every listener's peer address, unwrapped, goes through this at accept with the
        // listener's name ("stream", "pairing", "outside"); a non-empty answer replaces it. A test on loopback
        // stands in an outside source this way. Empty: the address accept() read.
        std::function<std::string(const char* listener, const std::string& peer)> test_source_address;
        // The home prefixes the local-source filter reads (§8.0), in place of the adapter read. nullopt: this PC's
        // own (homePrefixesOf its adapters), read again at most every 30 s, and once more for a source they don't
        // hold when the list is over 2 s old.
        std::optional<std::vector<outside::OnLinkPrefix>> on_link_prefixes;
        // Test seam: this PC's adapters, in place of adaptersOfThisPc (nullopt stands for a read that failed). Read
        // through homePrefixesOf, as the real ones are. Ignored when on_link_prefixes is set.
        std::function<std::optional<std::vector<outside::AdapterInfo>>()> adapters_for_test;
        // A read that fails keeps the last list, so a passing failure never turns the home phones away; but only for
        // this long after the last read that worked. Past it, home is loopback alone until a read works again: a
        // list that old may be another network's (the PC moved, and every read since has failed).
        uint32_t home_list_keep_ms = 5 * 60 * 1000;
        // Test seam: this PC's interface addresses as text, in place of the adapter read, for the overlay candidate
        // (outsideCandidates). Only the 100.64.0.0/10 ones are told.
        std::function<std::vector<std::string>()> interface_addresses_for_test;
        uint64_t connection_log_flush_ms = 10'000; // the connection log is written at most this often
        // Test seam (v3 §8.6): runs just before each connection log file write, on the writing thread, with only
        // the log's write lock held. A test makes the write slow here.
        std::function<void()> connection_log_write_hook;
        // Test seam: the 0x08 command bucket's refill, in commands a second (v3 §6.2). Below 0: the product's 4.
        double command_refill_per_second_for_test = -1;
        // Test seam for the outside listener's races, called with no lock held: "before_classify" (a handshake
        // completed, before it is classified under the server lock) and "redeem_committed" (a redemption was
        // accepted and the device added, before its reply is sent). A test closes the invite or turns outside off
        // there.
        std::function<void(const char* where)> outside_test_hook;
        // Test seam: called with each client frame's type on the connection's thread, before it is handled; a test
        // throws from it (std::bad_alloc) to show the connection closes and the host stays up.
        std::function<void(MsgType)> client_frame_hook_for_test;
        // Test seam: called right after a LAN connection is admitted, before its connection object is
        // made; a test throws from it (std::bad_alloc) to show the slot and the socket are given back.
        std::function<void()> after_lan_admit_for_test;
        // Test seam: called right after a LAN connection's start() returned it; a test throws from it to show the
        // connection keeps its socket and its slot (nothing after start() closes or frees them).
        std::function<void()> after_lan_start_for_test;
        // Test seams: told each home connection's on_closed reason, so a row can see it ended
        // through its own close ("closed"), not a socket closed under it; and how long a turned-off or evicted outside
        // handshake stays registered before its job ends.
        std::function<void(const std::string& why)> lan_closed_for_test;
        int aborted_linger_ms_for_test = 0;
        // Test seam (v3 §8.4): the unix-ms clock invites are made, checked and redeemed against. Empty: the real one.
        std::function<uint64_t()> invite_clock_for_test;
        // Test seam: told true when the PC is asked to stay awake (viewing from outside the home turned on) and
        // false when it is let go. Empty: Windows' own power request.
        std::function<void(bool awake)> keep_awake_for_test;
        // A real launch only (the viewer sets it): hold Windows' system-required power request while outside listening is
        // open. Off by default, so no test takes one.
        bool keep_awake = false;
        // Told true when outside listening opens and false when it closes or the host stops (a clean stop), once each
        // way, under the server's lock: the viewer's keep-running watchdog (keep_running.hpp). Must not call back in.
        std::function<void(bool on)> on_outside_listening;
    };

    HostStreamServer();
    ~HostStreamServer();

    // False with err when the host can't serve. identity_unusable is set when that is because pairing.json
    // (this PC's identity) is there but can't be read or parsed: it is kept as pairing.json.bad and left as
    // it was, nothing listens, and no new identity is made (only resetHostIdentity makes one).
    bool start(const Options& opts, std::string* err, bool* identity_unusable = nullptr);
    // Clean exit: v3 sessions get 0x0F host_shutting_down, last-seen times are saved.
    void shutdown();

    // ---- pairing state, as before (now answered from the device list) ---------------------------
    bool isPhonePaired() const; // any device listed
    std::string serviceId() const;
    // The most recently paired (or re-paired) device's certificate hash, first 6 bytes in hex; "None".
    std::string pinnedClientFingerprintShort() const;
    std::string serverFingerprintHex() const;
    // Successful pairings (and re-pairings) since start; the pairing window uses it to say "paired".
    uint64_t pairingsCompleted() const;
    // The text of the last refused pairing (e.g. the 16-device sentence), empty after a success.
    std::string lastPairingRefusal() const;

    // ---- the PC's phone settings (owner devices only, v3 §2.4) ----------------------------------
    bool allowPhoneModeSwitch() const;
    void setAllowPhoneModeSwitch(bool v);
    // Independent of mode switching; on by default. Turning it off sends owners a state with the
    // reason, and their 0x0A requests are answered with the state instead of acted on.
    bool allowPhoneSensorPowerControl() const;
    void setAllowPhoneSensorPowerControl(bool v);
    bool turnOffKinectOnPhoneDisconnect() const;
    void setTurnOffKinectOnPhoneDisconnect(bool v);

    // ---- sessions -------------------------------------------------------------------------------
    // The version of the most recently connected session (1 until its ping says 2 or more; 1 with none).
    int activeClientProtocolVersion() const;
    uint16_t streamPort() const { return stream_port_; }
    bool pairedClientConnected() const { return paired_client_connected_.load(); } // any session
    int sessionCount() const;
    std::string advertisementStatus() const;

    // Closes every session (the old "disconnect"): reported as a disconnect when one was available.
    void disconnectActiveClient();
    // The old whole-PC unpair: revokes every device (v3 §2.4) and rotates serviceId.
    void unpairPhone();

    // ---- devices (v3 §2.1, §3.2; pet-camera.md §3, §5) --------------------------------------------
    std::vector<HostDeviceInfo> devices() const;
    bool devicesFull() const;
    // The four device changes refuse, with err, while devices.json couldn't be read: "Can't change devices
    // until the device list can be read." and the read error (nothing is written over the unread list). err
    // also says why any other refusal happened (no such device, an empty name), and is cleared on success.
    bool renameDevice(const std::string& deviceId, const std::string& name, std::string* err = nullptr);
    // Takes effect at once: the device's sessions get a new hello, 0x07 and 0x0B.
    bool setDeviceRole(const std::string& deviceId, PetRole role, std::string* err = nullptr);
    // From the PC's list: 0x0F revoked to its v3 sessions, then closed. serviceId rotates (and _kv2s
    // stops) only when no device is left.
    bool revokeDevice(const std::string& deviceId, std::string* err = nullptr);
    // How many were revoked; 0 with err when refused.
    size_t revokeAllDevices(std::string* err = nullptr);

    // role: v3 D1 (Viewer by default). rePairDeviceId: "Re-pair" on that device's row.
    bool beginPairingWindow(int duration_sec, ActivePairingOffer& out, std::string* err,
                            PetRole role = PetRole::Viewer,
                            const std::optional<std::string>& rePairDeviceId = std::nullopt);
    void cancelPairingWindow();
    // pet-camera.md §3: the role can be changed in the pairing window until a device pairs. False when
    // no window is open, it is a Re-pair (that keeps the record's role), or role is Unknown.
    bool setPairingRole(PetRole role);
    // The PC user chose "Use a code" in the open pairing window: from now until it closes it also takes the 8-digit
    // code (ActivePairingOffer::shortCodeAllowed). False when no window is open or it has run out.
    bool allowShortCodeForPairing();
    std::optional<ActivePairingOffer> currentPairingOffer() const;

    // ---- pet-camera settings (pet-camera.md §4; saved in pet-camera.json) --------------------------
    // While pet-camera.json couldn't be read (saveError says so) privacy is on, and the setters below refuse a
    // change that would have to be written over it (sharing, the schedule, privacy off); privacy on is kept.
    PetSettings petSettings() const;
    void setSharingEnabled(bool on);
    void setSharingSchedule(const std::vector<SharingScheduleRow>& rows);
    void setPrivacyMode(bool on);
    // The PC's main view is replaying a depth/IR recording (HostViewState::isReplayingOnHost).
    void setPcReplaying(bool replaying);
    bool sharingScheduledOffNow() const;
    // Sessions holding a watcher slot ("Watching now"), live or playing a clip.
    std::vector<PetWatcher> watchingNow() const;
    int sessionsOnTier(LiveTier t) const;

    // ---- the camera roll (pet-camera.md §4, §6; v3 §5.1-§5.2, §5.6-§5.9) ----------------------------
    // Set before start(), already started on its folder. Without one (every host given no roll folder, a
    // test's included) the roll is off: hello says cameraRoll false, 0x0B keeps its defaults and
    // every roll request gets unsupported, exactly as a host without a roll (v3 §5.1, §10 "Test isolation").
    void setRollRecorder(RollRecorder* roll);
    // A camera-roll folder is set but couldn't be used (HostController's rollError() says why). There is no
    // roll on the wire (cameraRoll false, rollAvailable false), but 0x0B carries rollBlockedReason
    // folder_unavailable and start_clip is refused with it, so the owner learns why (v3 §5.1, §5.2).
    void setRollFolderUnavailable(bool unavailable);
    // The recorder prepared its folder (the first time the roll is on): the store to list and play from,
    // and the stars of devices no longer listed are dropped (v3 §3.2).
    void rollPrepared();
    bool rollAvailable() const; // a recorder is set: the roll is on the wire
    // The recorder's status changed (its thread): each v3 session's 0x0B is brought up to date.
    void rollStatusChanged();
    // The PC's roll settings (pet-camera.md §4), saved in pet-camera.json. Each is refused (false) while that
    // file couldn't be read, as sharing is (pet-camera.md §5); and false when the value is out of range.
    bool setRollEnabled(bool on);
    bool setMotionSettings(bool enabled, int sensitivity);
    bool setRecordSchedule(const std::vector<RecordScheduleRow>& rows, bool enabled);
    bool setRollRetention(int keepDays, uint64_t capBytes);
    bool setRollFolderSetting(const std::string& folderUtf8); // read by the PC app at its next start
    // The retention limits are known once a pet-camera.json holding them was read, or the owner set them
    // (setRollRetention), confirmed them (confirmRollLimits) or turned the roll on beside them (setRollEnabled).
    // Until then retention doesn't run and storage_full is never raised against the default cap.
    bool rollLimitsKnown() const;
    // Makes the limits shown (the current settings) the owner's and writes them. False with err when
    // pet-camera.json can't be read yet or the write failed (then they hold from memory and the write is retried).
    bool confirmRollLimits(std::string* err = nullptr);
    // The PC's own player holds a clip. While pinned, retention skips it, and a phone owner's delete first
    // calls onRollClipDeletedForPc (from a host thread, no lock held: post to the UI thread), then waits up to 2 s
    // for the unpin; still pinned, the delete is answered `internal` and nothing is deleted. 0: no such clip.
    uint64_t pinRollClipForPc(const std::string& clipId);
    void unpinRollClipForPc(uint64_t token);
    std::function<void(const std::string& clipId)> onRollClipDeletedForPc;
    // The PC's own star and delete (the PC counts as an owner, deviceId "pc"). Deleting stops every playback
    // of the clip first (0x17 stopped), as an owner's delete_clip does.
    roll::RollError setRollClipStarByPc(const std::string& clipId, bool starred);
    roll::RollError deleteRollClip(const std::string& clipId);
    int playbackCount() const; // playback streams allocated now
    // Test hook: each allocated playback's frames sent so far, and its session's queued bytes (§5.9 "Pacing").
    struct PlaybackProbe {
        uint32_t streamId = 0;
        uint64_t framesSent = 0;
        size_t queuedBytes = 0;
    };
    std::vector<PlaybackProbe> playbackProbes() const;

    // ---- video ------------------------------------------------------------------------------------
    // Set before start(); the server wants/releases tiers as sessions start and stop watching.
    void setLiveVideo(LiveVideoControl* live);
    // One encoded AU of one tier (encoder threads, possibly concurrently for different tiers): sent
    // to every session watching that tier, after that session's 0x02 when it needs one, with today's
    // back-pressure per session (a non-key AU is dropped while the session's previous video is still
    // queued; that session then waits for the next IDR, requested under the 500 ms coalescing rule).
    void deliverLiveAccessUnit(const TierAccessUnit& au);

    void notifyModeChanged(const HelloServerMessage& hello);
    // Builds each v2+ session's 0x07 (onClientConnectedInitialState, then the session's role and
    // availability) and sends it, all under one lock: concurrent pushes go out in stateSeq order.
    void pushHostState();
    uint64_t stateSeq() const;

    // Callbacks (called from network threads, some with the server's lock held: they must not call
    // back into the server's setters).
    // The hello's mode, layout, size and codec; the server fills in the version, the role's
    // allowPhoneModeSwitch and petCamera.
    std::function<HelloServerMessage()> onHelloSnapshot;
    std::function<void(int modeRaw, int sbsLayoutRaw)> onClientRequestedMode;
    std::function<void()> onPairingStateChanged;
    // v2: the host state shared by every session; the server sets stateSeq, the permissions by role
    // and the unavailable presentation per session.
    std::function<HostStateSyncMessage()> onClientConnectedInitialState;
    // Requests the session's role allows. Remote commands other than request_keyframe, and orbit
    // gestures, arrive only from owners while mode switching is allowed; power requests only from owners
    // while power control is allowed (v3 §3.1): a v3 owner also while unavailable (privacy mode, four
    // owners on the slots; its 0x0B says why there's no video), a v1/v2 owner only while available
    // (unavailable, its "off" 0x07 disables the switch and gives the reason).
    std::function<void(const RemoteControlCommandMessage&)> onClientRemoteCommand;
    std::function<void(const Orbit3DGestureMessage&)> onClientOrbit3DGesture;
    std::function<void(const SensorPowerControlMessage&)> onClientSensorPowerRequest;
    // "Turn the Kinect off when the iPhone disconnects" (v3 §2.3): the last available session ended.
    // Not on shutdown, a replacement, a revoke of one device, or a host-initiated close of an
    // unavailable or over-limit session. Called without the server's lock held.
    std::function<void()> onPairedClientDisconnected;
    // Pet-camera state changed (devices, sessions, settings): the PC refreshes its panel. No lock held.
    std::function<void()> onPetStateChanged;

    // ---- outside the home (v3 §8; pet-camera.md §3, §4, §7) --------------------------------------------
    OutsideView outsideView() const;
    // Refused, with err, unless sharing is on, and while pet-camera.json can't be read. On: the outside
    // listener opens on the outside port (a default one is picked once and saved) and the mapping is made.
    // Off: §8.1's four steps.
    bool setAllowOutside(bool on, std::string* err);
    // 1024-65535. Re-opens the listener and the mapping when on; an open invite (it carries the port) closes.
    bool setOutsidePort(uint16_t port, std::string* err);
    // "" clears; only a public IPv4 is accepted (the manual case's "entered by you, not checked").
    bool setOutsidePublicAddress(const std::string& addr, std::string* err);
    // address_changed -> mapped (§8.3), only for the address the PC user was shown (the re-share link's host) while
    // it is still the one read now.
    void acknowledgeAddressChange(const std::string& shownHost);
    // pet-camera.md §4 "Remove mapping now" (v3 §8.7): the router forward is removed on the mapper's worker and
    // the status becomes manual with the steps; the listener stays open. It stays removed (outsideMappingRemoved,
    // saved, so across a restart too): no 10-minute check maps it again,
    // until outside viewing is turned off or remapOutsideNow. Refused, with err, unless listening with a mapping
    // (mapped or address_changed).
    bool removeOutsideMappingNow(std::string* err);
    // "Map the port again": the forward removed by removeOutsideMappingNow is mapped now, and kept by the checks.
    // Refused, with err, unless outside listening is on and the forward was removed by the PC user.
    bool remapOutsideNow(std::string* err);
    // The address the outside listener is bound to ("127.0.0.1" or "::"), "" while closed: read from the socket.
    std::string outsideListenerAddress() const;
    // §8.4: at most one open, for 10 minutes, usable once; a new one closes any older one. Refused, with err,
    // unless outside listening is on and an outsideEndpoint exists.
    std::optional<OutsideInviteView> createOutsideInvite(PetRole role, std::string* err);
    void cancelOutsideInvite();
    std::optional<OutsideInviteView> currentOutsideInvite() const;
    std::string outsideEndpointLink() const; // kv2://endpoint?... or "" when there is no endpoint
    std::vector<outside::LogLine> connectionLog(int days) const; // the last N days, newest first
    // Outside listening, the mapping, the invite or the log changed. From a host thread, no lock held.
    std::function<void()> onOutsideChanged;
    uint16_t outsideListeningPort() const; // 0 while closed
    // Test seams: the 10-minute check now; the limiter; the handshake pool's size (0 while closed).
    void outsideRecheckNowForTest();
    uint64_t outsideRechecksForTest() const; // how many 10-minute checks the mapper's worker has been through
    std::shared_ptr<outside::OutsideLimiter> outsideLimiterForTest() const;
    int outsideHandshakeWorkersForTest() const;
    int outsideHandshakesRunningForTest() const;
    int outsideRedemptionsWaitingForTest() const { return redemptions_waiting_.load(); } // classified, reading the frame

    // Diagnostics.
    uint64_t rejectedHandshakeCount() const { return rejected_handshakes_.load(); }
    // 0x08 commands dropped past a session's bucket (v3 §6.2), since start.
    uint64_t droppedCommandCount() const { return commands_dropped_.load(); }
    uint64_t sentFramesCount() const;
    uint64_t droppedCongestionFrames() const { return dropped_congestion_.load(); }
    double lastClientDisplayLatencyMs() const { return last_display_latency_ms_.load(); }
    double lastClientBarcodeLatencyMs() const { return last_barcode_latency_ms_.load(); }
    double currentBitrateMbps() const { return bitrate_mbps_.load(); }
    std::vector<std::string> reconcileLog() const; // what the start-up reconciliation did
    // A save that failed and hasn't gone through on a retry yet (the device list and pairing.json, or
    // pet-camera.json), as a sentence for the PC's status line; empty when the disk has everything. The
    // change itself is in force from memory all along (fail closed: a revoked device stays refused,
    // privacy mode stays on); the maintenance loop retries (100 ms doubling to 5 s, back to 100 ms after a
    // success), outside the server's lock, and shutdown() once more. It also
    // carries a pet-camera.json that couldn't be read or parsed at start (privacy mode is on because of
    // it) until the settings are next saved.
    std::string saveError() const;

    // ---- test seams ---------------------------------------------------------------------------------
    // The clock the sharing schedule reads (nullopt: the real local time). Like the real clock, a change
    // is picked up by the once-a-second schedule check.
    void setLocalTimeForTest(const std::optional<LocalTime>& t);
    // Runs on the connection's thread after the TLS handshake (the verify callback has accepted the
    // leaf) and before on_ready takes the server lock: a test holds a handshake there while it revokes.
    std::function<void()> testHookBeforeSessionReady;
    // Every later device save stops after devices.json, as a crash between the two writes would, and
    // shutdown writes nothing (a crash has no clean exit). pet-camera.md §5, v3 §10.
    void setCrashBetweenDeviceWritesForTest(bool on);
    // How many state-file writes (device list, pairing.json, pet-camera.json) have been tried since start.
    uint64_t saveAttemptsForTest() const { return save_attempts_.load(); }

private:
    struct Session;
    struct Pending; // a connection closing on its own schedule (notice-only, notified, replaced)

    void handleStreamSocket(SOCKET s, const sockaddr* peer);
    void handlePairingSocket(SOCKET s, const sockaddr* peer);
    // The callbacks of a stream session's connection, for either path (the LAN listener's, or a paired
    // device's connection handed over by the outside listener's pool).
    // outside_gen: for a connection the outside pool hands off, the outside generation it was accepted under.
    TlsConnection::Callbacks streamCallbacksLocked(PetPath path, const std::string& addr, uint64_t outside_gen = 0);
    void onStreamReady(TlsConnection& conn, PetPath path, const std::string& addr, uint64_t outside_gen);
    void onStreamClosed(TlsConnection& conn);
    void handleClientFrame(TlsConnection& conn, MsgType type, std::vector<uint8_t>& payload);
    // m_ held. Takes one of the session's device's 0x08 command tokens: false when it has none left.
    bool takeCommandTokenLocked(Session& s);
    void handlePetCommandLocked(Session& s, const PetCameraCommandMessage& cmd);
    // Camera roll requests. Each checks roles, availability and privacy under m_ and answers a refusal at once; the
    // roll's file work (listing, reading a thumbnail, star, delete, opening a clip) is returned as a task
    // that the caller runs after m_ is released, on the same connection thread, so a slow disk never holds
    // up the fan-out or another session, and this session's answers keep their order.
    std::function<void()> handleRollCommandLocked(Session& s, const PetCameraCommandMessage& cmd);
    std::function<void()> handleRollListLocked(Session& s, const RollListRequestMessage& m);
    std::function<void()> handleRollThumbLocked(Session& s, const RollThumbRequestMessage& m);
    std::function<void()> handlePlaybackLocked(Session& s, const PlaybackRequestMessage& m);
    bool rollReadyLocked() const;
    // A roll is set and its folder isn't refused: the roll is on the wire (cameraRoll), whether or not the
    // folder is prepared yet. Without a store (a folder not made yet) it is an empty roll.
    bool rollConfiguredLocked() const;
    // The roll block with a folder that can't be used, in the §5.1 order (privacy_mode, roll_disabled, then
    // folder_unavailable); nullptr when no folder is set or the roll runs.
    const char* rollFolderBlockLocked() const;
    // nullptr when this session may use the roll now (list, thumbnails, playback, clips and stars), otherwise
    // the 0x0D code: unsupported without a roll; for a viewer pc_replaying, sharing_off or sharing_schedule
    // (v3 §3.1, §7.3). Privacy mode is checked by the requests it refuses, not here.
    const char* rollAccessLocked(const Session& s, PetRole role) const;
    // v3 §2.3 condition 5 for a playback: it holds a slot, one is free, or an owner can take a viewer's.
    bool canTakeSlotLocked(const Session& s, PetRole role) const;
    bool playingLocked(const Session& s) const;
    void pushRollSettingsLocked();
    // Ends the session's playback: with 0x17 stopped and the user-facing sentence for `reason` (v3 §9; a host-initiated stop,
    // §5.9), or with nothing sent (silent: the session is going). The stream's thread is joined later, outside
    // m_ (joinRetiredPlaybacks).
    void endPlaybackLocked(Session& s, const char* reason, bool silent);
    // Stops every playback of clipId with 0x17 stopped "This clip was deleted." and retires it; returns them,
    // to be joined outside m_ before the files are removed.
    std::vector<std::shared_ptr<RollPlayback>> stopPlaybacksOfClipLocked(const std::string& clipId);
    // Deletes a clip for an owner or the PC (v3 §5.2), its playbacks already stopped and the clip marked as
    // being deleted: joins them, stops any stream made since, removes the files, and refreshes storage_full.
    // Never with m_ held.
    roll::RollError deleteClipNow(const std::shared_ptr<roll::RollStore>& store, const std::string& clipId,
                                  std::vector<std::shared_ptr<RollPlayback>> stopped);
    void rollTestHook(const char* where) const;
    bool pcPinnedLocked(const std::string& clipId) const;
    // A phone owner's delete of a clip the PC holds: tells the PC and waits up to 2 s for the unpin. False when it
    // is still pinned (the delete is refused). Never with m_ held.
    bool waitForPcUnpin(const std::string& clipId);
    void joinRetiredPlaybacks(); // never with m_ held
    void retirePlaybackLocked(std::shared_ptr<RollPlayback> p); // to be joined by the maintenance loop at once
    void dropRevokedStars();     // the maintenance thread only: removeStarsByDevice for each device revoked since

    // m_ held. The accepted pairResponse frame, for the caller to send once the new list is written; empty
    // when the request was refused (that answer is already queued, and the refusal logged from addr) or the
    // connection closed.
    std::vector<uint8_t> processPairRequest(TlsConnection& conn, const PairRequestMessage& req, const std::string& addr);
    void closePairingWindowLocked();
    // True (with err) while devices.json couldn't be read: no device change may be made then.
    bool refuseDeviceChangeLocked(std::string* err) const;
    void updateAdvertisementLocked();
    // The saves. A change marks what it needs written (under m_), and flushSaves (never under m_) writes
    // a snapshot of it. A failure is recorded (dirty flag and error) for the retry and the PC, and
    // cleared on success; what memory holds stays in force either way (fail closed).
    void markPairingDirtyLocked();  // pairing.json alone: the phone settings
    void markDevicesDirtyLocked();  // devices.json, then pairing.json with the mirror
    void markSettingsDirtyLocked(); // pet-camera.json
    // privacy-on.pending: a privacy-on asked for while pet-camera.json can't be read (written like the saves).
    void markPrivacyPendingLocked();
    // The marker is there, or whether it is there couldn't be checked: privacy is on whatever pet-camera.json
    // says, and (once that file can be written) it is saved with privacy on. The first successful write of the
    // file removes the marker, whatever privacy it holds by then.
    void applyPrivacyMarkerLocked();
    enum class FlushKind {
        Change,     // right after a change, from the call that made it
        Retry,      // the maintenance loop, after a failure (backing off)
        TouchFlush, // the 10-minute last-seen flush
        Exit,       // shutdown: the device list always (it sets before = written)
    };
    // Takes io_m_, snapshots what is due under m_, writes it with m_ released, then records the result
    // under m_. Writes are serialised by io_m_, so a newer snapshot is never overwritten by an older one.
    // True when the PC's error text changed. Must not be called with m_ held.
    bool flushSaves(FlushKind kind);
    std::string stateErrorsLocked() const;
    void notifyPairingChanged();
    void notifyPetStateChanged();
    void notifyOutsideChanged();

    // ---- outside the home (v3 §8) ----
    // The peer's address, unwrapped (§8.0), through the test seam.
    outside::IpAddress sourceOf(const sockaddr* peer, const char* listener) const;
    // Reads the home prefixes (§8.0) into on_link_, unless a read finished since asked_ns; a read that fails keeps
    // the last list. Called with neither m_ nor home_m_ held: the adapter read is slow.
    void readHomePrefixes(uint64_t asked_ns);
    // §8.0's local-source rule against this PC's home prefixes (cached, read again at most every 30 s, and once more
    // for a source they don't hold when the list is over 2 s old). Called before m_ is taken.
    bool isLocal(const outside::IpAddress& a);
    // A connection log line, queued under m_ (memory only). drainLog records the queue into the log, and must be
    // called with m_ released; the maintenance loop does so every tick and writes the file outside m_.
    void drainLog() const;
    void logLocked(const char* event, PetPath path, const std::string& addr, const std::string& deviceId = {},
                   const std::string& deviceName = {}, const std::string& detail = {}, const std::string& who = {});
    // Outside listening wanted now: started, sharing on, allowOutside, pet-camera.json read, a valid port.
    bool outsideWantedLocked() const;
    // All of outsideWantedLocked but the port: outside viewing is turned on and allowed.
    bool outsideTurnedOnLocked() const;
    // Turned on with no port, because no default port bound (v3 §8.7): an error on the status line.
    bool outsideNoPortLocked() const;
    // v3 §8.7: the default outside port, the first of up to 20 random ports that binds on the outside listener's
    // address (through opts_.outside_port_probe when set); 0 when none did.
    uint16_t pickOutsidePortLocked() const;
    bool outsideListeningLocked() const { return outside_listener_ != nullptr; }
    // Opens or closes the outside listener to match outsideWantedLocked (and re-opens it on a port change).
    void applyOutsideLocked();
    void openOutsideLocked();
    // §8.1 "Turning it off", steps 1-4. The listener and the pool are retired, to be stopped outside m_
    // (joinRetiredOutside).
    void closeOutsideLocked();
    // Reachable from any network for as long as the PC is left running: while outside viewing is
    // on, the PC is asked not to sleep (a system-required power request; the display may still turn off).
    void keepAwakeLocked(bool awake);
    void joinRetiredOutside(); // never with m_ held
    void handleOutsideSocket(SOCKET s, const sockaddr* peer);
    void runOutsideHandshake(const std::shared_ptr<ServerHandshake>& hs, const std::shared_ptr<outside::Admission>& adm,
                             const outside::IpAddress& peer, ServerHandshake::Clock::time_point deadline,
                             ServerHandshake::Clock::time_point firstBy, uint64_t gen);
    // admitted_invite: the serial of the invite the verify callback admitted the leaf for (0: as a listed device).
    void runRedemption(const std::shared_ptr<ServerHandshake>& hs, const std::shared_ptr<outside::Admission>& adm,
                       const outside::IpAddress& peer, uint64_t admitted_invite);
    void onMapperChanged(const outside::MapperState& st);
    // §8.3: the endpoint devices are told of (both roles), or nullopt.
    std::optional<PetEndpoint> outsideEndpointLocked() const;
    // §8.3: the router reads another public address than devices were last told, and the PC user hasn't re-shared it.
    bool addressChangePendingLocked() const;
    bool reshareOfferedLocked() const; // OutsideView::reshareOffered
    // outsideCandidates (v3 §8.3): outsideEndpoint, then this PC's own overlay IPv4 addresses at the outside port,
    // verified false, while outside listening is on. Read from the PC's interfaces each time, at most every 5 s.
    std::vector<PetEndpoint> outsideCandidatesLocked(const std::optional<PetEndpoint>& ep);
    outside::OutsideStatus outsideStatusLocked() const;
    std::string outsideSentenceLocked() const;
    std::string cannotInviteReasonLocked() const;
    std::string inviteLanHostLocked() const;
    std::optional<OutsideInviteView> inviteViewLocked(const outside::Invite& inv) const;
    // §8.4: closes the open invite (InviteStore, so the outside verify callback stops taking any leaf), records
    // why for the PC, and logs it.
    void closeInviteLocked(const char* end, const char* detail);
    // Closes the invite when the endpoint it carries went away or changed (from reevaluateLocked).
    void syncInviteLocked();
    std::string inviteEndLocked() const;
    uint64_t inviteNowMs() const; // unix ms for invites (Options::invite_clock_for_test, or the real clock)
    void maintenanceLoop();
    // devices.json again, after it couldn't be read at start. True when it was read (the list is loaded).
    bool retryDeviceReadLocked(uint64_t now_ns);
    // pet-camera.json again, after it couldn't be read at start. True when it was read (settings adopted).
    bool retrySettingsReadLocked(uint64_t now_ns);
    void expiryLoop();

    Session* findSessionLocked(const TlsConnection& conn);
    const DeviceRecord* deviceOfLocked(const Session& s) const;
    PetRole roleOfLocked(const Session& s) const;
    bool scheduleOffNowLocked() const;
    // The heart of v3 §2.3: roles, availability, slots, presentation (0x07 / 0x0B), video start/stop and
    // tier changes, and the v1 close. Called after anything that can change any of them.
    void reevaluateLocked();
    LiveTier desiredTierLocked(const Session& s) const;
    void startVideoLocked(Session& s, LiveTier t);
    uint32_t epochShownLocked(const Session& s, const TierAccessUnit& au);
    void stopVideoLocked(Session& s);
    void noteDropLocked(Session& s, uint64_t now_ns, std::vector<Session*>& retier);
    HelloServerMessage helloForLocked(const Session& s);
    void sendHelloLocked(Session& s);
    void sendHostStateLocked(Session& s);
    void sendPetStateLocked(Session& s, bool force);
    PetCameraStateMessage petStateForLocked(const Session& s, bool sched_off);
    // Takes the session out of the live list now (no more video, no disconnect report) and closes its
    // connection: with a 0x0F first when it is v3 and a notice is given (its queued video dropped, so the
    // notice is the next thing it reads), then after a flush.
    void dropSessionLocked(Session& s, const char* notice_reason);
    void closeSessionsOfHashLocked(const Bytes& hash, const char* notice_reason);
    void touchDeviceLocked(const Session& s);
    bool anyAvailableLocked() const;
    void updateConnectedFlagLocked();

    mutable std::recursive_mutex m_;
    // Serialises every state-file write (and the snapshot it writes). Always taken before m_.
    std::mutex io_m_;
    Options opts_;
    HostPersistedState state_;
    DeviceStore store_{std::wstring()};
    PetSettings settings_;
    bool pc_replaying_ = false;
    std::optional<LocalTime> test_time_;
    bool crash_test_ = false;
    bool devices_dirty_ = false;  // devices.json / pairing.json are behind memory: a full save is due
    bool settings_dirty_ = false; // pet-camera.json is behind memory
    bool pairing_dirty_ = false;  // pairing.json alone is behind memory (the phone settings)
    // Bumped by each change, so a write that finishes after a newer change leaves it due.
    uint64_t devices_gen_ = 0, pairing_gen_ = 0, settings_gen_ = 0;
    std::string pairing_save_error_;
    uint64_t save_retry_ms_ = 100;    // the retry's back-off: 100 ms doubling to 5 s, reset on success
    uint64_t next_save_retry_ns_ = 0;
    std::string devices_save_error_;
    std::string settings_save_error_;
    std::string settings_load_error_; // pet-camera.json couldn't be read at start (privacy is on); until a save
    // pet-camera.json exists but couldn't be read at start: privacy is on in memory, it is never written
    // (Exit included), changes that would need it written are refused, and the maintenance loop reads it
    // again (back-off as for saves). A privacy-on asked for meanwhile is kept and wins after the read.
    bool settings_unread_ = false;
    bool privacy_on_asked_while_unread_ = false;
    // Camera roll retention (pet-camera.md §5 fail closed): settings_.rollLimitsKnown. The roll's keep days and cap
    // are the user's only once a pet-camera.json holding them was read and parsed (at start or on a retry), or
    // the owner set or confirmed them on the PC. A file that is missing, unreadable, kept as .bad, gone since
    // start, or from before the roll (no roll keys) leaves them unknown, and retention doesn't run: defaults never delete.
    // The PC player's pins (pinRollClipForPc), by token.
    struct PcPin {
        std::string clipId;
        std::shared_ptr<roll::RollStore> store;
    };
    static constexpr int kPcUnpinWaitMs = 2000;
    std::map<uint64_t, PcPin> pc_pins_;
    uint64_t next_pc_pin_ = 0;
    std::condition_variable_any pc_pin_cv_;
    // privacy-on.pending (owner-only, in the state folder) makes that privacy-on durable, so a restart that
    // reads an older "off" from pet-camera.json can't undo it: privacy-on always wins over what is read. It is
    // written outside m_ through flushSaves (retried with the same back-off, shown on the status line), and
    // removed after any successful pet-camera.json write (a failed removal is retried; a leftover marker only
    // forces privacy on). Every such write reflects the marker: it happens only once the file has been read,
    // and the marker is applied to settings_ then (at start or in retrySettingsReadLocked). So a privacy-off
    // written after that is the owner's choice, made after privacy was shown on, and must survive a restart.
    bool privacy_marker_write_due_ = false;
    bool privacy_marker_delete_due_ = false;
    bool privacy_marker_may_exist_ = false; // written, found at start, or not checkable
    // Known to be on disk: found Present at start, or written since (and not removed). "May exist" is not enough
    // for a new privacy-on: a marker that couldn't be checked at start may well be missing, so the request writes it.
    bool privacy_marker_on_disk_ = false;
    uint64_t privacy_marker_gen_ = 0;
    std::string privacy_marker_error_;
    uint64_t settings_read_retry_ms_ = 100;
    uint64_t next_settings_read_ns_ = 0;
    // devices.json couldn't be read at start: the list is empty, nothing device-side is written, no pairing
    // window opens, and the maintenance loop reads it again (back-off as for saves).
    bool devices_unread_ = false;
    // The list in force was rebuilt from pairing.json (DeviceStore::ReconcileCase::Migrated: devices.json missing or
    // unparsable). It records no revoke, so no star is dropped against it (dropRevokedStars).
    bool devices_rebuilt_ = false;
    std::string devices_read_error_;
    uint64_t read_retry_ms_ = 100;
    uint64_t next_read_retry_ns_ = 0;
    std::vector<std::string> reconcile_log_;
    std::unique_ptr<CryptoIdentity> identity_;
    std::shared_ptr<TlsContext> stream_ctx_;
    std::shared_ptr<TlsContext> pairing_ctx_;
    // ---- outside the home (v3 §8) ----
    std::shared_ptr<TlsContext> outside_ctx_; // its own verify callback: a listed device, or any leaf while an invite is open
    std::unique_ptr<TcpListener> outside_listener_; // open while outside listening is on
    void* power_request_ = nullptr; // the system-required power request (HANDLE), made the first time it is held
    bool awake_held_ = false;
    uint16_t outside_port_ = 0;                     // the port it listens on
    std::string outside_error_;                     // the listener couldn't open (shown as status error)
    std::shared_ptr<outside::OutsideLimiter> limiter_; // limits live in memory and reset on restart (§8.5)
    std::unique_ptr<HandshakePool> hs_pool_;
    std::vector<std::unique_ptr<HandshakePool>> retired_pools_; // stopped outside m_
    mutable std::mutex hs_live_m_; // the outside handshakes and redemptions running now, to abort when it turns off
    std::vector<std::shared_ptr<ServerHandshake>> hs_live_;
    // Each live outside handshake's admission, when it was accepted and from where, for evicting the oldest one
    // that has shown no certificate when the global cap is full.
    struct LiveHandshake {
        std::weak_ptr<outside::Admission> admission;
        uint64_t accepted_ns = 0;
        std::string addr;
        std::string prefix; // its IPv6 /48 ("/48"), or "" for IPv4
    };
    std::map<const ServerHandshake*, LiveHandshake> hs_info_;
    std::atomic<uint64_t> outside_gen_{0};
    std::atomic<int> redemptions_waiting_{0}; // bumped (under hs_live_m_) each time the listener closes or moves
    std::unique_ptr<outside::OutsideMapper> mapper_;
    outside::MapperState mapper_state_;
    uint64_t mapper_open_id_ = 0; // the open in force (MapperState::openId): earlier opens' reports are stale
    bool outside_pin_anew_ = false; // The next mapper open pins the router again (the PC user turned it on)
    std::string announced_wan_;   // §8.3: the public address devices were last told (an address change is against it)
    std::unique_ptr<outside::InviteStore> invites_;
    std::string invite_error_;
    bool invite_live_ = false;  // an invite made this run (or found open at start) hasn't been seen to end yet
    std::string invite_host_;   // the endpoint it carries ("" for one from before a restart, until a check)
    uint16_t invite_port_ = 0;
    std::string invite_end_;    // why the last one ended (OutsideView::inviteEnded)
    std::unique_ptr<outside::ConnectionLog> log_; // made once, in the first start(), and never replaced
    mutable std::mutex log_q_m_; // a leaf lock: log_q_ only
    mutable std::vector<std::pair<outside::LogLine, std::string>> log_q_; // (line, who), waiting for drainLog
    mutable std::mutex log_drain_m_; // one drainer at a time (taken before log_q_m_ and the log's own lock, never with m_)
    uint64_t last_log_prune_ns_ = 0;
    // The home prefixes and when they were read: under home_m_ (a leaf lock), never m_, so the accept threads decide
    // "local" before they take m_. home_read_m_ makes one adapter read at a time, and is never held with m_.
    mutable std::mutex home_m_;
    std::mutex home_read_m_;
    std::vector<outside::OnLinkPrefix> on_link_;
    uint64_t on_link_read_ns_ = 0;   // the last read, worked or not
    uint64_t on_link_good_ns_ = 0;   // the last read that worked (on_link_ is from it)
    bool on_link_read_ = false;
    std::vector<outside::IpAddress> overlay_;
    uint64_t overlay_read_ns_ = 0;
    bool overlay_read_ = false;
    TcpListener stream_listener_;
    std::unique_ptr<TcpListener> pairing_listener_;
    std::vector<std::unique_ptr<TcpListener>> retired_listeners_; // stopped, joined outside the lock
    std::vector<std::shared_ptr<TlsConnection>> conns_;            // every live connection
    // What the LAN listeners hold now, by source key and in all.
    struct LanCount {
        std::map<std::string, int> bySource;
        int total = 0;
    };
    LanCount lan_handshakes_, pairing_open_;
    bool lanAdmitLocked(LanCount& c, const std::string& key, int total, int perSource);
    void lanReleaseLocked(LanCount& c, const std::string& key);
    struct PairingConn {
        std::weak_ptr<TlsConnection> conn;
        uint64_t deadline_ns = 0; // set when its handshake completes
        bool spoke = false;       // its request arrived: it is being answered, never cut off
        const char* closed_by_host = nullptr; // why the host closed it before its request, when the host did
    };
    std::vector<std::shared_ptr<PairingConn>> pairing_conns_;
    std::vector<std::shared_ptr<Session>> sessions_;               // arrival order
    std::vector<std::shared_ptr<Pending>> pending_;
    uint64_t next_session_id_ = 0;
    uint64_t slot_counter_ = 0;
    uint64_t state_seq_ = 0; // host-wide, 0x07 and 0x0B
    LiveVideoControl* live_ = nullptr;
    RollRecorder* roll_ = nullptr;
    bool roll_folder_unavailable_ = false;
    bool roll_prepared_ = false; // the recorder's folder is prepared (rollPrepared)
    std::shared_ptr<roll::RollStore> roll_store_;
    std::vector<std::shared_ptr<RollPlayback>> retired_playbacks_; // stopped, their threads not joined yet
    std::vector<std::string> star_drops_; // device ids revoked, whose stars the roll still has to drop (v3 §3.2)
    // The roll's stars are reconciled with the device list (v3 §3.2 step 5): a device revoked while no roll ran
    // (or before this run) still has stars in the sidecars. Due at start and whenever a roll store is set or the
    // device list is read at last; done in dropRevokedStars, outside m_, for the hashes devices.json records as
    // revoked only, and never on an unread or rebuilt list.
    bool star_reconcile_due_ = false;
    // Serializes every star write against the revoke's star removal (taken before m_ when both are held), so a
    // star_clip that was checked against the device list can't land after that device's stars were dropped.
    std::mutex roll_star_m_;
    // Clips an owner (or the PC) is deleting: a 0x16 start for one gets not_found, so no stream is opened on a
    // clip whose files are about to go (v3 §5.2). A count, for two deletes of one clip at once.
    std::map<std::string, int> deleting_clips_;
    bool shutting_down_ = false;
    std::optional<ActivePairingOffer> offer_;
    uint64_t pairings_completed_ = 0;
    std::string last_pairing_refusal_;
    MdnsAdvertisement stream_ad_;
    MdnsAdvertisement pairing_ad_;
    std::string ad_status_;
    uint16_t stream_port_ = 0;
    bool started_ = false;

    std::thread expiry_thread_;
    std::condition_variable_any expiry_cv_;
    bool expiry_stop_ = false;

    std::thread maintenance_thread_;
    std::condition_variable_any maintenance_cv_;
    bool maintenance_stop_ = false;
    uint64_t last_flush_ns_ = 0;
    uint64_t last_schedule_check_ns_ = 0;
    bool last_schedule_off_ = false;

    std::atomic<bool> paired_client_connected_{false};
    std::atomic<uint64_t> rejected_handshakes_{0};
    std::atomic<uint64_t> commands_dropped_{0};
    // Each device's 0x08 command bucket (v3 §6.2), by its certificate's hash, so it outlives a session: a reconnect
    // doesn't refill it. Under m_.
    struct CommandBucket {
        double tokens = 0;
        uint64_t refill_ns = 0;
    };
    std::map<Bytes, CommandBucket> command_buckets_;
    std::atomic<uint64_t> save_attempts_{0};
    std::atomic<uint64_t> dropped_congestion_{0};
    std::atomic<uint64_t> sent_frames_base_{0};
    std::atomic<double> last_display_latency_ms_{0.0};
    std::atomic<double> last_barcode_latency_ms_{0.0};
    std::atomic<double> bitrate_mbps_{0.0};
    uint64_t sent_bytes_window_ = 0;
    uint64_t last_bitrate_sample_ns_ = 0;
};

} // namespace kvhost
