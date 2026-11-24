import CoreGraphics
import CoreMedia
import Foundation
import ImageIO
import Network

@main
struct KinectTestCLI {
    static func main() {
        let args = CommandLine.arguments
        var useSynthetic = false
        var requestedReal = false
        var durationSec: Double = 5.0
        var pipeline: kv_pipeline_kind_t = KV_PIPELINE_OPENGL
        var mode: kv_view_mode_t = KV_MODE_IR
        var sbsLayout: kv_sbs_layout_t = KV_SBS_IR_DEPTH
        var colormap: kv_colormap_kind_t = KV_COLORMAP_TURBO
        var depthMinMm: Float = 500.0
        var depthMaxMm: Float = 4500.0
        var verifyLockGuard = false
        var verifyBarcode = false
        var verifyNoFallback = false
        var testExposure = false
        var testStreamLatency = false
        var testRecording = false
        var testHardening = false
        var serveSimQRFile: String?

        var idx = 1
        while idx < args.count {
            switch args[idx] {
            case "--synthetic":
                useSynthetic = true
                requestedReal = false
            case "--real":
                useSynthetic = false
                requestedReal = true
            case "--duration":
                if idx + 1 < args.count, let d = Double(args[idx + 1]) {
                    durationSec = d
                    idx += 1
                }
            case "--pipeline":
                if idx + 1 < args.count {
                    let val = args[idx + 1].lowercased()
                    if val == "gl" || val == "opengl" {
                        pipeline = KV_PIPELINE_OPENGL
                    } else if val == "cl" || val == "opencl" {
                        pipeline = KV_PIPELINE_OPENCL
                    } else if val == "cpu" {
                        pipeline = KV_PIPELINE_CPU
                    } else {
                        fputs("Unknown pipeline: \(val)\n", stderr)
                        exit(2)
                    }
                    idx += 1
                }
            case "--mode":
                if idx + 1 < args.count {
                    let val = args[idx + 1].lowercased()
                    switch val {
                    case "ir": mode = KV_MODE_IR
                    case "depth": mode = KV_MODE_DEPTH
                    case "color", "colour": mode = KV_MODE_COLOR
                    case "sbs", "side-by-side": mode = KV_MODE_SIDE_BY_SIDE
                    default:
                        fputs("Unknown mode: \(val)\n", stderr)
                        exit(2)
                    }
                    idx += 1
                }
            case "--sbs-layout":
                if idx + 1 < args.count {
                    let val = args[idx + 1].lowercased()
                    switch val {
                    case "ir-depth": sbsLayout = KV_SBS_IR_DEPTH
                    case "color-depth": sbsLayout = KV_SBS_COLOR_DEPTH
                    case "color-ir": sbsLayout = KV_SBS_COLOR_IR
                    default: break
                    }
                    idx += 1
                }
            case "--colormap":
                if idx + 1 < args.count {
                    switch args[idx + 1].lowercased() {
                    case "turbo": colormap = KV_COLORMAP_TURBO
                    case "viridis": colormap = KV_COLORMAP_VIRIDIS
                    case "inferno": colormap = KV_COLORMAP_INFERNO
                    case "gray", "grayscale": colormap = KV_COLORMAP_GRAY
                    default: break
                    }
                    idx += 1
                }
            case "--depth-min":
                if idx + 1 < args.count, let v = Float(args[idx + 1]) {
                    depthMinMm = v
                    idx += 1
                }
            case "--depth-max":
                if idx + 1 < args.count, let v = Float(args[idx + 1]) {
                    depthMaxMm = v
                    idx += 1
                }
            case "--verify-lock":
                verifyLockGuard = true
            case "--verify-barcode":
                verifyBarcode = true
            case "--verify-no-fallback":
                verifyNoFallback = true
            case "--test-exposure":
                testExposure = true
            case "--test-stream-latency":
                testStreamLatency = true
            case "--test-recording":
                testRecording = true
            case "--test-hardening":
                testHardening = true
            case "--serve-simulator-test":
                if idx + 1 < args.count {
                    serveSimQRFile = args[idx + 1]
                    idx += 1
                }
            default:
                break
            }
            idx += 1
        }

        if verifyLockGuard {
            runLockGuardVerification()
            return
        }
        if verifyNoFallback {
            runNoFallbackVerification()
            return
        }
        if testStreamLatency {
            runStreamLatencyVerification(useSynthetic: useSynthetic, durationSec: durationSec)
            return
        }
        if testRecording {
            if requestedReal {
                fputs("REFUSED: --test-recording is synthetic-only and refuses --real. Real sensor frames are never written by CLI recording tests.\n", stderr)
                exit(2)
            }
            runRecordingVerification(clipDurationSec: durationSec)
            return
        }
        if testHardening {
            runHardeningVerification(includeRealHardwareCycle: !useSynthetic)
            return
        }
        if let qrPath = serveSimQRFile {
            runSimulatorStreamServer(qrFilePath: qrPath, durationSec: durationSec)
            return
        }

        if !useSynthetic {
            var ownerBuf = [CChar](repeating: 0, count: 256)
            if kv_is_locked_by_other(nil, &ownerBuf, ownerBuf.count) {
                let owner = decodeCString(ownerBuf)
                print("LOCK_BLOCKED: ~/KINECT-IN-USE is held by another process (\(owner)). Waiting without opening device.")
                exit(3)
            }
        }

        guard let engine = kv_engine_create(nil) else {
            fputs("Failed to allocate kv_engine\n", stderr)
            exit(1)
        }

        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = mode
        params.sbs_layout = sbsLayout
        params.depth_colormap = colormap
        params.depth_min_mm = depthMinMm
        params.depth_max_mm = depthMaxMm
        params.clock_overlay = useSynthetic && verifyBarcode

        var errBuf = [CChar](repeating: 0, count: 256)
        let sourceKind: kv_source_kind_t = useSynthetic ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL
        let ok = kv_engine_start_ex(engine, sourceKind, pipeline, &params, testExposure || mode == KV_MODE_COLOR || (mode == KV_MODE_SIDE_BY_SIDE && sbsLayout != KV_SBS_IR_DEPTH), &errBuf, errBuf.count)
        if !ok {
            let err = decodeCString(errBuf)
            fputs("START_FAILED: \(err)\n", stderr)
            kv_engine_destroy(engine)
            exit(1)
        }

        let lockPath = (NSHomeDirectory() as NSString).appendingPathComponent("KINECT-IN-USE")
        if !useSynthetic {
            let lockExistsDuringRun = FileManager.default.fileExists(atPath: lockPath)
            print("KINECT_LOCK_ACQUIRED=\(lockExistsDuringRun)")
        }

        if testExposure {
            runExposureReadbackSequence(engine: engine)
        }

        let startNs = kv_monotonic_ns()
        let endNs = startNs + UInt64(durationSec * 1_000_000_000.0)
        var lastSeenSeq: UInt64 = 0
        var frameBuf = Data(count: 1920 * 1080 * 4)
        var lastMeta = kv_mapped_frame_t()

        var sliceIndex = 1
        let sliceLenSec: Double = 10.0
        var sliceLatenciesMs: [Double] = []
        var barcodeCheckedCount = 0
        var barcodeValidCount = 0

        while kv_monotonic_ns() < endNs {
            var reqBytes: Int = 0
            var meta = kv_mapped_frame_t()
            let copied = frameBuf.withUnsafeMutableBytes { ptr -> Bool in
                guard let base = ptr.bindMemory(to: UInt8.self).baseAddress else { return false }
                return kv_engine_copy_latest_frame(engine, lastSeenSeq, base, ptr.count, &reqBytes, &meta)
            }
            if copied && meta.frame_seq > lastSeenSeq {
                lastSeenSeq = meta.frame_seq
                lastMeta = meta
                let nowNs = kv_monotonic_ns()
                let totalHostLatencyMs = nowNs >= meta.arrival_mach_ns
                    ? Double(nowNs - meta.arrival_mach_ns) / 1e6
                    : 0.0
                sliceLatenciesMs.append(totalHostLatencyMs)

                if useSynthetic && verifyBarcode {
                    barcodeCheckedCount += 1
                    var decodedSeq: UInt32 = 0
                    var decodedTsUs: UInt32 = 0
                    let valid = frameBuf.withUnsafeBytes { ptr -> Bool in
                        guard let base = ptr.bindMemory(to: UInt8.self).baseAddress else { return false }
                        return kv_decode_clock_barcode(base, meta.width, meta.height, meta.stride_bytes, 4, &decodedSeq, &decodedTsUs)
                    }
                    let expectedSeq = UInt32(meta.frame_seq & 0xFFFFFF)
                    let expectedTsUs = UInt32((meta.arrival_mach_ns / 1000) & 0xFFFFFFFF)
                    if valid && decodedSeq == expectedSeq && decodedTsUs == expectedTsUs {
                        barcodeValidCount += 1
                    }
                }
            }

            let elapsedSec = Double(kv_monotonic_ns() - startNs) / 1e9
            if durationSec >= 20.0 && elapsedSec >= Double(sliceIndex) * sliceLenSec {
                var st = kv_stats_snapshot_t()
                kv_engine_get_stats(engine, &st)
                let meanLat = sliceLatenciesMs.isEmpty ? 0.0 : sliceLatenciesMs.reduce(0, +) / Double(sliceLatenciesMs.count)
                let maxLat = sliceLatenciesMs.max() ?? 0.0
                print(String(
                    format: "SLICE_%02ds: fps=%.2f ir_frames=%llu drops=%llu dt_mean_ms=%.2f dt_p95_ms=%.2f map_ms=%.3f arrival_to_consume_mean_ms=%.3f arrival_to_consume_max_ms=%.3f",
                    sliceIndex * Int(sliceLenSec),
                    st.current_fps,
                    st.ir_frames,
                    st.ir_seq_drops,
                    st.inter_frame_mean_ms,
                    st.inter_frame_p95_ms,
                    st.map_time_mean_ms,
                    meanLat,
                    maxLat
                ))
                sliceLatenciesMs.removeAll(keepingCapacity: true)
                sliceIndex += 1
            }

            usleep(2000)
        }

        var finalStats = kv_stats_snapshot_t()
        kv_engine_get_stats(engine, &finalStats)
        let wallSec = Double(kv_monotonic_ns() - startNs) / 1e9

        kv_engine_stop(engine)
        kv_engine_destroy(engine)

        if !useSynthetic {
            let lockExistsAfterStop = FileManager.default.fileExists(atPath: lockPath)
            print("KINECT_LOCK_RELEASED=\(!lockExistsAfterStop)")
        }

        let hist = withUnsafePointer(to: &finalStats.histogram16) { ptr -> [UInt32] in
            ptr.withMemoryRebound(to: UInt32.self, capacity: 16) { buf in
                Array(UnsafeBufferPointer(start: buf, count: 16))
            }
        }
        let serial = withUnsafeBytes(of: &finalStats.serial) { rawBuf in
            String(decoding: rawBuf.prefix { $0 != 0 }, as: UTF8.self)
        }
        let firmware = withUnsafeBytes(of: &finalStats.firmware) { rawBuf in
            String(decoding: rawBuf.prefix { $0 != 0 }, as: UTF8.self)
        }

        let avgRate: Double
        switch mode {
        case KV_MODE_COLOR:
            avgRate = Double(finalStats.color_frames) / max(0.001, wallSec)
        case KV_MODE_DEPTH:
            avgRate = Double(finalStats.depth_frames) / max(0.001, wallSec)
        default:
            avgRate = Double(finalStats.rendered_frames) / max(0.001, wallSec)
        }

        print("=== SUMMARY ===")
        print(String(format: "source=%@ mode=%u pipeline=%u output=%ux%u", useSynthetic ? "synthetic" : "real_kinect_v2", mode.rawValue, pipeline.rawValue, lastMeta.width, lastMeta.height))
        print("serial=\(serial) firmware=\(firmware)")
        print(String(format: "wall_sec=%.3f avg_fps=%.2f rolling_fps=%.2f color_fps=%.2f", wallSec, avgRate, finalStats.current_fps, finalStats.color_fps))
        print("ir_frames=\(finalStats.ir_frames) depth_frames=\(finalStats.depth_frames) color_frames=\(finalStats.color_frames) rendered_frames=\(finalStats.rendered_frames)")
        print("ir_seq_drops=\(finalStats.ir_seq_drops) depth_seq_drops=\(finalStats.depth_seq_drops) color_seq_drops=\(finalStats.color_seq_drops)")
        print(String(format: "dt_mean_ms=%.3f dt_p95_ms=%.3f dt_max_ms=%.3f", finalStats.inter_frame_mean_ms, finalStats.inter_frame_p95_ms, finalStats.inter_frame_max_ms))
        print(String(format: "map_mean_ms=%.3f map_max_ms=%.3f arrival_to_map_done_ms=%.3f", finalStats.map_time_mean_ms, finalStats.map_time_max_ms, finalStats.arrival_to_map_done_ms))
        print(String(format: "raw_min=%.2f raw_max=%.2f raw_mean=%.2f saturated_px=%u invalid_px=%u", finalStats.raw_min, finalStats.raw_max, finalStats.raw_mean, finalStats.saturated_pixels, finalStats.invalid_pixels))
        print(String(format: "scale_low=%.1f scale_high=%.1f mapped_luma_min=%u mapped_luma_max=%u mapped_luma_mean=%.2f", finalStats.smoothed_scale_low, finalStats.smoothed_scale_high, finalStats.mapped_luma_min, finalStats.mapped_luma_max, finalStats.mapped_luma_mean))
        print(String(format: "color_exposure=%.2f color_gain=%.2f rb_mode=%u rb_int_ms=%.2f rb_gain=%.2f color_minus_depth_ticks=%d", finalStats.color_exposure, finalStats.color_gain, finalStats.readback_exposure_mode, finalStats.readback_integration_ms, finalStats.readback_analog_gain, finalStats.color_minus_depth_ticks))
        print("benign_driver_notices=\(finalStats.benign_driver_notices) driver_errors=\(finalStats.driver_errors)")
        print("histogram16=\(hist.map(String.init).joined(separator: ","))")
        if verifyBarcode {
            print("barcode_verified=\(barcodeValidCount)/\(barcodeCheckedCount)")
        }
    }

