import Foundation

enum WireMessageType: UInt8, Sendable {
    case helloServer      = 0x01
    case streamConfig     = 0x02
    case videoAccessUnit  = 0x03
    case ping             = 0x04
    case pong             = 0x05
    case selectModeReq    = 0x06

    // Phase 5 additive protocol v2 messages (backward compatible with v1)
    case hostStateSync    = 0x07
    case remoteControlCmd = 0x08
    case orbit3DGesture   = 0x09
    case sensorPowerReq   = 0x0A

    // Phase 9 additive protocol v3 pet-camera messages
    case petCameraState     = 0x0B
    case petCameraCmd       = 0x0C
    case commandResult      = 0x0D
    case liveViewReq        = 0x0E
    case sessionNotice      = 0x0F

    // Ephemeral pairing listener (and v3 outside invite listener)
    case pairRequest        = 0x10
    case pairResponse       = 0x11

    // Phase 9B camera-roll & clip playback wire types (recognized & safely skipped in 9A)
    case rollListReq        = 0x12
    case rollList           = 0x13
    case rollThumbReq       = 0x14
    case rollThumb          = 0x15
    case playbackReq        = 0x16
    case playbackState      = 0x17
    case playbackConfig     = 0x18
    case playbackAccessUnit = 0x19
}

enum WireCodecKind: UInt8, Sendable {
    case h264 = 0
    case hevc = 1
}

enum WirePairingMethod: UInt8, Sendable {
    case qrToken = 0
    case shortCode = 1
    case invite = 2
}

struct WireHeader: Sendable {
    static let size = 8
    static let magic0: UInt8 = 0x4B // 'K'
    static let magic1: UInt8 = 0x56 // 'V'
    /// Maximum payload size for host -> device frames (4 MiB).
    static let maxPayloadBytes = 4 * 1024 * 1024
    /// Maximum wire frame size for device -> host frames (64 KiB, protocol v3 §1).
    static let maxClientFrameBytes = 64 * 1024

    let type: WireMessageType
    let flags: UInt8
    let payloadLength: UInt32
}

struct PetCameraHello: Codable, Sendable, Equatable {
    let specVersion: Int
    let maxWatchers: Int
    let cameraRoll: Bool
    let outside: Bool
    let qualities: [String]
}

struct HelloServerMessage: Codable, Sendable, Equatable {
    let protocolVersion: Int
    let modeRaw: Int
    let sbsLayoutRaw: Int
    let allowPhoneModeSwitch: Bool
    let width: Int
    let height: Int
    let codecRaw: UInt8
    let petCamera: PetCameraHello?

    init(
        protocolVersion: Int,
        modeRaw: Int,
        sbsLayoutRaw: Int,
        allowPhoneModeSwitch: Bool,
        width: Int,
        height: Int,
        codecRaw: UInt8,
        petCamera: PetCameraHello? = nil
    ) {
        self.protocolVersion = protocolVersion
        self.modeRaw = modeRaw
        self.sbsLayoutRaw = sbsLayoutRaw
        self.allowPhoneModeSwitch = allowPhoneModeSwitch
        self.width = width
        self.height = height
        self.codecRaw = codecRaw
        self.petCamera = petCamera
    }
}

struct StreamConfigMessage: Codable, Sendable {
    let streamEpoch: UInt32
    let codecRaw: UInt8
    let width: Int
    let height: Int
    let modeRaw: Int
    let sbsLayoutRaw: Int
    let nalLengthHeaderBytes: Int
    let parameterSets: [Data] // [SPS, PPS] for H.264 or [VPS, SPS, PPS] for HEVC
}

struct VideoAccessUnitHeader: Sendable {
    static let binarySize = 29
    let streamEpoch: UInt32
    let frameSeq: UInt64
    let hostCaptureNs: UInt64
    let hostEncodeDoneNs: UInt64
    let flags: UInt8 // bit 0 = keyframe, bit 1 = clock_overlay

    var isKeyframe: Bool { (flags & 0x01) != 0 }
    var hasClockOverlay: Bool { (flags & 0x02) != 0 }

    func encode(withAvccPayload payload: Data) -> Data {
        var out = Data(capacity: Self.binarySize + payload.count)
        out.appendUInt32BE(streamEpoch)
        out.appendUInt64BE(frameSeq)
        out.appendUInt64BE(hostCaptureNs)
        out.appendUInt64BE(hostEncodeDoneNs)
        out.append(flags)
        out.append(payload)
        return out
    }

    static func decode(from data: Data) -> (header: VideoAccessUnitHeader, avccPayload: Data)? {
        guard data.count > Self.binarySize else { return nil }
        let epoch = data.readUInt32BE(at: 0)
        let seq = data.readUInt64BE(at: 4)
        let capNs = data.readUInt64BE(at: 12)
        let encNs = data.readUInt64BE(at: 20)
        let flg = data[data.startIndex + 28]
        let payload = data.subdata(in: (data.startIndex + Self.binarySize)..<data.endIndex)
        return (
            VideoAccessUnitHeader(
                streamEpoch: epoch,
                frameSeq: seq,
                hostCaptureNs: capNs,
                hostEncodeDoneNs: encNs,
                flags: flg
            ),
            payload
        )
    }
}

struct PingMessage: Codable, Sendable {
    let clientSendNs: UInt64
    let lastDecodedSeq: UInt64
    let lastHostToDisplayLatencyUs: UInt32
    let lastBarcodeLatencyUs: UInt32
    var protocolVersion: Int? = nil
}

