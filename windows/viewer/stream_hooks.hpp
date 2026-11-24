// iPhone streaming for the viewer (phase W3): wraps kvhost::HostController and the pairing window.
#pragma once

#include "keep_running.hpp"
#include "host_controller.hpp"
#include "outside_test_router.hpp"
#include "pet_test_device.hpp"
#include "viewer_app.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace kvwin {

class IphoneStreamHooks final : public StreamHooks {
public:
    IphoneStreamHooks();
    ~IphoneStreamHooks() override;

    void attach(kv_engine_t* engine, ViewerSettings* settings, const LaunchOptions& lo) override;
    void detach() override;
    void setUiCallbacks(std::function<void(int, int)> on_phone_mode, std::function<void()> on_state_changed) override;
    void setPhoneCallbacks(std::function<void(bool turn_on)> on_power,
                           std::function<void(const kvhost::RemoteControlCommandMessage&)> on_command) override;
    void setOrbitCallback(std::function<void(const kvhost::Orbit3DGestureMessage&)> on_orbit) override;
    void onPowerChanged(const kvhost::HostPowerModel::Snapshot& p) override;
    void onSensorRequested(bool turn_on) override;
    void markCleanStop() override;
    void onViewStateChanged(const kvhost::HostViewState& v) override;
    void pushHostState() override;
    bool allowPhoneSensorPower() const override;
    void setAllowPhoneSensorPower(bool allow) override;
    bool turnOffOnPhoneDisconnect() const override;
    void setTurnOffOnPhoneDisconnect(bool on) override;
    void onFrame(const uint8_t* bgra, const kv_mapped_frame_t& meta, const ViewerSettings& s) override;
    void onModeOrSourceChanged() override;
    void onTick() override;
    void openPairingWindow(HWND owner) override;
    HWND pairingWindow() const override { return pair_hwnd_; }
    void unpair() override;
    bool isPaired() const override;
    bool allowPhoneModeSwitch() const override;
    void setAllowPhoneModeSwitch(bool allow) override;
    std::wstring statusLine() const override;

    // The pet camera (W6).
    void setPetStateCallback(std::function<void()> on_pet_state) override;
    std::vector<kvhost::HostDeviceInfo> petDevices() const override;
    bool renameDevice(const std::string& deviceId, const std::string& name, std::string* err = nullptr) override;
    bool setDeviceRole(const std::string& deviceId, kvhost::PetRole role, std::string* err = nullptr) override;
    bool revokeDevice(const std::string& deviceId, std::string* err = nullptr) override;
    size_t revokeAllDevices(std::string* err = nullptr) override;
    void openPairingWindowFor(HWND owner, kvhost::PetRole role, const std::string& rePairDeviceId) override;
    kvhost::PetSettings petSettings() const override;
    void setSharingEnabled(bool on) override;
    void setSharingSchedule(const std::vector<kvhost::SharingScheduleRow>& rows) override;
    void setPrivacyMode(bool on) override;
    bool sharingScheduledOffNow() const override;
    std::vector<kvhost::PetWatcher> watchingNow() const override;
    std::vector<kvhost::HostTierStatus> tierStatus() const override;
    std::wstring stateDir() const override { return state_dir_; }
    bool stateDirIsTemporary() const override { return state_dir_temp_; }
    bool hostIdentityUnusable() const override { return !ctl_ && identity_unusable_; }
    std::string hostStartError() const override { return ctl_ ? std::string() : start_error_; }
    bool retryHostStart(std::string* err) override;
    bool resetHostIdentityAndStart(std::string* err, std::string* note, bool* identity_kept) override;
    std::string petSaveError() const override;

    // The camera roll (W7).
    void setRollCallback(std::function<void(const std::string& clipId)> on_clip_finalized) override;
    bool rollAvailable() const override { return ctl_ && ctl_->rollAvailable(); }
    std::string rollError() const override;
    std::wstring rollDir() const override { return rollAvailable() ? roll_dir_ : std::wstring(); }
    kvhost::RollRecorderStatus rollStatus() const override;
    bool rollPreparing() const override;
    std::optional<RollStorageView> rollStorageSnapshot() const override;
    bool rollLimitsKnown() const override;
    bool confirmRollLimits(std::string* err) override;
    bool turnOffSkippedForRoll() const override;
    bool rollHostTestRun() const override { return host_test_run_; }
    uint64_t pinRollClipForPc(const std::string& clipId) override;
    void unpinRollClipForPc(uint64_t token) override;
    size_t rollPcPinsHeld() const override;
    bool setRollClipDeletedCallback(std::function<void(const std::string& clipId)> on_deleted) override;
    uint64_t framesHandedToHost() const override { return frames_to_host_; }
    std::shared_ptr<kvhost::roll::RollStore> rollStore() const override;
    kvhost::StartClipResult startManualClip() override;
    kvhost::StopClipResult stopManualClip() override;
    void submitDepthForMotion(const uint16_t* raw16, uint32_t width, uint32_t height, uint64_t seq, uint64_t ts_ns,
                              bool live_sensor) override;
    bool setRollEnabled(bool on) override;
    bool setMotionSettings(bool enabled, int sensitivity) override;
    bool setRecordSchedule(const std::vector<kvhost::RecordScheduleRow>& rows, bool enabled) override;
    bool setRollRetention(int keepDays, uint64_t capBytes) override;
    bool setRollFolderSetting(const std::string& folderUtf8) override;
    kvhost::roll::RollError setRollClipStar(const std::string& clipId, bool starred) override;
    kvhost::roll::RollError deleteRollClip(const std::string& clipId) override;

