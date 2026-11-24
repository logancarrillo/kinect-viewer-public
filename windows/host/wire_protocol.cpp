#include "wire_protocol.hpp"

#include "json_lite.hpp"

namespace kvhost {

namespace {

void putU32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v >> 24));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void putU64(std::vector<uint8_t>& o, uint64_t v) {
    putU32(o, static_cast<uint32_t>(v >> 32));
    putU32(o, static_cast<uint32_t>(v));
}

uint32_t getU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t getU64(const uint8_t* p) {
    return (static_cast<uint64_t>(getU32(p)) << 32) | getU32(p + 4);
}

bool parseObject(const uint8_t* data, size_t n, json::Value& v) {
    return json::parse(reinterpret_cast<const char*>(data), n, v) && v.type == json::Value::Type::Object;
}

} // namespace

bool isKnownMsgType(uint8_t raw, int protocolVersion) {
    switch (raw) {
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x10: case 0x11:
            return true;
        case 0x07: case 0x08: case 0x09: case 0x0A:
            return protocolVersion >= 2;
        case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
            return protocolVersion >= kProtocolV3;
        default:
            return false;
    }
}

namespace text {
std::string inUseBy(const std::string& holder) {
    return "The Kinect is in use by " + holderOrAnotherProcess(holder);
}

const char* sentenceFor(const std::string& code) {
    struct Entry {
        const char* code;
        const char* sentence;
    };
    static const Entry kEntries[] = {
        {"privacy_mode", kPrivacyMode},         {"pc_replaying", kPcReplaying},
        {"sharing_off", kSharingOff},           {"sharing_schedule", kSharingSchedule},
        {"too_many_viewers", kTooManyViewers},  {"roll_disabled", kRollDisabled},
        {"roll_preparing", kRollPreparing},
        {"folder_unavailable", kFolderUnavailable},
        {"sensor_off", kSensorOff},             {"view_frozen", kViewFrozen},
        {"disk_floor", kDiskFloor},             {"storage_full", kStorageFull},
        {"revoked", kRevoked},                  {"replaced", kReplaced},
        {"outside_disabled", kOutsideDisabled}, {"address_changed", kAddressChanged},
        {"host_shutting_down", kHostShuttingDown}, {"too_many_sessions", kTooManySessions},
    };
    for (const auto& e : kEntries) {
        if (code == e.code) return e.sentence;
    }
    return nullptr;
}
} // namespace text

const char* toWire(SensorPowerState s) {
    switch (s) {
        case SensorPowerState::Off: return "off";
        case SensorPowerState::Starting: return "starting";
        case SensorPowerState::On: return "on";
        case SensorPowerState::InUseElsewhere: return "in_use_elsewhere";
        case SensorPowerState::Error: return "error";
    }
    return "error";
}

bool fromWire(const std::string& s, SensorPowerState& out) {
    if (s == "off") out = SensorPowerState::Off;
    else if (s == "starting") out = SensorPowerState::Starting;
    else if (s == "on") out = SensorPowerState::On;
    else if (s == "in_use_elsewhere") out = SensorPowerState::InUseElsewhere;
    else if (s == "error") out = SensorPowerState::Error;
    else return false;
    return true;
}

const char* toWire(RemoteControlAction a) {
    switch (a) {
        case RemoteControlAction::SelectMode: return "select_mode";
        case RemoteControlAction::SetCutoff: return "set_cutoff";
        case RemoteControlAction::SetHostFrozen: return "set_host_frozen";
        case RemoteControlAction::StartRecording: return "start_recording";
        case RemoteControlAction::StopRecording: return "stop_recording";
        case RemoteControlAction::RequestKeyframe: return "request_keyframe";
    }
    return "request_keyframe";
}

bool fromWire(const std::string& s, RemoteControlAction& out) {
    if (s == "select_mode") out = RemoteControlAction::SelectMode;
    else if (s == "set_cutoff") out = RemoteControlAction::SetCutoff;
    else if (s == "set_host_frozen") out = RemoteControlAction::SetHostFrozen;
    else if (s == "start_recording") out = RemoteControlAction::StartRecording;
    else if (s == "stop_recording") out = RemoteControlAction::StopRecording;
    else if (s == "request_keyframe") out = RemoteControlAction::RequestKeyframe;
    else return false;
    return true;
}

// ---- protocol v3 enums and string sets ----------------------------------------------------------

const char* toWire(PetRole v) {
    switch (v) {
        case PetRole::Owner: return "owner";
        case PetRole::Viewer: return "viewer";
        case PetRole::Unknown: break;
    }
    return "unknown";
}
const char* toWire(PetPath v) {
    switch (v) {
        case PetPath::Home: return "home";
        case PetPath::Outside: return "outside";
        case PetPath::Unknown: break;
    }
    return "unknown";
}
const char* toWire(LiveTier v) {
    switch (v) {
        case LiveTier::High: return "high";
        case LiveTier::Medium: return "medium";
        case LiveTier::Low: return "low";
        case LiveTier::Unknown: break;
    }
    return "unknown";
}
const char* toWire(LiveQuality v) {
    switch (v) {
        case LiveQuality::Auto: return "auto";
        case LiveQuality::High: return "high";
        case LiveQuality::Medium: return "medium";
        case LiveQuality::Low: return "low";
        case LiveQuality::Unknown: break;
    }
    return "unknown";
}
const char* toWire(ClipTrigger v) {
    switch (v) {
        case ClipTrigger::Manual: return "manual";
        case ClipTrigger::Motion: return "motion";
        case ClipTrigger::Schedule: return "schedule";
        case ClipTrigger::Unknown: break;
    }
    return "unknown";
}
const char* toWire(PlaybackState v) {
    switch (v) {
        case PlaybackState::Starting: return "starting";
        case PlaybackState::Playing: return "playing";
        case PlaybackState::Paused: return "paused";
        case PlaybackState::Ended: return "ended";
        case PlaybackState::Stopped: return "stopped";
        case PlaybackState::Error: return "error";
        case PlaybackState::Unknown: break;
    }
    return "unknown";
}

