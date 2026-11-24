import AVFoundation
import CoreGraphics
import CryptoKit
import Foundation
import ImageIO
import Network
import XCTest
@testable import KinectViewer

final class Phase9BSyncBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var value: T

    init(_ initial: T) {
        self.value = initial
    }

    func update<R>(_ body: (inout T) -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        return body(&value)
    }

    func read() -> T {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

/// Synthetic loopback TLS 1.3 Pet Camera host implementing Protocol v3 W6 + W7 (Camera Roll & Streamed Playback)
/// + W8 (Outside Listener, One-Time Invites & Endpoint Links) for synthetic unit testing.
/// Never touches real sensors, Keychain, or real camera-roll/state directories.
final class FakeV3CameraRollAndInviteHost: @unchecked Sendable {
    enum OutsideRedemptionBehavior: Sendable {
        case normal
        case closeImmediatelyAfterHandshakeWithoutReply
        case closeAfterPairRequestWithoutReply
        case delayRedemptionReply(seconds: Double)
        case refuseWithInviteClosedByPC
        case refuseWithTooManyPairedDevices
        case rejectClientCertAtTLS
    }
    struct PairedClientRecord: Sendable {
        let certSHA256: Data
        let deviceId: String
        var name: String
        var role: PetRole
        var kind: String
        let pairedAtUnixMs: UInt64
    }

    struct StoredSyntheticClip: Sendable {
        var clip: RollClip
        var day: String
        var starredByDeviceIds: Set<String>
        var jpegThumbnail: Data?
    }

    struct OpenInviteRecord: Sendable {
        let token: Data
        let expiresUnixSec: UInt64
        let role: PetRole
    }

    struct PlaybackSessionStream: Sendable {
        let streamId: UInt32
        let clipId: String
        let durationSec: Double
        var state: PlaybackStreamState
        var positionSec: Double
        var streamEpoch: UInt32
    }

    private final class SessionEntry: @unchecked Sendable {
        let id = UUID()
        let conn: NWConnection
        let reader = WireFrameReader()
        let certSHA256: Data
        let deviceId: String
        let path: PetPath
        var negotiatedVersion: Int = 1
        var liveWatching: Bool = true
        var liveQuality: LiveQuality = .auto
        var liveTier: LiveTier = .high
        var activePlayback: PlaybackSessionStream? = nil

        init(conn: NWConnection, certSHA256: Data, deviceId: String, path: PetPath) {
            self.conn = conn
            self.certSHA256 = certSHA256
            self.deviceId = deviceId
            self.path = path
        }
    }

    let serverIdentity: CryptoIdentity
    private let netQueue = DispatchQueue(label: "kinectviewer.tests.fakev3rollhost")
    private var lanStreamListener: NWListener?
    private var lanPairListener: NWListener?
    private var outsideListener: NWListener?
    private var lanIPv6OutsideListener: NWListener?

    let serviceId: String
    private var activeQRToken: Data = Data(repeating: 0x33, count: 16)
    private var nextPairingRole: PetRole = .owner
    private var openInvite: OpenInviteRecord? = nil {
        didSet { if openInvite?.token != oldValue?.token { wrongAttemptsOnOpenInvite = 0 } }
    }
    /// Wrong proofs and certificate mismatches on the open invite; the tenth burns it (as the hosts do).
    private var wrongAttemptsOnOpenInvite = 0
    static let wrongAttemptsThatBurnAnInvite = 10
    private var hostNowUnixSecOverride: UInt64? = nil
    private var admitExpiredRedemptionWithoutOpenInvite: Bool = false
    private var outsideRedemptionBehavior: OutsideRedemptionBehavior = .normal
    private var lanIPv6OutsideBehavior: OutsideRedemptionBehavior = .normal
    private var lanIPv6IdentityOverride: CryptoIdentity? = nil
    private let outsideAcceptCountBox = Phase9BSyncBox<Int>(0)
    private let lanIPv6OutsideAcceptCountBox = Phase9BSyncBox<Int>(0)

    private var pairedDevicesByHash: [Data: PairedClientRecord] = [:]
    private var sessions: [UUID: SessionEntry] = [:]
    private var stateSeq: UInt64 = 0
    private var nextPlaybackStreamId: UInt32 = 0

    // Host flags & W7/W8 state
    private var cameraRollShipped: Bool
    private var outsideShipped: Bool
    private var powerState: WireSensorPowerState = .on
    private var allowPhoneModeSwitch: Bool = true
    private var allowPhoneSensorPower: Bool = true
    private var modeRaw: Int = 1
    private var sbsLayoutRaw: Int = 0
    private var isHostFrozen: Bool = false
    private var isReplayingOnHost: Bool = false
    private var sharingEnabled: Bool = true
    private var sharingScheduleOff: Bool = false
    private var outsideEnabled: Bool = false
    private var outsideStatus: String? = nil
    private var outsideEndpointAdvertised: PetEndpoint? = nil
    private var outsideCandidatesAdvertised: [PetEndpoint]? = nil
    private var privacyMode: Bool = false
    private var rollEnabled: Bool = true
    private var rollPreparing: Bool = false
    private var folderUnavailable: Bool = false
    private var diskFloorReached: Bool = false
    private var storageFullReached: Bool = false
    private var customBlockedReasonOverride: (code: String, text: String?)? = nil
    private var motionEnabled: Bool = false
    private var motionSensitivity: Int = 5
    private var scheduleEnabled: Bool = false
    private var scheduleSummary: String? = nil
    private var clipRecording: Bool = false
    private var clipTrigger: ClipTrigger? = nil
    private var clipManualActive: Bool = false
    private var clipElapsedSec: Double = 0.0

    // Camera roll synthetic store & test controls
    private var clipsStore: [StoredSyntheticClip] = []
    private var holdPendingThumbnailRequests: Bool = false
    private var heldThumbRequests: [(sessionId: UUID, req: RollThumbRequestMessage)] = []
    private var holdPlaybackInStartingState: Bool = false

    // Pre-encoded synthetic H.264 clip frames (in memory only)
    private let syntheticClipParameterSets: [Data]
    private let syntheticClipAvccPayloads: [Data]
    private let defaultSyntheticJPEG: Data

    private let metricsBox = Phase9BSyncBox<(
        receivedRollListReqs: [RollListRequestMessage],
        receivedRollThumbReqs: [RollThumbRequestMessage],
        receivedPlaybackReqs: [PlaybackRequestMessage],
        receivedPetCommands: [PetCameraCommandMessage],
        receivedLiveViewReqs: [LiveViewRequestMessage],
        receivedPairReqs: [PairRequestMessage]
    )>(([], [], [], [], [], []))

    private(set) var lanStreamPort: UInt16 = 0
    private(set) var lanPairPort: UInt16 = 0
    private(set) var outsidePort: UInt16 = 0

    init(
        serviceId: String = "v3roll000001",
        cameraRollShipped: Bool = true,
        outsideShipped: Bool = true,
        startLanStream: Bool = true,
        startOutsideListener: Bool = true
    ) throws {
        self.serviceId = serviceId
        self.cameraRollShipped = cameraRollShipped
        self.outsideShipped = outsideShipped
        self.serverIdentity = try CryptoIdentity.generateNew(commonName: "kv3-fake-w7w8-pc")
        let encoded = Self.buildSyntheticEncodedClip()
        self.syntheticClipParameterSets = encoded.parameterSets
        self.syntheticClipAvccPayloads = encoded.avccPayloads
        self.defaultSyntheticJPEG = Self.makeSyntheticJPEGData(width: 64, height: 48)
        try startLanPairListener()
        if startLanStream {
            try startLanStreamListener()
        }
        if startOutsideListener {
            try openOutsideListener()
        }
    }

    func shutdown() {
        netQueue.sync {
            for (_, s) in sessions {
                s.conn.cancel()
            }
            sessions.removeAll()
            lanStreamListener?.cancel()
            lanStreamListener = nil
            lanPairListener?.cancel()
            lanPairListener = nil
            outsideListener?.cancel()
            outsideListener = nil
            lanIPv6OutsideListener?.cancel()
            lanIPv6OutsideListener = nil
        }
    }

    var outsideAcceptCount: Int { outsideAcceptCountBox.read() }
    var lanIPv6OutsideAcceptCount: Int { lanIPv6OutsideAcceptCountBox.read() }
    var heldThumbnailCount: Int { netQueue.sync { heldThumbRequests.count } }

    var metrics: (
        receivedRollListReqs: [RollListRequestMessage],
        receivedRollThumbReqs: [RollThumbRequestMessage],
        receivedPlaybackReqs: [PlaybackRequestMessage],
        receivedPetCommands: [PetCameraCommandMessage],
        receivedLiveViewReqs: [LiveViewRequestMessage],
        receivedPairReqs: [PairRequestMessage]
    ) {
        metricsBox.read()
    }

    // MARK: - Synthetic Media Builders (Zero Disk Writes)

    private static func makeSyntheticJPEGData(width: Int, height: Int) -> Data {
        var rgba = [UInt8](repeating: 0, count: width * height * 4)
        for y in 0..<height {
            for x in 0..<width {
                let i = (y * width + x) * 4
                rgba[i] = UInt8((x * 4) & 0xFF)
                rgba[i + 1] = UInt8((y * 5) & 0xFF)
                rgba[i + 2] = 160
                rgba[i + 3] = 255
            }
        }
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let cfData = Data(rgba) as CFData
        guard let provider = CGDataProvider(data: cfData),
              let cgImage = CGImage(
                  width: width,
                  height: height,
                  bitsPerComponent: 8,
                  bitsPerPixel: 32,
                  bytesPerRow: width * 4,
                  space: colorSpace,
                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                  provider: provider,
                  decode: nil,
                  shouldInterpolate: false,
                  intent: .defaultIntent
              ),
              let mutableOut = CFDataCreateMutable(kCFAllocatorDefault, 0),
              let dest = CGImageDestinationCreateWithData(mutableOut, "public.jpeg" as CFString, 1, nil) else {
            return Data([0xFF, 0xD8, 0xFF, 0xD9])
        }
        let opts: [CFString: Any] = [kCGImageDestinationLossyCompressionQuality: 0.7]
        CGImageDestinationAddImage(dest, cgImage, opts as CFDictionary)
        _ = CGImageDestinationFinalize(dest)
        return mutableOut as Data
    }

    private static func buildSyntheticEncodedClip(frameCount: Int = 4, width: Int = 320, height: Int = 240) -> (parameterSets: [Data], avccPayloads: [Data]) {
        let encoder = VideoToolboxEncoder()
        let box = Phase9BSyncBox<[EncodedVideoFrame]>([])
        let sem = DispatchSemaphore(value: 0)
        encoder.onEncodedFrame = { frame in
            box.update { $0.append(frame) }
            sem.signal()
        }
        let stride = width * 4
        var bgra = [UInt8](repeating: 0, count: stride * height)
        for i in 0..<frameCount {
            for y in 0..<height {
                for x in 0..<width {
                    let idx = y * stride + x * 4
                    bgra[idx] = UInt8((x + i * 23) & 0xFF)
                    bgra[idx + 1] = UInt8((y + i * 13) & 0xFF)
                    bgra[idx + 2] = 200
                    bgra[idx + 3] = 255
                }
            }
            bgra.withUnsafeBufferPointer { ptr in
                encoder.encodeBGRAFrame(
                    bgraPtr: ptr.baseAddress!,
                    srcWidth: width,
                    srcHeight: height,
                    srcStride: stride,
                    frameSeq: UInt64(i),
                    hostCaptureNs: UInt64(i) * 33_333_333,
                    modeRaw: 1,
                    sbsRaw: 0,
                    hasClockOverlay: false
                )
            }
            _ = sem.wait(timeout: .now() + 2.0)
        }
        encoder.teardown()
        let frames = box.read()
        let paramSets = frames.compactMap { $0.configIfChanged?.parameterSets }.first ?? []
        let avccs = frames.compactMap { VideoAccessUnitHeader.decode(from: $0.wirePayload)?.avccPayload }
        return (paramSets, avccs)
    }

    // MARK: - Test Mutators

    func seedClips(_ entries: [(day: String, clipId: String, startUnixMs: UInt64, startLocal: String, durationSec: Double, trigger: ClipTrigger, startedBy: String?, starredByIds: Set<String>, hasThumb: Bool)]) {
        netQueue.sync {
            self.clipsStore = entries.map { item in
                let rollClip = RollClip(
                    clipId: item.clipId,
                    startUnixMs: item.startUnixMs,
                    startLocal: item.startLocal,
                    durationSec: item.durationSec,
                    trigger: item.trigger,
                    startedBy: item.startedBy,
                    starred: !item.starredByIds.isEmpty,
                    starredByMe: false,
                    width: 320,
                    height: 240,
                    bytes: 120_000,
                    hasThumbnail: item.hasThumb
                )
                return StoredSyntheticClip(
                    clip: rollClip,
                    day: item.day,
                    starredByDeviceIds: item.starredByIds,
                    jpegThumbnail: item.hasThumb ? self.defaultSyntheticJPEG : nil
                )
            }
        }
    }

    func prepareLanPairingOffer(role: PetRole) -> String {
        netQueue.sync {
            self.nextPairingRole = role
            var tok = [UInt8](repeating: 0, count: 16)
            _ = SecRandomCopyBytes(kSecRandomDefault, tok.count, &tok)
            self.activeQRToken = Data(tok)
            return QRPairingPayload(
                serviceId: self.serviceId,
                serverCertSHA256: self.serverIdentity.certificateSHA256,
                oneTimeToken: self.activeQRToken,
                hostIP: "127.0.0.1",
                pairPort: self.lanPairPort
            ).toURIString()
        }
    }

    func createOutsideInvite(
        role: PetRole,
        expiresUnixSec: UInt64,
        lanHost: String = "127.0.0.1",
        outsideHost: String = "127.0.0.1",
        outsidePortOverride: UInt16? = nil
    ) -> InviteLinkPayload {
        netQueue.sync {
            var tok = [UInt8](repeating: 0, count: 16)
            _ = SecRandomCopyBytes(kSecRandomDefault, tok.count, &tok)
            let tokenData = Data(tok)
            self.openInvite = OpenInviteRecord(token: tokenData, expiresUnixSec: expiresUnixSec, role: role)
            self.outsideEnabled = true
            let port = outsidePortOverride ?? self.outsidePort
            self.outsideEndpointAdvertised = PetEndpoint(host: outsideHost, port: port, verified: true)
            return InviteLinkPayload(
                serviceId: self.serviceId,
                serverCertSHA256: self.serverIdentity.certificateSHA256,
                oneTimeToken: tokenData,
                outsideHost: outsideHost,
                outsidePort: port,
                lanHost: lanHost,
                expiresUnixSec: expiresUnixSec,
                roleHint: role
            )
        }
    }

    func importPairedDevicesFrom(_ other: FakeV3CameraRollAndInviteHost) {
        let copied = other.netQueue.sync { other.pairedDevicesByHash }
        netQueue.sync {
            for (k, v) in copied {
                self.pairedDevicesByHash[k] = v
            }
        }
    }

    func setLanStreamEnabled(_ enabled: Bool) throws {
        if enabled {
            try startLanStreamListener()
        } else {
            netQueue.sync {
                for (id, s) in sessions where s.path == .home {
                    s.conn.cancel()
                    sessions.removeValue(forKey: id)
                }
                lanStreamListener?.cancel()
                lanStreamListener = nil
            }
        }
    }

    func setOutsideListenerEnabled(_ enabled: Bool, sendNoticeOnDisable: Bool = false) throws {
        if enabled {
            try openOutsideListener()
            netQueue.sync {
                self.outsideEnabled = true
                broadcastAllSessionsOnQueue()
            }
        } else {
            netQueue.sync {
                self.outsideEnabled = false
                self.openInvite = nil
                for (id, s) in sessions where s.path == .outside {
                    if sendNoticeOnDisable && s.negotiatedVersion >= 3 {
                        let notice = SessionNoticeMessage(
                            reason: .outsideDisabled,
                            closing: true,
                            message: PetSessionNoticeReason.outsideDisabled.defaultSentence
                        )
                        if let pkt = try? WirePacket.packJSON(type: .sessionNotice, notice) {
                            s.conn.send(content: pkt, completion: .contentProcessed { _ in
                                s.conn.cancel()
                            })
                        } else {
                            s.conn.cancel()
                        }
                    } else {
                        s.conn.cancel()
                    }
                    sessions.removeValue(forKey: id)
                }
                outsideListener?.cancel()
                outsideListener = nil
                broadcastAllSessionsOnQueue()
            }
        }
    }

    func closeAllSessions() {
        netQueue.sync {
            for (_, s) in sessions {
                s.conn.cancel()
            }
            sessions.removeAll()
        }
    }

    func setAdmitExpiredRedemptionWithoutOpenInvite(_ admit: Bool, hostNowUnixSec: UInt64? = nil) {
        netQueue.sync {
            self.admitExpiredRedemptionWithoutOpenInvite = admit
            self.hostNowUnixSecOverride = hostNowUnixSec
        }
    }

    func setOutsideRedemptionBehavior(_ behavior: OutsideRedemptionBehavior) {
        netQueue.sync {
            self.outsideRedemptionBehavior = behavior
        }
    }

    func resetAcceptCounters() {
        outsideAcceptCountBox.update { $0 = 0 }
        lanIPv6OutsideAcceptCountBox.update { $0 = 0 }
    }

    /// Extra outside candidates in 0x0B (`outsideCandidates`), beside the main endpoint.
    func setAdvertisedOutsideCandidates(_ candidates: [PetEndpoint]?) {
        netQueue.sync {
            self.outsideCandidatesAdvertised = candidates
            broadcastAllSessionsOnQueue()
        }
    }

    func setAdvertisedOutsideEndpoint(
        _ endpoint: PetEndpoint?,
        outsideEnabled: Bool = true,
        outsideStatus: String? = "mapped"
    ) {
        netQueue.sync {
            self.outsideEndpointAdvertised = endpoint
            self.outsideEnabled = outsideEnabled
            self.outsideStatus = outsideStatus
            broadcastAllSessionsOnQueue()
        }
    }

    func setLanIPv6OutsideListenerEnabled(
        _ enabled: Bool,
        identityOverride: CryptoIdentity? = nil,
        behavior: OutsideRedemptionBehavior = .normal
    ) throws {
        var cancelSem: DispatchSemaphore? = nil
        netQueue.sync {
            if let existing = self.lanIPv6OutsideListener {
                let sem = DispatchSemaphore(value: 0)
                cancelSem = sem
                existing.stateUpdateHandler = { st in
                    if case .cancelled = st {
                        sem.signal()
                    }
                }
                existing.cancel()
                self.lanIPv6OutsideListener = nil
            }
            self.lanIPv6IdentityOverride = identityOverride
            self.lanIPv6OutsideBehavior = behavior
        }
        if let sem = cancelSem {
            _ = sem.wait(timeout: .now() + 2.0)
            // Brief settle for BSD socket port release on loopback
            Thread.sleep(forTimeInterval: 0.03)
        }
        guard enabled else { return }
        let readySem = DispatchSemaphore(value: 0)
        let lastVerifiedPeerBox = Phase9BSyncBox<Data?>(nil)
        let targetPort = NWEndpoint.Port(rawValue: outsidePort)!
        let serverIdent = identityOverride ?? serverIdentity

        let outParams = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdent,
            requireClientCert: true,
            verifyQueue: netQueue
        ) { [weak self] peerHash, _ in
            guard let self = self else { return false }
            if case .rejectClientCertAtTLS = self.lanIPv6OutsideBehavior {
                return false
            }
            let isListed = self.pairedDevicesByHash.keys.contains {
                CryptoIdentity.constantTimeEqual($0, peerHash)
            }
            if isListed || self.openInvite != nil || self.admitExpiredRedemptionWithoutOpenInvite {
                lastVerifiedPeerBox.update { $0 = peerHash }
                return true
            }
            return false
        }
        outParams.allowLocalEndpointReuse = true
        if let ipOpts = outParams.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
            ipOpts.version = .v6
        }
        outParams.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv6(.loopback), port: targetPort)
        let ol = try NWListener(using: outParams)
        ol.stateUpdateHandler = { st in
            if case .ready = st {
                readySem.signal()
            }
        }
        ol.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            self.lanIPv6OutsideAcceptCountBox.update { $0 += 1 }
            self.handleOutsideConnectionOnQueue(conn, lastVerifiedPeerBox: lastVerifiedPeerBox, isLanIPv6: true)
        }
        netQueue.sync { self.lanIPv6OutsideListener = ol }
        ol.start(queue: netQueue)
        _ = readySem.wait(timeout: .now() + 5.0)
    }

    func setHoldPendingThumbnails(_ hold: Bool) {
        netQueue.sync {
            self.holdPendingThumbnailRequests = hold
        }
    }

    func setHoldPlaybackInStartingState(_ hold: Bool) {
        netQueue.sync {
            self.holdPlaybackInStartingState = hold
        }
    }

    func releaseStartingPlaybacksToPlaying() {
        netQueue.sync {
            self.holdPlaybackInStartingState = false
            for (sid, s) in sessions {
                guard var pb = s.activePlayback, pb.state == .starting else { continue }
                pb.state = .playing
                sessions[sid]?.activePlayback = pb
                sendPlaybackConfigAndFramesOnQueue(session: s, pb: pb, frameCount: 2)
                let stMsg = PlaybackStateMessage(
                    requestId: nil,
                    streamId: pb.streamId,
                    clipId: pb.clipId,
                    state: .playing,
                    positionSec: pb.positionSec,
                    durationSec: pb.durationSec,
                    message: nil
                )
                if let pkt = try? WirePacket.packJSON(type: .playbackState, stMsg) {
                    s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
            }
        }
    }

    func simulatePlaybackReachedEnd() {
        netQueue.sync {
            for (sid, s) in sessions {
                guard var pb = s.activePlayback else { continue }
                pb.state = .ended
                pb.positionSec = pb.durationSec
                sessions[sid]?.activePlayback = pb
                let msg = PlaybackStateMessage(
                    requestId: nil,
                    streamId: pb.streamId,
                    clipId: pb.clipId,
                    state: .ended,
                    positionSec: pb.durationSec,
                    durationSec: pb.durationSec,
                    message: nil
                )
                if let pkt = try? WirePacket.packJSON(type: .playbackState, msg) {
                    s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
            }
        }
    }

    func simulatePlaybackIdle60sStop() {
        netQueue.sync {
            for (sid, s) in sessions {
                guard let pb = s.activePlayback else { continue }
                sessions[sid]?.activePlayback = nil
                let msg = PlaybackStateMessage(
                    requestId: nil,
                    streamId: pb.streamId,
                    clipId: pb.clipId,
                    state: .stopped,
                    positionSec: pb.positionSec,
                    durationSec: pb.durationSec,
                    message: PetCameraUserStrings.playbackIdleStoppedSentence
                )
                if let pkt = try? WirePacket.packJSON(type: .playbackState, msg) {
                    s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
                broadcastAllSessionsOnQueue()
            }
        }
    }

    func simulatePlaybackReadError() {
        netQueue.sync {
            for (sid, s) in sessions {
                guard let pb = s.activePlayback else { continue }
                sessions[sid]?.activePlayback = nil
                let msg = PlaybackStateMessage(
                    requestId: nil,
                    streamId: pb.streamId,
                    clipId: pb.clipId,
                    state: .error,
                    positionSec: pb.positionSec,
                    durationSec: pb.durationSec,
                    message: PetCameraUserStrings.playbackClipReadErrorSentence
                )
                if let pkt = try? WirePacket.packJSON(type: .playbackState, msg) {
                    s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
            }
        }
    }

    func injectStaleEpochPlaybackFrame(streamId: UInt32, staleEpoch: UInt32) {
        netQueue.sync {
            guard let firstAvcc = syntheticClipAvccPayloads.first else { return }
            let hdr = VideoAccessUnitHeader(
                streamEpoch: staleEpoch,
                frameSeq: 0,
                hostCaptureNs: 9_000_000_000,
                hostEncodeDoneNs: 0,
                flags: 0x01
            )
            let au = PlaybackAccessUnit(streamId: streamId, header: hdr, avccPayload: firstAvcc)
            let pkt = WirePacket.pack(type: .playbackAccessUnit, payload: au.encode())
            for (_, s) in sessions {
                s.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
    }

    func sendAddressChangedNotice(newEndpoint: PetEndpoint?) {
        netQueue.sync {
            self.outsideStatus = "address_changed"
            self.outsideEndpointAdvertised = newEndpoint
            let notice = SessionNoticeMessage(
                reason: .addressChanged,
                closing: false,
                message: PetSessionNoticeReason.addressChanged.defaultSentence
            )
            if let pkt = try? WirePacket.packJSON(type: .sessionNotice, notice) {
                for (_, s) in sessions where s.negotiatedVersion >= 3 {
                    s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
            }
            broadcastAllSessionsOnQueue()
        }
    }

    func configureState(
        privacyMode: Bool? = nil,
        sharingEnabled: Bool? = nil,
        isReplayingOnHost: Bool? = nil,
        powerState: WireSensorPowerState? = nil,
        isHostFrozen: Bool? = nil,
        rollEnabled: Bool? = nil,
        rollPreparing: Bool? = nil,
        folderUnavailable: Bool? = nil,
        diskFloorReached: Bool? = nil,
        storageFullReached: Bool? = nil,
        customBlockedReason: (code: String, text: String?)?? = nil,
        clipRecording: Bool? = nil,
        clipTrigger: ClipTrigger?? = nil,
        clipManualActive: Bool? = nil,
        clipElapsedSec: Double? = nil
    ) {
        netQueue.sync {
            let turningPrivacyOn = (privacyMode == true && !self.privacyMode)
            if let v = privacyMode { self.privacyMode = v }
            if let v = sharingEnabled { self.sharingEnabled = v }
            if let v = isReplayingOnHost { self.isReplayingOnHost = v }
            if let v = powerState { self.powerState = v }
            if let v = isHostFrozen { self.isHostFrozen = v }
            if let v = rollEnabled { self.rollEnabled = v }
            if let v = rollPreparing { self.rollPreparing = v }
            if let v = folderUnavailable { self.folderUnavailable = v }
            if let v = diskFloorReached { self.diskFloorReached = v }
            if let v = storageFullReached { self.storageFullReached = v }
            if let v = customBlockedReason { self.customBlockedReasonOverride = v }
            if let v = clipRecording { self.clipRecording = v }
            if let v = clipTrigger { self.clipTrigger = v }
            if let v = clipManualActive { self.clipManualActive = v }
            if let v = clipElapsedSec { self.clipElapsedSec = v }

            if turningPrivacyOn {
                // §6.4 & §5.9: Privacy mode stops all playbacks (`0x17 stopped`) and answers pending `0x14` thumbnails with `0x0D privacy_mode`!
                for (sid, s) in sessions {
                    if let pb = s.activePlayback {
                        sessions[sid]?.activePlayback = nil
                        let stopMsg = PlaybackStateMessage(
                            requestId: nil,
                            streamId: pb.streamId,
                            clipId: pb.clipId,
                            state: .stopped,
                            positionSec: pb.positionSec,
                            durationSec: pb.durationSec,
                            message: PetCameraUserStrings.privacyModeSentence
                        )
                        if let pkt = try? WirePacket.packJSON(type: .playbackState, stopMsg) {
                            s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                        }
                    }
                }
                let drained = self.heldThumbRequests
                self.heldThumbRequests.removeAll()
                for item in drained {
                    if let s = sessions[item.sessionId] {
                        sendCommandErrorOnQueue(
                            session: s,
                            requestId: item.req.requestId,
                            code: "privacy_mode",
                            message: PetCameraUserStrings.privacyModeSentence
                        )
                    }
                }
            }

            // If `folderUnavailable` flipped, re-send hello + 0x0B so clients see `cameraRoll: false` if needed.
            if folderUnavailable != nil {
                for (_, s) in sessions {
                    sendHelloOnQueue(session: s)
                }
            }
            broadcastAllSessionsOnQueue()
        }
    }

    // MARK: - Listeners Setup

    private func startLanPairListener() throws {
        let readySem = DispatchSemaphore(value: 0)
        let pairParams = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdentity,
            requireClientCert: true,
            verifyQueue: netQueue
        ) { _, _ in true }
        pairParams.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        let pl = try NWListener(using: pairParams)
        pl.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.lanPairPort = pl.port?.rawValue ?? 0
                readySem.signal()
            }
        }
        pl.newConnectionHandler = { [weak self] conn in
            self?.handleLanPairConnectionOnQueue(conn)
        }
        self.lanPairListener = pl
        pl.start(queue: netQueue)
        _ = readySem.wait(timeout: .now() + 5.0)
    }

    private func startLanStreamListener() throws {
        let alreadyUp = netQueue.sync { lanStreamListener != nil }
        if alreadyUp { return }

        let readySem = DispatchSemaphore(value: 0)
        let capturedPeer = Phase9BSyncBox<[ObjectIdentifier: Data]>([:])
        let targetPort: NWEndpoint.Port = (lanStreamPort > 0) ? NWEndpoint.Port(rawValue: lanStreamPort)! : .any
        let streamParams = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdentity,
            requireClientCert: true,
            verifyQueue: netQueue
        ) { [weak self] peerHash, _ in
            guard let self = self else { return false }
            let admitted = self.pairedDevicesByHash.keys.contains {
                CryptoIdentity.constantTimeEqual($0, peerHash)
            }
            if admitted {
                capturedPeer.update { $0[ObjectIdentifier(self)] = peerHash }
            }
            return admitted
        }
        streamParams.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: targetPort)
        let sl = try NWListener(using: streamParams)
        sl.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                if let p = sl.port?.rawValue, p > 0 {
                    self?.lanStreamPort = p
                }
                readySem.signal()
            }
        }
        sl.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            self.acceptStreamConnectionOnQueue(conn: conn, path: .home, capturedBox: capturedPeer)
        }
        netQueue.sync { self.lanStreamListener = sl }
        sl.start(queue: netQueue)
        _ = readySem.wait(timeout: .now() + 5.0)
    }

    private func openOutsideListener() throws {
        let alreadyUp = netQueue.sync { outsideListener != nil }
        if alreadyUp { return }

        let readySem = DispatchSemaphore(value: 0)
        let lastVerifiedPeerBox = Phase9BSyncBox<Data?>(nil)
        let targetPort: NWEndpoint.Port = (outsidePort > 0) ? NWEndpoint.Port(rawValue: outsidePort)! : .any

        let outParams = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdentity,
            requireClientCert: true,
            verifyQueue: netQueue
        ) { [weak self] peerHash, _ in
            guard let self = self else { return false }
            if case .rejectClientCertAtTLS = self.outsideRedemptionBehavior {
                return false
            }
            let isListed = self.pairedDevicesByHash.keys.contains {
                CryptoIdentity.constantTimeEqual($0, peerHash)
            }
            if isListed || self.openInvite != nil || self.admitExpiredRedemptionWithoutOpenInvite {
                lastVerifiedPeerBox.update { $0 = peerHash }
                return true
            }
            return false
        }
        outParams.allowLocalEndpointReuse = true
        if let ipOpts = outParams.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
            ipOpts.version = .v4
        }
        outParams.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.loopback), port: targetPort)
        let ol = try NWListener(using: outParams)
        ol.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                if let p = ol.port?.rawValue, p > 0 {
                    self?.outsidePort = p
                }
                readySem.signal()
            }
        }
        ol.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            self.outsideAcceptCountBox.update { $0 += 1 }
            self.handleOutsideConnectionOnQueue(conn, lastVerifiedPeerBox: lastVerifiedPeerBox, isLanIPv6: false)
        }
        netQueue.sync { self.outsideListener = ol }
        ol.start(queue: netQueue)
        _ = readySem.wait(timeout: .now() + 5.0)
    }

    // MARK: - Pairing & Outside Redemption

    private func handleLanPairConnectionOnQueue(_ conn: NWConnection) {
        let reader = WireFrameReader()
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            if case .ready = st {
                conn.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { data, _, _, err in
                    guard err == nil, let data = data else {
                        conn.cancel()
                        return
                    }
                    reader.append(data)
                    guard case .frame(let msgType, _, let payload) = reader.nextFrame(),
                          msgType == .pairRequest,
                          let req = try? JSONDecoder().decode(PairRequestMessage.self, from: payload) else {
                        conn.cancel()
                        return
                    }
                    self.metricsBox.update { $0.receivedPairReqs.append(req) }
                    // §4.4: LAN pairing listener takes methodRaw 0 or 1 only!
                    guard req.methodRaw == 0 || req.methodRaw == 1 else {
                        conn.cancel()
                        return
                    }
                    let clientHash = Data(SHA256.hash(data: req.clientCertDER))
                    let expectedProof = CryptoIdentity.computePairingProof(
                        secret: self.activeQRToken,
                        serverCertSHA256: self.serverIdentity.certificateSHA256,
                        clientCertSHA256: clientHash,
                        role: "kv2-pair-client"
                    )
                    guard CryptoIdentity.constantTimeEqual(req.clientProofHMAC, expectedProof) else {
                        let rej = PairResponseMessage(
                            accepted: false,
                            serviceId: "",
                            streamPort: 0,
                            serverProofHMAC: Data(),
                            message: "Invalid pairing token."
                        )
                        if let pkt = try? WirePacket.packJSON(type: .pairResponse, rej) {
                            conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                        }
                        return
                    }

                    let devId = clientHash.prefix(8).map { String(format: "%02x", $0) }.joined()
                    let name = req.deviceName ?? "iPhone"
                    let kind = req.deviceKind ?? "iphone"
                    let role = self.nextPairingRole
                    self.pairedDevicesByHash[clientHash] = PairedClientRecord(
                        certSHA256: clientHash,
                        deviceId: devId,
                        name: name,
                        role: role,
                        kind: kind,
                        pairedAtUnixMs: 1_795_000_000_000
                    )
                    let serverProof = CryptoIdentity.computePairingProof(
                        secret: self.activeQRToken,
                        serverCertSHA256: self.serverIdentity.certificateSHA256,
                        clientCertSHA256: clientHash,
                        role: "kv2-pair-server"
                    )
                    let resp = PairResponseMessage(
                        accepted: true,
                        serviceId: self.serviceId,
                        streamPort: self.lanStreamPort,
                        serverProofHMAC: serverProof,
                        message: "Paired",
                        role: role,
                        deviceId: devId,
                        deviceName: name,
                        protocolVersion: 3
                    )
                    if let pkt = try? WirePacket.packJSON(type: .pairResponse, resp) {
                        conn.send(content: pkt, completion: .contentProcessed { _ in
                            conn.cancel()
                        })
                    }
                    self.broadcastAllSessionsOnQueue()
                }
            }
        }
        conn.start(queue: netQueue)
    }

    private func handleOutsideConnectionOnQueue(
        _ conn: NWConnection,
        lastVerifiedPeerBox: Phase9BSyncBox<Data?>,
        isLanIPv6: Bool
    ) {
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            if case .ready = st {
                guard let peerHash = lastVerifiedPeerBox.read() else {
                    conn.cancel()
                    return
                }
                let behavior = isLanIPv6 ? self.lanIPv6OutsideBehavior : self.outsideRedemptionBehavior
                if case .closeImmediatelyAfterHandshakeWithoutReply = behavior {
                    conn.cancel()
                    return
                }
                // §8.1: Listed device -> ordinary stream session (`path: .outside`).
                if let matchedKey = self.pairedDevicesByHash.keys.first(where: { CryptoIdentity.constantTimeEqual($0, peerHash) }),
                   let rec = self.pairedDevicesByHash[matchedKey] {
                    self.registerActiveSessionOnQueue(conn: conn, record: rec, key: matchedKey, path: .outside)
                    return
                }
                // Otherwise redemption-only connection (`0x10` with `methodRaw == 2`, NO hello first!).
                let reader = WireFrameReader()
                conn.receive(minimumIncompleteLength: 1, maximumLength: 16_384) { data, _, _, err in
                    guard err == nil, let data = data, data.count <= 16_384 else {
                        conn.cancel()
                        return
                    }
                    reader.append(data)
                    guard case .frame(let msgType, _, let payload) = reader.nextFrame(),
                          msgType == .pairRequest,
                          let req = try? JSONDecoder().decode(PairRequestMessage.self, from: payload) else {
                        conn.cancel()
                        return
                    }
                    self.metricsBox.update { $0.receivedPairReqs.append(req) }
                    guard req.methodRaw == WirePairingMethod.invite.rawValue else {
                        conn.cancel()
                        return
                    }

                    switch behavior {
                    case .closeAfterPairRequestWithoutReply:
                        self.openInvite = nil
                        conn.cancel()
                        return
                    case .refuseWithInviteClosedByPC:
                        let rej = PairResponseMessage(
                            accepted: false,
                            serviceId: "",
                            streamPort: 0,
                            serverProofHMAC: Data(),
                            message: "This invite was closed. Ask for a new one."
                        )
                        if let pkt = try? WirePacket.packJSON(type: .pairResponse, rej) {
                            conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                        }
                        return
                    case .refuseWithTooManyPairedDevices:
                        let rej = PairResponseMessage(
                            accepted: false,
                            serviceId: "",
                            streamPort: 0,
                            serverProofHMAC: Data(),
                            message: PetCameraUserStrings.pairingTooManyDevicesSentence
                        )
                        if let pkt = try? WirePacket.packJSON(type: .pairResponse, rej) {
                            conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                        }
                        return
                    case .normal, .delayRedemptionReply, .closeImmediatelyAfterHandshakeWithoutReply, .rejectClientCertAtTLS:
                        break
                    }

                    let nowSec = self.hostNowUnixSecOverride ?? UInt64(Date().timeIntervalSince1970)
                    guard let invite = self.openInvite, nowSec <= invite.expiresUnixSec else {
                        self.openInvite = nil
                        let rej = PairResponseMessage(
                            accepted: false,
                            serviceId: "",
                            streamPort: 0,
                            serverProofHMAC: Data(),
                            message: PetCameraUserStrings.inviteExpiredOrUsedSentence
                        )
                        if let pkt = try? WirePacket.packJSON(type: .pairResponse, rej) {
                            conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                        }
                        return
                    }

                    let clientDerHash = Data(SHA256.hash(data: req.clientCertDER))
                    guard CryptoIdentity.constantTimeEqual(peerHash, clientDerHash) else {
                        // §4.4 & §8.4: TLS leaf != clientCertDER counts as a wrong attempt (the tenth burns the
                        // invite) and is always closed without a reply.
                        self.wrongAttemptsOnOpenInvite += 1
                        if self.wrongAttemptsOnOpenInvite >= Self.wrongAttemptsThatBurnAnInvite { self.openInvite = nil }
                        conn.cancel()
                        return
                    }

                    let expectedProof = CryptoIdentity.computePairingProof(
                        secret: invite.token,
                        serverCertSHA256: self.serverIdentity.certificateSHA256,
                        clientCertSHA256: clientDerHash,
                        role: "kv2-pair-client"
                    )
                    guard CryptoIdentity.constantTimeEqual(req.clientProofHMAC, expectedProof) else {
                        // A wrong proof leaves the invite open ("didn't match"); the tenth wrong attempt burns it.
                        self.wrongAttemptsOnOpenInvite += 1
                        let burned = self.wrongAttemptsOnOpenInvite >= Self.wrongAttemptsThatBurnAnInvite
                        if burned { self.openInvite = nil }
                        let rej = PairResponseMessage(
                            accepted: false,
                            serviceId: "",
                            streamPort: 0,
                            serverProofHMAC: Data(),
                            message: burned ? PetCameraUserStrings.inviteWrongCodeSentence
                                            : PetCameraUserStrings.inviteDidntMatchSentence
                        )
                        if let pkt = try? WirePacket.packJSON(type: .pairResponse, rej) {
                            conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                        }
                        return
                    }

                    // Consume single-use invite!
                    self.openInvite = nil
                    let devId = clientDerHash.prefix(8).map { String(format: "%02x", $0) }.joined()
                    let name = req.deviceName ?? "Invited Phone"
                    let kind = req.deviceKind ?? "iphone"
                    self.pairedDevicesByHash[clientDerHash] = PairedClientRecord(
                        certSHA256: clientDerHash,
                        deviceId: devId,
                        name: name,
                        role: invite.role,
                        kind: kind,
                        pairedAtUnixMs: 1_795_000_000_000
                    )
                    let serverProof = CryptoIdentity.computePairingProof(
                        secret: invite.token,
                        serverCertSHA256: self.serverIdentity.certificateSHA256,
                        clientCertSHA256: clientDerHash,
                        role: "kv2-pair-server"
                    )
                    let resp = PairResponseMessage(
                        accepted: true,
                        serviceId: self.serviceId,
                        streamPort: self.lanStreamPort,
                        serverProofHMAC: serverProof,
                        message: "Invited",
                        role: invite.role,
                        deviceId: devId,
                        deviceName: name,
                        protocolVersion: 3
                    )
                    if let pkt = try? WirePacket.packJSON(type: .pairResponse, resp) {
                        if case .delayRedemptionReply(let delaySec) = behavior {
                            self.netQueue.asyncAfter(deadline: .now() + delaySec) {
                                conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                            }
                        } else {
                            conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                        }
                    }
                    self.broadcastAllSessionsOnQueue()
                }
            }
        }
        conn.start(queue: netQueue)
    }

    private func acceptStreamConnectionOnQueue(
        conn: NWConnection,
        path: PetPath,
        capturedBox: Phase9BSyncBox<[ObjectIdentifier: Data]>
    ) {
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            if case .ready = st {
                guard let peerHash = capturedBox.read()[ObjectIdentifier(self)],
                      let matchedKey = self.pairedDevicesByHash.keys.first(where: { CryptoIdentity.constantTimeEqual($0, peerHash) }),
                      let rec = self.pairedDevicesByHash[matchedKey] else {
                    conn.cancel()
                    return
                }
                self.registerActiveSessionOnQueue(conn: conn, record: rec, key: matchedKey, path: path)
            }
        }
        conn.start(queue: netQueue)
    }

    private func registerActiveSessionOnQueue(conn: NWConnection, record: PairedClientRecord, key: Data, path: PetPath) {
        for (oldId, oldSess) in sessions where oldSess.deviceId == record.deviceId {
            oldSess.conn.cancel()
            sessions.removeValue(forKey: oldId)
        }
        let session = SessionEntry(conn: conn, certSHA256: key, deviceId: record.deviceId, path: path)
        sessions[session.id] = session
        sendHelloOnQueue(session: session)
        receiveSessionLoopOnQueue(session: session)
    }

    private func sendHelloOnQueue(session: SessionEntry) {
        guard let rec = pairedDevicesByHash[session.certSHA256] else { return }
        let effectiveCameraRoll = cameraRollShipped && !folderUnavailable
        let petHello = PetCameraHello(
            specVersion: 1,
            maxWatchers: 4,
            cameraRoll: effectiveCameraRoll,
            outside: outsideShipped,
            qualities: ["auto", "high", "medium", "low"]
        )
        let hello = HelloServerMessage(
            protocolVersion: 3,
            modeRaw: modeRaw,
            sbsLayoutRaw: sbsLayoutRaw,
            allowPhoneModeSwitch: rec.role == .owner ? allowPhoneModeSwitch : false,
            width: 320,
            height: 240,
            codecRaw: WireCodecKind.h264.rawValue,
            petCamera: petHello
        )
        if let pkt = try? WirePacket.packJSON(type: .helloServer, hello) {
            session.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
    }

    private func receiveSessionLoopOnQueue(session: SessionEntry) {
        session.conn.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, isComplete, err in
            guard let self = self, self.sessions[session.id] != nil else { return }
            if let data = data, !data.isEmpty {
                session.reader.append(data)
                while true {
                    switch session.reader.nextFrame() {
                    case .needMoreData:
                        break
                    case .protocolViolation:
                        session.conn.cancel()
                        self.sessions.removeValue(forKey: session.id)
                        return
                    case .unknownFrame:
                        continue
                    case .frame(let msgType, _, let payload):
                        if WireHeader.size + payload.count > WireHeader.maxClientFrameBytes {
                            session.conn.cancel()
                            self.sessions.removeValue(forKey: session.id)
                            return
                        }
                        self.handleClientFrameOnQueue(session: session, type: msgType, payload: payload)
                        continue
                    }
                    break
                }
            }
            if isComplete || err != nil {
                session.conn.cancel()
                self.sessions.removeValue(forKey: session.id)
                return
            }
            self.receiveSessionLoopOnQueue(session: session)
        }
    }

    // MARK: - Client Frame Handling (`0x04`, `0x0C`, `0x0E`, `0x12`, `0x14`, `0x16`)

    private func handleClientFrameOnQueue(session: SessionEntry, type: WireMessageType, payload: Data) {
        switch type {
        case .ping:
            guard let ping = try? JSONDecoder().decode(PingMessage.self, from: payload) else { return }
            let pong = PongMessage(clientSendNs: ping.clientSendNs, serverRecvNs: ping.clientSendNs + 1000, serverSendNs: ping.clientSendNs + 2000)
            if let pkt = try? WirePacket.packJSON(type: .pong, pong) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
            if session.negotiatedVersion == 1, let v = ping.protocolVersion, v >= 2 {
                session.negotiatedVersion = min(v, 3)
                sendHostStateAndPetStateOnQueue(session: session)
            }

        case .liveViewReq:
            guard session.negotiatedVersion >= 3,
                  let req = try? JSONDecoder().decode(LiveViewRequestMessage.self, from: payload) else { return }
            metricsBox.update { $0.receivedLiveViewReqs.append(req) }
            session.liveWatching = req.watch
            if let qRaw = req.quality, let q = LiveQuality(rawValue: qRaw), q != .unknown {
                session.liveQuality = q
            }
            broadcastAllSessionsOnQueue()

        case .petCameraCmd:
            guard session.negotiatedVersion >= 3,
                  let cmd = try? JSONDecoder().decode(PetCameraCommandMessage.self, from: payload),
                  let rec = pairedDevicesByHash[session.certSHA256] else { return }
            metricsBox.update { $0.receivedPetCommands.append(cmd) }
            handlePetCommandOnQueue(session: session, caller: rec, cmd: cmd)

        case .rollListReq:
            guard session.negotiatedVersion >= 3,
                  let req = try? JSONDecoder().decode(RollListRequestMessage.self, from: payload),
                  let rec = pairedDevicesByHash[session.certSHA256] else { return }
            metricsBox.update { $0.receivedRollListReqs.append(req) }
            handleRollListRequestOnQueue(session: session, caller: rec, req: req)

        case .rollThumbReq:
            guard session.negotiatedVersion >= 3,
                  let req = try? JSONDecoder().decode(RollThumbRequestMessage.self, from: payload),
                  let rec = pairedDevicesByHash[session.certSHA256] else { return }
            metricsBox.update { $0.receivedRollThumbReqs.append(req) }
            handleRollThumbRequestOnQueue(session: session, caller: rec, req: req)

        case .playbackReq:
            guard session.negotiatedVersion >= 3,
                  let req = try? JSONDecoder().decode(PlaybackRequestMessage.self, from: payload),
                  let rec = pairedDevicesByHash[session.certSHA256] else { return }
            metricsBox.update { $0.receivedPlaybackReqs.append(req) }
            handlePlaybackRequestOnQueue(session: session, caller: rec, req: req)

        default:
            break
        }
    }

    private func computeRollBlockedReasonAndText() -> (code: String?, text: String?) {
        if let custom = customBlockedReasonOverride {
            return (custom.code, custom.text)
        }
        // Exact §5.1 priority order:
        // privacy_mode, roll_disabled, roll_preparing, folder_unavailable, sensor_off, view_frozen, pc_replaying, disk_floor, storage_full
        if privacyMode { return ("privacy_mode", PetCameraUserStrings.privacyModeSentence) }
        if folderUnavailable { return ("folder_unavailable", PetCameraUserStrings.folderUnavailableSentence) }
        if !rollEnabled { return ("roll_disabled", PetCameraUserStrings.rollDisabledSentence) }
        if rollPreparing { return ("roll_preparing", PetCameraUserStrings.rollPreparingSentence) }
        if powerState != .on { return ("sensor_off", PetCameraUserStrings.sensorOffSentence) }
        if isHostFrozen { return ("view_frozen", PetCameraUserStrings.viewFrozenSentence) }
        if isReplayingOnHost { return ("pc_replaying", PetCameraUserStrings.pcReplayingSentence) }
        if diskFloorReached { return ("disk_floor", PetCameraUserStrings.diskFloorSentence) }
        if storageFullReached { return ("storage_full", PetCameraUserStrings.storageFullSentence) }
        return (nil, nil)
    }

    private func computeRollAccessError(for caller: PairedClientRecord) -> (code: String, text: String)? {
        if !cameraRollShipped || folderUnavailable {
            return ("unsupported", PetCameraUserStrings.folderUnavailableSentence)
        }
        if caller.role != .owner {
            if isReplayingOnHost { return ("pc_replaying", PetCameraUserStrings.pcReplayingSentence) }
            if !sharingEnabled { return ("sharing_off", PetCameraUserStrings.sharingOffSentence) }
            if sharingScheduleOff { return ("sharing_schedule", PetCameraUserStrings.sharingScheduleSentence) }
        }
        return nil
    }

    private func handlePetCommandOnQueue(session: SessionEntry, caller: PairedClientRecord, cmd: PetCameraCommandMessage) {
        switch cmd.action {
        case "set_roll":
            guard caller.role == .owner else {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "not_allowed_for_role", message: "Only an owner can change recording settings.")
                return
            }
            if let en = cmd.enabled { rollEnabled = en }
            sendCommandOkOnQueue(session: session, requestId: cmd.requestId)
            broadcastAllSessionsOnQueue()

        case "set_motion":
            guard caller.role == .owner else {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "not_allowed_for_role", message: "Only an owner can change motion settings.")
                return
            }
            if let s = cmd.sensitivity, !(1...10).contains(s) {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "invalid", message: "Invalid sensitivity.")
                return
            }
            if let en = cmd.enabled { motionEnabled = en }
            if let s = cmd.sensitivity { motionSensitivity = s }
            sendCommandOkOnQueue(session: session, requestId: cmd.requestId)
            broadcastAllSessionsOnQueue()

        case "set_privacy":
            guard caller.role == .owner else {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "not_allowed_for_role", message: "Only an owner can change privacy mode.")
                return
            }
            sendCommandOkOnQueue(session: session, requestId: cmd.requestId)
            if let en = cmd.enabled {
                let turningOn = (en && !privacyMode)
                privacyMode = en
                if turningOn {
                    for (sid, s) in sessions {
                        if let pb = s.activePlayback {
                            sessions[sid]?.activePlayback = nil
                            let stopMsg = PlaybackStateMessage(
                                requestId: nil,
                                streamId: pb.streamId,
                                clipId: pb.clipId,
                                state: .stopped,
                                positionSec: pb.positionSec,
                                durationSec: pb.durationSec,
                                message: PetCameraUserStrings.privacyModeSentence
                            )
                            if let pkt = try? WirePacket.packJSON(type: .playbackState, stopMsg) {
                                s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                            }
                        }
                    }
                }
                broadcastAllSessionsOnQueue()
            }

        case "start_clip":
            // §5.2: Privacy first, then viewer roll access (`pc_replaying`, `sharing_off`, `sharing_schedule`), then §5.1 blocked order!
            if privacyMode {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "privacy_mode", message: PetCameraUserStrings.privacyModeSentence)
                return
            }
            if caller.role != .owner {
                if isReplayingOnHost {
                    sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "pc_replaying", message: PetCameraUserStrings.pcReplayingSentence)
                    return
                }
                if !sharingEnabled {
                    sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "sharing_off", message: PetCameraUserStrings.sharingOffSentence)
                    return
                }
                if sharingScheduleOff {
                    sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "sharing_schedule", message: PetCameraUserStrings.sharingScheduleSentence)
                    return
                }
            }
            let (blockedCode, blockedText) = computeRollBlockedReasonAndText()
            if let code = blockedCode {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: code, message: blockedText ?? code)
                return
            }
            if !clipRecording {
                clipRecording = true
                clipTrigger = .manual
                clipElapsedSec = 0.0
            }
            clipManualActive = true
            sendCommandOkOnQueue(session: session, requestId: cmd.requestId)
            broadcastAllSessionsOnQueue()

        case "stop_clip":
            // §5.2: `ok` when manual active AND `ok` no-op when nothing is recording; `busy` ONLY when motion/schedule records with no manual trigger!
            if !clipRecording {
                sendCommandOkOnQueue(session: session, requestId: cmd.requestId)
                return
            }
            guard clipManualActive else {
                sendCommandErrorOnQueue(
                    session: session,
                    requestId: cmd.requestId,
                    code: "busy",
                    message: "A motion or scheduled clip is recording and can't be stopped manually."
                )
                return
            }
            clipManualActive = false
            if clipTrigger == .manual {
                clipRecording = false
                clipTrigger = nil
                clipElapsedSec = 0.0
            }
            sendCommandOkOnQueue(session: session, requestId: cmd.requestId)
            broadcastAllSessionsOnQueue()

        case "star_clip":
            if let accessErr = computeRollAccessError(for: caller) {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: accessErr.code, message: accessErr.text)
                return
            }
            guard let cid = cmd.clipId, let starred = cmd.starred,
                  let idx = clipsStore.firstIndex(where: { $0.clip.clipId == cid }) else {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "not_found", message: "Clip not found.")
                return
            }
            if starred {
                clipsStore[idx].starredByDeviceIds.insert(caller.deviceId)
            } else {
                if caller.role == .owner {
                    clipsStore[idx].starredByDeviceIds.removeAll()
                } else {
                    guard clipsStore[idx].starredByDeviceIds.contains(caller.deviceId) else {
                        sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "not_allowed_for_role", message: "Only the person who starred this clip or an owner can remove the star.")
                        return
                    }
                    clipsStore[idx].starredByDeviceIds.remove(caller.deviceId)
                }
            }
            clipsStore[idx].clip.starred = !clipsStore[idx].starredByDeviceIds.isEmpty
            sendCommandOkOnQueue(session: session, requestId: cmd.requestId)

        case "delete_clip":
            guard caller.role == .owner else {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "not_allowed_for_role", message: "Only an owner can delete clips.")
                return
            }
            guard let cid = cmd.clipId,
                  let idx = clipsStore.firstIndex(where: { $0.clip.clipId == cid }) else {
                sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "not_found", message: "Clip not found.")
                return
            }
            // §5.2: A playing clip is stopped first (`0x17 stopped`, "This clip was deleted.").
            for (sid, s) in sessions {
                if let pb = s.activePlayback, pb.clipId == cid {
                    sessions[sid]?.activePlayback = nil
                    let stopMsg = PlaybackStateMessage(
                        requestId: nil,
                        streamId: pb.streamId,
                        clipId: pb.clipId,
                        state: .stopped,
                        positionSec: pb.positionSec,
                        durationSec: pb.durationSec,
                        message: PetCameraUserStrings.playbackClipDeletedSentence
                    )
                    if let pkt = try? WirePacket.packJSON(type: .playbackState, stopMsg) {
                        s.conn.send(content: pkt, completion: .contentProcessed { _ in })
                    }
                }
            }
            clipsStore.remove(at: idx)
            sendCommandOkOnQueue(session: session, requestId: cmd.requestId)
            broadcastAllSessionsOnQueue()

        default:
            sendCommandErrorOnQueue(session: session, requestId: cmd.requestId, code: "unsupported", message: "Unsupported action.")
        }
    }

    private func handleRollListRequestOnQueue(session: SessionEntry, caller: PairedClientRecord, req: RollListRequestMessage) {
        if let accessErr = computeRollAccessError(for: caller) {
            sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: accessErr.code, message: accessErr.text)
            return
        }
        let limit = req.limit ?? 50
        guard (1...100).contains(limit) else {
            sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "invalid", message: "Invalid limit.")
            return
        }

        let storage = RollStorage(
            usedBytes: UInt64(clipsStore.count) * 120_000,
            capBytes: 200_000_000_000,
            freeBytes: 80_000_000_000,
            floorBytes: 20_000_000_000,
            keepDays: 30
        )

        if let day = req.day {
            let dayClipsAll = clipsStore
                .filter { $0.day == day }
                .sorted { ($0.clip.startUnixMs, $0.clip.clipId) > ($1.clip.startUnixMs, $1.clip.clipId) }
            guard !dayClipsAll.isEmpty else {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "not_found", message: "Day not found.")
                return
            }

            var filtered = dayClipsAll
            if let cursorB64 = req.cursor {
                guard let decodedData = QRPairingPayload.fromBase64URL(cursorB64),
                      let decodedStr = String(data: decodedData, encoding: .utf8),
                      let colonIdx = decodedStr.firstIndex(of: ":"),
                      let anchorMs = UInt64(decodedStr[..<colonIdx]) else {
                    sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "invalid", message: "Invalid cursor.")
                    return
                }
                let anchorId = String(decodedStr[decodedStr.index(after: colonIdx)...])
                filtered = dayClipsAll.filter { ($0.clip.startUnixMs, $0.clip.clipId) < (anchorMs, anchorId) }
            }

            let pageSlice = Array(filtered.prefix(limit))
            let hasMore = filtered.count > pageSlice.count
            let nextCur: String? = hasMore ? pageSlice.last.map {
                QRPairingPayload.base64URL(Data("\($0.clip.startUnixMs):\($0.clip.clipId)".utf8))
            } : nil

            let personalizedClips: [RollClip] = pageSlice.map { stored in
                var c = stored.clip
                c.starred = !stored.starredByDeviceIds.isEmpty
                c.starredByMe = stored.starredByDeviceIds.contains(caller.deviceId)
                return c
            }
            let resp = RollListMessage(
                requestId: req.requestId,
                day: day,
                days: nil,
                clips: personalizedClips,
                nextCursor: nextCur,
                storage: storage
            )
            if let pkt = try? WirePacket.packJSON(type: .rollList, resp) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        } else {
            // Group by day, newest first
            let grouped = Dictionary(grouping: clipsStore, by: \.day)
            var allDays: [RollDay] = grouped.map { dayKey, list in
                RollDay(
                    day: dayKey,
                    clipCount: list.count,
                    starredCount: list.filter { !$0.starredByDeviceIds.isEmpty }.count,
                    bytes: list.reduce(0) { $0 + $1.clip.bytes }
                )
            }.sorted { $0.day > $1.day }

            if let cursorB64 = req.cursor {
                guard let decodedData = QRPairingPayload.fromBase64URL(cursorB64),
                      let anchorDay = String(data: decodedData, encoding: .utf8),
                      !anchorDay.isEmpty else {
                    sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "invalid", message: "Invalid cursor.")
                    return
                }
                allDays = allDays.filter { $0.day < anchorDay }
            }

            let pageSlice = Array(allDays.prefix(limit))
            let hasMore = allDays.count > pageSlice.count
            let nextCur: String? = hasMore ? pageSlice.last.map {
                QRPairingPayload.base64URL(Data($0.day.utf8))
            } : nil

            let resp = RollListMessage(
                requestId: req.requestId,
                day: nil,
                days: pageSlice,
                clips: nil,
                nextCursor: nextCur,
                storage: storage
            )
            if let pkt = try? WirePacket.packJSON(type: .rollList, resp) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
    }

    private func handleRollThumbRequestOnQueue(session: SessionEntry, caller: PairedClientRecord, req: RollThumbRequestMessage) {
        if let accessErr = computeRollAccessError(for: caller) {
            sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: accessErr.code, message: accessErr.text)
            return
        }
        if privacyMode {
            sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "privacy_mode", message: PetCameraUserStrings.privacyModeSentence)
            return
        }
        if holdPendingThumbnailRequests {
            heldThumbRequests.append((sessionId: session.id, req: req))
            return
        }
        guard let stored = clipsStore.first(where: { $0.clip.clipId == req.clipId }),
              let jpeg = stored.jpegThumbnail else {
            sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "not_found", message: "Thumbnail not found.")
            return
        }
        let resp = RollThumbMessage(requestId: req.requestId, clipId: req.clipId, jpeg: jpeg)
        if let pkt = try? WirePacket.packJSON(type: .rollThumb, resp) {
            session.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
    }

    private func handlePlaybackRequestOnQueue(session: SessionEntry, caller: PairedClientRecord, req: PlaybackRequestMessage) {
        guard let action = PlaybackAction(rawValue: req.action) else {
            sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "unsupported", message: "Unsupported playback action.")
            return
        }

        switch action {
        case .start:
            if let accessErr = computeRollAccessError(for: caller) {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: accessErr.code, message: accessErr.text)
                return
            }
            if privacyMode {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "privacy_mode", message: PetCameraUserStrings.privacyModeSentence)
                return
            }
            guard let clipId = req.clipId,
                  let stored = clipsStore.first(where: { $0.clip.clipId == clipId }) else {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "not_found", message: "Clip not found.")
                return
            }

            // If an existing playback is active on this session, stop the old stream first (no requestId)!
            if let oldPb = session.activePlayback {
                let oldStop = PlaybackStateMessage(
                    requestId: nil,
                    streamId: oldPb.streamId,
                    clipId: oldPb.clipId,
                    state: .stopped,
                    positionSec: oldPb.positionSec,
                    durationSec: oldPb.durationSec,
                    message: nil
                )
                if let pkt = try? WirePacket.packJSON(type: .playbackState, oldStop) {
                    session.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
            }

            // Starting playback stops live watching (`watching` becomes false)!
            session.liveWatching = false
            nextPlaybackStreamId &+= 1
            let startPos = min(max(0.0, req.positionSec ?? 0.0), stored.clip.durationSec)
            var newPb = PlaybackSessionStream(
                streamId: nextPlaybackStreamId,
                clipId: clipId,
                durationSec: stored.clip.durationSec,
                state: .starting,
                positionSec: startPos,
                streamEpoch: 1
            )
            session.activePlayback = newPb
            broadcastAllSessionsOnQueue()

            let startingMsg = PlaybackStateMessage(
                requestId: req.requestId,
                streamId: newPb.streamId,
                clipId: newPb.clipId,
                state: .starting,
                positionSec: newPb.positionSec,
                durationSec: newPb.durationSec,
                message: nil
            )
            if let pkt = try? WirePacket.packJSON(type: .playbackState, startingMsg) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }

            if !holdPlaybackInStartingState {
                newPb.state = .playing
                session.activePlayback = newPb
                sendPlaybackConfigAndFramesOnQueue(session: session, pb: newPb, frameCount: 3)
                let playingMsg = PlaybackStateMessage(
                    requestId: nil,
                    streamId: newPb.streamId,
                    clipId: newPb.clipId,
                    state: .playing,
                    positionSec: newPb.positionSec,
                    durationSec: newPb.durationSec,
                    message: nil
                )
                if let pkt = try? WirePacket.packJSON(type: .playbackState, playingMsg) {
                    session.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
            }

        case .pause:
            guard var pb = session.activePlayback, let reqSid = req.streamId, reqSid == pb.streamId else {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "not_found", message: "Stream not found.")
                return
            }
            // §5.9: `ended` + `pause` -> `ended` (no change); `paused` + `pause` -> `paused`; `starting`/`playing` + `pause` -> `paused`.
            if pb.state != .ended {
                pb.state = .paused
                session.activePlayback = pb
            }
            let reply = PlaybackStateMessage(
                requestId: req.requestId,
                streamId: pb.streamId,
                clipId: pb.clipId,
                state: pb.state,
                positionSec: pb.positionSec,
                durationSec: pb.durationSec,
                message: nil
            )
            if let pkt = try? WirePacket.packJSON(type: .playbackState, reply) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }

        case .resume:
            guard var pb = session.activePlayback, let reqSid = req.streamId, reqSid == pb.streamId else {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "not_found", message: "Stream not found.")
                return
            }
            switch pb.state {
            case .starting:
                // §5.9: `starting` + `resume` stays `starting`, and plays once ready with no pause intent.
                break
            case .playing:
                // `playing` + `resume` stays `playing`.
                break
            case .paused:
                pb.state = .playing
                session.activePlayback = pb
                sendPlaybackFramesOnlyOnQueue(session: session, pb: pb, frameCount: 2)
            case .ended:
                // `ended` + `resume` -> `playing` from 0!
                pb.state = .playing
                pb.positionSec = 0.0
                pb.streamEpoch &+= 1
                session.activePlayback = pb
                sendPlaybackConfigAndFramesOnQueue(session: session, pb: pb, frameCount: 2)
            default:
                break
            }
            let reply = PlaybackStateMessage(
                requestId: req.requestId,
                streamId: pb.streamId,
                clipId: pb.clipId,
                state: pb.state,
                positionSec: pb.positionSec,
                durationSec: pb.durationSec,
                message: nil
            )
            if let pkt = try? WirePacket.packJSON(type: .playbackState, reply) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }

        case .seek:
            guard var pb = session.activePlayback,
                  let reqSid = req.streamId,
                  reqSid == pb.streamId,
                  let targetPos = req.positionSec else {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "not_found", message: "Stream not found.")
                return
            }
            let clamped = min(max(0.0, targetPos), pb.durationSec)
            pb.positionSec = clamped
            pb.streamEpoch &+= 1
            if pb.state == .paused {
                // §5.9: `paused` + `seek` stays `paused`, sends new `0x18` + 1 keyframe!
                session.activePlayback = pb
                sendPlaybackConfigAndFramesOnQueue(session: session, pb: pb, frameCount: 1)
            } else {
                // §5.9: `starting`/`playing`/`ended` + `seek` -> `playing`!
                pb.state = .playing
                session.activePlayback = pb
                sendPlaybackConfigAndFramesOnQueue(session: session, pb: pb, frameCount: 2)
            }
            let reply = PlaybackStateMessage(
                requestId: req.requestId,
                streamId: pb.streamId,
                clipId: pb.clipId,
                state: pb.state,
                positionSec: pb.positionSec,
                durationSec: pb.durationSec,
                message: nil
            )
            if let pkt = try? WirePacket.packJSON(type: .playbackState, reply) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }

        case .stop:
            guard let pb = session.activePlayback, let reqSid = req.streamId, reqSid == pb.streamId else {
                sendCommandErrorOnQueue(session: session, requestId: req.requestId, code: "not_found", message: "Stream not found.")
                return
            }
            session.activePlayback = nil
            let reply = PlaybackStateMessage(
                requestId: req.requestId,
                streamId: pb.streamId,
                clipId: pb.clipId,
                state: .stopped,
                positionSec: pb.positionSec,
                durationSec: pb.durationSec,
                message: nil
            )
            if let pkt = try? WirePacket.packJSON(type: .playbackState, reply) {
                session.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
            broadcastAllSessionsOnQueue()
        }
    }

    private func sendPlaybackConfigAndFramesOnQueue(session: SessionEntry, pb: PlaybackSessionStream, frameCount: Int) {
        let cfg = PlaybackConfigMessage(
            streamId: pb.streamId,
            clipId: pb.clipId,
            durationSec: pb.durationSec,
            streamEpoch: pb.streamEpoch,
            codecRaw: WireCodecKind.h264.rawValue,
            width: 320,
            height: 240,
            modeRaw: 1,
            sbsLayoutRaw: 0,
            nalLengthHeaderBytes: 4,
            parameterSets: syntheticClipParameterSets
        )
        if let cfgPkt = try? WirePacket.packJSON(type: .playbackConfig, cfg) {
            session.conn.send(content: cfgPkt, completion: .contentProcessed { _ in })
        }
        sendPlaybackFramesOnlyOnQueue(session: session, pb: pb, frameCount: frameCount)
    }

    private func sendPlaybackFramesOnlyOnQueue(session: SessionEntry, pb: PlaybackSessionStream, frameCount: Int) {
        let baseNs = UInt64(max(0.0, pb.positionSec) * 1_000_000_000.0)
        for i in 0..<min(frameCount, syntheticClipAvccPayloads.count) {
            let avcc = syntheticClipAvccPayloads[i]
            let hdr = VideoAccessUnitHeader(
                streamEpoch: pb.streamEpoch,
                frameSeq: UInt64(i),
                hostCaptureNs: baseNs + UInt64(i) * 33_333_333,
                hostEncodeDoneNs: 0,
                flags: 0x01
            )
            let au = PlaybackAccessUnit(streamId: pb.streamId, header: hdr, avccPayload: avcc)
            let pkt = WirePacket.pack(type: .playbackAccessUnit, payload: au.encode())
            session.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
    }

    // MARK: - State Broadcast (`0x07` & `0x0B`)

    private func sendCommandOkOnQueue(session: SessionEntry, requestId: UInt64) {
        let msg = CommandResultMessage(requestId: requestId, ok: true, error: nil, message: nil)
        if let pkt = try? WirePacket.packJSON(type: .commandResult, msg) {
            session.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
    }

    private func sendCommandErrorOnQueue(session: SessionEntry, requestId: UInt64, code: String, message: String) {
        let msg = CommandResultMessage(requestId: requestId, ok: false, error: code, message: message)
        if let pkt = try? WirePacket.packJSON(type: .commandResult, msg) {
            session.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
    }

    private func broadcastAllSessionsOnQueue() {
        for (_, s) in sessions where s.negotiatedVersion >= 2 {
            sendHostStateAndPetStateOnQueue(session: s)
        }
    }

    private func sendHostStateAndPetStateOnQueue(session: SessionEntry) {
        guard let rec = pairedDevicesByHash[session.certSHA256] else { return }
        stateSeq &+= 1
        let seq = stateSeq
        let isOwner = (rec.role == .owner)

        var liveUnavail: LiveUnavailableReason? = nil
        if privacyMode {
            liveUnavail = .privacyMode
        } else if !isOwner && isReplayingOnHost {
            liveUnavail = .pcReplaying
        } else if !isOwner && !sharingEnabled {
            liveUnavail = .sharingOff
        } else if !isOwner && sharingScheduleOff {
            liveUnavail = .sharingSchedule
        }
        let liveAllowed = (liveUnavail == nil)
        let watching = liveAllowed && session.liveWatching && session.activePlayback == nil && powerState == .on

        let hs = HostStateSyncMessage(
            protocolVersion: 3,
            stateSeq: seq,
            powerState: powerState,
            inUseHolder: nil,
            errorMessage: nil,
            modeRaw: modeRaw,
            sbsLayoutRaw: sbsLayoutRaw,
            allowPhoneModeSwitch: isOwner ? allowPhoneModeSwitch : false,
            allowPhoneSensorPower: isOwner ? allowPhoneSensorPower : false,
            sensorPowerDisabledReason: isOwner ? nil : "Only an owner can turn the camera on or off.",
            cutoffEnabled: false,
            cutoffNearMm: 500,
            cutoffFarMm: 2000,
            isHostFrozen: isHostFrozen,
            isRecording: false,
            canRecordCurrentMode: isOwner,
            recordingDisabledReason: isOwner ? nil : "Only an owner can record.",
            recordingReminderNeeded: false,
            recordingFrames: 0,
            recordingElapsedSec: 0,
            isReplayingOnHost: isReplayingOnHost
        )
        if let pkt = try? WirePacket.packJSON(type: .hostStateSync, hs) {
            session.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }

        guard session.negotiatedVersion >= 3 else { return }
        let rollAvailable = cameraRollShipped && !folderUnavailable && (isOwner || (sharingEnabled && !sharingScheduleOff && !isReplayingOnHost))
        let playbackAvailable = rollAvailable && !privacyMode
        let (rollBlockedReason, rollBlockedText) = cameraRollShipped ? computeRollBlockedReasonAndText() : (nil, nil)

        let ps = PetCameraStateMessage(
            protocolVersion: 3,
            stateSeq: seq,
            deviceId: rec.deviceId,
            deviceName: rec.name,
            role: rec.role,
            path: session.path,
            sharingEnabled: sharingEnabled,
            sharingScheduleOff: sharingScheduleOff,
            outsideEnabled: outsideEnabled,
            privacyMode: privacyMode,
            liveAllowed: liveAllowed,
            liveUnavailableReason: liveUnavail,
            watching: watching,
            liveQuality: session.liveQuality,
            liveTier: session.liveTier,
            rollAvailable: rollAvailable,
            playbackAvailable: playbackAvailable,
            rollEnabled: cameraRollShipped ? rollEnabled : false,
            motionEnabled: cameraRollShipped ? motionEnabled : false,
            motionAvailable: cameraRollShipped ? (powerState == .on && !isReplayingOnHost) : false,
            motionSensitivity: motionSensitivity,
            scheduleEnabled: cameraRollShipped ? scheduleEnabled : false,
            scheduleSummary: cameraRollShipped ? scheduleSummary : nil,
            clipRecording: cameraRollShipped ? clipRecording : false,
            clipTrigger: cameraRollShipped ? clipTrigger : nil,
            clipManualActive: cameraRollShipped ? clipManualActive : false,
            clipElapsedSec: cameraRollShipped ? clipElapsedSec : 0,
            rollBlockedReason: rollBlockedReason,
            rollBlockedText: rollBlockedText,
            outsideEndpoint: outsideEnabled ? outsideEndpointAdvertised : nil,
            outsideCandidates: outsideEnabled ? outsideCandidatesAdvertised : nil,
            outsideStatus: isOwner ? outsideStatus : nil,
            outsideStatusText: nil,
            watchingNow: isOwner ? [] : nil,
            devices: isOwner ? [] : nil
        )
        if let pkt = try? WirePacket.packJSON(type: .petCameraState, ps) {
            session.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
    }
}

@MainActor
final class Phase9BCameraRollAndInvitesTests: XCTestCase {

    private func waitUntil(
        timeout: TimeInterval = 4.0,
        pollMs: UInt64 = 15,
        _ predicate: @escaping @MainActor () -> Bool
    ) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() {
                return true
            }
            try? await Task.sleep(nanoseconds: pollMs * 1_000_000)
        }
        return predicate()
    }

    private func pairClient(
        _ client: PocketStreamClientCore,
        qrURI: String
    ) async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            client.pairWithQRURI(qrURI) { res in
                cont.resume(with: res)
            }
        }
    }

    // MARK: - 1. Listing, Cursor Paging, `roll_preparing`, `folder_unavailable` & Unknown Blocked Fallback

    func testCameraRollListingPaginationAndBlockedReasonSentences() async throws {
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }

        // Seed 4 days and 5 clips on the newest day (`2026-11-19`) to test cursor paging on both days and clips.
        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-05-00_manual", startUnixMs: 1_795_111_500_000, startLocal: "2026-11-19T18:05:00+00:00", durationSec: 12.0, trigger: .manual, startedBy: "Owner's iPhone", starredByIds: [], hasThumb: true),
            (day: "2026-11-19", clipId: "2026-11-19/17-40-10_motion", startUnixMs: 1_795_110_010_000, startLocal: "2026-11-19T17:40:10+00:00", durationSec: 15.0, trigger: .motion, startedBy: nil, starredByIds: [], hasThumb: true),
            (day: "2026-11-19", clipId: "2026-11-19/16-20-00_schedule", startUnixMs: 1_795_105_200_000, startLocal: "2026-11-19T16:20:00+00:00", durationSec: 60.0, trigger: .schedule, startedBy: nil, starredByIds: [], hasThumb: true),
            (day: "2026-11-19", clipId: "2026-11-19/14-10-00_motion", startUnixMs: 1_795_097_400_000, startLocal: "2026-11-19T14:10:00+00:00", durationSec: 18.0, trigger: .motion, startedBy: nil, starredByIds: [], hasThumb: true),
            (day: "2026-11-19", clipId: "2026-11-19/09-00-00_manual", startUnixMs: 1_795_078_800_000, startLocal: "2026-11-19T09:00:00+00:00", durationSec: 8.0, trigger: .manual, startedBy: "This PC", starredByIds: [], hasThumb: true),
            (day: "2026-11-18", clipId: "2026-11-18/20-00-00_motion", startUnixMs: 1_795_032_000_000, startLocal: "2026-11-18T20:00:00+00:00", durationSec: 14.0, trigger: .motion, startedBy: nil, starredByIds: [], hasThumb: true),
            (day: "2026-11-17", clipId: "2026-11-17/19-00-00_motion", startUnixMs: 1_794_942_000_000, startLocal: "2026-11-17T19:00:00+00:00", durationSec: 11.0, trigger: .motion, startedBy: nil, starredByIds: [], hasThumb: true),
            (day: "2026-11-16", clipId: "2026-11-16/18-00-00_schedule", startUnixMs: 1_794_852_000_000, startLocal: "2026-11-16T18:00:00+00:00", durationSec: 30.0, trigger: .schedule, startedBy: nil, starredByIds: [], hasThumb: true)
        ])

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Owner Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        try await pairClient(client, qrURI: host.prepareLanPairingOffer(role: .owner))
        let ready = await waitUntil { vm.isPetOwner && vm.showCameraRollButton }
        XCTAssertTrue(ready)

        // Page with `pageLimit = 2`
        vm.cameraRoll.pageLimit = 2
        await vm.cameraRoll.refreshDays(force: true)

        XCTAssertEqual(vm.cameraRoll.days.map(\.day), ["2026-11-19", "2026-11-18"])
        XCTAssertNotNil(vm.cameraRoll.daysNextCursor)
        XCTAssertEqual(vm.cameraRoll.selectedDay, "2026-11-19")
        XCTAssertEqual(vm.cameraRoll.selectedDayClips.count, 2)
        XCTAssertEqual(vm.cameraRoll.selectedDayClips.map(\.clipId), [
            "2026-11-19/18-05-00_manual",
            "2026-11-19/17-40-10_motion"
        ])
        XCTAssertNotNil(vm.cameraRoll.selectedDayNextCursor)

        // Load next page of days
        await vm.cameraRoll.loadMoreDays()
        XCTAssertEqual(vm.cameraRoll.days.map(\.day), ["2026-11-19", "2026-11-18", "2026-11-17", "2026-11-16"])
        XCTAssertNil(vm.cameraRoll.daysNextCursor)

        // Load next two pages of clips on 2026-11-19
        await vm.cameraRoll.loadMoreClipsForSelectedDay()
        XCTAssertEqual(vm.cameraRoll.selectedDayClips.count, 4)
        XCTAssertNotNil(vm.cameraRoll.selectedDayNextCursor)

        await vm.cameraRoll.loadMoreClipsForSelectedDay()
        XCTAssertEqual(vm.cameraRoll.selectedDayClips.count, 5)
        XCTAssertNil(vm.cameraRoll.selectedDayNextCursor)
        XCTAssertEqual(vm.cameraRoll.storage?.keepDays, 30)

        // `roll_preparing`: listing still works, blocked sentence shows exact §9 text, and `start_clip` is refused.
        host.configureState(rollPreparing: true)
        let sawPreparing = await waitUntil {
            vm.cameraRoll.rollBlockedSentence == "The camera roll is getting ready."
        }
        XCTAssertTrue(sawPreparing)
        await vm.cameraRoll.refreshDays(force: true)
        XCTAssertEqual(vm.cameraRoll.days.first?.day, "2026-11-19")
        vm.cameraRoll.startManualClip()
        let sawStartPreparingRefusal = await waitUntil {
            vm.cameraRoll.rollCommandError == "The camera roll is getting ready."
        }
        XCTAssertTrue(sawStartPreparingRefusal)

        // Unknown `rollBlockedReason` falls back to `rollBlockedText` from `0x0B`.
        host.configureState(
            rollPreparing: false,
            customBlockedReason: (code: "future_thermal_guard", text: "Recording paused while the sensor cools down.")
        )
        let sawUnknownFallback = await waitUntil {
            vm.cameraRoll.rollBlockedSentence == "Recording paused while the sensor cools down."
        }
        XCTAssertTrue(sawUnknownFallback)

        // `folder_unavailable`: `cameraRoll` and `rollAvailable` become false, exact §9 sentence is shown, and 0x12/0x14/0x16 answer `unsupported`.
        host.configureState(
            folderUnavailable: true,
            customBlockedReason: .some(nil)
        )
        let sawFolderUnavail = await waitUntil {
            vm.cameraRoll.supportsCameraRoll &&
            vm.cameraRoll.rollBlockedSentence == "The camera roll folder on the PC can't be used." &&
            vm.cameraRoll.rollUnavailableBrowseSentence == "The camera roll folder on the PC can't be used."
        }
        XCTAssertTrue(sawFolderUnavail)

        let rawListRes: Result<RollListMessage, CommandResultError> = await withCheckedContinuation { cont in
            client.requestRollList { cont.resume(returning: $0) }
        }
        if case .failure(let err) = rawListRes {
            XCTAssertEqual(err.code, "unsupported")
        } else {
            XCTFail("Expected 0x12 to fail with unsupported during folder_unavailable")
        }
    }

    // MARK: - 2. §5.9 Playback State Machine, 60 s Idle Stop, Privacy Stop & Live Resume

    func testPlaybackStateMachineEveryRowAndIdle60sStop() async throws {
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }

        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-05-00_manual", startUnixMs: 1_795_111_500_000, startLocal: "2026-11-19T18:05:00+00:00", durationSec: 12.0, trigger: .manual, startedBy: "Owner", starredByIds: [], hasThumb: true),
            (day: "2026-11-19", clipId: "2026-11-19/17-00-00_motion", startUnixMs: 1_795_107_600_000, startLocal: "2026-11-19T17:00:00+00:00", durationSec: 15.0, trigger: .motion, startedBy: nil, starredByIds: [], hasThumb: true)
        ])

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Owner Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        try await pairClient(client, qrURI: host.prepareLanPairingOffer(role: .owner))
        let connected = await waitUntil { vm.isPetOwner && vm.telemetry.petState.watching }
        XCTAssertTrue(connected)
        await vm.cameraRoll.refreshDays(force: true)
        let clipA = try XCTUnwrap(vm.cameraRoll.selectedDayClips.first)
        let clipB = try XCTUnwrap(vm.cameraRoll.selectedDayClips.last)

        // --- Row 1 (`none`): pause, resume, seek, stop all answer `not_found` ---
        for action in [PlaybackAction.pause, .resume, .seek, .stop] {
            let res: Result<PlaybackStateMessage, CommandResultError> = await withCheckedContinuation { cont in
                client.sendPlaybackRequest(action: action, streamId: 999, positionSec: action == .seek ? 1.0 : nil) {
                    cont.resume(returning: $0)
                }
            }
            guard case .failure(let err) = res else {
                XCTFail("Expected not_found for \(action) in state none")
                return
            }
            XCTAssertEqual(err.code, "not_found")
        }

        // --- Row 2a (`starting` + `resume` stays `starting`, then plays once ready) ---
        host.setHoldPlaybackInStartingState(true)
        vm.cameraRoll.startPlayback(clip: clipA)
        let reachedStarting = await waitUntil {
            vm.cameraRoll.playbackState?.state == .starting && (vm.cameraRoll.playbackState?.streamId ?? 0) > 0
        }
        XCTAssertTrue(reachedStarting)
        XCTAssertFalse(vm.telemetry.petState.watching, "Starting playback must stop live watching")

        let sid1 = try XCTUnwrap(vm.cameraRoll.playbackState?.streamId)
        let resumeWhileStarting: Result<PlaybackStateMessage, CommandResultError> = await withCheckedContinuation { cont in
            client.sendPlaybackRequest(action: .resume, streamId: sid1) { cont.resume(returning: $0) }
        }
        XCTAssertEqual(try resumeWhileStarting.get().state, .starting)

        let liveFramesBeforePlayback = vm.telemetry.receivedFrames
        host.releaseStartingPlaybacksToPlaying()
        let reachedPlaying = await waitUntil {
            vm.cameraRoll.playbackState?.state == .playing && vm.telemetry.decodedPlaybackFrames >= 2
        }
        XCTAssertTrue(reachedPlaying)
        XCTAssertEqual(vm.telemetry.receivedFrames, liveFramesBeforePlayback, "0x19 playback frames must NEVER increment live receivedFrames telemetry")

        // --- Row 2b (`playing` + `resume` stays `playing`; `playing` + `seek` -> `playing` with new epoch; stale epoch dropped) ---
        let resumeWhilePlaying: Result<PlaybackStateMessage, CommandResultError> = await withCheckedContinuation { cont in
            client.sendPlaybackRequest(action: .resume, streamId: sid1) { cont.resume(returning: $0) }
        }
        XCTAssertEqual(try resumeWhilePlaying.get().state, .playing)

        vm.cameraRoll.seekPlayback(to: 4.0)
        let soughtPlaying = await waitUntil {
            vm.cameraRoll.playbackState?.state == .playing && abs((vm.cameraRoll.playbackState?.positionSec ?? 0) - 4.0) < 0.01
        }
        XCTAssertTrue(soughtPlaying)

        let decodedBeforeStale = vm.telemetry.decodedPlaybackFrames
        host.injectStaleEpochPlaybackFrame(streamId: sid1, staleEpoch: 1)
        try? await Task.sleep(nanoseconds: 40_000_000)
        XCTAssertEqual(vm.telemetry.decodedPlaybackFrames, decodedBeforeStale, "Old streamEpoch frames arriving after a seek must be dropped")

        // --- Row 2c (`playing` + `start` replaces old stream with a new streamId) ---
        vm.cameraRoll.startPlayback(clip: clipB)
        let replacedPlaying = await waitUntil {
            vm.cameraRoll.playbackState?.clipId == clipB.clipId &&
            vm.cameraRoll.playbackState?.state == .playing &&
            (vm.cameraRoll.playbackState?.streamId ?? 0) > sid1
        }
        XCTAssertTrue(replacedPlaying)
        let sid2 = try XCTUnwrap(vm.cameraRoll.playbackState?.streamId)

        // --- Row 2d & Row 3 (`playing` + `pause` -> `paused`; `paused` + `pause` -> `paused`; `paused` + `seek` stays `paused` + 1 keyframe; `paused` + `resume` -> `playing`) ---
        vm.cameraRoll.togglePlayPause()
        let isPaused = await waitUntil { vm.cameraRoll.playbackState?.state == .paused }
        XCTAssertTrue(isPaused)

        let pauseAgain: Result<PlaybackStateMessage, CommandResultError> = await withCheckedContinuation { cont in
            client.sendPlaybackRequest(action: .pause, streamId: sid2) { cont.resume(returning: $0) }
        }
        XCTAssertEqual(try pauseAgain.get().state, .paused)

        let decodedBeforePausedSeek = vm.telemetry.decodedPlaybackFrames
        vm.cameraRoll.seekPlayback(to: 6.0)
        let pausedAfterSeek = await waitUntil {
            vm.cameraRoll.playbackState?.state == .paused &&
            abs((vm.cameraRoll.playbackState?.positionSec ?? 0) - 6.0) < 0.01 &&
            vm.telemetry.decodedPlaybackFrames == decodedBeforePausedSeek + 1
        }
        XCTAssertTrue(pausedAfterSeek)

        vm.cameraRoll.togglePlayPause()
        let resumedFromPause = await waitUntil { vm.cameraRoll.playbackState?.state == .playing }
        XCTAssertTrue(resumedFromPause)

        // --- Row 4 (`ended`: `pause` -> `ended`, `resume` -> `playing` from 0, `seek` -> `playing`, `stop` -> `stopped`) ---
        host.simulatePlaybackReachedEnd()
        let reachedEnded = await waitUntil { vm.cameraRoll.playbackState?.state == .ended }
        XCTAssertTrue(reachedEnded)

        let pauseOnEnded: Result<PlaybackStateMessage, CommandResultError> = await withCheckedContinuation { cont in
            client.sendPlaybackRequest(action: .pause, streamId: sid2) { cont.resume(returning: $0) }
        }
        XCTAssertEqual(try pauseOnEnded.get().state, .ended)

        vm.cameraRoll.togglePlayPause() // resume from ended -> playing from 0
        let resumedFromEnded = await waitUntil {
            vm.cameraRoll.playbackState?.state == .playing && (vm.cameraRoll.playbackState?.positionSec ?? 99) == 0.0
        }
        XCTAssertTrue(resumedFromEnded)

        host.simulatePlaybackReachedEnd()
        let endedAgain = await waitUntil { vm.cameraRoll.playbackState?.state == .ended }
        XCTAssertTrue(endedAgain)

        vm.cameraRoll.seekPlayback(to: 5.0) // ended + seek -> playing!
        let seekFromEnded = await waitUntil {
            vm.cameraRoll.playbackState?.state == .playing && abs((vm.cameraRoll.playbackState?.positionSec ?? 0) - 5.0) < 0.01
        }
        XCTAssertTrue(seekFromEnded)

        // --- 60 s Idle Stop (`paused`/`ended` -> unsolicited `0x17 stopped` with sentence & Play Again) ---
        vm.cameraRoll.togglePlayPause()
        let pausedForIdle = await waitUntil { vm.cameraRoll.playbackState?.state == .paused }
        XCTAssertTrue(pausedForIdle)

        host.simulatePlaybackIdle60sStop()
        let idleStopped = await waitUntil {
            vm.cameraRoll.playbackState?.state == .stopped &&
            vm.cameraRoll.playbackNoticeMessage == "Playback stopped after a minute without a command." &&
            vm.cameraRoll.canOfferPlayAgain
        }
        XCTAssertTrue(idleStopped)

        // Freed stream answers `not_found` on further commands, while Play Again starts a fresh stream!
        let actionAfterIdleFree: Result<PlaybackStateMessage, CommandResultError> = await withCheckedContinuation { cont in
            client.sendPlaybackRequest(action: .resume, streamId: sid2) { cont.resume(returning: $0) }
        }
        if case .failure(let err) = actionAfterIdleFree {
            XCTAssertEqual(err.code, "not_found")
        } else {
            XCTFail("Expected freed idle stream to answer not_found")
        }

        vm.cameraRoll.playAgain()
        let playingAgain = await waitUntil {
            vm.cameraRoll.playbackState?.state == .playing && (vm.cameraRoll.playbackState?.streamId ?? 0) > sid2
        }
        XCTAssertTrue(playingAgain)

        // --- §9 Read Error & Privacy Mode Mid-Playback & Close Resuming Live (`0x0E watch: true`) ---
        host.simulatePlaybackReadError()
        let sawReadError = await waitUntil {
            vm.cameraRoll.playbackState?.state == .error &&
            vm.cameraRoll.playbackNoticeMessage == "The PC couldn't read this clip." &&
            vm.cameraRoll.canOfferPlayAgain
        }
        XCTAssertTrue(sawReadError)

        vm.cameraRoll.playAgain()
        let playingBeforePrivacy = await waitUntil { vm.cameraRoll.playbackState?.state == .playing }
        XCTAssertTrue(playingBeforePrivacy)

        host.configureState(privacyMode: true)
        let stoppedByPrivacy = await waitUntil {
            vm.cameraRoll.playbackState?.state == .stopped &&
            vm.cameraRoll.playbackNoticeMessage == "Privacy mode is on. Turn it off on the PC or an owner's device."
        }
        XCTAssertTrue(stoppedByPrivacy)

        host.configureState(privacyMode: false)
        vm.cameraRoll.closePlayback(resumeLive: true)
        let liveResumed = await waitUntil {
            vm.cameraRoll.activeClip == nil && vm.telemetry.petState.watching
        }
        XCTAssertTrue(liveResumed)
        XCTAssertTrue(host.metrics.receivedLiveViewReqs.contains(where: { $0.watch == true }))
    }

    // MARK: - 3. Pending Thumbnail Answered With `0x0D` Shows Placeholder Without Error Loop

    func testPendingThumbnailAnsweredWith0x0DShowsPlaceholderWithoutErrorLoop() async throws {
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }

        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-05-00_manual", startUnixMs: 1_795_111_500_000, startLocal: "2026-11-19T18:05:00+00:00", durationSec: 10.0, trigger: .manual, startedBy: "Owner", starredByIds: [], hasThumb: true),
            (day: "2026-11-19", clipId: "2026-11-19/17-00-00_motion", startUnixMs: 1_795_107_600_000, startLocal: "2026-11-19T17:00:00+00:00", durationSec: 12.0, trigger: .motion, startedBy: nil, starredByIds: [], hasThumb: true)
        ])

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Owner Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        try await pairClient(client, qrURI: host.prepareLanPairingOffer(role: .owner))
        let ready = await waitUntil { vm.isPetOwner }
        XCTAssertTrue(ready)
        await vm.cameraRoll.refreshDays(force: true)
        let clip1 = try XCTUnwrap(vm.cameraRoll.selectedDayClips.first)
        let clip2 = try XCTUnwrap(vm.cameraRoll.selectedDayClips.last)

        // Hold clip1's `0x14` in flight, then turn privacy mode ON -> host answers pending `0x14` with `0x0D privacy_mode`!
        host.setHoldPendingThumbnails(true)
        vm.cameraRoll.requestThumbnailIfNeeded(for: clip1)
        let thumbReceivedOnHost = await waitUntil { host.metrics.receivedRollThumbReqs.count == 1 }
        XCTAssertTrue(thumbReceivedOnHost)

        host.configureState(privacyMode: true)
        let resolvedToPlaceholder = await waitUntil {
            vm.cameraRoll.thumbnails[clip1.clipId] == .unavailable(code: "privacy_mode")
        }
        XCTAssertTrue(resolvedToPlaceholder)
        XCTAssertNil(vm.telemetry.lastCommandError, "A 0x0D answer to a pending 0x14 thumbnail must NOT surface a command error")

        // Calling `requestThumbnailIfNeeded` again must NOT re-request in a loop!
        vm.cameraRoll.requestThumbnailIfNeeded(for: clip1)
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(host.metrics.receivedRollThumbReqs.count, 1)

        // Turn privacy mode OFF and load clip2's thumbnail -> succeeds with in-memory JPEG <= 64 KiB!
        host.setHoldPendingThumbnails(false)
        host.configureState(privacyMode: false)
        let privacyOffSynced = await waitUntil { !vm.telemetry.petState.privacyMode }
        XCTAssertTrue(privacyOffSynced)

        vm.cameraRoll.requestThumbnailIfNeeded(for: clip2)
        let loadedThumb2 = await waitUntil {
            if case .loaded(let bytes) = vm.cameraRoll.thumbnails[clip2.clipId] {
                return !bytes.isEmpty && bytes.count <= RollThumbMessage.maxThumbnailBytes
            }
            return false
        }
        XCTAssertTrue(loadedThumb2)
    }

    // MARK: - 4. Roles: Viewer Has No Delete and No Settings (iOS & Mac), Star & Owner Delete Rules

    func testViewerRoleHasNoDeleteAndNoSettingsOnIOSAndMac() async throws {
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }

        let ownerClient = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Owner Mac", deviceKind: "mac")
        ownerClient.disableBonjourForTesting = true
        let macOwner = MacCameraViewerController(client: ownerClient)

        let viewerClient = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Viewer's iPhone", deviceKind: "iphone")
        viewerClient.disableBonjourForTesting = true
        let iosViewer = PocketViewModel(client: viewerClient, registerLifecycleObservers: false)

        let macViewerClient = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Viewer's Mac", deviceKind: "mac")
        macViewerClient.disableBonjourForTesting = true
        let macViewer = MacCameraViewerController(client: macViewerClient)

        defer {
            ownerClient.stop()
            viewerClient.stop()
            macViewerClient.stop()
        }

        try await pairClient(ownerClient, qrURI: host.prepareLanPairingOffer(role: .owner))
        try await pairClient(viewerClient, qrURI: host.prepareLanPairingOffer(role: .viewer))
        try await pairClient(macViewerClient, qrURI: host.prepareLanPairingOffer(role: .viewer))

        let allReady = await waitUntil {
            macOwner.isPetOwner && iosViewer.isPetViewer && macViewer.isPetViewer
        }
        XCTAssertTrue(allReady)

        let ownerDevId = macOwner.telemetry.petState.deviceId
        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-05-00_manual", startUnixMs: 1_795_111_500_000, startLocal: "2026-11-19T18:05:00+00:00", durationSec: 10.0, trigger: .manual, startedBy: "Owner", starredByIds: [ownerDevId], hasThumb: true),
            (day: "2026-11-19", clipId: "2026-11-19/17-00-00_motion", startUnixMs: 1_795_107_600_000, startLocal: "2026-11-19T17:00:00+00:00", durationSec: 12.0, trigger: .motion, startedBy: nil, starredByIds: [], hasThumb: true)
        ])

        // Verify viewer role hides delete and all settings on both iOS and Mac!
        XCTAssertFalse(iosViewer.showOwnerSettingsAndRemoteControls)
        XCTAssertFalse(iosViewer.cameraRoll.showOwnerRollControls)
        XCTAssertFalse(iosViewer.cameraRoll.canDeleteClips)

        XCTAssertFalse(macViewer.showOwnerControls)
        XCTAssertFalse(macViewer.cameraRoll.showOwnerRollControls)
        XCTAssertFalse(macViewer.cameraRoll.canDeleteClips)

        XCTAssertTrue(macOwner.showOwnerControls)
        XCTAssertTrue(macOwner.cameraRoll.showOwnerRollControls)
        XCTAssertTrue(macOwner.cameraRoll.canDeleteClips)

        await iosViewer.cameraRoll.refreshDays(force: true)
        await macOwner.cameraRoll.refreshDays(force: true)

        let ownerStarredClipOnViewer = try XCTUnwrap(iosViewer.cameraRoll.selectedDayClips.first)
        let unstarredClipOnViewer = try XCTUnwrap(iosViewer.cameraRoll.selectedDayClips.last)

        // Viewer CANNOT unstar a clip starred by the owner (`starred == true && starredByMe == false`)
        XCTAssertTrue(ownerStarredClipOnViewer.starred)
        XCTAssertFalse(ownerStarredClipOnViewer.starredByMe)
        XCTAssertFalse(iosViewer.cameraRoll.canToggleStar(for: ownerStarredClipOnViewer))

        // Viewer CAN star an unstarred clip, and CAN unstar its own star (`starredByMe == true`)
        XCTAssertTrue(iosViewer.cameraRoll.canToggleStar(for: unstarredClipOnViewer))
        iosViewer.cameraRoll.toggleStar(for: unstarredClipOnViewer)
        let viewerStarred = await waitUntil {
            iosViewer.cameraRoll.selectedDayClips.last?.starred == true &&
            iosViewer.cameraRoll.selectedDayClips.last?.starredByMe == true
        }
        XCTAssertTrue(viewerStarred)

        let updatedClipOnViewer = try XCTUnwrap(iosViewer.cameraRoll.selectedDayClips.last)
        iosViewer.cameraRoll.toggleStar(for: updatedClipOnViewer)
        let viewerUnstarred = await waitUntil {
            iosViewer.cameraRoll.selectedDayClips.last?.starred == false
        }
        XCTAssertTrue(viewerUnstarred)

        // Viewer attempting delete or settings is blocked locally (no command sent)
        let cmdsBeforeViewerBlocked = host.metrics.receivedPetCommands.count
        iosViewer.cameraRoll.requestDeleteClip(ownerStarredClipOnViewer)
        XCTAssertNil(iosViewer.cameraRoll.clipPendingDelete)
        iosViewer.cameraRoll.setRollRecordingEnabled(false)
        iosViewer.cameraRoll.setMotionTriggerEnabled(true)
        try? await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(host.metrics.receivedPetCommands.count, cmdsBeforeViewerBlocked)

        // Owner starts playing clip on viewer, then deletes clip with confirmation -> viewer playback stops with "This clip was deleted."!
        iosViewer.cameraRoll.startPlayback(clip: ownerStarredClipOnViewer)
        let viewerPlaying = await waitUntil { iosViewer.cameraRoll.playbackState?.state == .playing }
        XCTAssertTrue(viewerPlaying)

        let clipToDeleteOnOwner = try XCTUnwrap(macOwner.cameraRoll.selectedDayClips.first)
        macOwner.cameraRoll.requestDeleteClip(clipToDeleteOnOwner)
        XCTAssertEqual(macOwner.cameraRoll.clipPendingDelete?.clipId, clipToDeleteOnOwner.clipId)
        macOwner.cameraRoll.confirmDeletePendingClip()

        let deletedAndStopped = await waitUntil {
            macOwner.cameraRoll.selectedDayClips.count == 1 &&
            iosViewer.cameraRoll.playbackState?.state == .stopped &&
            iosViewer.cameraRoll.playbackNoticeMessage == "This clip was deleted."
        }
        XCTAssertTrue(deletedAndStopped)
    }

    // MARK: - 5. `stop_clip` No-Op vs `busy`, and `start_clip` Refusal Priority

    func testStopClipNoOpAndBusyAndStartClipPriorityOrder() async throws {
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }

        let ownerClient = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Owner Phone", deviceKind: "iphone")
        ownerClient.disableBonjourForTesting = true
        let ownerVM = PocketViewModel(client: ownerClient, registerLifecycleObservers: false)

        let viewerClient = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Viewer Phone", deviceKind: "iphone")
        viewerClient.disableBonjourForTesting = true
        let viewerVM = PocketViewModel(client: viewerClient, registerLifecycleObservers: false)

        defer {
            ownerClient.stop()
            viewerClient.stop()
        }

        try await pairClient(ownerClient, qrURI: host.prepareLanPairingOffer(role: .owner))
        try await pairClient(viewerClient, qrURI: host.prepareLanPairingOffer(role: .viewer))
        let ready = await waitUntil { ownerVM.isPetOwner && viewerVM.isPetViewer }
        XCTAssertTrue(ready)

        // 1. `stop_clip` when nothing is recording -> `ok: true` no-op, NOT treated as an error!
        ownerVM.cameraRoll.stopManualClip()
        let sawFirstStop = await waitUntil {
            host.metrics.receivedPetCommands.contains(where: { $0.action == "stop_clip" })
        }
        XCTAssertTrue(sawFirstStop)
        XCTAssertNil(ownerVM.cameraRoll.rollCommandError)
        XCTAssertNil(ownerVM.telemetry.lastCommandError)

        // 2. Manual `start_clip` -> `clipRecording == true && clipManualActive == true`, then `stop_clip` -> `ok: true`!
        ownerVM.cameraRoll.startManualClip()
        let startedManual = await waitUntil {
            ownerVM.telemetry.petState.clipRecording && ownerVM.telemetry.petState.clipManualActive
        }
        XCTAssertTrue(startedManual)

        ownerVM.cameraRoll.stopManualClip()
        let stoppedManual = await waitUntil {
            !ownerVM.telemetry.petState.clipRecording && !ownerVM.telemetry.petState.clipManualActive
        }
        XCTAssertTrue(stoppedManual)
        XCTAssertNil(ownerVM.cameraRoll.rollCommandError)

        // 3. Motion clip recording WITHOUT manual trigger -> `stop_clip` returns `busy` and shows its reason!
        host.configureState(clipRecording: true, clipTrigger: .motion, clipManualActive: false, clipElapsedSec: 4.0)
        let motionActive = await waitUntil {
            ownerVM.telemetry.petState.clipRecording && !ownerVM.telemetry.petState.clipManualActive
        }
        XCTAssertTrue(motionActive)

        ownerVM.cameraRoll.stopManualClip()
        let sawBusyReason = await waitUntil {
            ownerVM.cameraRoll.rollCommandError != nil
        }
        XCTAssertTrue(sawBusyReason)

        // 4. `start_clip` refusals follow §5.1 priority (`privacy_mode` beats `roll_disabled` & `sensor_off`)
        host.configureState(
            privacyMode: true,
            powerState: .off,
            rollEnabled: false,
            clipRecording: false,
            clipTrigger: .some(nil),
            clipManualActive: false
        )
        let syncedPrivacy = await waitUntil { ownerVM.telemetry.petState.privacyMode }
        XCTAssertTrue(syncedPrivacy)

        let privacyRefusal: Result<CommandResultMessage, CommandResultError> = await withCheckedContinuation { cont in
            ownerClient.sendPetCameraCommandWithResult(action: .startClip) { cont.resume(returning: $0) }
        }
        if case .failure(let err) = privacyRefusal {
            XCTAssertEqual(err.code, "privacy_mode")
        } else {
            XCTFail("Expected privacy_mode priority refusal")
        }

        // 5. Viewer during PC replay gets `pc_replaying` even if `roll_disabled` is also true (§5.2)
        host.configureState(privacyMode: false, isReplayingOnHost: true, powerState: .on, rollEnabled: false)
        let syncedReplay = await waitUntil { !viewerVM.telemetry.petState.privacyMode && viewerVM.telemetry.hostState.isReplayingOnHost }
        XCTAssertTrue(syncedReplay)

        let viewerReplayRefusal: Result<CommandResultMessage, CommandResultError> = await withCheckedContinuation { cont in
            viewerClient.sendPetCameraCommandWithResult(action: .startClip) { cont.resume(returning: $0) }
        }
        if case .failure(let err) = viewerReplayRefusal {
            XCTAssertEqual(err.code, "pc_replaying")
        } else {
            XCTFail("Expected viewer start_clip during PC replay to return pc_replaying")
        }
    }

    // MARK: - 6. Outgoing Frame <= 64 KiB Cap & `0x12 limit` Clamped to 1...100

    func testEveryOutgoingFrameWithin64KiBAndRollListLimitClamped() async throws {
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }
        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-05-00_manual", startUnixMs: 1_795_111_500_000, startLocal: "2026-11-19T18:05:00+00:00", durationSec: 10.0, trigger: .manual, startedBy: "Owner", starredByIds: [], hasThumb: true)
        ])

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Cap Check Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let sentBox = Phase9BSyncBox<[(WireMessageType, Int)]>([])
        client.onOutgoingWirePacketSent = { type, pkt in
            sentBox.update { $0.append((type, pkt.count)) }
        }
        defer { client.stop() }

        try await pairClient(client, qrURI: host.prepareLanPairingOffer(role: .owner))
        let ready = await waitUntil { client.telemetry.isConnected && client.telemetry.hasConfirmedPetCameraState }
        XCTAssertTrue(ready)

        // Clamp check: limit 0 -> 1, limit 5000 -> 100
        _ = await withCheckedContinuation { (cont: CheckedContinuation<Result<RollListMessage, CommandResultError>, Never>) in
            client.requestRollList(limit: 0) { cont.resume(returning: $0) }
        }
        _ = await withCheckedContinuation { (cont: CheckedContinuation<Result<RollListMessage, CommandResultError>, Never>) in
            client.requestRollList(limit: 5000) { cont.resume(returning: $0) }
        }
        _ = await withCheckedContinuation { (cont: CheckedContinuation<Result<RollThumbMessage, CommandResultError>, Never>) in
            client.requestRollThumbnail(clipId: "2026-11-19/18-05-00_manual") { cont.resume(returning: $0) }
        }
        _ = await withCheckedContinuation { (cont: CheckedContinuation<Result<PlaybackStateMessage, CommandResultError>, Never>) in
            client.sendPlaybackRequest(action: .start, clipId: "2026-11-19/18-05-00_manual", positionSec: 0) { cont.resume(returning: $0) }
        }

        let recordedLimits = host.metrics.receivedRollListReqs.compactMap(\.limit)
        XCTAssertEqual(recordedLimits, [1, 100])
        for lim in recordedLimits {
            XCTAssertTrue((1...100).contains(lim))
        }

        let sentPackets = sentBox.read()
        XCTAssertFalse(sentPackets.isEmpty)
        for (msgType, byteLen) in sentPackets {
            XCTAssertLessThanOrEqual(byteLen, WireHeader.maxClientFrameBytes, "Outgoing \(msgType) frame exceeded 64 KiB cap")
        }

        // Oversize payload (> 64 KiB) is rejected locally before sending
        let oversizeRejected = !client.sendRawClientPayloadForTesting(
            type: .rollListReq,
            payload: Data(repeating: 0x41, count: WireHeader.maxClientFrameBytes)
        )
        XCTAssertTrue(oversizeRejected)
        XCTAssertEqual(client.telemetry.rejectedOversizeOutgoingFrames, 1)
    }

    // MARK: - 7. Static & Isolated Zero-Storage Check (Full iOS App Container Test Runs in `KinectPocketTests`)

    nonisolated private static func snapshotRecursiveRelativePaths(in root: URL) -> Set<String> {
        let fm = FileManager.default
        let stdRoot = root.resolvingSymlinksInPath().standardizedFileURL
        let rootPrefix = stdRoot.path.hasSuffix("/") ? stdRoot.path : stdRoot.path + "/"
        guard let enumerator = fm.enumerator(
            at: stdRoot,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: []
        ) else {
            return []
        }
        var found = Set<String>()
        for case let url as URL in enumerator {
            let vals = try? url.resourceValues(forKeys: [.isRegularFileKey])
            guard vals?.isRegularFile == true else { continue }
            let fullPath = url.resolvingSymlinksInPath().standardizedFileURL.path
            if fullPath.hasPrefix(rootPrefix) {
                found.insert(String(fullPath.dropFirst(rootPrefix.count)))
            } else {
                found.insert(fullPath)
            }
        }
        return found
    }

    func testZeroFilesInSandboxAfterThumbnailsAndStreamedPlayback() async throws {
        URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)
        let fm = FileManager.default
        // Never enumerate the Mac's real `~/Documents`; the real iOS app-container sandbox test runs in `KinectPocketTests`.
        let isolatedContainer = fm.temporaryDirectory.appendingPathComponent("kv-p9b-client-sandbox-\(UUID().uuidString)", isDirectory: true)
        defer { try? fm.removeItem(at: isolatedContainer) }
        let libraryCachesRoot = try XCTUnwrap(fm.urls(for: .cachesDirectory, in: .userDomainMask).first)
        let appCachesURL = libraryCachesRoot.appendingPathComponent(
            Bundle.main.bundleIdentifier ?? "com.example.kinectviewer.host",
            isDirectory: true
        )
        try fm.createDirectory(at: isolatedContainer, withIntermediateDirectories: true)
        try fm.createDirectory(at: appCachesURL, withIntermediateDirectories: true)

        // Probe self-check verifying our scanner detects nested files recursively by relative path (no date filter)
        let probeBefore = Self.snapshotRecursiveRelativePaths(in: isolatedContainer)
        let probeDir = isolatedContainer.appendingPathComponent("nested/inner", isDirectory: true)
        try fm.createDirectory(at: probeDir, withIntermediateDirectories: true)
        let probeFile = probeDir.appendingPathComponent("probe.bin")
        try Data([0x01, 0x02]).write(to: probeFile)
        let probeCheck = Self.snapshotRecursiveRelativePaths(in: isolatedContainer)
        XCTAssertTrue(probeCheck.subtracting(probeBefore).contains("nested/inner/probe.bin"))
        try fm.removeItem(at: isolatedContainer.appendingPathComponent("nested", isDirectory: true))

        // Start synthetic host BEFORE taking baseline snapshot
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }
        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-05-00_manual", startUnixMs: 1_795_111_500_000, startLocal: "2026-11-19T18:05:00+00:00", durationSec: 12.0, trigger: .manual, startedBy: "Owner", starredByIds: [], hasThumb: true)
        ])

        let beforeContainer = Self.snapshotRecursiveRelativePaths(in: isolatedContainer)
        let beforeCaches = Self.snapshotRecursiveRelativePaths(in: appCachesURL)

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Zero Storage iPhone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        let playbackLayer = AVSampleBufferDisplayLayer()
        vm.cameraRoll.playbackDisplayBridge.attach(layer: playbackLayer)
        XCTAssertTrue(playbackLayer.preventsCapture)
        defer { client.stop() }

        try await pairClient(client, qrURI: host.prepareLanPairingOffer(role: .owner))
        let ready = await waitUntil { vm.isPetOwner }
        XCTAssertTrue(ready)

        await vm.cameraRoll.refreshDays(force: true)
        let clip = try XCTUnwrap(vm.cameraRoll.selectedDayClips.first)
        vm.cameraRoll.requestThumbnailIfNeeded(for: clip)
        vm.cameraRoll.startPlayback(clip: clip)

        let streamed = await waitUntil {
            if case .loaded = vm.cameraRoll.thumbnails[clip.clipId],
               vm.cameraRoll.playbackState?.state == .playing,
               vm.telemetry.decodedPlaybackFrames >= 3 {
                return true
            }
            return false
        }
        XCTAssertTrue(streamed)
        vm.cameraRoll.closePlayback(resumeLive: true)

        let newContainer = Self.snapshotRecursiveRelativePaths(in: isolatedContainer).subtracting(beforeContainer)
        let newCaches = Self.snapshotRecursiveRelativePaths(in: appCachesURL).subtracting(beforeCaches)
        XCTAssertEqual(newContainer, [], "Streamed playback and thumbnails must leave 0 new files in sandbox")
        XCTAssertEqual(newCaches, [], "Streamed playback and thumbnails must leave 0 new files in Library/Caches")

        // Static audit of camera-roll client sources: zero Photo Library / share sheet / file-write calls
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let clientRollSources = [
            repoRoot.appendingPathComponent("shared/CameraRollController.swift"),
            repoRoot.appendingPathComponent("ios/CameraRollSheetView.swift"),
            repoRoot.appendingPathComponent("macos/App/MacCameraRollView.swift")
        ]
        for srcURL in clientRollSources {
            let text = try String(contentsOf: srcURL, encoding: .utf8)
            XCTAssertFalse(text.contains("PHPhotoLibrary"))
            XCTAssertFalse(text.contains("UIImageWriteToSavedPhotosAlbum"))
            XCTAssertFalse(text.contains("AVAssetWriter"))
            XCTAssertFalse(text.contains("ShareLink"))
            XCTAssertFalse(text.contains("UIActivityViewController"))
            XCTAssertFalse(text.contains(".write(to:"))
        }
    }

    // MARK: - 8. Outside Invites (`kv2://invite`), LAN-First -> Outside Routing, Back-Off & Endpoint Links (`kv2://endpoint`)

    func testOutsideInvitesSingleUseExpiryLanFirstFallbackBackoffAndEndpointLinks() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }

        let baseNow: UInt64 = UInt64(Date().timeIntervalSince1970)
        host.setAdmitExpiredRedemptionWithoutOpenInvite(false, hostNowUnixSec: baseNow + 595)

        // 1. URI round-trip & roleHint parsing (distinct `lanHost` and `outsideHost`)
        try host.setLanIPv6OutsideListenerEnabled(true, behavior: .normal)
        host.resetAcceptCounters()
        let invite1 = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "::1",
            outsideHost: "127.0.0.1"
        )
        let uri1 = invite1.toURIString()
        let decodedInvite = try XCTUnwrap(InviteLinkPayload.parse("  \(uri1)\n "))
        XCTAssertEqual(decodedInvite, invite1)
        XCTAssertEqual(decodedInvite.roleHint, .viewer)
        XCTAssertTrue(ParsedCameraLink.isSupportedQRURI(uri1))

        // 2. Expired invite (> exp + 60 s slack) is rejected locally without touching the host; within 60 s slack is allowed!
        let persistence = InMemoryPersistenceProvider()
        let testClock = ManualTestClockScheduler()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Viewer's iPad", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.timerScheduler = testClock
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let expiredRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(uri1, nowUnixSec: invite1.expiresUnixSec + 61) { cont.resume(returning: $0) }
        }
        guard case .failure(let expErr) = expiredRes else {
            XCTFail("Expected expired invite to fail locally")
            return
        }
        XCTAssertEqual(expErr.localizedDescription, "This invite has expired or was already used. Ask for a new one.")
        XCTAssertEqual(host.metrics.receivedPairReqs.count, 0)

        // Shut down LAN stream before redeeming so post-redemption streaming exercises LAN-first (`lh:streamPort` fails) -> Outside (`h:outsidePort` succeeds)!
        try host.setLanStreamEnabled(false)
        let connectOrderBox = Phase9BSyncBox<[PetPath]>([])
        client.onConnectAttemptForTesting = { origin, _ in
            connectOrderBox.update { $0.append(origin) }
        }

        // Redeem within 60 s clock slack (`now == exp + 45`): must hit `lh:p` (`::1`) first!
        let redeemRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(uri1, nowUnixSec: invite1.expiresUnixSec + 45) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try redeemRes.get())
        XCTAssertEqual(host.metrics.receivedPairReqs.last?.methodRaw, WirePairingMethod.invite.rawValue)
        XCTAssertEqual(host.lanIPv6OutsideAcceptCount, 1, "Redemption must try lh:p (::1) first")

        // Verify Keychain persistence saved `outsideEndpoint` (`h:p`) and `pinnedServerCertSHA256`
        let stored = try XCTUnwrap(persistence.rawStoredForTesting)
        XCTAssertEqual(stored.pinnedServerCertSHA256, host.serverIdentity.certificateSHA256)
        XCTAssertEqual(stored.outsideEndpoint, PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: true))

        // Verify LAN-first (`.home`), then fallback to outside (`.outside`) and session established on `.outside`!
        let connectedOutside = await waitUntil {
            vm.isPetViewer && vm.telemetry.petState.path == .outside
        }
        XCTAssertTrue(connectedOutside)
        let attempts = connectOrderBox.read()
        XCTAssertGreaterThanOrEqual(attempts.count, 2)
        XCTAssertEqual(attempts[0], .home, "Must try LAN stream endpoint first")
        XCTAssertEqual(attempts[1], .outside, "Must fall back to stored outside endpoint when LAN is unreachable")

        // Advance test clock 30.0 s (`outsideSessionStabilityResetSec == 30.0`) so outside `reconnectCount` resets to 0
        testClock.advance(bySeconds: 30.0)
        let resetDone = await waitUntil { vm.telemetry.reconnectCount == 0 }
        XCTAssertTrue(resetDone)

        // 3. Second device trying to reuse the same single-use invite is rejected!
        host.setAdmitExpiredRedemptionWithoutOpenInvite(true)
        let secondClient = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Second Phone", deviceKind: "iphone")
        secondClient.disableBonjourForTesting = true
        defer { secondClient.stop() }

        let reuseRes: Result<Void, Error> = await withCheckedContinuation { cont in
            secondClient.submitCameraLink(uri1, nowUnixSec: baseNow + 10) { cont.resume(returning: $0) }
        }
        if case .failure(let reuseErr) = reuseRes {
            XCTAssertEqual(reuseErr.localizedDescription, "This invite has expired or was already used. Ask for a new one.")
        } else {
            XCTFail("Second redemption of single-use invite must fail")
        }
        host.setAdmitExpiredRedemptionWithoutOpenInvite(false)

        // 4. Exponential back-off `1, 2, 4, 8, 16, 30, 30 s` when both LAN and outside are unreachable, plus plain unreachable text!
        let scheduledDelays = Phase9BSyncBox<[Double]>([])
        client.onReconnectScheduledForTesting = { delaySec, retryNow in
            let count = scheduledDelays.update { list -> Int in
                list.append(delaySec)
                return list.count
            }
            if count < 7 {
                retryNow()
            }
        }
        try host.setOutsideListenerEnabled(false, sendNoticeOnDisable: false)

        let collected7Delays = await waitUntil {
            scheduledDelays.read().count >= 7
        }
        XCTAssertTrue(collected7Delays)
        XCTAssertEqual(scheduledDelays.read(), [1.0, 2.0, 4.0, 8.0, 16.0, 30.0, 30.0])
        XCTAssertEqual(vm.disconnectedStatusTitle, PetCameraUserStrings.unreachableDefaultPC)

        // 5. `kv2://endpoint` validation:
        //    a) Mismatched camera serviceId/fingerprint -> refused!
        let wrongEndpoint = EndpointLinkPayload(
            serviceId: "other-camera-id",
            serverCertSHA256: Data(repeating: 0xAA, count: 32),
            outsideHost: "127.0.0.1",
            outsidePort: 45678
        ).toURIString()
        let wrongEpRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(wrongEndpoint) { cont.resume(returning: $0) }
        }
        if case .failure(let err) = wrongEpRes {
            XCTAssertEqual(err.localizedDescription, "This link is for a camera you aren't paired with")
        } else {
            XCTFail("Expected mismatched endpoint link to be refused")
        }
        XCTAssertEqual(persistence.rawStoredForTesting?.outsideEndpoint?.port, host.outsidePort)

        //    b) Matching `kv2://endpoint` pointing to an unreachable port -> keeps old endpoint and says "That link didn't reach your camera."!
        client.onReconnectScheduledForTesting = { _, _ in } // pause background retries during probe assertions
        let deadEndpointURI = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: 1
        ).toURIString()
        let deadEpRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(deadEndpointURI) { cont.resume(returning: $0) }
        }
        if case .failure(let err) = deadEpRes {
            XCTAssertEqual(err.localizedDescription, "That link didn't reach your camera.")
        } else {
            XCTFail("Expected unreachable candidate endpoint link to fail")
        }
        XCTAssertEqual(persistence.rawStoredForTesting?.outsideEndpoint?.port, host.outsidePort, "Old endpoint must be kept when candidate handshake fails")

        //    c) Bring outside listener back up and apply matching `kv2://endpoint` -> succeeds and updates stored endpoint after real session starts!
        try host.setOutsideListenerEnabled(true)
        let liveEndpointURI = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: host.outsidePort
        ).toURIString()
        let liveEpRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(liveEndpointURI) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try liveEpRes.get())
        let reconnectedViaLink = await waitUntil { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(reconnectedViaLink)

        //    d) Address-changed notice + unreachable -> shows exact §10 address-changed sentence; outside_disabled -> shows exact sentence!
        host.sendAddressChangedNotice(newEndpoint: nil)
        let sawAddrNotice = await waitUntil { vm.telemetry.sawAddressChangeNotice }
        XCTAssertTrue(sawAddrNotice)
        host.closeAllSessions()
        try host.setOutsideListenerEnabled(false, sendNoticeOnDisable: false)
        let disconnectedAfterAddrChange = await waitUntil { !vm.telemetry.isConnected }
        XCTAssertTrue(disconnectedAfterAddrChange)
        XCTAssertEqual(vm.disconnectedStatusTitle, PetCameraUserStrings.unreachableAddressChanged)

        try host.setOutsideListenerEnabled(true)
        client.start()
        let reconnectedAgain = await waitUntil { vm.telemetry.isConnected }
        XCTAssertTrue(reconnectedAgain)
        try host.setOutsideListenerEnabled(false, sendNoticeOnDisable: true)
        let sawOutsideDisabled = await waitUntil {
            !vm.telemetry.isConnected &&
            vm.disconnectedStatusTitle == "Viewing from outside the home was turned off on the PC."
        }
        XCTAssertTrue(sawOutsideDisabled)
    }

    // MARK: - 9. Older Hosts: v2 Host & W6 v3 Host Without Camera Roll Work Unchanged

    func testOlderV2HostAndV3HostWithoutCameraRollUnchanged() async throws {
        // A) v3 W6 host (`cameraRoll == false`, no `rollBlockedReason`) -> Camera Roll stays hidden on both iOS and Mac!
        let w6Host = try FakeV3CameraRollAndInviteHost(
            serviceId: "v3w6host0001",
            cameraRollShipped: false,
            outsideShipped: false,
            startLanStream: true,
            startOutsideListener: false
        )
        defer { w6Host.shutdown() }

        let w6ClientIOS = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "iOS on W6", deviceKind: "iphone")
        w6ClientIOS.disableBonjourForTesting = true
        let w6VMIOS = PocketViewModel(client: w6ClientIOS, registerLifecycleObservers: false)

        let w6ClientMac = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Mac on W6", deviceKind: "mac")
        w6ClientMac.disableBonjourForTesting = true
        let w6ViewerMac = MacCameraViewerController(client: w6ClientMac)

        defer {
            w6ClientIOS.stop()
            w6ClientMac.stop()
        }

        try await pairClient(w6ClientIOS, qrURI: w6Host.prepareLanPairingOffer(role: .owner))
        try await pairClient(w6ClientMac, qrURI: w6Host.prepareLanPairingOffer(role: .owner))

        let w6Connected = await waitUntil { w6VMIOS.isPetOwner && w6ViewerMac.isPetOwner }
        XCTAssertTrue(w6Connected)
        XCTAssertFalse(w6VMIOS.showCameraRollButton, "W6 v3 host without camera roll must keep Camera Roll hidden on iOS")
        XCTAssertFalse(w6ViewerMac.showCameraRollButton, "W6 v3 host without camera roll must keep Camera Roll hidden on Mac")

        // B) v2 Mac `HostController` -> `isPetCameraV3 == false`, `showCameraRollButton == false`, v2 controls work unchanged!
        let tempDir = FileManager.default.temporaryDirectory.appendingPathComponent("kv-phase9b-v2-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let v2Host = HostController(
            recordingsDirectoryURL: tempDir.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempDir.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempDir.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { v2Host.shutdown() }
        v2Host.openPairingWindow()
        let v2Offer = try XCTUnwrap(v2Host.activePairingOffer)
        let parsedV2 = try XCTUnwrap(QRPairingPayload.parse(v2Offer.qrURI))
        let loopbackV2URI = QRPairingPayload(
            serviceId: parsedV2.serviceId,
            serverCertSHA256: parsedV2.serverCertSHA256,
            oneTimeToken: v2Offer.qrToken,
            hostIP: "127.0.0.1",
            pairPort: v2Offer.port
        ).toURIString()

        let v2Client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "iPhone on v2", deviceKind: "iphone")
        v2Client.disableBonjourForTesting = true
        let v2VM = PocketViewModel(client: v2Client, registerLifecycleObservers: false)
        defer { v2Client.stop() }

        try await pairClient(v2Client, qrURI: loopbackV2URI)
        let v2Connected = await waitUntil { v2VM.telemetry.isConnected && v2VM.supportsHostV2 }
        XCTAssertTrue(v2Connected)
        XCTAssertFalse(v2VM.isPetCameraV3)
        XCTAssertFalse(v2VM.showCameraRollButton)
        XCTAssertTrue(v2VM.showOwnerSettingsAndRemoteControls)
    }
}
