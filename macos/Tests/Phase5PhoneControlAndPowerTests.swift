import Foundation
import XCTest
@testable import KinectViewer

private final class SyncBox<T>: @unchecked Sendable {
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

@MainActor
final class Phase5PhoneControlAndPowerTests: XCTestCase {

    /// Helper to rewrite a generated QR pairing URI's host IP to `127.0.0.1` for deterministic loopback mTLS testing.
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

    private func pairAndConnectLoopbackClient(to host: HostController) async throws -> PocketStreamClientCore {
        host.openPairingWindow()
        let offer = try XCTUnwrap(host.activePairingOffer)
        let certHash = try XCTUnwrap(host.hostServerCertSHA256)
        let loopbackURI = makeLoopbackQRURI(from: offer, serverCertSHA256: certHash, serviceId: host.presetsStore.fileURL.deletingLastPathComponent().lastPathComponent)

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider())
        let pairExp = expectation(description: "Loopback QR pairing completed")
        let pairErrorBox = SyncBox<Error?>(nil)
        client.pairWithQRURI(loopbackURI) { res in
            if case .failure(let err) = res {
                pairErrorBox.update { $0 = err }
            }
            pairExp.fulfill()
        }
        await fulfillment(of: [pairExp], timeout: 5.0)
        let pairErr = pairErrorBox.read()
        XCTAssertNil(pairErr, "Loopback QR pairing should succeed: \(String(describing: pairErr))")

        // Connect directly to loopback stream port using pinned server hash stored during pairing
        client.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let connected = await waitUntil(timeoutSec: 4.0) {
            client.telemetry.isConnected && client.telemetry.hasConfirmedHostState
        }
        XCTAssertTrue(connected, "Paired iPhone client must connect over mTLS 1.3 and receive confirmed HostStateSyncMessage")
        return client
    }

    func testOnOffRoundTripsOverLoopbackMtlsAndMacPhoneStayInSync() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase5-roundtrip-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let lockURL = tempRoot.appendingPathComponent("KINECT-IN-USE-test")
        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: lockURL,
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        let observedStates = SyncBox<[WireSensorPowerState]>([])
        let client = try await pairAndConnectLoopbackClient(to: host)
        defer { client.stop() }
        client.onHostStateReceived = { sync in
            observedStates.update { $0.append(sync.powerState) }
        }

