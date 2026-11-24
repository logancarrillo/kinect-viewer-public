import Combine
import CoreGraphics
import Foundation
#if canImport(UIKit)
import UIKit
#else
@testable import KinectViewer
#endif

enum PocketModeChoice: Int, CaseIterable, Identifiable {
    case ir = 0
    case color = 1
    case depth = 2
    case sideBySide = 3
    case pointCloud3D = 4

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .ir: return "IR"
        case .color: return "Colour"
        case .depth: return "Depth"
        case .sideBySide: return "Side×Side"
        case .pointCloud3D: return "3D Cloud"
        }
    }
}

enum PocketSbsChoice: Int, CaseIterable, Identifiable {
    case irDepth = 0
    case colorDepth = 1
    case colorIr = 2

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .irDepth: return "IR + Depth"
        case .colorDepth: return "Colour + Depth"
        case .colorIr: return "Colour + IR"
        }
    }
}

@MainActor
final class PocketViewModel: ObservableObject {
    @Published var telemetry = PocketClientTelemetry()
    @Published var showPairingSheet: Bool = false
    @Published var shortCodeInput: String = ""
    /// Short-code pairing only after the person confirms they're on their own network (see the warning).
    @Published var shortCodeNetworkConfirmed: Bool = false
    @Published var pairingInProgress: Bool = false
    @Published var pairingError: String?
    /// Bumped by `unpairDevice()`. A pairing or link completion that started under an older generation is ignored,
    /// so a probe that fails because of the unpair cannot put its message on the fresh pairing sheet.
    private(set) var pairingGeneration: UInt64 = 0
    @Published var keychainAccessError: String?
    @Published var isScreenCaptured: Bool = false
    @Published private(set) var isInactiveForSnapshot: Bool = false
    @Published var screenshotWarningCount: Int = 0
    @Published var showHUD: Bool = true

    // Phase 5: Local Zoom/Pan/Freeze, Health Toggle, Remote Sheet & Recording Reminder
    @Published var zoomScale: CGFloat = 1.0
    @Published var panOffset: CGSize = .zero
    @Published var isLocalFrozen: Bool = false
    @Published var prefer2DZoomIn3DMode: Bool = false
    @Published var showHealthDisplay: Bool = false
    @Published var showRemoteSheet: Bool = false
    @Published var showPhoneRecordingReminder: Bool = false
    @Published var phoneReminderSnoozeOneHour: Bool = false

    // Phase 9A & 9B: Protocol v3 Pet Camera & Camera Roll state
    @Published var revokedBannerText: String? = nil
    @Published var showOutsideDisableSharingAlert: Bool = false
    @Published var devicePendingRevoke: PetDevice? = nil
    @Published var showCameraRollSheet: Bool = false
    @Published var inviteOrLinkInput: String = ""

    // Test-observable haptic transition counters (Fix 2)
    private(set) var connectHapticCount: Int = 0
    private(set) var disconnectHapticCount: Int = 0

    let displayBridge = SampleBufferDisplayBridge()
    let cameraRoll = CameraRollController()
    private var client: PocketStreamClientCore?
    private var persistenceForRetry: PairingPersistenceProvider?
    private var deviceNameForRetry: String?
    private var deviceKindForRetry: String = "iphone"
    private var observers: [NSObjectProtocol] = []
    private var previousIsConnected: Bool = false

    init(
        client: PocketStreamClientCore?,
        registerLifecycleObservers: Bool = false
    ) {
        self.client = client
        if let core = client {
            self.telemetry = core.telemetry
            self.previousIsConnected = core.telemetry.isConnected
            wireClientCallbacks(core)
        }
        if registerLifecycleObservers {
            registerLifecycleAndCaptureObservers()
            refreshScreenCaptureState()
        }
    }

