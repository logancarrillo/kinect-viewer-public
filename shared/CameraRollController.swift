import Combine
import CoreMedia
import Foundation

enum RollThumbnailState: Equatable, Sendable {
    case idle
    case loading
    case loaded(Data)
    case unavailable(code: String?)
}

/// Shared Protocol v3 Camera Roll & Streamed Playback controller for iOS (`PocketViewModel`)
/// and macOS "Watch a Camera" (`MacCameraViewerController`).
/// Enforces zero disk writes (thumbnails live in a bounded RAM LRU; video streams directly into `AVSampleBufferDisplayLayer`),
/// strict role gating from `0x0B` (`petCameraState`), and on-demand pagination without polling loops.
@MainActor
final class CameraRollController: ObservableObject {
    static let maxCachedThumbnails = 120
    static let maxConcurrentThumbnailRequests = 4

    @Published private(set) var telemetry = PocketClientTelemetry()
    @Published private(set) var days: [RollDay] = []
    @Published private(set) var daysNextCursor: String? = nil
    @Published private(set) var isLoadingDays: Bool = false
    @Published private(set) var isLoadingMoreDays: Bool = false

    @Published var selectedDay: String? = nil
    @Published private(set) var clipsByDay: [String: [RollClip]] = [:]
    @Published private(set) var clipsNextCursorByDay: [String: String] = [:]
    @Published private(set) var loadingClipDays: Set<String> = []
    @Published private(set) var storage: RollStorage? = nil
    @Published var listErrorMessage: String? = nil

    @Published private(set) var thumbnails: [String: RollThumbnailState] = [:]
    @Published private(set) var isHiddenForSnapshot: Bool = false

    // Owner / Viewer actions & confirmation state
    @Published var clipPendingDelete: RollClip? = nil
    @Published var rollCommandError: String? = nil
    @Published var draftMotionSensitivity: Double = 5.0
    @Published private(set) var isEditingMotionSensitivity: Bool = false

    // Streamed clip playback (`0x16`–`0x19`)
    @Published private(set) var activeClip: RollClip? = nil
    @Published private(set) var playbackState: PlaybackStateMessage? = nil
    @Published private(set) var playbackPositionSec: Double = 0.0
    @Published var scrubPositionSec: Double = 0.0
    @Published var isScrubbing: Bool = false
    @Published var playbackNoticeMessage: String? = nil

    let playbackDisplayBridge = SampleBufferDisplayBridge()
    var pageLimit: Int = RollListRequestMessage.defaultLimit

    private weak var client: PocketStreamClientCore?
    private var thumbnailLRUOrder: [String] = []
    private var queuedThumbnailClipIds: [String] = []
    private var activeThumbnailRequestCount: Int = 0
    private var thumbnailQueueGeneration: UInt64 = 0
    private var visibleClipsOrdered: [String] = []
    private var visibleClipsById: [String: RollClip] = [:]
    private var lastManualRefreshDate: Date = .distantPast
    private var wasClipRecording: Bool = false

    init(client: PocketStreamClientCore? = nil) {
        self.client = client
        if let core = client {
            attach(to: core)
        }
    }

    func attach(to core: PocketStreamClientCore) {
        self.client = core
        self.telemetry = core.telemetry
        if core.telemetry.hasConfirmedPetCameraState {
            self.draftMotionSensitivity = Double(core.telemetry.petState.motionSensitivity)
            self.wasClipRecording = core.telemetry.petState.clipRecording
        }

        let pbBridge = self.playbackDisplayBridge
        core.onPlaybackSampleBufferReady = { sampleBuffer in
            pbBridge.enqueue(sampleBuffer)
        }
        core.onPlaybackStreamFlushed = {
            pbBridge.flushAndClearImage()
        }
        core.onPlaybackStateUpdated = { [weak self] stateMsg in
            Task { @MainActor in
                self?.handlePlaybackStateFromCore(stateMsg)
            }
        }
        core.onPlaybackMediaTimeUpdated = { [weak self] streamId, mediaSec in
            Task { @MainActor in
                guard let self = self,
                      !self.isScrubbing,
                      let st = self.playbackState,
                      st.streamId == streamId,
                      st.state == .playing else { return }
                let clamped = min(max(0.0, mediaSec), max(st.durationSec, mediaSec))
                self.playbackPositionSec = clamped
                self.scrubPositionSec = clamped
            }
        }
    }

