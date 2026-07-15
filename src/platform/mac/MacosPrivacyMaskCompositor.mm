// MacosPrivacyMaskCompositor.mm — CoreImage/Metal privacy-mask render pass.
//
// See include/AetherFlow/platform/mac/MacosPrivacyMaskCompositor.h for the
// behavioral contract. This .mm:
//   1. Builds a Metal-backed CIContext once at Initialize.
//   2. Maintains a 3-slot BGRA CVPixelBufferPool sized to capture dimensions.
//   3. Per-frame, composites privacy-mask regions over the source BGRA
//      buffer using CIFilter graphs (CIPixellate / CIGaussianBlur /
//      CIConstantColorGenerator) and renders the result into a pool buffer.
//   4. Falls back to clear-fill (blackout) on any filter/render error so the
//      encoder always receives a defined buffer; never re-throws CoreImage
//      errors to the caller.
//
// Coordinate system reminder:
//   FrameDecision::privacyMasks store regions in top-left-origin capture
//   space. CIImage / CIFilter operate in bottom-left-origin space. The Y
//   conversion happens inline below where the crop CGRect is built.

#include "AetherFlow/platform/mac/MacosPrivacyMaskCompositor.h"

#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdio>

namespace AetherFlow {
namespace platform {
namespace mac {

struct MacosPrivacyMaskCompositor::Impl {
    // CoreImage state ----------------------------------------------------
    // NOTE: ObjC pointers in a C++ struct require explicit `__strong` under
    // ARC; without it the field is `__unsafe_unretained` and the object can
    // be deallocated when the autorelease pool drains a few frames in,
    // crashing later uses with SIGTRAP. (This is the macOS-phase-2 first-pass
    // bug that masked itself behind "Blur/Mosaic crash, Blackout fine".)
    __strong id<MTLDevice> mtlDevice = nil;
    __strong CIContext* ciContext = nil;
    CGColorSpaceRef workingColorSpace = nullptr;

    // Pre-built filter instances. Reused across frames; per-region inputs
    // (extent, crop rect, color) are mutated each Compose.
    __strong CIFilter* blurFilter = nil;       // CIGaussianBlur
    __strong CIFilter* pixellateFilter = nil;  // CIPixellate
    // Cached black CIImage used by the blackout / fallback paths.
    __strong CIImage* blackFillImage = nil;

    // Output buffer pool. BGRA + IOSurface-backed, size 3.
    CVPixelBufferPoolRef pool = nullptr;

    // Geometry and policy
    int captureW = 0;
    int captureH = 0;
    int decisionW = 0;
    int decisionH = 0;
    int blockPx = 16;
    MacosPrivacyMaskMode mode = MacosPrivacyMaskMode::Blur;

