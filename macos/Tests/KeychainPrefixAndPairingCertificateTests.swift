import Foundation
import XCTest
@testable import KinectViewer

/// The Keychain service names come from the build's KV_BUNDLE_ID_PREFIX (the app's Info.plist key
/// KVBundleIDPrefix), and a pairing request is checked only against the client certificate the TLS handshake proved.
final class KeychainPrefixAndPairingCertificateTests: XCTestCase {
    func testKeychainServiceNamesUseTheBuildsBundleIDPrefix() throws {
        // The test host is KinectViewer.app, built as $(KV_BUNDLE_ID_PREFIX).host with the prefix in its Info.plist.
        let bundleID = try XCTUnwrap(Bundle.main.bundleIdentifier)
        XCTAssertTrue(bundleID.hasSuffix(".host"), bundleID)
        let infoPrefix = try XCTUnwrap(Bundle.main.object(forInfoDictionaryKey: "KVBundleIDPrefix") as? String)
        XCTAssertEqual(infoPrefix + ".host", bundleID, "Info.plist carries the prefix the bundle ID was built from")
        XCTAssertEqual(KeychainServicePrefix.current, infoPrefix)

        XCTAssertEqual(KeychainHostIdentityStore.defaultService, infoPrefix + ".host.pairing")
        XCTAssertEqual(KeychainPairingStore.defaultService, infoPrefix + ".pocket.pairing")
        XCTAssertEqual(KeychainPairingStore.macViewerService, infoPrefix + ".mac.viewer.pairing")
        XCTAssertNotEqual(KeychainPairingStore.defaultService, KeychainPairingStore.macViewerService)
    }

    func testAKeychainPrefixThatIsMissingOrUnexpandedFallsBackToTheExamplePrefix() {
        XCTAssertEqual(KeychainServicePrefix.resolve(infoValue: "org.example.mine"), "org.example.mine")
        XCTAssertEqual(KeychainServicePrefix.resolve(infoValue: " org.example.mine\n"), "org.example.mine")
        for bad in [nil, "", "   ", "$(KV_BUNDLE_ID_PREFIX)", ".org.example", "org.example."] as [String?] {
            XCTAssertEqual(KeychainServicePrefix.resolve(infoValue: bad), "com.example.kinectviewer", String(describing: bad))
        }
        XCTAssertEqual(KeychainServicePrefix.fallback, "com.example.kinectviewer")
    }

    func testAPairingRequestIsCheckedOnlyAgainstTheCertificateTheHandshakeProved() {
        let stated = Data([0x30, 0x82, 0x01, 0x0A, 0x01])
        let other = Data([0x30, 0x82, 0x01, 0x0A, 0x02])
        // The handshake proved the certificate the request names: that one.
        XCTAssertEqual(HostStreamServer.pairingClientCertificate(tlsProven: stated, requestStates: stated), stated)
        // It proved another one: no pairing.
        XCTAssertNil(HostStreamServer.pairingClientCertificate(tlsProven: other, requestStates: stated))
        // It proved none (no TLS metadata, an empty chain): no pairing, never the stated one on its word.
        XCTAssertNil(HostStreamServer.pairingClientCertificate(tlsProven: nil, requestStates: stated))
        XCTAssertNil(HostStreamServer.pairingClientCertificate(tlsProven: Data(), requestStates: stated))
        XCTAssertNil(HostStreamServer.pairingClientCertificate(tlsProven: Data(), requestStates: Data()))
    }
}
