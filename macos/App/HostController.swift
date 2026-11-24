import AppKit
import Combine
import CoreGraphics
import Foundation

enum ViewerModeOption: Int, CaseIterable, Identifiable {
    case ir = 0
    case color = 1
    case depth = 2
    case sideBySide = 3
    case pointCloud3D = 4

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .ir: return "IR Only"
        case .color: return "Colour"
        case .depth: return "Depth"
        case .sideBySide: return "Side by Side"
        case .pointCloud3D: return "3D Cloud"
        }
    }

    var cValue: kv_view_mode_t {
        kv_view_mode_t(rawValue: UInt32(rawValue))
    }
}

enum SbsLayoutOption: Int, CaseIterable, Identifiable {
    case irDepth = 0
    case colorDepth = 1
    case colorIr = 2

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .irDepth: return "IR + Depth"
        case .colorDepth: return "Colour + Depth"
        case .colorIr: return "Colour + IR"
        }
    }

    var cValue: kv_sbs_layout_t {
        kv_sbs_layout_t(rawValue: UInt32(rawValue))
    }

    var needsColor: Bool {
        self != .irDepth
    }
}

enum PipelineOption: Int, CaseIterable, Identifiable {
    case openGL = 0
    case openCL = 1
    case cpu = 2

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .openGL: return "OpenGL (gl)"
        case .openCL: return "OpenCL (cl)"
        case .cpu: return "CPU (cpu)"
        }
    }

    var shortCode: String {
        switch self {
        case .openGL: return "gl"
        case .openCL: return "cl"
        case .cpu: return "cpu"
        }
    }

    var cValue: kv_pipeline_kind_t {
        kv_pipeline_kind_t(rawValue: UInt32(rawValue))
    }
}

enum ColormapOption: Int, CaseIterable, Identifiable {
    case turbo = 0
    case viridis = 1
    case inferno = 2
    case gray = 3

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .turbo: return "Turbo"
        case .viridis: return "Viridis"
        case .inferno: return "Inferno"
        case .gray: return "Grayscale"
        }
    }

    var cValue: kv_colormap_kind_t {
        kv_colormap_kind_t(rawValue: UInt32(rawValue))
    }
}

enum ExposureModeOption: Int, CaseIterable, Identifiable {
    case auto = 0
    case semiAuto = 1
    case manual = 2

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .auto: return "Auto"
        case .semiAuto: return "Semi-Auto (Flicker-Free)"
        case .manual: return "Manual Lock"
        }
    }

    var cValue: kv_exposure_mode_t {
        kv_exposure_mode_t(rawValue: UInt32(rawValue))
    }
}

private final class StreamHelloSnapshotBox: @unchecked Sendable {
    private let lock = NSLock()
    private var modeRaw: Int = 0
    private var sbsRaw: Int = 0
    private var width: Int = 1024
    private var height: Int = 848

    func update(modeRaw: Int, sbsRaw: Int, width: Int? = nil, height: Int? = nil) {
        lock.lock()
        self.modeRaw = modeRaw
        self.sbsRaw = sbsRaw
        if let w = width, w > 0 { self.width = w }
        if let h = height, h > 0 { self.height = h }
        lock.unlock()
    }

    func snapshot() -> (modeRaw: Int, sbsRaw: Int, width: Int, height: Int) {
        lock.lock()
        defer { lock.unlock() }
        return (modeRaw, sbsRaw, width, height)
    }
}

private final class HostStateSnapshotBox: @unchecked Sendable {
    private let lock = NSLock()
    private var latest: HostStateSyncMessage = .unconfirmedInitial

    func update(_ msg: HostStateSyncMessage) {
        lock.lock()
        self.latest = msg
        lock.unlock()
    }

    func snapshot() -> HostStateSyncMessage {
        lock.lock()
        defer { lock.unlock() }
        return latest
    }
}

@MainActor
final class HostController: ObservableObject {
    @Published var previewImage: CGImage? {
        didSet {
            syncSecondDisplayFrame()
        }
    }
    @Published var isRunning: Bool = false
    @Published var useSyntheticSource: Bool = false
    @Published var syntheticClockOverlay: Bool = false { didSet { pushDisplayParams() } }
    @Published var statusText: String = "Idle"
    @Published var errorBanner: String?

