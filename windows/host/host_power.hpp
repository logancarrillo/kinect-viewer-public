// Kinect power from the phone (protocol v2, 0x0A) and from the PC's own Start/Stop: the state the
// host confirms to the phone, never one it assumes. Port of HostController.requestSensorPower,
// startStream, stopStream and the sleep/wake handlers in macos/App/HostController.swift,
// with one difference: a resume captures the power generation, so an Off (or another
// On) that arrives during the reopen delay wins. The Mac's wake path loses it.
//
// Not thread-safe by design: every call comes from one thread, in arrival order (the viewer's UI
// thread, through posted messages; a test's own queue). That serial path is what keeps two quick
// toggles in order. snapshot() alone may be read from any thread.
//
// Built into kv_viewer_model, not kv_host: the viewer's own Start/Stop goes through it even when
// it's built without the streaming host. It uses wire_protocol.hpp's enum only, no kv_host code.
#pragma once

#include "wire_protocol.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace kvhost {

// The parts of 0x07 the viewer owns (the power state comes from HostPowerModel, the two phone
// permissions from the stream server). Defaults are what this host can honestly say before the
// later stages exist: no cut-off, no freeze, no replay.
struct HostViewState {
    int modeRaw = 0;
    int sbsLayoutRaw = 0;
    bool cutoffEnabled = false;
    float cutoffNearMm = 500.0f;
    float cutoffFarMm = 2000.0f;
    bool isHostFrozen = false;
    bool isRecording = false;
    bool canRecordCurrentMode = true;
    std::optional<std::string> recordingDisabledReason;
    bool recordingReminderNeeded = true;
    uint64_t recordingFrames = 0;
    double recordingElapsedSec = 0.0;
    bool isReplayingOnHost = false;

    // Everything but recording progress (frames, elapsed), which is sent at about 1 Hz at most.
    bool sameExceptProgress(const HostViewState& o) const {
        return modeRaw == o.modeRaw && sbsLayoutRaw == o.sbsLayoutRaw && cutoffEnabled == o.cutoffEnabled &&
               cutoffNearMm == o.cutoffNearMm && cutoffFarMm == o.cutoffFarMm && isHostFrozen == o.isHostFrozen &&
               isRecording == o.isRecording && canRecordCurrentMode == o.canRecordCurrentMode &&
               recordingDisabledReason == o.recordingDisabledReason &&
               recordingReminderNeeded == o.recordingReminderNeeded && isReplayingOnHost == o.isReplayingOnHost;
    }
};

// What the viewer (or a test) does to the sensor. Called only from the power model's thread.
class SensorBackend {
public:
    struct OpenResult {
        bool ok = false;
        bool lock_blocked = false; // KINECT-IN-USE was taken by another process at open
        std::string error;
    };
    virtual ~SensorBackend() = default;
    // The holder's description when another process holds ~/KINECT-IN-USE, else nullopt. The
    // synthetic source takes no lock, so a viewer on --synthetic answers nullopt.
    virtual std::optional<std::string> externalLockHolder() = 0;
    // Opens the sensor and starts the frame flow. Never starts a recording.
    virtual OpenResult open() = 0;
    // Finishes any recording cleanly, stops the frames (so no more 0x03 video), closes USB and
    // releases ~/KINECT-IN-USE. The TLS session to the phone stays open.
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
};

class HostPowerModel {
public:
    struct Snapshot {
        SensorPowerState state = SensorPowerState::Off;
        std::optional<std::string> inUseHolder;  // set only with InUseElsewhere
        std::optional<std::string> errorMessage; // set only with Error
        uint64_t generation = 0;
    };

    // on_changed runs (on the model's thread) after every state the phone should hear, including
    // "starting" before an open and a repeat of the same state when a request changes nothing.
    HostPowerModel(SensorBackend& backend, std::function<void(const Snapshot&)> on_changed);

    // On or Off, from the phone or the PC. Off always wins over anything still pending.
    void request(bool turn_on);
    // Told each On or Off asked for, BEFORE the sensor is opened or closed (the keep-running block
    // records the person's choice first, so a crash while the sensor closes can't lose it).
    void setOnRequest(std::function<void(bool turn_on)> f) { on_request_ = std::move(f); }
    // System suspend: close and report Off. Returns the generation a later resume must match.
    uint64_t suspend();
    // Whether the sensor was open (or opening) when suspend() closed it.
    bool resumeWanted() const { return resume_wanted_; }
    // After the resume settle delay (or a USB replug): reopens only if nothing was requested since
    // suspend() or sensorLost() (the generation still matches) and the sensor was open before.
    // usb_present false reports Error with usb_missing_message instead. False if it did nothing.
    bool resumeIfCurrent(uint64_t generation, bool usb_present, const std::string& usb_missing_message);
    // The sensor went away while open (USB unplugged): close and report Error with the message.
    // Returns the generation a replug's resumeIfCurrent must match, as suspend() does.
    uint64_t sensorLost(const std::string& message);
    // The viewer restarted an open sensor in place (new source, pipeline, or colour turned on):
    // report how that went. Not a request, so nothing pending is cancelled.
    void reportRestart(const SensorBackend::OpenResult& r);

    Snapshot snapshot() const;

private:
    void set(SensorPowerState s, std::optional<std::string> holder, std::optional<std::string> error);
    void open();
    void report(const SensorBackend::OpenResult& r);

    SensorBackend& backend_;
    std::function<void(const Snapshot&)> on_changed_;
    std::function<void(bool)> on_request_;
    mutable std::mutex m_; // guards snap_ for snapshot()
    Snapshot snap_;
    uint64_t generation_ = 0;
    bool resume_wanted_ = false;
};

} // namespace kvhost
