// VideoToolboxH264Encoder.mm — macOS phase 1/2 H.264 encoder.
//
// Pipeline:
//   CVPixelBufferRef (BGRA, IOSurface-backed, from ScreenCaptureKit, possibly
//   re-rendered by MacosPrivacyMaskCompositor into a pool-owned BGRA buffer)
//   -> VTCompressionSession (H.264 Main, real-time, no B-frames, fixed 3 Mbps,
//   GOP = 2*fps) -> compressed CMSampleBuffer -> AVAssetWriter (passthrough,
//   MPEG-4) -> output/output.mp4.
//
// Design choices:
//   * Synchronous C-callback form of VTCompressionSessionCreate. Compatible
//     back to macOS 10.9 and avoids requiring blocks in the output handler.
//     The callback runs on a VideoToolbox-owned worker thread.
//   * Deferred AVAssetWriterInput creation. The first compressed sample
//     buffer carries the exact CMVideoFormatDescription (SPS/PPS) emitted by
//     VideoToolbox. We construct the AVAssetWriterInput with that format
//     description and `outputSettings:nil`, which puts the input into
//     passthrough mode — AVAssetWriter muxes the VT bitstream as-is and never
//     re-encodes.
//   * `appendSampleBuffer:` is performed on a dedicated serial dispatch queue
//     so VT's callback thread never blocks on UI/main work and the writer
//     sees ordered appends.
//   * EncodedFrameCount counts frames the muxer actually accepted (post
//     `appendSampleBuffer: == YES`). EncodeFailureCount counts frames where
//     VTCompressionSessionEncodeFrame returned non-zero or the muxer rejected
//     the sample. Submit-time success is reported by EncodeFrame.
//   * LastEncodeSubmitMs measures only the synchronous submit window
//     (VTCompressionSessionEncodeFrame + bookkeeping). The actual encode is
//     asynchronous — same semantics the Windows NVENC path exposes.
//   * Pixel format requested from VT == the format ScreenCaptureKit hands us
//     and what MacosPrivacyMaskCompositor emits (`kCVPixelFormatType_32BGRA`).
//     VTCompressionSession internally converts BGRA -> its preferred NV12
//     working surface with no CPU readback; the output Annex-B H.264 stream
//     muxed into the MP4 is bit-identical to the NV12-source path.

#include "AetherFlow/platform/mac/IPlatformEncoderMac.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace AetherFlow {
namespace platform {
namespace mac {

namespace {

constexpr int32_t kPtsTimescale = 90000;            // standard MPEG/H.264 timescale
constexpr int32_t kDefaultBitrateBps = 3'000'000;   // phase 1 fixed bitrate

class VideoToolboxH264Encoder final : public IPlatformEncoderMac {
public:
    VideoToolboxH264Encoder() = default;

    ~VideoToolboxH264Encoder() override {
        // Defensive: ensure session is torn down even if Flush() never ran.
        TeardownCompressionSession();
        // The writer (if alive) is best-effort released here. A clean shutdown
        // should have happened via Flush(); this is for crash-path safety.
        @autoreleasepool {
            m_writerInput = nil;
            m_writer = nil;
            m_writerQueue = nil;
        }
    }

    bool Initialize(int width, int height, int fps, const std::string& outputPath) override {
        if (m_initialized) {
            AppendError("Initialize called twice on the same encoder instance.");
            return false;
        }
        if (width <= 0 || height <= 0 || fps <= 0) {
            AppendError("Initialize: invalid dimensions or fps.");
            return false;
        }
        m_width = width;
        m_height = height;
        m_fps = fps;
        m_outputPath = outputPath;

        if (!PrepareOutputFile(outputPath)) {
            return false;
        }
        if (!CreateAssetWriter(outputPath)) {
            return false;
        }
        if (!CreateCompressionSession()) {
            // Tear down the writer we just created so the next Initialize()
            // attempt (if any) starts from a clean slate.
            @autoreleasepool {
                m_writer = nil;
                m_writerQueue = nil;
            }
            return false;
        }

        m_initialized = true;
        return true;
    }

