// MacosScreenCapture.mm — ScreenCaptureKit capture for AetherFlow phase 1/2.
//
// Cursor strategy: SCStreamConfiguration.showsCursor draws the cursor on the
// captured frame. We separately query NSEvent.mouseLocation for cursor
// coordinates so the scene-runtime CursorFocusModule has honest x/y in
// display-space pixels. Mac's NSEvent.mouseLocation has origin at the bottom
// left, so we flip to top-left to match the Windows convention used by
// FrameContext (cursorX/cursorY).
//
// Pixel format (phase 2): SCStreamConfiguration.pixelFormat is now
// kCVPixelFormatType_32BGRA (single-plane 32-bit BGRA) instead of bi-planar
// NV12. Rationale:
//   * CoreImage's CIFilter graphs (CIPixellate / CIGaussianBlur /
//     CIConstantColorGenerator) operate cleanly on single-plane BGRA. NV12
//     bi-planar requires per-plane conversions and degrades blur quality.
//   * VideoToolbox accepts BGRA source pixel buffers; VTCompressionSession
//     internally converts to its preferred NV12 working surface with no CPU
//     readback. The output H.264 bitstream is unchanged.
//   * IOSurface-backed zero-copy still applies — SCKit delivers IOSurface
//     pixel buffers regardless of plane layout, so CoreImage and VT both stay
//     on the GPU.

#include "AetherFlow/platform/mac/MacosScreenCapture.h"

#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <mach/mach_time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace {

// High-resolution monotonic mach_absolute_time converted to 100ns units (the
// same unit Windows uses for WGC SystemRelativeTime). This is the PD6 fallback
// when ScreenCaptureKit's per-sample CMTime PTS is invalid, exactly parallel
// to the Windows ScreenCapture::QpcNow100ns() QPC fallback. mach_absolute_time
// is strictly monotonic, so a timestamp always exists and never goes backward.
int64_t MachNow100ns() {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) {
        // numer/denom converts mach ticks -> nanoseconds. One-time init.
        mach_timebase_info(&tb);
        if (tb.denom == 0) {
            return 0;
        }
    }
    const uint64_t ticks = mach_absolute_time();
    // ticks * numer / denom = nanoseconds; nanoseconds / 100 = 100ns units.
    // Split the multiply/divide to avoid uint64 overflow on long runs (same
    // discipline as the Windows QpcNow100ns whole/remainder split).
    const uint64_t nsWhole = (ticks / tb.denom) * tb.numer;
    const uint64_t nsRem = ((ticks % tb.denom) * tb.numer) / tb.denom;
    return static_cast<int64_t>((nsWhole + nsRem) / 100ULL);
}

// Convert a ScreenCaptureKit per-sample CMTime PTS to 100ns units, matching
// the Windows WGC SystemRelativeTime unit so the cross-platform diagnostic
// math is identical. Returns 0 (caller treats as "use fallback") when the
// CMTime is invalid/indefinite/non-positive. CMTimeGetSeconds is exact for the
// rational SCKit timescale; ×1e7 yields 100ns ticks. (CMTimeConvertScale was
// the alternative; CMTimeGetSeconds was chosen because the consumer math is
// already delta-in-seconds based, so a single seconds→100ns conversion keeps
// one unit boundary instead of carrying a second rational timescale.)
int64_t CMTimeTo100ns(CMTime pts) {
    if (CMTIME_IS_INVALID(pts) || CMTIME_IS_INDEFINITE(pts) ||
        CMTIME_IS_NEGATIVE_INFINITY(pts) || CMTIME_IS_POSITIVE_INFINITY(pts)) {
        return 0;
    }
    const double seconds = CMTimeGetSeconds(pts);
    if (!(seconds > 0.0)) {  // also rejects NaN
        return 0;
    }
    return static_cast<int64_t>(seconds * 1.0e7);
}

} // namespace

