import SwiftUI

struct MainContentView: View {
    @ObservedObject var controller: HostController
    @State private var newPresetName: String = ""

    var body: some View {
        HSplitView {
            // Live / Frozen Interactive Sensor Canvas
            ZStack {
                Color.black

                if let image = controller.previewImage {
                    if controller.selectedMode == .pointCloud3D {
                        PointCloud3DCanvasView(
                            image: image,
                            onOrbit: { dYaw, dPitch in
                                controller.orbit3DCamera(deltaYaw: dYaw, deltaPitch: dPitch)
                            },
                            onZoom: { factor in
                                controller.zoom3DCamera(scaleFactor: factor)
                            },
                            onPan: { dRight, dUp in
                                controller.pan3DCamera(deltaRightMeters: dRight, deltaUpMeters: dUp)
                            },
                            onResetCamera: {
                                controller.reset3DCamera()
                            }
                        )
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        InteractiveSensorCanvas(
                            image: image,
                            crispNearest: controller.crispPixelScaling && controller.selectedMode != .color,
                            viewport: controller.viewportTransform,
                            toolMode: controller.canvasToolMode,
                            regionBox: controller.regionSelectionBox,
                            rulerState: controller.rulerState,
                            skeletonOverlay: controller.activeSkeletonOverlay,
                            onHoverViewPoint: { pt, size in
                                controller.updateCursorHover(viewPoint: pt, viewSize: size)
                            },
                            onZoomAround: { pt, size, factor in
                                controller.zoomCanvasAround(cursorInView: pt, viewSize: size, factor: factor)
                            },
                            onPanBy: { delta, size in
                                controller.panCanvasBy(delta: delta, viewSize: size)
                            },
                            onResetViewport: {
                                controller.resetViewport()
                            },
                            onRegionDragUpdate: { startPt, curPt, size in
                                controller.updateRegionSelectionDrag(startViewPt: startPt, currentViewPt: curPt, viewSize: size)
                            },
                            onSingleClickPoint: { pt, size in
                                controller.handleCanvasSingleClick(viewPoint: pt, viewSize: size)
                            }
                        )
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                } else {
                    VStack(spacing: 12) {
                        Image(systemName: controller.lockBlocked ? "lock.fill" : "video.slash")
                            .font(.system(size: 36))
                            .foregroundStyle(.secondary)
                        Text(controller.statusText)
                            .font(.headline)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, 24)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }

                // Canvas Overlays (Top HUD, Drop Warning Banner, Health Panel, Bottom Readouts, Help Sheet)
                VStack(alignment: .leading, spacing: 8) {
                    topHudRow

                    if controller.healthSnapshot.dropWarningActive {
                        HStack(spacing: 8) {
                            Image(systemName: "exclamationmark.triangle.fill")
                            Text(DropBannerEvaluator.warningBannerText)
                                .fontWeight(.semibold)
                        }
                        .font(.system(size: 12))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .background(Color.orange.opacity(0.92), in: RoundedRectangle(cornerRadius: 8))
                    }

                    if let notice = controller.shortcutNoticeBanner {
                        HStack(spacing: 8) {
                            Text(notice)
                            Button {
                                controller.shortcutNoticeBanner = nil
                            } label: {
                                Image(systemName: "xmark.circle.fill")
                            }
                            .buttonStyle(.plain)
                        }
                        .font(.caption)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
                    }

                    if controller.showHealthOverlay {
                        healthOverlayCard
                    }

                    Spacer()

                    if controller.isReplayingRecording {
                        replayTransportBar
                    }

                    // Bottom Readout & Inspection Cards
                    HStack(alignment: .bottom, spacing: 10) {
                        if let readout = controller.cursorReadout {
                            cursorReadoutCard(readout)
                        }

                        Spacer()

                        VStack(alignment: .trailing, spacing: 8) {
                            if let ruler = controller.rulerState {
                                rulerReadoutCard(ruler)
                            }
                            if let stats = controller.regionStats {
                                regionStatsCard(stats)
                            }
                        }
                    }
                }
                .padding(12)

                if controller.showShortcutsHelp {
                    shortcutsHelpOverlay
                }
            }
            .frame(minWidth: 640, minHeight: 500)

            // Inspector Sidebar
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    if let err = controller.errorBanner {
                        Text(err)
                            .font(.caption)
                            .foregroundStyle(.white)
                            .padding(10)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.red.opacity(0.85), in: RoundedRectangle(cornerRadius: 8))
                    }

                    // View Mode, Orientation & Pipeline
                    GroupBox("View Mode & Pipeline") {
                        VStack(alignment: .leading, spacing: 10) {
                            Picker("View Mode", selection: $controller.selectedMode) {
                                ForEach(ViewerModeOption.allCases) { mode in
                                    Text("\(mode.rawValue + 1). \(mode.title)").tag(mode)
                                }
                            }

                            HStack(spacing: 4) {
                                modeQuickButton(.ir, label: "1 IR")
                                modeQuickButton(.color, label: "2 RGB")
                                modeQuickButton(.depth, label: "3 Depth")
                                modeQuickButton(.sideBySide, label: "4 SBS")
                                modeQuickButton(.pointCloud3D, label: "5 3D")
                            }

                            if controller.selectedMode == .sideBySide {
                                Picker("Side-by-Side Pair", selection: $controller.selectedSbsLayout) {
                                    ForEach(SbsLayoutOption.allCases) { layout in
                                        Text(layout.title).tag(layout)
                                    }
                                }
                            }

                            Picker("Depth Pipeline", selection: $controller.selectedPipeline) {
                                ForEach(PipelineOption.allCases) { pipe in
                                    Text(pipe.title).tag(pipe)
                                }
                            }

                            HStack(spacing: 12) {
                                Toggle("Mirror (M)", isOn: $controller.mirrorHorizontal)
                                    .font(.caption)
                                Toggle("Rotate 180° (R)", isOn: $controller.rotate180)
                                    .font(.caption)
                            }

                            Text("Pipeline never falls back silently; errors halt loudly.")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        .padding(4)
                    }

                    // Phase 3.1: Metal 3D Point Cloud Controls (Mode 5)
                    if controller.selectedMode == .pointCloud3D {
                        GroupBox("3D Point Cloud (Metal)") {
                            VStack(alignment: .leading, spacing: 10) {
                                Picker("Point Colour", selection: $controller.pointCloudColorSource) {
                                    ForEach(PointCloudColorSourceOption.allCases) { src in
                                        Text(src.title).tag(src)
                                    }
                                }
                                .pickerStyle(.segmented)
                                .disabled(controller.isReplayingRecording && controller.pointCloudColorSource == .registeredColor)

                                VStack(alignment: .leading, spacing: 4) {
                                    HStack {
                                        Text("Point Size")
                                        Spacer()
                                        Text(String(format: "%.1f px", controller.pointCloudSize))
                                            .monospacedDigit()
                                    }
                                    Slider(value: $controller.pointCloudSize, in: 1.0...10.0, step: 0.5)
                                }

                                HStack {
                                    Button("Reset 3D View") {
                                        controller.reset3DCamera()
                                    }
                                    .buttonStyle(.bordered)
                                    .controlSize(.small)

                                    Spacer()

                                    Text(String(format: "Dist %.2f m", controller.pointCloudCamera.distanceMeters))
                                        .font(.caption.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                }

                                Text("Drag to orbit · Scroll to zoom · Shift-drag to pan · Double-click to reset.")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            .padding(4)
                        }
                    }

                    // Phase 3.3: Depth Cut-Off (Near/Far mm + Track Me 0.5–2.0 m)
                    GroupBox("Depth Cut-Off (All Modes + 3D)") {
                        VStack(alignment: .leading, spacing: 9) {
                            HStack {
                                Toggle("Enable Cut-Off Planes", isOn: $controller.cutoffEnabled)
                                    .font(.caption.weight(.medium))
                                Spacer()
                                Button("Track me (0.5–2.0 m)") {
                                    controller.applyTrackMeCutoffPreset()
                                }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.mini)
                            }

                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text("Near Cut")
                                    Spacer()
                                    Text(String(format: "%.0f mm (%.2f m)", controller.cutoffNearMm, controller.cutoffNearMm / 1000.0))
                                        .monospacedDigit()
                                }
                                Slider(value: $controller.cutoffNearMm, in: 200...5000, step: 50)
                            }

                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text("Far Cut")
                                    Spacer()
                                    Text(String(format: "%.0f mm (%.2f m)", controller.cutoffFarMm, controller.cutoffFarMm / 1000.0))
                                        .monospacedDigit()
                                }
                                Slider(value: $controller.cutoffFarMm, in: 400...8000, step: 50)
                            }

                            Text("Everything outside [Near, Far] goes pure black in every mode including 3D.")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        .font(.caption)
                        .padding(4)
                    }

                    // Phase 3.2: Live-Only Depth / Registered Colour Blend (0% – 100%)
                    if !controller.isReplayingRecording && controller.selectedMode != .pointCloud3D {
                        GroupBox("Depth / Colour Blend (Live Only)") {
                            VStack(alignment: .leading, spacing: 8) {
                                Toggle("Blend Depth Colours Over Registered Colour", isOn: $controller.depthColorBlendEnabled)
                                    .font(.caption.weight(.medium))

                                VStack(alignment: .leading, spacing: 4) {
                                    HStack {
                                        Text("Depth Fade")
                                        Spacer()
                                        Text(String(format: "%.0f%%", controller.depthColorBlendPercent))
                                            .monospacedDigit()
                                    }
                                    Slider(
                                        value: Binding(
                                            get: { controller.depthColorBlendPercent },
                                            set: { newVal in
                                                controller.depthColorBlendPercent = newVal
                                                if !controller.depthColorBlendEnabled && newVal > 0 {
                                                    controller.depthColorBlendEnabled = true
                                                }
                                            }
                                        ),
                                        in: 0...100,
                                        step: 1
                                    )
                                }

                                Text("0% = pure registered colour · 100% = depth colormap. Live in-memory only; recording is disabled while active.")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            .font(.caption)
                            .padding(4)
                        }
                    }

                    // Phase 4.1: On-Device Vision Body & Hand Skeleton Overlay
                    if !controller.isReplayingRecording {
                        GroupBox("Body & Hand Skeleton (Vision)") {
                            VStack(alignment: .leading, spacing: 8) {
                                Toggle("Show Body & Hand Skeleton Overlay", isOn: $controller.skeletonOverlayEnabled)
                                    .font(.caption.weight(.medium))

                                VStack(alignment: .leading, spacing: 4) {
                                    HStack {
                                        Text("Confidence Threshold")
                                        Spacer()
                                        Text(String(format: "%.2f", controller.skeletonConfidenceThreshold))
                                            .monospacedDigit()
                                    }
                                    Slider(value: $controller.skeletonConfidenceThreshold, in: 0.05...0.95, step: 0.05)
                                }

                                if controller.skeletonOverlayEnabled {
                                    Text("Detected: \(controller.detectedBodiesCount) bod\(controller.detectedBodiesCount == 1 ? "y" : "ies") · \(controller.detectedHandsCount) hand\(controller.detectedHandsCount == 1 ? "" : "s")")
                                        .font(.caption2.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                }

                                Text("On-device Apple Vision from live colour, mapped onto Depth, IR & 3D via libfreenect2 registration. Nothing is stored and no joint data leaves this Mac.")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            .font(.caption)
                            .padding(4)
                        }
                    }

                    // Phase 4.2: Second-Display / Projector Clean Full-Screen Output
                    GroupBox("Second-Display / Projector Output") {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack(spacing: 6) {
                                Picker(
                                    "Display",
                                    selection: Binding(
                                        get: { controller.selectedSecondDisplayID ?? 0 },
                                        set: { newID in
                                            controller.selectedSecondDisplayID = newID
                                            if controller.isSecondDisplayActive {
                                                _ = controller.startSecondDisplayOutput()
                                            }
                                        }
                                    )
                                ) {
                                    ForEach(controller.availableDisplays) { disp in
                                        Text(disp.title).tag(disp.displayID)
                                    }
                                }
                                .labelsHidden()

                                Button {
                                    controller.refreshAvailableDisplays()
                                } label: {
                                    Image(systemName: "arrow.clockwise")
                                }
                                .buttonStyle(.borderless)
                                .controlSize(.small)
                                .help("Refresh connected displays and projectors")
                            }

                            HStack(spacing: 8) {
                                Button(controller.isSecondDisplayActive ? "Stop Full-Screen Output" : "Show Full Screen on Display") {
                                    if controller.isSecondDisplayActive {
                                        controller.stopSecondDisplayOutput()
                                    } else {
                                        _ = controller.startSecondDisplayOutput()
                                    }
                                }
                                .buttonStyle(.borderedProminent)
                                .tint(controller.isSecondDisplayActive ? .red : .indigo)
                                .controlSize(.small)
                                .disabled(controller.availableDisplays.isEmpty)

                                Button(controller.isSecondDisplayBlackedOut ? "Unblank (B)" : "Black Out (B)") {
                                    controller.toggleSecondDisplayBlackout()
                                }
                                .buttonStyle(.bordered)
                                .tint(controller.isSecondDisplayBlackedOut ? .orange : nil)
                                .controlSize(.small)
                            }

                            Text("Shows any active view mode full screen with no interface on top. Press B at any time for instant black-out; main window keeps its controls.")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        .font(.caption)
                        .padding(4)
                    }

                    // Presets (Phase 1.6)
                    GroupBox("Presets") {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Built-In")
                                .font(.caption2.weight(.semibold))
                                .foregroundStyle(.secondary)

                            HStack(spacing: 6) {
                                ForEach(ViewerPreset.builtInPresets) { preset in
                                    Button(preset.name) {
                                        controller.applyPreset(preset)
                                    }
                                    .buttonStyle(.bordered)
                                    .tint(controller.activePresetName == preset.name ? .accentColor : nil)
                                    .controlSize(.small)
                                }
                            }

                            let customList = controller.presets.filter { !$0.isBuiltIn }
                            if !customList.isEmpty {
                                Divider()
                                Text("Saved Presets")
                                    .font(.caption2.weight(.semibold))
                                    .foregroundStyle(.secondary)

                                ForEach(customList) { preset in
                                    HStack {
                                        Button(preset.name) {
                                            controller.applyPreset(preset)
                                        }
                                        .buttonStyle(.bordered)
                                        .controlSize(.small)

                                        Spacer()

                                        Button(role: .destructive) {
                                            controller.deleteNamedPreset(preset.name)
                                        } label: {
                                            Image(systemName: "trash")
                                        }
                                        .buttonStyle(.borderless)
                                        .help("Delete preset \(preset.name)")
                                    }
                                }
                            }

                            Divider()
                            HStack(spacing: 6) {
                                TextField("Preset name…", text: $newPresetName)
                                    .textFieldStyle(.roundedBorder)
                                    .controlSize(.small)
                                Button("Save") {
                                    if controller.saveNamedPreset(newPresetName) {
                                        newPresetName = ""
                                    }
                                }
                                .controlSize(.small)
                                .disabled(newPresetName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                            }
                            Text("Saves mode, pipeline, IR mapping, colour map, distance window & exposure. Last-used settings restore automatically on launch.")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        .padding(4)
                    }

                    // Source & Device
                    GroupBox("Source & Device") {
                        VStack(alignment: .leading, spacing: 10) {
                            Picker("Input", selection: Binding(
                                get: { controller.useSyntheticSource },
                                set: { controller.toggleSource(synthetic: $0) }
                            )) {
                                Text("Kinect v2 (USB)").tag(false)
                                Text("Synthetic Test").tag(true)
                            }
                            .pickerStyle(.segmented)

                            if controller.useSyntheticSource {
                                Toggle("Synthetic Clock Barcode Overlay", isOn: $controller.syntheticClockOverlay)
                                    .font(.caption)
                            }

                            HStack {
                                Button(controller.isRunning ? "Stop" : "Start") {
                                    if controller.isRunning {
                                        controller.stopStream()
                                    } else {
                                        controller.startStream()
                                    }
                                }
                                .buttonStyle(.borderedProminent)
                                .tint(controller.isRunning ? .red : .accentColor)

                                Button(controller.isFrozen ? "Unfreeze (Space)" : "Freeze (Space)") {
                                    controller.toggleFreeze()
                                }
                                .controlSize(.regular)
                                .disabled(controller.previewImage == nil && !controller.isFrozen)

                                Spacer()

                                Text(controller.usbPresent ? "USB 3 Ready" : "USB Disconnected")
                                    .font(.caption)
                                    .foregroundStyle(controller.usbPresent ? .green : .secondary)
                            }

                            LabeledContent("Serial", value: controller.serialNumber)
                                .font(.caption.monospaced())
                            LabeledContent("USB Port", value: controller.healthSnapshot.usbLocation)
                                .font(.caption.monospaced())
                            LabeledContent("Output Size", value: controller.frameDimensions)
                                .font(.caption.monospaced())
                        }
                        .padding(4)
                    }

                    // Host-Only Recording (Depth & IR Only)
                    GroupBox("Host-Only Recording") {
                        VStack(alignment: .leading, spacing: 9) {
                            if controller.canRecordCurrentMode {
                                Picker("16-bit Format", selection: $controller.recordingLosslessOption) {
                                    ForEach(Lossless16ContainerOption.allCases) { opt in
                                        Text(opt.title).tag(opt)
                                    }
                                }
                                .disabled(controller.isRecording)
                                .font(.caption)
                            } else {
                                Text(HostRecorderError.colourLiveOnlyTooltip)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }

                            HStack {
                                // Wrap in a non-disabled container carrying `.help(...)` as well so AppKit
                                // always displays the tooltip even while the Record button itself is disabled.
                                ZStack {
                                    Button(controller.isRecording ? "Stop Recording" : "Start Recording") {
                                        controller.requestToggleRecordingWithReminder()
                                    }
                                    .buttonStyle(.borderedProminent)
                                    .tint(controller.isRecording ? .red : .orange)
                                    .disabled((!controller.isRunning || !controller.canRecordCurrentMode || controller.isReplayingRecording) && !controller.isRecording)
                                    .help(controller.recordControlTooltip)
                                }
                                .help(controller.recordControlTooltip)

                                Spacer()

                                Button("Open Folder") {
                                    controller.revealRecordingsFolder()
                                }
                                .controlSize(.small)
                            }

                            if controller.isRecording || controller.recordingFrames > 0 {
                                Divider()
                                LabeledContent("Active Codec", value: controller.recordingFormatLabel)
                                    .font(.caption2)
                                LabeledContent("Frames / Elapsed", value: String(format: "%llu (%.1f s)", controller.recordingFrames, controller.recordingElapsedSec))
                                    .font(.caption.monospacedDigit())
                                LabeledContent("Size on Disk", value: String(format: "%.1f MB", controller.recordingMegabytes))
                                    .font(.caption.monospacedDigit())
                                LabeledContent("Write Rate", value: String(format: "%.1f MB / min", controller.recordingMBPerMinute))
                                    .font(.caption.monospacedDigit().weight(.semibold))
                            }

                            Label(
                                controller.recordingExclusionsOK
                                    ? "~/kinect-viewer-recordings/ (.nosync + TM + Spotlight excluded)"
                                    : "Setting up folder exclusions…",
                                systemImage: controller.recordingExclusionsOK ? "externaldrive.badge.checkmark" : "exclamationmark.triangle"
                            )
                            .font(.caption2)
                            .foregroundStyle(controller.recordingExclusionsOK ? Color.secondary : Color.orange)
                        }
                        .padding(4)
                    }

                    // Phase 2 In-App Recordings List (Metadata Only — Zero Thumbnails or Previews)
                    GroupBox("Recordings (7-Day Auto-Delete)") {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Text("\(controller.recordedSessions.count) session\(controller.recordedSessions.count == 1 ? "" : "s")")
                                    .font(.caption2.weight(.semibold))
                                    .foregroundStyle(.secondary)
                                Spacer()
                                Button {
                                    controller.runExpiredRecordingsSweep()
                                } label: {
                                    Image(systemName: "arrow.clockwise")
                                }
                                .buttonStyle(.borderless)
                                .controlSize(.small)
                                .help("Refresh recordings list and purge expired (>7d) sessions")
                            }

                            if controller.recordedSessions.isEmpty {
                                Text("No Depth or IR recordings in folder.")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            } else {
                                ForEach(controller.recordedSessions) { session in
                                    recordingSessionRow(session)
                                    if session.id != controller.recordedSessions.last?.id {
                                        Divider()
                                    }
                                }
                            }
                        }
                        .padding(4)
                    }

                    // iPhone Live Stream & Pairing (Phase 3)
                    GroupBox("iPhone Live Stream (mTLS 1.3)") {
                        VStack(alignment: .leading, spacing: 9) {
                            HStack {
                                Circle()
                                    .fill(controller.isPhoneConnected ? Color.green : (controller.isPhonePaired ? Color.blue : Color.secondary))
                                    .frame(width: 8, height: 8)
                                Text(controller.isPhoneConnected ? "Streaming Encrypted to iPhone" : (controller.isPhonePaired ? "Paired (Waiting for iPhone)" : "Unpaired (mDNS Hidden)"))
                                    .font(.caption.weight(.semibold))
                            }

                            if let keyErr = controller.streamingSetupError {
                                Text(keyErr)
                                    .font(.caption)
                                    .foregroundStyle(.orange)
                                    .fixedSize(horizontal: false, vertical: true)
                                HStack(spacing: 8) {
                                    Button("Retry") {
                                        controller.retryStreamingServerSetup()
                                    }
                                    .controlSize(.small)

                                    if controller.canOfferResetPairing {
                                        Button("Reset pairing (your iPhone will need to pair again)", role: .destructive) {
                                            controller.requestResetPairing()
                                        }
                                        .controlSize(.small)
                                    }
                                }
                            }

                            if controller.isPhonePaired {
                                LabeledContent("Pinned Key", value: controller.pinnedPhoneFingerprint)
                                    .font(.caption.monospaced())
                            }

                            if controller.isPhoneConnected {
                                LabeledContent("Stream Bitrate", value: String(format: "%.2f Mbps", controller.streamBitrateMbps))
                                    .font(.caption.monospacedDigit())
                                LabeledContent("Phone Latency", value: String(format: "%.1f ms", controller.phoneDisplayLatencyMs))
                                    .font(.caption.monospacedDigit())
                                if controller.phoneBarcodeLatencyMs > 0 {
                                    LabeledContent("Barcode Latency", value: String(format: "%.1f ms", controller.phoneBarcodeLatencyMs))
                                        .font(.caption.monospacedDigit())
                                }
                            }

                            if controller.rejectedTlsHandshakes > 0 {
                                LabeledContent("Blocked Unpaired TLS", value: "\(controller.rejectedTlsHandshakes)")
                                    .font(.caption.monospacedDigit())
                                    .foregroundStyle(.orange)
                            }

                            Toggle("Allow iPhone to Turn Kinect On/Off", isOn: $controller.allowPhoneSensorPowerControl)
                                .font(.caption)

                            Toggle("Allow iPhone Remote Controls", isOn: $controller.allowPhoneModeSwitch)
                                .font(.caption)

                            Toggle("Turn the Kinect off when the iPhone disconnects", isOn: $controller.turnOffKinectOnPhoneDisconnect)
                                .font(.caption)

                            HStack {
                                Button(controller.isPhonePaired ? "Re-Pair iPhone…" : "Pair iPhone…") {
                                    controller.openPairingWindow()
                                }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                                .disabled(controller.streamingSetupError != nil)

                                if controller.isPhonePaired {
                                    Button("Unpair", role: .destructive) {
                                        controller.unpairPhone()
                                    }
                                    .controlSize(.small)
                                }
                            }
                        }
                        .padding(4)
                    }

                    // Mode-Specific Controls: IR
                    if controller.selectedMode == .ir ||
                        (controller.selectedMode == .pointCloud3D && controller.pointCloudColorSource == .ir) ||
                        (controller.selectedMode == .sideBySide && controller.selectedSbsLayout != .colorDepth) {
                        GroupBox("IR 16-bit → 8-bit Mapping") {
                            VStack(alignment: .leading, spacing: 10) {
                                Toggle("Percentile Auto-Scale (P1–P99.5)", isOn: $controller.irAutoScale)

                                VStack(alignment: .leading, spacing: 4) {
                                    HStack {
                                        Text("Gamma")
                                        Spacer()
                                        Text(String(format: "%.2f", controller.irGamma)).monospacedDigit()
                                    }
                                    Slider(value: $controller.irGamma, in: 0.20...1.40)
                                }

                                if controller.irAutoScale {
                                    VStack(alignment: .leading, spacing: 4) {
                                        HStack {
                                            Text("Temporal Smoothing (EMA α)")
                                            Spacer()
                                            Text(String(format: "%.2f", controller.irSmoothingAlpha)).monospacedDigit()
                                        }
                                        Slider(value: $controller.irSmoothingAlpha, in: 0.02...1.00)
                                    }
                                    Text(String(format: "Scale: %.0f – %.0f / 65535", controller.smoothedLow, controller.smoothedHigh))
                                        .font(.caption.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                } else {
                                    Slider(value: $controller.irManualMin, in: 0...30000) {
                                        Text("Black")
                                    }
                                    Slider(value: $controller.irManualMax, in: 500...65535) {
                                        Text("White")
                                    }
                                }

                                Button("Reset IR Defaults") {
                                    controller.resetIrMappingDefaults()
                                }
                                .controlSize(.small)
                            }
                            .padding(4)
                        }
                    }

                    // Mode-Specific Controls: Depth
                    if controller.selectedMode == .depth ||
                        controller.depthColorBlendEnabled ||
                        (controller.selectedMode == .pointCloud3D && controller.pointCloudColorSource == .depth) ||
                        (controller.selectedMode == .sideBySide && controller.selectedSbsLayout != .colorIr) {
                        GroupBox("Depth False Colour") {
                            VStack(alignment: .leading, spacing: 10) {
                                Picker("Colormap", selection: $controller.depthColormap) {
                                    ForEach(ColormapOption.allCases) { cmap in
                                        Text(cmap.title).tag(cmap)
                                    }
                                }

                                VStack(alignment: .leading, spacing: 4) {
                                    HStack {
                                        Text("Min Distance")
                                        Spacer()
                                        Text(String(format: "%.0f mm (%.2f m)", controller.depthMinMm, controller.depthMinMm / 1000.0))
                                            .monospacedDigit()
                                    }
                                    Slider(value: $controller.depthMinMm, in: 300...3000, step: 50)
                                }

                                VStack(alignment: .leading, spacing: 4) {
                                    HStack {
                                        Text("Max Distance")
                                        Spacer()
                                        Text(String(format: "%.0f mm (%.2f m)", controller.depthMaxMm, controller.depthMaxMm / 1000.0))
                                            .monospacedDigit()
                                    }
                                    Slider(value: $controller.depthMaxMm, in: 1000...8000, step: 50)
                                }

                                HStack(spacing: 6) {
                                    Button("Desk 0.4–2.5m") {
                                        controller.applyDepthPreset(minMm: 400, maxMm: 2500)
                                    }
                                    Button("Spec 0.5–4.5m") {
                                        controller.applyDepthPreset(minMm: 500, maxMm: 4500)
                                    }
                                    Button("Room 0.5–8m") {
                                        controller.applyDepthPreset(minMm: 500, maxMm: 8000)
                                    }
                                }
                                .controlSize(.mini)

                                Toggle("Near = Warm / Bright", isOn: $controller.depthInvertNearWarm)
                                Text("Invalid (≤0 / NaN / IR=65535) gated to black: \(controller.invalidPixels) px")
                                    .font(.caption2.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                            .padding(4)
                        }
                    }

                    // Mode-Specific Controls: Colour
                    if controller.currentModeNeedsColor {
                        GroupBox("Colour Exposure & Gain") {
                            VStack(alignment: .leading, spacing: 10) {
                                Picker("Mode", selection: $controller.exposureMode) {
                                    ForEach(ExposureModeOption.allCases) { em in
                                        Text(em.title).tag(em)
                                    }
                                }

                                switch controller.exposureMode {
                                case .auto:
                                    VStack(alignment: .leading, spacing: 4) {
                                        HStack {
                                            Text("Exposure Comp")
                                            Spacer()
                                            Text(String(format: "%+.2f EV", controller.autoExposureComp)).monospacedDigit()
                                        }
                                        Slider(value: $controller.autoExposureComp, in: -2.0...2.0, step: 0.1)
                                    }
                                case .semiAuto:
                                    VStack(alignment: .leading, spacing: 4) {
                                        HStack {
                                            Text("Pseudo-Exposure")
                                            Spacer()
                                            Text(String(format: "%.1f ms", controller.semiAutoPseudoMs)).monospacedDigit()
                                        }
                                        Slider(value: $controller.semiAutoPseudoMs, in: 4.0...66.0, step: 0.5)
                                        Text("10–16.6 ms snaps to flicker-free mains period (8.33 ms @ 60 Hz).")
                                            .font(.caption2)
                                            .foregroundStyle(.secondary)
                                    }
                                case .manual:
                                    VStack(alignment: .leading, spacing: 4) {
                                        HStack {
                                            Text("Shutter Time")
                                            Spacer()
                                            Text(String(format: "%.1f ms", controller.manualIntegrationMs)).monospacedDigit()
                                        }
                                        Slider(value: $controller.manualIntegrationMs, in: 1.0...66.0, step: 0.5)

                                        HStack {
                                            Text("Analog Gain")
                                            Spacer()
                                            Text(String(format: "%.2f×", controller.manualAnalogGain)).monospacedDigit()
                                        }
                                        Slider(value: $controller.manualAnalogGain, in: 1.0...4.0, step: 0.05)
                                    }
                                }

                                Text(String(
                                    format: "Footer exp=%.1f ms gain=%.2f | RGB %.1f fps",
                                    controller.colorExposureFooter,
                                    controller.colorGainFooter,
                                    controller.colorFps
                                ))
                                .font(.caption2.monospacedDigit())
                                .foregroundStyle(.secondary)
                            }
                            .padding(4)
                        }
                    }

                    // Diagnostics
                    GroupBox("Timing & Signal Diagnostics") {
                        VStack(alignment: .leading, spacing: 5) {
                            Toggle("Crisp Nearest-Pixel Upscale", isOn: $controller.crispPixelScaling)
                            Divider()
                            LabeledContent("IR / Depth / RGB Frames", value: "\(controller.irFrameCount) / \(controller.depthFrameCount) / \(controller.colorFrameCount)")
                            LabeledContent("Frame Interval (p50/p95)", value: String(format: "%.1f / %.1f ms", controller.healthSnapshot.interFrameP50Ms, controller.healthSnapshot.interFrameP95Ms))
                            LabeledContent("Depth Processing", value: String(format: "%.2f ms", controller.healthSnapshot.depthProcessingMs))
                            LabeledContent("Raw Min / Mean / Max", value: String(format: "%.0f / %.0f / %.0f", controller.rawMin, controller.rawMean, controller.rawMax))
                            LabeledContent("IR Sat / Depth Invalid", value: "\(controller.saturatedPixels) / \(controller.invalidPixels)")
                        }
                        .font(.caption.monospacedDigit())
                        .padding(4)
                    }
                }
                .padding(14)
            }
            .frame(minWidth: 320, idealWidth: 360, maxWidth: 400)
        }
    }

    // MARK: - Subviews

    private func modeQuickButton(_ mode: ViewerModeOption, label: String) -> some View {
        Button {
            controller.selectedMode = mode
        } label: {
            Text(label)
                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 4)
                .background(
                    controller.selectedMode == mode ? Color.accentColor.opacity(0.85) : Color.secondary.opacity(0.15),
                    in: RoundedRectangle(cornerRadius: 5)
                )
        }
        .buttonStyle(.plain)
    }

    private func recordingSessionRow(_ session: RecordingSessionItem) -> some View {
        let isActive = controller.activeReplaySession?.id == session.id && controller.isReplayingRecording
        let countdown = session.countdownText(at: controller.nowProvider())
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Text(session.modeTitle)
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 1)
                    .background(session.isDepthMode ? Color.blue : Color.purple, in: Capsule())

                Text(session.formattedDate)
                    .font(.caption.weight(.medium))
                    .lineLimit(1)

                Spacer()

                Text(countdown)
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(.orange)
            }