    bool EncodeFrame(CVPixelBufferRef pixelBuffer,
                     double elapsedSeconds,
                     const FrameDecision& decision) override {
        if (!m_initialized || !m_session || !pixelBuffer) {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Phase 1 ignores QP/ROI hints from `decision`; RoiSupported() == false.
        // We still touch fields so a later phase can hook in without the
        // compiler dropping them.
        (void)decision.frameIndex;
        (void)decision.scene.source;

        const auto submitStart = std::chrono::high_resolution_clock::now();

        const CMTime pts = CMTimeMakeWithSeconds(elapsedSeconds, kPtsTimescale);
        const CMTime duration = CMTimeMake(1, m_fps);

        VTEncodeInfoFlags infoFlags = 0;
        const OSStatus status = VTCompressionSessionEncodeFrame(
            m_session,
            pixelBuffer,
            pts,
            duration,
            /*frameProperties*/ nullptr,
            /*sourceFrameRefcon*/ nullptr,
            &infoFlags);

        const auto submitEnd = std::chrono::high_resolution_clock::now();
        m_lastSubmitMs.store(
            std::chrono::duration<double, std::milli>(submitEnd - submitStart).count(),
            std::memory_order_relaxed);

        if (status != noErr) {
            m_failures.fetch_add(1, std::memory_order_relaxed);
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "VTCompressionSessionEncodeFrame failed status=%d",
                          static_cast<int>(status));
            AppendError(msg);
            return false;
        }
        return true;
    }

    void Flush() override {
        // Safe to call even if Initialize() was never invoked.
        if (m_session) {
            VTCompressionSessionCompleteFrames(m_session, kCMTimeInvalid);
        }
        TeardownCompressionSession();

        @autoreleasepool {
            if (!m_writer) {
                return;
            }
            AVAssetWriterInput* input = m_writerInput;
            AVAssetWriter* writer = m_writer;
            dispatch_queue_t queue = m_writerQueue;

            // Mark the input finished on the writer queue so it serializes
            // after any in-flight appendSampleBuffer:.
            if (input != nil && queue != nil) {
                dispatch_sync(queue, ^{
                    [input markAsFinished];
                });
            }

            if (writer.status == AVAssetWriterStatusWriting) {
                dispatch_semaphore_t done = dispatch_semaphore_create(0);
                [writer finishWritingWithCompletionHandler:^{
                    dispatch_semaphore_signal(done);
                }];
                // 5s is generous — phase 1 only writes ~30s of 1080p30.
                dispatch_semaphore_wait(done,
                    dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)));
                if (writer.status == AVAssetWriterStatusFailed) {
                    NSString* desc = writer.error.localizedDescription ?: @"unknown";
                    AppendError(std::string("AVAssetWriter finishWriting failed: ")
                                + [desc UTF8String]);
                }
            } else if (writer.status == AVAssetWriterStatusFailed) {
                NSString* desc = writer.error.localizedDescription ?: @"unknown";
                AppendError(std::string("AVAssetWriter status=Failed before flush: ")
                            + [desc UTF8String]);
            }

            m_writerInput = nil;
            m_writer = nil;
            m_writerQueue = nil;
        }
    }

    int EncodedFrameCount() const override {
        return m_encoded.load(std::memory_order_relaxed);
    }
    int EncodeFailureCount() const override {
        return m_failures.load(std::memory_order_relaxed);
    }
    bool RoiSupported() const override { return false; }
    double LastEncodeSubmitMs() const override {
        return m_lastSubmitMs.load(std::memory_order_relaxed);
    }

    // Concrete-only diagnostic. Not on the abstract interface; the shim has
    // its own errors list in macos_smoke.json.
    std::vector<std::string> Errors() const {
        std::lock_guard<std::mutex> lock(m_errorsMutex);
        return m_errors;
    }

private:
    // --- Output handling -------------------------------------------------

    bool PrepareOutputFile(const std::string& outputPath) {
        @autoreleasepool {
            NSString* path = [NSString stringWithUTF8String:outputPath.c_str()];
            if (path == nil) {
                AppendError("PrepareOutputFile: outputPath is not valid UTF-8.");
                return false;
            }
            NSFileManager* fm = [NSFileManager defaultManager];
            NSString* parent = [path stringByDeletingLastPathComponent];
            if (parent.length > 0) {
                NSError* mkErr = nil;
                if (![fm createDirectoryAtPath:parent
                   withIntermediateDirectories:YES
                                    attributes:nil
                                         error:&mkErr]) {
                    AppendError(std::string("createDirectoryAtPath failed: ")
                                + [mkErr.localizedDescription UTF8String]);
                    return false;
                }
            }
            if ([fm fileExistsAtPath:path]) {
                NSError* rmErr = nil;
                if (![fm removeItemAtPath:path error:&rmErr]) {
                    AppendError(std::string("removeItemAtPath failed: ")
                                + [rmErr.localizedDescription UTF8String]);
                    return false;
                }
            }
            return true;
        }
    }

    bool CreateAssetWriter(const std::string& outputPath) {
        @autoreleasepool {
            NSString* path = [NSString stringWithUTF8String:outputPath.c_str()];
            NSURL* url = [NSURL fileURLWithPath:path];
            NSError* err = nil;
            AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                              fileType:AVFileTypeMPEG4
                                                                 error:&err];
            if (writer == nil || err != nil) {
                AppendError(std::string("AVAssetWriter init failed: ")
                            + (err ? [err.localizedDescription UTF8String] : "nil"));
                return false;
            }
            writer.shouldOptimizeForNetworkUse = YES;
            m_writer = writer;
            m_writerQueue = dispatch_queue_create(
                "com.aetherflow.mac.assetwriter", DISPATCH_QUEUE_SERIAL);
            return true;
        }
    }