// Lenient: exact, case-sensitive match on the snake_case value, anything else is Unknown. The
// string "unknown" itself is Unknown too, so it round-trips.
PetRole petRoleFromWire(const std::string& s) {
    if (s == "owner") return PetRole::Owner;
    if (s == "viewer") return PetRole::Viewer;
    return PetRole::Unknown;
}
PetPath petPathFromWire(const std::string& s) {
    if (s == "home") return PetPath::Home;
    if (s == "outside") return PetPath::Outside;
    return PetPath::Unknown;
}
LiveTier liveTierFromWire(const std::string& s) {
    if (s == "high") return LiveTier::High;
    if (s == "medium") return LiveTier::Medium;
    if (s == "low") return LiveTier::Low;
    return LiveTier::Unknown;
}
LiveQuality liveQualityFromWire(const std::string& s) {
    if (s == "auto") return LiveQuality::Auto;
    if (s == "high") return LiveQuality::High;
    if (s == "medium") return LiveQuality::Medium;
    if (s == "low") return LiveQuality::Low;
    return LiveQuality::Unknown;
}
ClipTrigger clipTriggerFromWire(const std::string& s) {
    if (s == "manual") return ClipTrigger::Manual;
    if (s == "motion") return ClipTrigger::Motion;
    if (s == "schedule") return ClipTrigger::Schedule;
    return ClipTrigger::Unknown;
}
PlaybackState playbackStateFromWire(const std::string& s) {
    if (s == "starting") return PlaybackState::Starting;
    if (s == "playing") return PlaybackState::Playing;
    if (s == "paused") return PlaybackState::Paused;
    if (s == "ended") return PlaybackState::Ended;
    if (s == "stopped") return PlaybackState::Stopped;
    if (s == "error") return PlaybackState::Error;
    return PlaybackState::Unknown;
}

namespace {
template <size_t N>
bool oneOf(const std::string& s, const char* const (&set)[N]) {
    for (const char* x : set) {
        if (s == x) return true;
    }
    return false;
}
} // namespace

namespace pet_action {
bool isKnown(const std::string& action) {
    static const char* const kAll[] = {kSetSharing, kSetPrivacy, kSetRoll,    kSetMotion,    kStartClip,
                                       kStopClip,   kStarClip,   kDeleteClip, kRevokeDevice};
    return oneOf(action, kAll);
}
} // namespace pet_action

namespace playback_action {
bool isKnown(const std::string& action) {
    static const char* const kAll[] = {kStart, kPause, kResume, kSeek, kStop};
    return oneOf(action, kAll);
}
} // namespace playback_action

namespace roll_blocked {
bool isKnown(const std::string& reason) {
    static const char* const kAll[] = {kPrivacyMode, kRollDisabled, kRollPreparing, kFolderUnavailable, kSensorOff, kViewFrozen,
                                       kPcReplaying, kDiskFloor,    kStorageFull};
    return oneOf(reason, kAll);
}
} // namespace roll_blocked

namespace pet_error {
bool isKnown(const std::string& code) {
    static const char* const kAll[] = {kNotAllowedForRole, kPrivacyMode, kSharingOff,  kSharingSchedule,
                                       kRollDisabled,      kRollPreparing, kFolderUnavailable, kSensorOff, kViewFrozen, kPcReplaying,
                                       kDiskFloor,         kStorageFull, kTooManyViewers, kNotFound,
                                       kBusy,              kInvalid,     kUnsupported, kInternal};
    return oneOf(code, kAll);
}
} // namespace pet_error

std::string normalizeDeviceKind(const std::optional<std::string>& kind) {
    static const char* const kAll[] = {device_kind::kIphone, device_kind::kIpad, device_kind::kMac, device_kind::kOther};
    return (kind && oneOf(*kind, kAll)) ? *kind : std::string(device_kind::kOther);
}

SessionNoticeMessage makeSessionNotice(const std::string& reason) {
    SessionNoticeMessage n;
    n.reason = reason;
    n.closing = reason != notice_reason::kAddressChanged;
    if (const char* s = text::sentenceFor(reason)) n.message = s;
    return n;
}

std::vector<uint8_t> packFrame(MsgType type, uint8_t flags, const uint8_t* payload, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(kWireHeaderSize + n);
    out.push_back(kWireMagic0);
    out.push_back(kWireMagic1);
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(flags);
    putU32(out, static_cast<uint32_t>(n));
    if (n) out.insert(out.end(), payload, payload + n);
    return out;
}

FrameReader::Result FrameReader::next(MsgType& type, uint8_t& flags, std::vector<uint8_t>& payload, std::string* why) {
    const size_t avail = buf_.size() - off_;
    if (avail < kWireHeaderSize) return Result::NeedMore;
    const uint8_t* h = buf_.data() + off_;
    if (h[0] != kWireMagic0 || h[1] != kWireMagic1) {
        if (why) *why = "Invalid wire magic bytes.";
        return Result::Violation;
    }
    const bool known = isKnownMsgType(h[2], version_);
    if (!known && !skip_unknown_) {
        if (why) *why = "Unknown wire message type.";
        return Result::Violation;
    }
    const uint32_t len = getU32(h + 4);
    if (len > max_payload_) {
        if (why) *why = "Wire frame exceeds max payload limit.";
        return Result::Violation;
    }
    if (avail < kWireHeaderSize + len) return Result::NeedMore;
    type = static_cast<MsgType>(h[2]);
    flags = h[3];
    payload.assign(h + kWireHeaderSize, h + kWireHeaderSize + len);
    off_ += kWireHeaderSize + len;
    if (off_ == buf_.size()) {
        buf_.clear(); // everything read: nothing to move
        off_ = 0;
    }
    return known ? Result::Frame : Result::Unknown;
}

namespace {
// The 29-byte 0x03 body header. 0x19 writes the same bytes behind its stream id (v3 §5.9), so both
// come from this one function.
void putAuHeader(std::vector<uint8_t>& out, const VideoAccessUnitHeader& h) {
    putU32(out, h.streamEpoch);
    putU64(out, h.frameSeq);
    putU64(out, h.hostCaptureNs);
    putU64(out, h.hostEncodeDoneNs);
    out.push_back(h.flags);
}
} // namespace