struct PongMessage: Codable, Sendable {
    let clientSendNs: UInt64
    let serverRecvNs: UInt64
    let serverSendNs: UInt64
}

struct SelectModeMessage: Codable, Sendable {
    let modeRaw: Int
    let sbsLayoutRaw: Int
}

// MARK: - Phase 5 Additive Messages (Protocol Version 2)

enum SharedRecordingReminderConstants {
    static let exactReminderText = "Only you in view. Not pointed at a window. Depth and IR only. Deleted after 7 days."
    static let snoozeDurationSeconds: TimeInterval = 3600.0
    static let viewerNotOpenText = "The viewer isn't open on the Mac"
    static let colourLiveOnlyText = "Colour is live only. Switch to IR or Depth to record."
    static let sensorPowerDisabledOnHostText = "Power control is turned off on the Mac"

    static func inUseByMessage(holder: String) -> String {
        let trimmed = holder.trimmingCharacters(in: .whitespacesAndNewlines)
        let effective = trimmed.isEmpty ? "another process" : trimmed
        return "The Kinect is in use by \(effective)"
    }
}

enum WireSensorPowerState: String, Codable, Sendable, Equatable {
    case off = "off"
    case starting = "starting"
    case on = "on"
    case inUseElsewhere = "in_use_elsewhere"
    case error = "error"

    var displayLabel: String {
        switch self {
        case .off: return "Off"
        case .starting: return "Starting"
        case .on: return "On"
        case .inUseElsewhere: return "In use elsewhere"
        case .error: return "Error"
        }
    }

    var isSwitchConfirmedOn: Bool {
        self == .on
    }
}

struct HostStateSyncMessage: Codable, Sendable, Equatable {
    let protocolVersion: Int
    let stateSeq: UInt64
    let powerState: WireSensorPowerState
    let inUseHolder: String?
    let errorMessage: String?
    let modeRaw: Int
    let sbsLayoutRaw: Int
    let allowPhoneModeSwitch: Bool
    let allowPhoneSensorPower: Bool
    let sensorPowerDisabledReason: String?
    let cutoffEnabled: Bool
    let cutoffNearMm: Float
    let cutoffFarMm: Float
    let isHostFrozen: Bool
    let isRecording: Bool
    let canRecordCurrentMode: Bool
    let recordingDisabledReason: String?
    let recordingReminderNeeded: Bool
    let recordingFrames: UInt64
    let recordingElapsedSec: Double
    let isReplayingOnHost: Bool

    enum CodingKeys: String, CodingKey {
        case protocolVersion
        case stateSeq
        case powerState
        case inUseHolder
        case errorMessage
        case modeRaw
        case sbsLayoutRaw
        case allowPhoneModeSwitch
        case allowPhoneSensorPower
        case sensorPowerDisabledReason
        case cutoffEnabled
        case cutoffNearMm
        case cutoffFarMm
        case isHostFrozen
        case isRecording
        case canRecordCurrentMode
        case recordingDisabledReason
        case recordingReminderNeeded
        case recordingFrames
        case recordingElapsedSec
        case isReplayingOnHost
    }

    init(
        protocolVersion: Int,
        stateSeq: UInt64,
        powerState: WireSensorPowerState,
        inUseHolder: String?,
        errorMessage: String?,
        modeRaw: Int,
        sbsLayoutRaw: Int,
        allowPhoneModeSwitch: Bool,
        allowPhoneSensorPower: Bool = true,
        sensorPowerDisabledReason: String? = nil,
        cutoffEnabled: Bool,
        cutoffNearMm: Float,
        cutoffFarMm: Float,
        isHostFrozen: Bool,
        isRecording: Bool,
        canRecordCurrentMode: Bool,
        recordingDisabledReason: String?,
        recordingReminderNeeded: Bool,
        recordingFrames: UInt64,
        recordingElapsedSec: Double,
        isReplayingOnHost: Bool
    ) {
        self.protocolVersion = protocolVersion
        self.stateSeq = stateSeq
        self.powerState = powerState
        self.inUseHolder = inUseHolder
        self.errorMessage = errorMessage
        self.modeRaw = modeRaw
        self.sbsLayoutRaw = sbsLayoutRaw
        self.allowPhoneModeSwitch = allowPhoneModeSwitch
        self.allowPhoneSensorPower = allowPhoneSensorPower
        self.sensorPowerDisabledReason = sensorPowerDisabledReason
        self.cutoffEnabled = cutoffEnabled
        self.cutoffNearMm = cutoffNearMm
        self.cutoffFarMm = cutoffFarMm
        self.isHostFrozen = isHostFrozen
        self.isRecording = isRecording
        self.canRecordCurrentMode = canRecordCurrentMode
        self.recordingDisabledReason = recordingDisabledReason
        self.recordingReminderNeeded = recordingReminderNeeded
        self.recordingFrames = recordingFrames
        self.recordingElapsedSec = recordingElapsedSec
        self.isReplayingOnHost = isReplayingOnHost
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        protocolVersion = try c.decode(Int.self, forKey: .protocolVersion)
        stateSeq = try c.decode(UInt64.self, forKey: .stateSeq)
        powerState = try c.decode(WireSensorPowerState.self, forKey: .powerState)
        inUseHolder = try c.decodeIfPresent(String.self, forKey: .inUseHolder)
        errorMessage = try c.decodeIfPresent(String.self, forKey: .errorMessage)
        modeRaw = try c.decode(Int.self, forKey: .modeRaw)
        sbsLayoutRaw = try c.decode(Int.self, forKey: .sbsLayoutRaw)
        allowPhoneModeSwitch = try c.decode(Bool.self, forKey: .allowPhoneModeSwitch)
        allowPhoneSensorPower = try c.decodeIfPresent(Bool.self, forKey: .allowPhoneSensorPower) ?? true
        sensorPowerDisabledReason = try c.decodeIfPresent(String.self, forKey: .sensorPowerDisabledReason)
        cutoffEnabled = try c.decode(Bool.self, forKey: .cutoffEnabled)
        cutoffNearMm = try c.decode(Float.self, forKey: .cutoffNearMm)
        cutoffFarMm = try c.decode(Float.self, forKey: .cutoffFarMm)
        isHostFrozen = try c.decode(Bool.self, forKey: .isHostFrozen)
        isRecording = try c.decode(Bool.self, forKey: .isRecording)
        canRecordCurrentMode = try c.decode(Bool.self, forKey: .canRecordCurrentMode)
        recordingDisabledReason = try c.decodeIfPresent(String.self, forKey: .recordingDisabledReason)
        recordingReminderNeeded = try c.decode(Bool.self, forKey: .recordingReminderNeeded)
        recordingFrames = try c.decode(UInt64.self, forKey: .recordingFrames)
        recordingElapsedSec = try c.decode(Double.self, forKey: .recordingElapsedSec)
        isReplayingOnHost = try c.decode(Bool.self, forKey: .isReplayingOnHost)
    }

