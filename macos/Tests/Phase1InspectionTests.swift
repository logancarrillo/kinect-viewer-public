import CoreGraphics
import XCTest
@testable import KinectViewer

@MainActor
final class Phase1InspectionTests: XCTestCase {
    func testRegionStatsMeanMedianStdDevMinMaxAndInvalidShare() {
        // 4x2 grid (8 pixels): 6 valid [1000, 1200, 1400, 1600, 1800, 2000], 2 invalid [0, NaN]
        let grid: [Float] = [
            1000, 1200, 0,    1400,
            1600, .nan, 1800, 2000,
        ]
        let stats = RegionDepthStats.compute(depthMm: grid, width: 4, height: 2, x0: 0, y0: 0, x1: 3, y1: 1)
        XCTAssertNotNil(stats)
        guard let s = stats else { return }

        XCTAssertEqual(s.totalPixels, 8)
        XCTAssertEqual(s.validPixels, 6)
        XCTAssertEqual(s.invalidPixels, 2)
        XCTAssertEqual(s.invalidShare, 0.25, accuracy: 1e-6)
        XCTAssertEqual(s.minMm ?? -1, 1000.0, accuracy: 1e-5)
        XCTAssertEqual(s.maxMm ?? -1, 2000.0, accuracy: 1e-5)
        XCTAssertEqual(s.meanMm ?? -1, 1500.0, accuracy: 1e-5)
        // Sorted valid: [1000, 1200, 1400, 1600, 1800, 2000] -> median = (1400+1600)/2 = 1500
        XCTAssertEqual(s.medianMm ?? -1, 1500.0, accuracy: 1e-5)
        // Variance = ((500^2 + 300^2 + 100^2)*2)/6 = (250000+90000+10000)/3 = 350000/3 -> sd = 341.5650255
        XCTAssertEqual(s.stdDevMm ?? -1, (350000.0 / 3.0).squareRoot(), accuracy: 1e-4)
    }

    func testRuler3DDistanceFromIntrinsicsAndNoDepthMessage() {
        var intr = kv_ir_intrinsics_t(
            valid: true,
            fx: 365.0,
            fy: 365.0,
            cx: 256.0,
            cy: 212.0,
            k1: 0.0,
            k2: 0.0,
            k3: 0.0,
            p1: 0.0,
            p2: 0.0
        )

        var depth = [Float](repeating: 0.0, count: 512 * 424)
        // Point A at principal point (256, 212), Z = 1000 mm -> (0, 0, 1000)
        // Point B at (256 + 365 = 500? keep inside 512: 256 + 182.5 -> use 256 + 73 = 329, Z = 1000 mm)
        // xd = 73 / 365 = 0.2 -> X = 200 mm, Y = 0, Z = 1000 mm -> Euclidean distance = 200.0 mm
        depth[212 * 512 + 256] = 1000.0
        depth[212 * 512 + 329] = 1000.0
        depth[100 * 512 + 100] = 0.0 // invalid pixel

        var measured: Float = 0
        let ok = depth.withUnsafeBufferPointer { buf in
            kv_measure_3d_distance_mm(&intr, buf.baseAddress, 512, 424, 256, 212, 329, 212, &measured)
        }
        XCTAssertTrue(ok)
        XCTAssertEqual(measured, 200.0, accuracy: 0.05)

        // Endpoint with invalid (0) depth must fail and produce "no depth here"
        let endA = RulerEndpoint(dispX: 256, dispY: 212, sensorX: 256, sensorY: 212, hasSensorCoord: true)
        let endInvalid = RulerEndpoint(dispX: 100, dispY: 100, sensorX: 100, sensorY: 100, hasSensorCoord: true)
        let evalInvalid = RulerMeasurementState.evaluateDistance(
            pointA: endA,
            pointB: endInvalid,
            intrinsics: intr,
            depthMm: depth
        )
        XCTAssertNil(evalInvalid.distanceMm)
        XCTAssertEqual(evalInvalid.errorMessage, "no depth here")
    }

