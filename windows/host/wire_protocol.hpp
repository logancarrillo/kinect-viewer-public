// Byte-for-byte C++ port of shared/WireProtocol.swift (the 'KV' framing and its JSON messages).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pet_role.hpp" // PetRole is shared with the device store

namespace kvhost {

enum class MsgType : uint8_t {
    HelloServer = 0x01,
    StreamConfig = 0x02,
    VideoAccessUnit = 0x03,
    Ping = 0x04,
    Pong = 0x05,
    SelectModeReq = 0x06,
    // Protocol v2 (docs/protocol-changes.md): additive, and sent only once both ends said v2.
    HostStateSync = 0x07,    // host -> phone
    RemoteControlCmd = 0x08, // phone -> host
    Orbit3DGesture = 0x09,   // phone -> host
    SensorPowerReq = 0x0A,   // phone -> host
    // Protocol v3, the pet camera (docs/protocol-v3-pet-camera.md §5): sent and read only on a
    // connection at version 3.
    PetCameraState = 0x0B, // host -> device
    PetCameraCmd = 0x0C,   // device -> host
    CommandResult = 0x0D,  // host -> device
    LiveViewReq = 0x0E,    // device -> host
    SessionNotice = 0x0F,  // host -> device
    PairRequest = 0x10,
    PairResponse = 0x11,
    // Protocol v3: the camera roll (docs/protocol-v3-pet-camera.md §5.6-5.9), version 3 only like
    // 0x0B-0x0F. 0x1A and up stay free (§1 rule 7).
    RollListReq = 0x12,        // device -> host
    RollList = 0x13,           // host -> device
    RollThumbReq = 0x14,       // device -> host
    RollThumb = 0x15,          // host -> device
    PlaybackReq = 0x16,        // device -> host
    PlaybackState = 0x17,      // host -> device
    PlaybackConfig = 0x18,     // host -> device: the 0x02 JSON plus a stream id
    PlaybackAccessUnit = 0x19, // host -> device, binary: a stream id, then the 0x03 body byte for byte
};

// Protocol v3 (the pet camera, docs/protocol-v3-pet-camera.md).
constexpr int kProtocolV3 = 3;
// The version this host speaks (v3). Every session starts at 1 and is raised once, by its first
// Ping that says "protocolVersion": 2 or more, to min(ping, 3) (v3 §1 rule 1). v1/v2 phones read a 3
// in hello and 0x07 as "2 or more", so they are safe to send 3 (v3 §1).
constexpr int kHostProtocolVersion = kProtocolV3;

// SharedRecordingReminderConstants in shared/WireProtocol.swift: one copy of each sentence the host
// uses. The phone shows sensorPowerDisabledReason as sent; it says "Mac" on the PC too until PC
// wording is chosen (an open question). The phone's own "The viewer isn't open on the Mac"
// never comes from a host.
namespace text {
constexpr const char* kRecordingReminder =
    "Only you in view. Not pointed at a window. Depth and IR only. Deleted after 7 days.";
constexpr double kReminderSnoozeSeconds = 3600.0;
constexpr const char* kColourLiveOnly = "Colour is live only. Switch to IR or Depth to record.";
constexpr const char* kSensorPowerDisabledOnHost = "Power control is turned off on the Mac";
// The lock holder trimmed of whitespace, or "another process" when that leaves nothing. Header-only,
// so HostPowerModel (in kv_viewer_model, without kv_host's code) uses it too.
inline std::string holderOrAnotherProcess(const std::string& holder) {
    const auto first = holder.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "another process";
    const auto last = holder.find_last_not_of(" \t\r\n");
    return holder.substr(first, last - first + 1);
}
// "The Kinect is in use by <holder>", with "another process" for an empty holder.
std::string inUseBy(const std::string& holder);

// Protocol v3 §9: one sentence each, shared by the v2 reasons (sensorPowerDisabledReason,
// recordingDisabledReason), rollBlockedText and the message of 0x0D, 0x0F and 0x17. Devices show
// them as sent, so the words live here once.
constexpr const char* kPrivacyMode = "Privacy mode is on. Turn it off on the PC or an owner's device.";
constexpr const char* kPcReplaying = "The PC is playing a recording.";
constexpr const char* kSharingOff = "Sharing is off on the PC.";
constexpr const char* kSharingSchedule = "Sharing is off at this time of day.";
constexpr const char* kTooManyViewers = "Four devices are already watching.";
constexpr const char* kViewerPower = "Only an owner can turn the camera on or off.";
constexpr const char* kViewerRecording = "Only an owner can record.";
constexpr const char* kRollDisabled = "Recording to the camera roll is off.";
constexpr const char* kRollPreparing = "The camera roll is getting ready.";
constexpr const char* kFolderUnavailable = "The camera roll folder on the PC can't be used.";
constexpr const char* kSensorOff = "The camera is off.";
constexpr const char* kViewFrozen = "The view on the PC is frozen.";
constexpr const char* kDiskFloor = "Recording stopped: less than 20 GB is free on the PC's disk.";
constexpr const char* kStorageFull = "Recording stopped: the camera roll is full of starred clips.";
constexpr const char* kRevoked = "This device was removed from the PC.";
constexpr const char* kReplaced = "This device connected again somewhere else.";
constexpr const char* kOutsideDisabled = "Viewing from outside the home was turned off on the PC.";
constexpr const char* kAddressChanged = "The PC's home address changed.";
constexpr const char* kHostShuttingDown = "The camera app on the PC was closed.";
constexpr const char* kTooManySessions = "Too many devices are connected to this PC.";
constexpr const char* kTooManyPairedDevices = "This PC has 16 paired devices. Revoke one first.";
constexpr const char* kInviteExpiredOrUsed = "This invite has expired or was already used. Ask for a new one.";
constexpr const char* kInviteWrongCode = "That invite didn't match. It can't be used again; ask for a new one.";
// A wrong proof that didn't burn the invite (remote_invite.hpp, kInviteWrongAttemptsToBurn): it stays open.
constexpr const char* kInviteDidntMatch = "That invite didn't match. Check that you used the whole link you were sent.";
// The PC closed the invite while this device was connecting.
constexpr const char* kInviteClosed = "This invite was closed. Ask for a new one.";
// 0x17's message with a host-initiated stop or an error (v3 §5.9, §9). 0x17 carries no code, only these.
constexpr const char* kClipDeleted = "This clip was deleted.";             // stopped: an owner or the PC deleted it
constexpr const char* kClipUnreadable = "The PC couldn't read this clip."; // error: the clip can't be read
constexpr const char* kPlaybackIdle = "Playback stopped after a minute without a command."; // stopped: idle
// The sentence for a §9 code or reason string (privacy_mode, revoked, ...), or nullptr when the
// code has none (not_found, busy, invalid, unsupported, internal, not_allowed_for_role).
const char* sentenceFor(const std::string& code);
} // namespace text

enum class CodecKind : uint8_t { H264 = 0, HEVC = 1 };
// Invite (v3 §4.4, methodRaw 2) is a remote invite: only the outside listener takes it, and the
// LAN pairing listener closes the connection on it without checking a proof.
enum class PairingMethod : uint8_t { QrToken = 0, ShortCode = 1, Invite = 2 };

constexpr size_t kWireHeaderSize = 8;
constexpr uint8_t kWireMagic0 = 0x4B; // 'K'
constexpr uint8_t kWireMagic1 = 0x56; // 'V'
constexpr uint32_t kWireMaxPayload = 4u * 1024u * 1024u;
// What the host reads from a device, on the stream and the pairing connections: nothing a device sends
// is video, and every message it sends is small JSON (a pairRequest with its certificate is about 2 KB).
// A frame whose header announces more closes the connection before its payload is waited for.
constexpr uint32_t kMaxClientPayload = 64u * 1024u;

// Known to a peer of the given protocol version: v1 knows 0x01-0x06 and 0x10/0x11, v2 adds 0x07-0x0A,
// v3 adds 0x0B-0x0F and, with the camera roll, 0x12-0x19.
bool isKnownMsgType(uint8_t raw, int protocolVersion = kHostProtocolVersion);

std::vector<uint8_t> packFrame(MsgType type, uint8_t flags, const uint8_t* payload, size_t n);
inline std::vector<uint8_t> packFrame(MsgType type, uint8_t flags, const std::string& payload) {
    return packFrame(type, flags, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}
inline std::vector<uint8_t> packFrame(MsgType type, uint8_t flags, const std::vector<uint8_t>& payload) {
    return packFrame(type, flags, payload.data(), payload.size());
}

// Incremental reader, same rules as WireFrameReader. Bad magic or an oversized payload is always a
// protocol violation. A type this reader's version doesn't know is a violation too, unless
// skip_unknown is set: then the whole frame is consumed and returned as Unknown (type holds the raw
// byte), as v2's WireFrameReader does on the stream connection. FrameReader(1) is a v1 peer's
// reader, which drops the connection on 0x07 and up; tests use it to stand in for an old phone.
// max_payload lowers the 4 MiB cap (the host reads devices with kMaxClientPayload); a header announcing
// more is a violation at once, whatever the type.
class FrameReader {
public:
    enum class Result { Frame, Unknown, NeedMore, Violation };
    explicit FrameReader(int protocolVersion = kHostProtocolVersion, bool skip_unknown = false,
                         uint32_t max_payload = kWireMaxPayload)
        : version_(protocolVersion), skip_unknown_(skip_unknown), max_payload_(std::min(max_payload, kWireMaxPayload)) {}
    // Consumed frames are dropped here, once per append, not once per frame.
    void append(const uint8_t* data, size_t n) {
        if (off_ > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off_));
            off_ = 0;
        }
        buf_.insert(buf_.end(), data, data + n);
    }
    void reset() {
        buf_.clear();
        off_ = 0;
    }
    Result next(MsgType& type, uint8_t& flags, std::vector<uint8_t>& payload, std::string* why = nullptr);
    size_t buffered() const { return buf_.size() - off_; }

private:
    std::vector<uint8_t> buf_;
    size_t off_ = 0; // the start of the first unread frame in buf_
    int version_;
    bool skip_unknown_;
    uint32_t max_payload_;
};

