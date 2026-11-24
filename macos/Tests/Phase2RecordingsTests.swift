import CoreGraphics
import Foundation
import XCTest
@testable import KinectViewer

@MainActor
final class Phase2RecordingsTests: XCTestCase {
    func testPreRecordingReminderPolicyDefaultCancelAndOneHourSnooze() throws {
        XCTAssertEqual(
            PreRecordingReminderPolicy.exactReminderText,
            "Only you in view. Not pointed at a window. Depth and IR only. Deleted after 7 days."
        )

        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase2-reminder-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        var fakeNow = Date(timeIntervalSince1970: 1_800_000_000)
        let controller = HostController(
            recordingsDirectoryURL: tempRoot.appendingPathComponent("recordings", isDirectory: true),
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }
        controller.nowProvider = { fakeNow }
        controller.useSyntheticSource = true
        controller.selectedMode = .depth
        controller.startStream()
        XCTAssertTrue(controller.isRunning)

        // 1. Requesting recording opens reminder dialog and does NOT start recording yet
        controller.requestToggleRecordingWithReminder()
        XCTAssertTrue(controller.showRecordingReminderDialog)
        XCTAssertFalse(controller.isRecording)

        // 2. Default action Cancel dismisses dialog without starting recording
        controller.cancelRecordingReminder()
        XCTAssertFalse(controller.showRecordingReminderDialog)
        XCTAssertFalse(controller.isRecording)

        // 3. Confirming without 1-hour snooze starts recording, then prompts again next time
        controller.requestToggleRecordingWithReminder()
        XCTAssertTrue(controller.showRecordingReminderDialog)
        controller.recordingReminderDontAskOneHour = false
        controller.confirmRecordingReminderAndStart()
        XCTAssertFalse(controller.showRecordingReminderDialog)
        XCTAssertTrue(controller.isRecording)
        controller.stopHostRecording()
        XCTAssertFalse(controller.isRecording)

        // 4. Confirming WITH "don't remind me for 1 hour" suppresses prompt within 3600s, then re-arms after 1h
        controller.requestToggleRecordingWithReminder()
        XCTAssertTrue(controller.showRecordingReminderDialog)
        controller.recordingReminderDontAskOneHour = true
        controller.confirmRecordingReminderAndStart()
        XCTAssertTrue(controller.isRecording)
        controller.stopHostRecording()

        // +30 minutes: starts immediately without showing dialog
        fakeNow = fakeNow.addingTimeInterval(1800)
        controller.requestToggleRecordingWithReminder()
        XCTAssertFalse(controller.showRecordingReminderDialog)
        XCTAssertTrue(controller.isRecording)
        controller.stopHostRecording()

        // +60 minutes + 1 second from confirmation: reminder dialog is required again
        fakeNow = fakeNow.addingTimeInterval(1801)
        controller.requestToggleRecordingWithReminder()
        XCTAssertTrue(controller.showRecordingReminderDialog)
        XCTAssertFalse(controller.isRecording)
        controller.cancelRecordingReminder()
    }

    func testRecordAndReplayDepthFFV1AndIrPNG16ThroughLivePipeline() async throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase2-replay-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("recordings", isDirectory: true)
        try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let controller = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        controller.useSyntheticSource = true
        controller.selectedMode = .depth
        controller.startStream()
        XCTAssertTrue(controller.isRunning)

        let intr = kv_ir_intrinsics_t(
            valid: true, fx: 365.456, fy: 365.456, cx: 254.878, cy: 205.395,
            k1: 0.0, k2: 0.0, k3: 0.0, p1: 0.0, p2: 0.0
        )

        // Record a 5-frame synthetic 16-bit Depth session using FFV1 (.mkv)
        _ = try controller.recorder.startRecording(
            mode: KV_MODE_DEPTH,
            width: 512,
            height: 424,
            losslessOption: .ffv1Mkv,
            irIntrinsics: intr
        )
        for f in 0..<5 {
            var px = [UInt16](repeating: UInt16(1200 + f * 100), count: 512 * 424)
            // Add invalid hole at (10, 10) and two ruler test points at row 205
            px[10 * 512 + 10] = 0
            px[205 * 512 + 255] = 1000
            px[205 * 512 + 328] = 1000
            controller.recorder.appendRaw16Frame(
                pixelsU16: px,
                width: 512,
                height: 424,
                seq: UInt64(f + 1),
                deviceTicks: UInt32(f * 267),
                arrivalNs: UInt64(1_000_000_000 + f * 33_333_333)
            )
        }
        _ = controller.recorder.stopRecording()

