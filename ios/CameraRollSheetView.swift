import AVFoundation
import SwiftUI
import UIKit

/// Hardware-protected `AVSampleBufferDisplayLayer` host for streamed camera-roll playback (`preventsCapture = true`, zero disk writes).
struct PlaybackSampleDisplayView: UIViewRepresentable {
    let bridge: SampleBufferDisplayBridge

    func makeUIView(context: Context) -> SampleBufferVideoLayerView {
        let view = SampleBufferVideoLayerView()
        view.backgroundColor = .black
        view.isOpaque = true
        bridge.attach(layer: view.sampleDisplayLayer)
        return view
    }

    func updateUIView(_ uiView: SampleBufferVideoLayerView, context: Context) {
        uiView.sampleDisplayLayer.preventsCapture = true
    }
}

struct CameraRollSheetView: View {
    @ObservedObject var viewModel: PocketViewModel
    @ObservedObject var roll: CameraRollController
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            // The grid stays mounted while the app is inactive so its scroll position and loaded thumbnails survive;
            // an opaque placeholder covers it instead (and every thumbnail cell also drops its image while hidden).
            ZStack {
                List {
                    if let activeClip = roll.activeClip {
                        Section {
                            streamedPlayerCard(for: activeClip)
                        } header: {
                            HStack {
                                Text("Playing Clip")
                                Spacer()
                                if let st = roll.playbackState {
                                    Text(st.state.displayLabel)
                                        .font(.caption2.weight(.semibold))
                                        .foregroundStyle(stateBadgeColor(st.state))
                                }
                            }
                        }
                    }

                    if roll.showOwnerRollControls {
                        ownerRollControlsSection
                    } else if let blockedUnavail = roll.rollUnavailableBrowseSentence {
                        Section {
                            Label(blockedUnavail, systemImage: "exclamationmark.circle")
                                .font(.subheadline)
                                .foregroundStyle(.orange)
                        }
                    }

                    daysAndClipsSections
                }
                .listStyle(.insetGrouped)
                .refreshable {
                    await roll.refreshDays(force: true)
                }
                .allowsHitTesting(!isHiddenForSnapshot)
                .accessibilityHidden(isHiddenForSnapshot)

                if isHiddenForSnapshot {
                    ZStack {
                        Color.black.ignoresSafeArea()
                        VStack(spacing: 10) {
                            Image(systemName: "eye.slash.fill")
                                .font(.title2)
                                .foregroundStyle(.secondary)
                            Text("Camera roll hidden while app is inactive.")
                                .font(.footnote.weight(.medium))
                                .foregroundStyle(.secondary)
                        }
                    }
                    .accessibilityIdentifier("cameraRollSnapshotPlaceholder")
                    // No fade: the cover must be fully opaque on the very frame the app goes inactive.
                    .transition(.identity)
                }
            }
            .navigationTitle("Camera Roll")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") {
                        roll.closePlayback(resumeLive: true)
                        dismiss()
                    }
                }
            }
            .onAppear {
                roll.loadInitialIfNeeded()
            }
            .alert(
                "Delete Clip?",
                isPresented: Binding(
                    get: { roll.clipPendingDelete != nil },
                    set: { if !$0 { roll.clipPendingDelete = nil } }
                ),
                presenting: roll.clipPendingDelete
            ) { _ in
                Button("Cancel", role: .cancel) {
                    roll.clipPendingDelete = nil
                }
                Button("Delete", role: .destructive) {
                    roll.confirmDeletePendingClip()
                }
            } message: { clip in
                Text("Delete the \(clip.trigger.displayLabel.lowercased()) clip at \(CameraRollController.formatClipStartTime(clip)) from the PC? This cannot be undone.")
            }
        }
    }

    private var isHiddenForSnapshot: Bool {
        viewModel.shouldHideRoomImagesForSnapshot || roll.isHiddenForSnapshot
    }

    // MARK: - Streamed Clip Player (`0x16`–`0x19`)

    private func streamedPlayerCard(for clip: RollClip) -> some View {
        let currentState = roll.playbackState?.state ?? .starting
        let duration = roll.effectivePlaybackDurationSec

        return VStack(alignment: .leading, spacing: 12) {
            ZStack {
                PlaybackSampleDisplayView(bridge: roll.playbackDisplayBridge)
                    .aspectRatio(
                        CGFloat(max(1, clip.width)) / CGFloat(max(1, clip.height)),
                        contentMode: .fit
                    )
                    .frame(maxWidth: .infinity, minHeight: 190, maxHeight: 260)
                    .background(Color.black)
                    .clipShape(RoundedRectangle(cornerRadius: 12))

                if currentState == .starting {
                    VStack(spacing: 8) {
                        ProgressView()
                            .tint(.cyan)
                        Text("Starting…")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.white)
                    }
                    .padding(12)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
                } else if let notice = roll.playbackNoticeMessage {
                    VStack(spacing: 10) {
                        Image(systemName: currentState == .error ? "exclamationmark.triangle.fill" : "stop.circle.fill")
                            .font(.title2)
                            .foregroundStyle(currentState == .error ? .red : .orange)
                        Text(notice)
                            .font(.footnote.weight(.semibold))
                            .foregroundStyle(.white)
                            .multilineTextAlignment(.center)
                        if roll.canOfferPlayAgain {
                            Button {
                                roll.playAgain()
                            } label: {
                                Label("Play Again", systemImage: "arrow.clockwise")
                                    .font(.caption.weight(.semibold))
                            }
                            .buttonStyle(.borderedProminent)
                            .controlSize(.small)
                        }
                    }
                    .padding(14)
                    .frame(maxWidth: 300)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                } else if currentState == .ended {
                    Button {
                        roll.playAgain()
                    } label: {
                        Label("Play Again", systemImage: "arrow.clockwise")
                            .font(.subheadline.weight(.semibold))
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.regular)
                }
            }

            // Scrubber & time readout
            VStack(spacing: 4) {
                Slider(
                    value: Binding(
                        get: { min(max(0.0, roll.scrubPositionSec), duration) },
                        set: { newVal in
                            roll.isScrubbing = true
                            roll.scrubPositionSec = newVal
                        }
                    ),
                    in: 0.0...max(0.1, duration),
                    onEditingChanged: { editing in
                        roll.isScrubbing = editing
                        if !editing {
                            roll.commitScrubSeek()
                        }
                    }
                )
                .tint(.cyan)

                HStack {
                    Text(CameraRollController.formatDuration(roll.isScrubbing ? roll.scrubPositionSec : roll.playbackPositionSec))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(currentState.displayLabel)
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(stateBadgeColor(currentState))
                    Spacer()
                    Text(CameraRollController.formatDuration(duration))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }

            // Player action buttons (NO save / download / share — zero-storage enforced)
            HStack(spacing: 14) {
                Button {
                    roll.togglePlayPause()
                } label: {
                    Label(
                        playPauseButtonTitle(for: currentState),
                        systemImage: playPauseButtonIcon(for: currentState)
                    )
                    .font(.subheadline.weight(.semibold))
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)

                Button {
                    roll.toggleStar(for: clip)
                } label: {
                    Image(systemName: clip.starred ? "star.fill" : "star")
                        .foregroundStyle(clip.starred ? .yellow : .secondary)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(!roll.canToggleStar(for: clip))
                .accessibilityLabel(clip.starred ? "Unstar Clip" : "Star Clip")

                if roll.canDeleteClips {
                    Button(role: .destructive) {
                        roll.requestDeleteClip(clip)
                    } label: {
                        Image(systemName: "trash")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .accessibilityLabel("Delete Clip")
                }

                Spacer()

                Button("Close") {
                    roll.closePlayback(resumeLive: true)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }
        }
        .padding(.vertical, 4)
    }

    // MARK: - Owner Camera Roll Controls (Owner Only — Hidden for Viewers)

    private var ownerRollControlsSection: some View {
        let pet = roll.telemetry.petState
        return Section("Camera Roll Controls (Owner)") {
            Toggle("Record to Camera Roll", isOn: Binding(
                get: { pet.rollEnabled },
                set: { roll.setRollRecordingEnabled($0) }
            ))

            HStack(spacing: 10) {
                if pet.clipRecording {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(Color.red)
                            .frame(width: 8, height: 8)
                        Text("\((pet.clipTrigger ?? .manual).displayLabel) · \(CameraRollController.formatDuration(pet.clipElapsedSec))")
                            .font(.subheadline.monospacedDigit().weight(.medium))
                            .foregroundStyle(.red)
                    }
                } else {
                    Text("Manual Clip")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                if !pet.clipManualActive {
                    Button {
                        roll.startManualClip()
                    } label: {
                        Label("Record Now", systemImage: "record.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
                    .controlSize(.small)
                    .disabled(roll.rollBlockedSentence != nil)
                }

                if pet.clipRecording || pet.clipManualActive {
                    Button {
                        roll.stopManualClip()
                    } label: {
                        Label("Stop", systemImage: "stop.fill")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
            }

            Toggle("Motion Trigger (by Depth)", isOn: Binding(
                get: { pet.motionEnabled },
                set: { roll.setMotionTriggerEnabled($0) }
            ))

            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Motion Sensitivity")
                        .font(.subheadline)
                    Spacer()
                    Text("\(Int(roll.draftMotionSensitivity.rounded())) / 10")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
                Slider(
                    value: $roll.draftMotionSensitivity,
                    in: 1...10,
                    step: 1,
                    onEditingChanged: { editing in
                        if editing {
                            roll.beginEditingMotionSensitivity()
                        } else {
                            roll.commitMotionSensitivity(Int(roll.draftMotionSensitivity.rounded()))
                        }
                    }
                )
                Text(pet.motionAvailable
                     ? "Depth motion detector is active."
                     : "Depth motion is paused while the live sensor is off or the PC is replaying.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            if pet.scheduleEnabled, let summary = pet.scheduleSummary, !summary.isEmpty {
                Label("Schedule: \(summary)", systemImage: "calendar.badge.clock")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let blocked = roll.rollBlockedSentence {
                Label(blocked, systemImage: "info.circle.fill")
                    .font(.footnote.weight(.medium))
                    .foregroundStyle(.orange)
            }

            if let cmdErr = roll.rollCommandError {
                Label(cmdErr, systemImage: "exclamationmark.triangle.fill")
                    .font(.footnote.weight(.medium))
                    .foregroundStyle(.red)
            }
        }
    }

    // MARK: - By-Day Browse & Clip Rows

    @ViewBuilder
    private var daysAndClipsSections: some View {
        if let err = roll.listErrorMessage {
            Section {
                Label(err, systemImage: "exclamationmark.circle")
                    .font(.footnote.weight(.medium))
                    .foregroundStyle(.orange)
            }
        }

        if roll.isLoadingDays && roll.days.isEmpty {
            Section {
                HStack(spacing: 10) {
                    ProgressView()
                    Text("Loading camera roll…")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            }
        } else if roll.days.isEmpty && roll.listErrorMessage == nil {
            Section("Recorded Clips") {
                Text("No clips in the camera roll yet.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        } else if !roll.days.isEmpty {
            Section("Days") {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(roll.days) { dayEntry in
                            let isSelected = (roll.selectedDay == dayEntry.day)
                            Button {
                                roll.selectDay(dayEntry.day)
                            } label: {
                                HStack(spacing: 5) {
                                    Text(dayEntry.day)
                                        .font(.caption.weight(.semibold))
                                    Text("(\(dayEntry.clipCount))")
                                        .font(.caption2.monospacedDigit())
                                    if dayEntry.starredCount > 0 {
                                        Image(systemName: "star.fill")
                                            .font(.caption2)
                                            .foregroundStyle(.yellow)
                                    }
                                }
                                .padding(.horizontal, 12)
                                .padding(.vertical, 7)
                                .background(
                                    isSelected ? Color.cyan.opacity(0.26) : Color.secondary.opacity(0.16),
                                    in: Capsule()
                                )
                                .foregroundStyle(isSelected ? .cyan : .primary)
                            }
                            .buttonStyle(.plain)
                        }

                        if roll.daysNextCursor != nil {
                            Button {
                                Task { await roll.loadMoreDays() }
                            } label: {
                                if roll.isLoadingMoreDays {
                                    ProgressView()
                                        .controlSize(.small)
                                } else {
                                    Text("More Days…")
                                        .font(.caption.weight(.semibold))
                                        .padding(.horizontal, 12)
                                        .padding(.vertical, 7)
                                        .background(Color.secondary.opacity(0.16), in: Capsule())
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(.vertical, 2)
                }

                if let st = roll.storage {
                    Text("Roll storage: \(CameraRollController.formatBytes(st.usedBytes)) / \(CameraRollController.formatBytes(st.capBytes)) · Keep \(st.keepDays)d")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }

            if let day = roll.selectedDay {
                Section("Clips — \(day)") {
                    if roll.isSelectedDayLoadingClips && roll.selectedDayClips.isEmpty {
                        HStack(spacing: 10) {
                            ProgressView()
                            Text("Loading clips…")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                    } else if roll.selectedDayClips.isEmpty {
                        Text("No clips on this day.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(roll.selectedDayClips) { clip in
                            clipRow(clip)
                        }

                        if roll.selectedDayNextCursor != nil {
                            Button {
                                Task { await roll.loadMoreClipsForSelectedDay() }
                            } label: {
                                HStack {
                                    Spacer()
                                    if roll.isSelectedDayLoadingClips {
                                        ProgressView()
                                            .controlSize(.small)
                                    } else {
                                        Text("Load More Clips")
                                            .font(.subheadline.weight(.semibold))
                                    }
                                    Spacer()
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    private func clipRow(_ clip: RollClip) -> some View {
        let isPlayingThis = (roll.activeClip?.clipId == clip.clipId)

        return HStack(spacing: 12) {
            Button {
                roll.startPlayback(clip: clip, positionSec: 0.0)
            } label: {
                HStack(spacing: 12) {
                    thumbnailView(for: clip)

                    VStack(alignment: .leading, spacing: 4) {
                        HStack(spacing: 6) {
                            Text(CameraRollController.formatClipStartTime(clip))
                                .font(.subheadline.monospacedDigit().weight(.semibold))
                                .foregroundStyle(isPlayingThis ? .cyan : .primary)

                            Text(clip.trigger.displayLabel)
                                .font(.system(size: 10, weight: .semibold))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(triggerBadgeColor(clip.trigger).opacity(0.22), in: Capsule())
                                .foregroundStyle(triggerBadgeColor(clip.trigger))
                        }

                        HStack(spacing: 6) {
                            Text(CameraRollController.formatDuration(clip.durationSec))
                                .font(.caption.monospacedDigit())
                                .foregroundStyle(.secondary)
                            if let startedBy = clip.startedBy, !startedBy.isEmpty {
                                Text("· \(startedBy)")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                        }
                    }
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            Spacer(minLength: 4)

            Button {
                roll.toggleStar(for: clip)
            } label: {
                Image(systemName: clip.starred ? "star.fill" : "star")
                    .font(.body)
                    .foregroundStyle(clip.starred ? .yellow : .secondary)
                    .frame(width: 30, height: 30)
            }
            .buttonStyle(.plain)
            .disabled(!roll.canToggleStar(for: clip))
            .accessibilityLabel(clip.starred ? "Unstar Clip" : "Star Clip")

            if roll.canDeleteClips {
                Button(role: .destructive) {
                    roll.requestDeleteClip(clip)
                } label: {
                    Image(systemName: "trash")
                        .font(.subheadline)
                        .foregroundStyle(.red)
                        .frame(width: 28, height: 30)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Delete Clip")
            }
        }
        .padding(.vertical, 2)
        .onAppear {
            roll.requestThumbnailIfNeeded(for: clip)
        }
        .onDisappear {
            roll.markClipDisappeared(clip.clipId)
        }
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            if roll.canDeleteClips {
                Button(role: .destructive) {
                    roll.requestDeleteClip(clip)
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            }
        }
    }

    @ViewBuilder
    private func thumbnailView(for clip: RollClip) -> some View {
        let state = roll.thumbnails[clip.clipId] ?? .idle
        let hideImages = viewModel.shouldHideRoomImagesForSnapshot || roll.isHiddenForSnapshot
        ZStack {
            RoundedRectangle(cornerRadius: 8)
                .fill(Color.secondary.opacity(0.18))

            if hideImages {
                Image(systemName: "eye.slash")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            } else {
                switch state {
                case .loaded:
                    if let jpegData = roll.visibleThumbnailJPEG(for: clip.clipId),
                       let uiImage = UIImage(data: jpegData) {
                        Image(uiImage: uiImage)
                            .resizable()
                            .scaledToFill()
                    } else {
                        Image(systemName: "film")
                            .foregroundStyle(.secondary)
                    }
                case .loading:
                    ProgressView()
                        .controlSize(.small)
                case .idle, .unavailable:
                    Image(systemName: roll.telemetry.petState.privacyMode ? "eye.slash" : "film")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: 84, height: 52)
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    // MARK: - Styling Helpers

    private func triggerBadgeColor(_ trigger: ClipTrigger) -> Color {
        switch trigger {
        case .manual: return .cyan
        case .motion: return .orange
        case .schedule: return .mint
        case .unknown: return .secondary
        }
    }

    private func stateBadgeColor(_ state: PlaybackStreamState) -> Color {
        switch state {
        case .playing: return .green
        case .starting: return .cyan
        case .paused: return .yellow
        case .ended: return .secondary
        case .stopped: return .orange
        case .error: return .red
        case .unknown: return .secondary
        }
    }

    private func playPauseButtonTitle(for state: PlaybackStreamState) -> String {
        switch state {
        case .playing, .starting: return "Pause"
        case .paused: return "Resume"
        case .ended, .stopped, .error, .unknown: return "Play"
        }
    }

    private func playPauseButtonIcon(for state: PlaybackStreamState) -> String {
        switch state {
        case .playing, .starting: return "pause.fill"
        case .paused, .ended, .stopped, .error, .unknown: return "play.fill"
        }
    }
}
