#include "audio_chunker.h"

#include <algorithm>

namespace vr {
namespace {
// SPEAKER_* from ksmedia.h, repeated here so this file needs no Windows headers (tests).
constexpr uint32_t kFL = 0x1, kFR = 0x2, kFC = 0x4, kLFE = 0x8, kBL = 0x10, kBR = 0x20, kFLC = 0x40, kFRC = 0x80,
                   kBC = 0x100, kSL = 0x200, kSR = 0x400;

uint32_t defaultMask(int channels) {
    switch (channels) {
        case 1: return kFC;
        case 2: return kFL | kFR;
        case 4: return kFL | kFR | kBL | kBR;
        case 6: return kFL | kFR | kFC | kLFE | kBL | kBR;
        case 8: return kFL | kFR | kFC | kLFE | kBL | kBR | kSL | kSR;
        default: return 0;
    }
}
}  // namespace

DownmixMatrix makeDownmix(int channels, uint32_t mask) {
    DownmixMatrix m;
    m.left.assign(size_t(channels), 0.0f);
    m.right.assign(size_t(channels), 0.0f);
    if (channels == 1) {
        m.left[0] = m.right[0] = 1.0f;
        return m;
    }
    if (mask == 0) mask = defaultMask(channels);
    constexpr float kHalf = 0.7071f;
    // Channels appear in the stream in ascending SPEAKER_* bit order.
    int index = 0;
    for (uint32_t bit = 1; bit != 0 && index < channels; bit <<= 1) {
        if (!(mask & bit)) continue;
        float l = 0, r = 0;
        switch (bit) {
            case kFL: l = 1; break;
            case kFR: r = 1; break;
            case kFC: case kBC: l = r = kHalf; break;
            case kLFE: break;
            case kBL: case kSL: case kFLC: l = kHalf; break;
            case kBR: case kSR: case kFRC: r = kHalf; break;
            default: break;
        }
        m.left[size_t(index)] = l;
        m.right[size_t(index)] = r;
        ++index;
    }
    if (index < channels && mask != 0 && index == 0) {
        // Unknown mask: first two channels as left/right.
        m.left[0] = 1;
        m.right[std::min(1, channels - 1)] = 1;
    }
    // Normalize so a full-scale signal in every channel does not clip too hard.
    float sumL = 0, sumR = 0;
    for (int c = 0; c < channels; ++c) {
        sumL += m.left[size_t(c)];
        sumR += m.right[size_t(c)];
    }
    const float norm = std::max({sumL, sumR, 1.0f});
    if (norm > 1.0f && channels > 2) {
        for (int c = 0; c < channels; ++c) {
            m.left[size_t(c)] /= norm * 0.5f + 0.5f;
            m.right[size_t(c)] /= norm * 0.5f + 0.5f;
        }
    }
    return m;
}

}  // namespace vr