void VideoAccessUnitHeader::encode(const uint8_t* avcc, size_t n, std::vector<uint8_t>& out) const {
    out.clear();
    out.reserve(kBinarySize + n);
    putAuHeader(out, *this);
    if (n) out.insert(out.end(), avcc, avcc + n);
}

bool VideoAccessUnitHeader::decode(const uint8_t* d, size_t n, VideoAccessUnitHeader& h, const uint8_t** payload,
                                   size_t* payload_len) {
    if (!d || n <= kBinarySize) return false;
    h.streamEpoch = getU32(d);
    h.frameSeq = getU64(d + 4);
    h.hostCaptureNs = getU64(d + 12);
    h.hostEncodeDoneNs = getU64(d + 20);
    h.flags = d[28];
    if (payload) *payload = d + kBinarySize;
    if (payload_len) *payload_len = n - kBinarySize;
    return true;
}

bool PlaybackAccessUnitHeader::encode(const uint8_t* avcc, size_t n, std::vector<uint8_t>& out) const {
    out.clear();
    if (!avcc || n == 0 || n > kWireMaxPayload - kBinarySize) return false;
    out.reserve(kBinarySize + n);
    putU32(out, streamId);
    putAuHeader(out, au);
    out.insert(out.end(), avcc, avcc + n);
    return true;
}

bool PlaybackAccessUnitHeader::decode(const uint8_t* d, size_t n, PlaybackAccessUnitHeader& h, const uint8_t** payload,
                                      size_t* payload_len) {
    if (!d || n <= kBinarySize) return false;
    h.streamId = getU32(d);
    return VideoAccessUnitHeader::decode(d + 4, n - 4, h.au, payload, payload_len);
}

// ---- JSON (declaration order, as Swift's synthesized Codable encodes) ----------------------------

// v3 nested objects: one writer and one reader each, shared by the top-level toJson/fromJson and the
// messages that embed them.
namespace {

json::ObjectWriter writer(const PetCameraHello& m) {
    json::ObjectWriter w;
    w.field("specVersion", m.specVersion)
        .field("maxWatchers", m.maxWatchers)
        .field("cameraRoll", m.cameraRoll)
        .field("outside", m.outside)
        .stringArray("qualities", m.qualities);
    return w;
}

json::ObjectWriter writer(const PetEndpoint& m) {
    json::ObjectWriter w;
    w.field("host", m.host).field("port", m.port).field("verified", m.verified);
    return w;
}

json::ObjectWriter writer(const PetWatcher& m) {
    json::ObjectWriter w;
    w.field("deviceId", m.deviceId)
        .field("deviceName", m.deviceName)
        .field("role", toWire(m.role))
        .field("path", toWire(m.path))
        .field("activity", m.activity);
    return w;
}

json::ObjectWriter writer(const PetDevice& m) {
    json::ObjectWriter w;
    w.field("deviceId", m.deviceId)
        .field("name", m.name)
        .field("role", toWire(m.role))
        .field("kind", m.kind)
        .field("pairedAtUnixMs", m.pairedAtUnixMs);
    if (m.lastSeenUnixMs) w.field("lastSeenUnixMs", *m.lastSeenUnixMs);
    w.field("connected", m.connected);
    return w;
}

// A required lenient-enum key: missing or not a string fails; an unknown value is Unknown.
template <typename E>
bool getEnum(const json::Value& v, const char* k, E& out, E (*from)(const std::string&)) {
    std::string s;
    if (!json::getString(v, k, s)) return false;
    out = from(s);
    return true;
}

bool fromValue(const json::Value& v, PetCameraHello& m) {
    return json::getInt(v, "specVersion", m.specVersion) && json::getInt(v, "maxWatchers", m.maxWatchers) &&
           json::getBool(v, "cameraRoll", m.cameraRoll) && json::getBool(v, "outside", m.outside) &&
           json::getStringArray(v, "qualities", m.qualities);
}

bool fromValue(const json::Value& v, PetEndpoint& m) {
    return json::getString(v, "host", m.host) && json::getU16(v, "port", m.port) &&
           json::getBool(v, "verified", m.verified);
}

bool fromValue(const json::Value& v, PetWatcher& m) {
    return json::getString(v, "deviceId", m.deviceId) && json::getString(v, "deviceName", m.deviceName) &&
           getEnum(v, "role", m.role, petRoleFromWire) && getEnum(v, "path", m.path, petPathFromWire) &&
           json::getString(v, "activity", m.activity);
}

bool fromValue(const json::Value& v, PetDevice& m) {
    return json::getString(v, "deviceId", m.deviceId) && json::getString(v, "name", m.name) &&
           getEnum(v, "role", m.role, petRoleFromWire) && json::getString(v, "kind", m.kind) &&
           json::getU64(v, "pairedAtUnixMs", m.pairedAtUnixMs) &&
           json::getOptU64(v, "lastSeenUnixMs", m.lastSeenUnixMs) && json::getBool(v, "connected", m.connected);
}

// 0x02's keys. 0x18 writes and reads exactly these before its own (v3 §5.9), so a device parses it
// with its 0x02 code.
json::ObjectWriter& writeStreamConfig(json::ObjectWriter& w, const StreamConfigMessage& m) {
    return w.field("streamEpoch", m.streamEpoch)
        .field("codecRaw", m.codecRaw)
        .field("width", m.width)
        .field("height", m.height)
        .field("modeRaw", m.modeRaw)
        .field("sbsLayoutRaw", m.sbsLayoutRaw)
        .field("nalLengthHeaderBytes", m.nalLengthHeaderBytes)
        .bytesArray("parameterSets", m.parameterSets);
}

bool fromValue(const json::Value& v, StreamConfigMessage& m) {
    if (!json::getU32(v, "streamEpoch", m.streamEpoch) || !json::getU8(v, "codecRaw", m.codecRaw) ||
        !json::getInt(v, "width", m.width) || !json::getInt(v, "height", m.height) || !json::getInt(v, "modeRaw", m.modeRaw) ||
        !json::getInt(v, "sbsLayoutRaw", m.sbsLayoutRaw) || !json::getInt(v, "nalLengthHeaderBytes", m.nalLengthHeaderBytes)) {
        return false;
    }
    const json::Value* sets = v.get("parameterSets");
    if (!sets || sets->type != json::Value::Type::Array) return false;
    m.parameterSets.clear();
    for (const auto& item : sets->items) {
        std::vector<uint8_t> b;
        if (!item.asBytes(b)) return false;
        m.parameterSets.push_back(std::move(b));
    }
    return true;
}

// ---- camera roll nested objects (v3 §5.7), keys in the order of the spec's tables -----------------

json::ObjectWriter writer(const RollDay& m) {
    json::ObjectWriter w;
    w.field("day", m.day).field("clipCount", m.clipCount).field("starredCount", m.starredCount).field("bytes", m.bytes);
    return w;
}

json::ObjectWriter writer(const RollClip& m) {
    json::ObjectWriter w;
    w.field("clipId", m.clipId)
        .field("startUnixMs", m.startUnixMs)
        .field("startLocal", m.startLocal)
        .doubleField("durationSec", m.durationSec)
        .field("trigger", m.trigger);
    if (m.startedBy) w.field("startedBy", *m.startedBy);
    w.field("starred", m.starred)
        .field("starredByMe", m.starredByMe)
        .field("width", m.width)
        .field("height", m.height)
        .field("bytes", m.bytes)
        .field("hasThumbnail", m.hasThumbnail);
    return w;
}

json::ObjectWriter writer(const RollStorage& m) {
    json::ObjectWriter w;
    w.field("usedBytes", m.usedBytes)
        .field("capBytes", m.capBytes)
        .field("freeBytes", m.freeBytes)
        .field("floorBytes", m.floorBytes)
        .field("keepDays", m.keepDays);
    return w;
}

bool fromValue(const json::Value& v, RollDay& m) {
    return json::getString(v, "day", m.day) && json::getInt(v, "clipCount", m.clipCount) &&
           json::getInt(v, "starredCount", m.starredCount) && json::getU64(v, "bytes", m.bytes);
}

// trigger stays a string, so an unknown one decodes (clipTriggerFromWire maps it to Unknown).
bool fromValue(const json::Value& v, RollClip& m) {
    return json::getString(v, "clipId", m.clipId) && json::getU64(v, "startUnixMs", m.startUnixMs) &&
           json::getString(v, "startLocal", m.startLocal) && json::getDouble(v, "durationSec", m.durationSec) &&
           json::getString(v, "trigger", m.trigger) && json::getOptString(v, "startedBy", m.startedBy) &&
           json::getBool(v, "starred", m.starred) && json::getBool(v, "starredByMe", m.starredByMe) &&
           json::getInt(v, "width", m.width) && json::getInt(v, "height", m.height) && json::getU64(v, "bytes", m.bytes) &&
           json::getBool(v, "hasThumbnail", m.hasThumbnail);
}

bool fromValue(const json::Value& v, RollStorage& m) {
    return json::getU64(v, "usedBytes", m.usedBytes) && json::getU64(v, "capBytes", m.capBytes) &&
           json::getU64(v, "freeBytes", m.freeBytes) && json::getU64(v, "floorBytes", m.floorBytes) &&
           json::getInt(v, "keepDays", m.keepDays);
}

// An optional array of objects, each read with fromValue; one bad element fails the whole key.
template <typename T>
bool getOptArrayOf(const json::Value& v, const char* k, std::optional<std::vector<T>>& out) {
    out.reset();
    std::optional<std::vector<const json::Value*>> items;
    if (!json::getOptObjectArray(v, k, items)) return false;
    if (!items) return true;
    std::vector<T> parsed;
    parsed.reserve(items->size());
    for (const json::Value* item : *items) {
        T x;
        if (!fromValue(*item, x)) return false;
        parsed.push_back(std::move(x));
    }
    out = std::move(parsed);
    return true;
}

template <typename T>
bool fromObjectJson(const uint8_t* d, size_t n, T& m) {
    json::Value v;
    return parseObject(d, n, v) && fromValue(v, m);
}

} // namespace