    private static func isTransientThumbnailFailure(_ code: String?) -> Bool {
        switch code {
        case "privacy_mode", "disconnected", "timeout", "sharing_off", "sharing_schedule", "pc_replaying":
            return true
        default:
            return false
        }
    }

    private var isRollReadyForThumbnails: Bool {
        guard telemetry.isConnected, supportsCameraRoll else { return false }
        if telemetry.hasConfirmedPetCameraState {
            return telemetry.petState.rollAvailable && !telemetry.petState.privacyMode
        }
        return true
    }

    func applyTelemetrySnapshot(_ snap: PocketClientTelemetry) {
        let wasConnected = telemetry.isConnected
        let previousPrivacy = telemetry.petState.privacyMode
        let wasReadyForThumbs = isRollReadyForThumbnails
        telemetry = snap

        if snap.hasConfirmedPetCameraState {
            if !isEditingMotionSensitivity {
                draftMotionSensitivity = Double(snap.petState.motionSensitivity)
            }
            let nowRecording = snap.petState.clipRecording
            if wasClipRecording && !nowRecording && !days.isEmpty && snap.petState.rollAvailable {
                // One-shot refresh when an active clip finishes finalizing on the host (no polling loop).
                wasClipRecording = false
                Task { @MainActor in
                    await self.refreshDays(force: false)
                }
            } else {
                wasClipRecording = nowRecording
            }
        }

        // If privacy mode just turned on, cancel queued/in-flight spinner states and mark `.loading`/`.idle` entries as privacy-unavailable.
        if !previousPrivacy && snap.hasConfirmedPetCameraState && snap.petState.privacyMode {
            thumbnailQueueGeneration &+= 1
            queuedThumbnailClipIds.removeAll()
            activeThumbnailRequestCount = 0
            for (cid, st) in thumbnails {
                switch st {
                case .idle, .loading:
                    thumbnails[cid] = .unavailable(code: "privacy_mode")
                case .loaded, .unavailable:
                    break
                }
            }
        }

        if wasConnected && !snap.isConnected {
            resetForDisconnect()
            return
        }

        // When the camera roll becomes available again (privacy turned off, or reconnected),
        // reset transient thumbnail blocks and re-request currently visible cells!
        if !wasReadyForThumbs && isRollReadyForThumbnails {
            resetTransientlyUnavailableThumbnails()
            requeueVisibleThumbnailsIfAvailable()
        }
    }

    func resetForDisconnect() {
        playbackDisplayBridge.flushAndClearImage()
        activeClip = nil
        playbackState = nil
        playbackPositionSec = 0.0
        scrubPositionSec = 0.0
        isScrubbing = false
        playbackNoticeMessage = nil
        isLoadingDays = false
        isLoadingMoreDays = false
        loadingClipDays.removeAll()
        thumbnailQueueGeneration &+= 1
        queuedThumbnailClipIds.removeAll()
        activeThumbnailRequestCount = 0
        for (cid, st) in thumbnails {
            switch st {
            case .loading, .idle:
                thumbnails.removeValue(forKey: cid)
            case .unavailable(let code) where Self.isTransientThumbnailFailure(code):
                thumbnails.removeValue(forKey: cid)
            default:
                break
            }
        }
        clipPendingDelete = nil
    }

    func clearAllInMemoryState() {
        resetForDisconnect()
        days.removeAll()
        daysNextCursor = nil
        selectedDay = nil
        clipsByDay.removeAll()
        clipsNextCursorByDay.removeAll()
        storage = nil
        listErrorMessage = nil
        thumbnails.removeAll()
        thumbnailLRUOrder.removeAll()
        visibleClipsOrdered.removeAll()
        visibleClipsById.removeAll()
        rollCommandError = nil
    }

    // MARK: - Role & Availability Computed Properties

    /// True against W7+ v3 hosts (`hello.petCamera.cameraRoll == true`, or `rollBlockedReason` such as `folder_unavailable` is reported).
    /// False against v1/v2 hosts and W6 hosts without a camera roll, keeping older hosts unchanged.
    var supportsCameraRoll: Bool {
        guard telemetry.isConnected, telemetry.isPetCameraV3 else { return false }
        if telemetry.petCameraHello?.cameraRoll == true {
            return true
        }
        if telemetry.hasConfirmedPetCameraState {
            if telemetry.petState.rollAvailable || telemetry.petState.parsedRollBlockedReason != nil {
                return true
            }
        }
        return false
    }

