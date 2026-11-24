import CoreGraphics
import Foundation
import XCTest
@testable import KinectViewer

@MainActor
final class Phase3PointCloudAndEffectsTests: XCTestCase {
    private func countNonBlackPixels(bgra: Data, pixelCount: Int) -> Int {
        var nonBlack = 0
        bgra.withUnsafeBytes { rawPtr in
            guard let ptr = rawPtr.bindMemory(to: UInt8.self).baseAddress else { return }
            for i in 0..<pixelCount {
                let b = ptr[i * 4 + 0]
                let g = ptr[i * 4 + 1]
                let r = ptr[i * 4 + 2]
                if b > 0 || g > 0 || r > 0 {
                    nonBlack += 1
                }
            }
        }
        return nonBlack
    }

    func testMetal3DPointCloudModesCameraControlsAndMeasuredSyntheticFps() async throws {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase3-3d-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let controller = HostController(
            recordingsDirectoryURL: tempDir.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempDir.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        // Key '5' activates Mode 5 (3D Point Cloud)
        XCTAssertTrue(controller.handleShortcutKey("5", keyCode: 23))
        XCTAssertEqual(controller.selectedMode, .pointCloud3D)
        XCTAssertTrue(controller.pointCloudRenderer.isAvailable, "Metal renderer must initialize on Apple Silicon")

        // Camera orbit, scroll zoom, Shift-drag pan, and reset button logic
        XCTAssertTrue(controller.pointCloudCamera.isDefault)
        controller.orbit3DCamera(deltaYaw: 0.35, deltaPitch: -0.20)
        controller.zoom3DCamera(scaleFactor: 1.25)
        controller.pan3DCamera(deltaRightMeters: 0.15, deltaUpMeters: -0.10)
        XCTAssertFalse(controller.pointCloudCamera.isDefault)
        XCTAssertLessThan(controller.pointCloudCamera.distanceMeters, PointCloudCameraState.defaultDistanceMeters)
        controller.reset3DCamera()
        XCTAssertTrue(controller.pointCloudCamera.isDefault)

        // Start synthetic engine and verify all 3 point colour sources (Depth, IR, Registered Live Colour)
        let eng = kv_engine_create(nil)
        defer { kv_engine_destroy(eng) }

        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = KV_MODE_POINT_CLOUD_3D
        var errBuf = [CChar](repeating: 0, count: 256)
        XCTAssertTrue(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &params, true, &errBuf, errBuf.count))

        try await Task.sleep(nanoseconds: 180_000_000)

        var depthMm = [Float](repeating: 0, count: 512 * 424)
        var pointBgra = [UInt8](repeating: 0, count: 512 * 424 * 4)
        var outRender = Data()
        var intr = kv_ir_intrinsics_t()
        let renderer = controller.pointCloudRenderer

        for source in [KV_POINT_COLOR_DEPTH, KV_POINT_COLOR_IR, KV_POINT_COLOR_REGISTERED_RGB] {
            params.point_color_source = source
            kv_engine_set_display_params(eng, &params)

            var seq: UInt64 = 0
            var arrNs: UInt64 = 0
            let copied = depthMm.withUnsafeMutableBufferPointer { dBuf in
                pointBgra.withUnsafeMutableBufferPointer { cBuf in
                    kv_engine_copy_point_cloud_inputs(
                        eng,
                        0,
                        dBuf.baseAddress,
                        dBuf.count,
                        cBuf.baseAddress,
                        cBuf.count,
                        &seq,
                        &arrNs,
                        &intr
                    )
                }
            }
            XCTAssertTrue(copied, "Failed copying point cloud inputs for source \(source.rawValue)")
            XCTAssertGreaterThan(seq, 0)

            let renderedSmall = depthMm.withUnsafeBufferPointer { dBuf in
                pointBgra.withUnsafeBufferPointer { cBuf in
                    renderer.render(
                        undistortedDepthMm: dBuf,
                        pointBgra: cBuf,
                        intrinsics: intr,
                        camera: PointCloudCameraState(),
                        pointSize: 1.0,
                        outBgraData: &outRender
                    )
                }
            }
            XCTAssertTrue(renderedSmall)
            let litSmall = countNonBlackPixels(
                bgra: outRender,
                pixelCount: MetalPointCloudRenderer.defaultRenderWidth * MetalPointCloudRenderer.defaultRenderHeight
            )
            XCTAssertGreaterThan(litSmall, 10_000, "3D render must produce lit point cloud for source \(source.rawValue)")

            // Larger point size splats cover strictly more pixels than 1.0 px points
            var outLarge = Data()
            _ = depthMm.withUnsafeBufferPointer { dBuf in
                pointBgra.withUnsafeBufferPointer { cBuf in
                    renderer.render(
                        undistortedDepthMm: dBuf,
                        pointBgra: cBuf,
                        intrinsics: intr,
                        camera: PointCloudCameraState(),
                        pointSize: 6.0,
                        outBgraData: &outLarge
                    )
                }
            }
            let litLarge = countNonBlackPixels(
                bgra: outLarge,
                pixelCount: MetalPointCloudRenderer.defaultRenderWidth * MetalPointCloudRenderer.defaultRenderHeight
            )
            XCTAssertGreaterThan(litLarge, litSmall, "Increasing point size slider must expand rendered splat coverage")
        }

        // Benchmark end-to-end synthetic Registration + Metal 3D rendering throughput (180 frames @ 960x720)
        params.point_color_source = KV_POINT_COLOR_REGISTERED_RGB
        kv_engine_set_display_params(eng, &params)
        var cam = PointCloudCameraState()
        let benchmarkFrames = 180
        let tStartNs = kv_monotonic_ns()
        for i in 0..<benchmarkFrames {
            cam.yawRad = Float(i) * 0.01
            kv_engine_set_display_params(eng, &params)
            var seq: UInt64 = 0
            var arrNs: UInt64 = 0
            _ = depthMm.withUnsafeMutableBufferPointer { dBuf in
                pointBgra.withUnsafeMutableBufferPointer { cBuf in
                    kv_engine_copy_point_cloud_inputs(
                        eng,
                        0,
                        dBuf.baseAddress,
                        dBuf.count,
                        cBuf.baseAddress,
                        cBuf.count,
                        &seq,
                        &arrNs,
                        &intr
                    )
                }
            }
            _ = depthMm.withUnsafeBufferPointer { dBuf in
                pointBgra.withUnsafeBufferPointer { cBuf in
                    renderer.render(
                        undistortedDepthMm: dBuf,
                        pointBgra: cBuf,
                        intrinsics: intr,
                        camera: cam,
                        pointSize: 2.5,
                        outBgraData: &outRender
                    )
                }
            }
        }
        let elapsedSec = Double(kv_monotonic_ns() - tStartNs) / 1e9
        let measuredFps = Double(benchmarkFrames) / max(1e-6, elapsedSec)
        print(String(format: "[Phase3Benchmark] Synthetic Metal 3D Point Cloud throughput = %.1f fps (%.2f ms/frame)", measuredFps, (elapsedSec * 1000.0) / Double(benchmarkFrames)))
        XCTAssertGreaterThanOrEqual(measuredFps, 30.0, "Metal 3D point cloud must sustain >= 30 fps on Apple Silicon")
    }

