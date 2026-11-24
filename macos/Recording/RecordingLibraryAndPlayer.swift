import CoreGraphics
import Foundation
import ImageIO

struct RecordedIrIntrinsics: Codable, Equatable, Sendable {
    var fx: Float
    var fy: Float
    var cx: Float
    var cy: Float
    var k1: Float
    var k2: Float
    var k3: Float
    var p1: Float
    var p2: Float

    init(from c: kv_ir_intrinsics_t) {
        self.fx = c.fx
        self.fy = c.fy
        self.cx = c.cx
        self.cy = c.cy
        self.k1 = c.k1
        self.k2 = c.k2
        self.k3 = c.k3
        self.p1 = c.p1
        self.p2 = c.p2
    }

    var cValue: kv_ir_intrinsics_t {
        kv_ir_intrinsics_t(
            valid: fx > 0 && fy > 0,
            fx: fx,
            fy: fy,
            cx: cx,
            cy: cy,
            k1: k1,
            k2: k2,
            k3: k3,
            p1: p1,
            p2: p2
        )
    }
}

struct RecordingSessionMetadata: Codable, Equatable, Sendable {
    var createdAtISO8601: String
    var modeSlug: String
    var container: String
    var width: Int
    var height: Int
    var frameCount: Int
    var durationSec: Double
    var irIntrinsics: RecordedIrIntrinsics?
}

enum DeletionTrigger: String, Sendable {
    case automatic = "automatic"
    case user = "user"
}

/// Pure metadata representation of a recorded Depth or IR session in `~/kinect-viewer-recordings/`.
/// Strictly metadata only — zero thumbnails or previews of content (Phase 2.1).
struct RecordingSessionItem: Identifiable, Equatable, Sendable {
    static let retentionSeconds: TimeInterval = 7.0 * 86_400.0

    let folderURL: URL
    let folderName: String
    let createdAt: Date
    let isDepthMode: Bool
    let container: String // "ffv1" or "png16"
    let width: Int
    let height: Int
    let frameCount: Int
    let durationSec: Double
    let sizeBytes: UInt64
    let irIntrinsics: RecordedIrIntrinsics?

    var id: String { folderName }

    var modeTitle: String {
        isDepthMode ? "Depth" : "IR"
    }

    var modeSlug: String {
        isDepthMode ? "depth16mm" : "ir16"
    }

    var formattedDate: String {
        let df = DateFormatter()
        df.dateStyle = .medium
        df.timeStyle = .short
        return df.string(from: createdAt)
    }

    var formattedLength: String {
        if durationSec >= 60.0 {
            let mins = Int(durationSec) / 60
            let secs = Int(durationSec.rounded()) % 60
            return String(format: "%dm %02ds (%d f)", mins, secs, frameCount)
        }
        return String(format: "%.1f s (%d f)", durationSec, frameCount)
    }

    var formattedSize: String {
        if sizeBytes >= 1_000_000 {
            return String(format: "%.1f MB", Double(sizeBytes) / 1_000_000.0)
        }
        return String(format: "%.0f KB", Double(sizeBytes) / 1_000.0)
    }

    var expiresAt: Date {
        createdAt.addingTimeInterval(Self.retentionSeconds)
    }

    func isExpired(at now: Date) -> Bool {
        now >= expiresAt
    }

    func countdownText(at now: Date) -> String {
        let remaining = expiresAt.timeIntervalSince(now)
        if remaining <= 0 {
            return "Expired"
        }
        let totalMinutes = Int(remaining / 60.0)
        let days = totalMinutes / (24 * 60)
        let hours = (totalMinutes % (24 * 60)) / 60
        let mins = totalMinutes % 60
        if days > 0 {
            return "\(days)d \(hours)h left"
        } else if hours > 0 {
            return "\(hours)h \(mins)m left"
        } else {
            return "\(max(1, mins))m left"
        }
    }
}

