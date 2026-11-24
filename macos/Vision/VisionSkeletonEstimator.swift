import AppKit
import CoreGraphics
import Foundation
import Vision
import simd

struct RawVisionJoint2D: Equatable, Sendable {
    let id: String
    /// Normalized [0..1] colour X coordinate (0 = left, 1 = right).
    let normTopLeftX: Float
    /// Normalized [0..1] colour Y coordinate (0 = top, 1 = bottom).
    let normTopLeftY: Float
    let confidence: Float
    let isHand: Bool
}

struct RawVisionBoneEdge: Equatable, Sendable {
    let fromId: String
    let toId: String
    let isHand: Bool
}

struct RawVisionPoseSnapshot: Equatable, Sendable {
    var colorSeq: UInt64 = 0
    var bodyCount: Int = 0
    var handCount: Int = 0
    var joints: [RawVisionJoint2D] = []
    var bones: [RawVisionBoneEdge] = []

    var isEmpty: Bool { joints.isEmpty }
}

struct MappedJointDepth3D: Equatable, Sendable {
    let depthPixelX: Float
    let depthPixelY: Float
    let xMeters: Float
    let yMeters: Float
    let zMeters: Float
}

struct Projected2DLine: Equatable, Sendable {
    /// Normalized [0..1] coordinates inside the displayed image rectangle (top-left origin).
    let x0: CGFloat
    let y0: CGFloat
    let x1: CGFloat
    let y1: CGFloat
    let isHand: Bool
}

struct Projected2DDot: Equatable, Sendable {
    let x: CGFloat
    let y: CGFloat
    let isHand: Bool
}

struct ProjectedSkeletonOverlay: Equatable, Sendable {
    var lines2D: [Projected2DLine] = []
    var dots2D: [Projected2DDot] = []
    var lineVertices3D: [Overlay3DVertex] = []
    var pointVertices3D: [Overlay3DVertex] = []

    var has2DElements: Bool { !lines2D.isEmpty || !dots2D.isEmpty }
    var has3DElements: Bool { !lineVertices3D.isEmpty || !pointVertices3D.isEmpty }
}

enum SkeletonOverlayProjector {
    private static let bodyLineColor3D = SIMD4<Float>(0.15, 0.95, 0.85, 0.95)
    private static let bodyJointColor3D = SIMD4<Float>(0.35, 1.00, 0.45, 1.00)
    private static let handLineColor3D = SIMD4<Float>(1.00, 0.72, 0.18, 0.95)
    private static let handJointColor3D = SIMD4<Float>(1.00, 0.38, 0.72, 1.00)

