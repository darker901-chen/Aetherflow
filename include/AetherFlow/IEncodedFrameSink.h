#pragma once

#include <cstddef>
#include <cstdint>

namespace AetherFlow {

// One encoded H.264 access unit (Annex B byte stream, exactly one frame) as
// it leaves an encoder backend.
struct EncodedAccessUnit {
    const uint8_t* data = nullptr;  // valid only for the duration of the call
    size_t size = 0;
    uint64_t pts90k = 0;            // presentation timestamp, 90 kHz clock
    bool keyframe = false;          // IDR (or intra) — safe stream entry point
};

// Optional tap on an encoder's compressed output, used by the SRT/MPEG-TS
// live output stage. Contract:
// - Called from the encoder's drain/writer-side thread, never the
//   capture/producer thread.
// - Implementations MUST NOT block: copy what they need and return. Anything
//   slow (muxing, network I/O) belongs on the implementation's own thread.
// - `data` is owned by the encoder and is invalid after the call returns.
class IEncodedFrameSink {
public:
    virtual ~IEncodedFrameSink() = default;
    virtual void OnEncodedAccessUnit(const EncodedAccessUnit& au) = 0;
};

}  // namespace AetherFlow