    // Diagnostics
    double lastComposeMs = 0.0;
    bool initialized = false;
};

namespace {

const char* PathLabelForMode(MacosPrivacyMaskMode mode) {
    switch (mode) {
    case MacosPrivacyMaskMode::Blackout: return "coreimage-bgra-clearfill";
    case MacosPrivacyMaskMode::Blur:     return "coreimage-bgra-blur";
    case MacosPrivacyMaskMode::Mosaic:   return "coreimage-bgra-mosaic";
    }
    return "coreimage-bgra-clearfill";
}

void LogWarn(const char* msg) {
    std::fprintf(stderr, "[AetherFlow][macOS][PrivacyMaskCompositor] %s\n", msg);
}

// Clamp a region to [0..captureW] / [0..captureH] in capture-space pixels
// (top-left origin). Returns false when the region collapses to empty.
bool ClampRegionToCapture(const AetherFlow::FrameRegion& r,
                          int captureW, int captureH,
                          int* outL, int* outT, int* outR, int* outB) {
    int l = r.left   < 0 ? 0 : r.left;
    int t = r.top    < 0 ? 0 : r.top;
    int rr = r.right  > captureW ? captureW : r.right;
    int b = r.bottom > captureH ? captureH : r.bottom;
    if (rr <= l || b <= t) return false;
    *outL = l; *outT = t; *outR = rr; *outB = b;
    return true;
}

// Build a CIImage bottom-left-origin crop rect from a top-left-origin
// capture-space region. CIImage's coordinate space has y=0 at the bottom of
// the image, so the rect's origin.y = captureH - bottom.
CGRect CIRectFromCaptureRegion(int left, int top, int right, int bottom,
                               int captureH) {
    const CGFloat x = (CGFloat)left;
    const CGFloat y = (CGFloat)(captureH - bottom);
    const CGFloat w = (CGFloat)(right - left);
    const CGFloat h = (CGFloat)(bottom - top);
    return CGRectMake(x, y, w, h);
}

// Best-effort warmup. Renders a 1x1 BGRA buffer through the chosen filter so
// the CIContext compiles its Metal kernels up front. Errors are logged but
// non-fatal — production runs survive without warmup.
void Warmup(MacosPrivacyMaskCompositor::Impl* impl) {
    @autoreleasepool {
        CVPixelBufferRef tiny = nullptr;
        NSDictionary* attrs = @{
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        CVReturn cv = CVPixelBufferCreate(kCFAllocatorDefault,
                                          1, 1,
                                          kCVPixelFormatType_32BGRA,
                                          (__bridge CFDictionaryRef)attrs,
                                          &tiny);
        if (cv != kCVReturnSuccess || tiny == nullptr) {
            LogWarn("warmup: CVPixelBufferCreate failed");
            if (tiny) CFRelease(tiny);
            return;
        }
        CIImage* src = [CIImage imageWithCVPixelBuffer:tiny
                                               options:@{kCIImageColorSpace:
                                                         (__bridge id)impl->workingColorSpace}];
        CIImage* result = src;
        if (impl->mode == MacosPrivacyMaskMode::Blur && impl->blurFilter) {
            [impl->blurFilter setValue:src forKey:kCIInputImageKey];
            [impl->blurFilter setValue:@(2.0) forKey:kCIInputRadiusKey];
            result = impl->blurFilter.outputImage ?: src;
        } else if (impl->mode == MacosPrivacyMaskMode::Mosaic && impl->pixellateFilter) {
            [impl->pixellateFilter setValue:src forKey:kCIInputImageKey];
            [impl->pixellateFilter setValue:@(1.0) forKey:kCIInputScaleKey];
            result = impl->pixellateFilter.outputImage ?: src;
        }
        [impl->ciContext render:result
                toCVPixelBuffer:tiny
                         bounds:CGRectMake(0, 0, 1, 1)
                     colorSpace:impl->workingColorSpace];
        CFRelease(tiny);
    }
}

} // namespace

MacosPrivacyMaskCompositor::MacosPrivacyMaskCompositor()
    : m_impl(std::make_unique<Impl>()) {}

MacosPrivacyMaskCompositor::~MacosPrivacyMaskCompositor() {
    if (!m_impl) return;
    @autoreleasepool {
        m_impl->ciContext = nil;
        m_impl->blurFilter = nil;
        m_impl->pixellateFilter = nil;
        m_impl->blackFillImage = nil;
        m_impl->mtlDevice = nil;
    }
    if (m_impl->pool) {
        CVPixelBufferPoolRelease(m_impl->pool);
        m_impl->pool = nullptr;
    }
    if (m_impl->workingColorSpace) {
        CGColorSpaceRelease(m_impl->workingColorSpace);
        m_impl->workingColorSpace = nullptr;
    }
}

bool MacosPrivacyMaskCompositor::Initialize(int captureWidth,
                                            int captureHeight,
                                            int decisionWidth,
                                            int decisionHeight,
                                            MacosPrivacyMaskMode mode,
                                            int mosaicBlockPx) {
    if (!m_impl) return false;
    if (captureWidth <= 0 || captureHeight <= 0) {
        LogWarn("Initialize: invalid capture dimensions");
        return false;
    }
    m_impl->captureW = captureWidth;
    m_impl->captureH = captureHeight;
    m_impl->decisionW = decisionWidth > 0 ? decisionWidth : captureWidth;
    m_impl->decisionH = decisionHeight > 0 ? decisionHeight : captureHeight;
    m_impl->mode = mode;
    m_impl->blockPx = mosaicBlockPx > 0 ? mosaicBlockPx : 16;

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            LogWarn("Initialize: MTLCreateSystemDefaultDevice returned nil");
            return false;
        }
        m_impl->mtlDevice = device;

        m_impl->workingColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (m_impl->workingColorSpace == nullptr) {
            LogWarn("Initialize: CGColorSpaceCreateWithName(sRGB) failed");
            return false;
        }

        NSDictionary* ctxOpts = @{
            kCIContextWorkingColorSpace: (__bridge id)m_impl->workingColorSpace,
            kCIContextOutputColorSpace:  (__bridge id)m_impl->workingColorSpace,
            kCIContextUseSoftwareRenderer: @NO,
        };
        m_impl->ciContext = [CIContext contextWithMTLDevice:device options:ctxOpts];
        if (m_impl->ciContext == nil) {
            LogWarn("Initialize: contextWithMTLDevice failed");
            return false;
        }

        m_impl->blurFilter = [CIFilter filterWithName:@"CIGaussianBlur"];
        m_impl->pixellateFilter = [CIFilter filterWithName:@"CIPixellate"];