        usleep(10_000) // Ensure distinct millisecond timestamp in folder name

        // Record a 4-frame synthetic 16-bit IR session using 16-bit PNG sequence
        _ = try controller.recorder.startRecording(
            mode: KV_MODE_IR,
            width: 512,
            height: 424,
            losslessOption: .png16Sequence,
            irIntrinsics: intr
        )
        for f in 0..<4 {
            let px = [UInt16](repeating: UInt16(4000 + f * 1000), count: 512 * 424)
            controller.recorder.appendRaw16Frame(
                pixelsU16: px,
                width: 512,
                height: 424,
                seq: UInt64(f + 1),
                deviceTicks: UInt32(f * 267),
                arrivalNs: UInt64(2_000_000_000 + f * 33_333_333)
            )
        }
        _ = controller.recorder.stopRecording()

        controller.stopStream()
        controller.refreshRecordedSessions()
        XCTAssertEqual(controller.recordedSessions.count, 2)

        guard let depthSession = controller.recordedSessions.first(where: { $0.isDepthMode }),
              let irSession = controller.recordedSessions.first(where: { !$0.isDepthMode }) else {
            XCTFail("Expected both Depth and IR recorded sessions in metadata list")
            return
        }

        XCTAssertEqual(depthSession.modeTitle, "Depth")
        XCTAssertEqual(depthSession.frameCount, 5)
        XCTAssertGreaterThan(depthSession.sizeBytes, 0)
        XCTAssertTrue(depthSession.countdownText(at: Date()).contains("left"))

        // Confirm zero thumbnail or preview images exist in the base recordings directory
        let topLevelItems = try FileManager.default.contentsOfDirectory(atPath: recDir.path)
        for name in topLevelItems {
            let lower = name.lowercased()
            XCTAssertFalse(lower.hasSuffix(".jpg") || lower.hasSuffix(".png") || lower.hasSuffix(".mp4"))
        }

        // Play back Depth session through the live mapping & inspection pipeline
        XCTAssertTrue(controller.startPlayback(of: depthSession))
        await controller.flushPendingReplayWorkForTesting()
        XCTAssertTrue(controller.isReplayingRecording)
        XCTAssertTrue(controller.isReplayPlaying)
        XCTAssertEqual(controller.replayTotalFrames, 5)
        XCTAssertEqual(controller.replayFrameIndex, 0)
        XCTAssertNotNil(controller.previewImage)

        // Pause, scrub to frame 3, and step +1 to frame 4 (depth = 1200 + 4*100 = 1600 mm)
        controller.togglePlaybackPause()
        XCTAssertFalse(controller.isReplayPlaying)
        controller.seekPlayback(toFrame: 3)
        await controller.flushPendingReplayWorkForTesting()
        XCTAssertEqual(controller.replayFrameIndex, 3)
        controller.stepPlayback(by: 1)
        await controller.flushPendingReplayWorkForTesting()
        XCTAssertEqual(controller.replayFrameIndex, 4)

        let viewSize = CGSize(width: 512, height: 424)
        controller.updateCursorHover(viewPoint: CGPoint(x: 100.5, y: 100.5), viewSize: viewSize)
        XCTAssertEqual(controller.cursorReadout?.depthMm ?? 0, 1600.0, accuracy: 0.5)

        // Region stats & 3D ruler work on the replayed frame
        controller.updateRegionSelectionDrag(
            startViewPt: CGPoint(x: 8.5, y: 8.5),
            currentViewPt: CGPoint(x: 12.5, y: 12.5),
            viewSize: viewSize
        )
        XCTAssertNotNil(controller.regionStats)
        XCTAssertEqual(controller.regionStats?.invalidPixels, 1)

        controller.handleCanvasSingleClick(viewPoint: CGPoint(x: 255.5, y: 205.5), viewSize: viewSize)
        controller.handleCanvasSingleClick(viewPoint: CGPoint(x: 328.5, y: 205.5), viewSize: viewSize)
        XCTAssertNotNil(controller.rulerState?.distanceMm)
        XCTAssertEqual(controller.rulerState?.distanceMm ?? 0, 200.0, accuracy: 2.0)

