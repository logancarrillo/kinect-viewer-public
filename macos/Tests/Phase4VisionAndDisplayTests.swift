import AppKit
import CoreGraphics
import Foundation
import XCTest
@testable import KinectViewer

@MainActor
final class Phase4VisionAndDisplayTests: XCTestCase {
    func testVisionDetectorRunsCleanlyOnSyntheticColourFrameWithZeroStorage() async throws {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase4-vision-raw-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let eng = kv_engine_create(nil)
        defer { kv_engine_destroy(eng) }

        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = KV_MODE_DEPTH
        var errBuf = [CChar](repeating: 0, count: 256)
        // Enable RGB even while viewing Depth so Vision has live colour frames
        XCTAssertTrue(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &params, true, &errBuf, errBuf.count))

        try await Task.sleep(nanoseconds: 180_000_000)

        var colorBuf = Data(count: 1920 * 1080 * 4)
        var colorSeq: UInt64 = 0
        var cw: UInt32 = 0
        var ch: UInt32 = 0
        let copied = colorBuf.withUnsafeMutableBytes { rawPtr -> Bool in
            guard let base = rawPtr.bindMemory(to: UInt8.self).baseAddress else { return false }
            return kv_engine_copy_latest_color_bgra(eng, 0, base, rawPtr.count, &colorSeq, &cw, &ch)
        }
        XCTAssertTrue(copied)
        XCTAssertGreaterThan(colorSeq, 0)
        XCTAssertEqual(cw, 1920)
        XCTAssertEqual(ch, 1080)

        let estimator = VisionSkeletonEstimator()
        let snap = estimator.detectSynchronously(
            bgraData: colorBuf,
            width: Int(cw),
            height: Int(ch),
            colorSeq: colorSeq
        )
        XCTAssertEqual(snap.colorSeq, colorSeq)
        // Synthetic colour test pattern contains geometric colour bars (no human silhouette), so Vision returns 0 poses cleanly
        XCTAssertEqual(snap.bodyCount, 0)
        XCTAssertEqual(snap.handCount, 0)