    var isOwner: Bool {
        telemetry.isConnected &&
        telemetry.isPetCameraV3 &&
        telemetry.hasConfirmedPetCameraState &&
        telemetry.petState.role == .owner
    }

    var isViewer: Bool {
        telemetry.isConnected &&
        telemetry.isPetCameraV3 &&
        telemetry.hasConfirmedPetCameraState &&
        telemetry.petState.role == .viewer
    }

    /// Viewers NEVER see camera-roll settings (`docs/protocol-v3-pet-camera.md` §3.1).
    var showOwnerRollControls: Bool {
        supportsCameraRoll && isOwner
    }

    /// Viewers NEVER see clip deletion (`docs/protocol-v3-pet-camera.md` §3.1).
    var canDeleteClips: Bool {
        supportsCameraRoll && isOwner
    }

    /// Resolved §9 sentence for `0x0B rollBlockedReason` (falling back to `rollBlockedText` on unknown codes).
    var rollBlockedSentence: String? {
        guard telemetry.isConnected,
              telemetry.isPetCameraV3,
              telemetry.hasConfirmedPetCameraState else {
            return nil
        }
        return telemetry.petState.resolvedRollBlockedSentence
    }

    /// Plain-English explanation when `rollAvailable` is false (e.g. `folder_unavailable`, or viewer during `sharing_off` / `pc_replaying`).
    var rollUnavailableBrowseSentence: String? {
        guard telemetry.isConnected,
              telemetry.isPetCameraV3,
              telemetry.hasConfirmedPetCameraState,
              !telemetry.petState.rollAvailable else {
            return nil
        }
        if telemetry.petState.parsedRollBlockedReason == .folderUnavailable {
            return PetCameraUserStrings.folderUnavailableSentence
        }
        if let liveReason = telemetry.petState.liveUnavailableReason,
           liveReason != .privacyMode,
           liveReason != .tooManyViewers {
            return liveReason.displaySentence
        }
        if let blocked = telemetry.petState.resolvedRollBlockedSentence {
            return blocked
        }
        return "The camera roll isn't available right now."
    }

    var selectedDayClips: [RollClip] {
        guard let day = selectedDay else { return [] }
        return clipsByDay[day] ?? []
    }

    var selectedDayNextCursor: String? {
        guard let day = selectedDay else { return nil }
        return clipsNextCursorByDay[day]
    }

    var isSelectedDayLoadingClips: Bool {
        guard let day = selectedDay else { return false }
        return loadingClipDays.contains(day)
    }

    var effectivePlaybackDurationSec: Double {
        if let st = playbackState, st.durationSec > 0 {
            return st.durationSec
        }
        return max(0.1, activeClip?.durationSec ?? 0.1)
    }

    var canOfferPlayAgain: Bool {
        guard activeClip != nil, let st = playbackState else { return false }
        if st.message == PetCameraUserStrings.playbackClipDeletedSentence {
            return false
        }
        return st.state == .stopped || st.state == .ended || st.state == .error
    }

    // MARK: - On-Demand Listing & Cursor Pagination (`0x12` / `0x13`)

    func loadInitialIfNeeded() {
        guard supportsCameraRoll else { return }
        if let unavail = rollUnavailableBrowseSentence {
            listErrorMessage = unavail
            return
        }
        if days.isEmpty && !isLoadingDays {
            Task { @MainActor in
                await refreshDays(force: true)
            }
        }
    }

