import AVFoundation
import Combine
import CryptoKit
import Foundation
import ImageIO
import Network
import SwiftUI
import UIKit
import VideoToolbox
import XCTest
@testable import KinectPocketViewer

final class PocketTestSyncBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var value: T

    init(_ initial: T) {
        self.value = initial
    }

    func update<R>(_ body: (inout T) -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        return body(&value)
    }

    func read() -> T {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
}

/// Self-contained synthetic loopback v3 Pet Camera host for iOS `KinectPocketTests`
/// (in-memory synthetic JPEG + synthetic H.264 frames; zero files written to disk).
final class PocketLoopbackV3Host: @unchecked Sendable {
    let serverIdentity: CryptoIdentity
    let serviceId = "iosloopback01"
    private let netQueue = DispatchQueue(label: "kinectviewer.pocket.tests.host", qos: .userInteractive)
    private var pairListener: NWListener?
    private var streamListener: NWListener?
    private var activeConnections: [NWConnection] = []
    private var pairedHash: Data?
    private var activeQRToken = Data(repeating: 0x5A, count: 16)
    private let greenThumbJPEG: Data
    private let parameterSets: [Data]
    private let avccPayloads: [Data]

    private(set) var pairPort: UInt16 = 0
    private(set) var streamPort: UInt16 = 0

    init() throws {
        self.serverIdentity = try CryptoIdentity.generateNew(commonName: "kv3-ios-loopback")
        self.greenThumbJPEG = Self.makeSolidGreenJPEG(width: 64, height: 48)
        let enc = Self.makeSyntheticH264Clip()
        self.parameterSets = enc.parameterSets
        self.avccPayloads = enc.avccPayloads
        try startPairListener()
        try startStreamListener()
    }

    func shutdown() {
        netQueue.sync {
            for c in activeConnections { c.cancel() }
            activeConnections.removeAll()
            pairListener?.cancel()
            pairListener = nil
            streamListener?.cancel()
            streamListener = nil
        }
    }

    func makeLanPairURI() -> String {
        QRPairingPayload(
            serviceId: serviceId,
            serverCertSHA256: serverIdentity.certificateSHA256,
            oneTimeToken: activeQRToken,
            hostIP: "127.0.0.1",
            pairPort: pairPort
        ).toURIString()
    }

    private static func makeSolidGreenJPEG(width: Int, height: Int) -> Data {
        var rgba = [UInt8](repeating: 0, count: width * height * 4)
        for i in stride(from: 0, to: rgba.count, by: 4) {
            rgba[i] = 0
            rgba[i + 1] = 255
            rgba[i + 2] = 0
            rgba[i + 3] = 255
        }
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let cg = CGImage(
                  width: width,
                  height: height,
                  bitsPerComponent: 8,
                  bitsPerPixel: 32,
                  bytesPerRow: width * 4,
                  space: colorSpace,
                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                  provider: provider,
                  decode: nil,
                  shouldInterpolate: false,
                  intent: .defaultIntent
              ),
              let out = CFDataCreateMutable(kCFAllocatorDefault, 0),
              let dest = CGImageDestinationCreateWithData(out, "public.jpeg" as CFString, 1, nil) else {
            return Data()
        }
        let opts: [CFString: Any] = [kCGImageDestinationLossyCompressionQuality: 0.98]
        CGImageDestinationAddImage(dest, cg, opts as CFDictionary)
        _ = CGImageDestinationFinalize(dest)
        return out as Data
    }