enum PreRecordingReminderPolicy {
    static let exactReminderText = SharedRecordingReminderConstants.exactReminderText
    static let snoozeDurationSeconds: TimeInterval = SharedRecordingReminderConstants.snoozeDurationSeconds
}

enum RecordingLibrary {
    static let deletionLogFileName = "deletion-log.txt"

    private static func isNonSymlinkDirectory(_ url: URL) -> Bool {
        guard let vals = try? url.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey]) else {
            return false
        }
        if vals.isSymbolicLink == true {
            return false
        }
        if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
           (attrs[.type] as? FileAttributeType) == .typeSymbolicLink {
            return false
        }
        return vals.isDirectory == true
    }

    static func listSessions(
        in baseDirURL: URL,
        excludingActiveFolderPath: String? = nil
    ) -> [RecordingSessionItem] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: baseDirURL,
            includingPropertiesForKeys: [.isSymbolicLinkKey, .isDirectoryKey, .creationDateKey, .contentModificationDateKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        var items: [RecordingSessionItem] = []
        for url in entries {
            guard url.pathExtension == "nosync" else { continue }
            guard isNonSymlinkDirectory(url) else { continue }
            if let activePath = excludingActiveFolderPath,
               url.standardizedFileURL.path == URL(fileURLWithPath: activePath).standardizedFileURL.path {
                continue
            }
            if let parsed = parseSessionDirectory(url) {
                items.append(parsed)
            }
        }

        items.sort { $0.createdAt > $1.createdAt }
        return items
    }

    /// Deletes any `.nosync` session directory whose type is not strictly `ir16` or `depth16mm`
    /// (such as legacy `*_color1080p.nosync` / `*_sidebyside.nosync` HEVC folders) and logs each as `automatic`.
    @discardableResult
    static func purgeDisallowedSessions(
        in baseDirURL: URL,
        now: Date = Date(),
        excludingActiveFolderPath: String? = nil
    ) -> [String] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: baseDirURL,
            includingPropertiesForKeys: [.isSymbolicLinkKey, .isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        var deletedNames: [String] = []
        for url in entries {
            let folderName = url.lastPathComponent
            guard url.pathExtension == "nosync", folderName != ".nosync" else { continue }
            guard isNonSymlinkDirectory(url) else { continue }
            if let activePath = excludingActiveFolderPath,
               url.standardizedFileURL.path == URL(fileURLWithPath: activePath).standardizedFileURL.path {
                continue
            }
            // Keep only valid ir16 / depth16mm sessions; purge anything else immediately
            if parseSessionDirectory(url) == nil {
                let sizeBeforeDelete = HostRecorder.directorySizeBytes(at: url)
                if (try? fm.removeItem(at: url)) != nil {
                    appendDeletionLogEntry(
                        in: baseDirURL,
                        fileName: folderName,
                        sizeBytes: sizeBeforeDelete,
                        timestamp: now,
                        trigger: .automatic
                    )
                    deletedNames.append(folderName)
                }
            }
        }
        return deletedNames
    }

    @discardableResult
    static func purgeExpiredSessions(
        in baseDirURL: URL,
        now: Date = Date(),
        excludingActiveFolderPath: String? = nil
    ) -> [String] {
        var deletedNames = purgeDisallowedSessions(
            in: baseDirURL,
            now: now,
            excludingActiveFolderPath: excludingActiveFolderPath
        )
        let sessions = listSessions(in: baseDirURL, excludingActiveFolderPath: excludingActiveFolderPath)
        for session in sessions where session.isExpired(at: now) {
            let sizeBeforeDelete = session.sizeBytes
            do {
                try FileManager.default.removeItem(at: session.folderURL)
                appendDeletionLogEntry(
                    in: baseDirURL,
                    fileName: session.folderName,
                    sizeBytes: sizeBeforeDelete,
                    timestamp: now,
                    trigger: .automatic
                )
                deletedNames.append(session.folderName)
            } catch {
                continue
            }
        }
        return deletedNames
    }

    static func deleteSessionByUser(
        _ session: RecordingSessionItem,
        in baseDirURL: URL,
        now: Date = Date()
    ) throws {
        guard isNonSymlinkDirectory(session.folderURL) else {
            throw NSError(
                domain: "RecordingLibrary",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "Refusing to delete symlinked or missing recording folder."]
            )
        }
        let sizeBeforeDelete = HostRecorder.directorySizeBytes(at: session.folderURL)
        try FileManager.default.removeItem(at: session.folderURL)
        appendDeletionLogEntry(
            in: baseDirURL,
            fileName: session.folderName,
            sizeBytes: sizeBeforeDelete > 0 ? sizeBeforeDelete : session.sizeBytes,
            timestamp: now,
            trigger: .user
        )
    }

    /// Appends strictly: ISO-8601 timestamp, file/session name, byte size, and trigger (`automatic` or `user`).
    /// Nothing about the content goes in the log (Phase 2.3).
    static func appendDeletionLogEntry(
        in baseDirURL: URL,
        fileName: String,
        sizeBytes: UInt64,
        timestamp: Date,
        trigger: DeletionTrigger
    ) {
        let fm = FileManager.default
        if !fm.fileExists(atPath: baseDirURL.path) {
            try? fm.createDirectory(at: baseDirURL, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        }
        let logURL = baseDirURL.appendingPathComponent(deletionLogFileName)
        let iso = ISO8601DateFormatter()
        let line = "\(iso.string(from: timestamp))\t\(fileName)\t\(sizeBytes) bytes\t\(trigger.rawValue)\n"
        guard let data = line.data(using: .utf8) else { return }

        if !fm.fileExists(atPath: logURL.path) {
            fm.createFile(atPath: logURL.path, contents: data, attributes: [.posixPermissions: 0o600])
        } else if let handle = try? FileHandle(forWritingTo: logURL) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
        }
    }

    private static func parseSessionDirectory(_ folderURL: URL) -> RecordingSessionItem? {
        let folderName = folderURL.lastPathComponent
        let sizeBytes = HostRecorder.directorySizeBytes(at: folderURL)
        let metaURL = folderURL.appendingPathComponent("session_meta.json")
        let iso = ISO8601DateFormatter()

        if let metaData = try? Data(contentsOf: metaURL),
           let meta = try? JSONDecoder().decode(RecordingSessionMetadata.self, from: metaData) {
            guard meta.modeSlug == "ir16" || meta.modeSlug == "depth16mm" else {
                // Disallow any colour/side-by-side or unrecognized recording type
                return nil
            }
            let created = parseFolderTimestamp(folderName)
                ?? iso.date(from: meta.createdAtISO8601)
                ?? fallbackFilesystemDate(folderURL)
            let isDepth = (meta.modeSlug == "depth16mm")
            var frames = meta.frameCount
            var duration = meta.durationSec
            if frames == 0 {
                let csvStats = parseCsvStats(folderURL: folderURL, stem: meta.modeSlug)
                frames = csvStats.frameCount
                if duration <= 0 { duration = csvStats.durationSec }
            }
            return RecordingSessionItem(
                folderURL: folderURL,
                folderName: folderName,
                createdAt: created,
                isDepthMode: isDepth,
                container: meta.container,
                width: meta.width > 0 ? meta.width : 512,
                height: meta.height > 0 ? meta.height : 424,
                frameCount: frames,
                durationSec: duration,
                sizeBytes: sizeBytes,
                irIntrinsics: meta.irIntrinsics
            )
        }

        // Legacy fallback when session_meta.json is absent: strictly require explicit ir16 or depth16mm session markers.
        // Never classify *_color1080p.nosync, *_sidebyside.nosync, or unknown folders as IR!
        let fm = FileManager.default
        let isDepth = folderName.hasSuffix("_depth16mm.nosync") ||
            fm.fileExists(atPath: folderURL.appendingPathComponent("depth16mm_timestamps.csv").path) ||
            fm.fileExists(atPath: folderURL.appendingPathComponent("depth16mm_lossless.mkv").path) ||
            fm.fileExists(atPath: folderURL.appendingPathComponent("depth16mm_000000.png").path)
        let isIr = !isDepth && (
            folderName.hasSuffix("_ir16.nosync") ||
            fm.fileExists(atPath: folderURL.appendingPathComponent("ir16_timestamps.csv").path) ||
            fm.fileExists(atPath: folderURL.appendingPathComponent("ir16_lossless.mkv").path) ||
            fm.fileExists(atPath: folderURL.appendingPathComponent("ir16_000000.png").path)
        )
        guard isDepth || isIr else {
            return nil
        }

        let stem = isDepth ? "depth16mm" : "ir16"
        let hasMkv = fm.fileExists(atPath: folderURL.appendingPathComponent("\(stem)_lossless.mkv").path)
        let container = hasMkv ? "ffv1" : "png16"
        let created = parseFolderTimestamp(folderName) ?? fallbackFilesystemDate(folderURL)
        let csvStats = parseCsvStats(folderURL: folderURL, stem: stem)

        return RecordingSessionItem(
            folderURL: folderURL,
            folderName: folderName,
            createdAt: created,
            isDepthMode: isDepth,
            container: container,
            width: 512,
            height: 424,
            frameCount: csvStats.frameCount,
            durationSec: csvStats.durationSec,
            sizeBytes: sizeBytes,
            irIntrinsics: nil
        )
    }

    static func parseFolderTimestamp(_ folderName: String) -> Date? {
        // Expected prefix: yyyyMMdd-HHmmss-SSS_...
        let parts = folderName.split(separator: "_")
        guard let prefix = parts.first else { return nil }
        let df = DateFormatter()
        df.locale = Locale(identifier: "en_US_POSIX")
        df.dateFormat = "yyyyMMdd-HHmmss-SSS"
        return df.date(from: String(prefix))
    }

    private static func fallbackFilesystemDate(_ url: URL) -> Date {
        let vals = try? url.resourceValues(forKeys: [.creationDateKey, .contentModificationDateKey])
        return vals?.creationDate ?? vals?.contentModificationDate ?? Date()
    }

    private static func parseCsvStats(folderURL: URL, stem: String) -> (frameCount: Int, durationSec: Double) {
        let csvURL = folderURL.appendingPathComponent("\(stem)_timestamps.csv")
        guard let text = try? String(contentsOf: csvURL, encoding: .utf8) else {
            return (0, 0.0)
        }
        let lines = text.split(whereSeparator: \.isNewline).dropFirst() // drop header
        guard !lines.isEmpty else { return (0, 0.0) }
        let count = lines.count
        var firstNs: UInt64?
        var lastNs: UInt64?
        for line in lines {
            let cols = line.split(separator: ",")
            if cols.count >= 4, let ns = UInt64(cols[3]) {
                if firstNs == nil { firstNs = ns }
                lastNs = ns
            }
        }
        if let f = firstNs, let l = lastNs, l > f {
            let span = Double(l - f) / 1e9 + (1.0 / 30.0)
            return (count, span)
        }
        return (count, Double(count) / 30.0)
    }
}