        // Switching view modes, colormaps, mirror, rotate180, and Space freeze all work during replay
        controller.depthColormap = .viridis
        controller.selectedMode = .sideBySide
        controller.selectedSbsLayout = .irDepth
        XCTAssertNotNil(controller.previewImage)
        XCTAssertEqual(controller.previewImage?.width, 1024)
        controller.selectedMode = .depth
        controller.toggleFreeze()
        XCTAssertTrue(controller.isFrozen)
        controller.toggleFreeze()
        XCTAssertFalse(controller.isFrozen)

        // Play back IR PNG16 session and verify scrubbed 16-bit IR values
        XCTAssertTrue(controller.startPlayback(of: irSession))
        await controller.flushPendingReplayWorkForTesting()
        XCTAssertEqual(controller.selectedMode, .ir)
        controller.seekPlayback(toFrame: 2) // 4000 + 2*1000 = 6000
        await controller.flushPendingReplayWorkForTesting()
        controller.updateCursorHover(viewPoint: CGPoint(x: 200.5, y: 200.5), viewSize: viewSize)
        XCTAssertEqual(controller.cursorReadout?.rawIr ?? 0, 6000.0, accuracy: 1.0)

        controller.exitPlayback(resumeLive: false)
        XCTAssertFalse(controller.isReplayingRecording)
    }

    func testSevenDayAutoDeletionAndMetadataOnlyDeletionLog() throws {
        let tempRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase2-expiry-\(UUID().uuidString)", isDirectory: true)
        let recDir = tempRoot.appendingPathComponent("recordings", isDirectory: true)
        try FileManager.default.createDirectory(at: recDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempRoot) }

        let now = Date()
        let df = DateFormatter()
        df.locale = Locale(identifier: "en_US_POSIX")
        df.dateFormat = "yyyyMMdd-HHmmss-SSS"

        // Create an expired 8-day-old session and a fresh 2-day-old session
        let expiredDate = now.addingTimeInterval(-8.0 * 86_400.0)
        let freshDate = now.addingTimeInterval(-2.0 * 86_400.0)
        let expiredName = "\(df.string(from: expiredDate))_depth16mm.nosync"
        let freshName = "\(df.string(from: freshDate))_ir16.nosync"

        let expiredDir = recDir.appendingPathComponent(expiredName, isDirectory: true)
        let freshDir = recDir.appendingPathComponent(freshName, isDirectory: true)
        try FileManager.default.createDirectory(at: expiredDir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: freshDir, withIntermediateDirectories: true)
        try Data(repeating: 0xAA, count: 2048).write(to: expiredDir.appendingPathComponent("depth16mm_timestamps.csv"))
        try Data(repeating: 0xBB, count: 4096).write(to: freshDir.appendingPathComponent("ir16_timestamps.csv"))

        // Launching HostController runs the 7-day automatic sweep immediately at init
        let controller = HostController(
            recordingsDirectoryURL: recDir,
            stateDirectoryURL: tempRoot.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        XCTAssertFalse(FileManager.default.fileExists(atPath: expiredDir.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: freshDir.path))
        XCTAssertEqual(controller.recordedSessions.count, 1)
        XCTAssertEqual(controller.recordedSessions.first?.folderName, freshName)

        // Delete the remaining session by user action
        if let remaining = controller.recordedSessions.first {
            controller.deleteRecordingSession(remaining)
        }
        XCTAssertTrue(controller.recordedSessions.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: freshDir.path))

        // Verify deletion-log.txt format: strictly <ISO8601>\t<fileName>\t<size> bytes\t<automatic|user>
        let logURL = recDir.appendingPathComponent(RecordingLibrary.deletionLogFileName)
        let logText = try String(contentsOf: logURL, encoding: .utf8)
        let lines = logText.split(whereSeparator: \.isNewline)
        XCTAssertEqual(lines.count, 2)

        let cols0 = lines[0].split(separator: "\t")
        XCTAssertEqual(cols0.count, 4)
        XCTAssertEqual(String(cols0[1]), expiredName)
        XCTAssertEqual(String(cols0[2]), "2048 bytes")
        XCTAssertEqual(String(cols0[3]), "automatic")

        let cols1 = lines[1].split(separator: "\t")
        XCTAssertEqual(cols1.count, 4)
        XCTAssertEqual(String(cols1[1]), freshName)
        XCTAssertEqual(String(cols1[2]), "4096 bytes")
        XCTAssertEqual(String(cols1[3]), "user")
    }
}