struct VideoAccessUnitHeader {
    static constexpr size_t kBinarySize = 29;
    uint32_t streamEpoch = 0;
    uint64_t frameSeq = 0;
    uint64_t hostCaptureNs = 0;
    uint64_t hostEncodeDoneNs = 0;
    uint8_t flags = 0; // bit 0 keyframe, bit 1 clock overlay

    bool isKeyframe() const { return (flags & 0x01) != 0; }
    bool hasClockOverlay() const { return (flags & 0x02) != 0; }

    void encode(const uint8_t* avcc, size_t n, std::vector<uint8_t>& out) const;
    // Swift requires data.count > binarySize (a non-empty payload).
    static bool decode(const uint8_t* data, size_t n, VideoAccessUnitHeader& out, const uint8_t** payload, size_t* payload_len);
};

// 0x19 playbackAccessUnit (v3 §5.9): a 4-byte big-endian stream id, then exactly the 0x03 body, so a
// device hands the rest to its existing 0x03 parser in a second decoder. For playback, au.hostCaptureNs
// is the media time in ns from the clip start, au.hostEncodeDoneNs is 0, au.frameSeq counts from 0 per
// streamEpoch, and only flags bit 0 (keyframe) is ever set.
struct PlaybackAccessUnitHeader {
    static constexpr size_t kBinarySize = 4 + VideoAccessUnitHeader::kBinarySize; // 33
    uint32_t streamId = 0;
    VideoAccessUnitHeader au;

