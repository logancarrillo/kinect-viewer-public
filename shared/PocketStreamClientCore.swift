import CoreMedia
import CoreVideo
import CryptoKit
import Foundation
import Network
import Security
import VideoToolbox

protocol PairingPersistenceProvider: Sendable {
    func loadResult() -> ClientPairingLoadResult
    func load() -> ClientPairingState?
    @discardableResult func save(_ state: ClientPairingState) -> Bool
    func wipe()
}

extension PairingPersistenceProvider {
    func load() -> ClientPairingState? {
        if case .loaded(let state) = loadResult() {
            return state
        }
        return nil
    }
}

struct KeychainPersistenceProvider: PairingPersistenceProvider {
    let service: String
    let account: String
    let primitives: ClientKeychainPrimitives

    init(
        service: String = KeychainPairingStore.defaultService,
        account: String = KeychainPairingStore.defaultAccount,
        primitives: ClientKeychainPrimitives = .system
    ) {
        self.service = service
        self.account = account
        self.primitives = primitives
    }

    func loadResult() -> ClientPairingLoadResult {
        KeychainPairingStore.loadResult(service: service, account: account, primitives: primitives)
    }

    @discardableResult
    func save(_ state: ClientPairingState) -> Bool {
        KeychainPairingStore.save(state, service: service, account: account, primitives: primitives)
    }

    func wipe() {
        KeychainPairingStore.wipe(service: service, account: account, primitives: primitives)
    }
}

final class InMemoryPersistenceProvider: PairingPersistenceProvider, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: ClientPairingState?
    private var simulatedLoadFailure: ClientPairingLoadResult?
    private var simulateSaveFailure: Bool = false

    init(initial: ClientPairingState? = nil) {
        self.stored = initial
    }

    func setSimulatedAccessFailure(_ failure: ClientPairingLoadResult?, failSaves: Bool = false) {
        lock.lock()
        self.simulatedLoadFailure = failure
        self.simulateSaveFailure = failSaves
        lock.unlock()
    }

    func loadResult() -> ClientPairingLoadResult {
        lock.lock()
        defer { lock.unlock() }
        if let failure = simulatedLoadFailure {
            return failure
        }
        if let stored = stored {
            return .loaded(stored)
        }
        return .notFound
    }

    @discardableResult
    func save(_ state: ClientPairingState) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if simulateSaveFailure {
            return false
        }
        stored = state
        return true
    }

    func wipe() {
        lock.lock()
        stored = nil
        lock.unlock()
    }

    var rawStoredForTesting: ClientPairingState? {
        lock.lock()
        defer { lock.unlock() }
        return stored
    }
}

/// Counters for one outside race; touched only on the client queue.
private final class OutsideRaceTally: @unchecked Sendable {
    var started = 0
    var ended = 0
    var sawLimiterClose = false
    var sawTLSRefusal = false
    var nextIndex: [Int] = []
    var groupsDone = 0
    var startNext: ((Int) -> Void)?
}

/// A raced outside connection that finished TLS with the pinned certificate, handed to the session code.
private struct OutsideRaceWinner {
    let conn: NWConnection
    let sawServerCertificate: LockedFlag
    let serverPinMatched: LockedFlag
    let attemptId: UInt64
}

/// One stream connection attempt, kept in memory (never written to disk) so the app can show why it can't reach the PC.
struct ConnectAttemptRecord: Sendable, Equatable {
    let id: UInt64
    let at: Date
    let path: PetPath
    var endpoint: String
    var result: String
}

struct PocketClientTelemetry: Sendable {
    /// The newest stream connection attempts, newest last (at most `maxRecentConnectAttempts`).
    var recentConnectAttempts: [ConnectAttemptRecord] = []
    /// Set while iOS itself blocks the network for this app (Cellular Data off, Wi-Fi off, Local Network off).
    var networkBlockedSentence: String? = nil
    static let maxRecentConnectAttempts = 6
    var connectionState: String = "Unpaired"
    var isPaired: Bool = false
    var isConnected: Bool = false
    var protocolVersion: Int = 1
    var hasConfirmedHostState: Bool = false
    var hostState: HostStateSyncMessage = .unconfirmedInitial
    var allowModeSwitch: Bool = false
    var currentModeRaw: Int = 0
    var currentSbsLayoutRaw: Int = 0
    var streamWidth: Int = 0
    var streamHeight: Int = 0
    var receivedFrames: UInt64 = 0
    var recentSeqDrops: UInt64 = 0
    var fps: Double = 0.0
    var bitrateMbps: Double = 0.0
    var rttMs: Double = 0.0
    var hostToDisplayLatencyMs: Double = 0.0
    var barcodePixelLatencyMs: Double = 0.0
    var barcodeVerifiedFrames: UInt64 = 0
    var reconnectCount: Int = 0
    var lastScheduledReconnectDelaySec: Double = 0.0
    var lastError: String?

    // Protocol v3 Pet Camera session state
    var isPetCameraV3: Bool = false
    var petCameraHello: PetCameraHello? = nil
    var hasConfirmedPetCameraState: Bool = false
    var petState: PetCameraStateMessage = .unconfirmedInitial
    var cachedPairedRole: PetRole? = nil
    var cachedHostProtocolVersion: Int? = nil
    var storedOutsideEndpoint: PetEndpoint? = nil
    var lastSawSharingEnabled: Bool? = nil
    var lastSawOutsideEnabled: Bool? = nil
    var sawAddressChangeNotice: Bool = false
    var lastSessionNotice: SessionNoticeMessage? = nil
    var tlsRefusalNotice: String? = nil
    var lastCommandError: String? = nil
    var revokedNotice: String? = nil
    var rejectedOversizeOutgoingFrames: Int = 0
    var latestPlaybackState: PlaybackStateMessage? = nil
    var decodedPlaybackFrames: UInt64 = 0

    var connectionQuality: PocketConnectionQuality {
        if isPetCameraV3 && hasConfirmedPetCameraState && !petState.liveAllowed {
            return isConnected ? .standby : .offline
        }
        return PocketConnectionQuality.evaluate(
            isConnected: isConnected,
            powerState: hasConfirmedHostState ? hostState.powerState : .on,
            modeRaw: currentModeRaw,
            fps: fps,
            delayMs: hostToDisplayLatencyMs,
            rttMs: rttMs,
            recentSeqDrops: recentSeqDrops
        )
    }
}

protocol PocketTimerScheduler: Sendable {
    func schedule(
        afterSeconds delaySec: Double,
        on queue: DispatchQueue,
        block: @escaping @Sendable () -> Void
    ) -> @Sendable () -> Void
}

struct SystemPocketTimerScheduler: PocketTimerScheduler {
    func schedule(
        afterSeconds delaySec: Double,
        on queue: DispatchQueue,
        block: @escaping @Sendable () -> Void
    ) -> @Sendable () -> Void {
        let cancelled = LockedFlag()
        queue.asyncAfter(deadline: .now() + delaySec) {
            guard !cancelled.isSet else { return }
            block()
        }
        return { _ = cancelled.trySet() }
    }
}

final class PocketStreamClientCore: @unchecked Sendable {
    private static let queueKey = DispatchSpecificKey<UInt8>()
    private let queue = DispatchQueue(label: "kinectviewer.pocket.client", qos: .userInteractive)
    private let store: PairingPersistenceProvider
    let deviceName: String?
    let deviceKind: String

    private var state: ClientPairingState
    private var clientIdentity: CryptoIdentity

    private var browser: NWBrowser?
    private var connection: NWConnection?
    private var frameReader = WireFrameReader()
    private var pingTimer: DispatchSourceTimer?
    private var reconnectWork: DispatchWorkItem?
    private var lanFallbackWork: DispatchWorkItem?
    private var lanAttemptTimeoutWork: DispatchWorkItem?
    private var outsideStabilityCancel: (@Sendable () -> Void)?
    private var probeTimeoutWork: DispatchWorkItem?
    private var nextConnectAttemptId: UInt64 = 0
    /// After the PC's limiter closes a connection before TLS, the next reconnect waits at least this long.
    var limiterClosePauseSec: Double = 10.0
    /// An outside dial that hasn't reached a session by then is given up and retried with back-off, so the app
    /// never sits on "connecting" for minutes on a network that swallows the packets.
    var outsideAttemptDeadlineSec: Double = 12.0
    private var outsideAttemptDeadlineWork: DispatchWorkItem?
    /// Set when an outside dial got nowhere: the next outside dial is pinned to cellular (a joined Wi-Fi with no
    /// internet, such as wireless CarPlay, otherwise keeps winning). Flips back after each stalled attempt.
    private(set) var pinNextOutsideDialToCellular: Bool = false
    /// The rest of this cycle's outside addresses (NAT64-synthesized IPv6 first, then IPv4, then the name form).
    private var pendingOutsideCandidates: [NWEndpoint] = []
    /// Test hook: replaces getaddrinfo for the outside address.
    var resolveOutsideCandidatesForTesting: (@Sendable (String, UInt16) -> [NWEndpoint])?
    private var minimumNextReconnectDelaySec: Double = 0
    /// Test hook: called on the client queue when `start()` finds a candidate probe in flight and leaves it alone.
    var onStartLeftProbeAloneForTesting: (@Sendable () -> Void)?
    /// Set when `start()` arrives while a candidate probe is in flight (e.g. screen capture ended mid-probe, after the
    /// probe began from a stopped client). If that probe then fails, the client reconnects as `start()` asked.
    private var startRequestedDuringProbe: Bool = false
    private var shouldStayConnected: Bool = false
    private var isReconnecting: Bool = false
    private var receivedServerHello: Bool = false
    private var transportActiveForPings: Bool = false
    private var sentV3PingInCurrentSession: Bool = false
    private var activeConnectOrigin: PetPath = .home
    private var isAutomaticLanFirstAttempt: Bool = false
    private var triedOutsideInCurrentCycle: Bool = false
    private var pendingHandshakeCallback: (@Sendable (Bool, String?) -> Void)?
    private var pendingSessionConfirmedCallback: (@Sendable (Bool, String?) -> Void)?
    private var activeCandidateProbeFailureHandler: (@Sendable () -> Void)?

    // Live media format & optional synthetic clock barcode hardware decoder
    private var formatDesc: CMVideoFormatDescription?
    private var currentStreamEpoch: UInt32 = 0
    private var decompSession: VTDecompressionSession?

    // Dedicated camera-roll playback decoder state (`0x18`/`0x19`, excluded from live telemetry per §4.2 & §5.9)
    private var playbackStreamId: UInt32?
    private var playbackClipId: String?
    private var playbackFormatDesc: CMVideoFormatDescription?
    private var playbackStreamEpoch: UInt32 = 0

    // Correlated v3 request callbacks (`0x0C`, `0x12`, `0x14`, `0x16`)
    private var pendingCommandCallbacks: [UInt64: @Sendable (Result<CommandResultMessage, CommandResultError>) -> Void] = [:]
    private var pendingRollListCallbacks: [UInt64: @Sendable (Result<RollListMessage, CommandResultError>) -> Void] = [:]
    private var pendingRollThumbCallbacks: [UInt64: @Sendable (Result<RollThumbMessage, CommandResultError>) -> Void] = [:]
    private var pendingPlaybackCallbacks: [UInt64: @Sendable (Result<PlaybackStateMessage, CommandResultError>) -> Void] = [:]

    // Clock sync (Cristian min-RTT estimator: host_ns ≈ client_ns + clockOffsetNs)
    private var bestRttNs: UInt64 = .max
    private var clockOffsetNs: Int64 = 0
    private var rttSamples: [(rtt: UInt64, offset: Int64)] = []

    // Rolling FPS & Bitrate (live stream only)
    private var frameArrivalTimes: [UInt64] = []
    private var recvBytesWindow: Int = 0
    private var lastBitrateTickNs: UInt64 = 0
    private var lastVideoFrameSeqInEpoch: UInt64?
    private var nextPowerRequestId: UInt64 = 0
    private var nextPetCommandRequestId: UInt64 = 0

    // Configurable timing seams (defaults follow `docs/protocol-v3-pet-camera.md`)
    var requestTimeoutSec: Double = 10.0
    var lanDiscoveryGraceBeforeOutsideSec: Double = 1.2
    var lanAttemptTimeoutBeforeOutsideSec: Double = 1.5
    var inviteHandshakeTimeoutSec: Double = 5.0
    var inviteRedemptionTimeoutSec: Double = 10.0
    var candidateProbeTimeoutSec: Double = 5.0
    var outsideSessionStabilityResetSec: Double = 30.0
    var timerScheduler: PocketTimerScheduler = SystemPocketTimerScheduler()
    var disableBonjourForTesting: Bool = false

    private(set) var telemetry = PocketClientTelemetry()

    var onSampleBufferReady: (@Sendable (CMSampleBuffer) -> Void)?
    var onPlaybackSampleBufferReady: (@Sendable (CMSampleBuffer) -> Void)?
    var onPlaybackStateUpdated: (@Sendable (PlaybackStateMessage?) -> Void)?
    var onPlaybackMediaTimeUpdated: (@Sendable (UInt32, Double) -> Void)?
    var onPlaybackStreamFlushed: (@Sendable () -> Void)?
    var onTelemetryUpdated: (@Sendable (PocketClientTelemetry) -> Void)?
    var onHostStateReceived: (@Sendable (HostStateSyncMessage) -> Void)?
    var onPetCameraStateReceived: (@Sendable (PetCameraStateMessage) -> Void)?
    var onCommandResult: (@Sendable (CommandResultMessage) -> Void)?
    var onRevoked: (@Sendable (String) -> Void)?
    var onStreamFlushed: (@Sendable () -> Void)?
    var onOutgoingWirePacketSent: (@Sendable (WireMessageType, Data) -> Void)?
    /// Optional test seam observing each scheduled reconnect delay (`delaySec`) and triggering the retry callback.
    var onReconnectScheduledForTesting: (@Sendable (Double, @escaping @Sendable () -> Void) -> Void)?
    /// Optional test seam observing connection target attempts (`.home` vs `.outside`).
    var onConnectAttemptForTesting: (@Sendable (PetPath, String) -> Void)?

    init(
        persistence: PairingPersistenceProvider = KeychainPersistenceProvider(),
        deviceName: String? = nil,
        deviceKind: String = "iphone"
    ) throws {
        queue.setSpecific(key: Self.queueKey, value: 1)
        self.store = persistence
        let normalizedKind = Self.normalizeDeviceKind(deviceKind)
        self.deviceName = Self.sanitizeDeviceName(deviceName)
        self.deviceKind = normalizedKind

        switch persistence.loadResult() {
        case .loaded(let existing):
            guard let ident = try? CryptoIdentity(record: existing.clientIdentity) else {
                throw ClientPairingStoreError.corruptedKeychainPairingState
            }
            self.state = existing
            self.clientIdentity = ident
            self.telemetry.cachedPairedRole = existing.pairedRole
            self.telemetry.cachedHostProtocolVersion = existing.lastKnownHostProtocolVersion

        case .notFound:
            // True first run only: generate a new identity and persist it.
            let ident = try CryptoIdentity.generateNew(commonName: "kv2-pocket")
            let fresh = ClientPairingState(
                clientIdentity: ident.persistedRecord,
                serviceId: nil,
                pinnedServerCertSHA256: nil,
                lastKnownHost: nil,
                lastKnownStreamPort: nil
            )
            guard persistence.save(fresh) else {
                throw ClientPairingStoreError.keychainSaveFailed
            }
            self.state = fresh
            self.clientIdentity = ident

        case .accessDeniedOrLocked(let status):
            // Never generate a new identity or touch the pairing when Keychain access is denied or locked.
            throw ClientPairingStoreError.keychainAccessRequired(deviceKind: normalizedKind, status: status)

        case .corruptedData:
            throw ClientPairingStoreError.corruptedKeychainPairingState
        }

        updatePairedFlagOnQueue()
    }

    deinit {
        stop()
    }

    static func sanitizeDeviceName(_ raw: String?) -> String? {
        guard let raw = raw else { return nil }
        let cleanedScalars = raw.unicodeScalars.map { scalar -> Character in
            if CharacterSet.controlCharacters.contains(scalar) {
                return " "
            }
            return Character(scalar)
        }
        let trimmed = String(cleanedScalars).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        var result = ""
        var utf8Count = 0
        for ch in trimmed {
            let chBytes = String(ch).utf8.count
            if utf8Count + chBytes > 64 { break }
            result.append(ch)
            utf8Count += chBytes
        }
        return result.isEmpty ? nil : result
    }

    static func normalizeDeviceKind(_ raw: String) -> String {
        let lower = raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        switch lower {
        case "iphone", "ipad", "mac", "other":
            return lower
        default:
            return "other"
        }
    }

    private static func monotonicNs() -> UInt64 {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        let ticks = mach_absolute_time()
        return (ticks &* UInt64(info.numer)) / UInt64(max(1, info.denom))
    }

    private func updatePairedFlagOnQueue() {
        telemetry.isPaired = (state.pinnedServerCertSHA256 != nil && state.serviceId != nil)
        telemetry.cachedPairedRole = state.pairedRole
        telemetry.cachedHostProtocolVersion = state.lastKnownHostProtocolVersion
        telemetry.storedOutsideEndpoint = state.outsideEndpoint
        if !telemetry.isPaired {
            telemetry.connectionState = "Unpaired"
        }
    }

