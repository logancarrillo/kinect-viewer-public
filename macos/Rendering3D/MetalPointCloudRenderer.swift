import CoreGraphics
import Foundation
import Metal
import simd

enum PointCloudColorSourceOption: Int, CaseIterable, Identifiable, Sendable {
    case depth = 0
    case ir = 1
    case registeredColor = 2

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .depth: return "Depth Map"
        case .ir: return "IR"
        case .registeredColor: return "Live Colour"
        }
    }

    var cValue: kv_point_color_source_t {
        kv_point_color_source_t(rawValue: UInt32(rawValue))
    }

    var needsLiveColor: Bool {
        self == .registeredColor
    }
}

struct PointCloudCameraState: Equatable, Sendable {
    static let defaultDistanceMeters: Float = 2.05
    static let defaultPivotZMeters: Float = 1.85
    static let minDistanceMeters: Float = 0.28
    static let maxDistanceMeters: Float = 9.50
    static let maxAbsPitchRad: Float = 1.48

    var yawRad: Float = 0.0
    var pitchRad: Float = 0.0
    var distanceMeters: Float = Self.defaultDistanceMeters
    var panOffset: SIMD3<Float> = .zero
    var fovYRadians: Float = 54.0 * (.pi / 180.0)

    var isDefault: Bool {
        abs(yawRad) < 1e-4 &&
        abs(pitchRad) < 1e-4 &&
        abs(distanceMeters - Self.defaultDistanceMeters) < 1e-3 &&
        simd_length(panOffset) < 1e-4
    }

    mutating func reset() {
        yawRad = 0.0
        pitchRad = 0.0
        distanceMeters = Self.defaultDistanceMeters
        panOffset = .zero
    }

    mutating func orbit(deltaYaw: Float, deltaPitch: Float) {
        if deltaYaw.isFinite {
            let twoPi: Float = 2.0 * .pi
            var wrapped = (yawRad + deltaYaw + .pi).truncatingRemainder(dividingBy: twoPi)
            if wrapped < 0.0 {
                wrapped += twoPi
            }
            yawRad = wrapped - .pi
        }
        if deltaPitch.isFinite {
            pitchRad = min(Self.maxAbsPitchRad, max(-Self.maxAbsPitchRad, pitchRad + deltaPitch))
        }
    }

    mutating func zoom(scaleFactor: Float) {
        guard scaleFactor.isFinite && scaleFactor > 0.01 else { return }
        let next = distanceMeters / scaleFactor
        distanceMeters = min(Self.maxDistanceMeters, max(Self.minDistanceMeters, next))
    }

    mutating func pan(deltaRightMeters: Float, deltaUpMeters: Float) {
        let basis = cameraBasis()
        panOffset += basis.right * deltaRightMeters + basis.up * deltaUpMeters
        panOffset.x = min(4.0, max(-4.0, panOffset.x))
        panOffset.y = min(4.0, max(-4.0, panOffset.y))
        panOffset.z = min(4.0, max(-4.0, panOffset.z))
    }

    private func cameraBasis() -> (eye: SIMD3<Float>, target: SIMD3<Float>, right: SIMD3<Float>, up: SIMD3<Float>, forward: SIMD3<Float>) {
        let baseTarget = SIMD3<Float>(0.0, 0.0, -Self.defaultPivotZMeters) + panOffset
        let cosP = cos(pitchRad)
        let offset = SIMD3<Float>(
            distanceMeters * cosP * sin(yawRad),
            distanceMeters * sin(pitchRad),
            distanceMeters * cosP * cos(yawRad)
        )
        let eye = baseTarget + offset
        let forward = simd_normalize(baseTarget - eye)
        let worldUp = SIMD3<Float>(0.0, 1.0, 0.0)
        var right = simd_cross(forward, worldUp)
        if simd_length_squared(right) < 1e-6 {
            right = SIMD3<Float>(1.0, 0.0, 0.0)
        } else {
            right = simd_normalize(right)
        }
        let camUp = simd_normalize(simd_cross(right, forward))
        return (eye, baseTarget, right, camUp, forward)
    }