    // False, with out cleared, when the payload is empty or the frame would pass the 4 MiB host -> device
    // cap (§1): such an AU can't be sent, and playback never skips silently, so the caller decides.
    bool encode(const uint8_t* avcc, size_t n, std::vector<uint8_t>& out) const;
    // As 0x03: the AVCC payload must be non-empty (n >= 1, §5.9).
    static bool decode(const uint8_t* data, size_t n, PlaybackAccessUnitHeader& out, const uint8_t** payload,
                       size_t* payload_len);
};

// v3 §4.1: what a v3 host offers, in hello. Roles and settings come later, in 0x0B.
struct PetCameraHello {
    int specVersion = 1;
    int maxWatchers = 4;
    bool cameraRoll = false; // true on a host with the camera roll
    bool outside = false;    // true on a host that can listen outside
    std::vector<std::string> qualities{"auto", "high", "medium", "low"};
};

struct HelloServerMessage {
    int protocolVersion = 1;
    int modeRaw = 0;
    int sbsLayoutRaw = 0;
    bool allowPhoneModeSwitch = true;
    int width = 0;
    int height = 0;
    uint8_t codecRaw = 0;
    // v3: written last and only when set, so a hello without it is byte-identical to v2's. Older
    // decoders (JSONDecoder, json_lite) ignore the extra key.
    std::optional<PetCameraHello> petCamera;
};

struct StreamConfigMessage {
    uint32_t streamEpoch = 0;
    uint8_t codecRaw = 0;
    int width = 0;
    int height = 0;
    int modeRaw = 0;
    int sbsLayoutRaw = 0;
    int nalLengthHeaderBytes = 4;
    std::vector<std::vector<uint8_t>> parameterSets; // [SPS, PPS]
};

struct PingMessage {
    uint64_t clientSendNs = 0;
    uint64_t lastDecodedSeq = 0;
    uint32_t lastHostToDisplayLatencyUs = 0;
    uint32_t lastBarcodeLatencyUs = 0;
    std::optional<int> protocolVersion; // v2 phones send 2; v1 phones leave it out
};

struct PongMessage {
    uint64_t clientSendNs = 0;
    uint64_t serverRecvNs = 0;
    uint64_t serverSendNs = 0;
};

struct SelectModeMessage {
    int modeRaw = 0;
    int sbsLayoutRaw = 0;
};

