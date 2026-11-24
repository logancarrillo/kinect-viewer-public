import SwiftUI

@main
struct KinectPocketApp: App {
    @StateObject private var viewModel = PocketViewModel()

    init() {
        URLCache.shared = URLCache(memoryCapacity: 0, diskCapacity: 0, diskPath: nil)
    }

    var body: some Scene {
        WindowGroup {
            ContentView(viewModel: viewModel)
        }
    }
}