    func viewProjectionMatrix(aspect: Float) -> simd_float4x4 {
        let b = cameraBasis()
        let f = b.forward
        let r = b.right
        let u = b.up
        let eye = b.eye

        // Column-major view matrix (camera looks down -Z in view space)
        let view = simd_float4x4(
            SIMD4<Float>(r.x, u.x, -f.x, 0.0),
            SIMD4<Float>(r.y, u.y, -f.y, 0.0),
            SIMD4<Float>(r.z, u.z, -f.z, 0.0),
            SIMD4<Float>(-simd_dot(r, eye), -simd_dot(u, eye), simd_dot(f, eye), 1.0)
        )

        // Right-handed perspective matrix with Metal [0, 1] NDC depth
        let safeAspect = max(0.1, aspect)
        let ys = 1.0 / tan(fovYRadians * 0.5)
        let xs = ys / safeAspect
        let zNear: Float = 0.05
        let zFar: Float = 18.0
        let zs = zFar / (zNear - zFar)
        let proj = simd_float4x4(
            SIMD4<Float>(xs, 0.0, 0.0, 0.0),
            SIMD4<Float>(0.0, ys, 0.0, 0.0),
            SIMD4<Float>(0.0, 0.0, zs, -1.0),
            SIMD4<Float>(0.0, 0.0, zs * zNear, 0.0)
        )
        return proj * view
    }
}

struct Overlay3DVertex: Equatable, Sendable {
    var positionAndSize: SIMD4<Float>
    var color: SIMD4<Float>

    /// Creates a 3D overlay vertex from libfreenect2 `getPointXYZ` camera coordinates (`x` right, `y` down, `z` forward in meters).
    static func fromSensorMeters(
        xMeters: Float,
        yMeters: Float,
        zMeters: Float,
        flipX: Bool,
        flipY: Bool,
        pointSize: Float,
        color: SIMD4<Float>
    ) -> Overlay3DVertex {
        let sx: Float = flipX ? -1.0 : 1.0
        let sy: Float = flipY ? 1.0 : -1.0
        return Overlay3DVertex(
            positionAndSize: SIMD4<Float>(xMeters * sx, yMeters * sy, -zMeters, pointSize),
            color: color
        )
    }
}

private struct PointCloudUniforms {
    var mvp: simd_float4x4
    var intrinsics: SIMD4<Float>
    var params: SIMD4<Float>
}

final class MetalPointCloudRenderer {
    static let defaultRenderWidth = 960
    static let defaultRenderHeight = 720
    static let sensorWidth = 512
    static let sensorHeight = 424
    static let pointCount = sensorWidth * sensorHeight

    let device: MTLDevice?
    private let commandQueue: MTLCommandQueue?
    private let pointPipeline: MTLRenderPipelineState?
    private let overlayLinePipeline: MTLRenderPipelineState?
    private let overlayPointPipeline: MTLRenderPipelineState?
    private let depthState: MTLDepthStencilState?
    private let overlayDepthState: MTLDepthStencilState?

    private var depthInputBuffer: MTLBuffer?
    private var colorInputBuffer: MTLBuffer?
    private var overlayLineBuffer: MTLBuffer?
    private var overlayPointBuffer: MTLBuffer?
    private var colorTarget: MTLTexture?
    private var depthTarget: MTLTexture?
    private var targetWidth: Int = 0
    private var targetHeight: Int = 0

    var isAvailable: Bool {
        device != nil && commandQueue != nil && pointPipeline != nil && depthState != nil
    }

