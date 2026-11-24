import AppKit
import SwiftUI

enum MacApplicationMode: String, CaseIterable, Identifiable {
    case localHost = "localHost"
    case watchCamera = "watchCamera"

    var id: String { rawValue }

    var label: String {
        switch self {
        case .localHost:
            return "This Mac's Kinect"
        case .watchCamera:
            return "Watch a Camera"
        }
    }
}

/// Root window container attaching local Host modals (`showPairingSheet`, `showResetPairingConfirmation`,
/// `showRecordingReminderDialog`) above the mode switch so a paired v2 iPhone's recording-reminder or
/// pairing request always surfaces even while the Mac window is in "Watch a Camera" mode.
struct HostRootWindowView: View {
    @ObservedObject var controller: HostController
    @ObservedObject var remoteViewer: MacCameraViewerController
    @Binding var selectedMode: MacApplicationMode

    var localHostStatusBannerInWatchMode: String? {
        if controller.showRecordingReminderDialog {
            return "Your iPhone requested a Depth/IR recording on this Mac."
        }
        if controller.isRecording {
            return "This Mac's Kinect is currently recording Depth/IR."
        }
        if controller.isPhoneConnected {
            return "Your iPhone is connected to this Mac's Kinect."
        }
        return nil
    }

    var body: some View {
        Group {
            switch selectedMode {
            case .localHost:
                MainContentView(controller: controller)
            case .watchCamera:
                MacCameraViewerView(
                    viewer: remoteViewer,
                    localHostStatusBanner: localHostStatusBannerInWatchMode,
                    onSwitchToLocalHost: {
                        selectedMode = .localHost
                    }
                )
            }
        }
        .toolbar {
            ToolbarItem(placement: .principal) {
                Picker("Mode", selection: $selectedMode) {
                    ForEach(MacApplicationMode.allCases) { mode in
                        Text(mode.label).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .frame(width: 280)
                .help("Switch between hosting this Mac's Kinect and watching a remote PC camera")
            }
        }
        .onChange(of: selectedMode) { _, newValue in
            if newValue == .watchCamera {
                remoteViewer.activate()
            } else {
                remoteViewer.deactivate()
            }
        }
        .sheet(isPresented: $controller.showPairingSheet) {
            HostPairingModalView(controller: controller)
        }
        .confirmationDialog(
            "Reset iPhone pairing?",
            isPresented: $controller.showResetPairingConfirmation,
            titleVisibility: .visible
        ) {
            Button("Reset pairing (your iPhone will need to pair again)", role: .destructive) {
                controller.confirmResetPairing()
            }
            Button("Cancel", role: .cancel) {
                controller.cancelResetPairing()
            }
        } message: {
            Text("The existing pairing.json will be saved as pairing.json.bad, a new pairing key will be created in the Keychain, and your iPhone will need to pair again.")
        }
        .sheet(isPresented: $controller.showRecordingReminderDialog) {
            PreRecordingReminderSheetView(controller: controller)
        }
    }
}

final class AppLifecycleDelegate: NSObject, NSApplicationDelegate {
    var onTerminate: (() -> Void)?
    private var signalSources: [DispatchSourceSignal] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        for sig in [SIGTERM, SIGINT] {
            signal(sig, SIG_IGN)
            let src = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            src.setEventHandler { [weak self] in
                self?.onTerminate?()
                self?.onTerminate = nil
                exit(0)
            }
            src.resume()
            signalSources.append(src)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        // Ignore hidden offscreen 1x1 GLFW context windows destroyed during pipeline switches;
        // terminate only when no visible main viewer window remains.
        let hasVisibleViewerWindow = sender.windows.contains { $0.isVisible && !$0.title.isEmpty }
        return !hasVisibleViewerWindow
    }

    func applicationWillTerminate(_ notification: Notification) {
        onTerminate?()
        onTerminate = nil
    }
}

@main
struct KinectHostApp: App {
    @NSApplicationDelegateAdaptor(AppLifecycleDelegate.self) private var appDelegate
    @StateObject private var controller = HostController()
    @StateObject private var remoteViewer = MacCameraViewerController()
    @State private var selectedMode: MacApplicationMode = .localHost

    private static var isRunningXCTest: Bool {
        ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
    }

    var body: some Scene {
        WindowGroup("Kinect Viewer") {
            if Self.isRunningXCTest {
                EmptyView()
            } else {
                HostRootWindowView(
                    controller: controller,
                    remoteViewer: remoteViewer,
                    selectedMode: $selectedMode
                )
                .onAppear {
                    let ctrl = controller
                    let viewer = remoteViewer
                    appDelegate.onTerminate = {
                        MainActor.assumeIsolated {
                            viewer.shutdown()
                            ctrl.shutdown()
                        }
                    }
                }
            }
        }
        .defaultSize(width: 980, height: 580)
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About Kinect Viewer") {
                    var options: [NSApplication.AboutPanelOptionKey: Any] = [:]
                    if let mark = NSImage(named: "LogoMark") {
                        mark.size = NSSize(width: 64, height: 64)
                        options[.applicationIcon] = mark
                    }
                    NSApplication.shared.orderFrontStandardAboutPanel(options: options)
                }
            }
            CommandGroup(after: .toolbar) {
                Divider()
                Button("This Mac's Kinect") {
                    selectedMode = .localHost
                }
                Button("Watch a Camera") {
                    selectedMode = .watchCamera
                }
            }
        }
    }
}
