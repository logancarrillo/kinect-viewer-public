import CryptoKit
import Foundation
import Security
import XCTest
@testable import KinectViewer

/// In-memory simulation of `SecItemCopyMatching` / `SecItemUpdate` / `SecItemAdd` / `SecItemDelete`
/// so tests exercise `KeychainHostIdentityStore` end-to-end without touching the real macOS Keychain.
private final class MockKeychainBackend: @unchecked Sendable {
    private let lock = NSLock()
    var storedData: Data?
    var forcedCopyMatchingStatus: OSStatus?
    var forcedUpdateStatus: OSStatus?
    var forcedAddStatus: OSStatus?

    private(set) var copyMatchingCalls = 0
    private(set) var updateCalls = 0
    private(set) var addCalls = 0
    private(set) var deleteCalls = 0

    init(initialData: Data? = nil) {
        self.storedData = initialData
    }

    var primitives: KeychainPrimitives {
        KeychainPrimitives(
            copyMatching: { [weak self] _, outPtr in
                guard let self = self else { return errSecInternalError }
                self.lock.lock()
                defer { self.lock.unlock() }
                self.copyMatchingCalls += 1
                if let forced = self.forcedCopyMatchingStatus {
                    return forced
                }
                guard let data = self.storedData else {
                    return errSecItemNotFound
                }
                outPtr?.pointee = data as CFData
                return errSecSuccess
            },
            update: { [weak self] _, attrs in
                guard let self = self else { return errSecInternalError }
                self.lock.lock()
                defer { self.lock.unlock() }
                self.updateCalls += 1
                if let forced = self.forcedUpdateStatus {
                    return forced
                }
                guard self.storedData != nil else {
                    return errSecItemNotFound
                }
                let dict = attrs as NSDictionary
                if let newData = dict[kSecValueData] as? Data {
                    self.storedData = newData
                    return errSecSuccess
                }
                return errSecParam
            },
            add: { [weak self] query, _ in
                guard let self = self else { return errSecInternalError }
                self.lock.lock()
                defer { self.lock.unlock() }
                self.addCalls += 1
                if let forced = self.forcedAddStatus {
                    return forced
                }
                if self.storedData != nil {
                    return errSecDuplicateItem
                }
                let dict = query as NSDictionary
                if let newData = dict[kSecValueData] as? Data {
                    self.storedData = newData
                    return errSecSuccess
                }
                return errSecParam
            },
            delete: { [weak self] _ in
                guard let self = self else { return errSecInternalError }
                self.lock.lock()
                defer { self.lock.unlock() }
                self.deleteCalls += 1
                if self.storedData == nil {
                    return errSecItemNotFound
                }
                self.storedData = nil
                return errSecSuccess
            }
        )
    }
}

private final class SpyIdentityStore: HostIdentityKeyStore, @unchecked Sendable {
    private let lock = NSLock()
    var nextLoadResult: HostIdentityLoadResult
    var saveResult: Bool = true
    private(set) var savedRecords: [PersistedIdentityRecord] = []
    private(set) var wipeCalls = 0

    init(loadResult: HostIdentityLoadResult) {
        self.nextLoadResult = loadResult
    }

    func load() -> HostIdentityLoadResult {
        lock.lock()
        defer { lock.unlock() }
        return nextLoadResult
    }

    @discardableResult
    func save(_ record: PersistedIdentityRecord) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard saveResult else { return false }
        savedRecords.append(record)
        nextLoadResult = .loaded(record)
        return true
    }

    func wipe() {
        lock.lock()
        wipeCalls += 1
        nextLoadResult = .notFound
        lock.unlock()
    }
}

private final class Phase8SyncBox<T>: @unchecked Sendable {
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

/// Exact copy of `main`'s original 4-field `PingMessage` before `protocolVersion` was added in Phase 7.
private struct OriginalMainV1PingMessage: Codable, Sendable, Equatable {
    let clientSendNs: UInt64
    let lastDecodedSeq: UInt64
    let lastHostToDisplayLatencyUs: UInt32
    let lastBarcodeLatencyUs: UInt32
}

/// Pre-Phase-8 on-disk `.host-state/pairing.json` layout used to verify first-launch Keychain migration.
private struct LegacyFileWithPrivateKeyState: Codable {
    var serverIdentity: PersistedIdentityRecord
    var serviceId: String
    var pinnedClientCertSHA256: Data?
    var allowPhoneModeSwitch: Bool
    var allowPhoneSensorPowerControl: Bool
    var turnOffKinectOnPhoneDisconnect: Bool
}

@MainActor
final class PlaybackWakeAndKeychainIdentityTests: XCTestCase {

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

    // MARK: - Next-chunk prefetch never blocks on-demand playback + `-fps_mode passthrough`