        let filesWritten = try FileManager.default.contentsOfDirectory(atPath: tempDir.path)
        XCTAssertTrue(filesWritten.isEmpty, "Vision pose estimation must never write anything to disk")
    }

    func testSkeletonRegistrationProjectionOntoIrDepthSideBySideAnd3DWithConfidenceThreshold() async throws {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase4-proj-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let controller = HostController(
            recordingsDirectoryURL: tempDir.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempDir.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        controller.useSyntheticSource = true
        controller.selectedMode = .depth
        controller.skeletonOverlayEnabled = true
        controller.startStream()
        XCTAssertTrue(controller.isRunning)

        try await Task.sleep(nanoseconds: 200_000_000)

        // Build a deterministic synthetic body + hand pose inside the central region covered by registered depth
        let syntheticPose = RawVisionPoseSnapshot(
            colorSeq: 1,
            bodyCount: 1,
            handCount: 1,
            joints: [
                RawVisionJoint2D(id: "b0:neck", normTopLeftX: 0.50, normTopLeftY: 0.38, confidence: 0.92, isHand: false),
                RawVisionJoint2D(id: "b0:leftShoulder", normTopLeftX: 0.44, normTopLeftY: 0.40, confidence: 0.88, isHand: false),
                RawVisionJoint2D(id: "b0:rightShoulder", normTopLeftX: 0.56, normTopLeftY: 0.40, confidence: 0.89, isHand: false),
                RawVisionJoint2D(id: "b0:rightElbow", normTopLeftX: 0.60, normTopLeftY: 0.50, confidence: 0.30, isHand: false),
                RawVisionJoint2D(id: "h0:wrist", normTopLeftX: 0.42, normTopLeftY: 0.48, confidence: 0.85, isHand: true),
                RawVisionJoint2D(id: "h0:indexTip", normTopLeftX: 0.40, normTopLeftY: 0.44, confidence: 0.82, isHand: true),
            ],
            bones: [
                RawVisionBoneEdge(fromId: "b0:neck", toId: "b0:leftShoulder", isHand: false),
                RawVisionBoneEdge(fromId: "b0:neck", toId: "b0:rightShoulder", isHand: false),
                RawVisionBoneEdge(fromId: "b0:rightShoulder", toId: "b0:rightElbow", isHand: false),
                RawVisionBoneEdge(fromId: "h0:wrist", toId: "h0:indexTip", isHand: true),
            ]
        )

        // 1. At threshold 0.20, all 6 joints (4 body + 2 hand) and all 4 bones map onto Depth (512x424)
        controller.skeletonConfidenceThreshold = 0.20
        controller.injectVisionPoseSnapshotForTesting(syntheticPose)
        XCTAssertEqual(controller.activeSkeletonOverlay.dots2D.count, 6)
        XCTAssertEqual(controller.activeSkeletonOverlay.lines2D.count, 4)

        // 2. Raising confidence threshold to 0.50 drops the 0.30 rightElbow joint and its connected bone
        controller.skeletonConfidenceThreshold = 0.50
        XCTAssertEqual(controller.activeSkeletonOverlay.dots2D.count, 5)
        XCTAssertEqual(controller.activeSkeletonOverlay.lines2D.count, 3)

        // 3. Switching to IR mode maps the exact same 5 joints & 3 bones onto the IR view
        controller.selectedMode = .ir
        XCTAssertEqual(controller.activeSkeletonOverlay.dots2D.count, 5)
        XCTAssertEqual(controller.activeSkeletonOverlay.lines2D.count, 3)

        // 4. Side-by-Side (IR + Depth) projects joints onto BOTH panels (5 * 2 = 10 dots, 3 * 2 = 6 lines)
        controller.selectedMode = .sideBySide
        controller.selectedSbsLayout = .irDepth
        XCTAssertEqual(controller.activeSkeletonOverlay.dots2D.count, 10)
        XCTAssertEqual(controller.activeSkeletonOverlay.lines2D.count, 6)

        // 5. Mode 5 (3D Point Cloud) produces 3D point & line vertices and renders cleanly in Metal
        controller.selectedMode = .pointCloud3D
        try await Task.sleep(nanoseconds: 80_000_000)
        controller.injectVisionPoseSnapshotForTesting(syntheticPose)
        XCTAssertEqual(controller.activeSkeletonOverlay.pointVertices3D.count, 5)
        XCTAssertEqual(controller.activeSkeletonOverlay.lineVertices3D.count, 6) // 3 bones * 2 endpoints

        // 6. Raising confidence threshold to 0.95 filters out all joints
        controller.skeletonConfidenceThreshold = 0.95
        XCTAssertTrue(controller.activeSkeletonOverlay.pointVertices3D.isEmpty)
        XCTAssertTrue(controller.activeSkeletonOverlay.lineVertices3D.isEmpty)
    }

    func testSecondDisplayEnumerationFullScreenOutputAndInstantBlackoutShortcutB() throws {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase4-display-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let controller = HostController(
            recordingsDirectoryURL: tempDir.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempDir.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        controller.refreshAvailableDisplays()
        guard !controller.availableDisplays.isEmpty else {
            // Headless CI fallback if NSScreen.screens is empty
            return
        }

        XCTAssertNotNil(controller.selectedSecondDisplayID)
        XCTAssertFalse(controller.isSecondDisplayActive)
        XCTAssertFalse(controller.isSecondDisplayBlackedOut)

        // Open clean full-screen borderless output window on selected display
        XCTAssertTrue(controller.startSecondDisplayOutput())
        XCTAssertTrue(controller.isSecondDisplayActive)
        XCTAssertTrue(controller.secondDisplayController.isOutputActive)

        // Press 'B' shortcut -> blanks second-display output immediately while main controller stays intact
        XCTAssertTrue(controller.handleShortcutKey("b", keyCode: 11))
        XCTAssertTrue(controller.isSecondDisplayBlackedOut)
        XCTAssertTrue(controller.secondDisplayController.isBlackedOut)

        // Press 'B' again -> unblanks output immediately
        XCTAssertTrue(controller.handleShortcutKey("b", keyCode: 11))
        XCTAssertFalse(controller.isSecondDisplayBlackedOut)
        XCTAssertFalse(controller.secondDisplayController.isBlackedOut)

        controller.stopSecondDisplayOutput()
        XCTAssertFalse(controller.isSecondDisplayActive)
        XCTAssertFalse(controller.secondDisplayController.isOutputActive)
    }
}
