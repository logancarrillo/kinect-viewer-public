// Win32 + D3D11 viewer application shell.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "host_power.hpp"
#include "kinect_core_c.h"
#include "outside_panel_model.hpp" // W8: OutsidePanelView, OutsideInvitePanelView, LogLine
#include "pet_camera_types.hpp"
#include "pet_settings.hpp"
#include "roll_recorder.hpp" // RollRecorderStatus, the roll's store (W7)
#include "viewer_model.hpp"
#include "wire_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kvwin {

// What the --test-pet-camera hook's fake v3 device has seen (a test hook only; see pet_test_device.hpp).
struct TestDeviceStatus {
    bool started = false;
    bool paired = false;    // the pairing window accepted it
    bool connected = false; // its stream connection's handshake finished
    bool got_hello = false;
    bool closed = false;    // the host closed its stream connection
    std::string name;       // the name it sent
    std::string error;
    std::string deviceId;    // from the pairResponse
    std::string role;        // from the pairResponse ("owner" / "viewer")
    std::string device_name; // the last 0x0B's deviceName (follows a rename on the PC)
    std::string notice;      // the last 0x0F reason
    std::string reason;      // the last 0x0B's liveUnavailableReason
    int pet_states = 0;      // 0x0B received
    int configs = 0;         // 0x02 received
    int aus = 0;             // 0x03 received
    int keyframes = 0;
    bool live_allowed = false, watching = false, privacy = false, sharing = false; // the last 0x0B
};

// The camera roll's storage line: the host's cached counts, read without walking the folder or waiting on the
// store's lock, so the UI thread can ask as often as it likes.
struct RollStorageView {
    uint64_t usedBytes = 0;
    std::optional<uint64_t> freeBytes; // unknown when the disk couldn't be asked
    uint64_t starredBytes = 0;
    uint64_t capBytes = 0;
    int keepDays = 0;
    uint64_t floorBytes = kvhost::roll::kDiskFloorBytes;
    bool limitsKnown = false; // the owner's limits; until then retention doesn't run
};

// The --test-outside hook's fake router (W8; a test hook only, never the real router): what it does when the host's
// mapper opens a session on it. Every field takes effect at the next session the host opens.
struct TestRouterScript {
    bool present = true;             // false: no UPnP router answers (NATUPnP's NULL collection)
    // The WAN address it reports for a mapping: a public IPv4 by default. This self-test runs inside the shipping
    // viewer, whose WAN check refuses the documentation blocks (192.0.2/24, 198.51.100/24, 203.0.113/24) with no
    // switch to change that, not even in a test launch. So its two samples, here and in viewer_app.cpp, are
    // 81.2.69.160 and 81.2.69.161, from 81.2.69.160/27 in MaxMind's published GeoIP2 test data (the MaxMind-DB
    // repository, source-data/GeoIP2-City-Test.json): a test set, not anyone's line. The separate test programs
    // use documentation addresses; test_outside_address.cpp pins the viewer's source to these two.
    std::string wan = "81.2.69.160";
    uint16_t takenPort = 0;          // a port another machine on the home network already holds on it
};
struct TestRouterCounts {
    int opens = 0, adds = 0, removes = 0;
    size_t mappings = 0; // this PC's mappings it holds now
};

