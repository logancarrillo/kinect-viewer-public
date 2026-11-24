#!/usr/bin/env swift

import AppKit
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

// NOTE: `design/logo.svg` ("Sensor and Beam Mark") is the authoritative source of truth.
// This script NEVER writes or modifies `design/logo.svg`; it renders the macOS AppIcon set,
// iOS 1024 opaque AppIcon variants (Light, Dark, Tinted), in-app LogoMark imagesets, and
// `design/icon-sheet.png` directly from `logo.svg`'s tile-unit geometry.

// MARK: - Palette & Theme Definitions

enum IconTheme {
    case standard       // macOS AppIcon, iOS Light 1024, LogoMark
    case iosDark        // iOS Dark 1024 (mark on near-black)
    case iosTinted      // iOS Tinted 1024 (opaque greyscale luminance)
}

struct RGBA {
    var r: Double
    var g: Double
    var b: Double
    var a: Double = 1.0

    func cgColor() -> CGColor {
        CGColor(srgbRed: r, green: g, blue: b, alpha: a)
    }

    func greyscaleLuminance() -> RGBA {
        let y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        return .gray(y, alpha: a)
    }

    static func hex(_ hex: UInt32, alpha: Double = 1.0) -> RGBA {
        let r = Double((hex >> 16) & 0xFF) / 255.0
        let g = Double((hex >> 8) & 0xFF) / 255.0
        let b = Double(hex & 0xFF) / 255.0
        return RGBA(r: r, g: g, b: b, a: alpha)
    }

    static func gray(_ v: Double, alpha: Double = 1.0) -> RGBA {
        let clamped = min(1.0, max(0.0, v))
        return RGBA(r: clamped, g: clamped, b: clamped, a: alpha)
    }
}

struct SensorBeamPalette {
    let bgTop: RGBA
    let bgBottom: RGBA
    let sensorBar: RGBA
    let lensOuter: RGBA
    let lensInner: RGBA
    let statusLight: RGBA
    let bandNear: RGBA
    let bandMid: RGBA
    let bandFar: RGBA

    static let standardPalette = SensorBeamPalette(
        bgTop: .hex(0x14284F),
        bgBottom: .hex(0x081530),
        sensorBar: .hex(0xE9EEF7),
        lensOuter: .hex(0x0B1530),
        lensInner: .hex(0x22B8CF),
        statusLight: .hex(0xFF5E57),
        bandNear: .hex(0xFFB238),
        bandMid: .hex(0x22B8CF),
        bandFar: .hex(0x3D63DD)
    )

    static func make(_ theme: IconTheme) -> SensorBeamPalette {
        switch theme {
        case .standard:
            return standardPalette
        case .iosDark:
            return SensorBeamPalette(
                bgTop: .hex(0x0A0E17),
                bgBottom: .hex(0x05070C),
                sensorBar: standardPalette.sensorBar,
                lensOuter: standardPalette.lensOuter,
                lensInner: standardPalette.lensInner,
                statusLight: standardPalette.statusLight,
                bandNear: standardPalette.bandNear,
                bandMid: standardPalette.bandMid,
                bandFar: standardPalette.bandFar
            )
        case .iosTinted:
            return SensorBeamPalette(
                bgTop: standardPalette.bgTop.greyscaleLuminance(),
                bgBottom: standardPalette.bgBottom.greyscaleLuminance(),
                sensorBar: standardPalette.sensorBar.greyscaleLuminance(),
                lensOuter: standardPalette.lensOuter.greyscaleLuminance(),
                lensInner: standardPalette.lensInner.greyscaleLuminance(),
                statusLight: standardPalette.statusLight.greyscaleLuminance(),
                bandNear: standardPalette.bandNear.greyscaleLuminance(),
                bandMid: standardPalette.bandMid.greyscaleLuminance(),
                bandFar: standardPalette.bandFar.greyscaleLuminance()
            )
        }
    }
}

// MARK: - Sensor and Beam Geometry (`design/logo.svg`, tile units 0...1)

enum SensorBeamGeometry {
    /// Half-width of each trapezoid depth band at tile-unit `y` (centred on `x = 0.5`),
    /// matching `design/logo.svg` polygons exactly: `0.08 + (y - 0.41) / 0.45 * 0.26`.
    static func beamHalfWidth(atTileY y: CGFloat) -> CGFloat {
        0.08 + ((y - 0.41) / 0.45) * 0.26
    }