    // --- Compression session ---------------------------------------------

    bool CreateCompressionSession() {
        // Mirror ScreenCaptureKit + MacosPrivacyMaskCompositor pixel format
        // (BGRA, IOSurface-backed). VTCompressionSession accepts BGRA source
        // buffers; it converts to its internal working surface with no CPU
        // readback, and the muxed H.264 bitstream is unchanged.
        NSDictionary* sourceAttrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey:
                @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey:  @(m_width),
            (id)kCVPixelBufferHeightKey: @(m_height),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };

        VTCompressionSessionRef session = nullptr;
        const OSStatus status = VTCompressionSessionCreate(
            kCFAllocatorDefault,
            m_width,
            m_height,
            kCMVideoCodecType_H264,
            /*encoderSpecification*/ nullptr,
            (__bridge CFDictionaryRef)sourceAttrs,
            /*compressedDataAllocator*/ nullptr,
            &VideoToolboxH264Encoder::CompressionOutputCallback,
            this,
            &session);
        if (status != noErr || session == nullptr) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "VTCompressionSessionCreate failed status=%d",
                          static_cast<int>(status));
            AppendError(msg);
            return false;
        }
        m_session = session;

        if (!ConfigureCompressionProperties()) {
            TeardownCompressionSession();
            return false;
        }

        const OSStatus prepared = VTCompressionSessionPrepareToEncodeFrames(m_session);
        if (prepared != noErr) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "VTCompressionSessionPrepareToEncodeFrames failed status=%d",
                          static_cast<int>(prepared));
            AppendError(msg);
            TeardownCompressionSession();
            return false;
        }
        return true;
    }

    bool ConfigureCompressionProperties() {
        if (!SetSessionProperty(kVTCompressionPropertyKey_RealTime,
                                (__bridge CFTypeRef)@YES)) return false;
        if (!SetSessionProperty(kVTCompressionPropertyKey_AllowFrameReordering,
                                (__bridge CFTypeRef)@NO)) return false;
        if (!SetSessionProperty(kVTCompressionPropertyKey_ProfileLevel,
                                kVTProfileLevel_H264_Main_AutoLevel))
            return false;
        if (!SetSessionProperty(kVTCompressionPropertyKey_AverageBitRate,
                                (__bridge CFTypeRef)@(kDefaultBitrateBps))) return false;
        if (!SetSessionProperty(kVTCompressionPropertyKey_ExpectedFrameRate,
                                (__bridge CFTypeRef)@(m_fps))) return false;
        // 1 keyframe every 2 seconds.
        if (!SetSessionProperty(kVTCompressionPropertyKey_MaxKeyFrameInterval,
                                (__bridge CFTypeRef)@(m_fps * 2))) return false;
        return true;
    }

    bool SetSessionProperty(CFStringRef key, CFTypeRef value) {
        const OSStatus status = VTSessionSetProperty(m_session, key, value);
        if (status != noErr) {
            CFStringRef keyDesc = key;
            char keyBuf[128] = {0};
            CFStringGetCString(keyDesc, keyBuf, sizeof(keyBuf), kCFStringEncodingUTF8);
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "VTSessionSetProperty(%s) failed status=%d",
                          keyBuf, static_cast<int>(status));
            AppendError(msg);
            return false;
        }
        return true;
    }

    void TeardownCompressionSession() {
        if (m_session) {
            VTCompressionSessionInvalidate(m_session);
            CFRelease(m_session);
            m_session = nullptr;
        }
    }

    // --- VT output callback ----------------------------------------------

    static void CompressionOutputCallback(
        void* outputCallbackRefCon,
        void* /*sourceFrameRefCon*/,
        OSStatus status,
        VTEncodeInfoFlags infoFlags,
        CMSampleBufferRef sampleBuffer)
    {
        auto* self = static_cast<VideoToolboxH264Encoder*>(outputCallbackRefCon);
        if (!self) return;
        if (status != noErr || sampleBuffer == nullptr) {
            self->m_failures.fetch_add(1, std::memory_order_relaxed);
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "VT output callback status=%d sampleBuffer=%s",
                          static_cast<int>(status),
                          sampleBuffer ? "ok" : "null");
            self->AppendError(msg);
            return;
        }
        if ((infoFlags & kVTEncodeInfo_FrameDropped) != 0) {
            self->m_failures.fetch_add(1, std::memory_order_relaxed);
            self->AppendError("VT output callback: frame dropped.");
            return;
        }
        // Ignore non-data sample buffers.
        if (!CMSampleBufferDataIsReady(sampleBuffer)) {
            self->m_failures.fetch_add(1, std::memory_order_relaxed);
            self->AppendError("VT output callback: sample buffer not ready.");
            return;
        }
        self->HandleCompressedSample(sampleBuffer);
    }

    void HandleCompressedSample(CMSampleBufferRef sampleBuffer) {
        @autoreleasepool {
            // Lazy AVAssetWriterInput creation on first sample so we can pass
            // VT's exact CMVideoFormatDescription through to passthrough mux.
            if (m_writerInput == nil) {
                if (!CreateWriterInputForSample(sampleBuffer)) {
                    m_failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (!StartWriterSessionWithSample(sampleBuffer)) {
                    m_failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }

            // Hand off to the serial writer queue. Retain the buffer; release
            // happens inside the block.
            CFRetain(sampleBuffer);
            CMSampleBufferRef toAppend = sampleBuffer;
            AVAssetWriterInput* input = m_writerInput;
            AVAssetWriter* writer = m_writer;
            dispatch_async(m_writerQueue, ^{
                @autoreleasepool {
                    // Lightweight spin if the writer is back-pressuring. At
                    // 30fps this almost never triggers, but it's safer than a
                    // silent drop.
                    int spins = 0;
                    while (!input.readyForMoreMediaData && spins < 100) {
                        usleep(1000); // 1 ms
                        ++spins;
                    }
                    if (!input.readyForMoreMediaData) {
                        m_failures.fetch_add(1, std::memory_order_relaxed);
                        AppendError("AVAssetWriterInput not ready after spin; dropping frame.");
                        CFRelease(toAppend);
                        return;
                    }
                    BOOL ok = [input appendSampleBuffer:toAppend];
                    if (!ok) {
                        m_failures.fetch_add(1, std::memory_order_relaxed);
                        NSString* desc = writer.error.localizedDescription ?: @"unknown";
                        AppendError(std::string("appendSampleBuffer failed: ")
                                    + [desc UTF8String]);
                    } else {
                        m_encoded.fetch_add(1, std::memory_order_relaxed);
                    }
                    CFRelease(toAppend);
                }
            });
        }
    }

    bool CreateWriterInputForSample(CMSampleBufferRef sampleBuffer) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (fmt == nullptr) {
            AppendError("CreateWriterInputForSample: missing format description.");
            return false;
        }
        // outputSettings:nil + a sourceFormatHint puts the input in passthrough
        // mode. AVAssetWriter will mux the VT bitstream as-is.
        AVAssetWriterInput* input = [[AVAssetWriterInput alloc]
            initWithMediaType:AVMediaTypeVideo
               outputSettings:nil
             sourceFormatHint:fmt];
        if (input == nil) {
            AppendError("AVAssetWriterInput init returned nil.");
            return false;
        }
        input.expectsMediaDataInRealTime = YES;
        if (![m_writer canAddInput:input]) {
            AppendError("AVAssetWriter canAddInput == NO.");
            return false;
        }
        [m_writer addInput:input];
        m_writerInput = input;
        return true;
    }

    bool StartWriterSessionWithSample(CMSampleBufferRef sampleBuffer) {
        if (![m_writer startWriting]) {
            NSString* desc = m_writer.error.localizedDescription ?: @"unknown";
            AppendError(std::string("AVAssetWriter startWriting failed: ")
                        + [desc UTF8String]);
            return false;
        }
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        [m_writer startSessionAtSourceTime:pts];
        return true;
    }

    // --- Diagnostics ------------------------------------------------------

    void AppendError(const std::string& msg) {
        std::fprintf(stderr, "[AetherFlow][macOS][VT] %s\n", msg.c_str());
        std::lock_guard<std::mutex> lock(m_errorsMutex);
        if (m_errors.size() < 64) {
            m_errors.push_back(msg);
        }
    }

    // --- State ------------------------------------------------------------

    bool m_initialized = false;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
    std::string m_outputPath;

    VTCompressionSessionRef m_session = nullptr;
    AVAssetWriter* m_writer = nil;
    AVAssetWriterInput* m_writerInput = nil;
    dispatch_queue_t m_writerQueue = nil;

    std::atomic<int> m_encoded{0};
    std::atomic<int> m_failures{0};
    std::atomic<double> m_lastSubmitMs{0.0};

    mutable std::mutex m_errorsMutex;
    std::vector<std::string> m_errors;
};

} // namespace

std::unique_ptr<IPlatformEncoderMac> MakeVideoToolboxH264Encoder() {
    return std::make_unique<VideoToolboxH264Encoder>();
}

} // namespace mac
} // namespace platform
} // namespace AetherFlow