// Optional iPhone-streaming hooks (phase W3). The viewer runs without them.
class StreamHooks {
public:
    virtual ~StreamHooks() = default;
    virtual void attach(kv_engine_t* engine, ViewerSettings* settings, const LaunchOptions& lo) = 0;
    virtual void detach() = 0;
    // UI-thread sinks: the phone asked for a mode; streaming/pairing state changed (any thread may call).
    virtual void setUiCallbacks(std::function<void(int, int)> on_phone_mode, std::function<void()> on_state_changed) = 0;
    // Protocol v2 sinks, called from network threads in arrival order; the viewer posts each to its
    // UI thread, whose message queue keeps that order. on_power: the phone's On/Off, or an Off
    // because the phone went away with "turn off on disconnect" set. on_command: a remote-control
    // command the server let through.
    virtual void setPhoneCallbacks(std::function<void(bool turn_on)> on_power,
                                   std::function<void(const kvhost::RemoteControlCommandMessage&)> on_command) = 0;
    // The phone's 0x09 orbit gestures for the 3D view (the server lets them through only when the
    // phone may control the view). Network thread; the viewer posts each to its UI thread.
    virtual void setOrbitCallback(std::function<void(const kvhost::Orbit3DGestureMessage&)> on_orbit) = 0;
    // What the phone is told (0x07). UI thread.
    virtual void onPowerChanged(const kvhost::HostPowerModel::Snapshot& p) = 0;
    // Each On or Off asked for (the PC's or a phone's), before the sensor is opened or closed.
    virtual void onSensorRequested(bool turn_on) { (void)turn_on; }
    // The person closed the window, or Windows is ending the session: the first thing the close handler does, before
    // any saving or teardown (a fault during the close is never restarted).
    virtual void markCleanStop() {}
    virtual void onViewStateChanged(const kvhost::HostViewState& v) = 0;
    virtual void pushHostState() = 0; // after a phone command, even one that changed nothing
    virtual bool allowPhoneSensorPower() const = 0;
    virtual void setAllowPhoneSensorPower(bool allow) = 0;
    virtual bool turnOffOnPhoneDisconnect() const = 0;
    virtual void setTurnOffOnPhoneDisconnect(bool on) = 0;
    // Called on the UI thread for every new mapped frame.
    virtual void onFrame(const uint8_t* bgra, const kv_mapped_frame_t& meta, const ViewerSettings& s) = 0;
    virtual void onModeOrSourceChanged() = 0;
    virtual void onTick() = 0;
    virtual void openPairingWindow(HWND owner) = 0;
    virtual HWND pairingWindow() const = 0;
    virtual void unpair() = 0;
    virtual bool isPaired() const = 0;
    virtual bool allowPhoneModeSwitch() const = 0;
    virtual void setAllowPhoneModeSwitch(bool allow) = 0;
    virtual std::wstring statusLine() const = 0;

    // ---- the pet camera (protocol v3, W6; pet-camera.md §4). Any thread unless noted; the defaults
    // are what a viewer without the streaming host shows. --------------------------------------------
    // Called (from any thread) whenever devices, sessions or the pet-camera settings change.
    virtual void setPetStateCallback(std::function<void()> on_pet_state) { (void)on_pet_state; }
    // The device list: name, role, kind, paired, last seen, connected and its path.
    virtual std::vector<kvhost::HostDeviceInfo> petDevices() const { return {}; }
    // The device changes. err: why one was refused (while the device list can't be read: "Can't change
    // devices until the device list can be read." and the read error), for the PC to show.
    virtual bool renameDevice(const std::string& deviceId, const std::string& name, std::string* err = nullptr) {
        (void)deviceId;
        (void)name;
        if (err) *err = "no streaming host";
        return false;
    }
    // Takes effect at once on the device's open session (new hello, 0x07 and 0x0B).
    virtual bool setDeviceRole(const std::string& deviceId, kvhost::PetRole role, std::string* err = nullptr) {
        (void)deviceId;
        (void)role;
        if (err) *err = "no streaming host";
        return false;
    }
    virtual bool revokeDevice(const std::string& deviceId, std::string* err = nullptr) {
        (void)deviceId;
        if (err) *err = "no streaming host";
        return false;
    }
    // How many were revoked; 0 with err when refused.
    virtual size_t revokeAllDevices(std::string* err = nullptr) {
        if (err) *err = "no streaming host";
        return 0;
    }
    // UI thread. The pairing window for a new device with the role picked on the PC (Viewer by
    // default, v3 D1), or, with a deviceId, "Re-pair" on that device's row (keeps its name and role).
    virtual void openPairingWindowFor(HWND owner, kvhost::PetRole role, const std::string& rePairDeviceId) {
        (void)owner;
        (void)role;
        (void)rePairDeviceId;
    }
    virtual kvhost::PetSettings petSettings() const { return {}; }
    virtual void setSharingEnabled(bool on) { (void)on; }
    virtual void setSharingSchedule(const std::vector<kvhost::SharingScheduleRow>& rows) { (void)rows; }
    virtual void setPrivacyMode(bool on) { (void)on; }
    virtual bool sharingScheduledOffNow() const { return false; }
    // "Watching now": every session holding a watcher slot.
    virtual std::vector<kvhost::PetWatcher> watchingNow() const { return {}; }
    // Per-tier diagnostics (encoder name, size, epoch, sessions).
    virtual std::vector<kvhost::HostTierStatus> tierStatus() const { return {}; }
    // Where the host keeps pairing.json, devices.json and pet-camera.json for this run.
    virtual std::wstring stateDir() const { return {}; }