    func refreshDays(force: Bool = true) async {
        guard let core = client, supportsCameraRoll else { return }
        if let unavail = rollUnavailableBrowseSentence {
            listErrorMessage = unavail
            isLoadingDays = false
            return
        }
        let now = Date()
        if !force && now.timeIntervalSince(lastManualRefreshDate) < 1.0 {
            return
        }
        lastManualRefreshDate = now
        isLoadingDays = true
        listErrorMessage = nil

        // If privacy mode is now off on a manual refresh, allow previously transiently-blocked thumbnails to load again.
        if isRollReadyForThumbnails {
            resetTransientlyUnavailableThumbnails()
        }

        let clampedLimit = min(RollListRequestMessage.maxLimit, max(RollListRequestMessage.minLimit, pageLimit))
        let result: Result<RollListMessage, CommandResultError> = await withCheckedContinuation { cont in
            core.requestRollList(day: nil, cursor: nil, limit: clampedLimit) { res in
                cont.resume(returning: res)
            }
        }

        isLoadingDays = false
        switch result {
        case .success(let msg):
            let loadedDays = msg.days ?? []
            self.days = loadedDays
            self.daysNextCursor = msg.nextCursor
            self.storage = msg.storage
            self.listErrorMessage = nil

            if let currentDay = selectedDay, loadedDays.contains(where: { $0.day == currentDay }) {
                await loadClips(forDay: currentDay, reset: true)
            } else if let firstDay = loadedDays.first?.day {
                selectedDay = firstDay
                await loadClips(forDay: firstDay, reset: true)
            } else {
                selectedDay = nil
            }

        case .failure(let err):
            if let unavail = rollUnavailableBrowseSentence {
                self.listErrorMessage = unavail
            } else {
                self.listErrorMessage = err.message
            }
        }
    }

    func loadMoreDays() async {
        guard let core = client,
              supportsCameraRoll,
              !isLoadingMoreDays,
              let cursor = daysNextCursor else { return }
        isLoadingMoreDays = true
        let clampedLimit = min(RollListRequestMessage.maxLimit, max(RollListRequestMessage.minLimit, pageLimit))
        let result: Result<RollListMessage, CommandResultError> = await withCheckedContinuation { cont in
            core.requestRollList(day: nil, cursor: cursor, limit: clampedLimit) { res in
                cont.resume(returning: res)
            }
        }
        isLoadingMoreDays = false
        switch result {
        case .success(let msg):
            let existingIds = Set(days.map(\.day))
            for d in (msg.days ?? []) where !existingIds.contains(d.day) {
                days.append(d)
            }
            daysNextCursor = msg.nextCursor
            storage = msg.storage
        case .failure(let err):
            listErrorMessage = err.message
        }
    }

    func selectDay(_ day: String) {
        selectedDay = day
        if clipsByDay[day] == nil && !loadingClipDays.contains(day) {
            Task { @MainActor in
                await loadClips(forDay: day, reset: true)
            }
        }
    }

    func loadClips(forDay day: String, reset: Bool) async {
        guard let core = client, supportsCameraRoll else { return }
        if loadingClipDays.contains(day) { return }
        let cursor: String?
        if reset {
            cursor = nil
        } else {
            guard let next = clipsNextCursorByDay[day] else { return }
            cursor = next
        }

        loadingClipDays.insert(day)
        let clampedLimit = min(RollListRequestMessage.maxLimit, max(RollListRequestMessage.minLimit, pageLimit))
        let result: Result<RollListMessage, CommandResultError> = await withCheckedContinuation { cont in
            core.requestRollList(day: day, cursor: cursor, limit: clampedLimit) { res in
                cont.resume(returning: res)
            }
        }
        loadingClipDays.remove(day)

        switch result {
        case .success(let msg):
            let pageClips = msg.clips ?? []
            if reset {
                clipsByDay[day] = pageClips
            } else {
                var current = clipsByDay[day] ?? []
                let seen = Set(current.map(\.clipId))
                for clip in pageClips where !seen.contains(clip.clipId) {
                    current.append(clip)
                }
                clipsByDay[day] = current
            }
            if let next = msg.nextCursor {
                clipsNextCursorByDay[day] = next
            } else {
                clipsNextCursorByDay.removeValue(forKey: day)
            }
            storage = msg.storage
            listErrorMessage = nil

        case .failure(let err):
            listErrorMessage = err.message
        }
    }

    func loadMoreClipsForSelectedDay() async {
        guard let day = selectedDay else { return }
        await loadClips(forDay: day, reset: false)
    }

    // MARK: - Bounded In-Memory Thumbnails (`0x14` / `0x15` / `0x0D`)

    private func resetTransientlyUnavailableThumbnails() {
        for (cid, st) in thumbnails {
            switch st {
            case .idle:
                thumbnails.removeValue(forKey: cid)
            case .unavailable(let code) where Self.isTransientThumbnailFailure(code):
                thumbnails.removeValue(forKey: cid)
            default:
                break
            }
        }
    }