    // MARK: - Phase 3 End-to-End Encrypted Streaming & Latency Verification

    private static func runStreamLatencyVerification(useSynthetic: Bool, durationSec: Double) {
        let tmpDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-cli-stream-test-\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.removeItem(at: tmpDir)
        try? FileManager.default.createDirectory(at: tmpDir, withIntermediateDirectories: true)
        let cleanupAndExit: @Sendable (Int32) -> Never = { code in
            try? FileManager.default.removeItem(at: tmpDir)
            exit(code)
        }
        defer { try? FileManager.default.removeItem(at: tmpDir) }

        if !useSynthetic {
            var ownerBuf = [CChar](repeating: 0, count: 256)
            if kv_is_locked_by_other(nil, &ownerBuf, ownerBuf.count) {
                print("LOCK_BLOCKED: \(decodeCString(ownerBuf))")
                cleanupAndExit(3)
            }
        }

        guard let engine = kv_engine_create(nil) else { cleanupAndExit(1) }
        defer {
            kv_engine_stop(engine)
            kv_engine_destroy(engine)
        }

        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = KV_MODE_IR
        params.clock_overlay = useSynthetic // Draw synthetic timestamp + 64-bit pixel barcode only on synthetic frames!

        var errBuf = [CChar](repeating: 0, count: 256)
        let src: kv_source_kind_t = useSynthetic ? KV_SOURCE_SYNTHETIC : KV_SOURCE_REAL
        guard kv_engine_start_ex(engine, src, KV_PIPELINE_OPENGL, &params, true, &errBuf, errBuf.count) else {
            fputs("Engine start failed: \(decodeCString(errBuf))\n", stderr)
            cleanupAndExit(1)
        }

        let encoder = VideoToolboxEncoder()
        let stateDir = tmpDir.appendingPathComponent("state", isDirectory: true)
        let keysDir = tmpDir.appendingPathComponent("keys", isDirectory: true)
        guard let throwawayKeyStore = try? TemporaryDirectoryHostIdentityStore(directoryURL: keysDir),
              let server = try? HostStreamServer(stateDirectoryURL: stateDir, identityStore: throwawayKeyStore) else {
            fputs("HostStreamServer failed to start\n", stderr)
            cleanupAndExit(1)
        }
        defer {
            server.shutdown()
            encoder.teardown()
        }

        final class ModeState: @unchecked Sendable {
            let lock = NSLock()
            var modeRaw: Int = 0
            var sbsRaw: Int = 0
        }
        let modeState = ModeState()

        encoder.onEncodedFrame = { encoded in
            server.broadcastEncodedFrame(encoded) {
                encoder.requestImmediateKeyframeAndConfig()
            }
        }
        server.onClientConnectedNeedsKeyframe = {
            encoder.requestImmediateKeyframeAndConfig()
            modeState.lock.lock()
            let mr = modeState.modeRaw
            let sr = modeState.sbsRaw
            modeState.lock.unlock()
            return HelloServerMessage(
                protocolVersion: 1,
                modeRaw: mr,
                sbsLayoutRaw: sr,
                allowPhoneModeSwitch: true,
                width: 1024,
                height: 848,
                codecRaw: WireCodecKind.h264.rawValue
            )
        }
        let engineBits = Int(bitPattern: engine)
        server.onClientRequestedMode = { newModeRaw, newSbsRaw in
            guard let engPtr = OpaquePointer(bitPattern: engineBits) else { return }
            modeState.lock.lock()
            modeState.modeRaw = newModeRaw
            modeState.sbsRaw = newSbsRaw
            modeState.lock.unlock()

            var p = kv_display_params_t()
            kv_default_display_params(&p)
            p.mode = kv_view_mode_t(rawValue: UInt32(newModeRaw))
            p.sbs_layout = kv_sbs_layout_t(rawValue: UInt32(newSbsRaw))
            p.clock_overlay = useSynthetic
            kv_engine_set_display_params(engPtr, &p)
            encoder.requestImmediateKeyframeAndConfig()
        }

        // 1. Verify Bonjour + 8-digit Short-Code Pairing first
        guard let shortOffer = try? server.beginPairingWindow(durationSec: 30.0),
              let shortClient = try? PocketStreamClientCore(persistence: InMemoryPersistenceProvider()) else {
            fputs("Failed to create short-code pairing test client\n", stderr)
            exit(1)
        }
        let shortPairSem = DispatchSemaphore(value: 0)
        final class BoolResultBox: @unchecked Sendable {
            var ok = false
            var msg = ""
        }
        let shortBox = BoolResultBox()
        shortClient.pairWithShortCode(shortOffer.shortCodeFormatted) { res in
            switch res {
            case .success: shortBox.ok = true
            case .failure(let e): shortBox.msg = e.localizedDescription
            }
            shortPairSem.signal()
        }
        _ = shortPairSem.wait(timeout: .now() + 10.0)
        print("BONJOUR_SHORTCODE_PAIR_OK=\(shortBox.ok) \(shortBox.msg)")
        shortClient.stop()
        server.unpairPhone()
        guard shortBox.ok else { exit(1) }

        // 2. Verify QR URI Pairing + Mutually-Authenticated TLS 1.3 Live Stream + Mode Switching + Latency
        guard let qrOffer = try? server.beginPairingWindow(durationSec: 30.0),
              let client = try? PocketStreamClientCore(persistence: InMemoryPersistenceProvider()) else {
            fputs("Failed to create QR pairing test client\n", stderr)
            exit(1)
        }

        final class LatencyCollector: @unchecked Sendable {
            let lock = NSLock()
            var displaySamplesMs: [Double] = []
            var barcodeSamplesMs: [Double] = []
            var sampleBuffersDelivered: Int = 0
            var resolutionsSeen: Set<String> = []
            var latestSnap = PocketClientTelemetry()
        }
        let collector = LatencyCollector()

        client.onSampleBufferReady = { sb in
            let dim = CMSampleBufferGetFormatDescription(sb).map { CMVideoFormatDescriptionGetDimensions($0) }
            collector.lock.lock()
            collector.sampleBuffersDelivered += 1
            if let d = dim {
                collector.resolutionsSeen.insert("\(d.width)x\(d.height)")
            }
            collector.lock.unlock()
        }
        client.onTelemetryUpdated = { snap in
            collector.lock.lock()
            collector.latestSnap = snap
            if snap.hostToDisplayLatencyMs > 0 {
                collector.displaySamplesMs.append(snap.hostToDisplayLatencyMs)
            }
            if snap.barcodePixelLatencyMs > 0 {
                collector.barcodeSamplesMs.append(snap.barcodePixelLatencyMs)
            }
            collector.lock.unlock()
        }

        let qrSem = DispatchSemaphore(value: 0)
        let qrBox = BoolResultBox()
        client.pairWithQRURI(qrOffer.qrURI) { res in
            switch res {
            case .success: qrBox.ok = true
            case .failure(let e): qrBox.msg = e.localizedDescription
            }
            qrSem.signal()
        }
        _ = qrSem.wait(timeout: .now() + 8.0)
        print("QR_URI_PAIR_OK=\(qrBox.ok) \(qrBox.msg)")
        guard qrBox.ok else { exit(1) }

        let startNs = kv_monotonic_ns()
        let endNs = startNs + UInt64(durationSec * 1_000_000_000.0)
        var lastSeq: UInt64 = 0
        var frameBuf = Data(count: 2048 * 1080 * 4)
        var switchedToDepth = false
        var switchedToColor = false
        var switchedToSbs = false

        while kv_monotonic_ns() < endNs {
            let elapsed = Double(kv_monotonic_ns() - startNs) / 1e9
            if elapsed > durationSec * 0.35 && !switchedToDepth {
                switchedToDepth = true
                client.requestModeSwitch(modeRaw: Int(KV_MODE_DEPTH.rawValue), sbsLayoutRaw: 0)
            } else if elapsed > durationSec * 0.60 && !switchedToColor {
                switchedToColor = true
                client.requestModeSwitch(modeRaw: Int(KV_MODE_COLOR.rawValue), sbsLayoutRaw: 0)
            } else if elapsed > durationSec * 0.80 && !switchedToSbs {
                switchedToSbs = true
                client.requestModeSwitch(modeRaw: Int(KV_MODE_SIDE_BY_SIDE.rawValue), sbsLayoutRaw: Int(KV_SBS_IR_DEPTH.rawValue))
            }

            var reqBytes = 0
            var meta = kv_mapped_frame_t()
            let copied = frameBuf.withUnsafeMutableBytes { ptr -> Bool in
                guard let base = ptr.bindMemory(to: UInt8.self).baseAddress else { return false }
                return kv_engine_copy_latest_frame(engine, lastSeq, base, ptr.count, &reqBytes, &meta)
            }
            if copied && meta.frame_seq > lastSeq {
                lastSeq = meta.frame_seq
                if server.pairedClientConnected {
                    modeState.lock.lock()
                    let mr = modeState.modeRaw
                    let sr = modeState.sbsRaw
                    modeState.lock.unlock()
                    frameBuf.withUnsafeBytes { rawPtr in
                        if let base = rawPtr.bindMemory(to: UInt8.self).baseAddress {
                            encoder.encodeBGRAFrame(
                                bgraPtr: base,
                                srcWidth: Int(meta.width),
                                srcHeight: Int(meta.height),
                                srcStride: Int(meta.stride_bytes),
                                frameSeq: meta.frame_seq,
                                hostCaptureNs: meta.arrival_mach_ns,
                                modeRaw: mr,
                                sbsRaw: sr,
                                hasClockOverlay: useSynthetic
                            )
                        }
                    }
                }
            }
            usleep(2000)
        }

        client.stop()

        collector.lock.lock()
        let dispSorted = collector.displaySamplesMs.sorted()
        let barSorted = collector.barcodeSamplesMs.sorted()
        let sbCount = collector.sampleBuffersDelivered
        let resList = collector.resolutionsSeen.sorted().joined(separator: ",")
        let snap = collector.latestSnap
        collector.lock.unlock()

        func pct(_ arr: [Double], _ q: Double) -> Double {
            guard !arr.isEmpty else { return 0.0 }
            let i = min(arr.count - 1, max(0, Int(Double(arr.count - 1) * q)))
            return arr[i]
        }

        let dispP50 = pct(dispSorted, 0.50)
        let dispP95 = pct(dispSorted, 0.95)
        let dispMax = dispSorted.last ?? 0.0

        let barP50 = pct(barSorted, 0.50)
        let barP95 = pct(barSorted, 0.95)
        let barMax = barSorted.last ?? 0.0

        print("=== PHASE 3 STREAM LATENCY SUMMARY ===")
        print("source=\(useSynthetic ? "synthetic" : "real_kinect_v2") duration_sec=\(String(format: "%.2f", durationSec))")
        print("server_sent_frames=\(server.sentFramesCount) congestion_drops=\(server.droppedCongestionFrames) rejected_handshakes=\(server.rejectedHandshakeCount)")
        print("client_sample_buffers=\(sbCount) client_fps=\(String(format: "%.2f", snap.fps)) bitrate_mbps=\(String(format: "%.2f", snap.bitrateMbps)) rtt_ms=\(String(format: "%.3f", snap.rttMs))")
        print("dynamic_resolutions_decoded=\(resList)")
        print(String(format: "host_to_display_ms: p50=%.2f p95=%.2f max=%.2f", dispP50, dispP95, dispMax))
        if useSynthetic {
            print(String(format: "barcode_pixel_glass_ms: p50=%.2f p95=%.2f max=%.2f verified_frames=%llu", barP50, barP95, barMax, snap.barcodeVerifiedFrames))
        }

        if sbCount < 30 || dispP95 >= 150.0 || (useSynthetic && (snap.barcodeVerifiedFrames < 20 || barP95 >= 150.0)) {
            fputs("LATENCY_OR_FRAME_CHECK_FAILED\n", stderr)
            cleanupAndExit(1)
        }
    }

