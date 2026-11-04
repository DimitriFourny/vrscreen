import CoreGraphics
import CoreText
import CoreVideo
import Foundation
import MirrorProtocol

/// Synthetic 420v (BT.709 video range) source: static color bars and grid, plus a
/// moving box, a sweeping line and a frame counter / timestamp drawn every frame.
final class TestPatternSource {
    let width: Int
    let height: Int
    let fps: Int
    private let queue: DispatchQueue
    private let handler: (CVPixelBuffer, UInt64) -> Void
    private var pool: CVPixelBufferPool?
    private var timer: DispatchSourceTimer?
    private var frameNumber = 0
    private let startMicros = HostClock.nowMicros()
    private let lumaTemplate: [UInt8]
    private let chromaTemplate: [UInt8]
    private let font: CTFont

    /// `handler` is called on `queue` with each frame and its capture time (µs, host clock).
    init(width: Int, height: Int, fps: Int, queue: DispatchQueue, handler: @escaping (CVPixelBuffer, UInt64) -> Void) {
        self.width = width
        self.height = height
        self.fps = fps
        self.queue = queue
        self.handler = handler
        (lumaTemplate, chromaTemplate) = Self.makeTemplate(width: width, height: height)
        font = CTFontCreateWithName("Menlo-Bold" as CFString, CGFloat(height) / 12, nil)
    }

    func start() throws {
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [CFString: Any]() as CFDictionary,
        ]
        let status = CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attributes as CFDictionary, &pool)
        guard status == kCVReturnSuccess else { throw OptionError("CVPixelBufferPoolCreate failed: \(status)") }
        log("source: test pattern \(width)x\(height) @ \(fps) fps (420v, BT.709 video range)")
        let timer = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
        timer.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / fps), leeway: .microseconds(500))
        timer.setEventHandler { [weak self] in self?.produceFrame() }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func produceFrame() {
        guard let pool else { return }
        var buffer: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) == kCVReturnSuccess,
              let buffer else { return }
        let now = HostClock.nowMicros()
        frameNumber += 1
        render(into: buffer, frame: frameNumber, elapsedMicros: now - startMicros)
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
        CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, .shouldPropagate)
        handler(buffer, now)
    }

    private func render(into buffer: CVPixelBuffer, frame: Int, elapsedMicros: UInt64) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let yBase = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!
        let yStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let uvBase = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!
        let uvStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        lumaTemplate.withUnsafeBytes { src in
            for row in 0..<height {
                (yBase + row * yStride).copyMemory(from: src.baseAddress! + row * width, byteCount: width)
            }
        }
        chromaTemplate.withUnsafeBytes { src in
            for row in 0..<height / 2 {
                (uvBase + row * uvStride).copyMemory(from: src.baseAddress! + row * width, byteCount: width)
            }
        }

        // Luma plane as an 8-bit gray bitmap. CG origin is bottom-left.
        guard let ctx = CGContext(data: yBase, width: width, height: height, bitsPerComponent: 8,
                                  bytesPerRow: yStride, space: CGColorSpaceCreateDeviceGray(),
                                  bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return }
        let white = CGColor(gray: 235 / 255, alpha: 1)
        let w = CGFloat(width), h = CGFloat(height)

        // Box bouncing horizontally across the lower band, one full sweep every 4 s.
        let t = Double(elapsedMicros) / 1e6
        let box = h / 6
        let phase = (t / 2).truncatingRemainder(dividingBy: 2)
        let x = (phase < 1 ? phase : 2 - phase) * Double(w - box)
        ctx.setFillColor(white)
        ctx.fill(CGRect(x: x, y: h * 0.05, width: box, height: box))

        // Vertical line sweeping left to right once per second (shows tearing/judder).
        let lineX = CGFloat(t.truncatingRemainder(dividingBy: 1)) * w
        ctx.fill(CGRect(x: lineX, y: 0, width: max(2, w / 480), height: h * 0.7))

        let attributes = [
            kCTFontAttributeName as NSAttributedString.Key: font,
            kCTForegroundColorAttributeName as NSAttributedString.Key: white,
        ] as [NSAttributedString.Key: Any]
        let text = String(format: "frame %06d   t=%.3f s", frame, t)
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attributes))
        let bounds = CTLineGetBoundsWithOptions(line, [])
        ctx.textPosition = CGPoint(x: (w - bounds.width) / 2, y: h * 0.42)
        CTLineDraw(line, ctx)
        ctx.flush()
    }

    /// Static part: eight 100% color bars on the top 30%, dark background with a grid below.
    private static func makeTemplate(width: Int, height: Int) -> ([UInt8], [UInt8]) {
        let bars: [(Double, Double, Double)] = [
            (1, 1, 1), (1, 1, 0), (0, 1, 1), (0, 1, 0), (1, 0, 1), (1, 0, 0), (0, 0, 1), (0, 0, 0),
        ]
        // BT.709 RGB → Y'CbCr, 8-bit video range.
        let yuv: [(UInt8, UInt8, UInt8)] = bars.map { r, g, b in
            let y = 0.2126 * r + 0.7152 * g + 0.0722 * b
            let cb = (b - y) / 1.8556
            let cr = (r - y) / 1.5748
            return (UInt8((16 + 219 * y).rounded()), UInt8((128 + 224 * cb).rounded()), UInt8((128 + 224 * cr).rounded()))
        }
        let barRows = height * 3 / 10
        var luma = [UInt8](repeating: 48, count: width * height)
        var chroma = [UInt8](repeating: 128, count: width * height / 2)
        let grid = max(8, height / 9)
        for row in 0..<height {
            for col in 0..<width {
                if row < barRows {
                    luma[row * width + col] = yuv[col * bars.count / width].0
                } else if row % grid == 0 || col % grid == 0 {
                    luma[row * width + col] = 96
                }
            }
        }
        for row in 0..<barRows / 2 {
            for pair in 0..<width / 2 {
                let c = yuv[pair * 2 * bars.count / width]
                chroma[row * width + pair * 2] = c.1
                chroma[row * width + pair * 2 + 1] = c.2
            }
        }
        return (luma, chroma)
    }
}
