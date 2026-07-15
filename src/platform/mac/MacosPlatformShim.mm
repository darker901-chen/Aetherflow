// MacosPlatformShim.mm — macOS phase 1 run loop.
//
// Mirrors the deterministic decision layer wiring used by main.cpp on
// Windows: BaselineSceneModule + CursorFocusModule + a macOS-specific
// secure-text-field placeholder. Phase 2 will add the privacy-mask
// compositor and a panic hotkey on top of this same structure.

#include "AetherFlow/platform/mac/MacosPlatformShim.h"

#include "AetherFlow/IAIFrameAnalyzer.h"
#include "AetherFlow/AsyncAnalyzerBridgeModule.h"
#include "AetherFlow/MockSlowAnalyzer.h"
#include "AetherFlow/platform/mac/IPlatformEncoderMac.h"
#include "AetherFlow/platform/mac/MacosNotificationProducerModule.h"
#include "AetherFlow/platform/mac/MacosPrivacyMaskCompositor.h"
#include "AetherFlow/platform/mac/MacosScreenCapture.h"
#include "AetherFlow/platform/mac/MacosSecureTextFieldPrivacyMaskModule.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace AetherFlow {
namespace platform {
namespace mac {

namespace {

std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch))
                    << std::dec << std::setfill(' ');
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

double MsBetween(std::chrono::high_resolution_clock::time_point end,
                 std::chrono::high_resolution_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void EnsureParentDir(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::path p(path);
    auto parent = p.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
}

void WriteMacosSmokeJson(const std::string& path,
                         const MacosRunResult& result,
                         const std::string& schemaVersion) {
    EnsureParentDir(path);
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << "{\n";
    out << "  \"schema_version\": " << schemaVersion << ",\n";
    out << "  \"platform\": \"macos\",\n";
    out << "  \"screen_capture_permission\": \""
        << JsonEscape(result.permissionState) << "\",\n";
    out << "  \"capture_backend\": \""
        << JsonEscape(result.captureBackend) << "\",\n";
    out << "  \"encoder_backend\": \""
        << JsonEscape(result.encoderBackend) << "\",\n";
    out << "  \"captured_frames\": " << result.capturedFrames << ",\n";
    out << "  \"encoded_frames\": " << result.encodedFrames << ",\n";
    out << "  \"encode_failure_frames\": " << result.encodeFailureFrames << ",\n";
    out << "  \"duration_seconds\": " << std::fixed << std::setprecision(3)
        << result.durationSeconds << ",\n";
    out << "  \"output_path\": \"" << JsonEscape(result.outputPath) << "\",\n";
    out << "  \"errors\": [";
    for (size_t i = 0; i < result.errors.size(); ++i) {
        if (i > 0) out << ", ";
        out << "\"" << JsonEscape(result.errors[i]) << "\"";
    }
    out << "]\n";
    out << "}\n";
}

// Per-frame trace record. Field names mirror WriteFrameTraceJson in
// src/main.cpp so tools/agent_summarize.py reads either platform's traces
// without modification.
struct TraceRecord {
    int frameIndex = 0;
    bool encodeOk = false;
    std::string sceneType = "unknown";
    std::string sceneSource = "none";
    float sceneConfidence = 0.0f;
    std::string sceneDebugLabel;
    int roiCenterX = 0;
    int roiCenterY = 0;
    size_t qualityRegionCount = 0;
    size_t privacyMaskCount = 0;
    int privacyMaskAppliedCount = 0;
    std::string privacyMaskSource = "none";
    std::string privacyMaskDebugLabel;
    std::string privacyMaskPath = "none";
    bool privacyMaskFallbackUsed = false;
    bool panicMaskActive = false;
    std::string decisionSource = "none";
    std::string debugLabel;
    double captureMs = 0.0;
    double decisionMs = 0.0;
    double maskMs = 0.0;
    double convertMs = 0.0;
    double encodeSubmitMs = 0.0;
    double totalMs = 0.0;
    // Bridge-hardening per-frame analyzer state. Only serialized when
    // emitAnalyzerFields is true (i.e. options.mockAnalyzerEnabled).
    bool analyzerSubmitted = false;
    bool analyzerContributed = false;
    double analyzerInferenceMs = 0.0;
    int analyzerStalenessFrames = 0;
};

void WriteTraceLine(std::ofstream& out, const TraceRecord& r, bool emitAnalyzerFields) {
    if (!out.is_open()) return;
    out << "{"
        << "\"frameIndex\":" << r.frameIndex
        << ",\"encodeOk\":" << (r.encodeOk ? "true" : "false")
        << ",\"sceneType\":\"" << JsonEscape(r.sceneType) << "\""
        << ",\"sceneSource\":\"" << JsonEscape(r.sceneSource) << "\""
        << ",\"sceneConfidence\":" << std::fixed << std::setprecision(3) << r.sceneConfidence
        << ",\"sceneDebugLabel\":\"" << JsonEscape(r.sceneDebugLabel) << "\""
        << ",\"roiCenterX\":" << r.roiCenterX
        << ",\"roiCenterY\":" << r.roiCenterY
        << ",\"qualityRegionCount\":" << r.qualityRegionCount
        << ",\"privacyMaskCount\":" << r.privacyMaskCount
        << ",\"privacyMaskAppliedCount\":" << r.privacyMaskAppliedCount
        << ",\"privacyMaskSource\":\"" << JsonEscape(r.privacyMaskSource) << "\""
        << ",\"privacyMaskDebugLabel\":\"" << JsonEscape(r.privacyMaskDebugLabel) << "\""
        << ",\"privacyMaskPath\":\"" << JsonEscape(r.privacyMaskPath) << "\""
        << ",\"privacyMaskFallbackUsed\":" << (r.privacyMaskFallbackUsed ? "true" : "false")
        << ",\"panicMaskActive\":" << (r.panicMaskActive ? "true" : "false")
        << ",\"decisionSource\":\"" << JsonEscape(r.decisionSource) << "\""
        << ",\"debugLabel\":\"" << JsonEscape(r.debugLabel) << "\""
        << ",\"captureMs\":" << r.captureMs
        << ",\"decisionMs\":" << r.decisionMs
        << ",\"maskMs\":" << r.maskMs
        << ",\"convertMs\":" << r.convertMs
        << ",\"encodeSubmitMs\":" << r.encodeSubmitMs
        << ",\"totalMs\":" << r.totalMs;
    if (emitAnalyzerFields) {
        // Strategy A: only emit these fields when the bridge is active. Keeps
        // the analyzer_bridge_no_mock regression byte-equivalent on macOS.
        out << ",\"analyzerSubmitted\":" << (r.analyzerSubmitted ? "true" : "false")
            << ",\"analyzerContributed\":" << (r.analyzerContributed ? "true" : "false")
            << ",\"analyzerInferenceMs\":" << std::fixed << std::setprecision(3) << r.analyzerInferenceMs
            << ",\"analyzerStalenessFrames\":" << r.analyzerStalenessFrames;
    }
    out << "}\n";
}

} // namespace

int RunMacosPipeline(const MacosRunOptions& options, MacosRunResult* outResult) {
    MacosRunResult localResult;
    if (!outResult) outResult = &localResult;
    *outResult = MacosRunResult{};

    // Resolve the output container path. Encoder agent will eventually open
    // this via AVAssetWriter; phase 1 stub records it but does not emit a
    // real .mp4 file.
    std::string outputPath;
    if (!options.outputDir.empty()) {
        std::filesystem::path p(options.outputDir);
        p /= "output.mp4";
        outputPath = p.string();
    } else {
        outputPath = "output/output.mp4";
    }
    outResult->outputPath = outputPath;
    EnsureParentDir(outputPath);

    // Step 1: probe permission. If denied, write the smoke artifact and exit.
    bool granted = MacosScreenCapture::ProbeScreenCapturePermission();
    outResult->permissionState = granted ? "granted" : "denied";
    if (!granted) {
        outResult->errors.push_back("screen recording permission not granted");
        WriteMacosSmokeJson(options.macosSmokeJsonPath, *outResult, "1");
        std::cerr << "[AetherFlow][macOS] Screen recording permission denied; aborting.\n";
        return 10;
    }

    // Step 2: bring up capture and encoder.
    MacosScreenCapture capture;
    if (!capture.Init(options.captureWidth, options.captureHeight, options.targetFps)) {
        outResult->errors.push_back("MacosScreenCapture::Init failed");
        WriteMacosSmokeJson(options.macosSmokeJsonPath, *outResult, "1");
        std::cerr << "[AetherFlow][macOS] Capture init failed.\n";
        return 11;
    }
    const int capW = capture.GetCaptureWidth();
    const int capH = capture.GetCaptureHeight();
    const int capL = capture.GetCaptureLeft();
    const int capT = capture.GetCaptureTop();

    auto encoder = MakeVideoToolboxH264Encoder();
    if (!encoder ||
        !encoder->Initialize(capW, capH, options.targetFps, outputPath)) {
        outResult->errors.push_back("VideoToolbox encoder init failed");
        WriteMacosSmokeJson(options.macosSmokeJsonPath, *outResult, "1");
        std::cerr << "[AetherFlow][macOS] Encoder init failed.\n";
        return 12;
    }

    // Step 3: deterministic policy engine wiring. Same modules main.cpp uses,
    // minus the privacy compositor (phase 2). We add the macOS secure text
    // field placeholder so the decision_sources histogram already records the
    // dedicated module name.
    BaselineSceneModule baselineScene;
    CursorFocusModule cursorFocus(options.cursorRoiRadiusPx > 0
                                      ? options.cursorRoiRadiusPx : 200);
    MacosSecureTextFieldPrivacyMaskModule secureTextField;
    // Notification / messenger producer (CGWindowList-based). Only wired when
    // explicitly enabled AND a non-empty owner-name whitelist was resolved by
    // main.cpp; mirrors the Windows shim's `if (notificationMaskEnabled)`
    // gating around NotificationProducerModule.
    const bool notificationProducerActive =
        options.notificationMaskEnabled && !options.notificationOwnerWhitelist.empty();
    MacosNotificationProducerModule notificationProducer(
        options.notificationOwnerWhitelist,
        options.notificationMaskPollFrames > 0 ? options.notificationMaskPollFrames : 5,
        options.notificationPaddingPx >= 0 ? options.notificationPaddingPx : 4);
    // Async analyzer bridge (P2) + mock slow analyzer. Pure C++,
    // cross-platform; only registered when explicitly enabled so the
    // deterministic phase-2 mosaic regression run stays unchanged.
    // Bridge-hardening (Phase 4 P0 prerequisite): submit cadence and mock
    // inference delay are user-tunable; defaults match Windows (every-frame,
    // 200 ms).
    MockSlowAnalyzer mockSlowAnalyzer(options.mockAnalyzerInferenceMs > 0
                                          ? options.mockAnalyzerInferenceMs : 200);
    AsyncAnalyzerBridgeModule asyncAnalyzerBridge(
        options.mockAnalyzerEnabled ? &mockSlowAnalyzer
                                    : static_cast<IAIFrameAnalyzer*>(nullptr),
        options.analyzerBridgeIntervalFrames > 0
            ? options.analyzerBridgeIntervalFrames : 1,
        /*dropWhenBusy=*/true);
    FramePolicyEngine policy;
    // Module order mirrors the Windows main.cpp pipeline:
    //   1. secure text field placeholder (privacy mask producer)
    //   2. baseline scene (low-confidence fallback under P1 merge)
    //   3. cursor focus (quality ROI)
    //   4. notification producer (privacy mask producer)
    //   5. async analyzer bridge (last, so confidence-based merge competes
    //      against an already-populated baseline / deterministic claim)
    policy.AddModule(&secureTextField);
    policy.AddModule(&baselineScene);
    policy.AddModule(&cursorFocus);
    if (notificationProducerActive) {
        policy.AddModule(&notificationProducer);
    }
    if (options.mockAnalyzerEnabled) {
        policy.AddModule(&asyncAnalyzerBridge);
    }
    if (!policy.Initialize(nullptr, capW, capH)) {
        outResult->errors.push_back("FramePolicyEngine init failed");
        WriteMacosSmokeJson(options.macosSmokeJsonPath, *outResult, "1");
        std::cerr << "[AetherFlow][macOS] Policy init failed.\n";
        return 13;
    }

    // CoreImage privacy-mask compositor. Runs after policy.Evaluate() and
    // before encoder->EncodeFrame(). Mirrors the Windows PrivacyMaskCompositor
    // position in the pipeline.
    MacosPrivacyMaskMode compositorMode = MacosPrivacyMaskMode::Blur;
    if (options.privacyMaskMode == "blackout") {
        compositorMode = MacosPrivacyMaskMode::Blackout;
    } else if (options.privacyMaskMode == "mosaic") {
        compositorMode = MacosPrivacyMaskMode::Mosaic;
    } else if (options.privacyMaskMode != "blur") {
        std::cerr << "[AetherFlow][macOS] Unknown privacy_mask_mode='"
                  << options.privacyMaskMode << "'; using 'blur'.\n";
    }
    MacosPrivacyMaskCompositor compositor;
    if (!compositor.Initialize(capW, capH, capW, capH, compositorMode,
                               options.privacyMaskMosaicBlockPx > 0
                                   ? options.privacyMaskMosaicBlockPx : 16)) {
        outResult->errors.push_back("MacosPrivacyMaskCompositor::Initialize failed");
        WriteMacosSmokeJson(options.macosSmokeJsonPath, *outResult, "1");
        std::cerr << "[AetherFlow][macOS] Compositor init failed.\n";
        return 15;
    }

    EnsureParentDir(options.traceJsonlPath);
    std::ofstream trace(options.traceJsonlPath, std::ios::out | std::ios::trunc);
    if (!trace.is_open()) {
        outResult->errors.push_back("frame_trace.jsonl open failed");
        WriteMacosSmokeJson(options.macosSmokeJsonPath, *outResult, "1");
        std::cerr << "[AetherFlow][macOS] Could not open frame trace at "
                  << options.traceJsonlPath << "\n";
        return 14;
    }

    std::cout << "[AetherFlow][macOS] Capture=" << capW << "x" << capH
              << " fps=" << options.targetFps
              << " duration_frames=" << options.durationFrames
              << " roi_radius=" << options.cursorRoiRadiusPx
              << " notification_mask=" << (notificationProducerActive ? "on" : "off")
              << " privacy_mask_mode=" << options.privacyMaskMode << "\n";

    // Warm up producers before the encode loop. The notification producer
    // starts its background poll thread here, so its CGWindowList cold-start
    // (CoreGraphics service connection) is paid off the capture/encode
    // thread instead of stalling the first encode frames.
    {
        FrameContext warmup;
        warmup.frameIndex = -1;
        warmup.width = capW;
        warmup.height = capH;
        warmup.captureLeft = capL;
        warmup.captureTop = capT;
        warmup.captureWidth = capW;
        warmup.captureHeight = capH;
        policy.Warmup(warmup);
    }

    int privacyMaskTotal = 0;
    int privacyMaskAppliedTotal = 0;
    int privacyMaskFallbackFrames = 0;

    // Step 4: run loop.
    auto runStart = std::chrono::high_resolution_clock::now();
    int captured = 0;
    int encoded = 0;
    int encodeFailures = 0;
    const double frameTimeoutSec = 1.0; // generous; ScreenCaptureKit cadence is fps-driven
    for (int frameIdx = 0; frameIdx < options.durationFrames; ++frameIdx) {
        TraceRecord rec;
        rec.frameIndex = frameIdx;

        auto t0 = std::chrono::high_resolution_clock::now();
        CVPixelBufferRef pix = nullptr;
        double elapsed = 0.0;
        bool hasCursor = false;
        int cursorX = 0;
        int cursorY = 0;
        const bool gotFrame = capture.WaitNextFrame(frameTimeoutSec,
                                                    &pix,
                                                    &elapsed,
                                                    &hasCursor,
                                                    &cursorX,
                                                    &cursorY);
        auto tCapture = std::chrono::high_resolution_clock::now();
        rec.captureMs = MsBetween(tCapture, t0);
        if (!gotFrame || !pix) {
            // Treat as a capture miss. Continue without bumping captured.
            rec.encodeOk = false;
            rec.totalMs = MsBetween(std::chrono::high_resolution_clock::now(), t0);
            WriteTraceLine(trace, rec, options.mockAnalyzerEnabled);
            continue;
        }
        ++captured;

        FrameContext ctx;
        ctx.nv12Texture = nullptr;
        ctx.frameIndex = frameIdx;
        ctx.elapsedSeconds = elapsed;
        ctx.width = capW;
        ctx.height = capH;
        ctx.captureLeft = capL;
        ctx.captureTop = capT;
        ctx.captureWidth = capW;
        ctx.captureHeight = capH;
        ctx.hasCursor = hasCursor;
        ctx.cursorX = cursorX;
        ctx.cursorY = cursorY;
        // Phase 1: no panic-mask hotkey on macOS yet.

        FrameDecision decision = policy.Evaluate(ctx);
        auto tDecision = std::chrono::high_resolution_clock::now();
        rec.decisionMs = MsBetween(tDecision, tCapture);

        // Bridge-hardening per-frame analyzer state. Always read; the writer
        // gates emission on options.mockAnalyzerEnabled.
        rec.analyzerSubmitted = asyncAnalyzerBridge.LastSubmitted();
        rec.analyzerContributed = asyncAnalyzerBridge.LastContributed();
        rec.analyzerInferenceMs = asyncAnalyzerBridge.LastInferenceMs();
        rec.analyzerStalenessFrames = asyncAnalyzerBridge.LastStalenessFrames();

        CVPixelBufferRef maskedPix = pix;
        MacosPrivacyMaskApplyStats maskStats;
        if (!compositor.Compose(pix, decision, &maskedPix, &maskStats)) {
            // Hard pool-acquire failure — skip encode for this frame, keep
            // tracing it as encodeOk=false. Compose() already noted the reason
            // in maskStats.failureReason.
            rec.encodeOk = false;
            rec.maskMs = compositor.LastComposeMs();
            rec.privacyMaskCount = static_cast<int>(decision.privacyMasks.size());
            rec.privacyMaskAppliedCount = 0;
            rec.privacyMaskPath = "none";
            rec.privacyMaskFallbackUsed = true;
            ++encodeFailures;
            ++privacyMaskFallbackFrames;
            if (pix) { CFRelease(pix); pix = nullptr; }
            rec.totalMs = MsBetween(std::chrono::high_resolution_clock::now(), t0);
            WriteTraceLine(trace, rec, options.mockAnalyzerEnabled);
            continue;
        }
        rec.maskMs = compositor.LastComposeMs();
        rec.convertMs = 0.0;
        auto tConvert = std::chrono::high_resolution_clock::now();

        const bool ok = encoder->EncodeFrame(maskedPix, elapsed, decision);
        auto tEncode = std::chrono::high_resolution_clock::now();
        rec.encodeSubmitMs = MsBetween(tEncode, tConvert);
        rec.encodeOk = ok;
        if (ok) {
            ++encoded;
        } else {
            ++encodeFailures;
        }

        // Compositor returns a +1-retained pool buffer when masks were
        // applied (maskedPix != pix). Release that here; the pool reclaims
        // it once VT releases its internal retain after the async encode.
        if (maskedPix && maskedPix != pix) {
            CFRelease(maskedPix);
        }
        if (pix) {
            CFRelease(pix);
            pix = nullptr;
        }
        maskedPix = nullptr;

        // Populate scene/region metadata.
        rec.sceneType = FrameSceneTypeName(decision.scene.type);
        rec.sceneSource = decision.scene.source.empty() ? std::string("none") : decision.scene.source;
        rec.sceneConfidence = decision.scene.confidence;
        rec.sceneDebugLabel = decision.scene.debugLabel;
        rec.qualityRegionCount = decision.qualityRegions.size();
        rec.privacyMaskCount = maskStats.requestedCount > 0
                                   ? maskStats.requestedCount
                                   : static_cast<int>(decision.privacyMasks.size());
        rec.privacyMaskAppliedCount = maskStats.appliedCount;
        rec.privacyMaskPath = maskStats.path;
        rec.privacyMaskFallbackUsed = maskStats.fallbackUsed;
        privacyMaskTotal += rec.privacyMaskCount;
        privacyMaskAppliedTotal += maskStats.appliedCount;
        if (maskStats.fallbackUsed) {
            ++privacyMaskFallbackFrames;
        }
        if (!decision.qualityRegions.empty()) {
            const auto& roi = decision.qualityRegions.front();
            rec.roiCenterX = roi.CenterX();
            rec.roiCenterY = roi.CenterY();
            rec.decisionSource = roi.source.empty() ? std::string("none") : roi.source;
            rec.debugLabel = roi.debugLabel;
        } else {
            rec.roiCenterX = hasCursor ? cursorX : (capW / 2);
            rec.roiCenterY = hasCursor ? cursorY : (capH / 2);
        }
        if (!decision.privacyMasks.empty()) {
            const auto& mask = decision.privacyMasks.front();
            rec.privacyMaskSource = mask.source.empty() ? std::string("none") : mask.source;
            rec.privacyMaskDebugLabel = mask.debugLabel;
        }
        rec.totalMs = MsBetween(tEncode, t0);
        WriteTraceLine(trace, rec, options.mockAnalyzerEnabled);
    }

    encoder->Flush();
    capture.Close();

    auto runEnd = std::chrono::high_resolution_clock::now();
    outResult->capturedFrames = captured;
    outResult->encodedFrames = encoded;
    outResult->encodeFailureFrames = encodeFailures;
    outResult->durationSeconds = std::chrono::duration<double>(runEnd - runStart).count();
    outResult->privacyMaskTotal = privacyMaskTotal;
    outResult->privacyMaskAppliedTotal = privacyMaskAppliedTotal;
    outResult->privacyMaskFallbackFrames = privacyMaskFallbackFrames;

    if (trace.is_open()) {
        trace.flush();
        trace.close();
    }

    WriteMacosSmokeJson(options.macosSmokeJsonPath, *outResult, "1");

    std::cout << "[AetherFlow][macOS] captured=" << captured
              << " encoded=" << encoded
              << " encode_failures=" << encodeFailures
              << " duration_s=" << std::fixed << std::setprecision(3)
              << outResult->durationSeconds << "\n";
    return 0;
}

} // namespace mac
} // namespace platform
} // namespace AetherFlow
