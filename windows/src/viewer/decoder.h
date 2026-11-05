// Software decoding of the stream for the test viewer (Media Foundation decoder MFTs, NV12 in
// system memory), plus PNG snapshots through WIC.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace vr {

struct DecodedPicture {
    int width = 0, height = 0;
    std::vector<uint8_t> nv12;  // width*height luma + interleaved chroma
    double meanLuma = 0;        // 0..255 (limited range values)
    uint64_t micros = 0;        // the FRAME's captureTimeMicros (decoders may output a frame late)
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    // (Re)configures for a CONFIG. False with `error` when no decoder exists for the codec.
    bool configure(const proto::ConfigMessage& config, std::string& error);
    // Feeds one access unit; returns decoded pictures (0 or more).
    bool decode(const proto::Bytes& accessUnit, uint64_t micros, std::vector<DecodedPicture>& out, std::string& error);
    std::string name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes NV12 (BT.709 limited range) as an RGB PNG.
bool writePng(const DecodedPicture& picture, const std::wstring& path, std::string& error);

}  // namespace vr
