import CoreMedia
import CoreVideo
import Foundation
import Testing
import VideoToolbox
@testable import MirrorProtocol

/// Encodes one gray 420v frame with VideoToolbox and returns the output sample.
private func encodeOneFrame(codec: VideoCodec, width: Int, height: Int) throws -> CMSampleBuffer {
    var session: VTCompressionSession?
    let codecType = codec == .hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264
    try #require(VTCompressionSessionCreate(
        allocator: nil, width: Int32(width), height: Int32(height), codecType: codecType,
        encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
        outputCallback: nil, refcon: nil, compressionSessionOut: &session) == noErr)
    let s = try #require(session)
    defer { VTCompressionSessionInvalidate(s) }
    VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)

    var pixelBuffer: CVPixelBuffer?
    try #require(CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                     [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary,
                                     &pixelBuffer) == kCVReturnSuccess)
    let pb = try #require(pixelBuffer)
    CVPixelBufferLockBaseAddress(pb, [])
    for plane in 0..<2 {
        memset(CVPixelBufferGetBaseAddressOfPlane(pb, plane), plane == 0 ? 120 : 128,
               CVPixelBufferGetBytesPerRowOfPlane(pb, plane) * CVPixelBufferGetHeightOfPlane(pb, plane))
    }
    CVPixelBufferUnlockBaseAddress(pb, [])

    nonisolated(unsafe) var output: CMSampleBuffer?
    try #require(VTCompressionSessionEncodeFrame(
        s, imageBuffer: pb, presentationTimeStamp: CMTime(value: 0, timescale: 1_000_000), duration: .invalid,
        frameProperties: nil, infoFlagsOut: nil) { _, _, sample in output = sample } == noErr)
    VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
    return try #require(output)
}

/// Decodes a length-prefixed sample built the way the viewer does it; returns the image size.
private func decode(annexBAccessUnit: Data, format: CMVideoFormatDescription, codec: VideoCodec) throws -> (Int, Int)? {
    let nals = NALUnits.split(annexB: annexBAccessUnit).filter { !NALUnits.isParameterSet($0, codec: codec) }
    let bytes = NALUnits.lengthPrefixed(nals)
    var block: CMBlockBuffer?
    try #require(CMBlockBufferCreateWithMemoryBlock(
        allocator: nil, memoryBlock: nil, blockLength: bytes.count, blockAllocator: nil, customBlockSource: nil,
        offsetToData: 0, dataLength: bytes.count, flags: kCMBlockBufferAssureMemoryNowFlag,
        blockBufferOut: &block) == noErr)
    let bb = try #require(block)
    _ = bytes.withUnsafeBytes {
        CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: bb, offsetIntoDestination: 0, dataLength: bytes.count)
    }
    var size = bytes.count
    var sample: CMSampleBuffer?
    try #require(CMSampleBufferCreateReady(
        allocator: nil, dataBuffer: bb, formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 0,
        sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sample) == noErr)

    var session: VTDecompressionSession?
    try #require(VTDecompressionSessionCreate(allocator: nil, formatDescription: format, decoderSpecification: nil,
                                              imageBufferAttributes: nil, outputCallback: nil,
                                              decompressionSessionOut: &session) == noErr)
    let ds = try #require(session)
    defer { VTDecompressionSessionInvalidate(ds) }
    nonisolated(unsafe) var decodedSize: (Int, Int)?
    try #require(VTDecompressionSessionDecodeFrame(ds, sampleBuffer: try #require(sample), flags: [],
                                                   infoFlagsOut: nil) { status, _, image, _, _ in
        if status == noErr, let image {
            decodedSize = (CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image))
        }
    } == noErr)
    VTDecompressionSessionWaitForAsynchronousFrames(ds)
    return decodedSize
}

/// Sender path (VT sample → parameter sets + Annex-B) and viewer path (CONFIG →
/// format description, Annex-B → decoder) must interoperate for both codecs.
@Test(arguments: [VideoCodec.hevc, .h264])
func parameterSetsSurviveTheWire(codec: VideoCodec) throws {
    let sample = try encodeOneFrame(codec: codec, width: 320, height: 240)
    let format = try #require(CMSampleBufferGetFormatDescription(sample))
    let extracted = try ParameterSets.extract(from: format)

    #expect(extracted.codec == codec)
    #expect(extracted.nalLengthSize == 4)
    #expect(extracted.nals.compactMap { NALUnits.type(of: $0, codec: codec) } == NALUnits.parameterSetTypes(for: codec))
    #expect(NALUnits.split(annexB: extracted.annexB) == extracted.nals)

    // Viewer side: rebuild the format description from the CONFIG blob.
    let rebuilt = try ParameterSets.makeFormatDescription(codec: codec, annexB: extracted.annexB)
    let dims = CMVideoFormatDescriptionGetDimensions(rebuilt)
    #expect(dims.width == 320 && dims.height == 240)
    #expect(try ParameterSets.extract(from: rebuilt).nals == extracted.nals)

    // Keyframe as sent on the wire: parameter sets + converted access unit.
    let block = try #require(CMSampleBufferGetDataBuffer(sample))
    var avcc = Data(count: CMBlockBufferGetDataLength(block))
    _ = avcc.withUnsafeMutableBytes { CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: $0.count, destination: $0.baseAddress!) }
    let accessUnit = extracted.annexB + (try NALUnits.annexB(fromLengthPrefixed: avcc, lengthSize: extracted.nalLengthSize))
    let decoded = try decode(annexBAccessUnit: accessUnit, format: rebuilt, codec: codec)
    #expect(decoded?.0 == 320 && decoded?.1 == 240)
}

@Test func formatDescriptionRequiresAllParameterSets() throws {
    let sample = try encodeOneFrame(codec: .hevc, width: 128, height: 128)
    let sets = try ParameterSets.extract(from: try #require(CMSampleBufferGetFormatDescription(sample)))
    let withoutPPS = NALUnits.annexB(joining: Array(sets.nals.dropLast()))
    #expect(throws: ParameterSets.Error.self) {
        try ParameterSets.makeFormatDescription(codec: .hevc, annexB: withoutPPS)
    }
    // Order on the wire does not matter to the receiver.
    let shuffled = NALUnits.annexB(joining: sets.nals.reversed())
    let format = try ParameterSets.makeFormatDescription(codec: .hevc, annexB: shuffled)
    #expect(try ParameterSets.extract(from: format).nals == sets.nals)
}
