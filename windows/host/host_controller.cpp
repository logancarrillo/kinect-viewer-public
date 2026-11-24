#include "host_controller.hpp"

#include <chrono>

namespace kvhost {

namespace {
uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

HostController::HostController() = default;

bool HostController::usesRealRouter(const Options& opts) {
    return !opts.router_factory && !opts.server.router_opener && !opts.test_run && !opts.server.loopback_only;
}

HostController::~HostController() {
    stop();
}

bool HostController::start(const Options& opts, std::string* err, bool* identity_unusable) {
    opts_ = opts;
    LiveTierSet::Options to;
    to.encoder = opts.encoder;
    to.hold = opts.tier_hold;
    to.hardware_allowed = opts.tier_hardware_allowed;
    tiers_ = std::make_unique<LiveTierSet>(to, epochs_, [this](const TierAccessUnit& au) { onTierOutput(au); });

    server_.setLiveVideo(this);
    server_.onHelloSnapshot = [this]() { return helloSnapshot(); };
    server_.onClientRequestedMode = [this](int mode, int sbs) {
        if (onClientRequestedMode) onClientRequestedMode(mode, sbs);
    };
    server_.onPairingStateChanged = [this]() {
        if (onPairingStateChanged) onPairingStateChanged();
    };
    server_.onPetStateChanged = [this]() {
        if (onPetStateChanged) onPetStateChanged();
    };
    server_.onClientConnectedInitialState = [this]() { return buildHostState(); };
    server_.onClientRemoteCommand = [this](const RemoteControlCommandMessage& cmd) {
        if (onClientRemoteCommand) onClientRemoteCommand(cmd);
    };
    server_.onClientOrbit3DGesture = [this](const Orbit3DGestureMessage& o) {
        if (onClientOrbit3DGesture) onClientOrbit3DGesture(o);
    };
    server_.onClientSensorPowerRequest = [this](const SensorPowerControlMessage& r) {
        if (onClientSensorPowerRequest) onClientSensorPowerRequest(r);
    };
    server_.onPairedClientDisconnected = [this]() {
        // v3 §2.3, D6: skipped while the camera roll is on, so motion and scheduled recording don't end
        // whenever nobody is watching (and the roll or privacy mode can't power the Kinect off this way).
        // The one predicate the PC panel's note uses too.
        if (onPhoneDisconnected) onPhoneDisconnected(server_.turnOffKinectOnPhoneDisconnect() && !turnOffSkippedForRoll());
    };
    server_.onRollClipDeletedForPc = [this](const std::string& clipId) {
        if (onRollClipDeletedForPc) onRollClipDeletedForPc(clipId);
    };
    server_.onOutsideChanged = [this]() {
        if (onOutsideChanged) onOutsideChanged();
    };
    // W7: the roll, before the server starts, so the first hello already says whether there is one. Only
    // the folder the caller named is ever used (v3 §10); a folder refused (§6.5) leaves the host without a
    // roll and the reason in rollError().
    roll_.reset();
    roll_error_.clear();
    server_.setRollFolderUnavailable(false);
    if (!opts.roll.dir.empty()) {
        auto rec = std::make_unique<RollRecorder>();
        rec->onStatusChanged = [this]() { server_.rollStatusChanged(); };
        rec->onArmedChanged = [this](bool armed) {
            // pet-camera.md §6.1: `medium` runs whenever the roll is armed, even with nobody watching.
            if (!tiers_) return;
            if (armed) tiers_->want(LiveTier::Medium);
            else tiers_->release(LiveTier::Medium);
        };
        rec->onKeyframeWanted = [this]() {
            if (tiers_) tiers_->requestKeyframe(LiveTier::Medium, /*with_config=*/false);
        };
        rec->onClipFinalized = [this](const std::string& id) {
            if (onRollClipFinalized) onRollClipFinalized(id);
        };
        rec->onPrepared = [this]() { server_.rollPrepared(); };
        rec->onUnavailable = [this](const std::string& why) {
            {
                std::lock_guard<std::mutex> lk(m_);
                roll_error_ = why;
            }
            server_.setRollFolderUnavailable(true); // fail closed: folder_unavailable (v3 §5.1)
        };
        std::string rerr;
        RollRecorder::Options ro = opts.roll;
        ro.testRun = ro.testRun || opts.test_run; // synthetic frames only, into a test roll
        if (rec->start(ro, &rerr)) {
            {
                std::lock_guard<std::mutex> lk(state_m_);
                rec->setSensorOn(power_.state == SensorPowerState::On);
                rec->setViewFrozen(view_.isHostFrozen);
            }
            roll_ = std::move(rec);
            server_.setRollRecorder(roll_.get());
        } else {
            {
                std::lock_guard<std::mutex> lk(m_); // never held while calling server_
                roll_error_ = rerr.empty() ? std::string("the camera roll couldn't start") : rerr;
            }
            // The owner sees why on the PC (rollError, rollStatus) and on devices (0x0B folder_unavailable).
            server_.setRollFolderUnavailable(true);
        }
    }
    // W8 (v3 §8.2, §10): the router the host may use. A test's fake when given; otherwise the real NATUPnP
    // session only outside a test run and off loopback. A test run gets no router (manual), and its outside
    // listener stays on 127.0.0.1.
    HostStreamServer::Options so = opts.server;
    if (opts.router_factory) {
        so.router_opener = opts.router_factory;
    } else if (usesRealRouter(opts)) {
        so.router_opener = [] { return outside::openNatUpnpSession(); };
        // v3 §8.3: the gateway of the forward's interface, pinned at turn-on and checked at an address change, and
        // that gateway's own answers for the public address (a search, its description and a SOAP call per WAN
        // service, given up after 8 s; a home network answers in well under one). A whole pass also reads NATUPnP
        // and the neighbour table, so it can take longer than that.
        if (!so.read_gateway) so.read_gateway = [](const std::string& lan) { return outside::readGatewayOn(lan); };
        if (!so.ask_gateway_wan) {
            // The description that answered last (the mapper keeps it, and the host saves it across restarts) is tried
            // first, straight to the gateway.
            so.ask_gateway_wan = [](const std::string& ip, const std::string& lan, std::string& lastGood) {
                return outside::askGatewayForWan(ip, lan, 8'000, lastGood);
            };
        }
    }
    if (opts.test_run) so.outside_loopback_only = true;
    // v3 §10: a test run never reads this PC's adapters for the forward's internal client, so no test depends on
    // this PC's network and the real LAN address never reaches a test's output. A test may pass its own.
    if (opts.test_run && !so.outside_lan_address) {
        so.outside_lan_address = [] { return std::optional<std::string>(kTestLanIPv4); };
    }
    if (!server_.start(so, err, identity_unusable)) {
        server_.setRollRecorder(nullptr);
        if (roll_) roll_->stop();
        roll_.reset();
        server_.setLiveVideo(nullptr);
        tiers_->close();
        tiers_.reset();
        return false;
    }
    return true;
}

void HostController::stop() {
    // The server first: after its shutdown no session wants a tier and no AU is delivered. Then the roll,
    // whose open clip is finalized with what `medium` gave it (pet-camera.md §6.1 "the app exits"). Then the
    // tiers, whose output callbacks only reach the (stopped) server and recorder.
    server_.shutdown();
    if (roll_) roll_->stop();
    if (tiers_) tiers_->close();
    server_.setLiveVideo(nullptr);
}

// ---- the camera roll -----------------------------------------------------------------------------------

bool HostController::rollAvailable() const {
    return roll_ != nullptr && rollError().empty();
}

std::string HostController::rollError() const {
    std::lock_guard<std::mutex> lk(m_);
    return roll_error_;
}

RollRecorderStatus HostController::rollStatus() const {
    RollRecorderStatus s = roll_ ? roll_->status() : RollRecorderStatus{};
    s.limitsKnown = server_.rollLimitsKnown(); // the server's, which the recorder learns a moment later
    const std::string err = rollError();
    if (!err.empty()) {
        s.folderUnavailable = true;
        s.preparing = false;
        s.lastError = err;
    }
    return s;
}

std::shared_ptr<roll::RollStore> HostController::rollStore() const {
    return roll_ ? roll_->store() : nullptr;
}

bool HostController::rollLimitsKnown() const { return server_.rollLimitsKnown(); }

bool HostController::confirmRollLimits(std::string* err) { return server_.confirmRollLimits(err); }

uint64_t HostController::pinRollClipForPc(const std::string& clipId) { return server_.pinRollClipForPc(clipId); }

void HostController::unpinRollClipForPc(uint64_t token) { server_.unpinRollClipForPc(token); }

HostController::RollStorageSnapshot HostController::rollStorageSnapshot() const {
    RollStorageSnapshot snap;
    const PetSettings ps = server_.petSettings();
    snap.keepDays = ps.rollKeepDays;
    snap.capBytes = ps.rollCapBytes;
    snap.limitsKnown = ps.rollLimitsKnown;
    const std::shared_ptr<roll::RollStore> store = rollStore();
    if (!store) return snap;
    // The store's cached counts (a leaf lock only), never a walk and never the store's own lock.
    const roll::RollStorageCounts c = store->counts();
    snap.available = true;
    snap.counted = c.counted;
    snap.usedBytes = c.usedBytes;
    snap.starredBytes = c.starredBytes;
    snap.freeBytes = c.freeBytes.value_or(0);
    return snap;
}

bool HostController::turnOffSkippedForRoll() const {
    // D6: skipped only while the roll is on and usable, so motion and scheduled recording don't end
    // whenever nobody is watching. A roll that is off, has no folder, or whose folder was refused or couldn't
    // be prepared records nothing, and the Kinect turns off as the setting says.
    // rollError() holds a folder refused at start, one that couldn't be prepared, and a test run's refusal.
    if (!roll_ || !rollError().empty()) return false;
    return server_.petSettings().rollEnabled;
}

// ---- outside the home (W8) -------------------------------------------------------------------------------

OutsideView HostController::outsideView() const { return server_.outsideView(); }
bool HostController::setAllowOutside(bool on, std::string* err) { return server_.setAllowOutside(on, err); }
bool HostController::setOutsidePort(uint16_t port, std::string* err) { return server_.setOutsidePort(port, err); }
bool HostController::setOutsidePublicAddress(const std::string& addr, std::string* err) {
    return server_.setOutsidePublicAddress(addr, err);
}
void HostController::acknowledgeAddressChange(const std::string& shownHost) { server_.acknowledgeAddressChange(shownHost); }
bool HostController::removeOutsideMappingNow(std::string* err) { return server_.removeOutsideMappingNow(err); }
bool HostController::remapOutsideNow(std::string* err) { return server_.remapOutsideNow(err); }
std::optional<OutsideInviteView> HostController::createOutsideInvite(PetRole role, std::string* err) {
    return server_.createOutsideInvite(role, err);
}
void HostController::cancelOutsideInvite() { server_.cancelOutsideInvite(); }
std::optional<OutsideInviteView> HostController::currentOutsideInvite() const { return server_.currentOutsideInvite(); }
std::string HostController::outsideEndpointLink() const { return server_.outsideEndpointLink(); }
std::vector<outside::LogLine> HostController::connectionLog(int days) const { return server_.connectionLog(days); }

StartClipResult HostController::startManualClip() {
    if (!roll_) {
        StartClipResult r;
        r.refused = RollBlock::RollDisabled;
        return r;
    }
    return roll_->startClip(ClipStartedBy::pc());
}

StopClipResult HostController::stopManualClip() {
    return roll_ ? roll_->stopClip() : StopClipResult::Ok;
}

void HostController::submitDepthForMotion(const uint16_t* raw16, uint32_t width, uint32_t height, uint64_t seq,
                                          uint64_t ts_ns, bool live_sensor) {
    if (roll_) roll_->submitDepth(raw16, width, height, seq, ts_ns, live_sensor);
}

// ---- LiveVideoControl ------------------------------------------------------------------------------

void HostController::wantTier(LiveTier t) {
    if (!tiers_) return;
    tiers_->want(t);
    if (onFrameRefreshWanted) onFrameRefreshWanted();
}

void HostController::releaseTier(LiveTier t) {
    if (tiers_) tiers_->release(t);
}

void HostController::requestTierKeyframe(LiveTier t) {
    if (tiers_) tiers_->requestKeyframe(t, /*with_config=*/false); // the server sends its session's 0x02
}

uint32_t HostController::nextLiveEpoch() {
    return epochs_.fetch_add(1) + 1; // the same counter the tiers number their epochs from
}

// ---- state ----------------------------------------------------------------------------------------------

HelloServerMessage HostController::helloSnapshot() {
    HelloServerMessage h;
    std::lock_guard<std::mutex> lk(m_);
    h.modeRaw = snap_mode_;
    h.sbsLayoutRaw = snap_sbs_;
    h.width = snap_w_;
    h.height = snap_h_;
    // The server sets protocolVersion, the role's allowPhoneModeSwitch, codecRaw and petCamera.
    h.protocolVersion = kHostProtocolVersion;
    h.codecRaw = static_cast<uint8_t>(CodecKind::H264);
    return h;
}

HostStateSyncMessage HostController::buildHostState() {
    // The shared part only; HostStreamServer adds stateSeq and the per-session permissions. It reads
    // nothing from the server, so it may run under the server's lock.
    HostStateSyncMessage m;
    std::lock_guard<std::mutex> lk(state_m_);
    m.protocolVersion = kHostProtocolVersion;
    m.powerState = power_.state;
    m.inUseHolder = power_.state == SensorPowerState::InUseElsewhere ? power_.inUseHolder : std::nullopt;
    m.errorMessage = power_.state == SensorPowerState::Error ? power_.errorMessage : std::nullopt;
    m.modeRaw = view_.modeRaw;
    m.sbsLayoutRaw = view_.sbsLayoutRaw;
    m.cutoffEnabled = view_.cutoffEnabled;
    m.cutoffNearMm = view_.cutoffNearMm;
    m.cutoffFarMm = view_.cutoffFarMm;
    m.isHostFrozen = view_.isHostFrozen;
    m.isRecording = view_.isRecording;
    m.canRecordCurrentMode = view_.canRecordCurrentMode;
    m.recordingDisabledReason = view_.recordingDisabledReason;
    m.recordingReminderNeeded = view_.recordingReminderNeeded;
    m.recordingFrames = view_.recordingFrames;
    m.recordingElapsedSec = view_.recordingElapsedSec;
    m.isReplayingOnHost = view_.isReplayingOnHost;
    return m;
}

uint64_t HostController::hostStateSeq() const {
    return server_.stateSeq();
}

HostViewState HostController::viewState() const {
    std::lock_guard<std::mutex> lk(state_m_);
    return view_;
}

void HostController::pushHostState() {
    // Built and sent under the server's lock, so two pushes from different threads reach the wire in
    // stateSeq order. Nothing is built for a v1 session.
    server_.pushHostState();
}

void HostController::setViewState(const HostViewState& v) {
    bool push = false;
    bool replay_changed = false;
    bool cutoff_changed = false;
    {
        std::lock_guard<std::mutex> lk(state_m_);
        const uint64_t now = nowNs();
        if (!v.sameExceptProgress(view_)) {
            push = true; // a real change, recording start and stop included: at once
            last_progress_push_ns_ = now;
        } else if (v.recordingFrames != view_.recordingFrames || v.recordingElapsedSec != view_.recordingElapsedSec) {
            if (last_progress_push_ns_ == 0 || now - last_progress_push_ns_ >= 1000000000ULL) {
                push = true; // progress only: about 1 Hz
                last_progress_push_ns_ = now;
            }
        }
        replay_changed = v.isReplayingOnHost != view_.isReplayingOnHost;
        cutoff_changed = v.cutoffEnabled != view_.cutoffEnabled || v.cutoffNearMm != view_.cutoffNearMm ||
                         v.cutoffFarMm != view_.cutoffFarMm;
        view_ = v;
    }
    if (roll_) {
        // pet-camera.md §6.2: a frozen view blocks the roll; a cut-off change restarts the motion background.
        roll_->setViewFrozen(v.isHostFrozen);
        if (cutoff_changed) roll_->resetMotion();
    }
    // v3 §2.3: a PC replay makes viewers unavailable (pc_replaying); owners keep getting it.
    if (replay_changed) server_.setPcReplaying(v.isReplayingOnHost);
    if (push) pushHostState();
}

void HostController::setPowerSnapshot(const HostPowerModel::Snapshot& p) {
    {
        std::lock_guard<std::mutex> lk(state_m_);
        power_ = p;
    }
    if (roll_) roll_->setSensorOn(p.state == SensorPowerState::On); // §6.2: the sensor off blocks the roll
    pushHostState(); // every confirmed power state, a repeat included, goes to the phones
}

void HostController::requestKeyframeAndConfig() {
    if (tiers_) tiers_->requestKeyframeAll(/*with_config=*/true);
}

void HostController::notifyModeChanged(int mode, int sbs, int w, int h) {
    {
        std::lock_guard<std::mutex> lk(m_);
        snap_mode_ = mode;
        snap_sbs_ = sbs;
        if (w > 0 && h > 0) {
            snap_w_ = w;
            snap_h_ = h;
        }
    }
    // A mode or size change starts a new epoch on every open tier when its frame arrives; this makes
    // sure a same-size, same-mode source change still sends a keyframe and config, as before.
    requestKeyframeAndConfig();
    server_.notifyModeChanged(helloSnapshot());
}

std::string HostController::encoderName() const {
    if (!tiers_) return std::string();
    const TierDiagnostics high = tiers_->diagnostics(LiveTier::High);
    if (!high.encoder_name.empty()) return high.encoder_name;
    for (LiveTier t : {LiveTier::Medium, LiveTier::Low}) {
        const TierDiagnostics d = tiers_->diagnostics(t);
        if (!d.encoder_name.empty()) return d.encoder_name;
    }
    std::lock_guard<std::mutex> lk(m_);
    return high_encoder_name_;
}

std::string HostController::lastError() const {
    if (!tiers_) return std::string();
    for (LiveTier t : {LiveTier::High, LiveTier::Medium, LiveTier::Low}) {
        const TierDiagnostics d = tiers_->diagnostics(t);
        if (!d.last_error.empty()) return d.last_error;
    }
    return std::string();
}

std::vector<HostTierStatus> HostController::tierStatus() const {
    std::vector<HostTierStatus> out;
    if (!tiers_) return out;
    for (LiveTier t : {LiveTier::High, LiveTier::Medium, LiveTier::Low}) {
        const TierDiagnostics d = tiers_->diagnostics(t);
        HostTierStatus s;
        s.tier = liveTierName(t);
        s.active = d.active;
        s.open = d.open;
        s.encoderName = d.encoder_name;
        s.hardware = d.hardware;
        s.width = d.width;
        s.height = d.height;
        s.epoch = d.epoch;
        s.framesOut = d.frames_out;
        s.forcedKeyframes = d.forced_keyframes;
        s.sessions = server_.sessionsOnTier(t);
        out.push_back(std::move(s));
    }
    return out;
}

void HostController::submitFrame(const uint8_t* bgra, const kv_mapped_frame_t& meta, int mode, int sbs, bool overlay) {
    {
        std::lock_guard<std::mutex> lk(m_);
        snap_mode_ = mode;
        snap_sbs_ = sbs;
        snap_w_ = static_cast<int>(meta.width);
        snap_h_ = static_cast<int>(meta.height);
    }
    // Real colour must never reach a test folder. In a test run a frame that isn't from the
    // synthetic source stops the roll (with the reason in rollError) before this frame can reach `medium` and the
    // recorder, and nothing more is kept or written.
    if (opts_.test_run && !meta.is_synthetic && roll_) {
        const std::string why =
            "a test run was given a frame that isn't from the synthetic source; the camera roll stopped and kept nothing";
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (roll_error_.empty()) {
                roll_error_ = why;
                first = true;
            }
        }
        roll_->refuseRealColour(why);
        if (first) server_.setRollFolderUnavailable(true); // fail closed: folder_unavailable (v3 §5.1)
    }
    if (!tiers_ || !bgra || meta.width == 0 || meta.height == 0) return;
    // Zero copying and encoding unless some session watches (or a tier is in its hold time).
    tiers_->submitFrame(bgra, meta.width, meta.height, meta.stride_bytes, meta.frame_seq, meta.arrival_mach_ns, mode, sbs,
                        overlay);
    // pet-camera.md §6.1: a clip's thumbnail is taken from the live frame about 1 s after it starts. Only
    // when one is due; the recorder keeps nothing else of the frame.
    if (roll_ && roll_->wantsLiveFrame() && (meta.is_synthetic || !opts_.test_run)) {
        roll_->onLiveFrame(bgra, meta.width, meta.height, meta.stride_bytes);
    }
}

void HostController::onTierOutput(const TierAccessUnit& au) {
    // Encoder threads, possibly several tiers at once. Never closes the tier set from here.
    ++encoded_frames_;
    if (au.encode_done_ns > au.capture_ns) last_encode_ms_ = static_cast<double>(au.encode_done_ns - au.capture_ns) / 1e6;
    if (au.tier == LiveTier::High) {
        std::lock_guard<std::mutex> lk(m_);
        if (high_encoder_name_ != au.encoder_name) high_encoder_name_ = au.encoder_name;
    }
    // pet-camera.md §6.1, D4: the roll records `medium` as encoded for live, before the sessions get it.
    if (roll_ && au.tier == LiveTier::Medium) roll_->onMediumAccessUnit(au);
    server_.deliverLiveAccessUnit(au);
}

} // namespace kvhost