    @discardableResult
    private func sendClientJSONOnQueue<T: Encodable>(conn: NWConnection, type: WireMessageType, _ value: T) -> Bool {
        do {
            let pkt = try WirePacket.packClientJSON(type: type, value)
            onOutgoingWirePacketSent?(type, pkt)
            conn.send(content: pkt, completion: .contentProcessed { _ in })
            return true
        } catch {
            telemetry.rejectedOversizeOutgoingFrames += 1
            telemetry.lastCommandError = error.localizedDescription
            publishTelemetryOnQueue()
            return false
        }
    }

    // MARK: - Pairing, Outside Invites & Endpoint Links (`kv2://pair`, `kv2://invite`, `kv2://endpoint`)

    func pairWithQRURI(_ uriString: String, completion: @escaping @Sendable (Result<Void, Error>) -> Void) {
        submitCameraLink(uriString, nowUnixSec: nil, completion: completion)
    }

    /// Handles `kv2://pair?...`, `kv2://invite?...` (§8.4), and `kv2://endpoint?...` (§8.4).
    func submitCameraLink(
        _ rawString: String,
        nowUnixSec: UInt64? = nil,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        let trimmed = rawString.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let parsed = ParsedCameraLink.parse(trimmed) else {
            let msg: String
            if trimmed.hasPrefix("kv2://invite") {
                msg = "Invalid invite link."
            } else if trimmed.hasPrefix("kv2://endpoint") {
                msg = "Invalid camera endpoint link."
            } else {
                msg = "Invalid pairing QR code."
            }
            completion(.failure(NSError(domain: "KinectPocket", code: 1, userInfo: [NSLocalizedDescriptionKey: msg])))
            return
        }

        switch parsed {
        case .lanPair(let qr):
            queue.async {
                guard let nwPort = NWEndpoint.Port(rawValue: qr.pairPort) else {
                    completion(.failure(NSError(domain: "KinectPocket", code: 1, userInfo: [NSLocalizedDescriptionKey: "Invalid pairing QR code."])))
                    return
                }
                let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(qr.hostIP), port: nwPort)
                self.executePairingHandshakeOnQueue(
                    endpoint: endpoint,
                    expectedServerCertSHA256: qr.serverCertSHA256,
                    method: .qrToken,
                    secret: qr.oneTimeToken,
                    explicitHostHint: qr.hostIP,
                    outsideEndpointToPersist: nil,
                    completion: completion
                )
            }

        case .endpoint(let epLink):
            queue.async {
                guard self.matchesStoredPairingOnQueue(serviceId: epLink.serviceId, serverCertSHA256: epLink.serverCertSHA256) else {
                    completion(.failure(NSError(
                        domain: "KinectPocket",
                        code: 9,
                        userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.endpointUnpairedCameraSentence]
                    )))
                    return
                }
                self.probeCandidateOutsideEndpointOnQueue(
                    host: epLink.outsideHost,
                    port: epLink.outsidePort,
                    completion: completion
                )
            }

        case .invite(let invite):
            let effectiveNow = nowUnixSec ?? UInt64(Date().timeIntervalSince1970)
            queue.async {
                // §8.4: If `s` and `fp` match our stored pairing, treat `h:p` as a candidate endpoint WITHOUT redeeming the invite!
                if self.matchesStoredPairingOnQueue(serviceId: invite.serviceId, serverCertSHA256: invite.serverCertSHA256) {
                    self.probeCandidateOutsideEndpointOnQueue(
                        host: invite.outsideHost,
                        port: invite.outsidePort,
                        completion: completion
                    )
                    return
                }

                // Refuse expired invites on device clock with 60 s slack (§8.4).
                guard !invite.isExpired(nowUnixSec: effectiveNow) else {
                    completion(.failure(NSError(
                        domain: "KinectPocket",
                        code: 10,
                        userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.inviteExpiredOrUsedSentence]
                    )))
                    return
                }