std::string toJson(const HelloServerMessage& m) {
    json::ObjectWriter w;
    w.field("protocolVersion", m.protocolVersion)
        .field("modeRaw", m.modeRaw)
        .field("sbsLayoutRaw", m.sbsLayoutRaw)
        .field("allowPhoneModeSwitch", m.allowPhoneModeSwitch)
        .field("width", m.width)
        .field("height", m.height)
        .field("codecRaw", m.codecRaw);
    if (m.petCamera) w.object("petCamera", writer(*m.petCamera)); // encodeIfPresent
    return w.finish();
}

std::string toJson(const StreamConfigMessage& m) {
    json::ObjectWriter w;
    return writeStreamConfig(w, m).finish();
}

std::string toJson(const PingMessage& m) {
    json::ObjectWriter w;
    w.field("clientSendNs", m.clientSendNs)
        .field("lastDecodedSeq", m.lastDecodedSeq)
        .field("lastHostToDisplayLatencyUs", m.lastHostToDisplayLatencyUs)
        .field("lastBarcodeLatencyUs", m.lastBarcodeLatencyUs);
    if (m.protocolVersion) w.field("protocolVersion", *m.protocolVersion); // encodeIfPresent
    return w.finish();
}

std::string toJson(const PongMessage& m) {
    return json::ObjectWriter()
        .field("clientSendNs", m.clientSendNs)
        .field("serverRecvNs", m.serverRecvNs)
        .field("serverSendNs", m.serverSendNs)
        .finish();
}

std::string toJson(const SelectModeMessage& m) {
    return json::ObjectWriter().field("modeRaw", m.modeRaw).field("sbsLayoutRaw", m.sbsLayoutRaw).finish();
}