        // Cached black-fill CIImage covers the full capture extent. Cropping
        // happens at composite time.
        CIImage* black = [CIImage imageWithColor:[CIColor colorWithRed:0
                                                                 green:0
                                                                  blue:0
                                                                 alpha:1]];
        m_impl->blackFillImage = [black imageByCroppingToRect:CGRectMake(0, 0,
                                                                          captureWidth,
                                                                          captureHeight)];

        // Output pixel buffer pool: BGRA + IOSurface-backed, capture-sized,
        // min 3 in flight to absorb VT submit pipeline.
        NSDictionary* pbAttrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey:            @(captureWidth),
            (id)kCVPixelBufferHeightKey:           @(captureHeight),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        NSDictionary* poolAttrs = @{
            (id)kCVPixelBufferPoolMinimumBufferCountKey: @3,
        };
        CVReturn cv = CVPixelBufferPoolCreate(
            kCFAllocatorDefault,
            (__bridge CFDictionaryRef)poolAttrs,
            (__bridge CFDictionaryRef)pbAttrs,
            &m_impl->pool);
        if (cv != kCVReturnSuccess || m_impl->pool == nullptr) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "Initialize: CVPixelBufferPoolCreate failed status=%d",
                          (int)cv);
            LogWarn(msg);
            return false;
        }
    }

    m_impl->initialized = true;

    // Best-effort warmup — pay first-frame CIContext compile cost up front.
    // Errors here do not fail Initialize; cold-start cost is amortized over
    // the first real frame instead.
    Warmup(m_impl.get());
    return true;
}