    private static func runSimulatorStreamServer(qrFilePath: String, durationSec: Double) {
        let tmpDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-cli-sim-server-\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.removeItem(at: tmpDir)
        try? FileManager.default.createDirectory(at: tmpDir, withIntermediateDirectories: true)
        let cleanupAndExit: @Sendable (Int32) -> Never = { code in
            try? FileManager.default.removeItem(at: tmpDir)
            exit(code)
        }
        defer { try? FileManager.default.removeItem(at: tmpDir) }

        guard let engine = kv_engine_create(nil) else { cleanupAndExit(1) }
        defer {
            kv_engine_stop(engine)
            kv_engine_destroy(engine)
        }

        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = KV_MODE_DEPTH
        params.depth_colormap = KV_COLORMAP_TURBO
        params.clock_overlay = true

        var errBuf = [CChar](repeating: 0, count: 256)
        guard kv_engine_start_ex(engine, KV_SOURCE_SYNTHETIC, KV_PIPELINE_OPENGL, &params, true, &errBuf, errBuf.count) else {
            cleanupAndExit(1)
        }

        let encoder = VideoToolboxEncoder()
        let stateDir = tmpDir.appendingPathComponent("state", isDirectory: true)
        let keysDir = tmpDir.appendingPathComponent("keys", isDirectory: true)
        guard let throwawayKeyStore = try? TemporaryDirectoryHostIdentityStore(directoryURL: keysDir),
              let server = try? HostStreamServer(stateDirectoryURL: stateDir, identityStore: throwawayKeyStore) else {
            cleanupAndExit(1)
        }
        defer {
            server.shutdown()
            encoder.teardown()
        }

        encoder.onEncodedFrame = { encoded in
            server.broadcastEncodedFrame(encoded) {
                encoder.requestImmediateKeyframeAndConfig()
            }
        }
        server.onClientConnectedNeedsKeyframe = {
            encoder.requestImmediateKeyframeAndConfig()
            return HelloServerMessage(
                protocolVersion: 1,
                modeRaw: Int(KV_MODE_DEPTH.rawValue),
                sbsLayoutRaw: 0,
                allowPhoneModeSwitch: true,
                width: 1024,
                height: 848,
                codecRaw: WireCodecKind.h264.rawValue
            )
        }

        guard let offer = try? server.beginPairingWindow(durationSec: durationSec + 15.0) else { exit(1) }
        try? offer.qrURI.write(toFile: qrFilePath, atomically: true, encoding: .utf8)
        print("SIM_SERVER_READY qr_file=\(qrFilePath) short_code=\(offer.shortCodeFormatted)")
        fflush(stdout)

        let startNs = kv_monotonic_ns()
        let endNs = startNs + UInt64(durationSec * 1_000_000_000.0)
        var lastSeq: UInt64 = 0
        var frameBuf = Data(count: 1920 * 1080 * 4)

        while kv_monotonic_ns() < endNs {
            var reqBytes = 0
            var meta = kv_mapped_frame_t()
            let copied = frameBuf.withUnsafeMutableBytes { ptr -> Bool in
                guard let base = ptr.bindMemory(to: UInt8.self).baseAddress else { return false }
                return kv_engine_copy_latest_frame(engine, lastSeq, base, ptr.count, &reqBytes, &meta)
            }
            if copied && meta.frame_seq > lastSeq {
                lastSeq = meta.frame_seq
                if server.pairedClientConnected {
                    frameBuf.withUnsafeBytes { rawPtr in
                        if let base = rawPtr.bindMemory(to: UInt8.self).baseAddress {
                            encoder.encodeBGRAFrame(
                                bgraPtr: base,
                                srcWidth: Int(meta.width),
                                srcHeight: Int(meta.height),
                                srcStride: Int(meta.stride_bytes),
                                frameSeq: meta.frame_seq,
                                hostCaptureNs: meta.arrival_mach_ns,
                                modeRaw: Int(KV_MODE_DEPTH.rawValue),
                                sbsRaw: 0,
                                hasClockOverlay: true
                            )
                        }
                    }
                }
            }
            usleep(2000)
        }

        print(String(
            format: "SIM_SERVER_SUMMARY paired=%@ sent_frames=%llu bitrate_mbps=%.2f sim_display_lat_ms=%.2f sim_barcode_lat_ms=%.2f",
            server.isPhonePaired ? "true" : "false",
            server.sentFramesCount,
            server.currentBitrateMbps,
            server.lastClientDisplayLatencyMs,
            server.lastClientBarcodeLatencyMs
        ))
    }

