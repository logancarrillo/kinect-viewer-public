import AppKit
import CoreGraphics
import SwiftUI

final class InteractiveCanvasNSView: NSView {
    var image: CGImage? {
        didSet { needsDisplay = true }
    }
    var crispNearest: Bool = true {
        didSet {
            if oldValue != crispNearest { needsDisplay = true }
        }
    }
    var viewport: ViewportTransform = ViewportTransform() {
        didSet {
            if oldValue != viewport { needsDisplay = true }
        }
    }
    var toolMode: CanvasToolMode = .inspect
    var regionBox: RegionSelectionBox? {
        didSet {
            if oldValue != regionBox { needsDisplay = true }
        }
    }
    var rulerState: RulerMeasurementState? {
        didSet {
            if oldValue != rulerState { needsDisplay = true }
        }
    }
    var skeletonOverlay: ProjectedSkeletonOverlay = ProjectedSkeletonOverlay() {
        didSet {
            if oldValue != skeletonOverlay { needsDisplay = true }
        }
    }

    var onHoverViewPoint: ((CGPoint?, CGSize) -> Void)?
    var onZoomAround: ((CGPoint, CGSize, CGFloat) -> Void)?
    var onPanBy: ((CGPoint, CGSize) -> Void)?
    var onResetViewport: (() -> Void)?
    var onRegionDragUpdate: ((CGPoint, CGPoint, CGSize) -> Void)?
    var onSingleClickPoint: ((CGPoint, CGSize) -> Void)?

    private var trackingAreaRef: NSTrackingArea?
    private var mouseDownPoint: CGPoint?
    private var activeGestureKind: GestureKind = .none

    private enum GestureKind {
        case none
        case pendingClick
        case panning
        case selectingRegion
    }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { false }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let existing = trackingAreaRef {
            removeTrackingArea(existing)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
        trackingAreaRef = area
    }

    private func localPoint(from event: NSEvent) -> CGPoint {
        convert(event.locationInWindow, from: nil)
    }

    override func mouseMoved(with event: NSEvent) {
        let pt = localPoint(from: event)
        onHoverViewPoint?(pt, bounds.size)
    }

    override func mouseExited(with event: NSEvent) {
        onHoverViewPoint?(nil, bounds.size)
    }

    override func scrollWheel(with event: NSEvent) {
        let pt = localPoint(from: event)
        let dy = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY * 0.008 : event.scrollingDeltaY * 0.08
        guard abs(dy) > 0.0001 else { return }
        let factor = CGFloat(exp(Double(dy)))
        onZoomAround?(pt, bounds.size, factor)
        onHoverViewPoint?(pt, bounds.size)
    }

    override func magnify(with event: NSEvent) {
        let pt = localPoint(from: event)
        let factor = max(0.2, 1.0 + event.magnification)
        onZoomAround?(pt, bounds.size, factor)
        onHoverViewPoint?(pt, bounds.size)
    }

    override func otherMouseDragged(with event: NSEvent) {
        onPanBy?(CGPoint(x: event.deltaX, y: event.deltaY), bounds.size)
    }

    override func mouseDown(with event: NSEvent) {
        let pt = localPoint(from: event)
        if event.clickCount >= 2 {
            mouseDownPoint = nil
            activeGestureKind = .none
            onResetViewport?()
            onHoverViewPoint?(pt, bounds.size)
            return
        }
        mouseDownPoint = pt
        activeGestureKind = .pendingClick
    }

    override func mouseDragged(with event: NSEvent) {
        let pt = localPoint(from: event)
        onHoverViewPoint?(pt, bounds.size)
        guard let start = mouseDownPoint else { return }

        if activeGestureKind == .pendingClick {
            let dist = hypot(pt.x - start.x, pt.y - start.y)
            guard dist >= 4.0 else { return }

            let optionDown = event.modifierFlags.contains(.option)
            let shiftDown = event.modifierFlags.contains(.shift)

            switch toolMode {
            case .pan:
                activeGestureKind = .panning
            case .regionBox:
                activeGestureKind = optionDown ? .panning : .selectingRegion
            case .ruler3D:
                activeGestureKind = viewport.isZoomedOrPanned ? .panning : .selectingRegion
            case .inspect:
                if optionDown {
                    activeGestureKind = .panning
                } else if shiftDown || !viewport.isZoomedOrPanned {
                    activeGestureKind = .selectingRegion
                } else {
                    activeGestureKind = .panning
                }
            }
        }

        switch activeGestureKind {
        case .panning:
            onPanBy?(CGPoint(x: event.deltaX, y: event.deltaY), bounds.size)
        case .selectingRegion:
            onRegionDragUpdate?(start, pt, bounds.size)
        default:
            break
        }
    }