    init(preferredDevice: MTLDevice? = MTLCreateSystemDefaultDevice()) {
        self.device = preferredDevice
        guard let dev = preferredDevice,
              let queue = dev.makeCommandQueue() else {
            self.commandQueue = nil
            self.pointPipeline = nil
            self.overlayLinePipeline = nil
            self.overlayPointPipeline = nil
            self.depthState = nil
            self.overlayDepthState = nil
            return
        }
        self.commandQueue = queue

        let shaderSource = """
        #include <metal_stdlib>
        using namespace metal;

        struct PointCloudUniforms {
            float4x4 mvp;
            float4 intrinsics; // x=fx, y=fy, z=cx, w=cy
            float4 params;     // x=pointSize, y=flipXSign, z=flipYSign, w=gridWidth
        };

        struct PointOut {
            float4 position [[position]];
            float4 color;
            float pointSize [[point_size]];
            float valid;
        };

        vertex PointOut pointCloudVertex(
            uint vid [[vertex_id]],
            constant float* depthMm [[buffer(0)]],
            constant uchar4* bgraColors [[buffer(1)]],
            constant PointCloudUniforms& u [[buffer(2)]]
        ) {
            PointOut out;
            float d_mm = depthMm[vid];
            if (!(d_mm > 0.0f) || isinf(d_mm) || isnan(d_mm)) {
                out.position = float4(2.0f, 2.0f, 2.0f, 1.0f);
                out.color = float4(0.0f);
                out.pointSize = 0.0f;
                out.valid = 0.0f;
                return out;
            }

            uint w = uint(u.params.w);
            uint col = vid % w;
            uint row = vid / w;

            float z_m = d_mm * 0.001f;
            float x_m = ((float(col) + 0.5f - u.intrinsics.z) / u.intrinsics.x) * z_m * u.params.y;
            float y_m = -((float(row) + 0.5f - u.intrinsics.w) / u.intrinsics.y) * z_m * u.params.z;

            out.position = u.mvp * float4(x_m, y_m, -z_m, 1.0f);
            uchar4 bgra = bgraColors[vid];
            out.color = float4(float(bgra.z) / 255.0f, float(bgra.y) / 255.0f, float(bgra.x) / 255.0f, 1.0f);
            out.pointSize = u.params.x;
            out.valid = 1.0f;
            return out;
        }

        fragment float4 pointCloudFragment(
            PointOut in [[stage_in]],
            float2 pointCoord [[point_coord]]
        ) {
            if (in.valid < 0.5f) {
                discard_fragment();
            }
            if (in.pointSize >= 2.5f) {
                float2 d = pointCoord - float2(0.5f, 0.5f);
                if (dot(d, d) > 0.25f) {
                    discard_fragment();
                }
            }
            return in.color;
        }

        struct Overlay3DVertex {
            float4 positionAndSize;
            float4 color;
        };

        struct OverlayOut {
            float4 position [[position]];
            float4 color;
            float pointSize [[point_size]];
        };

        vertex OverlayOut overlay3DVertex(
            uint vid [[vertex_id]],
            constant Overlay3DVertex* verts [[buffer(0)]],
            constant PointCloudUniforms& u [[buffer(1)]]
        ) {
            OverlayOut out;
            Overlay3DVertex v = verts[vid];
            out.position = u.mvp * float4(v.positionAndSize.xyz, 1.0f);
            out.color = v.color;
            out.pointSize = v.positionAndSize.w;
            return out;
        }

        fragment float4 overlay3DLineFragment(OverlayOut in [[stage_in]]) {
            return in.color;
        }

        fragment float4 overlay3DPointFragment(
            OverlayOut in [[stage_in]],
            float2 pointCoord [[point_coord]]
        ) {
            float2 d = pointCoord - float2(0.5f, 0.5f);
            if (dot(d, d) > 0.25f) {
                discard_fragment();
            }
            return in.color;
        }
        """

        let library = try? dev.makeLibrary(source: shaderSource, options: nil)

        if let lib = library,
           let vFn = lib.makeFunction(name: "pointCloudVertex"),
           let fFn = lib.makeFunction(name: "pointCloudFragment") {
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction = vFn
            desc.fragmentFunction = fFn
            desc.colorAttachments[0].pixelFormat = .bgra8Unorm
            desc.depthAttachmentPixelFormat = .depth32Float
            self.pointPipeline = try? dev.makeRenderPipelineState(descriptor: desc)
        } else {
            self.pointPipeline = nil
        }

        if let lib = library,
           let ovV = lib.makeFunction(name: "overlay3DVertex"),
           let ovLineF = lib.makeFunction(name: "overlay3DLineFragment"),
           let ovPtF = lib.makeFunction(name: "overlay3DPointFragment") {
            let lineDesc = MTLRenderPipelineDescriptor()
            lineDesc.vertexFunction = ovV
            lineDesc.fragmentFunction = ovLineF
            lineDesc.colorAttachments[0].pixelFormat = .bgra8Unorm
            lineDesc.colorAttachments[0].isBlendingEnabled = true
            lineDesc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
            lineDesc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
            lineDesc.colorAttachments[0].sourceAlphaBlendFactor = .one
            lineDesc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            lineDesc.depthAttachmentPixelFormat = .depth32Float
            self.overlayLinePipeline = try? dev.makeRenderPipelineState(descriptor: lineDesc)

            let ptDesc = MTLRenderPipelineDescriptor()
            ptDesc.vertexFunction = ovV
            ptDesc.fragmentFunction = ovPtF
            ptDesc.colorAttachments[0].pixelFormat = .bgra8Unorm
            ptDesc.colorAttachments[0].isBlendingEnabled = true
            ptDesc.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
            ptDesc.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
            ptDesc.colorAttachments[0].sourceAlphaBlendFactor = .one
            ptDesc.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            ptDesc.depthAttachmentPixelFormat = .depth32Float
            self.overlayPointPipeline = try? dev.makeRenderPipelineState(descriptor: ptDesc)
        } else {
            self.overlayLinePipeline = nil
            self.overlayPointPipeline = nil
        }

        let dDesc = MTLDepthStencilDescriptor()
        dDesc.depthCompareFunction = .less
        dDesc.isDepthWriteEnabled = true
        self.depthState = dev.makeDepthStencilState(descriptor: dDesc)

        let ovDDesc = MTLDepthStencilDescriptor()
        ovDDesc.depthCompareFunction = .always
        ovDDesc.isDepthWriteEnabled = false
        self.overlayDepthState = dev.makeDepthStencilState(descriptor: ovDDesc)

        self.depthInputBuffer = dev.makeBuffer(
            length: Self.pointCount * MemoryLayout<Float>.stride,
            options: .storageModeShared
        )
        self.colorInputBuffer = dev.makeBuffer(
            length: Self.pointCount * 4,
            options: .storageModeShared
        )
    }

