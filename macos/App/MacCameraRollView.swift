import AppKit
import SwiftUI

struct MacCameraRollView: View {
    @ObservedObject var roll: CameraRollController
    var onClose: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            // Header bar
            HStack(spacing: 12) {
                Label("Camera Roll", systemImage: "film.stack")
                    .font(.headline)

                if let st = roll.storage {
                    Text("\(CameraRollController.formatBytes(st.usedBytes)) / \(CameraRollController.formatBytes(st.capBytes)) · Keep \(st.keepDays)d")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }

                Spacer()

                Button {
                    Task { await roll.refreshDays(force: true) }
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .controlSize(.small)
                .disabled(roll.isLoadingDays)

                Button("Done") {
                    roll.closePlayback(resumeLive: true)
                    onClose()
                }
                .keyboardShortcut(.cancelAction)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)

            Divider()

            HSplitView {
                // Left: Days + Clips Browser
                VStack(alignment: .leading, spacing: 0) {
                    if let err = roll.listErrorMessage {
                        HStack(spacing: 8) {
                            Image(systemName: "exclamationmark.circle.fill")
                                .foregroundStyle(.orange)
                            Text(err)
                                .font(.caption.weight(.medium))
                                .foregroundStyle(.orange)
                        }
                        .padding(10)
                    }

                    // Day chips bar
                    if !roll.days.isEmpty {
                        ScrollView(.horizontal, showsIndicators: false) {
                            HStack(spacing: 6) {
                                ForEach(roll.days) { dayEntry in
                                    let isSelected = (roll.selectedDay == dayEntry.day)
                                    Button {
                                        roll.selectDay(dayEntry.day)
                                    } label: {
                                        HStack(spacing: 4) {
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
                                        .padding(.horizontal, 10)
                                        .padding(.vertical, 5)
                                        .background(
                                            isSelected ? Color.accentColor.opacity(0.24) : Color.secondary.opacity(0.14),
                                            in: Capsule()
                                        )
                                    }
                                    .buttonStyle(.plain)
                                }

                                if roll.daysNextCursor != nil {
                                    Button {
                                        Task { await roll.loadMoreDays() }
                                    } label: {
                                        Text(roll.isLoadingMoreDays ? "Loading…" : "More Days…")
                                            .font(.caption)
                                            .padding(.horizontal, 10)
                                            .padding(.vertical, 5)
                                            .background(Color.secondary.opacity(0.14), in: Capsule())
                                    }
                                    .buttonStyle(.plain)
                                    .disabled(roll.isLoadingMoreDays)
                                }
                            }
                            .padding(.horizontal, 12)
                            .padding(.vertical, 8)
                        }
                        Divider()
                    }

                    // Clips list
                    if roll.isLoadingDays && roll.days.isEmpty {
                        VStack(spacing: 8) {
                            ProgressView()
                            Text("Loading camera roll…")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else if roll.days.isEmpty {
                        VStack(spacing: 8) {
                            Image(systemName: "film")
                                .font(.title2)
                                .foregroundStyle(.secondary)
                            Text(roll.listErrorMessage ?? "No clips in the camera roll yet.")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                                .multilineTextAlignment(.center)
                                .padding(.horizontal, 20)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        ScrollView {
                            LazyVStack(alignment: .leading, spacing: 6) {
                                ForEach(roll.selectedDayClips) { clip in
                                    clipRow(clip)
                                }

                                if roll.selectedDayNextCursor != nil {
                                    Button {
                                        Task { await roll.loadMoreClipsForSelectedDay() }
                                    } label: {
                                        HStack {
                                            Spacer()
                                            Text(roll.isSelectedDayLoadingClips ? "Loading…" : "Load More Clips")
                                                .font(.caption.weight(.semibold))
                                            Spacer()
                                        }
                                        .padding(.vertical, 6)
                                    }
                                    .buttonStyle(.bordered)
                                    .disabled(roll.isSelectedDayLoadingClips)
                                }
                            }
                            .padding(10)
                        }
                    }
                }
                .frame(minWidth: 320, idealWidth: 350, maxWidth: 420)

                // Right: Streamed Clip Player + Owner Camera Roll Controls
                VStack(spacing: 12) {
                    streamedPlayerPane
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                    if roll.showOwnerRollControls {
                        ownerRollControlsGroupBox
                            .padding(.horizontal, 12)
                            .padding(.bottom, 12)
                    }
                }
                .frame(minWidth: 460, minHeight: 420)
            }
        }
        .frame(minWidth: 820, minHeight: 540)
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

    // MARK: - Clip Row

    private func clipRow(_ clip: RollClip) -> some View {
        let isSelected = (roll.activeClip?.clipId == clip.clipId)

        return HStack(spacing: 10) {
            Button {
                roll.startPlayback(clip: clip, positionSec: 0.0)
            } label: {
                HStack(spacing: 10) {
                    macThumbnailView(for: clip)

                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 6) {
                            Text(CameraRollController.formatClipStartTime(clip))
                                .font(.caption.monospacedDigit().weight(.semibold))
                            Text(clip.trigger.displayLabel)
                                .font(.system(size: 10, weight: .semibold))
                                .padding(.horizontal, 6)
                                .padding(.vertical, 1)
                                .background(triggerColor(clip.trigger).opacity(0.22), in: Capsule())
                                .foregroundStyle(triggerColor(clip.trigger))
                        }

                        HStack(spacing: 6) {
                            Text(CameraRollController.formatDuration(clip.durationSec))
                                .font(.caption2.monospacedDigit())
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
                    .foregroundStyle(clip.starred ? .yellow : .secondary)
            }
            .buttonStyle(.borderless)
            .disabled(!roll.canToggleStar(for: clip))
            .help(clip.starred ? "Unstar clip" : "Star clip")

            if roll.canDeleteClips {
                Button(role: .destructive) {
                    roll.requestDeleteClip(clip)
                } label: {
                    Image(systemName: "trash")
                        .foregroundStyle(.red)
                }
                .buttonStyle(.borderless)
                .help("Delete clip from PC")
            }
        }
        .padding(8)
        .background(
            isSelected ? Color.accentColor.opacity(0.18) : Color.secondary.opacity(0.08),
            in: RoundedRectangle(cornerRadius: 8)
        )
        .onAppear {
            roll.requestThumbnailIfNeeded(for: clip)
        }
        .onDisappear {
            roll.markClipDisappeared(clip.clipId)
        }
    }

    @ViewBuilder
    private func macThumbnailView(for clip: RollClip) -> some View {
        let state = roll.thumbnails[clip.clipId] ?? .idle
        ZStack {
            RoundedRectangle(cornerRadius: 6)
                .fill(Color.black.opacity(0.35))

            switch state {
            case .loaded(let jpegData):
                if let nsImg = NSImage(data: jpegData) {
                    Image(nsImage: nsImg)
                        .resizable()
                        .scaledToFill()
                } else {
                    Image(systemName: "film")
                        .foregroundStyle(.secondary)
                }
            case .loading:
                ProgressView()
                    .controlSize(.mini)
            case .idle, .unavailable:
                Image(systemName: roll.telemetry.petState.privacyMode ? "eye.slash" : "film")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .frame(width: 76, height: 46)
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }

    // MARK: - Streamed Clip Player (`0x16`–`0x19`)

    @ViewBuilder
    private var streamedPlayerPane: some View {
        if let clip = roll.activeClip {
            let currentState = roll.playbackState?.state ?? .starting
            let duration = roll.effectivePlaybackDurationSec

            VStack(spacing: 10) {
                ZStack {
                    Color.black
                    MacSampleBufferVideoView(bridge: roll.playbackDisplayBridge)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                    if currentState == .starting {
                        VStack(spacing: 8) {
                            ProgressView()
                            Text("Starting…")
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(.white)
                        }
                        .padding(14)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
                    } else if let notice = roll.playbackNoticeMessage {
                        VStack(spacing: 10) {
                            Image(systemName: currentState == .error ? "exclamationmark.triangle.fill" : "stop.circle.fill")
                                .font(.title2)
                                .foregroundStyle(currentState == .error ? .red : .orange)
                            Text(notice)
                                .font(.subheadline.weight(.semibold))
                                .foregroundStyle(.white)
                                .multilineTextAlignment(.center)
                            if roll.canOfferPlayAgain {
                                Button("Play Again") {
                                    roll.playAgain()
                                }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                            }
                        }
                        .padding(16)
                        .frame(maxWidth: 340)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
                    } else if currentState == .ended {
                        Button("Play Again") {
                            roll.playAgain()
                        }
                        .buttonStyle(.borderedProminent)
                    }
                }
                .clipShape(RoundedRectangle(cornerRadius: 10))
                .padding(.top, 12)
                .padding(.horizontal, 12)

                VStack(spacing: 6) {
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

                    HStack(spacing: 12) {
                        Button {
                            roll.togglePlayPause()
                        } label: {
                            Label(
                                (currentState == .playing || currentState == .starting) ? "Pause" : "Play",
                                systemImage: (currentState == .playing || currentState == .starting) ? "pause.fill" : "play.fill"
                            )
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)

                        Text("\(CameraRollController.formatDuration(roll.isScrubbing ? roll.scrubPositionSec : roll.playbackPositionSec)) / \(CameraRollController.formatDuration(duration))")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)

                        Text(currentState.displayLabel)
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(stateColor(currentState))

                        Spacer()

                        Button {
                            roll.toggleStar(for: clip)
                        } label: {
                            Label(clip.starred ? "Starred" : "Star", systemImage: clip.starred ? "star.fill" : "star")
                        }
                        .controlSize(.small)
                        .disabled(!roll.canToggleStar(for: clip))

                        if roll.canDeleteClips {
                            Button(role: .destructive) {
                                roll.requestDeleteClip(clip)
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                            .controlSize(.small)
                        }

                        Button("Close Clip") {
                            roll.closePlayback(resumeLive: true)
                        }
                        .controlSize(.small)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.bottom, 6)
            }
        } else {
            VStack(spacing: 10) {
                Image(systemName: "play.rectangle.on.rectangle")
                    .font(.system(size: 36))
                    .foregroundStyle(.secondary)
                Text("Select a Clip to Stream")
                    .font(.headline)
                Text("Clips stream directly from the PC into memory with zero files saved on this Mac.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 320)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    // MARK: - Owner Camera Roll Controls (Owner Only — Hidden for Viewers)

    private var ownerRollControlsGroupBox: some View {
        let pet = roll.telemetry.petState
        return GroupBox("Camera Roll Controls (Owner)") {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 14) {
                    Toggle("Record to Camera Roll", isOn: Binding(
                        get: { pet.rollEnabled },
                        set: { roll.setRollRecordingEnabled($0) }
                    ))
                    .font(.caption)

                    Spacer()

                    if pet.clipRecording {
                        HStack(spacing: 5) {
                            Circle()
                                .fill(Color.red)
                                .frame(width: 7, height: 7)
                            Text("\((pet.clipTrigger ?? .manual).displayLabel) · \(CameraRollController.formatDuration(pet.clipElapsedSec))")
                                .font(.caption.monospacedDigit().weight(.semibold))
                                .foregroundStyle(.red)
                        }
                    }

                    if !pet.clipManualActive {
                        Button("Record Now") {
                            roll.startManualClip()
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.red)
                        .controlSize(.small)
                        .disabled(roll.rollBlockedSentence != nil)
                    }

                    if pet.clipRecording || pet.clipManualActive {
                        Button("Stop") {
                            roll.stopManualClip()
                        }
                        .controlSize(.small)
                    }
                }

                HStack(spacing: 14) {
                    Toggle("Motion (by Depth)", isOn: Binding(
                        get: { pet.motionEnabled },
                        set: { roll.setMotionTriggerEnabled($0) }
                    ))
                    .font(.caption)

                    Text("Sensitivity \(Int(roll.draftMotionSensitivity.rounded()))")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)

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
                    .frame(maxWidth: 160)

                    Text(pet.motionAvailable ? "Motion ready" : "Sensor off / replay")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                if pet.scheduleEnabled, let summary = pet.scheduleSummary, !summary.isEmpty {
                    Text("Schedule: \(summary)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                if let blocked = roll.rollBlockedSentence {
                    Text(blocked)
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.orange)
                }

                if let cmdErr = roll.rollCommandError {
                    Text(cmdErr)
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.red)
                }
            }
            .padding(4)
        }
    }

    private func triggerColor(_ trigger: ClipTrigger) -> Color {
        switch trigger {
        case .manual: return .cyan
        case .motion: return .orange
        case .schedule: return .mint
        case .unknown: return .secondary
        }
    }

    private func stateColor(_ state: PlaybackStreamState) -> Color {
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
}
