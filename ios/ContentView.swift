import SwiftUI

struct ContentView: View {
    @ObservedObject var viewModel: PocketViewModel

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height

            ZStack {
                Color.black.ignoresSafeArea()

                if viewModel.isScreenCaptured {
                    screenCaptureBlockedView
                } else if let keychainErr = viewModel.keychainAccessError {
                    keychainAccessRequiredView(message: keychainErr)
                } else if viewModel.telemetry.isPaired {
                    VideoSampleDisplayView(
                        bridge: viewModel.displayBridge,
                        zoomScale: viewModel.zoomScale,
                        panOffset: viewModel.panOffset,
                        is3DOrbitMode: viewModel.is3DOrbitGestureActive,
                        onSingleTap: {
                            withAnimation(.easeInOut(duration: 0.18)) {
                                viewModel.showHUD.toggle()
                            }
                        },
                        onDoubleTap: {
                            viewModel.toggleLocalFreeze()
                        },
                        onUpdate2DTransform: { scale, pan in
                            viewModel.update2DTransform(scale: scale, pan: pan)
                        },
                        onOrbit3D: { yaw, pitch in
                            viewModel.send3DOrbit(deltaYaw: yaw, deltaPitch: pitch)
                        },
                        onZoom3D: { scale in
                            viewModel.send3DZoom(scaleFactor: scale)
                        },
                        onPan3D: { right, up in
                            viewModel.send3DPan(deltaRightMeters: right, deltaUpMeters: up)
                        }
                    )
                    .ignoresSafeArea()

                    centerStateOverlay

                    if viewModel.showHUD {
                        hudOverlay(isLandscape: isLandscape)
                    }
                } else {
                    unpairedWelcomeView
                }
            }
            .statusBarHidden(isLandscape || !viewModel.showHUD)
            .persistentSystemOverlays(!viewModel.showHUD ? .hidden : .automatic)
        }
        .preferredColorScheme(.dark)
        .sheet(isPresented: $viewModel.showPairingSheet) {
            PairingSheetView(viewModel: viewModel)
        }
        .sheet(isPresented: $viewModel.showCameraRollSheet, onDismiss: {
            viewModel.cameraRoll.closePlayback(resumeLive: true)
        }) {
            CameraRollSheetView(viewModel: viewModel, roll: viewModel.cameraRoll)
        }
        .sheet(isPresented: $viewModel.showRemoteSheet) {
            RemoteControlSheetView(viewModel: viewModel)
        }
        .sheet(isPresented: $viewModel.showPhoneRecordingReminder) {
            PhoneRecordingReminderSheet(viewModel: viewModel)
                .presentationDetents([.medium])
        }
        .alert("Turn Off Sharing?", isPresented: $viewModel.showOutsideDisableSharingAlert) {
            Button("Cancel", role: .cancel) {}
            Button("Turn Off Sharing", role: .destructive) {
                viewModel.confirmDisableSharingFromOutside()
            }
        } message: {
            Text(PetCameraUserStrings.outsideDisableSharingWarning)
        }
        .alert(
            "Remove Device?",
            isPresented: Binding(
                get: { viewModel.devicePendingRevoke != nil },
                set: { if !$0 { viewModel.devicePendingRevoke = nil } }
            ),
            presenting: viewModel.devicePendingRevoke
        ) { dev in
            Button("Cancel", role: .cancel) {
                viewModel.devicePendingRevoke = nil
            }
            Button("Remove", role: .destructive) {
                viewModel.confirmRevokePendingDevice()
            }
        } message: { dev in
            Text("Remove “\(dev.name)” from the camera? It will need to pair again to watch.")
        }
    }

    // MARK: - Center State Cards

    private func keychainAccessRequiredView(message: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: "key.fill")
                .font(.system(size: 40))
                .foregroundStyle(.orange)
            Text(message)
                .font(.headline)
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 28)
            Button("Retry") {
                viewModel.retryKeychainAccess()
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
        }
        .padding(24)
    }

    @ViewBuilder
    private var centerStateOverlay: some View {
        if !viewModel.telemetry.isConnected {
            VStack(spacing: 12) {
                Image(systemName: "desktopcomputer.trianglebadge.exclamationmark")
                    .font(.system(size: 36))
                    .foregroundStyle(.orange)
                Text(viewModel.disconnectedStatusTitle)
                    .font(.headline)
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                if (viewModel.telemetry.cachedHostProtocolVersion ?? 1) < 3 &&
                    viewModel.telemetry.tlsRefusalNotice == nil &&
                    viewModel.telemetry.lastSessionNotice == nil {
                    Text("Open Kinect Viewer on your paired Mac over home Wi-Fi to watch or turn on the Kinect.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 24)
                }
                if viewModel.telemetry.isPaired {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(viewModel.awayFromHomeAddressText)
                            .font(.caption2.weight(.medium))
                        ForEach(Array(viewModel.recentConnectAttemptLines.prefix(4).enumerated()), id: \.offset) { _, line in
                            Text(line)
                                .font(.caption2.monospaced())
                                .lineLimit(2)
                        }
                    }
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .accessibilityIdentifier("connectionDiagnostics")
                }
                if viewModel.telemetry.lastSessionNotice?.reason == .replaced {
                    Button("Reconnect Here") {
                        viewModel.reconnectManually()
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                }
            }
            .padding(20)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
            .padding(.horizontal, 24)
            .allowsHitTesting(viewModel.telemetry.lastSessionNotice?.reason == .replaced)
        } else if let unavailableReason = viewModel.liveUnavailableReasonText {
            let reasonEnum = viewModel.telemetry.petState.liveUnavailableReason ?? .unknown
            let canQuickUnmutePrivacy = (viewModel.isPetOwner && viewModel.telemetry.petState.privacyMode)
            VStack(spacing: 12) {
                Image(systemName: unavailableIconName(for: reasonEnum))
                    .font(.system(size: 34))
                    .foregroundStyle(.orange)
                Text(unavailableReason)
                    .font(.headline)
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                if viewModel.isPetOwner && viewModel.telemetry.hasConfirmedHostState {
                    Text("Sensor power: \(viewModel.confirmedPowerState.displayLabel)")
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.secondary)
                }
                if canQuickUnmutePrivacy {
                    Button("Turn Off Privacy Mode") {
                        viewModel.setPrivacyMode(false)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                }
            }
            .padding(20)
            .frame(maxWidth: 380)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
            .padding(.horizontal, 24)
            .allowsHitTesting(canQuickUnmutePrivacy)
        } else if viewModel.telemetry.hasConfirmedHostState && viewModel.confirmedPowerState != .on {
            VStack(spacing: 10) {
                if viewModel.confirmedPowerState == .starting {
                    ProgressView()
                        .tint(.cyan)
                    Text("Starting Kinect Sensor…")
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.white)
                } else if let inUse = viewModel.inUseBannerText {
                    Image(systemName: "lock.fill")
                        .font(.system(size: 30))
                        .foregroundStyle(.orange)
                    Text(inUse)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.orange)
                        .multilineTextAlignment(.center)
                } else if let err = viewModel.powerErrorBannerText {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.system(size: 30))
                        .foregroundStyle(.red)
                    Text(err)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.center)
                } else if viewModel.isPetViewer {
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
                    Text(viewModel.isPetCameraV3
                         ? "The camera is off. Use the switch above to turn it On."
                         : "USB closed and lock released on Mac. Use the switch above to turn it On.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }
            .padding(18)
            .frame(maxWidth: 360)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
            .padding(.horizontal, 24)
            .allowsHitTesting(false)
        }
    }

    private func unavailableIconName(for reason: LiveUnavailableReason) -> String {
        switch reason {
        case .privacyMode:
            return "eye.slash.fill"
        case .pcReplaying:
            return "play.rectangle.fill"
        case .sharingOff, .sharingSchedule:
            return "person.2.slash.fill"
        case .tooManyViewers:
            return "person.3.fill"
        case .unknown:
            return "video.slash.fill"
        }
    }

    private var screenCaptureBlockedView: some View {
        VStack(spacing: 14) {
            Image(systemName: "eye.trianglebadge.exclamationmark")
                .font(.system(size: 44))
                .foregroundStyle(.orange)
            Text("Live View Paused During Screen Capture")
                .font(.headline)
                .foregroundStyle(.white)
            Text("Screen recording or AirPlay mirroring is active. Stop capture to resume the encrypted Kinect feed.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
        }
    }

    private var unpairedWelcomeView: some View {
        VStack(spacing: 16) {
            Image("LogoMark")
                .resizable()
                .interpolation(.high)
                .scaledToFit()
                .frame(width: 48, height: 48)
                .accessibilityHidden(true)

            if let revoked = viewModel.revokedBannerText {
                Text(revoked)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                    .background(Color.orange.opacity(0.88), in: RoundedRectangle(cornerRadius: 12))
                    .padding(.horizontal, 24)
            }

            Text("Not Paired with a Camera")
                .font(.title3.weight(.semibold))
                .foregroundStyle(.white)
            Text("Pair once over your home Wi-Fi using a QR code or 8-digit short code. Zero frames or caches are ever written to your iPhone.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
            Button("Pair Camera") {
                viewModel.showPairingSheet = true
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
        }
    }

    // MARK: - HUD Overlay (Portrait + Full-Screen Landscape)

    private func hudOverlay(isLandscape: Bool) -> some View {
        VStack(spacing: isLandscape ? 6 : 10) {
            if isLandscape {
                HStack(alignment: .top, spacing: 10) {
                    topPowerAndToolbarCard(compact: true)
                    if viewModel.showHealthDisplay {
                        healthDisplayCard(compact: true)
                    }
                }
            } else {
                topPowerAndToolbarCard(compact: false)
                if viewModel.showHealthDisplay {
                    healthDisplayCard(compact: false)
                }
            }

            statusBadgesRow

            Spacer(minLength: 0)

            bottomControlsBar(compact: isLandscape)
        }
        .padding(.horizontal, isLandscape ? 16 : 14)
        .padding(.vertical, isLandscape ? 8 : 10)
    }

    private func topPowerAndToolbarCard(compact: Bool) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                if viewModel.telemetry.isConnected {
                    if viewModel.showSensorPowerToggle {
                        powerStateIndicatorDot

                        VStack(alignment: .leading, spacing: 1) {
                            HStack(spacing: 6) {
                                Text("Kinect Power")
                                    .font(.subheadline.weight(.semibold))
                                    .foregroundStyle(.white)
                                if viewModel.isPetOwner {
                                    Text("Owner")
                                        .font(.system(size: 10, weight: .bold))
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(Color.cyan.opacity(0.24), in: Capsule())
                                        .foregroundStyle(.cyan)
                                }
                            }
                            Text(confirmedStateSubtitle)
                                .font(.caption2.weight(.medium))
                                .foregroundStyle(powerSubtitleColor)
                                .lineLimit(1)
                        }

                        Spacer(minLength: 6)

                        if viewModel.isPowerTransitionStarting {
                            ProgressView()
                                .controlSize(.small)
                                .tint(.cyan)
                        }

                        // Confirmed On/Off switch (Option A: enabled for v3 owners even while liveAllowed == false)
                        Toggle("", isOn: Binding(
                            get: { viewModel.confirmedSwitchIsOn },
                            set: { wantOn in viewModel.requestSensorPower(turnOn: wantOn) }
                        ))
                        .labelsHidden()
                        .tint(.green)
                        .scaleEffect(compact ? 1.0 : 1.08)
                        .disabled(!viewModel.allowSensorPowerControl || viewModel.isPowerTransitionStarting)
                        .accessibilityLabel("Kinect Sensor Power")
                    } else if viewModel.isPetCameraV3 {
                        Circle()
                            .fill(viewModel.isLiveVideoUnavailableV3 ? Color.orange : Color.green)
                            .frame(width: 9, height: 9)

                        VStack(alignment: .leading, spacing: 1) {
                            HStack(spacing: 6) {
                                Text(viewModel.telemetry.petState.deviceName.isEmpty ? "Live Camera" : viewModel.telemetry.petState.deviceName)
                                    .font(.subheadline.weight(.semibold))
                                    .foregroundStyle(.white)
                                    .lineLimit(1)
                                Text(viewModel.currentPetRole == .viewer ? "Viewer" : "Connecting…")
                                    .font(.system(size: 10, weight: .bold))
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(Color.white.opacity(0.16), in: Capsule())
                                    .foregroundStyle(.secondary)
                            }
                            Text(viewModel.isLiveVideoUnavailableV3 ? "Video unavailable" : (viewModel.confirmedPowerState == .on ? "Watching live" : "Camera off"))
                                .font(.caption2.weight(.medium))
                                .foregroundStyle(viewModel.isLiveVideoUnavailableV3 ? .orange : .secondary)
                                .lineLimit(1)
                        }

                        Spacer(minLength: 6)
                    } else {
                        Circle()
                            .fill(Color.green)
                            .frame(width: 9, height: 9)

                        Text(viewModel.telemetry.connectionState)
                            .font(.footnote.weight(.semibold))
                            .foregroundStyle(.white)
                            .lineLimit(1)

                        Spacer(minLength: 6)
                    }
                } else {
                    Circle()
                        .fill(Color.orange)
                        .frame(width: 9, height: 9)

                    Text(viewModel.disconnectedStatusTitle)
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.white)
                        .lineLimit(1)

                    Spacer(minLength: 6)
                }

                Divider()
                    .frame(height: 22)

                Button {
                    withAnimation(.easeInOut(duration: 0.16)) {
                        viewModel.showHealthDisplay.toggle()
                    }
                } label: {
                    Image(systemName: viewModel.showHealthDisplay ? "waveform.path.ecg.rectangle.fill" : "waveform.path.ecg")
                        .font(.body)
                        .foregroundStyle(viewModel.showHealthDisplay ? .cyan : .white)
                }
                .accessibilityLabel("Toggle Health Display")

                if viewModel.showCameraRollButton {
                    Button {
                        viewModel.showCameraRollSheet = true
                    } label: {
                        ZStack(alignment: .topTrailing) {
                            Image(systemName: "film.stack")
                                .font(.body)
                                .foregroundStyle(.white)
                            if viewModel.telemetry.petState.clipRecording {
                                Circle()
                                    .fill(Color.red)
                                    .frame(width: 7, height: 7)
                                    .offset(x: 3, y: -2)
                            }
                        }
                    }
                    .accessibilityLabel("Open Camera Roll")
                }

                if viewModel.showOwnerSettingsAndRemoteControls {
                    Button {
                        viewModel.showRemoteSheet = true
                    } label: {
                        Image(systemName: "slider.horizontal.3")
                            .font(.body)
                            .foregroundStyle(.white)
                    }
                    .accessibilityLabel("Open Remote Control Sheet")
                }

                Menu {
                    Label("Zero-Storage Pinned TLS 1.3", systemImage: "checkmark.shield")
                    if viewModel.telemetry.streamWidth > 0 {
                        Text("Stream: \(viewModel.telemetry.streamWidth)×\(viewModel.telemetry.streamHeight)")
                    }
                    if viewModel.showCameraRollButton {
                        Button {
                            viewModel.showCameraRollSheet = true
                        } label: {
                            Label("Camera Roll", systemImage: "film.stack")
                        }
                    }
                    if (viewModel.telemetry.cachedHostProtocolVersion ?? 1) >= 3 || viewModel.telemetry.storedOutsideEndpoint != nil {
                        Button {
                            viewModel.pairingError = nil
                            viewModel.showPairingSheet = true
                        } label: {
                            Label("Paste Camera / Endpoint Link…", systemImage: "link")
                        }
                    }
                    Button {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            viewModel.showHUD = false
                        }
                    } label: {
                        Label("Full-Screen Clean View (Tap to Restore)", systemImage: "arrow.up.left.and.arrow.down.right")
                    }
                    Divider()
                    Button(role: .destructive) {
                        viewModel.unpairDevice()
                    } label: {
                        Label(
                            (viewModel.telemetry.cachedHostProtocolVersion ?? 1) >= 3 ? "Unpair Camera" : "Unpair Mac",
                            systemImage: "trash"
                        )
                    }
                } label: {
                    Image(systemName: "ellipsis.circle.fill")
                        .font(.title3)
                        .foregroundStyle(.white.opacity(0.88))
                }
            }

            if let unavailable = viewModel.liveUnavailableReasonText {
                Text(unavailable)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.orange)
            } else if let inUse = viewModel.inUseBannerText {
                Text(inUse)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.orange)
            } else if let err = viewModel.powerErrorBannerText {
                Text(err)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.red)
            } else if let powerDisabled = viewModel.sensorPowerDisabledReasonText {
                Text(powerDisabled)
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(.secondary)
            }

            if let cmdErr = viewModel.pairingError ?? viewModel.telemetry.lastCommandError {
                Text(cmdErr)
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(.red)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, compact ? 8 : 10)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
    }

    private func healthDisplayCard(compact: Bool) -> some View {
        let quality = viewModel.telemetry.connectionQuality
        return HStack(spacing: 12) {
            HStack(spacing: 5) {
                Circle()
                    .fill(qualityColor(quality))
                    .frame(width: 8, height: 8)
                Text(quality.rawValue)
                    .fontWeight(.semibold)
                    .foregroundStyle(qualityColor(quality))
            }

            Text("·").foregroundStyle(.secondary)

            Text(String(format: "%.1f fps", viewModel.telemetry.fps))
                .monospacedDigit()

            Text("·").foregroundStyle(.secondary)

            Text(String(format: "Delay %.0f ms", viewModel.effectiveDelayMs))
                .monospacedDigit()

            if !compact {
                Text("·").foregroundStyle(.secondary)
                Text(String(format: "%.1f Mbps", viewModel.telemetry.bitrateMbps))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            }

            if viewModel.telemetry.barcodeVerifiedFrames > 0 && !compact {
                Text(String(format: "(px %.0f ms)", viewModel.telemetry.barcodePixelLatencyMs))
                    .monospacedDigit()
                    .foregroundStyle(.cyan)
            }
        }
        .font(.system(size: 12, design: .monospaced))
        .foregroundStyle(.white)
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(.ultraThinMaterial, in: Capsule())
    }

    @ViewBuilder
    private var statusBadgesRow: some View {
        HStack(spacing: 8) {
            if viewModel.isPetOwner && viewModel.telemetry.petState.privacyMode {
                Button {
                    viewModel.setPrivacyMode(false)
                } label: {
                    Label("Privacy On (Tap to Resume)", systemImage: "eye.slash.fill")
                        .font(.caption2.weight(.semibold))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.orange.opacity(0.9), in: Capsule())
                        .foregroundStyle(.black)
                }
            }

            if viewModel.isLocalFrozen {
                Button {
                    viewModel.toggleLocalFreeze()
                } label: {
                    Label("Phone Frozen (Double-Tap)", systemImage: "pause.circle.fill")
                        .font(.caption2.weight(.semibold))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.cyan.opacity(0.85), in: Capsule())
                        .foregroundStyle(.black)
                }
            }

            if viewModel.showOwnerSettingsAndRemoteControls && viewModel.telemetry.hostState.isHostFrozen {
                Button {
                    viewModel.toggleHostFreeze()
                } label: {
                    Label(viewModel.isPetCameraV3 ? "Camera Frozen" : "Host Frozen", systemImage: "snowflake")
                        .font(.caption2.weight(.semibold))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.blue.opacity(0.85), in: Capsule())
                        .foregroundStyle(.white)
                }
            }

            if viewModel.showOwnerSettingsAndRemoteControls && viewModel.telemetry.hostState.isRecording {
                HStack(spacing: 5) {
                    Circle().fill(.red).frame(width: 7, height: 7)
                    Text(String(format: "REC %.0fs", viewModel.telemetry.hostState.recordingElapsedSec))
                        .monospacedDigit()
                }
                .font(.caption2.weight(.bold))
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(Color.red.opacity(0.28), in: Capsule())
                .foregroundStyle(.white)
            }

            if viewModel.zoomScale > 1.02 {
                Button {
                    viewModel.reset2DZoomAndPan()
                } label: {
                    Label(String(format: "%.1f× Reset", viewModel.zoomScale), systemImage: "arrow.counterclockwise")
                        .font(.caption2.weight(.semibold))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(.ultraThinMaterial, in: Capsule())
                        .foregroundStyle(.white)
                }
            }

            if viewModel.showOwnerSettingsAndRemoteControls &&
                viewModel.confirmedPowerState == .on &&
                viewModel.telemetry.currentModeRaw == PocketModeChoice.pointCloud3D.rawValue {
                Button {
                    viewModel.prefer2DZoomIn3DMode.toggle()
                } label: {
                    Label(
                        viewModel.prefer2DZoomIn3DMode ? "2D Pinch/Pan" : "3D Orbit Active",
                        systemImage: viewModel.prefer2DZoomIn3DMode ? "arrow.up.left.and.arrow.down.right.magnifyingglass" : "cube.transparent"
                    )
                    .font(.caption2.weight(.semibold))
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .background(.ultraThinMaterial, in: Capsule())
                    .foregroundStyle(.cyan)
                }

                if !viewModel.prefer2DZoomIn3DMode {
                    Button {
                        viewModel.reset3DCamera()
                    } label: {
                        Text("Reset 3D")
                            .font(.caption2.weight(.semibold))
                            .padding(.horizontal, 10)
                            .padding(.vertical, 5)
                            .background(.ultraThinMaterial, in: Capsule())
                            .foregroundStyle(.white)
                    }
                }
            }

            Spacer(minLength: 0)
        }
    }

    @ViewBuilder
    private func bottomControlsBar(compact: Bool) -> some View {
        if viewModel.telemetry.isConnected {
            VStack(spacing: compact ? 6 : 8) {
                if viewModel.showLiveQualityPicker {
                    HStack(spacing: 8) {
                        Text("Quality")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(.secondary)
                        Picker("Live Quality", selection: Binding(
                            get: { viewModel.selectedLiveQuality },
                            set: { viewModel.selectLiveQuality($0) }
                        )) {
                            ForEach(LiveQuality.selectableCases) { q in
                                Text(q.displayLabel).tag(q)
                            }
                        }
                        .pickerStyle(.segmented)

                        if viewModel.selectedLiveQuality == .auto && viewModel.telemetry.petState.watching {
                            Text(viewModel.activeLiveTierLabel)
                                .font(.system(size: 10, weight: .semibold, design: .monospaced))
                                .foregroundStyle(.cyan)
                        }
                    }
                }

                if viewModel.canControlHostMode {
                    if viewModel.telemetry.currentModeRaw == PocketModeChoice.sideBySide.rawValue {
                        Picker("Layout", selection: Binding(
                            get: { PocketSbsChoice(rawValue: viewModel.telemetry.currentSbsLayoutRaw) ?? .irDepth },
                            set: { viewModel.selectSbsLayout($0) }
                        )) {
                            ForEach(PocketSbsChoice.allCases) { item in
                                Text(item.label).tag(item)
                            }
                        }
                        .pickerStyle(.segmented)
                    }

                    HStack(spacing: 8) {
                        Picker("Mode", selection: Binding(
                            get: { PocketModeChoice(rawValue: viewModel.telemetry.currentModeRaw) ?? .ir },
                            set: { viewModel.selectMode($0) }
                        )) {
                            ForEach(viewModel.availableModes) { mode in
                                Text(mode.label).tag(mode)
                            }
                        }
                        .pickerStyle(.segmented)

                        if viewModel.showOwnerSettingsAndRemoteControls {
                            Button {
                                viewModel.showRemoteSheet = true
                            } label: {
                                Image(systemName: "slider.horizontal.3")
                                    .font(.subheadline.weight(.semibold))
                                    .padding(8)
                                    .background(Color.white.opacity(0.14), in: Circle())
                                    .foregroundStyle(.white)
                            }
                            .accessibilityLabel("Remote Controls")
                        }
                    }
                } else {
                    let modeTitle = PocketModeChoice(rawValue: viewModel.telemetry.currentModeRaw)?.label ?? "Live"
                    let viewOnlyText = viewModel.isPetCameraV3
                        ? (viewModel.isPetViewer ? "Viewing \(modeTitle)" : "View-Only Mode (\(modeTitle)) — Controlled on Camera")
                        : "View-Only Mode (\(modeTitle)) — Controlled on Mac"
                    HStack {
                        Text(viewOnlyText)
                            .font(.caption.weight(.medium))
                            .foregroundStyle(.secondary)
                        if viewModel.showOwnerSettingsAndRemoteControls {
                            Spacer()
                            Button {
                                viewModel.showRemoteSheet = true
                            } label: {
                                Label("Controls", systemImage: "slider.horizontal.3")
                                    .font(.caption.weight(.semibold))
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.mini)
                        }
                    }
                }
            }
            .padding(compact ? 8 : 10)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
        }
    }

    // MARK: - Formatting Helpers

    private var confirmedStateSubtitle: String {
        guard viewModel.telemetry.hasConfirmedHostState else {
            return viewModel.isPetCameraV3 ? "Syncing state with camera…" : "Syncing state with Mac…"
        }
        return viewModel.confirmedPowerState.displayLabel
    }

    private var powerSubtitleColor: Color {
        switch viewModel.confirmedPowerState {
        case .on: return .green
        case .starting: return .cyan
        case .inUseElsewhere: return .orange
        case .error: return .red
        case .off: return .secondary
        }
    }

    private var powerStateIndicatorDot: some View {
        Circle()
            .fill(powerSubtitleColor == .secondary ? Color.gray : powerSubtitleColor)
            .frame(width: 10, height: 10)
    }

    private func qualityColor(_ quality: PocketConnectionQuality) -> Color {
        switch quality {
        case .excellent: return .green
        case .good: return .mint
        case .fair: return .orange
        case .poor: return .red
        case .standby: return .secondary
        case .offline: return .orange
        }
    }
}