    private func requeueVisibleThumbnailsIfAvailable() {
        guard isRollReadyForThumbnails else { return }
        for cid in visibleClipsOrdered {
            if let clip = visibleClipsById[cid], thumbnails[cid] == nil {
                enqueueThumbnailLoad(for: clip)
            }
        }
    }

    func markClipDisappeared(_ clipId: String) {
        visibleClipsById.removeValue(forKey: clipId)
        visibleClipsOrdered.removeAll { $0 == clipId }
    }

    func requestThumbnailIfNeeded(for clip: RollClip) {
        visibleClipsById[clip.clipId] = clip
        if !visibleClipsOrdered.contains(clip.clipId) {
            visibleClipsOrdered.append(clip.clipId)
        }
        if let existing = thumbnails[clip.clipId] {
            switch existing {
            case .loaded, .loading:
                return
            case .unavailable(let code):
                guard Self.isTransientThumbnailFailure(code), isRollReadyForThumbnails else {
                    return
                }
                thumbnails.removeValue(forKey: clip.clipId)
            case .idle:
                thumbnails.removeValue(forKey: clip.clipId)
            }
        }
        enqueueThumbnailLoad(for: clip)
    }

    private func enqueueThumbnailLoad(for clip: RollClip) {
        guard clip.hasThumbnail else {
            thumbnails[clip.clipId] = .unavailable(code: "no_thumbnail")
            return
        }
        if telemetry.hasConfirmedPetCameraState && telemetry.petState.privacyMode {
            thumbnails[clip.clipId] = .unavailable(code: "privacy_mode")
            return
        }
        guard isRollReadyForThumbnails else {
            thumbnails.removeValue(forKey: clip.clipId)
            return
        }
        thumbnails[clip.clipId] = .loading
        if !queuedThumbnailClipIds.contains(clip.clipId) {
            queuedThumbnailClipIds.append(clip.clipId)
        }
        pumpThumbnailQueue()
    }

    private func pumpThumbnailQueue() {
        guard let core = client, isRollReadyForThumbnails else { return }
        let gen = thumbnailQueueGeneration
        while activeThumbnailRequestCount < Self.maxConcurrentThumbnailRequests,
              !queuedThumbnailClipIds.isEmpty {
            let clipId = queuedThumbnailClipIds.removeFirst()
            guard thumbnails[clipId] == .loading else { continue }
            activeThumbnailRequestCount += 1
            core.requestRollThumbnail(clipId: clipId) { [weak self] result in
                Task { @MainActor in
                    guard let self = self, self.thumbnailQueueGeneration == gen else { return }
                    self.activeThumbnailRequestCount = max(0, self.activeThumbnailRequestCount - 1)
                    switch result {
                    case .success(let thumb):
                        self.storeThumbnailInMemory(clipId: clipId, jpeg: thumb.jpeg)
                    case .failure(let err):
                        if !self.telemetry.isConnected || err.code == "disconnected" {
                            self.thumbnails.removeValue(forKey: clipId)
                        } else if err.code == "privacy_mode" {
                            self.thumbnails[clipId] = .unavailable(code: "privacy_mode")
                            self.queuedThumbnailClipIds.removeAll()
                            for (cid, st) in self.thumbnails {
                                if case .loading = st, self.activeThumbnailRequestCount == 0 {
                                    self.thumbnails[cid] = .unavailable(code: "privacy_mode")
                                }
                            }
                            return
                        } else {
                            // Clean placeholder on `0x0D` (e.g. `not_found`) — never retries in a loop!
                            self.thumbnails[clipId] = .unavailable(code: err.code)
                        }
                    }
                    self.pumpThumbnailQueue()
                }
            }
        }
    }

    func setHiddenForSnapshot(_ hidden: Bool) {
        if isHiddenForSnapshot != hidden {
            isHiddenForSnapshot = hidden
        }
        // The player stays mounted under the cover, so it must also stop drawing: drop incoming frames while covered.
        playbackDisplayBridge.setSuppressedForSnapshot(hidden)
        if hidden {
            playbackDisplayBridge.flushAndClearImage()
        }
    }

    func visibleThumbnailJPEG(for clipId: String) -> Data? {
        guard !isHiddenForSnapshot else { return nil }
        if telemetry.hasConfirmedPetCameraState && telemetry.petState.privacyMode {
            return nil
        }
        if case .loaded(let jpeg) = thumbnails[clipId] {
            return jpeg
        }
        return nil
    }