    private static func runExposureReadbackSequence(engine: OpaquePointer) {
        usleep(500_000)
        var ctrl = kv_color_controls_t()
        var errBuf = [CChar](repeating: 0, count: 256)
        var st = kv_stats_snapshot_t()

        ctrl.mode = KV_EXPOSURE_MANUAL
        ctrl.manual_integration_ms = 20.0
        ctrl.manual_analog_gain = 2.5
        _ = kv_engine_set_color_controls(engine, &ctrl, &errBuf, errBuf.count)
        usleep(400_000)
        kv_engine_get_stats(engine, &st)
        print(String(format: "EXPOSURE_TEST_MANUAL_20MS_2.5G: rb_mode=%u rb_int_ms=%.2f rb_gain=%.2f footer_exp=%.2f footer_gain=%.2f", st.readback_exposure_mode, st.readback_integration_ms, st.readback_analog_gain, st.color_exposure, st.color_gain))

        ctrl.mode = KV_EXPOSURE_SEMI_AUTO
        ctrl.semi_auto_pseudo_ms = 15.0
        _ = kv_engine_set_color_controls(engine, &ctrl, &errBuf, errBuf.count)
        usleep(400_000)
        kv_engine_get_stats(engine, &st)
        print(String(format: "EXPOSURE_TEST_SEMIAUTO_15MS: rb_mode=%u rb_int_ms=%.2f rb_gain=%.2f footer_exp=%.2f footer_gain=%.2f", st.readback_exposure_mode, st.readback_integration_ms, st.readback_analog_gain, st.color_exposure, st.color_gain))

        ctrl.mode = KV_EXPOSURE_AUTO
        ctrl.auto_exposure_compensation = 0.0
        _ = kv_engine_set_color_controls(engine, &ctrl, &errBuf, errBuf.count)
        usleep(300_000)
    }