// MARK: - Remote Control & Owner Pet Camera Sheet

struct RemoteControlSheetView: View {
    @ObservedObject var viewModel: PocketViewModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                if viewModel.showLiveQualityPicker {
                    Section("Live Stream Quality") {
                        Picker("Quality", selection: Binding(
                            get: { viewModel.selectedLiveQuality },
                            set: { viewModel.selectLiveQuality($0) }
                        )) {
                            ForEach(LiveQuality.selectableCases) { q in
                                Text(q.displayLabel).tag(q)
                            }
                        }
                        .pickerStyle(.segmented)

                        HStack {
                            Text("Active Encoder Tier")
                                .foregroundStyle(.secondary)
                            Spacer()
                            Text(viewModel.activeLiveTierLabel)
                                .monospacedDigit()
                        }
                    }
                }

                if viewModel.showPetCameraOwnerSection {
                    Section("Pet Camera (Owner)") {
                        Toggle("Share the Camera", isOn: Binding(
                            get: { viewModel.telemetry.petState.sharingEnabled },
                            set: { viewModel.requestSetSharing($0) }
                        ))

                        if viewModel.telemetry.petState.sharingScheduleOff {
                            Label("Sharing schedule currently has viewer access paused.", systemImage: "clock")
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }

                        Toggle("Privacy Mode (Stops All Video)", isOn: Binding(
                            get: { viewModel.telemetry.petState.privacyMode },
                            set: { viewModel.setPrivacyMode($0) }
                        ))
                        .tint(.orange)
                    }

                    Section("Watching Now (\(viewModel.watchingNowEntries.count))") {
                        if viewModel.watchingNowEntries.isEmpty {
                            Text("No devices are watching video right now.")
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        } else {
                            ForEach(viewModel.watchingNowEntries) { watcher in
                                HStack {
                                    VStack(alignment: .leading, spacing: 2) {
                                        HStack(spacing: 6) {
                                            Text(watcher.deviceName)
                                                .font(.subheadline.weight(.medium))
                                            if watcher.deviceId == viewModel.telemetry.petState.deviceId {
                                                Text("This Phone")
                                                    .font(.caption2.weight(.semibold))
                                                    .foregroundStyle(.cyan)
                                            }
                                        }
                                        Text("\(watcher.role.displayLabel) · \(watcher.path.displayLabel)")
                                            .font(.caption)
                                            .foregroundStyle(.secondary)
                                    }
                                    Spacer()
                                    Text(watcher.activity.capitalized)
                                        .font(.caption.monospacedDigit())
                                        .foregroundStyle(.green)
                                }
                            }
                        }
                    }

                    if !viewModel.pairedDeviceEntries.isEmpty {
                        Section("Paired Devices (\(viewModel.pairedDeviceEntries.count))") {
                            ForEach(viewModel.pairedDeviceEntries) { dev in
                                HStack {
                                    Circle()
                                        .fill(dev.connected ? Color.green : Color.gray.opacity(0.45))
                                        .frame(width: 8, height: 8)
                                    VStack(alignment: .leading, spacing: 2) {
                                        HStack(spacing: 6) {
                                            Text(dev.name)
                                                .font(.subheadline.weight(.medium))
                                            if dev.deviceId == viewModel.telemetry.petState.deviceId {
                                                Text("This Phone")
                                                    .font(.caption2.weight(.semibold))
                                                    .foregroundStyle(.cyan)
                                            }
                                        }
                                        Text(dev.role.displayLabel)
                                            .font(.caption)
                                            .foregroundStyle(.secondary)
                                    }
                                    Spacer()
                                    if dev.deviceId != viewModel.telemetry.petState.deviceId {
                                        Button("Remove", role: .destructive) {
                                            viewModel.requestRevokeDevice(dev)
                                        }
                                        .buttonStyle(.bordered)
                                        .controlSize(.small)
                                    }
                                }
                            }
                        }
                    }
                }