    func testCursorReadoutFreezeZoomPanOrientationAndShortcuts() throws {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase1-inspect-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let controller = HostController(
            recordingsDirectoryURL: tempDir.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempDir.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        controller.selectedMode = .depth
        let intr = kv_ir_intrinsics_t(
            valid: true, fx: 365.456, fy: 365.456, cx: 254.878, cy: 205.395,
            k1: 0.09, k2: -0.268, k3: 0.095, p1: 0.0, p2: 0.0
        )
        var depthA = [Float](repeating: 1500.0, count: 512 * 424)
        var irA = [Float](repeating: 3200.0, count: 512 * 424)
        depthA[100 * 512 + 50] = 1234.0
        irA[100 * 512 + 50] = 8765.0
        controller.loadSyntheticInspectionSnapshotForTesting(depthMm: depthA, irRaw: irA, intrinsics: intr)

        let viewSize = CGSize(width: 512, height: 424)
        controller.updateCursorHover(viewPoint: CGPoint(x: 50.5, y: 100.5), viewSize: viewSize)
        XCTAssertEqual(controller.cursorReadout?.pixelX, 50)
        XCTAssertEqual(controller.cursorReadout?.pixelY, 100)
        XCTAssertEqual(controller.cursorReadout?.depthMm ?? 0, 1234.0, accuracy: 0.5)
        XCTAssertEqual(controller.cursorReadout?.rawIr ?? 0, 8765.0, accuracy: 0.5)

        // Off-image hides cursor readout immediately
        controller.updateCursorHover(viewPoint: CGPoint(x: -5, y: 100), viewSize: viewSize)
        XCTAssertNil(controller.cursorReadout)

        // Zoom around focal point (50.5, 100.5) keeps exact same pixel (50, 100) under cursor!
        let focal = CGPoint(x: 50.5, y: 100.5)
        controller.zoomCanvasAround(cursorInView: focal, viewSize: viewSize, factor: 3.0)
        XCTAssertEqual(controller.viewportTransform.zoomScale, 3.0, accuracy: 0.001)
        controller.updateCursorHover(viewPoint: focal, viewSize: viewSize)
        XCTAssertEqual(controller.cursorReadout?.pixelX, 50)
        XCTAssertEqual(controller.cursorReadout?.pixelY, 100)

        // Reset viewport (double-click action)
        controller.resetViewport()
        XCTAssertEqual(controller.viewportTransform.zoomScale, 1.0, accuracy: 0.001)

        // Freeze frame with Space shortcut, then mutate live buffers and confirm frozen readout/stats/ruler persist
        XCTAssertTrue(controller.handleShortcutKey(" ", keyCode: 49))
        XCTAssertTrue(controller.isFrozen)

        let depthB = [Float](repeating: 4000.0, count: 512 * 424)
        let irB = [Float](repeating: 100.0, count: 512 * 424)
        controller.loadSyntheticInspectionSnapshotForTesting(depthMm: depthB, irRaw: irB, intrinsics: intr)

        controller.updateCursorHover(viewPoint: CGPoint(x: 50.5, y: 100.5), viewSize: viewSize)
        XCTAssertEqual(controller.cursorReadout?.depthMm ?? 0, 1234.0, accuracy: 0.5, "Frozen frame must stay in memory")

        controller.updateRegionSelectionDrag(
            startViewPt: CGPoint(x: 49.5, y: 99.5),
            currentViewPt: CGPoint(x: 51.5, y: 101.5),
            viewSize: viewSize
        )
        XCTAssertNotNil(controller.regionStats)
        XCTAssertEqual(controller.regionStats?.minMm ?? 0, 1234.0, accuracy: 0.5)

        controller.handleCanvasSingleClick(viewPoint: CGPoint(x: 50.5, y: 100.5), viewSize: viewSize)
        controller.handleCanvasSingleClick(viewPoint: CGPoint(x: 150.5, y: 100.5), viewSize: viewSize)
        XCTAssertNotNil(controller.rulerState?.distanceMm)

        // Esc clears region & ruler
        XCTAssertTrue(controller.handleShortcutKey("\u{1B}", keyCode: 53))
        XCTAssertNil(controller.regionStats)
        XCTAssertNil(controller.rulerState)

        // Mirror (M), Rotate 180° (R), Health (H), Help (?)
        XCTAssertTrue(controller.handleShortcutKey("m", keyCode: 46))
        XCTAssertTrue(controller.mirrorHorizontal)
        // Under horizontal mirror, sensor pixel x=50 appears at display x = 511 - 50 = 461
        controller.updateCursorHover(viewPoint: CGPoint(x: 461.5, y: 100.5), viewSize: viewSize)
        XCTAssertEqual(controller.cursorReadout?.pixelX, 50)
        XCTAssertEqual(controller.cursorReadout?.depthMm ?? 0, 1234.0, accuracy: 0.5)

        XCTAssertTrue(controller.handleShortcutKey("r", keyCode: 15))
        XCTAssertTrue(controller.rotate180)
        XCTAssertTrue(controller.handleShortcutKey("h", keyCode: 4))
        XCTAssertTrue(controller.showHealthOverlay)
        XCTAssertTrue(controller.handleShortcutKey("?", keyCode: 44, modifiers: [.shift]))
        XCTAssertTrue(controller.showShortcutsHelp)
    }

    func testPresetsBuiltInsSaveLoadAndRememberLastUsedAcrossLaunches() throws {
        let stateDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase1-presets-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: stateDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: stateDir) }

