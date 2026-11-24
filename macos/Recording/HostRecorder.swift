import CoreGraphics
import Darwin
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum Lossless16ContainerOption: Int, CaseIterable, Identifiable, Sendable {
    case ffv1Mkv = 0
    case png16Sequence = 1

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .ffv1Mkv: return "FFV1 16-bit Lossless (.mkv)"
        case .png16Sequence: return "16-bit PNG Sequence"
        }
    }
}

enum HostRecorderError: LocalizedError, Equatable, Sendable {
    case colourModeForbidden
    case realSensorForbiddenInTestMode

    static let colourLiveOnlyTooltip = "Colour is live only. Switch to IR or Depth to record."
    static let realSensorInXCTestTooltip = "Real sensor source is forbidden when XCTestConfigurationFilePath is set (synthetic only)."

    var errorDescription: String? {
        switch self {
        case .colourModeForbidden:
            return Self.colourLiveOnlyTooltip
        case .realSensorForbiddenInTestMode:
            return Self.realSensorInXCTestTooltip
        }
    }
}

struct RecordingStatusSnapshot: Sendable {
    var isRecording: Bool = false
    var formatSummary: String = "Idle"
    var sessionFolderPath: String = ""
    var framesWritten: UInt64 = 0
    var droppedFrames: UInt64 = 0
    var bytesOnDisk: UInt64 = 0
    var elapsedSec: Double = 0.0
    var megabytesPerMinute: Double = 0.0
    var lastError: String?
}

struct DirectoryExclusionAudit: Sendable {
    let basePath: String
    let exists: Bool
    let hasNoSyncFile: Bool
    let hasMetadataNeverIndex: Bool
    let resourceExcludedFromBackup: Bool
    let tmutilExcluded: Bool
}

/// Host-only recorder writing strictly 16-bit IR and Depth into `~/kinect-viewer-recordings/`
/// with iCloud, Time Machine, and Spotlight exclusions.
/// Colour frames are strictly live-only in memory and can never be written to disk.
final class HostRecorder: @unchecked Sendable {
    static let defaultBaseDirectoryPath = (NSHomeDirectory() as NSString).appendingPathComponent("kinect-viewer-recordings")

    private static let ioQueueKey = DispatchSpecificKey<UInt8>()
    let baseDirURL: URL
    let isRunningXCTest: Bool
    private let skipTimeMachineSubprocess: Bool
    private let ioQueue = DispatchQueue(label: "kinectviewer.host.recorder", qos: .userInitiated)
    private let stateLock = NSLock()

    private var activeBackend: RecorderBackend?
    private var snapshot = RecordingStatusSnapshot()
    private var inFlightFrames: Int = 0
    private let maxInFlightFrames = 12

    static func canRecord(mode: kv_view_mode_t) -> Bool {
        mode == KV_MODE_IR || mode == KV_MODE_DEPTH
    }

    init(
        baseDirectoryURL: URL? = nil,
        skipTimeMachineSubprocess: Bool = false,
        isRunningXCTestOverride: Bool? = nil
    ) {
        signal(SIGPIPE, SIG_IGN)
        ioQueue.setSpecific(key: Self.ioQueueKey, value: 1)
        let runningXCTest = isRunningXCTestOverride ?? (ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil)
        self.isRunningXCTest = runningXCTest
        if let explicitURL = baseDirectoryURL {
            self.baseDirURL = explicitURL
            self.skipTimeMachineSubprocess = skipTimeMachineSubprocess
        } else if runningXCTest {
            // Never touch ~/kinect-viewer-recordings/ during automated unit test runs.
            self.baseDirURL = FileManager.default.temporaryDirectory
                .appendingPathComponent("kinect-viewer-xctest-recordings-\(ProcessInfo.processInfo.processIdentifier)", isDirectory: true)
            self.skipTimeMachineSubprocess = true
        } else {
            self.baseDirURL = URL(fileURLWithPath: Self.defaultBaseDirectoryPath, isDirectory: true)
            self.skipTimeMachineSubprocess = skipTimeMachineSubprocess
        }
        _ = try? ensureExcludedRecordingsDirectory()
    }