    // Mode & Pipeline Controls (Phase 2)
    @Published var selectedMode: ViewerModeOption = .ir {
        didSet {
            handleModeOrLayoutChange()
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var selectedSbsLayout: SbsLayoutOption = .irDepth {
        didSet {
            handleModeOrLayoutChange()
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var selectedPipeline: PipelineOption = .openGL {
        didSet {
            persistLastUsedSettingsIfReady()
            if !isApplyingPresetOrInit && oldValue != selectedPipeline && isRunning && !isReplayingRecording {
                startStream()
            }
        }
    }

    // IR Tone-Mapping Controls
    @Published var irAutoScale: Bool = true { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }
    @Published var irGamma: Double = 0.45 { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }
    @Published var irSmoothingAlpha: Double = 0.12 { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }
    @Published var irLowPercentile: Double = 0.01 { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }
    @Published var irHighPercentile: Double = 0.995 { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }
    @Published var irManualMin: Double = 0.0 { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }
    @Published var irManualMax: Double = 20000.0 { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }

    // Depth False-Colour Controls
    @Published var depthColormap: ColormapOption = .turbo { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }
    @Published var depthMinMm: Double = 500.0 {
        didSet {
            if depthMaxMm < depthMinMm + 100.0 {
                depthMaxMm = depthMinMm + 100.0
            }
            pushDisplayParams()
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var depthMaxMm: Double = 4500.0 {
        didSet {
            if depthMaxMm < depthMinMm + 100.0 {
                depthMinMm = max(300.0, depthMaxMm - 100.0)
            }
            pushDisplayParams()
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var depthInvertNearWarm: Bool = true { didSet { pushDisplayParams(); persistLastUsedSettingsIfReady() } }

    // Colour Camera Exposure & Gain Controls
    @Published var exposureMode: ExposureModeOption = .auto { didSet { pushColorControls(); persistLastUsedSettingsIfReady() } }
    @Published var autoExposureComp: Double = 0.0 { didSet { pushColorControls(); persistLastUsedSettingsIfReady() } }
    @Published var semiAutoPseudoMs: Double = 16.0 { didSet { pushColorControls(); persistLastUsedSettingsIfReady() } }
    @Published var manualIntegrationMs: Double = 16.6 { didSet { pushColorControls(); persistLastUsedSettingsIfReady() } }
    @Published var manualAnalogGain: Double = 1.5 { didSet { pushColorControls(); persistLastUsedSettingsIfReady() } }

    // Orientation Controls (Phase 1: M = mirror, R = rotate 180°)
    @Published var mirrorHorizontal: Bool = false {
        didSet {
            handleOrientationChange(oldMirror: oldValue, oldRotate180: rotate180)
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var rotate180: Bool = false {
        didSet {
            handleOrientationChange(oldMirror: mirrorHorizontal, oldRotate180: oldValue)
            persistLastUsedSettingsIfReady()
        }
    }

    // Phase 1 Inspection Tools, Freeze, Zoom/Pan, Health & Shortcuts
    @Published var canvasToolMode: CanvasToolMode = .inspect
    @Published var isFrozen: Bool = false
    @Published var viewportTransform = ViewportTransform()
    @Published var cursorReadout: CursorReadout?
    @Published var regionSelectionBox: RegionSelectionBox?
    @Published var regionStats: RegionDepthStats?
    @Published var rulerState: RulerMeasurementState?
    @Published var showHealthOverlay: Bool = false
    @Published var showShortcutsHelp: Bool = false
    @Published var healthSnapshot = HealthMetricsSnapshot()
    @Published var presets: [ViewerPreset] = ViewerPreset.builtInPresets
    @Published var activePresetName: String = ""
    @Published var shortcutNoticeBanner: String?

    // Phase 3: Metal 3D Point Cloud, Depth/Colour Blend & Depth Cut-Off
    @Published var pointCloudCamera = PointCloudCameraState() {
        didSet {
            if oldValue != pointCloudCamera {
                rerenderCachedPointCloudIfNeeded(userCameraInteraction: true)
            }
        }
    }
    @Published var pointCloudSize: Double = 2.5 {
        didSet {
            rerenderCachedPointCloudIfNeeded(userCameraInteraction: true)
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var pointCloudColorSource: PointCloudColorSourceOption = .depth {
        didSet {
            handlePointCloudColorSourceChange()
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var depthColorBlendEnabled: Bool = false {
        didSet {
            handleDepthColorBlendChange()
            persistLastUsedSettingsIfReady()
        }
    }
    @Published var depthColorBlendPercent: Double = 50.0 {
        didSet {
            pushDisplayParams()
        }
    }
    @Published var cutoffEnabled: Bool = false {
        didSet {
            pushDisplayParams()
            persistLastUsedSettingsIfReady()
            pushHostStateSync()
        }
    }
    @Published var cutoffNearMm: Double = 500.0 {
        didSet {
            if cutoffFarMm < cutoffNearMm + 50.0 {
                cutoffFarMm = cutoffNearMm + 50.0
            }
            pushDisplayParams()
            persistLastUsedSettingsIfReady()
            pushHostStateSync()
        }
    }
    @Published var cutoffFarMm: Double = 2000.0 {
        didSet {
            if cutoffFarMm < cutoffNearMm + 50.0 {
                cutoffNearMm = max(200.0, cutoffFarMm - 50.0)
            }
            pushDisplayParams()
            persistLastUsedSettingsIfReady()
            pushHostStateSync()
        }
    }

    @Published var crispPixelScaling: Bool = true {
        didSet {
            syncSecondDisplayFrame()
        }
    }

    // Phase 4: Vision Body & Hand Skeleton Overlay + Second-Display Projector Output
    @Published var skeletonOverlayEnabled: Bool = false {
        didSet {
            handleSkeletonOverlayToggleChange()
        }
    }
    @Published var skeletonConfidenceThreshold: Double = 0.35 {
        didSet {
            reprojectActiveSkeletonOverlay()
            if selectedMode == .pointCloud3D {
                rerenderCachedPointCloudIfNeeded(userCameraInteraction: true)
            }
        }
    }
    @Published var activeSkeletonOverlay = ProjectedSkeletonOverlay() {
        didSet {
            syncSecondDisplayFrame()
        }
    }
    @Published var detectedBodiesCount: Int = 0
    @Published var detectedHandsCount: Int = 0

    @Published var availableDisplays: [DisplayTargetInfo] = []
    @Published var selectedSecondDisplayID: CGDirectDisplayID?
    @Published var isSecondDisplayActive: Bool = false
    @Published var isSecondDisplayBlackedOut: Bool = false {
        didSet {
            secondDisplayController.setBlackedOut(isSecondDisplayBlackedOut)
        }
    }

    // iPhone Live Stream, Pairing & Phase 5 Remote Power Control State
    @Published var isPhonePaired: Bool = false
    @Published var isPhoneConnected: Bool = false
    @Published var pinnedPhoneFingerprint: String = "None"
    @Published var allowPhoneModeSwitch: Bool = true {
        didSet {
            streamServer?.allowPhoneModeSwitch = allowPhoneModeSwitch
            pushHostStateSync()
        }
    }
    @Published var allowPhoneSensorPowerControl: Bool = true {
        didSet {
            streamServer?.allowPhoneSensorPowerControl = allowPhoneSensorPowerControl
            pushHostStateSync()
        }
    }
    @Published var turnOffKinectOnPhoneDisconnect: Bool = false {
        didSet {
            streamServer?.turnOffKinectOnPhoneDisconnect = turnOffKinectOnPhoneDisconnect
        }
    }
    @Published var sensorPowerState: WireSensorPowerState = .off
    @Published var sensorInUseHolder: String?
    @Published var sensorPowerError: String?
    var simulatedPowerOnDelayMs: UInt64 = 0
    var simulatedUsbPresentOverrideForTesting: Bool?
    private(set) var currentPowerGenerationForTesting: UInt64 = 0
    private var pendingPowerOnGeneration: UInt64 = 0 {
        didSet { currentPowerGenerationForTesting = pendingPowerOnGeneration }
    }
    private var isAwaitingAsyncPowerOn: Bool = false
    private var hostStateSeq: UInt64 = 0
    private(set) var hostStateSyncPushCountForTesting: UInt64 = 0
    private(set) var receivedRemoteControlCommandsForTesting: [RemoteControlCommandMessage] = []
    private var lastRecordingProgressSyncNs: UInt64 = 0
    private let hostStateSnapshotBox = HostStateSnapshotBox()
    @Published var showPairingSheet: Bool = false
    @Published var activePairingOffer: ActivePairingOffer?
    @Published var pairingQRImage: CGImage?
    @Published var pairingSecondsRemaining: Int = 0
    @Published var streamBitrateMbps: Double = 0.0
    @Published var phoneDisplayLatencyMs: Double = 0.0
    @Published var phoneBarcodeLatencyMs: Double = 0.0
    @Published var rejectedTlsHandshakes: Int = 0
    @Published var streamingSetupError: String?
    @Published private(set) var streamingPairingKeyError: HostPairingKeyError?
    @Published var showResetPairingConfirmation: Bool = false
    private var streamingStateDirURL: URL?
    private var streamingIdentityStore: HostIdentityKeyStore?

    var canOfferResetPairing: Bool {
        streamingPairingKeyError?.allowsResetPairing == true
    }

    // Host-Only Recording State & Phase 2 In-App Recordings / Playback / Reminder
    @Published var isRecording: Bool = false
    @Published var recordingLosslessOption: Lossless16ContainerOption = .ffv1Mkv
    @Published var recordingFormatLabel: String = "Idle"
    @Published var recordingFrames: UInt64 = 0
    @Published var recordingElapsedSec: Double = 0.0
    @Published var recordingMegabytes: Double = 0.0
    @Published var recordingMBPerMinute: Double = 0.0
    @Published var recordingExclusionsOK: Bool = false
    @Published var recordedSessions: [RecordingSessionItem] = []
    @Published var showRecordingReminderDialog: Bool = false
    @Published var recordingReminderDontAskOneHour: Bool = false
    private(set) var recordingReminderSuppressedUntil: Date?

    @Published var isReplayingRecording: Bool = false
    @Published var isReplayPlaying: Bool = false
    @Published var activeReplaySession: RecordingSessionItem?
    @Published var replayFrameIndex: Int = 0
    @Published var replayTotalFrames: Int = 0
    var nowProvider: () -> Date = { Date() }

    // Live Telemetry (Numbers)
    @Published var currentFps: Double = 0.0
    @Published var colorFps: Double = 0.0
    @Published var irFrameCount: UInt64 = 0
    @Published var depthFrameCount: UInt64 = 0
    @Published var colorFrameCount: UInt64 = 0
    @Published var renderedFrameCount: UInt64 = 0
    @Published var totalDropCount: UInt64 = 0
    @Published var arrivalToDisplayMs: Double = 0.0
    @Published var interFrameMeanMs: Double = 0.0
    @Published var interFrameP95Ms: Double = 0.0
    @Published var mapTimeMeanMs: Double = 0.0
    @Published var smoothedLow: Float = 0.0
    @Published var smoothedHigh: Float = 65535.0
    @Published var rawMin: Float = 0.0
    @Published var rawMax: Float = 0.0
    @Published var rawMean: Float = 0.0
    @Published var saturatedPixels: UInt32 = 0
    @Published var invalidPixels: UInt32 = 0
    @Published var colorExposureFooter: Float = 0.0
    @Published var colorGainFooter: Float = 1.0
    @Published var readbackIntegrationMs: Float = 0.0
    @Published var readbackAnalogGain: Float = 0.0
    @Published var serialNumber: String = "—"
    @Published var usbPresent: Bool = false
    @Published var lockBlocked: Bool = false
    @Published var frameDimensions: String = "512 × 424"
    private var lastFrameWidth: Int = 512
    private var lastFrameHeight: Int = 424
    private let helloSnapshotBox = StreamHelloSnapshotBox()

    private var engine: OpaquePointer?
    private let streamEncoder = VideoToolboxEncoder()
    private var streamServer: HostStreamServer?
    let recorder: HostRecorder
    let presetsStore: PresetsStore
    private var isApplyingPresetOrInit: Bool = false
    private let lifecycleMonitor = HardwareLifecycleMonitor()
    private var awaitingUsbReplug: Bool = false
    private var resumeAfterWake: Bool = false
    private var didPerformAutostart: Bool = false
    private var launchObserver: NSObjectProtocol?
    private var keyMonitor: Any?
    private var lastRecordedRaw16Seq: UInt64 = 0
    private var raw16Scratch = [UInt16](repeating: 0, count: 512 * 424)
    private var liveDepthMm = [Float](repeating: 0, count: 512 * 424)
    private var liveIrRaw = [Float](repeating: 0, count: 512 * 424)
    private var liveIntrinsics = kv_ir_intrinsics_t()
    private var frozenDepthMm = [Float](repeating: 0, count: 512 * 424)
    private var frozenIrRaw = [Float](repeating: 0, count: 512 * 424)
    private var frozenIntrinsics = kv_ir_intrinsics_t()
    private var frozenBgraData = Data()
    private var frozenWidth: Int = 512
    private var frozenHeight: Int = 424
    private var frozenMode: ViewerModeOption = .ir
    private var frozenSbsLayout: SbsLayoutOption = .irDepth
    private var lastHoverViewPoint: CGPoint?
    private var lastHoverViewSize: CGSize = .zero
    private var lastSeenSeq: UInt64 = 0
    private var lastPointCloudSeq: UInt64 = 0
    private var pointCloudEncodeSeq: UInt64 = 0
    let pointCloudRenderer = MetalPointCloudRenderer()
    private var cachedUndistortedDepthMm = [Float](repeating: 0, count: 512 * 424)
    private var cachedPointBgra = [UInt8](repeating: 0, count: 512 * 424 * 4)
    private var cachedPointIntrinsics = kv_ir_intrinsics_t()
    private var hasCachedPointCloudFrame: Bool = false
    private var pointCloudRenderBuffer = Data()
    private var pointCloudStreamCleanBuffer = Data()
    private let visionEstimator = VisionSkeletonEstimator()
    private var lastVisionColorSeq: UInt64 = 0
    private var visionColorBuffer = Data()
    private var latestVisionSnapshot = RawVisionPoseSnapshot()
    let secondDisplayController = SecondDisplayOutputController()
    private var screenParamsObserver: NSObjectProtocol?
    private var frameBuffer = Data()
    private var pollTimer: Timer?
    private var retentionTimer: Timer?
    private var replayReader: RecordingSessionReader?
    private var replaySessionGeneration: UInt64 = 0
    private var isReplayDecodeInFlight: Bool = false
    private var pendingReplayTargetFrame: Int?
    private var pendingReplayPrefetchNext: Bool = false
    private var nextReplayStepNs: UInt64 = 0
    private var statsTick: Int = 0
    private let colorSpace = CGColorSpaceCreateDeviceRGB()
    private let customLockFilePath: String?

    var activeStreamPort: UInt16 {
        streamServer?.streamPort ?? 0
    }

    var hostServerCertSHA256: Data? {
        streamServer?.serverCertSHA256
    }

    init(
        recordingsDirectoryURL: URL? = nil,
        stateDirectoryURL: URL? = nil,
        identityStore: HostIdentityKeyStore? = nil,
        lockFileURL: URL? = nil,
        enableStreamingServer: Bool? = nil,
        autostart: Bool? = nil
    ) {
        let isXCTest = ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
        let effectiveStateDir: URL? = stateDirectoryURL ?? (isXCTest
            ? FileManager.default.temporaryDirectory.appendingPathComponent("kv-xctest-state-\(UUID().uuidString)", isDirectory: true)
            : nil)
        self.customLockFilePath = lockFileURL?.path
        self.recorder = HostRecorder(
            baseDirectoryURL: recordingsDirectoryURL,
            skipTimeMachineSubprocess: recordingsDirectoryURL != nil || isXCTest
        )
        self.presetsStore = PresetsStore(stateDirectoryURL: effectiveStateDir)
        if let customPath = lockFileURL?.path {
            engine = customPath.withCString { cPath in
                kv_engine_create(cPath)
            }
        } else {
            engine = kv_engine_create(nil)
        }
        if isXCTest || CommandLine.arguments.contains("--synthetic") {
            useSyntheticSource = true
        }
        if CommandLine.arguments.contains("--clock-overlay") {
            syntheticClockOverlay = true
        }

        presets = presetsStore.allPresets
        if let saved = presetsStore.lastUsedSettings {
            applyPreset(saved, persistAsLastUsed: false, restartPipelineIfNeeded: false)
        }

        helloSnapshotBox.update(modeRaw: selectedMode.rawValue, sbsRaw: selectedSbsLayout.rawValue)
        pushHostStateSync()
        if enableStreamingServer ?? !isXCTest {
            setupStreamingServer(stateDirectoryURL: effectiveStateDir, identityStore: identityStore)
        }
        if !isXCTest {
            setupHardwareLifecycleCallbacks()
            installKeyboardShortcutsMonitor()
        }
        refreshAvailableDisplays()
        screenParamsObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in
                self?.refreshAvailableDisplays()
                self?.secondDisplayController.handleScreenParametersChanged()
                self?.isSecondDisplayActive = self?.secondDisplayController.isOutputActive ?? false
            }
        }
        let audit = recorder.auditExclusions()
        recordingExclusionsOK = audit.exists && audit.hasNoSyncFile && audit.hasMetadataNeverIndex && (audit.tmutilExcluded || audit.resourceExcludedFromBackup)
        runExpiredRecordingsSweep()
        startRetentionSweepTimer()
        startPollTimer()
        let shouldAutostart = autostart ?? (!isXCTest && !CommandLine.arguments.contains("--no-autostart"))
        if shouldAutostart {
            scheduleAutostartAfterFinishLaunching()
        }
    }

    private func scheduleAutostartAfterFinishLaunching() {
        if NSRunningApplication.current.isFinishedLaunching {
            DispatchQueue.main.async { [weak self] in
                self?.performAutostartIfNeeded()
            }
        } else {
            launchObserver = NotificationCenter.default.addObserver(
                forName: NSApplication.didFinishLaunchingNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                // Defer one main-runloop turn past finishLaunching so NSRunningApplication.isFinishedLaunching == true
                // before libfreenect2's OpenGLPacketPipeline calls glfwInit() (avoiding GLFW's [NSApp stop:nil] hook).
                DispatchQueue.main.async {
                    Task { @MainActor in
                        self?.performAutostartIfNeeded()
                    }
                }
            }
        }
    }

    private func performAutostartIfNeeded() {
        if let obs = launchObserver {
            NotificationCenter.default.removeObserver(obs)
            launchObserver = nil
        }
        guard !didPerformAutostart else { return }
        didPerformAutostart = true
        if !isRunning {
            startStream()
        }
    }

    private func setupStreamingServer(stateDirectoryURL: URL? = nil, identityStore: HostIdentityKeyStore? = nil) {
        self.streamingStateDirURL = stateDirectoryURL
        self.streamingIdentityStore = identityStore
        do {
            let server = try HostStreamServer(stateDirectoryURL: stateDirectoryURL, identityStore: identityStore)
            self.streamServer = server
            self.streamingSetupError = nil
            self.streamingPairingKeyError = nil
            self.showResetPairingConfirmation = false
            let initialAllowSwitch = server.allowPhoneModeSwitch
            if self.allowPhoneModeSwitch != initialAllowSwitch {
                self.allowPhoneModeSwitch = initialAllowSwitch
            }
            let initialAllowPower = server.allowPhoneSensorPowerControl
            if self.allowPhoneSensorPowerControl != initialAllowPower {
                self.allowPhoneSensorPowerControl = initialAllowPower
            }
            let initialTurnOffDisconnect = server.turnOffKinectOnPhoneDisconnect
            if self.turnOffKinectOnPhoneDisconnect != initialTurnOffDisconnect {
                self.turnOffKinectOnPhoneDisconnect = initialTurnOffDisconnect
            }

            let encoder = self.streamEncoder
            let snapBox = self.helloSnapshotBox
            let stateBox = self.hostStateSnapshotBox
            encoder.onEncodedFrame = { [weak server] encoded in
                server?.broadcastEncodedFrame(encoded) {
                    encoder.requestImmediateKeyframeAndConfig()
                }
            }

            server.onClientConnectedNeedsKeyframe = { [weak server] in
                encoder.requestImmediateKeyframeAndConfig()
                let snap = snapBox.snapshot()
                let allowSwitch = server?.allowPhoneModeSwitch ?? true
                return HelloServerMessage(
                    protocolVersion: 2,
                    modeRaw: snap.modeRaw,
                    sbsLayoutRaw: snap.sbsRaw,
                    allowPhoneModeSwitch: allowSwitch,
                    width: snap.width,
                    height: snap.height,
                    codecRaw: WireCodecKind.h264.rawValue
                )
            }

            server.onClientConnectedInitialState = {
                stateBox.snapshot()
            }

            // Route all remote client requests via strict FIFO main-queue hops rather than unstructured Tasks
            // so rapid On/Off and control sequences execute in exact arrival order.
            server.onClientRequestedMode = { [weak self] modeRaw, sbsRaw in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        guard let self = self else { return }
                        if let m = ViewerModeOption(rawValue: modeRaw) {
                            self.selectedMode = m
                        }
                        if let s = SbsLayoutOption(rawValue: sbsRaw) {
                            self.selectedSbsLayout = s
                        }
                    }
                }
            }

            server.onClientRemoteCommand = { [weak self] cmd in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        self?.handleRemoteControlCommand(cmd)
                    }
                }
            }

            server.onClientOrbit3DGesture = { [weak self] orbit in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        self?.handleRemoteOrbit3DGesture(orbit)
                    }
                }
            }

            server.onClientSensorPowerRequest = { [weak self] powerReq in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        self?.requestSensorPower(turnOn: powerReq.turnOn)
                    }
                }
            }

            server.onPairedClientDisconnected = { [weak self] in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        guard let self = self else { return }
                        if self.turnOffKinectOnPhoneDisconnect {
                            self.requestSensorPower(turnOn: false)
                        }
                    }
                }
            }

            server.onPairingStateChanged = { [weak self] in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        guard let self = self else { return }
                        self.syncStreamingStatus()
                        if self.isPhoneConnected {
                            self.pushHostStateSync()
                        }
                    }
                }
            }

            pushHostStateSync()
            syncStreamingStatus()
        } catch let keyErr as HostPairingKeyError {
            let msg = keyErr.localizedDescription
            streamingPairingKeyError = keyErr
            streamingSetupError = msg
            errorBanner = msg
        } catch {
            let msg = "Streaming listener failed to start: \(error.localizedDescription)"
            streamingPairingKeyError = nil
            streamingSetupError = msg
            errorBanner = msg
        }
    }

    func retryStreamingServerSetup() {
        guard streamServer == nil else { return }
        let prevSetupError = streamingSetupError
        setupStreamingServer(stateDirectoryURL: streamingStateDirURL, identityStore: streamingIdentityStore)
        if streamingSetupError == nil, errorBanner == prevSetupError {
            errorBanner = nil
        }
    }

    func requestResetPairing() {
        guard streamServer == nil, canOfferResetPairing else { return }
        showResetPairingConfirmation = true
    }

    func cancelResetPairing() {
        showResetPairingConfirmation = false
    }

