import AVFoundation
import CryptoKit
import Foundation
import Network
import XCTest
@testable import KinectViewer

/// Helper TCP listener that accepts incoming connections and holds them open (or closes at a chosen step)
/// on `127.0.0.1:0` so synthetic tests can deterministically exercise mid-handshake / mid-probe stops.
final class StalledLoopbackTCPServer: @unchecked Sendable {
    private let queue = DispatchQueue(label: "kinectviewer.tests.stalledtcp")
    private var listener: NWListener?
    private var heldConnections: [NWConnection] = []
    private let acceptCountBox = Phase9BSyncBox<Int>(0)
    private let closedCountBox = Phase9BSyncBox<Int>(0)
    private(set) var port: UInt16 = 0

    var acceptCount: Int { acceptCountBox.read() }
    /// Connections the client has since closed (seen as end-of-stream or an error on our side).
    var closedCount: Int { closedCountBox.read() }

    init() throws {
        let readySem = DispatchSemaphore(value: 0)
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        let l = try NWListener(using: params)
        l.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.port = l.port?.rawValue ?? 0
                readySem.signal()
            }
        }
        l.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            self.acceptCountBox.update { $0 += 1 }
            self.heldConnections.append(conn)
            conn.start(queue: self.queue)
            self.watchForCloseOnQueue(conn)
        }
        self.listener = l
        l.start(queue: queue)
        _ = readySem.wait(timeout: .now() + 5.0)
    }

    private func watchForCloseOnQueue(_ conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] _, _, isComplete, error in
            guard let self = self else { return }
            if isComplete || error != nil {
                self.closedCountBox.update { $0 += 1 }
            } else {
                self.watchForCloseOnQueue(conn)
            }
        }
    }

    func shutdown() {
        queue.sync {
            for c in heldConnections {
                c.cancel()
            }
            heldConnections.removeAll()
            listener?.cancel()
            listener = nil
        }
    }
}

/// Loopback TLS listener using a supplied server identity that completes the TLS handshake
/// and immediately closes before sending `0x0B` or `0x02`, exercising candidate probe failure in `receiveStreamLoopOnQueue`.
final class CloseBeforeSessionTLSServer: @unchecked Sendable {
    private let queue = DispatchQueue(label: "kinectviewer.tests.closebeforesession")
    private var listener: NWListener?
    private(set) var port: UInt16 = 0

    init(serverIdentity: CryptoIdentity) throws {
        let readySem = DispatchSemaphore(value: 0)
        let params = CryptoIdentity.makeTLSParameters(
            localIdentity: serverIdentity,
            requireClientCert: true,
            verifyQueue: queue
        ) { _, _ in true }
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        let l = try NWListener(using: params)
        l.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.port = l.port?.rawValue ?? 0
                readySem.signal()
            }
        }
        l.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            conn.stateUpdateHandler = { st in
                if case .ready = st {
                    conn.cancel()
                }
            }
            conn.start(queue: self.queue)
        }
        self.listener = l
        l.start(queue: queue)
        _ = readySem.wait(timeout: .now() + 5.0)
    }

    func shutdown() {
        queue.sync {
            listener?.cancel()
            listener = nil
        }
    }
}

/// Deterministic manual clock for testing 30.0 s outside stability reset without sleeping or lowering production constants.
final class ManualTestClockScheduler: PocketTimerScheduler, @unchecked Sendable {
    private struct Entry {
        let id: UInt64
        let fireAtSec: Double
        let queue: DispatchQueue
        let block: @Sendable () -> Void
        var cancelled: Bool = false
    }

    private let lock = NSLock()
    private var nowSec: Double = 0.0
    private var nextId: UInt64 = 1
    private var entries: [Entry] = []

    func schedule(
        afterSeconds delaySec: Double,
        on queue: DispatchQueue,
        block: @escaping @Sendable () -> Void
    ) -> @Sendable () -> Void {
        lock.lock()
        let id = nextId
        nextId += 1
        entries.append(Entry(id: id, fireAtSec: nowSec + delaySec, queue: queue, block: block))
        lock.unlock()
        return { [weak self] in
            guard let self = self else { return }
            self.lock.lock()
            for idx in self.entries.indices where self.entries[idx].id == id {
                self.entries[idx].cancelled = true
            }
            self.lock.unlock()
        }
    }

    func advance(bySeconds deltaSec: Double) {
        lock.lock()
        nowSec += deltaSec
        var ready: [Entry] = []
        for idx in entries.indices {
            if !entries[idx].cancelled && entries[idx].fireAtSec <= nowSec + 1e-9 {
                entries[idx].cancelled = true
                ready.append(entries[idx])
            }
        }
        entries.removeAll { $0.cancelled }
        lock.unlock()

        for entry in ready {
            let sem = DispatchSemaphore(value: 0)
            entry.queue.async {
                entry.block()
                sem.signal()
            }
            _ = sem.wait(timeout: .now() + 2.0)
        }
    }
}

@MainActor
final class EndpointProbeInviteAndThumbnailTests: XCTestCase {

