import CoreMedia
import CoreVideo
import Foundation
import MirrorProtocol
import VideoToolbox

/// Wraps a VTCompressionSession configured for low-latency screen streaming.
/// The session is (re)created whenever the input pixel buffer size changes.
/// Not thread-safe: call `encode` from one serial queue.
final class VideoEncoder {
    struct Output {
        var config: ConfigMessage
        var frame: FrameMessage
    }

    enum EncodeError: Error, CustomStringConvertible {
        case vt(OSStatus, String)
        case bitstream(String)

        var description: String {
            switch self {
            case .vt(let s, let what): "\(what) failed: OSStatus \(s)"
            case .bitstream(let what): what
            }
        }
    }

    let codec: VideoCodec
    let bitrate: Int
    let fps: Int
    let keyframeIntervalSeconds: Double

    private var session: VTCompressionSession?
    private var width = 0
    private var height = 0
    private var lastPTSMicros: Int64 = -1
    /// Logged once per session creation.
    private(set) var modeDescription = "not started"

    init(codec: VideoCodec, bitrate: Int, fps: Int, keyframeIntervalSeconds: Double) {
        self.codec = codec
        self.bitrate = bitrate
        self.fps = fps
        self.keyframeIntervalSeconds = keyframeIntervalSeconds
    }

    deinit { invalidate() }

    func invalidate() {
        guard let session else { return }
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
        self.session = nil
    }

    /// Creates (or recreates, on a size change) the session for buffers like this one.
    /// Called for every capture so the first client does not wait for session setup.
    func prepare(for pixelBuffer: CVPixelBuffer) throws {
        let w = CVPixelBufferGetWidth(pixelBuffer)
        let h = CVPixelBufferGetHeight(pixelBuffer)
        guard session == nil || w != width || h != height else { return }
        invalidate()
        session = try makeSession(width: w, height: h)
        width = w
        height = h
    }

