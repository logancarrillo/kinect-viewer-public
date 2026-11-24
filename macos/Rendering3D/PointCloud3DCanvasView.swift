import AppKit
import CoreGraphics
import SwiftUI

struct PointCloud3DCanvasView: NSViewRepresentable {
    let image: CGImage
    var onOrbit: (Float, Float) -> Void
    var onZoom: (Float) -> Void
    var onPan: (Float, Float) -> Void
    var onResetCamera: () -> Void

    func makeNSView(context: Context) -> PointCloud3DNSView {
        let view = PointCloud3DNSView()
        updateCallbacks(on: view)
        view.image = image
        return view
    }

    func updateNSView(_ nsView: PointCloud3DNSView, context: Context) {
        updateCallbacks(on: nsView)
        nsView.image = image
        nsView.needsDisplay = true
    }

    private func updateCallbacks(on view: PointCloud3DNSView) {
        view.onOrbit = onOrbit
        view.onZoom = onZoom
        view.onPan = onPan
        view.onResetCamera = onResetCamera
    }
}

final class PointCloud3DNSView: NSView {
    var image: CGImage?
    var onOrbit: ((Float, Float) -> Void)?
    var onZoom: ((Float) -> Void)?
    var onPan: ((Float, Float) -> Void)?
    var onResetCamera: (() -> Void)?

    private var lastDragPt: CGPoint = .zero
    private var isShiftPanDrag: Bool = false

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { false }

    private func viewPoint(from event: NSEvent) -> CGPoint {
        convert(event.locationInWindow, from: nil)
    }

    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 2 {
            onResetCamera?()
            return
        }
        lastDragPt = viewPoint(from: event)
        isShiftPanDrag = event.modifierFlags.contains(.shift) || event.modifierFlags.contains(.option)
    }

    override func mouseDragged(with event: NSEvent) {
        let cur = viewPoint(from: event)
        let dx = Float(cur.x - lastDragPt.x)
        let dy = Float(cur.y - lastDragPt.y)
        lastDragPt = cur

        if isShiftPanDrag || event.modifierFlags.contains(.shift) || event.modifierFlags.contains(.option) {
            // Drag right moves scene right (camera target moves left: -dx); drag down moves scene down (target moves up: +dy)
            let panScale: Float = 0.0032
            onPan?(-dx * panScale, dy * panScale)
        } else {
            // Orbit around scene center
            let orbitScale: Float = 0.0065
            onOrbit?(-dx * orbitScale, dy * orbitScale)
        }
    }

    override func rightMouseDown(with event: NSEvent) {
        lastDragPt = viewPoint(from: event)
    }

    override func rightMouseDragged(with event: NSEvent) {
        let cur = viewPoint(from: event)
        let dx = Float(cur.x - lastDragPt.x)
        let dy = Float(cur.y - lastDragPt.y)
        lastDragPt = cur
        let panScale: Float = 0.0032
        onPan?(-dx * panScale, dy * panScale)
    }

    override func otherMouseDown(with event: NSEvent) {
        lastDragPt = viewPoint(from: event)
    }

    override func otherMouseDragged(with event: NSEvent) {
        let cur = viewPoint(from: event)
        let dx = Float(cur.x - lastDragPt.x)
        let dy = Float(cur.y - lastDragPt.y)
        lastDragPt = cur
        let panScale: Float = 0.0032
        onPan?(-dx * panScale, dy * panScale)
    }

    override func scrollWheel(with event: NSEvent) {
        let dy = event.scrollingDeltaY
        guard abs(dy) > 0.001 else { return }
        let sensitivity: CGFloat = event.hasPreciseScrollingDeltas ? 0.006 : 0.08
        let factor = Float(exp(dy * sensitivity))
        onZoom?(factor)
    }

    override func magnify(with event: NSEvent) {
        let factor = Float(max(0.25, 1.0 + event.magnification))
        onZoom?(factor)
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(bounds)

        guard let cgImg = image, bounds.width > 1, bounds.height > 1 else { return }
        let imgW = CGFloat(cgImg.width)
        let imgH = CGFloat(cgImg.height)
        let scale = min(bounds.width / imgW, bounds.height / imgH)
        let drawW = imgW * scale
        let drawH = imgH * scale
        let drawRect = CGRect(
            x: (bounds.width - drawW) * 0.5,
            y: (bounds.height - drawH) * 0.5,
            width: drawW,
            height: drawH
        )

        ctx.saveGState()
        ctx.interpolationQuality = .high
        ctx.translateBy(x: drawRect.minX, y: drawRect.maxY)
        ctx.scaleBy(x: 1.0, y: -1.0)
        ctx.draw(cgImg, in: CGRect(x: 0, y: 0, width: drawRect.width, height: drawRect.height))
        ctx.restoreGState()
    }
}
