#pragma once

// ScreenCaptureKit-based capture wrapper for macOS phase 1.
//
// Mirrors the surface area of the Windows ScreenCapture class so the
// scene-runtime layer can call Init/WaitNextFrame/geometry-getters without
// platform-specific glue. Implemented in MacosScreenCapture.mm; this header
// stays pure C++ so it can be included from any translation unit in the
// macOS target.

#include <memory>

#if defined(__APPLE__)
  #include <CoreVideo/CoreVideo.h>
#else
  using CVPixelBufferRef = struct __CVBuffer*;
#endif

namespace AetherFlow {
namespace platform {
namespace mac {

class MacosScreenCapture {
public:
    MacosScreenCapture();
    ~MacosScreenCapture();

    MacosScreenCapture(const MacosScreenCapture&) = delete;
    MacosScreenCapture& operator=(const MacosScreenCapture&) = delete;

    // Init starts an SCStream against the main display at the requested
    // dimensions. desiredWidth/Height of 0 means "use the display's native
    // size". Returns false on permission failure or if no displays are
    // available; the caller should treat this as an unsupported run.
    bool Init(int desiredWidth, int desiredHeight, int targetFps);

    // Stops and releases the SCStream. Safe to call multiple times.
    void Close();

    // Pop the most recent CVPixelBuffer delivered by SCStream. Caller takes
    // ownership of *outBuffer and must release it via CFRelease. Cursor is
    // derived from NSEvent.mouseLocation translated to display coordinates.
    // Returns false on timeout (no frame within timeoutSeconds).
    //
    // Capture-timing root-fix PD6 (macOS measurement-diagnostic parity with
    // Windows PD4): the optional outCaptureStamp100ns / outCaptureStampFromSckit
    // out-params carry the REAL per-frame ScreenCaptureKit presentation
    // timestamp (CMSampleBufferGetPresentationTimeStamp, in 100ns units to
    // match the Windows WGC SystemRelativeTime unit), captured at the SCStream
    // delivery point. *outCaptureStampFromSckit is true when the stamp came
    // from a valid SCKit CMTime PTS, false when it fell back to a monotonic
    // mach_absolute_time stamp (parallel to the Windows QPC fallback). This is
    // DELIBERATELY separate from outElapsedSeconds — that synthetic
    // steady_clock wall time is unchanged so nothing downstream is perturbed;
    // only this stamp reflects true content time and feeds the
    // effective-capture-fps diagnostic.
    //
    // Shape rationale (lowest blast radius): WaitNextFrame already passes
    // per-frame state out via out-params and has exactly one caller
    // (RunMacosPipeline). Adding two trailing optional out-params (defaulting
    // to nullptr) mirrors the existing outElapsedSeconds flow without a new
    // accessor, an Impl-locking accessor race, or a signature break for any
    // other call site. (The Windows path used an accessor instead because
    // CaptureTexture() had multiple call sites; macOS does not, so an additive
    // out-param is the smaller, equivalent change here.)
    bool WaitNextFrame(double timeoutSeconds,
                       CVPixelBufferRef* outBuffer,
                       double* outElapsedSeconds,
                       bool* outHasCursor,
                       int* outCursorX,
                       int* outCursorY,
                       int64_t* outCaptureStamp100ns = nullptr,
                       bool* outCaptureStampFromSckit = nullptr);

    int GetCaptureWidth() const;
    int GetCaptureHeight() const;
    int GetCaptureLeft() const;
    int GetCaptureTop() const;

    // Static permission probe. Returns true if Screen Recording permission is
    // granted (kTCCServiceScreenCapture). Implemented by issuing a one-shot
    // SCShareableContent.getShareableContentWithCompletionHandler call and
    // looking at the resulting NSError.
    static bool ProbeScreenCapturePermission();

    // Defined in MacosScreenCapture.mm. Public so the Objective-C sink class
    // in the .mm can name the type directly (standard pImpl idiom: the type
    // is only fully defined in the implementation file and is not used by
    // external callers).
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace mac
} // namespace platform
} // namespace AetherFlow