    // ---- this PC's identity (pairing.json). UI thread. ---------------------------------------------------
    // The host is off because pairing.json is there but can't be read or parsed. It failed closed: nothing was
    // written and no new identity was made. hostStartError() says why the host is off (empty while it runs).
    virtual bool hostIdentityUnusable() const { return false; }
    virtual std::string hostStartError() const { return {}; }
    // "Try again": the host starts again the same way (for a file another program was holding). True when it
    // runs now; otherwise err says why.
    virtual bool retryHostStart(std::string* err) {
        if (err) *err = "no streaming host";
        return false;
    }
    // "Reset this PC's identity", once the owner confirmed it: only while hostIdentityUnusable(). The current
    // pairing.json is kept as pairing.json.bad, a new identity is written (every device must pair again), and
    // the host starts again. False with err when the reset or the start failed; note: what happened to the old
    // file. The file is loaded again first: when it can be used now (the "unusable" was stale), nothing is reset,
    // identity_kept is set, and the host just starts with it. A .bad copy that fails while the file can be read
    // refuses the reset (err).
    virtual bool resetHostIdentityAndStart(std::string* err, std::string* note, bool* identity_kept) {
        (void)note;
        (void)identity_kept;
        if (err) *err = "no streaming host";
        return false;
    }
    // True when stateDir() is this run's own temp folder: a --synthetic or test-hook launch without
    // --state-dir (v3 §10). It is deleted when the run ends.
    virtual bool stateDirIsTemporary() const { return false; }

    // A save that failed, or a pet-camera.json that couldn't be read (the host's saveError), for the reason a
    // setting is refused. Empty when the disk has everything.
    virtual std::string petSaveError() const { return {}; }

