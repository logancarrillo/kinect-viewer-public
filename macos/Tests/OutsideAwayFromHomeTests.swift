import Foundation
import Network
import XCTest
@testable import KinectViewer

/// Away from home: a phone paired at home learns the PC's outside address from 0x0B (the PC user's typed address,
/// unverified, as a host with no UPnP sends it), keeps it across an app restart, and dials it when home can't be reached.
/// Synthetic only: a loopback fake host. "Away" is the host's LAN listener turned off.
/// Accepts TCP on 127.0.0.1 and closes at once, before any TLS: the shape of the host's connection limiter.
final class LimiterCloseTCPServer: @unchecked Sendable {
    private let queue = DispatchQueue(label: "kinectviewer.tests.limiterclose")
    private var listener: NWListener?
    private let acceptCountBox = Phase9BSyncBox<Int>(0)
    private(set) var port: UInt16 = 0
    var acceptCount: Int { acceptCountBox.read() }

    init() throws {
        let ready = DispatchSemaphore(value: 0)
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        let l = try NWListener(using: params)
        l.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.port = l.port?.rawValue ?? 0
                ready.signal()
            }
        }
        l.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            self.acceptCountBox.update { $0 += 1 }
            conn.stateUpdateHandler = { st in
                if case .ready = st { conn.cancel() }
            }
            conn.start(queue: self.queue)
        }
        listener = l
        l.start(queue: queue)
        _ = ready.wait(timeout: .now() + 5.0)
    }

    func shutdown() {
        queue.sync {
            listener?.cancel()
            listener = nil
        }
    }
}

@MainActor
final class OutsideAwayFromHomeTests: XCTestCase {

