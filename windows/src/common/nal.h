// Annex-B helpers: split a byte stream into NAL units and rebuild the access units the protocol
// wants (4-byte start codes, no access unit delimiters, keyframes start with the parameter sets).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"

namespace vr::nal {

struct Unit {
    const uint8_t* data;  // first byte of the NAL header (after the start code)
    size_t size;
};

// NAL units of an Annex-B stream (3- or 4-byte start codes). Trailing zero bytes of a unit
// (part of the next 4-byte start code) are not included.
std::vector<Unit> split(const uint8_t* data, size_t size);

int type(proto::Codec codec, const Unit& unit);
bool isParameterSet(proto::Codec codec, int type);
bool isIdr(proto::Codec codec, int type);
bool isAccessUnitDelimiter(proto::Codec codec, int type);

struct AccessUnit {
    bool keyframe = false;
    // Annex-B parameter sets found in this unit (VPS SPS PPS / SPS PPS, in that order), empty
    // when the unit carries none.
    proto::Bytes parameterSets;
    // The access unit to send: 4-byte start codes, AUDs dropped; for keyframes the parameter
    // sets come first.
    proto::Bytes data;
};

// Rebuilds one encoder output. `cachedParameterSets` (Annex-B) is used for keyframes that do
// not repeat them. Returns false with `error` set when the output has no slice or a keyframe
// has no parameter sets at all.
bool rebuild(proto::Codec codec, const uint8_t* data, size_t size, const proto::Bytes& cachedParameterSets,
             AccessUnit& out, std::string& error);

}  // namespace vr::nal
