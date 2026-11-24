#include "stream_hooks.hpp"
#include "capture_exclusion.hpp"
#include "crash_recorder.hpp"

#include "qr_draw.hpp"
#ifdef KV_WITH_RECORDER
#include "recorder.hpp" // kvrec::defaultRecordingsDir: the depth/IR folder the roll refuses (pet-camera.md §6.5)
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>

namespace kvwin {

namespace {

constexpr UINT_PTR kPairTimer = 1;
constexpr int kCancelId = 200;
constexpr int kRoleViewerId = 201; // the pairing window's role choice (pet-camera.md §3)
constexpr int kRoleOwnerId = 202;
constexpr int kUseCodeId = 203; // the pairing window's "Use a code instead": the window takes the 8-digit code too
constexpr const wchar_t* kTempStatePrefix = L"kinect-viewer-test";
IphoneStreamHooks* g_hooks = nullptr;

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

LRESULT CALLBACK PairProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            if (g_hooks) g_hooks->paintPairing(hwnd);
            else {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                EndPaint(hwnd, &ps);
            }
            return 0;
        case WM_TIMER:
            if (g_hooks && wp == kPairTimer) g_hooks->onPairingTimer(hwnd);
            return 0;
        case WM_CTLCOLORSTATIC: // the role choice's radio buttons, on the window's white
            SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        case WM_COMMAND:
            if (LOWORD(wp) == kCancelId) DestroyWindow(hwnd);
            else if (g_hooks && HIWORD(wp) == BN_CLICKED && LOWORD(wp) == kUseCodeId) g_hooks->onPairingUseCode();
            else if (g_hooks && HIWORD(wp) == BN_CLICKED && (LOWORD(wp) == kRoleViewerId || LOWORD(wp) == kRoleOwnerId)) {
                g_hooks->onPairingRole(LOWORD(wp) == kRoleOwnerId ? kvhost::PetRole::Owner : kvhost::PetRole::Viewer);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (g_hooks) g_hooks->onPairingClosed(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

IphoneStreamHooks::IphoneStreamHooks() { g_hooks = this; }

IphoneStreamHooks::~IphoneStreamHooks() {
    detach();
    if (g_hooks == this) g_hooks = nullptr;
}

void IphoneStreamHooks::setUiCallbacks(std::function<void(int, int)> on_phone_mode, std::function<void()> on_state_changed) {
    on_phone_mode_ = std::move(on_phone_mode);
    on_state_changed_ = std::move(on_state_changed);
}

void IphoneStreamHooks::setPhoneCallbacks(std::function<void(bool)> on_power,
                                          std::function<void(const kvhost::RemoteControlCommandMessage&)> on_command) {
    on_phone_power_ = std::move(on_power);
    on_phone_command_ = std::move(on_command);
}

void IphoneStreamHooks::setOrbitCallback(std::function<void(const kvhost::Orbit3DGestureMessage&)> on_orbit) {
    on_phone_orbit_ = std::move(on_orbit);
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// v3 §10 "Test isolation": a --synthetic or test-hook launch keeps every piece of host state in the
// folder the test passed (--state-dir) or in a fresh temp folder of its own, and never reads or writes
// the real .host-state (paired devices, settings). `temp` says it is the temp folder; an empty dir is
// the real one. False (with err) when the host must not start: a --state-dir it may not make owner-only
// (parseLaunchArgs refuses those first; this is checked again right before the start), or a temp folder
// left by an earlier run with this PID that can't be cleared.
static bool hostStateDirFor(const ViewerSettings* settings, const LaunchOptions& lo, std::wstring* dir, bool* temp,
                            std::string* err) {
    *temp = false;
    dir->clear();
    std::wstring why;
    if (!lo.state_dir.empty()) {
        const std::wstring d = normalizeStateDir(lo.state_dir);
        if (!stateDirAcceptable(d, &why)) {
            *err = narrow(why);
            return false;
        }
        *dir = d;
        return true;
    }
    if (!isTestLaunch(settings ? *settings : ViewerSettings{}, lo)) return true; // the real one
    *dir = freshTemporaryStateDir(kTempStatePrefix, &why);
    if (dir->empty()) {
        *err = narrow(why);
        return false;
    }
    *temp = true;
    return true;
}

// pet-camera.md §4: the rollFolder setting, read here (read only) before the host starts, so a real launch knows
// which folder to record into; the host reads the file again itself. A missing file is the default (empty). One
// that is there but can't be read or parsed leaves the folder unknown (false, with why): the run then has no
// roll rather than guess, and would record nothing anyway, as privacy mode is on while that file can't be read
// (§5).
static bool readRollFolderSetting(const std::wstring& state_dir, std::string* folder, std::string* why) {
    folder->clear();
    const std::wstring path = state_dir + L"\\pet-camera.json";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        *why = "pet-camera.json can't be read (error " + std::to_string(e) +
               "), so the camera roll's folder isn't known; the roll starts at a later launch once the file can be read";
        return false;
    }
    LARGE_INTEGER size{};
    std::string text;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024;
    if (ok && size.QuadPart > 0) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size();
    }
    CloseHandle(h);
    kvhost::PetSettings ps;
    if (!ok || !kvhost::fromJson(text, ps)) {
        *why = "pet-camera.json can't be read, so the camera roll's folder isn't known; the roll starts at a later "
               "launch once the file can be read";
        return false;
    }
    *folder = ps.rollFolder;
    return true;
}

void IphoneStreamHooks::attach(kv_engine_t*, ViewerSettings* settings, const LaunchOptions& lo) {
    settings_ = settings;
    lo_ = lo;
    if (!hostStateDirFor(settings, lo, &state_dir_, &state_dir_temp_, &start_error_)) {
        // Never started, so nothing touched the folder; statusLine says why.
        state_dir_.clear();
        return;
    }
    state_dir_ok_ = true;
    if (state_dir_.empty()) state_dir_ = kvhost::defaultHostStateDir(); // the real one
    // The camera roll's folder. v3 §10: a test launch records only into --camera-roll-dir, or has no roll;
    // pet-camera.json's rollFolder is never read for it. A real launch: that setting, or the default.
    roll_note_.clear();
    roll_dir_.clear();
    if (isTestLaunch(settings ? *settings : ViewerSettings{}, lo)) {
        roll_dir_ = cameraRollDirForLaunch(true, lo.camera_roll_dir, std::string());
        if (roll_dir_.empty()) roll_note_ = "a test run without --camera-roll-dir has no camera roll (v3 §10)";
    } else {
        std::string folder;
        if (readRollFolderSetting(state_dir_, &folder, &roll_note_)) {
            roll_dir_ = realLaunchRollFolderNow(folder);
            if (roll_dir_.empty()) roll_note_ = "USERPROFILE isn't set, so the camera roll has no default folder; choose one";
        }
    }
    startHost(false);
}

bool IphoneStreamHooks::startHost(bool again) {
    start_error_.clear();
    identity_unusable_ = false;
    ctl_ = std::make_unique<kvhost::HostController>();
    ctl_->onPetStateChanged = [this]() {
        if (on_pet_state_) on_pet_state_();
        if (on_state_changed_) on_state_changed_();
    };
    ctl_->onFrameRefreshWanted = [this]() {
        // A session started watching a tier: a frozen view submits its frame again (the viewer does
        // that on request_keyframe), so a tier opened during a freeze has a picture to encode.
        if (on_phone_command_) {
            kvhost::RemoteControlCommandMessage key;
            key.action = kvhost::RemoteControlAction::RequestKeyframe;
            on_phone_command_(key);
        }
    };
    ctl_->onClientRequestedMode = [this](int mode, int sbs) {
        if (on_phone_mode_) on_phone_mode_(mode, sbs);
    };
    ctl_->onPairingStateChanged = [this]() {
        if (on_state_changed_) on_state_changed_();
    };
    ctl_->onClientSensorPowerRequest = [this](const kvhost::SensorPowerControlMessage& r) {
        if (on_phone_power_) on_phone_power_(r.turnOn);
    };
    ctl_->onPhoneDisconnected = [this](bool turn_off) {
        // The sensor keeps its state when the phone goes, unless this setting is on (off by default).
        if (turn_off && on_phone_power_) on_phone_power_(false);
    };
    ctl_->onClientRemoteCommand = [this](const kvhost::RemoteControlCommandMessage& cmd) {
        if (on_phone_command_) on_phone_command_(cmd);
    };
    ctl_->onClientOrbit3DGesture = [this](const kvhost::Orbit3DGestureMessage& g) {
        if (on_phone_orbit_) on_phone_orbit_(g); // the 3D camera (the viewer applies it on its UI thread)
    };
    kvhost::HostController::Options o;
    o.server.loopback_only = lo_.serve_loopback_only;
    o.server.advertise = !lo_.serve_loopback_only;
    o.server.state_dir = state_dir_;
    // The roll records only into roll_dir_ (attach chose it); empty means no roll at all. The depth/IR folder is
    // named so the roll refuses it, a folder inside it and one holding it (pet-camera.md §6.5).
    if (!roll_dir_.empty()) {
        o.roll.dir = roll_dir_;
#ifdef KV_WITH_RECORDER
        o.roll.recordingsDir = kvrec::defaultRecordingsDir();
#else
        o.roll.recordingsDir = knownRecordingsDir();
#endif
    }
    // A --synthetic or test-hook launch is a test run for the host too: its recorder refuses every frame
    // that isn't the synthetic source's (the roll stops with the reason in rollError()), and its roll folder
    // must be new, empty or a test roll.
    const bool test_run = isTestLaunch(settings_ ? *settings_ : ViewerSettings{}, lo_);
    o.test_run = test_run;
    host_test_run_ = test_run;
    ctl_->onRollClipFinalized = [this](const std::string& id) {
        if (on_roll_clip_) on_roll_clip_(id);
    };
    // A phone owner is deleting a clip; the host waits for the PC's unpin (a host thread).
    ctl_->onRollClipDeletedForPc = [this](const std::string& id) { rollClipDeletedForPc(id); };
    // Any outside change, reported from a host thread, reaches the panel as a pet-camera change (one coalesced
    // refresh on the UI thread).
    ctl_->onOutsideChanged = [this]() {
        if (on_pet_state_) on_pet_state_();
    };
    // v3 §10: a test run never reaches the real router. The host already never makes the real NATUPnP session
    // in a test run without a router_factory; --test-outside hands it the viewer's scripted fake instead, so the
    // panel can be driven through every status.
    router_injected_ = false;
    if (test_run && lo_.test_outside) {
        if (!test_router_) test_router_ = std::make_shared<OutsideTestRouter>();
        o.router_factory = test_router_->factory();
        router_injected_ = true;
    }
    // While viewing from outside the home is on, a watchdog starts the viewer again if it ends
    // without a clean stop. A real launch only: a test run never makes the real named objects or a watchdog.
    // The crash recorder, the keep-running watchdog's objects and the power request: never in a test run.
    if (!test_run) {
        if (!keep_running_) {
            // A fault from here on is recorded in <state folder>\crash.
            kvhost::installCrashRecorder(state_dir_ + L"\\crash");
            const std::wstring dir = state_dir_;
            // The sensor as launched (--no-autostart), or as last asked: in the block before it is marked filled.
            const bool sensor_off = sensor_requested_ ? !*sensor_requested_ : !lo_.autostart;
            keep_running_ = std::make_shared<kvhost::KeepRunning>(kvhost::keepRunningNames(), GetCurrentProcessId(),
                                                                  kvhost::ownArguments(), sensor_off,
                                                                  [dir] { return kvhost::spawnKeepRunning(dir); });
        }
        o.server.on_outside_listening = [kr = keep_running_](bool on) { kr->set(on); };
        o.server.keep_awake = true;
    }
    // A pairing.json that is there but can't be used fails the start closed (identity_unusable_); no start
    // makes a new identity over it, only resetHostIdentityAndStart.
    if (!ctl_->start(o, &start_error_, &identity_unusable_)) {
        ctl_.reset();
        return false;
    }
    if (again) {
        if (last_power_) ctl_->setPowerSnapshot(*last_power_);
        if (last_view_) ctl_->setViewState(*last_view_);
        onModeOrSourceChanged();
    }
    return true;
}

bool IphoneStreamHooks::retryHostStart(std::string* err) {
    if (ctl_) return true;
    if (!state_dir_ok_) {
        if (err) *err = start_error_;
        return false;
    }
    const bool ok = startHost(true);
    if (!ok && err) *err = start_error_;
    if (on_pet_state_) on_pet_state_();
    if (on_state_changed_) on_state_changed_();
    return ok;
}

bool IphoneStreamHooks::resetHostIdentityAndStart(std::string* err, std::string* note, bool* identity_kept) {
    if (note) note->clear();
    if (identity_kept) *identity_kept = false;
    if (ctl_ || !identity_unusable_ || !state_dir_ok_) {
        // The reset is only for an identity that can't be used: a running host's identity is in use.
        if (err) *err = "this PC's identity isn't unusable, so there is nothing to reset";
        return false;
    }
    std::string reset_err;
    const kvhost::IdentityReset r = kvhost::resetHostIdentity(state_dir_, &reset_err, note);
    if (r == kvhost::IdentityReset::Refused || r == kvhost::IdentityReset::Failed) {
        if (err) *err = "Resetting this PC's identity failed: " + reset_err;
        return false;
    }
    // NotNeeded: pairing.json can be used now (identity_unusable_ was stale), so this is a plain start with it.
    const bool kept = r == kvhost::IdentityReset::NotNeeded;
    if (identity_kept) *identity_kept = kept;
    const bool ok = startHost(true);
    if (!ok && err) {
        *err = (kept ? "pairing.json can be read now, but the streaming host didn't start: "
                     : "This PC has a new identity, but the streaming host didn't start: ") +
               start_error_;
    }
    if (on_pet_state_) on_pet_state_();
    if (on_state_changed_) on_state_changed_();
    return ok;
}

void IphoneStreamHooks::detach() {
    // A clean stop, whatever the host's own shutdown says: the window closing, a logoff, the hooks going away.
    if (keep_running_) keep_running_->set(false);
    if (owner_delete_th_.joinable()) owner_delete_th_.join();
    if (test_device_) test_device_->stop();
    test_device_.reset();
    if (pair_hwnd_) DestroyWindow(pair_hwnd_);
    if (ctl_) {
        ctl_->stop();
        ctl_.reset();
    }
    // A test run's own temp state folder goes with it (only that exact folder; never --state-dir's).
    if (state_dir_temp_) removeTemporaryStateDir(state_dir_, kTempStatePrefix);
}

void IphoneStreamHooks::onFrame(const uint8_t* bgra, const kv_mapped_frame_t& meta, const ViewerSettings& s) {
    if (!ctl_) return;
    ++frames_to_host_;
    ctl_->submitFrame(bgra, meta, static_cast<int>(s.mode), static_cast<int>(s.sbs_layout), s.synthetic && s.clock_overlay);
}

void IphoneStreamHooks::onModeOrSourceChanged() {
    if (!ctl_ || !settings_) return;
    ctl_->notifyModeChanged(static_cast<int>(settings_->mode), static_cast<int>(settings_->sbs_layout), 0, 0);
}

void IphoneStreamHooks::onTick() {}

bool IphoneStreamHooks::isPaired() const { return ctl_ && ctl_->server().isPhonePaired(); }

bool IphoneStreamHooks::allowPhoneModeSwitch() const {
    return !ctl_ || ctl_->server().allowPhoneModeSwitch();
}

void IphoneStreamHooks::setAllowPhoneModeSwitch(bool allow) {
    if (ctl_) ctl_->server().setAllowPhoneModeSwitch(allow);
}

void IphoneStreamHooks::unpair() {
    if (ctl_) ctl_->server().unpairPhone();
}

void IphoneStreamHooks::markCleanStop() {
    if (keep_running_) keep_running_->stopForGood();
}

// A restart never turns the sensor on against the person's choice, so the choice is written down
// before the sensor is closed or opened.
void IphoneStreamHooks::onSensorRequested(bool turn_on) {
    sensor_requested_ = turn_on;
    if (keep_running_) keep_running_->setSensorOff(!turn_on);
}

void IphoneStreamHooks::onPowerChanged(const kvhost::HostPowerModel::Snapshot& p) {
    last_power_ = p; // a host started later (Try again, a reset) is told too
    if (ctl_) ctl_->setPowerSnapshot(p);
}

void IphoneStreamHooks::onViewStateChanged(const kvhost::HostViewState& v) {
    last_view_ = v;
    if (ctl_) ctl_->setViewState(v);
}

void IphoneStreamHooks::pushHostState() {
    if (ctl_) ctl_->pushHostState();
}

bool IphoneStreamHooks::allowPhoneSensorPower() const {
    return !ctl_ || ctl_->server().allowPhoneSensorPowerControl();
}

void IphoneStreamHooks::setAllowPhoneSensorPower(bool allow) {
    if (ctl_) ctl_->server().setAllowPhoneSensorPowerControl(allow);
}

bool IphoneStreamHooks::turnOffOnPhoneDisconnect() const {
    return ctl_ && ctl_->server().turnOffKinectOnPhoneDisconnect();
}

void IphoneStreamHooks::setTurnOffOnPhoneDisconnect(bool on) {
    if (ctl_) ctl_->server().setTurnOffKinectOnPhoneDisconnect(on);
}

std::wstring IphoneStreamHooks::statusLine() const {
    if (!ctl_) return L"Streaming host off: " + widen(start_error_);
    // A save that keeps failing comes first: what the panel shows is in force, but not on disk yet.
    const std::string save_error = ctl_->server().saveError();
    const std::wstring first = save_error.empty() ? std::wstring() : widen(save_error) + L"\n";
    if (state_dir_temp_) {
        // v3 §10: say so, so nobody pairs a real phone into a folder that is deleted on exit.
        return first + L"Test run: pairing and pet-camera state in a temp folder, deleted on exit (" +
               std::to_wstring(ctl_->server().devices().size()) + L" device(s) paired).";
    }
    const auto& srv = ctl_->server();
    wchar_t buf[512];
    const auto devs = srv.devices();
    if (devs.empty()) {
        swprintf(buf, 512, L"Not paired. %ls", widen(srv.advertisementStatus()).c_str());
    } else if (srv.pairedClientConnected()) {
        swprintf(buf, 512, L"%d of %zu device(s) connected. %.1f Mbps, %llu sent, %llu dropped, phone latency %.0f ms\n%ls",
                 srv.sessionCount(), devs.size(), srv.currentBitrateMbps(),
                 static_cast<unsigned long long>(srv.sentFramesCount()),
                 static_cast<unsigned long long>(srv.droppedCongestionFrames()), srv.lastClientDisplayLatencyMs(),
                 widen(ctl_->encoderName()).c_str());
    } else {
        swprintf(buf, 512, L"%zu device(s) paired, none connected. Port %u. %ls", devs.size(), srv.streamPort(),
                 widen(srv.advertisementStatus()).c_str());
    }
    return first + buf;
}

// ---- the pet camera ---------------------------------------------------------------------------------

void IphoneStreamHooks::setPetStateCallback(std::function<void()> on_pet_state) { on_pet_state_ = std::move(on_pet_state); }

std::vector<kvhost::HostDeviceInfo> IphoneStreamHooks::petDevices() const {
    return ctl_ ? ctl_->server().devices() : std::vector<kvhost::HostDeviceInfo>{};
}

namespace {
bool hostOff(const std::string& start_error, std::string* err) {
    if (err) *err = "The streaming host is off: " + start_error;
    return false;
}
} // namespace

bool IphoneStreamHooks::renameDevice(const std::string& deviceId, const std::string& name, std::string* err) {
    return ctl_ ? ctl_->server().renameDevice(deviceId, name, err) : hostOff(start_error_, err);
}

bool IphoneStreamHooks::setDeviceRole(const std::string& deviceId, kvhost::PetRole role, std::string* err) {
    return ctl_ ? ctl_->server().setDeviceRole(deviceId, role, err) : hostOff(start_error_, err);
}

bool IphoneStreamHooks::revokeDevice(const std::string& deviceId, std::string* err) {
    return ctl_ ? ctl_->server().revokeDevice(deviceId, err) : hostOff(start_error_, err);
}

size_t IphoneStreamHooks::revokeAllDevices(std::string* err) {
    if (ctl_) return ctl_->server().revokeAllDevices(err);
    hostOff(start_error_, err);
    return 0;
}

kvhost::PetSettings IphoneStreamHooks::petSettings() const { return ctl_ ? ctl_->server().petSettings() : kvhost::PetSettings{}; }

void IphoneStreamHooks::setSharingEnabled(bool on) {
    if (ctl_) ctl_->server().setSharingEnabled(on);
}

void IphoneStreamHooks::setSharingSchedule(const std::vector<kvhost::SharingScheduleRow>& rows) {
    if (ctl_) ctl_->server().setSharingSchedule(rows);
}

void IphoneStreamHooks::setPrivacyMode(bool on) {
    if (ctl_) ctl_->server().setPrivacyMode(on);
}

bool IphoneStreamHooks::sharingScheduledOffNow() const { return ctl_ && ctl_->server().sharingScheduledOffNow(); }

std::vector<kvhost::PetWatcher> IphoneStreamHooks::watchingNow() const {
    return ctl_ ? ctl_->server().watchingNow() : std::vector<kvhost::PetWatcher>{};
}

std::vector<kvhost::HostTierStatus> IphoneStreamHooks::tierStatus() const {
    return ctl_ ? ctl_->tierStatus() : std::vector<kvhost::HostTierStatus>{};
}

std::string IphoneStreamHooks::petSaveError() const { return ctl_ ? ctl_->server().saveError() : std::string(); }

// ---- the camera roll ---------------------------------------------------------------------------------

void IphoneStreamHooks::setRollCallback(std::function<void(const std::string&)> on_clip_finalized) {
    on_roll_clip_ = std::move(on_clip_finalized);
}

std::string IphoneStreamHooks::rollError() const {
    if (!ctl_) return "the streaming host is off: " + start_error_;
    // With a roll: why it stopped, if it did (a folder that couldn't be prepared, a frame that isn't the
    // synthetic source's in a test run). Empty while it runs.
    if (ctl_->rollAvailable()) return ctl_->rollError();
    if (!roll_note_.empty()) return roll_note_;
    return ctl_->rollError(); // the folder was refused (pet-camera.md §6.5) or couldn't be prepared
}

kvhost::RollRecorderStatus IphoneStreamHooks::rollStatus() const {
    return ctl_ ? ctl_->rollStatus() : kvhost::RollRecorderStatus{};
}

bool IphoneStreamHooks::rollPreparing() const { return ctl_ && ctl_->rollAvailable() && ctl_->rollStatus().preparing; }

std::optional<RollStorageView> IphoneStreamHooks::rollStorageSnapshot() const {
    if (!ctl_ || !ctl_->rollAvailable()) return std::nullopt;
    // The host's cached counts; no walk of the folder and no wait on the store lock.
    const kvhost::HostController::RollStorageSnapshot s = ctl_->rollStorageSnapshot();
    RollStorageView v;
    v.usedBytes = s.usedBytes;
    v.freeBytes = s.freeBytes ? std::optional<uint64_t>(s.freeBytes) : std::nullopt;
    v.starredBytes = s.starredBytes;
    v.floorBytes = s.floorBytes;
    v.capBytes = s.capBytes;
    v.keepDays = s.keepDays;
    v.limitsKnown = s.limitsKnown;
    return v;
}

bool IphoneStreamHooks::rollLimitsKnown() const { return ctl_ && ctl_->rollLimitsKnown(); }

bool IphoneStreamHooks::confirmRollLimits(std::string* err) {
    if (!ctl_) return hostOff(start_error_, err);
    return ctl_->confirmRollLimits(err);
}

bool IphoneStreamHooks::turnOffSkippedForRoll() const { return ctl_ && ctl_->turnOffSkippedForRoll(); }

uint64_t IphoneStreamHooks::pinRollClipForPc(const std::string& clipId) {
    if (!ctl_) return 0;
    std::lock_guard<std::mutex> lk(pins_m_);
    const uint64_t token = ctl_->pinRollClipForPc(clipId);
    if (token) pc_pins_[token] = clipId;
    return token;
}

void IphoneStreamHooks::unpinRollClipForPc(uint64_t token) {
    if (!ctl_ || token == 0) return;
    std::lock_guard<std::mutex> lk(pins_m_);
    if (pc_pins_.erase(token) == 0) return;
    ctl_->unpinRollClipForPc(token);
}

size_t IphoneStreamHooks::rollPcPinsHeld() const {
    std::lock_guard<std::mutex> lk(pins_m_);
    return pc_pins_.size();
}

bool IphoneStreamHooks::setRollClipDeletedCallback(std::function<void(const std::string&)> on_deleted) {
    std::lock_guard<std::mutex> lk(pins_m_);
    on_roll_clip_deleted_ = std::move(on_deleted);
    return true; // the host tells the PC before a phone's delete of a pinned clip
}

void IphoneStreamHooks::rollClipDeletedForPc(const std::string& clipId) {
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lk(pins_m_);
        cb = on_roll_clip_deleted_;
    }
    if (cb) cb(clipId); // the viewer posts it to its UI thread
}

std::shared_ptr<kvhost::roll::RollStore> IphoneStreamHooks::rollStore() const { return ctl_ ? ctl_->rollStore() : nullptr; }

kvhost::StartClipResult IphoneStreamHooks::startManualClip() {
    if (ctl_) return ctl_->startManualClip();
    kvhost::StartClipResult r;
    r.refused = kvhost::RollBlock::RollDisabled;
    return r;
}

kvhost::StopClipResult IphoneStreamHooks::stopManualClip() {
    return ctl_ ? ctl_->stopManualClip() : kvhost::StopClipResult::Ok;
}

void IphoneStreamHooks::submitDepthForMotion(const uint16_t* raw16, uint32_t width, uint32_t height, uint64_t seq,
                                             uint64_t ts_ns, bool live_sensor) {
    if (ctl_) ctl_->submitDepthForMotion(raw16, width, height, seq, ts_ns, live_sensor);
}

bool IphoneStreamHooks::setRollEnabled(bool on) { return ctl_ && ctl_->server().setRollEnabled(on); }

bool IphoneStreamHooks::setMotionSettings(bool enabled, int sensitivity) {
    return ctl_ && ctl_->server().setMotionSettings(enabled, sensitivity);
}

bool IphoneStreamHooks::setRecordSchedule(const std::vector<kvhost::RecordScheduleRow>& rows, bool enabled) {
    return ctl_ && ctl_->server().setRecordSchedule(rows, enabled);
}

bool IphoneStreamHooks::setRollRetention(int keepDays, uint64_t capBytes) {
    return ctl_ && ctl_->server().setRollRetention(keepDays, capBytes);
}

bool IphoneStreamHooks::setRollFolderSetting(const std::string& folderUtf8) {
    return ctl_ && ctl_->server().setRollFolderSetting(folderUtf8);
}

kvhost::roll::RollError IphoneStreamHooks::setRollClipStar(const std::string& clipId, bool starred) {
    return ctl_ ? ctl_->server().setRollClipStarByPc(clipId, starred) : kvhost::roll::RollError::NotFound;
}

kvhost::roll::RollError IphoneStreamHooks::deleteRollClip(const std::string& clipId) {
    return ctl_ ? ctl_->server().deleteRollClip(clipId) : kvhost::roll::RollError::NotFound;
}

// ---- outside the home -----------------------------------------------------------------------------------

namespace {

// kvhost::OutsideView and OutsideInviteView, field for field, into the panel's own types.
OutsidePanelView fromHostView(const kvhost::OutsideView& h) {
    OutsidePanelView v;
    v.allowed = h.allowed;
    v.listening = h.listening;
    v.status = h.status;
    v.port = h.port;
    v.lanIPv4 = h.lanIPv4;
    v.wanAddress = h.wanAddress;
    v.endpointKnown = h.endpointKnown;
    v.endpointHost = h.endpointHost;
    v.endpointVerified = h.endpointVerified;
    v.publicAddressEntered = h.publicAddressEntered;
    v.sentence = h.sentence;
    v.manualSteps = h.manualSteps;
    v.checking = h.checking;
    v.canInvite = h.canInvite;
    v.cannotInviteReason = h.cannotInviteReason;
    v.inviteEnded = h.inviteEnded;
    v.mappingRemoved = h.mappingRemoved;
    v.gatewayPinKind = h.gatewayPinKind;
    v.gatewayChecking = h.gatewayChecking;
    v.gatewayHeld = h.gatewayHeld;
    v.gatewayContradicts = h.gatewayContradicts;
    v.gatewayAnswer = h.gatewayAnswer;
    v.gatewayFailedPasses = h.gatewayFailedPasses;
    v.reshareWithheld = h.status == kvhost::outside::OutsideStatus::AddressChanged && !h.reshareOffered;
    return v;
}

OutsideInvitePanelView fromHostInvite(const kvhost::OutsideInviteView& i) {
    OutsideInvitePanelView v;
    v.uri = i.uri;
    v.expiresUnix = i.expiresUnix;
    v.role = i.role;
    return v;
}

} // namespace

bool IphoneStreamHooks::outsideAvailable() const { return true; }

OutsidePanelView IphoneStreamHooks::outsideView() const { return ctl_ ? fromHostView(ctl_->outsideView()) : OutsidePanelView{}; }

bool IphoneStreamHooks::setAllowOutside(bool on, std::string* err) {
    return ctl_ ? ctl_->setAllowOutside(on, err) : hostOff(start_error_, err);
}

bool IphoneStreamHooks::setOutsidePort(uint16_t port, std::string* err) {
    return ctl_ ? ctl_->setOutsidePort(port, err) : hostOff(start_error_, err);
}

bool IphoneStreamHooks::setOutsidePublicAddress(const std::string& addr, std::string* err) {
    return ctl_ ? ctl_->setOutsidePublicAddress(addr, err) : hostOff(start_error_, err);
}

void IphoneStreamHooks::acknowledgeAddressChange(const std::string& shownHost) {
    if (ctl_) ctl_->acknowledgeAddressChange(shownHost);
}

bool IphoneStreamHooks::removeOutsideMappingNow(std::string* err) {
    return ctl_ ? ctl_->removeOutsideMappingNow(err) : hostOff(start_error_, err);
}

bool IphoneStreamHooks::remapOutsideNow(std::string* err) {
    return ctl_ ? ctl_->remapOutsideNow(err) : hostOff(start_error_, err);
}

std::optional<OutsideInvitePanelView> IphoneStreamHooks::createOutsideInvite(kvhost::PetRole role, std::string* err) {
    if (!ctl_) {
        hostOff(start_error_, err);
        return std::nullopt;
    }
    const auto made = ctl_->createOutsideInvite(role, err);
    if (!made) return std::nullopt;
    return fromHostInvite(*made);
}

void IphoneStreamHooks::cancelOutsideInvite() {
    if (ctl_) ctl_->cancelOutsideInvite();
}

std::optional<OutsideInvitePanelView> IphoneStreamHooks::currentOutsideInvite() const {
    if (!ctl_) return std::nullopt;
    const auto open = ctl_->currentOutsideInvite();
    if (!open) return std::nullopt;
    return fromHostInvite(*open);
}

std::string IphoneStreamHooks::outsideEndpointLink() const {
    return ctl_ ? ctl_->outsideEndpointLink() : std::string();
}

std::vector<kvhost::outside::LogLine> IphoneStreamHooks::connectionLog(int days) const {
    return ctl_ ? ctl_->connectionLog(days) : std::vector<kvhost::outside::LogLine>{};
}

void IphoneStreamHooks::testOutsideRecheckNow() {
    if (ctl_ && router_injected_) ctl_->server().outsideRecheckNowForTest(); // only with the scripted fake router
}

uint64_t IphoneStreamHooks::testOutsideRechecks() const {
    return ctl_ && router_injected_ ? ctl_->server().outsideRechecksForTest() : 0;
}

void IphoneStreamHooks::setTestRouter(const TestRouterScript& script) {
    if (test_router_) test_router_->setScript(script);
}

TestRouterCounts IphoneStreamHooks::testRouterCounts() const {
    return test_router_ ? test_router_->counts() : TestRouterCounts{};
}

// ---- pairing window -------------------------------------------------------------------------------

void IphoneStreamHooks::openPairingWindow(HWND owner) {
    openPairingWindowFor(owner, kvhost::PetRole::Viewer, std::string()); // v3 D1: Viewer by default
}

void IphoneStreamHooks::openPairingWindowFor(HWND owner, kvhost::PetRole role, const std::string& rePairDeviceId) {
    if (!ctl_) {
        MessageBoxW(owner, (L"The streaming host is not running: " + widen(start_error_)).c_str(), L"Pair iPhone", MB_ICONERROR);
        return;
    }
    if (pair_hwnd_) {
        SetForegroundWindow(pair_hwnd_);
        return;
    }
    kvhost::ActivePairingOffer offer;
    std::string err;
    const std::optional<std::string> repair =
        rePairDeviceId.empty() ? std::nullopt : std::optional<std::string>(rePairDeviceId);
    if (!ctl_->server().beginPairingWindow(120, offer, &err, role, repair)) {
        MessageBoxW(owner, widen("Could not open the pairing window: " + err).c_str(), L"Pair iPhone", MB_ICONERROR);
        return;
    }
    pair_uri_ = offer.qrURI;
    pair_code_ = offer.shortCodeFormatted;
    pair_host_ = offer.hostIP + ":" + std::to_string(offer.port);
    pair_result_.clear();
    pair_close_countdown_ = -1;
    pair_count_before_ = ctl_->server().pairingsCompleted();
    pair_role_ = role;
    pair_code_shown_ = false; // QR only until the PC user chooses "Use a code instead"
    pair_repair_ = repair.has_value();
    pair_repair_name_.clear();
    if (repair) {
        for (const auto& d : ctl_->server().devices()) {
            if (d.deviceId == *repair) pair_repair_name_ = d.name;
        }
    }

    static bool registered = false;
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = PairProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = L"KinectViewerPairing";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int dpi = static_cast<int>(GetDpiForWindow(owner));
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int w = S(380), h = S(600);
    pair_hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, L"KinectViewerPairing", pair_repair_ ? L"Re-pair a device" : L"Pair a device",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + S(320), orc.top + S(60), w, h, owner, nullptr,
                                 inst, nullptr);
    kvwin::excludeFromCapture(pair_hwnd_); // its QR code and code are live secrets: never in a screen share or screenshot
    pair_font_ = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    pair_code_font_ = CreateFontW(-S(30), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    RECT cr{};
    GetClientRect(pair_hwnd_, &cr);
    pair_cancel_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, (cr.right - S(110)) / 2,
                                   cr.bottom - S(44), S(110), S(30), pair_hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), inst, nullptr);
    SendMessageW(pair_cancel_, WM_SETFONT, reinterpret_cast<WPARAM>(pair_font_), TRUE);
    // Where the code would show: the code is offered only on request, as it is weaker than the QR code (anyone on
    // the home network can try to recover it). The window then also takes it.
    pair_use_code_ = CreateWindowExW(0, L"BUTTON", L"Use a code instead", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     (cr.right - S(170)) / 2, S(340), S(170), S(30), pair_hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUseCodeId)), inst, nullptr);
    SendMessageW(pair_use_code_, WM_SETFONT, reinterpret_cast<WPARAM>(pair_font_), TRUE);
    if (!pair_repair_) {
        // pet-camera.md §3: the role is picked here, Viewer by default, and can change until a device pairs.
        pair_viewer_ = CreateWindowExW(0, L"BUTTON", L"Viewer (watch only)", WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP |
                                       BS_AUTORADIOBUTTON, S(40), S(12), S(150), S(22), pair_hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRoleViewerId)), inst, nullptr);
        pair_owner_ = CreateWindowExW(0, L"BUTTON", L"Owner", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, S(210), S(12), S(120),
                                      S(22), pair_hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRoleOwnerId)), inst, nullptr);
        for (HWND b : {pair_viewer_, pair_owner_}) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(pair_font_), TRUE);
        SendMessageW(role == kvhost::PetRole::Owner ? pair_owner_ : pair_viewer_, BM_SETCHECK, BST_CHECKED, 0);
    }
    SetTimer(pair_hwnd_, kPairTimer, 500, nullptr);
    ShowWindow(pair_hwnd_, SW_SHOWNOACTIVATE);
}