    static let unconfirmedInitial = HostStateSyncMessage(
        protocolVersion: 2,
        stateSeq: 0,
        powerState: .off,
        inUseHolder: nil,
        errorMessage: nil,
        modeRaw: 0,
        sbsLayoutRaw: 0,
        allowPhoneModeSwitch: false,
        allowPhoneSensorPower: false,
        sensorPowerDisabledReason: nil,
        cutoffEnabled: false,
        cutoffNearMm: 500.0,
        cutoffFarMm: 2000.0,
        isHostFrozen: false,
        isRecording: false,
        canRecordCurrentMode: true,
        recordingDisabledReason: nil,
        recordingReminderNeeded: true,
        recordingFrames: 0,
        recordingElapsedSec: 0.0,
        isReplayingOnHost: false
    )
}

enum RemoteControlAction: String, Codable, Sendable, Equatable {
    case selectMode = "select_mode"
    case setCutoff = "set_cutoff"
    case setHostFrozen = "set_host_frozen"
    case startRecording = "start_recording"
    case stopRecording = "stop_recording"
    case requestKeyframe = "request_keyframe"
}

struct RemoteControlCommandMessage: Codable, Sendable, Equatable {
    let action: RemoteControlAction
    var modeRaw: Int? = nil
    var sbsLayoutRaw: Int? = nil
    var cutoffEnabled: Bool? = nil
    var applyTrackMePreset: Bool? = nil
    var hostFrozen: Bool? = nil
    var reminderConfirmedOnPhone: Bool? = nil
    var snoozeReminderOneHour: Bool? = nil
}

struct Orbit3DGestureMessage: Codable, Sendable, Equatable {
    let deltaYawRadians: Float
    let deltaPitchRadians: Float
    let zoomScale: Float
    let panRightMeters: Float
    let panUpMeters: Float
    let resetView: Bool
}

struct SensorPowerControlMessage: Codable, Sendable, Equatable {
    let requestId: UInt64
    let turnOn: Bool
}

enum PocketConnectionQuality: String, Sendable, Equatable {
    case offline = "Offline"
    case standby = "Standby (Sensor Off)"
    case excellent = "Excellent"
    case good = "Good"
    case fair = "Fair"
    case poor = "Poor"

    static func evaluate(
        isConnected: Bool,
        powerState: WireSensorPowerState,
        modeRaw: Int,
        fps: Double,
        delayMs: Double,
        rttMs: Double,
        recentSeqDrops: UInt64
    ) -> PocketConnectionQuality {
        guard isConnected else { return .offline }
        guard powerState == .on else { return .standby }

        let effectiveDelay = delayMs > 0 ? delayMs : (rttMs * 0.5)
        let isColorLowLightExpected = (modeRaw == 1 || modeRaw == 3)
        let goodFpsFloor = isColorLowLightExpected ? 11.5 : 22.0
        let fairFpsFloor = isColorLowLightExpected ? 8.0 : 14.0

        if fps >= goodFpsFloor && effectiveDelay < 90.0 && recentSeqDrops == 0 {
            return .excellent
        }
        if fps >= fairFpsFloor && effectiveDelay < 160.0 && recentSeqDrops <= 2 {
            return .good
        }
        if fps >= 5.0 && effectiveDelay < 320.0 {
            return .fair
        }
        return .poor
    }
}

// MARK: - Protocol v3 Pet Camera Enums & Messages (`docs/protocol-v3-pet-camera.md`)

enum PetRole: String, Sendable, Equatable {
    case owner = "owner"
    case viewer = "viewer"
    case unknown = "unknown"