/// Random-access reader for 16-bit lossless Depth and IR recordings (`.mkv` FFV1 and 16-bit PNG sequence)
/// supporting live-rate playback, pause, timeline scrubbing, and single-frame stepping off the main actor.
final class RecordingSessionReader: @unchecked Sendable {
    private static let decodeQueueKey = DispatchSpecificKey<UInt8>()
    private let decodeQueue = DispatchQueue(label: "kinectviewer.replay.decode", qos: .userInitiated)
    private let prefetchQueue = DispatchQueue(label: "kinectviewer.replay.prefetch", qos: .utility)
    private let cacheLock = NSLock()

    let session: RecordingSessionItem
    let totalFrames: Int
    let chunkSize: Int
    private let frameTimestampsNs: [UInt64]
    private let frameDeviceTicks: [UInt32]

    // Bounded active + prefetched chunk cache for FFV1 .mkv random access (guarded by cacheLock)
    private var cachedChunkIndex: Int = -1
    private var cachedChunkFrames: [[UInt16]] = []
    private var prefetchedChunkIndex: Int = -1
    private var prefetchedChunkFrames: [[UInt16]] = []
    private var inFlightPrefetchChunkIndex: Int = -1
    private var wantedPrefetchChunkIndex: Int = -1
    private var waitingOnDemandRequests: Int = 0
    var simulatedPrefetchDelayMsForTesting: UInt32 = 0
    private let graySpace = CGColorSpaceCreateDeviceGray()