    func testNextChunkPrefetchRunsOffDecodeQueueNeverBlocksOnDemandFrameAndUsesPassthroughFpsMode() async throws {
        // 1. Verify `-fps_mode passthrough` is present on FFV1 decode arguments (both chunk 0 and seeked chunks)
        let argsChunk0 = RecordingSessionReader.makeFfv1DecodeArguments(
            mkvPath: "/tmp/example.mkv",
            startFrame: 0,
            frameCount: 180
        )
        let argsChunk2 = RecordingSessionReader.makeFfv1DecodeArguments(
            mkvPath: "/tmp/example.mkv",
            startFrame: 360,
            frameCount: 180
        )
        for args in [argsChunk0, argsChunk2] {
            let idx = try XCTUnwrap(args.firstIndex(of: "-fps_mode"))
            XCTAssertLessThan(idx + 1, args.count)
            XCTAssertEqual(args[idx + 1], "passthrough")
        }
        XCTAssertTrue(argsChunk2.contains("-ss"))

        // 2. Verify slow background prefetch of chunk 1 never delays an on-demand frame from chunk 0
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-prefetch-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("rec", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let recorder = HostRecorder(baseDirectoryURL: recDir)
        _ = try recorder.startRecording(
            source: KV_SOURCE_SYNTHETIC,
            mode: KV_MODE_DEPTH,
            width: 512,
            height: 424,
            losslessOption: .ffv1Mkv,
            irIntrinsics: nil
        )
        for f in 0..<24 {
            while f > 6 && recorder.currentStatus.framesWritten + 6 < UInt64(f) {
                try? await Task.sleep(nanoseconds: 5_000_000)
            }
            let val = UInt16(1500 + f * 41)
            let px = [UInt16](repeating: val, count: 512 * 424)
            recorder.appendRaw16Frame(
                pixelsU16: px,
                width: 512,
                height: 424,
                seq: UInt64(f + 1),
                deviceTicks: UInt32(f * 267),
                arrivalNs: UInt64(1_000_000_000 + f * 33_333_333),
                source: KV_SOURCE_SYNTHETIC
            )
        }
        _ = recorder.stopRecording()

        let session = try XCTUnwrap(RecordingLibrary.listSessions(in: recDir).first)
        let reader = RecordingSessionReader(session: session, chunkSize: 10)
        reader.simulatedPrefetchDelayMsForTesting = 350

        // Prime chunk 0 into cachedChunkFrames
        let frame0: [UInt16]? = await withCheckedContinuation { cont in
            reader.readFrame16Async(at: 0, prefetchNextChunk: false) { cont.resume(returning: $0) }
        }
        XCTAssertEqual(frame0?.first, 1500)

        // Frame 5 (>= chunkSize / 2) triggers chunk-1 prefetch (which sleeps 350 ms on prefetchQueue)
        let frame5: [UInt16]? = await withCheckedContinuation { cont in
            reader.readFrame16Async(at: 5, prefetchNextChunk: true) { cont.resume(returning: $0) }
        }
        XCTAssertEqual(frame5?.first, UInt16(1500 + 5 * 41))

        // Immediately request on-demand frame 6 while prefetchQueue is in the middle of its 350 ms work!
        let t0 = kv_monotonic_ns()
        let frame6: [UInt16]? = await withCheckedContinuation { cont in
            reader.readFrame16Async(at: 6, prefetchNextChunk: false) { cont.resume(returning: $0) }
        }
        let onDemandMs = Double(kv_monotonic_ns() - t0) / 1e6
        XCTAssertEqual(frame6?.first, UInt16(1500 + 6 * 41))
        XCTAssertLessThan(onDemandMs, 120.0, "On-demand frame 6 must never wait behind the 350 ms background prefetch (took \(onDemandMs) ms)")

        // Wait for background prefetch of chunk 1 to finish, then confirm frame 12 hits the prefetched chunk cleanly
        await reader.waitForIdleForTesting()
        let tHit0 = kv_monotonic_ns()
        let frame12: [UInt16]? = await withCheckedContinuation { cont in
            reader.readFrame16Async(at: 12, prefetchNextChunk: false) { cont.resume(returning: $0) }
        }
        let hitMs = Double(kv_monotonic_ns() - tHit0) / 1e6
        XCTAssertEqual(frame12?.first, UInt16(1500 + 12 * 41))
        XCTAssertLessThan(hitMs, 50.0, "Prefetched chunk 1 should be served from memory without spawning ffmpeg again")
        await reader.waitForIdleForTesting()
    }

    // MARK: - Off during the wake reopen settle delay wins

    func testPowerOffDuringWakeReopenDelayWinsAndSkipsSensorRestart() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-wakeoff-\(UUID().uuidString)", isDirectory: true)
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

        host.openPairingWindow()
        let offer = try XCTUnwrap(host.activePairingOffer)
        let certHash = try XCTUnwrap(host.hostServerCertSHA256)
        let loopbackURI = makeLoopbackQRURI(from: offer, serverCertSHA256: certHash, serviceId: "wakeoff")

        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence)
        defer { client.stop() }
        let pairExp = expectation(description: "Paired")
        client.pairWithQRURI(loopbackURI) { _ in pairExp.fulfill() }
        await fulfillment(of: [pairExp], timeout: 5.0)

        client.connectDirect(host: "127.0.0.1", port: host.activeStreamPort)
        let connected = await waitUntil { client.telemetry.isConnected && client.telemetry.hasConfirmedHostState }
        XCTAssertTrue(connected)

        // 1. Start stream, sleep Mac, start 220 ms wake reopen delay, then send Off from phone at 40 ms
        host.useSyntheticSource = true
        host.startStream()
        let streaming = await waitUntil { host.isRunning && client.telemetry.hostState.powerState == .on }
        XCTAssertTrue(streaming)

        host.handleSystemWillSleep()
        XCTAssertFalse(host.isRunning)
        XCTAssertEqual(host.sensorPowerState, .off)

        host.handleSystemDidWake(settleDelayNs: 220_000_000)
        try? await Task.sleep(nanoseconds: 40_000_000)

        // Off arrives from phone during the wake reopen delay
        client.requestSensorPower(turnOn: false)
        try? await Task.sleep(nanoseconds: 300_000_000)

        XCTAssertFalse(host.isRunning, "Off arriving during wake settle delay must cancel wake reopen")
        XCTAssertEqual(host.sensorPowerState, .off)
        XCTAssertEqual(client.telemetry.hostState.powerState, .off)

        // 2. Also verify if USB is absent on wake, an Off during the wake delay keeps state .off (never .error)
        host.useSyntheticSource = true
        host.startStream()
        XCTAssertTrue(host.isRunning)
        host.handleSystemWillSleep()
        host.useSyntheticSource = false
        host.simulatedUsbPresentOverrideForTesting = false

        host.handleSystemDidWake(settleDelayNs: 200_000_000)
        try? await Task.sleep(nanoseconds: 30_000_000)
        host.requestSensorPower(turnOn: false)
        try? await Task.sleep(nanoseconds: 260_000_000)

        XCTAssertFalse(host.isRunning)
        XCTAssertEqual(host.sensorPowerState, .off, "Cancelled wake task must not overwrite Off with wake USB error")
    }

    // MARK: - An old Mac decodes the v2 ping using main's original 4-field PingMessage

    func testOriginalMainFourFieldPingMessageDecodesV2PingWithProtocolVersion() throws {
        let v2Ping = PingMessage(
            clientSendNs: 987_654_321_000,
            lastDecodedSeq: 420,
            lastHostToDisplayLatencyUs: 18_500,
            lastBarcodeLatencyUs: 21_250,
            protocolVersion: 2
        )
        let packed = try WirePacket.packJSON(type: .ping, v2Ping)
        let reader = WireFrameReader()
        reader.append(packed)

        guard case .frame(let msgType, _, let payload) = reader.nextFrame() else {
            XCTFail("Expected valid Ping wire frame")
            return
        }
        XCTAssertEqual(msgType, .ping)

        let rawJSON = String(decoding: payload, as: UTF8.self)
        XCTAssertTrue(rawJSON.contains("\"protocolVersion\":2"), "v2 ping JSON must carry protocolVersion: 2")

        let decodedByOldMac = try JSONDecoder().decode(OriginalMainV1PingMessage.self, from: payload)
        XCTAssertEqual(
            decodedByOldMac,
            OriginalMainV1PingMessage(
                clientSendNs: 987_654_321_000,
                lastDecodedSeq: 420,
                lastHostToDisplayLatencyUs: 18_500,
                lastBarcodeLatencyUs: 21_250
            )
        )
    }

    // MARK: - Symlinked `.nosync` folders pointing outside recordings folder are skipped

    func testSymlinkedNosyncFoldersPointingOutsideRecordingsDirAreSkippedWhenListingAndPurging() throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-symlinks-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("recordings", isDirectory: true)
        let outsideRoot = tempRoot.appendingPathComponent("outside", isDirectory: true)
        let outsideExpiredIrDir = outsideRoot.appendingPathComponent("external_ir16", isDirectory: true)
        let outsideDisallowedDir = outsideRoot.appendingPathComponent("external_other", isDirectory: true)