// MacosScreenCapture::Impl is declared (incomplete) in the public header as a
// nested type. The full definition lives below in this .mm. The @interface
// for the SCStream sink only needs a pointer to it, so an alias is enough.
using AetherFlowMacImpl = AetherFlow::platform::mac::MacosScreenCapture::Impl;

@interface AetherFlowSCStreamSink : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithImpl:(AetherFlowMacImpl*)impl;
@end

namespace AetherFlow {
namespace platform {
namespace mac {

// Internal state. Lives in a unique_ptr from the public class so the .h does
// not need to import any Objective-C headers.
struct MacosScreenCapture::Impl {
    SCStream* stream = nil;
    AetherFlowSCStreamSink* sink = nil;
    SCContentFilter* filter = nil;
    SCStreamConfiguration* config = nil;
    SCDisplay* display = nil;

    int captureWidth = 0;
    int captureHeight = 0;
    int captureLeft = 0;
    int captureTop = 0;

    // Single-slot newest-frame buffer. Drops older frames if the consumer
    // falls behind, keeping latency low and avoiding queue growth.
    std::mutex mtx;
    std::condition_variable cv;
    CVPixelBufferRef latestBuffer = nullptr;
    std::chrono::steady_clock::time_point startTime{};
    std::atomic<bool> running{false};

    // PD6 capture-timing diagnostic parity (mirrors the Windows
    // ScreenCapture::m_lastFrameSystemRelativeTime100ns / m_lastFrameTimestampFromWgc
    // members). The SCStreamOutput callback stamps the newest delivered frame
    // with the REAL ScreenCaptureKit presentation timestamp in 100ns units
    // (CMSampleBufferGetPresentationTimeStamp), or a monotonic mach fallback if
    // the CMTime is invalid. Stored alongside latestBuffer under mtx so it is
    // handed to the consumer atomically with the buffer it belongs to.
    int64_t latestStamp100ns = 0;
    bool latestStampFromSckit = false;
};

} // namespace mac
} // namespace platform
} // namespace AetherFlow

@implementation AetherFlowSCStreamSink {
    AetherFlowMacImpl* _impl;
}

- (instancetype)initWithImpl:(AetherFlowMacImpl*)impl {
    self = [super init];
    if (self) {
        _impl = impl;
    }
    return self;
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    if (!_impl || !_impl->running.load()) {
        return;
    }
    if (type != SCStreamOutputTypeScreen) {
        return;
    }
    if (!sampleBuffer || !CMSampleBufferIsValid(sampleBuffer)) {
        return;
    }
    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) {
        return;
    }
    CFRetain(imageBuffer);

    // PD6: read the REAL ScreenCaptureKit per-sample presentation timestamp
    // here, while the CMSampleBuffer is still alive (parallel to the Windows
    // path reading WGC Direct3D11CaptureFrame.SystemRelativeTime at Stage 1
    // before the frame wrapper is destroyed). CMSampleBufferGetPresentationTimeStamp
    // returns a CMTime by value and does NOT transfer ownership of anything —
    // no CF retain/release is involved, so there is no leak/over-release. If
    // the PTS is invalid we fall back to a monotonic mach stamp so a usable,
    // never-decreasing timestamp always exists (parallel to the Windows QPC
    // fallback). This is measurement-only: the pixel buffer handling below is
    // unchanged.
    const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t stamp100ns = CMTimeTo100ns(pts);
    bool stampFromSckit = (stamp100ns > 0);
    if (!stampFromSckit) {
        stamp100ns = MachNow100ns();
    }

    {
        std::lock_guard<std::mutex> lock(_impl->mtx);
        if (_impl->latestBuffer) {
            CFRelease(_impl->latestBuffer);
            _impl->latestBuffer = nullptr;
        }
        _impl->latestBuffer = (CVPixelBufferRef)imageBuffer;
        _impl->latestStamp100ns = stamp100ns;
        _impl->latestStampFromSckit = stampFromSckit;
    }
    _impl->cv.notify_one();
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    if (_impl) {
        _impl->running.store(false);
        _impl->cv.notify_all();
    }
    if (error) {
        NSLog(@"[AetherFlow] SCStream stopped with error: %@", error);
    }
}