void IphoneStreamHooks::paintPairing(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT cr{};
    GetClientRect(hwnd, &cr);
    FillRect(dc, &cr, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, pair_font_);

    int y = S(14) + S(30); // under the role choice
    if (!pair_result_.empty()) {
        RECT r{S(16), y + S(150), cr.right - S(16), y + S(260)};
        SelectObject(dc, pair_code_font_);
        DrawTextW(dc, widen(pair_result_).c_str(), -1, &r, DT_CENTER | DT_WORDBREAK);
    } else {
        RECT t{S(16), y, cr.right - S(16), y + S(40)};
        const std::wstring intro =
            (pair_repair_ ? L"Re-pair \u201C" + widen(pair_repair_name_) + L"\u201D: it keeps its name and role. "
                          : std::wstring(pair_role_ == kvhost::PetRole::Owner ? L"Pairing as an owner. " : L"Pairing as a viewer. ")) +
            (pair_code_shown_ ? L"Scan with Kinect Viewer on the iPhone, or type the code. One try; the window closes after 120 s."
                              : L"Scan with Kinect Viewer on the iPhone. One try; the window closes after 120 s.");
        DrawTextW(dc, intro.c_str(), -1, &t, DT_CENTER | DT_WORDBREAK);
        y += S(46);
        // QR code (ECC M, as CIQRCodeGenerator "M" on the Mac), 4-module quiet zone.
        const int size = drawQrCode(dc, pair_uri_, cr.right / 2, y, S(240));
        y += size + S(8);
        if (pair_code_shown_) {
            SelectObject(dc, pair_code_font_);
            RECT c{0, y, cr.right, y + S(40)};
            DrawTextW(dc, widen(pair_code_).c_str(), -1, &c, DT_CENTER | DT_SINGLELINE);
        }
        y += S(44);
        SelectObject(dc, pair_font_);
        int secs = 0;
        if (ctl_) {
            if (auto off = ctl_->server().currentPairingOffer()) {
                secs = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(off->expiresAt - std::chrono::steady_clock::now()).count());
            }
        }
        wchar_t line[160];
        swprintf(line, 160, L"Expires in %d s    Host %ls", std::max(0, secs), widen(pair_host_).c_str());
        RECT e{0, y, cr.right, y + S(22)};
        DrawTextW(dc, line, -1, &e, DT_CENTER | DT_SINGLELINE);
        if (!pair_repair_) {
            RECT hint{S(16), cr.bottom - S(92), cr.right - S(16), cr.bottom - S(52)};
            DrawTextW(dc, L"Pairing your own phone again? Use Re-pair on its row in Devices\u2026", -1, &hint,
                      DT_CENTER | DT_WORDBREAK);
        }
    }
    SelectObject(dc, old_font);
    EndPaint(hwnd, &ps);
}

