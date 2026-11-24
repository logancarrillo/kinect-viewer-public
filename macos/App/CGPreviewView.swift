import AppKit
import CoreGraphics
import SwiftUI

final class LayerImageNSView: NSView {
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect
        layer?.magnificationFilter = .nearest
        layer?.minificationFilter = .trilinear
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func updateImage(_ image: CGImage?, crispNearest: Bool) {
        layer?.magnificationFilter = crispNearest ? .nearest : .linear
        layer?.contents = image
    }
}

struct CGPreviewView: NSViewRepresentable {
    let image: CGImage?
    var crispNearest: Bool = true

    func makeNSView(context: Context) -> LayerImageNSView {
        LayerImageNSView(frame: .zero)
    }

    func updateNSView(_ nsView: LayerImageNSView, context: Context) {
        nsView.updateImage(image, crispNearest: crispNearest)
    }
}