                if viewModel.showSensorPowerToggle {
                    Section("Kinect Sensor Power") {
                        if viewModel.telemetry.isConnected {
                            HStack(spacing: 14) {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text("Kinect v2 Sensor")
                                        .font(.headline)
                                    Text("State: \(viewModel.telemetry.hasConfirmedHostState ? viewModel.confirmedPowerState.displayLabel : "Syncing…")")
                                        .font(.subheadline)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                if viewModel.isPowerTransitionStarting {
                                    ProgressView()
                                        .padding(.trailing, 4)
                                }
                                Toggle("", isOn: Binding(
                                    get: { viewModel.confirmedSwitchIsOn },
                                    set: { viewModel.requestSensorPower(turnOn: $0) }
                                ))
                                .labelsHidden()
                                .tint(.green)
                                .scaleEffect(1.12)
                                .disabled(!viewModel.allowSensorPowerControl || viewModel.isPowerTransitionStarting)
                            }
                            .padding(.vertical, 6)

                            if let unavailable = viewModel.liveUnavailableReasonText {
                                Label(unavailable, systemImage: "eye.slash")
                                    .font(.footnote.weight(.medium))
                                    .foregroundStyle(.orange)
                            }
                            if let inUse = viewModel.inUseBannerText {
                                Label(inUse, systemImage: "lock.fill")
                                    .font(.footnote.weight(.medium))
                                    .foregroundStyle(.orange)
                            } else if let err = viewModel.powerErrorBannerText {
                                Label(err, systemImage: "exclamationmark.triangle.fill")
                                    .font(.footnote.weight(.medium))
                                    .foregroundStyle(.red)
                            } else if let powerDisabled = viewModel.sensorPowerDisabledReasonText {
                                Label(powerDisabled, systemImage: "lock.slash")
                                    .font(.footnote)
                                    .foregroundStyle(.secondary)
                            }
                        } else {
                            Label(viewModel.disconnectedStatusTitle, systemImage: "desktopcomputer.trianglebadge.exclamationmark")
                                .font(.subheadline.weight(.semibold))
                                .foregroundStyle(.orange)
                                .padding(.vertical, 4)
                        }
                    }
                }

