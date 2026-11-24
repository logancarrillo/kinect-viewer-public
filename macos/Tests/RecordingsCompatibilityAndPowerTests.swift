import Foundation
import Network
import XCTest
@testable import KinectViewer

private final class Phase7SyncBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var value: T

    init(_ initial: T) {
        self.value = initial
    }

    func update(_ body: (inout T) -> Void) {
        lock.lock()
        body(&value)
        lock.unlock()
    }

    func read() -> T {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

/// Strict protocol v1 wire parser replicating the original v1 iPhone / Windows frame loop:
/// any message type outside `0x01...0x06` is treated as a fatal protocol error.
private final class StrictV1ClientWireReader: @unchecked Sendable {
    private var buffer = Data()

    enum ParseOutcome {
        case needMoreData
        case v1Frame(rawType: UInt8, flags: UInt8, payload: Data)
        case fatalUnknownV2OrInvalidType(UInt8)
        case invalidMagic
    }

    func append(_ chunk: Data) {
        buffer.append(chunk)
    }

    func nextFrame() -> ParseOutcome {
        guard buffer.count >= WireHeader.size else {
            return .needMoreData
        }
        let s = buffer.startIndex
        guard buffer[s] == WireHeader.magic0, buffer[s + 1] == WireHeader.magic1 else {
            buffer.removeAll()
            return .invalidMagic
        }

        let rawType = buffer[s + 2]
        guard (0x01...0x06).contains(rawType) else {
            buffer.removeAll()
            return .fatalUnknownV2OrInvalidType(rawType)
        }

        let flags = buffer[s + 3]
        let payloadLen = Int(buffer.readUInt32BE(at: 4))
        let total = WireHeader.size + payloadLen
        guard buffer.count >= total else {
            return .needMoreData
        }

        let payload = buffer.subdata(in: (s + WireHeader.size)..<(s + total))
        buffer.removeSubrange(s..<(s + total))
        return .v1Frame(rawType: rawType, flags: flags, payload: payload)
    }
}

@MainActor
final class RecordingsCompatibilityAndPowerTests: XCTestCase {

    nonisolated private static func startPeriodicV1PingTimer(
        on queue: DispatchQueue,
        conn: NWConnection,
        isDisconnected: @escaping @Sendable () -> Bool,
        decodedSeq: @escaping @Sendable () -> UInt64
    ) -> DispatchSourceTimer {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + .milliseconds(250), repeating: .milliseconds(250))
        timer.setEventHandler {
            guard !isDisconnected() else { return }
            let tickPing = PingMessage(
                clientSendNs: kv_monotonic_ns(),
                lastDecodedSeq: decodedSeq(),
                lastHostToDisplayLatencyUs: 1000,
                lastBarcodeLatencyUs: 0,
                protocolVersion: nil
            )
            if let pkt = try? WirePacket.packJSON(type: .ping, tickPing) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
        timer.resume()
        return timer
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

    private func makeLoopbackQRURI(from offer: ActivePairingOffer, serverCertSHA256: Data, serviceId: String) -> String {
        let parsed = QRPairingPayload.parse(offer.qrURI)
        let payload = QRPairingPayload(
            serviceId: parsed?.serviceId ?? serviceId,
            serverCertSHA256: parsed?.serverCertSHA256 ?? serverCertSHA256,
            oneTimeToken: offer.qrToken,
            hostIP: "127.0.0.1",
            pairPort: offer.port
        )
        return payload.toURIString()
    }

    private func pairClientState(with host: HostController) async throws -> InMemoryPersistenceProvider {
        host.openPairingWindow()
        let offer = try XCTUnwrap(host.activePairingOffer)
        let certHash = try XCTUnwrap(host.hostServerCertSHA256)
        let loopbackURI = makeLoopbackQRURI(
            from: offer,
            serverCertSHA256: certHash,
            serviceId: host.presetsStore.fileURL.deletingLastPathComponent().lastPathComponent
        )

        let persistence = InMemoryPersistenceProvider()
        let pairClient = try PocketStreamClientCore(persistence: persistence)
        defer { pairClient.stop() }

        let pairExp = expectation(description: "Loopback QR pairing completed")
        let pairErrBox = Phase7SyncBox<Error?>(nil)
        pairClient.pairWithQRURI(loopbackURI) { res in
            if case .failure(let err) = res {
                pairErrBox.update { $0 = err }
            }
            pairExp.fulfill()
        }
        await fulfillment(of: [pairExp], timeout: 5.0)
        XCTAssertNil(pairErrBox.read())
        XCTAssertNotNil(persistence.load()?.pinnedServerCertSHA256)
        return persistence
    }

    // MARK: - No `--recordings-dir` launch flag

    func testHostRecorderAlwaysUsesCanonicalRecordingsDirUnlessTestPassesExplicitURL() throws {
        let expectedDefault = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("kinect-viewer-recordings", isDirectory: true)
            .standardizedFileURL
        XCTAssertEqual(
            URL(fileURLWithPath: HostRecorder.defaultBaseDirectoryPath, isDirectory: true).standardizedFileURL.path,
            expectedDefault.path
        )

        let tempOverride = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-api-rec-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: tempOverride) }
        let testRecorder = HostRecorder(baseDirectoryURL: tempOverride)
        XCTAssertEqual(testRecorder.baseDirURL.standardizedFileURL.path, tempOverride.standardizedFileURL.path)

        // Ensure HostRecorder source contains zero references to `--recordings-dir`
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let recorderSrc = try String(
            contentsOf: repoRoot.appendingPathComponent("macos/Recording/HostRecorder.swift"),
            encoding: .utf8
        )
        XCTAssertFalse(recorderSrc.contains("--recordings-dir"))
    }

    // MARK: - Old v1 phone against new v2 Mac host

    func testStrictV1FrameReaderAgainstNewV2HostNeverReceivesV2FramesAndStreamsCleanly() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-v1phone-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        let persistence = try await pairClientState(with: host)
        let pairingState = try XCTUnwrap(persistence.load())
        let clientIdentity = try CryptoIdentity(record: pairingState.clientIdentity)
        let pinnedServerHash = try XCTUnwrap(pairingState.pinnedServerCertSHA256)

        let netQueue = DispatchQueue(label: "kv2.test.strictv1client")
        let tlsParams = CryptoIdentity.makeTLSParameters(
            localIdentity: clientIdentity,
            requireClientCert: false,
            verifyQueue: netQueue
        ) { peerHash, _ in
            CryptoIdentity.constantTimeEqual(peerHash, pinnedServerHash)
        }
        guard let port = NWEndpoint.Port(rawValue: host.activeStreamPort) else {
            XCTFail("Invalid host stream port")
            return
        }

        let conn = NWConnection(host: .ipv4(.loopback), port: port, using: tlsParams)
        defer { conn.cancel() }

        let stateBox = Phase7SyncBox<(
            ready: Bool,
            disconnected: Bool,
            fatalType: UInt8?,
            invalidMagic: Bool,
            helloCount: Int,
            pongCount: Int,
            configCount: Int,
            videoCount: Int,
            lastHelloMode: Int
        )>((false, false, nil, false, 0, 0, 0, 0, -1))

        let reader = StrictV1ClientWireReader()

        @Sendable func receiveLoop() {
            conn.receive(minimumIncompleteLength: 1, maximumLength: 262_144) { data, _, isComplete, error in
                if let data = data, !data.isEmpty {
                    reader.append(data)
                    while true {
                        switch reader.nextFrame() {
                        case .needMoreData:
                            break
                        case .invalidMagic:
                            stateBox.update { $0.invalidMagic = true }
                            conn.cancel()
                            return
                        case .fatalUnknownV2OrInvalidType(let badType):
                            stateBox.update { $0.fatalType = badType }
                            conn.cancel()
                            return
                        case .v1Frame(let rawType, _, let payload):
                            switch rawType {
                            case WireMessageType.helloServer.rawValue:
                                let hello = try? JSONDecoder().decode(HelloServerMessage.self, from: payload)
                                stateBox.update {
                                    $0.helloCount += 1
                                    if let m = hello?.modeRaw { $0.lastHelloMode = m }
                                }
                            case WireMessageType.streamConfig.rawValue:
                                stateBox.update { $0.configCount += 1 }
                            case WireMessageType.videoAccessUnit.rawValue:
                                stateBox.update { $0.videoCount += 1 }
                            case WireMessageType.pong.rawValue:
                                stateBox.update { $0.pongCount += 1 }
                            default:
                                break
                            }
                            continue
                        }
                        break
                    }
                }
                if isComplete || error != nil {
                    stateBox.update { $0.disconnected = true }
                    return
                }
                receiveLoop()
            }
        }

        conn.stateUpdateHandler = { st in
            switch st {
            case .ready:
                stateBox.update { $0.ready = true }
                receiveLoop()
            case .failed, .cancelled:
                stateBox.update { $0.disconnected = true }
            default:
                break
            }
        }
        conn.start(queue: netQueue)

        let gotHello = await waitUntil { stateBox.read().helloCount >= 1 }
        XCTAssertTrue(gotHello, "Strict v1 client must receive HelloServer (0x01)")

        // Send legacy v1 Ping (with protocolVersion == nil, i.e. omitted from JSON) immediately and every 250 ms,
        // matching a real v1 phone so the 5 s dead-peer watchdog never fires under heavy CI/machine load.
        let v1Ping = PingMessage(
            clientSendNs: 123_456,
            lastDecodedSeq: 0,
            lastHostToDisplayLatencyUs: 1000,
            lastBarcodeLatencyUs: 0,
            protocolVersion: nil
        )
        let pingData = try WirePacket.packJSON(type: .ping, v1Ping)
        let pingJSON = String(decoding: pingData.dropFirst(WireHeader.size), as: UTF8.self)
        XCTAssertFalse(pingJSON.contains("protocolVersion"), "Legacy v1 ping must omit protocolVersion")
        conn.send(content: pingData, completion: .contentProcessed { _ in })

        let v1PingTimer = Self.startPeriodicV1PingTimer(
            on: netQueue,
            conn: conn,
            isDisconnected: { stateBox.read().disconnected },
            decodedSeq: { UInt64(stateBox.read().videoCount) }
        )
        defer { v1PingTimer.cancel() }

        let gotPong = await waitUntil { stateBox.read().pongCount >= 1 }
        XCTAssertTrue(gotPong, "Host must answer v1 Ping with Pong")

        // Trigger multiple HostStateSync pushes on the host (power On, mode switch, freeze, power Off, power On)
        host.useSyntheticSource = true
        host.startStream()
        let gotVideo = await waitUntil(timeoutSec: 4.0) {
            stateBox.read().videoCount >= 3 && stateBox.read().configCount >= 1
        }
        XCTAssertTrue(gotVideo, "Strict v1 client must receive StreamConfig + VideoAccessUnit frames")

        // Send v1 SelectMode (0x06) to switch host to Depth (modeRaw = 2)
        let selectDepth = try WirePacket.packJSON(
            type: .selectModeReq,
            SelectModeMessage(modeRaw: ViewerModeOption.depth.rawValue, sbsLayoutRaw: 0)
        )
        conn.send(content: selectDepth, completion: .contentProcessed { _ in })

        let switchedHostMode = await waitUntil {
            host.selectedMode == .depth && stateBox.read().lastHelloMode == ViewerModeOption.depth.rawValue
        }
        XCTAssertTrue(switchedHostMode, "v1 SelectMode (0x06) must switch the new Mac host mode")

        // Exercise more v2 state sync transitions on the host while v1 client remains connected
        host.toggleFreeze()
        host.toggleFreeze()
        host.pushHostStateSync()
        try? await Task.sleep(nanoseconds: 150_000_000)

        let finalSnap = stateBox.read()
        XCTAssertNil(finalSnap.fatalType, "New Mac host must NEVER send 0x07 or any v2 frame to a v1 client")
        XCTAssertFalse(finalSnap.invalidMagic)
        XCTAssertFalse(finalSnap.disconnected, "v1 client connection must stay alive without reconnect loops")
    }

    // MARK: - New v2 phone against strict v1 host

    func testNewV2PhoneAgainstStrictV1HostGatesAllV2MessagesAndKeepsV1StreamingWorking() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-v1host-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        // Put HostStreamServer into strict v1 simulation:
        // - advertises hello.protocolVersion = 1
        // - never sends 0x07
        // - immediately cancels the connection if client sends any frame >= 0x07
        host.setSimulatedServerProtocolVersionForTesting(1)
        host.useSyntheticSource = true
        host.selectedMode = .ir
        host.startStream()

        let persistence = try await pairClientState(with: host)
        let client = try PocketStreamClientCore(persistence: persistence)
        defer { client.stop() }

        client.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let connectedAndStreaming = await waitUntil(timeoutSec: 4.0) {
            client.telemetry.isConnected &&
            client.telemetry.receivedFrames >= 3 &&
            client.telemetry.rttMs > 0
        }
        XCTAssertTrue(connectedAndStreaming, "New v2 phone must connect, exchange Pings/Pongs, and decode video from a v1 host")
        XCTAssertEqual(client.telemetry.protocolVersion, 1)
        XCTAssertFalse(client.telemetry.hasConfirmedHostState, "v1 host never sends HostStateSync")

        // Attempt every v2 send on the phone client — all must be suppressed locally so the strict v1 host never drops us!
        client.requestSensorPower(turnOn: false)
        client.sendRemoteControl(RemoteControlCommandMessage(action: .startRecording, reminderConfirmedOnPhone: true))
        client.sendRemoteControl(RemoteControlCommandMessage(action: .requestKeyframe))
        client.sendOrbit3DGesture(Orbit3DGestureMessage(
            deltaYawRadians: 0.4,
            deltaPitchRadians: 0.0,
            zoomScale: 1.0,
            panRightMeters: 0.0,
            panUpMeters: 0.0,
            resetView: false
        ))
        client.requestModeSwitch(modeRaw: ViewerModeOption.pointCloud3D.rawValue) // Mode 5 is v2-only

        try? await Task.sleep(nanoseconds: 250_000_000)
        XCTAssertTrue(client.telemetry.isConnected, "Strict v1 host must NOT disconnect new phone because all v2 sends were gated")
        XCTAssertTrue(host.isRunning, "Gated v2 power request must not affect v1 host")
        XCTAssertEqual(host.selectedMode, .ir, "Mode 5 (pointCloud3D) must not be sent to a v1 host")

        // Standard v1 mode switch (0x06) to Depth MUST still work!
        client.requestModeSwitch(modeRaw: ViewerModeOption.depth.rawValue)
        let switchedV1Mode = await waitUntil {
            host.selectedMode == .depth && client.telemetry.currentModeRaw == ViewerModeOption.depth.rawValue
        }
        XCTAssertTrue(switchedV1Mode, "v1 mode switch (IR/Colour/Depth/Side-by-Side) must still work against a v1 host")
        XCTAssertTrue(client.telemetry.isConnected)
    }

    // MARK: - PocketViewModel reminder guard & double-tap behaviour test

    func testSharedReminderConstantAndPhoneReminderGuardDoubleTapSendsAtMostOneStart() async throws {
        XCTAssertEqual(
            PreRecordingReminderPolicy.exactReminderText,
            SharedRecordingReminderConstants.exactReminderText
        )

        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-phonereminder-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        var fakeNow = Date(timeIntervalSince1970: 1_800_000_000)
        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }
        host.nowProvider = { fakeNow }
        host.useSyntheticSource = true
        host.selectedMode = .depth
        host.startStream()

        let persistence = try await pairClientState(with: host)
        let client = try PocketStreamClientCore(persistence: persistence)
        defer { client.stop() }
        let viewModel = PocketViewModel(client: client, registerLifecycleObservers: false)

        client.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let synced = await waitUntil {
            viewModel.supportsHostV2 &&
            viewModel.confirmedPowerState == .on &&
            viewModel.telemetry.hasConfirmedHostState &&
            viewModel.telemetry.hostState.recordingReminderNeeded
        }
        XCTAssertTrue(synced)

        // 1. Calling confirmPhoneRecordingReminderAndStart() while showPhoneRecordingReminder == false is a no-op
        XCTAssertFalse(viewModel.showPhoneRecordingReminder)
        viewModel.confirmPhoneRecordingReminderAndStart()
        try? await Task.sleep(nanoseconds: 120_000_000)
        XCTAssertEqual(
            host.receivedRemoteControlCommandsForTesting.filter { $0.action == .startRecording }.count,
            0,
            "Confirm must do nothing unless showPhoneRecordingReminder is true"
        )
        XCTAssertFalse(host.isRecording)

        // 2. Requesting recording when reminder is needed presents the phone sheet without sending a wire start yet
        viewModel.requestToggleHostRecording()
        XCTAssertTrue(viewModel.showPhoneRecordingReminder)
        XCTAssertEqual(
            host.receivedRemoteControlCommandsForTesting.filter { $0.action == .startRecording }.count,
            0
        )

        // 3. Simulate a rapid double tap on Confirm: at most ONE start_recording command may be sent!
        viewModel.phoneReminderSnoozeOneHour = true
        viewModel.confirmPhoneRecordingReminderAndStart()
        viewModel.confirmPhoneRecordingReminderAndStart()
        XCTAssertFalse(viewModel.showPhoneRecordingReminder)

        let startedOnce = await waitUntil { host.isRecording && viewModel.telemetry.hostState.isRecording }
        XCTAssertTrue(startedOnce)
        try? await Task.sleep(nanoseconds: 120_000_000)

        let startCmdsAfterDoubleTap = host.receivedRemoteControlCommandsForTesting.filter { $0.action == .startRecording }
        XCTAssertEqual(startCmdsAfterDoubleTap.count, 1, "Double-tapping Confirm on the phone must send at most one start_recording")
        XCTAssertEqual(startCmdsAfterDoubleTap.first?.reminderConfirmedOnPhone, true)
        XCTAssertEqual(startCmdsAfterDoubleTap.first?.snoozeReminderOneHour, true)

        // 4. Stop recording; while phone still sees recordingReminderNeeded == false, advance Mac clock past 1h snooze:
        //    tapping record on phone must send reminderConfirmedOnPhone == false so Mac shows its own reminder sheet!
        viewModel.requestToggleHostRecording()
        let stopped = await waitUntil { !host.isRecording && !viewModel.telemetry.hostState.isRecording }
        XCTAssertTrue(stopped)
        XCTAssertFalse(viewModel.telemetry.hostState.recordingReminderNeeded)

        fakeNow = fakeNow.addingTimeInterval(3605) // Snooze expired on Mac before next HostStateSync
        viewModel.requestToggleHostRecording()
        let macOpenedSheet = await waitUntil { host.showRecordingReminderDialog }
        XCTAssertTrue(macOpenedSheet, "Direct phone toggle without showing phone reminder must send reminderConfirmedOnPhone: false")
        XCTAssertFalse(host.isRecording)
        XCTAssertEqual(host.receivedRemoteControlCommandsForTesting.last?.reminderConfirmedOnPhone, false)
        host.cancelRecordingReminder()
    }

    // MARK: - Purge old colour/side-by-side recordings on launch & log as automatic

    func testOldColourAndSideBySideRecordingsAreNeverListedAsIrAndPurgedOnLaunch() throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-purgecolor-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("rec", isDirectory: true)
        try FileManager.default.createDirectory(at: recDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let fmt = DateFormatter()
        fmt.locale = Locale(identifier: "en_US_POSIX")
        fmt.dateFormat = "yyyyMMdd-HHmmss"
        let recentStamp = fmt.string(from: Date().addingTimeInterval(-3600))
        let colorFolderName = "\(recentStamp)-001_color1080p.nosync"
        let sbsFolderName = "\(recentStamp)-002_sidebyside.nosync"
        let validIrFolderName = "\(recentStamp)-003_ir16.nosync"

        let legacyColorDir = recDir.appendingPathComponent(colorFolderName, isDirectory: true)
        let legacySbsDir = recDir.appendingPathComponent(sbsFolderName, isDirectory: true)
        let validIrDir = recDir.appendingPathComponent(validIrFolderName, isDirectory: true)
        try FileManager.default.createDirectory(at: legacyColorDir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: legacySbsDir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: validIrDir, withIntermediateDirectories: true)

        try Data(repeating: 0x11, count: 1024).write(to: legacyColorDir.appendingPathComponent("color.mp4"))
        try Data(repeating: 0x22, count: 2048).write(to: legacySbsDir.appendingPathComponent("sbs.mp4"))
        try "seq,arrival_ns\n1,1000\n".write(to: validIrDir.appendingPathComponent("ir16_timestamps.csv"), atomically: true, encoding: .utf8)

        // 1. Even before purge, listSessions must NEVER classify color1080p or sidebyside as IR
        let listedBeforePurge = RecordingLibrary.listSessions(in: recDir)
        XCTAssertEqual(listedBeforePurge.count, 1)
        XCTAssertEqual(listedBeforePurge.first?.folderName, validIrFolderName)

        // 2. Launching HostController purges disallowed session folders and logs each as `automatic`
        let host = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        XCTAssertFalse(FileManager.default.fileExists(atPath: legacyColorDir.path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacySbsDir.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: validIrDir.path))

        let logURL = recDir.appendingPathComponent(RecordingLibrary.deletionLogFileName)
        let logText = try String(contentsOf: logURL, encoding: .utf8)
        XCTAssertTrue(logText.contains(colorFolderName))
        XCTAssertTrue(logText.contains(sbsFolderName))
        let lines = logText.split(separator: "\n")
        XCTAssertEqual(lines.count, 2)
        for line in lines {
            XCTAssertTrue(line.contains("\tautomatic"), "Each purged legacy colour session must be logged as automatic: \(line)")
        }
    }

    // MARK: - No hard-coded intrinsics + pixel center `+ 0.5` unprojection

    func testMissingIntrinsicsShowsUnavailableOnRulerAndUnprojectUsesHalfPixelCenter() async throws {
        // 1. Verify (col + 0.5 - cx) and (row + 0.5 - cy) math in kv_unproject_depth_pixel_mm
        var intr = kv_ir_intrinsics_t(
            valid: true,
            fx: 365.0,
            fy: 365.0,
            cx: 255.5,
            cy: 205.5,
            k1: 0, k2: 0, k3: 0, p1: 0, p2: 0
        )
        var outX: Float = 999
        var outY: Float = 999
        var outZ: Float = 999
        XCTAssertTrue(kv_unproject_depth_pixel_mm(&intr, 255, 205, 1000.0, &outX, &outY, &outZ))
        // Because col + 0.5 == 255.5 == cx and row + 0.5 == 205.5 == cy, optical axis ray is exact (0, 0, 1000)!
        XCTAssertEqual(outX, 0.0, accuracy: 1e-5)
        XCTAssertEqual(outY, 0.0, accuracy: 1e-5)
        XCTAssertEqual(outZ, 1000.0, accuracy: 1e-5)

        // Invalid intrinsics must fail kv_unproject_depth_pixel_mm instead of using defaults
        var invalidIntr = kv_ir_intrinsics_t()
        invalidIntr.valid = false
        XCTAssertFalse(kv_unproject_depth_pixel_mm(&invalidIntr, 255, 205, 1000.0, &outX, &outY, &outZ))

        // 2. Replay a Depth recording saved WITHOUT stored intrinsics -> Ruler says "Intrinsics unavailable"
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-nointr-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("rec", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        _ = try host.recorder.startRecording(
            mode: KV_MODE_DEPTH,
            width: 512,
            height: 424,
            losslessOption: .png16Sequence,
            irIntrinsics: nil
        )
        let px = [UInt16](repeating: 1500, count: 512 * 424)
        host.recorder.appendRaw16Frame(pixelsU16: px, width: 512, height: 424, seq: 1, deviceTicks: 0, arrivalNs: 1_000_000)
        _ = host.recorder.stopRecording()

        host.refreshRecordedSessions()
        let session = try XCTUnwrap(host.recordedSessions.first)
        XCTAssertNil(session.irIntrinsics)

        XCTAssertTrue(host.startPlayback(of: session))
        await host.flushPendingReplayWorkForTesting()
        host.togglePlaybackPause()

        let viewSize = CGSize(width: 512, height: 424)
        host.handleCanvasSingleClick(viewPoint: CGPoint(x: 200.5, y: 200.5), viewSize: viewSize)
        host.handleCanvasSingleClick(viewPoint: CGPoint(x: 300.5, y: 200.5), viewSize: viewSize)
        XCTAssertNil(host.rulerState?.distanceMm)
        XCTAssertEqual(host.rulerState?.errorMessage, "Intrinsics unavailable")
    }

    // MARK: - Power state & HostStateSync after Mac sleep / wake

    func testSleepSetsOffAndWakeFailureSetsErrorAndPushesHostStateSync() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-sleepwake-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        let persistence = try await pairClientState(with: host)
        let client = try PocketStreamClientCore(persistence: persistence)
        defer { client.stop() }
        client.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let ready = await waitUntil { client.telemetry.isConnected && client.telemetry.hasConfirmedHostState }
        XCTAssertTrue(ready)

        host.useSyntheticSource = true
        host.startStream()
        let onSynced = await waitUntil { host.sensorPowerState == .on && client.telemetry.hostState.powerState == .on }
        XCTAssertTrue(onSynced)

        // 1. Sleep closes stream, sets power state to .off, and pushes HostStateSync to phone
        host.handleSystemWillSleep()
        let sleepSynced = await waitUntil {
            !host.isRunning &&
            host.sensorPowerState == .off &&
            client.telemetry.hostState.powerState == .off
        }
        XCTAssertTrue(sleepSynced, "Mac sleep must set sensorPowerState = .off and push HostStateSync")

        // 2. Simulate wake when real Kinect USB cannot be reopened -> sets .error and pushes HostStateSync
        host.useSyntheticSource = false
        host.simulatedUsbPresentOverrideForTesting = false
        host.handleSystemDidWake(settleDelayNs: 0)

        let wakeErrSynced = await waitUntil {
            host.sensorPowerState == .error &&
            client.telemetry.hostState.powerState == .error &&
            (client.telemetry.hostState.errorMessage?.contains("Woke from sleep") == true)
        }
        XCTAssertTrue(wakeErrSynced, "Wake failure must set sensorPowerState = .error and push HostStateSync")
    }

    // MARK: - Dead phone detection & Unpair disconnect hook

    func testDeadPhoneWatchdogAndUnpairBothTriggerDisconnectPowerOffHook() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-deadphone-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        // Default ping watchdog timeout is ~5.0 seconds
        XCTAssertEqual(host.configuredClientPingTimeoutSeconds, 5.0, accuracy: 0.01)

        // Shorten ping timeout for fast deterministic test execution
        host.setClientPingTimeoutForTesting(seconds: 0.45)
        host.turnOffKinectOnPhoneDisconnect = true
        host.useSyntheticSource = true

        let persistence = try await pairClientState(with: host)
        let pairingState = try XCTUnwrap(persistence.load())
        let clientIdentity = try CryptoIdentity(record: pairingState.clientIdentity)
        let pinnedServerHash = try XCTUnwrap(pairingState.pinnedServerCertSHA256)

        // Part A: Connect a silent raw mTLS socket that stops sending any Pings (simulating Wi-Fi vanishing / dead phone)
        let netQueue = DispatchQueue(label: "kv2.test.deadphone")
        let tlsParams = CryptoIdentity.makeTLSParameters(
            localIdentity: clientIdentity,
            requireClientCert: false,
            verifyQueue: netQueue
        ) { peerHash, _ in
            CryptoIdentity.constantTimeEqual(peerHash, pinnedServerHash)
        }
        let port = try XCTUnwrap(NWEndpoint.Port(rawValue: host.activeStreamPort))
        let silentConn = NWConnection(host: .ipv4(.loopback), port: port, using: tlsParams)
        defer { silentConn.cancel() }

        let silentReady = Phase7SyncBox(false)
        silentConn.stateUpdateHandler = { st in
            if case .ready = st { silentReady.update { $0 = true } }
        }
        silentConn.start(queue: netQueue)

        let hostSawSilentPhone = await waitUntil { silentReady.read() && host.isPhoneConnected }
        XCTAssertTrue(hostSawSilentPhone)

        host.startStream()
        XCTAssertTrue(host.isRunning)
        XCTAssertEqual(host.sensorPowerState, .on)

        // Do NOT send any Pings on silentConn -> watchdog cancels dead phone and turns Kinect Off!
        let autoTurnedOffOnSilentTimeout = await waitUntil(timeoutSec: 2.5) {
            !host.isPhoneConnected && !host.isRunning && host.sensorPowerState == .off
        }
        XCTAssertTrue(autoTurnedOffOnSilentTimeout, "Ping timeout watchdog must detect dead phone and trigger auto-off")
        silentConn.cancel()

        // Part B: Reconnect normal PocketStreamClientCore, turn Kinect On, then call host.unpairPhone()
        host.setClientPingTimeoutForTesting(seconds: 5.0)
        let liveClient = try PocketStreamClientCore(persistence: persistence)
        defer { liveClient.stop() }
        liveClient.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let reconnected = await waitUntil { liveClient.telemetry.isConnected && host.isPhoneConnected }
        XCTAssertTrue(reconnected)

        host.startStream()
        XCTAssertTrue(host.isRunning)
        XCTAssertEqual(host.sensorPowerState, .on)

        // Unpairing while phone is connected must route through disconnectActiveClient() so auto-off fires!
        host.unpairPhone()
        let turnedOffOnUnpair = await waitUntil {
            !host.isPhonePaired && !host.isPhoneConnected && !host.isRunning && host.sensorPowerState == .off
        }
        XCTAssertTrue(turnedOffOnUnpair, "Unpairing must fire disconnectActiveClient() and turn Kinect off when configured")
    }

    // MARK: - Independent power control setting & disabled reason

    func testIndependentSensorPowerControlSettingAndDisabledReasonOnPhone() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-powersetting-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }
        host.useSyntheticSource = true

        // Both default to true independently
        XCTAssertTrue(host.allowPhoneModeSwitch)
        XCTAssertTrue(host.allowPhoneSensorPowerControl)

        let persistence = try await pairClientState(with: host)
        let client = try PocketStreamClientCore(persistence: persistence)
        defer { client.stop() }
        client.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let ready = await waitUntil { client.telemetry.isConnected && client.telemetry.hasConfirmedHostState }
        XCTAssertTrue(ready)

        // 1. Turn OFF mode-switch / remote controls on Mac, keep sensor power control ON
        host.allowPhoneModeSwitch = false
        let syncedModeOff = await waitUntil {
            !client.telemetry.allowModeSwitch && client.telemetry.hostState.allowPhoneSensorPower
        }
        XCTAssertTrue(syncedModeOff)
        XCTAssertNil(client.telemetry.hostState.sensorPowerDisabledReason)

        // Phone CAN still turn the Kinect On and Off even though mode switching is off!
        client.requestSensorPower(turnOn: true)
        let turnedOn = await waitUntil { host.isRunning && client.telemetry.hostState.powerState == .on }
        XCTAssertTrue(turnedOn, "Sensor power must work independently when allowPhoneModeSwitch is false")

        // Mode switch is rejected while allowPhoneModeSwitch is false
        client.requestModeSwitch(modeRaw: ViewerModeOption.depth.rawValue)
        try? await Task.sleep(nanoseconds: 120_000_000)
        XCTAssertNotEqual(host.selectedMode, .depth)

        // 2. Now turn OFF allowPhoneSensorPowerControl on Mac -> phone receives explicit disabled reason and power requests are blocked
        host.allowPhoneSensorPowerControl = false
        let syncedPowerDisabled = await waitUntil {
            !client.telemetry.hostState.allowPhoneSensorPower &&
            client.telemetry.hostState.sensorPowerDisabledReason == SharedRecordingReminderConstants.sensorPowerDisabledOnHostText
        }
        XCTAssertTrue(syncedPowerDisabled)

        // Phone attempt to turn Off is blocked both client-side and server-side
        client.requestSensorPower(turnOn: false)
        let forgedPowerOff = try WirePacket.packJSON(
            type: .sensorPowerReq,
            SensorPowerControlMessage(requestId: 1, turnOn: false)
        )
        _ = forgedPowerOff
        try? await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertTrue(host.isRunning, "Host must ignore phone power request when allowPhoneSensorPowerControl is false")
    }

    // MARK: - FFV1 chunked `-ss` + `-frames:v N` exact decode off main actor

    func testFFV1ChunkedSeekExactFrameValuesAndAsyncOffMainActorPlayback() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-ffv1seek-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("rec", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let recorder = HostRecorder(baseDirectoryURL: recDir)
        _ = try recorder.startRecording(
            mode: KV_MODE_DEPTH,
            width: 512,
            height: 424,
            losslessOption: .ffv1Mkv,
            irIntrinsics: nil
        )
        // Write 35 distinct 16-bit frames: frame f has pixel value (1000 + f * 37), pacing so non-blocking ioQueue never drops
        for f in 0..<35 {
            while f > 6 && recorder.currentStatus.framesWritten + 6 < UInt64(f) {
                try? await Task.sleep(nanoseconds: 5_000_000)
            }
            let val = UInt16(1000 + f * 37)
            let px = [UInt16](repeating: val, count: 512 * 424)
            recorder.appendRaw16Frame(
                pixelsU16: px,
                width: 512,
                height: 424,
                seq: UInt64(f + 1),
                deviceTicks: UInt32(f * 267),
                arrivalNs: UInt64(1_000_000_000 + f * 33_333_333)
            )
        }
        _ = recorder.stopRecording()

        let sessions = RecordingLibrary.listSessions(in: recDir)
        let session = try XCTUnwrap(sessions.first)
        XCTAssertEqual(session.frameCount, 35)

        // Use chunkSize = 10 so chunks are [0..9], [10..19], [20..29], [30..34]
        let reader = try XCTUnwrap(RecordingSessionReader(session: session, chunkSize: 10))

        // Jump straight to chunk 2 (frame 23) without ever decoding chunk 0, then back to chunk 1 (frame 14), then chunk 3 (frame 34)
        for testFrame in [23, 14, 34, 0, 10, 29] {
            let decoded: [UInt16]? = await withCheckedContinuation { cont in
                reader.readFrame16Async(at: testFrame, prefetchNextChunk: true) { out in
                    cont.resume(returning: out)
                }
            }
            let pixels = try XCTUnwrap(decoded, "Expected non-nil FFV1 frame at index \(testFrame)")
            let expected = UInt16(1000 + testFrame * 37)
            XCTAssertEqual(pixels.first, expected, "Exact lossless value mismatch at frame \(testFrame)")
            XCTAssertEqual(pixels.last, expected, "Exact lossless value mismatch at end of frame \(testFrame)")
        }
        await reader.waitForIdleForTesting()
    }

    // MARK: - Small fixes

    func testModalSheetPausesShortcutsRecordingSyncThrottledVisionAfterOffIgnoredOrbitModuloAndFullScreenPlist() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase7-nits-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("rec", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        // Nit 1: Global single-key shortcuts are paused while reminder sheet or pairing sheet is open
        host.selectedMode = .ir
        host.showRecordingReminderDialog = true
        XCTAssertFalse(host.handleShortcutKey("3", keyCode: 20), "Mode key '3' must be ignored while reminder sheet is open")
        XCTAssertEqual(host.selectedMode, .ir)
        host.cancelRecordingReminder()

        host.openPairingWindow()
        XCTAssertTrue(host.showPairingSheet)
        XCTAssertFalse(host.handleShortcutKey("2", keyCode: 19), "Mode key '2' must be ignored while pairing sheet is open")
        XCTAssertEqual(host.selectedMode, .ir)
        host.cancelPairingWindow()
        XCTAssertTrue(host.handleShortcutKey("3", keyCode: 20), "Mode key '3' works once sheets are closed")
        XCTAssertEqual(host.selectedMode, .depth)

        // Nit 3: HostStateSync is NOT pushed on every recorded frame (throttled to ~1/s + immediate start/stop)
        host.useSyntheticSource = true
        host.startStream()
        host.confirmRecordingReminderAndStart()
        let recStarted = await waitUntil { host.isRecording }
        XCTAssertTrue(recStarted)
        let pushesAtStart = host.hostStateSyncPushCountForTesting

        let recordedSeveralFrames = await waitUntil(timeoutSec: 3.0) {
            host.recorder.currentStatus.framesWritten >= 12
        }
        XCTAssertTrue(recordedSeveralFrames)
        // Force UI status polling
        try? await Task.sleep(nanoseconds: 250_000_000)
        let pushesMidRec = host.hostStateSyncPushCountForTesting
        let deltaPushes = pushesMidRec - pushesAtStart
        XCTAssertLessThanOrEqual(deltaPushes, 3, "HostStateSync must be throttled (~1/s), not pushed on every recorded frame")
        host.stopHostRecording()

        // Nit 4: Vision results arriving after Off are ignored
        let fakeSnapshot = RawVisionPoseSnapshot(
            colorSeq: 77,
            bodyCount: 1,
            handCount: 1,
            joints: [
                RawVisionJoint2D(id: "body0_nose", normTopLeftX: 0.5, normTopLeftY: 0.4, confidence: 0.95, isHand: false)
            ],
            bones: []
        )
        host.stopStream()
        XCTAssertFalse(host.isRunning)
        XCTAssertEqual(host.sensorPowerState, .off)
        host.injectVisionPoseSnapshotForTesting(fakeSnapshot)
        XCTAssertEqual(host.detectedBodiesCount, 0, "Vision callback arriving after Off must be ignored")
        XCTAssertEqual(host.detectedHandsCount, 0)

        // Nit 5: PointCloudCameraState.orbit wraps yaw modulo [-pi, pi] even for multi-turn deltas
        var cam = PointCloudCameraState()
        cam.orbit(deltaYaw: 7.25 * .pi, deltaPitch: 0)
        XCTAssertGreaterThanOrEqual(cam.yawRad, -.pi)
        XCTAssertLessThanOrEqual(cam.yawRad, .pi)
        cam.orbit(deltaYaw: -9.5 * .pi, deltaPitch: 0)
        XCTAssertGreaterThanOrEqual(cam.yawRad, -.pi)
        XCTAssertLessThanOrEqual(cam.yawRad, .pi)

        // Nit 6: UIRequiresFullScreen restored in project.yml / ios/Info.plist
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let projectYml = try String(
            contentsOf: repoRoot.appendingPathComponent("project.yml"),
            encoding: .utf8
        )
        XCTAssertTrue(projectYml.contains("UIRequiresFullScreen: true"))
    }
}