// Synthesized Codable encodes optionals with encodeIfPresent: a nil field is left out.
std::string toJson(const HostStateSyncMessage& m) {
    json::ObjectWriter w;
    w.field("protocolVersion", m.protocolVersion).field("stateSeq", m.stateSeq).field("powerState", toWire(m.powerState));
    if (m.inUseHolder) w.field("inUseHolder", *m.inUseHolder);
    if (m.errorMessage) w.field("errorMessage", *m.errorMessage);
    w.field("modeRaw", m.modeRaw)
        .field("sbsLayoutRaw", m.sbsLayoutRaw)
        .field("allowPhoneModeSwitch", m.allowPhoneModeSwitch)
        .field("allowPhoneSensorPower", m.allowPhoneSensorPower);
    if (m.sensorPowerDisabledReason) w.field("sensorPowerDisabledReason", *m.sensorPowerDisabledReason);
    w.field("cutoffEnabled", m.cutoffEnabled)
        .floatField("cutoffNearMm", m.cutoffNearMm)
        .floatField("cutoffFarMm", m.cutoffFarMm)
        .field("isHostFrozen", m.isHostFrozen)
        .field("isRecording", m.isRecording)
        .field("canRecordCurrentMode", m.canRecordCurrentMode);
    if (m.recordingDisabledReason) w.field("recordingDisabledReason", *m.recordingDisabledReason);
    w.field("recordingReminderNeeded", m.recordingReminderNeeded)
        .field("recordingFrames", m.recordingFrames)
        .doubleField("recordingElapsedSec", m.recordingElapsedSec)
        .field("isReplayingOnHost", m.isReplayingOnHost);
    return w.finish();
}

std::string toJson(const RemoteControlCommandMessage& m) {
    json::ObjectWriter w;
    w.field("action", toWire(m.action));
    if (m.modeRaw) w.field("modeRaw", *m.modeRaw);
    if (m.sbsLayoutRaw) w.field("sbsLayoutRaw", *m.sbsLayoutRaw);
    if (m.cutoffEnabled) w.field("cutoffEnabled", *m.cutoffEnabled);
    if (m.applyTrackMePreset) w.field("applyTrackMePreset", *m.applyTrackMePreset);
    if (m.hostFrozen) w.field("hostFrozen", *m.hostFrozen);
    if (m.reminderConfirmedOnPhone) w.field("reminderConfirmedOnPhone", *m.reminderConfirmedOnPhone);
    if (m.snoozeReminderOneHour) w.field("snoozeReminderOneHour", *m.snoozeReminderOneHour);
    return w.finish();
}

std::string toJson(const Orbit3DGestureMessage& m) {
    return json::ObjectWriter()
        .floatField("deltaYawRadians", m.deltaYawRadians)
        .floatField("deltaPitchRadians", m.deltaPitchRadians)
        .floatField("zoomScale", m.zoomScale)
        .floatField("panRightMeters", m.panRightMeters)
        .floatField("panUpMeters", m.panUpMeters)
        .field("resetView", m.resetView)
        .finish();
}

std::string toJson(const SensorPowerControlMessage& m) {
    return json::ObjectWriter().field("requestId", m.requestId).field("turnOn", m.turnOn).finish();
}

std::string toJson(const PairRequestMessage& m) {
    json::ObjectWriter w;
    w.field("methodRaw", m.methodRaw)
        .bytes("clientCertDER", m.clientCertDER)
        .bytes("clientProofHMAC", m.clientProofHMAC);
    if (m.protocolVersion) w.field("protocolVersion", *m.protocolVersion);
    if (m.deviceName) w.field("deviceName", *m.deviceName);
    if (m.deviceKind) w.field("deviceKind", *m.deviceKind);
    return w.finish();
}

std::string toJson(const PairResponseMessage& m) {
    json::ObjectWriter w;
    w.field("accepted", m.accepted)
        .field("serviceId", m.serviceId)
        .field("streamPort", m.streamPort)
        .bytes("serverProofHMAC", m.serverProofHMAC)
        .field("message", m.message);
    if (m.role) w.field("role", *m.role);
    if (m.deviceId) w.field("deviceId", *m.deviceId);
    if (m.deviceName) w.field("deviceName", *m.deviceName);
    if (m.protocolVersion) w.field("protocolVersion", *m.protocolVersion);
    return w.finish();
}

std::string toJson(const PetCameraHello& m) { return writer(m).finish(); }
std::string toJson(const PetEndpoint& m) { return writer(m).finish(); }
std::string toJson(const PetWatcher& m) { return writer(m).finish(); }
std::string toJson(const PetDevice& m) { return writer(m).finish(); }

// Keys in the order of the v3 §5.1 table; nil optionals left out (encodeIfPresent).
std::string toJson(const PetCameraStateMessage& m) {
    json::ObjectWriter w;
    w.field("protocolVersion", m.protocolVersion)
        .field("stateSeq", m.stateSeq)
        .field("deviceId", m.deviceId)
        .field("deviceName", m.deviceName)
        .field("role", toWire(m.role))
        .field("path", toWire(m.path))
        .field("sharingEnabled", m.sharingEnabled)
        .field("sharingScheduleOff", m.sharingScheduleOff)
        .field("outsideEnabled", m.outsideEnabled)
        .field("privacyMode", m.privacyMode)
        .field("liveAllowed", m.liveAllowed);
    if (m.liveUnavailableReason) w.field("liveUnavailableReason", *m.liveUnavailableReason);
    w.field("watching", m.watching)
        .field("liveQuality", m.liveQuality)
        .field("liveTier", m.liveTier)
        .field("rollAvailable", m.rollAvailable)
        .field("playbackAvailable", m.playbackAvailable)
        .field("rollEnabled", m.rollEnabled)
        .field("motionEnabled", m.motionEnabled)
        .field("motionAvailable", m.motionAvailable)
        .field("motionSensitivity", m.motionSensitivity)
        .field("scheduleEnabled", m.scheduleEnabled);
    if (m.scheduleSummary) w.field("scheduleSummary", *m.scheduleSummary);
    w.field("clipRecording", m.clipRecording);
    if (m.clipTrigger) w.field("clipTrigger", *m.clipTrigger);
    w.field("clipManualActive", m.clipManualActive).doubleField("clipElapsedSec", m.clipElapsedSec);
    if (m.rollBlockedReason) w.field("rollBlockedReason", *m.rollBlockedReason);
    if (m.rollBlockedText) w.field("rollBlockedText", *m.rollBlockedText);
    if (m.outsideEndpoint) w.object("outsideEndpoint", writer(*m.outsideEndpoint));
    if (m.outsideCandidates) {
        json::ArrayWriter a;
        for (const auto& x : *m.outsideCandidates) a.object(writer(x));
        w.array("outsideCandidates", a);
    }
    if (m.outsideStatus) w.field("outsideStatus", *m.outsideStatus);
    if (m.outsideStatusText) w.field("outsideStatusText", *m.outsideStatusText);
    if (m.watchingNow) {
        json::ArrayWriter a;
        for (const auto& x : *m.watchingNow) a.object(writer(x));
        w.array("watchingNow", a);
    }
    if (m.devices) {
        json::ArrayWriter a;
        for (const auto& x : *m.devices) a.object(writer(x));
        w.array("devices", a);
    }
    return w.finish();
}

