import AppKit
import AVFoundation
import SwiftUI

final class MacSampleBufferLayerContainerView: NSView {
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is unused")
    }

    override func makeBackingLayer() -> CALayer {
        let layer = AVSampleBufferDisplayLayer()
        layer.videoGravity = .resizeAspect
        layer.preventsCapture = true
        layer.backgroundColor = CGColor(gray: 0, alpha: 1)
        return layer
    }

    var sampleDisplayLayer: AVSampleBufferDisplayLayer {
        layer as! AVSampleBufferDisplayLayer
    }
}

struct MacSampleBufferVideoView: NSViewRepresentable {
    let bridge: SampleBufferDisplayBridge

    func makeNSView(context: Context) -> MacSampleBufferLayerContainerView {
        let view = MacSampleBufferLayerContainerView(frame: .zero)
        bridge.attach(layer: view.sampleDisplayLayer)
        return view
    }

    func updateNSView(_ nsView: MacSampleBufferLayerContainerView, context: Context) {
        nsView.sampleDisplayLayer.preventsCapture = true
    }
}

struct MacCameraViewerView: View {
    @ObservedObject var viewer: MacCameraViewerController
    var localHostStatusBanner: String? = nil
    var onSwitchToLocalHost: (() -> Void)? = nil

    var body: some View {
        HSplitView {
            // Left Live Video Viewport
            ZStack {
                Color.black

                if let keychainErr = viewer.keychainAccessError {
                    keychainAccessRequiredPrompt(message: keychainErr)
                } else if viewer.telemetry.isPaired {
                    MacSampleBufferVideoView(bridge: viewer.displayBridge)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                    centerOverlayCard

                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 10) {
                            statusPill
                            if viewer.showHealthOverlay {
                                healthPill
                            }
                            Spacer()
                        }
                        if let hostBanner = localHostStatusBanner {
                            localHostBannerPill(text: hostBanner)
                        }
                        Spacer()
                    }
                    .padding(12)
                } else {
                    VStack(spacing: 12) {
                        if let hostBanner = localHostStatusBanner {
                            localHostBannerPill(text: hostBanner)
                        }
                        unpairedCanvasPrompt
                    }
                }
            }
            .frame(minWidth: 640, minHeight: 500)

