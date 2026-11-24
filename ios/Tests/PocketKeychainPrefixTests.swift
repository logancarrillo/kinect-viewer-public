import Foundation
import XCTest
@testable import KinectPocketViewer

/// The phone keeps its pairing keys under the build's KV_BUNDLE_ID_PREFIX (Info.plist key KVBundleIDPrefix), so a
/// build with the prefix an install already used finds the keys it paired with.
final class PocketKeychainPrefixTests: XCTestCase {
    func testThePhonesKeychainServiceUsesTheBuildsBundleIDPrefix() throws {
        // The test host is KinectPocketViewer.app, built as $(KV_BUNDLE_ID_PREFIX).pocket.
        let bundleID = try XCTUnwrap(Bundle.main.bundleIdentifier)
        XCTAssertTrue(bundleID.hasSuffix(".pocket"), bundleID)
        let infoPrefix = try XCTUnwrap(Bundle.main.object(forInfoDictionaryKey: "KVBundleIDPrefix") as? String)
        XCTAssertEqual(infoPrefix + ".pocket", bundleID, "Info.plist carries the prefix the bundle ID was built from")
        XCTAssertEqual(KeychainServicePrefix.current, infoPrefix)
        XCTAssertEqual(KeychainPairingStore.defaultService, infoPrefix + ".pocket.pairing")
    }
}