    static func trapezoidPath(yTop: CGFloat, yBottom: CGFloat, tileSize: CGFloat) -> CGPath {
        let hwTop = beamHalfWidth(atTileY: yTop)
        let hwBottom = beamHalfWidth(atTileY: yBottom)
        let path = CGMutablePath()
        path.move(to: CGPoint(x: (0.5 - hwTop) * tileSize, y: yTop * tileSize))
        path.addLine(to: CGPoint(x: (0.5 + hwTop) * tileSize, y: yTop * tileSize))
        path.addLine(to: CGPoint(x: (0.5 + hwBottom) * tileSize, y: yBottom * tileSize))
        path.addLine(to: CGPoint(x: (0.5 - hwBottom) * tileSize, y: yBottom * tileSize))
        path.closeSubpath()
        return path
    }
}

// MARK: - CoreGraphics Renderer

enum IconRenderer {
    static let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!

    /// Draws the Sensor and Beam mark inside `[0, tileSize]^2` (top-left origin).
    /// All coordinates are expressed in tile units `[0, 1]` matching `design/logo.svg`.
    static func drawTileArtwork(in ctx: CGContext, tileSize: CGFloat, theme: IconTheme) {
        let palette = SensorBeamPalette.make(theme)

        // 1. Vertical background gradient (#14284F top -> #081530 bottom in standard theme)
        let bgColors = [palette.bgTop.cgColor(), palette.bgBottom.cgColor()] as CFArray
        let bgLocs: [CGFloat] = [0.0, 1.0]
        if let grad = CGGradient(colorsSpace: colorSpace, colors: bgColors, locations: bgLocs) {
            ctx.drawLinearGradient(
                grad,
                start: CGPoint(x: 0.5 * tileSize, y: 0.0),
                end: CGPoint(x: 0.5 * tileSize, y: tileSize),
                options: [.drawsBeforeStartLocation, .drawsAfterEndLocation]
            )
        }

        // 2. Sensor bar: rounded rect at x=0.14, y=0.18, w=0.72, h=0.17, radius=0.085
        let barRect = CGRect(
            x: 0.14 * tileSize,
            y: 0.18 * tileSize,
            width: 0.72 * tileSize,
            height: 0.17 * tileSize
        )
        let barRadius = 0.085 * tileSize
        let barPath = CGPath(roundedRect: barRect, cornerWidth: barRadius, cornerHeight: barRadius, transform: nil)
        ctx.addPath(barPath)
        ctx.setFillColor(palette.sensorBar.cgColor())
        ctx.fillPath()

        // 3. Lens outer circle at (0.5, 0.265), r=0.055, and inner circle r=0.022
        let lensCenter = CGPoint(x: 0.5 * tileSize, y: 0.265 * tileSize)
        let outerR = 0.055 * tileSize
        ctx.setFillColor(palette.lensOuter.cgColor())
        ctx.fillEllipse(in: CGRect(
            x: lensCenter.x - outerR,
            y: lensCenter.y - outerR,
            width: outerR * 2.0,
            height: outerR * 2.0
        ))

        let innerR = 0.022 * tileSize
        ctx.setFillColor(palette.lensInner.cgColor())
        ctx.fillEllipse(in: CGRect(
            x: lensCenter.x - innerR,
            y: lensCenter.y - innerR,
            width: innerR * 2.0,
            height: innerR * 2.0
        ))

        // 4. Status light circle at (0.25, 0.265), r=0.018
        let lightCenter = CGPoint(x: 0.25 * tileSize, y: 0.265 * tileSize)
        let lightR = 0.018 * tileSize
        ctx.setFillColor(palette.statusLight.cgColor())
        ctx.fillEllipse(in: CGRect(
            x: lightCenter.x - lightR,
            y: lightCenter.y - lightR,
            width: lightR * 2.0,
            height: lightR * 2.0
        ))

        // 5. Three depth trapezoid bands (near amber, mid teal, far blue)
        let bands: [(yTop: CGFloat, yBottom: CGFloat, fill: RGBA)] = [
            (0.41,  0.54,  palette.bandNear),
            (0.575, 0.705, palette.bandMid),
            (0.74,  0.86,  palette.bandFar),
        ]
        for band in bands {
            let path = SensorBeamGeometry.trapezoidPath(yTop: band.yTop, yBottom: band.yBottom, tileSize: tileSize)
            ctx.addPath(path)
            ctx.setFillColor(band.fill.cgColor())
            ctx.fillPath()
        }
    }