    init(session: RecordingSessionItem, chunkSize: Int = 180) {
        self.session = session
        self.chunkSize = max(1, chunkSize)
        self.decodeQueue.setSpecific(key: Self.decodeQueueKey, value: 1)

        let csvURL = session.folderURL.appendingPathComponent("\(session.modeSlug)_timestamps.csv")
        var tsList: [UInt64] = []
        var tickList: [UInt32] = []
        if let text = try? String(contentsOf: csvURL, encoding: .utf8) {
            for line in text.split(whereSeparator: \.isNewline).dropFirst() {
                let cols = line.split(separator: ",")
                if cols.count >= 4 {
                    tickList.append(UInt32(cols[2]) ?? 0)
                    tsList.append(UInt64(cols[3]) ?? 0)
                }
            }
        }
        self.frameTimestampsNs = tsList
        self.frameDeviceTicks = tickList
        self.totalFrames = max(session.frameCount, tsList.count)
    }

    private func syncOnDecodeQueue<T>(_ body: () -> T) -> T {
        if DispatchQueue.getSpecific(key: Self.decodeQueueKey) != nil {
            return body()
        }
        return decodeQueue.sync(execute: body)
    }

    func deviceTicks(forFrame index: Int) -> UInt32 {
        if index >= 0 && index < frameDeviceTicks.count {
            return frameDeviceTicks[index]
        }
        return UInt32(index * 267)
    }