                if viewModel.telemetry.isConnected && viewModel.showOwnerSettingsAndRemoteControls {
                    Section("View Mode") {
                        Picker("Mode", selection: Binding(
                            get: { PocketModeChoice(rawValue: viewModel.telemetry.currentModeRaw) ?? .ir },
                            set: { viewModel.selectMode($0) }
                        )) {
                            ForEach(PocketModeChoice.allCases) { m in
                                Text(m.label).tag(m)
                            }
                        }
                        .disabled(!viewModel.canControlHostMode)

                        if viewModel.telemetry.currentModeRaw == PocketModeChoice.sideBySide.rawValue {
                            Picker("Side-by-Side Layout", selection: Binding(
                                get: { PocketSbsChoice(rawValue: viewModel.telemetry.currentSbsLayoutRaw) ?? .irDepth },
                                set: { viewModel.selectSbsLayout($0) }
                            )) {
                                ForEach(PocketSbsChoice.allCases) { item in
                                    Text(item.label).tag(item)
                                }
                            }
                            .disabled(!viewModel.canControlHostMode)
                        }

                        if viewModel.telemetry.currentModeRaw == PocketModeChoice.pointCloud3D.rawValue {
                            Button("Reset 3D Orbit Camera") {
                                viewModel.reset3DCamera()
                            }
                            .disabled(!viewModel.canControlHostMode)
                        }
                    }

                    Section("Depth Cut-Off") {
                        Toggle("Enable Near/Far Cut-Off", isOn: Binding(
                            get: { viewModel.telemetry.hostState.cutoffEnabled },
                            set: { viewModel.setHostCutoffEnabled($0) }
                        ))
                        .disabled(!viewModel.canControlHostMode)

                        HStack {
                            Text("Active Window")
                                .foregroundStyle(.secondary)
                            Spacer()
                            Text("\(Int(viewModel.telemetry.hostState.cutoffNearMm))–\(Int(viewModel.telemetry.hostState.cutoffFarMm)) mm")
                                .monospacedDigit()
                        }

                        Button {
                            viewModel.applyHostTrackMePreset()
                        } label: {
                            Label("Apply “Track me” (0.5–2.0 m)", systemImage: "person.crop.rectangle")
                        }
                        .disabled(!viewModel.canControlHostMode)
                    }

                    Section("Freeze Controls") {
                        Toggle(viewModel.isPetCameraV3 ? "Freeze Camera View" : "Freeze Mac Host View", isOn: Binding(
                            get: { viewModel.telemetry.hostState.isHostFrozen },
                            set: { _ in viewModel.toggleHostFreeze() }
                        ))
                        .disabled(!viewModel.canControlHostMode || viewModel.confirmedPowerState != .on)

                        Toggle("Freeze iPhone View (In Memory)", isOn: Binding(
                            get: { viewModel.isLocalFrozen },
                            set: { _ in viewModel.toggleLocalFreeze() }
                        ))
                        .disabled(viewModel.confirmedPowerState != .on || viewModel.isLiveVideoUnavailableV3)
                    }

                    Section(viewModel.isPetCameraV3 ? "Depth / IR Recording" : "Host Depth / IR Recording (Mac Only)") {
                        let hs = viewModel.telemetry.hostState
                        if hs.isRecording {
                            HStack {
                                Label(viewModel.isPetCameraV3 ? "Recording on Camera" : "Recording on Mac", systemImage: "record.circle.fill")
                                    .foregroundStyle(.red)
                                Spacer()
                                Text(String(format: "%llu frames · %.1fs", hs.recordingFrames, hs.recordingElapsedSec))
                                    .font(.caption.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }

                            Button(role: .destructive) {
                                viewModel.requestToggleHostRecording()
                            } label: {
                                Label(viewModel.isPetCameraV3 ? "Stop Camera Recording" : "Stop Host Recording", systemImage: "stop.fill")
                            }
                            .disabled(!viewModel.canControlHostMode)
                        } else {
                            if !hs.canRecordCurrentMode {
                                Text(hs.recordingDisabledReason ?? SharedRecordingReminderConstants.colourLiveOnlyText)
                                    .font(.footnote)
                                    .foregroundStyle(.orange)
                            }

                            Button {
                                viewModel.requestToggleHostRecording()
                            } label: {
                                Label(viewModel.isPetCameraV3 ? "Start Depth/IR Recording" : "Start Host Depth/IR Recording", systemImage: "record.circle")
                            }
                            .disabled(
                                !viewModel.canControlHostMode ||
                                viewModel.confirmedPowerState != .on ||
                                !hs.canRecordCurrentMode
                            )
                        }
                    }

                    Section("Display & Telemetry") {
                        Toggle("Show Health Overlay on Video", isOn: $viewModel.showHealthDisplay)
                        HStack {
                            Text("Connection Quality")
                            Spacer()
                            Text(viewModel.telemetry.connectionQuality.rawValue)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .navigationTitle("Remote Controls")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}

struct PhoneRecordingReminderSheet: View {
    @ObservedObject var viewModel: PocketViewModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 18) {
                Label("Confirm Recording", systemImage: "exclamationmark.shield.fill")
                    .font(.title3.weight(.semibold))
                    .foregroundStyle(.orange)

                Text(SharedRecordingReminderConstants.exactReminderText)
                    .font(.body.weight(.medium))
                    .fixedSize(horizontal: false, vertical: true)

                Toggle("Don't remind me for 1 hour", isOn: $viewModel.phoneReminderSnoozeOneHour)
                    .font(.subheadline)

                Spacer(minLength: 8)

                VStack(spacing: 10) {
                    // Cancel is the primary/default prominent button matching Phase 2 policy
                    Button {
                        dismiss()
                    } label: {
                        Text("Cancel")
                            .fontWeight(.semibold)
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)

                    Button {
                        viewModel.confirmPhoneRecordingReminderAndStart()
                    } label: {
                        Text(viewModel.isPetCameraV3 ? "Confirm & Start Recording" : "Confirm & Start Recording on Mac")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.large)

                    Button(viewModel.isPetCameraV3 ? "Show Reminder on Camera Screen Instead" : "Show Reminder on Mac Screen Instead") {
                        viewModel.promptRecordingReminderOnMacInstead()
                    }
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                }
            }
            .padding(22)
            .navigationTitle("Recording Check")
            .navigationBarTitleDisplayMode(.inline)
        }
    }
}

struct PairingSheetView: View {
    @ObservedObject var viewModel: PocketViewModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    HStack(spacing: 10) {
                        Image("LogoMark")
                            .resizable()
                            .interpolation(.high)
                            .scaledToFit()
                            .frame(width: 32, height: 32)
                            .accessibilityHidden(true)
                        Text("Encrypted Home Wi-Fi Pairing")
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(.secondary)
                    }

