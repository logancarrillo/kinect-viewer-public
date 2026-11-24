import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

struct EncodedVideoFrame: Sendable {
    let configIfChanged: StreamConfigMessage?
    let header: VideoAccessUnitHeader
    let wirePayload: Data // VideoAccessUnitHeader + AVCC bytes
}

final class VideoToolboxEncoder: @unchecked Sendable {
    var onEncodedFrame: (@Sendable (EncodedVideoFrame) -> Void)?

    private let stateLock = NSLock()
    private var session: VTCompressionSession?
    private var encodedWidth: Int = 0
    private var encodedHeight: Int = 0
    private var streamEpoch: UInt32 = 0
    private var currentModeRaw: Int = 0
    private var currentSbsRaw: Int = 0
    private var forceNextKeyframe: Bool = true
    private var emitConfigNextKeyframe: Bool = true
    private var latestConfig: StreamConfigMessage?

    private final class FrameContext {
        let frameSeq: UInt64
        let hostCaptureNs: UInt64
        let hasClockOverlay: Bool
        let streamEpoch: UInt32
        let modeRaw: Int
        let sbsRaw: Int
        let width: Int
        let height: Int

        init(
            frameSeq: UInt64,
            hostCaptureNs: UInt64,
            hasClockOverlay: Bool,
            streamEpoch: UInt32,
            modeRaw: Int,
            sbsRaw: Int,
            width: Int,
            height: Int
        ) {
            self.frameSeq = frameSeq
            self.hostCaptureNs = hostCaptureNs
            self.hasClockOverlay = hasClockOverlay
            self.streamEpoch = streamEpoch
            self.modeRaw = modeRaw
            self.sbsRaw = sbsRaw
            self.width = width
            self.height = height
        }
    }

    deinit {
        teardown()
    }

    func teardown() {
        if let s = session {
            VTCompressionSessionInvalidate(s)
            session = nil
        }
        encodedWidth = 0
        encodedHeight = 0
    }

    func requestImmediateKeyframeAndConfig() {
        stateLock.lock()
        forceNextKeyframe = true
        emitConfigNextKeyframe = true
        stateLock.unlock()
    }

    var cachedConfig: StreamConfigMessage? {
        stateLock.lock()
        defer { stateLock.unlock() }
        return latestConfig
    }

    func encodeBGRAFrame(
        bgraPtr: UnsafePointer<UInt8>,
        srcWidth: Int,
        srcHeight: Int,
        srcStride: Int,
        frameSeq: UInt64,
        hostCaptureNs: UInt64,
        modeRaw: Int,
        sbsRaw: Int,
        hasClockOverlay: Bool
    ) {
        guard srcWidth > 0, srcHeight > 0 else { return }

        // Upscale 512x424 2x nearest -> 1024x848 so 4:2:0 chroma has full 512x424 resolution
        let shouldUpscale2x = (srcWidth == 512 && srcHeight == 424)
        let targetW = shouldUpscale2x ? (srcWidth * 2) : (srcWidth & ~1)
        let targetH = shouldUpscale2x ? (srcHeight * 2) : (srcHeight & ~1)

        if session == nil || encodedWidth != targetW || encodedHeight != targetH || currentModeRaw != modeRaw || currentSbsRaw != sbsRaw {
            guard recreateSession(width: targetW, height: targetH, modeRaw: modeRaw, sbsRaw: sbsRaw) else {
                return
            }
        }

        guard let compSession = session,
              let pool = VTCompressionSessionGetPixelBufferPool(compSession) else {
            return
        }

        var pixelBufferOut: CVPixelBuffer?
        let cvErr = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBufferOut)
        guard cvErr == kCVReturnSuccess, let pb = pixelBufferOut else {
            return
        }