        try FileManager.default.createDirectory(at: recDir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: outsideExpiredIrDir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: outsideDisallowedDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        // Populate outside targets (one looks like an expired 10-day-old IR session; one looks like a disallowed colour folder)
        let outsideCsv = outsideExpiredIrDir.appendingPathComponent("ir16_timestamps.csv")
        try "frame_index,engine_seq,device_ticks,arrival_mach_ns\n0,1,0,1000\n".write(to: outsideCsv, atomically: true, encoding: .utf8)
        let outsideSecret = outsideDisallowedDir.appendingPathComponent("keep_me.dat")
        try Data(repeating: 0x5A, count: 512).write(to: outsideSecret)

        let now = Date()
        let df = DateFormatter()
        df.locale = Locale(identifier: "en_US_POSIX")
        df.dateFormat = "yyyyMMdd-HHmmss-SSS"
        let tenDaysAgoPrefix = df.string(from: now.addingTimeInterval(-10.0 * 86_400.0))
        let freshPrefix = df.string(from: now.addingTimeInterval(-60.0))

        // Create symlinks inside recDir pointing outside recDir
        let symlinkExpiredIrURL = recDir.appendingPathComponent("\(tenDaysAgoPrefix)_ir16.nosync")
        let symlinkDisallowedURL = recDir.appendingPathComponent("\(tenDaysAgoPrefix)_color1080p.nosync")
        try FileManager.default.createSymbolicLink(at: symlinkExpiredIrURL, withDestinationURL: outsideExpiredIrDir)
        try FileManager.default.createSymbolicLink(at: symlinkDisallowedURL, withDestinationURL: outsideDisallowedDir)

        // Create one real non-symlink IR directory inside recDir
        let realIrURL = recDir.appendingPathComponent("\(freshPrefix)_ir16.nosync", isDirectory: true)
        try FileManager.default.createDirectory(at: realIrURL, withIntermediateDirectories: true)
        try "frame_index,engine_seq,device_ticks,arrival_mach_ns\n0,1,0,1000\n"
            .write(to: realIrURL.appendingPathComponent("ir16_timestamps.csv"), atomically: true, encoding: .utf8)

        // 1. Listing skips both symlinked `.nosync` entries and returns only the real session
        let listed = RecordingLibrary.listSessions(in: recDir)
        XCTAssertEqual(listed.map(\.folderName), [realIrURL.lastPathComponent])

        // 2. Purging disallowed & expired sessions skips both symlinked `.nosync` entries without touching symlinks or outside targets
        let purgedDisallowed = RecordingLibrary.purgeDisallowedSessions(in: recDir, now: now)
        XCTAssertTrue(purgedDisallowed.isEmpty)
        let purgedExpired = RecordingLibrary.purgeExpiredSessions(in: recDir, now: now)
        XCTAssertTrue(purgedExpired.isEmpty)

        // 3. Launching HostController also leaves symlinks and outside directories completely untouched
        let host = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { host.shutdown() }
        XCTAssertEqual(host.recordedSessions.map(\.folderName), [realIrURL.lastPathComponent])

        XCTAssertTrue(FileManager.default.fileExists(atPath: outsideCsv.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: outsideSecret.path))
        let linkAttrs1 = try FileManager.default.attributesOfItem(atPath: symlinkExpiredIrURL.path)
        let linkAttrs2 = try FileManager.default.attributesOfItem(atPath: symlinkDisallowedURL.path)
        XCTAssertEqual(linkAttrs1[.type] as? FileAttributeType, .typeSymbolicLink)
        XCTAssertEqual(linkAttrs2[.type] as? FileAttributeType, .typeSymbolicLink)
        XCTAssertFalse(FileManager.default.fileExists(atPath: recDir.appendingPathComponent(RecordingLibrary.deletionLogFileName).path))
    }

    // MARK: - Test-mode recordings refuse the real-sensor source

