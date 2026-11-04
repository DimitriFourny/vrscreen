import CoreVideo
import Foundation

/// `--av-sync` with `vrscreen-sender --source test`: pairs the test pattern's white flashes
/// (first decoded frame of each flash) with the beep onsets found in the received audio,
/// and logs audio − video in capture time (both on the sender clock, as sent).
struct AVSyncCheck {
    /// Mean brightness (0-255) above which a decoded frame counts as the white flash.
    static let flashLuma = 200.0
    /// A flash and a beep further apart than this are not paired.
    static let maxPairingMicros = 500_000.0

    private var previousFrameWasFlash = false
    private var flashes: [Double] = []
    private var beeps: [Double] = []
    private(set) var offsetsMs: [Double] = []

    mutating func frame(captureMicros: UInt64, meanLuma: Double) {
        let isFlash = meanLuma > Self.flashLuma
        if isFlash && !previousFrameWasFlash { flashes.append(Double(captureMicros)) }
        previousFrameWasFlash = isFlash
        pair()
    }

    mutating func beep(atMicros micros: Double) {
        beeps.append(micros)
        pair()
    }

    private mutating func pair() {
        var f = 0
        while f < flashes.count {
            let flash = flashes[f]
            if let b = beeps.firstIndex(where: { abs($0 - flash) < Self.maxPairingMicros }) {
                let offset = (beeps[b] - flash) / 1000
                offsetsMs.append(offset)
                log(String(format: "av-sync: flash frame captured at %.0f µs, beep onset at %.0f µs: audio − video = %+.3f ms",
                           flash, beeps[b], offset))
                flashes.remove(at: f)
                beeps.remove(at: b)
            } else {
                f += 1
            }
        }
        // Unpaired leftovers (e.g. a flash whose beep was dropped) age out.
        let newest = max(flashes.last ?? 0, beeps.last ?? 0)
        flashes.removeAll { newest - $0 > 2 * Self.maxPairingMicros }
        beeps.removeAll { newest - $0 > 2 * Self.maxPairingMicros }
    }

    var summary: String? {
        guard !offsetsMs.isEmpty else { return nil }
        let mean = offsetsMs.reduce(0, +) / Double(offsetsMs.count)
        return String(format: "av-sync: %d pairs, audio − video mean %+.3f ms, min %+.3f ms, max %+.3f ms",
                      offsetsMs.count, mean, offsetsMs.min()!, offsetsMs.max()!)
    }

    /// Mean brightness (0-255) over a sparse grid: luma plane of biplanar YUV images, or the
    /// mean of B, G, R for 32BGRA (what the viewer's decoder outputs).
    static func meanLuma(_ image: CVPixelBuffer) -> Double {
        CVPixelBufferLockBaseAddress(image, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        let planar = CVPixelBufferGetPlaneCount(image) > 0
        guard let base = planar ? CVPixelBufferGetBaseAddressOfPlane(image, 0) : CVPixelBufferGetBaseAddress(image)
        else { return 0 }
        let width = planar ? CVPixelBufferGetWidthOfPlane(image, 0) : CVPixelBufferGetWidth(image)
        let height = planar ? CVPixelBufferGetHeightOfPlane(image, 0) : CVPixelBufferGetHeight(image)
        let stride = planar ? CVPixelBufferGetBytesPerRowOfPlane(image, 0) : CVPixelBufferGetBytesPerRow(image)
        let bgra = !planar && CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_32BGRA
        let bytes = base.assumingMemoryBound(to: UInt8.self)
        var sum = 0, count = 0
        for gy in 0..<18 {
            let y = (gy * 2 + 1) * height / 36
            for gx in 0..<32 {
                let x = (gx * 2 + 1) * width / 64
                if bgra {
                    let p = y * stride + x * 4
                    sum += (Int(bytes[p]) + Int(bytes[p + 1]) + Int(bytes[p + 2])) / 3
                } else {
                    sum += Int(bytes[y * stride + x])
                }
                count += 1
            }
        }
        return Double(sum) / Double(count)
    }
}