    /// Renders a full-bleed opaque square icon (required for iOS 1024x1024 Light, Dark, Tinted — no alpha, no baked corners).
    static func renderOpaqueSquareIcon(pixelSize: Int, theme: IconTheme) -> CGImage {
        let ctx = CGContext(
            data: nil,
            width: pixelSize,
            height: pixelSize,
            bitsPerComponent: 8,
            bytesPerRow: pixelSize * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue
        )!
        ctx.interpolationQuality = .high
        ctx.setAllowsAntialiasing(true)
        ctx.setShouldAntialias(true)

        // Top-left origin matching [0, 1024]^2
        ctx.translateBy(x: 0, y: CGFloat(pixelSize))
        ctx.scaleBy(x: CGFloat(pixelSize) / 1024.0, y: -CGFloat(pixelSize) / 1024.0)

        drawTileArtwork(in: ctx, tileSize: 1024.0, theme: theme)
        return ctx.makeImage()!
    }

    /// Renders a macOS rounded-square AppIcon (or LogoMark) on a transparent RGBA canvas.
    /// By default (`fullBleedRatio == nil`), the square starts `64/1024` in (`bodySize = 896`)
    /// with corner radius `0.225 * bodySize` (`201.60` at 1024), matching `design/logo.svg`.
    static func renderMacRoundedIcon(pixelSize: Int, fullBleedRatio: CGFloat? = nil, includeDropShadow: Bool = true) -> CGImage {
        let ctx = CGContext(
            data: nil,
            width: pixelSize,
            height: pixelSize,
            bitsPerComponent: 8,
            bytesPerRow: pixelSize * 4,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        )!
        ctx.interpolationQuality = .high
        ctx.setAllowsAntialiasing(true)
        ctx.setShouldAntialias(true)

        ctx.clear(CGRect(x: 0, y: 0, width: pixelSize, height: pixelSize))

        // Top-left origin matching [0, 1024]^2
        ctx.translateBy(x: 0, y: CGFloat(pixelSize))
        ctx.scaleBy(x: CGFloat(pixelSize) / 1024.0, y: -CGFloat(pixelSize) / 1024.0)

        // Mac square starts 64/1024 in -> 896/1024 = 0.875, corner radius 0.225 of square
        let bodyFraction: CGFloat = fullBleedRatio ?? (896.0 / 1024.0)
        let bodySize = 1024.0 * bodyFraction
        let margin = (1024.0 - bodySize) * 0.5
        let bodyRect = CGRect(x: margin, y: margin, width: bodySize, height: bodySize)
        let cornerRadius = bodySize * 0.225
        let maskPath = CGPath(roundedRect: bodyRect, cornerWidth: cornerRadius, cornerHeight: cornerRadius, transform: nil)

        // Subtle macOS drop shadow (scaled proportionally to pixelSize; fits cleanly within 64/1024 margin)
        if includeDropShadow {
            let pxScale = CGFloat(pixelSize) / 1024.0
            let shadowOffset = pixelSize <= 16 ? 0.25 : (12.0 * pxScale)
            let shadowBlur = pixelSize <= 16 ? 0.65 : (24.0 * pxScale)
            let shadowAlpha: CGFloat = pixelSize <= 16 ? 0.26 : 0.34
            ctx.saveGState()
            ctx.setShadow(
                offset: CGSize(width: 0, height: -shadowOffset),
                blur: shadowBlur,
                color: CGColor(srgbRed: 0, green: 0, blue: 0, alpha: shadowAlpha)
            )
            ctx.addPath(maskPath)
            ctx.setFillColor(RGBA.hex(0x081530).cgColor())
            ctx.fillPath()
            ctx.restoreGState()
        }

        // Clip artwork inside the rounded-square tile
        ctx.saveGState()
        ctx.addPath(maskPath)
        ctx.clip()

        ctx.translateBy(x: bodyRect.minX, y: bodyRect.minY)
        drawTileArtwork(in: ctx, tileSize: bodySize, theme: .standard)
        ctx.restoreGState()

        return ctx.makeImage()!
    }