    override func mouseUp(with event: NSEvent) {
        let pt = localPoint(from: event)
        defer {
            mouseDownPoint = nil
            activeGestureKind = .none
        }

        if activeGestureKind == .pendingClick && event.clickCount == 1 {
            if toolMode == .inspect || toolMode == .ruler3D {
                onSingleClickPoint?(pt, bounds.size)
            }
        }
        onHoverViewPoint?(pt, bounds.size)
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(bounds)

        guard let cgImg = image else { return }
        let imgW = cgImg.width
        let imgH = cgImg.height
        let destRect = viewport.displayedImageRect(
            viewSize: bounds.size,
            imageSize: CGSize(width: imgW, height: imgH)
        )
        guard destRect.width > 0, destRect.height > 0 else { return }

        ctx.saveGState()
        ctx.interpolationQuality = crispNearest ? .none : .medium
        // Flip local CGContext vertically around destRect so CGImage renders top-down inside our flipped NSView
        ctx.translateBy(x: destRect.minX, y: destRect.maxY)
        ctx.scaleBy(x: 1.0, y: -1.0)
        ctx.draw(cgImg, in: CGRect(x: 0, y: 0, width: destRect.width, height: destRect.height))
        ctx.restoreGState()

        // Draw 2D Vision Body & Hand Skeleton Overlay (Mac display only — never stored or sent off-device)
        if skeletonOverlay.has2DElements {
            Skeleton2DCanvasDrawer.draw(overlay: skeletonOverlay, in: destRect, ctx: ctx)
        }

        // Draw Region Selection Box if active
        if let box = regionBox, imgW > 0, imgH > 0 {
            let p0 = viewport.displayPixelCenterInView(
                dispX: box.minDispX,
                dispY: box.minDispY,
                viewSize: bounds.size,
                imageWidth: imgW,
                imageHeight: imgH
            )
            let p1 = viewport.displayPixelCenterInView(
                dispX: box.maxDispX,
                dispY: box.maxDispY,
                viewSize: bounds.size,
                imageWidth: imgW,
                imageHeight: imgH
            )
            let pxStepX = destRect.width / CGFloat(imgW)
            let pxStepY = destRect.height / CGFloat(imgH)
            let selRect = CGRect(
                x: p0.x - pxStepX * 0.5,
                y: p0.y - pxStepY * 0.5,
                width: (p1.x - p0.x) + pxStepX,
                height: (p1.y - p0.y) + pxStepY
            )
            ctx.saveGState()
            ctx.setFillColor(NSColor.systemCyan.withAlphaComponent(0.16).cgColor)
            ctx.fill(selRect)
            ctx.setStrokeColor(NSColor.systemCyan.cgColor)
            ctx.setLineWidth(1.5)
            ctx.setLineDash(phase: 0, lengths: [5, 3])
            ctx.stroke(selRect)
            ctx.restoreGState()
        }

        // Draw 3D Ruler endpoints & segment if active
        if let ruler = rulerState, imgW > 0, imgH > 0 {
            let aPt = viewport.displayPixelCenterInView(
                dispX: ruler.pointA.dispX,
                dispY: ruler.pointA.dispY,
                viewSize: bounds.size,
                imageWidth: imgW,
                imageHeight: imgH
            )
            ctx.saveGState()
            if let b = ruler.pointB {
                let bPt = viewport.displayPixelCenterInView(
                    dispX: b.dispX,
                    dispY: b.dispY,
                    viewSize: bounds.size,
                    imageWidth: imgW,
                    imageHeight: imgH
                )
                ctx.setStrokeColor(
                    (ruler.errorMessage == nil ? NSColor.systemYellow : NSColor.systemOrange).cgColor
                )
                ctx.setLineWidth(2.0)
                ctx.move(to: aPt)
                ctx.addLine(to: bPt)
                ctx.strokePath()
                drawEndpointMarker(ctx: ctx, at: bPt, color: .systemYellow)
            }
            drawEndpointMarker(ctx: ctx, at: aPt, color: .systemYellow)
            ctx.restoreGState()
        }
    }

    private func drawEndpointMarker(ctx: CGContext, at pt: CGPoint, color: NSColor) {
        let r: CGFloat = 4.5
        let circle = CGRect(x: pt.x - r, y: pt.y - r, width: r * 2, height: r * 2)
        ctx.setFillColor(color.cgColor)
        ctx.fillEllipse(in: circle)
        ctx.setStrokeColor(NSColor.black.cgColor)
        ctx.setLineWidth(1.2)
        ctx.strokeEllipse(in: circle)
    }
}

struct InteractiveSensorCanvas: NSViewRepresentable {
    let image: CGImage?
    var crispNearest: Bool
    var viewport: ViewportTransform
    var toolMode: CanvasToolMode
    var regionBox: RegionSelectionBox?
    var rulerState: RulerMeasurementState?
    var skeletonOverlay: ProjectedSkeletonOverlay = ProjectedSkeletonOverlay()

    var onHoverViewPoint: (CGPoint?, CGSize) -> Void
    var onZoomAround: (CGPoint, CGSize, CGFloat) -> Void
    var onPanBy: (CGPoint, CGSize) -> Void
    var onResetViewport: () -> Void
    var onRegionDragUpdate: (CGPoint, CGPoint, CGSize) -> Void
    var onSingleClickPoint: (CGPoint, CGSize) -> Void

    func makeNSView(context: Context) -> InteractiveCanvasNSView {
        let v = InteractiveCanvasNSView(frame: .zero)
        bindCallbacks(to: v)
        return v
    }

    func updateNSView(_ nsView: InteractiveCanvasNSView, context: Context) {
        bindCallbacks(to: nsView)
        nsView.crispNearest = crispNearest
        nsView.toolMode = toolMode
        nsView.viewport = viewport
        nsView.regionBox = regionBox
        nsView.rulerState = rulerState
        nsView.skeletonOverlay = skeletonOverlay
        nsView.image = image
    }

    private func bindCallbacks(to v: InteractiveCanvasNSView) {
        v.onHoverViewPoint = onHoverViewPoint
        v.onZoomAround = onZoomAround
        v.onPanBy = onPanBy
        v.onResetViewport = onResetViewport
        v.onRegionDragUpdate = onRegionDragUpdate
        v.onSingleClickPoint = onSingleClickPoint
    }
}