    private func storeThumbnailInMemory(clipId: String, jpeg: Data) {
        thumbnails[clipId] = .loaded(jpeg)
        thumbnailLRUOrder.removeAll { $0 == clipId }
        thumbnailLRUOrder.append(clipId)
        while thumbnailLRUOrder.count > Self.maxCachedThumbnails {
            let evicted = thumbnailLRUOrder.removeFirst()
            thumbnails.removeValue(forKey: evicted)
        }
    }

    // MARK: - Star (Owner & Viewer) and Delete (Owner Only)

    func canToggleStar(for clip: RollClip) -> Bool {
        guard supportsCameraRoll else { return false }
        if isOwner {
            return true
        }
        if isViewer {
            // §3.1 & §5.2: A viewer may star an unstarred clip, and may unstar ONLY its own stars (`starredByMe`).
            return !clip.starred || clip.starredByMe
        }
        return false
    }

    func toggleStar(for clip: RollClip) {
        guard let core = client, canToggleStar(for: clip) else { return }
        let targetStarred = isOwner ? !clip.starred : !clip.starredByMe
        rollCommandError = nil
        core.sendPetCameraCommandWithResult(
            action: .starClip,
            clipId: clip.clipId,
            starred: targetStarred
        ) { [weak self] result in
            Task { @MainActor in
                guard let self = self else { return }
                switch result {
                case .success:
                    self.applyLocalStarUpdate(clipId: clip.clipId, starred: targetStarred)
                case .failure(let err):
                    self.rollCommandError = err.message
                }
            }
        }
    }

    private func applyLocalStarUpdate(clipId: String, starred: Bool) {
        for (day, var list) in clipsByDay {
            if let idx = list.firstIndex(where: { $0.clipId == clipId }) {
                let wasStarred = list[idx].starred
                list[idx].starred = starred
                list[idx].starredByMe = starred
                clipsByDay[day] = list
                if wasStarred != starred, let dIdx = days.firstIndex(where: { $0.day == day }) {
                    let delta = starred ? 1 : -1
                    days[dIdx].starredCount = max(0, days[dIdx].starredCount + delta)
                }
                if activeClip?.clipId == clipId {
                    activeClip = list[idx]
                }
                break
            }
        }
    }

    func requestDeleteClip(_ clip: RollClip) {
        guard canDeleteClips else { return }
        clipPendingDelete = clip
    }

    func confirmDeletePendingClip() {
        guard let clip = clipPendingDelete else { return }
        clipPendingDelete = nil
        guard let core = client, canDeleteClips else { return }
        rollCommandError = nil
        core.sendPetCameraCommandWithResult(
            action: .deleteClip,
            clipId: clip.clipId
        ) { [weak self] result in
            Task { @MainActor in
                guard let self = self else { return }
                switch result {
                case .success:
                    self.applyLocalClipRemoval(clipId: clip.clipId)
                case .failure(let err):
                    self.rollCommandError = err.message
                }
            }
        }
    }

    private func applyLocalClipRemoval(clipId: String) {
        thumbnails.removeValue(forKey: clipId)
        thumbnailLRUOrder.removeAll { $0 == clipId }
        visibleClipsById.removeValue(forKey: clipId)
        visibleClipsOrdered.removeAll { $0 == clipId }
        queuedThumbnailClipIds.removeAll { $0 == clipId }
        for (day, var list) in clipsByDay {
            if let idx = list.firstIndex(where: { $0.clipId == clipId }) {
                let removed = list.remove(at: idx)
                clipsByDay[day] = list
                if let dIdx = days.firstIndex(where: { $0.day == day }) {
                    days[dIdx].clipCount = max(0, days[dIdx].clipCount - 1)
                    if removed.starred {
                        days[dIdx].starredCount = max(0, days[dIdx].starredCount - 1)
                    }
                    if days[dIdx].bytes >= removed.bytes {
                        days[dIdx].bytes -= removed.bytes
                    }
                    if days[dIdx].clipCount == 0 && clipsNextCursorByDay[day] == nil {
                        days.remove(at: dIdx)
                        clipsByDay.removeValue(forKey: day)
                        if selectedDay == day {
                            selectedDay = days.first?.day
                            if let nextDay = selectedDay, clipsByDay[nextDay] == nil {
                                Task { @MainActor in
                                    await self.loadClips(forDay: nextDay, reset: true)
                                }
                            }
                        }
                    }
                }
                break
            }
        }
    }

