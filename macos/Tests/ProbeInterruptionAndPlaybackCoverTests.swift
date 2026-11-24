import Combine
import Foundation
import XCTest
@testable import KinectViewer

/// a pasted link's endpoint probe vs. unpair and brief interruptions.
/// Synthetic only: a loopback fake host and a stalled loopback TCP port, never a real camera.
@MainActor
final class ProbeInterruptionAndPlaybackCoverTests: XCTestCase {

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

    /// Pairs as owner over the loopback LAN offer, then pastes an endpoint link at a stalled port so a candidate probe
    /// is left waiting (its own timeout is 5 s).
    private func pairAndStartStalledProbe(
        host: FakeV3CameraRollAndInviteHost,
        stalled: StalledLoopbackTCPServer
    ) async throws -> (PocketStreamClientCore, PocketViewModel) {
        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Follow-up Phone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let connected = await waitUntil { vm.isPetOwner && vm.telemetry.isConnected }
        XCTAssertTrue(connected)

        vm.inviteOrLinkInput = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: stalled.port
        ).toURIString()
        vm.submitCameraLinkInput()
        XCTAssertTrue(vm.pairingInProgress)
        let probeStarted = await waitUntil(timeout: 2.0) { stalled.acceptCount >= 1 }
        XCTAssertTrue(probeStarted, "Expected the candidate endpoint probe to reach the stalled port")
        return (client, vm)
    }

    // MARK: - unpairing during a probe leaves no stale message on the fresh pairing sheet

    func testFollowup1_UnpairDuringProbeLeavesNoMessage() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }

        let (client, vm) = try await pairAndStartStalledProbe(host: host, stalled: stalled)
        defer { client.stop() }

        vm.unpairDevice()
        XCTAssertTrue(vm.showPairingSheet)

        let unpaired = await waitUntil(timeout: 2.0) { !vm.telemetry.isPaired }
        XCTAssertTrue(unpaired)
        // The unpair cancels the probe, which closes its connection; wait for the stalled port to see that close, then let
        // the cancelled probe's failure (posted to the main actor right after the close) arrive.
        let probeClosed = await waitUntil(timeout: 2.0) { stalled.closedCount >= 1 }
        XCTAssertTrue(probeClosed, "Unpair must close the probe's connection")
        for _ in 0..<10 { await Task.yield() }

        XCTAssertNil(vm.pairingError, "The fresh pairing sheet must not show the cancelled probe's message")
        XCTAssertNil(vm.telemetry.lastError, "unpair() must clear the core's lastError too")
        XCTAssertNil(client.telemetry.lastError)
        XCTAssertFalse(vm.pairingInProgress)
        XCTAssertTrue(vm.showPairingSheet)
    }

    // MARK: - brief interruptions and a second start() leave a pasted link's probe running

    func testFollowup2_ResignBecomeActiveAndSecondStartLeaveProbeRunning() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }

        let (client, vm) = try await pairAndStartStalledProbe(host: host, stalled: stalled)
        defer { client.stop() }

        let leftAlone = Phase9BSyncBox<Int>(0)
        client.onStartLeftProbeAloneForTesting = { leftAlone.update { $0 += 1 } }
        let acceptsBefore = stalled.acceptCount

        // Pulling down Control Center: willResignActive then didBecomeActive (which calls `client.start()`).
        vm.handleWillResignActive()
        vm.handleDidBecomeActive()
        // A second, direct `start()` while the client is already running and probing.
        client.start()

        let bothLeftAlone = await waitUntil(timeout: 2.0) { leftAlone.read() >= 2 }
        XCTAssertTrue(bothLeftAlone, "Both start() calls must leave the probe alone (got \(leftAlone.read()))")
        XCTAssertEqual(stalled.closedCount, 0, "The probe's connection must still be open")

        XCTAssertTrue(vm.pairingInProgress, "The probe must still be running after a brief interruption")
        XCTAssertNil(vm.pairingError)
        XCTAssertEqual(stalled.acceptCount, acceptsBefore, "No new probe or reconnect may have been started")

        // A real stop (backgrounding) still cancels the probe and says so.
        vm.handleDidEnterBackground()
        let stopFailedProbe = await waitUntil(timeout: 1.5) {
            !vm.pairingInProgress && vm.pairingError == PetCameraUserStrings.candidateLinkUnreachableSentence
        }
        XCTAssertTrue(stopFailedProbe, "Backgrounding must still cancel the probe")
    }

    // MARK: - screen capture ends during a probe that began from a stopped client

    func testFollowup1b_StartDuringProbeFromStoppedClientReconnectsWhenProbeFails() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }

        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Capture Phone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        client.candidateProbeTimeoutSec = 1.0
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let connected = await waitUntil { vm.isPetOwner && vm.telemetry.isConnected }
        XCTAssertTrue(connected)

        // Screen capture starts: the client is stopped (as `refreshScreenCaptureState` does). `stop()` is synchronous.
        client.stop()

        // Record reconnects instead of waiting on telemetry (which `stop()` does not republish), and run each at once.
        let scheduledReconnects = Phase9BSyncBox<Int>(0)
        client.onReconnectScheduledForTesting = { _, fire in
            scheduledReconnects.update { $0 += 1 }
            fire()
        }

        // The pairing sheet stays reachable during capture, so a link can be pasted: its probe starts from a stopped client.
        vm.inviteOrLinkInput = EndpointLinkPayload(
            serviceId: host.serviceId,
            serverCertSHA256: host.serverIdentity.certificateSHA256,
            outsideHost: "127.0.0.1",
            outsidePort: stalled.port
        ).toURIString()
        vm.submitCameraLinkInput()
        let probeStarted = await waitUntil(timeout: 2.0) { stalled.acceptCount >= 1 }
        XCTAssertTrue(probeStarted)

        // Capture ends inside the probe's window: `start()` is left for the probe, but remembered.
        let leftAlone = Phase9BSyncBox<Int>(0)
        client.onStartLeftProbeAloneForTesting = { leftAlone.update { $0 += 1 } }
        client.start()
        let startSeen = await waitUntil(timeout: 2.0) { leftAlone.read() >= 1 }
        XCTAssertTrue(startSeen)

        // The probe times out and fails; because `start()` was asked for, the live view comes back on its own.
        let probeFailed = await waitUntil(timeout: 4.0) {
            !vm.pairingInProgress && vm.pairingError == PetCameraUserStrings.candidateLinkUnreachableSentence
        }
        XCTAssertTrue(probeFailed)
        let reconnectScheduled = await waitUntil(timeout: 2.0) { scheduledReconnects.read() >= 1 }
        XCTAssertTrue(reconnectScheduled, "A start() asked for during the probe must reconnect once the probe fails")
        let reconnected = await waitUntil(timeout: 5.0) { vm.telemetry.isConnected && vm.isPetOwner }
        XCTAssertTrue(reconnected, "The live view comes back after the reconnect")
    }

    // MARK: - a playing clip under the snapshot cover draws nothing new

    func testFollowup2b_PlayingClipUnderCoverDrawsNoFrames() async throws {
        let host = try FakeV3CameraRollAndInviteHost()
        defer { host.shutdown() }
        host.seedClips([
            (day: "2026-11-19", clipId: "2026-11-19/18-05-00_manual", startUnixMs: 1_795_111_500_000, startLocal: "2026-11-19T18:05:00+00:00", durationSec: 12.0, trigger: .manual, startedBy: "Owner", starredByIds: [], hasThumb: true)
        ])

        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Cover iPhone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        let roll = vm.cameraRoll
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let ready = await waitUntil { vm.isPetOwner && roll.supportsCameraRoll }
        XCTAssertTrue(ready)

        await roll.refreshDays(force: true)
        let clip = try XCTUnwrap(roll.selectedDayClips.first)
        roll.startPlayback(clip: clip)
        let playing = await waitUntil {
            roll.playbackState?.state == .playing && vm.telemetry.decodedPlaybackFrames >= 3
        }
        XCTAssertTrue(playing)
        let shownBeforeCover = roll.playbackDisplayBridge.framesHandedToRendererCount
        XCTAssertGreaterThan(shownBeforeCover, 0, "Frames reach the player while uncovered")

        roll.togglePlayPause()
        let paused = await waitUntil { roll.playbackState?.state == .paused }
        XCTAssertTrue(paused)

        // Cover (app inactive), then let more frames arrive under it.
        roll.setHiddenForSnapshot(true)
        let decodedAtCover = vm.telemetry.decodedPlaybackFrames
        roll.togglePlayPause()
        let moreFramesUnderCover = await waitUntil { vm.telemetry.decodedPlaybackFrames >= decodedAtCover + 2 }
        XCTAssertTrue(moreFramesUnderCover, "The host must keep sending frames while covered")
        XCTAssertEqual(
            roll.playbackDisplayBridge.framesHandedToRendererCount,
            shownBeforeCover,
            "No frame may reach the player while the cover is up"
        )

        // Uncovered, frames draw again.
        roll.setHiddenForSnapshot(false)
        roll.togglePlayPause()
        _ = await waitUntil { roll.playbackState?.state == .paused }
        roll.togglePlayPause()
        let drawsAgain = await waitUntil { roll.playbackDisplayBridge.framesHandedToRendererCount > shownBeforeCover }
        XCTAssertTrue(drawsAgain, "Frames reach the player again once uncovered")
    }

    /// Control Center / a notification banner / the app switcher: willResignActive then didBecomeActive with no
    /// backgrounding. A live session must be kept as it is: no new connect cycle, and the app must never read
    /// disconnected in between (that drops the live stream and owner state for a moment).
    func testBriefInterruptionKeepsTheLiveSessionWithoutReconnecting() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true)
        defer { host.shutdown() }
        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(),
                                                deviceName: "Interrupted iPhone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let live = await waitUntil { vm.telemetry.isConnected && vm.isPetOwner }
        XCTAssertTrue(live)
        try? await Task.sleep(nanoseconds: 300_000_000)  // let the session settle

        let attempts = Phase9BSyncBox<[String]>([])
        client.onConnectAttemptForTesting = { _, ep in attempts.update { $0.append(ep) } }
        let sawDisconnected = Phase9BSyncBox<Bool>(false)
        let sub = vm.$telemetry.sink { t in if !t.isConnected { sawDisconnected.update { $0 = true } } }
        defer { sub.cancel() }

        vm.handleWillResignActive()
        vm.handleDidBecomeActive()
        try? await Task.sleep(nanoseconds: 1_000_000_000)

        XCTAssertEqual(attempts.read(), [], "Becoming active again must not start a new connect cycle")
        XCTAssertFalse(sawDisconnected.read(), "The app must never read disconnected across a brief interruption")
        XCTAssertTrue(vm.telemetry.isConnected && vm.isPetOwner)
    }

    private func pairedLiveClient(_ host: FakeV3CameraRollAndInviteHost) async throws -> (PocketStreamClientCore, PocketViewModel) {
        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(),
                                                deviceName: "Interrupted iPhone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let live = await waitUntil { vm.telemetry.isConnected && vm.isPetOwner }
        XCTAssertTrue(live)
        try? await Task.sleep(nanoseconds: 300_000_000)
        return (client, vm)
    }

    /// The session died quietly while the app was inactive (NAT timeout, PC restart with no FIN): nothing has
    /// arrived for longer than the silence limit, so becoming active reconnects at once instead of keeping a stale
    /// "connected".
    func testBriefInterruptionReconnectsAtOnceWhenTheSessionWentSilent() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true)
        defer { host.shutdown() }
        let (client, vm) = try await pairedLiveClient(host)
        defer { client.stop() }
        let silent = Phase9BSyncBox<Int>(0)
        client.onResumeFoundSilentSessionForTesting = { silent.update { $0 += 1 } }
        let attempts = Phase9BSyncBox<Int>(0)
        client.onConnectAttemptForTesting = { _, _ in attempts.update { $0 += 1 } }

        vm.handleWillResignActive()
        client.simulateInboundSilenceForTesting()
        vm.handleDidBecomeActive()

        let redialled = await waitUntil(timeout: 2.0) { silent.read() == 1 && attempts.read() >= 1 }
        XCTAssertTrue(redialled, "A silent session must be redialled on become-active (silent \(silent.read()), attempts \(attempts.read()))")
        let back = await waitUntil(timeout: 5.0) { vm.telemetry.isConnected && vm.isPetOwner }
        XCTAssertTrue(back, "The fresh session comes up")
    }

    /// Kept on become-active, but the line goes silent right after: the follow-up check one silence-limit later
    /// notices and reconnects.
    func testAResumedSessionThatGoesSilentRightAfterIsRedialledByTheFollowUpCheck() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true)
        defer { host.shutdown() }
        let (client, vm) = try await pairedLiveClient(host)
        defer { client.stop() }
        client.resumeMaxInboundSilenceSec = 0.5
        let kept = Phase9BSyncBox<Int>(0)
        let silent = Phase9BSyncBox<Int>(0)
        client.onResumeKeptLiveSessionForTesting = { kept.update { $0 += 1 } }
        client.onResumeFoundSilentSessionForTesting = { silent.update { $0 += 1 } }
        let attempts = Phase9BSyncBox<Int>(0)
        client.onConnectAttemptForTesting = { _, _ in attempts.update { $0 += 1 } }

        vm.handleWillResignActive()
        vm.handleDidBecomeActive()
        let wasKept = await waitUntil(timeout: 1.0) { kept.read() == 1 }
        XCTAssertTrue(wasKept)
        XCTAssertEqual(attempts.read(), 0)
        client.simulateInboundSilenceForTesting()

        let redialled = await waitUntil(timeout: 2.0) { silent.read() == 1 && attempts.read() >= 1 }
        XCTAssertTrue(redialled, "The follow-up check must redial a session that went silent (silent \(silent.read()), attempts \(attempts.read()))")
        let back = await waitUntil(timeout: 5.0) { vm.telemetry.isConnected && vm.isPetOwner }
        XCTAssertTrue(back)
    }
}

/// Short-code pairing can't tell the real PC from an impostor on the same network, so it needs the person to confirm
/// they're on their own Wi-Fi first.
@MainActor
final class ShortCodeTrustedNetworkTests: XCTestCase {
    func testShortCodeIsNotSubmittedWithoutTheTrustedNetworkConfirmation() throws {
        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Code Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        vm.shortCodeInput = "1234-5678"
        XCTAssertFalse(vm.shortCodeNetworkConfirmed, "Off until the person confirms")
        vm.submitShortCode()
        XCTAssertFalse(vm.pairingInProgress, "No short-code pairing without the confirmation")
        vm.shortCodeNetworkConfirmed = true
        vm.submitShortCode()
        XCTAssertTrue(vm.pairingInProgress, "With the confirmation, pairing starts")
    }

    func testTheWarningSaysWhyAndPointsToTheQRCode() {
        let w = PetCameraUserStrings.shortCodeTrustedNetworkWarning
        XCTAssertTrue(w.contains("own home Wi-Fi"))
        XCTAssertTrue(w.contains("pretend to be your PC"))
        XCTAssertTrue(w.contains("QR code is safer"))
    }
}