// ---- protocol v2 ------------------------------------------------------------------------------

// WireSensorPowerState: what the host confirms, never what the phone assumes.
enum class SensorPowerState { Off, Starting, On, InUseElsewhere, Error };
const char* toWire(SensorPowerState s); // "off", "starting", "on", "in_use_elsewhere", "error"
bool fromWire(const std::string& s, SensorPowerState& out);

struct HostStateSyncMessage {
    int protocolVersion = 2; // the message's own version; the host always sets kHostProtocolVersion
    uint64_t stateSeq = 0;
    SensorPowerState powerState = SensorPowerState::Off;
    std::optional<std::string> inUseHolder;  // only with InUseElsewhere
    std::optional<std::string> errorMessage; // only with Error
    int modeRaw = 0;
    int sbsLayoutRaw = 0;
    bool allowPhoneModeSwitch = true;
    bool allowPhoneSensorPower = true; // decoded as true when absent
    std::optional<std::string> sensorPowerDisabledReason;
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
};

enum class RemoteControlAction { SelectMode, SetCutoff, SetHostFrozen, StartRecording, StopRecording, RequestKeyframe };
const char* toWire(RemoteControlAction a); // "select_mode", ...
bool fromWire(const std::string& s, RemoteControlAction& out);

struct RemoteControlCommandMessage {
    RemoteControlAction action = RemoteControlAction::RequestKeyframe;
    std::optional<int> modeRaw;
    std::optional<int> sbsLayoutRaw;
    std::optional<bool> cutoffEnabled;
    std::optional<bool> applyTrackMePreset;
    std::optional<bool> hostFrozen;
    std::optional<bool> reminderConfirmedOnPhone;
    std::optional<bool> snoozeReminderOneHour;
};

struct Orbit3DGestureMessage {
    float deltaYawRadians = 0.0f;
    float deltaPitchRadians = 0.0f;
    float zoomScale = 1.0f;
    float panRightMeters = 0.0f;
    float panUpMeters = 0.0f;
    bool resetView = false;
};

struct SensorPowerControlMessage {
    uint64_t requestId = 0;
    bool turnOn = false;
};

struct PairRequestMessage {
    uint8_t methodRaw = 0;
    std::vector<uint8_t> clientCertDER;
    std::vector<uint8_t> clientProofHMAC;
    // v3 §4.4, sent by v3 devices only. The host trims and cleans deviceName and maps an unknown
    // deviceKind to "other" (normalizeDeviceKind); the decode keeps what was sent.
    std::optional<int> protocolVersion;
    std::optional<std::string> deviceName;
    std::optional<std::string> deviceKind;
};

struct PairResponseMessage {
    bool accepted = false;
    std::string serviceId;
    uint16_t streamPort = 0;
    std::vector<uint8_t> serverProofHMAC;
    std::string message;
    // v3 §4.4, on acceptance only; a rejection keeps today's shape.
    std::optional<std::string> role; // a PetRole wire string
    std::optional<std::string> deviceId;
    std::optional<std::string> deviceName;
    std::optional<int> protocolVersion;
};

// ---- protocol v3: the pet camera (docs/protocol-v3-pet-camera.md) -----------------------------

// v3 §1 rule 6: every v3 enum decodes leniently. An unknown string becomes Unknown and never fails
// the message, so a newer peer's value can't drop a whole 0x0B (the v2 powerState trap). toWire of
// Unknown is "unknown", as the Swift raw value; the host never sends it.
enum class PetPath { Home, Outside, Unknown };      // home = LAN stream listener, outside = the outside listener
// LiveTier values index the tier arrays in live_tiers (High = 0 .. Low = 2); Unknown is decode-only.
enum class LiveTier : int { High = 0, Medium = 1, Low = 2, Unknown = 3 };
enum class LiveQuality { Auto, High, Medium, Low, Unknown };
enum class ClipTrigger { Manual, Motion, Schedule, Unknown };
// 0x17 state (v3 §5.9).
enum class PlaybackState { Starting, Playing, Paused, Ended, Stopped, Error, Unknown };

const char* toWire(PetRole v);
const char* toWire(PetPath v);
const char* toWire(LiveTier v);
const char* toWire(LiveQuality v);
const char* toWire(ClipTrigger v);
const char* toWire(PlaybackState v); // "starting", "playing", "paused", "ended", "stopped", "error"
PetRole petRoleFromWire(const std::string& s);
PetPath petPathFromWire(const std::string& s);
LiveTier liveTierFromWire(const std::string& s);
LiveQuality liveQualityFromWire(const std::string& s);
ClipTrigger clipTriggerFromWire(const std::string& s);
PlaybackState playbackStateFromWire(const std::string& s);