@end

namespace AetherFlow {
namespace platform {
namespace mac {

MacosScreenCapture::MacosScreenCapture() : m_impl(std::make_unique<Impl>()) {}
MacosScreenCapture::~MacosScreenCapture() { Close(); }

bool MacosScreenCapture::ProbeScreenCapturePermission() {
    __block BOOL granted = NO;
    __block BOOL completed = NO;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentWithCompletionHandler:^(SCShareableContent* content,
                                                   NSError* error) {
            if (error || !content || content.displays.count == 0) {
                granted = NO;
            } else {
                granted = YES;
            }
            completed = YES;
            dispatch_semaphore_signal(sem);
        }];
    dispatch_semaphore_wait(sem,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5.0 * NSEC_PER_SEC)));
    if (!completed) {
        return false;
    }
    return granted == YES;
}

bool MacosScreenCapture::Init(int desiredWidth, int desiredHeight, int targetFps) {
    if (targetFps <= 0) {
        targetFps = 30;
    }

    __block SCDisplay* foundDisplay = nil;
    __block NSError* fetchError = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentWithCompletionHandler:^(SCShareableContent* content,
                                                   NSError* error) {
            if (error) {
                fetchError = error;
            } else if (content.displays.count > 0) {
                foundDisplay = content.displays.firstObject;
            }
            dispatch_semaphore_signal(sem);
        }];
    dispatch_semaphore_wait(sem,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5.0 * NSEC_PER_SEC)));

    if (!foundDisplay) {
        if (fetchError) {
            NSLog(@"[AetherFlow] SCShareableContent failed: %@", fetchError);
        } else {
            NSLog(@"[AetherFlow] No SCDisplay available");
        }
        return false;
    }

    int displayWidth = (int)foundDisplay.width;
    int displayHeight = (int)foundDisplay.height;
    int width = desiredWidth > 0 ? desiredWidth : displayWidth;
    int height = desiredHeight > 0 ? desiredHeight : displayHeight;
    if (width <= 0 || height <= 0) {
        NSLog(@"[AetherFlow] Invalid capture size %dx%d", width, height);
        return false;
    }

    m_impl->display = foundDisplay;
    m_impl->captureWidth = width;
    m_impl->captureHeight = height;
    m_impl->captureLeft = 0;
    m_impl->captureTop = 0;

    SCContentFilter* filter =
        [[SCContentFilter alloc] initWithDisplay:foundDisplay
                                excludingWindows:@[]];
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = (size_t)width;
    config.height = (size_t)height;
    config.minimumFrameInterval = CMTimeMake(1, targetFps);
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.showsCursor = YES;
    config.colorSpaceName = kCGColorSpaceSRGB;
    config.queueDepth = 3;

    AetherFlowSCStreamSink* sink =
        [[AetherFlowSCStreamSink alloc] initWithImpl:m_impl.get()];
    SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:config
                                               delegate:sink];
    NSError* addError = nil;
    BOOL added = [stream addStreamOutput:sink
                                    type:SCStreamOutputTypeScreen
                      sampleHandlerQueue:dispatch_get_global_queue(
                                             QOS_CLASS_USER_INTERACTIVE, 0)
                                   error:&addError];
    if (!added) {
        NSLog(@"[AetherFlow] addStreamOutput failed: %@", addError);
        return false;
    }

    m_impl->filter = filter;
    m_impl->config = config;
    m_impl->sink = sink;
    m_impl->stream = stream;
    m_impl->startTime = std::chrono::steady_clock::now();
    m_impl->running.store(true);

    __block NSError* startError = nil;
    __block BOOL started = NO;
    dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
    [stream startCaptureWithCompletionHandler:^(NSError* error) {
        if (error) {
            startError = error;
        } else {
            started = YES;
        }
        dispatch_semaphore_signal(startSem);
    }];
    dispatch_semaphore_wait(startSem,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5.0 * NSEC_PER_SEC)));

    if (!started) {
        NSLog(@"[AetherFlow] startCapture failed: %@", startError);
        m_impl->running.store(false);
        return false;
    }
    return true;
}