    private func waitUntil(
        timeout: TimeInterval = 5.0,
        pollMs: UInt64 = 20,
        condition: @escaping @MainActor () -> Bool
    ) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() {
                return true
            }
            try? await Task.sleep(nanoseconds: pollMs * 1_000_000)
        }
        return condition()
    }

    // MARK: - Stopping / backgrounding during a candidate endpoint probe never saves an untested endpoint

    func testFix1_EndpointLinkStoppedBefore5sKeepsOldEndpointAndShowsUnreachable() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }

        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }

        let baseNow = UInt64(Date().timeIntervalSince1970)
        let invite = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "127.0.0.1",
            outsideHost: "127.0.0.1"
        )

        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(
            persistence: persistence,
            deviceName: "Viewer's iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let initialRedeem: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(invite.toURIString(), nowUnixSec: baseNow + 5) {
                cont.resume(returning: $0)
            }
        }
        XCTAssertNoThrow(try initialRedeem.get())

        let initialConnected = await waitUntil { vm.isPetViewer && vm.telemetry.isConnected }
        XCTAssertTrue(initialConnected)

        let originalEndpoint = PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: true)
        XCTAssertEqual(persistence.rawStoredForTesting?.outsideEndpoint, originalEndpoint)

        // Paste a `kv2://endpoint` link pointing at a stalled port (`stalled.port != host.outsidePort`)
        let untestedCandidatePort = stalled.port
        XCTAssertNotEqual(untestedCandidatePort, host.outsidePort)
        vm.inviteOrLinkInput = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: untestedCandidatePort
        ).toURIString()
        vm.submitCameraLinkInput()
        XCTAssertTrue(vm.pairingInProgress)

        let probeStarted = await waitUntil(timeout: 2.0) { stalled.acceptCount >= 1 }
        XCTAssertTrue(probeStarted, "Expected candidate endpoint probe to initiate TCP connection")

        // Stop well before the 5 s probe timeout (simulating backgrounding the app), then start again on LAN!
        // Clear host's advertised outsideEndpoint on 0x0B so a stale `pendingSessionConfirmedCallback` cannot be masked by 0x0B overwriting it back.
        host.setAdvertisedOutsideEndpoint(nil, outsideEnabled: false, outsideStatus: "off")
        client.stop()

        let failedPromptly = await waitUntil(timeout: 1.0) {
            !vm.pairingInProgress && vm.pairingError == PetCameraUserStrings.candidateLinkUnreachableSentence
        }
        XCTAssertTrue(
            failedPromptly,
            "Expected pairingInProgress to reset and 'That link didn't reach your camera.' to be shown on stop(); got pairingInProgress=\(vm.pairingInProgress), pairingError=\(String(describing: vm.pairingError))"
        )

        client.start()
        let reconnectedOnLan = await waitUntil { vm.telemetry.isConnected && vm.telemetry.petState.path == .home }
        XCTAssertTrue(reconnectedOnLan)

        // Old endpoint MUST still be preserved — the untested `stalled.port` must NEVER be saved!
        XCTAssertEqual(
            persistence.rawStoredForTesting?.outsideEndpoint,
            originalEndpoint,
            "Untested candidate endpoint must never overwrite stored outsideEndpoint on the next session start"
        )
        XCTAssertEqual(vm.pairingError, "That link didn't reach your camera.")

        // 9B follow-up 2: a direct `start()` while a candidate probe is in flight leaves the probe alone;
        // only a real stop fails it, and `originalEndpoint` is kept either way.
        let prevStalledAccepts = stalled.acceptCount
        vm.inviteOrLinkInput = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: untestedCandidatePort
        ).toURIString()
        vm.submitCameraLinkInput()
        XCTAssertTrue(vm.pairingInProgress)
        let secondProbeStarted = await waitUntil(timeout: 2.0) { stalled.acceptCount > prevStalledAccepts }
        XCTAssertTrue(secondProbeStarted)

        let leftAlone = Phase9BSyncBox<Int>(0)
        client.onStartLeftProbeAloneForTesting = { leftAlone.update { $0 += 1 } }
        client.start()
        let startLeftProbeAlone = await waitUntil(timeout: 2.0) { leftAlone.read() >= 1 }
        XCTAssertTrue(startLeftProbeAlone, "start() must have reached the probe-in-flight branch")
        XCTAssertTrue(vm.pairingInProgress, "start() while a pasted link's probe is in flight must leave the probe alone")
        XCTAssertNil(vm.pairingError)

        client.stop()
        let stopFailedProbe = await waitUntil(timeout: 1.5) {
            !vm.pairingInProgress &&
            vm.pairingError == PetCameraUserStrings.candidateLinkUnreachableSentence
        }
        XCTAssertTrue(stopFailedProbe)
        XCTAssertEqual(persistence.rawStoredForTesting?.outsideEndpoint, originalEndpoint)
    }

    // MARK: - Invite close without reply -> "Couldn't use this invite; ask for a new one.", no retry, and separate 5 s / 10 s timers

    func testFix2_InviteCloseWithoutReplyShowsSentenceWithNoRetryAndSeparateHandshakeRedemptionTimers() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }

        let baseNow = UInt64(Date().timeIntervalSince1970)
        let expectedSentence = "Couldn't use this invite; ask for a new one."

        // A) PC closes right after TLS handshake without replying (invite ran out mid-handshake)
        host.resetAcceptCounters()
        host.setOutsideRedemptionBehavior(.closeImmediatelyAfterHandshakeWithoutReply)
        let invA = host.createOutsideInvite(role: .viewer, expiresUnixSec: baseNow + 600)

        let clientA = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Phone A", deviceKind: "iphone")
        clientA.disableBonjourForTesting = true
        let reconnectsA = Phase9BSyncBox<Int>(0)
        clientA.onReconnectScheduledForTesting = { _, _ in reconnectsA.update { $0 += 1 } }
        let vmA = PocketViewModel(client: clientA, registerLifecycleObservers: false)
        defer { clientA.stop() }

        vmA.inviteOrLinkInput = invA.toURIString()
        vmA.submitCameraLinkInput(nowUnixSec: baseNow + 5)
        let doneA = await waitUntil(timeout: 3.0) { !vmA.pairingInProgress && vmA.pairingError != nil }
        XCTAssertTrue(doneA)
        XCTAssertEqual(vmA.pairingError, expectedSentence)
        try? await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(host.outsideAcceptCount, 1, "Close without reply must never retry")
        XCTAssertEqual(reconnectsA.read(), 0, "Failed invite must never schedule stream reconnects")

        // B) PC closes after receiving `0x10 pairRequest` without replying (TLS leaf mismatch)
        host.resetAcceptCounters()
        host.setOutsideRedemptionBehavior(.closeAfterPairRequestWithoutReply)
        let invB = host.createOutsideInvite(role: .viewer, expiresUnixSec: baseNow + 600)

        let clientB = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Phone B", deviceKind: "iphone")
        clientB.disableBonjourForTesting = true
        let reconnectsB = Phase9BSyncBox<Int>(0)
        clientB.onReconnectScheduledForTesting = { _, _ in reconnectsB.update { $0 += 1 } }
        defer { clientB.stop() }

        let resB: Result<Void, Error> = await withCheckedContinuation { cont in
            clientB.submitCameraLink(invB.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        if case .failure(let errB) = resB {
            XCTAssertEqual(errB.localizedDescription, expectedSentence)
        } else {
            XCTFail("Expected close after pairRequest without reply to fail")
        }
        XCTAssertEqual(host.outsideAcceptCount, 1)
        XCTAssertEqual(reconnectsB.read(), 0)

        // C) Separate 5 s handshake limit and 10 s redemption limit per address:
        //    When `lh:p` (`::1`) has a fallback `h:p` (`127.0.0.1`), once `0x10` is sent at `lh:p`,
        //    a 3.0 s redemption reply (> old 2.5 s shared budget, < 10 s redemption budget) MUST succeed!
        host.resetAcceptCounters()
        host.setOutsideRedemptionBehavior(.normal)
        let invC = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "::1",
            outsideHost: "127.0.0.1"
        )
        try host.setLanIPv6OutsideListenerEnabled(true, behavior: .delayRedemptionReply(seconds: 3.0))

        let clientC = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Phone C", deviceKind: "iphone")
        clientC.disableBonjourForTesting = true
        defer { clientC.stop() }

        let resC: Result<Void, Error> = await withCheckedContinuation { cont in
            clientC.submitCameraLink(invC.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try resC.get(), "Once 0x10 is sent, redemption has its own 10 s window and must not fail at 2.5 s with 'Can't reach the PC'")
        XCTAssertEqual(host.lanIPv6OutsideAcceptCount, 1)
        XCTAssertEqual(host.outsideAcceptCount, 0)
    }

    // MARK: - Thumbnails never stick on spinner after privacy-on/off or reconnect

    func testFix3_TenThumbnailsWithFourInFlightPrivacyOnThenOffAndReconnectAllLoad() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: false)
        defer { host.shutdown() }

        let seeded = (0..<10).map { idx in
            (
                day: "2026-11-19",
                clipId: String(format: "2026-11-19/18-%02d-00_manual", idx),
                startUnixMs: UInt64(1_795_100_000_000 + idx * 60_000),
                startLocal: String(format: "2026-11-19T18:%02d:00-08:00", idx),
                durationSec: 10.0,
                trigger: ClipTrigger.manual,
                startedBy: "Owner" as String?,
                starredByIds: Set<String>(),
                hasThumb: true
            )
        }
        host.seedClips(seeded)
        host.setHoldPendingThumbnails(true)

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Thumb iPhone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let connected = await waitUntil { vm.isPetOwner && vm.cameraRoll.supportsCameraRoll }
        XCTAssertTrue(connected)

        await vm.cameraRoll.refreshDays(force: true)
        let clips = vm.cameraRoll.selectedDayClips
        XCTAssertEqual(clips.count, 10)

        // Ask for all 10 thumbnails -> 4 in flight, 6 queued
        for clip in clips {
            vm.cameraRoll.requestThumbnailIfNeeded(for: clip)
        }
        let fourInFlight = await waitUntil(timeout: 2.0) { host.heldThumbnailCount == 4 }
        XCTAssertTrue(fourInFlight, "Expected 4 thumbnail requests in flight on the wire")

        // Turn privacy ON -> none of the 10 may remain stuck in `.loading`!
        host.configureState(privacyMode: true)
        let privacySettled = await waitUntil(timeout: 2.0) {
            vm.telemetry.petState.privacyMode &&
            clips.allSatisfy { vm.cameraRoll.thumbnails[$0.clipId] == .unavailable(code: "privacy_mode") }
        }
        XCTAssertTrue(
            privacySettled,
            "All 10 queued/in-flight thumbnails must exit .loading on privacy-on; got \(vm.cameraRoll.thumbnails)"
        )

        // Turn privacy OFF -> all 10 visible cells must automatically be requested again and load!
        host.setHoldPendingThumbnails(false)
        host.configureState(privacyMode: false)

        let allTenLoaded = await waitUntil(timeout: 3.0) {
            clips.allSatisfy {
                if case .loaded(let data) = vm.cameraRoll.thumbnails[$0.clipId] {
                    return !data.isEmpty
                }
                return false
            }
        }
        XCTAssertTrue(allTenLoaded, "All 10 thumbnails must load once privacy turns off; got \(vm.cameraRoll.thumbnails)")

        // Also verify disconnect mid-load resets `.loading` entries and reloads visible cells on reconnect!
        vm.cameraRoll.clearAllInMemoryState()
        await vm.cameraRoll.refreshDays(force: true)
        host.setHoldPendingThumbnails(true)
        for clip in vm.cameraRoll.selectedDayClips {
            vm.cameraRoll.requestThumbnailIfNeeded(for: clip)
        }
        let fourHeldBeforeDrop = await waitUntil(timeout: 2.0) { host.heldThumbnailCount == 4 }
        XCTAssertTrue(fourHeldBeforeDrop)

        try host.setLanStreamEnabled(false)
        let disconnectedCleanly = await waitUntil(timeout: 2.0) {
            !vm.telemetry.isConnected &&
            !vm.cameraRoll.thumbnails.values.contains(.loading)
        }
        XCTAssertTrue(disconnectedCleanly, "No thumbnail may stay .loading across disconnect")

        host.setHoldPendingThumbnails(false)
        try host.setLanStreamEnabled(true)
        let reloadedAfterReconnect = await waitUntil(timeout: 4.0) {
            vm.telemetry.isConnected &&
            clips.allSatisfy {
                if case .loaded = vm.cameraRoll.thumbnails[$0.clipId] { return true }
                return false
            }
        }
        XCTAssertTrue(reloadedAfterReconnect, "Visible cells must reload thumbnails after reconnecting")
    }

    // MARK: - Resigning active hides camera-roll grid & thumbnail images synchronously

    func testFix4_WillResignActiveHidesCameraRollGridAndThumbnails() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: false)
        defer { host.shutdown() }
        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-00-00_manual", startUnixMs: 1_795_111_200_000, startLocal: "2026-11-19T18:00:00+00:00", durationSec: 10.0, trigger: .manual, startedBy: "Owner", starredByIds: [], hasThumb: true)
        ])

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Snapshot Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let connected = await waitUntil { vm.isPetOwner }
        XCTAssertTrue(connected)

        await vm.cameraRoll.refreshDays(force: true)
        let clip = try XCTUnwrap(vm.cameraRoll.selectedDayClips.first)
        vm.cameraRoll.requestThumbnailIfNeeded(for: clip)
        let loaded = await waitUntil { vm.cameraRoll.visibleThumbnailJPEG(for: clip.clipId) != nil }
        XCTAssertTrue(loaded)
        XCTAssertFalse(vm.shouldHideRoomImagesForSnapshot)
        XCTAssertFalse(vm.cameraRoll.isHiddenForSnapshot)

        vm.handleWillResignActive()
        XCTAssertTrue(vm.shouldHideRoomImagesForSnapshot)
        XCTAssertTrue(vm.cameraRoll.isHiddenForSnapshot)
        XCTAssertNil(vm.cameraRoll.visibleThumbnailJPEG(for: clip.clipId), "Thumbnails must not be exposed while inactive for app-switcher snapshot")

        vm.handleDidBecomeActive()
        XCTAssertFalse(vm.shouldHideRoomImagesForSnapshot)
        XCTAssertFalse(vm.cameraRoll.isHiddenForSnapshot)
        XCTAssertNotNil(vm.cameraRoll.visibleThumbnailJPEG(for: clip.clipId))
    }

    // MARK: - `0x0F outside_disabled` keeps stored endpoint, shows sentence, and retries on normal back-off

    func testFix5_OutsideDisabledKeepsStoredEndpointShowsSentenceAndRetriesOnNormalBackoff() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: false, startOutsideListener: true)
        defer { host.shutdown() }

        let baseNow = UInt64(Date().timeIntervalSince1970)
        let invite = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "127.0.0.1",
            outsideHost: "127.0.0.1"
        )

        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(
            persistence: persistence,
            deviceName: "Away iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let redeemRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(invite.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try redeemRes.get())

        let expectedEndpoint = PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: true)
        let connectedOutside = await waitUntil { vm.isPetViewer && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(connectedOutside)
        XCTAssertEqual(persistence.rawStoredForTesting?.outsideEndpoint, expectedEndpoint)

        let scheduledDelays = Phase9BSyncBox<[Double]>([])
        let retryBox = Phase9BSyncBox<(@Sendable () -> Void)?>(nil)
        client.onReconnectScheduledForTesting = { delaySec, retryNow in
            let n = scheduledDelays.update { arr -> Int in
                arr.append(delaySec)
                return arr.count
            }
            if n < 3 {
                retryNow()
            } else {
                retryBox.update { $0 = retryNow }
            }
        }

        // Host turns outside listening off and sends `0x0F outside_disabled`
        try host.setOutsideListenerEnabled(false, sendNoticeOnDisable: true)

        let waitedForBackoff = await waitUntil(timeout: 3.0) {
            !vm.telemetry.isConnected && scheduledDelays.read().count >= 3
        }
        XCTAssertTrue(waitedForBackoff)

        // Stored endpoint MUST be preserved in Keychain & telemetry!
        XCTAssertEqual(
            persistence.rawStoredForTesting?.outsideEndpoint,
            expectedEndpoint,
            "0x0F outside_disabled must never delete the stored outsideEndpoint"
        )
        XCTAssertEqual(vm.telemetry.storedOutsideEndpoint, expectedEndpoint)
        XCTAssertEqual(vm.disconnectedStatusTitle, "Viewing from outside the home was turned off on the PC.")
        XCTAssertEqual(scheduledDelays.read(), [1.0, 2.0, 4.0])

        // Re-enabling outside listening on the PC allows the next retry to reconnect using that preserved endpoint!
        try host.setOutsideListenerEnabled(true)
        retryBox.read()?()
        let reconnectedOutside = await waitUntil(timeout: 3.0) {
            vm.telemetry.isConnected && vm.telemetry.petState.path == .outside
        }
        XCTAssertTrue(reconnectedOutside, "Device must reconnect once outside viewing is turned back on because outsideEndpoint was kept")
    }

    // MARK: - Pin mismatch or refused handshake at `lh:p` never falls through to `h:p`

    func testFix6_FailureAtLanHostNeverFallsThroughToOutsideHostUnlessTCPUnreachable() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }

        let baseNow = UInt64(Date().timeIntervalSince1970)

        // A) Pin mismatch at `lh:p` (`::1`) while `h:p` (`127.0.0.1`) has a valid open invite -> MUST NOT fall through to `h:p`!
        let wrongServerIdentity = try CryptoIdentity.generateNew(commonName: "kv3-impostor")
        let inviteA = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "::1",
            outsideHost: "127.0.0.1"
        )
        try host.setLanIPv6OutsideListenerEnabled(true, identityOverride: wrongServerIdentity, behavior: .normal)
        host.resetAcceptCounters()

        let clientA = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Pin Test", deviceKind: "iphone")
        clientA.disableBonjourForTesting = true
        defer { clientA.stop() }

        let resA: Result<Void, Error> = await withCheckedContinuation { cont in
            clientA.submitCameraLink(inviteA.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        if case .failure(let errA) = resA {
            XCTAssertEqual(errA.localizedDescription, "Camera certificate pin mismatch.")
        } else {
            XCTFail("Expected pin mismatch at lh:p to fail")
        }
        XCTAssertEqual(host.lanIPv6OutsideAcceptCount, 1)
        XCTAssertEqual(host.outsideAcceptCount, 0, "Pin mismatch at lh:p must stop redemption and never fall through to h:p")

        // B) Handshake refused by PC at `lh:p` (`::1` rejects client cert at TLS) while `h:p` (`127.0.0.1`) is up -> MUST NOT fall through to `h:p`!
        let inviteB = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "::1",
            outsideHost: "127.0.0.1"
        )
        try host.setLanIPv6OutsideListenerEnabled(true, identityOverride: nil, behavior: .rejectClientCertAtTLS)
        host.resetAcceptCounters()

        let clientB = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Refused Test", deviceKind: "iphone")
        clientB.disableBonjourForTesting = true
        defer { clientB.stop() }

        let resB: Result<Void, Error> = await withCheckedContinuation { cont in
            clientB.submitCameraLink(inviteB.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        if case .failure(let errB) = resB {
            XCTAssertEqual(errB.localizedDescription, PetCameraUserStrings.inviteClosedWithoutReplySentence)
        } else {
            XCTFail("Expected handshake refused by PC at lh:p to fail without falling through to h:p")
        }
        XCTAssertEqual(host.lanIPv6OutsideAcceptCount, 1)
        XCTAssertEqual(host.outsideAcceptCount, 0, "Refused handshake at lh:p must never count a second attempt at h:p")

        // C) `lh:p` (`::1`) unreachable at TCP (listener down -> ECONNREFUSED) -> DOES fall through to `h:p` (`127.0.0.1`) and succeeds!
        try host.setLanIPv6OutsideListenerEnabled(false)
        let inviteC = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "::1",
            outsideHost: "127.0.0.1"
        )
        host.resetAcceptCounters()

        let clientC = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Away Fallback", deviceKind: "iphone")
        clientC.disableBonjourForTesting = true
        defer { clientC.stop() }

        let resC: Result<Void, Error> = await withCheckedContinuation { cont in
            clientC.submitCameraLink(inviteC.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try resC.get())
        XCTAssertEqual(host.lanIPv6OutsideAcceptCount, 0)
        XCTAssertEqual(host.outsideAcceptCount, 1, "Unreachable TCP at lh:p must fall through to h:p")
    }

    // MARK: - A failed candidate endpoint probe schedules reconnect once, never twice

    func testFix7_FailedEndpointProbeSchedulesReconnectOnlyOnce() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }

        let closeAfterTLS = try CloseBeforeSessionTLSServer(serverIdentity: host.serverIdentity)
        defer { closeAfterTLS.shutdown() }

        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Probe Phone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let connected = await waitUntil { vm.isPetOwner && vm.telemetry.isConnected }
        XCTAssertTrue(connected)

        let scheduledDelays = Phase9BSyncBox<[Double]>([])
        client.onReconnectScheduledForTesting = { delaySec, _ in
            scheduledDelays.update { $0.append(delaySec) }
        }

        // 1) Candidate endpoint completes TLS then closes before session (`receiveStreamLoopOnQueue` path)
        let closeAfterTLSLink = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: closeAfterTLS.port
        ).toURIString()

        let probeRes1: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(closeAfterTLSLink) { cont.resume(returning: $0) }
        }
        if case .failure(let err1) = probeRes1 {
            XCTAssertEqual(err1.localizedDescription, PetCameraUserStrings.candidateLinkUnreachableSentence)
        } else {
            XCTFail("Expected probe that closed before 0x0B/0x02 to fail")
        }
        try? await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(
            scheduledDelays.read().count,
            1,
            "Failed endpoint probe must schedule reconnect exactly once; got \(scheduledDelays.read())"
        )
        XCTAssertEqual(client.telemetry.reconnectCount, 1)

        // 2) Candidate endpoint refused at TCP (`waiting`/`failed` path) also schedules reconnect exactly once
        scheduledDelays.update { $0.removeAll() }
        let refusedPortLink = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: 1
        ).toURIString()

        let probeRes2: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(refusedPortLink) { cont.resume(returning: $0) }
        }
        if case .failure(let err2) = probeRes2 {
            XCTAssertEqual(err2.localizedDescription, PetCameraUserStrings.candidateLinkUnreachableSentence)
        } else {
            XCTFail("Expected refused-port probe to fail")
        }
        try? await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(
            scheduledDelays.read().count,
            1,
            "TCP-refused endpoint probe must schedule reconnect exactly once; got \(scheduledDelays.read())"
        )
    }

    // MARK: - 30 s back-off reset on test clock, distinct `lh`/`h` invite order, `0x0B` endpoint update, invite sentences & no retry

    func testFix8_OutsideBackoffResetsOnlyAfter30sOnTestClock() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: false, startOutsideListener: true)
        defer { host.shutdown() }

        let baseNow = UInt64(Date().timeIntervalSince1970)
        let invite = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "127.0.0.1",
            outsideHost: "127.0.0.1"
        )

        let testClock = ManualTestClockScheduler()
        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Clock iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        client.timerScheduler = testClock
        // Keep the real production 30.0 s stability threshold untouched!
        XCTAssertEqual(client.outsideSessionStabilityResetSec, 30.0)
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let redeemRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(invite.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try redeemRes.get())
        let firstConn = await waitUntil { vm.isPetViewer && vm.telemetry.isConnected }
        XCTAssertTrue(firstConn)

        // Drive two failed reconnect cycles while the outside listener is down so `reconnectCount == 2` (`> 0`)!
        let delays = Phase9BSyncBox<[Double]>([])
        let retryBox = Phase9BSyncBox<(@Sendable () -> Void)?>(nil)
        client.onReconnectScheduledForTesting = { delaySec, retryNow in
            let count = delays.update { list -> Int in
                list.append(delaySec)
                return list.count
            }
            if count < 2 {
                retryNow()
            } else {
                retryBox.update { $0 = retryNow }
            }
        }
        try host.setOutsideListenerEnabled(false, sendNoticeOnDisable: false)
        let reachedTwoRetries = await waitUntil { delays.read().count == 2 && vm.telemetry.reconnectCount == 2 }
        XCTAssertTrue(reachedTwoRetries)
        XCTAssertEqual(delays.read(), [1.0, 2.0])

        // Reconnect on `.outside` with `reconnectCount == 2` (> 0): must NOT reset on connect or at 29.9 s!
        try host.setOutsideListenerEnabled(true)
        retryBox.read()?()
        let reconnectedShort = await waitUntil { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(reconnectedShort)
        XCTAssertEqual(vm.telemetry.reconnectCount, 2, "Outside session must not reset reconnectCount on connect")

        testClock.advance(bySeconds: 29.9)
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(vm.telemetry.reconnectCount, 2, "Outside session held for 29.9 s (< 30 s) must keep reconnectCount > 0")

        // Dropping at 29.9 s must escalate back-off to 4.0 s (`reconnectCount == 3`), proving 29.9 s did not reset!
        client.onReconnectScheduledForTesting = { delaySec, retryNow in
            delays.update { $0.append(delaySec) }
            retryBox.update { $0 = retryNow }
        }
        host.closeAllSessions()
        let droppedAt29_9 = await waitUntil { !vm.telemetry.isConnected && delays.read().count == 3 }
        XCTAssertTrue(droppedAt29_9)
        XCTAssertEqual(delays.read().last, 4.0)
        XCTAssertEqual(vm.telemetry.reconnectCount, 3)

        // Reconnect again (`reconnectCount == 3`), hold 29.9 s -> still 3; advance +0.1 s (30.0 s total) -> resets to 0!
        retryBox.read()?()
        let reconnectedStable = await waitUntil { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(reconnectedStable)
        XCTAssertEqual(vm.telemetry.reconnectCount, 3)

        testClock.advance(bySeconds: 29.9)
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(vm.telemetry.reconnectCount, 3)

        testClock.advance(bySeconds: 0.1)
        let resetAt30s = await waitUntil(timeout: 1.5) { vm.telemetry.reconnectCount == 0 }
        XCTAssertTrue(resetAt30s, "reconnectCount must reset to 0 once the outside session reaches 30.0 s on the clock")

        // Next drop starts fresh at 1.0 s!
        host.closeAllSessions()
        let freshDelay = await waitUntil { !vm.telemetry.isConnected && delays.read().count == 4 }
        XCTAssertTrue(freshDelay)
        XCTAssertEqual(delays.read().last, 1.0)
    }

    func testFix8_DistinctLanHostAndOutsideHostTriesLanFirst() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        try host.setLanIPv6OutsideListenerEnabled(true, behavior: .normal)
        host.resetAcceptCounters()

        let baseNow = UInt64(Date().timeIntervalSince1970)
        let invite = host.createOutsideInvite(
            role: .viewer,
            expiresUnixSec: baseNow + 600,
            lanHost: "::1",
            outsideHost: "127.0.0.1"
        )
        XCTAssertNotEqual(invite.lanHost, invite.outsideHost)

        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(
            persistence: persistence,
            deviceName: "LAN Order iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        defer { client.stop() }

        let redeemRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(invite.toURIString(), nowUnixSec: baseNow + 5) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try redeemRes.get())
        XCTAssertEqual(host.lanIPv6OutsideAcceptCount, 1, "lh:p (::1) must be tried first")
        XCTAssertEqual(host.outsideAcceptCount, 0, "h:p (127.0.0.1) must not be touched when lh:p succeeds")
        XCTAssertEqual(
            persistence.rawStoredForTesting?.outsideEndpoint,
            PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: true),
            "Stored outsideEndpoint must still save h:p (127.0.0.1)"
        )
    }

    func testFix8_0x0BOutsideEndpointUpdateInviteSentencesRefusedNeverRetriedAndCloseWithoutReply() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }

        // 1) `0x0B outsideEndpoint` updates Keychain persistence (`nil` -> verified -> manual unverified -> kept when nil)
        host.setAdvertisedOutsideEndpoint(nil, outsideEnabled: false, outsideStatus: "off")
        let persistence = InMemoryPersistenceProvider()
        let ownerClient = try PocketStreamClientCore(persistence: persistence, deviceName: "Owner iPhone", deviceKind: "iphone")
        ownerClient.disableBonjourForTesting = true
        let ownerVM = PocketViewModel(client: ownerClient, registerLifecycleObservers: false)
        defer { ownerClient.stop() }

        let lanPair: Result<Void, Error> = await withCheckedContinuation { cont in
            ownerClient.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try lanPair.get())
        let ownerReady = await waitUntil { ownerVM.isPetOwner }
        XCTAssertTrue(ownerReady)
        XCTAssertNil(persistence.rawStoredForTesting?.outsideEndpoint)

        let epVerified = PetEndpoint(host: "203.0.113.42", port: 31415, verified: true)
        host.setAdvertisedOutsideEndpoint(epVerified, outsideEnabled: true, outsideStatus: "mapped")
        let updated1 = await waitUntil { ownerVM.telemetry.storedOutsideEndpoint == epVerified }
        XCTAssertTrue(updated1)
        XCTAssertEqual(persistence.rawStoredForTesting?.outsideEndpoint, epVerified)

        let epManual = PetEndpoint(host: "198.51.100.77", port: 42424, verified: false)
        host.setAdvertisedOutsideEndpoint(epManual, outsideEnabled: true, outsideStatus: "manual")
        let updated2 = await waitUntil { ownerVM.telemetry.storedOutsideEndpoint == epManual }
        XCTAssertTrue(updated2)
        XCTAssertEqual(persistence.rawStoredForTesting?.outsideEndpoint, epManual)

        host.setAdvertisedOutsideEndpoint(nil, outsideEnabled: false, outsideStatus: "off")
        try? await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertEqual(
            persistence.rawStoredForTesting?.outsideEndpoint,
            epManual,
            "0x0B without outsideEndpoint must keep last stored endpoint in Keychain (§5.5)"
        )

        // 2) All §9 / W8 invite sentences + refused redemptions NEVER retried + close with no reply
        let baseNow = UInt64(Date().timeIntervalSince1970)

        func runSingleInviteAttempt(
            uri: String,
            nowSec: UInt64
        ) async throws -> (errorText: String, acceptsDelta: Int, reconnects: Int) {
            host.resetAcceptCounters()
            let reconnectsBox = Phase9BSyncBox<Int>(0)
            let c = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Invite Probe", deviceKind: "iphone")
            c.disableBonjourForTesting = true
            c.onReconnectScheduledForTesting = { _, _ in reconnectsBox.update { $0 += 1 } }
            defer { c.stop() }
            let res: Result<Void, Error> = await withCheckedContinuation { cont in
                c.submitCameraLink(uri, nowUnixSec: nowSec) { cont.resume(returning: $0) }
            }
            try? await Task.sleep(nanoseconds: 100_000_000)
            guard case .failure(let err) = res else {
                XCTFail("Expected invite redemption to fail")
                return ("", host.outsideAcceptCount, reconnectsBox.read())
            }
            XCTAssertFalse(c.telemetry.isPaired)
            return (err.localizedDescription, host.outsideAcceptCount, reconnectsBox.read())
        }

        func wrongToken(for invite: InviteLinkPayload) -> InviteLinkPayload {
            InviteLinkPayload(
                serviceId: invite.serviceId,
                serverCertSHA256: invite.serverCertSHA256,
                oneTimeToken: Data(repeating: 0xEE, count: 16),
                outsideHost: invite.outsideHost,
                outsidePort: invite.outsidePort,
                lanHost: invite.lanHost,
                expiresUnixSec: invite.expiresUnixSec,
                roleHint: .viewer
            )
        }

        // a) One wrong code (`0x11 accepted: false`) -> "didn't match" sentence, never retried, and the invite stays open.
        host.setOutsideRedemptionBehavior(.normal)
        let validInvite = host.createOutsideInvite(role: .viewer, expiresUnixSec: baseNow + 600)
        let wrongRes = try await runSingleInviteAttempt(uri: wrongToken(for: validInvite).toURIString(), nowSec: baseNow + 5)
        XCTAssertEqual(wrongRes.errorText, "That invite didn't match. Check that you used the whole link you were sent.")
        XCTAssertEqual(wrongRes.errorText, PetCameraUserStrings.inviteDidntMatchSentence)
        XCTAssertEqual(wrongRes.acceptsDelta, 1)
        XCTAssertEqual(wrongRes.reconnects, 0)

        // a') The original token then redeems: one wrong code didn't burn the invite.
        do {
            let c = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Invite Retry", deviceKind: "iphone")
            c.disableBonjourForTesting = true
            defer { c.stop() }
            let res: Result<Void, Error> = await withCheckedContinuation { cont in
                c.submitCameraLink(validInvite.toURIString(), nowUnixSec: baseNow + 6) { cont.resume(returning: $0) }
            }
            XCTAssertNoThrow(try res.get(), "After one wrong code the original invite must still redeem")
        }

        // b) A burned invite takes ten wrong attempts: the first nine "didn't match", the tenth "can't be used again".
        let tenInvite = host.createOutsideInvite(role: .viewer, expiresUnixSec: baseNow + 600)
        for attempt in 1...9 {
            let r = try await runSingleInviteAttempt(uri: wrongToken(for: tenInvite).toURIString(), nowSec: baseNow + 7)
            XCTAssertEqual(r.errorText, PetCameraUserStrings.inviteDidntMatchSentence, "wrong attempt \(attempt)")
            XCTAssertEqual(r.reconnects, 0)
        }
        let tenthRes = try await runSingleInviteAttempt(uri: wrongToken(for: tenInvite).toURIString(), nowSec: baseNow + 7)
        XCTAssertEqual(tenthRes.errorText, "That invite didn't match. It can't be used again; ask for a new one.")
        XCTAssertEqual(tenthRes.errorText, PetCameraUserStrings.inviteWrongCodeSentence)
        XCTAssertEqual(tenthRes.acceptsDelta, 1)
        XCTAssertEqual(tenthRes.reconnects, 0)

        // b') Trying the burned invite afterward with the original token -> expired/used sentence, never retried!
        host.setAdmitExpiredRedemptionWithoutOpenInvite(true)
        let burnedRes = try await runSingleInviteAttempt(uri: tenInvite.toURIString(), nowSec: baseNow + 8)
        XCTAssertEqual(burnedRes.errorText, "This invite has expired or was already used. Ask for a new one.")
        XCTAssertEqual(burnedRes.acceptsDelta, 1)
        XCTAssertEqual(burnedRes.reconnects, 0)
        host.setAdmitExpiredRedemptionWithoutOpenInvite(false)

        // c) Closed by the PC while connecting (W8 second round) -> exact sentence, never retried!
        let closedInvite = host.createOutsideInvite(role: .viewer, expiresUnixSec: baseNow + 600)
        host.setOutsideRedemptionBehavior(.refuseWithInviteClosedByPC)
        let closedByPCRes = try await runSingleInviteAttempt(uri: closedInvite.toURIString(), nowSec: baseNow + 5)
        XCTAssertEqual(closedByPCRes.errorText, "This invite was closed. Ask for a new one.")
        XCTAssertEqual(closedByPCRes.acceptsDelta, 1)
        XCTAssertEqual(closedByPCRes.reconnects, 0)

        // d) 16 paired devices cap -> exact sentence, never retried!
        let fullInvite = host.createOutsideInvite(role: .viewer, expiresUnixSec: baseNow + 600)
        host.setOutsideRedemptionBehavior(.refuseWithTooManyPairedDevices)
        let fullRes = try await runSingleInviteAttempt(uri: fullInvite.toURIString(), nowSec: baseNow + 5)
        XCTAssertEqual(fullRes.errorText, "This PC has 16 paired devices. Revoke one first.")
        XCTAssertEqual(fullRes.acceptsDelta, 1)
        XCTAssertEqual(fullRes.reconnects, 0)

        // e) Close with no reply -> exact sentence, never retried!
        let silentCloseInvite = host.createOutsideInvite(role: .viewer, expiresUnixSec: baseNow + 600)
        host.setOutsideRedemptionBehavior(.closeAfterPairRequestWithoutReply)
        let silentCloseRes = try await runSingleInviteAttempt(uri: silentCloseInvite.toURIString(), nowSec: baseNow + 5)
        XCTAssertEqual(silentCloseRes.errorText, "Couldn't use this invite; ask for a new one.")
        XCTAssertEqual(silentCloseRes.acceptsDelta, 1)
        XCTAssertEqual(silentCloseRes.reconnects, 0)
    }
}