    static func writePNG(_ image: CGImage, to url: URL, opaque: Bool) throws {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        guard let dest = CGImageDestinationCreateWithURL(
            url as CFURL,
            UTType.png.identifier as CFString,
            1,
            nil
        ) else {
            throw NSError(domain: "RenderIcons", code: 1, userInfo: [NSLocalizedDescriptionKey: "Failed to create PNG destination at \(url.path)"])
        }
        let props: [CFString: Any] = [
            kCGImagePropertyHasAlpha: !opaque,
        ]
        CGImageDestinationAddImage(dest, image, props as CFDictionary)
        guard CGImageDestinationFinalize(dest) else {
            throw NSError(domain: "RenderIcons", code: 2, userInfo: [NSLocalizedDescriptionKey: "Failed to finalize PNG at \(url.path)"])
        }
    }
}

// MARK: - Contact Sheet Renderer (`design/icon-sheet.png`)

enum ContactSheetRenderer {
    static func render(
        macImagesByPx: [Int: CGImage],
        iosLight: CGImage,
        iosDark: CGImage,
        iosTinted: CGImage,
        logoMark192: CGImage
    ) -> CGImage {
        let width = 1760
        let height = 1180
        let cs = IconRenderer.colorSpace
        let ctx = CGContext(
            data: nil,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: width * 4,
            space: cs,
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue
        )!
        ctx.interpolationQuality = .high

        let nsCtx = NSGraphicsContext(cgContext: ctx, flipped: false)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = nsCtx

        // Background
        let bgColors = [
            RGBA.hex(0x0B121E).cgColor(),
            RGBA.hex(0x111A2B).cgColor(),
        ] as CFArray
        if let bg = CGGradient(colorsSpace: cs, colors: bgColors, locations: [0.0, 1.0]) {
            ctx.drawLinearGradient(
                bg,
                start: CGPoint(x: 0, y: CGFloat(height)),
                end: CGPoint(x: CGFloat(width), y: 0),
                options: []
            )
        }

        // The sheet carries no text: a system font baked into the image can't be redistributed. Each tile's name
        // and size are listed in design/icon-sheet.md instead. The calls stay so the layout is unchanged.
        func drawLabel(_ text: String, x: CGFloat, topY: CGFloat, size: CGFloat, weight: NSFont.Weight = .medium, color: NSColor = .white) {
            _ = (text, x, topY, size, weight, color)
        }

        func drawImageTopLeft(_ img: CGImage, x: CGFloat, topY: CGFloat, w: CGFloat, h: CGFloat, nearestNeighbor: Bool = false) {
            ctx.saveGState()
            ctx.interpolationQuality = nearestNeighbor ? .none : .high
            let rect = CGRect(x: x, y: CGFloat(height) - topY - h, width: w, height: h)
            ctx.draw(img, in: rect)
            ctx.restoreGState()
        }

        func drawCard(x: CGFloat, topY: CGFloat, w: CGFloat, h: CGFloat) {
            let rect = CGRect(x: x, y: CGFloat(height) - topY - h, width: w, height: h)
            let path = CGPath(roundedRect: rect, cornerWidth: 18, cornerHeight: 18, transform: nil)
            ctx.saveGState()
            ctx.addPath(path)
            ctx.setFillColor(RGBA.hex(0x162238, alpha: 0.85).cgColor())
            ctx.fillPath()
            ctx.addPath(path)
            ctx.setStrokeColor(RGBA.hex(0x2D446B, alpha: 0.7).cgColor())
            ctx.setLineWidth(1.5)
            ctx.strokePath()
            ctx.restoreGState()
        }

        // Title header (neutral — never includes forbidden brand words)
        drawLabel("SENSOR AND BEAM MARK — APP ICON CONTACT SHEET", x: 56, topY: 42, size: 22, weight: .bold, color: NSColor(srgbRed: 0.88, green: 0.96, blue: 1.0, alpha: 1.0))
        drawLabel("Rendered from design/logo.svg source shapes via design/render-icons.swift (logo.svg is read-only source)", x: 56, topY: 76, size: 14, weight: .regular, color: NSColor(srgbRed: 0.56, green: 0.68, blue: 0.84, alpha: 1.0))

        // Section 1: macOS AppIcon Sizes (1024 down to 16 + 10x magnified 16px pixel check)
        drawCard(x: 48, topY: 120, w: 1664, h: 500)
        drawLabel("macOS AppIcon Set — Rounded-Square Tile (64/1024 Inset, r=0.225, Drop Shadow) — 16px to 1024px", x: 76, topY: 142, size: 15, weight: .semibold, color: NSColor(srgbRed: 0.40, green: 0.88, blue: 0.96, alpha: 1.0))

        let macPreviewSpecs: [(px: Int, drawSize: CGFloat, label: String)] = [
            (1024, 320, "1024×1024 (512@2x)"),
            (512,  240, "512×512 (256@2x / 512@1x)"),
            (256,  180, "256×256 (128@2x / 256@1x)"),
            (128,  128, "128×128 (@1x)"),
            (64,    64, "64×64 (32@2x)"),
            (32,    32, "32×32"),
            (16,    16, "16×16"),
        ]

        var cursorX: CGFloat = 78
        let baselineBottomTopY: CGFloat = 530
        for spec in macPreviewSpecs {
            if let img = macImagesByPx[spec.px] {
                let topY = baselineBottomTopY - spec.drawSize
                drawImageTopLeft(img, x: cursorX, topY: topY, w: spec.drawSize, h: spec.drawSize)
                drawLabel(spec.label, x: cursorX, topY: baselineBottomTopY + 16, size: 12, weight: .medium, color: NSColor(white: 0.82, alpha: 1.0))
                cursorX += max(spec.drawSize, 96) + 38
            }
        }

        // Magnified 16x16 nearest-neighbor legibility proof
        if let img16 = macImagesByPx[16] {
            let zoomX: CGFloat = 1480
            let zoomTopY: CGFloat = 360
            drawImageTopLeft(img16, x: zoomX, topY: zoomTopY, w: 160, h: 160, nearestNeighbor: true)
            drawLabel("16×16 (10× pixel zoom)", x: zoomX, topY: 546, size: 12, weight: .semibold, color: NSColor(srgbRed: 0.45, green: 0.95, blue: 0.86, alpha: 1.0))
        }

        // Section 2: iOS 1024x1024 Full-Bleed Opaque Masters (Light, Dark, Tinted) + In-App LogoMark
        drawCard(x: 48, topY: 648, w: 1664, h: 484)
        drawLabel("iOS AppIcon 1024×1024 Full-Bleed Opaque Variants (No Alpha, No Baked Corners) & Small In-App Mark", x: 76, topY: 670, size: 15, weight: .semibold, color: NSColor(srgbRed: 0.40, green: 0.88, blue: 0.96, alpha: 1.0))

        let iosSpecs: [(img: CGImage, title: String, subtitle: String, x: CGFloat)] = [
            (iosLight,  "iOS Light (1024×1024)",  "Full-Bleed Gradient — 24-bit RGB", 84),
            (iosDark,   "iOS Dark (1024×1024)",   "Mark on Near-Black — 24-bit RGB",  504),
            (iosTinted, "iOS Tinted (1024×1024)", "Greyscale Luminance — 24-bit RGB", 924),
        ]

        for item in iosSpecs {
            drawImageTopLeft(item.img, x: item.x, topY: 718, w: 330, h: 330)
            drawLabel(item.title, x: item.x, topY: 1062, size: 13, weight: .semibold, color: .white)
            drawLabel(item.subtitle, x: item.x, topY: 1084, size: 11, weight: .regular, color: NSColor(white: 0.68, alpha: 1.0))
        }

        // In-App small LogoMark preview (1x=64, 2x=128, 3x=192, shown at 64 & 40 pt)
        drawImageTopLeft(logoMark192, x: 1370, topY: 770, w: 140, h: 140)
        drawImageTopLeft(logoMark192, x: 1545, topY: 846, w: 64, h: 64)
        drawImageTopLeft(logoMark192, x: 1632, topY: 870, w: 40, h: 40)
        drawLabel("In-App LogoMark", x: 1370, topY: 1062, size: 13, weight: .semibold, color: .white)
        drawLabel("iOS Pairing & Mac About Box", x: 1370, topY: 1084, size: 11, weight: .regular, color: NSColor(white: 0.68, alpha: 1.0))

        NSGraphicsContext.restoreGraphicsState()
        return ctx.makeImage()!
    }
}

