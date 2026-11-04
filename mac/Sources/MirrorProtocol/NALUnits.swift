import Foundation

/// Helpers for converting between length-prefixed NAL units (AVCC/HVCC, what
/// VideoToolbox produces and consumes) and Annex-B byte streams (what the wire carries).
public enum NALUnits {
    public static let startCode: [UInt8] = [0, 0, 0, 1]

    public enum ConversionError: Error, Equatable {
        case unsupportedLengthSize(Int)
        case truncated(offset: Int)
    }

    /// Converts length-prefixed NAL units to Annex-B (4-byte start codes).
    public static func annexB(fromLengthPrefixed data: Data, lengthSize: Int = 4) throws(ConversionError) -> Data {
        guard [1, 2, 4].contains(lengthSize) else { throw .unsupportedLengthSize(lengthSize) }
        var out = Data(capacity: data.count + 16)
        var offset = data.startIndex
        while offset < data.endIndex {
            guard data.endIndex - offset >= lengthSize else { throw .truncated(offset: offset - data.startIndex) }
            var length = 0
            for i in 0..<lengthSize { length = length << 8 | Int(data[offset + i]) }
            offset += lengthSize
            guard data.endIndex - offset >= length else { throw .truncated(offset: offset - data.startIndex) }
            out.append(contentsOf: startCode)
            out.append(data[offset..<offset + length])
            offset += length
        }
        return out
    }

    /// Splits an Annex-B stream (3- or 4-byte start codes) into NAL units without
    /// start codes. Bytes before the first start code are ignored; trailing zero
    /// bytes of each NAL unit (trailing_zero_8bits) are dropped.
    public static func split(annexB data: Data) -> [Data] {
        var nals: [Data] = []
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let bytes = raw.bindMemory(to: UInt8.self)
            let n = bytes.count
            // Index just after each 00 00 01.
            var starts: [Int] = []
            var i = 0
            while i + 2 < n {
                if bytes[i + 2] > 1 {
                    i += 3
                } else if bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 {
                    starts.append(i + 3)
                    i += 3
                } else {
                    i += 1
                }
            }
            for (k, start) in starts.enumerated() {
                var end = k + 1 < starts.count ? starts[k + 1] - 3 : n
                while end > start && bytes[end - 1] == 0 { end -= 1 }
                if end > start {
                    nals.append(Data(UnsafeBufferPointer(rebasing: bytes[start..<end])))
                }
            }
        }
        return nals
    }

    /// Joins NAL units into Annex-B with 4-byte start codes.
    public static func annexB(joining nals: [Data]) -> Data {
        var out = Data(capacity: nals.reduce(0) { $0 + $1.count + 4 })
        for nal in nals {
            out.append(contentsOf: startCode)
            out.append(nal)
        }
        return out
    }

    /// Joins NAL units with 4-byte big-endian length prefixes (for VideoToolbox).
    public static func lengthPrefixed(_ nals: [Data]) -> Data {
        var out = Data(capacity: nals.reduce(0) { $0 + $1.count + 4 })
        for nal in nals {
            out.appendBigEndian(UInt32(nal.count))
            out.append(nal)
        }
        return out
    }

    /// NAL unit type from the first header byte.
    public static func type(of nal: Data, codec: VideoCodec) -> UInt8? {
        guard let first = nal.first else { return nil }
        switch codec {
        case .h264: return first & 0x1F
        case .hevc: return (first >> 1) & 0x3F
        }
    }

    /// Parameter-set NAL types in the order they must appear: HEVC VPS(32), SPS(33),
    /// PPS(34); H.264 SPS(7), PPS(8).
    public static func parameterSetTypes(for codec: VideoCodec) -> [UInt8] {
        switch codec {
        case .h264: [7, 8]
        case .hevc: [32, 33, 34]
        }
    }

    public static func isParameterSet(_ nal: Data, codec: VideoCodec) -> Bool {
        guard let t = type(of: nal, codec: codec) else { return false }
        return parameterSetTypes(for: codec).contains(t)
    }
}