    private static func runNoFallbackVerification() {
        let tmpDir = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("tmp", isDirectory: true)
        try? FileManager.default.createDirectory(at: tmpDir, withIntermediateDirectories: true)
        let testLockURL = tmpDir.appendingPathComponent("KINECT-NO-FALLBACK-LOCK")
        try? FileManager.default.removeItem(at: testLockURL)

        guard let engine = kv_engine_create(testLockURL.path) else { exit(1) }
        var params = kv_display_params_t()
        kv_default_display_params(&params)
        var errBuf = [CChar](repeating: 0, count: 256)
        let bogusPipeline = kv_pipeline_kind_t(rawValue: 99)
        let started = kv_engine_start(engine, KV_SOURCE_REAL, bogusPipeline, &params, &errBuf, errBuf.count)
        let running = kv_engine_is_running(engine)
        let lockLeftBehind = FileManager.default.fileExists(atPath: testLockURL.path)
        let msg = decodeCString(errBuf)
        kv_engine_destroy(engine)
        print("NO_FALLBACK_HALTED=\(!started && !running)")
        print("NO_FALLBACK_LOCK_CLEAN=\(!lockLeftBehind)")
        print("NO_FALLBACK_ERROR=\(msg)")
        if started || running || lockLeftBehind {
            exit(1)
        }
    }

    private static func runLockGuardVerification() {
        let tmpDir = URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("tmp", isDirectory: true)
        try? FileManager.default.createDirectory(at: tmpDir, withIntermediateDirectories: true)
        let testLockURL = tmpDir.appendingPathComponent("KINECT-IN-USE-TEST")
        try? "agent: OtherMeasurementTool\npid: 99999\nopened_at: 2026-11-10T12:00:00Z\n".write(to: testLockURL, atomically: true, encoding: .utf8)

        guard let engine = kv_engine_create(testLockURL.path) else {
            fputs("Engine create failed\n", stderr)
            exit(1)
        }
        var params = kv_display_params_t()
        kv_default_display_params(&params)
        var errBuf = [CChar](repeating: 0, count: 256)
        let started = kv_engine_start(engine, KV_SOURCE_REAL, KV_PIPELINE_OPENGL, &params, &errBuf, errBuf.count)
        let errStr = decodeCString(errBuf)
        let stillExists = FileManager.default.fileExists(atPath: testLockURL.path)
        kv_engine_destroy(engine)
        let stillExistsAfterDestroy = FileManager.default.fileExists(atPath: testLockURL.path)
        try? FileManager.default.removeItem(at: testLockURL)

        print("LOCK_REFUSED_FOREIGN=\(!started)")
        print("LOCK_PRESERVED_FOREIGN=\(stillExists && stillExistsAfterDestroy)")
        print("LOCK_MESSAGE=\(errStr)")
        if started || !stillExists || !stillExistsAfterDestroy {
            exit(1)
        }
    }

    // MARK: - Phase 4 Host-Only Recording, Folder Exclusions, Lossless Bit-Exactness & MB/min Verification

    private static func runRecordingVerification(clipDurationSec: Double) {
        let isolatedDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-cli-rec-verify-\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.removeItem(at: isolatedDir)

        let recorder = HostRecorder(baseDirectoryURL: isolatedDir, skipTimeMachineSubprocess: true)
        let cleanupAndExit: @Sendable (Int32, Bool) -> Never = { code, useFastExit in
            _ = recorder.stopRecording()
            try? FileManager.default.removeItem(at: isolatedDir)
            if useFastExit {
                _exit(code)
            } else {
                exit(code)
            }
        }
        defer {
            _ = recorder.stopRecording()
            try? FileManager.default.removeItem(at: isolatedDir)
        }

        let hardTimeoutSec = max(30.0, clipDurationSec * 5.0 + 25.0)
        DispatchQueue.global(qos: .userInteractive).asyncAfter(deadline: .now() + hardTimeoutSec) {
            fputs("RECORDING_TEST_HARD_TIMEOUT (\(hardTimeoutSec)s)\n", stderr)
            cleanupAndExit(124, true)
        }

        guard let audit = try? recorder.ensureExcludedRecordingsDirectory() else {
            fputs("Failed to setup isolated recordings dir\n", stderr)
            cleanupAndExit(1, false)
        }
        print("=== PHASE 4 RECORDING EXCLUSION AUDIT ===")
        print("path=\(audit.basePath)")
        print("exists=\(audit.exists) has_nosync=\(audit.hasNoSyncFile) has_metadata_never_index=\(audit.hasMetadataNeverIndex) backup_excluded_resource=\(audit.resourceExcludedFromBackup) tmutil_excluded=\(audit.tmutilExcluded)")
        guard audit.exists && audit.hasNoSyncFile && audit.hasMetadataNeverIndex && (audit.tmutilExcluded || audit.resourceExcludedFromBackup) else {
            fputs("EXCLUSION_AUDIT_FAILED\n", stderr)
            cleanupAndExit(1, false)
        }

        // Verify Colour and Side-by-Side modes strictly refuse to start recording and create zero session folders
        for forbiddenMode in [KV_MODE_COLOR, KV_MODE_SIDE_BY_SIDE] {
            var rejected = false
            do {
                _ = try recorder.startRecording(mode: forbiddenMode, width: 1920, height: 1080, losslessOption: .ffv1Mkv)
            } catch let err as HostRecorderError where err == .colourModeForbidden {
                rejected = true
            } catch {}
            let leftoverDirs = (try? FileManager.default.contentsOfDirectory(atPath: isolatedDir.path))?.filter { $0.hasSuffix(".nosync") && $0 != ".nosync" } ?? []
            guard rejected && !recorder.currentStatus.isRecording && leftoverDirs.isEmpty else {
                fputs("Expected colour/side-by-side mode \(forbiddenMode.rawValue) to be forbidden!\n", stderr)
                cleanupAndExit(1, false)
            }
        }
        print("COLOUR_RECORDING_BLOCKED=true")

        guard let engine = kv_engine_create(nil) else { cleanupAndExit(1, false) }
        defer {
            kv_engine_stop(engine)
            kv_engine_destroy(engine)
        }

        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = KV_MODE_IR
        var errBuf = [CChar](repeating: 0, count: 256)
        guard kv_engine_start_ex(engine, KV_SOURCE_SYNTHETIC, KV_PIPELINE_OPENGL, &params, false, &errBuf, errBuf.count) else {
            fputs("Engine start failed: \(decodeCString(errBuf))\n", stderr)
            kv_engine_destroy(engine)
            cleanupAndExit(1, false)
        }

        usleep(400_000) // Warmup streams

        struct CaseSpec {
            let label: String
            let mode: kv_view_mode_t
            let width: Int
            let height: Int
            let losslessOpt: Lossless16ContainerOption
        }

        let cases: [CaseSpec] = [
            CaseSpec(label: "IR_16BIT_FFV1_MKV", mode: KV_MODE_IR, width: 512, height: 424, losslessOpt: .ffv1Mkv),
            CaseSpec(label: "IR_16BIT_PNG_SEQ", mode: KV_MODE_IR, width: 512, height: 424, losslessOpt: .png16Sequence),
            CaseSpec(label: "DEPTH_16MM_FFV1_MKV", mode: KV_MODE_DEPTH, width: 512, height: 424, losslessOpt: .ffv1Mkv)
        ]

        var rawScratch = [UInt16](repeating: 0, count: 512 * 424)

        for tc in cases {
            params.mode = tc.mode
            kv_engine_set_display_params(engine, &params)
            usleep(120_000)

            guard let sessionDir = try? recorder.startRecording(mode: tc.mode, width: tc.width, height: tc.height, losslessOption: tc.losslessOpt) else {
                fputs("Failed to start recording case \(tc.label)\n", stderr)
                kv_engine_stop(engine)
                kv_engine_destroy(engine)
                cleanupAndExit(1, false)
            }

            var firstRawFrameCaptured: [UInt16]?
            var lastRawSeq: UInt64 = 0
            let tEnd = kv_monotonic_ns() + UInt64(clipDurationSec * 1_000_000_000.0)

            while kv_monotonic_ns() < tEnd {
                var rSeq: UInt64 = 0
                var rArr: UInt64 = 0
                var rTicks: UInt32 = 0
                var rW: UInt32 = 0
                var rH: UInt32 = 0
                let got = rawScratch.withUnsafeMutableBufferPointer { buf -> Bool in
                    guard let base = buf.baseAddress else { return false }
                    return kv_engine_copy_latest_raw16(engine, tc.mode == KV_MODE_DEPTH, lastRawSeq, base, buf.count, &rSeq, &rArr, &rTicks, &rW, &rH)
                }
                if got && rSeq > lastRawSeq {
                    lastRawSeq = rSeq
                    let copyArr = Array(rawScratch.prefix(Int(rW * rH)))
                    if firstRawFrameCaptured == nil {
                        firstRawFrameCaptured = copyArr
                    }
                    recorder.appendRaw16Frame(pixelsU16: copyArr, width: Int(rW), height: Int(rH), seq: rSeq, deviceTicks: rTicks, arrivalNs: rArr)
                }
                usleep(2000)
            }

            let finalStatus = recorder.stopRecording()

            // Mathematically verify synthetic 16-bit bit-exactness (max_abs_diff == 0)
            var losslessNote = ""
            if let expected = firstRawFrameCaptured {
                if tc.losslessOpt == .ffv1Mkv, let ffmpegPath = HostRecorder.resolveFFmpegBinary() {
                    let stem = tc.mode == KV_MODE_IR ? "ir16" : "depth16mm"
                    let mkvFile = sessionDir.appendingPathComponent("\(stem)_lossless.mkv")
                    let rawOutFile = sessionDir.appendingPathComponent("verify_frame0.raw")
                    let p = Process()
                    p.executableURL = URL(fileURLWithPath: ffmpegPath)
                    p.arguments = ["-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", mkvFile.path, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "gray16le", rawOutFile.path]
                    p.standardInput = FileHandle.nullDevice
                    p.standardOutput = FileHandle.nullDevice
                    p.standardError = FileHandle.nullDevice
                    try? p.run()
                    let exitedClean = HostRecorder.waitProcess(p, timeoutSec: 5.0)
                    guard exitedClean, p.terminationStatus == 0,
                          let decodedData = try? Data(contentsOf: rawOutFile),
                          decodedData.count == expected.count * 2 else {
                        fputs("FFV1 decode check failed or timed out for \(tc.label)\n", stderr)
                        kv_engine_stop(engine)
                        kv_engine_destroy(engine)
                        cleanupAndExit(1, false)
                    }
                    var maxDiff = 0
                    decodedData.withUnsafeBytes { rawPtr in
                        let u16Ptr = rawPtr.bindMemory(to: UInt16.self)
                        for i in 0..<expected.count {
                            let d = abs(Int(u16Ptr[i]) - Int(expected[i]))
                            if d > maxDiff { maxDiff = d }
                        }
                    }
                    losslessNote = " lossless_max_abs_diff=\(maxDiff)"
                    if maxDiff != 0 {
                        fputs("FFV1 bit-exact check failed (maxDiff=\(maxDiff))!\n", stderr)
                        kv_engine_stop(engine)
                        kv_engine_destroy(engine)
                        cleanupAndExit(1, false)
                    }
                } else if tc.losslessOpt == .png16Sequence {
                    let pngFile = sessionDir.appendingPathComponent("ir16_000000.png")
                    if let srcImg = CGImageSourceCreateWithURL(pngFile as CFURL, nil),
                       let props = CGImageSourceCopyPropertiesAtIndex(srcImg, 0, nil) as? [CFString: Any],
                       let depthBits = props[kCGImagePropertyDepth] as? Int {
                        losslessNote = " png_bit_depth=\(depthBits)"
                        if depthBits != 16 {
                            fputs("Expected 16-bit PNG, got \(depthBits)!\n", stderr)
                            kv_engine_stop(engine)
                            kv_engine_destroy(engine)
                            cleanupAndExit(1, false)
                        }
                    }
                }
            }

            // Clean up test session directory immediately after measuring byte counts
            try? FileManager.default.removeItem(at: sessionDir)

            print(String(
                format: "REC_CASE %@: frames=%llu bytes=%llu elapsed_s=%.2f mb_per_min=%.2f%@",
                tc.label,
                finalStatus.framesWritten,
                finalStatus.bytesOnDisk,
                finalStatus.elapsedSec,
                finalStatus.megabytesPerMinute,
                losslessNote
            ))

            if finalStatus.framesWritten == 0 || finalStatus.bytesOnDisk == 0 {
                fputs("Recording produced 0 bytes for \(tc.label)\n", stderr)
                kv_engine_stop(engine)
                kv_engine_destroy(engine)
                cleanupAndExit(1, false)
            }
        }
    }