void IphoneStreamHooks::onPairingTimer(HWND hwnd) {
    if (pair_close_countdown_ >= 0) {
        if (--pair_close_countdown_ <= 0) DestroyWindow(hwnd);
        return;
    }
    if (ctl_ && !ctl_->server().currentPairingOffer()) {
        const bool newly_paired = ctl_->server().pairingsCompleted() != pair_count_before_;
        const std::string refusal = ctl_->server().lastPairingRefusal();
        pair_result_ = newly_paired ? "Device paired" : (refusal.empty() ? "Pairing window closed" : refusal);
        pair_close_countdown_ = 4; // 2 s
        ShowWindow(pair_cancel_, SW_HIDE);
        for (HWND b : {pair_viewer_, pair_owner_, pair_use_code_}) {
            if (b) ShowWindow(b, SW_HIDE);
        }
        if (on_state_changed_) on_state_changed_();
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void IphoneStreamHooks::onPairingClosed(HWND hwnd) {
    KillTimer(hwnd, kPairTimer);
    if (ctl_ && ctl_->server().currentPairingOffer()) ctl_->server().cancelPairingWindow();
    if (pair_font_) DeleteObject(pair_font_);
    if (pair_code_font_) DeleteObject(pair_code_font_);
    pair_font_ = pair_code_font_ = nullptr;
    pair_hwnd_ = nullptr;
    pair_cancel_ = nullptr;
    pair_viewer_ = pair_owner_ = nullptr;
    pair_use_code_ = nullptr;
    pair_code_shown_ = false;
}

void IphoneStreamHooks::onPairingUseCode() {
    if (!ctl_ || !pair_hwnd_ || !pair_result_.empty() || pair_code_shown_) return;
    if (!ctl_->server().allowShortCodeForPairing()) return; // the window has closed meanwhile
    pair_code_shown_ = true;
    if (pair_use_code_) ShowWindow(pair_use_code_, SW_HIDE);
    InvalidateRect(pair_hwnd_, nullptr, FALSE);
}

void IphoneStreamHooks::onPairingRole(kvhost::PetRole role) {
    if (!ctl_ || pair_repair_ || !pair_result_.empty()) return;
    if (ctl_->server().setPairingRole(role)) pair_role_ = role;
    if (pair_hwnd_) InvalidateRect(pair_hwnd_, nullptr, FALSE);
}

// ---- the --test-pet-camera hook's fake device -------------------------------------------------------

bool IphoneStreamHooks::startTestDevice(const std::string& name, std::string* err) {
    if (!ctl_) {
        if (err) *err = "the streaming host is not running: " + start_error_;
        return false;
    }
    const auto offer = ctl_->server().currentPairingOffer();
    if (!offer) {
        if (err) *err = "no pairing window is open";
        return false;
    }
    if (!test_device_) test_device_ = std::make_unique<PetTestDevice>();
    // Loopback only: --test-pet-camera implies --loopback-only, so the host listens on 127.0.0.1.
    test_device_->start("127.0.0.1", offer->port, offer->qrToken, ctl_->server().streamPort(), name);
    return true;
}

TestDeviceStatus IphoneStreamHooks::testDeviceStatus() const {
    return test_device_ ? test_device_->status() : TestDeviceStatus{};
}

void IphoneStreamHooks::stopTestDevice() {
    if (test_device_) test_device_->stop();
}

// ---- the --test-camera-roll hook's owner delete ---------------------------------------------------
// The sequence the host runs for a phone owner's delete_clip of a clip the PC holds: tell the PC (from a thread
// that isn't the UI's), wait up to 2 s for its unpin, then delete. Run here, through the same callback the host
// calls and the PC's own delete, so the hook can check the viewer's half: the player closes with "This clip was
// deleted.", unpins, and the clip is gone.
bool IphoneStreamHooks::startTestOwnerDelete(const std::string& clipId, std::string* err) {
    if (!ctl_ || !lo_.test_camera_roll) {
        if (err) *err = !ctl_ ? "the streaming host is not running" : "only under --test-camera-roll";
        return false;
    }
    if (owner_delete_th_.joinable()) owner_delete_th_.join();
    owner_delete_done_ = false;
    owner_delete_result_.clear();
    owner_delete_th_ = std::thread([this, clipId]() {
        auto held = [&]() {
            std::lock_guard<std::mutex> lk(pins_m_);
            for (const auto& p : pc_pins_) {
                if (p.second == clipId) return true;
            }
            return false;
        };
        const bool was_held = held();
        rollClipDeletedForPc(clipId);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (held() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool released = !held();
        const kvhost::roll::RollError e = ctl_->server().deleteRollClip(clipId);
        char b[160];
        std::snprintf(b, sizeof(b), "held=%d released=%d deleted=%d", was_held ? 1 : 0, released ? 1 : 0,
                      e == kvhost::roll::RollError::None ? 1 : 0);
        owner_delete_result_ = b;
        owner_delete_done_ = true;
    });
    return true;
}

bool IphoneStreamHooks::testOwnerDeleteDone(std::string* result) const {
    if (!owner_delete_done_) return false;
    if (result) *result = owner_delete_result_;
    return true;
}

} // namespace kvwin