                    if let err = viewModel.pairingError {
                        Text(err)
                            .font(.footnote.weight(.medium))
                            .foregroundStyle(.white)
                            .padding(10)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.85), in: RoundedRectangle(cornerRadius: 10))
                    }

                    VStack(alignment: .leading, spacing: 8) {
                        Text("1. Scan QR Code")
                            .font(.headline)
                        Text("Scans a home Wi-Fi pairing QR code, an outside-the-home invite QR code, or a re-shared camera address QR code.")
                            .font(.caption)
                            .foregroundStyle(.secondary)

                        QRScannerView { scannedURI in
                            viewModel.submitQRCodeURI(scannedURI)
                        }
                        .frame(height: 210)
                    }

                    Divider()

                    VStack(alignment: .leading, spacing: 10) {
                        Text("2. Or Enter 8-Digit Short Code (At Home)")
                            .font(.headline)
                        Text("Finds your camera on your home Wi-Fi.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Label(PetCameraUserStrings.shortCodeTrustedNetworkWarning, systemImage: "exclamationmark.shield")
                            .font(.caption)
                            .foregroundStyle(.orange)
                        Toggle(PetCameraUserStrings.shortCodeTrustedNetworkConfirm, isOn: $viewModel.shortCodeNetworkConfirmed)
                            .font(.caption)

                        HStack(spacing: 10) {
                            TextField("1234-5678", text: $viewModel.shortCodeInput)
                                .keyboardType(.numberPad)
                                .font(.system(.title3, design: .monospaced))
                                .textFieldStyle(.roundedBorder)

                            Button {
                                viewModel.submitShortCode()
                            } label: {
                                if viewModel.pairingInProgress {
                                    ProgressView()
                                } else {
                                    Text("Pair")
                                        .fontWeight(.semibold)
                                }
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(viewModel.pairingInProgress || !viewModel.shortCodeNetworkConfirmed ||
                                      QRPairingPayload.normalizeShortCode(viewModel.shortCodeInput).count != 8)
                        }
                    }

                    Divider()

                    VStack(alignment: .leading, spacing: 10) {
                        Text("3. Or Paste Invite / Camera Link")
                            .font(.headline)
                        Text("Paste a one-time outside invite (kv2://invite?…) or an updated camera address link (kv2://endpoint?…).")
                            .font(.caption)
                            .foregroundStyle(.secondary)

                        if let inv = InviteLinkPayload.parse(viewModel.inviteOrLinkInput) {
                            let article = (inv.roleHint == .owner) ? "an owner" : "a viewer"
                            Label("You're invited as \(article).", systemImage: "person.badge.key.fill")
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(.cyan)
                        }

                        HStack(spacing: 10) {
                            TextField("kv2://invite?… or kv2://endpoint?…", text: $viewModel.inviteOrLinkInput)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .font(.system(.footnote, design: .monospaced))
                                .textFieldStyle(.roundedBorder)

                            Button {
                                viewModel.submitCameraLinkInput()
                            } label: {
                                if viewModel.pairingInProgress {
                                    ProgressView()
                                } else {
                                    Text("Use Link")
                                        .fontWeight(.semibold)
                                }
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(viewModel.pairingInProgress || viewModel.inviteOrLinkInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                        }
                    }
                }
                .padding(18)
            }
            .navigationTitle("Pair Kinect Viewer")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                if viewModel.telemetry.isPaired {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Close") { dismiss() }
                    }
                }
            }
        }
    }
}