// String-valued keys whose set may grow. They travel as plain strings so an unknown value still
// decodes (the host answers an unknown action with "unsupported"; a device ignores an unknown reason).
namespace pet_action { // 0x0C action (v3 §5.2)
constexpr const char* kSetSharing = "set_sharing";
constexpr const char* kSetPrivacy = "set_privacy";
constexpr const char* kSetRoll = "set_roll";
constexpr const char* kSetMotion = "set_motion";
constexpr const char* kStartClip = "start_clip";
constexpr const char* kStopClip = "stop_clip";
constexpr const char* kStarClip = "star_clip";
constexpr const char* kDeleteClip = "delete_clip";
constexpr const char* kRevokeDevice = "revoke_device";
bool isKnown(const std::string& action);
} // namespace pet_action

namespace playback_action { // 0x16 action (v3 §5.9); an unknown one gets "unsupported"
constexpr const char* kStart = "start";
constexpr const char* kPause = "pause";
constexpr const char* kResume = "resume";
constexpr const char* kSeek = "seek";
constexpr const char* kStop = "stop";
bool isKnown(const std::string& action);
} // namespace playback_action

namespace pet_error { // 0x0D error (v3 §9)
constexpr const char* kNotAllowedForRole = "not_allowed_for_role";
constexpr const char* kPrivacyMode = "privacy_mode";
constexpr const char* kSharingOff = "sharing_off";
constexpr const char* kSharingSchedule = "sharing_schedule";
constexpr const char* kRollDisabled = "roll_disabled";
constexpr const char* kRollPreparing = "roll_preparing"; // the folder is being prepared
constexpr const char* kFolderUnavailable = "folder_unavailable"; // a roll folder set but unusable
constexpr const char* kSensorOff = "sensor_off";
constexpr const char* kViewFrozen = "view_frozen";
constexpr const char* kPcReplaying = "pc_replaying";
constexpr const char* kDiskFloor = "disk_floor";
constexpr const char* kStorageFull = "storage_full";
constexpr const char* kTooManyViewers = "too_many_viewers";
constexpr const char* kNotFound = "not_found";
constexpr const char* kBusy = "busy";
constexpr const char* kInvalid = "invalid";
constexpr const char* kUnsupported = "unsupported";
constexpr const char* kInternal = "internal";
bool isKnown(const std::string& code);
} // namespace pet_error

namespace live_unavailable { // 0x0B liveUnavailableReason (v3 §5.1)
constexpr const char* kPrivacyMode = "privacy_mode";
constexpr const char* kPcReplaying = "pc_replaying";
constexpr const char* kSharingOff = "sharing_off";
constexpr const char* kSharingSchedule = "sharing_schedule";
constexpr const char* kTooManyViewers = "too_many_viewers";
} // namespace live_unavailable

namespace roll_blocked { // 0x0B rollBlockedReason, and the start_clip error (v3 §5.1, §5.2)
constexpr const char* kPrivacyMode = "privacy_mode";
constexpr const char* kRollDisabled = "roll_disabled";
// The roll is on and its folder is being prepared: right after roll_disabled in the §5.1
// order. Additive: a device that doesn't know it shows rollBlockedText.
constexpr const char* kRollPreparing = "roll_preparing";
// A camera-roll folder is set on the PC but can't be used (a missing drive, a refused folder): there is no
// roll (cameraRoll false), and the owner is told why. After roll_disabled in the §5.1 order.
constexpr const char* kFolderUnavailable = "folder_unavailable";
constexpr const char* kSensorOff = "sensor_off";
constexpr const char* kViewFrozen = "view_frozen";
constexpr const char* kPcReplaying = "pc_replaying";
constexpr const char* kDiskFloor = "disk_floor";
constexpr const char* kStorageFull = "storage_full";
bool isKnown(const std::string& reason);
} // namespace roll_blocked

namespace outside_status { // 0x0B outsideStatus, owners only (v3 §5.1)
constexpr const char* kOff = "off";
constexpr const char* kMapping = "mapping";
constexpr const char* kMapped = "mapped";
constexpr const char* kManual = "manual";
constexpr const char* kCarrierNat = "carrier_nat";
constexpr const char* kAddressChanged = "address_changed";
constexpr const char* kError = "error";
} // namespace outside_status

namespace notice_reason { // 0x0F reason (v3 §5.5)
constexpr const char* kRevoked = "revoked";
constexpr const char* kTooManySessions = "too_many_sessions";
constexpr const char* kReplaced = "replaced";
constexpr const char* kOutsideDisabled = "outside_disabled";
constexpr const char* kHostShuttingDown = "host_shutting_down";
constexpr const char* kAddressChanged = "address_changed";
} // namespace notice_reason

