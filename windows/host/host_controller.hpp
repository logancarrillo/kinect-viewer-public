// Glue between the core's mapped frames, the live encoder tiers (LiveTierSet, protocol v3 §6) and
// HostStreamServer (the streaming half of macos/App/HostController.swift). A tier encodes only while
// a session watches it (plus its 60 s hold); `high` is today's v1/v2 stream byte for byte. It also
// owns the camera-roll recorder (RollRecorder), which keeps `medium` open while the roll is armed and is
// fed its AUs, the live frame for each clip's thumbnail, the sensor and frozen-view state, and depth for
// motion.
#pragma once

#include "host_power.hpp"
#include "router_mapper.hpp"
#include "kinect_core_c.h"
#include "live_tiers.hpp"
#include "roll_recorder.hpp"
#include "stream_server.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kvhost {

// v3 §10: the LAN IPv4 a test run's host uses as the router forward's internal client instead of reading this PC's
// adapters. A made-up private address, for tests only; nothing is ever sent to it.
constexpr const char* kTestLanIPv4 = "192.168.77.10";

class HostController final : private LiveVideoControl {
public:
    struct Options {
        HostStreamServer::Options server;
        EncoderOptions encoder;
        // v3 §6.1: a tier stays open this long after its last session leaves. Tests shorten it.
        std::chrono::milliseconds tier_hold{60000};
        // Tests turn hardware off for a tier to take the software MFT.
        std::array<bool, kLiveTierCount> tier_hardware_allowed{true, true, true};
        // The camera roll. roll.dir empty (the default) means no roll at all: nothing is recorded, nothing
        // is opened, and devices see a host without one (v3 §10 "Test isolation": a test or temp-state host
        // never falls back to a real camera roll). The PC app passes its folder here;
        // roll.recordingsDir is the depth/IR folder, refused as or around the roll (pet-camera.md §6.5).
        RollRecorder::Options roll;
        // A test run (the viewer's --synthetic or a test hook; kinect-cli's --synthetic or --loopback-only). Real
        // colour must never reach a test folder: the recorder refuses every frame that isn't
        // from the synthetic source, and the roll stops with the reason in rollError(). And roll.dir must be new,
        // empty, or hold the test-roll marker (roll::kTestRollMarker), which the host writes when it prepares a
        // test roll; anything else is refused as folder_unavailable with the reason.
        bool test_run = false;
        // The router (v3 §8.2). Tests inject a fake here. Empty: outside a test run (and not loopback-only)
        // the real NATUPnP session; in a test run no router at all, so the status is manual with the steps. A
        // test run never creates the real session, not even for a read-only query, and
        // its outside listener binds 127.0.0.1 only.
        std::function<std::unique_ptr<outside::RouterSession>()> router_factory;
    };

    // What the PC panel shows of the roll's storage: cached counts, never a walk of the folder, and never
    // a wait on the store's lock while retention or a start-up pass runs. available is false without a store.
    struct RollStorageSnapshot {
        bool available = false;
        bool counted = false; // the store has walked the folder at least once
        uint64_t usedBytes = 0, freeBytes = 0, starredBytes = 0;
        uint64_t floorBytes = roll::kDiskFloorBytes;
        int keepDays = roll::kDefaultKeepDays; // the limits shown: the current settings
        uint64_t capBytes = roll::kDefaultCapBytes;
        bool limitsKnown = false;
    };

    HostController();
    ~HostController() override;

    // v3 §10: whether a host started with these options would use the real NATUPnP session: only with no
    // router of the caller's, outside a test run and off loopback.
    static bool usesRealRouter(const Options& opts);

    // As HostStreamServer::start: identity_unusable says pairing.json is there but can't be used.
    bool start(const Options& opts, std::string* err, bool* identity_unusable = nullptr);
    void stop();

    HostStreamServer& server() { return server_; }
    const HostStreamServer& server() const { return server_; }

