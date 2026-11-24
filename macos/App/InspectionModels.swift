import CoreGraphics
import Foundation

enum CanvasToolMode: String, CaseIterable, Identifiable {
    case inspect = "Inspect"
    case pan = "Pan"
    case regionBox = "Region"
    case ruler3D = "Ruler"

    var id: String { rawValue }

    var symbolName: String {
        switch self {
        case .inspect: return "cursorarrow.rays"
        case .pan: return "hand.draw"
        case .regionBox: return "rectangle.dashed"
        case .ruler3D: return "ruler"
        }
    }

    var hint: String {
        switch self {
        case .inspect:
            return "Click 2 pts for 3D ruler · Drag box (or Shift-drag when zoomed) for region stats"
        case .pan:
            return "Drag to pan · Scroll to zoom · Double-click to reset"
        case .regionBox:
            return "Drag a rectangle to inspect depth stats · Esc clears"
        case .ruler3D:
            return "Click two points to measure 3D distance in mm · Esc clears"
        }
    }
}

struct CursorReadout: Equatable {
    let pixelX: Int
    let pixelY: Int
    let hasSensorDepthIr: Bool
    let depthMm: Float?
    let rawIr: Float?

    var coordText: String {
        "(\(pixelX), \(pixelY))"
    }

    var depthText: String {
        guard hasSensorDepthIr else { return "—" }
        if let d = depthMm, d.isFinite, d > 0 {
            return String(format: "%.0f mm", d)
        }
        return "No depth"
    }

    var irText: String {
        guard hasSensorDepthIr else { return "—" }
        if let ir = rawIr, ir.isFinite {
            return String(format: "%.0f", ir)
        }
        return "—"
    }
}

struct RegionSelectionBox: Equatable {
    /// Display-image pixel coordinates (inclusive, clamped to rendered image bounds).
    var dispX0: Int
    var dispY0: Int
    var dispX1: Int
    var dispY1: Int

    var minDispX: Int { min(dispX0, dispX1) }
    var maxDispX: Int { max(dispX0, dispX1) }
    var minDispY: Int { min(dispY0, dispY1) }
    var maxDispY: Int { max(dispY0, dispY1) }
}

struct RegionDepthStats: Equatable {
    let sensorMinX: Int
    let sensorMinY: Int
    let sensorMaxX: Int
    let sensorMaxY: Int
    let totalPixels: Int
    let validPixels: Int
    let invalidPixels: Int
    let invalidShare: Double
    let meanMm: Double?
    let medianMm: Double?
    let stdDevMm: Double?
    let minMm: Double?
    let maxMm: Double?

    static func compute(
        depthMm: [Float],
        width: Int,
        height: Int,
        x0: Int,
        y0: Int,
        x1: Int,
        y1: Int
    ) -> RegionDepthStats? {
        guard width > 0, height > 0, depthMm.count >= width * height else { return nil }
        let minX = max(0, min(x0, x1))
        let maxX = min(width - 1, max(x0, x1))
        let minY = max(0, min(y0, y1))
        let maxY = min(height - 1, max(y0, y1))
        guard minX <= maxX, minY <= maxY else { return nil }

        let total = (maxX - minX + 1) * (maxY - minY + 1)
        guard total > 0 else { return nil }

        var validValues: [Double] = []
        validValues.reserveCapacity(total)
        var invalidCount = 0

        for y in minY...maxY {
            let rowBase = y * width
            for x in minX...maxX {
                let d = depthMm[rowBase + x]
                if d.isFinite && d > 0.0 {
                    validValues.append(Double(d))
                } else {
                    invalidCount += 1
                }
            }
        }

        let invalidRatio = Double(invalidCount) / Double(total)
        guard !validValues.isEmpty else {
            return RegionDepthStats(
                sensorMinX: minX,
                sensorMinY: minY,
                sensorMaxX: maxX,
                sensorMaxY: maxY,
                totalPixels: total,
                validPixels: 0,
                invalidPixels: invalidCount,
                invalidShare: invalidRatio,
                meanMm: nil,
                medianMm: nil,
                stdDevMm: nil,
                minMm: nil,
                maxMm: nil
            )
        }

        validValues.sort()
        let count = validValues.count
        let minVal = validValues[0]
        let maxVal = validValues[count - 1]

        var sum = 0.0
        for v in validValues { sum += v }
        let mean = sum / Double(count)

        let median: Double
        if count % 2 == 1 {
            median = validValues[count / 2]
        } else {
            median = 0.5 * (validValues[count / 2 - 1] + validValues[count / 2])
        }

        var sqDiffSum = 0.0
        for v in validValues {
            let diff = v - mean
            sqDiffSum += diff * diff
        }
        let stdDev = (sqDiffSum / Double(count)).squareRoot()

        return RegionDepthStats(
            sensorMinX: minX,
            sensorMinY: minY,
            sensorMaxX: maxX,
            sensorMaxY: maxY,
            totalPixels: total,
            validPixels: count,
            invalidPixels: invalidCount,
            invalidShare: invalidRatio,
            meanMm: mean,
            medianMm: median,
            stdDevMm: stdDev,
            minMm: minVal,
            maxMm: maxVal
        )
    }
}