    func testXCTestRefusesRealSensorSourceInRecorderControllerAndCoreEngine() throws {
        XCTAssertNotNil(
            ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"],
            "XCTestConfigurationFilePath must be set while running unit tests"
        )

        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-realrefuse-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("rec", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        // 1. HostRecorder refuses KV_SOURCE_REAL in XCTest before creating any session directory
        let recorder = HostRecorder(baseDirectoryURL: recDir)
        XCTAssertTrue(recorder.isRunningXCTest)
        XCTAssertThrowsError(
            try recorder.startRecording(
                source: KV_SOURCE_REAL,
                mode: KV_MODE_DEPTH,
                width: 512,
                height: 424,
                losslessOption: .png16Sequence
            )
        ) { error in
            XCTAssertEqual(error as? HostRecorderError, .realSensorForbiddenInTestMode)
        }
        XCTAssertFalse(recorder.currentStatus.isRecording)
        let sessionFoldersAfterRealAttempt = ((try? FileManager.default.contentsOfDirectory(atPath: recDir.path)) ?? [])
            .filter { $0.hasSuffix(".nosync") && $0 != ".nosync" }
        XCTAssertTrue(sessionFoldersAfterRealAttempt.isEmpty, "Zero session folders may be created when KV_SOURCE_REAL is refused")

        // Synthetic source still records normally in XCTest
        let synthDir = try recorder.startRecording(
            source: KV_SOURCE_SYNTHETIC,
            mode: KV_MODE_IR,
            width: 512,
            height: 424,
            losslessOption: .png16Sequence
        )
        // Even if someone passes KV_SOURCE_REAL to appendRaw16Frame mid-session, it is dropped in XCTest
        recorder.appendRaw16Frame(
            pixelsU16: [UInt16](repeating: 2000, count: 512 * 424),
            width: 512,
            height: 424,
            seq: 1,
            deviceTicks: 0,
            arrivalNs: 1_000_000,
            source: KV_SOURCE_REAL
        )
        let stoppedSnap = recorder.stopRecording()
        XCTAssertEqual(stoppedSnap.framesWritten, 0)
        try? FileManager.default.removeItem(at: synthDir)

        // 2. HostController refuses starting stream or recording when useSyntheticSource == false in XCTest
        let lockURL = tempRoot.appendingPathComponent("KINECT-IN-USE-test")
        let host = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            lockFileURL: lockURL,
            enableStreamingServer: false,
            autostart: false
        )
        defer { host.shutdown() }
        host.useSyntheticSource = false
        host.startStream()
        XCTAssertFalse(host.isRunning)
        XCTAssertEqual(host.sensorPowerState, .error)
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path))

        // 3. C++ core engine also refuses KV_SOURCE_REAL when XCTestConfigurationFilePath is set
        let eng = try XCTUnwrap(kv_engine_create(lockURL.path))
        defer { kv_engine_destroy(eng) }
        var params = kv_display_params_t()
        kv_default_display_params(&params)
        var errBuf = [CChar](repeating: 0, count: 256)
        let startedReal = kv_engine_start_ex(eng, KV_SOURCE_REAL, KV_PIPELINE_CPU, &params, false, &errBuf, errBuf.count)
        XCTAssertFalse(startedReal)
        XCTAssertFalse(kv_engine_is_running(eng))
        XCTAssertFalse(FileManager.default.fileExists(atPath: lockURL.path))
    }

    // MARK: - Pairing private key lives in Keychain only, migrates existing file, and CLI uses temp folder

    func testPairingPrivateKeyLivesInKeychainOnlyMigratesLegacyFileAndCLIKeepsThrowawayKeysInTempDir() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-keychain-\(UUID().uuidString)", isDirectory: true)
        let stateDir = tempRoot.appendingPathComponent("state", isDirectory: true)
        try FileManager.default.createDirectory(at: stateDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let mockKeychain = MockKeychainBackend()
        let keychainStore = KeychainHostIdentityStore(
            service: "com.example.kinectviewer.host.pairing.mock",
            account: "server-identity-v1",
            primitives: mockKeychain.primitives
        )
        if case .notFound = keychainStore.load() {} else {
            XCTFail("Expected empty mock keychain to report .notFound")
        }

        // 1. Seed a pre-Phase-8 `pairing.json` containing a private key + already-paired iPhone client pin
        let legacyHostIdentity = try CryptoIdentity.generateNew(commonName: "kv2-host")
        let pairedClientIdentity = try CryptoIdentity.generateNew(commonName: "kv2-pocket")
        let legacyState = LegacyFileWithPrivateKeyState(
            serverIdentity: legacyHostIdentity.persistedRecord,
            serviceId: "a1b2c3d4e5f6",
            pinnedClientCertSHA256: pairedClientIdentity.certificateSHA256,
            allowPhoneModeSwitch: true,
            allowPhoneSensorPowerControl: false,
            turnOffKinectOnPhoneDisconnect: true
        )
        let pairingFileURL = stateDir.appendingPathComponent("pairing.json")
        try JSONEncoder().encode(legacyState).write(to: pairingFileURL, options: .atomic)

        let rawKeyBase64 = legacyHostIdentity.persistedRecord.privateKeyRaw.base64EncodedString()
        let rawKeyJSONEscaped = rawKeyBase64.replacingOccurrences(of: "/", with: "\\/")
        let beforeMigrationJSON = try String(contentsOf: pairingFileURL, encoding: .utf8)
        XCTAssertTrue(beforeMigrationJSON.contains("privateKeyRaw"))
        XCTAssertTrue(beforeMigrationJSON.contains(rawKeyBase64) || beforeMigrationJSON.contains(rawKeyJSONEscaped))

        // 2. First launch migrates the private key into the Keychain store and deletes it from `pairing.json`
        var server: HostStreamServer? = try HostStreamServer(stateDirectoryURL: stateDir, identityStore: keychainStore)
        defer { server?.shutdown() }

        guard case .loaded(let migratedKeychainRecord) = keychainStore.load() else {
            XCTFail("Private key must be migrated into Keychain store")
            return
        }
        XCTAssertEqual(migratedKeychainRecord.privateKeyRaw, legacyHostIdentity.persistedRecord.privateKeyRaw)
        XCTAssertEqual(migratedKeychainRecord.certificateDER, legacyHostIdentity.persistedRecord.certificateDER)
        XCTAssertEqual(mockKeychain.deleteCalls, 0, "Normal migration save must never call SecItemDelete")

        let afterMigrationData = try Data(contentsOf: pairingFileURL)
        let afterMigrationJSON = try XCTUnwrap(String(data: afterMigrationData, encoding: .utf8))
        XCTAssertFalse(afterMigrationJSON.contains("serverIdentity"), "serverIdentity must be deleted from pairing.json")
        XCTAssertFalse(afterMigrationJSON.contains("privateKeyRaw"), "privateKeyRaw key name must be absent from pairing.json")
        XCTAssertFalse(afterMigrationJSON.contains(rawKeyBase64), "Raw private key scalar bytes must be absent from pairing.json")
        XCTAssertFalse(afterMigrationJSON.contains(rawKeyJSONEscaped), "JSON-escaped private key scalar bytes must be absent from pairing.json")
        XCTAssertTrue(afterMigrationJSON.contains("serverCertSHA256"), "Fingerprint must remain in pairing.json")
        XCTAssertTrue(afterMigrationJSON.contains("serverPublicKeyDER"), "Public key DER must remain in pairing.json")

        XCTAssertEqual(server?.serverCertSHA256, legacyHostIdentity.certificateSHA256)
        XCTAssertEqual(server?.serviceId, "a1b2c3d4e5f6")
        XCTAssertEqual(server?.isPhonePaired, true)
        XCTAssertEqual(server?.allowPhoneSensorPowerControl, false)
        XCTAssertEqual(server?.turnOffKinectOnPhoneDisconnect, true)

        // 3. Verify the previously paired iPhone client connects over mTLS 1.3 without re-pairing,
        //    both immediately after migration and after a full server restart loading from Keychain!
        server?.onClientConnectedNeedsKeyframe = {
            HelloServerMessage(
                protocolVersion: 2,
                modeRaw: 0,
                sbsLayoutRaw: 0,
                allowPhoneModeSwitch: true,
                width: 1024,
                height: 848,
                codecRaw: WireCodecKind.h264.rawValue
            )
        }
        server?.onClientConnectedInitialState = { .unconfirmedInitial }

        let clientPersistence = InMemoryPersistenceProvider(initial: ClientPairingState(
            clientIdentity: pairedClientIdentity.persistedRecord,
            serviceId: "a1b2c3d4e5f6",
            pinnedServerCertSHA256: legacyHostIdentity.certificateSHA256,
            lastKnownHost: "127.0.0.1",
            lastKnownStreamPort: server?.streamPort
        ))
        let client = try PocketStreamClientCore(persistence: clientPersistence)
        defer { client.stop() }

        client.connectDirect(host: "127.0.0.1", port: try XCTUnwrap(server?.streamPort))
        let connected1 = await waitUntil { client.telemetry.isConnected && server?.pairedClientConnected == true }
        XCTAssertTrue(connected1, "Existing paired iPhone must connect seamlessly after Keychain migration")
        client.stop()

        // Restart HostStreamServer and confirm it loads the migrated key from Keychain
        server?.shutdown()
        server = nil
        let restarted = try HostStreamServer(stateDirectoryURL: stateDir, identityStore: keychainStore)
        server = restarted
        restarted.onClientConnectedNeedsKeyframe = {
            HelloServerMessage(
                protocolVersion: 2,
                modeRaw: 0,
                sbsLayoutRaw: 0,
                allowPhoneModeSwitch: true,
                width: 1024,
                height: 848,
                codecRaw: WireCodecKind.h264.rawValue
            )
        }
        restarted.onClientConnectedInitialState = { .unconfirmedInitial }

        client.connectDirect(host: "127.0.0.1", port: restarted.streamPort)
        let connected2 = await waitUntil { client.telemetry.isConnected && restarted.pairedClientConnected }
        XCTAssertTrue(connected2, "Restarted HostStreamServer must load private key from Keychain and accept paired iPhone")

        // 4. Verify CLI TemporaryDirectoryHostIdentityStore refuses non-temp directories and works inside $TMPDIR
        let nonTempURL = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
            .appendingPathComponent("kinect-viewer/not-a-temp-dir", isDirectory: true)
        XCTAssertThrowsError(try TemporaryDirectoryHostIdentityStore(directoryURL: nonTempURL))

        let cliTempStateDir = tempRoot.appendingPathComponent("cli-state", isDirectory: true)
        let cliTempKeysDir = tempRoot.appendingPathComponent("cli-keys", isDirectory: true)
        let cliKeyStore = try TemporaryDirectoryHostIdentityStore(directoryURL: cliTempKeysDir)
        let cliServer = try HostStreamServer(stateDirectoryURL: cliTempStateDir, identityStore: cliKeyStore)
        defer { cliServer.shutdown() }

        let cliPairingJSON = try String(contentsOf: cliTempStateDir.appendingPathComponent("pairing.json"), encoding: .utf8)
        XCTAssertFalse(cliPairingJSON.contains("privateKeyRaw"), "CLI pairing.json must also omit privateKeyRaw")
        if case .loaded = cliKeyStore.load() {} else {
            XCTFail("Throwaway CLI key must live in the temporary keys folder")
        }
    }

    // MARK: - Keychain read/write errors never regenerate identity or unpair iPhone

    func testKeychainFailureModesNeverDestroyExistingPairingOrDeleteKeys() throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-keychain-safety-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        // 1a. True first run (notFound + no pairing.json) -> generates a new identity
        let firstRunDirA = tempRoot.appendingPathComponent("firstrun-no-file", isDirectory: true)
        let spyFirstRunA = SpyIdentityStore(loadResult: .notFound)
        let serverFirstA = try HostStreamServer(stateDirectoryURL: firstRunDirA, identityStore: spyFirstRunA)
        defer { serverFirstA.shutdown() }
        XCTAssertEqual(spyFirstRunA.savedRecords.count, 1)
        XCTAssertEqual(spyFirstRunA.wipeCalls, 0)

        // 1b. True first run (notFound + pairing.json present without serverCertSHA256) -> generates a new identity while keeping serviceId/settings
        let firstRunDirB = tempRoot.appendingPathComponent("firstrun-no-cert", isDirectory: true)
        try FileManager.default.createDirectory(at: firstRunDirB, withIntermediateDirectories: true)
        try Data("{\"serviceId\":\"112233445566\",\"allowPhoneModeSwitch\":false}".utf8)
            .write(to: firstRunDirB.appendingPathComponent("pairing.json"), options: .atomic)
        let spyFirstRunB = SpyIdentityStore(loadResult: .notFound)
        let serverFirstB = try HostStreamServer(stateDirectoryURL: firstRunDirB, identityStore: spyFirstRunB)
        defer { serverFirstB.shutdown() }
        XCTAssertEqual(spyFirstRunB.savedRecords.count, 1)
        XCTAssertEqual(serverFirstB.serviceId, "112233445566")
        XCTAssertFalse(serverFirstB.allowPhoneModeSwitch)

        // Prepare an established paired state (`pairing.json` with serverCertSHA256 + pinnedClientCertSHA256)
        let pairedDir = tempRoot.appendingPathComponent("paired-state", isDirectory: true)
        try FileManager.default.createDirectory(at: pairedDir, withIntermediateDirectories: true)
        let existingHostIdent = try CryptoIdentity.generateNew(commonName: "kv2-host")
        let existingPhoneIdent = try CryptoIdentity.generateNew(commonName: "kv2-pocket")
        let existingState = HostPersistedState(
            serverCertSHA256: existingHostIdent.certificateSHA256,
            serverPublicKeyDER: existingHostIdent.privateKey.publicKey.derRepresentation,
            serviceId: "deadbeefcafe",
            pinnedClientCertSHA256: existingPhoneIdent.certificateSHA256,
            allowPhoneModeSwitch: true,
            allowPhoneSensorPowerControl: true,
            turnOffKinectOnPhoneDisconnect: false
        )
        let pairedFileURL = pairedDir.appendingPathComponent("pairing.json")
        try JSONEncoder().encode(existingState).write(to: pairedFileURL, options: .atomic)
        let originalFileBytes = try Data(contentsOf: pairedFileURL)

        // 2. Access denied / locked / interaction not allowed -> throws clear error, no new identity, pairing.json byte-identical, pinned phone kept
        for deniedStatus in [errSecAuthFailed, errSecInteractionNotAllowed] {
            let mockDenied = MockKeychainBackend(initialData: try JSONEncoder().encode(existingHostIdent.persistedRecord))
            mockDenied.forcedCopyMatchingStatus = deniedStatus
            let deniedStore = KeychainHostIdentityStore(
                service: "com.example.kinectviewer.host.pairing.mock",
                account: "server-identity-v1",
                primitives: mockDenied.primitives
            )
            if case .accessDeniedOrLocked(let st) = deniedStore.load() {
                XCTAssertEqual(st, deniedStatus)
            } else {
                XCTFail("Expected .accessDeniedOrLocked(\(deniedStatus))")
            }

            XCTAssertThrowsError(try HostStreamServer(stateDirectoryURL: pairedDir, identityStore: deniedStore)) { err in
                XCTAssertEqual(err as? HostPairingKeyError, .keychainAccessRequired(deniedStatus))
                XCTAssertEqual(err.localizedDescription, "Allow Keychain access to keep your iPhone paired.")
            }
            XCTAssertEqual(mockDenied.updateCalls, 0)
            XCTAssertEqual(mockDenied.addCalls, 0)
            XCTAssertEqual(mockDenied.deleteCalls, 0)
            XCTAssertEqual(try Data(contentsOf: pairedFileURL), originalFileBytes, "pairing.json must stay byte-identical on Keychain access denial")
            let preserved = try JSONDecoder().decode(HostPersistedState.self, from: Data(contentsOf: pairedFileURL))
            XCTAssertEqual(preserved.pinnedClientCertSHA256, existingPhoneIdent.certificateSHA256)
            XCTAssertEqual(preserved.serverCertSHA256, existingHostIdent.certificateSHA256)
        }

        // Also verify HostController displays the exact message in errorBanner even after startStream() runs,
        // and recovers cleanly once Keychain access is subsequently granted!
        let mockRecoverable = MockKeychainBackend(initialData: try JSONEncoder().encode(existingHostIdent.persistedRecord))
        mockRecoverable.forcedCopyMatchingStatus = errSecAuthFailed
        let recoverableStore = KeychainHostIdentityStore(
            service: "com.example.kinectviewer.host.pairing.mock",
            account: "server-identity-v1",
            primitives: mockRecoverable.primitives
        )
        let host = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: pairedDir,
            identityStore: recoverableStore,
            lockFileURL: tempRoot.appendingPathComponent("KINECT-IN-USE-test"),
            enableStreamingServer: true,
            autostart: false
        )
        defer { host.shutdown() }
        XCTAssertEqual(host.errorBanner, "Allow Keychain access to keep your iPhone paired.")
        XCTAssertEqual(host.streamingSetupError, "Allow Keychain access to keep your iPhone paired.")
        host.useSyntheticSource = true
        host.startStream()
        XCTAssertEqual(host.errorBanner, "Allow Keychain access to keep your iPhone paired.", "Autostart must not wipe Keychain access banner")
        XCTAssertEqual(try Data(contentsOf: pairedFileURL), originalFileBytes)

        // Now user clicks Allow on retry -> server starts and keeps the pinned iPhone!
        mockRecoverable.forcedCopyMatchingStatus = nil
        host.retryStreamingServerSetup()
        XCTAssertNil(host.streamingSetupError)
        XCTAssertNil(host.errorBanner)
        XCTAssertTrue(host.isPhonePaired)
        XCTAssertEqual(host.hostServerCertSHA256, existingHostIdent.certificateSHA256)

        // 3. Item not found in Keychain while pairing.json already has serverCertSHA256 -> throws error, never generates a new identity
        let spyMissingKey = SpyIdentityStore(loadResult: .notFound)
        XCTAssertThrowsError(try HostStreamServer(stateDirectoryURL: pairedDir, identityStore: spyMissingKey)) { err in
            XCTAssertEqual(err as? HostPairingKeyError, .missingKeychainIdentityForExistingPairing)
            XCTAssertEqual(err.localizedDescription, "This Mac's pairing key is missing from the Keychain.")
        }
        XCTAssertTrue(spyMissingKey.savedRecords.isEmpty, "Must never generate or save a new identity when pairing.json already has serverCertSHA256")
        XCTAssertEqual(spyMissingKey.wipeCalls, 0)
        XCTAssertEqual(try Data(contentsOf: pairedFileURL), originalFileBytes)

        // 4. Corrupt data in Keychain -> throws error, never regenerates or touches pairing.json
        let mockCorrupt = MockKeychainBackend(initialData: Data([0xDE, 0xAD, 0xBE, 0xEF]))
        let corruptStore = KeychainHostIdentityStore(
            service: "com.example.kinectviewer.host.pairing.mock",
            account: "server-identity-v1",
            primitives: mockCorrupt.primitives
        )
        if case .corruptedData = corruptStore.load() {} else {
            XCTFail("Expected .corruptedData for undecodable Keychain bytes")
        }
        XCTAssertThrowsError(try HostStreamServer(stateDirectoryURL: pairedDir, identityStore: corruptStore)) { err in
            XCTAssertEqual(err as? HostPairingKeyError, .corruptedKeychainIdentity)
        }
        XCTAssertEqual(mockCorrupt.updateCalls, 0)
        XCTAssertEqual(mockCorrupt.addCalls, 0)
        XCTAssertEqual(mockCorrupt.deleteCalls, 0)
        XCTAssertEqual(try Data(contentsOf: pairedFileURL), originalFileBytes)

        // 5. save() never calls SecItemDelete on a non-not-found update error (e.g. denied/locked/write-perm),
        //    and only adds when update reports errSecItemNotFound
        let originalSeedBytes = try JSONEncoder().encode(existingHostIdent.persistedRecord)
        let mockSaveGuard = MockKeychainBackend(initialData: originalSeedBytes)
        let saveGuardStore = KeychainHostIdentityStore(
            service: "com.example.kinectviewer.host.pairing.mock",
            account: "server-identity-v1",
            primitives: mockSaveGuard.primitives
        )
        let newCandidate = try CryptoIdentity.generateNew(commonName: "kv2-host")
        for failureStatus in [errSecAuthFailed, errSecInteractionNotAllowed, errSecWrPerm] {
            mockSaveGuard.forcedUpdateStatus = failureStatus
            XCTAssertFalse(saveGuardStore.save(newCandidate.persistedRecord))
            XCTAssertEqual(mockSaveGuard.deleteCalls, 0, "save() must never call SecItemDelete on status \(failureStatus)")
            XCTAssertEqual(mockSaveGuard.addCalls, 0, "save() must not call SecItemAdd when update failed with \(failureStatus)")
            XCTAssertEqual(mockSaveGuard.storedData, originalSeedBytes, "Existing Keychain item must remain intact")
        }

        // When item is genuinely absent (update returns errSecItemNotFound), save() adds cleanly without deleting
        let mockFreshSave = MockKeychainBackend(initialData: nil)
        let freshSaveStore = KeychainHostIdentityStore(
            service: "com.example.kinectviewer.host.pairing.mock",
            account: "server-identity-v1",
            primitives: mockFreshSave.primitives
        )
        XCTAssertTrue(freshSaveStore.save(newCandidate.persistedRecord))
        XCTAssertEqual(mockFreshSave.updateCalls, 1)
        XCTAssertEqual(mockFreshSave.addCalls, 1)
        XCTAssertEqual(mockFreshSave.deleteCalls, 0)
    }

    // MARK: - Adopt Keychain identity when pairing.json is missing or settings-only

    func testKeychainLoadedWithoutServerCertAdoptsExistingKeychainIdentityAndPreservesPartialSettings() throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-adopt-keychain-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let survivingKeychainIdent = try CryptoIdentity.generateNew(commonName: "kv2-host")

        // Case A: Keychain has an identity and pairing.json does not exist at all (state folder deleted / app reinstalled)
        let missingFileDir = tempRoot.appendingPathComponent("missing-file", isDirectory: true)
        let spyA = SpyIdentityStore(loadResult: .loaded(survivingKeychainIdent.persistedRecord))
        let serverA = try HostStreamServer(stateDirectoryURL: missingFileDir, identityStore: spyA)
        defer { serverA.shutdown() }

        XCTAssertEqual(serverA.serverCertSHA256, survivingKeychainIdent.certificateSHA256, "Must adopt surviving Keychain identity")
        XCTAssertTrue(spyA.savedRecords.isEmpty, "Adopting an already-stored Keychain identity must not generate or overwrite Keychain")
        XCTAssertFalse(serverA.isPhonePaired)
        XCTAssertTrue(serverA.allowPhoneModeSwitch)
        XCTAssertTrue(serverA.allowPhoneSensorPowerControl)
        XCTAssertFalse(serverA.turnOffKinectOnPhoneDisconnect)

        let adoptedFileDataA = try Data(contentsOf: missingFileDir.appendingPathComponent("pairing.json"))
        let adoptedTextA = try XCTUnwrap(String(data: adoptedFileDataA, encoding: .utf8))
        XCTAssertFalse(adoptedTextA.contains("privateKeyRaw"))
        let adoptedStateA = try JSONDecoder().decode(HostPersistedState.self, from: adoptedFileDataA)
        XCTAssertEqual(adoptedStateA.serverCertSHA256, survivingKeychainIdent.certificateSHA256)
        XCTAssertEqual(adoptedStateA.serverPublicKeyDER, survivingKeychainIdent.privateKey.publicKey.derRepresentation)
        XCTAssertNil(adoptedStateA.pinnedClientCertSHA256)

        // Case B: Keychain has an identity and pairing.json is settings-only (no serverCertSHA256)
        let settingsOnlyDir = tempRoot.appendingPathComponent("settings-only", isDirectory: true)
        try FileManager.default.createDirectory(at: settingsOnlyDir, withIntermediateDirectories: true)
        let partialJSON = """
        {
          "serviceId": "cafe00112233",
          "allowPhoneModeSwitch": false,
          "allowPhoneSensorPowerControl": false,
          "turnOffKinectOnPhoneDisconnect": true
        }
        """
        try Data(partialJSON.utf8).write(to: settingsOnlyDir.appendingPathComponent("pairing.json"), options: .atomic)

        let spyB = SpyIdentityStore(loadResult: .loaded(survivingKeychainIdent.persistedRecord))
        let serverB = try HostStreamServer(stateDirectoryURL: settingsOnlyDir, identityStore: spyB)
        defer { serverB.shutdown() }

        XCTAssertEqual(serverB.serverCertSHA256, survivingKeychainIdent.certificateSHA256)
        XCTAssertTrue(spyB.savedRecords.isEmpty)
        XCTAssertEqual(serverB.serviceId, "cafe00112233")
        XCTAssertFalse(serverB.isPhonePaired)
        XCTAssertFalse(serverB.allowPhoneModeSwitch)
        XCTAssertFalse(serverB.allowPhoneSensorPowerControl)
        XCTAssertTrue(serverB.turnOffKinectOnPhoneDisconnect)
    }

    // MARK: - Distinct per-error messages & explicit confirmed Reset with `.bad` backup

    func testDistinctPairingFailureMessagesAndConfirmedResetWithBadBackupAndWorkingNewIdentity() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase8-reset-flow-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let oldHostIdent = try CryptoIdentity.generateNew(commonName: "kv2-host")
        let otherIdent = try CryptoIdentity.generateNew(commonName: "kv2-host")
        let oldPhoneIdent = try CryptoIdentity.generateNew(commonName: "kv2-pocket")

        func makeSeededPairedDir(_ name: String) throws -> (dir: URL, fileURL: URL, rawBytes: Data) {
            let dir = tempRoot.appendingPathComponent(name, isDirectory: true)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let st = HostPersistedState(
                serverCertSHA256: oldHostIdent.certificateSHA256,
                serverPublicKeyDER: oldHostIdent.privateKey.publicKey.derRepresentation,
                serviceId: "aabbccddeeff",
                pinnedClientCertSHA256: oldPhoneIdent.certificateSHA256,
                allowPhoneModeSwitch: false,
                allowPhoneSensorPowerControl: true,
                turnOffKinectOnPhoneDisconnect: true
            )
            let fileURL = dir.appendingPathComponent("pairing.json")
            try JSONEncoder().encode(st).write(to: fileURL, options: .atomic)
            return (dir, fileURL, try Data(contentsOf: fileURL))
        }

        // 1. Verify all 6 HostPairingKeyError cases have distinct plain-English messages,
        //    and Reset is offered ONLY for the 4 unrecoverable mismatch/missing/corruption cases.
        var observedMessages: [String] = []

        // (a) keychainAccessRequired -> Retry only (no Reset)
        let (dirDenied, _, _) = try makeSeededPairedDir("case-denied")
        let spyDenied = SpyIdentityStore(loadResult: .accessDeniedOrLocked(errSecAuthFailed))
        let hostDenied = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec-a", isDirectory: true),
            stateDirectoryURL: dirDenied,
            identityStore: spyDenied,
            enableStreamingServer: true,
            autostart: false
        )
        defer { hostDenied.shutdown() }
        XCTAssertEqual(hostDenied.streamingSetupError, "Allow Keychain access to keep your iPhone paired.")
        XCTAssertFalse(hostDenied.canOfferResetPairing)
        hostDenied.requestResetPairing()
        XCTAssertFalse(hostDenied.showResetPairingConfirmation, "Reset must not be available on keychainAccessRequired")
        observedMessages.append(try XCTUnwrap(hostDenied.streamingSetupError))

        // (b) keychainSaveFailed -> Retry only (no Reset)
        let dirSaveFail = tempRoot.appendingPathComponent("case-savefail", isDirectory: true)
        let spySaveFail = SpyIdentityStore(loadResult: .notFound)
        spySaveFail.saveResult = false
        let hostSaveFail = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec-b", isDirectory: true),
            stateDirectoryURL: dirSaveFail,
            identityStore: spySaveFail,
            enableStreamingServer: true,
            autostart: false
        )
        defer { hostSaveFail.shutdown() }
        XCTAssertEqual(hostSaveFail.streamingSetupError, "Couldn't save this Mac's pairing key to the Keychain.")
        XCTAssertFalse(hostSaveFail.canOfferResetPairing)
        hostSaveFail.requestResetPairing()
        XCTAssertFalse(hostSaveFail.showResetPairingConfirmation, "Reset must not be available on keychainSaveFailed")
        observedMessages.append(try XCTUnwrap(hostSaveFail.streamingSetupError))

        // (c) missingKeychainIdentityForExistingPairing -> offers Reset
        let (dirMissing, _, _) = try makeSeededPairedDir("case-missing")
        let spyMissing = SpyIdentityStore(loadResult: .notFound)
        let hostMissing = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec-c", isDirectory: true),
            stateDirectoryURL: dirMissing,
            identityStore: spyMissing,
            enableStreamingServer: true,
            autostart: false
        )
        defer { hostMissing.shutdown() }
        XCTAssertEqual(hostMissing.streamingSetupError, "This Mac's pairing key is missing from the Keychain.")
        XCTAssertTrue(hostMissing.canOfferResetPairing)
        observedMessages.append(try XCTUnwrap(hostMissing.streamingSetupError))

        // (d) mismatchedKeychainIdentity -> offers Reset
        let (dirMismatch, mismatchFileURL, mismatchOriginalBytes) = try makeSeededPairedDir("case-mismatch")
        let mockMismatchKC = MockKeychainBackend(initialData: try JSONEncoder().encode(otherIdent.persistedRecord))
        let mismatchStore = KeychainHostIdentityStore(
            service: "com.example.kinectviewer.host.pairing.mock",
            account: "server-identity-v1",
            primitives: mockMismatchKC.primitives
        )
        let hostMismatch = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec-d", isDirectory: true),
            stateDirectoryURL: dirMismatch,
            identityStore: mismatchStore,
            enableStreamingServer: true,
            autostart: false
        )
        defer { hostMismatch.shutdown() }
        XCTAssertEqual(hostMismatch.streamingSetupError, "This Mac's pairing key doesn't match its saved pairing.")
        XCTAssertTrue(hostMismatch.canOfferResetPairing)
        observedMessages.append(try XCTUnwrap(hostMismatch.streamingSetupError))

        // (e) corruptedKeychainIdentity -> offers Reset
        let (dirCorruptKC, _, _) = try makeSeededPairedDir("case-corrupt-kc")
        let spyCorruptKC = SpyIdentityStore(loadResult: .corruptedData)
        let hostCorruptKC = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec-e", isDirectory: true),
            stateDirectoryURL: dirCorruptKC,
            identityStore: spyCorruptKC,
            enableStreamingServer: true,
            autostart: false
        )
        defer { hostCorruptKC.shutdown() }
        XCTAssertEqual(hostCorruptKC.streamingSetupError, "This Mac's pairing key in the Keychain is damaged.")
        XCTAssertTrue(hostCorruptKC.canOfferResetPairing)
        observedMessages.append(try XCTUnwrap(hostCorruptKC.streamingSetupError))

        // (f) corruptedPairingStateFile -> offers Reset
        let dirCorruptFile = tempRoot.appendingPathComponent("case-corrupt-file", isDirectory: true)
        try FileManager.default.createDirectory(at: dirCorruptFile, withIntermediateDirectories: true)
        let corruptFileURL = dirCorruptFile.appendingPathComponent("pairing.json")
        let corruptRawBytes = Data("{broken-json-not-valid".utf8)
        try corruptRawBytes.write(to: corruptFileURL, options: .atomic)
        let mockCorruptFileKC = MockKeychainBackend(initialData: nil)
        let corruptFileStore = KeychainHostIdentityStore(
            service: "com.example.kinectviewer.host.pairing.mock",
            account: "server-identity-v1",
            primitives: mockCorruptFileKC.primitives
        )
        let hostCorruptFile = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("rec-f", isDirectory: true),
            stateDirectoryURL: dirCorruptFile,
            identityStore: corruptFileStore,
            enableStreamingServer: true,
            autostart: false
        )
        defer { hostCorruptFile.shutdown() }
        XCTAssertEqual(hostCorruptFile.streamingSetupError, "The saved pairing file is damaged.")
        XCTAssertTrue(hostCorruptFile.canOfferResetPairing)
        observedMessages.append(try XCTUnwrap(hostCorruptFile.streamingSetupError))

        XCTAssertEqual(Set(observedMessages).count, 6, "Every HostPairingKeyError case must have its own distinct sentence")

        // 2. Verify nothing resets on its own, on Retry, on unrequested confirm, or on Cancel!
        let mismatchBadURL = mismatchFileURL.appendingPathExtension("bad")
        XCTAssertFalse(FileManager.default.fileExists(atPath: mismatchBadURL.path))
        hostMismatch.retryStreamingServerSetup()
        XCTAssertFalse(FileManager.default.fileExists(atPath: mismatchBadURL.path))
        XCTAssertEqual(try Data(contentsOf: mismatchFileURL), mismatchOriginalBytes)

        hostMismatch.confirmResetPairing() // Ignored because requestResetPairing() wasn't pressed!
        XCTAssertFalse(FileManager.default.fileExists(atPath: mismatchBadURL.path))
        XCTAssertEqual(try Data(contentsOf: mismatchFileURL), mismatchOriginalBytes)

        hostMismatch.requestResetPairing()
        XCTAssertTrue(hostMismatch.showResetPairingConfirmation)
        XCTAssertFalse(FileManager.default.fileExists(atPath: mismatchBadURL.path), "Prompting must not reset before confirmation")
        hostMismatch.cancelResetPairing()
        XCTAssertFalse(hostMismatch.showResetPairingConfirmation)
        XCTAssertFalse(FileManager.default.fileExists(atPath: mismatchBadURL.path))
        XCTAssertEqual(try Data(contentsOf: mismatchFileURL), mismatchOriginalBytes)

        // 3. Press Reset -> Confirm on mismatched state: keeps exact `.bad` copy, creates working new identity, preserves settings, and pairs cleanly!
        hostMismatch.requestResetPairing()
        XCTAssertTrue(hostMismatch.showResetPairingConfirmation)
        hostMismatch.confirmResetPairing()

        XCTAssertFalse(hostMismatch.showResetPairingConfirmation)
        XCTAssertNil(hostMismatch.streamingSetupError)
        XCTAssertNil(hostMismatch.errorBanner)
        XCTAssertFalse(hostMismatch.canOfferResetPairing)
        XCTAssertEqual(mockMismatchKC.deleteCalls, 0, "Reset updates Keychain in place without SecItemDelete")
        XCTAssertEqual(try Data(contentsOf: mismatchBadURL), mismatchOriginalBytes, "pairing.json.bad must hold exact pre-reset bytes")

        let resetCertHash = try XCTUnwrap(hostMismatch.hostServerCertSHA256)
        XCTAssertNotEqual(resetCertHash, oldHostIdent.certificateSHA256)
        XCTAssertNotEqual(resetCertHash, otherIdent.certificateSHA256)
        XCTAssertFalse(hostMismatch.isPhonePaired)
        XCTAssertFalse(hostMismatch.allowPhoneModeSwitch, "Readable toggle settings from old pairing.json are preserved across reset")
        XCTAssertTrue(hostMismatch.turnOffKinectOnPhoneDisconnect)

        // Verify the newly reset host can immediately pair with an iPhone over loopback mTLS 1.3
        hostMismatch.openPairingWindow()
        let offer = try XCTUnwrap(hostMismatch.activePairingOffer)
        let loopbackURI = makeLoopbackQRURI(from: offer, serverCertSHA256: resetCertHash, serviceId: "resetpair")
        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider())
        defer { client.stop() }
        let pairExp = expectation(description: "Paired with newly reset host identity")
        client.pairWithQRURI(loopbackURI) { _ in pairExp.fulfill() }
        await fulfillment(of: [pairExp], timeout: 5.0)
        client.connectDirect(host: "127.0.0.1", port: hostMismatch.activeStreamPort)
        let connected = await waitUntil { client.telemetry.isConnected && hostMismatch.isPhonePaired }
        XCTAssertTrue(connected)

        // 4. Also verify Reset -> Confirm on a damaged pairing.json keeps exact `.bad` bytes and recovers cleanly
        hostCorruptFile.requestResetPairing()
        hostCorruptFile.confirmResetPairing()
        let corruptBadURL = corruptFileURL.appendingPathExtension("bad")
        XCTAssertEqual(try Data(contentsOf: corruptBadURL), corruptRawBytes, "Damaged pairing.json must be backed up byte-for-byte to pairing.json.bad")
        XCTAssertNil(hostCorruptFile.streamingSetupError)
        XCTAssertNotNil(hostCorruptFile.hostServerCertSHA256)
    }
}