    private func syncOnIOQueue<T>(_ body: () -> T) -> T {
        if DispatchQueue.getSpecific(key: Self.ioQueueKey) != nil {
            return body()
        }
        return ioQueue.sync(execute: body)
    }

    var currentStatus: RecordingStatusSnapshot {
        stateLock.lock()
        defer { stateLock.unlock() }
        return snapshot
    }

    // MARK: - Triple Exclusion Setup & Verification (iCloud + Time Machine + Spotlight)

    @discardableResult
    func ensureExcludedRecordingsDirectory() throws -> DirectoryExclusionAudit {
        let fm = FileManager.default
        if !fm.fileExists(atPath: baseDirURL.path) {
            try fm.createDirectory(at: baseDirURL, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        } else {
            try? fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: baseDirURL.path)
        }

        // 1. iCloud exclusion marker (.nosync)
        let noSyncURL = baseDirURL.appendingPathComponent(".nosync")
        if !fm.fileExists(atPath: noSyncURL.path) {
            fm.createFile(atPath: noSyncURL.path, contents: Data(), attributes: [.posixPermissions: 0o600])
        }

        // 2. Spotlight exclusion marker (.metadata_never_index)
        let neverIndexURL = baseDirURL.appendingPathComponent(".metadata_never_index")
        if !fm.fileExists(atPath: neverIndexURL.path) {
            fm.createFile(atPath: neverIndexURL.path, contents: Data(), attributes: [.posixPermissions: 0o600])
        }

        // 3. Time Machine + Backup exclusion (sticky xattr via NSURLIsExcludedFromBackupKey + tmutil addexclusion)
        var mutableURL = baseDirURL
        var resourceValues = URLResourceValues()
        resourceValues.isExcludedFromBackup = true
        try? mutableURL.setResourceValues(resourceValues)

        if !skipTimeMachineSubprocess && FileManager.default.isExecutableFile(atPath: "/usr/bin/tmutil") {
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: "/usr/bin/tmutil")
            proc.arguments = ["addexclusion", baseDirURL.path]
            proc.standardInput = FileHandle.nullDevice
            proc.standardOutput = FileHandle.nullDevice
            proc.standardError = FileHandle.nullDevice
            try? proc.run()
            _ = Self.waitProcess(proc, timeoutSec: 3.0)
        }