struct RulerEndpoint: Equatable {
    let dispX: Int
    let dispY: Int
    let sensorX: Int
    let sensorY: Int
    let hasSensorCoord: Bool
}

struct RulerMeasurementState: Equatable {
    static let noDepthMessageText = "no depth here"
    static let intrinsicsUnavailableText = "Intrinsics unavailable"

    var pointA: RulerEndpoint
    var pointB: RulerEndpoint?
    var distanceMm: Float?
    var errorMessage: String?

    var formattedLabel: String {
        if let err = errorMessage {
            return err
        }
        if let d = distanceMm {
            return String(format: "%.1f mm (%.3f m)", d, d / 1000.0)
        }
        return "Click second point…"
    }

    static func evaluateDistance(
        pointA: RulerEndpoint,
        pointB: RulerEndpoint,
        intrinsics: kv_ir_intrinsics_t,
        depthMm: [Float],
        width: Int = 512,
        height: Int = 424
    ) -> (distanceMm: Float?, errorMessage: String?) {
        guard pointA.hasSensorCoord, pointB.hasSensorCoord else {
            return (nil, noDepthMessageText)
        }
        guard intrinsics.valid, intrinsics.fx > 0, intrinsics.fy > 0 else {
            return (nil, intrinsicsUnavailableText)
        }
        guard depthMm.count >= width * height else {
            return (nil, noDepthMessageText)
        }
        var dist: Float = 0.0
        var intr = intrinsics
        let ok = depthMm.withUnsafeBufferPointer { buf -> Bool in
            guard let base = buf.baseAddress else { return false }
            return kv_measure_3d_distance_mm(
                &intr,
                base,
                UInt32(width),
                UInt32(height),
                Int32(pointA.sensorX),
                Int32(pointA.sensorY),
                Int32(pointB.sensorX),
                Int32(pointB.sensorY),
                &dist
            )
        }
        if ok && dist.isFinite {
            return (dist, nil)
        }
        return (nil, noDepthMessageText)
    }
}

struct ViewportTransform: Equatable {
    static let minZoom: CGFloat = 1.0
    static let maxZoom: CGFloat = 16.0

    var zoomScale: CGFloat = 1.0
    var panOffset: CGPoint = .zero

    var isZoomedOrPanned: Bool {
        zoomScale > 1.001 || abs(panOffset.x) > 0.5 || abs(panOffset.y) > 0.5
    }

    mutating func reset() {
        zoomScale = 1.0
        panOffset = .zero
    }

    func baseAspectFitRect(viewSize: CGSize, imageSize: CGSize) -> CGRect {
        guard viewSize.width > 1, viewSize.height > 1, imageSize.width > 0, imageSize.height > 0 else {
            return .zero
        }
        let scale = min(viewSize.width / imageSize.width, viewSize.height / imageSize.height)
        let w = imageSize.width * scale
        let h = imageSize.height * scale
        let x = (viewSize.width - w) * 0.5
        let y = (viewSize.height - h) * 0.5
        return CGRect(x: x, y: y, width: w, height: h)
    }

