import AppKit
import CoreGraphics
import Foundation

struct DisplayTargetInfo: Identifiable, Equatable, Sendable {
    let displayID: CGDirectDisplayID
    let title: String
    let frame: CGRect
    let isMain: Bool

    var id: CGDirectDisplayID { displayID }
}

private final class CleanSecondDisplayNSView: NSView {
    var image: CGImage? {
        didSet { needsDisplay = true }
    }
    var crispNearest: Bool = true {
        didSet {
            if oldValue != crispNearest { needsDisplay = true }
        }
    }
    var skeletonOverlay: ProjectedSkeletonOverlay = ProjectedSkeletonOverlay() {
        didSet {
            if oldValue != skeletonOverlay { needsDisplay = true }
        }
    }
    var isBlackedOut: Bool = false {
        didSet {
            if oldValue != isBlackedOut { needsDisplay = true }
        }
    }

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(bounds)

        // Instant blackout ('B' key): pure black frame with zero chrome or interface
        guard !isBlackedOut, let cgImg = image, bounds.width > 1, bounds.height > 1 else {
            return
        }

        let imgW = CGFloat(cgImg.width)
        let imgH = CGFloat(cgImg.height)
        guard imgW > 0, imgH > 0 else { return }

        let scale = min(bounds.width / imgW, bounds.height / imgH)
        let drawW = imgW * scale
        let drawH = imgH * scale
        let destRect = CGRect(
            x: (bounds.width - drawW) * 0.5,
            y: (bounds.height - drawH) * 0.5,
            width: drawW,
            height: drawH
        )

        ctx.saveGState()
        ctx.interpolationQuality = crispNearest ? .none : .high
        ctx.translateBy(x: destRect.minX, y: destRect.maxY)
        ctx.scaleBy(x: 1.0, y: -1.0)
        ctx.draw(cgImg, in: CGRect(x: 0, y: 0, width: destRect.width, height: destRect.height))
        ctx.restoreGState()

        if skeletonOverlay.has2DElements {
            Skeleton2DCanvasDrawer.draw(overlay: skeletonOverlay, in: destRect, ctx: ctx)
        }
    }
}

private final class BorderlessProjectorWindow: NSWindow {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

/// Manages a clean, chrome-free full-screen output window on any chosen `NSScreen` (e.g. a projector),
/// while keeping all controls and keyboard focus on the main host window.
@MainActor
final class SecondDisplayOutputController {
    private var outputWindow: BorderlessProjectorWindow?
    private var outputView: CleanSecondDisplayNSView?
    private(set) var activeDisplayID: CGDirectDisplayID?
    private(set) var isBlackedOut: Bool = false

    var isOutputActive: Bool {
        outputWindow != nil && activeDisplayID != nil
    }

    static func listAvailableDisplays() -> [DisplayTargetInfo] {
        let mainID = CGMainDisplayID()
        return NSScreen.screens.compactMap { screen in
            guard let num = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else {
                return nil
            }
            let did = CGDirectDisplayID(num.uint32Value)
            let w = Int(screen.frame.width * screen.backingScaleFactor)
            let h = Int(screen.frame.height * screen.backingScaleFactor)
            let isMain = (did == mainID)
            let suffix = isMain ? " — Main" : ""
            let title = "\(screen.localizedName) (\(w)×\(h))\(suffix)"
            return DisplayTargetInfo(displayID: did, title: title, frame: screen.frame, isMain: isMain)
        }
    }

    private static func findScreen(displayID: CGDirectDisplayID) -> NSScreen? {
        for screen in NSScreen.screens {
            if let num = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
               CGDirectDisplayID(num.uint32Value) == displayID {
                return screen
            }
        }
        return nil
    }

    @discardableResult
    func startOutput(
        on displayID: CGDirectDisplayID,
        initialImage: CGImage?,
        crispNearest: Bool,
        skeletonOverlay: ProjectedSkeletonOverlay,
        blackedOut: Bool
    ) -> Bool {
        guard let screen = Self.findScreen(displayID: displayID) else {
            stopOutput()
            return false
        }

        let view: CleanSecondDisplayNSView
        if let existingWin = outputWindow, let existingView = outputView {
            view = existingView
            existingWin.setFrame(screen.frame, display: true)
        } else {
            let v = CleanSecondDisplayNSView(frame: NSRect(origin: .zero, size: screen.frame.size))
            let win = BorderlessProjectorWindow(
                contentRect: screen.frame,
                styleMask: [.borderless],
                backing: .buffered,
                defer: false
            )
            win.isReleasedWhenClosed = false
            win.backgroundColor = .black
            win.isOpaque = true
            win.hasShadow = false
            win.ignoresMouseEvents = true
            win.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]
            win.contentView = v
            outputWindow = win
            outputView = v
            view = v
        }

        // On an external display/projector, cover menu bar & Dock cleanly; on the main display, keep below floating HUDs
        let isMainScreen = (displayID == CGMainDisplayID())
        outputWindow?.level = isMainScreen ? .floating : NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.mainMenuWindow)) + 1)

        activeDisplayID = displayID
        isBlackedOut = blackedOut
        view.crispNearest = crispNearest
        view.skeletonOverlay = skeletonOverlay
        view.image = initialImage
        view.isBlackedOut = blackedOut
        outputWindow?.setFrame(screen.frame, display: true)
        outputWindow?.orderFrontRegardless()
        return true
    }

    func updateFrame(
        image: CGImage?,
        crispNearest: Bool,
        skeletonOverlay: ProjectedSkeletonOverlay
    ) {
        guard let view = outputView else { return }
        view.crispNearest = crispNearest
        view.skeletonOverlay = skeletonOverlay
        view.image = image
    }

    func setBlackedOut(_ blackedOut: Bool) {
        isBlackedOut = blackedOut
        outputView?.isBlackedOut = blackedOut
        outputView?.displayIfNeeded()
    }

    func handleScreenParametersChanged() {
        guard let activeID = activeDisplayID else { return }
        if let screen = Self.findScreen(displayID: activeID) {
            outputWindow?.setFrame(screen.frame, display: true)
        } else {
            stopOutput()
        }
    }

    func stopOutput() {
        outputWindow?.orderOut(nil)
        outputWindow?.close()
        outputWindow = nil
        outputView = nil
        activeDisplayID = nil
    }
}
