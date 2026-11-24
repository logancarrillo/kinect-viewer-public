import Foundation
import XCTest
@testable import KinectViewer

final class Phase0ColourRecordingTests: XCTestCase {
    private var tempRecordingsDir: URL!
    private var tempStateDir: URL!

    override func setUpWithError() throws {
        try super.setUpWithError()
        let base = FileManager.default.temporaryDirectory
            .appendingPathComponent("kinect-phase0-tests-\(UUID().uuidString)", isDirectory: true)
        tempRecordingsDir = base.appendingPathComponent("recordings", isDirectory: true)
        tempStateDir = base.appendingPathComponent("state", isDirectory: true)
    }

    override func tearDownWithError() throws {
        if let root = tempRecordingsDir?.deletingLastPathComponent() {
            try? FileManager.default.removeItem(at: root)
        }
        try super.tearDownWithError()
    }

    func testHostRecorderRejectsColourAndSideBySideModesWithZeroDiskOutput() throws {
        let recorder = HostRecorder(baseDirectoryURL: tempRecordingsDir, skipTimeMachineSubprocess: true)

        XCTAssertTrue(HostRecorder.canRecord(mode: KV_MODE_IR))
        XCTAssertTrue(HostRecorder.canRecord(mode: KV_MODE_DEPTH))
        XCTAssertFalse(HostRecorder.canRecord(mode: KV_MODE_COLOR))
        XCTAssertFalse(HostRecorder.canRecord(mode: KV_MODE_SIDE_BY_SIDE))

        for forbidden in [KV_MODE_COLOR, KV_MODE_SIDE_BY_SIDE] {
            XCTAssertThrowsError(
                try recorder.startRecording(
                    mode: forbidden,
                    width: 1920,
                    height: 1080,
                    losslessOption: .png16Sequence
                )
            ) { err in
                XCTAssertEqual(err as? HostRecorderError, .colourModeForbidden)
                XCTAssertEqual(err.localizedDescription, "Colour is live only. Switch to IR or Depth to record.")
            }
            XCTAssertFalse(recorder.currentStatus.isRecording)
        }

        // Verify zero session folders or .mp4/.mkv/.png files exist in tempRecordingsDir
        let entries = try FileManager.default.contentsOfDirectory(atPath: tempRecordingsDir.path)
        let recordingArtifacts = entries.filter { name in
            name != ".nosync" && name != ".metadata_never_index"
        }
        XCTAssertTrue(recordingArtifacts.isEmpty, "Unexpected recording artifacts created: \(recordingArtifacts)")
    }

    @MainActor
    func testHostControllerDisablesAndBlocksRecordingInColourAndAllSideBySideLayouts() throws {
        let controller = HostController(
            recordingsDirectoryURL: tempRecordingsDir,
            stateDirectoryURL: tempStateDir,
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        controller.useSyntheticSource = true
        controller.startStream()
        XCTAssertTrue(controller.isRunning)

        // 1. Colour mode cannot start recording and exposes exact required tooltip
        controller.selectedMode = .color
        XCTAssertFalse(controller.canRecordCurrentMode)
        XCTAssertEqual(controller.recordControlTooltip, "Colour is live only. Switch to IR or Depth to record.")
        XCTAssertFalse(controller.startHostRecording())
        XCTAssertFalse(controller.isRecording)
        XCTAssertEqual(controller.errorBanner, "Colour is live only. Switch to IR or Depth to record.")

        // 2. Side-by-Side mode (all layouts) cannot start recording and exposes exact required tooltip
        controller.selectedMode = .sideBySide
        for layout in SbsLayoutOption.allCases {
            controller.selectedSbsLayout = layout
            XCTAssertFalse(controller.canRecordCurrentMode, "Layout \(layout.title) must not allow recording")
            XCTAssertEqual(controller.recordControlTooltip, "Colour is live only. Switch to IR or Depth to record.")
            XCTAssertFalse(controller.startHostRecording(), "Layout \(layout.title) must refuse startHostRecording()")
            XCTAssertFalse(controller.isRecording)
        }

        // 3. Switching from an active IR recording into Colour immediately stops recording cleanly
        controller.selectedMode = .ir
        controller.recordingLosslessOption = .png16Sequence
        XCTAssertTrue(controller.canRecordCurrentMode)
        XCTAssertTrue(controller.startHostRecording())
        XCTAssertTrue(controller.isRecording)

        controller.selectedMode = .color
        XCTAssertFalse(controller.isRecording, "Switching to Colour must immediately stop any active recording")
        XCTAssertFalse(controller.canRecordCurrentMode)
    }
}