    func displayedImageRect(viewSize: CGSize, imageSize: CGSize) -> CGRect {
        let base = baseAspectFitRect(viewSize: viewSize, imageSize: imageSize)
        guard base.width > 0, base.height > 0 else { return .zero }
        let center = CGPoint(x: viewSize.width * 0.5, y: viewSize.height * 0.5)
        let w = base.width * zoomScale
        let h = base.height * zoomScale
        let x = center.x + (base.minX - center.x) * zoomScale + panOffset.x
        let y = center.y + (base.minY - center.y) * zoomScale + panOffset.y
        return CGRect(x: x, y: y, width: w, height: h)
    }

    mutating func zoomAround(
        cursorInView: CGPoint,
        viewSize: CGSize,
        imageSize: CGSize,
        factor: CGFloat
    ) {
        let oldZoom = zoomScale
        let newZoom = min(Self.maxZoom, max(Self.minZoom, oldZoom * factor))
        if newZoom <= 1.001 {
            reset()
            return
        }
        let ratio = newZoom / oldZoom
        let center = CGPoint(x: viewSize.width * 0.5, y: viewSize.height * 0.5)
        // Keep the image point currently under cursorInView fixed at cursorInView:
        // newRectOrigin = cursor - (cursor - oldRectOrigin) * ratio
        let newPanX = (cursorInView.x - center.x) * (1.0 - ratio) + panOffset.x * ratio
        let newPanY = (cursorInView.y - center.y) * (1.0 - ratio) + panOffset.y * ratio
        zoomScale = newZoom
        panOffset = clampedPan(
            proposed: CGPoint(x: newPanX, y: newPanY),
            zoom: newZoom,
            viewSize: viewSize,
            imageSize: imageSize
        )
    }

    mutating func panBy(delta: CGPoint, viewSize: CGSize, imageSize: CGSize) {
        guard zoomScale > 1.001 else {
            panOffset = .zero
            return
        }
        let next = CGPoint(x: panOffset.x + delta.x, y: panOffset.y + delta.y)
        panOffset = clampedPan(proposed: next, zoom: zoomScale, viewSize: viewSize, imageSize: imageSize)
    }

    private func clampedPan(proposed: CGPoint, zoom: CGFloat, viewSize: CGSize, imageSize: CGSize) -> CGPoint {
        let base = baseAspectFitRect(viewSize: viewSize, imageSize: imageSize)
        let w = base.width * zoom
        let h = base.height * zoom
        let maxX = max(0.0, (w - viewSize.width) * 0.5 + viewSize.width * 0.25)
        let maxY = max(0.0, (h - viewSize.height) * 0.5 + viewSize.height * 0.25)
        return CGPoint(
            x: min(maxX, max(-maxX, proposed.x)),
            y: min(maxY, max(-maxY, proposed.y))
        )
    }

    /// Converts a view coordinate (top-left origin, matching flipped NSView) to display-image pixel `(dispX, dispY)`.
    /// Returns `nil` when the cursor is outside the displayed image rectangle.
    func viewPointToDisplayPixel(
        _ viewPoint: CGPoint,
        viewSize: CGSize,
        imageWidth: Int,
        imageHeight: Int
    ) -> (dispX: Int, dispY: Int)? {
        guard imageWidth > 0, imageHeight > 0 else { return nil }
        let rect = displayedImageRect(
            viewSize: viewSize,
            imageSize: CGSize(width: imageWidth, height: imageHeight)
        )
        guard rect.width > 0, rect.height > 0, rect.contains(viewPoint) else {
            return nil
        }
        let normX = (viewPoint.x - rect.minX) / rect.width
        let normY = (viewPoint.y - rect.minY) / rect.height
        let px = min(imageWidth - 1, max(0, Int(normX * CGFloat(imageWidth))))
        let py = min(imageHeight - 1, max(0, Int(normY * CGFloat(imageHeight))))
        return (px, py)
    }