    // ---- the camera roll (W7; pet-camera.md §4, §6). UI thread unless noted; the defaults are a viewer
    // without the streaming host, which has no roll. ----------------------------------------------------------
    // A clip was finalized (any thread): the roll browser may refresh.
    virtual void setRollCallback(std::function<void(const std::string& clipId)> on_clip_finalized) {
        (void)on_clip_finalized;
    }
    // The roll runs in this process (the folder was accepted); rollError() says why not otherwise: a refused
    // folder, or a test run without --camera-roll-dir (v3 §10). rollDir(): the folder this run records into.
    virtual bool rollAvailable() const { return false; }
    virtual std::string rollError() const { return "no streaming host"; }
    virtual std::wstring rollDir() const { return {}; }
    virtual kvhost::RollRecorderStatus rollStatus() const { return {}; }
    // The folder is being prepared (rollStatus's `preparing`).
    virtual bool rollPreparing() const { return false; }
    // Used and free space, the cap and the floor, for the panel's Keep row and the browser (nullopt without a
    // roll). The host's cached counts; the folder is never walked for it on the UI thread.
    virtual std::optional<RollStorageView> rollStorageSnapshot() const { return std::nullopt; }
    // The retention limits are the owner's (pet-camera.json held them, or the owner set or confirmed them).
    // Until then retention doesn't run, and the panel offers Confirm. confirmRollLimits writes the limits shown
    // (the current settings) and makes them known; false with err when they couldn't be saved.
    virtual bool rollLimitsKnown() const { return false; }
    virtual bool confirmRollLimits(std::string* err) {
        if (err) *err = "no streaming host";
        return false;
    }
    // D6: the roll is on and usable, so "turn the Kinect off when the iPhone disconnects" is skipped. The
    // panel's note shows exactly when this is true (the host's own skip uses the same predicate).
    virtual bool turnOffSkippedForRoll() const { return false; }
    // This run's host was started as a test run (HostController::Options::test_run set), so its recorder
    // refuses every frame that isn't the synthetic source's.
    virtual bool rollHostTestRun() const { return false; }
    // The PC's player holds its clip while it is open: retention skips it, and a phone owner's delete
    // first tells the PC (the callback, on any thread) and waits for the unpin. 0: no such clip.
    virtual uint64_t pinRollClipForPc(const std::string& clipId) {
        (void)clipId;
        return 0;
    }
    virtual void unpinRollClipForPc(uint64_t token) { (void)token; }
    // How many of the PC's pins are held now (a test hook's check that the player let go).
    virtual size_t rollPcPinsHeld() const { return 0; }
    // A phone owner is deleting a clip the PC may be playing (a host thread): the viewer posts it to its UI
    // thread, closes the player on it and unpins. False when the host can't tell the PC yet.
    virtual bool setRollClipDeletedCallback(std::function<void(const std::string& clipId)> on_deleted) {
        (void)on_deleted;
        return false;
    }
    // Every frame this hook has handed the host (onFrame calls), for the hook tests' count by source.
    virtual uint64_t framesHandedToHost() const { return 0; }
    // The roll's files, for the PC's browser and player (null without a roll).
    virtual std::shared_ptr<kvhost::roll::RollStore> rollStore() const { return nullptr; }
    // The panel's "Record a clip" and "Stop" (startedBy "This PC").
    virtual kvhost::StartClipResult startManualClip() {
        kvhost::StartClipResult r;
        r.refused = kvhost::RollBlock::RollDisabled;
        return r;
    }
    virtual kvhost::StopClipResult stopManualClip() { return kvhost::StopClipResult::Ok; }
    // The motion feed (pet-camera.md §6.2): each new depth frame, with live_sensor false for a replay.
    virtual void submitDepthForMotion(const uint16_t* raw16, uint32_t width, uint32_t height, uint64_t seq, uint64_t ts_ns,
                                      bool live_sensor) {
        (void)raw16;
        (void)width;
        (void)height;
        (void)seq;
        (void)ts_ns;
        (void)live_sensor;
    }
    // The roll's settings, saved in pet-camera.json. False when refused: pet-camera.json couldn't be read
    // (petSaveError says why), or a value out of range.
    virtual bool setRollEnabled(bool on) {
        (void)on;
        return false;
    }
    virtual bool setMotionSettings(bool enabled, int sensitivity) {
        (void)enabled;
        (void)sensitivity;
        return false;
    }
    virtual bool setRecordSchedule(const std::vector<kvhost::RecordScheduleRow>& rows, bool enabled) {
        (void)rows;
        (void)enabled;
        return false;
    }
    virtual bool setRollRetention(int keepDays, uint64_t capBytes) {
        (void)keepDays;
        (void)capBytes;
        return false;
    }
    // The folder a real launch records into from its next start (a change needs a
    // restart). Checked by the caller first (rollFolderChoiceAcceptable).
    virtual bool setRollFolderSetting(const std::string& folderUtf8) {
        (void)folderUtf8;
        return false;
    }
    // The PC's star (as an owner, deviceId "pc") and delete (stops any playback of the clip first).
    virtual kvhost::roll::RollError setRollClipStar(const std::string& clipId, bool starred) {
        (void)clipId;
        (void)starred;
        return kvhost::roll::RollError::NotFound;
    }
    virtual kvhost::roll::RollError deleteRollClip(const std::string& clipId) {
        (void)clipId;
        return kvhost::roll::RollError::NotFound;
    }