// MARK: - Asset Catalog JSON Writers

func writeJSON(_ string: String, to url: URL) throws {
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try string.write(to: url, atomically: true, encoding: .utf8)
}

let rootCatalogJSON = """
{
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
"""

let iosAppIconContentsJSON = """
{
  "images" : [
    {
      "filename" : "AppIcon-iOS-Light-1024.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    },
    {
      "appearances" : [
        {
          "appearance" : "luminosity",
          "value" : "dark"
        }
      ],
      "filename" : "AppIcon-iOS-Dark-1024.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    },
    {
      "appearances" : [
        {
          "appearance" : "luminosity",
          "value" : "tinted"
        }
      ],
      "filename" : "AppIcon-iOS-Tinted-1024.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
"""

let macAppIconContentsJSON = """
{
  "images" : [
    {
      "filename" : "icon_16x16.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "16x16"
    },
    {
      "filename" : "icon_16x16@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "16x16"
    },
    {
      "filename" : "icon_32x32.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "32x32"
    },
    {
      "filename" : "icon_32x32@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "32x32"
    },
    {
      "filename" : "icon_128x128.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "128x128"
    },
    {
      "filename" : "icon_128x128@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "128x128"
    },
    {
      "filename" : "icon_256x256.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "256x256"
    },
    {
      "filename" : "icon_256x256@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "256x256"
    },
    {
      "filename" : "icon_512x512.png",
      "idiom" : "mac",
      "scale" : "1x",
      "size" : "512x512"
    },
    {
      "filename" : "icon_512x512@2x.png",
      "idiom" : "mac",
      "scale" : "2x",
      "size" : "512x512"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
"""