    // MARK: - Phase 5 Hardening Suite (Unpaired Zero-Leak, Two Phones, Reconnects, Unplug/Sleep Cycle)

    private static func runHardeningVerification(includeRealHardwareCycle: Bool) {
        let tmpDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("kv-cli-hardening-\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.removeItem(at: tmpDir)
        try? FileManager.default.createDirectory(at: tmpDir, withIntermediateDirectories: true)
        let cleanupAndExit: @Sendable (Int32, Bool) -> Never = { code, fast in
            try? FileManager.default.removeItem(at: tmpDir)
            if fast {
                _exit(code)
            } else {
                exit(code)
            }
        }
        defer { try? FileManager.default.removeItem(at: tmpDir) }

        DispatchQueue.global(qos: .userInteractive).asyncAfter(deadline: .now() + 45.0) {
            fputs("HARDENING_TEST_HARD_TIMEOUT (45s)\n", stderr)
            cleanupAndExit(124, true)
        }

        guard let engine = kv_engine_create(nil) else { cleanupAndExit(1, false) }
        defer {
            kv_engine_stop(engine)
            kv_engine_destroy(engine)
        }

        var params = kv_display_params_t()
        kv_default_display_params(&params)
        params.mode = KV_MODE_IR
        var errBuf = [CChar](repeating: 0, count: 256)
        guard kv_engine_start_ex(engine, KV_SOURCE_SYNTHETIC, KV_PIPELINE_OPENGL, &params, false, &errBuf, errBuf.count) else {
            fputs("Synthetic engine start failed: \(decodeCString(errBuf))\n", stderr)
            cleanupAndExit(1, false)
        }

        let encoder = VideoToolboxEncoder()
        let stateDir = tmpDir.appendingPathComponent("state", isDirectory: true)
        let keysDir = tmpDir.appendingPathComponent("keys", isDirectory: true)
        guard let throwawayKeyStore = try? TemporaryDirectoryHostIdentityStore(directoryURL: keysDir),
              let initialServer = try? HostStreamServer(stateDirectoryURL: stateDir, identityStore: throwawayKeyStore) else {
            fputs("HostStreamServer failed to start\n", stderr)
            cleanupAndExit(1, false)
        }
        var server: HostStreamServer? = initialServer
        defer {
            server?.shutdown()
            encoder.teardown()
        }

        final class ServerRefBox: @unchecked Sendable {
            let lock = NSLock()
            var ref: HostStreamServer?
            init(_ r: HostStreamServer?) { self.ref = r }
            func get() -> HostStreamServer? { lock.lock(); defer { lock.unlock() }; return ref }
            func set(_ r: HostStreamServer?) { lock.lock(); ref = r; lock.unlock() }
        }
        let srvBox = ServerRefBox(server)

        func wireCallbacks(_ srv: HostStreamServer) {
            srv.onClientConnectedNeedsKeyframe = {
                encoder.requestImmediateKeyframeAndConfig()
                return HelloServerMessage(
                    protocolVersion: 1,
                    modeRaw: 0,
                    sbsLayoutRaw: 0,
                    allowPhoneModeSwitch: true,
                    width: 1024,
                    height: 848,
                    codecRaw: WireCodecKind.h264.rawValue
                )
            }
        }
        wireCallbacks(server!)

        encoder.onEncodedFrame = { encoded in
            srvBox.get()?.broadcastEncodedFrame(encoded) {
                encoder.requestImmediateKeyframeAndConfig()
            }
        }

        // Background pump feeding synthetic/real frames into VideoToolbox whenever paired client is connected
        final class PumpFlag: @unchecked Sendable {
            let lock = NSLock()
            var running = true
            func isRunning() -> Bool { lock.lock(); defer { lock.unlock() }; return running }
            func stop() { lock.lock(); running = false; lock.unlock() }
        }
        let pumpFlag = PumpFlag()
        let engineBits = Int(bitPattern: engine)
        let pumpDone = DispatchSemaphore(value: 0)
        DispatchQueue.global(qos: .userInteractive).async {
            guard let engPtr = OpaquePointer(bitPattern: engineBits) else {
                pumpDone.signal()
                return
            }
            var lastSeq: UInt64 = 0
            var buf = Data(count: 1024 * 848 * 4)
            while pumpFlag.isRunning() {
                var req = 0
                var meta = kv_mapped_frame_t()
                let got = buf.withUnsafeMutableBytes { ptr -> Bool in
                    guard let base = ptr.bindMemory(to: UInt8.self).baseAddress else { return false }
                    return kv_engine_copy_latest_frame(engPtr, lastSeq, base, ptr.count, &req, &meta)
                }
                if got {
                    lastSeq = meta.frame_seq
                    if srvBox.get()?.pairedClientConnected == true {
                        buf.withUnsafeBytes { rawPtr in
                            if let base = rawPtr.bindMemory(to: UInt8.self).baseAddress {
                                encoder.encodeBGRAFrame(
                                    bgraPtr: base,
                                    srcWidth: Int(meta.width),
                                    srcHeight: Int(meta.height),
                                    srcStride: Int(meta.stride_bytes),
                                    frameSeq: meta.frame_seq,
                                    hostCaptureNs: meta.arrival_mach_ns,
                                    modeRaw: 0,
                                    sbsRaw: 0,
                                    hasClockOverlay: false
                                )
                            }
                        }
                    }
                }
                usleep(3000)
            }
            pumpDone.signal()
        }
        defer {
            pumpFlag.stop()
            _ = pumpDone.wait(timeout: .now() + 2.0)
        }

        print("=== PHASE 5 HARDENING VERIFICATION ===")

        // 1. Unpaired Zero-Leakage Test (mDNS silent + raw TCP probe + wrong short-code single-attempt burn)
        final class BrowseCountBox: @unchecked Sendable {
            let lock = NSLock()
            var names: [String] = []
            func update(_ n: [String]) { lock.lock(); names = n; lock.unlock() }
            func get() -> [String] { lock.lock(); defer { lock.unlock() }; return names }
        }
        let unpairedBrowse = BrowseCountBox()
        let bQueue = DispatchQueue(label: "test.mdns.probe")
        let probeBrowser = NWBrowser(for: .bonjour(type: BonjourConstants.streamServiceType, domain: BonjourConstants.domain), using: .tcp)
        probeBrowser.browseResultsChangedHandler = { res, _ in
            let found = res.compactMap { item -> String? in
                if case let .service(name: n, type: _, domain: _, interface: _) = item.endpoint { return n }
                return nil
            }
            unpairedBrowse.update(found)
        }
        probeBrowser.start(queue: bQueue)
        usleep(450_000)
        let leakedWhenUnpaired = unpairedBrowse.get().filter { $0 == server!.serviceId }.count

        // Raw TCP HTTP probe to streamPort
        final class ByteCountBox: @unchecked Sendable {
            let lock = NSLock()
            var bytes = 0
            func add(_ n: Int) { lock.lock(); bytes += n; lock.unlock() }
            func get() -> Int { lock.lock(); defer { lock.unlock() }; return bytes }
        }
        let rawBytesBox = ByteCountBox()
        let rawSem = DispatchSemaphore(value: 0)
        let rawConn = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: server!.streamPort)!, using: .tcp)
        rawConn.stateUpdateHandler = { st in
            if case .ready = st {
                rawConn.send(content: Data("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n".utf8), completion: .contentProcessed { _ in })
                rawConn.receive(minimumIncompleteLength: 1, maximumLength: 4096) { d, _, _, _ in
                    // Note: raw TCP might see TLS ServerHello/Alert record bytes, NEVER 'KV' application header!
                    if let d = d, d.count >= 2, d[d.startIndex] == 0x4B, d[d.startIndex + 1] == 0x56 {
                        rawBytesBox.add(d.count)
                    }
                    rawSem.signal()
                }
            } else if case .failed = st {
                rawSem.signal()
            }
        }
        rawConn.start(queue: bQueue)
        _ = rawSem.wait(timeout: .now() + 1.5)
        rawConn.cancel()