    func frameIntervalSeconds(from index: Int) -> Double {
        if index >= 0 && (index + 1) < frameTimestampsNs.count {
            let t0 = frameTimestampsNs[index]
            let t1 = frameTimestampsNs[index + 1]
            if t1 > t0 {
                let dt = Double(t1 - t0) / 1e9
                return min(0.25, max(1.0 / 120.0, dt))
            }
        }
        return 1.0 / 30.0
    }

    func readFrame16(at index: Int) -> [UInt16]? {
        cacheLock.lock()
        waitingOnDemandRequests += 1
        cacheLock.unlock()
        return syncOnDecodeQueue {
            defer {
                cacheLock.lock()
                waitingOnDemandRequests = max(0, waitingOnDemandRequests - 1)
                cacheLock.unlock()
            }
            return readFrame16OnQueue(at: index)
        }
    }

    func readFrame16Async(
        at index: Int,
        prefetchNextChunk: Bool = false,
        completion: @escaping @Sendable ([UInt16]?) -> Void
    ) {
        cacheLock.lock()
        waitingOnDemandRequests += 1
        cacheLock.unlock()

        decodeQueue.async { [weak self] in
            guard let self = self else {
                completion(nil)
                return
            }
            let pixels = self.readFrame16OnQueue(at: index)
            self.cacheLock.lock()
            self.waitingOnDemandRequests = max(0, self.waitingOnDemandRequests - 1)
            let hasWaitingRequest = self.waitingOnDemandRequests > 0
            self.cacheLock.unlock()

            completion(pixels)

            if prefetchNextChunk, !hasWaitingRequest, self.session.container != "png16" {
                self.scheduleNextChunkPrefetchIfNeeded(currentFrameIndex: index)
            }
        }
    }