    func confirmResetPairing() {
        guard showResetPairingConfirmation, streamServer == nil, canOfferResetPairing else { return }
        showResetPairingConfirmation = false
        let prevSetupError = streamingSetupError
        do {
            try HostStreamServer.resetPairingAfterBackup(
                stateDirectoryURL: streamingStateDirURL,
                identityStore: streamingIdentityStore
            )
            setupStreamingServer(stateDirectoryURL: streamingStateDirURL, identityStore: streamingIdentityStore)
            if streamingSetupError == nil, errorBanner == prevSetupError {
                errorBanner = nil
            }
        } catch let keyErr as HostPairingKeyError {
            let msg = keyErr.localizedDescription
            streamingPairingKeyError = keyErr
            streamingSetupError = msg
            errorBanner = msg
        } catch {
            let msg = "Could not reset pairing: \(error.localizedDescription)"
            streamingSetupError = msg
            errorBanner = msg
        }
    }

    func openPairingWindow() {
        if streamServer == nil {
            retryStreamingServerSetup()
        }
        guard let server = streamServer else {
            if let setupErr = streamingSetupError {
                errorBanner = setupErr
            }
            return
        }
        do {
            let offer = try server.beginPairingWindow(durationSec: 120.0)
            activePairingOffer = offer
            pairingQRImage = HostStreamServer.makeQRCodeCGImage(from: offer.qrURI, scale: 8.0)
            pairingSecondsRemaining = max(0, Int(offer.expiresAt.timeIntervalSinceNow.rounded(.up)))
            showPairingSheet = true
            syncStreamingStatus()
        } catch {
            errorBanner = "Could not open pairing listener: \(error.localizedDescription)"
        }
    }

    func cancelPairingWindow() {
        streamServer?.cancelPairingWindow()
        activePairingOffer = nil
        pairingQRImage = nil
        showPairingSheet = false
        syncStreamingStatus()
    }

    func unpairPhone() {
        streamServer?.unpairPhone()
        syncStreamingStatus()
    }

    func disconnectActivePhoneClientForTesting() {
        streamServer?.disconnectActiveClient()
        syncStreamingStatus()
    }

    func setSimulatedServerProtocolVersionForTesting(_ version: Int?) {
        streamServer?.setSimulatedServerProtocolVersionForTesting(version)
    }

    func setClientPingTimeoutForTesting(seconds: Double) {
        streamServer?.setClientPingTimeoutForTesting(seconds: seconds)
    }

    var configuredClientPingTimeoutSeconds: Double {
        streamServer?.configuredClientPingTimeoutSeconds ?? 5.0
    }

    private func buildCurrentHostStateSync(seq: UInt64) -> HostStateSyncMessage {
        let disabledReason: String?
        if !canRecordCurrentMode {
            disabledReason = HostRecorderError.colourLiveOnlyTooltip
        } else if isReplayingRecording {
            disabledReason = "Exit recording playback before starting a new live recording."
        } else {
            disabledReason = nil
        }

        let powerDisabledReason: String? = allowPhoneSensorPowerControl
            ? nil
            : SharedRecordingReminderConstants.sensorPowerDisabledOnHostText

        return HostStateSyncMessage(
            protocolVersion: 2,
            stateSeq: seq,
            powerState: sensorPowerState,
            inUseHolder: sensorPowerState == .inUseElsewhere ? sensorInUseHolder : nil,
            errorMessage: sensorPowerState == .error ? sensorPowerError : nil,
            modeRaw: selectedMode.rawValue,
            sbsLayoutRaw: selectedSbsLayout.rawValue,
            allowPhoneModeSwitch: allowPhoneModeSwitch,
            allowPhoneSensorPower: allowPhoneSensorPowerControl,
            sensorPowerDisabledReason: powerDisabledReason,
            cutoffEnabled: cutoffEnabled,
            cutoffNearMm: Float(cutoffNearMm),
            cutoffFarMm: Float(cutoffFarMm),
            isHostFrozen: isFrozen,
            isRecording: isRecording,
            canRecordCurrentMode: canRecordCurrentMode && !isReplayingRecording,
            recordingDisabledReason: disabledReason,
            recordingReminderNeeded: isRecordingReminderNeeded,
            recordingFrames: recordingFrames,
            recordingElapsedSec: recordingElapsedSec,
            isReplayingOnHost: isReplayingRecording
        )
    }

    func pushHostStateSync() {
        hostStateSeq &+= 1
        hostStateSyncPushCountForTesting &+= 1
        let msg = buildCurrentHostStateSync(seq: hostStateSeq)
        hostStateSnapshotBox.update(msg)
        streamServer?.broadcastHostState(msg)
    }

    private func syncStreamingStatus() {
        guard let server = streamServer else { return }
        isPhonePaired = server.isPhonePaired
        isPhoneConnected = server.pairedClientConnected
        pinnedPhoneFingerprint = server.pinnedClientFingerprintShort
        streamBitrateMbps = server.currentBitrateMbps
        phoneDisplayLatencyMs = server.lastClientDisplayLatencyMs
        phoneBarcodeLatencyMs = server.lastClientBarcodeLatencyMs
        rejectedTlsHandshakes = server.rejectedHandshakeCount

        if let offer = server.currentPairingOffer {
            activePairingOffer = offer
            pairingSecondsRemaining = max(0, Int(offer.expiresAt.timeIntervalSinceNow.rounded(.up)))
        } else {
            if showPairingSheet && isPhonePaired {
                showPairingSheet = false
            }
            activePairingOffer = nil
            pairingQRImage = nil
            pairingSecondsRemaining = 0
        }
    }

    var canRecordCurrentMode: Bool {
        HostRecorder.canRecord(mode: selectedMode.cValue) && !depthColorBlendEnabled
    }

    var recordControlTooltip: String {
        if !canRecordCurrentMode {
            return HostRecorderError.colourLiveOnlyTooltip
        }
        if isReplayingRecording {
            return "Exit recording playback before starting a new live recording."
        }
        return isRecording
            ? "Stop recording 16-bit depth/IR to ~/kinect-viewer-recordings/"
            : "Record lossless 16-bit IR or Depth on this Mac"
    }

    var isRecordingReminderNeeded: Bool {
        guard let until = recordingReminderSuppressedUntil else { return true }
        return nowProvider() >= until
    }

    func requestToggleRecordingWithReminder() {
        if isRecording {
            stopHostRecording()
            return
        }
        guard canRecordCurrentMode else {
            errorBanner = HostRecorderError.colourLiveOnlyTooltip
            return
        }
        guard isRunning && !isReplayingRecording else {
            errorBanner = isReplayingRecording
                ? "Exit recording playback before starting a new live recording."
                : "Start the Kinect or Synthetic stream before recording."
            return
        }
        if isRecordingReminderNeeded {
            recordingReminderDontAskOneHour = false
            showRecordingReminderDialog = true
        } else {
            startHostRecording()
        }
    }

    func confirmRecordingReminderAndStart() {
        if recordingReminderDontAskOneHour {
            recordingReminderSuppressedUntil = nowProvider().addingTimeInterval(PreRecordingReminderPolicy.snoozeDurationSeconds)
        }
        showRecordingReminderDialog = false
        startHostRecording()
        pushHostStateSync()
    }

    func cancelRecordingReminder() {
        showRecordingReminderDialog = false
        pushHostStateSync()
    }

    func toggleRecording() {
        if isRecording {
            stopHostRecording()
        } else {
            startHostRecording()
        }
    }

    @discardableResult
    func startHostRecording() -> Bool {
        guard canRecordCurrentMode else {
            errorBanner = HostRecorderError.colourLiveOnlyTooltip
            return false
        }
        guard isRunning && !isReplayingRecording else {
            errorBanner = isReplayingRecording
                ? "Exit recording playback before starting a new live recording."
                : "Start the Kinect or Synthetic stream before recording."
            return false
        }
        do {
            lastRecordedRaw16Seq = 0
            pullLatestInspectionBuffersFromEngine()
            let activeSource: kv_source_kind_t = useSyntheticSource ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL
            _ = try recorder.startRecording(
                source: activeSource,
                mode: selectedMode.cValue,
                width: lastFrameWidth,
                height: lastFrameHeight,
                losslessOption: recordingLosslessOption,
                irIntrinsics: liveIntrinsics
            )
            let audit = recorder.auditExclusions()
            recordingExclusionsOK = audit.exists && audit.hasNoSyncFile && audit.hasMetadataNeverIndex && (audit.tmutilExcluded || audit.resourceExcludedFromBackup)
            syncRecordingStatus()
            return true
        } catch {
            errorBanner = "Recording failed to start: \(error.localizedDescription)"
            return false
        }
    }

    func stopHostRecording() {
        _ = recorder.stopRecording()
        syncRecordingStatus()
        refreshRecordedSessions()
    }

    func revealRecordingsFolder() {
        NSWorkspace.shared.open(recorder.baseDirURL)
    }

    // MARK: - Phase 2 Recordings List, 7-Day Auto-Delete & In-App Replay

