import CoreGraphics
import CoreVideo
import Foundation
import ImageIO

/// Writes a BGRA pixel buffer as a PNG file.
func writePNG(_ buffer: CVPixelBuffer, to path: String) throws {
    guard CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA else {
        throw OptionError("snapshot: unexpected pixel format")
    }
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
    let bitmapInfo = CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
    guard let ctx = CGContext(data: CVPixelBufferGetBaseAddress(buffer),
                              width: CVPixelBufferGetWidth(buffer), height: CVPixelBufferGetHeight(buffer),
                              bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer),
                              space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: bitmapInfo),
          let image = ctx.makeImage() else {
        throw OptionError("snapshot: cannot create image")
    }
    let url = URL(fileURLWithPath: path)
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else {
        throw OptionError("snapshot: cannot create \(path)")
    }
    CGImageDestinationAddImage(dest, image, nil)
    guard CGImageDestinationFinalize(dest) else { throw OptionError("snapshot: cannot write \(path)") }
}
