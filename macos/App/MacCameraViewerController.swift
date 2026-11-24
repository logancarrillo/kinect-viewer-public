import AppKit
import Combine
import Foundation

enum MacViewerModeChoice: Int, CaseIterable, Identifiable {
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

enum MacViewerSbsChoice: Int, CaseIterable, Identifiable {
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
final class MacCameraViewerController: ObservableObject {
    @Published var telemetry = PocketClientTelemetry()
    @Published var pairURIInput: String = ""
    @Published var shortCodeInput: String = ""
    /// Short-code pairing only after the person confirms they're on their own network (see the warning).
    @Published var shortCodeNetworkConfirmed: Bool = false
    @Published var pairingInProgress: Bool = false
    @Published var pairingError: String?
    @Published var keychainAccessError: String?
    @Published var revokedBannerText: String?
    @Published var isLocalFrozen: Bool = false
    @Published var showHealthOverlay: Bool = false
    @Published var showOutsideDisableSharingAlert: Bool = false
    @Published var devicePendingRevoke: PetDevice?
    @Published var showCameraRollSheet: Bool = false

    let displayBridge = SampleBufferDisplayBridge()
    let cameraRoll = CameraRollController()
    private var client: PocketStreamClientCore?
    private let customPersistence: PairingPersistenceProvider?
    private let customDeviceName: String?
    private var isActiveMode: Bool = false

    init(
        client: PocketStreamClientCore? = nil,
        persistence: PairingPersistenceProvider? = nil,
        deviceName: String? = nil
    ) {
        self.client = client
        self.customPersistence = persistence
        self.customDeviceName = deviceName
        if let core = client {
            self.telemetry = core.telemetry
            wireClientCallbacks(core)
        }
    }

    @discardableResult
    private func ensureClientInitialized() -> PocketStreamClientCore? {
        if let existing = client {
            return existing
        }
        URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)
        let isXCTest = ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
        let provider: PairingPersistenceProvider = customPersistence
            ?? (isXCTest
                ? InMemoryPersistenceProvider()
                : KeychainPersistenceProvider(service: KeychainPairingStore.macViewerService))
        let resolvedName = customDeviceName ?? Host.current().localizedName ?? "Mac"
        do {
            let created = try PocketStreamClientCore(
                persistence: provider,
                deviceName: resolvedName,
                deviceKind: "mac"
            )
            self.client = created
            self.keychainAccessError = nil
            self.telemetry = created.telemetry
            wireClientCallbacks(created)
            return created
        } catch {
            self.client = nil
            self.keychainAccessError = error.localizedDescription
            return nil
        }
    }

    func retryKeychainAccess() {
        keychainAccessError = nil
        activate()
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
        displayBridge.flushAndClearImage()
        cameraRoll.clearAllInMemoryState()
        showCameraRollSheet = false
        showOutsideDisableSharingAlert = false
        devicePendingRevoke = nil
        revokedBannerText = banner
        pairingError = banner
    }

    private func applyTelemetrySnapshot(_ snap: PocketClientTelemetry) {
        let wasConnected = telemetry.isConnected
        telemetry = snap
        cameraRoll.applyTelemetrySnapshot(snap)
        if let rev = snap.revokedNotice, revokedBannerText != rev {
            handleRevokedOnMain(banner: rev)
            return
        }
        if wasConnected && !snap.isConnected {
            isLocalFrozen = false
            displayBridge.flushAndClearImage()
            cameraRoll.playbackDisplayBridge.flushAndClearImage()
            if showCameraRollSheet {
                showCameraRollSheet = false
            }
        }
    }

    func activate() {
        isActiveMode = true
        guard let core = ensureClientInitialized() else { return }
        if core.telemetry.isPaired {
            core.start()
        }
    }

    func deactivate() {
        isActiveMode = false
        isLocalFrozen = false
        displayBridge.flushAndClearImage()
        cameraRoll.closePlayback(resumeLive: false)
        showCameraRollSheet = false
        client?.stop()
        pairingInProgress = false
    }

    func shutdown() {
        deactivate()
    }

    // MARK: - Role & Capability Gating (Matches iOS `PocketViewModel`)

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

    var showOwnerControls: Bool {
        guard supportsHostV2 else { return false }
        if isPetCameraV3 {
            return isPetOwner
        }
        return true
    }

    var showPetCameraOwnerSection: Bool {
        isPetOwner && telemetry.hasConfirmedPetCameraState
    }