std::string toJson(const PetCameraCommandMessage& m) {
    json::ObjectWriter w;
    w.field("requestId", m.requestId).field("action", m.action);
    if (m.enabled) w.field("enabled", *m.enabled);
    if (m.sensitivity) w.field("sensitivity", *m.sensitivity);
    if (m.clipId) w.field("clipId", *m.clipId);
    if (m.starred) w.field("starred", *m.starred);
    if (m.deviceId) w.field("deviceId", *m.deviceId);
    return w.finish();
}

std::string toJson(const CommandResultMessage& m) {
    json::ObjectWriter w;
    w.field("requestId", m.requestId).field("ok", m.ok);
    if (m.error) w.field("error", *m.error);
    if (m.message) w.field("message", *m.message);
    return w.finish();
}

std::string toJson(const LiveViewRequestMessage& m) {
    json::ObjectWriter w;
    w.field("watch", m.watch);
    if (m.quality) w.field("quality", *m.quality);
    return w.finish();
}

std::string toJson(const SessionNoticeMessage& m) {
    json::ObjectWriter w;
    w.field("reason", m.reason).field("closing", m.closing);
    if (m.message) w.field("message", *m.message);
    return w.finish();
}

// ---- camera roll (v3 §5.6-5.9): keys in the order of the spec's tables; nil optionals left out -----

std::string toJson(const RollListRequestMessage& m) {
    json::ObjectWriter w;
    w.field("requestId", m.requestId);
    if (m.day) w.field("day", *m.day);
    if (m.cursor) w.field("cursor", *m.cursor);
    if (m.limit) w.field("limit", *m.limit);
    return w.finish();
}

std::string toJson(const RollDay& m) { return writer(m).finish(); }
std::string toJson(const RollClip& m) { return writer(m).finish(); }
std::string toJson(const RollStorage& m) { return writer(m).finish(); }

std::string toJson(const RollListMessage& m) {
    json::ObjectWriter w;
    w.field("requestId", m.requestId);
    if (m.day) w.field("day", *m.day);
    if (m.days) {
        json::ArrayWriter a;
        for (const auto& x : *m.days) a.object(writer(x));
        w.array("days", a);
    }
    if (m.clips) {
        json::ArrayWriter a;
        for (const auto& x : *m.clips) a.object(writer(x));
        w.array("clips", a);
    }
    if (m.nextCursor) w.field("nextCursor", *m.nextCursor);
    w.object("storage", writer(m.storage));
    return w.finish();
}

std::string toJson(const RollThumbRequestMessage& m) {
    return json::ObjectWriter().field("requestId", m.requestId).field("clipId", m.clipId).finish();
}

std::string toJson(const RollThumbMessage& m) {
    return json::ObjectWriter().field("requestId", m.requestId).field("clipId", m.clipId).bytes("jpeg", m.jpeg).finish();
}

std::string toJson(const PlaybackRequestMessage& m) {
    json::ObjectWriter w;
    w.field("requestId", m.requestId).field("action", m.action);
    if (m.clipId) w.field("clipId", *m.clipId);
    if (m.streamId) w.field("streamId", *m.streamId);
    if (m.positionSec) w.doubleField("positionSec", *m.positionSec);
    return w.finish();
}

std::string toJson(const PlaybackStateMessage& m) {
    json::ObjectWriter w;
    if (m.requestId) w.field("requestId", *m.requestId);
    w.field("streamId", m.streamId)
        .field("clipId", m.clipId)
        .field("state", m.state)
        .doubleField("positionSec", m.positionSec)
        .doubleField("durationSec", m.durationSec);
    if (m.message) w.field("message", *m.message);
    return w.finish();
}

std::string toJson(const PlaybackConfigMessage& m) {
    json::ObjectWriter w;
    writeStreamConfig(w, m.config)
        .field("streamId", m.streamId)
        .field("clipId", m.clipId)
        .doubleField("durationSec", m.durationSec);
    return w.finish();
}

// petCamera is decodeIfPresent: absent or null leaves it unset; present but malformed fails the
// hello, as Swift's synthesized decoder does.
bool fromJson(const uint8_t* d, size_t n, HelloServerMessage& m) {
    json::Value v;
    if (!parseObject(d, n, v) || !json::getInt(v, "protocolVersion", m.protocolVersion) ||
        !json::getInt(v, "modeRaw", m.modeRaw) || !json::getInt(v, "sbsLayoutRaw", m.sbsLayoutRaw) ||
        !json::getBool(v, "allowPhoneModeSwitch", m.allowPhoneModeSwitch) || !json::getInt(v, "width", m.width) ||
        !json::getInt(v, "height", m.height) || !json::getU8(v, "codecRaw", m.codecRaw)) {
        return false;
    }
    m.petCamera.reset();
    const json::Value* pet = nullptr;
    if (!json::getOptObject(v, "petCamera", pet)) return false;
    if (pet) {
        PetCameraHello h;
        if (!fromValue(*pet, h)) return false;
        m.petCamera = std::move(h);
    }
    return true;
}

bool fromJson(const uint8_t* d, size_t n, StreamConfigMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && fromValue(v, m);
}

bool fromJson(const uint8_t* d, size_t n, PingMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "clientSendNs", m.clientSendNs) &&
           json::getU64(v, "lastDecodedSeq", m.lastDecodedSeq) &&
           json::getU32(v, "lastHostToDisplayLatencyUs", m.lastHostToDisplayLatencyUs) &&
           json::getU32(v, "lastBarcodeLatencyUs", m.lastBarcodeLatencyUs) &&
           json::getOptInt(v, "protocolVersion", m.protocolVersion);
}

