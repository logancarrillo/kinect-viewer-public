import AVFoundation
import CoreGraphics
import CoreMedia
import Foundation

/// Thread-safe handle to the underlying hardware-accelerated AVSampleBufferVideoRenderer.
/// Enforces zero disk caching and hardware compositor screenshot/recording protection (`preventsCapture = true`).
final class SampleBufferDisplayBridge: @unchecked Sendable {
    private let lock = NSLock()
    private weak var renderer: AVSampleBufferVideoRenderer?
    private var pausedInMemory: Bool = false
    private var suppressedForSnapshot: Bool = false
    private var framesHandedToRenderer: UInt64 = 0

    func attach(layer: AVSampleBufferDisplayLayer) {
        layer.preventsCapture = true
        layer.videoGravity = .resizeAspect
        layer.backgroundColor = CGColor(gray: 0, alpha: 1)
        lock.lock()
        self.renderer = layer.sampleBufferRenderer
        lock.unlock()
    }

    func setPausedInMemory(_ paused: Bool) {
        lock.lock()
        pausedInMemory = paused
        lock.unlock()
    }

    var isPausedInMemory: Bool {
        lock.lock()
        defer { lock.unlock() }
        return pausedInMemory
    }

    /// While covered (app inactive or screen captured) frames are dropped, so nothing new is drawn under the cover.
    func setSuppressedForSnapshot(_ suppressed: Bool) {
        lock.lock()
        suppressedForSnapshot = suppressed
        lock.unlock()
    }

    /// How many frames passed the pause and cover checks on their way to the renderer (for tests).
    var framesHandedToRendererCount: UInt64 {
        lock.lock()
        defer { lock.unlock() }
        return framesHandedToRenderer
    }

    func enqueue(_ sampleBuffer: CMSampleBuffer) {
        lock.lock()
        let r = renderer
        let paused = pausedInMemory
        let suppressed = suppressedForSnapshot
        if !paused && !suppressed {
            framesHandedToRenderer &+= 1
        }
        lock.unlock()
        guard let r = r, !paused, !suppressed else { return }

        if r.status == .failed || r.requiresFlushToResumeDecoding {
            r.flush()
        }
        if r.isReadyForMoreMediaData {
            r.enqueue(sampleBuffer)
        }
    }

    func flushAndClearImage() {
        lock.lock()
        pausedInMemory = false
        let r = renderer
        lock.unlock()
        r?.flush(removingDisplayedImage: true, completionHandler: nil)
    }
}