    var showCameraRollButton: Bool {
        cameraRoll.supportsCameraRoll
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

    var liveUnavailableReasonText: String? {
        guard isLiveVideoUnavailableV3 else { return nil }
        return (telemetry.petState.liveUnavailableReason ?? .unknown).displaySentence
    }

    var disconnectedStatusTitle: String {
        if let rev = revokedBannerText {
            return rev
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
                return PetCameraUserStrings.macRevokedText
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
        return "Waiting for paired camera on home network…"
    }

    /// Amended spec Option A: a v3 owner may send `0x0A` whenever `allowPhoneSensorPower` is true,
    /// even while `liveAllowed == false`.
    var allowSensorPowerControl: Bool {
        showOwnerControls && (!telemetry.hasConfirmedHostState || telemetry.hostState.allowPhoneSensorPower)
    }

    var sensorPowerDisabledReasonText: String? {
        guard showOwnerControls,
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

    var canControlHostMode: Bool {
        if isPetCameraV3 && !isPetOwner {
            return false
        }
        return telemetry.allowModeSwitch
    }

    var effectiveDelayMs: Double {
        if telemetry.hostToDisplayLatencyMs > 0 {
            return telemetry.hostToDisplayLatencyMs
        }
        return telemetry.rttMs * 0.5
    }

    // MARK: - Pairing & Session Actions

    func submitPairURI(nowUnixSec: UInt64? = nil) {
        guard !pairingInProgress, let core = ensureClientInitialized() else { return }
        let trimmed = pairURIInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        pairingInProgress = true
        pairingError = nil
        core.submitCameraLink(trimmed, nowUnixSec: nowUnixSec) { [weak self] result in
            Task { @MainActor in
                guard let self = self else { return }
                self.pairingInProgress = false
                switch result {
                case .success:
                    self.revokedBannerText = nil
                    self.pairingError = nil
                    self.pairURIInput = ""
                    self.shortCodeInput = ""
                case .failure(let err):
                    self.pairingError = err.localizedDescription
                }
            }
        }
    }

    func submitShortCode(hostOverride: String? = nil, portOverride: UInt16? = nil) {
        guard !pairingInProgress, shortCodeNetworkConfirmed, let core = ensureClientInitialized() else { return }
        pairingInProgress = true
        pairingError = nil
        core.pairWithShortCode(shortCodeInput, hostOverride: hostOverride, portOverride: portOverride) { [weak self] result in
            Task { @MainActor in
                guard let self = self else { return }
                self.pairingInProgress = false
                switch result {
                case .success:
                    self.revokedBannerText = nil
                    self.pairingError = nil
                    self.shortCodeInput = ""
                    self.pairURIInput = ""
                case .failure(let err):
                    self.pairingError = err.localizedDescription
                }
            }
        }
    }

    func unpairCamera() {
        pairingInProgress = false
        isLocalFrozen = false
        displayBridge.flushAndClearImage()
        cameraRoll.clearAllInMemoryState()
        showCameraRollSheet = false
        revokedBannerText = nil
        pairingError = nil
        client?.unpair()
    }

    func reconnectManually() {
        guard let core = ensureClientInitialized(), core.telemetry.isPaired else { return }
        core.start()
    }

    // MARK: - Live Quality & Owner Controls

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
        guard isPetOwner, target.deviceId != telemetry.petState.deviceId else { return }
        client?.sendPetCameraCommand(action: .revokeDevice, deviceId: target.deviceId)
    }

    func requestSensorPower(turnOn: Bool) {
        guard showOwnerControls else { return }
        client?.requestSensorPower(turnOn: turnOn)
    }

    func selectMode(_ mode: MacViewerModeChoice) {
        guard canControlHostMode else { return }
        client?.requestModeSwitch(modeRaw: mode.rawValue, sbsLayoutRaw: telemetry.currentSbsLayoutRaw)
    }

    func selectSbsLayout(_ layout: MacViewerSbsChoice) {
        guard canControlHostMode else { return }
        client?.requestModeSwitch(modeRaw: MacViewerModeChoice.sideBySide.rawValue, sbsLayoutRaw: layout.rawValue)
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
        let next = !telemetry.hostState.isHostFrozen
        client?.sendRemoteControl(RemoteControlCommandMessage(
            action: .setHostFrozen,
            hostFrozen: next
        ))
    }

    func toggleLocalFreeze() {
        guard telemetry.isConnected,
              !isLiveVideoUnavailableV3,
              (!supportsHostV2 || confirmedPowerState == .on) else { return }
        let next = !isLocalFrozen
        isLocalFrozen = next
        displayBridge.setPausedInMemory(next)
        if !next && supportsHostV2 {
            client?.sendRemoteControl(RemoteControlCommandMessage(action: .requestKeyframe))
        }
    }
}