    private static func makeSyntheticH264Clip() -> (parameterSets: [Data], avccPayloads: [Data]) {
        let width = 320
        let height = 240
        var session: VTCompressionSession?
        let box = PocketTestSyncBox<([Data], [Data])>(([], []))
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: Int32(width),
            height: Int32(height),
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,
            refcon: nil,
            compressionSessionOut: &session
        )
        guard status == noErr, let comp = session else { return ([], []) }
        VTSessionSetProperty(comp, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(comp, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTCompressionSessionPrepareToEncodeFrames(comp)

        for f in 0..<4 {
            var pbOut: CVPixelBuffer?
            CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA, nil, &pbOut)
            guard let pb = pbOut else { continue }
            CVPixelBufferLockBaseAddress(pb, [])
            if let base = CVPixelBufferGetBaseAddress(pb) {
                memset(base, Int32(40 + f * 30), CVPixelBufferGetDataSize(pb))
            }
            CVPixelBufferUnlockBaseAddress(pb, [])
            let sem = DispatchSemaphore(value: 0)
            let pts = CMTime(value: Int64(f), timescale: 30)
            let props: CFDictionary? = (f == 0) ? ([kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary) : nil
            VTCompressionSessionEncodeFrame(comp, imageBuffer: pb, presentationTimeStamp: pts, duration: .invalid, frameProperties: props, infoFlagsOut: nil) { st, _, sb in
                defer { sem.signal() }
                guard st == noErr, let sb = sb, let fmt = CMSampleBufferGetFormatDescription(sb),
                      let dataBuf = CMSampleBufferGetDataBuffer(sb) else { return }
                var len = 0
                var ptr: UnsafeMutablePointer<Int8>?
                guard CMBlockBufferGetDataPointer(dataBuf, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr) == kCMBlockBufferNoErr,
                      let p = ptr, len > 0 else { return }
                let avcc = Data(bytes: p, count: len)
                var spsPtr: UnsafePointer<UInt8>?
                var spsLen = 0
                var ppsPtr: UnsafePointer<UInt8>?
                var ppsLen = 0
                var count = 0
                var nalHdr: Int32 = 4
                var psets: [Data] = []
                if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, parameterSetIndex: 0, parameterSetPointerOut: &spsPtr, parameterSetSizeOut: &spsLen, parameterSetCountOut: &count, nalUnitHeaderLengthOut: &nalHdr) == noErr,
                   let sp = spsPtr,
                   CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, parameterSetIndex: 1, parameterSetPointerOut: &ppsPtr, parameterSetSizeOut: &ppsLen, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr,
                   let pp = ppsPtr {
                    psets = [Data(bytes: sp, count: spsLen), Data(bytes: pp, count: ppsLen)]
                }
                box.update { state in
                    if state.0.isEmpty && !psets.isEmpty { state.0 = psets }
                    state.1.append(avcc)
                }
            }
            _ = sem.wait(timeout: .now() + 2.0)
        }
        VTCompressionSessionInvalidate(comp)
        return box.read()
    }