            HStack(spacing: 8) {
                Text("\(session.formattedLength) · \(session.formattedSize)")
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)

                Spacer()

                if isActive {
                    Button("Stop") {
                        controller.exitPlayback(resumeLive: false)
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.mini)
                } else {
                    Button("Play") {
                        _ = controller.startPlayback(of: session)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.mini)
                }

                Button(role: .destructive) {
                    controller.deleteRecordingSession(session)
                } label: {
                    Image(systemName: "trash")
                }
                .buttonStyle(.borderless)
                .controlSize(.mini)
                .help("Delete recording and log removal to deletion-log.txt")
            }
        }
        .padding(.vertical, 2)
    }

    private var replayTransportBar: some View {
        let maxIdx = max(1, controller.replayTotalFrames - 1)
        return HStack(spacing: 10) {
            Text("REPLAY")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(.black)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(Color.mint, in: Capsule())

            Button {
                controller.togglePlaybackPause()
            } label: {
                Image(systemName: controller.isReplayPlaying ? "pause.fill" : "play.fill")
                    .frame(width: 18)
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .help(controller.isReplayPlaying ? "Pause replay" : "Play replay")

            Button {
                controller.stepPlayback(by: -1)
            } label: {
                Image(systemName: "backward.frame.fill")
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .disabled(controller.replayFrameIndex <= 0)
            .help("Step back 1 frame")

            Slider(
                value: Binding(
                    get: { Double(controller.replayFrameIndex) },
                    set: { controller.seekPlayback(toFrame: Int($0.rounded())) }
                ),
                in: 0...Double(maxIdx)
            )
            .frame(minWidth: 160)

            Button {
                controller.stepPlayback(by: 1)
            } label: {
                Image(systemName: "forward.frame.fill")
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .disabled(controller.replayFrameIndex >= controller.replayTotalFrames - 1)
            .help("Step forward 1 frame")

            Text("\(min(controller.replayTotalFrames, controller.replayFrameIndex + 1)) / \(controller.replayTotalFrames)")
                .font(.system(size: 11, design: .monospaced))
                .monospacedDigit()

            Button("Exit Replay") {
                controller.exitPlayback(resumeLive: false)
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
    }

    private var topHudRow: some View {
        HStack(spacing: 10) {
            HStack(spacing: 10) {
                statusDot
                Text(controller.selectedMode.title)
                    .fontWeight(.semibold)

                if controller.isFrozen {
                    Text("FROZEN")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundStyle(.black)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(Color.yellow, in: Capsule())
                }

                if controller.isReplayingRecording {
                    Text("REPLAY")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundStyle(.black)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(Color.mint, in: Capsule())
                }

                if controller.viewportTransform.zoomScale > 1.01 {
                    Button {
                        controller.resetViewport()
                    } label: {
                        Text(String(format: "%.1f×", controller.viewportTransform.zoomScale))
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Color.cyan.opacity(0.25), in: Capsule())
                    }
                    .buttonStyle(.plain)
                    .help("Double-click canvas or click here to reset zoom & pan")
                }

                if controller.selectedMode == .pointCloud3D && !controller.pointCloudCamera.isDefault {
                    Button {
                        controller.reset3DCamera()
                    } label: {
                        Text("Reset 3D")
                            .font(.system(size: 11, weight: .semibold, design: .monospaced))
                            .padding(.horizontal, 7)
                            .padding(.vertical, 2)
                            .background(Color.cyan.opacity(0.30), in: Capsule())
                    }
                    .buttonStyle(.plain)
                    .help("Reset 3D orbit, zoom & pan")
                }

                if controller.cutoffEnabled {
                    Text(String(format: "CUT %.1f–%.1fm", controller.cutoffNearMm / 1000.0, controller.cutoffFarMm / 1000.0))
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.orange)
                }

                if controller.depthColorBlendEnabled && controller.selectedMode != .pointCloud3D {
                    Text(String(format: "BLEND %.0f%%", controller.depthColorBlendPercent))
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.pink)
                }

                if controller.isSecondDisplayActive {
                    Text(controller.isSecondDisplayBlackedOut ? "OUT: BLACKOUT (B)" : "OUT: LIVE")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundStyle(controller.isSecondDisplayBlackedOut ? Color.orange : Color.indigo)
                }

                if controller.mirrorHorizontal {
                    Text("MIRROR")
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.cyan)
                }
                if controller.rotate180 {
                    Text("180°")
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundStyle(.cyan)
                }

                Divider().frame(height: 14)
                Text(String(format: "%.1f FPS", controller.currentFps))
                    .monospacedDigit()
                    .fontWeight(.semibold)
                Divider().frame(height: 14)
                Text("Pipe: \(controller.selectedPipeline.shortCode)")
                Divider().frame(height: 14)
                Text(String(format: "%.1f ms", controller.arrivalToDisplayMs))
                    .monospacedDigit()

                if controller.isPhoneConnected {
                    Divider().frame(height: 14)
                    Label(
                        String(format: "iPhone %.0f ms", controller.phoneDisplayLatencyMs),
                        systemImage: "iphone.radiowaves.left.and.right"
                    )
                    .foregroundStyle(.green)
                }

                if controller.isRecording {
                    Divider().frame(height: 14)
                    Label(
                        String(format: "REC %.0f MB/min", controller.recordingMBPerMinute),
                        systemImage: "record.circle.fill"
                    )
                    .foregroundStyle(.red)
                }
            }
            .font(.system(size: 12, design: .monospaced))
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))

            Spacer()

            // Interactive Tool Mode + Health + Help Pill
            HStack(spacing: 4) {
                ForEach(CanvasToolMode.allCases) { tool in
                    Button {
                        controller.canvasToolMode = tool
                    } label: {
                        Label(tool.rawValue, systemImage: tool.symbolName)
                            .font(.system(size: 11, weight: .medium))
                            .padding(.horizontal, 7)
                            .padding(.vertical, 4)
                            .background(
                                controller.canvasToolMode == tool ? Color.accentColor.opacity(0.85) : Color.clear,
                                in: RoundedRectangle(cornerRadius: 6)
                            )
                    }
                    .buttonStyle(.plain)
                    .help(tool.hint)
                }

                Divider().frame(height: 14)

                Button {
                    controller.showHealthOverlay.toggle()
                } label: {
                    Text("H")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 4)
                        .background(
                            controller.showHealthOverlay ? Color.green.opacity(0.75) : Color.clear,
                            in: RoundedRectangle(cornerRadius: 6)
                        )
                }
                .buttonStyle(.plain)
                .help("Toggle Health Display (H)")

                Button {
                    controller.showShortcutsHelp.toggle()
                } label: {
                    Text("?")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 4)
                        .background(
                            controller.showShortcutsHelp ? Color.accentColor.opacity(0.85) : Color.clear,
                            in: RoundedRectangle(cornerRadius: 6)
                        )
                }
                .buttonStyle(.plain)
                .help("Keyboard Shortcuts (?)")
            }
            .padding(4)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
        }
    }

    private var healthOverlayCard: some View {
        let h = controller.healthSnapshot
        return VStack(alignment: .leading, spacing: 5) {
            HStack {
                Label("Stream Health (H)", systemImage: "waveform.path.ecg")
                    .font(.caption.weight(.bold))
                Spacer()
                Button {
                    controller.showHealthOverlay = false
                } label: {
                    Image(systemName: "xmark")
                        .font(.caption2)
                }
                .buttonStyle(.plain)
            }
            Divider()
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 4) {
                GridRow {
                    Text("Stream FPS (IR / Depth / RGB)")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%.1f / %.1f / %.1f fps", h.irFps, h.depthFps, h.colorFps))
                }
                GridRow {
                    Text("Drops / min (IR / Depth / RGB)")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%.0f / %.0f / %.0f", h.irDropsPerMin, h.depthDropsPerMin, h.colorDropsPerMin))
                        .foregroundStyle((h.irDropsPerMin + h.depthDropsPerMin) > 0 ? Color.orange : Color.primary)
                }
                GridRow {
                    Text("10 s Drop Share (IR / Depth)")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%.1f%% / %.1f%%", h.irDropRatio10s * 100.0, h.depthDropRatio10s * 100.0))
                        .foregroundStyle(h.dropWarningActive ? Color.red : Color.primary)
                }
                GridRow {
                    Text("Frame-to-Frame (p50 / p95)")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%.2f / %.2f ms", h.interFrameP50Ms, h.interFrameP95Ms))
                }
                GridRow {
                    Text("Depth Processing Time")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%.2f ms", h.depthProcessingMs))
                }
                GridRow {
                    Text("Driver Errors")
                        .foregroundStyle(.secondary)
                    Text(h.lastDriverError.isEmpty ? "\(h.driverErrors)" : "\(h.driverErrors) (\(h.lastDriverError))")
                        .foregroundStyle(h.driverErrors > 0 ? Color.orange : Color.primary)
                        .lineLimit(1)
                }
                GridRow {
                    Text("USB Location")
                        .foregroundStyle(.secondary)
                    Text(h.usbLocation)
                }
            }
            .font(.system(size: 11, design: .monospaced))
        }
        .padding(10)
        .frame(maxWidth: 460)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
    }

    private func cursorReadoutCard(_ readout: CursorReadout) -> some View {
        HStack(spacing: 12) {
            Label(readout.coordText, systemImage: "scope")
            Divider().frame(height: 13)
            Text("Depth: \(readout.depthText)")
            Divider().frame(height: 13)
            Text("IR: \(readout.irText)")
        }
        .font(.system(size: 12, weight: .medium, design: .monospaced))
        .padding(.horizontal, 11)
        .padding(.vertical, 6)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private func rulerReadoutCard(_ ruler: RulerMeasurementState) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "ruler")
                .foregroundStyle(.yellow)
            VStack(alignment: .leading, spacing: 2) {
                Text("3D Ruler: \(ruler.formattedLabel)")
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundStyle(ruler.errorMessage != nil ? Color.orange : Color.primary)
                if let b = ruler.pointB {
                    Text("A(\(ruler.pointA.sensorX),\(ruler.pointA.sensorY)) → B(\(b.sensorX),\(b.sensorY)) · libfreenect2 IR intrinsics")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                } else {
                    Text("A(\(ruler.pointA.sensorX),\(ruler.pointA.sensorY)) · Click second point on depth/IR view")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
            }
            Button {
                controller.rulerState = nil
            } label: {
                Image(systemName: "xmark.circle.fill")
            }
            .buttonStyle(.plain)
            .help("Clear Ruler (Esc)")
        }
        .padding(.horizontal, 11)
        .padding(.vertical, 7)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private func regionStatsCard(_ stats: RegionDepthStats) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Label(
                    "Region (\(stats.sensorMinX),\(stats.sensorMinY))–(\(stats.sensorMaxX),\(stats.sensorMaxY))",
                    systemImage: "rectangle.dashed"
                )
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundStyle(.cyan)
                Spacer()
                Button {
                    controller.regionSelectionBox = nil
                    controller.regionStats = nil
                } label: {
                    Image(systemName: "xmark.circle.fill")
                }
                .buttonStyle(.plain)
                .help("Clear Region (Esc)")
            }

            if let mean = stats.meanMm,
               let median = stats.medianMm,
               let sd = stats.stdDevMm,
               let minV = stats.minMm,
               let maxV = stats.maxMm {
                Text(String(format: "Mean %.1f mm · Median %.1f mm · SD %.1f mm", mean, median, sd))
                    .font(.system(size: 11, design: .monospaced))
                Text(String(
                    format: "Min %.0f mm · Max %.0f mm · Invalid %.1f%% (%d/%d)",
                    minV,
                    maxV,
                    stats.invalidShare * 100.0,
                    stats.invalidPixels,
                    stats.totalPixels
                ))
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(.secondary)
            } else {
                Text(String(
                    format: "All pixels invalid (0) · Invalid 100.0%% (%d/%d)",
                    stats.invalidPixels,
                    stats.totalPixels
                ))
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(.orange)
            }
        }
        .padding(.horizontal, 11)
        .padding(.vertical, 7)
        .frame(maxWidth: 420)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var shortcutsHelpOverlay: some View {
        ZStack {
            Color.black.opacity(0.55)
                .ignoresSafeArea()
                .onTapGesture {
                    controller.showShortcutsHelp = false
                }

            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text("Keyboard & Mouse Shortcuts")
                        .font(.headline)
                    Spacer()
                    Button("Close (Esc)") {
                        controller.showShortcutsHelp = false
                    }
                    .controlSize(.small)
                }
                Divider()
                Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 6) {
                    shortcutRow("1 – 5", "Switch view mode (1 IR, 2 Colour, 3 Depth, 4 Side-by-Side, 5 3D)")
                    shortcutRow("3D Mode (5)", "Drag = Orbit · Scroll = Zoom · Shift-drag = Pan · Double-click = Reset")
                    shortcutRow("Space", "Freeze / unfreeze displayed frame in memory")
                    shortcutRow("Scroll / Pinch", "Zoom in or out around the cursor")
                    shortcutRow("Drag (zoomed) / Opt-drag", "Pan zoomed view")
                    shortcutRow("Double-click", "Reset zoom and pan")
                    shortcutRow("Drag box / Shift-drag", "Select region for mean, median, SD, min, max & invalid %")
                    shortcutRow("Click two points", "Measure 3D distance in mm from IR camera intrinsics")
                    shortcutRow("Esc", "Clear region selection, 3D ruler, or close help")
                    shortcutRow("B", "Instant black-out toggle for second-display / projector output")
                    shortcutRow("H", "Toggle stream health display")
                    shortcutRow("F", "Toggle full screen")
                    shortcutRow("M", "Mirror horizontally")
                    shortcutRow("R", "Rotate 180°")
                    shortcutRow("?", "Toggle this shortcuts help overlay")
                }
                .font(.system(size: 12))
            }
            .padding(20)
            .frame(width: 520)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
            .shadow(radius: 18)
        }
    }

    private func shortcutRow(_ key: String, _ desc: String) -> some View {
        GridRow {
            Text(key)
                .font(.system(size: 12, weight: .semibold, design: .monospaced))
                .padding(.horizontal, 7)
                .padding(.vertical, 2)
                .background(.quaternary, in: RoundedRectangle(cornerRadius: 5))
            Text(desc)
                .foregroundStyle(.secondary)
        }
    }

    private var statusDot: some View {
        Circle()
            .fill(controller.isRunning ? Color.green : (controller.lockBlocked ? Color.orange : Color.gray))
            .frame(width: 9, height: 9)
    }
}