    // MARK: - Owner Roll Controls (`set_roll`, `start_clip` / `stop_clip`, `set_motion`)

    func setRollRecordingEnabled(_ enabled: Bool) {
        guard let core = client, isOwner else { return }
        rollCommandError = nil
        core.sendPetCameraCommandWithResult(action: .setRoll, enabled: enabled) { [weak self] result in
            Task { @MainActor in
                if case .failure(let err) = result {
                    self?.rollCommandError = err.message
                }
            }
        }
    }

    func setMotionTriggerEnabled(_ enabled: Bool) {
        guard let core = client, isOwner else { return }
        rollCommandError = nil
        core.sendPetCameraCommandWithResult(action: .setMotion, enabled: enabled) { [weak self] result in
            Task { @MainActor in
                if case .failure(let err) = result {
                    self?.rollCommandError = err.message
                }
            }
        }
    }

    func beginEditingMotionSensitivity() {
        isEditingMotionSensitivity = true
    }

    func commitMotionSensitivity(_ rawValue: Int) {
        isEditingMotionSensitivity = false
        guard let core = client, isOwner else { return }
        let clamped = min(10, max(1, rawValue))
        draftMotionSensitivity = Double(clamped)
        rollCommandError = nil
        core.sendPetCameraCommandWithResult(action: .setMotion, sensitivity: clamped) { [weak self] result in
            Task { @MainActor in
                if case .failure(let err) = result {
                    self?.rollCommandError = err.message
                }
            }
        }
    }

    func startManualClip() {
        guard let core = client, telemetry.isConnected, telemetry.isPetCameraV3 else { return }
        rollCommandError = nil
        core.sendPetCameraCommandWithResult(action: .startClip) { [weak self] result in
            Task { @MainActor in
                guard let self = self else { return }
                switch result {
                case .success:
                    self.rollCommandError = nil
                case .failure(let err):
                    self.rollCommandError = err.message
                }
            }
        }
    }

    /// Sends `0x0C stop_clip`.
    /// Per §5.2: `ok: true` when a manual clip was active AND as a no-op when nothing is recording (never treated as an error);
    /// `busy` ONLY while a motion or schedule clip is recording with no manual trigger active (surfaces its reason).
    func stopManualClip() {
        guard let core = client, telemetry.isConnected, telemetry.isPetCameraV3 else { return }
        rollCommandError = nil
        core.sendPetCameraCommandWithResult(action: .stopClip) { [weak self] result in
            Task { @MainActor in
                guard let self = self else { return }
                switch result {
                case .success:
                    self.rollCommandError = nil
                case .failure(let err):
                    self.rollCommandError = err.message
                }
            }
        }
    }

    // MARK: - Streamed Clip Playback (`0x16`–`0x19`)

    func startPlayback(clip: RollClip, positionSec: Double = 0.0) {
        guard let core = client, supportsCameraRoll else { return }
        activeClip = clip
        playbackState = PlaybackStateMessage(
            requestId: nil,
            streamId: 0,
            clipId: clip.clipId,
            state: .starting,
            positionSec: positionSec,
            durationSec: clip.durationSec,
            message: nil
        )
        playbackPositionSec = positionSec
        scrubPositionSec = positionSec
        isScrubbing = false
        playbackNoticeMessage = nil
        playbackDisplayBridge.flushAndClearImage()

        core.sendPlaybackRequest(action: .start, clipId: clip.clipId, positionSec: positionSec) { [weak self] result in
            Task { @MainActor in
                guard let self = self, self.activeClip?.clipId == clip.clipId else { return }
                if case .failure(let err) = result {
                    self.playbackState = PlaybackStateMessage(
                        requestId: nil,
                        streamId: 0,
                        clipId: clip.clipId,
                        state: .stopped,
                        positionSec: 0.0,
                        durationSec: clip.durationSec,
                        message: err.message
                    )
                    self.playbackNoticeMessage = err.message
                }
            }
        }
    }