    // Called for each new mapped BGRA frame (any thread). Copied once, and only when a tier is active.
    void submitFrame(const uint8_t* bgra, const kv_mapped_frame_t& meta, int modeRaw, int sbsRaw, bool clockOverlay);
    // Mode or layout changed on the host: a keyframe and config on every open tier, and a fresh hello.
    void notifyModeChanged(int modeRaw, int sbsRaw, int width, int height);
    void requestKeyframeAndConfig();

    // The high tier's encoder (the v1/v2 stream), or the first open tier's.
    std::string encoderName() const;
    std::string lastError() const;
    uint64_t encodedFrames() const { return encoded_frames_.load(); }
    double lastEncodeMs() const { return last_encode_ms_.load(); }
    std::vector<HostTierStatus> tierStatus() const;

    // ---- protocol v2 state (0x07) ----
    // Each call that changes something pushes a 0x07 with a new stateSeq to every v2+ session (built
    // per session by the server). Recording progress alone (frames, elapsed) goes out at most about
    // once a second; recording start and stop go out at once.
    void setViewState(const HostViewState& v);
    void setPowerSnapshot(const HostPowerModel::Snapshot& p);
    void pushHostState(); // unconditionally (after every phone command, even one that changed nothing)
    HostViewState viewState() const;
    // The 0x07 every session shares, before the server's per-session part (stateSeq, role, availability).
    HostStateSyncMessage buildHostState();
    uint64_t hostStateSeq() const;

    std::function<void(int modeRaw, int sbsRaw)> onClientRequestedMode;
    std::function<void()> onPairingStateChanged;
    std::function<void()> onPetStateChanged; // devices, sessions or pet-camera settings changed
    // v2 phone requests, already gated by the server by role and settings. request_keyframe was
    // handled for the session already and is forwarded too. Called from network threads: hop to one
    // serial thread.
    std::function<void(const RemoteControlCommandMessage&)> onClientRemoteCommand;
    std::function<void(const Orbit3DGestureMessage&)> onClientOrbit3DGesture;
    std::function<void(const SensorPowerControlMessage&)> onClientSensorPowerRequest;
    // The last available session ended (not on shutdown). turn_off is the "Turn the Kinect off when
    // the iPhone disconnects" setting at that moment.
    std::function<void(bool turn_off)> onPhoneDisconnected;
    // A session started watching a tier: a frozen view should submit its frame again so the tier has
    // something to encode (v3 §6.2). Called with the server's lock held: only post from it.
    std::function<void()> onFrameRefreshWanted;

    // ---- the camera roll (pet-camera.md §6) -----------------------------------------------------------
    // True when Options::roll.dir was given and the folder was accepted; rollError() says why not otherwise.
    bool rollAvailable() const;
    std::string rollError() const;
    RollRecorderStatus rollStatus() const; // for the PC panel (also what 0x0B carries)
    // The store, for the PC's roll browser (days, clips, thumbnails, the .mp4 its own player opens). Null
    // without a roll.
    std::shared_ptr<roll::RollStore> rollStore() const;
    // The PC's "record a clip" button (startedBy "This PC"). Refused with the block while blocked.
    StartClipResult startManualClip();
    StopClipResult stopManualClip();
    // The motion feed (pet-camera.md §6.2): the viewer calls this from its loop with
    // each new depth frame from kv_engine_copy_latest_raw16 (512x424 mm, with its sequence number and capture
    // time). live_sensor is false for a replay, whose depth is never motion. Ignored while the roll is off or
    // blocked; a repeated sequence number is ignored and a gap restarts the 3-frame run.
    void submitDepthForMotion(const uint16_t* raw16, uint32_t width, uint32_t height, uint64_t seq, uint64_t ts_ns,
                              bool live_sensor);
    // A clip was finalized (the recorder's thread): the PC's browser may refresh.
    std::function<void(const std::string& clipId)> onRollClipFinalized;

