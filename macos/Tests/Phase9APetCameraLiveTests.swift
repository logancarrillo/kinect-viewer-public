import AVFoundation
import CoreGraphics
import CryptoKit
import Foundation
import Network
import XCTest
@testable import KinectViewer

private final class Phase9SyncBox<T>: @unchecked Sendable {
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

/// Synthetic loopback TLS 1.3 Pet Camera host implementing Protocol v3 (W6) for unit testing.
/// Uses synthetic in-memory BGRA test patterns only; never touches real sensors, Keychain, or disk.
private final class FakeV3PetCameraHost: @unchecked Sendable {
    struct PairedClientRecord: Sendable {
        let certSHA256: Data
        let deviceId: String
        var name: String
        var role: PetRole
        var kind: String
        let pairedAtUnixMs: UInt64
    }

    private final class SessionEntry: @unchecked Sendable {
        let id = UUID()
        let conn: NWConnection
        let reader = WireFrameReader()
        let certSHA256: Data
        let deviceId: String
        var negotiatedVersion: Int = 1
        var liveWatching: Bool = true
        var liveQuality: LiveQuality = .auto
        var liveTier: LiveTier = .high

        init(conn: NWConnection, certSHA256: Data, deviceId: String) {
            self.conn = conn
            self.certSHA256 = certSHA256
            self.deviceId = deviceId
        }
    }

    let serverIdentity: CryptoIdentity
    private let netQueue = DispatchQueue(label: "kinectviewer.tests.fakev3host")
    private var streamListener: NWListener?
    private var pairListener: NWListener?

    private var serviceId: String = "v3fake000001"
    private var activeQRToken: Data = Data(repeating: 0x42, count: 16)
    private var nextPairingRole: PetRole = .owner

    private var pairedDevicesByHash: [Data: PairedClientRecord] = [:]
    private var sessions: [UUID: SessionEntry] = [:]
    private var stateSeq: UInt64 = 0

    // Host state
    private var powerState: WireSensorPowerState = .on
    private var allowPhoneModeSwitch: Bool = true
    private var allowPhoneSensorPower: Bool = true
    private var modeRaw: Int = 1 // Colour allowed over H.264 on v3 pet camera
    private var sbsLayoutRaw: Int = 0
    private var cutoffEnabled: Bool = false
    private var isHostFrozen: Bool = false
    private var isReplayingOnHost: Bool = false
    private var sharingEnabled: Bool = true
    private var sharingScheduleOff: Bool = false
    private var privacyMode: Bool = false
    private var forcedUnavailableReasonOverride: LiveUnavailableReason?
    private var simulateTLSRejection: Bool = false
    private var simulateNoticeOnlyTooManySessions: Bool = false

    // Recorded metrics for test assertions
    private let metricsBox = Phase9SyncBox<(
        receivedClientTypes: [WireMessageType],
        receivedPingVersions: [Int?],
        receivedPetCommands: [PetCameraCommandMessage],
        receivedLiveViewReqs: [LiveViewRequestMessage],
        receivedPowerReqs: [SensorPowerControlMessage],
        receivedPairRequests: [PairRequestMessage],
        streamConnectionAttempts: Int,
        oversizeHeaderClosedCount: Int,
        sensorPowerTurnedOffByUnpairAll: Bool
    )>(([], [], [], [], [], [], 0, 0, false))

    private(set) var streamPort: UInt16 = 0
    private(set) var pairPort: UInt16 = 0

    init() throws {
        self.serverIdentity = try CryptoIdentity.generateNew(commonName: "kv3-fake-pc")
        try startListeners()
    }

    func shutdown() {
        netQueue.sync {
            for (_, s) in sessions {
                s.conn.cancel()
            }
            sessions.removeAll()
            streamListener?.cancel()
            streamListener = nil
            pairListener?.cancel()
            pairListener = nil
        }
    }

    var metrics: (
        receivedClientTypes: [WireMessageType],
        receivedPingVersions: [Int?],
        receivedPetCommands: [PetCameraCommandMessage],
        receivedLiveViewReqs: [LiveViewRequestMessage],
        receivedPowerReqs: [SensorPowerControlMessage],
        receivedPairRequests: [PairRequestMessage],
        streamConnectionAttempts: Int,
        oversizeHeaderClosedCount: Int,
        sensorPowerTurnedOffByUnpairAll: Bool
    ) {
        metricsBox.read()
    }

    var currentPowerState: WireSensorPowerState {
        netQueue.sync { powerState }
    }

