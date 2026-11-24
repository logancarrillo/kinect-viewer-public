import AVFoundation
import CoreMedia
import SwiftUI
import UIKit

final class SampleBufferVideoLayerView: UIView {
    override class var layerClass: AnyClass {
        AVSampleBufferDisplayLayer.self
    }

    var sampleDisplayLayer: AVSampleBufferDisplayLayer {
        layer as! AVSampleBufferDisplayLayer
    }
}

final class InteractiveVideoViewportUIView: UIView, UIGestureRecognizerDelegate {
    let videoView = SampleBufferVideoLayerView()

    var is3DOrbitMode: Bool = false
    var current2DZoom: CGFloat = 1.0
    var current2DPan: CGSize = .zero

    var onSingleTap: (() -> Void)?
    var onDoubleTap: (() -> Void)?
    var onUpdate2DTransform: ((CGFloat, CGSize) -> Void)?
    var onOrbit3D: ((Float, Float) -> Void)?
    var onZoom3D: ((Float) -> Void)?
    var onPan3D: ((Float, Float) -> Void)?

    private var pinchStartZoom: CGFloat = 1.0
    private var lastPinchScale: CGFloat = 1.0
    private var lastOneFingerPt: CGPoint = .zero
    private var lastTwoFingerPt: CGPoint = .zero

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isOpaque = true
        clipsToBounds = true