    private func scheduleNextChunkPrefetchIfNeeded(currentFrameIndex index: Int) {
        let targetChunk = index / chunkSize
        let offsetInChunk = index - targetChunk * chunkSize
        let nextChunk = targetChunk + 1
        guard offsetInChunk >= (chunkSize / 2),
              nextChunk * chunkSize < totalFrames else {
            return
        }

        cacheLock.lock()
        wantedPrefetchChunkIndex = nextChunk
        let shouldStart = waitingOnDemandRequests == 0 &&
            cachedChunkIndex != nextChunk &&
            prefetchedChunkIndex != nextChunk &&
            inFlightPrefetchChunkIndex != nextChunk
        if shouldStart {
            inFlightPrefetchChunkIndex = nextChunk
        }
        cacheLock.unlock()

        guard shouldStart else { return }

        prefetchQueue.async { [weak self] in
            guard let self = self else { return }
            self.cacheLock.lock()
            let stillNeeded = self.wantedPrefetchChunkIndex == nextChunk &&
                self.cachedChunkIndex != nextChunk &&
                self.prefetchedChunkIndex != nextChunk
            let testDelayMs = self.simulatedPrefetchDelayMsForTesting
            if !stillNeeded {
                if self.inFlightPrefetchChunkIndex == nextChunk {
                    self.inFlightPrefetchChunkIndex = -1
                }
                self.cacheLock.unlock()
                return
            }
            self.cacheLock.unlock()

            if testDelayMs > 0 {
                usleep(testDelayMs * 1_000)
            }

            let decoded = self.decodeFfv1ChunkFrames(chunkIndex: nextChunk)

            self.cacheLock.lock()
            if self.inFlightPrefetchChunkIndex == nextChunk {
                self.inFlightPrefetchChunkIndex = -1
            }
            if let nextFrames = decoded, self.cachedChunkIndex != nextChunk {
                self.prefetchedChunkIndex = nextChunk
                self.prefetchedChunkFrames = nextFrames
            }
            self.cacheLock.unlock()
        }
    }