bool MacosPrivacyMaskCompositor::Compose(CVPixelBufferRef sourceBgra,
                                         const AetherFlow::FrameDecision& decision,
                                         CVPixelBufferRef* outMaskedBgra,
                                         MacosPrivacyMaskApplyStats* stats) {
    if (!outMaskedBgra) return false;
    *outMaskedBgra = nullptr;
    MacosPrivacyMaskApplyStats localStats;
    if (!stats) stats = &localStats;
    *stats = MacosPrivacyMaskApplyStats{};

    if (!m_impl || !m_impl->initialized) {
        stats->failureReason = "compositor not initialized";
        return false;
    }

    // No masks -> zero-copy passthrough. Cheapest, common path.
    if (decision.privacyMasks.empty()) {
        stats->path = "none";
        stats->requestedCount = 0;
        stats->appliedCount = 0;
        stats->fallbackUsed = false;
        *outMaskedBgra = sourceBgra;
        m_impl->lastComposeMs = 0.0;
        return true;
    }
    if (sourceBgra == nullptr) {
        stats->failureReason = "sourceBgra is null";
        return false;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    const int captureW = m_impl->captureW;
    const int captureH = m_impl->captureH;
    stats->requestedCount = (int)decision.privacyMasks.size();
    stats->path = PathLabelForMode(m_impl->mode);

    @autoreleasepool {
        // Acquire output buffer from pool. Hard failure here means we cannot
        // give the encoder anything coherent; surface as a failure.
        CVPixelBufferRef outBuffer = nullptr;
        CVReturn cv = CVPixelBufferPoolCreatePixelBuffer(
            kCFAllocatorDefault, m_impl->pool, &outBuffer);
        if (cv != kCVReturnSuccess || outBuffer == nullptr) {
            stats->fallbackUsed = true;
            stats->path = "coreimage-bgra-clearfill-fallback";
            stats->failureReason = "CVPixelBufferPoolCreatePixelBuffer failed";
            LogWarn(stats->failureReason.c_str());
            return false;
        }
        // CVPixelBufferPool retains the buffer for us via the pool; the
        // returned ref has a +1 count, but the pool keeps a strong ref via
        // its lifecycle. The local +1 is released after the encoder copies
        // / VT internally retains. Since the encoder submits asynchronously
        // and we want the buffer alive for the submit window, we attach it
        // to the autoreleasepool by transferring ownership through __bridge
        // semantics: hand the pointer back to the caller as a borrowed ref,
        // and release the +1 here. CVPixelBufferRetain inside VT keeps it
        // alive across the async encode.
        // Note: we release the +1 only AFTER VT submit by handing the buffer
        // to the caller. The caller of Compose pairs this with CFRelease
        // semantics inherited from the source buffer pattern in the shim.
        // The shim treats compositor-vended buffers as borrowed.

        // Build the source CIImage.
        CIImage* sourceImage = [CIImage imageWithCVPixelBuffer:sourceBgra
                                                       options:@{kCIImageColorSpace:
                                                                 (__bridge id)m_impl->workingColorSpace}];
        if (sourceImage == nil) {
            // Fall back to clear-filled output.
            [m_impl->ciContext render:m_impl->blackFillImage
                      toCVPixelBuffer:outBuffer
                               bounds:CGRectMake(0, 0, captureW, captureH)
                           colorSpace:m_impl->workingColorSpace];
            stats->fallbackUsed = true;
            stats->path = "coreimage-bgra-clearfill-fallback";
            stats->failureReason = "imageWithCVPixelBuffer returned nil";
            *outMaskedBgra = outBuffer;
            // Caller (shim) is responsible for CFRelease.
            auto t1 = std::chrono::high_resolution_clock::now();
            m_impl->lastComposeMs =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            stats->appliedCount = 0;
            return true;
        }

        // Composite each privacy-mask region over the source image, picking
        // the per-region filter graph from the configured mode.
        CIImage* composed = sourceImage;
        bool anyFailure = false;
        int applied = 0;
        for (const auto& mask : decision.privacyMasks) {
            if (mask.purpose != AetherFlow::FrameRegionPurpose::PrivacyMask) {
                continue;
            }
            int L, T, R, B;
            if (!ClampRegionToCapture(mask, captureW, captureH, &L, &T, &R, &B)) {
                continue;
            }
            // Top-left -> bottom-left flip for CIImage space.
            CGRect ciRect = CIRectFromCaptureRegion(L, T, R, B, captureH);

            CIImage* patch = nil;
            switch (m_impl->mode) {
            case MacosPrivacyMaskMode::Blackout: {
                // Crop a black image to the masked rect.
                patch = [m_impl->blackFillImage imageByCroppingToRect:ciRect];
                break;
            }
            case MacosPrivacyMaskMode::Blur: {
                // Crop the source rect, blur, then crop back to the same rect
                // so the blur kernel does not leak outside the masked area.
                CIImage* cropped = [sourceImage imageByCroppingToRect:ciRect];
                [m_impl->blurFilter setValue:cropped forKey:kCIInputImageKey];
                [m_impl->blurFilter setValue:@(10.0) forKey:kCIInputRadiusKey];
                CIImage* blurred = m_impl->blurFilter.outputImage;
                if (blurred == nil) { anyFailure = true; break; }
                patch = [blurred imageByCroppingToRect:ciRect];
                break;
            }
            case MacosPrivacyMaskMode::Mosaic: {
                CIImage* cropped = [sourceImage imageByCroppingToRect:ciRect];
                [m_impl->pixellateFilter setValue:cropped forKey:kCIInputImageKey];
                [m_impl->pixellateFilter setValue:@((double)m_impl->blockPx)
                                          forKey:kCIInputScaleKey];
                CIImage* pixelated = m_impl->pixellateFilter.outputImage;
                if (pixelated == nil) { anyFailure = true; break; }
                patch = [pixelated imageByCroppingToRect:ciRect];
                break;
            }
            }
            if (patch == nil) { anyFailure = true; continue; }
            // Source-over composite the patch back onto the running result.
            composed = [patch imageByCompositingOverImage:composed];
            ++applied;
        }

        if (anyFailure) {
            // CoreImage either succeeds for the whole batch or falls back.
            // Mirrors Windows compositor semantics for consistent verifier
            // counts.
            [m_impl->ciContext render:m_impl->blackFillImage
                      toCVPixelBuffer:outBuffer
                               bounds:CGRectMake(0, 0, captureW, captureH)
                           colorSpace:m_impl->workingColorSpace];
            stats->fallbackUsed = true;
            stats->path = "coreimage-bgra-clearfill-fallback";
            stats->failureReason = "CIFilter produced nil outputImage";
            stats->appliedCount = stats->requestedCount;
            *outMaskedBgra = outBuffer;
            // Caller (shim) is responsible for CFRelease.
            auto t1 = std::chrono::high_resolution_clock::now();
            m_impl->lastComposeMs =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            return true;
        }

        // Render the final composite into the output pool buffer.
        [m_impl->ciContext render:composed
                  toCVPixelBuffer:outBuffer
                           bounds:CGRectMake(0, 0, captureW, captureH)
                       colorSpace:m_impl->workingColorSpace];

        stats->appliedCount = stats->requestedCount;
        stats->fallbackUsed = false;
        // stats->path was already set above to the mode-specific label.

        *outMaskedBgra = outBuffer;
        // Caller (shim) is responsible for CFRelease. The pool reclaims the
        // buffer back into its free list once VT and the shim both release.
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    m_impl->lastComposeMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    return true;
}

double MacosPrivacyMaskCompositor::LastComposeMs() const {
    return m_impl ? m_impl->lastComposeMs : 0.0;
}

MacosPrivacyMaskMode MacosPrivacyMaskCompositor::Mode() const {
    return m_impl ? m_impl->mode : MacosPrivacyMaskMode::Blur;
}

} // namespace mac
} // namespace platform
} // namespace AetherFlow
