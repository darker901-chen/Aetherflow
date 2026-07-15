#pragma once

// macOS H.264 encoder interface (VideoToolbox + AVAssetWriter).
//
// This header is the boundary between AetherFlow's macOS scene-runtime layer
// (MacosPlatformShim) and the platform encoder. It is intentionally pure C++
// and exposes Core Video's CVPixelBufferRef as the input surface. Objective-C
// types stay confined to the .mm implementation; the rest of the runtime can
// include this header from plain .cpp translation units (the public header
// only requires CoreVideo to be available).
//
// This file is compiled into the macOS target only. Windows builds never see
// it because CMake adds the macOS sources under `if(APPLE)`.

#include <memory>
#include <string>

#if defined(__APPLE__)
  #include <CoreVideo/CoreVideo.h>
#else
  // Defensive forward declaration. The macOS target always has __APPLE__
  // defined; this branch only exists so accidental inclusion on another
  // platform fails with a clear linker error rather than a CoreVideo header
  // not found compile error.
  using CVPixelBufferRef = struct __CVBuffer*;
#endif

#include "AetherFlow/IAIFrameAnalyzer.h"

namespace AetherFlow {
namespace platform {
namespace mac {

class IPlatformEncoderMac {
public:
    virtual ~IPlatformEncoderMac() = default;

    // Initialize the encoder for a given output container path. Returns false
    // on hard failure; the shim will record the failure and exit nonzero.
    virtual bool Initialize(int width, int height, int fps, const std::string& outputPath) = 0;

    // Submit a CVPixelBufferRef for encoding. Implementations must retain the
    // pixel buffer for the duration of the asynchronous encode (the shim
    // releases its own reference immediately after this returns). The
    // FrameDecision is provided so future implementations can drive QP /
    // reference parameters; phase 1 implementations may ignore it.
    //
    // Returns true on successful submit, false on submit failure (the shim
    // increments encoder failure counters but continues the run loop).
    virtual bool EncodeFrame(CVPixelBufferRef pixelBuffer,
                             double elapsedSeconds,
                             const FrameDecision& decision) = 0;

    // Flush any pending encoded frames to the output container.
    virtual void Flush() = 0;

    // Number of frames successfully encoded (post-callback) so far.
    virtual int EncodedFrameCount() const = 0;

    // Number of submit-time failures observed so far.
    virtual int EncodeFailureCount() const = 0;

    // Phase 1 macOS pipeline does not implement ROI; the policy engine still
    // emits ROI regions per scene, but the encoder ignores them and reports
    // false here. Trace summaries should expose this fact via
    // `roi_supported=false` so the architecture/benchmark agents can tell the
    // ROI gate apart from a regression.
    virtual bool RoiSupported() const { return false; }

    // Most recent encode-submit latency in milliseconds, sampled by the shim
    // for per-frame trace timing. Implementations should update this inside
    // EncodeFrame.
    virtual double LastEncodeSubmitMs() const = 0;
};

// Factory provided by VideoToolboxH264Encoder.mm. Returns a unique_ptr so
// ownership is unambiguous and the platform shim can dispose of the encoder
// before AVAssetWriter completion if a permission/init failure forces an
// early exit.
std::unique_ptr<IPlatformEncoderMac> MakeVideoToolboxH264Encoder();

} // namespace mac
} // namespace platform
} // namespace AetherFlow