// HostStateSyncMessage.init(from:): decodeIfPresent for the optionals, and allowPhoneSensorPower
// defaults to true when an older v2 host left it out.
bool fromJson(const uint8_t* d, size_t n, HostStateSyncMessage& m) {
    json::Value v;
    std::string power;
    if (!parseObject(d, n, v) || !json::getInt(v, "protocolVersion", m.protocolVersion) ||
        !json::getU64(v, "stateSeq", m.stateSeq) || !json::getString(v, "powerState", power) ||
        !fromWire(power, m.powerState) || !json::getOptString(v, "inUseHolder", m.inUseHolder) ||
        !json::getOptString(v, "errorMessage", m.errorMessage) || !json::getInt(v, "modeRaw", m.modeRaw) ||
        !json::getInt(v, "sbsLayoutRaw", m.sbsLayoutRaw) ||
        !json::getBool(v, "allowPhoneModeSwitch", m.allowPhoneModeSwitch)) {
        return false;
    }
    std::optional<bool> allow_power;
    if (!json::getOptBool(v, "allowPhoneSensorPower", allow_power)) return false;
    m.allowPhoneSensorPower = allow_power.value_or(true);
    return json::getOptString(v, "sensorPowerDisabledReason", m.sensorPowerDisabledReason) &&
           json::getBool(v, "cutoffEnabled", m.cutoffEnabled) && json::getFloat(v, "cutoffNearMm", m.cutoffNearMm) &&
           json::getFloat(v, "cutoffFarMm", m.cutoffFarMm) && json::getBool(v, "isHostFrozen", m.isHostFrozen) &&
           json::getBool(v, "isRecording", m.isRecording) &&
           json::getBool(v, "canRecordCurrentMode", m.canRecordCurrentMode) &&
           json::getOptString(v, "recordingDisabledReason", m.recordingDisabledReason) &&
           json::getBool(v, "recordingReminderNeeded", m.recordingReminderNeeded) &&
           json::getU64(v, "recordingFrames", m.recordingFrames) &&
           json::getDouble(v, "recordingElapsedSec", m.recordingElapsedSec) &&
           json::getBool(v, "isReplayingOnHost", m.isReplayingOnHost);
}

bool fromJson(const uint8_t* d, size_t n, RemoteControlCommandMessage& m) {
    json::Value v;
    std::string action;
    return parseObject(d, n, v) && json::getString(v, "action", action) && fromWire(action, m.action) &&
           json::getOptInt(v, "modeRaw", m.modeRaw) && json::getOptInt(v, "sbsLayoutRaw", m.sbsLayoutRaw) &&
           json::getOptBool(v, "cutoffEnabled", m.cutoffEnabled) &&
           json::getOptBool(v, "applyTrackMePreset", m.applyTrackMePreset) &&
           json::getOptBool(v, "hostFrozen", m.hostFrozen) &&
           json::getOptBool(v, "reminderConfirmedOnPhone", m.reminderConfirmedOnPhone) &&
           json::getOptBool(v, "snoozeReminderOneHour", m.snoozeReminderOneHour);
}

bool fromJson(const uint8_t* d, size_t n, Orbit3DGestureMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getFloat(v, "deltaYawRadians", m.deltaYawRadians) &&
           json::getFloat(v, "deltaPitchRadians", m.deltaPitchRadians) && json::getFloat(v, "zoomScale", m.zoomScale) &&
           json::getFloat(v, "panRightMeters", m.panRightMeters) && json::getFloat(v, "panUpMeters", m.panUpMeters) &&
           json::getBool(v, "resetView", m.resetView);
}

bool fromJson(const uint8_t* d, size_t n, SensorPowerControlMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getBool(v, "turnOn", m.turnOn);
}

bool fromJson(const uint8_t* d, size_t n, PongMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "clientSendNs", m.clientSendNs) &&
           json::getU64(v, "serverRecvNs", m.serverRecvNs) && json::getU64(v, "serverSendNs", m.serverSendNs);
}

bool fromJson(const uint8_t* d, size_t n, SelectModeMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getInt(v, "modeRaw", m.modeRaw) && json::getInt(v, "sbsLayoutRaw", m.sbsLayoutRaw);
}

bool fromJson(const uint8_t* d, size_t n, PairRequestMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU8(v, "methodRaw", m.methodRaw) &&
           json::getBytes(v, "clientCertDER", m.clientCertDER) && json::getBytes(v, "clientProofHMAC", m.clientProofHMAC) &&
           json::getOptInt(v, "protocolVersion", m.protocolVersion) && json::getOptString(v, "deviceName", m.deviceName) &&
           json::getOptString(v, "deviceKind", m.deviceKind);
}

bool fromJson(const uint8_t* d, size_t n, PairResponseMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getBool(v, "accepted", m.accepted) && json::getString(v, "serviceId", m.serviceId) &&
           json::getU16(v, "streamPort", m.streamPort) && json::getBytes(v, "serverProofHMAC", m.serverProofHMAC) &&
           json::getString(v, "message", m.message) && json::getOptString(v, "role", m.role) &&
           json::getOptString(v, "deviceId", m.deviceId) && json::getOptString(v, "deviceName", m.deviceName) &&
           json::getOptInt(v, "protocolVersion", m.protocolVersion);
}

bool fromJson(const uint8_t* d, size_t n, PetCameraHello& m) { return fromObjectJson(d, n, m); }
bool fromJson(const uint8_t* d, size_t n, PetEndpoint& m) { return fromObjectJson(d, n, m); }
bool fromJson(const uint8_t* d, size_t n, PetWatcher& m) { return fromObjectJson(d, n, m); }
bool fromJson(const uint8_t* d, size_t n, PetDevice& m) { return fromObjectJson(d, n, m); }