    private func startRetentionSweepTimer() {
        retentionTimer?.invalidate()
        retentionTimer = Timer.scheduledTimer(withTimeInterval: 3600.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.runExpiredRecordingsSweep()
            }
        }
    }

    @discardableResult
    func runExpiredRecordingsSweep() -> [String] {
        let now = nowProvider()
        let activeRecPath = recorder.currentStatus.isRecording ? recorder.currentStatus.sessionFolderPath : nil
        let activeReplayPath = activeReplaySession?.folderURL.path

        // Purge any disallowed non-IR/non-depth sessions (e.g. legacy *_color1080p.nosync / *_sidebyside.nosync)
        // on launch and log each in deletion-log.txt as automatic.
        var deleted = RecordingLibrary.purgeDisallowedSessions(
            in: recorder.baseDirURL,
            now: now,
            excludingActiveFolderPath: activeRecPath
        )

        let sessions = RecordingLibrary.listSessions(in: recorder.baseDirURL, excludingActiveFolderPath: activeRecPath)
        for session in sessions where session.isExpired(at: now) {
            if let rp = activeReplayPath,
               session.folderURL.standardizedFileURL.path == URL(fileURLWithPath: rp).standardizedFileURL.path {
                exitPlayback(resumeLive: false)
            }
            let sizeBeforeDelete = session.sizeBytes
            if (try? FileManager.default.removeItem(at: session.folderURL)) != nil {
                RecordingLibrary.appendDeletionLogEntry(
                    in: recorder.baseDirURL,
                    fileName: session.folderName,
                    sizeBytes: sizeBeforeDelete,
                    timestamp: now,
                    trigger: .automatic
                )
                deleted.append(session.folderName)
            }
        }
        refreshRecordedSessions()
        return deleted
    }

    func refreshRecordedSessions() {
        let activeRecPath = recorder.currentStatus.isRecording ? recorder.currentStatus.sessionFolderPath : nil
        recordedSessions = RecordingLibrary.listSessions(
            in: recorder.baseDirURL,
            excludingActiveFolderPath: activeRecPath
        )
    }

    func deleteRecordingSession(_ session: RecordingSessionItem) {
        if activeReplaySession?.id == session.id {
            exitPlayback(resumeLive: false)
        }
        do {
            try RecordingLibrary.deleteSessionByUser(session, in: recorder.baseDirURL, now: nowProvider())
            refreshRecordedSessions()
        } catch {
            errorBanner = "Could not delete recording: \(error.localizedDescription)"
        }
    }

    @discardableResult
    func startPlayback(of session: RecordingSessionItem) -> Bool {
        guard let eng = engine else { return false }
        if isRecording {
            stopHostRecording()
        }
        errorBanner = nil
        isFrozen = false
        clearInspectionOverlays()

        let reader = RecordingSessionReader(session: session)
        guard reader.totalFrames > 0 else {
            errorBanner = "Recording session contains 0 frames."
            return false
        }

        isApplyingPresetOrInit = true
        depthColorBlendEnabled = false
        skeletonOverlayEnabled = false
        if pointCloudColorSource == .registeredColor {
            pointCloudColorSource = .depth
        }
        selectedMode = session.isDepthMode ? .depth : .ir
        isApplyingPresetOrInit = false
        helloSnapshotBox.update(modeRaw: selectedMode.rawValue, sbsRaw: selectedSbsLayout.rawValue)

        var params = buildDisplayParams()
        var intr = session.irIntrinsics?.cValue ?? kv_ir_intrinsics_t()
        let hasValidIntr = intr.valid
        let started = withUnsafePointer(to: &intr) { intrPtr -> Bool in
            kv_engine_start_replay(eng, &params, hasValidIntr ? intrPtr : nil)
        }
        guard started else {
            errorBanner = "Failed to initialize replay pipeline."
            return false
        }

        lastSeenSeq = 0
        lastPointCloudSeq = 0
        hasCachedPointCloudFrame = false
        replaySessionGeneration &+= 1
        isReplayDecodeInFlight = false
        pendingReplayTargetFrame = nil
        pendingReplayPrefetchNext = false
        replayReader = reader
        activeReplaySession = session
        replayTotalFrames = reader.totalFrames
        replayFrameIndex = 0
        isReplayingRecording = true
        isRunning = true

        isReplayPlaying = true
        // Wait until frame 0 finishes async off-main-actor decode before counting down to frame 1.
        nextReplayStepNs = .max
        sensorPowerState = .off
        refreshStats()
        pushHostStateSync()
        scheduleReplayDecodeOffMainActor(frameIndex: 0, prefetchNextChunk: true, failPlaybackOnNil: true)
        return true
    }

    func togglePlaybackPause() {
        guard isReplayingRecording else { return }
        isReplayPlaying.toggle()
        if isReplayPlaying, let reader = replayReader {
            let dtNs = UInt64(reader.frameIntervalSeconds(from: replayFrameIndex) * 1e9)
            nextReplayStepNs = kv_monotonic_ns() &+ dtNs
        }
    }

    func seekPlayback(toFrame index: Int) {
        guard isReplayingRecording, replayTotalFrames > 0 else { return }
        let clamped = max(0, min(replayTotalFrames - 1, index))
        if isFrozen {
            isFrozen = false
        }
        scheduleReplayDecodeOffMainActor(frameIndex: clamped, prefetchNextChunk: isReplayPlaying)
        if isReplayPlaying, let reader = replayReader {
            let dtNs = UInt64(reader.frameIntervalSeconds(from: clamped) * 1e9)
            nextReplayStepNs = kv_monotonic_ns() &+ dtNs
        }
    }

    func stepPlayback(by delta: Int) {
        guard isReplayingRecording, replayTotalFrames > 0 else { return }
        isReplayPlaying = false
        if isFrozen {
            isFrozen = false
        }
        let target = max(0, min(replayTotalFrames - 1, replayFrameIndex + delta))
        scheduleReplayDecodeOffMainActor(frameIndex: target, prefetchNextChunk: false)
    }

    func flushPendingReplayWorkForTesting() async {
        while isReplayDecodeInFlight || pendingReplayTargetFrame != nil {
            if let reader = replayReader {
                await reader.waitForIdleForTesting()
            }
            await Task.yield()
            await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                DispatchQueue.main.async {
                    cont.resume()
                }
            }
        }
    }

    func exitPlayback(resumeLive: Bool = false) {
        clearReplayStateOnly()
        if let eng = engine {
            kv_engine_stop(eng)
        }
        isRunning = false
        sensorPowerState = .off
        previewImage = nil
        refreshStats()
        pushHostStateSync()
        if resumeLive {
            startStream()
        }
    }

    private func clearReplayStateOnly() {
        replaySessionGeneration &+= 1
        isReplayDecodeInFlight = false
        pendingReplayTargetFrame = nil
        pendingReplayPrefetchNext = false
        isReplayPlaying = false
        isReplayingRecording = false
        activeReplaySession = nil
        replayReader = nil
        replayFrameIndex = 0
        replayTotalFrames = 0
        nextReplayStepNs = 0
    }

    private func scheduleReplayDecodeOffMainActor(
        frameIndex: Int,
        prefetchNextChunk: Bool,
        failPlaybackOnNil: Bool = false
    ) {
        guard isReplayingRecording,
              let reader = replayReader,
              let session = activeReplaySession else {
            return
        }
        replayFrameIndex = frameIndex
        if isReplayDecodeInFlight {
            pendingReplayTargetFrame = frameIndex
            pendingReplayPrefetchNext = prefetchNextChunk
            return
        }

        isReplayDecodeInFlight = true
        let gen = replaySessionGeneration
        let ticks = reader.deviceTicks(forFrame: frameIndex)
        reader.readFrame16Async(at: frameIndex, prefetchNextChunk: prefetchNextChunk) { [weak self] pixels in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let self = self,
                          self.isReplayingRecording,
                          self.replaySessionGeneration == gen else {
                        return
                    }
                    self.isReplayDecodeInFlight = false
                    if let pixels = pixels, let eng = self.engine {
                        let ok = pixels.withUnsafeBufferPointer { buf -> Bool in
                            guard let base = buf.baseAddress else { return false }
                            return kv_engine_push_replay_frame16(
                                eng,
                                session.isDepthMode,
                                base,
                                UInt32(session.width),
                                UInt32(session.height),
                                UInt32(frameIndex),
                                ticks
                            )
                        }
                        if ok {
                            self.replayFrameIndex = frameIndex
                            _ = self.consumeLatestMappedFrameFromEngine()
                            if failPlaybackOnNil, self.isReplayPlaying, let activeReader = self.replayReader {
                                let dtNs = UInt64(activeReader.frameIntervalSeconds(from: frameIndex) * 1e9)
                                self.nextReplayStepNs = kv_monotonic_ns() &+ dtNs
                            }
                        }
                    } else if failPlaybackOnNil {
                        self.exitPlayback(resumeLive: false)
                        self.errorBanner = "Failed to decode frame 0 from recording."
                        return
                    }

                    if let nextTarget = self.pendingReplayTargetFrame {
                        let nextPrefetch = self.pendingReplayPrefetchNext
                        self.pendingReplayTargetFrame = nil
                        self.pendingReplayPrefetchNext = false
                        self.scheduleReplayDecodeOffMainActor(
                            frameIndex: nextTarget,
                            prefetchNextChunk: nextPrefetch
                        )
                    }
                }
            }
        }
    }

    private func syncRecordingStatus() {
        let wasRecording = isRecording
        let prevFrames = recordingFrames
        let st = recorder.currentStatus
        isRecording = st.isRecording
        recordingFormatLabel = st.formatSummary
        recordingFrames = st.framesWritten
        recordingElapsedSec = st.elapsedSec
        recordingMegabytes = Double(st.bytesOnDisk) / 1_000_000.0
        recordingMBPerMinute = st.megabytesPerMinute
        if let recErr = st.lastError {
            errorBanner = "Recorder error: \(recErr)"
        }
        let nowNs = kv_monotonic_ns()
        if wasRecording != isRecording {
            lastRecordingProgressSyncNs = nowNs
            pushHostStateSync()
        } else if isRecording && recordingFrames != prevFrames {
            // Throttle periodic recording progress syncs to ~1 per second while keeping state transitions instant.
            if lastRecordingProgressSyncNs == 0 || (nowNs > lastRecordingProgressSyncNs && (nowNs - lastRecordingProgressSyncNs) >= 1_000_000_000) {
                lastRecordingProgressSyncNs = nowNs
                pushHostStateSync()
            }
        }
    }

    private func setupHardwareLifecycleCallbacks() {
        lifecycleMonitor.onUsbDetached = { [weak self] in
            Task { @MainActor in
                self?.handleUsbUnplugged()
            }
        }
        lifecycleMonitor.onUsbAttached = { [weak self] in
            Task { @MainActor in
                self?.handleUsbReplugged()
            }
        }
        lifecycleMonitor.onSystemWillSleep = { [weak self] in
            Task { @MainActor in
                self?.handleSystemWillSleep()
            }
        }
        lifecycleMonitor.onSystemDidWake = { [weak self] in
            Task { @MainActor in
                self?.handleSystemDidWake()
            }
        }
    }

    func handleUsbUnplugged() {
        guard !useSyntheticSource else { return }
        if isRecording {
            stopHostRecording()
        }
        if isRunning, let eng = engine {
            kv_engine_stop(eng)
            streamEncoder.teardown()
            isRunning = false
            previewImage = nil
            awaitingUsbReplug = true
            statusText = "Kinect USB unplugged — waiting for replug…"
            sensorPowerError = statusText
            sensorPowerState = .error
            pushHostStateSync()
        }
    }

    func handleUsbReplugged() {
        guard !useSyntheticSource, awaitingUsbReplug else { return }
        let replugGen = pendingPowerOnGeneration
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 500_000_000)
            guard let self = self,
                  self.pendingPowerOnGeneration == replugGen,
                  self.awaitingUsbReplug,
                  !self.useSyntheticSource else { return }
            if kv_is_kinect_usb_present() {
                var ownerBuf = [CChar](repeating: 0, count: 256)
                if !kv_is_locked_by_other(nil, &ownerBuf, ownerBuf.count) {
                    self.awaitingUsbReplug = false
                    self.startStream()
                }
            }
        }
    }

    func handleSystemWillSleep() {
        pendingPowerOnGeneration &+= 1
        let shouldResume = (isRunning && !isReplayingRecording) || isAwaitingAsyncPowerOn
        isAwaitingAsyncPowerOn = false
        if isRecording {
            stopHostRecording()
        }
        if isReplayingRecording {
            clearReplayStateOnly()
        }
        if isRunning, let eng = engine {
            kv_engine_stop(eng)
        }
        streamEncoder.teardown()
        if shouldResume || isRunning || sensorPowerState != .off {
            resumeAfterWake = shouldResume
            isRunning = false
            previewImage = nil
            clearSkeletonOverlayState()
            sensorInUseHolder = nil
            sensorPowerError = nil
            sensorPowerState = .off
            statusText = "Paused for Mac sleep…"
            pushHostStateSync()
        }
    }

    func handleSystemDidWake(settleDelayNs: UInt64 = 850_000_000) {
        guard resumeAfterWake else { return }
        resumeAfterWake = false
        let wakeGen = pendingPowerOnGeneration
        Task { @MainActor [weak self] in
            if settleDelayNs > 0 {
                try? await Task.sleep(nanoseconds: settleDelayNs)
            }
            guard let self = self,
                  self.pendingPowerOnGeneration == wakeGen,
                  !self.isRunning else { return }
            let usbPresentNow = self.simulatedUsbPresentOverrideForTesting ?? kv_is_kinect_usb_present()
            if !self.useSyntheticSource && !usbPresentNow {
                self.awaitingUsbReplug = true
                let msg = "Woke from sleep — waiting for Kinect USB…"
                self.statusText = msg
                self.sensorInUseHolder = nil
                self.sensorPowerError = msg
                self.errorBanner = msg
                self.sensorPowerState = .error
                self.pushHostStateSync()
                return
            }
            self.startStream()
        }
    }

    func shutdown() {
        if let monitor = keyMonitor {
            NSEvent.removeMonitor(monitor)
            keyMonitor = nil
        }
        if let obs = screenParamsObserver {
            NotificationCenter.default.removeObserver(obs)
            screenParamsObserver = nil
        }
        secondDisplayController.stopOutput()
        isSecondDisplayActive = false
        lifecycleMonitor.stop()
        retentionTimer?.invalidate()
        retentionTimer = nil
        pollTimer?.invalidate()
        pollTimer = nil
        clearReplayStateOnly()
        if recorder.currentStatus.isRecording {
            _ = recorder.stopRecording()
        }
        streamServer?.shutdown()
        streamServer = nil
        streamEncoder.teardown()
        if let eng = engine {
            kv_engine_stop(eng)
            kv_engine_destroy(eng)
            engine = nil
        }
        isRunning = false
    }

    var currentModeNeedsColor: Bool {
        selectedMode == .color ||
        depthColorBlendEnabled ||
        (skeletonOverlayEnabled && !isReplayingRecording) ||
        (selectedMode == .pointCloud3D && pointCloudColorSource.needsLiveColor) ||
        (selectedMode == .sideBySide && selectedSbsLayout.needsColor)
    }

    private func checkExternalLockHolder() -> String? {
        guard !useSyntheticSource || customLockFilePath != nil else { return nil }
        var ownerBuf = [CChar](repeating: 0, count: 256)
        let locked: Bool
        if let customPath = customLockFilePath {
            locked = customPath.withCString { cPath in
                kv_is_locked_by_other(cPath, &ownerBuf, ownerBuf.count)
            }
        } else {
            locked = kv_is_locked_by_other(nil, &ownerBuf, ownerBuf.count)
        }
        guard locked else { return nil }
        let owner = Self.decodeCStringBuffer(ownerBuf).trimmingCharacters(in: .whitespacesAndNewlines)
        return owner.isEmpty ? "another process" : owner
    }

    func requestSensorPower(turnOn: Bool) {
        if !turnOn {
            pendingPowerOnGeneration &+= 1
            if isAwaitingAsyncPowerOn && !isRunning {
                isAwaitingAsyncPowerOn = false
                sensorInUseHolder = nil
                sensorPowerError = nil
                sensorPowerState = .off
                pushHostStateSync()
                return
            }
            isAwaitingAsyncPowerOn = false
            stopStream()
            return
        }

        // Turning On never starts a recording on its own (Phase 5.7).
        if isRunning && !isReplayingRecording && sensorPowerState == .on {
            pushHostStateSync()
            return
        }

        if let holder = checkExternalLockHolder() {
            pendingPowerOnGeneration &+= 1
            isAwaitingAsyncPowerOn = false
            isRunning = false
            lockBlocked = true
            previewImage = nil
            sensorInUseHolder = holder
            sensorPowerError = nil
            sensorPowerState = .inUseElsewhere
            errorBanner = SharedRecordingReminderConstants.inUseByMessage(holder: holder)
            pushHostStateSync()
            return
        }

        pendingPowerOnGeneration &+= 1
        let gen = pendingPowerOnGeneration
        isAwaitingAsyncPowerOn = true
        sensorInUseHolder = nil
        sensorPowerError = nil
        errorBanner = streamingSetupError
        sensorPowerState = .starting
        pushHostStateSync()

        let delayMs = simulatedPowerOnDelayMs
        Task { @MainActor [weak self] in
            guard let self = self else { return }
            if delayMs > 0 {
                try? await Task.sleep(nanoseconds: delayMs * 1_000_000)
            } else {
                await Task.yield()
            }
            guard self.pendingPowerOnGeneration == gen, self.isAwaitingAsyncPowerOn else {
                return
            }
            self.isAwaitingAsyncPowerOn = false
            self.startStream()
        }
    }

    func startStream(forceEnableRgb: Bool = false) {
        guard let eng = engine else { return }
        pendingPowerOnGeneration &+= 1
        isAwaitingAsyncPowerOn = false

        if isReplayingRecording {
            clearReplayStateOnly()
        }

        if let holder = checkExternalLockHolder() {
            isRunning = false
            lockBlocked = true
            previewImage = nil
            sensorInUseHolder = holder
            sensorPowerError = nil
            sensorPowerState = .inUseElsewhere
            errorBanner = SharedRecordingReminderConstants.inUseByMessage(holder: holder)
            pushHostStateSync()
            return
        }

        errorBanner = streamingSetupError
        sensorInUseHolder = nil
        sensorPowerError = nil
        if sensorPowerState != .starting {
            sensorPowerState = .starting
            pushHostStateSync()
        }

        lastSeenSeq = 0
        lastPointCloudSeq = 0
        lastVisionColorSeq = 0
        hasCachedPointCloudFrame = false

        var params = buildDisplayParams()
        let src: kv_source_kind_t = useSyntheticSource ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL
        if recorder.isRunningXCTest && src == KV_SOURCE_REAL {
            isRunning = false
            previewImage = nil
            let msg = HostRecorderError.realSensorInXCTestTooltip
            sensorInUseHolder = nil
            sensorPowerError = msg
            sensorPowerState = .error
            errorBanner = msg
            pushHostStateSync()
            return
        }
        let wantRgb = forceEnableRgb || currentModeNeedsColor
        var errBuf = [CChar](repeating: 0, count: 256)

        let ok = kv_engine_start_ex(eng, src, selectedPipeline.cValue, &params, wantRgb, &errBuf, errBuf.count)
        let errStr = Self.decodeCStringBuffer(errBuf)
        refreshStats()

        if !ok {
            isRunning = false
            previewImage = nil
            if lockBlocked {
                let holder = checkExternalLockHolder() ?? (errStr.isEmpty ? "another process" : errStr)
                sensorInUseHolder = holder
                sensorPowerError = nil
                sensorPowerState = .inUseElsewhere
                errorBanner = SharedRecordingReminderConstants.inUseByMessage(holder: holder)
            } else {
                let msg = errStr.isEmpty ? "Failed to start Kinect source (no fallback performed)." : errStr
                sensorInUseHolder = nil
                sensorPowerError = msg
                sensorPowerState = .error
                errorBanner = msg
            }
            pushHostStateSync()
        } else {
            isRunning = true
            sensorInUseHolder = nil
            sensorPowerError = nil
            sensorPowerState = .on
            streamEncoder.requestImmediateKeyframeAndConfig()
            if wantRgb {
                pushColorControls()
            }
            pushHostStateSync()
        }
    }

    func stopStream() {
        pendingPowerOnGeneration &+= 1
        isAwaitingAsyncPowerOn = false
        awaitingUsbReplug = false
        resumeAfterWake = false
        clearReplayStateOnly()
        if isRecording {
            stopHostRecording()
        }
        if isFrozen {
            isFrozen = false
        }
        guard let eng = engine else { return }
        kv_engine_stop(eng)
        streamEncoder.teardown()
        isRunning = false
        previewImage = nil
        clearSkeletonOverlayState()
        refreshStats()
        sensorInUseHolder = nil
        sensorPowerError = nil
        errorBanner = streamingSetupError
        sensorPowerState = .off
        pushHostStateSync()
    }

    func handleRemoteControlCommand(_ cmd: RemoteControlCommandMessage) {
        receivedRemoteControlCommandsForTesting.append(cmd)
        switch cmd.action {
        case .selectMode:
            if let modeRaw = cmd.modeRaw, let m = ViewerModeOption(rawValue: modeRaw) {
                selectedMode = m
            }
            if let sbsRaw = cmd.sbsLayoutRaw, let s = SbsLayoutOption(rawValue: sbsRaw) {
                selectedSbsLayout = s
            }
            pushHostStateSync()

        case .setCutoff:
            if cmd.applyTrackMePreset == true {
                applyTrackMeCutoffPreset()
            } else if let en = cmd.cutoffEnabled {
                cutoffEnabled = en
            }
            pushHostStateSync()

        case .setHostFrozen:
            if let targetFrozen = cmd.hostFrozen, isFrozen != targetFrozen {
                toggleFreeze()
            } else {
                pushHostStateSync()
            }

        case .startRecording:
            guard !isRecording else {
                pushHostStateSync()
                return
            }
            guard canRecordCurrentMode && isRunning && !isReplayingRecording else {
                errorBanner = !canRecordCurrentMode
                    ? HostRecorderError.colourLiveOnlyTooltip
                    : "Start the Kinect or Synthetic stream before recording."
                pushHostStateSync()
                return
            }
            if isRecordingReminderNeeded {
                if cmd.reminderConfirmedOnPhone == true {
                    if cmd.snoozeReminderOneHour == true {
                        recordingReminderSuppressedUntil = nowProvider().addingTimeInterval(PreRecordingReminderPolicy.snoozeDurationSeconds)
                    }
                    showRecordingReminderDialog = false
                    startHostRecording()
                } else {
                    recordingReminderDontAskOneHour = false
                    showRecordingReminderDialog = true
                }
            } else {
                startHostRecording()
            }
            pushHostStateSync()

        case .stopRecording:
            if isRecording {
                stopHostRecording()
            }
            pushHostStateSync()

        case .requestKeyframe:
            streamEncoder.requestImmediateKeyframeAndConfig()
            if isFrozen,
               streamServer?.pairedClientConnected == true,
               frozenWidth > 0,
               frozenHeight > 0,
               frozenBgraData.count >= frozenWidth * frozenHeight * 4 {
                pointCloudEncodeSeq &+= 1
                let seq = pointCloudEncodeSeq
                let capNs = kv_monotonic_ns()
                let modeRaw = frozenMode.rawValue
                let sbsRaw = frozenSbsLayout.rawValue
                frozenBgraData.withUnsafeBytes { rawPtr in
                    if let base = rawPtr.bindMemory(to: UInt8.self).baseAddress {
                        streamEncoder.encodeBGRAFrame(
                            bgraPtr: base,
                            srcWidth: frozenWidth,
                            srcHeight: frozenHeight,
                            srcStride: frozenWidth * 4,
                            frameSeq: seq,
                            hostCaptureNs: capNs,
                            modeRaw: modeRaw,
                            sbsRaw: sbsRaw,
                            hasClockOverlay: false
                        )
                    }
                }
            }
        }
    }

    func handleRemoteOrbit3DGesture(_ orbit: Orbit3DGestureMessage) {
        guard selectedMode == .pointCloud3D else { return }
        var cam = pointCloudCamera
        if orbit.resetView {
            cam.reset()
        } else {
            if orbit.deltaYawRadians != 0 || orbit.deltaPitchRadians != 0 {
                cam.orbit(deltaYaw: orbit.deltaYawRadians, deltaPitch: orbit.deltaPitchRadians)
            }
            if orbit.zoomScale > 0.01 && abs(orbit.zoomScale - 1.0) > 0.0001 {
                // Wire zoomScale is multiplicative camera distance (< 1.0 = closer, > 1.0 = farther)
                cam.zoom(scaleFactor: 1.0 / orbit.zoomScale)
            }
            if orbit.panRightMeters != 0 || orbit.panUpMeters != 0 {
                cam.pan(deltaRightMeters: orbit.panRightMeters, deltaUpMeters: orbit.panUpMeters)
            }
        }
        pointCloudCamera = cam
    }

    func toggleSource(synthetic: Bool) {
        guard useSyntheticSource != synthetic else { return }
        useSyntheticSource = synthetic
        if !synthetic {
            syntheticClockOverlay = false
        }
        startStream()
    }

    func applyDepthPreset(minMm: Double, maxMm: Double) {
        depthMinMm = minMm
        depthMaxMm = maxMm
    }

    func applyTrackMeCutoffPreset() {
        isApplyingPresetOrInit = true
        cutoffNearMm = 500.0
        cutoffFarMm = 2000.0
        cutoffEnabled = true
        isApplyingPresetOrInit = false
        pushDisplayParams()
        persistLastUsedSettingsIfReady()
    }

    func orbit3DCamera(deltaYaw: Float, deltaPitch: Float) {
        pointCloudCamera.orbit(deltaYaw: deltaYaw, deltaPitch: deltaPitch)
    }

    func zoom3DCamera(scaleFactor: Float) {
        pointCloudCamera.zoom(scaleFactor: scaleFactor)
    }

    func pan3DCamera(deltaRightMeters: Float, deltaUpMeters: Float) {
        pointCloudCamera.pan(deltaRightMeters: deltaRightMeters, deltaUpMeters: deltaUpMeters)
    }

    func reset3DCamera() {
        pointCloudCamera.reset()
    }

    private func handlePointCloudColorSourceChange() {
        guard !isApplyingPresetOrInit, let eng = engine else {
            pushDisplayParams()
            return
        }
        if isReplayingRecording && pointCloudColorSource.needsLiveColor {
            pointCloudColorSource = .depth
            return
        }
        if isRunning && !isReplayingRecording && currentModeNeedsColor && !kv_engine_is_rgb_active(eng) {
            startStream(forceEnableRgb: true)
            return
        }
        pushDisplayParams()
        if selectedMode == .pointCloud3D {
            _ = consumeLatestPointCloudFromEngine()
        }
    }

    private func handleDepthColorBlendChange() {
        if depthColorBlendEnabled && isRecording {
            stopHostRecording()
        }
        if depthColorBlendEnabled && isReplayingRecording {
            depthColorBlendEnabled = false
            return
        }
        clearInspectionOverlays()
        guard !isApplyingPresetOrInit, let eng = engine else {
            pushDisplayParams()
            return
        }
        if isRunning && !isReplayingRecording && currentModeNeedsColor && !kv_engine_is_rgb_active(eng) {
            startStream(forceEnableRgb: true)
            return
        }
        pushDisplayParams()
        reprojectActiveSkeletonOverlay()
        streamEncoder.requestImmediateKeyframeAndConfig()
        pushHostStateSync()
    }

    // MARK: - Phase 4 Vision Skeleton Overlay & Second-Display Output

    private func handleSkeletonOverlayToggleChange() {
        if !skeletonOverlayEnabled || isReplayingRecording {
            if isReplayingRecording && skeletonOverlayEnabled {
                skeletonOverlayEnabled = false
                return
            }
            clearSkeletonOverlayState()
            if selectedMode == .pointCloud3D {
                rerenderCachedPointCloudIfNeeded(userCameraInteraction: true)
            }
            return
        }
        guard !isApplyingPresetOrInit, let eng = engine else { return }
        if isRunning && !isReplayingRecording && !kv_engine_is_rgb_active(eng) {
            startStream(forceEnableRgb: true)
        }
    }

    private func clearSkeletonOverlayState() {
        latestVisionSnapshot = RawVisionPoseSnapshot()
        activeSkeletonOverlay = ProjectedSkeletonOverlay()
        detectedBodiesCount = 0
        detectedHandsCount = 0
    }

    private func pollAndRunVisionSkeletonIfNeeded() {
        guard skeletonOverlayEnabled,
              !isReplayingRecording,
              !isFrozen,
              !visionEstimator.isBusy,
              let eng = engine else {
            return
        }

        let requiredBytes = 1920 * 1080 * 4
        if visionColorBuffer.count < requiredBytes {
            visionColorBuffer.count = requiredBytes
        }

        var colorSeq: UInt64 = 0
        var cw: UInt32 = 0
        var ch: UInt32 = 0
        let copied = visionColorBuffer.withUnsafeMutableBytes { rawPtr -> Bool in
            guard let base = rawPtr.bindMemory(to: UInt8.self).baseAddress else { return false }
            return kv_engine_copy_latest_color_bgra(
                eng,
                lastVisionColorSeq,
                base,
                rawPtr.count,
                &colorSeq,
                &cw,
                &ch
            )
        }
        guard copied && colorSeq > lastVisionColorSeq && cw > 0 && ch > 0 else { return }
        lastVisionColorSeq = colorSeq

        let frameCopy = Data(visionColorBuffer.prefix(Int(cw * ch * 4)))
        let width = Int(cw)
        let height = Int(ch)
        let submittedGen = pendingPowerOnGeneration
        visionEstimator.submitLiveColorFrameIfIdle(
            bgraData: frameCopy,
            width: width,
            height: height,
            colorSeq: colorSeq
        ) { [weak self] snapshot in
            Task { @MainActor in
                guard let self = self else { return }
                self.applyVisionSnapshot(snapshot, expectedPowerGeneration: submittedGen)
            }
        }
    }

    private func applyVisionSnapshot(_ snapshot: RawVisionPoseSnapshot, expectedPowerGeneration: UInt64? = nil) {
        // Ignore late Vision results that arrive after the sensor was turned Off or restarted (Nit 4).
        guard isRunning,
              sensorPowerState == .on,
              !isReplayingRecording,
              skeletonOverlayEnabled else {
            return
        }
        if let expectedGen = expectedPowerGeneration, pendingPowerOnGeneration != expectedGen {
            return
        }
        latestVisionSnapshot = snapshot
        detectedBodiesCount = snapshot.bodyCount
        detectedHandsCount = snapshot.handCount
        reprojectActiveSkeletonOverlay()
        if selectedMode == .pointCloud3D {
            rerenderCachedPointCloudIfNeeded(userCameraInteraction: false)
        }
    }

    /// Deterministic testing hook to inject a synthetic normalized pose snapshot and run the live registration projector.
    func injectVisionPoseSnapshotForTesting(_ snapshot: RawVisionPoseSnapshot, expectedPowerGeneration: UInt64? = nil) {
        skeletonOverlayEnabled = true
        applyVisionSnapshot(snapshot, expectedPowerGeneration: expectedPowerGeneration)
    }

    func reprojectActiveSkeletonOverlay() {
        guard skeletonOverlayEnabled, !latestVisionSnapshot.isEmpty, let eng = engine else {
            if activeSkeletonOverlay.has2DElements || activeSkeletonOverlay.has3DElements {
                activeSkeletonOverlay = ProjectedSkeletonOverlay()
            }
            return
        }

        let projected = SkeletonOverlayProjector.project(
            snapshot: latestVisionSnapshot,
            confidenceThreshold: Float(skeletonConfidenceThreshold),
            mode: selectedMode,
            sbsLayout: selectedSbsLayout,
            depthColorBlendEnabled: depthColorBlendEnabled,
            mirrorHorizontal: mirrorHorizontal,
            rotate180: rotate180
        ) { normX, normY in
            var dx: Float = 0
            var dy: Float = 0
            var xm: Float = 0
            var ym: Float = 0
            var zm: Float = 0
            guard kv_engine_map_color_norm_to_depth_and_3d(eng, normX, normY, &dx, &dy, &xm, &ym, &zm) else {
                return nil
            }
            return MappedJointDepth3D(depthPixelX: dx, depthPixelY: dy, xMeters: xm, yMeters: ym, zMeters: zm)
        }
        activeSkeletonOverlay = projected
    }

    func refreshAvailableDisplays() {
        let screens = SecondDisplayOutputController.listAvailableDisplays()
        availableDisplays = screens
        if let cur = selectedSecondDisplayID, screens.contains(where: { $0.displayID == cur }) {
            return
        }
        // Prefer an external display/projector first if connected; otherwise default to the first display
        selectedSecondDisplayID = screens.first(where: { !$0.isMain })?.displayID ?? screens.first?.displayID
    }

    @discardableResult
    func startSecondDisplayOutput() -> Bool {
        refreshAvailableDisplays()
        guard let targetID = selectedSecondDisplayID else {
            errorBanner = "No display available for full-screen output."
            return false
        }
        let ok = secondDisplayController.startOutput(
            on: targetID,
            initialImage: previewImage,
            crispNearest: crispPixelScaling && selectedMode != .color && selectedMode != .pointCloud3D,
            skeletonOverlay: selectedMode == .pointCloud3D ? ProjectedSkeletonOverlay() : activeSkeletonOverlay,
            blackedOut: isSecondDisplayBlackedOut
        )
        isSecondDisplayActive = ok
        return ok
    }

    func stopSecondDisplayOutput() {
        secondDisplayController.stopOutput()
        isSecondDisplayActive = false
    }

    func toggleSecondDisplayBlackout() {
        isSecondDisplayBlackedOut.toggle()
    }

    private func syncSecondDisplayFrame() {
        guard secondDisplayController.isOutputActive else { return }
        secondDisplayController.updateFrame(
            image: previewImage,
            crispNearest: crispPixelScaling && selectedMode != .color && selectedMode != .pointCloud3D,
            skeletonOverlay: selectedMode == .pointCloud3D ? ProjectedSkeletonOverlay() : activeSkeletonOverlay
        )
    }

    func resetIrMappingDefaults() {
        irAutoScale = true
        irGamma = 0.45
        irSmoothingAlpha = 0.12
        irLowPercentile = 0.01
        irHighPercentile = 0.995
        irManualMin = 0.0
        irManualMax = 20000.0
    }

    private func handleModeOrLayoutChange() {
        helloSnapshotBox.update(modeRaw: selectedMode.rawValue, sbsRaw: selectedSbsLayout.rawValue)
        if isFrozen {
            isFrozen = false
        }
        clearInspectionOverlays()
        if isRecording {
            stopHostRecording()
        }
        guard !isApplyingPresetOrInit, let eng = engine else {
            pushDisplayParams()
            return
        }
        if isRunning && !isReplayingRecording && currentModeNeedsColor && !kv_engine_is_rgb_active(eng) {
            startStream(forceEnableRgb: true)
            return
        }
        pushDisplayParams()
        reprojectActiveSkeletonOverlay()
        streamEncoder.requestImmediateKeyframeAndConfig()
        if let server = streamServer {
            server.notifyModeChanged(HelloServerMessage(
                protocolVersion: 2,
                modeRaw: selectedMode.rawValue,
                sbsLayoutRaw: selectedSbsLayout.rawValue,
                allowPhoneModeSwitch: allowPhoneModeSwitch,
                width: lastFrameWidth,
                height: lastFrameHeight,
                codecRaw: WireCodecKind.h264.rawValue
            ))
        }
        pushHostStateSync()
    }

    private func buildDisplayParams() -> kv_display_params_t {
        var p = kv_display_params_t()
        kv_default_display_params(&p)
        p.mode = selectedMode.cValue
        p.sbs_layout = selectedSbsLayout.cValue
        p.ir_auto_scale = irAutoScale
        p.ir_gamma = Float(irGamma)
        p.ir_ema_alpha = Float(irSmoothingAlpha)
        p.ir_low_percentile = Float(irLowPercentile)
        p.ir_high_percentile = Float(irHighPercentile)
        p.ir_manual_min = Float(irManualMin)
        p.ir_manual_max = Float(irManualMax)
        p.depth_colormap = depthColormap.cValue
        p.depth_min_mm = Float(depthMinMm)
        p.depth_max_mm = Float(depthMaxMm)
        p.depth_invert = depthInvertNearWarm
        p.clock_overlay = useSyntheticSource && syntheticClockOverlay && !isReplayingRecording
        p.mirror_horizontal = mirrorHorizontal
        p.rotate_180 = rotate180
        p.cutoff_enabled = cutoffEnabled
        p.cutoff_near_mm = Float(cutoffNearMm)
        p.cutoff_far_mm = Float(cutoffFarMm)
        p.depth_color_blend_enabled = depthColorBlendEnabled && !isReplayingRecording
        p.depth_color_blend = Float(min(1.0, max(0.0, depthColorBlendPercent / 100.0)))
        p.point_color_source = pointCloudColorSource.cValue
        return p
    }

    private func pushDisplayParams() {
        guard let eng = engine else { return }
        var p = buildDisplayParams()
        kv_engine_set_display_params(eng, &p)
        if isReplayingRecording || selectedMode == .pointCloud3D {
            _ = consumeLatestMappedFrameFromEngine()
        }
    }

    func pushColorControls() {
        guard let eng = engine else { return }
        var c = kv_color_controls_t()
        c.mode = exposureMode.cValue
        c.auto_exposure_compensation = Float(autoExposureComp)
        c.semi_auto_pseudo_ms = Float(semiAutoPseudoMs)
        c.manual_integration_ms = Float(manualIntegrationMs)
        c.manual_analog_gain = Float(manualAnalogGain)
        var errBuf = [CChar](repeating: 0, count: 256)
        _ = kv_engine_set_color_controls(eng, &c, &errBuf, errBuf.count)
    }

    private func startPollTimer() {
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.tick()
            }
        }
    }

    @discardableResult
    private func consumeLatestMappedFrameFromEngine() -> Bool {
        if selectedMode == .pointCloud3D {
            return consumeLatestPointCloudFromEngine()
        }
        guard let eng = engine else { return false }
        var required: Int = 0
        var meta = kv_mapped_frame_t()

        if frameBuffer.count < 512 * 424 * 4 {
            frameBuffer.count = 512 * 424 * 4
        }

        var copied = frameBuffer.withUnsafeMutableBytes { rawPtr -> Bool in
            guard let base = rawPtr.bindMemory(to: UInt8.self).baseAddress else { return false }
            return kv_engine_copy_latest_frame(eng, lastSeenSeq, base, rawPtr.count, &required, &meta)
        }

        if !copied && required > frameBuffer.count {
            frameBuffer.count = required
            copied = frameBuffer.withUnsafeMutableBytes { rawPtr -> Bool in
                guard let base = rawPtr.bindMemory(to: UInt8.self).baseAddress else { return false }
                return kv_engine_copy_latest_frame(eng, lastSeenSeq, base, rawPtr.count, &required, &meta)
            }
        }

        guard copied && meta.frame_seq > lastSeenSeq && meta.width > 0 && meta.height > 0 else {
            return false
        }

        lastSeenSeq = meta.frame_seq
        lastFrameWidth = Int(meta.width)
        lastFrameHeight = Int(meta.height)
        helloSnapshotBox.update(
            modeRaw: selectedMode.rawValue,
            sbsRaw: selectedSbsLayout.rawValue,
            width: lastFrameWidth,
            height: lastFrameHeight
        )

        pullLatestInspectionBuffersFromEngine()

        if !isFrozen {
            let byteLen = Int(meta.stride_bytes) * Int(meta.height)
            let rawSlice = Data(frameBuffer.prefix(byteLen))
            if let cgImg = makeCGImage(fromBGRA: rawSlice, width: Int(meta.width), height: Int(meta.height), stride: Int(meta.stride_bytes)) {
                previewImage = cgImg
                frozenBgraData = rawSlice
                let nowNs = kv_monotonic_ns()
                if nowNs >= meta.arrival_mach_ns {
                    arrivalToDisplayMs = Double(nowNs - meta.arrival_mach_ns) / 1e6
                }
                frameDimensions = "\(meta.width) × \(meta.height)"
            }
            refreshActiveInspectionReadouts()
        }

        // Zero encoding overhead unless the single paired iPhone is actively connected (and host view isn't frozen)!
        if !isFrozen && streamServer?.pairedClientConnected == true {
            let modeRaw = selectedMode.rawValue
            let sbsRaw = selectedSbsLayout.rawValue
            let hasOverlay = useSyntheticSource && syntheticClockOverlay && !isReplayingRecording
            frameBuffer.withUnsafeBytes { rawPtr in
                if let base = rawPtr.bindMemory(to: UInt8.self).baseAddress {
                    streamEncoder.encodeBGRAFrame(
                        bgraPtr: base,
                        srcWidth: Int(meta.width),
                        srcHeight: Int(meta.height),
                        srcStride: Int(meta.stride_bytes),
                        frameSeq: meta.frame_seq,
                        hostCaptureNs: meta.arrival_mach_ns,
                        modeRaw: modeRaw,
                        sbsRaw: sbsRaw,
                        hasClockOverlay: hasOverlay
                    )
                }
            }
        }
        return true
    }

    @discardableResult
    private func consumeLatestPointCloudFromEngine() -> Bool {
        guard let eng = engine else { return false }
        var pcSeq: UInt64 = 0
        var pcArrivalNs: UInt64 = 0
        var pcIntr = kv_ir_intrinsics_t()

        let copied = cachedUndistortedDepthMm.withUnsafeMutableBufferPointer { depthBuf -> Bool in
            cachedPointBgra.withUnsafeMutableBufferPointer { bgraBuf -> Bool in
                kv_engine_copy_point_cloud_inputs(
                    eng,
                    lastPointCloudSeq,
                    depthBuf.baseAddress,
                    depthBuf.count,
                    bgraBuf.baseAddress,
                    bgraBuf.count,
                    &pcSeq,
                    &pcArrivalNs,
                    &pcIntr
                )
            }
        }
        guard copied && pcSeq > lastPointCloudSeq else { return false }

        lastPointCloudSeq = pcSeq
        cachedPointIntrinsics = pcIntr
        hasCachedPointCloudFrame = true
        pullLatestInspectionBuffersFromEngine()

        if !isFrozen {
            renderCurrentPointCloudToPreviewAndStream(arrivalNs: pcArrivalNs, forceWhenFrozen: false)
        }
        return true
    }

    private func rerenderCachedPointCloudIfNeeded(userCameraInteraction: Bool) {
        guard selectedMode == .pointCloud3D, hasCachedPointCloudFrame else { return }
        renderCurrentPointCloudToPreviewAndStream(
            arrivalNs: kv_monotonic_ns(),
            forceWhenFrozen: userCameraInteraction
        )
    }

    private func renderCurrentPointCloudToPreviewAndStream(arrivalNs: UInt64, forceWhenFrozen: Bool) {
        guard hasCachedPointCloudFrame else { return }
        if isFrozen && !forceWhenFrozen {
            return
        }

        let rw = MetalPointCloudRenderer.defaultRenderWidth
        let rh = MetalPointCloudRenderer.defaultRenderHeight
        let overlayLines = skeletonOverlayEnabled ? activeSkeletonOverlay.lineVertices3D : []
        let overlayPoints = skeletonOverlayEnabled ? activeSkeletonOverlay.pointVertices3D : []
        let rendered = cachedUndistortedDepthMm.withUnsafeBufferPointer { depthBuf -> Bool in
            cachedPointBgra.withUnsafeBufferPointer { bgraBuf -> Bool in
                pointCloudRenderer.render(
                    undistortedDepthMm: depthBuf,
                    pointBgra: bgraBuf,
                    intrinsics: cachedPointIntrinsics,
                    camera: pointCloudCamera,
                    pointSize: Float(pointCloudSize),
                    mirrorHorizontal: mirrorHorizontal,
                    rotate180: rotate180,
                    overlayLineVertices: overlayLines,
                    overlayPointVertices: overlayPoints,
                    renderWidth: rw,
                    renderHeight: rh,
                    outBgraData: &pointCloudRenderBuffer
                )
            }
        }
        guard rendered else { return }

        lastFrameWidth = rw
        lastFrameHeight = rh
        helloSnapshotBox.update(
            modeRaw: selectedMode.rawValue,
            sbsRaw: selectedSbsLayout.rawValue,
            width: rw,
            height: rh
        )

        if let cgImg = makeCGImage(fromBGRA: pointCloudRenderBuffer, width: rw, height: rh, stride: rw * 4) {
            previewImage = cgImg
            frozenBgraData = pointCloudRenderBuffer
            frozenWidth = rw
            frozenHeight = rh
            let nowNs = kv_monotonic_ns()
            if arrivalNs > 0 && nowNs >= arrivalNs {
                arrivalToDisplayMs = Double(nowNs - arrivalNs) / 1e6
            }
            frameDimensions = "\(rw) × \(rh) (Metal 3D)"
        }

        // Phase 4.1: No joint data leaves the Mac — if streaming to iPhone while 3D skeleton is active,
        // encode a clean point-cloud render without any Vision skeleton overlay!
        if streamServer?.pairedClientConnected == true {
            pointCloudEncodeSeq &+= 1
            let encodeSeq = pointCloudEncodeSeq
            let captureNs = kv_monotonic_ns()
            let modeRaw = selectedMode.rawValue
            let sbsRaw = selectedSbsLayout.rawValue
            let has3DSkeleton = !overlayLines.isEmpty || !overlayPoints.isEmpty
            if has3DSkeleton {
                _ = cachedUndistortedDepthMm.withUnsafeBufferPointer { depthBuf in
                    cachedPointBgra.withUnsafeBufferPointer { bgraBuf in
                        pointCloudRenderer.render(
                            undistortedDepthMm: depthBuf,
                            pointBgra: bgraBuf,
                            intrinsics: cachedPointIntrinsics,
                            camera: pointCloudCamera,
                            pointSize: Float(pointCloudSize),
                            mirrorHorizontal: mirrorHorizontal,
                            rotate180: rotate180,
                            renderWidth: rw,
                            renderHeight: rh,
                            outBgraData: &pointCloudStreamCleanBuffer
                        )
                    }
                }
            }
            let streamSource = has3DSkeleton ? pointCloudStreamCleanBuffer : pointCloudRenderBuffer
            streamSource.withUnsafeBytes { rawPtr in
                if let base = rawPtr.bindMemory(to: UInt8.self).baseAddress {
                    streamEncoder.encodeBGRAFrame(
                        bgraPtr: base,
                        srcWidth: rw,
                        srcHeight: rh,
                        srcStride: rw * 4,
                        frameSeq: encodeSeq,
                        hostCaptureNs: captureNs,
                        modeRaw: modeRaw,
                        sbsRaw: sbsRaw,
                        hasClockOverlay: false
                    )
                }
            }
        }
    }

    private func tick() {
        guard let eng = engine else { return }

        if isRunning {
            if isReplayingRecording {
                if isReplayPlaying && !isFrozen, let reader = replayReader, replayTotalFrames > 0 {
                    let nowNs = kv_monotonic_ns()
                    if nowNs >= nextReplayStepNs && !isReplayDecodeInFlight {
                        let nextIdx = (replayFrameIndex + 1) % replayTotalFrames
                        let dtNs = UInt64(reader.frameIntervalSeconds(from: nextIdx) * 1e9)
                        nextReplayStepNs = nowNs &+ dtNs
                        scheduleReplayDecodeOffMainActor(frameIndex: nextIdx, prefetchNextChunk: true)
                    }
                }
                _ = consumeLatestMappedFrameFromEngine()
            } else {
                _ = consumeLatestMappedFrameFromEngine()
                pollAndRunVisionSkeletonIfNeeded()

                // Feed 16-bit lossless recorder if recording IR or Depth on host
                if isRecording && (selectedMode == .ir || selectedMode == .depth) {
                    let wantDepth = (selectedMode == .depth)
                    var rawSeq: UInt64 = 0
                    var rawArrNs: UInt64 = 0
                    var rawTicks: UInt32 = 0
                    var rawW: UInt32 = 0
                    var rawH: UInt32 = 0
                    let rawCopied = raw16Scratch.withUnsafeMutableBufferPointer { buf -> Bool in
                        guard let base = buf.baseAddress else { return false }
                        return kv_engine_copy_latest_raw16(
                            eng,
                            wantDepth,
                            lastRecordedRaw16Seq,
                            base,
                            buf.count,
                            &rawSeq,
                            &rawArrNs,
                            &rawTicks,
                            &rawW,
                            &rawH
                        )
                    }
                    if rawCopied && rawSeq > lastRecordedRaw16Seq && rawW > 0 && rawH > 0 {
                        lastRecordedRaw16Seq = rawSeq
                        let count = Int(rawW) * Int(rawH)
                        let activeSource: kv_source_kind_t = useSyntheticSource ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL
                        recorder.appendRaw16Frame(
                            pixelsU16: Array(raw16Scratch.prefix(count)),
                            width: Int(rawW),
                            height: Int(rawH),
                            seq: rawSeq,
                            deviceTicks: rawTicks,
                            arrivalNs: rawArrNs,
                            source: activeSource
                        )
                    }
                }
            }
        }

        statsTick += 1
        if statsTick >= 6 {
            statsTick = 0
            refreshStats()
            syncStreamingStatus()
            syncRecordingStatus()
        }
    }

    private func refreshStats() {
        guard let eng = engine else { return }
        var st = kv_stats_snapshot_t()
        kv_engine_get_stats(eng, &st)

        isRunning = st.running
        usbPresent = st.device_present
        lockBlocked = st.lock_blocked
        currentFps = st.current_fps
        colorFps = st.color_fps
        irFrameCount = st.ir_frames
        depthFrameCount = st.depth_frames
        colorFrameCount = st.color_frames
        renderedFrameCount = st.rendered_frames
        totalDropCount = st.ir_seq_drops + st.color_seq_drops
        interFrameMeanMs = st.inter_frame_mean_ms
        interFrameP95Ms = st.inter_frame_p95_ms
        mapTimeMeanMs = st.map_time_mean_ms
        smoothedLow = st.smoothed_scale_low
        smoothedHigh = st.smoothed_scale_high
        rawMin = st.raw_min
        rawMax = st.raw_max
        rawMean = st.raw_mean
        saturatedPixels = st.saturated_pixels
        invalidPixels = st.invalid_pixels
        colorExposureFooter = st.color_exposure
        colorGainFooter = st.color_gain
        readbackIntegrationMs = st.readback_integration_ms
        readbackAnalogGain = st.readback_analog_gain

        let s = withUnsafeBytes(of: &st.serial) { rawBuf -> String in
            let prefix = rawBuf.prefix { $0 != 0 }
            return String(decoding: prefix, as: UTF8.self)
        }
        serialNumber = s.isEmpty ? "—" : s

        statusText = withUnsafeBytes(of: &st.status_message) { rawBuf -> String in
            let prefix = rawBuf.prefix { $0 != 0 }
            return String(decoding: prefix, as: UTF8.self)
        }

        var hs = kv_health_snapshot_t()
        kv_engine_get_health(eng, &hs)
        let drvErr = withUnsafeBytes(of: &hs.last_driver_error) { rawBuf -> String in
            String(decoding: rawBuf.prefix { $0 != 0 }, as: UTF8.self)
        }
        let usbLoc = withUnsafeBytes(of: &hs.usb_location) { rawBuf -> String in
            String(decoding: rawBuf.prefix { $0 != 0 }, as: UTF8.self)
        }
        healthSnapshot = HealthMetricsSnapshot(
            irFps: hs.ir_fps,
            depthFps: hs.depth_fps,
            colorFps: hs.color_fps,
            irDropsPerMin: hs.ir_drops_per_min,
            depthDropsPerMin: hs.depth_drops_per_min,
            colorDropsPerMin: hs.color_drops_per_min,
            irDropRatio10s: hs.ir_drop_ratio_10s,
            depthDropRatio10s: hs.depth_drop_ratio_10s,
            dropWarningActive: hs.drop_warning_active || DropBannerEvaluator.shouldShowBanner(
                irDropRatio10s: hs.ir_drop_ratio_10s,
                depthDropRatio10s: hs.depth_drop_ratio_10s
            ),
            interFrameP50Ms: hs.inter_frame_p50_ms,
            interFrameP95Ms: hs.inter_frame_p95_ms,
            depthProcessingMs: hs.depth_processing_ms,
            driverErrors: hs.driver_errors,
            lastDriverError: drvErr,
            usbLocation: usbLoc.isEmpty ? "USB Disconnected" : usbLoc
        )

        // Belt-and-suspenders USB unplug/replug state sync
        if st.running && !useSyntheticSource && !st.device_present {
            handleUsbUnplugged()
        } else if awaitingUsbReplug && !useSyntheticSource {
            if st.device_present && !st.lock_blocked {
                awaitingUsbReplug = false
                startStream()
            } else {
                statusText = "Kinect USB unplugged — waiting for replug…"
            }
        }
    }

    // MARK: - Phase 1 Inspection, Freeze, Zoom/Pan, Presets & Shortcuts

    private func makeCGImage(fromBGRA data: Data, width: Int, height: Int, stride: Int) -> CGImage? {
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        let bitmapInfo = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue
        let useNearest = crispPixelScaling && width <= 1280
        return CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: stride,
            space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue: bitmapInfo),
            provider: provider,
            decode: nil,
            shouldInterpolate: !useNearest,
            intent: .defaultIntent
        )
    }

    private func pullLatestInspectionBuffersFromEngine() {
        guard let eng = engine else { return }
        var w: UInt32 = 0
        var h: UInt32 = 0
        var intr = kv_ir_intrinsics_t()
        let ok = liveDepthMm.withUnsafeMutableBufferPointer { depthPtr -> Bool in
            liveIrRaw.withUnsafeMutableBufferPointer { irPtr -> Bool in
                kv_engine_copy_inspection_buffers(
                    eng,
                    depthPtr.baseAddress,
                    depthPtr.count,
                    irPtr.baseAddress,
                    irPtr.count,
                    &w,
                    &h,
                    &intr
                )
            }
        }
        if ok {
            liveIntrinsics = intr
        }
    }

    /// Injects synthetic inspection buffers directly for deterministic unit testing without a sensor.
    func loadSyntheticInspectionSnapshotForTesting(
        depthMm: [Float],
        irRaw: [Float],
        intrinsics: kv_ir_intrinsics_t,
        imageWidth: Int = 512,
        imageHeight: Int = 424
    ) {
        liveDepthMm = depthMm
        liveIrRaw = irRaw
        liveIntrinsics = intrinsics
        lastFrameWidth = imageWidth
        lastFrameHeight = imageHeight
        var bgra = Data(count: imageWidth * imageHeight * 4)
        bgra.withUnsafeMutableBytes { rawPtr in
            guard let ptr = rawPtr.bindMemory(to: UInt8.self).baseAddress else { return }
            for i in 0..<(imageWidth * imageHeight) {
                ptr[i * 4 + 3] = 255
            }
        }
        frozenBgraData = bgra
        previewImage = makeCGImage(fromBGRA: bgra, width: imageWidth, height: imageHeight, stride: imageWidth * 4)
    }

    private var activeInspectionWidth: Int { isFrozen ? frozenWidth : lastFrameWidth }
    private var activeInspectionHeight: Int { isFrozen ? frozenHeight : lastFrameHeight }
    private var activeInspectionMode: ViewerModeOption { isFrozen ? frozenMode : selectedMode }
    private var activeInspectionSbsLayout: SbsLayoutOption { isFrozen ? frozenSbsLayout : selectedSbsLayout }
    private var activeDepthBuffer: [Float] { isFrozen ? frozenDepthMm : liveDepthMm }
    private var activeIrBuffer: [Float] { isFrozen ? frozenIrRaw : liveIrRaw }
    private var activeIntrinsics: kv_ir_intrinsics_t { isFrozen ? frozenIntrinsics : liveIntrinsics }

    func toggleFreeze() {
        if isFrozen {
            isFrozen = false
            streamEncoder.requestImmediateKeyframeAndConfig()
            refreshActiveInspectionReadouts()
        } else {
            guard previewImage != nil else { return }
            frozenDepthMm = liveDepthMm
            frozenIrRaw = liveIrRaw
            frozenIntrinsics = liveIntrinsics
            frozenWidth = lastFrameWidth
            frozenHeight = lastFrameHeight
            frozenMode = selectedMode
            frozenSbsLayout = selectedSbsLayout
            isFrozen = true
            refreshActiveInspectionReadouts()
        }
        pushHostStateSync()
    }

    private func handleOrientationChange(oldMirror: Bool, oldRotate180: Bool) {
        reprojectActiveSkeletonOverlay()
        if selectedMode == .pointCloud3D {
            pushDisplayParams()
            rerenderCachedPointCloudIfNeeded(userCameraInteraction: true)
            return
        }
        let oldFlipX = (oldMirror != oldRotate180)
        let oldFlipY = oldRotate180
        let newFlipX = (mirrorHorizontal != rotate180)
        let newFlipY = rotate180
        let toggleX = (oldFlipX != newFlipX)
        let toggleY = (oldFlipY != newFlipY)

        if isFrozen, frozenWidth > 0, frozenHeight > 0, frozenBgraData.count >= frozenWidth * frozenHeight * 4 {
            frozenBgraData.withUnsafeMutableBytes { rawPtr in
                let px = rawPtr.bindMemory(to: UInt32.self)
                guard px.count >= frozenWidth * frozenHeight else { return }
                let w = frozenWidth
                let h = frozenHeight
                if toggleX && toggleY {
                    let total = w * h
                    for i in 0..<(total / 2) {
                        px.swapAt(i, total - 1 - i)
                    }
                } else if toggleX {
                    for y in 0..<h {
                        let row = y * w
                        for x in 0..<(w / 2) {
                            px.swapAt(row + x, row + (w - 1 - x))
                        }
                    }
                } else if toggleY {
                    for y in 0..<(h / 2) {
                        let top = y * w
                        let bot = (h - 1 - y) * w
                        for x in 0..<w {
                            px.swapAt(top + x, bot + x)
                        }
                    }
                }
            }
            previewImage = makeCGImage(
                fromBGRA: frozenBgraData,
                width: frozenWidth,
                height: frozenHeight,
                stride: frozenWidth * 4
            )
        } else {
            pushDisplayParams()
        }
        clearInspectionOverlays()
        refreshActiveInspectionReadouts()
    }

    func zoomCanvasAround(cursorInView: CGPoint, viewSize: CGSize, factor: CGFloat) {
        let imgSize = CGSize(width: activeInspectionWidth, height: activeInspectionHeight)
        viewportTransform.zoomAround(
            cursorInView: cursorInView,
            viewSize: viewSize,
            imageSize: imgSize,
            factor: factor
        )
    }

    func panCanvasBy(delta: CGPoint, viewSize: CGSize) {
        let imgSize = CGSize(width: activeInspectionWidth, height: activeInspectionHeight)
        viewportTransform.panBy(delta: delta, viewSize: viewSize, imageSize: imgSize)
    }

    func resetViewport() {
        viewportTransform.reset()
    }

    func clearInspectionOverlays() {
        regionSelectionBox = nil
        regionStats = nil
        rulerState = nil
    }

    func updateCursorHover(viewPoint: CGPoint?, viewSize: CGSize) {
        lastHoverViewPoint = viewPoint
        lastHoverViewSize = viewSize
        guard previewImage != nil,
              let pt = viewPoint,
              let disp = viewportTransform.viewPointToDisplayPixel(
                  pt,
                  viewSize: viewSize,
                  imageWidth: activeInspectionWidth,
                  imageHeight: activeInspectionHeight
              ) else {
            cursorReadout = nil
            return
        }

        let mapped = ViewportTransform.mapDisplayPixelToSensor(
            dispX: disp.dispX,
            dispY: disp.dispY,
            imageWidth: activeInspectionWidth,
            imageHeight: activeInspectionHeight,
            mode: activeInspectionMode,
            sbsLayout: activeInspectionSbsLayout,
            mirrorHorizontal: mirrorHorizontal,
            rotate180: rotate180,
            depthColorBlendEnabled: depthColorBlendEnabled
        )

        if mapped.hasDepthIr {
            let idx = mapped.pixelY * 512 + mapped.pixelX
            let d = (idx >= 0 && idx < activeDepthBuffer.count) ? activeDepthBuffer[idx] : 0.0
            let ir = (idx >= 0 && idx < activeIrBuffer.count) ? activeIrBuffer[idx] : 0.0
            cursorReadout = CursorReadout(
                pixelX: mapped.pixelX,
                pixelY: mapped.pixelY,
                hasSensorDepthIr: true,
                depthMm: (d.isFinite && d > 0) ? d : nil,
                rawIr: ir.isFinite ? ir : nil
            )
        } else {
            cursorReadout = CursorReadout(
                pixelX: mapped.pixelX,
                pixelY: mapped.pixelY,
                hasSensorDepthIr: false,
                depthMm: nil,
                rawIr: nil
            )
        }
    }

    func updateRegionSelectionDrag(startViewPt: CGPoint, currentViewPt: CGPoint, viewSize: CGSize) {
        guard previewImage != nil, activeInspectionWidth > 0, activeInspectionHeight > 0 else { return }
        let rect = viewportTransform.displayedImageRect(
            viewSize: viewSize,
            imageSize: CGSize(width: activeInspectionWidth, height: activeInspectionHeight)
        )
        guard rect.width > 0, rect.height > 0 else { return }

        func clampToDisp(_ pt: CGPoint) -> (Int, Int) {
            let nx = min(0.999999, max(0.0, (pt.x - rect.minX) / rect.width))
            let ny = min(0.999999, max(0.0, (pt.y - rect.minY) / rect.height))
            return (Int(nx * CGFloat(activeInspectionWidth)), Int(ny * CGFloat(activeInspectionHeight)))
        }

        let (x0, y0) = clampToDisp(startViewPt)
        let (x1, y1) = clampToDisp(currentViewPt)
        let box = RegionSelectionBox(dispX0: x0, dispY0: y0, dispX1: x1, dispY1: y1)
        regionSelectionBox = box
        recomputeRegionStats(for: box)
    }

    private func recomputeRegionStats(for box: RegionSelectionBox) {
        let p0 = ViewportTransform.mapDisplayPixelToSensor(
            dispX: box.minDispX,
            dispY: box.minDispY,
            imageWidth: activeInspectionWidth,
            imageHeight: activeInspectionHeight,
            mode: activeInspectionMode,
            sbsLayout: activeInspectionSbsLayout,
            mirrorHorizontal: mirrorHorizontal,
            rotate180: rotate180,
            depthColorBlendEnabled: depthColorBlendEnabled
        )
        let p1 = ViewportTransform.mapDisplayPixelToSensor(
            dispX: box.maxDispX,
            dispY: box.maxDispY,
            imageWidth: activeInspectionWidth,
            imageHeight: activeInspectionHeight,
            mode: activeInspectionMode,
            sbsLayout: activeInspectionSbsLayout,
            mirrorHorizontal: mirrorHorizontal,
            rotate180: rotate180,
            depthColorBlendEnabled: depthColorBlendEnabled
        )
        guard p0.hasDepthIr && p1.hasDepthIr else {
            regionStats = nil
            return
        }
        regionStats = RegionDepthStats.compute(
            depthMm: activeDepthBuffer,
            width: 512,
            height: 424,
            x0: p0.pixelX,
            y0: p0.pixelY,
            x1: p1.pixelX,
            y1: p1.pixelY
        )
    }

    func handleCanvasSingleClick(viewPoint: CGPoint, viewSize: CGSize) {
        guard previewImage != nil,
              let disp = viewportTransform.viewPointToDisplayPixel(
                  viewPoint,
                  viewSize: viewSize,
                  imageWidth: activeInspectionWidth,
                  imageHeight: activeInspectionHeight
              ) else { return }

        let mapped = ViewportTransform.mapDisplayPixelToSensor(
            dispX: disp.dispX,
            dispY: disp.dispY,
            imageWidth: activeInspectionWidth,
            imageHeight: activeInspectionHeight,
            mode: activeInspectionMode,
            sbsLayout: activeInspectionSbsLayout,
            mirrorHorizontal: mirrorHorizontal,
            rotate180: rotate180,
            depthColorBlendEnabled: depthColorBlendEnabled
        )
        let endpoint = RulerEndpoint(
            dispX: disp.dispX,
            dispY: disp.dispY,
            sensorX: mapped.pixelX,
            sensorY: mapped.pixelY,
            hasSensorCoord: mapped.hasDepthIr
        )

        if let current = rulerState, current.pointB == nil {
            let eval = RulerMeasurementState.evaluateDistance(
                pointA: current.pointA,
                pointB: endpoint,
                intrinsics: activeIntrinsics,
                depthMm: activeDepthBuffer
            )
            rulerState = RulerMeasurementState(
                pointA: current.pointA,
                pointB: endpoint,
                distanceMm: eval.distanceMm,
                errorMessage: eval.errorMessage
            )
        } else {
            rulerState = RulerMeasurementState(
                pointA: endpoint,
                pointB: nil,
                distanceMm: nil,
                errorMessage: nil
            )
        }
    }

    private func refreshActiveInspectionReadouts() {
        if let pt = lastHoverViewPoint, lastHoverViewSize.width > 0 {
            updateCursorHover(viewPoint: pt, viewSize: lastHoverViewSize)
        }
        if let box = regionSelectionBox {
            recomputeRegionStats(for: box)
        }
        if let r = rulerState, let b = r.pointB {
            let eval = RulerMeasurementState.evaluateDistance(
                pointA: r.pointA,
                pointB: b,
                intrinsics: activeIntrinsics,
                depthMm: activeDepthBuffer
            )
            rulerState = RulerMeasurementState(
                pointA: r.pointA,
                pointB: b,
                distanceMm: eval.distanceMm,
                errorMessage: eval.errorMessage
            )
        }
    }

    // MARK: - Presets & Last-Used Settings Persistence

    func currentSettingsAsPreset(named name: String, isBuiltIn: Bool = false) -> ViewerPreset {
        ViewerPreset(
            name: name,
            isBuiltIn: isBuiltIn,
            modeRaw: selectedMode.rawValue,
            sbsLayoutRaw: selectedSbsLayout.rawValue,
            pipelineRaw: selectedPipeline.rawValue,
            irAutoScale: irAutoScale,
            irGamma: irGamma,
            irSmoothingAlpha: irSmoothingAlpha,
            irLowPercentile: irLowPercentile,
            irHighPercentile: irHighPercentile,
            irManualMin: irManualMin,
            irManualMax: irManualMax,
            depthColormapRaw: depthColormap.rawValue,
            depthMinMm: depthMinMm,
            depthMaxMm: depthMaxMm,
            depthInvertNearWarm: depthInvertNearWarm,
            exposureModeRaw: exposureMode.rawValue,
            autoExposureComp: autoExposureComp,
            semiAutoPseudoMs: semiAutoPseudoMs,
            manualIntegrationMs: manualIntegrationMs,
            manualAnalogGain: manualAnalogGain,
            mirrorHorizontal: mirrorHorizontal,
            rotate180: rotate180,
            cutoffEnabled: cutoffEnabled,
            cutoffNearMm: cutoffNearMm,
            cutoffFarMm: cutoffFarMm,
            pointCloudSize: pointCloudSize,
            pointCloudColorSourceRaw: pointCloudColorSource.rawValue
        )
    }

    func applyPreset(
        _ preset: ViewerPreset,
        persistAsLastUsed: Bool = true,
        restartPipelineIfNeeded: Bool = true
    ) {
        let prevPipeline = selectedPipeline
        isApplyingPresetOrInit = true
        if let m = ViewerModeOption(rawValue: preset.modeRaw) { selectedMode = m }
        if let sbs = SbsLayoutOption(rawValue: preset.sbsLayoutRaw) { selectedSbsLayout = sbs }
        if let pipe = PipelineOption(rawValue: preset.pipelineRaw) { selectedPipeline = pipe }

        irAutoScale = preset.irAutoScale
        irGamma = preset.irGamma
        irSmoothingAlpha = preset.irSmoothingAlpha
        irLowPercentile = preset.irLowPercentile
        irHighPercentile = preset.irHighPercentile
        irManualMin = preset.irManualMin
        irManualMax = preset.irManualMax

        if let cmap = ColormapOption(rawValue: preset.depthColormapRaw) { depthColormap = cmap }
        depthMinMm = preset.depthMinMm
        depthMaxMm = max(preset.depthMinMm + 100.0, preset.depthMaxMm)
        depthInvertNearWarm = preset.depthInvertNearWarm

        if let exp = ExposureModeOption(rawValue: preset.exposureModeRaw) { exposureMode = exp }
        autoExposureComp = preset.autoExposureComp
        semiAutoPseudoMs = preset.semiAutoPseudoMs
        manualIntegrationMs = preset.manualIntegrationMs
        manualAnalogGain = preset.manualAnalogGain

        mirrorHorizontal = preset.mirrorHorizontal
        rotate180 = preset.rotate180

        if let ce = preset.cutoffEnabled { cutoffEnabled = ce }
        if let cn = preset.cutoffNearMm { cutoffNearMm = cn }
        if let cf = preset.cutoffFarMm { cutoffFarMm = max(cutoffNearMm + 50.0, cf) }
        if let ps = preset.pointCloudSize { pointCloudSize = ps }
        if let pcsRaw = preset.pointCloudColorSourceRaw,
           let pcs = PointCloudColorSourceOption(rawValue: pcsRaw) {
            pointCloudColorSource = pcs
        }
        isApplyingPresetOrInit = false

        if preset.name != "_last_used" {
            activePresetName = preset.name
        }
        pushDisplayParams()
        pushColorControls()
        if persistAsLastUsed {
            presetsStore.saveLastUsedSettings(currentSettingsAsPreset(named: "_last_used"))
        }
        if restartPipelineIfNeeded && isRunning && !isReplayingRecording {
            if prevPipeline != selectedPipeline || (currentModeNeedsColor && engine.map({ !kv_engine_is_rgb_active($0) }) == true) {
                startStream(forceEnableRgb: currentModeNeedsColor)
            }
        }
    }

    @discardableResult
    func saveNamedPreset(_ name: String) -> Bool {
        let p = currentSettingsAsPreset(named: name, isBuiltIn: false)
        let ok = presetsStore.saveCustomPreset(p)
        if ok {
            presets = presetsStore.allPresets
            activePresetName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return ok
    }

    func deleteNamedPreset(_ name: String) {
        presetsStore.deleteCustomPreset(named: name)
        presets = presetsStore.allPresets
        if activePresetName == name {
            activePresetName = ""
        }
    }

    private func persistLastUsedSettingsIfReady() {
        guard !isApplyingPresetOrInit else { return }
        presetsStore.saveLastUsedSettings(currentSettingsAsPreset(named: "_last_used"))
    }

    // MARK: - Keyboard Shortcuts

    var isAnyModalSheetOpen: Bool {
        showRecordingReminderDialog || showPairingSheet || showResetPairingConfirmation
    }

    private func installKeyboardShortcutsMonitor() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self = self else { return event }
            // Pause global single-key monitor whenever any sheet is open (Nit 1)
            if self.isAnyModalSheetOpen || event.window?.attachedSheet != nil || event.window?.sheetParent != nil {
                return event
            }
            // Ignore Cmd/Ctrl shortcuts and ignore when typing into any text field
            if event.modifierFlags.contains(.command) || event.modifierFlags.contains(.control) {
                return event
            }
            if let responder = event.window?.firstResponder, responder is NSTextView || responder is NSText {
                return event
            }
            let chars = event.charactersIgnoringModifiers ?? ""
            if self.handleShortcutKey(chars, keyCode: event.keyCode, modifiers: event.modifierFlags) {
                return nil
            }
            return event
        }
    }

    @discardableResult
    func handleShortcutKey(_ characters: String, keyCode: UInt16, modifiers: NSEvent.ModifierFlags = []) -> Bool {
        if isAnyModalSheetOpen {
            return false
        }
        // Esc (keyCode 53)
        if keyCode == 53 || characters == "\u{1B}" {
            var handled = false
            if showShortcutsHelp {
                showShortcutsHelp = false
                handled = true
            }
            if regionSelectionBox != nil || rulerState != nil {
                clearInspectionOverlays()
                handled = true
            }
            if !handled && isSecondDisplayActive && selectedSecondDisplayID == CGMainDisplayID() {
                stopSecondDisplayOutput()
                handled = true
            }
            return handled
        }

        // Space (keyCode 49)
        if keyCode == 49 || characters == " " {
            toggleFreeze()
            return true
        }

        if characters == "?" || (characters == "/" && modifiers.contains(.shift)) {
            showShortcutsHelp.toggle()
            return true
        }

        switch characters.lowercased() {
        case "1":
            shortcutNoticeBanner = nil
            selectedMode = .ir
            return true
        case "2":
            shortcutNoticeBanner = nil
            selectedMode = .color
            return true
        case "3":
            shortcutNoticeBanner = nil
            selectedMode = .depth
            return true
        case "4":
            shortcutNoticeBanner = nil
            selectedMode = .sideBySide
            return true
        case "5":
            shortcutNoticeBanner = nil
            selectedMode = .pointCloud3D
            return true
        case "b":
            toggleSecondDisplayBlackout()
            return true
        case "h":
            showHealthOverlay.toggle()
            return true
        case "f":
            NSApp.keyWindow?.toggleFullScreen(nil)
            return true
        case "m":
            mirrorHorizontal.toggle()
            return true
        case "r":
            rotate180.toggle()
            return true
        default:
            return false
        }
    }

    private static func decodeCStringBuffer(_ buf: [CChar]) -> String {
        let bytes = buf.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
        return String(decoding: bytes, as: UTF8.self)
    }
}