        // Single-attempt pairing burn test: wrong short code must immediately close offer!
        guard let burnOffer = try? server!.beginPairingWindow(durationSec: 30.0),
              let attackerClient = try? PocketStreamClientCore(persistence: InMemoryPersistenceProvider()) else { exit(1) }
        let wrongCode = burnOffer.shortCodeNormalized == "00000000" ? "99999999" : "00000000"
        let burnSem1 = DispatchSemaphore(value: 0)
        final class BoolBox: @unchecked Sendable {
            let lock = NSLock()
            var val = false
            func set(_ v: Bool) { lock.lock(); val = v; lock.unlock() }
            func get() -> Bool { lock.lock(); defer { lock.unlock() }; return val }
        }
        let wrongAccepted = BoolBox()
        attackerClient.pairWithShortCode(wrongCode, hostOverride: "127.0.0.1", portOverride: burnOffer.port) { res in
            if case .success = res { wrongAccepted.set(true) }
            burnSem1.signal()
        }
        _ = burnSem1.wait(timeout: .now() + 3.0)
        usleep(100_000)
        let offerBurned = (server!.currentPairingOffer == nil)

        print("UNPAIRED_TEST: mdns_advertised_when_unpaired=\(leakedWhenUnpaired) app_bytes_leaked_to_raw_probe=\(rawBytesBox.get()) wrong_code_accepted=\(wrongAccepted.get()) offer_burned_immediately=\(offerBurned)")
        guard leakedWhenUnpaired == 0 && rawBytesBox.get() == 0 && !wrongAccepted.get() && offerBurned else {
            fputs("UNPAIRED_SECURITY_CHECK_FAILED\n", stderr)
            exit(1)
        }

        // 2. Two-Phone Exclusivity Test (Phone A paired & streaming; Phone B rejected inside TLS 1.3 handshake)
        let storeA = InMemoryPersistenceProvider()
        let storeB = InMemoryPersistenceProvider()
        guard let phoneA = try? PocketStreamClientCore(persistence: storeA),
              let phoneB = try? PocketStreamClientCore(persistence: storeB),
              let offerA = try? server!.beginPairingWindow(durationSec: 30.0) else { exit(1) }

        let pairASem = DispatchSemaphore(value: 0)
        let pairAOk = BoolBox()
        phoneA.pairWithQRURI(offerA.qrURI) { res in
            if case .success = res { pairAOk.set(true) }
            pairASem.signal()
        }
        _ = pairASem.wait(timeout: .now() + 5.0)
        usleep(800_000) // Let Phone A receive ~20 frames

        let phoneAFramesBefore = phoneA.telemetry.receivedFrames
        let rejectsBeforeB = server!.rejectedHandshakeCount

        // Extract server cert hash from offerA URI so Phone B even knows the exact server pin + port, yet lacks pinned client key!
        let parsedQR = QRPairingPayload.parse(offerA.qrURI)!
        let phoneBHandshakeSem = DispatchSemaphore(value: 0)
        let phoneBAccepted = BoolBox()
        phoneB.connectDirect(host: "127.0.0.1", port: server!.streamPort, pinnedServerHashOverride: parsedQR.serverCertSHA256) { accepted, _ in
            phoneBAccepted.set(accepted)
            phoneBHandshakeSem.signal()
        }
        _ = phoneBHandshakeSem.wait(timeout: .now() + 3.0)
        usleep(500_000)

        let phoneAFramesAfterAttack = phoneA.telemetry.receivedFrames
        let phoneBFrames = phoneB.telemetry.receivedFrames
        let rejectsAfterB = server!.rejectedHandshakeCount

        // Check Bonjour service name opacity while paired
        let activeSvcName = server!.serviceId
        let isOpaqueHex = activeSvcName.count == 12 && activeSvcName.allSatisfy { $0.isHexDigit }