            // Right Inspector Sidebar
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    if let keychainErr = viewer.keychainAccessError {
                        VStack(alignment: .leading, spacing: 8) {
                            Text(keychainErr)
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(.white)
                            Button("Retry") {
                                viewer.retryKeychainAccess()
                            }
                            .buttonStyle(.borderedProminent)
                            .controlSize(.small)
                        }
                        .padding(10)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.orange.opacity(0.9), in: RoundedRectangle(cornerRadius: 8))
                    } else if let revoked = viewer.revokedBannerText {
                        Text(revoked)
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.white)
                            .padding(10)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.orange.opacity(0.9), in: RoundedRectangle(cornerRadius: 8))
                    } else if let err = viewer.pairingError ?? viewer.telemetry.lastCommandError {
                        Text(err)
                            .font(.caption)
                            .foregroundStyle(.white)
                            .padding(10)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.85), in: RoundedRectangle(cornerRadius: 8))
                    }

                    if viewer.keychainAccessError != nil {
                        Text("Your saved camera pairing is untouched in the Keychain. Allow access and click Retry.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    } else if !viewer.telemetry.isPaired {
                        pairingGroupBox
                    } else {
                        connectionHeaderGroupBox

                        if viewer.showCameraRollButton {
                            cameraRollGroupBox
                        }

                        if viewer.showLiveQualityPicker {
                            liveQualityGroupBox
                        }

                        if viewer.showOwnerControls {
                            sensorPowerGroupBox
                        }

                        if viewer.showPetCameraOwnerSection {
                            petCameraOwnerGroupBox
                            watchingNowGroupBox
                            if !viewer.pairedDeviceEntries.isEmpty {
                                pairedDevicesGroupBox
                            }
                        }

                        if viewer.showOwnerControls && viewer.telemetry.isConnected {
                            hostViewControlsGroupBox
                        }

                        displayOptionsGroupBox
                    }
                }
                .padding(14)
            }
            .frame(minWidth: 290, idealWidth: 320, maxWidth: 380)
        }
        .alert("Turn Off Sharing?", isPresented: $viewer.showOutsideDisableSharingAlert) {
            Button("Cancel", role: .cancel) {}
            Button("Turn Off Sharing", role: .destructive) {
                viewer.confirmDisableSharingFromOutside()
            }
        } message: {
            Text(PetCameraUserStrings.outsideDisableSharingWarning)
        }
        .alert(
            "Remove Device?",
            isPresented: Binding(
                get: { viewer.devicePendingRevoke != nil },
                set: { if !$0 { viewer.devicePendingRevoke = nil } }
            ),
            presenting: viewer.devicePendingRevoke
        ) { dev in
            Button("Cancel", role: .cancel) {
                viewer.devicePendingRevoke = nil
            }
            Button("Remove", role: .destructive) {
                viewer.confirmRevokePendingDevice()
            }
        } message: { dev in
            Text("Remove “\(dev.name)” from the camera? It will need to pair again to watch.")
        }
        .sheet(isPresented: $viewer.showCameraRollSheet, onDismiss: {
            viewer.cameraRoll.closePlayback(resumeLive: true)
        }) {
            MacCameraRollView(roll: viewer.cameraRoll) {
                viewer.showCameraRollSheet = false
            }
        }
    }

    // MARK: - Canvas Overlays

    private func keychainAccessRequiredPrompt(message: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: "key.fill")
                .font(.system(size: 38))
                .foregroundStyle(.orange)
            Text(message)
                .font(.headline)
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 420)
            Button("Retry") {
                viewer.retryKeychainAccess()
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
        }
        .padding(24)
    }

    private func localHostBannerPill(text: String) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "desktopcomputer")
                .foregroundStyle(.cyan)
            Text(text)
                .font(.caption.weight(.medium))
                .foregroundStyle(.white)
            if let switchAction = onSwitchToLocalHost {
                Button("Show This Mac's Kinect") {
                    switchAction()
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.mini)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    @ViewBuilder
    private var centerOverlayCard: some View {
        if !viewer.telemetry.isConnected {
            VStack(spacing: 12) {
                Image(systemName: "desktopcomputer.trianglebadge.exclamationmark")
                    .font(.system(size: 36))
                    .foregroundStyle(.orange)
                Text(viewer.disconnectedStatusTitle)
                    .font(.headline)
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                if viewer.telemetry.lastSessionNotice?.reason == .replaced {
                    Button("Reconnect Here") {
                        viewer.reconnectManually()
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                }
            }
            .padding(20)
            .frame(maxWidth: 400)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
        } else if let unavailable = viewer.liveUnavailableReasonText {
            VStack(spacing: 12) {
                Image(systemName: viewer.telemetry.petState.privacyMode ? "eye.slash.fill" : "video.slash.fill")
                    .font(.system(size: 34))
                    .foregroundStyle(.orange)
                Text(unavailable)
                    .font(.headline)
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                if viewer.isPetOwner && viewer.telemetry.hasConfirmedHostState {
                    Text("Sensor power: \(viewer.confirmedPowerState.displayLabel)")
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.secondary)
                }
                if viewer.isPetOwner && viewer.telemetry.petState.privacyMode {
                    Button("Turn Off Privacy Mode") {
                        viewer.setPrivacyMode(false)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                }
            }
            .padding(20)
            .frame(maxWidth: 400)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
        } else if viewer.telemetry.hasConfirmedHostState && viewer.confirmedPowerState != .on {
            VStack(spacing: 10) {
                if viewer.confirmedPowerState == .starting {
                    ProgressView()
                    Text("Starting Kinect Sensor…")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.white)
                } else if viewer.isPetViewer {
                    Image(systemName: "power.circle")
                        .font(.system(size: 34))
                        .foregroundStyle(.secondary)
                    Text("The camera is off.")
                        .font(.headline)
                        .foregroundStyle(.white)
                } else {
                    Image(systemName: "power.circle")
                        .font(.system(size: 34))
                        .foregroundStyle(.secondary)
                    Text("Kinect Sensor is Off")
                        .font(.headline)
                        .foregroundStyle(.white)
                    Text("Use the Kinect Sensor Power switch on the right to turn it On.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }
            .padding(20)
            .frame(maxWidth: 380)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
        }
    }

    private var unpairedCanvasPrompt: some View {
        VStack(spacing: 14) {
            Image("LogoMark")
                .resizable()
                .interpolation(.high)
                .scaledToFit()
                .frame(width: 56, height: 56)
                .accessibilityHidden(true)
            Text("Watch a Camera on Your Home Network")
                .font(.title3.weight(.semibold))
                .foregroundStyle(.white)
            Text("Pair with your PC's Kinect Viewer using its 8-digit short code or pairing link in the sidebar. Live H.264 video renders directly in memory with zero disk writes.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 420)
        }
        .padding(24)
    }

    private var statusPill: some View {
        HStack(spacing: 7) {
            Circle()
                .fill(viewer.telemetry.isConnected ? (viewer.isLiveVideoUnavailableV3 ? Color.orange : Color.green) : Color.orange)
                .frame(width: 8, height: 8)
            Text(viewer.telemetry.isConnected ? (viewer.isPetCameraV3 ? "Connected · \(viewer.currentPetRole.displayLabel)" : "Connected") : "Connecting…")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.white)
            if viewer.isLocalFrozen {
                Text("· Frozen")
                    .font(.caption.weight(.bold))
                    .foregroundStyle(.cyan)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(.ultraThinMaterial, in: Capsule())
    }

    private var healthPill: some View {
        HStack(spacing: 10) {
            Text(viewer.telemetry.connectionQuality.rawValue)
                .fontWeight(.semibold)
            Text(String(format: "%.1f fps", viewer.telemetry.fps))
                .monospacedDigit()
            Text(String(format: "%.0f ms", viewer.effectiveDelayMs))
                .monospacedDigit()
            Text(String(format: "%.1f Mbps", viewer.telemetry.bitrateMbps))
                .monospacedDigit()
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.white)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(.ultraThinMaterial, in: Capsule())
    }

    // MARK: - Sidebar GroupBoxes

    private var pairingGroupBox: some View {
        GroupBox("Pair with PC Camera") {
            VStack(alignment: .leading, spacing: 12) {
                Text("Enter the 8-digit short code at home, or paste a pairing / outside invite link (kv2://pair, kv2://invite, kv2://endpoint).")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                VStack(alignment: .leading, spacing: 6) {
                    Text("8-Digit Short Code (At Home)")
                        .font(.caption.weight(.semibold))
                    Label(PetCameraUserStrings.shortCodeTrustedNetworkWarning, systemImage: "exclamationmark.shield")
                        .font(.caption)
                        .foregroundStyle(.orange)
                    Toggle(PetCameraUserStrings.shortCodeTrustedNetworkConfirm, isOn: $viewer.shortCodeNetworkConfirmed)
                        .font(.caption)
                    HStack(spacing: 8) {
                        TextField("1234-5678", text: $viewer.shortCodeInput)
                            .font(.system(.body, design: .monospaced))
                            .textFieldStyle(.roundedBorder)
                        Button("Pair") {
                            viewer.submitShortCode()
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(viewer.pairingInProgress || !viewer.shortCodeNetworkConfirmed ||
                                  QRPairingPayload.normalizeShortCode(viewer.shortCodeInput).count != 8)
                    }
                }

                Divider()

                VStack(alignment: .leading, spacing: 6) {
                    Text("Or Paste Invite / Pairing Link")
                        .font(.caption.weight(.semibold))

                    if let inv = InviteLinkPayload.parse(viewer.pairURIInput) {
                        let article = (inv.roleHint == .owner) ? "an owner" : "a viewer"
                        Text("You're invited as \(article).")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.cyan)
                    }

                    HStack(spacing: 8) {
                        TextField("kv2://invite?… or kv2://pair?…", text: $viewer.pairURIInput)
                            .font(.system(.caption, design: .monospaced))
                            .textFieldStyle(.roundedBorder)
                        Button("Use Link") {
                            viewer.submitPairURI()
                        }
                        .buttonStyle(.bordered)
                        .disabled(viewer.pairingInProgress || viewer.pairURIInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                }

                if viewer.pairingInProgress {
                    HStack(spacing: 6) {
                        ProgressView().controlSize(.small)
                        Text("Verifying mutual TLS 1.3 pairing…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .padding(4)
        }
    }

    private var connectionHeaderGroupBox: some View {
        GroupBox("Camera Session") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(viewer.telemetry.isConnected ? "Connected" : "Searching…")
                            .font(.subheadline.weight(.semibold))
                        if viewer.isPetCameraV3 {
                            Text("Role: \(viewer.currentPetRole.displayLabel) · \(viewer.telemetry.petState.path.displayLabel)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    Spacer()
                    Button("Unpair", role: .destructive) {
                        viewer.unpairCamera()
                    }
                    .controlSize(.small)
                }

                if viewer.telemetry.streamWidth > 0 {
                    Text("Stream: \(viewer.telemetry.streamWidth)×\(viewer.telemetry.streamHeight)")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }

                if (viewer.telemetry.cachedHostProtocolVersion ?? 1) >= 3 || viewer.telemetry.storedOutsideEndpoint != nil {
                    Divider()
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Update Outside Address Link")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(.secondary)
                        HStack(spacing: 6) {
                            TextField("kv2://endpoint?…", text: $viewer.pairURIInput)
                                .font(.system(.caption2, design: .monospaced))
                                .textFieldStyle(.roundedBorder)
                            Button("Apply") {
                                viewer.submitPairURI()
                            }
                            .controlSize(.small)
                            .disabled(viewer.pairingInProgress || viewer.pairURIInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                        }
                    }
                }
            }
            .padding(4)
        }
    }

    private var cameraRollGroupBox: some View {
        GroupBox("Camera Roll") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    if viewer.telemetry.petState.clipRecording {
                        HStack(spacing: 5) {
                            Circle()
                                .fill(Color.red)
                                .frame(width: 7, height: 7)
                            Text("Recording \((viewer.telemetry.petState.clipTrigger ?? .manual).displayLabel) · \(CameraRollController.formatDuration(viewer.telemetry.petState.clipElapsedSec))")
                                .font(.caption.monospacedDigit().weight(.medium))
                                .foregroundStyle(.red)
                        }
                    } else {
                        Text("Dated clips streamed in memory")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    Spacer()

                    Button {
                        viewer.showCameraRollSheet = true
                    } label: {
                        Label("Open Camera Roll…", systemImage: "film.stack")
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                }

                if let blocked = viewer.cameraRoll.rollBlockedSentence, viewer.isPetOwner {
                    Text(blocked)
                        .font(.caption2)
                        .foregroundStyle(.orange)
                } else if let unavail = viewer.cameraRoll.rollUnavailableBrowseSentence {
                    Text(unavail)
                        .font(.caption2)
                        .foregroundStyle(.orange)
                }
            }
            .padding(4)
        }
    }

    private var liveQualityGroupBox: some View {
        GroupBox("Live Quality") {
            VStack(alignment: .leading, spacing: 8) {
                Picker("Quality", selection: Binding(
                    get: { viewer.selectedLiveQuality },
                    set: { viewer.selectLiveQuality($0) }
                )) {
                    ForEach(LiveQuality.selectableCases) { q in
                        Text(q.displayLabel).tag(q)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()

                HStack {
                    Text("Active Tier")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(viewer.activeLiveTierLabel)
                        .font(.caption.monospacedDigit())
                }
            }
            .padding(4)
        }
    }

    private var sensorPowerGroupBox: some View {
        GroupBox("Kinect Sensor Power") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Sensor State")
                            .font(.caption.weight(.semibold))
                        Text(viewer.telemetry.hasConfirmedHostState ? viewer.confirmedPowerState.displayLabel : "Syncing…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    if viewer.isPowerTransitionStarting {
                        ProgressView().controlSize(.small)
                    }
                    Toggle("", isOn: Binding(
                        get: { viewer.confirmedSwitchIsOn },
                        set: { viewer.requestSensorPower(turnOn: $0) }
                    ))
                    .labelsHidden()
                    .toggleStyle(.switch)
                    .disabled(!viewer.allowSensorPowerControl || viewer.isPowerTransitionStarting)
                }

                if let unavailable = viewer.liveUnavailableReasonText {
                    Text(unavailable)
                        .font(.caption2)
                        .foregroundStyle(.orange)
                } else if let reason = viewer.sensorPowerDisabledReasonText {
                    Text(reason)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(4)
        }
    }

    private var petCameraOwnerGroupBox: some View {
        GroupBox("Pet Camera (Owner)") {
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Share the Camera", isOn: Binding(
                    get: { viewer.telemetry.petState.sharingEnabled },
                    set: { viewer.requestSetSharing($0) }
                ))
                .font(.caption)

                Toggle("Privacy Mode (Stops All Video)", isOn: Binding(
                    get: { viewer.telemetry.petState.privacyMode },
                    set: { viewer.setPrivacyMode($0) }
                ))
                .font(.caption)
            }
            .padding(4)
        }
    }

    private var watchingNowGroupBox: some View {
        GroupBox("Watching Now (\(viewer.watchingNowEntries.count))") {
            VStack(alignment: .leading, spacing: 6) {
                if viewer.watchingNowEntries.isEmpty {
                    Text("No devices are watching right now.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(viewer.watchingNowEntries) { w in
                        HStack {
                            Text(w.deviceName)
                                .font(.caption.weight(.medium))
                            Text("(\(w.role.displayLabel))")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                            Spacer()
                            Text(w.path.displayLabel)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .padding(4)
        }
    }

    private var pairedDevicesGroupBox: some View {
        GroupBox("Paired Devices (\(viewer.pairedDeviceEntries.count))") {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(viewer.pairedDeviceEntries) { dev in
                    HStack {
                        Circle()
                            .fill(dev.connected ? Color.green : Color.gray.opacity(0.45))
                            .frame(width: 7, height: 7)
                        Text(dev.name)
                            .font(.caption.weight(.medium))
                        Text(dev.role.displayLabel)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                        Spacer()
                        if dev.deviceId != viewer.telemetry.petState.deviceId {
                            Button("Remove", role: .destructive) {
                                viewer.requestRevokeDevice(dev)
                            }
                            .controlSize(.mini)
                        }
                    }
                }
            }
            .padding(4)
        }
    }

    private var hostViewControlsGroupBox: some View {
        GroupBox("Camera View Controls") {
            VStack(alignment: .leading, spacing: 8) {
                Picker("Mode", selection: Binding(
                    get: { MacViewerModeChoice(rawValue: viewer.telemetry.currentModeRaw) ?? .ir },
                    set: { viewer.selectMode($0) }
                )) {
                    ForEach(MacViewerModeChoice.allCases) { mode in
                        Text(mode.label).tag(mode)
                    }
                }
                .disabled(!viewer.canControlHostMode)

                if viewer.telemetry.currentModeRaw == MacViewerModeChoice.sideBySide.rawValue {
                    Picker("Layout", selection: Binding(
                        get: { MacViewerSbsChoice(rawValue: viewer.telemetry.currentSbsLayoutRaw) ?? .irDepth },
                        set: { viewer.selectSbsLayout($0) }
                    )) {
                        ForEach(MacViewerSbsChoice.allCases) { item in
                            Text(item.label).tag(item)
                        }
                    }
                    .disabled(!viewer.canControlHostMode)
                }

                Toggle("Enable Depth Cut-Off", isOn: Binding(
                    get: { viewer.telemetry.hostState.cutoffEnabled },
                    set: { viewer.setHostCutoffEnabled($0) }
                ))
                .font(.caption)
                .disabled(!viewer.canControlHostMode)

                Button("Apply “Track me” (0.5–2.0 m)") {
                    viewer.applyHostTrackMePreset()
                }
                .controlSize(.small)
                .disabled(!viewer.canControlHostMode)

                Toggle("Freeze Camera View", isOn: Binding(
                    get: { viewer.telemetry.hostState.isHostFrozen },
                    set: { _ in viewer.toggleHostFreeze() }
                ))
                .font(.caption)
                .disabled(!viewer.canControlHostMode || viewer.confirmedPowerState != .on)
            }
            .padding(4)
        }
    }

    private var displayOptionsGroupBox: some View {
        GroupBox("Local Display") {
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Freeze Local Frame (In Memory)", isOn: Binding(
                    get: { viewer.isLocalFrozen },
                    set: { _ in viewer.toggleLocalFreeze() }
                ))
                .font(.caption)
                .disabled(!viewer.telemetry.isConnected || viewer.confirmedPowerState != .on || viewer.isLiveVideoUnavailableV3)

                Toggle("Show Health Overlay", isOn: $viewer.showHealthOverlay)
                    .font(.caption)
            }
            .padding(4)
        }
    }
}