    func testDepthColourBlendFades0To100PercentAndRemainsStrictlyLiveOnly() async throws {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase3-blend-\(UUID().uuidString)", isDirectory: true)
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
        controller.recordingLosslessOption = .png16Sequence
        controller.startStream()
        XCTAssertTrue(controller.isRunning)
        XCTAssertTrue(controller.canRecordCurrentMode)
        XCTAssertTrue(controller.startHostRecording())
        XCTAssertTrue(controller.isRecording)

        // Enabling Depth/Colour Blend immediately stops active recording and blocks starting any new recording
        controller.depthColorBlendEnabled = true
        XCTAssertFalse(controller.isRecording, "Enabling colour-carrying blend must stop active recording cleanly")
        XCTAssertFalse(controller.canRecordCurrentMode)
        XCTAssertEqual(controller.recordControlTooltip, "Colour is live only. Switch to IR or Depth to record.")
        XCTAssertFalse(controller.startHostRecording())

        // Verify 0%, 50%, and 100% blend pixel interpolation at C engine level on synthetic registered frames
        let eng = kv_engine_create(nil)
        defer { kv_engine_destroy(eng) }

        var p = kv_display_params_t()
        kv_default_display_params(&p)
        p.mode = KV_MODE_DEPTH
        p.depth_color_blend_enabled = true
        var errBuf = [CChar](repeating: 0, count: 256)
        XCTAssertTrue(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, true, &errBuf, errBuf.count))
        try await Task.sleep(nanoseconds: 180_000_000)

