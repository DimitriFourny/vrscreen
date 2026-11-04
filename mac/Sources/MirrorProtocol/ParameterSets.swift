import CoreMedia
import Foundation

/// Moves codec parameter sets between CoreMedia format descriptions and the wire.
public enum ParameterSets {
    public enum Error: Swift.Error, CustomStringConvertible {
        case unsupportedCodec(FourCharCode)
        case coreMedia(OSStatus, String)
        case missingParameterSets(VideoCodec)

        public var description: String {
            switch self {
            case .unsupportedCodec(let c): "unsupported codec fourcc 0x\(String(c, radix: 16))"
            case .coreMedia(let s, let what): "\(what) failed: OSStatus \(s)"
            case .missingParameterSets(let c): "\(c) parameter sets incomplete"
            }
        }
    }

    public struct Extracted: Equatable, Sendable {
        public var codec: VideoCodec
        /// Parameter-set NAL units (no start codes), ordered VPS, SPS, PPS / SPS, PPS.
        public var nals: [Data]
        /// Size of the NAL length prefix used by the sample data (normally 4).
        public var nalLengthSize: Int

        public var annexB: Data { NALUnits.annexB(joining: nals) }
    }

    /// Reads the parameter sets out of an H.264/HEVC format description (e.g. from
    /// VTCompressionSession output). Only VPS/SPS/PPS (HEVC) or SPS/PPS (H.264) are kept.
    public static func extract(from format: CMFormatDescription) throws(Error) -> Extracted {
        let subtype = CMFormatDescriptionGetMediaSubType(format)
        let codec: VideoCodec
        switch subtype {
        case kCMVideoCodecType_HEVC: codec = .hevc
        case kCMVideoCodecType_H264: codec = .h264
        default: throw .unsupportedCodec(subtype)
        }

        func get(_ index: Int, _ ptr: UnsafeMutablePointer<UnsafePointer<UInt8>?>?,
                 _ size: UnsafeMutablePointer<Int>?, _ count: UnsafeMutablePointer<Int>?,
                 _ header: UnsafeMutablePointer<Int32>?) -> OSStatus {
            switch codec {
            case .hevc:
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    format, parameterSetIndex: index, parameterSetPointerOut: ptr,
                    parameterSetSizeOut: size, parameterSetCountOut: count, nalUnitHeaderLengthOut: header)
            case .h264:
                CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format, parameterSetIndex: index, parameterSetPointerOut: ptr,
                    parameterSetSizeOut: size, parameterSetCountOut: count, nalUnitHeaderLengthOut: header)
            }
        }

        var count = 0
        var headerLength: Int32 = 0
        let countStatus = get(0, nil, nil, &count, &headerLength)
        guard countStatus == noErr else { throw .coreMedia(countStatus, "get parameter set count") }
        var nals: [Data] = []
        for index in 0..<count {
            var ptr: UnsafePointer<UInt8>?
            var size = 0
            let status = get(index, &ptr, &size, nil, nil)
            guard status == noErr, let ptr else { throw .coreMedia(status, "get parameter set \(index)") }
            nals.append(Data(bytes: ptr, count: size))
        }
        let order = NALUnits.parameterSetTypes(for: codec)
        let ordered = order.flatMap { t in nals.filter { NALUnits.type(of: $0, codec: codec) == t } }
        guard Set(ordered.compactMap { NALUnits.type(of: $0, codec: codec) }).count == order.count else {
            throw .missingParameterSets(codec)
        }
        return Extracted(codec: codec, nals: ordered, nalLengthSize: Int(headerLength))
    }

    /// Builds a format description from parameter sets as received on the wire
    /// (Annex-B, any order; non-parameter-set NAL units are ignored). Samples
    /// decoded with it must use 4-byte NAL length prefixes.
    public static func makeFormatDescription(codec: VideoCodec, annexB: Data) throws(Error) -> CMVideoFormatDescription {
        let all = NALUnits.split(annexB: annexB)
        let order = NALUnits.parameterSetTypes(for: codec)
        let nals = order.flatMap { t in all.filter { NALUnits.type(of: $0, codec: codec) == t } }
        guard Set(nals.compactMap { NALUnits.type(of: $0, codec: codec) }).count == order.count else {
            throw .missingParameterSets(codec)
        }

        // One contiguous buffer so all pointers stay valid for the CoreMedia call.
        let joined = [UInt8](nals.reduce(into: Data()) { $0.append($1) })
        let sizes = nals.map(\.count)
        var format: CMVideoFormatDescription?
        let status = joined.withUnsafeBufferPointer { buf -> OSStatus in
            var pointers: [UnsafePointer<UInt8>] = []
            var offset = 0
            for size in sizes {
                pointers.append(buf.baseAddress! + offset)
                offset += size
            }
            switch codec {
            case .hevc:
                return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: nals.count,
                    parameterSetPointers: pointers, parameterSetSizes: sizes,
                    nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &format)
            case .h264:
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: nals.count,
                    parameterSetPointers: pointers, parameterSetSizes: sizes,
                    nalUnitHeaderLength: 4, formatDescriptionOut: &format)
            }
        }
        guard status == noErr, let format else { throw .coreMedia(status, "create \(codec) format description") }
        return format
    }
}