        print("TWO_PHONE_TEST_PART1: phoneA_paired=\(pairAOk.get()) phoneA_frames=\(phoneAFramesBefore)->\(phoneAFramesAfterAttack) phoneB_accepted=\(phoneBAccepted.get()) phoneB_frames=\(phoneBFrames) server_rejected_handshakes=\(rejectsBeforeB)->\(rejectsAfterB) bonjour_id_opaque_12hex=\(isOpaqueHex)")
        guard pairAOk.get() && phoneAFramesAfterAttack > phoneAFramesBefore && !phoneBAccepted.get() && phoneBFrames == 0 && rejectsAfterB > rejectsBeforeB && isOpaqueHex else {
            fputs("TWO_PHONE_PART1_FAILED\n", stderr)
            exit(1)
        }

        // Now Re-Pair Phone B on Mac -> Phone A must be kicked/rejected, and ONLY Phone B works!
        phoneB.stop()
        guard let offerB = try? server!.beginPairingWindow(durationSec: 30.0) else { exit(1) }
        let pairBSem = DispatchSemaphore(value: 0)
        let pairBOk = BoolBox()
        phoneB.pairWithQRURI(offerB.qrURI) { res in
            if case .success = res { pairBOk.set(true) }
            pairBSem.signal()
        }
        _ = pairBSem.wait(timeout: .now() + 5.0)
        usleep(800_000)

        // Phone A tries to reconnect using its old pinned cert -> must be rejected!
        phoneA.stop()
        let phoneAReconnectSem = DispatchSemaphore(value: 0)
        let phoneAReconnectAccepted = BoolBox()
        phoneA.connectDirect(host: "127.0.0.1", port: server!.streamPort, pinnedServerHashOverride: parsedQR.serverCertSHA256) { accepted, _ in
            phoneAReconnectAccepted.set(accepted)
            phoneAReconnectSem.signal()
        }
        _ = phoneAReconnectSem.wait(timeout: .now() + 3.0)
        let phoneBFramesActive = phoneB.telemetry.receivedFrames
        print("TWO_PHONE_TEST_PART2_REPAIR: phoneB_paired=\(pairBOk.get()) phoneB_frames=\(phoneBFramesActive) displaced_phoneA_accepted=\(phoneAReconnectAccepted.get())")
        phoneA.stop()
        probeBrowser.cancel()
        guard pairBOk.get() && phoneBFramesActive >= 5 && !phoneAReconnectAccepted.get() else {
            fputs("TWO_PHONE_REPAIR_EXCLUSIVITY_FAILED\n", stderr)
            exit(1)
        }

        // 3. Reconnect Tests: (a) Socket Drop Reconnect, and (b) Full Server Restart on Brand-New Ephemeral Port via Bonjour!
        let framesBeforeDrop = phoneB.telemetry.receivedFrames
        let dropStartNs = kv_monotonic_ns()
        server!.disconnectActiveClient()
        var dropRecoveredMs: Double = -1.0
        while Double(kv_monotonic_ns() - dropStartNs) / 1e9 < 6.0 {
            if phoneB.telemetry.isConnected && phoneB.telemetry.receivedFrames > framesBeforeDrop + 2 {
                dropRecoveredMs = Double(kv_monotonic_ns() - dropStartNs) / 1e6
                break
            }
            usleep(20_000)
        }
        print(String(format: "RECONNECT_SOCKET_DROP: recovered=%@ recovery_ms=%.1f reconnect_count=%d", dropRecoveredMs > 0 ? "true" : "false", dropRecoveredMs, phoneB.telemetry.reconnectCount))
        guard dropRecoveredMs > 0 else {
            fputs("SOCKET_DROP_RECONNECT_FAILED\n", stderr)
            exit(1)
        }

        // (b) Full server shutdown & restart on a DIFFERENT OS-assigned port (verifying stale port cache clear + Bonjour _kv2s._tcp discovery!)
        let oldPort = server!.streamPort
        let restartStartNs = kv_monotonic_ns()
        srvBox.set(nil)
        server!.shutdown()
        server = nil
        usleep(250_000)

        guard let restartedServer = try? HostStreamServer(stateDirectoryURL: stateDir, identityStore: throwawayKeyStore) else {
            fputs("Failed to restart HostStreamServer\n", stderr)
            cleanupAndExit(1, false)
        }
        server = restartedServer
        wireCallbacks(restartedServer)
        srvBox.set(restartedServer)
        let newPort = restartedServer.streamPort
        let framesAtRestart = phoneB.telemetry.receivedFrames

        var portChangeRecoveredMs: Double = -1.0
        while Double(kv_monotonic_ns() - restartStartNs) / 1e9 < 8.0 {
            if phoneB.telemetry.isConnected && phoneB.telemetry.receivedFrames > framesAtRestart + 2 {
                portChangeRecoveredMs = Double(kv_monotonic_ns() - restartStartNs) / 1e6
                break
            }
            usleep(20_000)
        }
        print(String(format: "RECONNECT_SERVER_RESTART_BONJOUR: old_port=%u new_port=%u recovered=%@ recovery_ms=%.1f", oldPort, newPort, portChangeRecoveredMs > 0 ? "true" : "false", portChangeRecoveredMs))
        guard portChangeRecoveredMs > 0 else {
            fputs("SERVER_RESTART_BONJOUR_RECONNECT_FAILED\n", stderr)
            exit(1)
        }

        // 4. Real Hardware Stop -> Lock Release -> Reopen Cycle (Simulating USB Unplug/Replug & Mac Sleep/Wake)
        if includeRealHardwareCycle {
            var ownerBuf = [CChar](repeating: 0, count: 256)
            if kv_is_locked_by_other(nil, &ownerBuf, ownerBuf.count) {
                print("REAL_CYCLE_SKIPPED_LOCKED_BY_OTHER=\(decodeCString(ownerBuf))")
            } else {
                let lockPath = (NSHomeDirectory() as NSString).appendingPathComponent("KINECT-IN-USE")
                kv_engine_stop(engine) // Stop synthetic

                let ok1 = kv_engine_start_ex(engine, KV_SOURCE_REAL, KV_PIPELINE_OPENGL, &params, false, &errBuf, errBuf.count)
                let lockHeld1 = FileManager.default.fileExists(atPath: lockPath)
                usleep(1_000_000)
                let framesPhase1 = phoneB.telemetry.receivedFrames

                // Simulate Sleep / Unplug teardown
                kv_engine_stop(engine)
                let lockReleasedMid = !FileManager.default.fileExists(atPath: lockPath)
                usleep(500_000)

                // Simulate Wake / Replug reopen
                let reopenT0 = kv_monotonic_ns()
                let ok2 = kv_engine_start_ex(engine, KV_SOURCE_REAL, KV_PIPELINE_OPENGL, &params, false, &errBuf, errBuf.count)
                encoder.requestImmediateKeyframeAndConfig()
                let lockHeld2 = FileManager.default.fileExists(atPath: lockPath)
                var firstFrameAfterReopenMs: Double = -1.0
                while Double(kv_monotonic_ns() - reopenT0) / 1e9 < 5.0 {
                    if phoneB.telemetry.receivedFrames > framesPhase1 + 3 {
                        firstFrameAfterReopenMs = Double(kv_monotonic_ns() - reopenT0) / 1e6
                        break
                    }
                    usleep(20_000)
                }
                kv_engine_stop(engine)
                let lockReleasedFinal = !FileManager.default.fileExists(atPath: lockPath)

                print(String(
                    format: "REAL_HARDWARE_SLEEP_REPLUG_CYCLE: open1=%@ lock1=%@ mid_lock_released=%@ reopen2=%@ lock2=%@ stream_resumed_ms=%.1f final_lock_clean=%@",
                    ok1 ? "true" : "false",
                    lockHeld1 ? "true" : "false",
                    lockReleasedMid ? "true" : "false",
                    ok2 ? "true" : "false",
                    lockHeld2 ? "true" : "false",
                    firstFrameAfterReopenMs,
                    lockReleasedFinal ? "true" : "false"
                ))
                guard ok1 && lockHeld1 && lockReleasedMid && ok2 && lockHeld2 && firstFrameAfterReopenMs > 0 && lockReleasedFinal else {
                    fputs("REAL_HARDWARE_CYCLE_FAILED\n", stderr)
                    exit(1)
                }
            }
        }

        phoneB.stop()
    }

    private static func decodeCString(_ buf: [CChar]) -> String {
        let bytes = buf.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
        return String(decoding: bytes, as: UTF8.self)
    }
}