        videoView.backgroundColor = .black
        videoView.isOpaque = true
        addSubview(videoView)

        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(handleDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2

        let singleTap = UITapGestureRecognizer(target: self, action: #selector(handleSingleTap(_:)))
        singleTap.numberOfTapsRequired = 1
        singleTap.require(toFail: doubleTap)

        let pinch = UIPinchGestureRecognizer(target: self, action: #selector(handlePinch(_:)))
        pinch.delegate = self

        let oneFingerPan = UIPanGestureRecognizer(target: self, action: #selector(handleOneFingerPan(_:)))
        oneFingerPan.minimumNumberOfTouches = 1
        oneFingerPan.maximumNumberOfTouches = 1
        oneFingerPan.delegate = self

        let twoFingerPan = UIPanGestureRecognizer(target: self, action: #selector(handleTwoFingerPan(_:)))
        twoFingerPan.minimumNumberOfTouches = 2
        twoFingerPan.maximumNumberOfTouches = 2
        twoFingerPan.delegate = self

        addGestureRecognizer(doubleTap)
        addGestureRecognizer(singleTap)
        addGestureRecognizer(pinch)
        addGestureRecognizer(oneFingerPan)
        addGestureRecognizer(twoFingerPan)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is unused")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        videoView.bounds = bounds
        videoView.center = CGPoint(x: bounds.midX, y: bounds.midY)
        apply2DTransform()
    }

    func apply2DTransform() {
        let clampedScale = min(8.0, max(1.0, current2DZoom))
        let clampedPan = Self.clampPan(current2DPan, scale: clampedScale, in: bounds.size)
        let scaled = CGAffineTransform(scaleX: clampedScale, y: clampedScale)
        videoView.transform = scaled.translatedBy(
            x: clampedPan.width / clampedScale,
            y: clampedPan.height / clampedScale
        )
    }

    static func clampPan(_ pan: CGSize, scale: CGFloat, in size: CGSize) -> CGSize {
        guard scale > 1.001, size.width > 1, size.height > 1 else {
            return .zero
        }
        let maxX = (size.width * (scale - 1.0)) * 0.5
        let maxY = (size.height * (scale - 1.0)) * 0.5
        return CGSize(
            width: min(maxX, max(-maxX, pan.width)),
            height: min(maxY, max(-maxY, pan.height))
        )
    }

    func gestureRecognizer(
        _ gestureRecognizer: UIGestureRecognizer,
        shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer
    ) -> Bool {
        if gestureRecognizer is UIPinchGestureRecognizer && otherGestureRecognizer is UIPanGestureRecognizer {
            return true
        }
        if gestureRecognizer is UIPanGestureRecognizer && otherGestureRecognizer is UIPinchGestureRecognizer {
            return true
        }
        return false
    }

    @objc private func handleSingleTap(_ gr: UITapGestureRecognizer) {
        guard gr.state == .ended else { return }
        onSingleTap?()
    }

    @objc private func handleDoubleTap(_ gr: UITapGestureRecognizer) {
        guard gr.state == .ended else { return }
        onDoubleTap?()
    }

    @objc private func handlePinch(_ gr: UIPinchGestureRecognizer) {
        switch gr.state {
        case .began:
            pinchStartZoom = current2DZoom
            lastPinchScale = 1.0
        case .changed:
            if is3DOrbitMode {
                let step = max(0.5, min(2.0, gr.scale / max(0.01, lastPinchScale)))
                lastPinchScale = gr.scale
                // Pinching apart (step > 1) moves camera closer (distance factor < 1)
                onZoom3D?(Float(1.0 / step))
            } else {
                let nextScale = min(8.0, max(1.0, pinchStartZoom * gr.scale))
                let nextPan = nextScale <= 1.01 ? .zero : Self.clampPan(current2DPan, scale: nextScale, in: bounds.size)
                current2DZoom = nextScale
                current2DPan = nextPan
                apply2DTransform()
                onUpdate2DTransform?(nextScale, nextPan)
            }
        default:
            break
        }
    }

    @objc private func handleOneFingerPan(_ gr: UIPanGestureRecognizer) {
        let pt = gr.translation(in: self)
        switch gr.state {
        case .began:
            lastOneFingerPt = pt
        case .changed:
            let dx = pt.x - lastOneFingerPt.x
            let dy = pt.y - lastOneFingerPt.y
            lastOneFingerPt = pt
            if is3DOrbitMode {
                let yaw = Float(-dx) * 0.0075
                let pitch = Float(dy) * 0.0075
                onOrbit3D?(yaw, pitch)
            } else if current2DZoom > 1.01 {
                let nextPan = Self.clampPan(
                    CGSize(width: current2DPan.width + dx, height: current2DPan.height + dy),
                    scale: current2DZoom,
                    in: bounds.size
                )
                current2DPan = nextPan
                apply2DTransform()
                onUpdate2DTransform?(current2DZoom, nextPan)
            }
        default:
            break
        }
    }

    @objc private func handleTwoFingerPan(_ gr: UIPanGestureRecognizer) {
        let pt = gr.translation(in: self)
        switch gr.state {
        case .began:
            lastTwoFingerPt = pt
        case .changed:
            let dx = pt.x - lastTwoFingerPt.x
            let dy = pt.y - lastTwoFingerPt.y
            lastTwoFingerPt = pt
            if is3DOrbitMode {
                let panRight = Float(-dx) * 0.0035
                let panUp = Float(dy) * 0.0035
                onPan3D?(panRight, panUp)
            } else if current2DZoom > 1.01 {
                let nextPan = Self.clampPan(
                    CGSize(width: current2DPan.width + dx, height: current2DPan.height + dy),
                    scale: current2DZoom,
                    in: bounds.size
                )
                current2DPan = nextPan
                apply2DTransform()
                onUpdate2DTransform?(current2DZoom, nextPan)
            }
        default:
            break
        }
    }
}

struct VideoSampleDisplayView: UIViewRepresentable {
    let bridge: SampleBufferDisplayBridge
    let zoomScale: CGFloat
    let panOffset: CGSize
    let is3DOrbitMode: Bool
    let onSingleTap: () -> Void
    let onDoubleTap: () -> Void
    let onUpdate2DTransform: (CGFloat, CGSize) -> Void
    let onOrbit3D: (Float, Float) -> Void
    let onZoom3D: (Float) -> Void
    let onPan3D: (Float, Float) -> Void

    func makeUIView(context: Context) -> InteractiveVideoViewportUIView {
        let container = InteractiveVideoViewportUIView()
        bridge.attach(layer: container.videoView.sampleDisplayLayer)
        updateCallbacks(on: container)
        return container
    }

    func updateUIView(_ uiView: InteractiveVideoViewportUIView, context: Context) {
        uiView.videoView.sampleDisplayLayer.preventsCapture = true
        uiView.is3DOrbitMode = is3DOrbitMode
        if abs(uiView.current2DZoom - zoomScale) > 0.0005 || uiView.current2DPan != panOffset {
            uiView.current2DZoom = zoomScale
            uiView.current2DPan = panOffset
            uiView.apply2DTransform()
        }
        updateCallbacks(on: uiView)
    }

    private func updateCallbacks(on uiView: InteractiveVideoViewportUIView) {
        uiView.onSingleTap = onSingleTap
        uiView.onDoubleTap = onDoubleTap
        uiView.onUpdate2DTransform = onUpdate2DTransform
        uiView.onOrbit3D = onOrbit3D
        uiView.onZoom3D = onZoom3D
        uiView.onPan3D = onPan3D
    }
}
