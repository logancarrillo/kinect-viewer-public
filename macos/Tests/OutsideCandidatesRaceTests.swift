import Foundation
import Network
import XCTest
@testable import KinectViewer

/// Several outside routes to the PC (the typed public address plus 0x0B `outsideCandidates`, e.g. a Tailscale
/// address): a staggered race of pinned-TLS dials where the first to finish TLS with the pinned certificate wins.
/// Synthetic only, on loopback; no real address appears here.
@MainActor
final class OutsideCandidatesRaceTests: XCTestCase {

    private func waitUntil(timeout: TimeInterval = 5.0, pollMs: UInt64 = 20,
                           condition: @escaping @MainActor () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return true }
            try? await Task.sleep(nanoseconds: pollMs * 1_000_000)
        }
        return condition()
    }

    /// Pairs at home with the given main outside endpoint and candidates advertised, then takes home away.
    private func pairThenLeaveHome(
        host: FakeV3CameraRollAndInviteHost,
        main: PetEndpoint,
        candidates: [PetEndpoint]
    ) async throws -> (PocketStreamClientCore, PocketViewModel, InMemoryPersistenceProvider) {
        host.setAdvertisedOutsideEndpoint(main, outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates(candidates)
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Race Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let learned = await waitUntil {
            persistence.rawStoredForTesting?.outsideCandidates == candidates && vm.telemetry.isConnected
        }
        XCTAssertTrue(learned, "0x0B outsideCandidates must be stored (got \(String(describing: persistence.rawStoredForTesting?.outsideCandidates)))")
        try host.setLanStreamEnabled(false)
        return (client, vm, persistence)
    }

    func testAStalledMainAddressLosesTheRaceToAWorkingCandidate() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }
        let (client, vm, _) = try await pairThenLeaveHome(
            host: host,
            main: PetEndpoint(host: "127.0.0.1", port: stalled.port, verified: false),
            candidates: [PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false)]
        )
        defer { client.stop() }
        let won = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(won, "\(vm.recentConnectAttemptLines)")
        XCTAssertGreaterThanOrEqual(stalled.acceptCount, 1, "The stalled main address was raced too")
        let winningRow = vm.telemetry.recentConnectAttempts.last(where: { $0.path == .outside && $0.result == "reached" })
        XCTAssertEqual(winningRow?.endpoint.hasSuffix(":\(host.outsidePort)"), true, "The row shows the candidate that won")
    }

    func testAStaleCandidateThatRefusesDoesNotStopTheRaceWhenAnotherConnects() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        // A different PC's certificate: a pin mismatch, i.e. a refusal after the certificate.
        let stranger = try CloseBeforeSessionTLSServer(serverIdentity: try CryptoIdentity.generateNew(commonName: "other-pc"))
        defer { stranger.shutdown() }
        let (client, vm, _) = try await pairThenLeaveHome(
            host: host,
            main: PetEndpoint(host: "127.0.0.1", port: stranger.port, verified: false),
            candidates: [PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false)]
        )
        defer { client.stop() }
        let won = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(won, "\(vm.recentConnectAttemptLines)")
        XCTAssertNil(vm.telemetry.tlsRefusalNotice, "One candidate's refusal is not a stop when another connected")
    }

    func testWhenEveryCandidateRefusesItStopsAsBefore() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let strangerA = try CloseBeforeSessionTLSServer(serverIdentity: try CryptoIdentity.generateNew(commonName: "other-pc-a"))
        defer { strangerA.shutdown() }
        let strangerB = try CloseBeforeSessionTLSServer(serverIdentity: try CryptoIdentity.generateNew(commonName: "other-pc-b"))
        defer { strangerB.shutdown() }
        let (client, vm, _) = try await pairThenLeaveHome(
            host: host,
            main: PetEndpoint(host: "127.0.0.1", port: strangerA.port, verified: false),
            candidates: [PetEndpoint(host: "127.0.0.1", port: strangerB.port, verified: false)]
        )
        defer { client.stop() }
        let refused = await waitUntil(timeout: 8.0) { vm.telemetry.tlsRefusalNotice != nil }
        XCTAssertTrue(refused, "Every candidate refused: recorded as a refusal, as with a single address (\(vm.recentConnectAttemptLines))")
        XCTAssertFalse(vm.telemetry.isConnected)
    }

    /// A losing racer's "iOS is blocking the network" sentence must not outlive a winner.
    func testAWinClearsABlockedNetworkSentenceFromALosingRacer() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: stalled.port, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates([PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false)])
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Race Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.outsideRaceStaggerSec = 1.0 // time to set the sentence before the winner starts
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates != nil && vm.telemetry.isConnected }
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        try host.setLanStreamEnabled(false)
        let raceStarted = await waitUntil(timeout: 5.0) { stalled.acceptCount >= 1 }
        XCTAssertTrue(raceStarted)
        client.setNetworkBlockedSentenceForTesting(PetCameraUserStrings.cellularDataOffSentence)
        let shown = await waitUntil(timeout: 2.0) { vm.disconnectedStatusTitle == PetCameraUserStrings.cellularDataOffSentence }
        XCTAssertTrue(shown, "The losing racer's sentence leads the banner first")

        let won = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(won, "\(vm.recentConnectAttemptLines)")
        XCTAssertNil(vm.telemetry.networkBlockedSentence, "A win clears a losing racer's blocked-network sentence")
    }

    /// After a stall, only the typed address's racers are pinned to cellular; an extra candidate (e.g. a VPN's
    /// 100.x) keeps its own route.
    func testACellularPinAppliesOnlyToTheTypedAddressRacers() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalledA = try StalledLoopbackTCPServer()
        defer { stalledA.shutdown() }
        let stalledB = try StalledLoopbackTCPServer()
        defer { stalledB.shutdown() }
        let (client, vm, _) = try await pairThenLeaveHome(
            host: host,
            main: PetEndpoint(host: "127.0.0.1", port: stalledA.port, verified: false),
            candidates: [PetEndpoint(host: "127.0.0.1", port: stalledB.port, verified: false)]
        )
        defer { client.stop() }
        client.outsideAttemptDeadlineSec = 1.0
        // Wait out one stalled race; the next race is the pinned one.
        // The first race already started with the default 12 s deadline; the pinned race follows it.
        let pinnedRaceSeen = await waitUntil(timeout: 20.0) {
            vm.telemetry.recentConnectAttempts.contains(where: { $0.endpoint.hasSuffix("[cellular only]") })
        }
        XCTAssertTrue(pinnedRaceSeen, "\(vm.recentConnectAttemptLines)")
        let pinnedRows = vm.telemetry.recentConnectAttempts.filter { $0.endpoint.hasSuffix("[cellular only]") }
        XCTAssertTrue(pinnedRows.allSatisfy { $0.endpoint.contains(":\(stalledA.port)") },
                      "Only the typed address is pinned: \(vm.recentConnectAttemptLines)")
    }

    /// Never more than one dial in flight per host: a host's second address starts only after its first has ended.
    func testOnlyOneDialInFlightPerHost() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let firstAddress = try StalledLoopbackTCPServer()   // never answers TLS: stays in flight
        defer { firstAddress.shutdown() }
        let secondAddress = try StalledLoopbackTCPServer()
        defer { secondAddress.shutdown() }
        let mainPort: UInt16 = 40000 // documentation port: the resolver hook maps it to the two loopback servers
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: mainPort, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates([PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false)])
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Race Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.outsideRaceStaggerSec = 0.5
        let a1 = NWEndpoint.hostPort(host: .ipv4(IPv4Address("127.0.0.1")!), port: NWEndpoint.Port(rawValue: firstAddress.port)!)
        let a2 = NWEndpoint.hostPort(host: .ipv4(IPv4Address("127.0.0.1")!), port: NWEndpoint.Port(rawValue: secondAddress.port)!)
        let live = NWEndpoint.hostPort(host: .ipv4(IPv4Address("127.0.0.1")!), port: NWEndpoint.Port(rawValue: host.outsidePort)!)
        client.resolveOutsideCandidatesForTesting = { _, port in port == mainPort ? [a1, a2] : [live] }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates != nil && vm.telemetry.isConnected }
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        try host.setLanStreamEnabled(false)

        let won = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(won, "\(vm.recentConnectAttemptLines)")
        XCTAssertGreaterThanOrEqual(firstAddress.acceptCount, 1)
        XCTAssertEqual(secondAddress.acceptCount, 0, "The host's second address must not start while its first is in flight")
    }

    /// The stored candidate list is capped at 4 and replaced whole on every 0x0B; an absent field clears it.
    func testCandidateListIsCappedAndReplacedWhole() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        let six = (1...6).map { PetEndpoint(host: "127.0.0.1", port: UInt16(40000 + $0), verified: false) }
        host.setAdvertisedOutsideCandidates(six)
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Race Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let capped = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates == Array(six.prefix(4)) }
        XCTAssertTrue(capped, "Capped at 4: \(String(describing: persistence.rawStoredForTesting?.outsideCandidates))")
        host.setAdvertisedOutsideCandidates([six[5]])
        let replaced = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates == [six[5]] }
        XCTAssertTrue(replaced, "Replaced whole")
        host.setAdvertisedOutsideCandidates(nil)
        let cleared = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates == nil }
        XCTAssertTrue(cleared, "An absent field clears it")
        _ = vm
    }

    /// A stalled first address gives way to its host's next address after a few seconds, inside the race deadline.
    func testAStalledFirstAddressGivesWayToTheHostsNextAddress() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalledFirst = try StalledLoopbackTCPServer()
        defer { stalledFirst.shutdown() }
        let stalledOther = try StalledLoopbackTCPServer()
        defer { stalledOther.shutdown() }
        let mainPort: UInt16 = 40000 // documentation port, mapped by the resolver hook
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: mainPort, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates([PetEndpoint(host: "127.0.0.1", port: stalledOther.port, verified: false)])
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Race Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.outsideRaceAddressTimeoutSec = 1.0
        let lo = IPv4Address("127.0.0.1")!
        let first = NWEndpoint.hostPort(host: .ipv4(lo), port: NWEndpoint.Port(rawValue: stalledFirst.port)!)
        let live = NWEndpoint.hostPort(host: .ipv4(lo), port: NWEndpoint.Port(rawValue: host.outsidePort)!)
        let other = NWEndpoint.hostPort(host: .ipv4(lo), port: NWEndpoint.Port(rawValue: stalledOther.port)!)
        client.resolveOutsideCandidatesForTesting = { _, port in port == mainPort ? [first, live] : [other] }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates != nil && vm.telemetry.isConnected }
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        try host.setLanStreamEnabled(false)
        // Well inside the 12 s race deadline.
        let won = await waitUntil(timeout: 5.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(won, "\(vm.recentConnectAttemptLines)")
        XCTAssertTrue(vm.telemetry.recentConnectAttempts.contains(where: { $0.result.hasPrefix("no answer in 1.0 s (next address)") }))
    }

    /// stop() in a gap between racers starting: the race's deadline must not fire later and set the pin or "unreachable".
    func testStopDuringARaceGapLeavesNoPinAndNoUnreachable() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalledA = try StalledLoopbackTCPServer()
        defer { stalledA.shutdown() }
        let stalledB = try StalledLoopbackTCPServer()
        defer { stalledB.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: stalledA.port, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates([PetEndpoint(host: "127.0.0.1", port: stalledB.port, verified: false)])
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Race Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.outsideRaceStaggerSec = 2.0
        client.outsideAttemptDeadlineSec = 1.0
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates != nil && vm.telemetry.isConnected }
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        try host.setLanStreamEnabled(false)
        let firstRacerUp = await waitUntil(timeout: 5.0) { stalledA.acceptCount >= 1 }
        XCTAssertTrue(firstRacerUp)
        client.stop() // the second racer hasn't started yet: a gap
        try? await Task.sleep(nanoseconds: 4_000_000_000) // past stagger + deadline
        XCTAssertFalse(client.pinNextOutsideDialToCellular, "A stopped client's race must not set the pin")
        XCTAssertNotEqual(vm.telemetry.lastError, PetCameraUserStrings.unreachableDefaultPC)
        XCTAssertEqual(stalledB.acceptCount, 0, "The stopped race's second racer never starts")
    }

    func testVPNRangeDetection() {
        XCTAssertTrue(PocketStreamClientCore.isVPNRangeIPv4("100.64.0.1"))
        XCTAssertTrue(PocketStreamClientCore.isVPNRangeIPv4("100.127.255.254"))
        XCTAssertFalse(PocketStreamClientCore.isVPNRangeIPv4("100.63.0.1"))
        XCTAssertFalse(PocketStreamClientCore.isVPNRangeIPv4("100.128.0.1"))
        XCTAssertFalse(PocketStreamClientCore.isVPNRangeIPv4("203.0.113.5"))
        XCTAssertFalse(PocketStreamClientCore.isVPNRangeIPv4("fd7a::1"))
    }

    /// A phone with no VPN up (e.g. one that isn't on the tailnet) leaves a 100.64/10 candidate out entirely and runs
    /// the single public path, with a row saying why.
    func testAVPNRangeCandidateIsSkippedWithoutAVPN() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates([PetEndpoint(host: "100.64.0.1", port: 40000, verified: false)])
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "No VPN Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.vpnUpOverrideForTesting = false
        let modes = Phase9BSyncBox<[String]>([])
        client.onOutsideDialModeForTesting = { m in modes.update { $0.append(m) } }
        let dialled = Phase9BSyncBox<[String]>([])
        client.onConnectAttemptForTesting = { origin, ep in if origin == .outside { dialled.update { $0.append(ep) } } }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates != nil && vm.telemetry.isConnected }
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        try host.setLanStreamEnabled(false)
        let won = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(won, "\(vm.recentConnectAttemptLines)")
        XCTAssertFalse(dialled.read().contains(where: { $0.hasPrefix("100.64.0.1") }), "Never dialled: \(dialled.read())")
        XCTAssertEqual(modes.read().last, "single", "With the overlay skipped, the single-address path runs, not a one-group race")
        XCTAssertTrue(vm.telemetry.recentConnectAttempts.contains(where: {
            $0.endpoint.hasPrefix("100.64.0.1") && $0.result == "skipped (no VPN)"
        }), "\(vm.recentConnectAttemptLines)")
    }

    /// With a VPN up, the 100.64/10 candidate is raced.
    func testAVPNRangeCandidateIsRacedWithAVPN() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates([PetEndpoint(host: "100.64.0.1", port: 40000, verified: false)])
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "VPN Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.vpnUpOverrideForTesting = true
        // No stagger: the local primary answers in well under 50 ms, so with any stagger it can win before the
        // candidate's dial starts. At 0 both dials are queued before either can finish.
        client.outsideRaceStaggerSec = 0
        let dialled = Phase9BSyncBox<[String]>([])
        client.onConnectAttemptForTesting = { origin, ep in if origin == .outside { dialled.update { $0.append(ep) } } }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates != nil && vm.telemetry.isConnected }
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        try host.setLanStreamEnabled(false)
        let raced = await waitUntil(timeout: 8.0) { dialled.read().contains(where: { $0.hasPrefix("100.64.0.1") }) }
        XCTAssertTrue(raced, "With a VPN up the candidate is raced: \(dialled.read())")
        XCTAssertFalse(vm.telemetry.recentConnectAttempts.contains(where: { $0.result == "skipped (no VPN)" }))
    }

    /// The VPN check runs at every connect cycle: a VPN that goes down mid-session makes the next cycle skip the
    /// overlay candidate and take the single-address path.
    func testAVPNGoingDownMidSessionMakesTheNextCycleSkipTheCandidate() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        host.setAdvertisedOutsideEndpoint(PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false),
                                          outsideEnabled: true, outsideStatus: "manual")
        host.setAdvertisedOutsideCandidates([PetEndpoint(host: "100.64.0.1", port: 40000, verified: false)])
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "VPN Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        client.outsideRaceStaggerSec = 0.05
        client.vpnUpOverrideForTesting = true
        let modes = Phase9BSyncBox<[String]>([])
        client.onOutsideDialModeForTesting = { m in modes.update { $0.append(m) } }
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { persistence.rawStoredForTesting?.outsideCandidates != nil && vm.telemetry.isConnected }
        client.onReconnectScheduledForTesting = { delay, fire in if delay < 5.0 { fire() } }
        try host.setLanStreamEnabled(false)
        let outside = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected && vm.telemetry.petState.path == .outside }
        XCTAssertTrue(outside)
        XCTAssertEqual(modes.read().last, "race", "VPN up: the overlay candidate is raced")

        // The VPN goes down, and the session drops: the next cycle checks again and skips the candidate.
        client.vpnUpOverrideForTesting = false
        host.closeAllSessions()
        let skipped = await waitUntil(timeout: 8.0) {
            modes.read().last == "single" &&
                vm.telemetry.recentConnectAttempts.contains(where: { $0.endpoint.hasPrefix("100.64.0.1") && $0.result == "skipped (no VPN)" })
        }
        XCTAssertTrue(skipped, "modes \(modes.read()); \(vm.recentConnectAttemptLines)")
    }

    func testTheSameResolvedAddressIsNeverRacedTwice() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: true)
        defer { host.shutdown() }
        let stalled = try StalledLoopbackTCPServer()
        defer { stalled.shutdown() }
        let same = PetEndpoint(host: "127.0.0.1", port: stalled.port, verified: false)
        let (client, vm, _) = try await pairThenLeaveHome(
            host: host, main: same,
            candidates: [same, PetEndpoint(host: "127.0.0.1", port: host.outsidePort, verified: false)]
        )
        defer { client.stop() }
        _ = await waitUntil(timeout: 8.0) { vm.telemetry.isConnected }
        let stalledRows = vm.telemetry.recentConnectAttempts.filter { $0.path == .outside && $0.endpoint.hasSuffix(":\(stalled.port)") }
        let distinct = Set(stalledRows.map(\.endpoint))
        XCTAssertEqual(stalledRows.count, distinct.count, "No address raced twice: \(vm.recentConnectAttemptLines)")
    }

    func testANetworkPathChangeRestartsAWaitingCycle() async throws {
        let host = try FakeV3CameraRollAndInviteHost(startLanStream: true, startOutsideListener: false)
        defer { host.shutdown() }
        let persistence = InMemoryPersistenceProvider()
        let client = try PocketStreamClientCore(persistence: persistence, deviceName: "Path Phone", deviceKind: "iphone")
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: false)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.prepareLanPairingOffer(role: .owner)) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        _ = await waitUntil { vm.telemetry.isConnected }
        let delays = Phase9BSyncBox<[Double]>([])
        client.onReconnectScheduledForTesting = { d, _ in delays.update { $0.append(d) } } // hold every back-off
        try host.setLanStreamEnabled(false)
        _ = await waitUntil { !delays.read().isEmpty && !vm.telemetry.isConnected }

        let restarted = Phase9BSyncBox<Int>(0)
        client.onPathChangeRestartForTesting = { restarted.update { $0 += 1 } }
        try host.setLanStreamEnabled(true)
        client.simulateNetworkPathChangeForTesting(signature: "satisfied|cellular")
        client.simulateNetworkPathChangeForTesting(signature: "satisfied|wifi")
        let back = await waitUntil(timeout: 5.0) { restarted.read() >= 1 && vm.telemetry.isConnected }
        XCTAssertTrue(back, "Arriving home (a path change) must restart the cycle at once, not wait out the back-off")
    }
}