    func waitForIdleForTesting() async {
        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            decodeQueue.async {
                cont.resume()
            }
        }
        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            prefetchQueue.async {
                cont.resume()
            }
        }
    }

    private func readFrame16OnQueue(at index: Int) -> [UInt16]? {
        guard index >= 0 && index < totalFrames else { return nil }
        if session.container == "png16" {
            return readPng16FrameOnQueue(at: index)
        } else {
            return readFfv1FrameOnQueue(at: index)
        }
    }

    private func readPng16FrameOnQueue(at index: Int) -> [UInt16]? {
        let pngURL = session.folderURL.appendingPathComponent(String(format: "%@_%06d.png", session.modeSlug, index))
        guard let src = CGImageSourceCreateWithURL(pngURL as CFURL, nil),
              let cgImg = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            return nil
        }
        let w = session.width
        let h = session.height
        var out = [UInt16](repeating: 0, count: w * h)
        let ok = out.withUnsafeMutableBytes { rawPtr -> Bool in
            guard let base = rawPtr.baseAddress,
                  let ctx = CGContext(
                      data: base,
                      width: w,
                      height: h,
                      bitsPerComponent: 16,
                      bytesPerRow: w * 2,
                      space: graySpace,
                      bitmapInfo: CGBitmapInfo.byteOrder16Little.rawValue
                  ) else {
                return false
            }
            ctx.interpolationQuality = .none
            ctx.draw(cgImg, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        return ok ? out : nil
    }

    private func readFfv1FrameOnQueue(at index: Int) -> [UInt16]? {
        let targetChunk = index / chunkSize
        let offset = index - targetChunk * chunkSize

        cacheLock.lock()
        if targetChunk == cachedChunkIndex {
            let hit = (offset >= 0 && offset < cachedChunkFrames.count) ? cachedChunkFrames[offset] : nil
            cacheLock.unlock()
            return hit
        }
        if targetChunk == prefetchedChunkIndex && !prefetchedChunkFrames.isEmpty {
            cachedChunkIndex = prefetchedChunkIndex
            cachedChunkFrames = prefetchedChunkFrames
            prefetchedChunkIndex = -1
            prefetchedChunkFrames = []
            let hit = (offset >= 0 && offset < cachedChunkFrames.count) ? cachedChunkFrames[offset] : nil
            cacheLock.unlock()
            return hit
        }
        cacheLock.unlock()

        guard let decoded = decodeFfv1ChunkFrames(chunkIndex: targetChunk) else { return nil }
        cacheLock.lock()
        cachedChunkIndex = targetChunk
        cachedChunkFrames = decoded
        let hit = (offset >= 0 && offset < cachedChunkFrames.count) ? cachedChunkFrames[offset] : nil
        cacheLock.unlock()
        return hit
    }

    static func makeFfv1DecodeArguments(mkvPath: String, startFrame: Int, frameCount: Int) -> [String] {
        var args: [String] = [
            "-nostdin",
            "-hide_banner",
            "-loglevel", "error",
        ]
        if startFrame > 0 {
            // Matroska container timestamps use 1 ms ticks (round(frame * 1000 / 30) ms) and FFV1 is all-intra (-g 1);
            // input -ss seeks directly in the container index without decoding from frame 0.
            let roundedMs = (Double(startFrame) * 1000.0 / 30.0).rounded()
            let seekSec = max(0.0, (roundedMs / 1000.0) - 0.0004)
            args += ["-ss", String(format: "%.6f", seekSec)]
        }
        args += [
            "-i", mkvPath,
            "-frames:v", "\(frameCount)",
            "-fps_mode", "passthrough",
            "-f", "rawvideo",
            "-pix_fmt", "gray16le",
            "pipe:1",
        ]
        return args
    }

    private func decodeFfv1ChunkFrames(chunkIndex: Int) -> [[UInt16]]? {
        guard let ffmpegPath = HostRecorder.resolveFFmpegBinary() else { return nil }
        let mkvURL = session.folderURL.appendingPathComponent("\(session.modeSlug)_lossless.mkv")
        guard FileManager.default.fileExists(atPath: mkvURL.path) else { return nil }

        let startFrame = chunkIndex * chunkSize
        let endFrame = min(totalFrames - 1, startFrame + chunkSize - 1)
        guard startFrame <= endFrame else { return nil }
        let frameCount = endFrame - startFrame + 1

        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: ffmpegPath)
        proc.arguments = Self.makeFfv1DecodeArguments(
            mkvPath: mkvURL.path,
            startFrame: startFrame,
            frameCount: frameCount
        )

        let outPipe = Pipe()
        proc.standardInput = FileHandle.nullDevice
        proc.standardOutput = outPipe
        proc.standardError = FileHandle.nullDevice

        do {
            try proc.run()
        } catch {
            return nil
        }

        // Read pipe to EOF BEFORE waitProcess so ffmpeg never blocks on a full OS pipe buffer!
        let rawData = outPipe.fileHandleForReading.readDataToEndOfFile()
        _ = HostRecorder.waitProcess(proc, timeoutSec: 10.0)

        let pxPerFrame = session.width * session.height
        let bytesPerFrame = pxPerFrame * 2
        guard bytesPerFrame > 0, rawData.count >= bytesPerFrame else { return nil }

        let decodedFramesCount = rawData.count / bytesPerFrame
        var frames: [[UInt16]] = []
        frames.reserveCapacity(decodedFramesCount)

        rawData.withUnsafeBytes { rawBuf in
            guard let baseU16 = rawBuf.bindMemory(to: UInt16.self).baseAddress else { return }
            for f in 0..<decodedFramesCount {
                let startPtr = baseU16.advanced(by: f * pxPerFrame)
                frames.append(Array(UnsafeBufferPointer(start: startPtr, count: pxPerFrame)))
            }
        }

        return frames.isEmpty ? nil : frames
    }
}