    func preparePairingOffer(role: PetRole) -> String {
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
                pairPort: self.pairPort
            ).toURIString()
        }
    }

    func setHostAvailability(
        privacyMode: Bool? = nil,
        sharingEnabled: Bool? = nil,
        sharingScheduleOff: Bool? = nil,
        isReplayingOnHost: Bool? = nil,
        forcedReason: LiveUnavailableReason?? = nil
    ) {
        netQueue.sync {
            if let p = privacyMode { self.privacyMode = p }
            if let s = sharingEnabled { self.sharingEnabled = s }
            if let sch = sharingScheduleOff { self.sharingScheduleOff = sch }
            if let rep = isReplayingOnHost { self.isReplayingOnHost = rep }
            if let fr = forcedReason { self.forcedUnavailableReasonOverride = fr }
            broadcastAllSessionsOnQueue()
        }
    }

    func setSimulateTLSRejection(_ reject: Bool, closeExistingSessions: Bool = false) {
        netQueue.sync {
            self.simulateTLSRejection = reject
            if closeExistingSessions {
                for (_, s) in self.sessions {
                    s.conn.cancel()
                }
                self.sessions.removeAll()
            }
        }
    }

    func setSimulateNoticeOnlyTooManySessions(_ enabled: Bool, closeExistingSessions: Bool = false) {
        netQueue.sync {
            self.simulateNoticeOnlyTooManySessions = enabled
            if closeExistingSessions {
                for (_, s) in self.sessions {
                    s.conn.cancel()
                }
                self.sessions.removeAll()
            }
        }
    }

    func pushRawJSONFrameToAllV3Sessions(type: WireMessageType, jsonString: String) {
        netQueue.sync {
            let pkt = WirePacket.pack(type: type, payload: Data(jsonString.utf8))
            for (_, s) in sessions where s.negotiatedVersion >= 3 {
                s.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
    }

    func sendEncodedSyntheticFrames(count: Int, width: Int = 320, height: Int = 240) {
        let encoder = VideoToolboxEncoder()
        let generatedBox = Phase9SyncBox<[EncodedVideoFrame]>([])
        let sem = DispatchSemaphore(value: 0)
        encoder.onEncodedFrame = { frame in
            generatedBox.update { $0.append(frame) }
            sem.signal()
        }

        let stride = width * 4
        var bgra = [UInt8](repeating: 0, count: stride * height)
        for i in 0..<count {
            for y in 0..<height {
                for x in 0..<width {
                    let idx = y * stride + x * 4
                    bgra[idx] = UInt8((x + i * 11) & 0xFF)
                    bgra[idx + 1] = UInt8((y + i * 7) & 0xFF)
                    bgra[idx + 2] = 180
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
                    hostCaptureNs: UInt64(i + 1) * 33_333_333,
                    modeRaw: 1,
                    sbsRaw: 0,
                    hasClockOverlay: false
                )
            }
            _ = sem.wait(timeout: .now() + 2.0)
        }
        encoder.teardown()
        let generated = generatedBox.read()

        netQueue.sync {
            for (_, s) in sessions where s.liveWatching {
                for frame in generated {
                    if let cfg = frame.configIfChanged,
                       let cfgPkt = try? WirePacket.packJSON(type: .streamConfig, cfg) {
                        s.conn.send(content: cfgPkt, completion: .contentProcessed { _ in })
                    }
                    let auPkt = WirePacket.pack(type: .videoAccessUnit, payload: frame.wirePayload)
                    s.conn.send(content: auPkt, completion: .contentProcessed { _ in })
                }
            }
        }
    }

    func revokeDevice(deviceId: String) {
        netQueue.sync {
            revokeDeviceOnQueue(deviceId: deviceId)
        }
    }

    /// Simulates "Unpair all" ("Revoke all", `docs/protocol-v3-pet-camera.md` §2.3 & §2.4):
    /// revokes every device, rotates `serviceId`, and fires the camera turn-off rule.
    func unpairAllDevices() {
        netQueue.sync {
            let hadAnyDevice = !pairedDevicesByHash.isEmpty
            let allIds = pairedDevicesByHash.values.map(\.deviceId)
            for id in allIds {
                revokeDeviceOnQueue(deviceId: id)
            }
            if hadAnyDevice && powerState == .on {
                powerState = .off
                metricsBox.update { $0.sensorPowerTurnedOffByUnpairAll = true }
            }
            serviceId = "v3rotated9999"
        }
    }

    private func revokeDeviceOnQueue(deviceId: String) {
        if let key = pairedDevicesByHash.first(where: { $0.value.deviceId == deviceId })?.key {
            pairedDevicesByHash.removeValue(forKey: key)
        }
        let matching = sessions.values.filter { $0.deviceId == deviceId }
        for s in matching {
            sessions.removeValue(forKey: s.id)
            if s.negotiatedVersion >= 3 {
                let notice = SessionNoticeMessage(
                    reason: .revoked,
                    closing: true,
                    message: PetSessionNoticeReason.revoked.defaultSentence
                )
                if let pkt = try? WirePacket.packJSON(type: .sessionNotice, notice) {
                    s.conn.send(content: pkt, isComplete: true, completion: .contentProcessed { [weak self] _ in
                        self?.netQueue.asyncAfter(deadline: .now() + .milliseconds(40)) {
                            s.conn.cancel()
                        }
                    })
                } else {
                    s.conn.cancel()
                }
            } else {
                s.conn.cancel()
            }
        }
        if pairedDevicesByHash.isEmpty {
            serviceId = "v3rotated0002"
        } else {
            broadcastAllSessionsOnQueue()
        }
    }

    private func startListeners() throws {
        // 1. Pairing listener (accepts any client leaf cert)
        let pairReady = DispatchSemaphore(value: 0)
        let pairTLS = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdentity,
            requireClientCert: true,
            verifyQueue: netQueue
        ) { _, _ in true }
        let pListener = try NWListener(using: pairTLS, on: .any)
        pListener.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.pairPort = pListener.port?.rawValue ?? 0
                pairReady.signal()
            }
        }
        pListener.newConnectionHandler = { [weak self] conn in
            self?.handlePairConnectionOnQueue(conn)
        }
        self.pairListener = pListener
        pListener.start(queue: netQueue)
        _ = pairReady.wait(timeout: .now() + 3.0)

        // 2. Stream listener (admits ONLY listed devices in TLS handshake)
        let streamReady = DispatchSemaphore(value: 0)
        let peerHashPerConn = Phase9SyncBox<Data?>(nil)
        let streamTLS = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdentity,
            requireClientCert: true,
            verifyQueue: netQueue
        ) { [weak self] peerHash, _ in
            guard let self = self else { return false }
            self.metricsBox.update { $0.streamConnectionAttempts += 1 }
            guard !self.simulateTLSRejection else {
                return false
            }
            guard self.pairedDevicesByHash[peerHash] != nil else {
                return false
            }
            peerHashPerConn.update { $0 = peerHash }
            return true
        }
        let sListener = try NWListener(using: streamTLS, on: .any)
        sListener.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.streamPort = sListener.port?.rawValue ?? 0
                streamReady.signal()
            }
        }
        sListener.newConnectionHandler = { [weak self] conn in
            self?.handleStreamConnectionOnQueue(conn, lastVerifiedHashBox: peerHashPerConn)
        }
        self.streamListener = sListener
        sListener.start(queue: netQueue)
        _ = streamReady.wait(timeout: .now() + 3.0)
    }

    private func handlePairConnectionOnQueue(_ conn: NWConnection) {
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            if case .ready = st {
                conn.receive(minimumIncompleteLength: 1, maximumLength: 70_000) { data, _, _, err in
                    guard err == nil, let data = data, data.count >= WireHeader.size else {
                        conn.cancel()
                        return
                    }
                    let payloadLen = Int(data.readUInt32BE(at: 4))
                    if WireHeader.size + payloadLen > WireHeader.maxClientFrameBytes {
                        self.metricsBox.update { $0.oversizeHeaderClosedCount += 1 }
                        conn.cancel()
                        return
                    }
                    let reader = WireFrameReader()
                    reader.append(data)
                    guard case .frame(let t, _, let payload) = reader.nextFrame(),
                          t == .pairRequest,
                          let req = try? JSONDecoder().decode(PairRequestMessage.self, from: payload) else {
                        conn.cancel()
                        return
                    }
                    self.metricsBox.update {
                        $0.receivedClientTypes.append(.pairRequest)
                        $0.receivedPairRequests.append(req)
                    }

                    let clientHash = Data(CryptoKit.SHA256.hash(data: req.clientCertDER))
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
                    let role = self.nextPairingRole
                    let storedName = req.deviceName ?? "iPhone"
                    let rec = PairedClientRecord(
                        certSHA256: clientHash,
                        deviceId: devId,
                        name: storedName,
                        role: role,
                        kind: req.deviceKind ?? "iphone",
                        pairedAtUnixMs: 1_700_000_000_000
                    )
                    self.pairedDevicesByHash[clientHash] = rec

                    let srvProof = CryptoIdentity.computePairingProof(
                        secret: self.activeQRToken,
                        serverCertSHA256: self.serverIdentity.certificateSHA256,
                        clientCertSHA256: clientHash,
                        role: "kv2-pair-server"
                    )
                    let resp = PairResponseMessage(
                        accepted: true,
                        serviceId: self.serviceId,
                        streamPort: self.streamPort,
                        serverProofHMAC: srvProof,
                        message: "Paired",
                        role: role,
                        deviceId: devId,
                        deviceName: storedName,
                        protocolVersion: 3
                    )
                    if let pkt = try? WirePacket.packJSON(type: .pairResponse, resp) {
                        conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                    }
                    self.broadcastAllSessionsOnQueue()
                }
            }
        }
        conn.start(queue: netQueue)
    }

    private func handleStreamConnectionOnQueue(_ conn: NWConnection, lastVerifiedHashBox: Phase9SyncBox<Data?>) {
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self else { return }
            if case .ready = st {
                guard let peerHash = lastVerifiedHashBox.read(),
                      let rec = self.pairedDevicesByHash[peerHash] else {
                    conn.cancel()
                    return
                }
                let entry = SessionEntry(conn: conn, certSHA256: peerHash, deviceId: rec.deviceId)
                self.sessions[entry.id] = entry

                // Send 0x01 helloServer with protocolVersion=3 and petCamera capability
                let hello = HelloServerMessage(
                    protocolVersion: 3,
                    modeRaw: self.modeRaw,
                    sbsLayoutRaw: self.sbsLayoutRaw,
                    allowPhoneModeSwitch: rec.role == .owner && self.allowPhoneModeSwitch,
                    width: 1280,
                    height: 720,
                    codecRaw: WireCodecKind.h264.rawValue,
                    petCamera: PetCameraHello(
                        specVersion: 1,
                        maxWatchers: 4,
                        cameraRoll: false,
                        outside: false,
                        qualities: ["auto", "high", "medium", "low"]
                    )
                )
                if let pkt = try? WirePacket.packJSON(type: .helloServer, hello) {
                    conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
                self.receiveLoopOnQueue(entry: entry)
            }
        }
        conn.start(queue: netQueue)
    }

    private func receiveLoopOnQueue(entry: SessionEntry) {
        entry.conn.receive(minimumIncompleteLength: 1, maximumLength: 131_072) { [weak self] data, _, isComplete, err in
            guard let self = self, self.sessions[entry.id] != nil else { return }
            if let data = data, !data.isEmpty {
                // Enforce 64 KiB device-to-PC cap at header before reading payload (v3 §1)
                if data.count >= WireHeader.size &&
                    data[data.startIndex] == WireHeader.magic0 &&
                    data[data.startIndex + 1] == WireHeader.magic1 {
                    let declaredLen = Int(data.readUInt32BE(at: 4))
                    if WireHeader.size + declaredLen > WireHeader.maxClientFrameBytes {
                        self.metricsBox.update { $0.oversizeHeaderClosedCount += 1 }
                        self.sessions.removeValue(forKey: entry.id)
                        entry.conn.cancel()
                        return
                    }
                }

                entry.reader.append(data)
                while true {
                    switch entry.reader.nextFrame() {
                    case .needMoreData:
                        break
                    case .protocolViolation:
                        self.sessions.removeValue(forKey: entry.id)
                        entry.conn.cancel()
                        return
                    case .unknownFrame:
                        continue
                    case .frame(let msgType, _, let payload):
                        self.handleClientFrameOnQueue(entry: entry, type: msgType, payload: payload)
                        continue
                    }
                    break
                }
            }
            if isComplete || err != nil {
                self.sessions.removeValue(forKey: entry.id)
                entry.conn.cancel()
                return
            }
            self.receiveLoopOnQueue(entry: entry)
        }
    }

    private func handleClientFrameOnQueue(entry: SessionEntry, type: WireMessageType, payload: Data) {
        guard let rec = pairedDevicesByHash[entry.certSHA256] else { return }
        metricsBox.update { $0.receivedClientTypes.append(type) }

        switch type {
        case .ping:
            guard let ping = try? JSONDecoder().decode(PingMessage.self, from: payload) else { return }
            metricsBox.update { $0.receivedPingVersions.append(ping.protocolVersion) }
            let wasBelow3 = entry.negotiatedVersion < 3
            if let pv = ping.protocolVersion, pv >= 2, entry.negotiatedVersion == 1 {
                entry.negotiatedVersion = min(pv, 3)
            }

            if simulateNoticeOnlyTooManySessions && entry.negotiatedVersion >= 3 {
                // Notice-only connection (`docs/protocol-v3-pet-camera.md` §2.2):
                // sends NO 0x02, 0x03, 0x07, or 0x0B — only 0x0F `too_many_sessions` and closes.
                let notice = SessionNoticeMessage(
                    reason: .tooManySessions,
                    closing: true,
                    message: PetSessionNoticeReason.tooManySessions.defaultSentence
                )
                self.sessions.removeValue(forKey: entry.id)
                if let pkt = try? WirePacket.packJSON(type: .sessionNotice, notice) {
                    entry.conn.send(content: pkt, isComplete: true, completion: .contentProcessed { [weak self] _ in
                        self?.netQueue.asyncAfter(deadline: .now() + .milliseconds(30)) {
                            entry.conn.cancel()
                        }
                    })
                } else {
                    entry.conn.cancel()
                }
                return
            }

            let nowNs: UInt64 = 1_000_000_000
            let pong = PongMessage(clientSendNs: ping.clientSendNs, serverRecvNs: nowNs, serverSendNs: nowNs + 1000)
            if let pkt = try? WirePacket.packJSON(type: .pong, pong) {
                entry.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
            if wasBelow3 && entry.negotiatedVersion >= 2 {
                sendSessionStateOnQueue(entry: entry, rec: rec)
            }

        case .sensorPowerReq:
            guard let req = try? JSONDecoder().decode(SensorPowerControlMessage.self, from: payload) else { return }
            metricsBox.update { $0.receivedPowerReqs.append(req) }
            // Amended spec Option A: v3 owner allowed whenever allowPhoneSensorPower is true, even when unavailable!
            guard rec.role == .owner, allowPhoneSensorPower else {
                sendSessionStateOnQueue(entry: entry, rec: rec)
                return
            }
            powerState = req.turnOn ? .on : .off
            broadcastAllSessionsOnQueue()

        case .liveViewReq:
            guard entry.negotiatedVersion >= 3,
                  let req = try? JSONDecoder().decode(LiveViewRequestMessage.self, from: payload) else { return }
            metricsBox.update { $0.receivedLiveViewReqs.append(req) }
            entry.liveWatching = req.watch
            if let qStr = req.quality {
                let q = LiveQuality(rawValue: qStr) ?? .unknown
                if q != .unknown {
                    entry.liveQuality = q
                    switch q {
                    case .auto, .high: entry.liveTier = .high
                    case .medium: entry.liveTier = .medium
                    case .low: entry.liveTier = .low
                    case .unknown: break
                    }
                }
            }
            sendSessionStateOnQueue(entry: entry, rec: rec)

        case .petCameraCmd:
            guard entry.negotiatedVersion >= 3,
                  let cmd = try? JSONDecoder().decode(PetCameraCommandMessage.self, from: payload) else { return }
            metricsBox.update { $0.receivedPetCommands.append(cmd) }
            guard rec.role == .owner else {
                let res = CommandResultMessage(
                    requestId: cmd.requestId,
                    ok: false,
                    error: "not_allowed_for_role",
                    message: "Only an owner can change camera settings."
                )
                if let pkt = try? WirePacket.packJSON(type: .commandResult, res) {
                    entry.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
                return
            }
            switch cmd.action {
            case PetCameraCommandAction.setSharing.rawValue:
                if let en = cmd.enabled { sharingEnabled = en }
                sendOkResult(to: entry, requestId: cmd.requestId)
                broadcastAllSessionsOnQueue()
            case PetCameraCommandAction.setPrivacy.rawValue:
                if let en = cmd.enabled { privacyMode = en }
                sendOkResult(to: entry, requestId: cmd.requestId)
                broadcastAllSessionsOnQueue()
            case PetCameraCommandAction.revokeDevice.rawValue:
                guard let targetId = cmd.deviceId, targetId != rec.deviceId else {
                    let res = CommandResultMessage(requestId: cmd.requestId, ok: false, error: "invalid", message: "Invalid device.")
                    if let pkt = try? WirePacket.packJSON(type: .commandResult, res) {
                        entry.conn.send(content: pkt, completion: .contentProcessed { _ in })
                    }
                    return
                }
                sendOkResult(to: entry, requestId: cmd.requestId)
                revokeDeviceOnQueue(deviceId: targetId)
            default:
                let res = CommandResultMessage(requestId: cmd.requestId, ok: false, error: "unsupported", message: "Unsupported in W6.")
                if let pkt = try? WirePacket.packJSON(type: .commandResult, res) {
                    entry.conn.send(content: pkt, completion: .contentProcessed { _ in })
                }
            }

        default:
            break
        }
    }

    private func sendOkResult(to entry: SessionEntry, requestId: UInt64) {
        let res = CommandResultMessage(requestId: requestId, ok: true, error: nil, message: nil)
        if let pkt = try? WirePacket.packJSON(type: .commandResult, res) {
            entry.conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
    }

    private func computeAvailability(for role: PetRole) -> (allowed: Bool, reason: LiveUnavailableReason?) {
        if let forced = forcedUnavailableReasonOverride {
            return (false, forced)
        }
        if privacyMode {
            return (false, .privacyMode)
        }
        if role == .viewer {
            if isReplayingOnHost {
                return (false, .pcReplaying)
            }
            if !sharingEnabled {
                return (false, .sharingOff)
            }
            if sharingScheduleOff {
                return (false, .sharingSchedule)
            }
        }
        return (true, nil)
    }

    private func broadcastAllSessionsOnQueue() {
        for (_, s) in sessions {
            if let rec = pairedDevicesByHash[s.certSHA256] {
                sendSessionStateOnQueue(entry: s, rec: rec)
            }
        }
    }

    private func sendSessionStateOnQueue(entry: SessionEntry, rec: PairedClientRecord) {
        stateSeq &+= 1
        let seq = stateSeq
        let (liveAllowed, unavailReason) = computeAvailability(for: rec.role)

        if entry.negotiatedVersion >= 2 {
            // v3 owners see the real sensor power state even while unavailable (Option A)
            let sync = HostStateSyncMessage(
                protocolVersion: 3,
                stateSeq: seq,
                powerState: (entry.negotiatedVersion < 3 && !liveAllowed) ? .off : powerState,
                inUseHolder: nil,
                errorMessage: nil,
                modeRaw: modeRaw,
                sbsLayoutRaw: sbsLayoutRaw,
                allowPhoneModeSwitch: rec.role == .owner && allowPhoneModeSwitch,
                allowPhoneSensorPower: rec.role == .owner && allowPhoneSensorPower && (entry.negotiatedVersion >= 3 || liveAllowed),
                sensorPowerDisabledReason: rec.role == .owner ? nil : "Only an owner can turn the camera on or off.",
                cutoffEnabled: cutoffEnabled,
                cutoffNearMm: 500,
                cutoffFarMm: 2000,
                isHostFrozen: isHostFrozen,
                isRecording: false,
                canRecordCurrentMode: false,
                recordingDisabledReason: rec.role == .owner ? SharedRecordingReminderConstants.colourLiveOnlyText : "Only an owner can record.",
                recordingReminderNeeded: false,
                recordingFrames: 0,
                recordingElapsedSec: 0,
                isReplayingOnHost: isReplayingOnHost
            )
            if let pkt = try? WirePacket.packJSON(type: .hostStateSync, sync) {
                entry.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }

        if entry.negotiatedVersion >= 3 {
            let watchers: [PetWatcher]? = (rec.role == .owner) ? sessions.values.compactMap { s in
                guard let r = pairedDevicesByHash[s.certSHA256] else { return nil }
                let avail = computeAvailability(for: r.role).allowed
                guard avail && s.liveWatching && powerState == .on else { return nil }
                return PetWatcher(
                    deviceId: r.deviceId,
                    deviceName: r.name,
                    role: r.role,
                    path: .home,
                    activity: "live"
                )
            } : nil

            let devicesList: [PetDevice]? = (rec.role == .owner) ? pairedDevicesByHash.values.sorted(by: { $0.name < $1.name }).map { d in
                PetDevice(
                    deviceId: d.deviceId,
                    name: d.name,
                    role: d.role,
                    kind: d.kind,
                    pairedAtUnixMs: d.pairedAtUnixMs,
                    lastSeenUnixMs: d.pairedAtUnixMs,
                    connected: sessions.values.contains { $0.deviceId == d.deviceId }
                )
            } : nil

            let petMsg = PetCameraStateMessage(
                protocolVersion: 3,
                stateSeq: seq,
                deviceId: rec.deviceId,
                deviceName: rec.name,
                role: rec.role,
                path: .home,
                sharingEnabled: sharingEnabled,
                sharingScheduleOff: sharingScheduleOff,
                outsideEnabled: false,
                privacyMode: privacyMode,
                liveAllowed: liveAllowed,
                liveUnavailableReason: unavailReason,
                watching: liveAllowed && entry.liveWatching && powerState == .on,
                liveQuality: entry.liveQuality,
                liveTier: entry.liveTier,
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
                outsideStatus: rec.role == .owner ? "off" : nil,
                outsideStatusText: nil,
                watchingNow: watchers,
                devices: devicesList
            )
            if let pkt = try? WirePacket.packJSON(type: .petCameraState, petMsg) {
                entry.conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
    }
}

@MainActor
final class Phase9APetCameraLiveTests: XCTestCase {

    nonisolated private static func makeTCPDropListener(on port: UInt16?) throws -> (listener: NWListener, readyFlag: Phase9SyncBox<Bool>) {
        let dropQueue = DispatchQueue(label: "kv2.test.v2drop")
        let readyFlag = Phase9SyncBox<Bool>(false)
        let listener: NWListener
        if let p = port, let nwPort = NWEndpoint.Port(rawValue: p) {
            listener = try NWListener(using: .tcp, on: nwPort)
        } else {
            listener = try NWListener(using: .tcp, on: .any)
        }
        listener.stateUpdateHandler = { st in
            if case .ready = st {
                readyFlag.update { $0 = true }
            }
        }
        listener.newConnectionHandler = { conn in
            conn.start(queue: dropQueue)
            conn.cancel()
        }
        listener.start(queue: dropQueue)
        return (listener, readyFlag)
    }

    private func waitUntil(
        timeoutSec: TimeInterval = 4.0,
        condition: () -> Bool
    ) async -> Bool {
        let deadline = Date().addingTimeInterval(timeoutSec)
        while Date() < deadline {
            if condition() {
                return true
            }
            try? await Task.sleep(nanoseconds: 15_000_000)
        }
        return condition()
    }

    func testViewerRoleGatingHidesOwnerControlsOnIOSAndMacWhileAllowingLiveQuality() async throws {
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let phoneClient = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Guest iPhone",
            deviceKind: "iphone"
        )
        let phoneVM = PocketViewModel(client: phoneClient, registerLifecycleObservers: false)
        defer { phoneClient.stop() }

        let macClient = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Living Room Mac",
            deviceKind: "mac"
        )
        let macViewer = MacCameraViewerController(client: macClient)
        defer { macViewer.shutdown() }

        phoneVM.submitQRCodeURI(host.preparePairingOffer(role: .viewer))
        let phoneReady = await waitUntil {
            phoneVM.isPetCameraV3 && phoneVM.telemetry.hasConfirmedPetCameraState && phoneVM.currentPetRole == .viewer
        }
        XCTAssertTrue(phoneReady, "iOS viewer did not complete v3 handshake and receive 0x0B")

        macViewer.pairURIInput = host.preparePairingOffer(role: .viewer)
        macViewer.submitPairURI()
        let macReady = await waitUntil {
            macViewer.isPetCameraV3 && macViewer.telemetry.hasConfirmedPetCameraState && macViewer.currentPetRole == .viewer
        }
        XCTAssertTrue(macReady, "Mac viewer did not complete v3 handshake and receive 0x0B")

        // Verify ping negotiated protocolVersion == 3 and pairRequest sent deviceKind
        XCTAssertTrue(host.metrics.receivedPingVersions.contains(3))
        XCTAssertTrue(host.metrics.receivedPairRequests.contains { $0.deviceKind == "mac" && $0.protocolVersion == 3 })

        // Viewer role gating hides all owner controls on both iOS and Mac
        XCTAssertTrue(phoneVM.isPetViewer)
        XCTAssertFalse(phoneVM.isPetOwner)
        XCTAssertFalse(phoneVM.showOwnerSettingsAndRemoteControls)
        XCTAssertFalse(phoneVM.showSensorPowerToggle)
        XCTAssertFalse(phoneVM.allowSensorPowerControl)
        XCTAssertFalse(phoneVM.showPetCameraOwnerSection)
        XCTAssertFalse(phoneVM.canControlHostMode)
        XCTAssertTrue(phoneVM.watchingNowEntries.isEmpty)
        XCTAssertTrue(phoneVM.pairedDeviceEntries.isEmpty)
        XCTAssertTrue(phoneVM.showLiveQualityPicker)

        XCTAssertTrue(macViewer.isPetViewer)
        XCTAssertFalse(macViewer.isPetOwner)
        XCTAssertFalse(macViewer.showOwnerControls)
        XCTAssertFalse(macViewer.allowSensorPowerControl)
        XCTAssertFalse(macViewer.showPetCameraOwnerSection)
        XCTAssertFalse(macViewer.canControlHostMode)
        XCTAssertTrue(macViewer.watchingNowEntries.isEmpty)
        XCTAssertTrue(macViewer.pairedDeviceEntries.isEmpty)
        XCTAssertTrue(macViewer.showLiveQualityPicker)

        // Attempt owner actions from viewer -> client must block all of them locally
        let priorCmdCount = host.metrics.receivedPetCommands.count
        let priorPowerCount = host.metrics.receivedPowerReqs.count
        phoneVM.requestSetSharing(false)
        phoneVM.setPrivacyMode(true)
        phoneVM.requestSensorPower(turnOn: false)
        phoneVM.selectMode(.depth)
        phoneVM.setHostCutoffEnabled(true)
        phoneVM.toggleHostFreeze()
        macViewer.requestSetSharing(false)
        macViewer.setPrivacyMode(true)
        macViewer.requestSensorPower(turnOn: false)
        try? await Task.sleep(nanoseconds: 120_000_000)

        XCTAssertEqual(host.metrics.receivedPetCommands.count, priorCmdCount, "Viewer must not send 0x0C owner commands")
        XCTAssertEqual(host.metrics.receivedPowerReqs.count, priorPowerCount, "Viewer must not send 0x0A sensor power")

        // Viewer CAN select live quality (auto, high, medium, low) via 0x0E
        phoneVM.selectLiveQuality(.low)
        let lowApplied = await waitUntil {
            phoneVM.selectedLiveQuality == .low && phoneVM.telemetry.petState.liveTier == .low
        }
        XCTAssertTrue(lowApplied)

        macViewer.selectLiveQuality(.medium)
        let medApplied = await waitUntil {
            macViewer.selectedLiveQuality == .medium && macViewer.telemetry.petState.liveTier == .medium
        }
        XCTAssertTrue(medApplied)
    }

    func testOwnerControlsOptionASensorPowerWhileUnavailableAndWatchingNow() async throws {
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let ownerClient = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Owner's iPhone",
            deviceKind: "iphone"
        )
        let ownerVM = PocketViewModel(client: ownerClient, registerLifecycleObservers: false)
        defer { ownerClient.stop() }

        ownerVM.submitQRCodeURI(host.preparePairingOffer(role: .owner))
        let connected = await waitUntil {
            ownerVM.isPetOwner && ownerVM.telemetry.hasConfirmedHostState
        }
        XCTAssertTrue(connected)
        XCTAssertTrue(ownerVM.showOwnerSettingsAndRemoteControls)
        XCTAssertTrue(ownerVM.showPetCameraOwnerSection)
        XCTAssertEqual(ownerVM.watchingNowEntries.count, 1)
        XCTAssertEqual(ownerVM.watchingNowEntries.first?.deviceName, "Owner's iPhone")

        // Owner toggles Privacy Mode ON -> video becomes unavailable (`liveAllowed: false`),
        // yet Option A keeps sensor power switch showing real state (On) AND allows 0x0A Off/On!
        ownerVM.setPrivacyMode(true)
        let privacyOn = await waitUntil {
            ownerVM.isLiveVideoUnavailableV3 &&
            ownerVM.liveUnavailableReasonText == "Privacy mode is on. Turn it off on the PC or an owner's device."
        }
        XCTAssertTrue(privacyOn)
        XCTAssertTrue(ownerVM.confirmedSwitchIsOn, "Real sensor state (On) must stay visible during privacy mode")
        XCTAssertTrue(ownerVM.allowSensorPowerControl, "Option A: v3 owner must keep sensor power control while unavailable")

        ownerVM.requestSensorPower(turnOn: false)
        let turnedOff = await waitUntil {
            ownerVM.confirmedPowerState == .off && !ownerVM.confirmedSwitchIsOn
        }
        XCTAssertTrue(turnedOff, "Option A: v3 owner's 0x0A off must succeed while liveAllowed == false")
        XCTAssertEqual(ownerVM.liveUnavailableReasonText, "Privacy mode is on. Turn it off on the PC or an owner's device.")

        ownerVM.requestSensorPower(turnOn: true)
        let turnedOn = await waitUntil {
            ownerVM.confirmedPowerState == .on && ownerVM.confirmedSwitchIsOn
        }
        XCTAssertTrue(turnedOn, "Option A: v3 owner's 0x0A on must succeed while liveAllowed == false")
    }

    func testEachLiveUnavailableReasonShowsPlainWordsAndLenientEnumsDoNotDropState() async throws {
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let viewerClient = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Viewer Phone",
            deviceKind: "iphone"
        )
        let viewerVM = PocketViewModel(client: viewerClient, registerLifecycleObservers: false)
        defer { viewerClient.stop() }

        viewerVM.submitQRCodeURI(host.preparePairingOffer(role: .viewer))
        let ready = await waitUntil { viewerVM.isPetViewer && viewerVM.telemetry.hasConfirmedPetCameraState }
        XCTAssertTrue(ready)
        XCTAssertNil(viewerVM.liveUnavailableReasonText)

        let expectedReasons: [(LiveUnavailableReason, String)] = [
            (.privacyMode, "Privacy mode is on. Turn it off on the PC or an owner's device."),
            (.pcReplaying, "The PC is playing a recording."),
            (.sharingOff, "Sharing is off on the PC."),
            (.sharingSchedule, "Sharing is off at this time of day."),
            (.tooManyViewers, "Four devices are already watching."),
        ]

        for (reason, expectedSentence) in expectedReasons {
            host.setHostAvailability(forcedReason: .some(reason))
            let matched = await waitUntil {
                viewerVM.isLiveVideoUnavailableV3 && viewerVM.liveUnavailableReasonText == expectedSentence
            }
            XCTAssertTrue(matched, "Expected '\(expectedSentence)' for \(reason.rawValue), got \(String(describing: viewerVM.liveUnavailableReasonText))")
        }

        // Verify lenient v3 enum decoding: unknown enum values decode to .unknown instead of dropping 0x0B
        let futureJSON = """
        {"protocolVersion":3,"stateSeq":9999,"deviceId":"0102030405060708","deviceName":"Viewer Phone","role":"viewer","path":"future_satellite","sharingEnabled":true,"sharingScheduleOff":false,"outsideEnabled":false,"privacyMode":false,"liveAllowed":false,"liveUnavailableReason":"future_reason_xyz","watching":false,"liveQuality":"ultra_8k","liveTier":"mega_tier","rollAvailable":false,"playbackAvailable":false,"rollEnabled":false,"motionEnabled":false,"motionAvailable":false,"motionSensitivity":5,"scheduleEnabled":false,"clipRecording":false,"clipManualActive":false,"clipElapsedSec":0}
        """
        host.pushRawJSONFrameToAllV3Sessions(type: .petCameraState, jsonString: futureJSON)
        let lenientUpdated = await waitUntil {
            viewerVM.telemetry.petState.stateSeq == 9999
        }
        XCTAssertTrue(lenientUpdated, "Unknown v3 enum strings must decode leniently to .unknown without dropping 0x0B")
        XCTAssertEqual(viewerVM.telemetry.petState.path, .unknown)
        XCTAssertEqual(viewerVM.telemetry.petState.liveQuality, .unknown)
        XCTAssertEqual(viewerVM.telemetry.petState.liveTier, .unknown)
        XCTAssertEqual(viewerVM.liveUnavailableReasonText, "Live video isn't available right now.")
    }

    func testRevokeAndUnpairAllReturnToPairingWithNoReconnectLoop() async throws {
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let phoneClient = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Revoke Test iPhone",
            deviceKind: "iphone"
        )
        let phoneVM = PocketViewModel(client: phoneClient, registerLifecycleObservers: false)
        defer { phoneClient.stop() }

        phoneVM.submitQRCodeURI(host.preparePairingOffer(role: .viewer))
        let connected = await waitUntil {
            phoneVM.isPetViewer && phoneVM.telemetry.hasConfirmedPetCameraState
        }
        XCTAssertTrue(connected)
        let phoneId = phoneVM.telemetry.petState.deviceId
        let attemptsBeforeRevoke = host.metrics.streamConnectionAttempts

        // 1. Revoke single device -> shows exact sentence, opens pairing sheet, zero reconnect loop
        host.revokeDevice(deviceId: phoneId)
        let revokedHandled = await waitUntil {
            !phoneVM.telemetry.isPaired &&
            phoneVM.showPairingSheet &&
            phoneVM.revokedBannerText == "This phone was removed from the camera"
        }
        XCTAssertTrue(revokedHandled)

        // Wait past normal 0.4s/1.0s reconnect intervals and assert zero reconnect attempts
        try? await Task.sleep(nanoseconds: 900_000_000)
        XCTAssertEqual(
            host.metrics.streamConnectionAttempts,
            attemptsBeforeRevoke,
            "Revoked client must not retry in a reconnect loop"
        )

        // 2. Whole-PC "Unpair all" (§2.3): revokes all devices AND turns the camera off
        let ownerClient = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Owner iPhone",
            deviceKind: "iphone"
        )
        let ownerVM = PocketViewModel(client: ownerClient, registerLifecycleObservers: false)
        defer { ownerClient.stop() }

        let macClient = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Desk Mac",
            deviceKind: "mac"
        )
        let macViewer = MacCameraViewerController(client: macClient)
        defer { macViewer.shutdown() }

        ownerVM.submitQRCodeURI(host.preparePairingOffer(role: .owner))
        let ownerReady = await waitUntil {
            ownerVM.isPetOwner && ownerVM.confirmedPowerState == .on
        }
        XCTAssertTrue(ownerReady)

        macViewer.pairURIInput = host.preparePairingOffer(role: .viewer)
        macViewer.submitPairURI()
        let bothReady = await waitUntil {
            ownerVM.isPetOwner && macViewer.isPetViewer && ownerVM.confirmedPowerState == .on
        }
        XCTAssertTrue(bothReady)
        let attemptsBeforeUnpairAll = host.metrics.streamConnectionAttempts

        host.unpairAllDevices()

        let bothUnpaired = await waitUntil {
            !ownerVM.telemetry.isPaired &&
            ownerVM.revokedBannerText == "This phone was removed from the camera" &&
            !macViewer.telemetry.isPaired &&
            macViewer.revokedBannerText == "This Mac was removed from the camera"
        }
        XCTAssertTrue(bothUnpaired)
        XCTAssertEqual(host.currentPowerState, .off, "Unpair all must turn the camera off (§2.3)")
        XCTAssertTrue(host.metrics.sensorPowerTurnedOffByUnpairAll)
        XCTAssertEqual(ownerVM.confirmedPowerState, .off)

        try? await Task.sleep(nanoseconds: 700_000_000)
        XCTAssertEqual(host.metrics.streamConnectionAttempts, attemptsBeforeUnpairAll)
    }

    func testOutgoingDeviceToPCFramesStayAtOrUnder64KiB() async throws {
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let sentSizes = Phase9SyncBox<[Int]>([])
        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "64KiB Cap Test Phone",
            deviceKind: "iphone"
        )
        client.onOutgoingWirePacketSent = { _, pkt in
            sentSizes.update { $0.append(pkt.count) }
        }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        vm.submitQRCodeURI(host.preparePairingOffer(role: .owner))
        let ready = await waitUntil { vm.isPetOwner && vm.telemetry.hasConfirmedHostState }
        XCTAssertTrue(ready)

        vm.selectLiveQuality(.medium)
        vm.setPrivacyMode(true)
        vm.setPrivacyMode(false)
        vm.requestSensorPower(turnOn: false)
        vm.requestSensorPower(turnOn: true)
        vm.selectMode(.depth)
        vm.setHostCutoffEnabled(true)
        vm.send3DOrbit(deltaYaw: 0.1, deltaPitch: -0.05)

        let capturedEnough = await waitUntil { sentSizes.read().count >= 8 }
        XCTAssertTrue(capturedEnough)

        for byteCount in sentSizes.read() {
            XCTAssertGreaterThanOrEqual(byteCount, WireHeader.size)
            XCTAssertLessThanOrEqual(
                byteCount,
                WireHeader.maxClientFrameBytes,
                "Every outgoing device -> PC frame must be <= 64 KiB (65,536 bytes), got \(byteCount)"
            )
        }

        // Exact 64 KiB boundary (8-byte header + 65,528-byte payload == 65,536 bytes) succeeds
        let exact64KiBPayload = Data(repeating: 0x61, count: WireHeader.maxClientFrameBytes - WireHeader.size)
        let exactPacket = try WirePacket.packClientToHost(type: .ping, payload: exact64KiBPayload)
        XCTAssertEqual(exactPacket.count, 65_536)

        // 64 KiB + 1 byte (65,537 bytes) is rejected before touching the wire
        let oversizePayload = Data(repeating: 0x61, count: WireHeader.maxClientFrameBytes - WireHeader.size + 1)
        XCTAssertThrowsError(try WirePacket.packClientToHost(type: .ping, payload: oversizePayload)) { err in
            XCTAssertEqual(err as? WirePacketError, .clientFrameExceeds64KiBCap(frameBytes: 65_537))
        }
        let sentCountBeforeReject = sentSizes.read().count
        let sendResult = client.sendRawClientPayloadForTesting(type: .petCameraCmd, payload: oversizePayload)
        XCTAssertFalse(sendResult, "Client must refuse to send frames larger than 64 KiB")
        XCTAssertEqual(sentSizes.read().count, sentCountBeforeReject, "Oversize frame must never be written to the socket")
        XCTAssertEqual(client.telemetry.rejectedOversizeOutgoingFrames, 1)
    }

    func testV2MacHostBehaviourRemainsUnchanged() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase9a-v2compat-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let v2Host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { v2Host.shutdown() }

        v2Host.openPairingWindow()
        let offer = try XCTUnwrap(v2Host.activePairingOffer)
        let parsed = try XCTUnwrap(QRPairingPayload.parse(offer.qrURI))
        let loopbackURI = QRPairingPayload(
            serviceId: parsed.serviceId,
            serverCertSHA256: parsed.serverCertSHA256,
            oneTimeToken: offer.qrToken,
            hostIP: "127.0.0.1",
            pairPort: offer.port
        ).toURIString()

        let outgoingPings = Phase9SyncBox<[Int?]>([])
        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "iPhone against v2 Mac",
            deviceKind: "iphone"
        )
        client.onOutgoingWirePacketSent = { type, pkt in
            guard type == .ping else { return }
            let json = pkt.dropFirst(WireHeader.size)
            if let ping = try? JSONDecoder().decode(PingMessage.self, from: Data(json)) {
                outgoingPings.update { $0.append(ping.protocolVersion) }
            }
        }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        vm.submitQRCodeURI(loopbackURI)
        let connected = await waitUntil {
            vm.telemetry.isConnected &&
            vm.supportsHostV2 &&
            vm.telemetry.hasConfirmedHostState &&
            !outgoingPings.read().isEmpty
        }
        XCTAssertTrue(connected)

        // Against a v2 host: pet-camera controls stay hidden, pings use protocolVersion 2, v2 controls work unchanged
        XCTAssertFalse(vm.isPetCameraV3)
        XCTAssertFalse(vm.showLiveQualityPicker)
        XCTAssertFalse(vm.showPetCameraOwnerSection)
        XCTAssertTrue(vm.showOwnerSettingsAndRemoteControls)
        XCTAssertTrue(vm.showSensorPowerToggle)
        XCTAssertTrue(vm.allowSensorPowerControl)
        XCTAssertTrue(outgoingPings.read().allSatisfy { $0 == 2 }, "Against a v2 host, client must ping with protocolVersion 2")

        vm.selectMode(.depth)
        let switchedToDepth = await waitUntil { v2Host.selectedMode == .depth }
        XCTAssertTrue(switchedToDepth)
    }

    private func recursiveRelativeFilePaths(in rootURL: URL) -> Set<String> {
        let fm = FileManager.default
        let stdRoot = rootURL.resolvingSymlinksInPath().standardizedFileURL
        let rootPrefix = stdRoot.path.hasSuffix("/") ? stdRoot.path : stdRoot.path + "/"
        guard fm.fileExists(atPath: stdRoot.path),
              let enumerator = fm.enumerator(
                  at: stdRoot,
                  includingPropertiesForKeys: [.isRegularFileKey],
                  options: []
              ) else {
            return []
        }
        var paths: Set<String> = []
        for case let url as URL in enumerator {
            let vals = try? url.resourceValues(forKeys: [.isRegularFileKey])
            guard vals?.isRegularFile == true else { continue }
            let stdPath = url.resolvingSymlinksInPath().standardizedFileURL.path
            if stdPath.hasPrefix(rootPrefix) {
                paths.insert(String(stdPath.dropFirst(rootPrefix.count)))
            } else {
                paths.insert(stdPath)
            }
        }
        return paths
    }

    /// The Mac viewer is not sandboxed (no entitlements), so there is no container to scan here. The test checks the
    /// Mac app's real per-app caches folder (`~/Library/Caches/<bundle id>`) and `URLCache`. The whole iOS app container is
    /// scanned by `KinectPocketTests.testFix8_ZeroFilesInIOSSandboxRecursiveByPathAfterLiveThumbnailsAndStreamedPlayback`.
    func testLivePlaybackLeavesZeroFilesInMacViewerCaches() async throws {
        URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)

        let fm = FileManager.default
        let libraryCachesRoot = try XCTUnwrap(fm.urls(for: .cachesDirectory, in: .userDomainMask).first)
        let appCachesDir = libraryCachesRoot.appendingPathComponent(
            Bundle.main.bundleIdentifier ?? "com.example.kinectviewer.host",
            isDirectory: true
        )
        try fm.createDirectory(at: appCachesDir, withIntermediateDirectories: true)

        // Self-check proving `recursiveRelativeFilePaths` detects nested files in the real caches folder by relative path
        let probeRoot = appCachesDir.appendingPathComponent("kv-p9a-probe-\(UUID().uuidString)", isDirectory: true)
        let probeBefore = recursiveRelativeFilePaths(in: appCachesDir)
        let probeSubdir = probeRoot.appendingPathComponent("nested/sub", isDirectory: true)
        try fm.createDirectory(at: probeSubdir, withIntermediateDirectories: true)
        try Data([0x01]).write(to: probeSubdir.appendingPathComponent("probe.tmp"))
        let probeAfter = recursiveRelativeFilePaths(in: appCachesDir)
        XCTAssertTrue(probeAfter.subtracting(probeBefore).contains("\(probeRoot.lastPathComponent)/nested/sub/probe.tmp"))
        try fm.removeItem(at: probeRoot)

        // Start synthetic host and prepare encoded H.264 frames BEFORE taking the snapshot
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let beforeCaches = recursiveRelativeFilePaths(in: appCachesDir)

        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Zero Storage iPhone",
            deviceKind: "iphone"
        )
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        let layer = AVSampleBufferDisplayLayer()
        vm.displayBridge.attach(layer: layer)
        XCTAssertTrue(layer.preventsCapture, "Display layer must enforce preventsCapture == true")
        defer { client.stop() }

        vm.submitQRCodeURI(host.preparePairingOffer(role: .viewer))
        let ready = await waitUntil { vm.isPetViewer && vm.telemetry.hasConfirmedPetCameraState }
        XCTAssertTrue(ready)

        host.sendEncodedSyntheticFrames(count: 12)
        let decodedFrames = await waitUntil { vm.telemetry.receivedFrames >= 10 }
        XCTAssertTrue(decodedFrames, "Expected synthetic live H.264 frames to stream into display bridge, got \(vm.telemetry.receivedFrames)")

        vm.selectLiveQuality(.medium)
        let tierUpdated = await waitUntil { vm.selectedLiveQuality == .medium }
        XCTAssertTrue(tierUpdated)
        host.sendEncodedSyntheticFrames(count: 4)
        let moreFrames = await waitUntil { vm.telemetry.receivedFrames >= 14 }
        XCTAssertTrue(moreFrames)

        let newCacheFiles = recursiveRelativeFilePaths(in: appCachesDir).subtracting(beforeCaches)

        XCTAssertEqual(newCacheFiles, [], "Live playback must write 0 files in app Library/Caches")
        XCTAssertEqual(URLCache.shared.currentDiskUsage, 0)

        // Static audit of client streaming & display path: no AVAssetWriter or file-URL writers
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let displayPathFiles = [
            "shared/PocketStreamClientCore.swift",
            "shared/SampleBufferDisplayBridge.swift",
            "ios/VideoSampleDisplayView.swift",
            "ios/PocketViewModel.swift",
            "ios/ContentView.swift",
            "macos/App/MacCameraViewerController.swift",
            "macos/App/MacCameraViewerView.swift",
        ]
        for relPath in displayPathFiles {
            let src = try String(contentsOf: repoRoot.appendingPathComponent(relPath), encoding: .utf8)
            XCTAssertFalse(src.contains("AVAssetWriter"), "\(relPath) must not reference AVAssetWriter")
            XCTAssertFalse(src.contains("AVAssetExportSession"), "\(relPath) must not reference AVAssetExportSession")
            XCTAssertFalse(src.contains("fileURLWithPath"), "\(relPath) must not construct file URLs")
            XCTAssertFalse(src.contains("createFile(atPath:"), "\(relPath) must not create files on disk")
        }
    }

    func testTLSRejectionNeverWipesPairingAndRetriesWhile0x0FRevokedClearsPairing() async throws {
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let store = InMemoryPersistenceProvider()
        let scheduledDelays = Phase9SyncBox<[Double]>([])
        let retryTriggerBox = Phase9SyncBox<(@Sendable () -> Void)?>(nil)

        let client = try PocketStreamClientCore(
            persistence: store,
            deviceName: "Startup TLS Test iPhone",
            deviceKind: "iphone"
        )
        client.onReconnectScheduledForTesting = { delay, retry in
            scheduledDelays.update { $0.append(delay) }
            retryTriggerBox.update { $0 = retry }
        }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        vm.submitQRCodeURI(host.preparePairingOffer(role: .owner))
        let pairedAndConnected = await waitUntil {
            vm.isPetOwner && vm.telemetry.isConnected
        }
        XCTAssertTrue(pairedAndConnected)

        let savedPairing = try XCTUnwrap(store.rawStoredForTesting)
        XCTAssertNotNil(savedPairing.pinnedServerCertSHA256)
        let originalClientCertDER = savedPairing.clientIdentity.certificateDER

        // 1. Simulate Windows host rejecting at TLS handshake (e.g. `devices.json` temporarily unreadable at startup)
        host.setSimulateTLSRejection(true, closeExistingSessions: true)

        // Wait for disconnect from the closed session and trigger 6 TLS-rejected retries to verify back-off cap
        let firstRetryScheduled = await waitUntil { !scheduledDelays.read().isEmpty }
        XCTAssertTrue(firstRetryScheduled)

        for expectedCount in 2...7 {
            let retry = retryTriggerBox.read()
            retryTriggerBox.update { $0 = nil }
            retry?()
            let gotNext = await waitUntil { scheduledDelays.read().count >= expectedCount }
            XCTAssertTrue(gotNext, "Expected reconnect attempt \(expectedCount) after TLS rejection")
        }

        // Pairing and Keychain identity MUST be completely preserved!
        XCTAssertTrue(vm.telemetry.isPaired, "TLS rejection must NEVER unpair the phone")
        XCTAssertFalse(vm.showPairingSheet, "TLS rejection must NOT open the pairing sheet")
        XCTAssertNil(vm.revokedBannerText)
        XCTAssertEqual(
            vm.disconnectedStatusTitle,
            "The camera refused this phone. It may be starting up — trying again shortly."
        )
        let afterTLSRejections = try XCTUnwrap(store.rawStoredForTesting)
        XCTAssertEqual(afterTLSRejections.clientIdentity.certificateDER, originalClientCertDER, "Client identity must be kept on TLS rejection")
        XCTAssertEqual(afterTLSRejections.pinnedServerCertSHA256, savedPairing.pinnedServerCertSHA256)
        XCTAssertEqual(scheduledDelays.read().last, 30.0, "TLS rejection back-off must cap at 30 s")

        // Restore host TLS admission -> next retry reconnects cleanly using the preserved pairing!
        host.setSimulateTLSRejection(false)
        retryTriggerBox.read()?()
        let recovered = await waitUntil { vm.telemetry.isConnected && vm.isPetOwner }
        XCTAssertTrue(recovered, "Client must recover automatically once host stops rejecting TLS")

        // 2. Explicit `0x0F revoked` -> pairing IS cleared and no reconnect is scheduled
        let delaysCountBeforeRevoke = scheduledDelays.read().count
        host.revokeDevice(deviceId: vm.telemetry.petState.deviceId)
        let revoked = await waitUntil {
            !vm.telemetry.isPaired &&
            vm.showPairingSheet &&
            vm.revokedBannerText == "This phone was removed from the camera"
        }
        XCTAssertTrue(revoked)
        XCTAssertNil(store.rawStoredForTesting?.pinnedServerCertSHA256, "Explicit 0x0F revoked must clear pinned server hash")
        XCTAssertNotEqual(store.rawStoredForTesting?.clientIdentity.certificateDER, originalClientCertDER)
        try? await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(scheduledDelays.read().count, delaysCountBeforeRevoke, "0x0F revoked must schedule zero reconnects")
    }

    func testTenNoticeOnlyTooManySessionsConnectsNeverShowConnectedFireZeroHapticsAndReach30sBackoffCap() async throws {
        let host = try FakeV3PetCameraHost()
        defer { host.shutdown() }

        let store = InMemoryPersistenceProvider()
        let scheduledDelays = Phase9SyncBox<[Double]>([])
        let retryTriggerBox = Phase9SyncBox<(@Sendable () -> Void)?>(nil)
        let everSawConnectedTrue = Phase9SyncBox<Bool>(false)

        // Pair while host has free capacity, then stop before enabling notice-only mode
        do {
            let setupClient = try PocketStreamClientCore(
                persistence: store,
                deviceName: "9th Session iPhone",
                deviceKind: "iphone"
            )
            let pairExp = expectation(description: "Initial pair")
            setupClient.pairWithQRURI(host.preparePairingOffer(role: .viewer)) { _ in
                pairExp.fulfill()
            }
            await fulfillment(of: [pairExp], timeout: 4.0)
            let initialReady = await waitUntil { setupClient.telemetry.isConnected }
            XCTAssertTrue(initialReady)
            setupClient.stop()
        }

        host.setSimulateNoticeOnlyTooManySessions(true, closeExistingSessions: true)

        let client = try PocketStreamClientCore(
            persistence: store,
            deviceName: "9th Session iPhone",
            deviceKind: "iphone"
        )
        client.onReconnectScheduledForTesting = { delay, retry in
            scheduledDelays.update { $0.append(delay) }
            retryTriggerBox.update { $0 = retry }
        }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let cancellable = vm.$telemetry.sink { snap in
            if snap.isConnected {
                everSawConnectedTrue.update { $0 = true }
            }
        }
        defer { cancellable.cancel() }

        // Drive 10 consecutive notice-only (`0x0F too_many_sessions`) connects
        client.start()
        for attempt in 1...10 {
            let scheduled = await waitUntil { scheduledDelays.read().count >= attempt }
            XCTAssertTrue(scheduled, "Expected notice-only connect #\(attempt) to schedule back-off")
            if attempt < 10 {
                let trigger = retryTriggerBox.read()
                retryTriggerBox.update { $0 = nil }
                trigger?()
            }
        }

        let settled = await waitUntil {
            vm.disconnectedStatusTitle == "Too many people are watching right now"
        }
        XCTAssertTrue(settled)

        XCTAssertFalse(everSawConnectedTrue.read(), "Notice-only connections must NEVER report isConnected == true")
        XCTAssertEqual(vm.connectHapticCount, 0, "Notice-only connections must NEVER fire connect haptic")
        XCTAssertEqual(vm.disconnectHapticCount, 0, "Notice-only connections must NEVER fire disconnect haptic")
        XCTAssertEqual(
            scheduledDelays.read(),
            [1.0, 2.0, 4.0, 8.0, 16.0, 30.0, 30.0, 30.0, 30.0, 30.0],
            "Back-off must grow across notice-only connects and reach the 30 s cap"
        )

        // When a session slot opens on the 11th connect, 0x0B arrives -> reports connected once and resets reconnectCount
        host.setSimulateNoticeOnlyTooManySessions(false)
        retryTriggerBox.read()?()
        let nowConnected = await waitUntil {
            vm.telemetry.isConnected && vm.isPetViewer && vm.telemetry.reconnectCount == 0
        }
        XCTAssertTrue(nowConnected)
        XCTAssertEqual(vm.connectHapticCount, 1)
    }

    func testClientKeychainDeniedOrLockedPreservesPairingNeverDeletesAndRecoversOnRetry() throws {
        // Fake Keychain backend verifying `SecItemUpdate` first, zero `SecItemDelete` on error, and denied/locked preservation
        final class FakeKeychainBackend: @unchecked Sendable {
            let lock = NSLock()
            var dataByService: [String: Data] = [:]
            var forcedCopyStatus: OSStatus?
            var forcedUpdateStatus: OSStatus?
            var deleteCallCount: Int = 0
            var updateCallCount: Int = 0
            var addCallCount: Int = 0

            func makePrimitives() -> ClientKeychainPrimitives {
                ClientKeychainPrimitives(
                    copyMatching: { [weak self] query, result in
                        guard let self = self else { return errSecIO }
                        self.lock.lock()
                        defer { self.lock.unlock() }
                        if let forced = self.forcedCopyStatus {
                            return forced
                        }
                        let dict = query as NSDictionary
                        let svc = (dict[kSecAttrService] as? String) ?? ""
                        guard let bytes = self.dataByService[svc] else {
                            return errSecItemNotFound
                        }
                        result?.pointee = bytes as CFData
                        return errSecSuccess
                    },
                    update: { [weak self] query, attrs in
                        guard let self = self else { return errSecIO }
                        self.lock.lock()
                        defer { self.lock.unlock() }
                        self.updateCallCount += 1
                        if let forced = self.forcedUpdateStatus {
                            return forced
                        }
                        let dict = query as NSDictionary
                        let svc = (dict[kSecAttrService] as? String) ?? ""
                        guard self.dataByService[svc] != nil else {
                            return errSecItemNotFound
                        }
                        let attrDict = attrs as NSDictionary
                        if let newBytes = attrDict[kSecValueData] as? Data {
                            self.dataByService[svc] = newBytes
                        }
                        return errSecSuccess
                    },
                    add: { [weak self] query, _ in
                        guard let self = self else { return errSecIO }
                        self.lock.lock()
                        defer { self.lock.unlock() }
                        self.addCallCount += 1
                        let dict = query as NSDictionary
                        let svc = (dict[kSecAttrService] as? String) ?? ""
                        if let bytes = dict[kSecValueData] as? Data {
                            self.dataByService[svc] = bytes
                        }
                        return errSecSuccess
                    },
                    delete: { [weak self] query in
                        guard let self = self else { return errSecIO }
                        self.lock.lock()
                        defer { self.lock.unlock() }
                        self.deleteCallCount += 1
                        let dict = query as NSDictionary
                        let svc = (dict[kSecAttrService] as? String) ?? ""
                        self.dataByService.removeValue(forKey: svc)
                        return errSecSuccess
                    }
                )
            }
        }

        let backend = FakeKeychainBackend()
        let primitives = backend.makePrimitives()
        let macProvider = KeychainPersistenceProvider(
            service: KeychainPairingStore.macViewerService,
            primitives: primitives
        )
        let iosProvider = KeychainPersistenceProvider(
            service: KeychainPairingStore.defaultService,
            primitives: primitives
        )

        let originalMacIdentity = try CryptoIdentity.generateNew(commonName: "mac-paired")
        let originalHostHash = Data(repeating: 0xAB, count: 32)
        let macPairedState = ClientPairingState(
            clientIdentity: originalMacIdentity.persistedRecord,
            serviceId: "svc-mac-01",
            pinnedServerCertSHA256: originalHostHash,
            lastKnownHost: "192.168.1.20",
            lastKnownStreamPort: 49400,
            pairedDeviceId: "0102030405060708",
            pairedRole: .owner,
            lastKnownHostProtocolVersion: 3
        )
        XCTAssertTrue(macProvider.save(macPairedState))
        XCTAssertEqual(backend.addCallCount, 1)
        XCTAssertEqual(backend.deleteCallCount, 0, "Initial save must use update-then-add without calling SecItemDelete")

        let iosPairedState = ClientPairingState(
            clientIdentity: originalMacIdentity.persistedRecord,
            serviceId: "svc-ios-01",
            pinnedServerCertSHA256: originalHostHash,
            lastKnownHost: "192.168.1.20",
            lastKnownStreamPort: 49400,
            pairedRole: .owner,
            lastKnownHostProtocolVersion: 3
        )
        XCTAssertTrue(iosProvider.save(iosPairedState))

        // Verify `save()` updates in place without deleting, and NEVER deletes on an update error!
        backend.forcedUpdateStatus = errSecAuthFailed
        XCTAssertFalse(macProvider.save(macPairedState))
        XCTAssertEqual(backend.deleteCallCount, 0, "Failed SecItemUpdate must NEVER call SecItemDelete")
        backend.forcedUpdateStatus = nil

        // Simulate user clicking "Deny" (`errSecAuthFailed`) and locked Keychain (`errSecInteractionNotAllowed`)
        for deniedStatus in [errSecAuthFailed, errSecInteractionNotAllowed] {
            backend.forcedCopyStatus = deniedStatus

            let macViewer = MacCameraViewerController(persistence: macProvider, deviceName: "Owner's Mac")
            macViewer.activate()
            XCTAssertEqual(
                macViewer.keychainAccessError,
                "Allow Keychain access to keep this Mac paired to the camera."
            )
            XCTAssertFalse(macViewer.telemetry.isPaired)
            XCTAssertEqual(backend.deleteCallCount, 0)
            XCTAssertEqual(backend.addCallCount, 2, "Denied/locked Keychain must NEVER generate or add a new identity")

            let iosVM = PocketViewModel(persistence: iosProvider, deviceName: "Owner's iPhone", deviceKind: "iphone", autostart: true)
            XCTAssertEqual(
                iosVM.keychainAccessError,
                "Allow Keychain access to keep your iPhone paired to the camera."
            )
            XCTAssertFalse(iosVM.showPairingSheet, "Denied/locked Keychain must show Retry, not the pairing sheet")
            XCTAssertEqual(backend.deleteCallCount, 0)
            XCTAssertEqual(backend.addCallCount, 2)
        }

        // Grant Keychain access and click Retry -> recovers the exact original pairing & identity!
        let macViewer = MacCameraViewerController(persistence: macProvider, deviceName: "Owner's Mac")
        backend.forcedCopyStatus = errSecAuthFailed
        macViewer.activate()
        XCTAssertEqual(macViewer.keychainAccessError, "Allow Keychain access to keep this Mac paired to the camera.")

        backend.forcedCopyStatus = nil
        macViewer.retryKeychainAccess()
        defer { macViewer.shutdown() }

        XCTAssertNil(macViewer.keychainAccessError)
        XCTAssertTrue(macViewer.telemetry.isPaired)
        XCTAssertEqual(macProvider.load(), macPairedState, "Original pairing state and private key must remain intact")
    }

    func testHostPromptsAttachedAtAppRootInWatchModeAndV2BackoffUnchanged() async throws {
        // 1. Verify HostRootWindowView attaches host modals above the localHost/watchCamera mode switch
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let appSrc = try String(contentsOf: repoRoot.appendingPathComponent("macos/App/KinectHostApp.swift"), encoding: .utf8)
        let mainViewSrc = try String(contentsOf: repoRoot.appendingPathComponent("macos/App/MainContentView.swift"), encoding: .utf8)

        XCTAssertTrue(appSrc.contains(".sheet(isPresented: $controller.showPairingSheet)"))
        XCTAssertTrue(appSrc.contains(".sheet(isPresented: $controller.showRecordingReminderDialog)"))
        XCTAssertFalse(mainViewSrc.contains(".sheet(isPresented: $controller.showRecordingReminderDialog)"), "Recording reminder sheet must live on HostRootWindowView, not only MainContentView")

        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase9a-rootprompt-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let hostCtrl = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { hostCtrl.shutdown() }

        let remoteViewer = MacCameraViewerController(persistence: InMemoryPersistenceProvider())
        defer { remoteViewer.shutdown() }

        var mode: MacApplicationMode = .watchCamera
        let rootView = HostRootWindowView(
            controller: hostCtrl,
            remoteViewer: remoteViewer,
            selectedMode: .init(get: { mode }, set: { mode = $0 })
        )
        hostCtrl.showRecordingReminderDialog = true
        XCTAssertEqual(
            rootView.localHostStatusBannerInWatchMode,
            "Your iPhone requested a Depth/IR recording on this Mac."
        )

        // 2. Verify v2 Mac host reconnect back-off stays [0.40, 0.40, 1.0, 1.0] and resets only after a real session starts
        hostCtrl.showRecordingReminderDialog = false
        hostCtrl.openPairingWindow()
        let offer = try XCTUnwrap(hostCtrl.activePairingOffer)
        let parsed = try XCTUnwrap(QRPairingPayload.parse(offer.qrURI))
        let loopbackURI = QRPairingPayload(
            serviceId: parsed.serviceId,
            serverCertSHA256: parsed.serverCertSHA256,
            oneTimeToken: offer.qrToken,
            hostIP: "127.0.0.1",
            pairPort: offer.port
        ).toURIString()

        let v2Delays = Phase9SyncBox<[Double]>([])
        let retryBox = Phase9SyncBox<(@Sendable () -> Void)?>(nil)
        let v2Client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "v2 Backoff iPhone",
            deviceKind: "iphone"
        )
        v2Client.onReconnectScheduledForTesting = { delay, retry in
            v2Delays.update { $0.append(delay) }
            retryBox.update { $0 = retry }
        }
        let v2VM = PocketViewModel(client: v2Client, registerLifecycleObservers: false)
        defer { v2Client.stop() }

        v2VM.submitQRCodeURI(loopbackURI)
        let connected = await waitUntil { v2VM.telemetry.isConnected }
        XCTAssertTrue(connected)

        // Shut down v2 host and stand up a plain TCP drop listener so retries fail without TLS alert
        hostCtrl.shutdown()
        let firstV2Drop = await waitUntil { v2Delays.read().count >= 1 }
        XCTAssertTrue(firstV2Drop)

        let (dropListener, dropReady) = try Self.makeTCPDropListener(on: nil)
        defer { dropListener.cancel() }
        let listenerReady = await waitUntil { dropReady.read() }
        XCTAssertTrue(listenerReady)
        let dropPort = try XCTUnwrap(dropListener.port?.rawValue)

        for idx in 2...4 {
            v2Client.connectDirect(host: "127.0.0.1", port: dropPort)
            let gotDelay = await waitUntil { v2Delays.read().count >= idx }
            XCTAssertTrue(gotDelay)
        }
        XCTAssertEqual(v2Delays.read(), [0.40, 0.40, 1.0, 1.0], "v2 reconnect back-off must stay unchanged")
        XCTAssertEqual(v2VM.disconnectedStatusTitle, SharedRecordingReminderConstants.viewerNotOpenText)
    }
}