    private func startPairListener() throws {
        let sem = DispatchSemaphore(value: 0)
        let params = CryptoIdentity.makeTLSParameters(localIdentity: serverIdentity, requireClientCert: true, verifyQueue: netQueue) { _, _ in true }
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        let l = try NWListener(using: params)
        l.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.pairPort = l.port?.rawValue ?? 0
                sem.signal()
            }
        }
        l.newConnectionHandler = { [weak self] conn in
            self?.handlePairConn(conn)
        }
        self.pairListener = l
        l.start(queue: netQueue)
        _ = sem.wait(timeout: .now() + 5.0)
    }

    private func handlePairConn(_ conn: NWConnection) {
        activeConnections.append(conn)
        let reader = WireFrameReader()
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self, case .ready = st else { return }
            conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { data, _, _, _ in
                guard let data = data else { conn.cancel(); return }
                reader.append(data)
                guard case .frame(.pairRequest, _, let payload) = reader.nextFrame(),
                      let req = try? JSONDecoder().decode(PairRequestMessage.self, from: payload) else {
                    conn.cancel()
                    return
                }
                let clientHash = Data(SHA256.hash(data: req.clientCertDER))
                self.pairedHash = clientHash
                let serverProof = CryptoIdentity.computePairingProof(
                    secret: self.activeQRToken,
                    serverCertSHA256: self.serverIdentity.certificateSHA256,
                    clientCertSHA256: clientHash,
                    role: "kv2-pair-server"
                )
                let resp = PairResponseMessage(
                    accepted: true,
                    serviceId: self.serviceId,
                    streamPort: self.streamPort,
                    serverProofHMAC: serverProof,
                    message: "Paired",
                    role: .owner,
                    deviceId: "aa11bb22cc33dd44",
                    deviceName: "iPhone",
                    protocolVersion: 3
                )
                if let pkt = try? WirePacket.packJSON(type: .pairResponse, resp) {
                    conn.send(content: pkt, completion: .contentProcessed { _ in conn.cancel() })
                }
            }
        }
        conn.start(queue: netQueue)
    }

    private func startStreamListener() throws {
        let sem = DispatchSemaphore(value: 0)
        let params = CryptoIdentity.makeTLSParameters(localIdentity: serverIdentity, requireClientCert: true, verifyQueue: netQueue) { [weak self] peerHash, _ in
            guard let self = self, let expected = self.pairedHash else { return false }
            return CryptoIdentity.constantTimeEqual(peerHash, expected)
        }
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        let l = try NWListener(using: params)
        l.stateUpdateHandler = { [weak self] st in
            if case .ready = st {
                self?.streamPort = l.port?.rawValue ?? 0
                sem.signal()
            }
        }
        l.newConnectionHandler = { [weak self] conn in
            self?.handleStreamConn(conn)
        }
        self.streamListener = l
        l.start(queue: netQueue)
        _ = sem.wait(timeout: .now() + 5.0)
    }

    private func handleStreamConn(_ conn: NWConnection) {
        activeConnections.append(conn)
        let reader = WireFrameReader()
        conn.stateUpdateHandler = { [weak self] st in
            guard let self = self, case .ready = st else { return }
            let hello = HelloServerMessage(
                protocolVersion: 3,
                modeRaw: 1,
                sbsLayoutRaw: 0,
                allowPhoneModeSwitch: true,
                width: 320,
                height: 240,
                codecRaw: 0,
                petCamera: PetCameraHello(specVersion: 1, maxWatchers: 4, cameraRoll: true, outside: true, qualities: ["auto", "high", "medium", "low"])
            )
            if let pkt = try? WirePacket.packJSON(type: .helloServer, hello) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
            self.receiveStreamLoop(conn: conn, reader: reader, sentInitialState: PocketTestSyncBox<Bool>(false))
        }
        conn.start(queue: netQueue)
    }

    private func receiveStreamLoop(conn: NWConnection, reader: WireFrameReader, sentInitialState: PocketTestSyncBox<Bool>) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, err in
            guard let self = self else { return }
            if let data = data, !data.isEmpty {
                reader.append(data)
                while case .frame(let msgType, _, let payload) = reader.nextFrame() {
                    switch msgType {
                    case .ping:
                        let firstPing = sentInitialState.update { already -> Bool in
                            if already { return false }
                            already = true
                            return true
                        }
                        if firstPing {
                            self.sendInitialStateAndLiveFrames(conn: conn)
                        }
                    case .rollListReq:
                        if let req = try? JSONDecoder().decode(RollListRequestMessage.self, from: payload) {
                            self.replyRollList(conn: conn, req: req)
                        }
                    case .rollThumbReq:
                        if let req = try? JSONDecoder().decode(RollThumbRequestMessage.self, from: payload) {
                            let thumb = RollThumbMessage(requestId: req.requestId, clipId: req.clipId, jpeg: self.greenThumbJPEG)
                            if let pkt = try? WirePacket.packJSON(type: .rollThumb, thumb) {
                                conn.send(content: pkt, completion: .contentProcessed { _ in })
                            }
                        }
                    case .playbackReq:
                        if let req = try? JSONDecoder().decode(PlaybackRequestMessage.self, from: payload) {
                            self.replyPlayback(conn: conn, req: req)
                        }
                    default:
                        break
                    }
                }
            }
            if isComplete || err != nil {
                conn.cancel()
                return
            }
            self.receiveStreamLoop(conn: conn, reader: reader, sentInitialState: sentInitialState)
        }
    }

    private func sendInitialStateAndLiveFrames(conn: NWConnection) {
        let hs = HostStateSyncMessage.unconfirmedInitial
        if let pkt = try? WirePacket.packJSON(type: .hostStateSync, hs) {
            conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
        let ps = PetCameraStateMessage(
            protocolVersion: 3,
            stateSeq: 1,
            deviceId: "aa11bb22cc33dd44",
            deviceName: "iPhone",
            role: .owner,
            path: .home,
            sharingEnabled: true,
            sharingScheduleOff: false,
            outsideEnabled: false,
            privacyMode: false,
            liveAllowed: true,
            liveUnavailableReason: nil,
            watching: true,
            liveQuality: .auto,
            liveTier: .high,
            rollAvailable: true,
            playbackAvailable: true,
            rollEnabled: true,
            motionEnabled: false,
            motionAvailable: true,
            motionSensitivity: 5,
            scheduleEnabled: false,
            scheduleSummary: nil,
            clipRecording: false,
            clipTrigger: nil,
            clipManualActive: false,
            clipElapsedSec: 0,
            rollBlockedReason: nil,
            rollBlockedText: nil,
            outsideEndpoint: nil,
            outsideStatus: "off",
            outsideStatusText: nil,
            watchingNow: [],
            devices: []
        )
        if let pkt = try? WirePacket.packJSON(type: .petCameraState, ps) {
            conn.send(content: pkt, completion: .contentProcessed { _ in })
        }
        if !parameterSets.isEmpty {
            let cfg = StreamConfigMessage(
                streamEpoch: 1,
                codecRaw: 0,
                width: 320,
                height: 240,
                modeRaw: 1,
                sbsLayoutRaw: 0,
                nalLengthHeaderBytes: 4,
                parameterSets: parameterSets
            )
            if let pkt = try? WirePacket.packJSON(type: .streamConfig, cfg) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
            for (idx, avcc) in avccPayloads.enumerated() {
                let hdr = VideoAccessUnitHeader(streamEpoch: 1, frameSeq: UInt64(idx), hostCaptureNs: UInt64(idx) * 33_333_333, hostEncodeDoneNs: 0, flags: 0x01)
                let pkt = WirePacket.pack(type: .videoAccessUnit, payload: hdr.encode(withAvccPayload: avcc))
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
    }

    private func replyRollList(conn: NWConnection, req: RollListRequestMessage) {
        let storage = RollStorage(
            usedBytes: 512_000,
            capBytes: 10_000_000_000,
            freeBytes: 80_000_000_000,
            floorBytes: 20_000_000_000,
            keepDays: 7
        )
        if req.day == nil {
            let msg = RollListMessage(
                requestId: req.requestId,
                day: nil,
                days: [RollDay(day: "2026-11-20", clipCount: 2, starredCount: 0, bytes: 224_000)],
                clips: nil,
                nextCursor: nil,
                storage: storage
            )
            if let pkt = try? WirePacket.packJSON(type: .rollList, msg) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        } else {
            let clips = [
                RollClip(clipId: "2026-11-20/08-00-00_manual", startUnixMs: 1_795_161_600_000, startLocal: "2026-11-20T08:00:00+00:00", durationSec: 8.0, trigger: .manual, startedBy: "Owner", starred: false, starredByMe: false, width: 320, height: 240, bytes: 96_000, hasThumbnail: true),
                RollClip(clipId: "2026-11-20/08-05-00_motion", startUnixMs: 1_795_161_900_000, startLocal: "2026-11-20T08:05:00+00:00", durationSec: 12.0, trigger: .motion, startedBy: nil, starred: false, starredByMe: false, width: 320, height: 240, bytes: 128_000, hasThumbnail: true)
            ]
            let msg = RollListMessage(requestId: req.requestId, day: req.day, days: nil, clips: clips, nextCursor: nil, storage: storage)
            if let pkt = try? WirePacket.packJSON(type: .rollList, msg) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
    }

    private func replyPlayback(conn: NWConnection, req: PlaybackRequestMessage) {
        let action = PlaybackAction(rawValue: req.action) ?? .stop
        switch action {
        case .start:
            let clipId = req.clipId ?? "2026-11-20/08-00-00_manual"
            let cfg = PlaybackConfigMessage(streamId: 1, clipId: clipId, durationSec: 8.0, streamEpoch: 1, codecRaw: 0, width: 320, height: 240, modeRaw: 1, sbsLayoutRaw: 0, nalLengthHeaderBytes: 4, parameterSets: parameterSets)
            if let pkt = try? WirePacket.packJSON(type: .playbackConfig, cfg) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
            for (idx, avcc) in avccPayloads.enumerated() {
                let hdr = VideoAccessUnitHeader(streamEpoch: 1, frameSeq: UInt64(idx), hostCaptureNs: UInt64(idx) * 33_333_333, hostEncodeDoneNs: 0, flags: 0x01)
                let au = PlaybackAccessUnit(streamId: 1, header: hdr, avccPayload: avcc)
                let pkt = WirePacket.pack(type: .playbackAccessUnit, payload: au.encode())
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
            let st = PlaybackStateMessage(requestId: req.requestId, streamId: 1, clipId: clipId, state: .playing, positionSec: 0.0, durationSec: 8.0, message: nil)
            if let pkt = try? WirePacket.packJSON(type: .playbackState, st) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        default:
            let st = PlaybackStateMessage(requestId: req.requestId, streamId: req.streamId ?? 1, clipId: "2026-11-20/08-00-00_manual", state: action == .pause ? .paused : .stopped, positionSec: req.positionSec ?? 0.0, durationSec: 8.0, message: nil)
            if let pkt = try? WirePacket.packJSON(type: .playbackState, st) {
                conn.send(content: pkt, completion: .contentProcessed { _ in })
            }
        }
    }
}

@MainActor
final class PocketAppSnapshotAndSandboxTests: XCTestCase {

    private func waitUntil(
        timeout: TimeInterval = 5.0,
        pollMs: UInt64 = 20,
        condition: @escaping @MainActor () -> Bool
    ) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return true }
            try? await Task.sleep(nanoseconds: pollMs * 1_000_000)
        }
        return condition()
    }

    private func countBrightGreenPixels(in image: UIImage) -> Int {
        guard let cg = image.cgImage else { return 0 }
        let width = cg.width
        let height = cg.height
        var rgba = [UInt8](repeating: 0, count: width * height * 4)
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let ctx = CGContext(
            data: &rgba,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        ) else { return 0 }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: width, height: height))
        var matches = 0
        for i in stride(from: 0, to: rgba.count, by: 4) {
            let r = rgba[i]
            let g = rgba[i + 1]
            let b = rgba[i + 2]
            if g > 190 && r < 60 && b < 60 {
                matches += 1
            }
        }
        return matches
    }

    private func snapshotWindow(_ window: UIWindow) -> UIImage {
        let fmt = UIGraphicsImageRendererFormat()
        fmt.scale = 1.0
        let renderer = UIGraphicsImageRenderer(bounds: window.bounds, format: fmt)
        return renderer.image { _ in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: true)
        }
    }

    // MARK: - App-switcher snapshot hides camera-roll grid and thumbnail images on willResignActive

    func testFix4_WillResignActiveHidesCameraRollGridAndThumbnailsFromSnapshot() async throws {
        let host = try PocketLoopbackV3Host()
        defer { host.shutdown() }

        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Snapshot iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: true)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.makeLanPairURI()) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let connected = await waitUntil { vm.isPetOwner && vm.cameraRoll.supportsCameraRoll }
        XCTAssertTrue(connected)

        await vm.cameraRoll.refreshDays(force: true)
        let clips = vm.cameraRoll.selectedDayClips
        XCTAssertEqual(clips.count, 2)
        for clip in clips {
            vm.cameraRoll.requestThumbnailIfNeeded(for: clip)
        }
        let thumbsLoaded = await waitUntil {
            clips.allSatisfy {
                if case .loaded = vm.cameraRoll.thumbnails[$0.clipId] { return true }
                return false
            }
        }
        XCTAssertTrue(thumbsLoaded)

        // Mount `CameraRollSheetView` in a live UIWindow and verify the bright-green thumbnails render while active!
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(x: 0, y: 0, width: 390, height: 844)
        let hostVC = UIHostingController(rootView: CameraRollSheetView(viewModel: vm, roll: vm.cameraRoll))
        window.rootViewController = hostVC
        window.makeKeyAndVisible()
        defer {
            window.isHidden = true
            window.rootViewController = nil
        }

        let activeRendered = await waitUntil(timeout: 2.0) {
            let snap = self.snapshotWindow(window)
            return self.countBrightGreenPixels(in: snap) > 100
        }
        XCTAssertTrue(activeRendered, "Expected synthetic green thumbnails to be visible before willResignActive")

        // Post `willResignActiveNotification` and IMMEDIATELY snapshot (same runloop turn, matching SpringBoard)!
        NotificationCenter.default.post(name: UIApplication.willResignActiveNotification, object: UIApplication.shared)
        let inactiveSnap = snapshotWindow(window)
        let greenInAppSwitcherSnapshot = countBrightGreenPixels(in: inactiveSnap)
        XCTAssertEqual(
            greenInAppSwitcherSnapshot,
            0,
            "App-switcher snapshot after willResignActive must contain 0 thumbnail room pixels (found \(greenInAppSwitcherSnapshot))"
        )

        // Restore active state and confirm thumbnails reappear cleanly!
        NotificationCenter.default.post(name: UIApplication.didBecomeActiveNotification, object: UIApplication.shared)
        let restoredRendered = await waitUntil(timeout: 2.0) {
            let snap = self.snapshotWindow(window)
            return self.countBrightGreenPixels(in: snap) > 100
        }
        XCTAssertTrue(restoredRendered, "Expected camera-roll thumbnails to reappear after didBecomeActive")
    }

    // MARK: - 9B follow-up 3: resigning active covers the camera-roll grid instead of rebuilding it

    private static func firstScrollView(in view: UIView) -> UIScrollView? {
        if let scroll = view as? UIScrollView, scroll.contentSize.height > 0 { return scroll }
        for sub in view.subviews {
            if let found = firstScrollView(in: sub) { return found }
        }
        return nil
    }

    /// On the iPhone itself, through the real lifecycle notifications: Control Center / a notification banner / the
    /// app switcher (willResignActive then didBecomeActive, no backgrounding) must keep the live session. No new
    /// connect cycle, and the app never reads disconnected in between. While it reads disconnected the camera roll
    /// loses its owner rows, the List shrinks, and its scroll offset gets clamped.
    func testBecomingActiveAgainKeepsTheLiveSessionOnTheIPhone() async throws {
        let host = try PocketLoopbackV3Host()
        defer { host.shutdown() }
        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Interrupted iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: true)
        defer { client.stop() }
        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.makeLanPairURI()) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let live = await waitUntil { vm.telemetry.isConnected && vm.isPetOwner }
        XCTAssertTrue(live)
        try? await Task.sleep(nanoseconds: 300_000_000)

        let attempts = PocketTestSyncBox<[String]>([])
        client.onConnectAttemptForTesting = { _, ep in attempts.update { $0.append(ep) } }
        let sawDisconnected = PocketTestSyncBox<Bool>(false)
        let sub = vm.$telemetry.sink { t in if !t.isConnected { sawDisconnected.update { $0 = true } } }
        defer { sub.cancel() }

        NotificationCenter.default.post(name: UIApplication.willResignActiveNotification, object: UIApplication.shared)
        NotificationCenter.default.post(name: UIApplication.didBecomeActiveNotification, object: UIApplication.shared)
        try? await Task.sleep(nanoseconds: 1_000_000_000)

        XCTAssertEqual(attempts.read(), [], "Becoming active again must not start a new connect cycle")
        XCTAssertFalse(sawDisconnected.read(), "The app must never read disconnected across a brief interruption")
        XCTAssertTrue(vm.telemetry.isConnected && vm.isPetOwner)
    }

    func testFollowup3_ResignActiveKeepsCameraRollGridScrollAndThumbnails() async throws {
        let host = try PocketLoopbackV3Host()
        defer { host.shutdown() }

        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Scroll iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: true)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.makeLanPairURI()) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())
        let connected = await waitUntil { vm.isPetOwner && vm.cameraRoll.supportsCameraRoll }
        XCTAssertTrue(connected)

        await vm.cameraRoll.refreshDays(force: true)
        let clips = vm.cameraRoll.selectedDayClips
        XCTAssertEqual(clips.count, 2)
        for clip in clips {
            vm.cameraRoll.requestThumbnailIfNeeded(for: clip)
        }
        let thumbsLoaded = await waitUntil {
            clips.allSatisfy {
                if case .loaded = vm.cameraRoll.thumbnails[$0.clipId] { return true }
                return false
            }
        }
        XCTAssertTrue(thumbsLoaded)

        // A short window so the grid has something to scroll.
        let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(x: 0, y: 0, width: 390, height: 320)
        let hostVC = UIHostingController(rootView: CameraRollSheetView(viewModel: vm, roll: vm.cameraRoll))
        window.rootViewController = hostVC
        window.makeKeyAndVisible()
        defer {
            window.isHidden = true
            window.rootViewController = nil
        }

        let gridReady = await waitUntil(timeout: 2.0) { Self.firstScrollView(in: hostVC.view) != nil }
        XCTAssertTrue(gridReady)
        let gridBefore = try XCTUnwrap(Self.firstScrollView(in: hostVC.view))
        window.layoutIfNeeded()
        // Scroll to a point inside the grid's real scroll range, so no later layout pass has a reason to clamp it.
        let minY = -gridBefore.adjustedContentInset.top
        let maxY = gridBefore.contentSize.height + gridBefore.adjustedContentInset.bottom - gridBefore.bounds.height
        XCTAssertGreaterThan(maxY - minY, 20, "The grid must be scrollable in this window (range \(minY)...\(maxY))")
        let targetY = min(minY + 40, maxY - 1)
        gridBefore.setContentOffset(CGPoint(x: gridBefore.contentOffset.x, y: targetY), animated: false)
        window.layoutIfNeeded()
        let offsetBefore = gridBefore.contentOffset
        XCTAssertEqual(offsetBefore.y, targetY, accuracy: 0.5)
        // The visual scroll position: the offset measured from the top inset (the inset itself can change when the
        // app becomes active, which moves contentOffset without moving the list).
        let visualBefore = offsetBefore.y + gridBefore.adjustedContentInset.top

        NotificationCenter.default.post(name: UIApplication.willResignActiveNotification, object: UIApplication.shared)
        let inactiveSnap = snapshotWindow(window)
        XCTAssertEqual(countBrightGreenPixels(in: inactiveSnap), 0, "The inactive snapshot must still show no thumbnails")
        let gridWhileHidden = Self.firstScrollView(in: hostVC.view)
        XCTAssertTrue(gridWhileHidden === gridBefore, "The grid must stay mounted (covered) while the app is inactive")

        NotificationCenter.default.post(name: UIApplication.didBecomeActiveNotification, object: UIApplication.shared)
        try? await Task.sleep(nanoseconds: 300_000_000)
        window.layoutIfNeeded()

        let gridAfter = try XCTUnwrap(Self.firstScrollView(in: hostVC.view))
        XCTAssertTrue(gridAfter === gridBefore, "The grid must not be rebuilt when the app becomes active again")
        XCTAssertEqual(gridAfter.contentOffset.y + gridAfter.adjustedContentInset.top, visualBefore, accuracy: 0.5,
                       "Scroll position must survive resign/become active")
        XCTAssertTrue(clips.allSatisfy {
            if case .loaded = vm.cameraRoll.thumbnails[$0.clipId] { return true }
            return false
        }, "Loaded thumbnails must survive resign/become active")
    }

    // MARK: - Zero files in real iOS app sandbox counted recursively by path (no folder-date filtering)

    nonisolated private static func recursiveRelativeFilePaths(in rootDirectory: URL) -> Set<String> {
        let fm = FileManager.default
        let standardizedRoot = rootDirectory.resolvingSymlinksInPath().standardizedFileURL
        let rootPrefix = standardizedRoot.path.hasSuffix("/")
            ? standardizedRoot.path
            : standardizedRoot.path + "/"
        guard let enumerator = fm.enumerator(
            at: standardizedRoot,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: []
        ) else {
            return []
        }
        var relativePaths = Set<String>()
        for case let fileURL as URL in enumerator {
            let vals = try? fileURL.resourceValues(forKeys: [.isRegularFileKey])
            guard vals?.isRegularFile == true else { continue }
            let fullPath = fileURL.resolvingSymlinksInPath().standardizedFileURL.path
            if fullPath.hasPrefix(rootPrefix) {
                relativePaths.insert(String(fullPath.dropFirst(rootPrefix.count)))
            } else {
                relativePaths.insert(fullPath)
            }
        }
        return relativePaths
    }

    func testFix8_ZeroFilesInIOSSandboxRecursiveByPathAfterLiveThumbnailsAndStreamedPlayback() async throws {
        URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)

        let fm = FileManager.default
        let homePath = NSHomeDirectory()
        XCTAssertFalse(
            homePath.range(of: "^/Users/[^/]+/?$", options: .regularExpression) != nil || homePath.hasSuffix("/Documents"),
            "Zero-files test must run inside the iOS app container, never against the Mac's ~/Documents (got \(homePath))"
        )

        let docsURL = try XCTUnwrap(fm.urls(for: .documentDirectory, in: .userDomainMask).first)
        let cachesURL = try XCTUnwrap(fm.urls(for: .cachesDirectory, in: .userDomainMask).first)
        let tmpURL = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        try fm.createDirectory(at: docsURL, withIntermediateDirectories: true)
        try fm.createDirectory(at: cachesURL, withIntermediateDirectories: true)
        try fm.createDirectory(at: tmpURL, withIntermediateDirectories: true)

        // Positive self-check proving nested files are counted recursively by relative path without any date filter!
        let probeBefore = Self.recursiveRelativeFilePaths(in: tmpURL)
        let nestedDir = tmpURL
            .appendingPathComponent("kv-probe-outer", isDirectory: true)
            .appendingPathComponent("inner", isDirectory: true)
        try fm.createDirectory(at: nestedDir, withIntermediateDirectories: true)
        let probeFile = nestedDir.appendingPathComponent("probe.bin")
        try Data([0xAA, 0xBB]).write(to: probeFile)
        // Even if the parent directory's modification timestamp is old, recursive by-path enumeration still finds `probe.bin`!
        let oldTimestamp = Date(timeIntervalSince1970: 946_684_800) // 2000-01-01
        try fm.setAttributes([.modificationDate: oldTimestamp], ofItemAtPath: nestedDir.deletingLastPathComponent().path)
        try fm.setAttributes([.modificationDate: oldTimestamp], ofItemAtPath: nestedDir.path)
        try fm.setAttributes([.modificationDate: oldTimestamp], ofItemAtPath: probeFile.path)

        let probeAfter = Self.recursiveRelativeFilePaths(in: tmpURL)
        XCTAssertTrue(
            probeAfter.subtracting(probeBefore).contains("kv-probe-outer/inner/probe.bin"),
            "Recursive by-path scanner must find nested files regardless of folder/file modification dates"
        )
        try fm.removeItem(at: nestedDir.deletingLastPathComponent())

        // Start synthetic loopback host BEFORE capturing the iOS sandbox baseline
        let host = try PocketLoopbackV3Host()
        defer { host.shutdown() }

        let containerURL = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
        let beforeContainer = Self.recursiveFileSizes(in: containerURL)
        let beforeDocs = Self.recursiveRelativeFilePaths(in: docsURL)
        let beforeCaches = Self.recursiveRelativeFilePaths(in: cachesURL)
        let beforeTmp = Self.recursiveRelativeFilePaths(in: tmpURL)

        let client = try PocketStreamClientCore(
            persistence: InMemoryPersistenceProvider(),
            deviceName: "Zero Storage iPhone",
            deviceKind: "iphone"
        )
        client.disableBonjourForTesting = true
        let vm = PocketViewModel(client: client, registerLifecycleObservers: true)
        let liveLayer = AVSampleBufferDisplayLayer()
        let playbackLayer = AVSampleBufferDisplayLayer()
        vm.displayBridge.attach(layer: liveLayer)
        vm.cameraRoll.playbackDisplayBridge.attach(layer: playbackLayer)
        XCTAssertTrue(liveLayer.preventsCapture)
        XCTAssertTrue(playbackLayer.preventsCapture)
        defer { client.stop() }

        let pairRes: Result<Void, Error> = await withCheckedContinuation { cont in
            client.submitCameraLink(host.makeLanPairURI()) { cont.resume(returning: $0) }
        }
        XCTAssertNoThrow(try pairRes.get())

        let liveDecoded = await waitUntil { vm.isPetOwner && vm.telemetry.receivedFrames >= 3 }
        XCTAssertTrue(liveDecoded, "Expected live synthetic H.264 frames to decode on iOS")

        await vm.cameraRoll.refreshDays(force: true)
        let clips = vm.cameraRoll.selectedDayClips
        XCTAssertEqual(clips.count, 2)
        for clip in clips {
            vm.cameraRoll.requestThumbnailIfNeeded(for: clip)
        }
        vm.cameraRoll.startPlayback(clip: clips[0], positionSec: 0.0)

        let playbackAndThumbsReady = await waitUntil {
            let allThumbsLoaded = clips.allSatisfy {
                if case .loaded = vm.cameraRoll.thumbnails[$0.clipId] { return true }
                return false
            }
            return allThumbsLoaded &&
                vm.cameraRoll.playbackState?.state == .playing &&
                vm.telemetry.decodedPlaybackFrames >= 3
        }
        XCTAssertTrue(playbackAndThumbsReady, "Expected thumbnails and streamed H.264 playback to decode in memory on iOS")

        vm.cameraRoll.togglePlayPause()
        let paused = await waitUntil { vm.cameraRoll.playbackState?.state == .paused }
        XCTAssertTrue(paused)
        vm.cameraRoll.closePlayback(resumeLive: true)

        let newDocs = Self.recursiveRelativeFilePaths(in: docsURL).subtracting(beforeDocs)
        let newCaches = Self.recursiveRelativeFilePaths(in: cachesURL).subtracting(beforeCaches)
        let newTmp = Self.recursiveRelativeFilePaths(in: tmpURL).subtracting(beforeTmp)

        XCTAssertEqual(newDocs, [], "iOS Documents must contain 0 new files after live stream, thumbnails, and playback")
        XCTAssertEqual(newCaches, [], "iOS Library/Caches must contain 0 new files after live stream, thumbnails, and playback")
        XCTAssertEqual(newTmp, [], "iOS tmp must contain 0 new files after live stream, thumbnails, and playback")
        XCTAssertEqual(URLCache.shared.currentDiskUsage, 0)

        // 9B follow-up 4: the WHOLE app container (Documents, Library/Application Support, Library/Preferences, Caches,
        // tmp, …). Preferences may legitimately change, so any file may appear or change, but none may be an image or
        // video, and none that appeared or changed may be over 64 KiB.
        // Known limit: this judges by file extension and size only. A small frame written under another extension (or
        // none), or one split across files under 64 KiB each, would pass; the strict "0 new files" checks above on
        // Documents, Caches and tmp, and the static audits of the display path, cover those places.
        let afterContainer = Self.recursiveFileSizes(in: containerURL)
        let mediaExtensions: Set<String> = ["jpg", "jpeg", "png", "heic", "mov", "mp4", "h264"]
        let appearedOrChanged = afterContainer.filter { beforeContainer[$0.key] != $0.value }
        let newMedia = appearedOrChanged.keys.filter { mediaExtensions.contains(($0 as NSString).pathExtension.lowercased()) }
        let oversized = appearedOrChanged.filter { $0.value > 64 * 1024 }
        XCTAssertEqual(newMedia.sorted(), [], "No new image or video file may appear anywhere in the app container")
        XCTAssertEqual(oversized.keys.sorted(), [], "No new or changed file over 64 KiB may appear anywhere in the app container: \(oversized)")
    }

    /// Every regular file under `rootDirectory`, by relative path, with its size in bytes.
    nonisolated private static func recursiveFileSizes(in rootDirectory: URL) -> [String: Int] {
        let standardizedRoot = rootDirectory.resolvingSymlinksInPath().standardizedFileURL
        let rootPrefix = standardizedRoot.path.hasSuffix("/") ? standardizedRoot.path : standardizedRoot.path + "/"
        guard let enumerator = FileManager.default.enumerator(
            at: standardizedRoot,
            includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey],
            options: []
        ) else {
            return [:]
        }
        var sizes: [String: Int] = [:]
        for case let fileURL as URL in enumerator {
            let vals = try? fileURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
            guard vals?.isRegularFile == true else { continue }
            let fullPath = fileURL.resolvingSymlinksInPath().standardizedFileURL.path
            let key = fullPath.hasPrefix(rootPrefix) ? String(fullPath.dropFirst(rootPrefix.count)) : fullPath
            sizes[key] = vals?.fileSize ?? 0
        }
        return sizes
    }

    /// Self-check for the whole-container scanner: a nested media file and an oversized file under Library are both seen.
    func testFollowup4_WholeContainerScannerSeesLibraryMediaAndLargeFiles() throws {
        let fm = FileManager.default
        let containerURL = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
        let libraryURL = try XCTUnwrap(fm.urls(for: .libraryDirectory, in: .userDomainMask).first)
        let probeDir = libraryURL
            .appendingPathComponent("Application Support", isDirectory: true)
            .appendingPathComponent("kv-9b-scan-probe-\(UUID().uuidString)", isDirectory: true)
        try fm.createDirectory(at: probeDir, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: probeDir) }

        let before = Self.recursiveFileSizes(in: containerURL)
        try Data([0xFF, 0xD8, 0xFF]).write(to: probeDir.appendingPathComponent("frame.jpeg"))
        try Data(count: 64 * 1024 + 1).write(to: probeDir.appendingPathComponent("blob.bin"))
        let after = Self.recursiveFileSizes(in: containerURL)

        let appeared = after.filter { before[$0.key] != $0.value }
        XCTAssertTrue(appeared.keys.contains { $0.hasSuffix("/frame.jpeg") && $0.hasPrefix("Library/Application Support/") })
        XCTAssertTrue(appeared.contains { $0.key.hasSuffix("/blob.bin") && $0.value > 64 * 1024 })
    }
}