    func togglePlayPause() {
        guard let core = client, let clip = activeClip else { return }
        guard let st = playbackState else {
            startPlayback(clip: clip, positionSec: 0.0)
            return
        }
        switch st.state {
        case .playing, .starting:
            guard st.streamId > 0 else { return }
            core.sendPlaybackRequest(action: .pause, streamId: st.streamId) { [weak self] res in
                Task { @MainActor in
                    if case .failure(let err) = res {
                        self?.playbackNoticeMessage = err.message
                    }
                }
            }
        case .paused, .ended:
            guard st.streamId > 0 else {
                startPlayback(clip: clip, positionSec: 0.0)
                return
            }
            core.sendPlaybackRequest(action: .resume, streamId: st.streamId) { [weak self] res in
                Task { @MainActor in
                    if case .failure(let err) = res {
                        self?.playbackNoticeMessage = err.message
                    }
                }
            }
        case .stopped, .error, .unknown:
            startPlayback(clip: clip, positionSec: 0.0)
        }
    }

    func playAgain() {
        guard let clip = activeClip else { return }
        startPlayback(clip: clip, positionSec: 0.0)
    }

    func commitScrubSeek() {
        let target = min(max(0.0, scrubPositionSec), effectivePlaybackDurationSec)
        isScrubbing = false
        seekPlayback(to: target)
    }

    func seekPlayback(to positionSec: Double) {
        guard let core = client, let clip = activeClip else { return }
        let clamped = min(max(0.0, positionSec), effectivePlaybackDurationSec)
        playbackPositionSec = clamped
        scrubPositionSec = clamped

        if let st = playbackState, !st.state.freesStreamAllocation, st.streamId > 0 {
            core.sendPlaybackRequest(action: .seek, streamId: st.streamId, positionSec: clamped) { [weak self] res in
                Task { @MainActor in
                    if case .failure(let err) = res {
                        self?.playbackNoticeMessage = err.message
                    }
                }
            }
        } else {
            startPlayback(clip: clip, positionSec: clamped)
        }
    }

    func closePlayback(resumeLive: Bool = true) {
        guard activeClip != nil || playbackState != nil else { return }
        activeClip = nil
        playbackState = nil
        playbackPositionSec = 0.0
        scrubPositionSec = 0.0
        isScrubbing = false
        playbackNoticeMessage = nil
        playbackDisplayBridge.flushAndClearImage()
        client?.stopPlaybackAndResumeLiveIfNeeded(resumeLive: resumeLive)
    }

    private func handlePlaybackStateFromCore(_ stateMsg: PlaybackStateMessage?) {
        guard let stateMsg = stateMsg else {
            playbackState = nil
            return
        }
        guard activeClip == nil || activeClip?.clipId == stateMsg.clipId else {
            return
        }
        playbackState = stateMsg
        if !isScrubbing {
            playbackPositionSec = stateMsg.positionSec
            scrubPositionSec = stateMsg.positionSec
        }
        if stateMsg.state == .stopped || stateMsg.state == .error {
            let trimmed = stateMsg.message?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            if !trimmed.isEmpty {
                playbackNoticeMessage = trimmed
            } else if stateMsg.state == .error {
                playbackNoticeMessage = PetCameraUserStrings.playbackClipReadErrorSentence
            } else {
                playbackNoticeMessage = nil
            }
        } else {
            playbackNoticeMessage = nil
        }
    }

    // MARK: - Formatting Helpers

    static func formatDuration(_ seconds: Double) -> String {
        let total = max(0, Int(seconds.rounded()))
        let hrs = total / 3600
        let mins = (total % 3600) / 60
        let secs = total % 60
        if hrs > 0 {
            return String(format: "%d:%02d:%02d", hrs, mins, secs)
        }
        return String(format: "%d:%02d", mins, secs)
    }

    static func formatClipStartTime(_ clip: RollClip) -> String {
        // `startLocal` is ISO 8601 PC-local time (`YYYY-MM-DDTHH:MM:SS±HH:MM`); extract `HH:MM:SS` cleanly or format `startUnixMs`.
        if let tIndex = clip.startLocal.firstIndex(of: "T") {
            let afterT = clip.startLocal[clip.startLocal.index(after: tIndex)...]
            let timePart = afterT.prefix(8)
            if timePart.count == 8 {
                return String(timePart)
            }
        }
        let date = Date(timeIntervalSince1970: Double(clip.startUnixMs) / 1000.0)
        let fmt = DateFormatter()
        fmt.dateStyle = .none
        fmt.timeStyle = .medium
        return fmt.string(from: date)
    }

    static func formatBytes(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(min(bytes, UInt64(Int64.max))), countStyle: .file)
    }
}