namespace watcher_activity { // PetWatcher activity
constexpr const char* kLive = "live";
constexpr const char* kPlayback = "playback";
} // namespace watcher_activity

namespace device_kind { // pairRequest deviceKind and PetDevice kind
constexpr const char* kIphone = "iphone";
constexpr const char* kIpad = "ipad";
constexpr const char* kMac = "mac";
constexpr const char* kOther = "other";
} // namespace device_kind
// v3 §4.4: "iphone", "ipad", "mac" or "other"; anything else (absent included) becomes "other".
std::string normalizeDeviceKind(const std::optional<std::string>& kind);

// v3 §5.1 PetEndpoint: the address to use from outside the home.
struct PetEndpoint {
    std::string host; // dotted IPv4, or a DNS name only if one is set later
    uint16_t port = 0;
    bool verified = false; // false when typed in on the PC and not read from the router
};

// One entry per session holding a watcher slot.
struct PetWatcher {
    std::string deviceId;
    std::string deviceName;
    PetRole role = PetRole::Viewer;
    PetPath path = PetPath::Home;
    std::string activity = watcher_activity::kLive; // "live" or "playback"
};

// One entry per paired device.
struct PetDevice {
    std::string deviceId;
    std::string name;
    PetRole role = PetRole::Viewer;
    std::string kind = device_kind::kOther;
    uint64_t pairedAtUnixMs = 0;
    std::optional<uint64_t> lastSeenUnixMs; // absent if it never connected
    bool connected = false;
};

// 0x0B, host -> device. The defaults are what a host without the camera roll or outside listening sends (v3 §5.1).
// Owner-only keys (outsideStatus, outsideStatusText, watchingNow, devices) must stay unset for a
// viewer: they are left out, never sent empty.
struct PetCameraStateMessage {
    int protocolVersion = kProtocolV3;
    uint64_t stateSeq = 0;
    std::string deviceId;
    std::string deviceName;
    PetRole role = PetRole::Viewer;
    PetPath path = PetPath::Home;
    bool sharingEnabled = false;
    bool sharingScheduleOff = false;
    bool outsideEnabled = false;
    bool privacyMode = false;
    bool liveAllowed = false;
    std::optional<std::string> liveUnavailableReason; // live_unavailable::*, when liveAllowed is false
    bool watching = false;
    std::string liveQuality = "auto"; // LiveQuality wire string, as requested
    std::string liveTier = "high";    // LiveTier wire string, as sent ("high" when not watching)
    bool rollAvailable = false;
    bool playbackAvailable = false;
    bool rollEnabled = false;
    bool motionEnabled = false;
    bool motionAvailable = false;
    int motionSensitivity = 5; // 1 (least) to 10 (most)
    bool scheduleEnabled = false;
    std::optional<std::string> scheduleSummary;
    bool clipRecording = false;
    std::optional<std::string> clipTrigger; // ClipTrigger wire string, while recording
    bool clipManualActive = false;
    double clipElapsedSec = 0.0;
    std::optional<std::string> rollBlockedReason; // roll_blocked::*
    std::optional<std::string> rollBlockedText;   // its text:: sentence
    std::optional<PetEndpoint> outsideEndpoint;   // both roles
    // Both roles: every address a device may dial from outside, outsideEndpoint first, then this PC's own overlay
    // (Tailscale) IPv4 address, verified false. The host sends it in every 0x0B, empty when there are none, so a
    // device clears its list. Absent from older hosts: a device uses outsideEndpoint.
    std::optional<std::vector<PetEndpoint>> outsideCandidates;
    std::optional<std::string> outsideStatus;     // owners only: outside_status::*
    std::optional<std::string> outsideStatusText; // owners only
    std::optional<std::vector<PetWatcher>> watchingNow; // owners only
    std::optional<std::vector<PetDevice>> devices;      // owners only
};

// 0x0C, device -> host. Every request carries a client-chosen, increasing requestId that the
// answer echoes. action is a plain string so an unknown one still decodes and gets "unsupported".
struct PetCameraCommandMessage {
    uint64_t requestId = 0;
    std::string action;                 // pet_action::*
    std::optional<bool> enabled;        // set_sharing, set_privacy, set_roll, set_motion
    std::optional<int> sensitivity;     // set_motion (1-10, otherwise "invalid")
    std::optional<std::string> clipId;  // star_clip, delete_clip
    std::optional<bool> starred;        // star_clip
    std::optional<std::string> deviceId; // revoke_device
};

// 0x0D, host -> device: the one answer to a 0x0C (and to a failed 0x12/0x14/0x16).
struct CommandResultMessage {
    uint64_t requestId = 0;
    bool ok = false;
    std::optional<std::string> error;   // pet_error::*, when ok is false
    std::optional<std::string> message; // a sentence to show
};

