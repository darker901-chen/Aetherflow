// Unit tests for the AnnexB NAL helpers used by the SRT/MPEG-TS output stage
// (mid-stream join support: SPS/PPS detection, extraction, and the keyframe
// prepend decision). Pure logic; builds without FFmpeg or D3D11.

#include "AetherFlow/streaming/AnnexB.h"
#include "test_assert.h"

#include <cstdint>
#include <vector>

using AetherFlow::annexb::ContainsSpsPps;
using AetherFlow::annexb::ExtractSpsPps;
using AetherFlow::annexb::NalUnit;
using AetherFlow::annexb::ScanNalUnits;

namespace {

void Append(std::vector<uint8_t>* out, std::initializer_list<uint8_t> bytes) {
    out->insert(out->end(), bytes.begin(), bytes.end());
}

// NAL header bytes as emitted by real encoders: 0x67 SPS, 0x68 PPS, 0x65 IDR
// slice, 0x41 non-IDR slice, 0x06 SEI.
std::vector<uint8_t> MakeIdrAuWithSpsPps() {
    std::vector<uint8_t> au;
    Append(&au, {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F});  // SPS (4-byte SC)
    Append(&au, {0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80});  // PPS (4-byte SC)
    Append(&au, {0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x33});  // IDR (3-byte SC)
    return au;
}

std::vector<uint8_t> MakeIdrAuWithoutSpsPps() {
    std::vector<uint8_t> au;
    Append(&au, {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x33});
    return au;
}

}  // namespace

int main() {
    // --- Degenerate inputs never crash and never report parameter sets.
    CHECK(ScanNalUnits(nullptr, 0).empty());
    CHECK(ScanNalUnits(nullptr, 128).empty());
    const uint8_t tiny[] = {0x00, 0x00, 0x01};
    CHECK(ScanNalUnits(tiny, sizeof(tiny)).empty());  // start code with no payload byte counts nothing usable
    CHECK(!ContainsSpsPps(nullptr, 0));

    // --- Full IDR access unit: three NALs, correct types, correct spans.
    const std::vector<uint8_t> idrAu = MakeIdrAuWithSpsPps();
    const std::vector<NalUnit> nals = ScanNalUnits(idrAu.data(), idrAu.size());
    CHECK_MSG(nals.size() == 3, "expected SPS + PPS + IDR");
    if (nals.size() == 3) {
        CHECK(nals[0].type == 7);
        CHECK(nals[1].type == 8);
        CHECK(nals[2].type == 5);
        CHECK(nals[0].offset == 0);
        CHECK(nals[0].size == 8);       // 4-byte SC + 4 payload bytes
        CHECK(nals[1].offset == 8);
        CHECK(nals[1].size == 8);
        CHECK(nals[2].offset == 16);
        CHECK(nals[2].size == idrAu.size() - 16);  // runs to end of buffer
    }
    CHECK(ContainsSpsPps(idrAu.data(), idrAu.size()));

    // --- IDR without parameter sets: the prepend path must trigger.
    const std::vector<uint8_t> bareIdr = MakeIdrAuWithoutSpsPps();
    CHECK(!ContainsSpsPps(bareIdr.data(), bareIdr.size()));
    const std::vector<NalUnit> bareNals = ScanNalUnits(bareIdr.data(), bareIdr.size());
    CHECK(bareNals.size() == 1);
    if (bareNals.size() == 1) {
        CHECK(bareNals[0].type == 5);
    }

    // --- SPS alone (no PPS) must NOT count as "has parameter sets".
    std::vector<uint8_t> spsOnly;
    Append(&spsOnly, {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F});
    CHECK(!ContainsSpsPps(spsOnly.data(), spsOnly.size()));
    std::vector<uint8_t> spsOnlyOut;
    CHECK(!ExtractSpsPps(spsOnly.data(), spsOnly.size(), &spsOnlyOut));

    // --- Extraction returns exactly the SPS+PPS bytes (start codes included),
    //     i.e. the block the streaming sink prepends on mid-stream keyframes.
    std::vector<uint8_t> extracted;
    CHECK(ExtractSpsPps(idrAu.data(), idrAu.size(), &extracted));
    CHECK(extracted.size() == 16);
    CHECK(std::vector<uint8_t>(idrAu.begin(), idrAu.begin() + 16) == extracted);

    // Prepending the extracted block onto a bare IDR yields a joinable AU.
    std::vector<uint8_t> joined = extracted;
    joined.insert(joined.end(), bareIdr.begin(), bareIdr.end());
    CHECK(ContainsSpsPps(joined.data(), joined.size()));
    const std::vector<NalUnit> joinedNals = ScanNalUnits(joined.data(), joined.size());
    CHECK(joinedNals.size() == 3);

    // --- Mixed non-IDR AU (SEI + slice), 3-byte start codes.
    std::vector<uint8_t> pAu;
    Append(&pAu, {0x00, 0x00, 0x01, 0x06, 0x05, 0x10});        // SEI
    Append(&pAu, {0x00, 0x00, 0x01, 0x41, 0x9A, 0x02, 0x03});  // non-IDR slice
    const std::vector<NalUnit> pNals = ScanNalUnits(pAu.data(), pAu.size());
    CHECK(pNals.size() == 2);
    if (pNals.size() == 2) {
        CHECK(pNals[0].type == 6);
        CHECK(pNals[1].type == 1);
        CHECK(pNals[0].size == 6);
    }
    CHECK(!ContainsSpsPps(pAu.data(), pAu.size()));

    // --- Payload bytes containing 00 00 0x sequences must not split a NAL
    //     (only real 00 00 01 / 00 00 00 01 patterns do).
    std::vector<uint8_t> tricky;
    Append(&tricky, {0x00, 0x00, 0x01, 0x41, 0x00, 0x00, 0x02, 0x77, 0x00, 0x00});
    const std::vector<NalUnit> trickyNals = ScanNalUnits(tricky.data(), tricky.size());
    CHECK(trickyNals.size() == 1);
    if (trickyNals.size() == 1) {
        CHECK(trickyNals[0].size == tricky.size());
    }

    return aetherflow_test::Summary();
}