        // Initial confirmed state is Off
        XCTAssertEqual(client.telemetry.hostState.powerState, .off)
        XCTAssertFalse(client.telemetry.hostState.powerState.isSwitchConfirmedOn)
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path))

        // 1. Turn On from iPhone: transitions Starting -> On, acquires lock, never starts recording
        client.requestSensorPower(turnOn: true)
        let turnedOn = await waitUntil {
            host.isRunning &&
            host.sensorPowerState == .on &&
            client.telemetry.hostState.powerState == .on
        }
        XCTAssertTrue(turnedOn)
        XCTAssertTrue(FileManager.default.fileExists(atPath: lockURL.path), "Turning On must create KINECT-IN-USE lock")
        XCTAssertFalse(host.isRecording, "Turning On must never start a recording on its own")
        XCTAssertFalse(client.telemetry.hostState.isRecording)

        let sawStarting = observedStates.read().contains(.starting)
        XCTAssertTrue(sawStarting, "Phone should observe confirmed Starting state before On")

        // 2. Turn Off from iPhone: stops streaming, closes sensor, deletes lock, stays paired & connected
        client.requestSensorPower(turnOn: false)
        let turnedOff = await waitUntil {
            !host.isRunning &&
            host.sensorPowerState == .off &&
            client.telemetry.hostState.powerState == .off
        }
        XCTAssertTrue(turnedOff)
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path), "Turning Off must delete KINECT-IN-USE lock")
        XCTAssertTrue(host.isPhonePaired, "Host must remain paired after turning Kinect Off")
        XCTAssertTrue(client.telemetry.isConnected, "Host app keeps running and stays connected ready for On")

        // 3. Mac's own Start/Stop stays in sync with the phone's switch
        host.startStream()
        let syncedMacOn = await waitUntil {
            client.telemetry.hostState.powerState == .on
        }
        XCTAssertTrue(syncedMacOn)
        XCTAssertTrue(FileManager.default.fileExists(atPath: lockURL.path))

        host.stopStream()
        let syncedMacOff = await waitUntil {
            client.telemetry.hostState.powerState == .off
        }
        XCTAssertTrue(syncedMacOff)
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path))
    }

    func testLockHeldByAnotherProcessShowsHolderAndStaysOff() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase5-lockheld-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let lockURL = tempRoot.appendingPathComponent("KINECT-IN-USE-test")
        // Simulate another process holding ~/KINECT-IN-USE
        try "Protonect (pid: 99999)\n".write(to: lockURL, atomically: true, encoding: .utf8)

        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: lockURL,
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        let client = try await pairAndConnectLoopbackClient(to: host)
        defer { client.stop() }

        client.requestSensorPower(turnOn: true)
        let gotInUse = await waitUntil {
            client.telemetry.hostState.powerState == .inUseElsewhere
        }
        XCTAssertTrue(gotInUse)
        XCTAssertFalse(host.isRunning, "Host must not open sensor when lock is held by another process")
        XCTAssertFalse(client.telemetry.hostState.powerState.isSwitchConfirmedOn, "Phone switch must stay Off")

        let holder = try XCTUnwrap(client.telemetry.hostState.inUseHolder)
        let displayMessage = SharedRecordingReminderConstants.inUseByMessage(holder: holder)
        XCTAssertEqual(displayMessage, "The Kinect is in use by Protonect (pid: 99999)")

        // Once the external process releases the lock, turning On succeeds cleanly
        try FileManager.default.removeItem(at: lockURL)
        client.requestSensorPower(turnOn: true)
        let nowOn = await waitUntil {
            host.isRunning && client.telemetry.hostState.powerState == .on
        }
        XCTAssertTrue(nowOn)
    }

    func testRecordingRunningWhenOffIsPressedFinalizesCleanlyAndReleasesLock() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase5-recoff-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let lockURL = tempRoot.appendingPathComponent("KINECT-IN-USE-test")
        let recDir = tempRoot.appendingPathComponent("rec", isDirectory: true)
        let host = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: lockURL,
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        let client = try await pairAndConnectLoopbackClient(to: host)
        defer { client.stop() }

        host.selectedMode = .depth
        client.requestSensorPower(turnOn: true)
        let started = await waitUntil { host.isRunning && client.telemetry.hostState.powerState == .on }
        XCTAssertTrue(started)

        // Attempt remote recording start WITHOUT reminder confirmation -> must NOT start blindly; opens Mac reminder
        client.sendRemoteControl(RemoteControlCommandMessage(
            action: .startRecording,
            reminderConfirmedOnPhone: false,
            snoozeReminderOneHour: false
        ))
        let reminderOpenedOnMac = await waitUntil { host.showRecordingReminderDialog }
        XCTAssertTrue(reminderOpenedOnMac)
        XCTAssertFalse(host.isRecording, "Recording must not start until Phase 2 reminder is confirmed on Mac or phone")
        host.cancelRecordingReminder()

        // Confirm Phase 2 reminder on phone -> starts recording on Mac
        client.sendRemoteControl(RemoteControlCommandMessage(
            action: .startRecording,
            reminderConfirmedOnPhone: true,
            snoozeReminderOneHour: true
        ))
        let recStarted = await waitUntil {
            host.isRecording && client.telemetry.hostState.isRecording
        }
        XCTAssertTrue(recStarted)
        XCTAssertFalse(host.isRecordingReminderNeeded, "1-hour snooze from phone confirmation should apply")

        // Allow several synthetic 16-bit depth frames to be written
        let wroteFrames = await waitUntil(timeoutSec: 3.0) {
            host.recorder.currentStatus.framesWritten >= 4
        }
        XCTAssertTrue(wroteFrames)

        // Press Off on the phone while recording is running!
        client.requestSensorPower(turnOn: false)
        let stoppedCleanly = await waitUntil {
            !host.isRunning &&
            !host.isRecording &&
            client.telemetry.hostState.powerState == .off &&
            !client.telemetry.hostState.isRecording
        }
        XCTAssertTrue(stoppedCleanly)
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path), "Off must delete KINECT-IN-USE lock")

        // Verify recording file & metadata were finalized cleanly on disk
        XCTAssertEqual(host.recordedSessions.count, 1)
        let session = try XCTUnwrap(host.recordedSessions.first)
        XCTAssertGreaterThanOrEqual(session.frameCount, 4)
        XCTAssertGreaterThan(session.sizeBytes, 0)
    }

    func testPhoneDisconnectsMidToggleWithDefaultKeepAndOptionalAutoOffSetting() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase5-disconnect-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let lockURL = tempRoot.appendingPathComponent("KINECT-IN-USE-test")
        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: lockURL,
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }
        host.simulatedPowerOnDelayMs = 140

        // Default setting: "Turn the Kinect off when the iPhone disconnects" is OFF
        XCTAssertFalse(host.turnOffKinectOnPhoneDisconnect)

        let client = try await pairAndConnectLoopbackClient(to: host)
        defer { client.stop() }

        // Case A (default OFF): Phone triggers On, then disconnects mid-toggle while host is in `.starting`
        client.requestSensorPower(turnOn: true)
        let enteredStartingA = await waitUntil { host.sensorPowerState == .starting }
        XCTAssertTrue(enteredStartingA)

        client.stop()
        host.disconnectActivePhoneClientForTesting()

        // Host finishes its transition and keeps sensor On despite phone disconnect
        let finishedOnAfterDisconnect = await waitUntil {
            host.isRunning && host.sensorPowerState == .on
        }
        XCTAssertTrue(finishedOnAfterDisconnect, "With auto-off setting disabled (default), mid-toggle disconnect keeps requested state")

        // Reconnect phone, turn Off, then enable "Turn the Kinect off when the iPhone disconnects"
        client.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let reconnected = await waitUntil { client.telemetry.isConnected && client.telemetry.hasConfirmedHostState }
        XCTAssertTrue(reconnected)

        host.stopStream()
        XCTAssertEqual(host.sensorPowerState, .off)
        host.turnOffKinectOnPhoneDisconnect = true

        // Case B (setting ON): Phone triggers On, enters `.starting`, then disconnects mid-toggle -> cancels & stays Off!
        client.requestSensorPower(turnOn: true)
        let enteredStartingB = await waitUntil { host.sensorPowerState == .starting }
        XCTAssertTrue(enteredStartingB)

        client.stop()
        host.disconnectActivePhoneClientForTesting()
        try? await Task.sleep(nanoseconds: 220_000_000)

        XCTAssertFalse(host.isRunning, "With auto-off on disconnect enabled, mid-toggle disconnect must leave sensor Off")
        XCTAssertEqual(host.sensorPowerState, .off)
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path))
    }

    func testTwoTogglesSentQuicklyInARowSettleDeterministically() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase5-rapid-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let lockURL = tempRoot.appendingPathComponent("KINECT-IN-USE-test")
        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: lockURL,
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }
        host.simulatedPowerOnDelayMs = 40

        let client = try await pairAndConnectLoopbackClient(to: host)
        defer { client.stop() }

        // Burst 1: On -> Off sent back-to-back with zero delay; must settle cleanly on Off
        client.requestSensorPower(turnOn: true)
        client.requestSensorPower(turnOn: false)
        try? await Task.sleep(nanoseconds: 140_000_000)

        let settledOff = await waitUntil {
            !host.isRunning &&
            host.sensorPowerState == .off &&
            client.telemetry.hostState.powerState == .off
        }
        XCTAssertTrue(settledOff)
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path))

        // Burst 2: Off -> On -> Off -> On sent back-to-back; must settle cleanly on On
        client.requestSensorPower(turnOn: false)
        client.requestSensorPower(turnOn: true)
        client.requestSensorPower(turnOn: false)
        client.requestSensorPower(turnOn: true)

        let settledOn = await waitUntil {
            host.isRunning &&
            host.sensorPowerState == .on &&
            client.telemetry.hostState.powerState == .on
        }
        XCTAssertTrue(settledOn)
        XCTAssertTrue(FileManager.default.fileExists(atPath: lockURL.path))
    }

    func testAdditiveWireProtocolUnknownFrameTolerance3DOrbitAndRemoteControls() async throws {
        // 1. Verify WireFrameReader skips unknown future message types cleanly
        let reader = WireFrameReader()
        let futurePacket = WirePacket.packRawType(0x7E, payload: Data("{\"future\":true}".utf8))
        let validPing = try WirePacket.packJSON(
            type: .ping,
            PingMessage(clientSendNs: 42, lastDecodedSeq: 1, lastHostToDisplayLatencyUs: 1000, lastBarcodeLatencyUs: 0)
        )
        reader.append(futurePacket)
        reader.append(validPing)

        if case .unknownFrame(let rawType, _, _) = reader.nextFrame() {
            XCTAssertEqual(rawType, 0x7E)
        } else {
            XCTFail("Expected WireFrameReader to classify unrecognized 0x7E message as .unknownFrame")
        }
        if case .frame(let msgType, _, _) = reader.nextFrame() {
            XCTAssertEqual(msgType, .ping)
        } else {
            XCTFail("Expected valid Ping frame immediately after skipped unknown frame")
        }

        // 2. Verify remote 3D orbit, mode switch, cut-off "Track me", and host freeze
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase5-controls-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }

        let client = try await pairAndConnectLoopbackClient(to: host)
        defer { client.stop() }

        host.startStream()
        let running = await waitUntil { host.isRunning && host.previewImage != nil }
        XCTAssertTrue(running)

        // Switch to Mode 5 (3D Cloud) from phone and orbit/zoom/pan 3D camera
        client.requestModeSwitch(modeRaw: ViewerModeOption.pointCloud3D.rawValue)
        let switchedTo3D = await waitUntil {
            host.selectedMode == .pointCloud3D &&
            client.telemetry.currentModeRaw == ViewerModeOption.pointCloud3D.rawValue
        }
        XCTAssertTrue(switchedTo3D)

        let initialYaw = host.pointCloudCamera.yawRad
        let initialDistance = host.pointCloudCamera.distanceMeters
        client.sendOrbit3DGesture(Orbit3DGestureMessage(
            deltaYawRadians: 0.35,
            deltaPitchRadians: -0.15,
            zoomScale: 0.80,
            panRightMeters: 0.12,
            panUpMeters: -0.05,
            resetView: false
        ))
        let orbited = await waitUntil {
            abs(host.pointCloudCamera.yawRad - (initialYaw + 0.35)) < 0.001 &&
            host.pointCloudCamera.distanceMeters < initialDistance
        }
        XCTAssertTrue(orbited)

        // Apply "Track me" cut-off preset remotely
        client.sendRemoteControl(RemoteControlCommandMessage(
            action: .setCutoff,
            cutoffEnabled: true,
            applyTrackMePreset: true
        ))
        let cutoffSynced = await waitUntil {
            host.cutoffEnabled &&
            host.cutoffNearMm == 500.0 &&
            host.cutoffFarMm == 2000.0 &&
            client.telemetry.hostState.cutoffEnabled
        }
        XCTAssertTrue(cutoffSynced)

        // Freeze host view remotely
        client.sendRemoteControl(RemoteControlCommandMessage(
            action: .setHostFrozen,
            hostFrozen: true
        ))
        let hostFrozenSynced = await waitUntil {
            host.isFrozen && client.telemetry.hostState.isHostFrozen
        }
        XCTAssertTrue(hostFrozenSynced)

        // Verify connection quality evaluator
        XCTAssertEqual(
            PocketConnectionQuality.evaluate(
                isConnected: true,
                powerState: .on,
                modeRaw: 0,
                fps: 29.8,
                delayMs: 38.0,
                rttMs: 6.0,
                recentSeqDrops: 0
            ),
            .excellent
        )
        XCTAssertEqual(
            PocketConnectionQuality.evaluate(
                isConnected: false,
                powerState: .off,
                modeRaw: 0,
                fps: 0,
                delayMs: 0,
                rttMs: 0,
                recentSeqDrops: 0
            ),
            .offline
        )
    }
}