    var displayLabel: String {
        switch self {
        case .owner: return "Owner"
        case .viewer: return "Viewer"
        case .unknown: return "Unknown"
        }
    }
}

extension PetRole: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = PetRole(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

enum PetPath: String, Sendable, Equatable {
    case home = "home"
    case outside = "outside"
    case unknown = "unknown"

    var displayLabel: String {
        switch self {
        case .home: return "Home"
        case .outside: return "Outside"
        case .unknown: return "Unknown"
        }
    }
}

extension PetPath: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = PetPath(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

enum LiveTier: String, Sendable, Equatable {
    case high = "high"
    case medium = "medium"
    case low = "low"
    case unknown = "unknown"

    var displayLabel: String {
        switch self {
        case .high: return "High"
        case .medium: return "Medium"
        case .low: return "Low"
        case .unknown: return "Auto"
        }
    }
}

extension LiveTier: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = LiveTier(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

enum LiveQuality: String, Sendable, Equatable, Identifiable {
    case auto = "auto"
    case high = "high"
    case medium = "medium"
    case low = "low"
    case unknown = "unknown"

    static let selectableCases: [LiveQuality] = [.auto, .high, .medium, .low]

    var id: String { rawValue }

    var displayLabel: String {
        switch self {
        case .auto: return "Auto"
        case .high: return "High"
        case .medium: return "Medium"
        case .low: return "Low"
        case .unknown: return "Auto"
        }
    }
}

extension LiveQuality: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = LiveQuality(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

enum ClipTrigger: String, Sendable, Equatable {
    case manual = "manual"
    case motion = "motion"
    case schedule = "schedule"
    case unknown = "unknown"

    var displayLabel: String {
        switch self {
        case .manual: return "Manual"
        case .motion: return "Motion"
        case .schedule: return "Schedule"
        case .unknown: return "Clip"
        }
    }
}

extension ClipTrigger: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = ClipTrigger(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

/// Full priority-ordered `0x0B rollBlockedReason` codes (`docs/protocol-v3-pet-camera.md` §5.1 & §9).
enum RollBlockedReason: Sendable, Equatable {
    case privacyMode
    case rollDisabled
    case rollPreparing
    case folderUnavailable
    case sensorOff
    case viewFrozen
    case pcReplaying
    case diskFloor
    case storageFull
    case unknown(String)

    init(rawCode: String) {
        switch rawCode {
        case "privacy_mode": self = .privacyMode
        case "roll_disabled": self = .rollDisabled
        case "roll_preparing": self = .rollPreparing
        case "folder_unavailable": self = .folderUnavailable
        case "sensor_off": self = .sensorOff
        case "view_frozen": self = .viewFrozen
        case "pc_replaying": self = .pcReplaying
        case "disk_floor": self = .diskFloor
        case "storage_full": self = .storageFull
        default: self = .unknown(rawCode)
        }
    }

    var rawCode: String {
        switch self {
        case .privacyMode: return "privacy_mode"
        case .rollDisabled: return "roll_disabled"
        case .rollPreparing: return "roll_preparing"
        case .folderUnavailable: return "folder_unavailable"
        case .sensorOff: return "sensor_off"
        case .viewFrozen: return "view_frozen"
        case .pcReplaying: return "pc_replaying"
        case .diskFloor: return "disk_floor"
        case .storageFull: return "storage_full"
        case .unknown(let raw): return raw
        }
    }

    /// Returns the exact §9 sentence for known codes, or `fallbackText` (`rollBlockedText`) for unknown future codes.
    func displaySentence(fallbackText: String?) -> String {
        switch self {
        case .privacyMode:
            return PetCameraUserStrings.privacyModeSentence
        case .rollDisabled:
            return PetCameraUserStrings.rollDisabledSentence
        case .rollPreparing:
            return PetCameraUserStrings.rollPreparingSentence
        case .folderUnavailable:
            return PetCameraUserStrings.folderUnavailableSentence
        case .sensorOff:
            return PetCameraUserStrings.sensorOffSentence
        case .viewFrozen:
            return PetCameraUserStrings.viewFrozenSentence
        case .pcReplaying:
            return PetCameraUserStrings.pcReplayingSentence
        case .diskFloor:
            return PetCameraUserStrings.diskFloorSentence
        case .storageFull:
            return PetCameraUserStrings.storageFullSentence
        case .unknown:
            let trimmed = fallbackText?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            return trimmed.isEmpty ? "Camera roll recording isn't available right now." : trimmed
        }
    }
}

enum LiveUnavailableReason: String, Sendable, Equatable {
    case privacyMode = "privacy_mode"
    case pcReplaying = "pc_replaying"
    case sharingOff = "sharing_off"
    case sharingSchedule = "sharing_schedule"
    case tooManyViewers = "too_many_viewers"
    case unknown = "unknown"

    /// Exact plain-English sentences from `docs/protocol-v3-pet-camera.md` §9.
    var displaySentence: String {
        switch self {
        case .privacyMode:
            return "Privacy mode is on. Turn it off on the PC or an owner's device."
        case .pcReplaying:
            return "The PC is playing a recording."
        case .sharingOff:
            return "Sharing is off on the PC."
        case .sharingSchedule:
            return "Sharing is off at this time of day."
        case .tooManyViewers:
            return "Four devices are already watching."
        case .unknown:
            return "Live video isn't available right now."
        }
    }
}

extension LiveUnavailableReason: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = LiveUnavailableReason(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

enum PetSessionNoticeReason: String, Sendable, Equatable {
    case revoked = "revoked"
    case tooManySessions = "too_many_sessions"
    case replaced = "replaced"
    case outsideDisabled = "outside_disabled"
    case hostShuttingDown = "host_shutting_down"
    case addressChanged = "address_changed"
    case unknown = "unknown"

    var defaultSentence: String {
        switch self {
        case .revoked:
            return "This device was removed from the PC."
        case .tooManySessions:
            return "Too many devices are connected to this PC."
        case .replaced:
            return "This device connected again somewhere else."
        case .outsideDisabled:
            return "Viewing from outside the home was turned off on the PC."
        case .hostShuttingDown:
            return "The camera app on the PC was closed."
        case .addressChanged:
            return "The PC's home address changed."
        case .unknown:
            return "Disconnected from the camera."
        }
    }
}

extension PetSessionNoticeReason: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = PetSessionNoticeReason(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

enum PetCameraUserStrings {
    static let phoneRevokedText = "This phone was removed from the camera"
    static let macRevokedText = "This Mac was removed from the camera"
    static let phoneTLSRefusedText = "The camera refused this phone. It may be starting up — trying again shortly."
    static let macTLSRefusedText = "The camera refused this Mac. It may be starting up — trying again shortly."
    static let tooManySessionsUserText = "Too many people are watching right now"
    static let outsideDisableSharingWarning = "You'll be disconnected, and it can only be turned back on at home or on the PC."
    static let unreachableDefaultPC = "Can't reach the PC. It may be off or asleep, sharing may be off, or its home address may have changed."
    static let unreachableAddressChanged = "The PC's home address changed. Ask for the new link, or connect at home once."
    static let macKeychainAccessRequired = "Allow Keychain access to keep this Mac paired to the camera."
    static let iosKeychainAccessRequired = "Allow Keychain access to keep your iPhone paired to the camera."

    // Exact §9 sentences (`docs/protocol-v3-pet-camera.md` §9)
    static let privacyModeSentence = "Privacy mode is on. Turn it off on the PC or an owner's device."
    static let pcReplayingSentence = "The PC is playing a recording."
    static let sharingOffSentence = "Sharing is off on the PC."
    static let sharingScheduleSentence = "Sharing is off at this time of day."
    static let tooManyViewersSentence = "Four devices are already watching."
    static let rollDisabledSentence = "Recording to the camera roll is off."
    static let rollPreparingSentence = "The camera roll is getting ready."
    static let folderUnavailableSentence = "The camera roll folder on the PC can't be used."
    static let sensorOffSentence = "The camera is off."
    static let viewFrozenSentence = "The view on the PC is frozen."
    static let diskFloorSentence = "Recording stopped: less than 20 GB is free on the PC's disk."
    static let storageFullSentence = "Recording stopped: the camera roll is full of starred clips."
    static let playbackClipDeletedSentence = "This clip was deleted."
    static let playbackClipReadErrorSentence = "The PC couldn't read this clip."
    static let playbackIdleStoppedSentence = "Playback stopped after a minute without a command."
    static let pairingTooManyDevicesSentence = "This PC has 16 paired devices. Revoke one first."
    static let inviteExpiredOrUsedSentence = "This invite has expired or was already used. Ask for a new one."
    /// A wrong proof that leaves the invite open (the host burns it only on the tenth wrong attempt).
    /// Byte-identical to the Windows host's `text::kInviteDidntMatch`.
    static let inviteDidntMatchSentence = "That invite didn't match. Check that you used the whole link you were sent."
    /// The tenth wrong attempt on one invite: the host burns it.
    static let inviteWrongCodeSentence = "That invite didn't match. It can't be used again; ask for a new one."
    static let inviteClosedByPCSentence = "This invite was closed. Ask for a new one."
    static let inviteClosedWithoutReplySentence = "Couldn't use this invite; ask for a new one."
    static let endpointUnpairedCameraSentence = "This link is for a camera you aren't paired with"
    static let candidateLinkUnreachableSentence = "That link didn't reach your camera."
    /// Short-code pairing can't tell the real PC from an impostor on the same network: the code is its only secret.
    static let shortCodeTrustedNetworkWarning = "Only use a short code on your own home Wi-Fi. On a shared or public network, someone nearby could pretend to be your PC. Scanning the QR code is safer: it carries your PC's fingerprint."
    static let shortCodeTrustedNetworkConfirm = "This is my own home Wi-Fi"
    /// The PC closed the connection before any TLS (its connection limiter): not a refusal, so the app retries after a pause.
    static let pcBusyTryAgainShortlySentence = "The PC is busy right now. Trying again shortly."
    static let cellularDataOffSentence = "iPhone is blocking cellular data for this app. Settings → Cellular → turn on Kinect Viewer."
    static let wifiOffForAppSentence = "Wi-Fi is off for Kinect Viewer. Turn it on in Settings → Kinect Viewer."
    static let localNetworkOffSentence = "Kinect Viewer can't use your home network. Turn on Local Network in Settings → Privacy & Security → Local Network."

    static func revokedBannerText(forDeviceKind kind: String) -> String {
        if kind.lowercased() == "mac" {
            return macRevokedText
        }
        return phoneRevokedText
    }

    static func tlsRefusedBannerText(forDeviceKind kind: String) -> String {
        if kind.lowercased() == "mac" {
            return macTLSRefusedText
        }
        return phoneTLSRefusedText
    }

    static func keychainAccessRequiredText(forDeviceKind kind: String) -> String {
        if kind.lowercased() == "mac" {
            return macKeychainAccessRequired
        }
        return iosKeychainAccessRequired
    }
}

struct PetEndpoint: Codable, Sendable, Equatable {
    let host: String
    let port: UInt16
    let verified: Bool
}

struct PetWatcher: Codable, Sendable, Equatable, Identifiable {
    let deviceId: String
    let deviceName: String
    let role: PetRole
    let path: PetPath
    let activity: String

    var id: String { "\(deviceId)-\(path.rawValue)-\(activity)" }
}

struct PetDevice: Codable, Sendable, Equatable, Identifiable {
    let deviceId: String
    let name: String
    let role: PetRole
    let kind: String
    let pairedAtUnixMs: UInt64
    let lastSeenUnixMs: UInt64?
    let connected: Bool

    var id: String { deviceId }
}

struct PetCameraStateMessage: Codable, Sendable, Equatable {
    let protocolVersion: Int
    let stateSeq: UInt64
    let deviceId: String
    let deviceName: String
    let role: PetRole
    let path: PetPath
    let sharingEnabled: Bool
    let sharingScheduleOff: Bool
    let outsideEnabled: Bool
    let privacyMode: Bool
    let liveAllowed: Bool
    let liveUnavailableReason: LiveUnavailableReason?
    let watching: Bool
    let liveQuality: LiveQuality
    let liveTier: LiveTier
    let rollAvailable: Bool
    let playbackAvailable: Bool
    let rollEnabled: Bool
    let motionEnabled: Bool
    let motionAvailable: Bool
    let motionSensitivity: Int
    let scheduleEnabled: Bool
    let scheduleSummary: String?
    let clipRecording: Bool
    let clipTrigger: ClipTrigger?
    let clipManualActive: Bool
    let clipElapsedSec: Double
    let rollBlockedReason: String?
    let rollBlockedText: String?
    let outsideEndpoint: PetEndpoint?
    /// Extra outside addresses beside `outsideEndpoint` (e.g. the PC's Tailscale 100.x). Absent from older hosts.
    var outsideCandidates: [PetEndpoint]? = nil
    let outsideStatus: String?
    let outsideStatusText: String?
    let watchingNow: [PetWatcher]?
    let devices: [PetDevice]?

    var parsedRollBlockedReason: RollBlockedReason? {
        guard let raw = rollBlockedReason?.trimmingCharacters(in: .whitespacesAndNewlines),
              !raw.isEmpty else {
            return nil
        }
        return RollBlockedReason(rawCode: raw)
    }

    /// Resolves the §9 sentence for `rollBlockedReason`, falling back to `rollBlockedText` for unknown values.
    var resolvedRollBlockedSentence: String? {
        guard let parsed = parsedRollBlockedReason else { return nil }
        return parsed.displaySentence(fallbackText: rollBlockedText)
    }

    static let unconfirmedInitial = PetCameraStateMessage(
        protocolVersion: 3,
        stateSeq: 0,
        deviceId: "",
        deviceName: "",
        role: .unknown,
        path: .home,
        sharingEnabled: false,
        sharingScheduleOff: false,
        outsideEnabled: false,
        privacyMode: false,
        liveAllowed: true,
        liveUnavailableReason: nil,
        watching: false,
        liveQuality: .auto,
        liveTier: .high,
        rollAvailable: false,
        playbackAvailable: false,
        rollEnabled: false,
        motionEnabled: false,
        motionAvailable: false,
        motionSensitivity: 5,
        scheduleEnabled: false,
        scheduleSummary: nil,
        clipRecording: false,
        clipTrigger: nil,
        clipManualActive: false,
        clipElapsedSec: 0,
        rollBlockedReason: nil,
        rollBlockedText: nil,
        outsideEndpoint: nil,
        outsideStatus: nil,
        outsideStatusText: nil,
        watchingNow: nil,
        devices: nil
    )
}

enum PetCameraCommandAction: String, Codable, Sendable, Equatable {
    case setSharing = "set_sharing"
    case setPrivacy = "set_privacy"
    case setRoll = "set_roll"
    case setMotion = "set_motion"
    case startClip = "start_clip"
    case stopClip = "stop_clip"
    case starClip = "star_clip"
    case deleteClip = "delete_clip"
    case revokeDevice = "revoke_device"

    var requiresOwnerRole: Bool {
        switch self {
        case .setSharing, .setPrivacy, .setRoll, .setMotion, .deleteClip, .revokeDevice:
            return true
        case .startClip, .stopClip, .starClip:
            return false
        }
    }
}

struct PetCameraCommandMessage: Codable, Sendable, Equatable {
    let requestId: UInt64
    let action: String
    var enabled: Bool? = nil
    var sensitivity: Int? = nil
    var clipId: String? = nil
    var starred: Bool? = nil
    var deviceId: String? = nil
}

struct CommandResultMessage: Codable, Sendable, Equatable {
    let requestId: UInt64
    let ok: Bool
    let error: String?
    let message: String?
}

struct LiveViewRequestMessage: Codable, Sendable, Equatable {
    let watch: Bool
    var quality: String? = nil
}

struct SessionNoticeMessage: Codable, Sendable, Equatable {
    let reason: PetSessionNoticeReason
    let closing: Bool
    let message: String?
}

struct PairRequestMessage: Codable, Sendable {
    let methodRaw: UInt8
    let clientCertDER: Data
    let clientProofHMAC: Data
    var protocolVersion: Int? = nil
    var deviceName: String? = nil
    var deviceKind: String? = nil
}

struct PairResponseMessage: Codable, Sendable {
    let accepted: Bool
    let serviceId: String
    let streamPort: UInt16
    let serverProofHMAC: Data
    let message: String
    var role: PetRole? = nil
    var deviceId: String? = nil
    var deviceName: String? = nil
    var protocolVersion: Int? = nil
}

// MARK: - Protocol v3 Camera Roll & Streamed Playback (`0x12`–`0x19`)

struct CommandResultError: LocalizedError, Equatable, Sendable {
    let code: String?
    let message: String

    var errorDescription: String? { message }
}

struct RollListRequestMessage: Codable, Sendable, Equatable {
    static let minLimit = 1
    static let maxLimit = 100
    static let defaultLimit = 50

    let requestId: UInt64
    var day: String? = nil
    var cursor: String? = nil
    var limit: Int? = nil

    init(requestId: UInt64, day: String? = nil, cursor: String? = nil, limit: Int? = defaultLimit) {
        self.requestId = requestId
        self.day = day
        self.cursor = cursor
        if let rawLimit = limit {
            self.limit = min(Self.maxLimit, max(Self.minLimit, rawLimit))
        } else {
            self.limit = nil
        }
    }
}

struct RollDay: Codable, Sendable, Equatable, Identifiable {
    let day: String
    var clipCount: Int
    var starredCount: Int
    var bytes: UInt64

    var id: String { day }
}

struct RollClip: Codable, Sendable, Equatable, Identifiable {
    let clipId: String
    let startUnixMs: UInt64
    let startLocal: String
    let durationSec: Double
    let trigger: ClipTrigger
    let startedBy: String?
    var starred: Bool
    var starredByMe: Bool
    let width: Int
    let height: Int
    let bytes: UInt64
    let hasThumbnail: Bool

    var id: String { clipId }
}

struct RollStorage: Codable, Sendable, Equatable {
    let usedBytes: UInt64
    let capBytes: UInt64
    let freeBytes: UInt64
    let floorBytes: UInt64
    let keepDays: Int
}

struct RollListMessage: Codable, Sendable, Equatable {
    let requestId: UInt64
    let day: String?
    let days: [RollDay]?
    let clips: [RollClip]?
    let nextCursor: String?
    let storage: RollStorage
}

struct RollThumbRequestMessage: Codable, Sendable, Equatable {
    let requestId: UInt64
    let clipId: String
}

struct RollThumbMessage: Codable, Sendable, Equatable {
    static let maxThumbnailBytes = 64 * 1024

    let requestId: UInt64
    let clipId: String
    let jpeg: Data
}

enum PlaybackAction: String, Codable, Sendable, Equatable {
    case start = "start"
    case pause = "pause"
    case resume = "resume"
    case seek = "seek"
    case stop = "stop"
}

struct PlaybackRequestMessage: Codable, Sendable, Equatable {
    let requestId: UInt64
    let action: String
    var clipId: String? = nil
    var streamId: UInt32? = nil
    var positionSec: Double? = nil

    init(
        requestId: UInt64,
        action: PlaybackAction,
        clipId: String? = nil,
        streamId: UInt32? = nil,
        positionSec: Double? = nil
    ) {
        self.requestId = requestId
        self.action = action.rawValue
        self.clipId = clipId
        self.streamId = streamId
        self.positionSec = positionSec
    }
}

enum PlaybackStreamState: String, Sendable, Equatable {
    case starting = "starting"
    case playing = "playing"
    case paused = "paused"
    case ended = "ended"
    case stopped = "stopped"
    case error = "error"
    case unknown = "unknown"

    var displayLabel: String {
        switch self {
        case .starting: return "Starting…"
        case .playing: return "Playing"
        case .paused: return "Paused"
        case .ended: return "Ended"
        case .stopped: return "Stopped"
        case .error: return "Error"
        case .unknown: return "Playback"
        }
    }

    var freesStreamAllocation: Bool {
        self == .stopped || self == .error
    }
}

extension PlaybackStreamState: Codable {
    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = PlaybackStreamState(rawValue: raw) ?? .unknown
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode(rawValue)
    }
}

struct PlaybackStateMessage: Codable, Sendable, Equatable {
    let requestId: UInt64?
    let streamId: UInt32
    let clipId: String
    let state: PlaybackStreamState
    let positionSec: Double
    let durationSec: Double
    let message: String?
}

struct PlaybackConfigMessage: Codable, Sendable, Equatable {
    let streamId: UInt32
    let clipId: String
    let durationSec: Double
    let streamEpoch: UInt32
    let codecRaw: UInt8
    let width: Int
    let height: Int
    let modeRaw: Int
    let sbsLayoutRaw: Int
    let nalLengthHeaderBytes: Int
    let parameterSets: [Data]
}

struct PlaybackAccessUnit: Sendable {
    static let streamIdPrefixBytes = 4
    static let binaryHeaderBytes = streamIdPrefixBytes + VideoAccessUnitHeader.binarySize

    let streamId: UInt32
    let header: VideoAccessUnitHeader
    let avccPayload: Data

    func encode() -> Data {
        var out = Data(capacity: Self.binaryHeaderBytes + avccPayload.count)
        out.appendUInt32BE(streamId)
        out.append(header.encode(withAvccPayload: avccPayload))
        return out
    }

    static func decode(from data: Data) -> PlaybackAccessUnit? {
        guard data.count > Self.binaryHeaderBytes else { return nil }
        let sid = data.readUInt32BE(at: 0)
        let innerSlice = data.subdata(in: (data.startIndex + Self.streamIdPrefixBytes)..<data.endIndex)
        guard let (hdr, avcc) = VideoAccessUnitHeader.decode(from: innerSlice), !avcc.isEmpty else {
            return nil
        }
        return PlaybackAccessUnit(streamId: sid, header: hdr, avccPayload: avcc)
    }
}

enum WirePacketError: LocalizedError, Equatable {
    case clientFrameExceeds64KiBCap(frameBytes: Int)

    var errorDescription: String? {
        switch self {
        case .clientFrameExceeds64KiBCap(let bytes):
            return "Outgoing device-to-host frame (\(bytes) B) exceeds the 64 KiB protocol v3 cap."
        }
    }
}

enum WirePacket {
    static func pack(type: WireMessageType, flags: UInt8 = 0, payload: Data) -> Data {
        var out = Data(capacity: WireHeader.size + payload.count)
        out.append(WireHeader.magic0)
        out.append(WireHeader.magic1)
        out.append(type.rawValue)
        out.append(flags)
        out.appendUInt32BE(UInt32(payload.count))
        out.append(payload)
        return out
    }

    static func packRawType(_ rawType: UInt8, flags: UInt8 = 0, payload: Data) -> Data {
        var out = Data(capacity: WireHeader.size + payload.count)
        out.append(WireHeader.magic0)
        out.append(WireHeader.magic1)
        out.append(rawType)
        out.append(flags)
        out.appendUInt32BE(UInt32(payload.count))
        out.append(payload)
        return out
    }

    static func packJSON<T: Encodable>(type: WireMessageType, _ value: T) throws -> Data {
        let data = try JSONEncoder().encode(value)
        return pack(type: type, payload: data)
    }

    /// Packs a device -> host frame, enforcing the 64 KiB cap (`docs/protocol-v3-pet-camera.md` §1).
    static func packClientToHost(type: WireMessageType, flags: UInt8 = 0, payload: Data) throws -> Data {
        let totalBytes = WireHeader.size + payload.count
        guard totalBytes <= WireHeader.maxClientFrameBytes else {
            throw WirePacketError.clientFrameExceeds64KiBCap(frameBytes: totalBytes)
        }
        return pack(type: type, flags: flags, payload: payload)
    }

    static func packClientJSON<T: Encodable>(type: WireMessageType, _ value: T) throws -> Data {
        let data = try JSONEncoder().encode(value)
        return try packClientToHost(type: type, payload: data)
    }
}

final class WireFrameReader: @unchecked Sendable {
    private var buffer = Data()

    enum ParseResult {
        case frame(WireMessageType, UInt8, Data)
        case unknownFrame(UInt8, UInt8, Data)
        case needMoreData
        case protocolViolation(String)
    }

    func append(_ chunk: Data) {
        buffer.append(chunk)
    }

    func reset() {
        buffer.removeAll(keepingCapacity: false)
    }

    func nextFrame() -> ParseResult {
        guard buffer.count >= WireHeader.size else {
            return .needMoreData
        }
        let s = buffer.startIndex
        guard buffer[s] == WireHeader.magic0, buffer[s + 1] == WireHeader.magic1 else {
            return .protocolViolation("Invalid wire magic bytes.")
        }
        let rawType = buffer[s + 2]
        let flags = buffer[s + 3]
        let len = Int(buffer.readUInt32BE(at: 4))
        if len > WireHeader.maxPayloadBytes {
            return .protocolViolation("Wire frame exceeds max payload limit (\(len) B).")
        }
        guard buffer.count >= WireHeader.size + len else {
            return .needMoreData
        }
        let payload = buffer.subdata(in: (s + WireHeader.size)..<(s + WireHeader.size + len))
        buffer.removeSubrange(s..<(s + WireHeader.size + len))
        guard let msgType = WireMessageType(rawValue: rawType) else {
            return .unknownFrame(rawType, flags, payload)
        }
        return .frame(msgType, flags, payload)
    }
}

extension Data {
    mutating func appendUInt32BE(_ v: UInt32) {
        var be = v.bigEndian
        Swift.withUnsafeBytes(of: &be) { append(contentsOf: $0) }
    }

    mutating func appendUInt64BE(_ v: UInt64) {
        var be = v.bigEndian
        Swift.withUnsafeBytes(of: &be) { append(contentsOf: $0) }
    }

    func readUInt32BE(at offset: Int) -> UInt32 {
        let i = startIndex + offset
        return (UInt32(self[i]) << 24) |
               (UInt32(self[i + 1]) << 16) |
               (UInt32(self[i + 2]) << 8) |
               UInt32(self[i + 3])
    }

    func readUInt64BE(at offset: Int) -> UInt64 {
        let hi = UInt64(readUInt32BE(at: offset))
        let lo = UInt64(readUInt32BE(at: offset + 4))
        return (hi << 32) | lo
    }
}
