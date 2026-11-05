#!/usr/bin/env swift
// Renders the VRScreen app icon (dark navy squircle with a glowing curved screen, in the
// style of the GitHub preview) and packs it into an .icns with iconutil.
//
//   swift scripts/make-icon.swift mac/Resources/AppIcon.icns [preview.png]
//
// Every size is drawn from vectors (no downscaling), with fewer details at small sizes.
import AppKit
import CoreGraphics
import Foundation

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

func gradient(_ stops: [(CGFloat, CGColor)]) -> CGGradient {
    CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: stops.map(\.1) as CFArray,
               locations: stops.map(\.0))!
}

/// The curved screen as seen from inside the cylinder: straight sides, top and bottom edges
/// bowed toward the middle.
func screenPath(_ r: CGRect, sag: CGFloat) -> CGPath {
    let path = CGMutablePath()
    path.move(to: CGPoint(x: r.minX, y: r.maxY))
    path.addQuadCurve(to: CGPoint(x: r.maxX, y: r.maxY), control: CGPoint(x: r.midX, y: r.maxY - 2 * sag))
    path.addLine(to: CGPoint(x: r.maxX, y: r.minY))
    path.addQuadCurve(to: CGPoint(x: r.minX, y: r.minY), control: CGPoint(x: r.midX, y: r.minY + 2 * sag))
    path.closeSubpath()
    return path
}

func render(size px: Int) -> CGImage {
    let s = CGFloat(px)
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high
    let detail = px >= 64

    // macOS icon grid: an 824/1024 body with a continuous-corner radius of ~185/1024.
    let body = CGRect(x: s * 100 / 1024, y: s * 100 / 1024, width: s * 824 / 1024, height: s * 824 / 1024)
    let squircle = CGPath(roundedRect: body, cornerWidth: s * 185 / 1024, cornerHeight: s * 185 / 1024, transform: nil)

    // Drop shadow under the body.
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -s * 10 / 1024), blur: s * 24 / 1024, color: color(0x000000, 0.45))
    ctx.addPath(squircle)
    ctx.setFillColor(color(0x0B0F2A))
    ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(squircle)
    ctx.clip()
    // Navy background, lighter at the top, with a blue glow behind the screen.
    ctx.drawLinearGradient(gradient([(0, color(0x1B2156)), (0.55, color(0x0D1133)), (1, color(0x05071A))]),
                           start: CGPoint(x: 0, y: body.maxY), end: CGPoint(x: 0, y: body.minY), options: [])
    let center = CGPoint(x: body.midX, y: body.minY + body.height * 0.56)
    ctx.drawRadialGradient(gradient([(0, color(0x4C6EF5, 0.42)), (0.5, color(0x3B5BDB, 0.12)), (1, color(0x3B5BDB, 0))]),
                           startCenter: center, startRadius: 0, endCenter: center, endRadius: body.width * 0.62, options: [])

    // Floor grid in perspective below the screen.
    let horizon = body.minY + body.height * 0.27
    if detail {
        ctx.saveGState()
        ctx.clip(to: CGRect(x: body.minX, y: body.minY, width: body.width, height: horizon - body.minY))
        ctx.setStrokeColor(color(0x5C7CFA, 0.28))
        ctx.setLineWidth(max(1, s * 2.2 / 1024))
        let vanishing = CGPoint(x: body.midX, y: horizon + body.height * 0.08)
        for i in -9...9 {
            ctx.move(to: vanishing)
            ctx.addLine(to: CGPoint(x: body.midX + CGFloat(i) * body.width * 0.16, y: body.minY))
        }
        var y = horizon
        var step = body.height * 0.022
        while y > body.minY {
            ctx.move(to: CGPoint(x: body.minX, y: y))
            ctx.addLine(to: CGPoint(x: body.maxX, y: y))
            y -= step
            step *= 1.45
        }
        ctx.strokePath()
        // Fade the grid toward the horizon.
        ctx.drawLinearGradient(gradient([(0, color(0x0D1133, 0)), (1, color(0x0D1133, 0.9))]),
                               start: CGPoint(x: 0, y: body.minY), end: CGPoint(x: 0, y: horizon), options: [])
        ctx.restoreGState()
    }

    // The curved screen.
    let screen = CGRect(x: body.minX + body.width * 0.13, y: body.minY + body.height * 0.30,
                        width: body.width * 0.74, height: body.height * 0.47)
    let sag = screen.height * 0.10
    let path = screenPath(screen, sag: sag)
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: s * 60 / 1024, color: color(0x5C7CFA, 0.95))
    ctx.addPath(path)
    ctx.setFillColor(color(0x2A44B0))
    ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    ctx.drawLinearGradient(gradient([(0, color(0x3A5BD6)), (1, color(0x1B2A78))]),
                           start: CGPoint(x: 0, y: screen.maxY), end: CGPoint(x: 0, y: screen.minY), options: [])
    if detail {
        // A window with traffic lights and code lines, like the preview.
        let window = CGRect(x: screen.minX + screen.width * 0.11, y: screen.minY + screen.height * 0.19,
                            width: screen.width * 0.78, height: screen.height * 0.60)
        let radius = s * 18 / 1024
        ctx.addPath(CGPath(roundedRect: window, cornerWidth: radius, cornerHeight: radius, transform: nil))
        ctx.setFillColor(color(0x0E1222, 0.92))
        ctx.fillPath()
        let dot = window.height * 0.085
        for (i, hex) in [0xFF5F57, 0xFEBC2E, 0x28C840].enumerated() {
            ctx.setFillColor(color(UInt32(hex)))
            ctx.fillEllipse(in: CGRect(x: window.minX + dot * (1.0 + 1.6 * CGFloat(i)), y: window.maxY - dot * 1.9,
                                       width: dot, height: dot))
        }
        let lines: [(indent: CGFloat, length: CGFloat, hex: UInt32)] = [
            (0, 0.55, 0x74A0FF), (0.08, 0.38, 0x8A90A6), (0, 0.70, 0x8A90A6), (0, 0.34, 0xB18CFF), (0.08, 0.52, 0x8A90A6),
        ]
        let lineHeight = window.height * 0.06
        for (i, line) in lines.enumerated() {
            let y = window.maxY - window.height * (0.36 + 0.13 * CGFloat(i))
            let x = window.minX + window.width * (0.07 + line.indent)
            let rect = CGRect(x: x, y: y, width: window.width * line.length, height: lineHeight)
            ctx.addPath(CGPath(roundedRect: rect, cornerWidth: lineHeight / 2, cornerHeight: lineHeight / 2, transform: nil))
            ctx.setFillColor(color(line.hex))
            ctx.fillPath()
        }
    }
    ctx.restoreGState()
    // Glowing rim.
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: s * 26 / 1024, color: color(0x91A7FF, 1))
    ctx.addPath(path)
    ctx.setStrokeColor(color(0xA5B8FF))
    ctx.setLineWidth(max(1, s * 9 / 1024))
    ctx.setLineJoin(.round)
    ctx.strokePath()
    ctx.restoreGState()

    // A faint highlight along the squircle's top edge.
    ctx.addPath(squircle)
    ctx.setStrokeColor(color(0xFFFFFF, 0.10))
    ctx.setLineWidth(max(1, s * 4 / 1024))
    ctx.strokePath()
    ctx.restoreGState()
    return ctx.makeImage()!
}