    // ---- outside the home (W8; v3 §8, pet-camera.md §4). UI thread unless noted; the defaults are a viewer
    // without the streaming host, which has no outside listener. ------------------------------------------------
    // The host's outside change reports (from a host thread) reach the viewer through setPetStateCallback's
    // callback, so they coalesce with the other pet-camera changes into one refresh on the UI thread.
    // This build's host has outside listening at all (the fixed interface is there).
    virtual bool outsideAvailable() const { return false; }
    // What the panel shows: cheap, and never waits on the router (the host's own worker does that).
    virtual OutsidePanelView outsideView() const { return {}; }
    // "Allow viewing from outside the home": refused (err) unless sharing is on, or while pet-camera.json can't
    // be read. The port: 1024-65535. The public address: "" clears it; only a public IPv4 is taken.
    virtual bool setAllowOutside(bool on, std::string* err) {
        (void)on;
        if (err) *err = "no streaming host";
        return false;
    }
    virtual bool setOutsidePort(uint16_t port, std::string* err) {
        (void)port;
        if (err) *err = "no streaming host";
        return false;
    }
    virtual bool setOutsidePublicAddress(const std::string& addr, std::string* err) {
        (void)addr;
        if (err) *err = "no streaming host";
        return false;
    }
    // address_changed back to mapped, once the PC user re-shared (§8.3): shownHost is the address in the link shown,
    // and the host applies it only while that is still the address read now.
    virtual void acknowledgeAddressChange(const std::string& shownHost) { (void)shownHost; }
    // pet-camera.md §4 "Remove mapping now" (HostController::removeOutsideMappingNow).
    virtual bool removeOutsideMappingNow(std::string* err) {
        if (err) *err = "no streaming host";
        return false;
    }
    // "Map the port again" (HostController::remapOutsideNow), after "Remove mapping now".
    virtual bool remapOutsideNow(std::string* err) {
        if (err) *err = "no streaming host";
        return false;
    }
    // §8.4: one open at a time, for 10 minutes; a new one closes any older one. The role is picked on the PC.
    virtual std::optional<OutsideInvitePanelView> createOutsideInvite(kvhost::PetRole role, std::string* err) {
        (void)role;
        if (err) *err = "no streaming host";
        return std::nullopt;
    }
    virtual void cancelOutsideInvite() {}
    virtual std::optional<OutsideInvitePanelView> currentOutsideInvite() const { return std::nullopt; }
    // kv2://endpoint?... (no secret), or "" when there is no outside endpoint.
    virtual std::string outsideEndpointLink() const { return {}; }
    // The connection log's last `days` days, newest first (§8.6; the PC only).
    virtual std::vector<kvhost::outside::LogLine> connectionLog(int days) const {
        (void)days;
        return {};
    }
    // --test-outside only: the fake router the host was handed (Options::router_factory). testRouterInjected():
    // the host took it, so it never opens the real router in this run.
    virtual bool testRouterInjected() const { return false; }
    virtual void setTestRouter(const TestRouterScript& script) { (void)script; }
    // --test-outside only: the host checks the router now instead of at its next 10-minute check (§8.2, §8.3).
    virtual void testOutsideRecheckNow() {}
    // --test-outside only: how many 10-minute checks the host's router worker has been through (a skipped one too).
    virtual uint64_t testOutsideRechecks() const { return 0; }
    virtual TestRouterCounts testRouterCounts() const { return {}; }

    // ---- test hook only (--test-pet-camera; synthetic, loopback only) ----------------------------------
    // A fake v3 device on this PC pairs through the open pairing window and then streams over loopback.
    // False (with the reason) when no pairing window is open or the host isn't running.
    virtual bool startTestDevice(const std::string& name, std::string* err) {
        (void)name;
        if (err) *err = "no streaming host";
        return false;
    }
    virtual TestDeviceStatus testDeviceStatus() const { return {}; }
    virtual void stopTestDevice() {}
    // --test-camera-roll only: a phone owner's delete of clipId as the host runs it, started on a thread
    // of its own: the PC is told (the setRollClipDeletedCallback callback), the delete waits up to 2 s for the PC
    // to unpin the clip, then the clip is deleted. testOwnerDeleteDone() is set once it ran; its result in err.
    virtual bool startTestOwnerDelete(const std::string& clipId, std::string* err) {
        (void)clipId;
        if (err) *err = "no streaming host";
        return false;
    }
    virtual bool testOwnerDeleteDone(std::string* result) const {
        (void)result;
        return false;
    }
};

class ViewerApp {
public:
    ViewerApp();
    ~ViewerApp();
    int run(HINSTANCE inst, int argc, const wchar_t* const* argv, StreamHooks* hooks);

    struct Impl;

private:
    Impl* impl_;
};

} // namespace kvwin