        return auditExclusions()
    }

    func auditExclusions() -> DirectoryExclusionAudit {
        let fm = FileManager.default
        let exists = fm.fileExists(atPath: baseDirURL.path)
        let hasNoSync = fm.fileExists(atPath: baseDirURL.appendingPathComponent(".nosync").path)
        let hasNeverIndex = fm.fileExists(atPath: baseDirURL.appendingPathComponent(".metadata_never_index").path)

        let vals = try? baseDirURL.resourceValues(forKeys: [.isExcludedFromBackupKey])
        let resourceExcluded = vals?.isExcludedFromBackup ?? false

        var tmExcluded = false
        if exists && !skipTimeMachineSubprocess && fm.isExecutableFile(atPath: "/usr/bin/tmutil") {
            let pipe = Pipe()
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: "/usr/bin/tmutil")
            proc.arguments = ["isexcluded", baseDirURL.path]
            proc.standardInput = FileHandle.nullDevice
            proc.standardOutput = pipe
            proc.standardError = FileHandle.nullDevice
            try? proc.run()
            _ = Self.waitProcess(proc, timeoutSec: 3.0)
            let outData = pipe.fileHandleForReading.readDataToEndOfFile()
            let str = String(decoding: outData, as: UTF8.self)
            tmExcluded = str.contains("[Excluded]")
        }

        return DirectoryExclusionAudit(
            basePath: baseDirURL.path,
            exists: exists,
            hasNoSyncFile: hasNoSync,
            hasMetadataNeverIndex: hasNeverIndex,
            resourceExcludedFromBackup: resourceExcluded,
            tmutilExcluded: tmExcluded
        )
    }

    // MARK: - Start / Stop / Feed 16-Bit IR & Depth Frames Only

    func startRecording(
        source: kv_source_kind_t = KV_SOURCE_SYNTHETIC,
        mode: kv_view_mode_t,
        width: Int,
        height: Int,
        losslessOption: Lossless16ContainerOption = .ffv1Mkv,
        irIntrinsics: kv_ir_intrinsics_t? = nil
    ) throws -> URL {
        // When XCTestConfigurationFilePath is set, never allow recording from the real sensor to $TMPDIR.
        if isRunningXCTest && source == KV_SOURCE_REAL {
            throw HostRecorderError.realSensorForbiddenInTestMode
        }
        // Colour is live-only; reject any colour-carrying or non-IR/Depth mode before creating any directory.
        guard Self.canRecord(mode: mode) else {
            throw HostRecorderError.colourModeForbidden
        }

        stopRecording()
        _ = try ensureExcludedRecordingsDirectory()

        let createdAt = Date()
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss-SSS"
        let stamp = formatter.string(from: createdAt)

        let modeSlug = (mode == KV_MODE_IR) ? "ir16" : "depth16mm"

        // Session directory also carries `.nosync` suffix as defense-in-depth
        let sessionDir = baseDirURL.appendingPathComponent("\(stamp)_\(modeSlug).nosync", isDirectory: true)
        try FileManager.default.createDirectory(at: sessionDir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        var mutSession = sessionDir
        var rv = URLResourceValues()
        rv.isExcludedFromBackup = true
        try? mutSession.setResourceValues(rv)

        let backend: RecorderBackend
        let summary: String
        let containerTag: String
        let w = width > 0 ? width : 512
        let h = height > 0 ? height : 424

        if losslessOption == .ffv1Mkv, let ffmpegPath = Self.resolveFFmpegBinary() {
            backend = try FFV1Gray16Backend(
                ffmpegPath: ffmpegPath,
                sessionDir: sessionDir,
                stem: modeSlug,
                width: w,
                height: h
            )
            containerTag = "ffv1"
            summary = mode == KV_MODE_IR ? "16-bit IR FFV1 Lossless (.mkv)" : "16-bit Depth (mm) FFV1 Lossless (.mkv)"
        } else {
            backend = try PNG16SequenceBackend(sessionDir: sessionDir, stem: modeSlug, width: w, height: h)
            containerTag = "png16"
            summary = mode == KV_MODE_IR ? "16-bit IR PNG Sequence" : "16-bit Depth (mm) PNG Sequence"
        }

        backend.createdAt = createdAt
        backend.modeSlug = modeSlug
        backend.containerTag = containerTag
        backend.frameWidth = w
        backend.frameHeight = h
        backend.irIntrinsics = irIntrinsics
        Self.writeSessionMetadata(backend: backend, elapsedSec: 0.0)

        let startNs = kv_monotonic_ns()
        syncOnIOQueue {
            self.activeBackend = backend
            self.stateLock.lock()
            self.inFlightFrames = 0
            self.snapshot = RecordingStatusSnapshot(
                isRecording: true,
                formatSummary: summary,
                sessionFolderPath: sessionDir.path,
                framesWritten: 0,
                droppedFrames: 0,
                bytesOnDisk: 0,
                elapsedSec: 0.0,
                megabytesPerMinute: 0.0,
                lastError: nil
            )
            self.stateLock.unlock()
            backend.startNs = startNs
        }
        return sessionDir
    }

    @discardableResult
    func stopRecording() -> RecordingStatusSnapshot {
        syncOnIOQueue {
            guard let backend = self.activeBackend else { return }
            self.activeBackend = nil
            _ = backend.finishAndClose()
            let endNs = kv_monotonic_ns()
            let elapsed = endNs > backend.startNs ? Double(endNs - backend.startNs) / 1e9 : 0.0
            Self.writeSessionMetadata(backend: backend, elapsedSec: elapsed)
            let finalBytes = Self.directorySizeBytes(at: backend.sessionDir)
            let mbPerMin = elapsed > 0.05 ? (Double(finalBytes) / 1_000_000.0) * (60.0 / elapsed) : 0.0

            self.stateLock.lock()
            self.snapshot.isRecording = false
            self.snapshot.bytesOnDisk = finalBytes
            self.snapshot.elapsedSec = elapsed
            self.snapshot.megabytesPerMinute = mbPerMin
            self.stateLock.unlock()
        }
        return currentStatus
    }

    func appendRaw16Frame(
        pixelsU16: [UInt16],
        width: Int,
        height: Int,
        seq: UInt64,
        deviceTicks: UInt32,
        arrivalNs: UInt64,
        source: kv_source_kind_t = KV_SOURCE_SYNTHETIC
    ) {
        if isRunningXCTest && source == KV_SOURCE_REAL {
            return
        }
        stateLock.lock()
        guard snapshot.isRecording else {
            stateLock.unlock()
            return
        }
        if inFlightFrames >= maxInFlightFrames {
            snapshot.droppedFrames += 1
            stateLock.unlock()
            return
        }
        inFlightFrames += 1
        stateLock.unlock()

        ioQueue.async { [weak self] in
            guard let self = self else { return }
            defer {
                self.stateLock.lock()
                self.inFlightFrames = max(0, self.inFlightFrames - 1)
                self.stateLock.unlock()
            }
            guard let backend = self.activeBackend else { return }
            do {
                try backend.writeRaw16(
                    pixelsU16: pixelsU16,
                    width: width,
                    height: height,
                    seq: seq,
                    deviceTicks: deviceTicks,
                    arrivalNs: arrivalNs
                )
                self.updateProgressOnQueue(backend: backend)
            } catch {
                self.recordErrorOnQueue(error.localizedDescription)
            }
        }
    }

    private func updateProgressOnQueue(backend: RecorderBackend) {
        backend.framesWritten += 1
        let nowNs = kv_monotonic_ns()
        // Stat directory bytes periodically (~6x/sec) to avoid unnecessary syscall overhead
        if backend.framesWritten == 1 || (backend.framesWritten % 5) == 0 {
            backend.cachedBytes = Self.directorySizeBytes(at: backend.sessionDir)
        }
        let elapsed = nowNs > backend.startNs ? Double(nowNs - backend.startNs) / 1e9 : 0.0
        let mbPerMin = elapsed > 0.25 ? (Double(backend.cachedBytes) / 1_000_000.0) * (60.0 / elapsed) : 0.0

        stateLock.lock()
        snapshot.framesWritten = backend.framesWritten
        snapshot.bytesOnDisk = backend.cachedBytes
        snapshot.elapsedSec = elapsed
        snapshot.megabytesPerMinute = mbPerMin
        stateLock.unlock()
    }

    private func recordErrorOnQueue(_ msg: String) {
        stateLock.lock()
        snapshot.lastError = msg
        stateLock.unlock()
    }

    static func resolveFFmpegBinary() -> String? {
        let candidates = ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"]
        for path in candidates where FileManager.default.isExecutableFile(atPath: path) {
            return path
        }
        return nil
    }

    @discardableResult
    static func waitProcess(_ proc: Process, timeoutSec: Double) -> Bool {
        let deadline = Date().addingTimeInterval(timeoutSec)
        while proc.isRunning && Date() < deadline {
            usleep(10_000)
        }
        if proc.isRunning {
            proc.terminate()
            usleep(100_000)
            if proc.isRunning {
                kill(proc.processIdentifier, SIGKILL)
            }
            return false
        }
        return true
    }

    static func directorySizeBytes(at dirURL: URL) -> UInt64 {
        guard let items = try? FileManager.default.contentsOfDirectory(
            at: dirURL,
            includingPropertiesForKeys: [.isSymbolicLinkKey, .fileSizeKey, .isRegularFileKey],
            options: []
        ) else {
            return 0
        }
        var total: UInt64 = 0
        for item in items {
            if let vals = try? item.resourceValues(forKeys: [.isSymbolicLinkKey, .isRegularFileKey, .fileSizeKey]),
               vals.isSymbolicLink != true,
               vals.isRegularFile == true,
               let sz = vals.fileSize {
                total += UInt64(max(0, sz))
            }
        }
        return total
    }

    private static func writeSessionMetadata(backend: RecorderBackend, elapsedSec: Double) {
        let iso = ISO8601DateFormatter()
        let intrRecord: RecordedIrIntrinsics? = {
            guard let intr = backend.irIntrinsics, intr.valid else { return nil }
            return RecordedIrIntrinsics(from: intr)
        }()
        let meta = RecordingSessionMetadata(
            createdAtISO8601: iso.string(from: backend.createdAt),
            modeSlug: backend.modeSlug,
            container: backend.containerTag,
            width: backend.frameWidth,
            height: backend.frameHeight,
            frameCount: Int(backend.framesWritten),
            durationSec: elapsedSec,
            irIntrinsics: intrRecord
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(meta) else { return }
        let url = backend.sessionDir.appendingPathComponent("session_meta.json")
        try? data.write(to: url, options: .atomic)
        try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }
}

// MARK: - 16-Bit Depth & IR Backend Implementations Only

private class RecorderBackend {
    let sessionDir: URL
    var createdAt: Date = Date()
    var modeSlug: String = "ir16"
    var containerTag: String = "ffv1"
    var frameWidth: Int = 512
    var frameHeight: Int = 424
    var irIntrinsics: kv_ir_intrinsics_t?
    var startNs: UInt64 = 0
    var framesWritten: UInt64 = 0
    var cachedBytes: UInt64 = 0

    init(sessionDir: URL) {
        self.sessionDir = sessionDir
    }

    func writeRaw16(pixelsU16: [UInt16], width: Int, height: Int, seq: UInt64, deviceTicks: UInt32, arrivalNs: UInt64) throws {}
    func finishAndClose() -> UInt64 { HostRecorder.directorySizeBytes(at: sessionDir) }
}

private final class FFV1Gray16Backend: RecorderBackend {
    private let process: Process
    private let stdinPipe: Pipe
    private let csvHandle: FileHandle
    private let mkvURL: URL

    init(ffmpegPath: String, sessionDir: URL, stem: String, width: Int, height: Int) throws {
        self.mkvURL = sessionDir.appendingPathComponent("\(stem)_lossless.mkv")
        let csvURL = sessionDir.appendingPathComponent("\(stem)_timestamps.csv")
        FileManager.default.createFile(
            atPath: csvURL.path,
            contents: Data("frame_index,engine_seq,device_ticks,arrival_mach_ns\n".utf8),
            attributes: [.posixPermissions: 0o600]
        )
        self.csvHandle = try FileHandle(forWritingTo: csvURL)
        try self.csvHandle.seekToEnd()

        self.stdinPipe = Pipe()
        self.process = Process()
        process.executableURL = URL(fileURLWithPath: ffmpegPath)
        process.arguments = [
            "-nostdin",
            "-hide_banner",
            "-loglevel", "error",
            "-y",
            "-f", "rawvideo",
            "-pixel_format", "gray16le",
            "-video_size", "\(width)x\(height)",
            "-framerate", "30",
            "-i", "pipe:0",
            "-c:v", "ffv1",
            "-level", "3",
            "-coder", "1",
            "-context", "1",
            "-g", "1",
            "-slicecrc", "1",
            mkvURL.path
        ]
        process.standardInput = stdinPipe
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        try process.run()
        super.init(sessionDir: sessionDir)
    }

    override func writeRaw16(pixelsU16: [UInt16], width: Int, height: Int, seq: UInt64, deviceTicks: UInt32, arrivalNs: UInt64) throws {
        let data = pixelsU16.withUnsafeBufferPointer { buf in
            Data(buffer: buf)
        }
        try stdinPipe.fileHandleForWriting.write(contentsOf: data)
        let line = "\(framesWritten),\(seq),\(deviceTicks),\(arrivalNs)\n"
        if let lineData = line.data(using: .utf8) {
            try? csvHandle.write(contentsOf: lineData)
        }
    }

    override func finishAndClose() -> UInt64 {
        try? stdinPipe.fileHandleForWriting.close()
        _ = HostRecorder.waitProcess(process, timeoutSec: 8.0)
        try? csvHandle.close()
        return HostRecorder.directorySizeBytes(at: sessionDir)
    }
}

private final class PNG16SequenceBackend: RecorderBackend {
    private let width: Int
    private let height: Int
    private let stem: String
    private let csvHandle: FileHandle
    private let graySpace = CGColorSpaceCreateDeviceGray()

    init(sessionDir: URL, stem: String, width: Int, height: Int) throws {
        self.width = width
        self.height = height
        self.stem = stem
        let csvURL = sessionDir.appendingPathComponent("\(stem)_timestamps.csv")
        FileManager.default.createFile(
            atPath: csvURL.path,
            contents: Data("frame_index,engine_seq,device_ticks,arrival_mach_ns\n".utf8),
            attributes: [.posixPermissions: 0o600]
        )
        self.csvHandle = try FileHandle(forWritingTo: csvURL)
        try self.csvHandle.seekToEnd()
        super.init(sessionDir: sessionDir)
    }

    override func writeRaw16(pixelsU16: [UInt16], width: Int, height: Int, seq: UInt64, deviceTicks: UInt32, arrivalNs: UInt64) throws {
        let rawBytes = pixelsU16.withUnsafeBufferPointer { Data(buffer: $0) } as CFData
        guard let provider = CGDataProvider(data: rawBytes),
              let cgImage = CGImage(
                  width: width,
                  height: height,
                  bitsPerComponent: 16,
                  bitsPerPixel: 16,
                  bytesPerRow: width * 2,
                  space: graySpace,
                  bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder16Little.rawValue),
                  provider: provider,
                  decode: nil,
                  shouldInterpolate: false,
                  intent: .defaultIntent
              ) else {
            throw NSError(domain: "HostRecorder", code: 10, userInfo: [NSLocalizedDescriptionKey: "Failed to construct 16-bit CGImage."])
        }

        let fileURL = sessionDir.appendingPathComponent(String(format: "%@_%06llu.png", stem, framesWritten))
        guard let dest = CGImageDestinationCreateWithURL(fileURL as CFURL, UTType.png.identifier as CFString, 1, nil) else {
            throw NSError(domain: "HostRecorder", code: 11, userInfo: [NSLocalizedDescriptionKey: "Failed to create PNG destination."])
        }
        CGImageDestinationAddImage(dest, cgImage, nil)
        guard CGImageDestinationFinalize(dest) else {
            throw NSError(domain: "HostRecorder", code: 12, userInfo: [NSLocalizedDescriptionKey: "Failed to finalize 16-bit PNG."])
        }

        let line = "\(framesWritten),\(seq),\(deviceTicks),\(arrivalNs)\n"
        if let lineData = line.data(using: .utf8) {
            try? csvHandle.write(contentsOf: lineData)
        }
    }

    override func finishAndClose() -> UInt64 {
        try? csvHandle.close()
        return HostRecorder.directorySizeBytes(at: sessionDir)
    }
}