        CVPixelBufferLockBaseAddress(pb, [])
        if let dstBase = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self) {
            let dstStride = CVPixelBufferGetBytesPerRow(pb)
            if shouldUpscale2x {
                for sy in 0..<srcHeight {
                    let sRow = bgraPtr.advanced(by: sy * srcStride)
                    let dRow0 = dstBase.advanced(by: (sy * 2) * dstStride)
                    let dRow1 = dstBase.advanced(by: (sy * 2 + 1) * dstStride)
                    let sRow32 = UnsafeRawPointer(sRow).assumingMemoryBound(to: UInt32.self)
                    let dRow0_32 = UnsafeMutableRawPointer(dRow0).assumingMemoryBound(to: UInt32.self)
                    for sx in 0..<srcWidth {
                        let px = sRow32[sx]
                        dRow0_32[sx * 2] = px
                        dRow0_32[sx * 2 + 1] = px
                    }
                    memcpy(dRow1, dRow0, targetW * 4)
                }
            } else {
                let copyRowBytes = targetW * 4
                for y in 0..<targetH {
                    memcpy(dstBase.advanced(by: y * dstStride), bgraPtr.advanced(by: y * srcStride), copyRowBytes)
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, [])

        var frameProps: [CFString: Any]? = nil
        stateLock.lock()
        if forceNextKeyframe {
            frameProps = [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue as Any]
            forceNextKeyframe = false
        }
        stateLock.unlock()

        let ctx = FrameContext(
            frameSeq: frameSeq,
            hostCaptureNs: hostCaptureNs,
            hasClockOverlay: hasClockOverlay,
            streamEpoch: streamEpoch,
            modeRaw: modeRaw,
            sbsRaw: sbsRaw,
            width: targetW,
            height: targetH
        )
        let opaqueCtx = Unmanaged.passRetained(ctx).toOpaque()
        let pts = CMTime(value: Int64(hostCaptureNs / 1000), timescale: 1_000_000)

        var infoFlags = VTEncodeInfoFlags()
        let status = VTCompressionSessionEncodeFrame(
            compSession,
            imageBuffer: pb,
            presentationTimeStamp: pts,
            duration: CMTime(value: 1, timescale: 30),
            frameProperties: frameProps as CFDictionary?,
            sourceFrameRefcon: opaqueCtx,
            infoFlagsOut: &infoFlags
        )
        if status != noErr {
            Unmanaged<FrameContext>.fromOpaque(opaqueCtx).release()
        }
    }

    private func recreateSession(width: Int, height: Int, modeRaw: Int, sbsRaw: Int) -> Bool {
        teardown()

        let pbAttrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary
        ]

        let encoderSpec: [CFString: Any] = [
            kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: kCFBooleanTrue as Any,
            kVTVideoEncoderSpecification_EnableLowLatencyRateControl: kCFBooleanTrue as Any
        ]

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        var newSession: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: Int32(width),
            height: Int32(height),
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: encoderSpec as CFDictionary,
            imageBufferAttributes: pbAttrs as CFDictionary,
            compressedDataAllocator: nil,
            outputCallback: Self.compressionOutputCallback,
            refcon: selfPtr,
            compressionSessionOut: &newSession
        )
        guard status == noErr, let s = newSession else {
            return false
        }

        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxFrameDelayCount, value: 0 as CFNumber)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: 30 as CFNumber)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 30 as CFNumber)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: 1.0 as CFNumber)

        let avgBitrate = width >= 1600 ? 10_000_000 : 5_000_000
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: avgBitrate as CFNumber)

        VTCompressionSessionPrepareToEncodeFrames(s)

        self.session = s
        self.encodedWidth = width
        self.encodedHeight = height
        self.currentModeRaw = modeRaw
        self.currentSbsRaw = sbsRaw
        self.streamEpoch &+= 1
        stateLock.lock()
        self.forceNextKeyframe = true
        self.emitConfigNextKeyframe = true
        stateLock.unlock()
        return true
    }

    private static let compressionOutputCallback: VTCompressionOutputCallback = { refcon, sourceFrameRefcon, status, _, sampleBuffer in
        guard let sourceFrameRefcon = sourceFrameRefcon else { return }
        let ctx = Unmanaged<FrameContext>.fromOpaque(sourceFrameRefcon).takeRetainedValue()
        guard status == noErr,
              let refcon = refcon,
              let sampleBuffer = sampleBuffer,
              CMSampleBufferDataIsReady(sampleBuffer) else {
            return
        }
        let encoder = Unmanaged<VideoToolboxEncoder>.fromOpaque(refcon).takeUnretainedValue()
        encoder.handleCompressedSampleBuffer(sampleBuffer, ctx: ctx)
    }

    private func handleCompressedSampleBuffer(_ sampleBuffer: CMSampleBuffer, ctx: FrameContext) {
        let encodeDoneNs = kv_monotonic_ns()

        // Determine if IDR keyframe
        var isKeyframe = true
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFDictionary.self)
            let keyPtr = Unmanaged.passUnretained(kCMSampleAttachmentKey_NotSync).toOpaque()
            if let valPtr = CFDictionaryGetValue(dict, keyPtr) {
                let cfBool = unsafeBitCast(valPtr, to: CFBoolean.self)
                isKeyframe = !CFBooleanGetValue(cfBool)
            }
        }

        var configOut: StreamConfigMessage? = nil
        if isKeyframe, let fmt = CMSampleBufferGetFormatDescription(sampleBuffer) {
            var paramSetCount = 0
            var nalHeaderLen: Int32 = 4
            let countStatus = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                fmt,
                parameterSetIndex: 0,
                parameterSetPointerOut: nil,
                parameterSetSizeOut: nil,
                parameterSetCountOut: &paramSetCount,
                nalUnitHeaderLengthOut: &nalHeaderLen
            )
            if countStatus == noErr && paramSetCount >= 2 {
                var sets: [Data] = []
                for idx in 0..<paramSetCount {
                    var ptr: UnsafePointer<UInt8>?
                    var size = 0
                    if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                        fmt,
                        parameterSetIndex: idx,
                        parameterSetPointerOut: &ptr,
                        parameterSetSizeOut: &size,
                        parameterSetCountOut: nil,
                        nalUnitHeaderLengthOut: nil
                    ) == noErr, let p = ptr, size > 0 {
                        sets.append(Data(bytes: p, count: size))
                    }
                }
                if sets.count >= 2 {
                    let cfg = StreamConfigMessage(
                        streamEpoch: ctx.streamEpoch,
                        codecRaw: WireCodecKind.h264.rawValue,
                        width: ctx.width,
                        height: ctx.height,
                        modeRaw: ctx.modeRaw,
                        sbsLayoutRaw: ctx.sbsRaw,
                        nalLengthHeaderBytes: Int(nalHeaderLen),
                        parameterSets: sets
                    )
                    stateLock.lock()
                    latestConfig = cfg
                    if emitConfigNextKeyframe {
                        configOut = cfg
                        emitConfigNextKeyframe = false
                    }
                    stateLock.unlock()
                }
            }
        }

        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
        let totalLen = CMBlockBufferGetDataLength(dataBuffer)
        guard totalLen > 0 else { return }

        var avccData = Data(count: totalLen)
        let copyStatus = avccData.withUnsafeMutableBytes { rawPtr -> OSStatus in
            guard let base = rawPtr.baseAddress else { return -1 }
            return CMBlockBufferCopyDataBytes(dataBuffer, atOffset: 0, dataLength: totalLen, destination: base)
        }
        guard copyStatus == kCMBlockBufferNoErr else { return }

        var flags: UInt8 = 0
        if isKeyframe { flags |= 0x01 }
        if ctx.hasClockOverlay { flags |= 0x02 }

        let header = VideoAccessUnitHeader(
            streamEpoch: ctx.streamEpoch,
            frameSeq: ctx.frameSeq,
            hostCaptureNs: ctx.hostCaptureNs,
            hostEncodeDoneNs: encodeDoneNs,
            flags: flags
        )
        let packedPayload = header.encode(withAvccPayload: avccData)
        onEncodedFrame?(EncodedVideoFrame(configIfChanged: configOut, header: header, wirePayload: packedPayload))
    }
}