    // The retention limits are known (pet-camera.json held them, or the owner set them). Until then the panel
    // says "Retention starts when you confirm the limits." with a Confirm button, which calls confirmRollLimits:
    // the limits shown (the current settings) are written and retention starts. False, with the reason in err,
    // when they can't be saved (they hold from memory then, and the write is retried). The reason goes in err
    // rather than rollError(), which would take the whole roll off the wire.
    bool rollLimitsKnown() const;
    bool confirmRollLimits(std::string* err = nullptr);
    // The PC's player holds a clip (see HostStreamServer::pinRollClipForPc). 0 when it isn't found.
    uint64_t pinRollClipForPc(const std::string& clipId);
    void unpinRollClipForPc(uint64_t token);
    // A phone owner is deleting a clip the PC holds: close the player, then unpin, within 2 s. Called from a host
    // thread with no lock held: PostMessage to the UI thread.
    std::function<void(const std::string& clipId)> onRollClipDeletedForPc;
    RollStorageSnapshot rollStorageSnapshot() const;
    // D6: the one predicate for "the roll is on and usable", which skips turn-off-on-disconnect. The host's
    // own skip uses it, and the PC panel shows its note exactly when it is true.
    bool turnOffSkippedForRoll() const;

    // ---- outside the home (v3 §8; a fixed interface) --------------------------------------------------
    // Cheap, and never waits on the router (the mapper has a worker of its own).
    OutsideView outsideView() const;
    // Refused (with err) unless sharing is on, or while pet-camera.json can't be read.
    bool setAllowOutside(bool on, std::string* err);
    bool setOutsidePort(uint16_t port, std::string* err); // 1024..65535; re-opens the listener and the mapping
    bool setOutsidePublicAddress(const std::string& addr, std::string* err); // "" clears; only a public IPv4
    void acknowledgeAddressChange(const std::string& shownHost); // address_changed -> mapped, for that address (§8.3)
    // pet-camera.md §4 "Remove mapping now": see HostStreamServer::removeOutsideMappingNow.
    bool removeOutsideMappingNow(std::string* err);
    // "Map the port again": see HostStreamServer::remapOutsideNow.
    bool remapOutsideNow(std::string* err);
    // At most one open; 10 min; closes any older one.
    std::optional<OutsideInviteView> createOutsideInvite(PetRole role, std::string* err);
    void cancelOutsideInvite();
    std::optional<OutsideInviteView> currentOutsideInvite() const;
    std::string outsideEndpointLink() const; // kv2://endpoint?... or "" when no endpoint
    std::vector<outside::LogLine> connectionLog(int days) const; // the last N days, newest first
    // Called from a host thread on any change; the PC posts to its UI thread.
    std::function<void()> onOutsideChanged;

private:
    // LiveVideoControl (the server's lock is held).
    void wantTier(LiveTier t) override;
    void releaseTier(LiveTier t) override;
    void requestTierKeyframe(LiveTier t) override;
    uint32_t nextLiveEpoch() override;

    void onTierOutput(const TierAccessUnit& au);
    HelloServerMessage helloSnapshot();

    HostStreamServer server_;
    Options opts_;
    std::unique_ptr<RollRecorder> roll_;
    std::string roll_error_; // under m_: the recorder's thread sets it when the folder can't be prepared
    std::atomic<uint32_t> epochs_{0}; // the host-wide live epoch counter (v3 §6.2)
    std::unique_ptr<LiveTierSet> tiers_;

    mutable std::mutex m_; // the hello snapshot and diagnostics (never held while calling server_)
    int snap_mode_ = 0, snap_sbs_ = 0, snap_w_ = 0, snap_h_ = 0;
    std::string high_encoder_name_;
    std::atomic<uint64_t> encoded_frames_{0};
    std::atomic<double> last_encode_ms_{0.0};

    mutable std::mutex state_m_; // the 0x07 parts below (never held while calling server_)
    HostViewState view_;
    HostPowerModel::Snapshot power_;
    uint64_t last_progress_push_ns_ = 0;
};

} // namespace kvhost