let iosLogoMarkContentsJSON = """
{
  "images" : [
    {
      "filename" : "LogoMark@1x.png",
      "idiom" : "universal",
      "scale" : "1x"
    },
    {
      "filename" : "LogoMark@2x.png",
      "idiom" : "universal",
      "scale" : "2x"
    },
    {
      "filename" : "LogoMark@3x.png",
      "idiom" : "universal",
      "scale" : "3x"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
"""

let macLogoMarkContentsJSON = """
{
  "images" : [
    {
      "filename" : "LogoMark@1x.png",
      "idiom" : "mac",
      "scale" : "1x"
    },
    {
      "filename" : "LogoMark@2x.png",
      "idiom" : "mac",
      "scale" : "2x"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
"""

// MARK: - Main Entrypoint

let scriptURL = URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL
let repoRoot = scriptURL.deletingLastPathComponent().deletingLastPathComponent()

let designDir = repoRoot.appendingPathComponent("design", isDirectory: true)
let iosAssetsDir = repoRoot.appendingPathComponent("ios/Assets.xcassets", isDirectory: true)
let macAssetsDir = repoRoot.appendingPathComponent("macos/Assets.xcassets", isDirectory: true)

// Verify `design/logo.svg` exists as the read-only source (never written by this script)
let svgURL = designDir.appendingPathComponent("logo.svg")
guard FileManager.default.fileExists(atPath: svgURL.path) else {
    throw NSError(domain: "RenderIcons", code: 3, userInfo: [NSLocalizedDescriptionKey: "Missing authoritative source SVG at \(svgURL.path)"])
}

// 1. Render iOS 1024x1024 Light, Dark, and Tinted full-bleed opaque masters
let iosAppIconDir = iosAssetsDir.appendingPathComponent("AppIcon.appiconset", isDirectory: true)
let iosLight = IconRenderer.renderOpaqueSquareIcon(pixelSize: 1024, theme: .standard)
let iosDark = IconRenderer.renderOpaqueSquareIcon(pixelSize: 1024, theme: .iosDark)
let iosTinted = IconRenderer.renderOpaqueSquareIcon(pixelSize: 1024, theme: .iosTinted)