                var targets: [(host: String, port: UInt16, isLan: Bool)] = []
                if invite.lanHost != invite.outsideHost {
                    targets.append((invite.lanHost, invite.outsidePort, true))
                }
                targets.append((invite.outsideHost, invite.outsidePort, false))
                self.attemptInviteRedemptionOnQueue(
                    invite: invite,
                    targets: targets,
                    index: 0,
                    completion: completion
                )
            }
        }
    }

    private func matchesStoredPairingOnQueue(serviceId: String, serverCertSHA256: Data) -> Bool {
        guard let storedServiceId = state.serviceId,
              storedServiceId == serviceId,
              let storedPin = state.pinnedServerCertSHA256 else {
            return false
        }
        return CryptoIdentity.constantTimeEqual(storedPin, serverCertSHA256)
    }

    private func attemptInviteRedemptionOnQueue(
        invite: InviteLinkPayload,
        targets: [(host: String, port: UInt16, isLan: Bool)],
        index: Int,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        let target = targets[index]
        guard let nwPort = NWEndpoint.Port(rawValue: target.port) else {
            completion(.failure(NSError(domain: "KinectPocket", code: 11, userInfo: [NSLocalizedDescriptionKey: "Invalid invite port."])))
            return
        }
        let endpoint = target.isLan
            ? NWEndpoint.hostPort(host: NWEndpoint.Host(target.host), port: nwPort)
            : Self.outsideEndpoint(host: target.host, port: nwPort)
        let hasFallbackTarget = (index + 1 < targets.count)

        let sawPeerCertificate = LockedFlag()
        let pinMismatchDetected = LockedFlag()
        let pinnedMatched = LockedFlag()
        let sentRedemptionRequest = LockedFlag()
        let finished = LockedFlag()

        let tlsParams = CryptoIdentity.makeTLSParameters(
            localIdentity: clientIdentity,
            requireClientCert: false,
            verifyQueue: queue
        ) { peerHash, _ in
            _ = sawPeerCertificate.trySet()
            let ok = CryptoIdentity.constantTimeEqual(peerHash, invite.serverCertSHA256)
            if ok {
                _ = pinnedMatched.trySet()
            } else {
                _ = pinMismatchDetected.trySet()
            }
            return ok
        }

        let redeemConn = NWConnection(to: endpoint, using: tlsParams)
        let redeemReader = WireFrameReader()
        let handshakeTimeoutBox = LockedBox<DispatchWorkItem?>(nil)
        let redemptionTimeoutBox = LockedBox<DispatchWorkItem?>(nil)

        // §8.5: Separate 5 s TLS handshake budget per address (cancelled as soon as TLS reaches `.ready`).
        let hsWork = DispatchWorkItem { [weak self] in
            guard let self = self, !sentRedemptionRequest.isSet, finished.trySet() else { return }
            redeemConn.cancel()
            if pinMismatchDetected.isSet {
                completion(.failure(NSError(
                    domain: "KinectPocket",
                    code: 13,
                    userInfo: [NSLocalizedDescriptionKey: "Camera certificate pin mismatch."]
                )))
            } else if sawPeerCertificate.isSet {
                completion(.failure(NSError(
                    domain: "KinectPocket",
                    code: 5,
                    userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.inviteClosedWithoutReplySentence]
                )))
            } else if hasFallbackTarget {
                self.attemptInviteRedemptionOnQueue(
                    invite: invite,
                    targets: targets,
                    index: index + 1,
                    completion: completion
                )
            } else {
                completion(.failure(NSError(
                    domain: "KinectPocket",
                    code: 12,
                    userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.unreachableDefaultPC]
                )))
            }
        }
        handshakeTimeoutBox.set(hsWork)
        queue.asyncAfter(deadline: .now() + inviteHandshakeTimeoutSec, execute: hsWork)

        redeemConn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            switch st {
            case .ready:
                handshakeTimeoutBox.get()?.cancel()
                guard pinnedMatched.isSet else {
                    if finished.trySet() {
                        redeemConn.cancel()
                        completion(.failure(NSError(domain: "KinectPocket", code: 13, userInfo: [NSLocalizedDescriptionKey: "Camera certificate pin mismatch."])))
                    }
                    return
                }
                _ = sentRedemptionRequest.trySet()
                let clientProof = CryptoIdentity.computePairingProof(
                    secret: invite.oneTimeToken,
                    serverCertSHA256: invite.serverCertSHA256,
                    clientCertSHA256: self.clientIdentity.certificateSHA256,
                    role: "kv2-pair-client"
                )
                let req = PairRequestMessage(
                    methodRaw: WirePairingMethod.invite.rawValue,
                    clientCertDER: self.clientIdentity.certificateDER,
                    clientProofHMAC: clientProof,
                    protocolVersion: 3,
                    deviceName: self.deviceName,
                    deviceKind: self.deviceKind
                )
                guard self.sendClientJSONOnQueue(conn: redeemConn, type: .pairRequest, req) else {
                    if finished.trySet() {
                        redeemConn.cancel()
                        completion(.failure(WirePacketError.clientFrameExceeds64KiBCap(frameBytes: WireHeader.maxClientFrameBytes + 1)))
                    }
                    return
                }

                // §8.5: Separate 10 s redemption reply budget once `0x10` is sent.
                let rdWork = DispatchWorkItem {
                    guard finished.trySet() else { return }
                    redeemConn.cancel()
                    completion(.failure(NSError(
                        domain: "KinectPocket",
                        code: 5,
                        userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.inviteClosedWithoutReplySentence]
                    )))
                }
                redemptionTimeoutBox.set(rdWork)
                self.queue.asyncAfter(deadline: .now() + self.inviteRedemptionTimeoutSec, execute: rdWork)

                self.receiveInviteRedemptionReplyOnQueue(
                    conn: redeemConn,
                    reader: redeemReader,
                    invite: invite,
                    finished: finished,
                    redemptionTimeoutBox: redemptionTimeoutBox,
                    completion: completion
                )

            case .waiting(let err), .failed(let err):
                if finished.trySet() {
                    handshakeTimeoutBox.get()?.cancel()
                    redemptionTimeoutBox.get()?.cancel()
                    redeemConn.cancel()
                    let canFallThroughToNextTarget =
                        hasFallbackTarget &&
                        !sentRedemptionRequest.isSet &&
                        !pinMismatchDetected.isSet &&
                        !sawPeerCertificate.isSet &&
                        Self.isTCPConnectUnreachableError(err)
                    if canFallThroughToNextTarget {
                        self.attemptInviteRedemptionOnQueue(
                            invite: invite,
                            targets: targets,
                            index: index + 1,
                            completion: completion
                        )
                    } else if pinMismatchDetected.isSet {
                        completion(.failure(NSError(
                            domain: "KinectPocket",
                            code: 13,
                            userInfo: [NSLocalizedDescriptionKey: "Camera certificate pin mismatch."]
                        )))
                    } else if sentRedemptionRequest.isSet || sawPeerCertificate.isSet || !Self.isTCPConnectUnreachableError(err) {
                        completion(.failure(NSError(
                            domain: "KinectPocket",
                            code: 5,
                            userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.inviteClosedWithoutReplySentence]
                        )))
                    } else {
                        completion(.failure(NSError(
                            domain: "KinectPocket",
                            code: 12,
                            userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.unreachableDefaultPC]
                        )))
                    }
                }
            default:
                break
            }
        }
        redeemConn.start(queue: queue)
    }

    private func receiveInviteRedemptionReplyOnQueue(
        conn redeemConn: NWConnection,
        reader redeemReader: WireFrameReader,
        invite: InviteLinkPayload,
        finished: LockedFlag,
        redemptionTimeoutBox: LockedBox<DispatchWorkItem?>,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        redeemConn.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] data, _, isComplete, err in
            guard let self = self else { return }
            if let data = data, !data.isEmpty {
                redeemReader.append(data)
            }
            switch redeemReader.nextFrame() {
            case .needMoreData:
                if isComplete || err != nil || data == nil {
                    guard finished.trySet() else { return }
                    redemptionTimeoutBox.get()?.cancel()
                    redeemConn.cancel()
                    completion(.failure(NSError(
                        domain: "KinectPocket",
                        code: 5,
                        userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.inviteClosedWithoutReplySentence]
                    )))
                    return
                }
                self.receiveInviteRedemptionReplyOnQueue(
                    conn: redeemConn,
                    reader: redeemReader,
                    invite: invite,
                    finished: finished,
                    redemptionTimeoutBox: redemptionTimeoutBox,
                    completion: completion
                )

            case .protocolViolation, .unknownFrame:
                guard finished.trySet() else { return }
                redemptionTimeoutBox.get()?.cancel()
                redeemConn.cancel()
                completion(.failure(NSError(domain: "KinectPocket", code: 6, userInfo: [NSLocalizedDescriptionKey: "Invalid invite response."])))

            case .frame(let msgType, _, let payload):
                guard finished.trySet() else { return }
                redemptionTimeoutBox.get()?.cancel()
                defer { redeemConn.cancel() }
                guard msgType == .pairResponse,
                      let resp = try? JSONDecoder().decode(PairResponseMessage.self, from: payload) else {
                    completion(.failure(NSError(domain: "KinectPocket", code: 6, userInfo: [NSLocalizedDescriptionKey: "Invalid invite response."])))
                    return
                }
                guard resp.accepted else {
                    completion(.failure(NSError(domain: "KinectPocket", code: 7, userInfo: [NSLocalizedDescriptionKey: resp.message])))
                    return
                }
                guard resp.serviceId == invite.serviceId else {
                    completion(.failure(NSError(domain: "KinectPocket", code: 14, userInfo: [NSLocalizedDescriptionKey: "Invite camera service mismatch."])))
                    return
                }
                let expectedServerProof = CryptoIdentity.computePairingProof(
                    secret: invite.oneTimeToken,
                    serverCertSHA256: invite.serverCertSHA256,
                    clientCertSHA256: self.clientIdentity.certificateSHA256,
                    role: "kv2-pair-server"
                )
                guard CryptoIdentity.constantTimeEqual(resp.serverProofHMAC, expectedServerProof) else {
                    completion(.failure(NSError(domain: "KinectPocket", code: 8, userInfo: [NSLocalizedDescriptionKey: "Server authentication proof mismatch."])))
                    return
                }

                var updated = self.state
                updated.serviceId = resp.serviceId
                updated.pinnedServerCertSHA256 = invite.serverCertSHA256
                updated.lastKnownHost = invite.lanHost
                updated.lastKnownStreamPort = resp.streamPort
                updated.pairedDeviceId = resp.deviceId
                updated.pairedRole = resp.role
                updated.lastKnownHostProtocolVersion = resp.protocolVersion ?? 3
                updated.outsideEndpoint = PetEndpoint(
                    host: invite.outsideHost,
                    port: invite.outsidePort,
                    verified: true
                )
                guard self.store.save(updated) else {
                    completion(.failure(ClientPairingStoreError.keychainSaveFailed))
                    return
                }
                self.state = updated

                self.telemetry.revokedNotice = nil
                self.telemetry.tlsRefusalNotice = nil
                self.telemetry.lastSessionNotice = nil
                self.telemetry.sawAddressChangeNotice = false
                self.telemetry.lastCommandError = nil
                self.telemetry.reconnectCount = 0
                self.updatePairedFlagOnQueue()
                self.publishTelemetryOnQueue()

                completion(.success(()))
                self.shouldStayConnected = true
                self.startStreamingOnQueue()
            }
        }
    }

    /// Cancels and fails any in-flight candidate endpoint probe so an untested `h:p` can never be saved later.
    private func cancelPendingCandidateProbeOnQueue() {
        probeTimeoutWork?.cancel()
        probeTimeoutWork = nil
        pendingSessionConfirmedCallback = nil
        if let failHandler = activeCandidateProbeFailureHandler {
            activeCandidateProbeFailureHandler = nil
            failHandler()
        }
    }

    /// Tests a candidate `h:p` endpoint pinned to our stored `pinnedServerCertSHA256` (§8.4).
    /// Replaces `state.outsideEndpoint` ONLY after a handshake at `h:p` succeeds and produces a real session (`0x0B`/`0x02`).
    private func probeCandidateOutsideEndpointOnQueue(
        host: String,
        port: UInt16,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        guard let nwPort = NWEndpoint.Port(rawValue: port) else {
            completion(.failure(NSError(
                domain: "KinectPocket",
                code: 15,
                userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.candidateLinkUnreachableSentence]
            )))
            return
        }
        cancelPendingCandidateProbeOnQueue()
        let wasStayingConnected = shouldStayConnected
        startRequestedDuringProbe = false
        reconnectWork?.cancel()
        reconnectWork = nil
        lanFallbackWork?.cancel()
        lanFallbackWork = nil
        browser?.cancel()
        browser = nil

        let candidateEp = Self.outsideEndpoint(host: host, port: nwPort)
        let probeDone = LockedFlag()

        let finishProbe: @Sendable (Bool, Bool) -> Void = { [weak self] succeeded, allowReconnect in
            guard let self = self, probeDone.trySet() else { return }
            self.probeTimeoutWork?.cancel()
            self.probeTimeoutWork = nil
            self.pendingSessionConfirmedCallback = nil
            self.activeCandidateProbeFailureHandler = nil
            let startWasRequested = self.startRequestedDuringProbe
            self.startRequestedDuringProbe = false

            if succeeded {
                var updated = self.state
                updated.outsideEndpoint = PetEndpoint(host: host, port: port, verified: true)
                _ = self.store.save(updated)
                self.state = updated
                self.telemetry.storedOutsideEndpoint = updated.outsideEndpoint
                self.telemetry.sawAddressChangeNotice = false
                self.telemetry.lastError = nil
                self.shouldStayConnected = true
                self.publishTelemetryOnQueue()
                completion(.success(()))
            } else {
                // Keep old `state.outsideEndpoint` untouched!
                self.teardownConnectionOnQueue()
                self.telemetry.lastError = PetCameraUserStrings.candidateLinkUnreachableSentence
                self.publishTelemetryOnQueue()
                if allowReconnect && (wasStayingConnected || startWasRequested) {
                    self.shouldStayConnected = true
                    self.scheduleReconnectOnQueue()
                }
                completion(.failure(NSError(
                    domain: "KinectPocket",
                    code: 16,
                    userInfo: [NSLocalizedDescriptionKey: PetCameraUserStrings.candidateLinkUnreachableSentence]
                )))
            }
        }

        activeCandidateProbeFailureHandler = {
            finishProbe(false, false)
        }

        let tWork = DispatchWorkItem {
            finishProbe(false, true)
        }
        probeTimeoutWork = tWork
        queue.asyncAfter(deadline: .now() + candidateProbeTimeoutSec, execute: tWork)

        pendingSessionConfirmedCallback = { accepted, _ in
            finishProbe(accepted, true)
        }

        connectToStreamEndpointOnQueue(
            endpoint: candidateEp,
            origin: .outside,
            isAutomaticLanFirst: false,
            pinnedHashOverride: nil,
            onHandshakeResult: { accepted, _ in
                if !accepted {
                    finishProbe(false, true)
                }
            }
        )
    }

    func pairWithShortCode(_ rawCode: String, hostOverride: String? = nil, portOverride: UInt16? = nil, completion: @escaping @Sendable (Result<Void, Error>) -> Void) {
        let digits = QRPairingPayload.normalizeShortCode(rawCode)
        guard digits.count == 8 else {
            completion(.failure(NSError(domain: "KinectPocket", code: 2, userInfo: [NSLocalizedDescriptionKey: "Enter the 8-digit pairing code shown on your camera."])))
            return
        }
        queue.async {
            if let h = hostOverride, let p = portOverride, let nwPort = NWEndpoint.Port(rawValue: p) {
                let ep = NWEndpoint.hostPort(host: NWEndpoint.Host(h), port: nwPort)
                self.executePairingHandshakeOnQueue(
                    endpoint: ep,
                    expectedServerCertSHA256: nil,
                    method: .shortCode,
                    secret: Data(digits.utf8),
                    explicitHostHint: h,
                    outsideEndpointToPersist: nil,
                    completion: completion
                )
                return
            }

            // Discover active pairing endpoint via Bonjour _kv2p._tcp
            self.telemetry.connectionState = "Searching for Camera Pairing…"
            self.publishTelemetryOnQueue()

            let browseDesc = NWBrowser.Descriptor.bonjour(type: BonjourConstants.pairingServiceType, domain: BonjourConstants.domain)
            let tempBrowser = NWBrowser(for: browseDesc, using: .tcp)
            let finished = LockedFlag()

            tempBrowser.browseResultsChangedHandler = { [weak self] results, _ in
                guard let self = self, let first = results.first else { return }
                guard finished.trySet() else { return }
                tempBrowser.cancel()
                self.executePairingHandshakeOnQueue(
                    endpoint: first.endpoint,
                    expectedServerCertSHA256: nil,
                    method: .shortCode,
                    secret: Data(digits.utf8),
                    explicitHostHint: nil,
                    outsideEndpointToPersist: nil,
                    completion: completion
                )
            }
            tempBrowser.start(queue: self.queue)

            self.queue.asyncAfter(deadline: .now() + 8.0) {
                guard finished.trySet() else { return }
                tempBrowser.cancel()
                completion(.failure(NSError(domain: "KinectPocket", code: 3, userInfo: [NSLocalizedDescriptionKey: "No camera in pairing mode found on Local Network."])))
            }
        }
    }

    private func executePairingHandshakeOnQueue(
        endpoint: NWEndpoint,
        expectedServerCertSHA256: Data?,
        method: WirePairingMethod,
        secret: Data,
        explicitHostHint: String?,
        outsideEndpointToPersist: PetEndpoint?,
        completion: @escaping @Sendable (Result<Void, Error>) -> Void
    ) {
        let capturedServerHash = LockedBox<Data?>(nil)
        let tlsParams = CryptoIdentity.makeTLSParameters(
            localIdentity: clientIdentity,
            requireClientCert: false,
            verifyQueue: queue
        ) { peerHash, _ in
            if let expected = expectedServerCertSHA256 {
                guard CryptoIdentity.constantTimeEqual(peerHash, expected) else {
                    return false
                }
            }
            capturedServerHash.set(peerHash)
            return true
        }

        let pairConn = NWConnection(to: endpoint, using: tlsParams)
        let pairReader = WireFrameReader()
        let done = LockedFlag()

        pairConn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            switch st {
            case .ready:
                guard let serverHash = capturedServerHash.get() else {
                    if done.trySet() {
                        pairConn.cancel()
                        completion(.failure(NSError(domain: "KinectPocket", code: 4, userInfo: [NSLocalizedDescriptionKey: "Missing server certificate."])))
                    }
                    return
                }
                let clientProof = CryptoIdentity.computePairingProof(
                    secret: secret,
                    serverCertSHA256: serverHash,
                    clientCertSHA256: self.clientIdentity.certificateSHA256,
                    role: "kv2-pair-client"
                )
                let req = PairRequestMessage(
                    methodRaw: method.rawValue,
                    clientCertDER: self.clientIdentity.certificateDER,
                    clientProofHMAC: clientProof,
                    protocolVersion: 3,
                    deviceName: self.deviceName,
                    deviceKind: self.deviceKind
                )
                guard self.sendClientJSONOnQueue(conn: pairConn, type: .pairRequest, req) else {
                    if done.trySet() {
                        pairConn.cancel()
                        completion(.failure(WirePacketError.clientFrameExceeds64KiBCap(frameBytes: WireHeader.maxClientFrameBytes + 1)))
                    }
                    return
                }

                pairConn.receive(minimumIncompleteLength: 1, maximumLength: 16384) { data, _, _, err in
                    guard done.trySet() else { return }
                    defer { pairConn.cancel() }
                    guard err == nil, let data = data else {
                        completion(.failure(NSError(domain: "KinectPocket", code: 5, userInfo: [NSLocalizedDescriptionKey: "Pairing connection closed."])))
                        return
                    }
                    pairReader.append(data)
                    guard case .frame(let msgType, _, let payload) = pairReader.nextFrame(),
                          msgType == .pairResponse,
                          let resp = try? JSONDecoder().decode(PairResponseMessage.self, from: payload) else {
                        completion(.failure(NSError(domain: "KinectPocket", code: 6, userInfo: [NSLocalizedDescriptionKey: "Invalid pairing response."])))
                        return
                    }
                    guard resp.accepted else {
                        completion(.failure(NSError(domain: "KinectPocket", code: 7, userInfo: [NSLocalizedDescriptionKey: resp.message])))
                        return
                    }

                    let expectedServerProof = CryptoIdentity.computePairingProof(
                        secret: secret,
                        serverCertSHA256: serverHash,
                        clientCertSHA256: self.clientIdentity.certificateSHA256,
                        role: "kv2-pair-server"
                    )
                    guard CryptoIdentity.constantTimeEqual(resp.serverProofHMAC, expectedServerProof) else {
                        completion(.failure(NSError(domain: "KinectPocket", code: 8, userInfo: [NSLocalizedDescriptionKey: "Server authentication proof mismatch."])))
                        return
                    }

                    let sameCamera = (self.state.serviceId == resp.serviceId && self.state.pinnedServerCertSHA256 == serverHash)
                    var updated = self.state
                    updated.serviceId = resp.serviceId
                    updated.pinnedServerCertSHA256 = serverHash
                    updated.lastKnownHost = explicitHostHint
                    updated.lastKnownStreamPort = resp.streamPort
                    updated.pairedDeviceId = resp.deviceId
                    updated.pairedRole = resp.role
                    updated.lastKnownHostProtocolVersion = resp.protocolVersion
                    if let explicitOutside = outsideEndpointToPersist {
                        updated.outsideEndpoint = explicitOutside
                    } else if !sameCamera {
                        updated.outsideEndpoint = nil
                        updated.outsideCandidates = nil
                    }
                    guard self.store.save(updated) else {
                        completion(.failure(ClientPairingStoreError.keychainSaveFailed))
                        return
                    }
                    self.state = updated

                    self.telemetry.revokedNotice = nil
                    self.telemetry.tlsRefusalNotice = nil
                    self.telemetry.lastSessionNotice = nil
                    self.telemetry.sawAddressChangeNotice = false
                    self.telemetry.lastCommandError = nil
                    self.telemetry.reconnectCount = 0
                    self.updatePairedFlagOnQueue()
                    self.publishTelemetryOnQueue()

                    completion(.success(()))
                    self.shouldStayConnected = true
                    self.startStreamingOnQueue()
                }

            case .failed(let err):
                if done.trySet() {
                    pairConn.cancel()
                    completion(.failure(err))
                }
            default:
                break
            }
        }
        pairConn.start(queue: queue)
    }

    // MARK: - Live Encrypted Mutually-Authenticated Stream

    /// Starts a fresh connect cycle. On a live session this tears it down and reconnects (a manual reconnect);
    /// coming back from a brief interruption uses `resume()` instead.
    func start() {
        queue.async { self.startOnQueue() }
    }

    /// Coming back active (Control Center, a notification, the app switcher): a live session is kept as it is.
    /// `start()` would tear it down and reconnect, so for a moment the live stream would drop and the app would
    /// read disconnected and lose its owner state. With no live session (after backgrounding, `stop()` already ran)
    /// this is the same as `start()`.
    ///
    /// A session that died silently while the app was inactive (a NAT timeout, the PC restarting without a FIN) must
    /// not be kept as a stale "connected". Both hosts answer every ping (sent every 250 ms) with a pong, so a live
    /// session never goes more than about a quarter second without inbound bytes. The session is kept only if
    /// something arrived within `resumeMaxInboundSilenceSec`, and that is checked once more the same interval
    /// after resuming. If the line is silent either time, it reconnects at once.
    func resume() {
        queue.async {
            guard self.shouldStayConnected, let conn = self.connection, self.receivedServerHello,
                  self.telemetry.isConnected else {
                self.startOnQueue()
                return
            }
            guard self.inboundIsFreshOnQueue() else {
                self.onResumeFoundSilentSessionForTesting?()
                self.startOnQueue()
                return
            }
            self.onResumeKeptLiveSessionForTesting?()
            let silenceSec = self.resumeMaxInboundSilenceSec
            self.queue.asyncAfter(deadline: .now() + silenceSec) { [weak self] in
                guard let self = self, self.connection === conn, self.shouldStayConnected else { return }
                if !self.inboundIsFreshOnQueue() {
                    self.onResumeFoundSilentSessionForTesting?()
                    self.startOnQueue()
                }
            }
        }
    }

    /// Inbound silence after which a resumed session counts as dead (pongs arrive every ~250 ms on a live one).
    var resumeMaxInboundSilenceSec: Double = 2.0
    private var lastInboundMonotonicNs: UInt64 = 0

    private func inboundIsFreshOnQueue() -> Bool {
        let now = Self.monotonicNs()
        guard now >= lastInboundMonotonicNs else { return true }
        return Double(now - lastInboundMonotonicNs) / 1_000_000_000 <= resumeMaxInboundSilenceSec
    }

    var onResumeKeptLiveSessionForTesting: (@Sendable () -> Void)?
    var onResumeFoundSilentSessionForTesting: (@Sendable () -> Void)?
    /// Test only: from now until the next connection, the session counts as silent (as if it died quietly),
    /// even though the loopback host keeps answering.
    func simulateInboundSilenceForTesting() {
        queue.sync {
            self.inboundSilenceSimulatedForTesting = true
            self.lastInboundMonotonicNs = 0
        }
    }
    private var inboundSilenceSimulatedForTesting = false

    private func startOnQueue() {
        guard state.pinnedServerCertSHA256 != nil, state.serviceId != nil else {
            updatePairedFlagOnQueue()
            publishTelemetryOnQueue()
            return
        }
        // A pasted link's endpoint probe survives brief interruptions (Control Center, a second `start()`):
        // only a real stop (`stop()` on backgrounding or screen capture, or `unpair()`) cancels it.
        if isCandidateProbeInFlightOnQueue {
            startRequestedDuringProbe = true
            onStartLeftProbeAloneForTesting?()
            return
        }
        shouldStayConnected = true
        if !disableBonjourForTesting { startPathMonitorOnQueue() }
        startStreamingOnQueue()
    }

    /// True while a candidate `h:p` endpoint probe started by a pasted link is still waiting for its result.
    private var isCandidateProbeInFlightOnQueue: Bool {
        activeCandidateProbeFailureHandler != nil
    }

    func stop() {
        let teardownBlock = {
            self.shouldStayConnected = false
            self.stopPathMonitorOnQueue()
            self.lanFallbackWork?.cancel()
            self.lanFallbackWork = nil
            self.cancelPendingCandidateProbeOnQueue()
            self.teardownConnectionOnQueue()
            self.browser?.cancel()
            self.browser = nil
            self.reconnectWork?.cancel()
            self.reconnectWork = nil
        }
        if DispatchQueue.getSpecific(key: Self.queueKey) != nil {
            teardownBlock()
        } else {
            queue.sync(execute: teardownBlock)
        }
    }

    func unpair() {
        queue.async {
            self.shouldStayConnected = false
            self.stopPathMonitorOnQueue()
            self.lanFallbackWork?.cancel()
            self.lanFallbackWork = nil
            self.cancelPendingCandidateProbeOnQueue()
            self.teardownConnectionOnQueue()
            self.browser?.cancel()
            self.browser = nil
            self.reconnectWork?.cancel()
            self.reconnectWork = nil
            self.store.wipe()
            if let freshIdent = try? CryptoIdentity.generateNew(commonName: "kv2-pocket") {
                self.clientIdentity = freshIdent
                self.state = ClientPairingState(
                    clientIdentity: freshIdent.persistedRecord,
                    serviceId: nil,
                    pinnedServerCertSHA256: nil,
                    lastKnownHost: nil,
                    lastKnownStreamPort: nil
                )
                _ = self.store.save(self.state)
            }
            self.telemetry.revokedNotice = nil
            self.telemetry.tlsRefusalNotice = nil
            self.telemetry.lastSessionNotice = nil
            self.telemetry.sawAddressChangeNotice = false
            self.telemetry.lastSawSharingEnabled = nil
            self.telemetry.lastSawOutsideEnabled = nil
            self.telemetry.lastCommandError = nil
            // The probe cancelled above reports "That link didn't reach your camera." on its way out; that sentence
            // belongs to the camera we just forgot, so it must not reach the fresh pairing sheet.
            self.telemetry.lastError = nil
            self.telemetry.reconnectCount = 0
            self.telemetry.hostState = .unconfirmedInitial
            self.telemetry.petState = .unconfirmedInitial
            self.updatePairedFlagOnQueue()
            self.onStreamFlushed?()
            self.onPlaybackStreamFlushed?()
            self.publishTelemetryOnQueue()
        }
    }

    private var isConfirmedNonOwnerV3OnQueue: Bool {
        guard telemetry.isPetCameraV3 else { return false }
        if telemetry.hasConfirmedPetCameraState {
            return telemetry.petState.role != .owner
        }
        return telemetry.cachedPairedRole == .viewer
    }

    func requestModeSwitch(modeRaw: Int, sbsLayoutRaw: Int = 0) {
        queue.async {
            guard !self.isConfirmedNonOwnerV3OnQueue,
                  self.telemetry.allowModeSwitch,
                  let conn = self.connection,
                  self.telemetry.isConnected else { return }
            // Against a v1 host, only v1 modes (0..3) exist; never send Mode 5 (modeRaw == 4).
            if self.telemetry.protocolVersion < 2 && modeRaw > 3 {
                return
            }
            let msg = SelectModeMessage(modeRaw: modeRaw, sbsLayoutRaw: sbsLayoutRaw)
            self.sendClientJSONOnQueue(conn: conn, type: .selectModeReq, msg)
        }
    }

    func sendRemoteControl(_ cmd: RemoteControlCommandMessage) {
        queue.async {
            guard self.telemetry.protocolVersion >= 2,
                  let conn = self.connection,
                  self.telemetry.isConnected else { return }
            if cmd.action != .requestKeyframe {
                guard !self.isConfirmedNonOwnerV3OnQueue, self.telemetry.allowModeSwitch else {
                    return
                }
            }
            self.sendClientJSONOnQueue(conn: conn, type: .remoteControlCmd, cmd)
        }
    }

    func sendOrbit3DGesture(_ gesture: Orbit3DGestureMessage) {
        queue.async {
            guard self.telemetry.protocolVersion >= 2,
                  !self.isConfirmedNonOwnerV3OnQueue,
                  self.telemetry.allowModeSwitch,
                  let conn = self.connection,
                  self.telemetry.isConnected else { return }
            self.sendClientJSONOnQueue(conn: conn, type: .orbit3DGesture, gesture)
        }
    }

    /// Amended spec Option A: a v3 owner may send `0x0A` whenever `allowPhoneSensorPower` is true,
    /// even while `liveAllowed == false` (for example during privacy mode or when 4 owners are watching).
    func requestSensorPower(turnOn: Bool) {
        queue.async {
            guard !self.isConfirmedNonOwnerV3OnQueue else { return }
            let allowPower = !self.telemetry.hasConfirmedHostState || self.telemetry.hostState.allowPhoneSensorPower
            guard self.telemetry.protocolVersion >= 2,
                  allowPower,
                  let conn = self.connection,
                  self.telemetry.isConnected else { return }
            self.nextPowerRequestId &+= 1
            let req = SensorPowerControlMessage(requestId: self.nextPowerRequestId, turnOn: turnOn)
            self.sendClientJSONOnQueue(conn: conn, type: .sensorPowerReq, req)
        }
    }

    // MARK: - Protocol v3 Live View, Camera Roll, Playback & Commands (`0x0C`–`0x19`)

    func setLiveQuality(_ quality: LiveQuality) {
        guard quality != .unknown else { return }
        setLiveWatching(true, quality: quality)
    }

    func setLiveWatching(_ watch: Bool, quality: LiveQuality? = nil) {
        queue.async {
            guard self.telemetry.isConnected,
                  self.telemetry.isPetCameraV3,
                  self.sentV3PingInCurrentSession,
                  let conn = self.connection else { return }
            if let q = quality, q == .unknown {
                return
            }
            let req = LiveViewRequestMessage(watch: watch, quality: quality?.rawValue)
            self.sendClientJSONOnQueue(conn: conn, type: .liveViewReq, req)
        }
    }

    func sendPetCameraCommand(
        action: PetCameraCommandAction,
        enabled: Bool? = nil,
        sensitivity: Int? = nil,
        clipId: String? = nil,
        starred: Bool? = nil,
        deviceId: String? = nil
    ) {
        sendPetCameraCommandWithResult(
            action: action,
            enabled: enabled,
            sensitivity: sensitivity,
            clipId: clipId,
            starred: starred,
            deviceId: deviceId,
            completion: nil
        )
    }

    func sendPetCameraCommandWithResult(
        action: PetCameraCommandAction,
        enabled: Bool? = nil,
        sensitivity: Int? = nil,
        clipId: String? = nil,
        starred: Bool? = nil,
        deviceId: String? = nil,
        completion: (@Sendable (Result<CommandResultMessage, CommandResultError>) -> Void)?
    ) {
        queue.async {
            guard self.telemetry.isConnected,
                  self.telemetry.isPetCameraV3,
                  self.sentV3PingInCurrentSession,
                  let conn = self.connection else {
                completion?(.failure(CommandResultError(code: "disconnected", message: "Not connected to the camera.")))
                return
            }
            if action.requiresOwnerRole {
                guard self.telemetry.hasConfirmedPetCameraState,
                      self.telemetry.petState.role == .owner else {
                    completion?(.failure(CommandResultError(code: "not_allowed_for_role", message: "Only an owner can change this setting.")))
                    return
                }
            }
            if action == .revokeDevice {
                guard let target = deviceId,
                      !target.isEmpty,
                      target != self.telemetry.petState.deviceId else {
                    completion?(.failure(CommandResultError(code: "invalid", message: "Invalid device selection.")))
                    return
                }
            }
            let clampedSensitivity = sensitivity.map { min(10, max(1, $0)) }
            self.nextPetCommandRequestId &+= 1
            let reqId = self.nextPetCommandRequestId
            let cmd = PetCameraCommandMessage(
                requestId: reqId,
                action: action.rawValue,
                enabled: enabled,
                sensitivity: clampedSensitivity,
                clipId: clipId,
                starred: starred,
                deviceId: deviceId
            )
            if let completion = completion {
                self.pendingCommandCallbacks[reqId] = completion
                self.scheduleRequestTimeoutOnQueue(requestId: reqId)
            }
            guard self.sendClientJSONOnQueue(conn: conn, type: .petCameraCmd, cmd) else {
                let cb = self.pendingCommandCallbacks.removeValue(forKey: reqId)
                cb?(.failure(CommandResultError(code: "oversize", message: "Request exceeded the 64 KiB cap.")))
                return
            }
        }
    }

    func requestRollList(
        day: String? = nil,
        cursor: String? = nil,
        limit: Int = RollListRequestMessage.defaultLimit,
        completion: @escaping @Sendable (Result<RollListMessage, CommandResultError>) -> Void
    ) {
        queue.async {
            guard self.telemetry.isConnected,
                  self.telemetry.isPetCameraV3,
                  self.sentV3PingInCurrentSession,
                  let conn = self.connection else {
                completion(.failure(CommandResultError(code: "disconnected", message: "Not connected to the camera.")))
                return
            }
            self.nextPetCommandRequestId &+= 1
            let reqId = self.nextPetCommandRequestId
            // `RollListRequestMessage` clamps `limit` strictly to `1...100` (§5.6).
            let req = RollListRequestMessage(requestId: reqId, day: day, cursor: cursor, limit: limit)
            self.pendingRollListCallbacks[reqId] = completion
            self.scheduleRequestTimeoutOnQueue(requestId: reqId)
            guard self.sendClientJSONOnQueue(conn: conn, type: .rollListReq, req) else {
                let cb = self.pendingRollListCallbacks.removeValue(forKey: reqId)
                cb?(.failure(CommandResultError(code: "oversize", message: "Request exceeded the 64 KiB cap.")))
                return
            }
        }
    }

    func requestRollThumbnail(
        clipId: String,
        completion: @escaping @Sendable (Result<RollThumbMessage, CommandResultError>) -> Void
    ) {
        queue.async {
            guard self.telemetry.isConnected,
                  self.telemetry.isPetCameraV3,
                  self.sentV3PingInCurrentSession,
                  let conn = self.connection else {
                completion(.failure(CommandResultError(code: "disconnected", message: "Not connected to the camera.")))
                return
            }
            self.nextPetCommandRequestId &+= 1
            let reqId = self.nextPetCommandRequestId
            let req = RollThumbRequestMessage(requestId: reqId, clipId: clipId)
            self.pendingRollThumbCallbacks[reqId] = completion
            self.scheduleRequestTimeoutOnQueue(requestId: reqId)
            guard self.sendClientJSONOnQueue(conn: conn, type: .rollThumbReq, req) else {
                let cb = self.pendingRollThumbCallbacks.removeValue(forKey: reqId)
                cb?(.failure(CommandResultError(code: "oversize", message: "Request exceeded the 64 KiB cap.")))
                return
            }
        }
    }

    func sendPlaybackRequest(
        action: PlaybackAction,
        clipId: String? = nil,
        streamId: UInt32? = nil,
        positionSec: Double? = nil,
        completion: (@Sendable (Result<PlaybackStateMessage, CommandResultError>) -> Void)? = nil
    ) {
        queue.async {
            guard self.telemetry.isConnected,
                  self.telemetry.isPetCameraV3,
                  self.sentV3PingInCurrentSession,
                  let conn = self.connection else {
                completion?(.failure(CommandResultError(code: "disconnected", message: "Not connected to the camera.")))
                return
            }
            let effectiveStreamId: UInt32?
            if action == .start {
                effectiveStreamId = nil
            } else {
                effectiveStreamId = streamId ?? self.playbackStreamId
                guard effectiveStreamId != nil else {
                    completion?(.failure(CommandResultError(code: "not_found", message: "No active clip playback stream.")))
                    return
                }
            }
            self.nextPetCommandRequestId &+= 1
            let reqId = self.nextPetCommandRequestId
            let req = PlaybackRequestMessage(
                requestId: reqId,
                action: action,
                clipId: action == .start ? clipId : nil,
                streamId: effectiveStreamId,
                positionSec: (action == .start || action == .seek) ? positionSec : nil
            )
            if let completion = completion {
                self.pendingPlaybackCallbacks[reqId] = completion
                self.scheduleRequestTimeoutOnQueue(requestId: reqId)
            }
            guard self.sendClientJSONOnQueue(conn: conn, type: .playbackReq, req) else {
                let cb = self.pendingPlaybackCallbacks.removeValue(forKey: reqId)
                cb?(.failure(CommandResultError(code: "oversize", message: "Request exceeded the 64 KiB cap.")))
                return
            }
        }
    }

    func stopPlaybackAndResumeLiveIfNeeded(resumeLive: Bool) {
        queue.async {
            if let activeSid = self.playbackStreamId,
               self.telemetry.isConnected,
               self.telemetry.isPetCameraV3,
               self.sentV3PingInCurrentSession,
               let conn = self.connection {
                self.nextPetCommandRequestId &+= 1
                let stopReq = PlaybackRequestMessage(
                    requestId: self.nextPetCommandRequestId,
                    action: .stop,
                    streamId: activeSid
                )
                self.sendClientJSONOnQueue(conn: conn, type: .playbackReq, stopReq)
            }
            self.playbackStreamId = nil
            self.playbackClipId = nil
            self.playbackFormatDesc = nil
            self.playbackStreamEpoch = 0
            self.telemetry.latestPlaybackState = nil
            self.onPlaybackStreamFlushed?()
            self.onPlaybackStateUpdated?(nil)
            self.publishTelemetryOnQueue()

            if resumeLive,
               self.telemetry.isConnected,
               self.telemetry.isPetCameraV3,
               self.sentV3PingInCurrentSession,
               let conn = self.connection {
                let watchReq = LiveViewRequestMessage(watch: true, quality: nil)
                self.sendClientJSONOnQueue(conn: conn, type: .liveViewReq, watchReq)
            }
        }
    }

    private func scheduleRequestTimeoutOnQueue(requestId: UInt64) {
        let timeout = requestTimeoutSec
        queue.asyncAfter(deadline: .now() + timeout) { [weak self] in
            guard let self = self else { return }
            let timeoutErr = CommandResultError(code: "timeout", message: "The camera didn't answer in time.")
            if let cb = self.pendingRollThumbCallbacks.removeValue(forKey: requestId) {
                cb(.failure(timeoutErr))
            }
            if let cb = self.pendingRollListCallbacks.removeValue(forKey: requestId) {
                cb(.failure(timeoutErr))
            }
            if let cb = self.pendingPlaybackCallbacks.removeValue(forKey: requestId) {
                cb(.failure(timeoutErr))
            }
            if let cb = self.pendingCommandCallbacks.removeValue(forKey: requestId) {
                cb(.failure(timeoutErr))
            }
        }
    }

    private func failAllPendingRequestsOnQueue(reason: String) {
        let err = CommandResultError(code: "disconnected", message: reason)
        let thumbs = pendingRollThumbCallbacks
        let lists = pendingRollListCallbacks
        let playbacks = pendingPlaybackCallbacks
        let cmds = pendingCommandCallbacks
        pendingRollThumbCallbacks.removeAll()
        pendingRollListCallbacks.removeAll()
        pendingPlaybackCallbacks.removeAll()
        pendingCommandCallbacks.removeAll()
        for (_, cb) in thumbs { cb(.failure(err)) }
        for (_, cb) in lists { cb(.failure(err)) }
        for (_, cb) in playbacks { cb(.failure(err)) }
        for (_, cb) in cmds { cb(.failure(err)) }
    }

    /// Test helper verifying the 64 KiB outgoing frame cap on the client's send path.
    func sendRawClientPayloadForTesting(type: WireMessageType, payload: Data) -> Bool {
        queue.sync {
            do {
                let pkt = try WirePacket.packClientToHost(type: type, payload: payload)
                if let conn = self.connection, self.telemetry.isConnected {
                    self.onOutgoingWirePacketSent?(type, pkt)
                    conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
                return true
            } catch {
                self.telemetry.rejectedOversizeOutgoingFrames += 1
                self.telemetry.lastCommandError = error.localizedDescription
                self.publishTelemetryOnQueue()
                return false
            }
        }
    }

    /// Connect directly to a known host/port (also used by security tests to verify unpaired TLS rejection).
    func connectDirect(host: String, port: UInt16, pinnedServerHashOverride: Data? = nil, onHandshakeResult: (@Sendable (Bool, String?) -> Void)? = nil) {
        queue.async {
            guard let nwPort = NWEndpoint.Port(rawValue: port) else { return }
            let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: nwPort)
            self.connectToStreamEndpointOnQueue(
                endpoint: endpoint,
                origin: .home,
                isAutomaticLanFirst: false,
                pinnedHashOverride: pinnedServerHashOverride,
                onHandshakeResult: onHandshakeResult
            )
        }
    }

    private func startStreamingOnQueue() {
        cancelPendingCandidateProbeOnQueue()
        guard let targetServiceId = state.serviceId,
              state.pinnedServerCertSHA256 != nil else {
            telemetry.connectionState = "Unpaired"
            publishTelemetryOnQueue()
            return
        }

        lanFallbackWork?.cancel()
        lanFallbackWork = nil
        teardownConnectionOnQueue()
        browser?.cancel()
        browser = nil
        triedOutsideInCurrentCycle = false
        pendingOutsideCandidates = []
        connectCycleId &+= 1

        let hasOutsideEndpoint = hasAnyOutsideAddressOnQueue
        if telemetry.tlsRefusalNotice == nil && telemetry.lastSessionNotice == nil {
            telemetry.connectionState = ((state.lastKnownHostProtocolVersion ?? 1) >= 3 || hasOutsideEndpoint)
                ? "Searching for Camera…"
                : "Searching for Mac…"
        }
        publishTelemetryOnQueue()

        if !disableBonjourForTesting {
            let desc = NWBrowser.Descriptor.bonjour(type: BonjourConstants.streamServiceType, domain: BonjourConstants.domain)
            let b = NWBrowser(for: desc, using: .tcp)
            b.browseResultsChangedHandler = { [weak self] results, _ in
                guard let self = self, self.shouldStayConnected, self.connection == nil else { return }
                for res in results {
                    if case let .service(name: name, type: _, domain: _, interface: _) = res.endpoint,
                       name == targetServiceId {
                        self.lanFallbackWork?.cancel()
                        self.lanFallbackWork = nil
                        self.connectToStreamEndpointOnQueue(
                            endpoint: res.endpoint,
                            origin: .home,
                            isAutomaticLanFirst: true,
                            pinnedHashOverride: nil,
                            onHandshakeResult: nil
                        )
                        break
                    }
                }
            }
            self.browser = b
            b.start(queue: queue)
        }

        // §8.4: Always try LAN first (`lastKnownHost:lastKnownStreamPort` + Bonjour `_kv2s._tcp`), then fall back to `outsideEndpoint`.
        if let h = state.lastKnownHost, let p = state.lastKnownStreamPort, p > 0, let nwPort = NWEndpoint.Port(rawValue: p) {
            let ep = NWEndpoint.hostPort(host: NWEndpoint.Host(h), port: nwPort)
            connectToStreamEndpointOnQueue(
                endpoint: ep,
                origin: .home,
                isAutomaticLanFirst: true,
                pinnedHashOverride: nil,
                onHandshakeResult: nil
            )
            return
        }

        if hasOutsideEndpoint {
            if disableBonjourForTesting || lanDiscoveryGraceBeforeOutsideSec <= 0 {
                _ = tryOutsideFallbackInCurrentCycleOnQueue()
            } else {
                let work = DispatchWorkItem { [weak self] in
                    guard let self = self, self.shouldStayConnected, self.connection == nil else { return }
                    _ = self.tryOutsideFallbackInCurrentCycleOnQueue()
                }
                lanFallbackWork = work
                queue.asyncAfter(deadline: .now() + lanDiscoveryGraceBeforeOutsideSec, execute: work)
            }
        }
    }

    @discardableResult
    private func tryOutsideFallbackInCurrentCycleOnQueue() -> Bool {
        guard shouldStayConnected, !triedOutsideInCurrentCycle else { return false }
        // The typed/router address first, then any extra candidates (e.g. Tailscale), each host once.
        var hosts: [PetEndpoint] = []
        if let ep = state.outsideEndpoint { hosts.append(ep) }
        let vpnUp = vpnUpOverrideForTesting ?? vpnInterfaceUp
        for c in state.outsideCandidates ?? [] where !hosts.contains(where: { $0.host == c.host && $0.port == c.port }) {
            // A VPN-range candidate (e.g. Tailscale 100.x) only while a VPN interface is up; otherwise the phone runs
            // exactly the public path, and the row says why the candidate was left out.
            if Self.isVPNRangeIPv4(c.host) && !vpnUp {
                if let p = NWEndpoint.Port(rawValue: c.port) {
                    let id = recordConnectAttemptOnQueue(origin: .outside, endpoint: Self.outsideEndpoint(host: c.host, port: p))
                    updateConnectAttemptOnQueue(id, result: "skipped (no VPN)")
                }
                continue
            }
            hosts.append(c)
        }
        guard let outsideEp = hosts.first, let nwPort = NWEndpoint.Port(rawValue: outsideEp.port) else { return false }
        triedOutsideInCurrentCycle = true
        pendingOutsideCandidates = []
        let host = outsideEp.host
        let port = outsideEp.port
        let extraHosts = Array(hosts.dropFirst())
        let resolver = resolveOutsideCandidatesForTesting
        let cycleId = connectCycleId
        let delivered = LockedFlag()
        // A lookup that hangs is given up: the cycle goes on with the name form (and extra hosts' name forms).
        queue.asyncAfter(deadline: .now() + outsideLookupTimeoutSec) { [weak self] in
            guard let self = self, delivered.trySet(), self.connectCycleId == cycleId,
                  self.shouldStayConnected, self.connection == nil else { return }
            let nameForms = hosts.compactMap { h in NWEndpoint.Port(rawValue: h.port).map { Self.outsideEndpoint(host: h.host, port: $0) } }
            if nameForms.count > 1 {
                self.raceOutsideHostsOnQueue(nameForms.map { [$0] })
            } else if let only = nameForms.first {
                self.connectToStreamEndpointOnQueue(endpoint: only, origin: .outside, isAutomaticLanFirst: false,
                                                    pinnedHashOverride: nil, onHandshakeResult: nil)
            }
        }
        // getaddrinfo can wait on DNS64 discovery, so it runs off the client queue.
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let resolved = resolver?(host, port) ?? Self.synthesizedOutsideEndpoints(host: host, port: port)
            // Each extra host: its resolved addresses, then its name form.
            var extraGroups: [[NWEndpoint]] = []
            for h in extraHosts {
                guard let p = NWEndpoint.Port(rawValue: h.port) else { continue }
                var g = resolver?(h.host, h.port) ?? Self.synthesizedOutsideEndpoints(host: h.host, port: h.port)
                let nf = Self.outsideEndpoint(host: h.host, port: p)
                if !g.contains(where: { "\($0)" == "\(nf)" }) { g.append(nf) }
                extraGroups.append(g)
            }
            guard let self = self else { return }
            self.queue.async {
                // Drop a stale answer (an older cycle, or one the lookup timeout already replaced).
                guard self.connectCycleId == cycleId, delivered.trySet(),
                      self.shouldStayConnected, self.connection == nil else { return }
                var candidates = resolved
                let nameForm = Self.outsideEndpoint(host: host, port: nwPort)
                if !candidates.contains(where: { "\($0)" == "\(nameForm)" }) { candidates.append(nameForm) }
                if !extraGroups.isEmpty {
                    // Several outside hosts: race them, one dial in flight per host (group 0 is the typed address).
                    self.onOutsideDialModeForTesting?("race")
                    self.raceOutsideHostsOnQueue([candidates] + extraGroups)
                    return
                }
                // One host (including when a VPN-range candidate was skipped): the proven single-address path.
                self.onOutsideDialModeForTesting?("single")
                self.pendingOutsideCandidates = Array(candidates.dropFirst())
                self.connectToStreamEndpointOnQueue(
                    endpoint: candidates[0],
                    origin: .outside,
                    isAutomaticLanFirst: false,
                    pinnedHashOverride: nil,
                    onHandshakeResult: nil
                )
            }
        }
        return true
    }

    private var outsideRaceId: UInt64 = 0
    /// Bumped at every connect cycle; a getaddrinfo answer from an older cycle is dropped.
    private var connectCycleId: UInt64 = 0
    /// getaddrinfo gets this long; then the cycle goes on with the name form alone.
    var outsideLookupTimeoutSec: Double = 3.0
    private var outsideRacers: [NWConnection] = []
    private var outsideRaceDeadlineWork: DispatchWorkItem?
    private var currentRaceTally: OutsideRaceTally?
    /// Gap between racers starting (Happy Eyeballs style).
    var outsideRaceStaggerSec: Double = 0.25
    /// Inside a race, a host's address that hasn't answered by then gives way to that host's next address, so a
    /// stalled first address can't hide the others for the whole race deadline.
    var outsideRaceAddressTimeoutSec: Double = 3.5

    private var pathMonitor: NWPathMonitor?
    private var lastPathSignature: String?
    /// Whether the phone has a VPN-type interface up (a utun shows as `.other`), from the path monitor. Unknown
    /// counts as no VPN.
    private var vpnInterfaceUp: Bool = false
    /// Test hook: overrides the VPN check (read at the start of every connect cycle).
    var vpnUpOverrideForTesting: Bool?
    /// Test hook: which outside dial ran this cycle, "single" (one host, addresses in turn) or "race".
    var onOutsideDialModeForTesting: (@Sendable (String) -> Void)?

    /// 100.64.0.0/10 (carrier-grade NAT range, used by Tailscale and similar VPNs).
    static func isVPNRangeIPv4(_ host: String) -> Bool {
        let parts = host.split(separator: ".").compactMap { Int($0) }
        guard parts.count == 4, parts.allSatisfy({ (0...255).contains($0) }) else { return false }
        return parts[0] == 100 && (64...127).contains(parts[1])
    }

    /// A network change (home Wi-Fi back, cellular up) restarts a waiting connect cycle at once, instead of waiting
    /// out an outside run or a back-off.
    private func startPathMonitorOnQueue() {
        guard pathMonitor == nil else { return }
        let m = NWPathMonitor()
        m.pathUpdateHandler = { [weak self] path in
            self?.vpnInterfaceUp = path.availableInterfaces.contains { $0.type == .other }
            let types = [NWInterface.InterfaceType.wifi, .cellular, .wiredEthernet, .loopback, .other]
                .filter { path.usesInterfaceType($0) }.map { "\($0)" }.joined(separator: ",")
            self?.handleNetworkPathChangeOnQueue(signature: "\(path.status)|\(types)")
        }
        pathMonitor = m
        m.start(queue: queue)
    }

    private func handleNetworkPathChangeOnQueue(signature: String) {
        defer { lastPathSignature = signature }
        guard let previous = lastPathSignature, previous != signature,
              shouldStayConnected, !telemetry.isConnected, !isCandidateProbeInFlightOnQueue,
              state.pinnedServerCertSHA256 != nil else { return }
        reconnectWork?.cancel()
        reconnectWork = nil
        onPathChangeRestartForTesting?()
        startStreamingOnQueue()
    }

    /// Test hooks: feed a path change, and see that it restarted the cycle.
    func simulateNetworkPathChangeForTesting(signature: String) {
        queue.async { self.handleNetworkPathChangeOnQueue(signature: signature) }
    }
    var onPathChangeRestartForTesting: (@Sendable () -> Void)?

    /// Test hook: as if a losing racer had seen iOS block the network (a Mac can't produce "cellular denied").
    func setNetworkBlockedSentenceForTesting(_ sentence: String?) {
        queue.sync {
            telemetry.networkBlockedSentence = sentence
            publishTelemetryOnQueue()
        }
    }

    static let maxOutsideCandidates = 4

    private func stopPathMonitorOnQueue() {
        pathMonitor?.cancel()
        pathMonitor = nil
        lastPathSignature = nil
    }

    private var hasAnyOutsideAddressOnQueue: Bool {
        state.outsideEndpoint != nil || !(state.outsideCandidates ?? []).isEmpty
    }

    private func cancelOutsideRaceOnQueue() {
        outsideRaceId &+= 1
        currentRaceTally?.startNext = nil
        currentRaceTally = nil
        outsideRaceDeadlineWork?.cancel()
        outsideRaceDeadlineWork = nil
        for c in outsideRacers { c.cancel() }
        outsideRacers = []
    }

    /// Races the PC's outside hosts: each host is one group of resolved addresses. Hosts start
    /// `outsideRaceStaggerSec` apart (Happy Eyeballs style), but **within a host only one dial is ever in flight**:
    /// its next address starts only when the previous one has ended (the PC allows 2 in flight and a burst of 4 per
    /// source). Every dial is full pinned TLS (never a bare TCP probe, which its limiter would count). The first to
    /// finish TLS with the pinned certificate wins and becomes the session; the others are cancelled. Group 0 is the
    /// typed/router address: only its dials are pinned to cellular after a stall (a VPN candidate keeps its route).
    private func raceOutsideHostsOnQueue(_ rawGroups: [[NWEndpoint]]) {
        cancelOutsideRaceOnQueue()
        // Never race the same resolved address twice (across hosts too).
        var seen = Set<String>()
        let groups = rawGroups.map { $0.filter { seen.insert("\($0)").inserted } }.filter { !$0.isEmpty }
        guard !groups.isEmpty, let expected = state.pinnedServerCertSHA256 else { return }
        let raceId = outsideRaceId
        let tally = OutsideRaceTally()
        let pinnedToCellular = pinNextOutsideDialToCellular
        tally.nextIndex = [Int](repeating: 0, count: groups.count)
        currentRaceTally = tally

        let finishIfAllEnded: () -> Void = { [weak self] in
            guard let self = self, self.outsideRaceId == raceId, tally.groupsDone == groups.count else { return }
            self.cancelOutsideRaceOnQueue()
            if pinnedToCellular { self.pinNextOutsideDialToCellular = false }
            if tally.sawTLSRefusal {
                self.recordTLSRefusalWithoutWipingPairingOnQueue()
            } else if tally.sawLimiterClose {
                self.telemetry.lastError = PetCameraUserStrings.pcBusyTryAgainShortlySentence
                self.minimumNextReconnectDelaySec = max(self.minimumNextReconnectDelaySec, self.limiterClosePauseSec)
            }
            self.publishTelemetryOnQueue()
            self.scheduleReconnectOnQueue()
        }

        // Starts group g's next address, or marks the group done.
        tally.startNext = { [weak self] g in
            guard let self = self, self.outsideRaceId == raceId, self.shouldStayConnected, self.connection == nil else { return }
            guard tally.nextIndex[g] < groups[g].count else {
                tally.groupsDone += 1
                finishIfAllEnded()
                return
            }
            let ep = groups[g][tally.nextIndex[g]]
            tally.nextIndex[g] += 1
            tally.started += 1
            self.onConnectAttemptForTesting?(.outside, "\(ep)")
            let attemptId = self.recordConnectAttemptOnQueue(origin: .outside, endpoint: ep)
            let sawCert = LockedFlag()
            let pinMatched = LockedFlag()
            let params = CryptoIdentity.makeTLSParameters(
                localIdentity: self.clientIdentity,
                requireClientCert: false,
                verifyQueue: self.queue
            ) { peerHash, _ in
                _ = sawCert.trySet()
                let ok = CryptoIdentity.constantTimeEqual(peerHash, expected)
                if ok { _ = pinMatched.trySet() }
                return ok
            }
            if pinnedToCellular && g == 0 {
                params.requiredInterfaceType = .cellular
                if let j = self.telemetry.recentConnectAttempts.lastIndex(where: { $0.id == attemptId }) {
                    self.telemetry.recentConnectAttempts[j].endpoint += " [cellular only]"
                }
            }
            let conn = NWConnection(to: ep, using: params)
            self.outsideRacers.append(conn)
            let done = LockedFlag()
            conn.stateUpdateHandler = { [weak self] st in
                guard let self = self, self.outsideRaceId == raceId else { return }
                switch st {
                case .ready:
                    guard done.trySet() else { return }
                    // Winner: stop the others and hand this connection to the session code.
                    let winner = OutsideRaceWinner(conn: conn, sawServerCertificate: sawCert,
                                                   serverPinMatched: pinMatched, attemptId: attemptId)
                    self.outsideRacers.removeAll { $0 === conn }
                    self.cancelOutsideRaceOnQueue()
                    if pinnedToCellular { self.pinNextOutsideDialToCellular = false }
                    self.connectToStreamEndpointOnQueue(
                        endpoint: ep, origin: .outside, isAutomaticLanFirst: false,
                        pinnedHashOverride: nil, onHandshakeResult: nil, raceWinner: winner
                    )
                case .waiting(let err), .failed(let err):
                    guard done.trySet() else { return }
                    var note = "waiting"
                    if case .failed = st { note = "failed" }
                    if let path = conn.currentPath, path.status != .satisfied {
                        note += " [\(Self.describe(path.unsatisfiedReason))]"
                        if let blocked = Self.blockedNetworkSentence(for: path.unsatisfiedReason) {
                            self.telemetry.networkBlockedSentence = blocked
                        }
                    }
                    if !sawCert.isSet && Self.isClosedByPeerError(err) {
                        tally.sawLimiterClose = true
                        self.updateConnectAttemptOnQueue(attemptId, result: "closed by the PC before TLS (busy)")
                    } else if sawCert.isSet && Self.isTLSRejectionError(err) {
                        tally.sawTLSRefusal = true
                        self.updateConnectAttemptOnQueue(attemptId, result: pinMatched.isSet ? "refused by the PC" : "certificate didn't match (refused)")
                    } else {
                        self.updateConnectAttemptOnQueue(attemptId, result: "\(note): \(Self.shortErrorText(err))")
                    }
                    conn.cancel()
                    self.outsideRacers.removeAll { $0 === conn }
                    tally.ended += 1
                    // This host's previous dial has ended: only now may its next address start.
                    tally.startNext?(g)
                case .preparing:
                    self.updateConnectAttemptOnQueue(attemptId, result: "connecting")
                default:
                    break
                }
            }
            conn.start(queue: self.queue)

            // This host has another address: give this one `outsideRaceAddressTimeoutSec`, then move on.
            if tally.nextIndex[g] < groups[g].count {
                let timeout = self.outsideRaceAddressTimeoutSec
                self.queue.asyncAfter(deadline: .now() + timeout) { [weak self] in
                    guard let self = self, self.outsideRaceId == raceId, done.trySet() else { return }
                    self.updateConnectAttemptOnQueue(attemptId, result: "no answer in \(timeout) s (next address)")
                    conn.cancel()
                    self.outsideRacers.removeAll { $0 === conn }
                    tally.ended += 1
                    tally.startNext?(g)
                }
            }
        }

        for g in groups.indices {
            queue.asyncAfter(deadline: .now() + Double(g) * outsideRaceStaggerSec) { tally.startNext?(g) }
        }

        // One deadline for the whole race.
        let dWork = DispatchWorkItem { [weak self] in
            guard let self = self, self.outsideRaceId == raceId, self.shouldStayConnected, self.connection == nil else { return }
            for a in self.telemetry.recentConnectAttempts where a.path == .outside && a.result.hasPrefix("connecting") {
                self.updateConnectAttemptOnQueue(a.id, result: "no answer in \(self.outsideAttemptDeadlineSec) s")
            }
            self.cancelOutsideRaceOnQueue()
            self.pinNextOutsideDialToCellular = !pinnedToCellular
            self.telemetry.lastError = PetCameraUserStrings.unreachableDefaultPC
            self.publishTelemetryOnQueue()
            self.scheduleReconnectOnQueue()
        }
        outsideRaceDeadlineWork = dWork
        queue.asyncAfter(deadline: .now() + Double(groups.count - 1) * outsideRaceStaggerSec + outsideAttemptDeadlineSec,
                         execute: dWork)
    }

    /// Dials the next outside address of this cycle, if any is left.
    private func tryNextOutsideCandidateOnQueue() -> Bool {
        guard shouldStayConnected, !pendingOutsideCandidates.isEmpty else { return false }
        let next = pendingOutsideCandidates.removeFirst()
        connectToStreamEndpointOnQueue(
            endpoint: next,
            origin: .outside,
            isAutomaticLanFirst: false,
            pinnedHashOverride: nil,
            onHandshakeResult: nil
        )
        return true
    }

    /// Apple's documented NAT64 path: getaddrinfo with AI_DEFAULT on the address text. On an IPv6-only carrier
    /// it returns the synthesized IPv6 address (64:ff9b::/96 or the carrier's prefix); on IPv4 networks
    /// the IPv4 address. IPv6 results come first. Empty if nothing resolves.
    static func synthesizedOutsideEndpoints(host: String, port: UInt16) -> [NWEndpoint] {
        guard let nwPort = NWEndpoint.Port(rawValue: port) else { return [] }
        var hints = addrinfo()
        hints.ai_flags = AI_DEFAULT
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        hints.ai_protocol = IPPROTO_TCP
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, String(port), &hints, &res) == 0, let first = res else { return [] }
        defer { freeaddrinfo(first) }
        var v6: [NWEndpoint] = []
        var v4: [NWEndpoint] = []
        var cur: UnsafeMutablePointer<addrinfo>? = first
        while let ai = cur {
            if ai.pointee.ai_family == AF_INET6, let sa = ai.pointee.ai_addr {
                let sin6 = sa.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { $0.pointee }
                var a = sin6.sin6_addr
                let data = withUnsafeBytes(of: &a) { Data($0) }
                if let addr = IPv6Address(data) {
                    let ep = NWEndpoint.hostPort(host: .ipv6(addr), port: nwPort)
                    if !v6.contains(where: { "\($0)" == "\(ep)" }) { v6.append(ep) }
                }
            } else if ai.pointee.ai_family == AF_INET, let sa = ai.pointee.ai_addr {
                let sin = sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee }
                var a = sin.sin_addr
                let data = withUnsafeBytes(of: &a) { Data($0) }
                if let addr = IPv4Address(data) {
                    let ep = NWEndpoint.hostPort(host: .ipv4(addr), port: nwPort)
                    if !v4.contains(where: { "\($0)" == "\(ep)" }) { v4.append(ep) }
                }
            }
            cur = ai.pointee.ai_next
        }
        return v6 + v4
    }

    private static func isTLSRejectionError(_ error: NWError?) -> Bool {
        guard let error = error else { return false }
        if case .tls = error {
            return true
        }
        return false
    }

    /// The far end accepted TCP and then closed or reset it (as the PC's connection limiter does before TLS).
    private static func isClosedByPeerError(_ error: NWError) -> Bool {
        if case .posix(let code) = error {
            switch code {
            case .ECONNRESET, .ECONNABORTED, .ENOTCONN, .EPIPE: return true
            default: return false
            }
        }
        return false
    }

    /// True ONLY when TCP could not reach `lh:p` at all (refused, host/net unreachable, DNS, or connect timeout).
    /// Any TLS error or reset/abort after TCP connected returns `false` so `lh:p` never falls through to `h:p` (§8.5).
    private static func isTCPConnectUnreachableError(_ error: NWError) -> Bool {
        switch error {
        case .dns:
            return true
        case .posix(let code):
            switch code {
            case .ECONNREFUSED, .EHOSTUNREACH, .ENETUNREACH, .ENETDOWN, .EHOSTDOWN, .EADDRNOTAVAIL, .ETIMEDOUT:
                return true
            default:
                return false
            }
        case .tls:
            return false
        default:
            return false
        }
    }

    private func recordTLSRefusalWithoutWipingPairingOnQueue() {
        // NEVER wipe pairing or Keychain identity on a TLS rejection!
        // The Windows host also rejects at TLS while `devices.json` cannot be read at startup.
        let refusal = PetCameraUserStrings.tlsRefusedBannerText(forDeviceKind: deviceKind)
        telemetry.tlsRefusalNotice = refusal
        telemetry.lastError = refusal
        telemetry.connectionState = "Disconnected"
    }

    private func connectToStreamEndpointOnQueue(
        endpoint: NWEndpoint,
        origin: PetPath,
        isAutomaticLanFirst: Bool,
        pinnedHashOverride: Data?,
        onHandshakeResult: (@Sendable (Bool, String?) -> Void)?,
        raceWinner: OutsideRaceWinner? = nil
    ) {
        teardownConnectionOnQueue()
        self.activeConnectOrigin = origin
        self.isAutomaticLanFirstAttempt = isAutomaticLanFirst
        let expectedServerHash = pinnedHashOverride ?? state.pinnedServerCertSHA256
        let serverPinMatched = raceWinner?.serverPinMatched ?? LockedFlag()
        // Set once the PC presents its certificate. A close before that is the PC's connection limiter, not a refusal.
        let sawServerCertificate = raceWinner?.sawServerCertificate ?? LockedFlag()
        let hasExplicitHandshakeCallback = (onHandshakeResult != nil)

        let attemptId: UInt64
        if let w = raceWinner {
            attemptId = w.attemptId
        } else {
            onConnectAttemptForTesting?(origin, "\(endpoint)")
            attemptId = recordConnectAttemptOnQueue(origin: origin, endpoint: endpoint)
        }

        let tlsParams = CryptoIdentity.makeTLSParameters(
            localIdentity: clientIdentity,
            requireClientCert: false,
            verifyQueue: queue
        ) { peerHash, _ in
            _ = sawServerCertificate.trySet()
            guard let expected = expectedServerHash else { return false }
            let ok = CryptoIdentity.constantTimeEqual(peerHash, expected)
            if ok {
                _ = serverPinMatched.trySet()
            }
            return ok
        }

        let pinnedToCellular = (raceWinner == nil && origin == .outside && pinNextOutsideDialToCellular &&
                                !hasExplicitHandshakeCallback && pinnedHashOverride == nil)
        if pinnedToCellular {
            tlsParams.requiredInterfaceType = .cellular
            if let i = telemetry.recentConnectAttempts.lastIndex(where: { $0.id == attemptId }) {
                telemetry.recentConnectAttempts[i].endpoint += " [cellular only]"
            }
        }

        let conn = raceWinner?.conn ?? NWConnection(to: endpoint, using: tlsParams)
        conn.pathUpdateHandler = { [weak self] path in
            guard let self = self else { return }
            let via = [NWInterface.InterfaceType.wifi, .cellular, .wiredEthernet, .loopback, .other]
                .first(where: { path.usesInterfaceType($0) }).map { "\($0)" } ?? "no interface"
            let status: String
            switch path.status {
            case .satisfied: status = "satisfied"
            case .unsatisfied: status = "unsatisfied: \(Self.describe(path.unsatisfiedReason))"
            case .requiresConnection: status = "requires connection"
            @unknown default: status = "unknown"
            }
            self.updateConnectAttemptOnQueue(attemptId, result: "connecting via \(via) (\(status))" + (pinnedToCellular ? ", cellular only" : ""))
        }
        let handshakeReported = LockedFlag()
        self.receivedServerHello = false
        self.transportActiveForPings = false
        self.sentV3PingInCurrentSession = false
        self.pendingHandshakeCallback = { accepted, msg in
            if handshakeReported.trySet() {
                onHandshakeResult?(accepted, msg)
            }
        }

        // If trying LAN first while a stored outside endpoint is available, bound the LAN attempt so a blackholed LAN IP falls back promptly.
        if isAutomaticLanFirst && hasAnyOutsideAddressOnQueue && !triedOutsideInCurrentCycle {
            let tWork = DispatchWorkItem { [weak self] in
                guard let self = self, self.connection === conn, !self.telemetry.isConnected else { return }
                self.updateConnectAttemptOnQueue(attemptId, result: "no answer in \(self.lanAttemptTimeoutBeforeOutsideSec) s")
                self.teardownConnectionOnQueue()
                if !self.tryOutsideFallbackInCurrentCycleOnQueue() {
                    self.scheduleReconnectOnQueue()
                }
            }
            lanAttemptTimeoutWork = tWork
            queue.asyncAfter(deadline: .now() + lanAttemptTimeoutBeforeOutsideSec, execute: tWork)
        }

        if origin == .outside && !hasExplicitHandshakeCallback && pinnedHashOverride == nil {
            let deadlineSec = outsideAttemptDeadlineSec
            let dWork = DispatchWorkItem { [weak self] in
                guard let self = self, self.connection === conn, !self.receivedServerHello else { return }
                self.updateConnectAttemptOnQueue(attemptId, result: "no answer in \(deadlineSec) s")
                // A normal dial that stalls pins the next one to cellular; a pinned one that stalls unpins it.
                self.pinNextOutsideDialToCellular = !pinnedToCellular
                self.telemetry.isConnected = false
                self.telemetry.lastError = PetCameraUserStrings.unreachableDefaultPC
                self.teardownConnectionOnQueue()
                if !pinnedToCellular {
                    // Retry the address that stalled, pinned to cellular, before moving on.
                    self.connectToStreamEndpointOnQueue(endpoint: endpoint, origin: .outside, isAutomaticLanFirst: false,
                                                        pinnedHashOverride: nil, onHandshakeResult: nil)
                    return
                }
                if self.tryNextOutsideCandidateOnQueue() { return }
                self.scheduleReconnectOnQueue()
            }
            outsideAttemptDeadlineWork = dWork
            queue.asyncAfter(deadline: .now() + deadlineSec, execute: dWork)
        }

        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            switch st {
            case .ready:
                self.isReconnecting = false
                if pinnedToCellular { self.pinNextOutsideDialToCellular = false }
                self.telemetry.networkBlockedSentence = nil
                self.updateConnectAttemptOnQueue(attemptId, result: "reached")
                if self.telemetry.tlsRefusalNotice == nil && self.telemetry.lastSessionNotice == nil {
                    self.telemetry.connectionState = "Verifying Mutual Pin…"
                }
                self.frameReader.reset()
                self.bestRttNs = .max
                self.rttSamples.removeAll()
                self.receiveStreamLoopOnQueue(
                    conn: conn,
                    serverPinMatched: serverPinMatched,
                    hasExplicitHandshakeCallback: hasExplicitHandshakeCallback,
                    isTestOverride: pinnedHashOverride != nil
                )
                self.publishTelemetryOnQueue()

            case .waiting(let err):
                // Reason first and the error short, so the row isn't cut off on a phone screen.
                var waitingNote = "waiting"
                if let path = conn.currentPath, path.status != .satisfied {
                    waitingNote += " [\(Self.describe(path.unsatisfiedReason))]"
                    if let blocked = Self.blockedNetworkSentence(for: path.unsatisfiedReason) {
                        self.telemetry.networkBlockedSentence = blocked
                    }
                }
                waitingNote += ": \(Self.shortErrorText(err))"
                self.updateConnectAttemptOnQueue(attemptId, result: waitingNote)
                // A cellular-pinned dial gets exactly one try: however it ends without connecting, the next is unpinned.
                if pinnedToCellular { self.pinNextOutsideDialToCellular = false }
                let wasCandidateProbe = (self.activeCandidateProbeFailureHandler != nil || self.pendingSessionConfirmedCallback != nil)
                let wasLanFirst = self.isAutomaticLanFirstAttempt
                if handshakeReported.trySet(), hasExplicitHandshakeCallback {
                    onHandshakeResult?(false, err.localizedDescription)
                    conn.cancel()
                    if wasCandidateProbe { return }
                }
                if !wasCandidateProbe, self.connection === conn {
                    self.telemetry.isConnected = false
                    self.telemetry.connectionState = "Waiting for Network…"
                    self.telemetry.lastError = err.localizedDescription
                    // iOS reports the PC's limiter (TCP accepted, then reset before any certificate) as `.waiting`.
                    if origin == .outside && !hasExplicitHandshakeCallback && pinnedHashOverride == nil &&
                        !sawServerCertificate.isSet && Self.isClosedByPeerError(err) {
                        self.telemetry.lastError = PetCameraUserStrings.pcBusyTryAgainShortlySentence
                        self.minimumNextReconnectDelaySec = max(self.minimumNextReconnectDelaySec, self.limiterClosePauseSec)
                        self.updateConnectAttemptOnQueue(attemptId, result: "closed by the PC before TLS (busy)")
                    }
                    if self.state.outsideEndpoint == nil {
                        self.state.lastKnownStreamPort = nil
                    }
                    self.publishTelemetryOnQueue()
                    self.teardownConnectionOnQueue()
                    if wasLanFirst && self.tryOutsideFallbackInCurrentCycleOnQueue() {
                        return
                    }
                    if origin == .outside && !hasExplicitHandshakeCallback && self.tryNextOutsideCandidateOnQueue() {
                        return
                    }
                    self.scheduleReconnectOnQueue()
                }

            case .failed(let err):
                self.updateConnectAttemptOnQueue(attemptId, result: "failed: \(Self.shortErrorText(err))")
                if pinnedToCellular { self.pinNextOutsideDialToCellular = false }
                let wasCandidateProbe = (self.activeCandidateProbeFailureHandler != nil || self.pendingSessionConfirmedCallback != nil)
                let wasLanFirst = self.isAutomaticLanFirstAttempt
                if handshakeReported.trySet() {
                    onHandshakeResult?(false, err.localizedDescription)
                }
                if !wasCandidateProbe, self.connection === conn {
                    self.telemetry.isConnected = false
                    self.telemetry.connectionState = "Disconnected"
                    self.teardownConnectionOnQueue()
                    // Only the PC's outside listener has a connection limiter.
                    let closedBeforeTLS = origin == .outside && !sawServerCertificate.isSet &&
                        (Self.isClosedByPeerError(err) || Self.isTLSRejectionError(err))
                    if !hasExplicitHandshakeCallback && pinnedHashOverride == nil && closedBeforeTLS {
                        // The PC's limiter: TCP accepted, then closed before any certificate. Try again after a pause,
                        // and never record it as a refusal.
                        self.telemetry.lastError = PetCameraUserStrings.pcBusyTryAgainShortlySentence
                        self.minimumNextReconnectDelaySec = max(self.minimumNextReconnectDelaySec, self.limiterClosePauseSec)
                        self.updateConnectAttemptOnQueue(attemptId, result: "closed by the PC before TLS (busy)")
                    } else if !hasExplicitHandshakeCallback && pinnedHashOverride == nil && Self.isTLSRejectionError(err) {
                        self.recordTLSRefusalWithoutWipingPairingOnQueue()
                    } else {
                        if self.state.outsideEndpoint == nil {
                            self.state.lastKnownStreamPort = nil
                        }
                        self.telemetry.lastError = err.localizedDescription
                    }
                    self.publishTelemetryOnQueue()
                    if !hasExplicitHandshakeCallback && wasLanFirst && self.tryOutsideFallbackInCurrentCycleOnQueue() {
                        return
                    }
                    if origin == .outside && !hasExplicitHandshakeCallback && self.tryNextOutsideCandidateOnQueue() {
                        return
                    }
                    self.scheduleReconnectOnQueue()
                }

            case .preparing:
                self.updateConnectAttemptOnQueue(attemptId, result: "connecting" + (pinnedToCellular ? " (cellular only)" : ""))
            case .cancelled:
                if self.connection === conn {
                    self.telemetry.isConnected = false
                    self.publishTelemetryOnQueue()
                }
            default:
                break
            }
        }

        self.connection = conn
        if raceWinner != nil {
            // Already through TLS with the pinned certificate: run the session's `.ready` path now.
            conn.stateUpdateHandler?(.ready)
        } else {
            conn.start(queue: queue)
        }
    }

    private func scheduleReconnectOnQueue() {
        guard shouldStayConnected, state.pinnedServerCertSHA256 != nil else { return }
        reconnectWork?.cancel()
        isReconnecting = true
        telemetry.reconnectCount += 1

        let usesV3ExponentialBackoff =
            (state.lastKnownHostProtocolVersion ?? 1) >= 3 ||
            state.outsideEndpoint != nil ||
            telemetry.lastSessionNotice != nil ||
            telemetry.tlsRefusalNotice != nil

        var delaySec: Double
        if usesV3ExponentialBackoff {
            // Exponential back-off (1, 2, 4, 8, 16, 30 s), capped at 30 s (`docs/protocol-v3-pet-camera.md` §8.5).
            let expStep = min(5, max(0, telemetry.reconnectCount - 1))
            delaySec = min(30.0, Double(1 << expStep))
        } else {
            // Keep v2's exact reconnect back-off unchanged against v1/v2 hosts.
            delaySec = telemetry.reconnectCount <= 2 ? 0.40 : 1.0
        }
        // After the PC's limiter closed us before TLS, wait out its window rather than knock again at once.
        delaySec = max(delaySec, minimumNextReconnectDelaySec)
        minimumNextReconnectDelaySec = 0
        telemetry.lastScheduledReconnectDelaySec = delaySec
        publishTelemetryOnQueue()

        if let testScheduler = onReconnectScheduledForTesting {
            testScheduler(delaySec) { [weak self] in
                guard let self = self else { return }
                self.queue.async {
                    guard self.shouldStayConnected else { return }
                    self.startStreamingOnQueue()
                }
            }
            return
        }

        let work = DispatchWorkItem { [weak self] in
            guard let self = self, self.shouldStayConnected else { return }
            self.startStreamingOnQueue()
        }
        reconnectWork = work
        queue.asyncAfter(deadline: .now() + delaySec, execute: work)
    }

    /// The PC's outside address `h:p`, always as a host *name*, even when it's an IPv4 literal like 203.0.113.5.
    /// `NWEndpoint.Host("1.2.3.4")` becomes `.ipv4`, which an IPv6-only carrier (NAT64/464XLAT) has no route
    /// for, so the connection waits and no packet leaves. As a name it goes through the resolver, which synthesizes the
    /// NAT64 IPv6 address there and returns the IPv4 address on IPv4 networks. Pinning is unchanged (the verify block).
    static func outsideEndpoint(host: String, port: NWEndpoint.Port) -> NWEndpoint {
        .hostPort(host: .name(host, nil), port: port)
    }

    /// What to tell the person when iOS, not the PC, is what stops the connection.
    static func blockedNetworkSentence(for reason: NWPath.UnsatisfiedReason) -> String? {
        switch reason {
        case .cellularDenied: return PetCameraUserStrings.cellularDataOffSentence
        case .wifiDenied: return PetCameraUserStrings.wifiOffForAppSentence
        case .localNetworkDenied: return PetCameraUserStrings.localNetworkOffSentence
        default: return nil
        }
    }

    /// A short, exact error for the attempt rows, e.g. "POSIX 50 ENETDOWN", "DNS -65554", "TLS -9806".
    static func shortErrorText(_ error: NWError) -> String {
        switch error {
        case .posix(let code):
            let names: [POSIXErrorCode: String] = [
                .ENETDOWN: "ENETDOWN", .ENETUNREACH: "ENETUNREACH", .EHOSTUNREACH: "EHOSTUNREACH",
                .EHOSTDOWN: "EHOSTDOWN", .ECONNREFUSED: "ECONNREFUSED", .ECONNRESET: "ECONNRESET",
                .ECONNABORTED: "ECONNABORTED", .ETIMEDOUT: "ETIMEDOUT", .EADDRNOTAVAIL: "EADDRNOTAVAIL",
                .ENOTCONN: "ENOTCONN", .EPIPE: "EPIPE", .ECANCELED: "ECANCELED",
            ]
            return "POSIX \(code.rawValue)" + (names[code].map { " \($0)" } ?? "")
        case .dns(let code): return "DNS \(code)"
        case .tls(let code): return "TLS \(code)"
        @unknown default: return "\(error)"
        }
    }

    private static func describe(_ reason: NWPath.UnsatisfiedReason) -> String {
        switch reason {
        case .cellularDenied: return "cellular denied"
        case .wifiDenied: return "wi-fi denied"
        case .localNetworkDenied: return "local network denied"
        case .notAvailable: return "no network"
        default: return "unsatisfied"
        }
    }

    @discardableResult
    private func recordConnectAttemptOnQueue(origin: PetPath, endpoint: NWEndpoint) -> UInt64 {
        nextConnectAttemptId &+= 1
        let record = ConnectAttemptRecord(id: nextConnectAttemptId, at: Date(), path: origin, endpoint: "\(endpoint)", result: "trying")
        telemetry.recentConnectAttempts.append(record)
        if telemetry.recentConnectAttempts.count > PocketClientTelemetry.maxRecentConnectAttempts {
            telemetry.recentConnectAttempts.removeFirst(telemetry.recentConnectAttempts.count - PocketClientTelemetry.maxRecentConnectAttempts)
        }
        publishTelemetryOnQueue()
        return nextConnectAttemptId
    }

    private func updateConnectAttemptOnQueue(_ id: UInt64, result: String) {
        guard let i = telemetry.recentConnectAttempts.lastIndex(where: { $0.id == id }) else { return }
        let current = telemetry.recentConnectAttempts[i].result
        guard current != result else { return }
        // Progress notes ("trying…", "connecting…") never overwrite an outcome a late state event may follow.
        let currentIsProgress = current.hasPrefix("trying") || current.hasPrefix("connecting")
        let newIsProgress = result.hasPrefix("trying") || result.hasPrefix("connecting")
        if newIsProgress && !currentIsProgress { return }
        telemetry.recentConnectAttempts[i].result = result
        publishTelemetryOnQueue()
    }

    private func teardownConnectionOnQueue() {
        // Always end any race, including in the gaps between racers starting, so a stopped or restarted client
        // never has a race deadline set the cellular pin or "unreachable" later.
        cancelOutsideRaceOnQueue()
        lanAttemptTimeoutWork?.cancel()
        lanAttemptTimeoutWork = nil
        outsideAttemptDeadlineWork?.cancel()
        outsideAttemptDeadlineWork = nil
        outsideStabilityCancel?()
        outsideStabilityCancel = nil
        pingTimer?.cancel()
        pingTimer = nil
        connection?.cancel()
        connection = nil
        frameReader.reset()
        receivedServerHello = false
        inboundSilenceSimulatedForTesting = false
        transportActiveForPings = false
        sentV3PingInCurrentSession = false
        isAutomaticLanFirstAttempt = false
        telemetry.isConnected = false
        telemetry.protocolVersion = 1
        telemetry.isPetCameraV3 = false
        telemetry.petCameraHello = nil
        telemetry.hasConfirmedHostState = false
        telemetry.hasConfirmedPetCameraState = false
        telemetry.fps = 0.0
        telemetry.bitrateMbps = 0.0
        frameArrivalTimes.removeAll()
        lastVideoFrameSeqInEpoch = nil
        if let ds = decompSession {
            VTDecompressionSessionInvalidate(ds)
            decompSession = nil
        }
        formatDesc = nil

        let hadPlayback = (playbackStreamId != nil || playbackFormatDesc != nil)
        playbackStreamId = nil
        playbackClipId = nil
        playbackFormatDesc = nil
        playbackStreamEpoch = 0
        telemetry.latestPlaybackState = nil
        if hadPlayback {
            onPlaybackStreamFlushed?()
            onPlaybackStateUpdated?(nil)
        }
        failAllPendingRequestsOnQueue(reason: "Connection to the camera closed.")
    }

    /// Called ONLY when an explicit `0x0F revoked` message arrives from the authenticated host.
    private func handleRevokedByHostOnQueue() {
        shouldStayConnected = false
        reconnectWork?.cancel()
        reconnectWork = nil
        lanFallbackWork?.cancel()
        lanFallbackWork = nil
        cancelPendingCandidateProbeOnQueue()
        browser?.cancel()
        browser = nil
        teardownConnectionOnQueue()

        store.wipe()
        if let freshIdent = try? CryptoIdentity.generateNew(commonName: "kv2-pocket") {
            self.clientIdentity = freshIdent
            self.state = ClientPairingState(
                clientIdentity: freshIdent.persistedRecord,
                serviceId: nil,
                pinnedServerCertSHA256: nil,
                lastKnownHost: nil,
                lastKnownStreamPort: nil
            )
            _ = self.store.save(self.state)
        }
        telemetry.hostState = .unconfirmedInitial
        telemetry.petState = .unconfirmedInitial
        telemetry.tlsRefusalNotice = nil
        telemetry.sawAddressChangeNotice = false
        telemetry.lastSawSharingEnabled = nil
        telemetry.lastSawOutsideEnabled = nil
        telemetry.reconnectCount = 0
        updatePairedFlagOnQueue()

        let banner = PetCameraUserStrings.revokedBannerText(forDeviceKind: deviceKind)
        telemetry.revokedNotice = banner
        telemetry.lastError = banner
        onStreamFlushed?()
        onPlaybackStreamFlushed?()
        publishTelemetryOnQueue()
        onRevoked?(banner)
    }

    /// Marks the session as genuinely connected and resets back-off/notices.
    /// On v3 hosts, this runs ONLY once the first `0x0B` (`petCameraState`) or `0x02` (`streamConfig`) arrives,
    /// so notice-only connections (`0x0F too_many_sessions`) never report `isConnected`, fire haptics, or reset back-off.
    /// On the `outside` path (§8.5), `reconnectCount` resets only after the session lasts 30 s so rapid drops never drain the host's bucket.
    private func confirmRealSessionStartedOnQueue() {
        lanAttemptTimeoutWork?.cancel()
        lanAttemptTimeoutWork = nil
        lanFallbackWork?.cancel()
        lanFallbackWork = nil

        let sessionCb = pendingSessionConfirmedCallback
        pendingSessionConfirmedCallback = nil
        sessionCb?(true, nil)

        guard !telemetry.isConnected else { return }
        telemetry.isConnected = true
        telemetry.lastSessionNotice = nil
        telemetry.sawAddressChangeNotice = false
        telemetry.tlsRefusalNotice = nil
        telemetry.lastError = nil
        telemetry.connectionState = "Connected (mTLS 1.3 Pin)"

        if activeConnectOrigin == .outside {
            outsideStabilityCancel?()
            let resetDelay = outsideSessionStabilityResetSec
            outsideStabilityCancel = timerScheduler.schedule(afterSeconds: resetDelay, on: queue) { [weak self] in
                guard let self = self, self.telemetry.isConnected else { return }
                self.outsideStabilityCancel = nil
                self.telemetry.reconnectCount = 0
                self.telemetry.lastScheduledReconnectDelaySec = 0.0
                self.publishTelemetryOnQueue()
            }
        } else {
            telemetry.reconnectCount = 0
            telemetry.lastScheduledReconnectDelaySec = 0.0
        }
    }

    private func sendPingPacketOnQueue(conn: NWConnection) {
        guard self.connection === conn, self.transportActiveForPings else { return }
        let pingProto = self.telemetry.isPetCameraV3 ? 3 : 2
        let ping = PingMessage(
            clientSendNs: Self.monotonicNs(),
            lastDecodedSeq: self.telemetry.receivedFrames,
            lastHostToDisplayLatencyUs: UInt32(max(0.0, min(4_000_000.0, self.telemetry.hostToDisplayLatencyMs * 1000.0))),
            lastBarcodeLatencyUs: UInt32(max(0.0, min(4_000_000.0, self.telemetry.barcodePixelLatencyMs * 1000.0))),
            protocolVersion: pingProto
        )
        if sendClientJSONOnQueue(conn: conn, type: .ping, ping), pingProto >= 3 {
            self.sentV3PingInCurrentSession = true
        }
    }

    private func startPingLoopOnQueue(conn: NWConnection) {
        pingTimer?.cancel()
        sendPingPacketOnQueue(conn: conn)
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + .milliseconds(250), repeating: .milliseconds(250))
        timer.setEventHandler { [weak self] in
            self?.sendPingPacketOnQueue(conn: conn)
        }
        self.pingTimer = timer
        timer.resume()
    }

    private func receiveStreamLoopOnQueue(
        conn: NWConnection,
        serverPinMatched: LockedFlag,
        hasExplicitHandshakeCallback: Bool,
        isTestOverride: Bool
    ) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 262_144) { [weak self] data, _, isComplete, err in
            guard let self = self, self.connection === conn else { return }
            if let data = data, !data.isEmpty {
                if !self.inboundSilenceSimulatedForTesting { self.lastInboundMonotonicNs = Self.monotonicNs() }
                self.frameReader.append(data)
                while true {
                    switch self.frameReader.nextFrame() {
                    case .needMoreData:
                        break
                    case .protocolViolation:
                        conn.cancel()
                        return
                    case .unknownFrame(_, _, let payload):
                        self.accountLiveWireBytesOnQueue(WireHeader.size + payload.count)
                        continue
                    case .frame(let msgType, _, let payload):
                        // §4.2: Playback (`0x18`/`0x19`) is excluded from live stream telemetry.
                        if msgType != .playbackConfig && msgType != .playbackAccessUnit {
                            self.accountLiveWireBytesOnQueue(WireHeader.size + payload.count)
                        }
                        self.handleServerMessageOnQueue(type: msgType, payload: payload)
                        // If a session notice (`0x0F revoked` or `0x0F replaced`) tore down this connection, stop here.
                        guard self.connection === conn else { return }
                        continue
                    }
                    break
                }
            }
            if isComplete || err != nil {
                let hadHello = self.receivedServerHello
                let hadRealSession = self.telemetry.isConnected
                let wasLanFirst = self.isAutomaticLanFirstAttempt
                let wasCandidateProbe = !hadRealSession && (self.activeCandidateProbeFailureHandler != nil || self.pendingSessionConfirmedCallback != nil)
                if !hadHello {
                    let cb = self.pendingHandshakeCallback
                    self.pendingHandshakeCallback = nil
                    cb?(false, err?.localizedDescription ?? "Rejected by host mutual TLS pin.")
                }
                if !hadRealSession {
                    let sessionCb = self.pendingSessionConfirmedCallback
                    self.pendingSessionConfirmedCallback = nil
                    sessionCb?(false, err?.localizedDescription ?? "Connection closed before session started.")
                }
                if wasCandidateProbe {
                    self.onStreamFlushed?()
                    return
                }
                self.teardownConnectionOnQueue()
                if !hadHello &&
                    !hasExplicitHandshakeCallback &&
                    !isTestOverride &&
                    (Self.isTLSRejectionError(err) || serverPinMatched.isSet) {
                    // TLS 1.3 peer-certificate rejection after server pin check: keep pairing and retry with back-off.
                    self.recordTLSRefusalWithoutWipingPairingOnQueue()
                }
                self.onStreamFlushed?()
                self.publishTelemetryOnQueue()
                if !hadRealSession && !hasExplicitHandshakeCallback && wasLanFirst && self.tryOutsideFallbackInCurrentCycleOnQueue() {
                    return
                }
                self.scheduleReconnectOnQueue()
                return
            }
            self.receiveStreamLoopOnQueue(
                conn: conn,
                serverPinMatched: serverPinMatched,
                hasExplicitHandshakeCallback: hasExplicitHandshakeCallback,
                isTestOverride: isTestOverride
            )
        }
    }

    private func accountLiveWireBytesOnQueue(_ byteCount: Int) {
        recvBytesWindow += byteCount
        let nowNs = Self.monotonicNs()
        if lastBitrateTickNs == 0 {
            lastBitrateTickNs = nowNs
        } else if nowNs - lastBitrateTickNs >= 1_000_000_000 {
            let dt = Double(nowNs - lastBitrateTickNs) / 1e9
            telemetry.bitrateMbps = (Double(recvBytesWindow) * 8.0) / (dt * 1_000_000.0)
            recvBytesWindow = 0
            lastBitrateTickNs = nowNs
        }
    }

    private func handleServerMessageOnQueue(type: WireMessageType, payload: Data) {
        switch type {
        case .helloServer:
            guard let hello = try? JSONDecoder().decode(HelloServerMessage.self, from: payload) else { return }
            let isV3Pet = (hello.protocolVersion >= 3 && hello.petCamera != nil)
            telemetry.protocolVersion = hello.protocolVersion
            telemetry.isPetCameraV3 = isV3Pet
            telemetry.petCameraHello = isV3Pet ? hello.petCamera : nil
            telemetry.allowModeSwitch = hello.allowPhoneModeSwitch
            telemetry.currentModeRaw = hello.modeRaw
            telemetry.currentSbsLayoutRaw = hello.sbsLayoutRaw
            telemetry.streamWidth = hello.width
            telemetry.streamHeight = hello.height
            telemetry.lastCommandError = nil

            if state.lastKnownHostProtocolVersion != hello.protocolVersion {
                state.lastKnownHostProtocolVersion = hello.protocolVersion
                telemetry.cachedHostProtocolVersion = hello.protocolVersion
                _ = store.save(state)
            }

            if !receivedServerHello {
                receivedServerHello = true
                transportActiveForPings = true
                // Against v1/v2 hosts (which never use notice-only connections), hello starts the session as before.
                // Against v3 pet-camera hosts, wait for the first `0x0B` or `0x02` before reporting `isConnected`
                // or resetting `reconnectCount` / `lastSessionNotice`.
                if !isV3Pet {
                    confirmRealSessionStartedOnQueue()
                }
                if let conn = self.connection {
                    startPingLoopOnQueue(conn: conn)
                }
                let cb = pendingHandshakeCallback
                pendingHandshakeCallback = nil
                cb?(true, nil)
            }
            publishTelemetryOnQueue()

        case .hostStateSync:
            guard let stateSync = try? JSONDecoder().decode(HostStateSyncMessage.self, from: payload) else { return }
            if telemetry.hasConfirmedHostState && stateSync.stateSeq < telemetry.hostState.stateSeq {
                return
            }
            if !telemetry.isPetCameraV3 {
                confirmRealSessionStartedOnQueue()
            }
            telemetry.protocolVersion = max(telemetry.protocolVersion, stateSync.protocolVersion)
            telemetry.hasConfirmedHostState = true
            telemetry.hostState = stateSync
            telemetry.allowModeSwitch = stateSync.allowPhoneModeSwitch
            telemetry.currentModeRaw = stateSync.modeRaw
            telemetry.currentSbsLayoutRaw = stateSync.sbsLayoutRaw
            if stateSync.powerState != .on {
                telemetry.fps = 0.0
                telemetry.bitrateMbps = 0.0
                frameArrivalTimes.removeAll()
                lastVideoFrameSeqInEpoch = nil
                onStreamFlushed?()
            }
            onHostStateReceived?(stateSync)
            publishTelemetryOnQueue()

        case .petCameraState:
            guard telemetry.isPetCameraV3,
                  let petState = try? JSONDecoder().decode(PetCameraStateMessage.self, from: payload) else { return }
            if telemetry.hasConfirmedPetCameraState && petState.stateSeq < telemetry.petState.stateSeq {
                return
            }
            confirmRealSessionStartedOnQueue()
            telemetry.hasConfirmedPetCameraState = true
            telemetry.petState = petState
            telemetry.cachedPairedRole = petState.role
            telemetry.lastSawSharingEnabled = petState.sharingEnabled
            telemetry.lastSawOutsideEnabled = petState.outsideEnabled
            if petState.outsideStatus == "address_changed" {
                telemetry.sawAddressChangeNotice = true
            } else if let status = petState.outsideStatus, status != "address_changed" {
                telemetry.sawAddressChangeNotice = false
            }

            var stateChanged = false
            if state.pairedRole != petState.role {
                state.pairedRole = petState.role
                stateChanged = true
            }
            if state.pairedDeviceId != petState.deviceId {
                state.pairedDeviceId = petState.deviceId
                stateChanged = true
            }
            // Replaced whole on every 0x0B, capped at 4; an absent field clears it.
            let advertisedCandidates = petState.outsideCandidates.map { Array($0.prefix(Self.maxOutsideCandidates)) }
            if state.outsideCandidates != advertisedCandidates {
                state.outsideCandidates = advertisedCandidates
                stateChanged = true
            }
            if let ep = petState.outsideEndpoint, state.outsideEndpoint != ep {
                state.outsideEndpoint = ep
                telemetry.storedOutsideEndpoint = ep
                telemetry.sawAddressChangeNotice = false
                stateChanged = true
            }
            if stateChanged {
                _ = store.save(state)
            }

            if !petState.liveAllowed || !petState.watching {
                telemetry.fps = 0.0
                telemetry.bitrateMbps = 0.0
                frameArrivalTimes.removeAll()
                lastVideoFrameSeqInEpoch = nil
                onStreamFlushed?()
            }
            if petState.privacyMode {
                onPlaybackStreamFlushed?()
            }
            onPetCameraStateReceived?(petState)
            publishTelemetryOnQueue()

        case .commandResult:
            guard telemetry.isPetCameraV3,
                  let result = try? JSONDecoder().decode(CommandResultMessage.self, from: payload) else { return }
            let cmdErr = CommandResultError(
                code: result.error,
                message: result.message ?? result.error ?? "Command failed."
            )

            // 1. Pending `0x14` thumbnail requests answered with `0x0D` (e.g. privacy_mode / not_found):
            //    resolve cleanly to placeholder WITHOUT setting `telemetry.lastCommandError` or looping (§5.9)!
            if let thumbCb = pendingRollThumbCallbacks.removeValue(forKey: result.requestId) {
                thumbCb(.failure(cmdErr))
                onCommandResult?(result)
                return
            }

            // 2. Pending `0x12` camera-roll list request answered with `0x0D`:
            if let listCb = pendingRollListCallbacks.removeValue(forKey: result.requestId) {
                listCb(.failure(cmdErr))
                onCommandResult?(result)
                return
            }

            // 3. Pending `0x16` playback request answered with `0x0D`:
            if let pbCb = pendingPlaybackCallbacks.removeValue(forKey: result.requestId) {
                pbCb(.failure(cmdErr))
                onCommandResult?(result)
                return
            }

            // 4. Pending or fire-and-forget `0x0C` petCameraCmd:
            if result.ok {
                telemetry.lastCommandError = nil
            } else {
                telemetry.lastCommandError = cmdErr.message
            }
            if let cmdCb = pendingCommandCallbacks.removeValue(forKey: result.requestId) {
                if result.ok {
                    cmdCb(.success(result))
                } else {
                    cmdCb(.failure(cmdErr))
                }
            }
            onCommandResult?(result)
            publishTelemetryOnQueue()

        case .rollList:
            guard telemetry.isPetCameraV3,
                  let listMsg = try? JSONDecoder().decode(RollListMessage.self, from: payload) else { return }
            if let cb = pendingRollListCallbacks.removeValue(forKey: listMsg.requestId) {
                cb(.success(listMsg))
            }

        case .rollThumb:
            guard telemetry.isPetCameraV3,
                  let thumbMsg = try? JSONDecoder().decode(RollThumbMessage.self, from: payload) else { return }
            guard let cb = pendingRollThumbCallbacks.removeValue(forKey: thumbMsg.requestId) else { return }
            guard thumbMsg.jpeg.count <= RollThumbMessage.maxThumbnailBytes else {
                cb(.failure(CommandResultError(code: "oversize", message: "Thumbnail exceeded the 64 KiB limit.")))
                return
            }
            cb(.success(thumbMsg))

        case .playbackState:
            guard telemetry.isPetCameraV3,
                  let pbState = try? JSONDecoder().decode(PlaybackStateMessage.self, from: payload) else { return }
            if let reqId = pbState.requestId, let cb = pendingPlaybackCallbacks.removeValue(forKey: reqId) {
                cb(.success(pbState))
            } else if pbState.requestId == nil,
                      let currentSid = playbackStreamId,
                      pbState.streamId != currentSid {
                // Ignore unsolicited `0x17` (such as old stream's `stopped`) when a newer stream is already active (§5.9).
                return
            }

            telemetry.latestPlaybackState = pbState
            if pbState.state.freesStreamAllocation {
                if playbackStreamId == nil || playbackStreamId == pbState.streamId {
                    playbackStreamId = nil
                    playbackClipId = nil
                    playbackFormatDesc = nil
                    playbackStreamEpoch = 0
                }
                onPlaybackStreamFlushed?()
            } else {
                playbackStreamId = pbState.streamId
                playbackClipId = pbState.clipId
            }
            onPlaybackStateUpdated?(pbState)
            publishTelemetryOnQueue()

        case .playbackConfig:
            guard telemetry.isPetCameraV3,
                  let cfg = try? JSONDecoder().decode(PlaybackConfigMessage.self, from: payload),
                  cfg.parameterSets.count >= 2 else { return }
            if let activeSid = playbackStreamId, cfg.streamId != activeSid {
                return
            }
            guard let fmt = Self.makeH264FormatDescription(
                parameterSets: cfg.parameterSets,
                nalLengthHeaderBytes: cfg.nalLengthHeaderBytes
            ) else { return }
            if playbackStreamEpoch != 0 && playbackStreamEpoch != cfg.streamEpoch {
                onPlaybackStreamFlushed?()
            }
            playbackStreamId = cfg.streamId
            playbackClipId = cfg.clipId
            playbackFormatDesc = fmt
            playbackStreamEpoch = cfg.streamEpoch

        case .playbackAccessUnit:
            guard telemetry.isPetCameraV3,
                  let au = PlaybackAccessUnit.decode(from: payload),
                  let activeSid = playbackStreamId,
                  au.streamId == activeSid,
                  let fmt = playbackFormatDesc,
                  au.header.streamEpoch == playbackStreamEpoch else {
                return
            }
            if telemetry.hasConfirmedPetCameraState && telemetry.petState.privacyMode {
                return
            }
            guard let sb = Self.makeImmediateDisplaySampleBuffer(
                avccData: au.avccPayload,
                presentationTimeNs: au.header.hostCaptureNs,
                formatDescription: fmt
            ) else { return }
            telemetry.decodedPlaybackFrames += 1
            onPlaybackSampleBufferReady?(sb)
            onPlaybackMediaTimeUpdated?(au.streamId, Double(au.header.hostCaptureNs) / 1e9)

        case .sessionNotice:
            guard let notice = try? JSONDecoder().decode(SessionNoticeMessage.self, from: payload) else { return }
            telemetry.lastSessionNotice = notice
            telemetry.tlsRefusalNotice = nil
            switch notice.reason {
            case .revoked:
                handleRevokedByHostOnQueue()
            case .replaced:
                shouldStayConnected = false
                reconnectWork?.cancel()
                reconnectWork = nil
                lanFallbackWork?.cancel()
                lanFallbackWork = nil
                browser?.cancel()
                browser = nil
                teardownConnectionOnQueue()
                let msg = notice.message ?? PetSessionNoticeReason.replaced.defaultSentence
                telemetry.connectionState = "Connected Elsewhere"
                telemetry.lastError = msg
                onStreamFlushed?()
                onPlaybackStreamFlushed?()
                publishTelemetryOnQueue()
            case .outsideDisabled:
                telemetry.lastSawOutsideEnabled = false
                telemetry.lastError = notice.message ?? PetSessionNoticeReason.outsideDisabled.defaultSentence
                publishTelemetryOnQueue()
            case .tooManySessions:
                telemetry.lastError = PetCameraUserStrings.tooManySessionsUserText
                publishTelemetryOnQueue()
            case .addressChanged:
                telemetry.sawAddressChangeNotice = true
                if notice.closing {
                    telemetry.lastError = notice.message ?? PetSessionNoticeReason.addressChanged.defaultSentence
                }
                publishTelemetryOnQueue()
            case .hostShuttingDown, .unknown:
                telemetry.lastError = notice.message ?? notice.reason.defaultSentence
                publishTelemetryOnQueue()
            }

        case .streamConfig:
            guard let cfg = try? JSONDecoder().decode(StreamConfigMessage.self, from: payload),
                  cfg.parameterSets.count >= 2 else { return }
            confirmRealSessionStartedOnQueue()
            rebuildFormatDescriptionOnQueue(cfg)

        case .videoAccessUnit:
            guard let (hdr, avcc) = VideoAccessUnitHeader.decode(from: payload) else { return }
            handleVideoAccessUnitOnQueue(header: hdr, avccData: avcc)

        case .pong:
            let clientRecvNs = Self.monotonicNs()
            guard let pong = try? JSONDecoder().decode(PongMessage.self, from: payload),
                  clientRecvNs >= pong.clientSendNs else { return }
            let totalElapsed = clientRecvNs - pong.clientSendNs
            let serverProc = pong.serverSendNs >= pong.serverRecvNs ? (pong.serverSendNs - pong.serverRecvNs) : 0
            let rtt = totalElapsed > serverProc ? (totalElapsed - serverProc) : totalElapsed

            // Cristian offset: host_ns - client_ns = ((t2 - t1) + (t3 - t4)) / 2
            let t2_minus_t1 = Int64(bitPattern: pong.serverRecvNs) &- Int64(bitPattern: pong.clientSendNs)
            let t3_minus_t4 = Int64(bitPattern: pong.serverSendNs) &- Int64(bitPattern: clientRecvNs)
            let offset = (t2_minus_t1 / 2) &+ (t3_minus_t4 / 2)

            rttSamples.append((rtt: rtt, offset: offset))
            if rttSamples.count > 16 { rttSamples.removeFirst() }
            if let best = rttSamples.min(by: { $0.rtt < $1.rtt }) {
                bestRttNs = best.rtt
                clockOffsetNs = best.offset
                telemetry.rttMs = Double(best.rtt) / 1e6
            }

        default:
            break
        }
    }

    private static func makeH264FormatDescription(
        parameterSets: [Data],
        nalLengthHeaderBytes: Int
    ) -> CMVideoFormatDescription? {
        var ptrs: [UnsafePointer<UInt8>] = []
        var sizes: [Int] = []
        var fmtOut: CMFormatDescription?

        func withAllPointers(_ index: Int) {
            if index == parameterSets.count {
                _ = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault,
                    parameterSetCount: parameterSets.count,
                    parameterSetPointers: ptrs,
                    parameterSetSizes: sizes,
                    nalUnitHeaderLength: Int32(nalLengthHeaderBytes),
                    formatDescriptionOut: &fmtOut
                )
                return
            }
            parameterSets[index].withUnsafeBytes { rawBuf in
                if let base = rawBuf.bindMemory(to: UInt8.self).baseAddress {
                    ptrs.append(base)
                    sizes.append(rawBuf.count)
                    withAllPointers(index + 1)
                }
            }
        }
        withAllPointers(0)
        return fmtOut
    }

    private static func makeImmediateDisplaySampleBuffer(
        avccData: Data,
        presentationTimeNs: UInt64,
        formatDescription: CMVideoFormatDescription
    ) -> CMSampleBuffer? {
        var blockBufOut: CMBlockBuffer?
        let len = avccData.count
        guard len > 0,
              CMBlockBufferCreateWithMemoryBlock(
                  allocator: kCFAllocatorDefault,
                  memoryBlock: nil,
                  blockLength: len,
                  blockAllocator: kCFAllocatorDefault,
                  customBlockSource: nil,
                  offsetToData: 0,
                  dataLength: len,
                  flags: 0,
                  blockBufferOut: &blockBufOut
              ) == kCMBlockBufferNoErr,
              let blockBuf = blockBufOut else { return nil }

        let copyOk = avccData.withUnsafeBytes { rawPtr -> Bool in
            guard let base = rawPtr.baseAddress else { return false }
            return CMBlockBufferReplaceDataBytes(
                with: base,
                blockBuffer: blockBuf,
                offsetIntoDestination: 0,
                dataLength: len
            ) == kCMBlockBufferNoErr
        }
        guard copyOk else { return nil }

        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: 30),
            presentationTimeStamp: CMTime(value: Int64(presentationTimeNs / 1000), timescale: 1_000_000),
            decodeTimeStamp: .invalid
        )
        var sampleSize = len
        var sbOut: CMSampleBuffer?
        guard CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuf,
            formatDescription: formatDescription,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sbOut
        ) == noErr, let sb = sbOut else { return nil }

        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: true) {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(
                dict,
                Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque()
            )
        }
        return sb
    }

    private func rebuildFormatDescriptionOnQueue(_ cfg: StreamConfigMessage) {
        if let ds = decompSession {
            VTDecompressionSessionInvalidate(ds)
            decompSession = nil
        }
        formatDesc = nil

        guard let fmt = Self.makeH264FormatDescription(
            parameterSets: cfg.parameterSets,
            nalLengthHeaderBytes: cfg.nalLengthHeaderBytes
        ) else { return }
        self.formatDesc = fmt
        self.currentStreamEpoch = cfg.streamEpoch
        self.lastVideoFrameSeqInEpoch = nil
        self.telemetry.recentSeqDrops = 0
        self.telemetry.currentModeRaw = cfg.modeRaw
        self.telemetry.currentSbsLayoutRaw = cfg.sbsLayoutRaw
        self.telemetry.streamWidth = cfg.width
        self.telemetry.streamHeight = cfg.height
        publishTelemetryOnQueue()
    }

    private func handleVideoAccessUnitOnQueue(header: VideoAccessUnitHeader, avccData: Data) {
        guard let fmt = formatDesc, header.streamEpoch == currentStreamEpoch else { return }
        if telemetry.isPetCameraV3 && telemetry.hasConfirmedPetCameraState && !telemetry.petState.liveAllowed {
            return
        }
        guard let sb = Self.makeImmediateDisplaySampleBuffer(
            avccData: avccData,
            presentationTimeNs: header.hostCaptureNs,
            formatDescription: fmt
        ) else { return }

        onSampleBufferReady?(sb)

        let nowClientNs = Self.monotonicNs()
        if let prevSeq = lastVideoFrameSeqInEpoch, header.frameSeq > prevSeq + 1 {
            telemetry.recentSeqDrops = header.frameSeq - prevSeq - 1
        } else {
            telemetry.recentSeqDrops = 0
        }
        lastVideoFrameSeqInEpoch = header.frameSeq
        telemetry.receivedFrames += 1
        frameArrivalTimes.append(nowNs: nowClientNs)

        if frameArrivalTimes.count >= 2 {
            let span = frameArrivalTimes.last! - frameArrivalTimes.first!
            if span > 0 {
                telemetry.fps = Double(frameArrivalTimes.count - 1) * 1e9 / Double(span)
            }
        }

        // Compute Cristian-synced Host Capture -> Client Display Enqueue latency
        let estimatedHostNowNs = UInt64(bitPattern: Int64(bitPattern: nowClientNs) &+ clockOffsetNs)
        if bestRttNs != .max && estimatedHostNowNs >= header.hostCaptureNs {
            let latMs = Double(estimatedHostNowNs - header.hostCaptureNs) / 1e6
            if latMs >= 0.0 && latMs < 5000.0 {
                telemetry.hostToDisplayLatencyMs = latMs
            }
        }

        // If synthetic clock overlay flag is set, decode Y-plane in RAM via VTDecompressionSession and read the 64-bit pixel barcode!
        if header.hasClockOverlay {
            decodeAndMeasureClockBarcodeOnQueue(sampleBuffer: sb, fmt: fmt)
        }

        if (telemetry.receivedFrames % 3) == 0 {
            publishTelemetryOnQueue()
        }
    }

    private func decodeAndMeasureClockBarcodeOnQueue(sampleBuffer: CMSampleBuffer, fmt: CMVideoFormatDescription) {
        if decompSession == nil {
            let attrs: [CFString: Any] = [
                kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            ]
            var dsOut: VTDecompressionSession?
            VTDecompressionSessionCreate(
                allocator: kCFAllocatorDefault,
                formatDescription: fmt,
                decoderSpecification: nil,
                imageBufferAttributes: attrs as CFDictionary,
                outputCallback: nil,
                decompressionSessionOut: &dsOut
            )
            decompSession = dsOut
        }
        guard let ds = decompSession else { return }

        var flagsOut = VTDecodeInfoFlags()
        VTDecompressionSessionDecodeFrame(
            ds,
            sampleBuffer: sampleBuffer,
            flags: [],
            infoFlagsOut: &flagsOut
        ) { [weak self] status, _, imageBuffer, _, _ in
            guard status == noErr, let self = self, let pb = imageBuffer else { return }
            let decodeDoneClientNs = Self.monotonicNs()
            CVPixelBufferLockBaseAddress(pb, .readOnly)
            defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }

            let w = CVPixelBufferGetWidthOfPlane(pb, 0)
            let h = CVPixelBufferGetHeightOfPlane(pb, 0)
            let stride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
            guard let yBase = CVPixelBufferGetBaseAddressOfPlane(pb, 0)?.assumingMemoryBound(to: UInt8.self) else {
                return
            }

            if let (_, barcodeTsLow32Us) = Self.decodeClockBarcodeFromLuma(yBase: yBase, width: w, height: h, stride: stride) {
                self.queue.async {
                    guard self.bestRttNs != .max else { return }
                    let estHostNowNs = UInt64(bitPattern: Int64(bitPattern: decodeDoneClientNs) &+ self.clockOffsetNs)
                    let nowLow32Us = UInt32((estHostNowNs / 1000) & 0xFFFF_FFFF)
                    let diffUs = nowLow32Us &- barcodeTsLow32Us
                    if diffUs < 2_000_000 { // Sanity < 2.0 s
                        self.telemetry.barcodePixelLatencyMs = Double(diffUs) / 1000.0
                        self.telemetry.barcodeVerifiedFrames += 1
                    }
                }
            }
        }
    }

    static func decodeClockBarcodeFromLuma(
        yBase: UnsafePointer<UInt8>,
        width: Int,
        height: Int,
        stride: Int
    ) -> (seq24: UInt32, tsLow32Us: UInt32)? {
        guard width >= 64, height >= 16 else { return nil }
        let barH = max(16, height / 22)
        let sampleY = min(height - 1, barH / 2)
        let row = yBase.advanced(by: sampleY * stride)

        var word: UInt64 = 0
        for bit in 0..<64 {
            let x0 = (bit * width) / 64
            let x1 = ((bit + 1) * width) / 64
            let cx = (x0 + x1) / 2
            if row[cx] >= 128 {
                word |= (1 << UInt64(63 - bit))
            }
        }
        let magic = UInt8((word >> 56) & 0xFF)
        guard magic == 0xA5 else { return nil }
        let seq24 = UInt32((word >> 32) & 0xFF_FFFF)
        let tsLow32 = UInt32(word & 0xFFFF_FFFF)
        return (seq24, tsLow32)
    }

    private func publishTelemetryOnQueue() {
        let snap = telemetry
        onTelemetryUpdated?(snap)
    }
}

private extension Array where Element == UInt64 {
    mutating func append(nowNs: UInt64) {
        append(nowNs)
        while count > 2 && (nowNs - first!) > 2_000_000_000 {
            removeFirst()
        }
    }
}

private final class LockedFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var setFlag = false
    var isSet: Bool {
        lock.lock()
        defer { lock.unlock() }
        return setFlag
    }
    func trySet() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if setFlag { return false }
        setFlag = true
        return true
    }
}

private final class LockedBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var val: T
    init(_ initial: T) { self.val = initial }
    func set(_ v: T) { lock.lock(); val = v; lock.unlock() }
    func get() -> T { lock.lock(); defer { lock.unlock() }; return val }
}