    static func project(
        snapshot: RawVisionPoseSnapshot,
        confidenceThreshold: Float,
        mode: ViewerModeOption,
        sbsLayout: SbsLayoutOption,
        depthColorBlendEnabled: Bool,
        mirrorHorizontal: Bool,
        rotate180: Bool,
        mapColorNormToDepthAnd3D: (Float, Float) -> MappedJointDepth3D?
    ) -> ProjectedSkeletonOverlay {
        guard !snapshot.joints.isEmpty else { return ProjectedSkeletonOverlay() }

        let flipX = (mirrorHorizontal != rotate180)
        let flipY = rotate180

        func orientNorm(_ nx: CGFloat, _ ny: CGFloat) -> (CGFloat, CGFloat) {
            let ox = flipX ? (1.0 - nx) : nx
            let oy = flipY ? (1.0 - ny) : ny
            return (min(1.0, max(0.0, ox)), min(1.0, max(0.0, oy)))
        }

        // Filter joints by confidence threshold
        var validJoints: [String: RawVisionJoint2D] = [:]
        validJoints.reserveCapacity(snapshot.joints.count)
        for j in snapshot.joints where j.confidence >= confidenceThreshold {
            validJoints[j.id] = j
        }
        guard !validJoints.isEmpty else { return ProjectedSkeletonOverlay() }

        // Map each surviving joint through libfreenect2 registration (needed for IR, Depth, Blend, SBS, and 3D)
        var mappedByJoint: [String: MappedJointDepth3D] = [:]
        mappedByJoint.reserveCapacity(validJoints.count)
        for (id, j) in validJoints {
            if let mapped = mapColorNormToDepthAnd3D(j.normTopLeftX, j.normTopLeftY) {
                mappedByJoint[id] = mapped
            }
        }

        var out = ProjectedSkeletonOverlay()

        // 1. Always build 3D overlay vertices when in Mode 5 (3D Cloud)
        if mode == .pointCloud3D {
            for (id, j) in validJoints {
                guard let m = mappedByJoint[id] else { continue }
                let color = j.isHand ? handJointColor3D : bodyJointColor3D
                let ptSize: Float = j.isHand ? 6.5 : 9.0
                out.pointVertices3D.append(
                    Overlay3DVertex.fromSensorMeters(
                        xMeters: m.xMeters,
                        yMeters: m.yMeters,
                        zMeters: m.zMeters,
                        flipX: flipX,
                        flipY: flipY,
                        pointSize: ptSize,
                        color: color
                    )
                )
            }
            for bone in snapshot.bones {
                guard let m0 = mappedByJoint[bone.fromId],
                      let m1 = mappedByJoint[bone.toId] else { continue }
                let color = bone.isHand ? handLineColor3D : bodyLineColor3D
                out.lineVertices3D.append(
                    Overlay3DVertex.fromSensorMeters(
                        xMeters: m0.xMeters,
                        yMeters: m0.yMeters,
                        zMeters: m0.zMeters,
                        flipX: flipX,
                        flipY: flipY,
                        pointSize: 1.0,
                        color: color
                    )
                )
                out.lineVertices3D.append(
                    Overlay3DVertex.fromSensorMeters(
                        xMeters: m1.xMeters,
                        yMeters: m1.yMeters,
                        zMeters: m1.zMeters,
                        flipX: flipX,
                        flipY: flipY,
                        pointSize: 1.0,
                        color: color
                    )
                )
            }
            return out
        }

        // 2. Helper to emit 2D lines & dots given a per-joint canonical (unflipped) normalized [0..1] mapper
        func appendPanel2D(_ coordForJoint: (String, RawVisionJoint2D) -> (CGFloat, CGFloat)?) {
            var panelCoords: [String: (CGFloat, CGFloat)] = [:]
            panelCoords.reserveCapacity(validJoints.count)

            for (id, j) in validJoints {
                guard let rawNorm = coordForJoint(id, j) else { continue }
                let oriented = orientNorm(rawNorm.0, rawNorm.1)
                panelCoords[id] = oriented
                out.dots2D.append(Projected2DDot(x: oriented.0, y: oriented.1, isHand: j.isHand))
            }

            for bone in snapshot.bones {
                guard let p0 = panelCoords[bone.fromId],
                      let p1 = panelCoords[bone.toId] else { continue }
                out.lines2D.append(
                    Projected2DLine(x0: p0.0, y0: p0.1, x1: p1.0, y1: p1.1, isHand: bone.isHand)
                )
            }
        }

        if depthColorBlendEnabled {
            // Registered 512x424 Depth+Colour blend
            appendPanel2D { id, _ in
                guard let m = mappedByJoint[id] else { return nil }
                return (CGFloat(m.depthPixelX) / 511.0, CGFloat(m.depthPixelY) / 423.0)
            }
            return out
        }

        switch mode {
        case .ir, .depth:
            appendPanel2D { id, _ in
                guard let m = mappedByJoint[id] else { return nil }
                return (CGFloat(m.depthPixelX) / 511.0, CGFloat(m.depthPixelY) / 423.0)
            }

        case .color:
            appendPanel2D { _, j in
                (CGFloat(j.normTopLeftX), CGFloat(j.normTopLeftY))
            }

        case .sideBySide:
            switch sbsLayout {
            case .irDepth:
                // Left panel [0..511] = IR, Right panel [512..1023] = Depth (total width 1024, height 424)
                appendPanel2D { id, _ in
                    guard let m = mappedByJoint[id] else { return nil }
                    return (CGFloat(m.depthPixelX) / 1023.0, CGFloat(m.depthPixelY) / 423.0)
                }
                appendPanel2D { id, _ in
                    guard let m = mappedByJoint[id] else { return nil }
                    return ((512.0 + CGFloat(m.depthPixelX)) / 1023.0, CGFloat(m.depthPixelY) / 423.0)
                }

            case .colorDepth, .colorIr:
                // Left panel [0..753] = scaled 1080p Colour, [754..767] = divider, Right panel [768..1279] = 512x424 Depth/IR
                appendPanel2D { _, j in
                    ((CGFloat(j.normTopLeftX) * 753.0) / 1279.0, CGFloat(j.normTopLeftY))
                }
                appendPanel2D { id, _ in
                    guard let m = mappedByJoint[id] else { return nil }
                    return ((768.0 + CGFloat(m.depthPixelX)) / 1279.0, CGFloat(m.depthPixelY) / 423.0)
                }
            }

        case .pointCloud3D:
            break
        }

        return out
    }
}