try IconRenderer.writePNG(iosLight, to: iosAppIconDir.appendingPathComponent("AppIcon-iOS-Light-1024.png"), opaque: true)
try IconRenderer.writePNG(iosDark, to: iosAppIconDir.appendingPathComponent("AppIcon-iOS-Dark-1024.png"), opaque: true)
try IconRenderer.writePNG(iosTinted, to: iosAppIconDir.appendingPathComponent("AppIcon-iOS-Tinted-1024.png"), opaque: true)
try writeJSON(rootCatalogJSON + "\n", to: iosAssetsDir.appendingPathComponent("Contents.json"))
try writeJSON(iosAppIconContentsJSON + "\n", to: iosAppIconDir.appendingPathComponent("Contents.json"))
print("Rendered iOS 1024x1024 Light, Dark, and Tinted AppIcon variants.")

// 2. Render macOS rounded-square AppIcon set (16 to 1024 @1x and @2x)
let macAppIconDir = macAssetsDir.appendingPathComponent("AppIcon.appiconset", isDirectory: true)
let macIconFiles: [(filename: String, px: Int)] = [
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

var macCache: [Int: CGImage] = [:]
for entry in macIconFiles {
    let img: CGImage
    if let cached = macCache[entry.px] {
        img = cached
    } else {
        let rendered = IconRenderer.renderMacRoundedIcon(pixelSize: entry.px)
        macCache[entry.px] = rendered
        img = rendered
    }
    try IconRenderer.writePNG(img, to: macAppIconDir.appendingPathComponent(entry.filename), opaque: false)
}
try writeJSON(rootCatalogJSON + "\n", to: macAssetsDir.appendingPathComponent("Contents.json"))
try writeJSON(macAppIconContentsJSON + "\n", to: macAppIconDir.appendingPathComponent("Contents.json"))
print("Rendered macOS rounded-square AppIcon set (16–1024 @1x/@2x).")

// 3. Render small in-app LogoMark imagesets (used on iOS pairing screen & Mac About box)
let iosLogoDir = iosAssetsDir.appendingPathComponent("LogoMark.imageset", isDirectory: true)
let macLogoDir = macAssetsDir.appendingPathComponent("LogoMark.imageset", isDirectory: true)

let logo64 = IconRenderer.renderMacRoundedIcon(pixelSize: 64, fullBleedRatio: 0.90, includeDropShadow: false)
let logo128 = IconRenderer.renderMacRoundedIcon(pixelSize: 128, fullBleedRatio: 0.90, includeDropShadow: false)
let logo192 = IconRenderer.renderMacRoundedIcon(pixelSize: 192, fullBleedRatio: 0.90, includeDropShadow: false)

try IconRenderer.writePNG(logo64, to: iosLogoDir.appendingPathComponent("LogoMark@1x.png"), opaque: false)
try IconRenderer.writePNG(logo128, to: iosLogoDir.appendingPathComponent("LogoMark@2x.png"), opaque: false)
try IconRenderer.writePNG(logo192, to: iosLogoDir.appendingPathComponent("LogoMark@3x.png"), opaque: false)
try writeJSON(iosLogoMarkContentsJSON + "\n", to: iosLogoDir.appendingPathComponent("Contents.json"))

try IconRenderer.writePNG(logo64, to: macLogoDir.appendingPathComponent("LogoMark@1x.png"), opaque: false)
try IconRenderer.writePNG(logo128, to: macLogoDir.appendingPathComponent("LogoMark@2x.png"), opaque: false)
try writeJSON(macLogoMarkContentsJSON + "\n", to: macLogoDir.appendingPathComponent("Contents.json"))
print("Rendered in-app LogoMark imagesets.")

// 4. Render Contact Sheet (`design/icon-sheet.png`)
let sheet = ContactSheetRenderer.render(
    macImagesByPx: macCache,
    iosLight: iosLight,
    iosDark: iosDark,
    iosTinted: iosTinted,
    logoMark192: logo192
)
let sheetURL = designDir.appendingPathComponent("icon-sheet.png")
try IconRenderer.writePNG(sheet, to: sheetURL, opaque: true)
print("Wrote contact sheet: \(sheetURL.path)")