void MacosScreenCapture::Close() {
    if (!m_impl) {
        return;
    }
    if (m_impl->running.exchange(false)) {
        if (m_impl->stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [m_impl->stream stopCaptureWithCompletionHandler:^(NSError* error) {
                (void)error;
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem,
                dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)));
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);
        if (m_impl->latestBuffer) {
            CFRelease(m_impl->latestBuffer);
            m_impl->latestBuffer = nullptr;
        }
    }
    m_impl->stream = nil;
    m_impl->sink = nil;
    m_impl->filter = nil;
    m_impl->config = nil;
    m_impl->display = nil;
}

bool MacosScreenCapture::WaitNextFrame(double timeoutSeconds,
                                       CVPixelBufferRef* outBuffer,
                                       double* outElapsedSeconds,
                                       bool* outHasCursor,
                                       int* outCursorX,
                                       int* outCursorY,
                                       int64_t* outCaptureStamp100ns,
                                       bool* outCaptureStampFromSckit) {
    if (outCaptureStamp100ns) *outCaptureStamp100ns = 0;
    if (outCaptureStampFromSckit) *outCaptureStampFromSckit = false;
    if (!outBuffer) {
        return false;
    }
    *outBuffer = nullptr;
    if (!m_impl || !m_impl->running.load()) {
        return false;
    }

    std::unique_lock<std::mutex> lock(m_impl->mtx);
    auto pred = [&]() {
        return m_impl->latestBuffer != nullptr || !m_impl->running.load();
    };
    if (timeoutSeconds <= 0.0) {
        m_impl->cv.wait(lock, pred);
    } else {
        auto durMs = std::chrono::milliseconds(
            static_cast<long long>(timeoutSeconds * 1000.0));
        if (!m_impl->cv.wait_for(lock, durMs, pred)) {
            return false;
        }
    }
    if (!m_impl->latestBuffer) {
        return false;
    }
    *outBuffer = m_impl->latestBuffer;
    m_impl->latestBuffer = nullptr;
    lock.unlock();

    if (outElapsedSeconds) {
        auto now = std::chrono::steady_clock::now();
        *outElapsedSeconds = std::chrono::duration<double>(
            now - m_impl->startTime).count();
    }

    if (outHasCursor || outCursorX || outCursorY) {
        // NSEvent.mouseLocation returns bottom-left origin in screen points.
        // Translate to top-left pixel coordinates of the captured display.
        NSPoint pt = [NSEvent mouseLocation];
        CGFloat dispH = (CGFloat)m_impl->display.height;
        CGFloat dispW = (CGFloat)m_impl->display.width;
        CGFloat scaleX = (CGFloat)m_impl->captureWidth / (dispW > 0 ? dispW : 1);
        CGFloat scaleY = (CGFloat)m_impl->captureHeight / (dispH > 0 ? dispH : 1);
        int x = (int)(pt.x * scaleX);
        int y = (int)((dispH - pt.y) * scaleY);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= m_impl->captureWidth) x = m_impl->captureWidth - 1;
        if (y >= m_impl->captureHeight) y = m_impl->captureHeight - 1;
        if (outHasCursor) *outHasCursor = true;
        if (outCursorX) *outCursorX = x;
        if (outCursorY) *outCursorY = y;
    }
    return true;
}

int MacosScreenCapture::GetCaptureWidth() const {
    return m_impl ? m_impl->captureWidth : 0;
}
int MacosScreenCapture::GetCaptureHeight() const {
    return m_impl ? m_impl->captureHeight : 0;
}
int MacosScreenCapture::GetCaptureLeft() const {
    return m_impl ? m_impl->captureLeft : 0;
}
int MacosScreenCapture::GetCaptureTop() const {
    return m_impl ? m_impl->captureTop : 0;
}

} // namespace mac
} // namespace platform
} // namespace AetherFlow
