import AppKit
import Foundation
import IOKit
import IOKit.usb

/// Observes macOS system sleep/wake (`NSWorkspace`) and Kinect v2 USB 3.0 hotplug (`0x045e:0x02c4` via `IONotificationPort`).
final class HardwareLifecycleMonitor: @unchecked Sendable {
    var onUsbAttached: (() -> Void)?
    var onUsbDetached: (() -> Void)?
    var onSystemWillSleep: (() -> Void)?
    var onSystemDidWake: (() -> Void)?

    private var notifyPort: IONotificationPortRef?
    private var addedIterator: io_iterator_t = 0
    private var removedIterator: io_iterator_t = 0
    private var workspaceObservers: [NSObjectProtocol] = []

    init() {
        setupWorkspaceSleepWakeObservers()
        setupUSBHotplugNotifications()
    }

    deinit {
        stop()
    }

    func stop() {
        let nc = NSWorkspace.shared.notificationCenter
        for obs in workspaceObservers {
            nc.removeObserver(obs)
        }
        workspaceObservers.removeAll()

        if addedIterator != 0 {
            IOObjectRelease(addedIterator)
            addedIterator = 0
        }
        if removedIterator != 0 {
            IOObjectRelease(removedIterator)
            removedIterator = 0
        }
        if let port = notifyPort {
            IONotificationPortDestroy(port)
            notifyPort = nil
        }
    }

    private func setupWorkspaceSleepWakeObservers() {
        let nc = NSWorkspace.shared.notificationCenter
        workspaceObservers.append(
            nc.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
                self?.onSystemWillSleep?()
            }
        )
        workspaceObservers.append(
            nc.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
                self?.onSystemDidWake?()
            }
        )
    }

    private func setupUSBHotplugNotifications() {
        guard let port = IONotificationPortCreate(kIOMainPortDefault) else { return }
        self.notifyPort = port
        if let runLoopSource = IONotificationPortGetRunLoopSource(port)?.takeUnretainedValue() {
            CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, .commonModes)
        }

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()

        if let matchAdd = Self.makeKinectMatchingDict() {
            let kr = IOServiceAddMatchingNotification(
                port,
                kIOFirstMatchNotification,
                matchAdd,
                Self.usbAddedCallback,
                selfPtr,
                &addedIterator
            )
            if kr == KERN_SUCCESS {
                // Drain initial iterator to arm future hotplug notifications
                Self.drainIterator(addedIterator)
            }
        }

        if let matchRem = Self.makeKinectMatchingDict() {
            let kr = IOServiceAddMatchingNotification(
                port,
                kIOTerminatedNotification,
                matchRem,
                Self.usbRemovedCallback,
                selfPtr,
                &removedIterator
            )
            if kr == KERN_SUCCESS {
                Self.drainIterator(removedIterator)
            }
        }
    }

    private static func makeKinectMatchingDict() -> CFMutableDictionary? {
        guard let dict = IOServiceMatching(kIOUSBDeviceClassName) as NSMutableDictionary? else {
            return nil
        }
        dict[kUSBVendorID as String] = NSNumber(value: 0x045e)
        dict[kUSBProductID as String] = NSNumber(value: 0x02c4)
        return dict
    }

    @discardableResult
    private static func drainIterator(_ iterator: io_iterator_t) -> Int {
        var count = 0
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            IOObjectRelease(service)
            count += 1
        }
        return count
    }

    private static let usbAddedCallback: IOServiceMatchingCallback = { refcon, iterator in
        let drained = HardwareLifecycleMonitor.drainIterator(iterator)
        guard drained > 0, let refcon = refcon else { return }
        let monitor = Unmanaged<HardwareLifecycleMonitor>.fromOpaque(refcon).takeUnretainedValue()
        monitor.onUsbAttached?()
    }

    private static let usbRemovedCallback: IOServiceMatchingCallback = { refcon, iterator in
        let drained = HardwareLifecycleMonitor.drainIterator(iterator)
        guard drained > 0, let refcon = refcon else { return }
        let monitor = Unmanaged<HardwareLifecycleMonitor>.fromOpaque(refcon).takeUnretainedValue()
        monitor.onUsbDetached?()
    }
}