        do {
            let first = HostController(
                recordingsDirectoryURL: stateDir.appendingPathComponent("rec", isDirectory: true),
                stateDirectoryURL: stateDir,
                enableStreamingServer: false,
                autostart: false
            )
            defer { first.shutdown() }

            let builtInNames = ViewerPreset.builtInPresets.map(\.name)
            XCTAssertEqual(builtInNames, ["Desk", "Room", "Dark room IR"])

            first.applyPreset(.builtInDarkRoomIR)
            XCTAssertEqual(first.selectedMode, .ir)
            XCTAssertEqual(first.irGamma, 0.36, accuracy: 1e-4)

            // Customize settings and save a named preset
            first.selectedMode = .depth
            first.selectedPipeline = .cpu
            first.depthColormap = .inferno
            first.depthMinMm = 650.0
            first.depthMaxMm = 3100.0
            first.exposureMode = .manual
            first.manualIntegrationMs = 22.5
            first.manualAnalogGain = 2.4
            XCTAssertTrue(first.saveNamedPreset("Workbench"))
        }

        // Second controller simulating next app launch restores last-used settings and saved preset
        let second = HostController(
            recordingsDirectoryURL: stateDir.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: stateDir,
            enableStreamingServer: false,
            autostart: false
        )
        defer { second.shutdown() }

        XCTAssertEqual(second.selectedMode, .depth)
        XCTAssertEqual(second.selectedPipeline, .cpu)
        XCTAssertEqual(second.depthColormap, .inferno)
        XCTAssertEqual(second.depthMinMm, 650.0, accuracy: 1e-4)
        XCTAssertEqual(second.depthMaxMm, 3100.0, accuracy: 1e-4)
        XCTAssertEqual(second.exposureMode, .manual)
        XCTAssertEqual(second.manualIntegrationMs, 22.5, accuracy: 1e-4)
        XCTAssertTrue(second.presets.contains(where: { $0.name == "Workbench" && !$0.isBuiltIn }))
    }

    func testDropWarningThresholdAndSyntheticHealthMetrics() async throws {
        XCTAssertFalse(DropBannerEvaluator.shouldShowBanner(irDropRatio10s: 0.019, depthDropRatio10s: 0.02))
        XCTAssertTrue(DropBannerEvaluator.shouldShowBanner(irDropRatio10s: 0.025, depthDropRatio10s: 0.0))
        XCTAssertTrue(DropBannerEvaluator.shouldShowBanner(irDropRatio10s: 0.0, depthDropRatio10s: 0.031))
        XCTAssertEqual(
            DropBannerEvaluator.warningBannerText,
            "Frames are dropping. Try a different USB port, ideally a rear port directly on the motherboard."
        )

        let eng = kv_engine_create(nil)
        defer { kv_engine_destroy(eng) }
        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = KV_MODE_DEPTH
        var errBuf = [CChar](repeating: 0, count: 256)
        XCTAssertTrue(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &params, true, &errBuf, errBuf.count))

        try await Task.sleep(nanoseconds: 450_000_000)

        var health = kv_health_snapshot_t()
        kv_engine_get_health(eng, &health)
        XCTAssertGreaterThan(health.ir_fps, 20.0)
        XCTAssertGreaterThan(health.depth_fps, 20.0)
        XCTAssertGreaterThan(health.inter_frame_p50_ms, 15.0)
        XCTAssertFalse(health.drop_warning_active)

        var intr = kv_ir_intrinsics_t()
        XCTAssertTrue(kv_engine_get_ir_intrinsics(eng, &intr))
        XCTAssertTrue(intr.valid)
        XCTAssertGreaterThan(intr.fx, 300.0)
    }
}
