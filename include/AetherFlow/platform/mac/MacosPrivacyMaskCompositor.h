#pragma once

// MacosPrivacyMaskCompositor — macOS phase 2 privacy-mask render pass.
//
// Consumes a BGRA CVPixelBuffer from ScreenCaptureKit and the
// FrameDecision::privacyMasks emitted by the deterministic policy engine,
// then produces a BGRA CVPixelBuffer that has each masked region replaced
// according to the configured mode (Blackout / Blur / Mosaic). The encoder
// then submits the masked buffer to VTCompressionSession.
//
// Design notes:
//   * Pure C++ header. No Obj-C, no `#import`. CVPixelBufferRef is a C-typed
//     handle from <CoreVideo/CVPixelBuffer.h> so plain .cpp callers (or other
//     .mm files) can include this without dragging Cocoa/CoreImage in.
//   * pImpl is `public` so the `.mm` can fully define the Impl struct in its
//     own translation unit while the unique_ptr destructor in the same TU
//     still sees the complete type. (Same pattern as MacosScreenCapture::Impl.
//     The phase 1 work hit a compile error here when Impl was private.)
//   * Output buffers are vended from an internal CVPixelBufferPool sized at 3.
//     The pool hands out a +1-retained CVPixelBufferRef; the **caller must
//     CFRelease `*outMaskedBgra`** when done (typically after the encoder's
//     submit returns). On the no-masks zero-copy path, `*outMaskedBgra ==
//     sourceBgra`; the caller's existing CFRelease on the source covers it.
//     On masked paths the caller must CFRelease the new buffer separately.
//     (Earlier draft of this contract said "do NOT CFRelease" and the
//     implementation matched, which produced a use-after-free SIGTRAP as
//     soon as any chat-window mask fired. The pool reclaiming the buffer
//     mid-encode took the encoder down with it.)
//   * On any CoreImage filter / render failure the compositor falls back to
//     a clear-fill (blackout) into the output buffer and surfaces this via
//     `stats->fallbackUsed = true` and `stats->path =
//     "coreimage-bgra-clearfill-fallback"`. The return value stays `true`
//     because the output buffer is in a defined, encoder-safe state.
//
// Coordinate space:
//   FrameDecision::privacyMasks regions are emitted in capture-space
//   coordinates with a top-left origin (Windows convention; macOS producers
//   in this repo follow the same shape). CIImage uses bottom-left origin, so
//   the implementation flips Y when constructing CIImage / CIFilter crop
//   rects. Documented inline next to the conversion.

#include <memory>
#include <string>

#if defined(__APPLE__)
  #include <CoreVideo/CVPixelBuffer.h>
#else
  // Defensive fallback so accidental inclusion on a non-Apple platform fails
  // at link time with a clear missing-symbol error rather than at parse time.
  using CVPixelBufferRef = struct __CVBuffer*;
#endif

#include "AetherFlow/IAIFrameAnalyzer.h"

namespace AetherFlow {
namespace platform {
namespace mac {

enum class MacosPrivacyMaskMode {
    Blackout,
    Blur,
    Mosaic,
};

// Per-Compose() statistics surfaced into frame_trace.jsonl.
//
// `path` is one of:
//   "none"                                  — no privacyMasks in the decision
//   "coreimage-bgra-clearfill"              — Blackout success path
//   "coreimage-bgra-blur"                   — Blur success path
//   "coreimage-bgra-mosaic"                 — Mosaic success path
//   "coreimage-bgra-clearfill-fallback"     — any mode, filter/render failed
//                                             and we fell back to blackout
struct MacosPrivacyMaskApplyStats {
    int requestedCount = 0;
    int appliedCount = 0;
    bool fallbackUsed = false;
    std::string path = "none";
    std::string failureReason;
};

class MacosPrivacyMaskCompositor {
public:
    MacosPrivacyMaskCompositor();
    ~MacosPrivacyMaskCompositor();

    MacosPrivacyMaskCompositor(const MacosPrivacyMaskCompositor&) = delete;
    MacosPrivacyMaskCompositor& operator=(const MacosPrivacyMaskCompositor&) = delete;

    // captureWidth/Height: actual capture frame size (matches CVPixelBuffer
    //                      dimensions delivered by ScreenCaptureKit).
    // decisionWidth/Height: coordinate space the policy emitted regions in.
    //                      Currently equal to capture* on macOS but kept
    //                      separate for future parity with the Windows
    //                      decision-vs-capture scaling.
    // mode:               Blackout / Blur / Mosaic — picked from
    //                     MacosRunOptions::privacyMaskMode upstream.
    // mosaicBlockPx:      CIPixellate inputScale (pixel block edge in capture
    //                     space). Ignored when mode != Mosaic.
    bool Initialize(int captureWidth,
                    int captureHeight,
                    int decisionWidth,
                    int decisionHeight,
                    MacosPrivacyMaskMode mode,
                    int mosaicBlockPx);

    // Compose applies privacyMasks from `decision` onto `sourceBgra`.
    //
    //   * No masks -> writes `sourceBgra` straight to `*outMaskedBgra`
    //                 (zero-copy passthrough); stats.path = "none". Caller's
    //                 existing CFRelease on the source covers this case.
    //   * Masks present, success -> acquires a pool buffer (+1 retain),
    //                 renders the composited CoreImage graph into it, writes
    //                 it to `*outMaskedBgra`. **Caller MUST CFRelease the
    //                 returned buffer** when done with the encoder submit.
    //   * Masks present, failure -> still writes a defined output buffer
    //                 (clear-filled to black). Stats note the fallback.
    //                 Caller's CFRelease rule is the same as the success case.
    //
    // Returns true if `*outMaskedBgra` is in a defined state on exit, false
    // only on a hard pool-acquire failure (in which case the caller should
    // skip the encode for this frame).
    bool Compose(CVPixelBufferRef sourceBgra,
                 const AetherFlow::FrameDecision& decision,
                 CVPixelBufferRef* outMaskedBgra,
                 MacosPrivacyMaskApplyStats* stats);

    // Wall-clock elapsed milliseconds of the most recent Compose() call. The
    // shim writes this into trace records as `maskMs`.
    double LastComposeMs() const;

    MacosPrivacyMaskMode Mode() const;

    // Public — see header rationale above.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace mac
} // namespace platform
} // namespace AetherFlow