    // Outside the home (W8).
    bool outsideAvailable() const override;
    OutsidePanelView outsideView() const override;
    bool setAllowOutside(bool on, std::string* err) override;
    bool setOutsidePort(uint16_t port, std::string* err) override;
    bool setOutsidePublicAddress(const std::string& addr, std::string* err) override;
    void acknowledgeAddressChange(const std::string& shownHost) override;
    bool removeOutsideMappingNow(std::string* err) override;
    bool remapOutsideNow(std::string* err) override;
    std::optional<OutsideInvitePanelView> createOutsideInvite(kvhost::PetRole role, std::string* err) override;
    void cancelOutsideInvite() override;
    std::optional<OutsideInvitePanelView> currentOutsideInvite() const override;
    std::string outsideEndpointLink() const override;
    std::vector<kvhost::outside::LogLine> connectionLog(int days) const override;
    bool testRouterInjected() const override { return router_injected_; }
    void setTestRouter(const TestRouterScript& script) override;
    void testOutsideRecheckNow() override;
    uint64_t testOutsideRechecks() const override;
    TestRouterCounts testRouterCounts() const override;

    bool startTestDevice(const std::string& name, std::string* err) override;
    TestDeviceStatus testDeviceStatus() const override;
    void stopTestDevice() override;
    bool startTestOwnerDelete(const std::string& clipId, std::string* err) override;
    bool testOwnerDeleteDone(std::string* result) const override;

    // Pairing window internals (window procedure).
    void paintPairing(HWND hwnd);
    void onPairingTimer(HWND hwnd);
    void onPairingClosed(HWND hwnd);
    void onPairingRole(kvhost::PetRole role); // the window's Viewer / Owner choice
    void onPairingUseCode();                  // the window's "Use a code instead": the code shows, and is taken

private:
    // Makes the controller, wires it to the viewer and starts it in state_dir_. On a failure ctl_ is empty and
    // start_error_ (and identity_unusable_) say why. again: a restart, which hands the new controller what the
    // viewer told the old one (or nobody, while the host was off).
    bool startHost(bool again);
    // The host is about to delete a clip a phone owner asked for: any thread.
    void rollClipDeletedForPc(const std::string& clipId);

    std::unique_ptr<kvhost::HostController> ctl_;
    std::shared_ptr<kvhost::KeepRunning> keep_running_; // a real launch only; shared with the host's callback
    std::optional<bool> sensor_requested_;              // the last On/Off asked for, before keep_running_ is made
    ViewerSettings* settings_ = nullptr;
    LaunchOptions lo_;
    std::string start_error_;
    bool identity_unusable_ = false;
    bool state_dir_ok_ = false; // hostStateDirFor accepted the folder: a start may be tried (again)
    std::optional<kvhost::HostPowerModel::Snapshot> last_power_;
    std::optional<kvhost::HostViewState> last_view_;
    std::function<void(int, int)> on_phone_mode_;
    std::function<void()> on_state_changed_;
    std::function<void(bool)> on_phone_power_;
    std::function<void(const kvhost::RemoteControlCommandMessage&)> on_phone_command_;
    std::function<void(const kvhost::Orbit3DGestureMessage&)> on_phone_orbit_;
    std::function<void()> on_pet_state_;
    std::function<void(const std::string&)> on_roll_clip_;
    // W7: the folder this run's roll records into (empty: no roll), and why there is none when that is the
    // launch's doing rather than the host's (a test run without --camera-roll-dir, an unreadable setting).
    std::wstring roll_dir_;
    std::string roll_note_;
    bool host_test_run_ = false; // Options::test_run as the last start set it
    uint64_t frames_to_host_ = 0; // onFrame calls handed on (UI thread)
    // The PC player's pins (token to clip), and the host's way to tell the PC a phone owner deletes one.
    mutable std::mutex pins_m_;
    std::map<uint64_t, std::string> pc_pins_;
    std::function<void(const std::string&)> on_roll_clip_deleted_;
    // The --test-camera-roll owner delete (startTestOwnerDelete).
    std::thread owner_delete_th_;
    std::atomic<bool> owner_delete_done_{false};
    std::string owner_delete_result_; // written by its thread before owner_delete_done_
    std::wstring state_dir_;
    bool state_dir_temp_ = false;
    std::unique_ptr<PetTestDevice> test_device_;
    // W8, --test-outside only: the fake router handed to the host (Options::router_factory), and whether the host
    // took it. It outlives every host started in this run, so a session the host's worker still holds stays valid.
    std::shared_ptr<OutsideTestRouter> test_router_;
    bool router_injected_ = false;
    HWND pair_hwnd_ = nullptr;
    HWND pair_cancel_ = nullptr;
    HWND pair_viewer_ = nullptr;
    HWND pair_owner_ = nullptr;
    HWND pair_use_code_ = nullptr; // "Use a code instead"
    bool pair_code_shown_ = false; // the PC user chose it: the code shows, and the host takes it
    std::string pair_repair_name_;
    HFONT pair_font_ = nullptr;
    HFONT pair_code_font_ = nullptr;
    std::string pair_uri_;
    std::string pair_code_;
    std::string pair_host_;
    std::string pair_result_;
    uint64_t pair_count_before_ = 0;
    kvhost::PetRole pair_role_ = kvhost::PetRole::Viewer;
    bool pair_repair_ = false;
    int pair_close_countdown_ = -1;
};

} // namespace kvwin