struct HostPairingModalView: View {
    @ObservedObject var controller: HostController

    var body: some View {
        VStack(spacing: 16) {
            Text("Pair Your iPhone")
                .font(.title2.weight(.semibold))

            Text("Scan this one-time QR code in Kinect Pocket on your iPhone, or type the 8-digit code below. Only one paired phone is pinned at a time.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 380)

            if let qr = controller.pairingQRImage {
                Image(decorative: qr, scale: 1.0, orientation: .up)
                    .interpolation(.none)
                    .resizable()
                    .scaledToFit()
                    .frame(width: 220, height: 220)
                    .padding(12)
                    .background(Color.white, in: RoundedRectangle(cornerRadius: 12))
            }

            if let offer = controller.activePairingOffer {
                VStack(spacing: 4) {
                    Text("8-Digit Short Code")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(offer.shortCodeFormatted)
                        .font(.system(size: 28, weight: .bold, design: .monospaced))
                        .textSelection(.enabled)
                }
                .padding(.vertical, 6)
                .padding(.horizontal, 18)
                .background(.quaternary, in: RoundedRectangle(cornerRadius: 10))
            }

            Text("Expires in \(controller.pairingSecondsRemaining)s · Single-attempt protected")
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)

            Button("Cancel") {
                controller.cancelPairingWindow()
            }
            .keyboardShortcut(.cancelAction)
        }
        .padding(28)
        .frame(width: 440)
    }
}

struct PreRecordingReminderSheetView: View {
    @ObservedObject var controller: HostController

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 10) {
                Image(systemName: "exclamationmark.shield.fill")
                    .font(.title2)
                    .foregroundStyle(.orange)
                Text("Before Starting Recording")
                    .font(.headline)
            }

            Text(PreRecordingReminderPolicy.exactReminderText)
                .font(.body)
                .fixedSize(horizontal: false, vertical: true)

            Toggle("Don't remind me for 1 hour", isOn: $controller.recordingReminderDontAskOneHour)
                .font(.callout)

            HStack {
                Button("Start Recording") {
                    controller.confirmRecordingReminderAndStart()
                }
                .tint(.orange)

                Spacer()

                // Defaults to Cancel (Return/Enter activates Cancel) per Phase 2.4
                Button("Cancel") {
                    controller.cancelRecordingReminder()
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 440)
        .onExitCommand {
            controller.cancelRecordingReminder()
        }
        .background(
            Button("") {
                controller.cancelRecordingReminder()
            }
            .keyboardShortcut(.cancelAction)
            .opacity(0)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        )
    }
}