    private func waitUntil(
        timeout: TimeInterval = 5.0,
        pollMs: UInt64 = 20,
        condition: @escaping @MainActor () -> Bool
    ) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return true }
            try? await Task.sleep(nanoseconds: pollMs * 1_000_000)
        }
        return condition()
    }

    func testPairedAtHomeThenAwayDialsTheTypedOutsideAddressAfterRestart() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        // A PC with no UPnP: status "manual", the typed public address advertised unverified.
        let typed = PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false)
        host.setAdvertisedOutsideEndpoint(typed, outsideEnabled: true, outsideStatus: "manual")

        let persistence = InMemoryPersistenceProvider()

        // At home: pair over the LAN, and get a 0x0B carrying the address.
        do {
            let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Logy", deviceKind: "iphone")
            client.disableBonjourForTesting = true
            let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
            let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
                client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
            }
            XCTAssertNoThrow(try pairRes.get())
            let homeSession = await waitUntil {
                vm.isPetOwner && vm.telemetry.isConnected && vm.telemetry.petState.path == .home
            }
            XCTAssertTrue(homeSession)
            let learned = await waitUntil { persistence.rawStoredForTesting?.outsideEndpoint == typed }
            XCTAssertTrue(learned, "The typed outside address from 0x0B must be stored (got \(String(describing: persistence.rawStoredForTesting?.outsideEndpoint)))")
            client.stop()
        }

        // Away: home can't be reached any more, and the app is started fresh from what it stored.
        try host.setLanStreamEnabled(false)
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Logy", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let attempts = Phase9BSyncBox<[PetPath]>([])
        client.onConnectAttemptForTesting = { origin, _ in attempts.update { $0.append(origin) } }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        client.start()

        let dialledOutside = await waitUntil(timeout: 8.0) { attempts.read().contains(.outside) }
        XCTAssertTrue(dialledOutside, "Away from home the app must dial the outside address (attempts: \(attempts.read()))")
        let outsideSession = await waitUntil(timeout: 8.0) {
            vm.telemetry.isConnected && vm.telemetry.petState.path == .outside
        }
        XCTAssertTrue(outsideSession, "The app must reach the PC from outside (state: \(vm.telemetry.connectionState))")

        // The "can't reach" screen's diagnostics: the stored address in plain words, and the attempts with their results.
        XCTAssertEqual(vm.awayFromHomeAddressText, "Away from home: 127.0.0.1:\(host.outsidePort) (typed in on the PC)")
        let outsideAttempt = try XCTUnwrap(vm.telemetry.recentConnectAttempts.last(where: { $0.path == .outside }))
        XCTAssertEqual(outsideAttempt.result, "reached")
        XCTAssertTrue(vm.telemetry.recentConnectAttempts.contains(where: { $0.path == .home && $0.result != "reached" }),
                      "The failed home attempt is listed too: \(vm.recentConnectAttemptLines)")
        XCTAssertLessThanOrEqual(vm.telemetry.recentConnectAttempts.count, PocketClientTelemetry.maxRecentConnectAttempts)
        XCTAssertTrue(vm.recentConnectAttemptLines.first?.contains("outside 127.0.0.1:\(host.outsidePort) — reached") == true,
                      "Newest first: \(vm.recentConnectAttemptLines)")
    }

    /// The host's limiter closes a connection before TLS. That must read as "busy, try again shortly": a pause of
    /// about 10 s, then a retry, and never a refusal that would stop viewing until the app is reopened.
    func testClosedByTheLimiterBeforeTLSRetriesAfterAPauseWithNoRefusal() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let limiter = try LimiterCloseTCPServer()
        defer { limiter.shutdown() }
        // The PC's typed address leads to the limiter (as when the phone's carrier address is blocked).
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: limiter.port, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")

        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Logy", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let learned = await waitUntil { persistence.rawStoredForTesting?.outsideEndpoint?.port == limiter.port }
        XCTAssertTrue(learned)

        let delays = Phase9BSyncBox<[Double]>([])
        // Record every reconnect; run the short ones (home dropped) so the app goes on to the outside address,
        // and hold the long one (the limiter pause) so the test can read it.
        client.onReconnectScheduledForTesting = { delay, fire in
            delays.update { $0.append(delay) }
            if delay < 5.0 { fire() }
        }
        try host.setLanStreamEnabled(false)

        let limited = await waitUntil(timeout: 8.0) { limiter.acceptCount >= 1 && (delays.read().last ?? 0) >= 5.0 }
        XCTAssertTrue(limited, "The app must reach the limiter and schedule a retry (accepts \(limiter.acceptCount), delays \(delays.read()))")
        XCTAssertGreaterThanOrEqual(delays.read().last ?? 0, 10.0, "A limiter close waits about 10 s before retrying")
        XCTAssertNil(vm.telemetry.tlsRefusalNotice, "A close before TLS is not a refusal")
        XCTAssertEqual(vm.telemetry.lastError, PetCameraUserStrings.pcBusyTryAgainShortlySentence)
        XCTAssertTrue(vm.telemetry.recentConnectAttempts.contains(where: {
            $0.path == .outside && $0.result == "closed by the PC before TLS (busy)"
        }), "\(vm.recentConnectAttemptLines)")
    }

    func testBlockedNetworkReasonsSayWhatToTurnOn() async throws {
        XCTAssertEqual(PocketStreamClientCore.blockedNetworkSentence(for: .cellularDenied),
                       PetCameraUserStrings.cellularDataOffSentence)
        XCTAssertEqual(PocketStreamClientCore.blockedNetworkSentence(for: .wifiDenied),
                       PetCameraUserStrings.wifiOffForAppSentence)
        XCTAssertEqual(PocketStreamClientCore.blockedNetworkSentence(for: .localNetworkDenied),
                       PetCameraUserStrings.localNetworkOffSentence)
        XCTAssertNil(PocketStreamClientCore.blockedNetworkSentence(for: .notAvailable))

        // The "can't reach" screen leads with it, instead of the generic line.
        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Logy", deviceKind: "iphone")
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        var t = vm.telemetry
        t.cachedHostProtocolVersion = 3
        t.networkBlockedSentence = PetCameraUserStrings.cellularDataOffSentence
        vm.telemetry = t
        XCTAssertEqual(vm.disconnectedStatusTitle, PetCameraUserStrings.cellularDataOffSentence)
    }

    /// On an IPv6-only carrier an IPv4 literal must go through the resolver (NAT64 synthesis), so the outside address
    /// is always passed as a host name, never as an `.ipv4` address.
    func testOutsideIPv4LiteralIsDialledAsAHostNameNotAnIPv4Address() throws {
        let port = try XCTUnwrap(NWEndpoint.Port(rawValue: 40000))
        guard case let .hostPort(host, p) = PocketStreamClientCore.outsideEndpoint(host: "203.0.113.5", port: port) else {
            return XCTFail("expected a host:port endpoint")
        }
        XCTAssertEqual(p, port)
        if case let .name(name, _) = host {
            XCTAssertEqual(name, "203.0.113.5")
        } else {
            XCTFail("An IPv4-literal outside address must be a host name, got \(host)")
        }
        // The plain initializer is what made it .ipv4 before.
        if case .ipv4 = NWEndpoint.Host("203.0.113.5") {} else {
            XCTFail("Sanity: NWEndpoint.Host(\"203.0.113.5\") is expected to parse as .ipv4")
        }
    }

    /// An outside dial that gets nowhere (TCP accepted, TLS never answered; or a carrier that swallows the SYN) is given
    /// up at its deadline and retried with back-off, instead of sitting on "connecting".
    func testOutsideDialThatNeverAnswersIsGivenUpAtItsDeadline() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: stalled.port, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Logy", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.outsideAttemptDeadlineSec = 1.0
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let learned = await waitUntil { persistence.rawStoredForTesting?.outsideEndpoint?.port == stalled.port }
        XCTAssertTrue(learned)

        let delays = Phase9BSyncBox<[Double]>([])
        client.onReconnectScheduledForTesting = { delay, fire in
            delays.update { $0.append(delay) }
            if delays.read().count <= 1 { fire() } // run the home-drop reconnect, hold the next one
        }
        try host.setLanStreamEnabled(false)

        let gaveUp = await waitUntil(timeout: 6.0) {
            vm.telemetry.recentConnectAttempts.contains(where: { $0.path == .outside && $0.result == "no answer in 1.0 s" })
        }
        XCTAssertTrue(gaveUp, "The outside dial must be given up at its deadline: \(vm.recentConnectAttemptLines)")
        XCTAssertGreaterThanOrEqual(stalled.acceptCount, 1)
        let retried = await waitUntil(timeout: 2.0) { delays.read().count >= 2 }
        XCTAssertTrue(retried, "A back-off reconnect follows the deadline (delays \(delays.read()))")
        XCTAssertFalse(vm.telemetry.isConnected)
        // The stalled address is retried at once, pinned to cellular (this Mac has none, so that try ends at once).
        // The tag is added to the row just after it is created, and the view model's copy can lag by a publish: wait.
        let pinnedRetry = await waitUntil(timeout: 2.0) {
            vm.telemetry.recentConnectAttempts.contains(where: {
                $0.path == .outside && $0.endpoint.hasPrefix("127.0.0.1:\(stalled.port)") && $0.endpoint.hasSuffix("[cellular only]")
            })
        }
        XCTAssertTrue(pinnedRetry, "The stalled address is retried pinned to cellular: \(vm.recentConnectAttemptLines)")

        // A pinned dial gets exactly one try: the pin is clear again, and the next outside dial is a normal one.
        let unpinned = await waitUntil(timeout: 4.0) { !client.pinNextOutsideDialToCellular }
        XCTAssertTrue(unpinned, "A pinned dial that ends without connecting must clear the pin: \(vm.recentConnectAttemptLines)")
        client.onReconnectScheduledForTesting = { _, _ in }
        let attemptsBefore = vm.telemetry.recentConnectAttempts.map(\.id)
        client.start()
        let nextIsNormal = await waitUntil(timeout: 4.0) {
            vm.telemetry.recentConnectAttempts.contains(where: {
                !attemptsBefore.contains($0.id) && $0.path == .outside && !$0.endpoint.hasSuffix("[cellular only]")
            })
        }
        XCTAssertTrue(nextIsNormal, "After the pinned try, the next outside dial is unpinned: \(vm.recentConnectAttemptLines)")
    }

    /// getaddrinfo(AI_DEFAULT) on an IPv4 literal: on this IPv4 Mac it yields the IPv4 address (on a NAT64 carrier it
    /// yields the synthesized IPv6 first).
    func testOutsideAddressIsResolvedThroughGetaddrinfo() {
        let eps = PocketStreamClientCore.synthesizedOutsideEndpoints(host: "127.0.0.1", port: 40000)
        XCTAssertFalse(eps.isEmpty)
        XCTAssertTrue(eps.contains(where: {
            if case let .hostPort(.ipv4(a), p) = $0 { return "\(a)" == "127.0.0.1" && p.rawValue == 40000 }
            return false
        }), "\(eps)")
    }

    /// Each resolved outside address gets a try in the same cycle: a dead first one (IPv6 first, as NAT64 orders them)
    /// is followed by the next, which reaches the PC.
    func testEveryResolvedOutsideAddressIsTriedInTurn() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Logy", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let deadV6 = NWEndpoint.hostPort(host: .ipv6(IPv6Address("::1")!), port: 1)
        let live = NWEndpoint.hostPort(host: .ipv4(IPv4Address("127.0.0.1")!), port: NWEndpoint.Port(rawValue: host.outsidePort)!)
        client.resolveOutsideCandidatesForTesting = { _, _ in [deadV6, live] }
        client.onReconnectScheduledForTesting = { _, fire in fire() }
        let outsideTried = Phase9BSyncBox<[String]>([])
        client.onConnectAttemptForTesting = { origin, ep in if origin == .outside { outsideTried.update { $0.append(ep) } } }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideEndpoint != nil && vm.telemetry.isConnected }
        try host.setLanStreamEnabled(false)

        let reached = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(reached, "\(vm.recentConnectAttemptLines)")
        let tried = outsideTried.read()
        XCTAssertGreaterThanOrEqual(tried.count, 2, "\(tried)")
        XCTAssertEqual(tried.first, "\(deadV6)", "The IPv6 (synthesized) address is tried first: \(tried)")
        XCTAssertTrue(tried.contains("\(live)"), "\(tried)")
    }

    func testAttemptRowErrorsAreShortAndExact() {
        XCTAssertEqual(PocketStreamClientCore.shortErrorText(.posix(.ENETDOWN)), "POSIX 50 ENETDOWN")
        XCTAssertEqual(PocketStreamClientCore.shortErrorText(.posix(.ECONNRESET)), "POSIX 54 ECONNRESET")
        XCTAssertEqual(PocketStreamClientCore.shortErrorText(.tls(-9806)), "TLS -9806")
        XCTAssertEqual(PocketStreamClientCore.shortErrorText(.dns(-65554)), "DNS -65554")
    }

    func testNoStoredAddressSaysSoInPlainWords() async throws {
        let client = try PocketStreamClientCore(persistence: InMemoryPersistenceProvider(), deviceName: "Logy", deviceKind: "iphone")
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        XCTAssertTrue(vm.awayFromHomeAddressText.hasPrefix("Away from home: no address yet."))
    }

    /// What the owner did: watching at home on Wi-Fi, then Wi-Fi off. The live home session dies mid-stream and the app,
    /// still running, must find the PC at its outside address on its own.
    func testLiveHomeSessionDroppedThenReconnectsOutsideWithoutRestart() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let typed = PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false)
        host.setAdvertisedOutsideEndpoint(typed, outsideEnabled: true, outsideStatus: "manual")

        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Logy", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let attempts = Phase9BSyncBox<[PetPath]>([])
        client.onConnectAttemptForTesting = { origin, _ in attempts.update { $0.append(origin) } }
        // Run reconnects at once, so the test sees the path the app takes, not the back-off.
        client.onReconnectScheduledForTesting = { _, fire in fire() }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let watchingAtHome = await waitUntil {
            vm.telemetry.isConnected && vm.telemetry.petState.path == .home &&
                persistence.rawStoredForTesting?.outsideEndpoint == typed
        }
        XCTAssertTrue(watchingAtHome)

        // Wi-Fi off: the home session goes, and home can't be reached again.
        attempts.update { $0.removeAll() }
        try host.setLanStreamEnabled(false)

        let dialledOutside = await waitUntil(timeout: 8.0) { attempts.read().contains(.outside) }
        XCTAssertTrue(dialledOutside, "After the home session drops, the app must dial the outside address (attempts: \(attempts.read()))")
        let outsideSession = await waitUntil(timeout: 8.0) {
            vm.telemetry.isConnected && vm.telemetry.petState.path == .outside
        }
        XCTAssertTrue(outsideSession, "The app must be watching from outside (state: \(vm.telemetry.connectionState))")
    }
}