    func displayPixelCenterInView(
        dispX: Int,
        dispY: Int,
        viewSize: CGSize,
        imageWidth: Int,
        imageHeight: Int
    ) -> CGPoint {
        let rect = displayedImageRect(
            viewSize: viewSize,
            imageSize: CGSize(width: imageWidth, height: imageHeight)
        )
        guard rect.width > 0, rect.height > 0, imageWidth > 0, imageHeight > 0 else {
            return .zero
        }
        let nx = (CGFloat(dispX) + 0.5) / CGFloat(imageWidth)
        let ny = (CGFloat(dispY) + 0.5) / CGFloat(imageHeight)
        return CGPoint(x: rect.minX + nx * rect.width, y: rect.minY + ny * rect.height)
    }

    /// Maps a display-image pixel `(dispX, dispY)` back through orientation (`mirrorHorizontal`, `rotate180`)
    /// and Side-by-Side panel layout into the underlying `512×424` Depth/IR sensor coordinate.
    static func mapDisplayPixelToSensor(
        dispX: Int,
        dispY: Int,
        imageWidth: Int,
        imageHeight: Int,
        mode: ViewerModeOption,
        sbsLayout: SbsLayoutOption,
        mirrorHorizontal: Bool,
        rotate180: Bool,
        depthColorBlendEnabled: Bool = false
    ) -> (pixelX: Int, pixelY: Int, hasDepthIr: Bool) {
        guard imageWidth > 0, imageHeight > 0 else {
            return (0, 0, false)
        }
        let flipX = (mirrorHorizontal != rotate180)
        let flipY = rotate180
        let canonX = flipX ? (imageWidth - 1 - dispX) : dispX
        let canonY = flipY ? (imageHeight - 1 - dispY) : dispY

        if depthColorBlendEnabled && mode != .pointCloud3D {
            let sx = min(511, max(0, canonX))
            let sy = min(423, max(0, canonY))
            return (sx, sy, imageWidth == 512 && imageHeight == 424)
        }

        switch mode {
        case .ir, .depth:
            let sx = min(511, max(0, canonX))
            let sy = min(423, max(0, canonY))
            return (sx, sy, imageWidth == 512 && imageHeight == 424)

        case .color, .pointCloud3D:
            return (canonX, canonY, false)

        case .sideBySide:
            switch sbsLayout {
            case .irDepth:
                // 1024x424: [0..511] = IR, [512..1023] = Depth
                let sx = (canonX >= 512) ? (canonX - 512) : canonX
                return (min(511, max(0, sx)), min(423, max(0, canonY)), imageWidth == 1024 && imageHeight == 424)

            case .colorDepth, .colorIr:
                // 1280x424: [0..753] = scaled 1080p Colour, [754..767] = divider, [768..1279] = 512x424 Depth/IR
                if canonX >= 768 {
                    let sx = min(511, max(0, canonX - 768))
                    let sy = min(423, max(0, canonY))
                    return (sx, sy, true)
                } else if canonX < 754 {
                    let cx = Int((Double(canonX) * 1920.0) / 754.0)
                    let cy = Int((Double(canonY) * 1080.0) / 424.0)
                    return (min(1919, max(0, cx)), min(1079, max(0, cy)), false)
                } else {
                    return (canonX, canonY, false)
                }
            }
        }
    }
}

enum DropBannerEvaluator {
    static let warningBannerText = "Frames are dropping. Try a different USB port, ideally a rear port directly on the motherboard."
    static let thresholdRatio10s: Double = 0.02

    static func shouldShowBanner(irDropRatio10s: Double, depthDropRatio10s: Double) -> Bool {
        irDropRatio10s > thresholdRatio10s || depthDropRatio10s > thresholdRatio10s
    }
}

struct HealthMetricsSnapshot: Equatable {
    var irFps: Double = 0.0
    var depthFps: Double = 0.0
    var colorFps: Double = 0.0
    var irDropsPerMin: Double = 0.0
    var depthDropsPerMin: Double = 0.0
    var colorDropsPerMin: Double = 0.0
    var irDropRatio10s: Double = 0.0
    var depthDropRatio10s: Double = 0.0
    var dropWarningActive: Bool = false
    var interFrameP50Ms: Double = 0.0
    var interFrameP95Ms: Double = 0.0
    var depthProcessingMs: Double = 0.0
    var driverErrors: UInt32 = 0
    var lastDriverError: String = ""
    var usbLocation: String = "USB Disconnected"
}