bool fromJson(const uint8_t* d, size_t n, PetCameraStateMessage& m) {
    json::Value v;
    if (!parseObject(d, n, v)) return false;
    const json::Value* endpoint = nullptr;
    const bool ok =
        json::getInt(v, "protocolVersion", m.protocolVersion) && json::getU64(v, "stateSeq", m.stateSeq) &&
        json::getString(v, "deviceId", m.deviceId) && json::getString(v, "deviceName", m.deviceName) &&
        getEnum(v, "role", m.role, petRoleFromWire) && getEnum(v, "path", m.path, petPathFromWire) &&
        json::getBool(v, "sharingEnabled", m.sharingEnabled) &&
        json::getBool(v, "sharingScheduleOff", m.sharingScheduleOff) &&
        json::getBool(v, "outsideEnabled", m.outsideEnabled) && json::getBool(v, "privacyMode", m.privacyMode) &&
        json::getBool(v, "liveAllowed", m.liveAllowed) &&
        json::getOptString(v, "liveUnavailableReason", m.liveUnavailableReason) &&
        json::getBool(v, "watching", m.watching) && json::getString(v, "liveQuality", m.liveQuality) &&
        json::getString(v, "liveTier", m.liveTier) && json::getBool(v, "rollAvailable", m.rollAvailable) &&
        json::getBool(v, "playbackAvailable", m.playbackAvailable) && json::getBool(v, "rollEnabled", m.rollEnabled) &&
        json::getBool(v, "motionEnabled", m.motionEnabled) && json::getBool(v, "motionAvailable", m.motionAvailable) &&
        json::getInt(v, "motionSensitivity", m.motionSensitivity) &&
        json::getBool(v, "scheduleEnabled", m.scheduleEnabled) &&
        json::getOptString(v, "scheduleSummary", m.scheduleSummary) &&
        json::getBool(v, "clipRecording", m.clipRecording) && json::getOptString(v, "clipTrigger", m.clipTrigger) &&
        json::getBool(v, "clipManualActive", m.clipManualActive) &&
        json::getDouble(v, "clipElapsedSec", m.clipElapsedSec) &&
        json::getOptString(v, "rollBlockedReason", m.rollBlockedReason) &&
        json::getOptString(v, "rollBlockedText", m.rollBlockedText) &&
        json::getOptObject(v, "outsideEndpoint", endpoint) && getOptArrayOf(v, "outsideCandidates", m.outsideCandidates) &&
        json::getOptString(v, "outsideStatus", m.outsideStatus) &&
        json::getOptString(v, "outsideStatusText", m.outsideStatusText) &&
        getOptArrayOf(v, "watchingNow", m.watchingNow) && getOptArrayOf(v, "devices", m.devices);
    if (!ok) return false;
    m.outsideEndpoint.reset();
    if (endpoint) {
        PetEndpoint e;
        if (!fromValue(*endpoint, e)) return false;
        m.outsideEndpoint = std::move(e);
    }
    return true;
}

bool fromJson(const uint8_t* d, size_t n, PetCameraCommandMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getString(v, "action", m.action) &&
           json::getOptBool(v, "enabled", m.enabled) && json::getOptInt(v, "sensitivity", m.sensitivity) &&
           json::getOptString(v, "clipId", m.clipId) && json::getOptBool(v, "starred", m.starred) &&
           json::getOptString(v, "deviceId", m.deviceId);
}

bool fromJson(const uint8_t* d, size_t n, CommandResultMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getBool(v, "ok", m.ok) &&
           json::getOptString(v, "error", m.error) && json::getOptString(v, "message", m.message);
}

bool fromJson(const uint8_t* d, size_t n, LiveViewRequestMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getBool(v, "watch", m.watch) && json::getOptString(v, "quality", m.quality);
}

bool fromJson(const uint8_t* d, size_t n, SessionNoticeMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getString(v, "reason", m.reason) && json::getBool(v, "closing", m.closing) &&
           json::getOptString(v, "message", m.message);
}

// ---- camera roll decoders. Shape only (§5.10); roll_wire.hpp checks the values a device sends. -----

bool fromJson(const uint8_t* d, size_t n, RollListRequestMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getOptString(v, "day", m.day) &&
           json::getOptString(v, "cursor", m.cursor) && json::getOptInt(v, "limit", m.limit);
}

bool fromJson(const uint8_t* d, size_t n, RollDay& m) { return fromObjectJson(d, n, m); }
bool fromJson(const uint8_t* d, size_t n, RollClip& m) { return fromObjectJson(d, n, m); }
bool fromJson(const uint8_t* d, size_t n, RollStorage& m) { return fromObjectJson(d, n, m); }

bool fromJson(const uint8_t* d, size_t n, RollListMessage& m) {
    json::Value v;
    const json::Value* storage = nullptr;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getOptString(v, "day", m.day) &&
           getOptArrayOf(v, "days", m.days) && getOptArrayOf(v, "clips", m.clips) &&
           json::getOptString(v, "nextCursor", m.nextCursor) && json::getObject(v, "storage", storage) &&
           fromValue(*storage, m.storage);
}

bool fromJson(const uint8_t* d, size_t n, RollThumbRequestMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getString(v, "clipId", m.clipId);
}

bool fromJson(const uint8_t* d, size_t n, RollThumbMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getString(v, "clipId", m.clipId) &&
           json::getBytes(v, "jpeg", m.jpeg) && m.jpeg.size() <= RollThumbMessage::kMaxJpegBytes;
}

bool fromJson(const uint8_t* d, size_t n, PlaybackRequestMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getU64(v, "requestId", m.requestId) && json::getString(v, "action", m.action) &&
           json::getOptString(v, "clipId", m.clipId) && json::getOptU32(v, "streamId", m.streamId) &&
           json::getOptDouble(v, "positionSec", m.positionSec);
}

// state stays a string, so a newer host's state decodes (playbackStateFromWire maps it to Unknown).
bool fromJson(const uint8_t* d, size_t n, PlaybackStateMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && json::getOptU64(v, "requestId", m.requestId) &&
           json::getU32(v, "streamId", m.streamId) && json::getString(v, "clipId", m.clipId) &&
           json::getString(v, "state", m.state) && json::getDouble(v, "positionSec", m.positionSec) &&
           json::getDouble(v, "durationSec", m.durationSec) && json::getOptString(v, "message", m.message);
}

bool fromJson(const uint8_t* d, size_t n, PlaybackConfigMessage& m) {
    json::Value v;
    return parseObject(d, n, v) && fromValue(v, m.config) && json::getU32(v, "streamId", m.streamId) &&
           json::getString(v, "clipId", m.clipId) && json::getDouble(v, "durationSec", m.durationSec);
}

} // namespace kvhost