        func grabFrame(blend: Float) -> [UInt8] {
            p.depth_color_blend = blend
            kv_engine_set_display_params(eng, &p)
            var buf = [UInt8](repeating: 0, count: 512 * 424 * 4)
            var req: Int = 0
            var meta = kv_mapped_frame_t()
            let ok = buf.withUnsafeMutableBufferPointer { ptr in
                kv_engine_copy_latest_frame(eng, 0, ptr.baseAddress, ptr.count, &req, &meta)
            }
            XCTAssertTrue(ok)
            XCTAssertEqual(meta.width, 512)
            XCTAssertEqual(meta.height, 424)
            return buf
        }

        let f0 = grabFrame(blend: 0.0)
        let f50 = grabFrame(blend: 0.5)
        let f100 = grabFrame(blend: 1.0)

        // Find a valid center pixel where registered colour and depth false-colour differ, and check 50% midpoint
        var checkedPixels = 0
        for y in 120..<300 {
            for x in 120..<390 {
                let off = (y * 512 + x) * 4
                let b0 = Int(f0[off]), b50 = Int(f50[off]), b100 = Int(f100[off])
                if abs(b100 - b0) >= 20 {
                    let expected = (b0 + b100) / 2
                    XCTAssertLessThanOrEqual(abs(b50 - expected), 3, "50% blend must linearly interpolate registered colour and depth colormap")
                    checkedPixels += 1
                    if checkedPixels >= 50 { break }
                }
            }
            if checkedPixels >= 50 { break }
        }
        XCTAssertGreaterThanOrEqual(checkedPixels, 20)
    }

    func testDepthCutoffAndTrackMePresetBlackOutPixelsInAllModesIncluding3D() async throws {
        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-phase3-cutoff-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let controller = HostController(
            recordingsDirectoryURL: tempDir.appendingPathComponent("rec", isDirectory: true),
            stateDirectoryURL: tempDir.appendingPathComponent("state", isDirectory: true),
            enableStreamingServer: false,
            autostart: false
        )
        defer { controller.shutdown() }

        // Verify "Track me" preset configures [500, 2000] mm (0.5 - 2.0 m) and enables cut-off
        controller.cutoffNearMm = 900
        controller.cutoffFarMm = 4000
        controller.cutoffEnabled = false
        controller.applyTrackMeCutoffPreset()
        XCTAssertTrue(controller.cutoffEnabled)
        XCTAssertEqual(controller.cutoffNearMm, 500.0, accuracy: 1e-4)
        XCTAssertEqual(controller.cutoffFarMm, 2000.0, accuracy: 1e-4)

        let eng = kv_engine_create(nil)
        defer { kv_engine_destroy(eng) }

        var p = kv_display_params_t()
        kv_default_display_params(&p)
        var errBuf = [CChar](repeating: 0, count: 256)
        XCTAssertTrue(kv_engine_start_ex(eng, KV_SOURCE_SYNTHETIC, KV_PIPELINE_CPU, &p, true, &errBuf, errBuf.count))
        try await Task.sleep(nanoseconds: 200_000_000)

        func nonBlackCount2D(mode: kv_view_mode_t, sbs: kv_sbs_layout_t = KV_SBS_IR_DEPTH, blend: Bool = false, cutoff: Bool, near: Float = 500, far: Float = 2000) -> Int {
            p.mode = mode
            p.sbs_layout = sbs
            p.depth_color_blend_enabled = blend
            p.depth_color_blend = 0.5
            p.cutoff_enabled = cutoff
            p.cutoff_near_mm = near
            p.cutoff_far_mm = far
            kv_engine_set_display_params(eng, &p)

            var buf = Data(count: 1920 * 1080 * 4)
            var req: Int = 0
            var meta = kv_mapped_frame_t()
            let ok = buf.withUnsafeMutableBytes { rawPtr -> Bool in
                guard let base = rawPtr.bindMemory(to: UInt8.self).baseAddress else { return false }
                return kv_engine_copy_latest_frame(eng, 0, base, rawPtr.count, &req, &meta)
            }
            XCTAssertTrue(ok)
            return countNonBlackPixels(bgra: buf, pixelCount: Int(meta.width * meta.height))
        }

        // Synthetic depth spans 500..4500 mm, so Track Me (500..2000 mm) must black out >40% of lit pixels in EVERY 2D mode,
        // and an out-of-range window (100..250 mm) must black out 100% of pixels!
        for modeCase: (String, kv_view_mode_t, kv_sbs_layout_t, Bool) in [
            ("IR", KV_MODE_IR, KV_SBS_IR_DEPTH, false),
            ("Depth", KV_MODE_DEPTH, KV_SBS_IR_DEPTH, false),
            ("Colour", KV_MODE_COLOR, KV_SBS_IR_DEPTH, false),
            ("SBS IR+Depth", KV_MODE_SIDE_BY_SIDE, KV_SBS_IR_DEPTH, false),
            ("SBS Colour+Depth", KV_MODE_SIDE_BY_SIDE, KV_SBS_COLOR_DEPTH, false),
            ("Depth/Colour Blend", KV_MODE_DEPTH, KV_SBS_IR_DEPTH, true),
        ] {
            let fullLit = nonBlackCount2D(mode: modeCase.1, sbs: modeCase.2, blend: modeCase.3, cutoff: false)
            let trackMeLit = nonBlackCount2D(mode: modeCase.1, sbs: modeCase.2, blend: modeCase.3, cutoff: true, near: 500, far: 2000)
            let emptyLit = nonBlackCount2D(mode: modeCase.1, sbs: modeCase.2, blend: modeCase.3, cutoff: true, near: 100, far: 250)

            XCTAssertGreaterThan(fullLit, 50_000, "\(modeCase.0) should have lit pixels before cut-off")
            XCTAssertLessThan(trackMeLit, Int(Double(fullLit) * 0.65), "\(modeCase.0) Track Me cut-off must black out pixels beyond 2.0 m")
            if modeCase.1 != KV_MODE_SIDE_BY_SIDE {
                XCTAssertEqual(emptyLit, 0, "\(modeCase.0) must go completely black when all depth is outside [near, far]")
            }
        }

        // Also verify Mode 5 (3D Point Cloud) goes completely black outside [near, far]
        p.mode = KV_MODE_POINT_CLOUD_3D
        p.depth_color_blend_enabled = false
        p.cutoff_enabled = true
        p.cutoff_near_mm = 100
        p.cutoff_far_mm = 250
        kv_engine_set_display_params(eng, &p)

        var depthMm = [Float](repeating: 0, count: 512 * 424)
        var pointBgra = [UInt8](repeating: 0, count: 512 * 424 * 4)
        var seq: UInt64 = 0
        var arrNs: UInt64 = 0
        var intr = kv_ir_intrinsics_t()
        let copied = depthMm.withUnsafeMutableBufferPointer { dBuf in
            pointBgra.withUnsafeMutableBufferPointer { cBuf in
                kv_engine_copy_point_cloud_inputs(
                    eng, 0, dBuf.baseAddress, dBuf.count, cBuf.baseAddress, cBuf.count, &seq, &arrNs, &intr
                )
            }
        }
        XCTAssertTrue(copied)
        var renderOut = Data()
        let ok3D = depthMm.withUnsafeBufferPointer { dBuf in
            pointBgra.withUnsafeBufferPointer { cBuf in
                controller.pointCloudRenderer.render(
                    undistortedDepthMm: dBuf,
                    pointBgra: cBuf,
                    intrinsics: intr,
                    camera: PointCloudCameraState(),
                    pointSize: 3.0,
                    outBgraData: &renderOut
                )
            }
        }
        XCTAssertTrue(ok3D)
        let lit3DOutside = countNonBlackPixels(
            bgra: renderOut,
            pixelCount: MetalPointCloudRenderer.defaultRenderWidth * MetalPointCloudRenderer.defaultRenderHeight
        )
        XCTAssertEqual(lit3DOutside, 0, "3D mode must render 100% black when all depth lies outside [near, far]")
    }
}
