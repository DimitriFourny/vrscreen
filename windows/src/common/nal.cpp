#include "nal.h"

#include <array>

namespace vr::nal {

std::vector<Unit> split(const uint8_t* data, size_t size) {
    std::vector<Unit> units;
    auto findStart = [&](size_t from) -> size_t {
        for (size_t k = from; k + 3 <= size; ++k) {
            if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) return k;
        }
        return size;
    };
    size_t sc = findStart(0);
    while (sc < size) {
        const size_t begin = sc + 3;
        const size_t nextSc = findStart(begin);
        size_t end = nextSc;
        while (end > begin && data[end - 1] == 0) --end;  // zero_byte of a 4-byte start code / trailing zeros
        if (end > begin) units.push_back({data + begin, end - begin});
        sc = nextSc;
    }
    return units;
}

int type(proto::Codec codec, const Unit& unit) {
    if (unit.size == 0) return -1;
    return codec == proto::Codec::HEVC ? (unit.data[0] >> 1) & 0x3F : unit.data[0] & 0x1F;
}

bool isParameterSet(proto::Codec codec, int t) {
    return codec == proto::Codec::HEVC ? (t == 32 || t == 33 || t == 34) : (t == 7 || t == 8);
}

bool isIdr(proto::Codec codec, int t) {
    return codec == proto::Codec::HEVC ? (t == 19 || t == 20) : t == 5;
}

bool isAccessUnitDelimiter(proto::Codec codec, int t) { return codec == proto::Codec::HEVC ? t == 35 : t == 9; }

static bool isSlice(proto::Codec codec, int t) { return codec == proto::Codec::HEVC ? t <= 31 : (t >= 1 && t <= 5); }

static void appendUnit(proto::Bytes& out, const uint8_t* data, size_t size) {
    static constexpr uint8_t kStart[4] = {0, 0, 0, 1};
    out.insert(out.end(), kStart, kStart + 4);
    out.insert(out.end(), data, data + size);
}

bool rebuild(proto::Codec codec, const uint8_t* data, size_t size, const proto::Bytes& cachedParameterSets,
             AccessUnit& out, std::string& error) {
    out = AccessUnit{};
    const std::vector<Unit> units = split(data, size);
    // Parameter sets in protocol order; the last of each type wins.
    const std::array<int, 3> order = codec == proto::Codec::HEVC ? std::array<int, 3>{32, 33, 34}
                                                                 : std::array<int, 3>{7, 8, -1};
    std::array<const Unit*, 3> sets{};
    bool hasSlice = false;
    for (const Unit& u : units) {
        const int t = type(codec, u);
        for (size_t k = 0; k < order.size(); ++k) {
            if (order[k] == t) sets[k] = &u;
        }
        if (isIdr(codec, t)) out.keyframe = true;
        if (isSlice(codec, t)) hasSlice = true;
    }
    if (!hasSlice) {
        error = "encoder output without a picture slice";
        return false;
    }
    bool haveAll = true;
    for (size_t k = 0; k < order.size(); ++k) {
        if (order[k] >= 0 && !sets[k]) haveAll = false;
    }
    if (haveAll) {
        for (size_t k = 0; k < order.size(); ++k) {
            if (sets[k]) appendUnit(out.parameterSets, sets[k]->data, sets[k]->size);
        }
    }
    out.data.reserve(size + 64);
    if (out.keyframe) {
        const proto::Bytes& ps = haveAll ? out.parameterSets : cachedParameterSets;
        if (ps.empty()) {
            error = "keyframe without parameter sets";
            return false;
        }
        out.data.insert(out.data.end(), ps.begin(), ps.end());
    }
    for (const Unit& u : units) {
        const int t = type(codec, u);
        if (isAccessUnitDelimiter(codec, t) || isParameterSet(codec, t)) continue;
        appendUnit(out.data, u.data, u.size);
    }
    return true;
}

}  // namespace vr::nal
