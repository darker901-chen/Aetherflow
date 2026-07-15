#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AetherFlow {
namespace annexb {

// Minimal H.264 Annex B helpers for the SRT/MPEG-TS output stage. Pure logic
// (no FFmpeg, no platform headers) so they build and unit-test everywhere.
//
// A mid-stream viewer can only start decoding at a keyframe that carries
// SPS/PPS. NVENC/oneVPL emit SPS+PPS with the *first* IDR but not necessarily
// with later ones, so the streaming sink caches the parameter sets and
// prepends them to keyframe access units that lack them.

struct NalUnit {
    size_t offset = 0;   // offset of the start code within the buffer
    size_t size = 0;     // start code + payload, up to the next start code
    uint8_t type = 0;    // nal_unit_type (low 5 bits of the NAL header byte)
};

// Scans an Annex B buffer for 00 00 01 / 00 00 00 01 start codes. Each
// returned unit spans from its start code to the next start code (or end of
// buffer).
std::vector<NalUnit> ScanNalUnits(const uint8_t* data, size_t size);

// True when the buffer contains both an SPS (type 7) and a PPS (type 8).
bool ContainsSpsPps(const uint8_t* data, size_t size);

// Appends every SPS/PPS NAL unit (start codes included, stream order) to
// *out. Returns true only when at least one SPS and one PPS were found.
bool ExtractSpsPps(const uint8_t* data, size_t size, std::vector<uint8_t>* out);

}  // namespace annexb
}  // namespace AetherFlow