// 0x0E, device -> host. No requestId: the answer is always the next 0x0B.
struct LiveViewRequestMessage {
    bool watch = true;
    std::optional<std::string> quality; // LiveQuality wire string; absent keeps the current one
};

// 0x0F, host -> device.
struct SessionNoticeMessage {
    std::string reason; // notice_reason::*
    bool closing = true;
    std::optional<std::string> message; // the text:: sentence
};
// The notice for a reason as v3 §5.5 and §9 give it: closing is false only for address_changed,
// and message is the reason's sentence when it has one.
SessionNoticeMessage makeSessionNotice(const std::string& reason);

// ---- protocol v3: the camera roll (v3 §5.6-5.9) -------------------------------------------------
// The decoders below check JSON shape only (§5.10: a message that fails is ignored, not answered).
// The values a device sends (clip id, day, cursor, limit, the keys an action needs) are checked
// afterwards by roll_wire.hpp, which picks the 0x0D code a well-formed request gets.

// 0x12, device -> host. No day lists the days; a day lists that day's clips.
struct RollListRequestMessage {
    uint64_t requestId = 0;
    std::optional<std::string> day;    // YYYY-MM-DD
    std::optional<std::string> cursor; // an earlier nextCursor, opaque to the device
    std::optional<int> limit;          // 1-100, default 50
};

struct RollDay {
    std::string day; // YYYY-MM-DD
    int clipCount = 0;
    int starredCount = 0;
    uint64_t bytes = 0;
};

struct RollClip {
    std::string clipId; // §7.1
    uint64_t startUnixMs = 0;
    std::string startLocal; // PC-local ISO 8601 with offset, e.g. 2026-11-18T14:03:22-05:00
    double durationSec = 0.0;
    std::string trigger = "manual";       // ClipTrigger wire string: the trigger that started it
    std::optional<std::string> startedBy; // a device name or "This PC"; absent for motion and schedule
    bool starred = false;
    bool starredByMe = false; // this device set the star, so a viewer may unstar it
    int width = 0;
    int height = 0;
    uint64_t bytes = 0;
    bool hasThumbnail = false;
};

// The whole roll.
struct RollStorage {
    uint64_t usedBytes = 0;
    uint64_t capBytes = 0;
    uint64_t freeBytes = 0;               // free space on the roll's disk
    uint64_t floorBytes = 20000000000ULL; // the disk floor (pet-camera.md §6.4)
    int keepDays = 30;
};

// 0x13, host -> device. Both lists are newest first; exactly one is set.
struct RollListMessage {
    uint64_t requestId = 0;
    std::optional<std::string> day;                // echoed when clips are listed
    std::optional<std::vector<RollDay>> days;      // when listing days
    std::optional<std::vector<RollClip>> clips;    // when listing clips
    std::optional<std::string> nextCursor;         // absent on the last page
    RollStorage storage;
};

// 0x14, device -> host. Both keys required.
struct RollThumbRequestMessage {
    uint64_t requestId = 0;
    std::string clipId;
};

// 0x15, host -> device.
struct RollThumbMessage {
    static constexpr size_t kMaxJpegBytes = 64u * 1024u; // decoded (§5.8)
    uint64_t requestId = 0;
    std::string clipId;
    std::vector<uint8_t> jpeg; // padded standard base64 on the wire
};

// 0x16, device -> host. action is a plain string so an unknown one still decodes and gets
// "unsupported", as 0x0C does.
struct PlaybackRequestMessage {
    uint64_t requestId = 0;
    std::string action;                // playback_action::*
    std::optional<std::string> clipId; // required for start
    std::optional<uint32_t> streamId;  // required for every action except start
    std::optional<double> positionSec; // start (default 0) and seek (required); clamped to [0, duration]
};

// 0x17, host -> device: on every state change, as the answer to a 0x16, and at most 1 Hz while playing.
struct PlaybackStateMessage {
    std::optional<uint64_t> requestId; // present when it answers a 0x16
    uint32_t streamId = 0;
    std::string clipId;
    std::string state = "starting"; // PlaybackState wire string
    double positionSec = 0.0;
    double durationSec = 0.0;
    std::optional<std::string> message; // with error, and with a host-initiated stopped
};

// 0x18, host -> device: every 0x02 key, in 0x02's order and with its types, then streamId, clipId and
// durationSec. config.streamEpoch is per stream: 1 at the start, one higher on each seek.
// config.modeRaw and sbsLayoutRaw are the view the clip was recorded in.
struct PlaybackConfigMessage {
    StreamConfigMessage config;
    uint32_t streamId = 0; // >= 1, unique per session
    std::string clipId;
    double durationSec = 0.0;
};