enum Skeleton2DCanvasDrawer {
    static func draw(overlay: ProjectedSkeletonOverlay, in imageRect: CGRect, ctx: CGContext) {
        guard overlay.has2DElements, imageRect.width > 1, imageRect.height > 1 else { return }

        ctx.saveGState()
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)

        for line in overlay.lines2D {
            let p0 = CGPoint(x: imageRect.minX + line.x0 * imageRect.width, y: imageRect.minY + line.y0 * imageRect.height)
            let p1 = CGPoint(x: imageRect.minX + line.x1 * imageRect.width, y: imageRect.minY + line.y1 * imageRect.height)
            ctx.setStrokeColor(
                (line.isHand ? NSColor.systemOrange : NSColor.systemCyan).withAlphaComponent(0.92).cgColor
            )
            ctx.setLineWidth(line.isHand ? 2.0 : 2.8)
            ctx.move(to: p0)
            ctx.addLine(to: p1)
            ctx.strokePath()
        }

        for dot in overlay.dots2D {
            let pt = CGPoint(x: imageRect.minX + dot.x * imageRect.width, y: imageRect.minY + dot.y * imageRect.height)
            let r: CGFloat = dot.isHand ? 3.2 : 4.6
            let ellipse = CGRect(x: pt.x - r, y: pt.y - r, width: r * 2.0, height: r * 2.0)
            ctx.setFillColor((dot.isHand ? NSColor.systemPink : NSColor.systemGreen).cgColor)
            ctx.fillEllipse(in: ellipse)
            ctx.setStrokeColor(NSColor.black.withAlphaComponent(0.85).cgColor)
            ctx.setLineWidth(1.0)
            ctx.strokeEllipse(in: ellipse)
        }

        ctx.restoreGState()
    }
}

/// On-device body & hand pose estimator powered by Apple's `Vision` framework.
/// Operates strictly in memory on live colour frames; nothing is stored and no joint data ever leaves the Mac.
final class VisionSkeletonEstimator: @unchecked Sendable {
    private static let bodyBonePairs: [(VNHumanBodyPoseObservation.JointName, VNHumanBodyPoseObservation.JointName)] = [
        (.nose, .neck),
        (.nose, .leftEye), (.leftEye, .leftEar),
        (.nose, .rightEye), (.rightEye, .rightEar),
        (.neck, .leftShoulder), (.leftShoulder, .leftElbow), (.leftElbow, .leftWrist),
        (.neck, .rightShoulder), (.rightShoulder, .rightElbow), (.rightElbow, .rightWrist),
        (.neck, .root),
        (.root, .leftHip), (.leftHip, .leftKnee), (.leftKnee, .leftAnkle),
        (.root, .rightHip), (.rightHip, .rightKnee), (.rightKnee, .rightAnkle),
    ]

    private static let allBodyJoints: [VNHumanBodyPoseObservation.JointName] = [
        .nose, .leftEye, .rightEye, .leftEar, .rightEar,
        .neck, .leftShoulder, .rightShoulder, .leftElbow, .rightElbow, .leftWrist, .rightWrist,
        .root, .leftHip, .rightHip, .leftKnee, .rightKnee, .leftAnkle, .rightAnkle,
    ]

    private static let handBonePairs: [(VNHumanHandPoseObservation.JointName, VNHumanHandPoseObservation.JointName)] = [
        (.wrist, .thumbCMC), (.thumbCMC, .thumbMP), (.thumbMP, .thumbIP), (.thumbIP, .thumbTip),
        (.wrist, .indexMCP), (.indexMCP, .indexPIP), (.indexPIP, .indexDIP), (.indexDIP, .indexTip),
        (.wrist, .middleMCP), (.middleMCP, .middlePIP), (.middlePIP, .middleDIP), (.middleDIP, .middleTip),
        (.wrist, .ringMCP), (.ringMCP, .ringPIP), (.ringPIP, .ringDIP), (.ringDIP, .ringTip),
        (.wrist, .littleMCP), (.littleMCP, .littlePIP), (.littleDIP, .littleTip), (.littlePIP, .littleDIP),
    ]

    private static let allHandJoints: [VNHumanHandPoseObservation.JointName] = [
        .wrist,
        .thumbCMC, .thumbMP, .thumbIP, .thumbTip,
        .indexMCP, .indexPIP, .indexDIP, .indexTip,
        .middleMCP, .middlePIP, .middleDIP, .middleTip,
        .ringMCP, .ringPIP, .ringDIP, .ringTip,
        .littleMCP, .littlePIP, .littleDIP, .littleTip,
    ]