func writePNG(_ image: CGImage, to url: URL) throws {
    let rep = NSBitmapImageRep(cgImage: image)
    guard let data = rep.representation(using: .png, properties: [:]) else {
        throw NSError(domain: "make-icon", code: 1, userInfo: [NSLocalizedDescriptionKey: "PNG encoding failed"])
    }
    try data.write(to: url)
}

let args = CommandLine.arguments
guard args.count >= 2, args[1].hasSuffix(".icns") else {
    FileHandle.standardError.write("usage: swift scripts/make-icon.swift OUTPUT.icns [PREVIEW.png]\n".data(using: .utf8)!)
    exit(2)
}
let output = URL(fileURLWithPath: args[1])
let iconset = FileManager.default.temporaryDirectory.appendingPathComponent("AppIcon-\(getpid()).iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: iconset) }
for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let name = "icon_\(points)x\(points)\(scale == 2 ? "@2x" : "").png"
        try writePNG(render(size: points * scale), to: iconset.appendingPathComponent(name))
    }
}
if args.count >= 3 { try writePNG(render(size: 1024), to: URL(fileURLWithPath: args[2])) }
try FileManager.default.createDirectory(at: output.deletingLastPathComponent(), withIntermediateDirectories: true)
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", output.path]
try iconutil.run()
iconutil.waitUntilExit()
guard iconutil.terminationStatus == 0 else { exit(iconutil.terminationStatus) }
print("wrote \(output.path)")