std::string toJson(const HelloServerMessage& m);
std::string toJson(const StreamConfigMessage& m);
std::string toJson(const PingMessage& m);
std::string toJson(const PongMessage& m);
std::string toJson(const SelectModeMessage& m);
std::string toJson(const HostStateSyncMessage& m);
std::string toJson(const RemoteControlCommandMessage& m);
std::string toJson(const Orbit3DGestureMessage& m);
std::string toJson(const SensorPowerControlMessage& m);
std::string toJson(const PairRequestMessage& m);
std::string toJson(const PairResponseMessage& m);
std::string toJson(const PetCameraHello& m);
std::string toJson(const PetEndpoint& m);
std::string toJson(const PetWatcher& m);
std::string toJson(const PetDevice& m);
std::string toJson(const PetCameraStateMessage& m);
std::string toJson(const PetCameraCommandMessage& m);
std::string toJson(const CommandResultMessage& m);
std::string toJson(const LiveViewRequestMessage& m);
std::string toJson(const SessionNoticeMessage& m);
std::string toJson(const RollListRequestMessage& m);
std::string toJson(const RollDay& m);
std::string toJson(const RollClip& m);
std::string toJson(const RollStorage& m);
std::string toJson(const RollListMessage& m);
std::string toJson(const RollThumbRequestMessage& m);
std::string toJson(const RollThumbMessage& m);
std::string toJson(const PlaybackRequestMessage& m);
std::string toJson(const PlaybackStateMessage& m);
std::string toJson(const PlaybackConfigMessage& m);

bool fromJson(const uint8_t* data, size_t n, HelloServerMessage& m);
bool fromJson(const uint8_t* data, size_t n, StreamConfigMessage& m);
bool fromJson(const uint8_t* data, size_t n, PingMessage& m);
bool fromJson(const uint8_t* data, size_t n, PongMessage& m);
bool fromJson(const uint8_t* data, size_t n, SelectModeMessage& m);
bool fromJson(const uint8_t* data, size_t n, HostStateSyncMessage& m);
bool fromJson(const uint8_t* data, size_t n, RemoteControlCommandMessage& m);
bool fromJson(const uint8_t* data, size_t n, Orbit3DGestureMessage& m);
bool fromJson(const uint8_t* data, size_t n, SensorPowerControlMessage& m);
bool fromJson(const uint8_t* data, size_t n, PairRequestMessage& m);
bool fromJson(const uint8_t* data, size_t n, PairResponseMessage& m);
// v3 (§5.10): false on bad JSON, a missing required key or a key of the wrong type; the message is
// then ignored and not answered. Enum-valued keys never fail on an unknown value (§1 rule 6).
bool fromJson(const uint8_t* data, size_t n, PetCameraHello& m);
bool fromJson(const uint8_t* data, size_t n, PetEndpoint& m);
bool fromJson(const uint8_t* data, size_t n, PetWatcher& m);
bool fromJson(const uint8_t* data, size_t n, PetDevice& m);
bool fromJson(const uint8_t* data, size_t n, PetCameraStateMessage& m);
bool fromJson(const uint8_t* data, size_t n, PetCameraCommandMessage& m);
bool fromJson(const uint8_t* data, size_t n, CommandResultMessage& m);
bool fromJson(const uint8_t* data, size_t n, LiveViewRequestMessage& m);
bool fromJson(const uint8_t* data, size_t n, SessionNoticeMessage& m);
bool fromJson(const uint8_t* data, size_t n, RollListRequestMessage& m);
bool fromJson(const uint8_t* data, size_t n, RollDay& m);
bool fromJson(const uint8_t* data, size_t n, RollClip& m);
bool fromJson(const uint8_t* data, size_t n, RollStorage& m);
bool fromJson(const uint8_t* data, size_t n, RollListMessage& m);
bool fromJson(const uint8_t* data, size_t n, RollThumbRequestMessage& m);
// Also false when jpeg decodes to more than RollThumbMessage::kMaxJpegBytes.
bool fromJson(const uint8_t* data, size_t n, RollThumbMessage& m);
bool fromJson(const uint8_t* data, size_t n, PlaybackRequestMessage& m);
bool fromJson(const uint8_t* data, size_t n, PlaybackStateMessage& m);
bool fromJson(const uint8_t* data, size_t n, PlaybackConfigMessage& m);

template <typename T>
bool fromJson(const std::vector<uint8_t>& v, T& m) {
    return fromJson(v.data(), v.size(), m);
}
template <typename T>
bool fromJson(const std::string& s, T& m) {
    return fromJson(reinterpret_cast<const uint8_t*>(s.data()), s.size(), m);
}

} // namespace kvhost
