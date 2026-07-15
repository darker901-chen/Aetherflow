#include "AetherFlow/streaming/AnnexB.h"

namespace AetherFlow {
namespace annexb {

std::vector<NalUnit> ScanNalUnits(const uint8_t* data, size_t size) {
    std::vector<NalUnit> out;
    if (!data || size < 4) {
        return out;
    }
    constexpr size_t kNone = static_cast<size_t>(-1);
    size_t prevStart = kNone;
    uint8_t prevType = 0;
    size_t i = 0;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            size_t startCodeLen = 0;
            if (data[i + 2] == 1) {
                startCodeLen = 3;
            } else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                startCodeLen = 4;
            }
            if (startCodeLen != 0) {
                if (prevStart != kNone) {
                    out.push_back({prevStart, i - prevStart, prevType});
                }
                prevStart = i;
                const size_t payload = i + startCodeLen;
                prevType = (payload < size) ? static_cast<uint8_t>(data[payload] & 0x1F) : 0;
                i = payload;
                continue;
            }
        }
        ++i;
    }
    if (prevStart != kNone) {
        out.push_back({prevStart, size - prevStart, prevType});
    }
    return out;
}

bool ContainsSpsPps(const uint8_t* data, size_t size) {
    bool sawSps = false;
    bool sawPps = false;
    for (const NalUnit& nal : ScanNalUnits(data, size)) {
        if (nal.type == 7) sawSps = true;
        if (nal.type == 8) sawPps = true;
        if (sawSps && sawPps) return true;
    }
    return false;
}

bool ExtractSpsPps(const uint8_t* data, size_t size, std::vector<uint8_t>* out) {
    if (!out) {
        return false;
    }
    bool sawSps = false;
    bool sawPps = false;
    for (const NalUnit& nal : ScanNalUnits(data, size)) {
        if (nal.type == 7 || nal.type == 8) {
            out->insert(out->end(), data + nal.offset, data + nal.offset + nal.size);
            if (nal.type == 7) sawSps = true;
            if (nal.type == 8) sawPps = true;
        }
    }
    return sawSps && sawPps;
}

}  // namespace annexb
}  // namespace AetherFlow