    /// Submits one frame. `completion` runs on a VideoToolbox thread with the
    /// encoded output, `nil` if the encoder dropped the frame, or an error.
    func encode(_ pixelBuffer: CVPixelBuffer, captureMicros: UInt64, forceKeyframe: Bool,
                completion: @escaping (Result<Output?, Error>) -> Void) {
        do {
            try prepare(for: pixelBuffer)
        } catch {
            completion(.failure(error))
            return
        }
        guard let session else { return }

        // Encoder timestamps must strictly increase; a re-encode of the same capture
        // (keyframe for a newly connected client) reuses the capture time.
        let ptsMicros = max(Int64(captureMicros), lastPTSMicros + 1)
        lastPTSMicros = ptsMicros
        let properties: CFDictionary? = forceKeyframe
            ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue] as CFDictionary : nil
        let codec = codec
        let status = VTCompressionSessionEncodeFrame(
            session, imageBuffer: pixelBuffer,
            presentationTimeStamp: CMTime(value: ptsMicros, timescale: 1_000_000),
            duration: .invalid, frameProperties: properties, infoFlagsOut: nil
        ) { status, flags, sampleBuffer in
            guard status == noErr else {
                completion(.failure(EncodeError.vt(status, "encode")))
                return
            }
            guard let sampleBuffer, !flags.contains(.frameDropped) else {
                completion(.success(nil))
                return
            }
            completion(Result { try Self.makeOutput(sampleBuffer, codec: codec, captureMicros: captureMicros) })
        }
        if status != noErr {
            completion(.failure(EncodeError.vt(status, "VTCompressionSessionEncodeFrame")))
        }
    }

    private static func makeOutput(_ sample: CMSampleBuffer, codec: VideoCodec, captureMicros: UInt64) throws -> Output {
        guard let format = CMSampleBufferGetFormatDescription(sample),
              let block = CMSampleBufferGetDataBuffer(sample) else {
            throw EncodeError.bitstream("encoded sample without format or data")
        }
        let sets = try ParameterSets.extract(from: format)
        let dims = CMVideoFormatDescriptionGetDimensions(format)

        let length = CMBlockBufferGetDataLength(block)
        var avcc = Data(count: length)
        let copyStatus = avcc.withUnsafeMutableBytes {
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!)
        }
        guard copyStatus == kCMBlockBufferNoErr else { throw EncodeError.vt(copyStatus, "CMBlockBufferCopyDataBytes") }

        var keyframe = true
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]],
           let first = attachments.first, let notSync = first[kCMSampleAttachmentKey_NotSync] as? Bool {
            keyframe = !notSync
        }

        let parameterSets = sets.annexB
        var accessUnit = Data()
        let converted = try NALUnits.annexB(fromLengthPrefixed: avcc, lengthSize: sets.nalLengthSize)
        if keyframe {
            accessUnit.reserveCapacity(parameterSets.count + converted.count)
            accessUnit.append(parameterSets)
        }
        accessUnit.append(converted)

        return Output(
            config: ConfigMessage(codec: codec, width: UInt32(dims.width), height: UInt32(dims.height),
                                  parameterSets: parameterSets),
            frame: FrameMessage(isKeyframe: keyframe, captureTimeMicros: captureMicros, accessUnit: accessUnit))
    }

    private func makeSession(width: Int, height: Int) throws -> VTCompressionSession {
        let codecType = codec == .hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264
        func create(lowLatency: Bool) -> (OSStatus, VTCompressionSession?) {
            var spec: [CFString: Any] = [kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: true]
            if lowLatency { spec[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true }
            var s: VTCompressionSession?
            let status = VTCompressionSessionCreate(
                allocator: kCFAllocatorDefault, width: Int32(width), height: Int32(height),
                codecType: codecType, encoderSpecification: spec as CFDictionary,
                imageBufferAttributes: nil, compressedDataAllocator: nil,
                outputCallback: nil, refcon: nil, compressionSessionOut: &s)
            return (status, s)
        }

        // Low-latency rate control selects Apple's real-time encoder
        // (…videoencoder.hevc.rtvc / .h264.rtvc, hardware-backed on Apple silicon).
        var lowLatency = true
        var (status, created) = create(lowLatency: true)
        if status != noErr || created == nil {
            log("encoder: low-latency rate control rejected for \(codec) (OSStatus \(status)); using standard rate control")
            lowLatency = false
            (status, created) = create(lowLatency: false)
        }
        guard status == noErr, let session = created else { throw EncodeError.vt(status, "VTCompressionSessionCreate") }

        func set(_ key: CFString, _ value: CFTypeRef, required: Bool = true) throws {
            let s = VTSessionSetProperty(session, key: key, value: value)
            if s != noErr {
                if required { throw EncodeError.vt(s, "set \(key)") }
                log("encoder: property \(key) not supported (OSStatus \(s)), ignored")
            }
        }

        try set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        try set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        let profile = codec == .hevc
            ? kVTProfileLevel_HEVC_Main_AutoLevel
            : (lowLatency ? kVTProfileLevel_H264_ConstrainedHigh_AutoLevel : kVTProfileLevel_H264_High_AutoLevel)
        try set(kVTCompressionPropertyKey_ProfileLevel, profile)
        try set(kVTCompressionPropertyKey_AverageBitRate, bitrate as CFNumber)
        try set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFNumber, required: false)
        try set(kVTCompressionPropertyKey_MaxKeyFrameInterval,
                max(1, Int(Double(fps) * keyframeIntervalSeconds)) as CFNumber)
        try set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, keyframeIntervalSeconds as CFNumber)
        // BT.709 VUI, matching the BT.709 matrix requested from ScreenCaptureKit.
        try set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2, required: false)
        try set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2, required: false)
        try set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2, required: false)
        if codec == .h264 {
            try set(kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CABAC, required: false)
        }
        if !lowLatency {
            // Bound short-term bursts when the low-latency rate controller is unavailable.
            let bytesPerSecond = Double(bitrate) / 8 * 1.5
            try set(kVTCompressionPropertyKey_DataRateLimits, [bytesPerSecond, 1.0] as CFArray, required: false)
        }
        VTCompressionSessionPrepareToEncodeFrames(session)

        var encoderID: CFString?
        _ = withUnsafeMutablePointer(to: &encoderID) {
            VTSessionCopyProperty(session, key: kVTCompressionPropertyKey_EncoderID,
                                  allocator: nil, valueOut: UnsafeMutableRawPointer($0))
        }
        modeDescription = "\(codec) \(width)x\(height), \(lowLatency ? "low-latency" : "standard") rate control "
            + "(encoder \(encoderID.map { $0 as String } ?? "unknown")), "
            + "\(String(format: "%.1f", Double(bitrate) / 1e6)) Mbit/s, keyframe every \(keyframeIntervalSeconds) s max"
        log("encoder: \(modeDescription)")
        return session
    }
}