    init(
        persistence: PairingPersistenceProvider,
        deviceName: String = "iPhone",
        deviceKind: String = "iphone",
        autostart: Bool = true
    ) {
        URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)
        self.persistenceForRetry = persistence
        self.deviceNameForRetry = deviceName
        self.deviceKindForRetry = deviceKind
        initializeClientFromPersistence(autostart: autostart)
    }

    convenience init() {
        // Hard requirement #1: Disable all URL/disk caching globally before any network activity.
        URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)

        let useInMemory = CommandLine.arguments.contains("--ephemeral-pairing")
        let provider: PairingPersistenceProvider = useInMemory ? InMemoryPersistenceProvider() : KeychainPersistenceProvider()

        #if canImport(UIKit)
        let kind = UIDevice.current.userInterfaceIdiom == .pad ? "ipad" : "iphone"
        let devName = UIDevice.current.name
        #else
        let kind = "iphone"
        let devName = "iPhone"
        #endif

        self.init(persistence: provider, deviceName: devName, deviceKind: kind, autostart: false)
        registerLifecycleAndCaptureObservers()
        refreshScreenCaptureState()
        if keychainAccessError == nil {
            handleLaunchArgumentsOrAutostart()
        }
    }

    private func initializeClientFromPersistence(autostart: Bool) {
        guard let provider = persistenceForRetry else { return }
        do {
            let created = try PocketStreamClientCore(
                persistence: provider,
                deviceName: deviceNameForRetry,
                deviceKind: deviceKindForRetry
            )
            self.client = created
            self.keychainAccessError = nil
            self.telemetry = created.telemetry
            self.previousIsConnected = created.telemetry.isConnected
            wireClientCallbacks(created)
            if autostart {
                if telemetry.isPaired && !isScreenCaptured {
                    #if canImport(UIKit)
                    UIApplication.shared.isIdleTimerDisabled = true
                    #endif
                    created.start()
                } else if !telemetry.isPaired {
                    showPairingSheet = true
                }
            }
        } catch {
            // Preserve existing Keychain item untouched; surface clear error + Retry instead of generating a new identity.
            self.client = nil
            self.keychainAccessError = error.localizedDescription
            self.showPairingSheet = false
        }
    }

    func retryKeychainAccess() {
        guard persistenceForRetry != nil else { return }
        initializeClientFromPersistence(autostart: true)
    }

    private func wireClientCallbacks(_ core: PocketStreamClientCore) {
        let bridge = self.displayBridge
        cameraRoll.attach(to: core)
        core.onSampleBufferReady = { sampleBuffer in
            bridge.enqueue(sampleBuffer)
        }
        core.onStreamFlushed = { [weak self] in
            bridge.flushAndClearImage()
            Task { @MainActor in
                self?.isLocalFrozen = false
            }
        }
        core.onRevoked = { [weak self] banner in
            bridge.flushAndClearImage()
            Task { @MainActor in
                self?.handleRevokedOnMain(banner: banner)
            }
        }
        core.onTelemetryUpdated = { [weak self] snap in
            Task { @MainActor in
                self?.applyTelemetrySnapshot(snap)
            }
        }
    }

    private func handleRevokedOnMain(banner: String) {
        pairingInProgress = false
        isLocalFrozen = false
        reset2DZoomAndPan()
        displayBridge.flushAndClearImage()
        cameraRoll.clearAllInMemoryState()
        showCameraRollSheet = false
        showRemoteSheet = false
        showPhoneRecordingReminder = false
        showOutsideDisableSharingAlert = false
        devicePendingRevoke = nil
        revokedBannerText = banner
        pairingError = banner
        #if canImport(UIKit)
        UIApplication.shared.isIdleTimerDisabled = false
        #endif
        showPairingSheet = true
    }

    private func applyTelemetrySnapshot(_ snap: PocketClientTelemetry) {
        let wasConnected = previousIsConnected
        previousIsConnected = snap.isConnected
        if snap.currentModeRaw != telemetry.currentModeRaw {
            reset2DZoomAndPan()
            if snap.currentModeRaw != PocketModeChoice.pointCloud3D.rawValue {
                prefer2DZoomIn3DMode = false
            }
        }
        telemetry = snap
        cameraRoll.applyTelemetrySnapshot(snap)

        if let rev = snap.revokedNotice, revokedBannerText != rev {
            handleRevokedOnMain(banner: rev)
            return
        }

        if !snap.isConnected || snap.protocolVersion < 2 {
            if showRemoteSheet { showRemoteSheet = false }
            if showPhoneRecordingReminder { showPhoneRecordingReminder = false }
            if !snap.isConnected && showCameraRollSheet { showCameraRollSheet = false }
        } else if snap.isPetCameraV3 && snap.hasConfirmedPetCameraState && snap.petState.role != .owner {
            if showRemoteSheet { showRemoteSheet = false }
            if showPhoneRecordingReminder { showPhoneRecordingReminder = false }
        }

        // Phase 5.5: Light haptic tap on connect; distinct haptic on disconnect.
        if !wasConnected && snap.isConnected {
            connectHapticCount += 1
            #if canImport(UIKit)
            let gen = UIImpactFeedbackGenerator(style: .light)
            gen.impactOccurred()
            #endif
        } else if wasConnected && !snap.isConnected {
            disconnectHapticCount += 1
            isLocalFrozen = false
            displayBridge.flushAndClearImage()
            cameraRoll.playbackDisplayBridge.flushAndClearImage()
            #if canImport(UIKit)
            let notify = UINotificationFeedbackGenerator()
            notify.notificationOccurred(.warning)
            #endif
        }
    }

    // MARK: - Confirmed Host Power State, v1/v2 & v3 Pet Camera Capability Helpers

    var supportsHostV2: Bool {
        telemetry.isConnected && telemetry.protocolVersion >= 2
    }

    var isPetCameraV3: Bool {
        telemetry.isConnected && telemetry.isPetCameraV3
    }

    var currentPetRole: PetRole {
        guard isPetCameraV3 else { return .unknown }
        if telemetry.hasConfirmedPetCameraState {
            return telemetry.petState.role
        }
        return telemetry.cachedPairedRole ?? .unknown
    }

    var isPetOwner: Bool {
        isPetCameraV3 && telemetry.hasConfirmedPetCameraState && telemetry.petState.role == .owner
    }

    var isPetViewer: Bool {
        isPetCameraV3 && !isPetOwner
    }

    /// Viewers get no settings and no device management (`docs/protocol-v3-pet-camera.md` §3.1).
    var showOwnerSettingsAndRemoteControls: Bool {
        guard supportsHostV2 else { return false }
        if isPetCameraV3 {
            return isPetOwner
        }
        return true
    }

    var showSensorPowerToggle: Bool {
        showOwnerSettingsAndRemoteControls
    }

    var showPetCameraOwnerSection: Bool {
        isPetOwner && telemetry.hasConfirmedPetCameraState
    }

    var showCameraRollButton: Bool {
        cameraRoll.supportsCameraRoll
    }

    var shouldHideRoomImagesForSnapshot: Bool {
        isInactiveForSnapshot || isScreenCaptured
    }

    var showLiveQualityPicker: Bool {
        isPetCameraV3
    }

    var selectedLiveQuality: LiveQuality {
        guard isPetCameraV3, telemetry.hasConfirmedPetCameraState else { return .auto }
        let q = telemetry.petState.liveQuality
        return q == .unknown ? .auto : q
    }

    var activeLiveTierLabel: String {
        guard isPetCameraV3, telemetry.hasConfirmedPetCameraState else { return "High" }
        return telemetry.petState.liveTier.displayLabel
    }

    var watchingNowEntries: [PetWatcher] {
        guard showPetCameraOwnerSection else { return [] }
        return telemetry.petState.watchingNow ?? []
    }

    var pairedDeviceEntries: [PetDevice] {
        guard showPetCameraOwnerSection else { return [] }
        return telemetry.petState.devices ?? []
    }

    var isLiveVideoUnavailableV3: Bool {
        isPetCameraV3 && telemetry.hasConfirmedPetCameraState && !telemetry.petState.liveAllowed
    }

    /// Plain-English reason whenever v3 `0x0B` says `liveAllowed: false` (`docs/protocol-v3-pet-camera.md` §9).
    var liveUnavailableReasonText: String? {
        guard isLiveVideoUnavailableV3 else { return nil }
        return (telemetry.petState.liveUnavailableReason ?? .unknown).displaySentence
    }

    var disconnectedStatusTitle: String {
        if let rev = revokedBannerText {
            return rev
        }
        if let blocked = telemetry.networkBlockedSentence {
            return blocked
        }
        if let tlsRefusal = telemetry.tlsRefusalNotice {
            return tlsRefusal
        }
        if let notice = telemetry.lastSessionNotice {
            switch notice.reason {
            case .tooManySessions:
                return PetCameraUserStrings.tooManySessionsUserText
            case .outsideDisabled:
                return notice.message ?? PetSessionNoticeReason.outsideDisabled.defaultSentence
            case .addressChanged:
                return PetCameraUserStrings.unreachableAddressChanged
            case .hostShuttingDown, .replaced:
                return notice.message ?? notice.reason.defaultSentence
            case .revoked:
                return PetCameraUserStrings.phoneRevokedText
            case .unknown:
                break
            }
        }
        if telemetry.sawAddressChangeNotice {
            return PetCameraUserStrings.unreachableAddressChanged
        }
        if (telemetry.cachedHostProtocolVersion ?? 1) >= 3 || telemetry.storedOutsideEndpoint != nil {
            return PetCameraUserStrings.unreachableDefaultPC
        }
        return SharedRecordingReminderConstants.viewerNotOpenText
    }

    /// Where the app will look for the PC away from home, in plain words, for the "can't reach" screen.
    var awayFromHomeAddressText: String {
        guard let ep = telemetry.storedOutsideEndpoint else {
            return "Away from home: no address yet. Open the app once at home, with viewing from outside on in Kinect Viewer on the PC."
        }
        return "Away from home: \(ep.host):\(ep.port)" + (ep.verified ? "" : " (typed in on the PC)")
    }

    /// The newest connection attempts, newest first, one line each, e.g. "01:44:05 outside 203.0.113.5:40000 — reached".
    var recentConnectAttemptLines: [String] {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return telemetry.recentConnectAttempts.reversed().map {
            "\(f.string(from: $0.at)) \($0.path == .outside ? "outside" : "home") \($0.endpoint) — \($0.result)"
        }
    }

    var availableModes: [PocketModeChoice] {
        supportsHostV2 ? PocketModeChoice.allCases : [.ir, .color, .depth, .sideBySide]
    }

    /// Amended spec Option A: a v3 owner may send `0x0A` whenever the PC allows phone power,
    /// even while `liveAllowed == false` (it can't watch).
    var allowSensorPowerControl: Bool {
        showSensorPowerToggle && (!telemetry.hasConfirmedHostState || telemetry.hostState.allowPhoneSensorPower)
    }

    var sensorPowerDisabledReasonText: String? {
        guard showSensorPowerToggle,
              telemetry.hasConfirmedHostState,
              !telemetry.hostState.allowPhoneSensorPower else {
            return nil
        }
        return telemetry.hostState.sensorPowerDisabledReason
            ?? SharedRecordingReminderConstants.sensorPowerDisabledOnHostText
    }

    var confirmedPowerState: WireSensorPowerState {
        guard telemetry.isConnected, telemetry.hasConfirmedHostState else { return .off }
        return telemetry.hostState.powerState
    }

    var confirmedSwitchIsOn: Bool {
        supportsHostV2 && telemetry.hasConfirmedHostState && telemetry.hostState.powerState == .on
    }

    var isPowerTransitionStarting: Bool {
        supportsHostV2 && telemetry.hasConfirmedHostState && telemetry.hostState.powerState == .starting
    }

    var inUseBannerText: String? {
        guard supportsHostV2,
              telemetry.hasConfirmedHostState,
              telemetry.hostState.powerState == .inUseElsewhere else {
            return nil
        }
        return SharedRecordingReminderConstants.inUseByMessage(
            holder: telemetry.hostState.inUseHolder ?? "another process"
        )
    }

    var powerErrorBannerText: String? {
        guard supportsHostV2,
              telemetry.hasConfirmedHostState,
              telemetry.hostState.powerState == .error else {
            return nil
        }
        let msg = telemetry.hostState.errorMessage?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if !msg.isEmpty {
            return msg
        }
        return isPetCameraV3 ? "Sensor error on camera." : "Sensor error on Mac host."
    }

    var canControlHostMode: Bool {
        if isPetCameraV3 && !isPetOwner {
            return false
        }
        return telemetry.allowModeSwitch
    }

    var is3DOrbitGestureActive: Bool {
        supportsHostV2 &&
        confirmedPowerState == .on &&
        !isLiveVideoUnavailableV3 &&
        telemetry.currentModeRaw == PocketModeChoice.pointCloud3D.rawValue &&
        !prefer2DZoomIn3DMode &&
        !isLocalFrozen &&
        canControlHostMode
    }

    var effectiveDelayMs: Double {
        if telemetry.hostToDisplayLatencyMs > 0 {
            return telemetry.hostToDisplayLatencyMs
        }
        return telemetry.rttMs * 0.5
    }

    private func handleLaunchArgumentsOrAutostart() {
        let args = CommandLine.arguments
        if let idx = args.firstIndex(of: "--pair-uri"), idx + 1 < args.count {
            submitQRCodeURI(args[idx + 1])
            return
        }
        if let idx = args.firstIndex(of: "--short-code"), idx + 1 < args.count {
            shortCodeInput = args[idx + 1]
            submitShortCode()
            return
        }

        if telemetry.isPaired && !isScreenCaptured {
            #if canImport(UIKit)
            UIApplication.shared.isIdleTimerDisabled = true
            #endif
            client?.start()
        } else if !telemetry.isPaired && keychainAccessError == nil {
            showPairingSheet = true
        }
    }

    func handleWillResignActive() {
        isInactiveForSnapshot = true
        isLocalFrozen = false
        displayBridge.flushAndClearImage()
        cameraRoll.setHiddenForSnapshot(true)
        #if canImport(UIKit)
        UIApplication.shared.isIdleTimerDisabled = false
        #endif
    }

    func handleDidEnterBackground() {
        isInactiveForSnapshot = true
        isLocalFrozen = false
        displayBridge.flushAndClearImage()
        cameraRoll.setHiddenForSnapshot(true)
        cameraRoll.closePlayback(resumeLive: false)
        client?.stop()
        pairingInProgress = false
    }

    func handleDidBecomeActive() {
        isInactiveForSnapshot = false
        refreshScreenCaptureState()
        if !isScreenCaptured {
            cameraRoll.setHiddenForSnapshot(false)
        }
        if telemetry.isPaired && !isScreenCaptured {
            #if canImport(UIKit)
            UIApplication.shared.isIdleTimerDisabled = true
            #endif
            // Keep a live session through a brief interruption; reconnect only if there is none.
            client?.resume()
        }
    }

    private func registerLifecycleAndCaptureObservers() {
        #if canImport(UIKit)
        let nc = NotificationCenter.default

        // Synchronously wipe the GPU display layers and hide the camera-roll thumbnail grid BEFORE SpringBoard snapshots!
        observers.append(nc.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            if Thread.isMainThread {
                MainActor.assumeIsolated {
                    self?.handleWillResignActive()
                }
            } else {
                Task { @MainActor in
                    self?.handleWillResignActive()
                }
            }
        })

        observers.append(nc.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
            if Thread.isMainThread {
                MainActor.assumeIsolated {
                    self?.handleDidEnterBackground()
                }
            } else {
                Task { @MainActor in
                    self?.handleDidEnterBackground()
                }
            }
        })

        observers.append(nc.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            if Thread.isMainThread {
                MainActor.assumeIsolated {
                    self?.handleDidBecomeActive()
                }
            } else {
                Task { @MainActor in
                    self?.handleDidBecomeActive()
                }
            }
        })

        observers.append(nc.addObserver(forName: UIScreen.capturedDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            if Thread.isMainThread {
                MainActor.assumeIsolated {
                    self?.refreshScreenCaptureState()
                }
            } else {
                Task { @MainActor in
                    self?.refreshScreenCaptureState()
                }
            }
        })

        observers.append(nc.addObserver(forName: UIApplication.userDidTakeScreenshotNotification, object: nil, queue: .main) { [weak self] _ in
            if Thread.isMainThread {
                MainActor.assumeIsolated {
                    self?.screenshotWarningCount += 1
                    self?.isLocalFrozen = false
                    self?.displayBridge.flushAndClearImage()
                    self?.cameraRoll.playbackDisplayBridge.flushAndClearImage()
                }
            } else {
                Task { @MainActor in
                    self?.screenshotWarningCount += 1
                    self?.isLocalFrozen = false
                    self?.displayBridge.flushAndClearImage()
                    self?.cameraRoll.playbackDisplayBridge.flushAndClearImage()
                }
            }
        })
        #endif
    }

    private func refreshScreenCaptureState() {
        #if canImport(UIKit)
        let captured = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .contains { $0.screen.isCaptured }
        isScreenCaptured = captured
        if captured {
            isLocalFrozen = false
            displayBridge.flushAndClearImage()
            cameraRoll.setHiddenForSnapshot(true)
            cameraRoll.closePlayback(resumeLive: false)
            client?.stop()
        } else {
            if !isInactiveForSnapshot {
                cameraRoll.setHiddenForSnapshot(false)
            }
            // resume(), not start(): this also runs on every become-active, where start() would drop and redial a
            // live session. When capture has just ended, stop() already ran, so resume() starts a fresh cycle.
            if telemetry.isPaired {
                client?.resume()
            }
        }
        #endif
    }

    // MARK: - Pairing, Outside Invites & Endpoint Links

    func submitQRCodeURI(_ uri: String) {
        guard let client = client, !pairingInProgress else { return }
        pairingInProgress = true
        pairingError = nil
        let generation = pairingGeneration
        client.submitCameraLink(uri) { [weak self] result in
            Task { @MainActor in
                // Ignore a result that arrives after `unpairDevice()`.
                guard let self = self, self.pairingGeneration == generation else { return }
                self.pairingInProgress = false
                switch result {
                case .success:
                    self.revokedBannerText = nil
                    self.pairingError = nil
                    self.showPairingSheet = false
                    self.shortCodeInput = ""
                    self.inviteOrLinkInput = ""
                    #if canImport(UIKit)
                    UIApplication.shared.isIdleTimerDisabled = true
                    #endif
                case .failure(let err):
                    self.pairingError = err.localizedDescription
                }
            }
        }
    }

    func submitCameraLinkInput(nowUnixSec: UInt64? = nil) {
        let trimmed = inviteOrLinkInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let client = client, !pairingInProgress, !trimmed.isEmpty else { return }
        pairingInProgress = true
        pairingError = nil
        let generation = pairingGeneration
        client.submitCameraLink(trimmed, nowUnixSec: nowUnixSec) { [weak self] result in
            Task { @MainActor in
                // Ignore a result that arrives after `unpairDevice()`.
                guard let self = self, self.pairingGeneration == generation else { return }
                self.pairingInProgress = false
                switch result {
                case .success:
                    self.revokedBannerText = nil
                    self.pairingError = nil
                    self.showPairingSheet = false
                    self.shortCodeInput = ""
                    self.inviteOrLinkInput = ""
                    #if canImport(UIKit)
                    UIApplication.shared.isIdleTimerDisabled = true
                    #endif
                case .failure(let err):
                    self.pairingError = err.localizedDescription
                }
            }
        }
    }

    func submitShortCode() {
        guard let client = client, !pairingInProgress, shortCodeNetworkConfirmed else { return }
        pairingInProgress = true
        pairingError = nil
        let generation = pairingGeneration
        client.pairWithShortCode(shortCodeInput) { [weak self] result in
            Task { @MainActor in
                // Ignore a result that arrives after `unpairDevice()`.
                guard let self = self, self.pairingGeneration == generation else { return }
                self.pairingInProgress = false
                switch result {
                case .success:
                    self.revokedBannerText = nil
                    self.pairingError = nil
                    self.showPairingSheet = false
                    self.shortCodeInput = ""
                    self.inviteOrLinkInput = ""
                    #if canImport(UIKit)
                    UIApplication.shared.isIdleTimerDisabled = true
                    #endif
                case .failure(let err):
                    self.pairingError = err.localizedDescription
                }
            }
        }
    }

    func unpairDevice() {
        pairingGeneration &+= 1
        pairingInProgress = false
        isLocalFrozen = false
        reset2DZoomAndPan()
        displayBridge.flushAndClearImage()
        cameraRoll.clearAllInMemoryState()
        revokedBannerText = nil
        pairingError = nil
        client?.unpair()
        #if canImport(UIKit)
        UIApplication.shared.isIdleTimerDisabled = false
        #endif
        showCameraRollSheet = false
        showRemoteSheet = false
        showPairingSheet = true
    }

    func reconnectManually() {
        guard telemetry.isPaired, !isScreenCaptured else { return }
        client?.start()
    }

    // MARK: - Local Viewport Freeze, Pinch-Zoom & Pan

    func toggleLocalFreeze() {
        guard telemetry.isConnected,
              !isLiveVideoUnavailableV3,
              (!supportsHostV2 || confirmedPowerState == .on) else { return }
        let nextFrozen = !isLocalFrozen
        isLocalFrozen = nextFrozen
        displayBridge.setPausedInMemory(nextFrozen)
        if !nextFrozen && supportsHostV2 {
            // Request an immediate H.264 IDR keyframe so live motion resumes cleanly on the next frame
            client?.sendRemoteControl(RemoteControlCommandMessage(action: .requestKeyframe))
        }
    }

    func update2DTransform(scale: CGFloat, pan: CGSize) {
        zoomScale = scale
        panOffset = pan
    }

    func reset2DZoomAndPan() {
        zoomScale = 1.0
        panOffset = .zero
    }

    // MARK: - Protocol v3 Live Quality & Owner Pet Camera Controls

    func selectLiveQuality(_ quality: LiveQuality) {
        guard isPetCameraV3, quality != .unknown else { return }
        client?.setLiveQuality(quality)
    }

    func requestSetSharing(_ enabled: Bool) {
        guard isPetOwner else { return }
        if !enabled && telemetry.petState.path == .outside {
            showOutsideDisableSharingAlert = true
            return
        }
        client?.sendPetCameraCommand(action: .setSharing, enabled: enabled)
    }

    func confirmDisableSharingFromOutside() {
        showOutsideDisableSharingAlert = false
        guard isPetOwner else { return }
        client?.sendPetCameraCommand(action: .setSharing, enabled: false)
    }

    func setPrivacyMode(_ enabled: Bool) {
        guard isPetOwner else { return }
        client?.sendPetCameraCommand(action: .setPrivacy, enabled: enabled)
    }

    func requestRevokeDevice(_ device: PetDevice) {
        guard isPetOwner, device.deviceId != telemetry.petState.deviceId else { return }
        devicePendingRevoke = device
    }

    func confirmRevokePendingDevice() {
        guard let target = devicePendingRevoke else { return }
        devicePendingRevoke = nil
        revokeDeviceImmediately(deviceId: target.deviceId)
    }

    func revokeDeviceImmediately(deviceId: String) {
        guard isPetOwner, deviceId != telemetry.petState.deviceId else { return }
        client?.sendPetCameraCommand(action: .revokeDevice, deviceId: deviceId)
    }

    // MARK: - Remote Power, Mode, Cut-Off, Freeze, Recording & 3D Orbit

    func requestSensorPower(turnOn: Bool) {
        guard showSensorPowerToggle else { return }
        // Never assume local state: send request and wait for HostStateSyncMessage confirmation.
        client?.requestSensorPower(turnOn: turnOn)
    }

    func selectMode(_ mode: PocketModeChoice) {
        guard canControlHostMode else { return }
        client?.requestModeSwitch(modeRaw: mode.rawValue, sbsLayoutRaw: telemetry.currentSbsLayoutRaw)
    }

    func selectSbsLayout(_ layout: PocketSbsChoice) {
        guard canControlHostMode else { return }
        client?.requestModeSwitch(modeRaw: PocketModeChoice.sideBySide.rawValue, sbsLayoutRaw: layout.rawValue)
    }

    func setHostCutoffEnabled(_ enabled: Bool) {
        guard canControlHostMode else { return }
        client?.sendRemoteControl(RemoteControlCommandMessage(
            action: .setCutoff,
            cutoffEnabled: enabled,
            applyTrackMePreset: false
        ))
    }

    func applyHostTrackMePreset() {
        guard canControlHostMode else { return }
        client?.sendRemoteControl(RemoteControlCommandMessage(
            action: .setCutoff,
            cutoffEnabled: true,
            applyTrackMePreset: true
        ))
    }

    func toggleHostFreeze() {
        guard canControlHostMode else { return }
        let nextHostFrozen = !telemetry.hostState.isHostFrozen
        client?.sendRemoteControl(RemoteControlCommandMessage(
            action: .setHostFrozen,
            hostFrozen: nextHostFrozen
        ))
    }

    func requestToggleHostRecording() {
        guard showOwnerSettingsAndRemoteControls else { return }
        if telemetry.hostState.isRecording {
            client?.sendRemoteControl(RemoteControlCommandMessage(action: .stopRecording))
            return
        }
        guard confirmedPowerState == .on, telemetry.hostState.canRecordCurrentMode else { return }
        if telemetry.hostState.recordingReminderNeeded {
            phoneReminderSnoozeOneHour = false
            showPhoneRecordingReminder = true
        } else {
            client?.sendRemoteControl(RemoteControlCommandMessage(
                action: .startRecording,
                reminderConfirmedOnPhone: false,
                snoozeReminderOneHour: false
            ))
        }
    }

    func confirmPhoneRecordingReminderAndStart() {
        guard showPhoneRecordingReminder, showOwnerSettingsAndRemoteControls else { return }
        let snooze = phoneReminderSnoozeOneHour
        showPhoneRecordingReminder = false
        client?.sendRemoteControl(RemoteControlCommandMessage(
            action: .startRecording,
            reminderConfirmedOnPhone: true,
            snoozeReminderOneHour: snooze
        ))
    }

    func promptRecordingReminderOnMacInstead() {
        guard showPhoneRecordingReminder, showOwnerSettingsAndRemoteControls else { return }
        showPhoneRecordingReminder = false
        client?.sendRemoteControl(RemoteControlCommandMessage(
            action: .startRecording,
            reminderConfirmedOnPhone: false,
            snoozeReminderOneHour: false
        ))
    }

    func send3DOrbit(deltaYaw: Float, deltaPitch: Float) {
        guard canControlHostMode else { return }
        client?.sendOrbit3DGesture(Orbit3DGestureMessage(
            deltaYawRadians: deltaYaw,
            deltaPitchRadians: deltaPitch,
            zoomScale: 1.0,
            panRightMeters: 0.0,
            panUpMeters: 0.0,
            resetView: false
        ))
    }

    func send3DZoom(scaleFactor: Float) {
        guard canControlHostMode else { return }
        client?.sendOrbit3DGesture(Orbit3DGestureMessage(
            deltaYawRadians: 0.0,
            deltaPitchRadians: 0.0,
            zoomScale: scaleFactor,
            panRightMeters: 0.0,
            panUpMeters: 0.0,
            resetView: false
        ))
    }

    func send3DPan(deltaRightMeters: Float, deltaUpMeters: Float) {
        guard canControlHostMode else { return }
        client?.sendOrbit3DGesture(Orbit3DGestureMessage(
            deltaYawRadians: 0.0,
            deltaPitchRadians: 0.0,
            zoomScale: 1.0,
            panRightMeters: deltaRightMeters,
            panUpMeters: deltaUpMeters,
            resetView: false
        ))
    }

    func reset3DCamera() {
        guard canControlHostMode else { return }
        client?.sendOrbit3DGesture(Orbit3DGestureMessage(
            deltaYawRadians: 0.0,
            deltaPitchRadians: 0.0,
            zoomScale: 1.0,
            panRightMeters: 0.0,
            panUpMeters: 0.0,
            resetView: true
        ))
    }
}