    private let queue = DispatchQueue(label: "kinectviewer.vision.pose", qos: .userInitiated)
    private let lock = NSLock()
    private var inFlight = false
    private let colorSpace = CGColorSpaceCreateDeviceRGB()

    var isBusy: Bool {
        lock.lock()
        defer { lock.unlock() }
        return inFlight
    }

    func submitLiveColorFrameIfIdle(
        bgraData: Data,
        width: Int,
        height: Int,
        colorSeq: UInt64,
        onComplete: @escaping @Sendable (RawVisionPoseSnapshot) -> Void
    ) {
        lock.lock()
        if inFlight {
            lock.unlock()
            return
        }
        inFlight = true
        lock.unlock()

        queue.async { [weak self] in
            guard let self = self else { return }
            let snap = self.detectSynchronously(bgraData: bgraData, width: width, height: height, colorSeq: colorSeq)
            self.lock.lock()
            self.inFlight = false
            self.lock.unlock()
            onComplete(snap)
        }
    }

    /// Runs `VNDetectHumanBodyPoseRequest` + `VNDetectHumanHandPoseRequest` synchronously in memory.
    func detectSynchronously(
        bgraData: Data,
        width: Int,
        height: Int,
        colorSeq: UInt64
    ) -> RawVisionPoseSnapshot {
        guard width > 0, height > 0, bgraData.count >= width * height * 4,
              let provider = CGDataProvider(data: bgraData as CFData),
              let cgImage = CGImage(
                  width: width,
                  height: height,
                  bitsPerComponent: 8,
                  bitsPerPixel: 32,
                  bytesPerRow: width * 4,
                  space: colorSpace,
                  bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                  provider: provider,
                  decode: nil,
                  shouldInterpolate: false,
                  intent: .defaultIntent
              ) else {
            return RawVisionPoseSnapshot(colorSeq: colorSeq)
        }

        let bodyReq = VNDetectHumanBodyPoseRequest()
        let handReq = VNDetectHumanHandPoseRequest()
        handReq.maximumHandCount = 4

        let handler = VNImageRequestHandler(cgImage: cgImage, orientation: .up, options: [:])
        do {
            try handler.perform([bodyReq, handReq])
        } catch {
            return RawVisionPoseSnapshot(colorSeq: colorSeq)
        }

        var joints: [RawVisionJoint2D] = []
        var bones: [RawVisionBoneEdge] = []

        let bodyObsList = bodyReq.results ?? []
        for (bIdx, obs) in bodyObsList.enumerated() {
            guard let recognized = try? obs.recognizedPoints(.all) else { continue }
            let prefix = "b\(bIdx):"
            for jName in Self.allBodyJoints {
                guard let pt = recognized[jName], pt.confidence > 0.01 else { continue }
                joints.append(
                    RawVisionJoint2D(
                        id: "\(prefix)\(jName.rawValue.rawValue)",
                        normTopLeftX: Float(pt.location.x),
                        normTopLeftY: Float(1.0 - pt.location.y),
                        confidence: pt.confidence,
                        isHand: false
                    )
                )
            }
            for (fromJ, toJ) in Self.bodyBonePairs {
                bones.append(
                    RawVisionBoneEdge(
                        fromId: "\(prefix)\(fromJ.rawValue.rawValue)",
                        toId: "\(prefix)\(toJ.rawValue.rawValue)",
                        isHand: false
                    )
                )
            }
        }

        let handObsList = handReq.results ?? []
        for (hIdx, obs) in handObsList.enumerated() {
            guard let recognized = try? obs.recognizedPoints(.all) else { continue }
            let prefix = "h\(hIdx):"
            for jName in Self.allHandJoints {
                guard let pt = recognized[jName], pt.confidence > 0.01 else { continue }
                joints.append(
                    RawVisionJoint2D(
                        id: "\(prefix)\(jName.rawValue.rawValue)",
                        normTopLeftX: Float(pt.location.x),
                        normTopLeftY: Float(1.0 - pt.location.y),
                        confidence: pt.confidence,
                        isHand: true
                    )
                )
            }
            for (fromJ, toJ) in Self.handBonePairs {
                bones.append(
                    RawVisionBoneEdge(
                        fromId: "\(prefix)\(fromJ.rawValue.rawValue)",
                        toId: "\(prefix)\(toJ.rawValue.rawValue)",
                        isHand: true
                    )
                )
            }
        }

        return RawVisionPoseSnapshot(
            colorSeq: colorSeq,
            bodyCount: bodyObsList.count,
            handCount: handObsList.count,
            joints: joints,
            bones: bones
        )
    }
}