    private func ensureRenderTargets(width: Int, height: Int) -> Bool {
        guard let dev = device, width > 0, height > 0 else { return false }
        if targetWidth == width && targetHeight == height && colorTarget != nil && depthTarget != nil {
            return true
        }
        let cDesc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: width,
            height: height,
            mipmapped: false
        )
        cDesc.usage = [.renderTarget, .shaderRead]
        cDesc.storageMode = .shared

        let dDesc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .depth32Float,
            width: width,
            height: height,
            mipmapped: false
        )
        dDesc.usage = [.renderTarget]
        dDesc.storageMode = .private

        guard let cTex = dev.makeTexture(descriptor: cDesc),
              let dTex = dev.makeTexture(descriptor: dDesc) else {
            return false
        }
        colorTarget = cTex
        depthTarget = dTex
        targetWidth = width
        targetHeight = height
        return true
    }

    /// Renders a `512×424` registered depth + BGRA point cloud on Metal into `outBgraData` (`width × height × 4`).
    @discardableResult
    func render(
        undistortedDepthMm: UnsafeBufferPointer<Float>,
        pointBgra: UnsafeBufferPointer<UInt8>,
        intrinsics: kv_ir_intrinsics_t,
        camera: PointCloudCameraState,
        pointSize: Float,
        mirrorHorizontal: Bool = false,
        rotate180: Bool = false,
        overlayLineVertices: [Overlay3DVertex] = [],
        overlayPointVertices: [Overlay3DVertex] = [],
        renderWidth: Int = MetalPointCloudRenderer.defaultRenderWidth,
        renderHeight: Int = MetalPointCloudRenderer.defaultRenderHeight,
        outBgraData: inout Data
    ) -> Bool {
        guard let dev = device,
              let queue = commandQueue,
              let pipeline = pointPipeline,
              let dState = depthState,
              let depthBuf = depthInputBuffer,
              let colorBuf = colorInputBuffer,
              undistortedDepthMm.count >= Self.pointCount,
              pointBgra.count >= Self.pointCount * 4,
              ensureRenderTargets(width: renderWidth, height: renderHeight),
              let cTex = colorTarget,
              let dTex = depthTarget else {
            return false
        }

        if let dSrc = undistortedDepthMm.baseAddress {
            memcpy(depthBuf.contents(), dSrc, Self.pointCount * MemoryLayout<Float>.stride)
        }
        if let cSrc = pointBgra.baseAddress {
            memcpy(colorBuf.contents(), cSrc, Self.pointCount * 4)
        }

        guard intrinsics.valid, intrinsics.fx > 1.0, intrinsics.fy > 1.0 else {
            return false
        }
        let fx = intrinsics.fx
        let fy = intrinsics.fy
        let cx = intrinsics.cx
        let cy = intrinsics.cy

        let flipX = (mirrorHorizontal != rotate180)
        let flipY = rotate180
        let aspect = Float(renderWidth) / Float(max(1, renderHeight))
        var uniforms = PointCloudUniforms(
            mvp: camera.viewProjectionMatrix(aspect: aspect),
            intrinsics: SIMD4<Float>(fx, fy, cx, cy),
            params: SIMD4<Float>(
                max(1.0, min(16.0, pointSize)),
                flipX ? -1.0 : 1.0,
                flipY ? -1.0 : 1.0,
                Float(Self.sensorWidth)
            )
        )

        let passDesc = MTLRenderPassDescriptor()
        passDesc.colorAttachments[0].texture = cTex
        passDesc.colorAttachments[0].loadAction = .clear
        // Pure black background (0, 0, 0, 1) so everything outside near/far cut-off planes is strictly black
        passDesc.colorAttachments[0].clearColor = MTLClearColor(red: 0.0, green: 0.0, blue: 0.0, alpha: 1.0)
        passDesc.colorAttachments[0].storeAction = .store
        passDesc.depthAttachment.texture = dTex
        passDesc.depthAttachment.loadAction = .clear
        passDesc.depthAttachment.clearDepth = 1.0
        passDesc.depthAttachment.storeAction = .dontCare

        guard let cmdBuf = queue.makeCommandBuffer(),
              let enc = cmdBuf.makeRenderCommandEncoder(descriptor: passDesc) else {
            return false
        }

        enc.setRenderPipelineState(pipeline)
        enc.setDepthStencilState(dState)
        enc.setVertexBuffer(depthBuf, offset: 0, index: 0)
        enc.setVertexBuffer(colorBuf, offset: 0, index: 1)
        enc.setVertexBytes(&uniforms, length: MemoryLayout<PointCloudUniforms>.stride, index: 2)
        enc.drawPrimitives(type: .point, vertexStart: 0, vertexCount: Self.pointCount)

        if !overlayLineVertices.isEmpty,
           let linePipe = overlayLinePipeline,
           let ovDepth = overlayDepthState {
            let byteLen = overlayLineVertices.count * MemoryLayout<Overlay3DVertex>.stride
            if overlayLineBuffer == nil || (overlayLineBuffer?.length ?? 0) < byteLen {
                overlayLineBuffer = dev.makeBuffer(length: max(byteLen, 4096), options: .storageModeShared)
            }
            if let lBuf = overlayLineBuffer {
                overlayLineVertices.withUnsafeBytes { raw in
                    if let base = raw.baseAddress {
                        memcpy(lBuf.contents(), base, byteLen)
                    }
                }
                enc.setRenderPipelineState(linePipe)
                enc.setDepthStencilState(ovDepth)
                enc.setVertexBuffer(lBuf, offset: 0, index: 0)
                enc.setVertexBytes(&uniforms, length: MemoryLayout<PointCloudUniforms>.stride, index: 1)
                enc.drawPrimitives(type: .line, vertexStart: 0, vertexCount: overlayLineVertices.count)
            }
        }

        if !overlayPointVertices.isEmpty,
           let ptPipe = overlayPointPipeline,
           let ovDepth = overlayDepthState {
            let byteLen = overlayPointVertices.count * MemoryLayout<Overlay3DVertex>.stride
            if overlayPointBuffer == nil || (overlayPointBuffer?.length ?? 0) < byteLen {
                overlayPointBuffer = dev.makeBuffer(length: max(byteLen, 4096), options: .storageModeShared)
            }
            if let pBuf = overlayPointBuffer {
                overlayPointVertices.withUnsafeBytes { raw in
                    if let base = raw.baseAddress {
                        memcpy(pBuf.contents(), base, byteLen)
                    }
                }
                enc.setRenderPipelineState(ptPipe)
                enc.setDepthStencilState(ovDepth)
                enc.setVertexBuffer(pBuf, offset: 0, index: 0)
                enc.setVertexBytes(&uniforms, length: MemoryLayout<PointCloudUniforms>.stride, index: 1)
                enc.drawPrimitives(type: .point, vertexStart: 0, vertexCount: overlayPointVertices.count)
            }
        }

        enc.endEncoding()
        cmdBuf.commit()
        cmdBuf.waitUntilCompleted()

        let outBytes = renderWidth * renderHeight * 4
        if outBgraData.count != outBytes {
            outBgraData.count = outBytes
        }
        outBgraData.withUnsafeMutableBytes { rawPtr in
            if let dst = rawPtr.baseAddress {
                cTex.getBytes(
                    dst,
                    bytesPerRow: renderWidth * 4,
                    from: MTLRegionMake2D(0, 0, renderWidth, renderHeight),
                    mipmapLevel: 0
                )
            }
        }
        return true
    }
}
