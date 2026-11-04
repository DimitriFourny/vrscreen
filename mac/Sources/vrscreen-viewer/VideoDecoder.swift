import CoreMedia
import CoreVideo
import Foundation
import MirrorProtocol
import VideoToolbox

/// Decodes the wire stream (CONFIG + Annex-B FRAMEs) with VTDecompressionSession into
/// BGRA IOSurface-backed pixel buffers. Not thread-safe: use from one serial queue.
final class VideoDecoder {
    /// Called on a VideoToolbox thread: decoded image, capture time (sender clock, µs),
    /// and the local host time (µs) at which decoding finished.
    var onDecoded: ((CVPixelBuffer, UInt64, UInt64) -> Void)?

    private(set) var config: ConfigMessage?
    private var format: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    private var waitingForKeyframe = true
    private(set) var droppedBeforeKeyframe = 0

    deinit { reset() }

    /// Forget the stream (e.g. after a disconnect); the next stream starts with CONFIG.
    func reset() {
        if let session {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        format = nil
        config = nil
        waitingForKeyframe = true
    }

    func configure(_ newConfig: ConfigMessage) throws {
        guard newConfig != config else { return }
        let newFormat = try ParameterSets.makeFormatDescription(codec: newConfig.codec, annexB: newConfig.parameterSets)
        if let session, VTDecompressionSessionCanAcceptFormatDescription(session, formatDescription: newFormat) {
            format = newFormat
        } else {
            if let session {
                VTDecompressionSessionWaitForAsynchronousFrames(session)
                VTDecompressionSessionInvalidate(session)
            }
            session = nil
            format = newFormat
            session = try makeSession(format: newFormat)
        }
        config = newConfig
        waitingForKeyframe = true
        log("decoder: \(newConfig.codec) \(newConfig.width)x\(newConfig.height)")
    }

    func decode(_ frame: FrameMessage) {
        guard let session, let format, let codec = config?.codec else { return }
        if waitingForKeyframe {
            guard frame.isKeyframe else {
                droppedBeforeKeyframe += 1
                return
            }
            waitingForKeyframe = false
        }
        // Parameter sets travel in CONFIG (and are repeated on keyframes); the
        // decoder gets them from the format description, not the sample.
        let nals = NALUnits.split(annexB: frame.accessUnit).filter { !NALUnits.isParameterSet($0, codec: codec) }
        let sampleData = NALUnits.lengthPrefixed(nals)
        do {
            let sample = try makeSampleBuffer(sampleData, format: format, captureMicros: frame.captureTimeMicros)
            let captureMicros = frame.captureTimeMicros
            // Asynchronous so the network queue (and PONG timing) never waits on a 4K decode.
            let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample,
                                                           flags: ._EnableAsynchronousDecompression,
                                                           infoFlagsOut: nil) { [weak self] status, _, image, _, _ in
                let doneMicros = HostClock.nowMicros()
                guard status == noErr, let image else {
                    if status != noErr { log("decoder: frame decode failed: OSStatus \(status)") }
                    return
                }
                self?.onDecoded?(image, captureMicros, doneMicros)
            }
            if status != noErr {
                log("decoder: VTDecompressionSessionDecodeFrame failed: OSStatus \(status); waiting for keyframe")
                waitingForKeyframe = true
            }
        } catch {
            log("decoder: \(error)")
            waitingForKeyframe = true
        }
    }

    private func makeSession(format: CMVideoFormatDescription) throws -> VTDecompressionSession {
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey: [CFString: Any]() as CFDictionary,
        ]
        var session: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault, formatDescription: format, decoderSpecification: nil,
            imageBufferAttributes: attributes as CFDictionary, outputCallback: nil,
            decompressionSessionOut: &session)
        guard status == noErr, let session else { throw OptionError("VTDecompressionSessionCreate failed: OSStatus \(status)") }
        VTSessionSetProperty(session, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        return session
    }

    private func makeSampleBuffer(_ data: Data, format: CMVideoFormatDescription, captureMicros: UInt64) throws -> CMSampleBuffer {
        var block: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: data.count,
            blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0,
            dataLength: data.count, flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block)
        guard status == kCMBlockBufferNoErr, let block else { throw OptionError("CMBlockBufferCreate failed: \(status)") }
        status = data.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0,
                                          dataLength: data.count)
        }
        guard status == kCMBlockBufferNoErr else { throw OptionError("CMBlockBufferReplaceDataBytes failed: \(status)") }

        var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: HostClock.time(micros: captureMicros),
                                        decodeTimeStamp: .invalid)
        var size = data.count
        var sample: CMSampleBuffer?
        status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format, sampleCount: 1,
            sampleTimingEntryCount: 1, sampleTimingArray: &timing, sampleSizeEntryCount: 1,
            sampleSizeArray: &size, sampleBufferOut: &sample)
        guard status == noErr, let sample else { throw OptionError("CMSampleBufferCreateReady failed: \(status)") }
        return sample
    }
}
