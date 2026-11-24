import AppKit
import CoreGraphics
import CryptoKit
import Foundation
import ImageIO
import XCTest
@testable import KinectViewer

@MainActor
final class Phase6IconAssetsTests: XCTestCase {
    private var repoRootURL: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // macos
            .deletingLastPathComponent() // repo root
    }

    private func decodeSRGBPixels(at url: URL) throws -> (width: Int, height: Int, bytes: [UInt8]) {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
            throw NSError(domain: "Phase6IconAssetsTests", code: 1, userInfo: [NSLocalizedDescriptionKey: "Failed to decode \(url.lastPathComponent)"])
        }
        let w = img.width
        let h = img.height
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        let cs = CGColorSpace(name: CGColorSpace.sRGB)!
        guard let ctx = CGContext(
            data: &bytes,
            width: w,
            height: h,
            bitsPerComponent: 8,
            bytesPerRow: w * 4,
            space: cs,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        ) else {
            throw NSError(domain: "Phase6IconAssetsTests", code: 2, userInfo: [NSLocalizedDescriptionKey: "Failed to create bitmap context"])
        }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        return (w, h, bytes)
    }

    private func pixel(_ bitmap: (width: Int, height: Int, bytes: [UInt8]), x: Int, y: Int) -> (r: Int, g: Int, b: Int, a: Int) {
        let idx = (y * bitmap.width + x) * 4
        return (
            Int(bitmap.bytes[idx]),
            Int(bitmap.bytes[idx + 1]),
            Int(bitmap.bytes[idx + 2]),
            Int(bitmap.bytes[idx + 3])
        )
    }

    func testMasterSVGExistsIsUnchangedAndContainsNoForbiddenBrandTerms() throws {
        let svgURL = repoRootURL.appendingPathComponent("design/logo.svg")
        XCTAssertTrue(FileManager.default.fileExists(atPath: svgURL.path))

        let svgData = try Data(contentsOf: svgURL)
        let digestHex = SHA256.hash(data: svgData).map { String(format: "%02x", $0) }.joined()
        XCTAssertEqual(
            digestHex,
            "f4d63ba19e509a77cddcfe32991cf05e1a5ef0c0c4950b9299550bf9d6692161",
            "design/logo.svg is the authoritative source and must stay byte-for-byte unchanged"
        )

        let svgText = try XCTUnwrap(String(data: svgData, encoding: .utf8))
        XCTAssertTrue(svgText.contains("<svg"))
        XCTAssertTrue(svgText.contains("viewBox=\"0 0 1024 1024\""))

        let lowered = svgText.lowercased()
        XCTAssertFalse(lowered.contains("kinect"), "Art must not contain the word 'Kinect'")
        XCTAssertFalse(lowered.contains("microsoft"), "Art must not contain 'Microsoft'")
        XCTAssertFalse(lowered.contains("xbox"), "Art must not contain 'Xbox'")
    }

    func testIOSAppIconsAre1024OpaqueWithoutAlphaChannel() throws {
        let iosIconDir = repoRootURL.appendingPathComponent("ios/Assets.xcassets/AppIcon.appiconset")
        let files = [
            "AppIcon-iOS-Light-1024.png",
            "AppIcon-iOS-Dark-1024.png",
            "AppIcon-iOS-Tinted-1024.png",
        ]

        for name in files {
            let url = iosIconDir.appendingPathComponent(name)
            XCTAssertTrue(FileManager.default.fileExists(atPath: url.path), "Missing \(name)")

            guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
                  let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any],
                  let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
                XCTFail("Could not decode \(name)")
                continue
            }

            XCTAssertEqual(img.width, 1024, "\(name) width must be 1024")
            XCTAssertEqual(img.height, 1024, "\(name) height must be 1024")

            let hasAlpha = (props[kCGImagePropertyHasAlpha] as? Bool) ?? false
            XCTAssertFalse(hasAlpha, "\(name) must not contain an alpha channel")
            XCTAssertTrue(
                [CGImageAlphaInfo.none, .noneSkipLast, .noneSkipFirst].contains(img.alphaInfo),
                "\(name) CGImageAlphaInfo must be opaque RGB, got \(img.alphaInfo.rawValue)"
            )

            // Verify PNG IHDR color-type byte (offset 25 == 2 means 24-bit RGB without alpha)
            let rawPNG = try Data(contentsOf: url)
            XCTAssertGreaterThan(rawPNG.count, 30)
            XCTAssertEqual(rawPNG[25], 2, "\(name) PNG IHDR color type must be 2 (RGB, no alpha)")

            // Confirm corner pixel (0, 0) is opaque (no baked rounded-corner transparency)
            let bmp = try decodeSRGBPixels(at: url)
            let topLeft = pixel(bmp, x: 0, y: 0)
            XCTAssertEqual(topLeft.a, 255, "\(name) top-left corner must be opaque (no baked corners)")
        }

        // Verify Tinted variant is pure greyscale luminance
        let tintedBmp = try decodeSRGBPixels(at: iosIconDir.appendingPathComponent("AppIcon-iOS-Tinted-1024.png"))
        for sample in [(512, 265), (512, 480), (512, 640), (512, 800), (100, 500)] {
            let px = pixel(tintedBmp, x: sample.0, y: sample.1)
            XCTAssertLessThanOrEqual(abs(px.r - px.g), 1, "Tinted icon must be greyscale at \(sample)")
            XCTAssertLessThanOrEqual(abs(px.g - px.b), 1, "Tinted icon must be greyscale at \(sample)")
        }
    }

    func testMacAppIconsCoverAllSizes16Through1024AndContactSheetExists() throws {
        let macIconDir = repoRootURL.appendingPathComponent("macos/Assets.xcassets/AppIcon.appiconset")
        let expected: [(String, Int)] = [
            ("icon_16x16.png", 16),
            ("icon_16x16@2x.png", 32),
            ("icon_32x32.png", 32),
            ("icon_32x32@2x.png", 64),
            ("icon_128x128.png", 128),
            ("icon_128x128@2x.png", 256),
            ("icon_256x256.png", 256),
            ("icon_256x256@2x.png", 512),
            ("icon_512x512.png", 512),
            ("icon_512x512@2x.png", 1024),
        ]

        for (file, size) in expected {
            let url = macIconDir.appendingPathComponent(file)
            XCTAssertTrue(FileManager.default.fileExists(atPath: url.path), "Missing \(file)")
            guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
                  let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
                XCTFail("Could not decode \(file)")
                continue
            }
            XCTAssertEqual(img.width, size, "\(file) width")
            XCTAssertEqual(img.height, size, "\(file) height")
        }

        // Verify contact sheet & bundled LogoMark
        let sheetURL = repoRootURL.appendingPathComponent("design/icon-sheet.png")
        XCTAssertTrue(FileManager.default.fileExists(atPath: sheetURL.path))
        XCTAssertNotNil(CGImageSourceCreateWithURL(sheetURL as CFURL, nil))
        XCTAssertNotNil(NSImage(named: "LogoMark"), "Bundled LogoMark asset should load in KinectHostApp")
    }

    func testMac1024IconSensorAndBeamPixels() throws {
        let mac1024URL = repoRootURL.appendingPathComponent("macos/Assets.xcassets/AppIcon.appiconset/icon_512x512@2x.png")
        let bmp = try decodeSRGBPixels(at: mac1024URL)
        XCTAssertEqual(bmp.width, 1024)
        XCTAssertEqual(bmp.height, 1024)

        // 1. Sensor bar is light (#E9EEF7)
        let bar = pixel(bmp, x: 680, y: 301)
        XCTAssertEqual(bar.a, 255)
        XCTAssertGreaterThan(bar.r, 215, "Sensor bar should be light (#E9EEF7), got \(bar)")
        XCTAssertGreaterThan(bar.g, 220, "Sensor bar should be light (#E9EEF7), got \(bar)")
        XCTAssertGreaterThan(bar.b, 230, "Sensor bar should be light (#E9EEF7), got \(bar)")

        // 2. Lens centre is teal (#22B8CF)
        let lensCentre = pixel(bmp, x: 512, y: 301)
        XCTAssertEqual(lensCentre.a, 255)
        XCTAssertLessThan(lensCentre.r, 65, "Lens centre R should be low teal (#22B8CF), got \(lensCentre)")
        XCTAssertGreaterThan(lensCentre.g, 160, "Lens centre G should be teal (#22B8CF), got \(lensCentre)")
        XCTAssertGreaterThan(lensCentre.b, 185, "Lens centre B should be teal (#22B8CF), got \(lensCentre)")

        // 3. Near band is amber (#FFB238)
        let nearBand = pixel(bmp, x: 512, y: 490)
        XCTAssertEqual(nearBand.a, 255)
        XCTAssertGreaterThan(nearBand.r, 230, "Near band R should be amber (#FFB238), got \(nearBand)")
        XCTAssertTrue((155...200).contains(nearBand.g), "Near band G should be amber (#FFB238), got \(nearBand)")
        XCTAssertLessThan(nearBand.b, 85, "Near band B should be amber (#FFB238), got \(nearBand)")

        // 4. Far band is blue (#3D63DD)
        let farBand = pixel(bmp, x: 512, y: 780)
        XCTAssertEqual(farBand.a, 255)
        XCTAssertTrue((40...85).contains(farBand.r), "Far band R should be blue (#3D63DD), got \(farBand)")
        XCTAssertTrue((80...125).contains(farBand.g), "Far band G should be blue (#3D63DD), got \(farBand)")
        XCTAssertGreaterThan(farBand.b, 195, "Far band B should be blue (#3D63DD), got \(farBand)")

        // 5. Background field is dark blue (#14284F -> #081530)
        let field = pixel(bmp, x: 160, y: 500)
        XCTAssertEqual(field.a, 255)
        XCTAssertLessThan(field.r, 32, "Field R should be dark blue, got \(field)")
        XCTAssertTrue((15...52).contains(field.g), "Field G should be dark blue, got \(field)")
        XCTAssertTrue((42...92).contains(field.b), "Field B should be dark blue, got \(field)")
        XCTAssertGreaterThan(field.b, field.g, "Field should be blue-dominant, got \(field)")
        XCTAssertGreaterThan(field.g, field.r, "Field should be dark navy, got \(field)")

        // Outer margin outside 64/1024 rounded square stays transparent
        let outerCorner = pixel(bmp, x: 8, y: 8)
        XCTAssertEqual(outerCorner.a, 0, "Outer corner outside macOS rounded square must be transparent")
    }

    func testAppNamesVersion1_0AndCopyrightInInfoPlists() throws {
        // Verify built KinectViewer.app bundle Info.plist (test host)
        let hostInfo = try XCTUnwrap(Bundle.main.infoDictionary)
        XCTAssertEqual(hostInfo["CFBundleName"] as? String, "Kinect Viewer")
        XCTAssertEqual(hostInfo["CFBundleDisplayName"] as? String, "Kinect Viewer")
        XCTAssertEqual(hostInfo["CFBundleExecutable"] as? String, "KinectViewer")
        XCTAssertEqual(hostInfo["CFBundleShortVersionString"] as? String, "1.0")
        XCTAssertEqual(hostInfo["CFBundleVersion"] as? String, "1")
        XCTAssertEqual(hostInfo["NSHumanReadableCopyright"] as? String, "© 2026 The Kinect Viewer Authors")

        // Verify source macos/Info.plist and ios/Info.plist on disk
        let macPlistURL = repoRootURL.appendingPathComponent("macos/Info.plist")
        let macPlistData = try Data(contentsOf: macPlistURL)
        let macPlist = try XCTUnwrap(
            PropertyListSerialization.propertyList(from: macPlistData, options: [], format: nil) as? [String: Any]
        )
        XCTAssertEqual(macPlist["CFBundleName"] as? String, "Kinect Viewer")
        XCTAssertEqual(macPlist["CFBundleDisplayName"] as? String, "Kinect Viewer")
        XCTAssertEqual(macPlist["CFBundleShortVersionString"] as? String, "1.0")
        XCTAssertEqual(macPlist["CFBundleVersion"] as? String, "1")
        XCTAssertEqual(macPlist["NSHumanReadableCopyright"] as? String, "© 2026 The Kinect Viewer Authors")

        let iosPlistURL = repoRootURL.appendingPathComponent("ios/Info.plist")
        let iosPlistData = try Data(contentsOf: iosPlistURL)
        let iosPlist = try XCTUnwrap(
            PropertyListSerialization.propertyList(from: iosPlistData, options: [], format: nil) as? [String: Any]
        )
        // Since the TestFlight prep, the iPhone app's version and build come from build settings in project.yml
        // (MARKETING_VERSION / CURRENT_PROJECT_VERSION, the build number raised for every upload).
        XCTAssertEqual(iosPlist["CFBundleShortVersionString"] as? String, "$(MARKETING_VERSION)")
        XCTAssertEqual(iosPlist["CFBundleVersion"] as? String, "$(CURRENT_PROJECT_VERSION)")
        let projectYML = try String(contentsOf: repoRootURL.appendingPathComponent("project.yml"), encoding: .utf8)
        XCTAssertTrue(projectYML.contains("        MARKETING_VERSION: \"1.0\""), "The iPhone app's marketing version is 1.0")
        let buildRegex = try NSRegularExpression(pattern: "^        CURRENT_PROJECT_VERSION: \"([0-9]+)\"$", options: [.anchorsMatchLines])
        let buildMatch = try XCTUnwrap(buildRegex.firstMatch(in: projectYML, range: NSRange(projectYML.startIndex..., in: projectYML)))
        let buildNumber = try XCTUnwrap(Int((projectYML as NSString).substring(with: buildMatch.range(at: 1))))
        XCTAssertGreaterThanOrEqual(buildNumber, 1, "The iPhone app's build number is a positive integer")
        XCTAssertEqual(iosPlist["NSHumanReadableCopyright"] as? String, "© 2026 The Kinect Viewer Authors")
        let iosDisplayName = try XCTUnwrap(iosPlist["CFBundleDisplayName"] as? String)
        XCTAssertTrue(["Kinect Viewer", "Kinect Pocket"].contains(iosDisplayName))
    }
}
