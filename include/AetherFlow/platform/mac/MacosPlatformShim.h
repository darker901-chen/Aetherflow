#pragma once

// Macos platform shim: orchestrates ScreenCaptureKit capture, the deterministic
// FramePolicyEngine (BaselineSceneModule + CursorFocusModule + macOS secure
// text-field stub), and the VideoToolbox encoder. This is the macOS
// counterpart of main.cpp's Windows pipeline; it produces frame_trace.jsonl
// using the same per-frame schema as Windows so tools/agent_summarize.py and
// tools/agent_verify.py work unchanged.

#include <string>
#include <vector>

namespace AetherFlow {
namespace platform {
namespace mac {

// Forward declaration. Encoder agent's stage 2 will introduce
// MacosPrivacyMaskCompositor (BGRA capture + CoreImage blur/mosaic/blackout).
// We only need the name visible in this header so a future opaque
// std::unique_ptr<MacosPrivacyMaskCompositor> member can live on the shim's
// frame-loop state struct without pulling in the not-yet-existing header.
class MacosPrivacyMaskCompositor;

struct MacosRunOptions {
    int targetFps = 30;
    int durationFrames = 500;             // matches Windows default (AETHERFLOW_MAX_FRAMES fallback)
    int captureWidth = 0;                 // 0 -> use main display native size
    int captureHeight = 0;
    int cursorRoiRadiusPx = 200;
    std::string outputDir;                // run_dir/artifacts (where output/output.mp4 lives)
    std::string traceJsonlPath;           // run_dir/frame_trace.jsonl
    std::string macosSmokeJsonPath;       // run_dir/macos_smoke.json
    std::string consoleLogPath;           // optional duplicate console sink

    // Notification / privacy-mask producer wiring. Defaults match the Windows
    // demo path: enabled, blur mode, 5-frame poll, 4px padding. The default
    // owner-name whitelist (LINE / Slack / Discord / Teams / Telegram /
    // WhatsApp / Messages) is materialized by main.cpp when env + CLI leave
    // the list empty.
    bool notificationMaskEnabled = true;
    std::vector<std::string> notificationOwnerWhitelist;     // resolved by main.cpp
    int notificationMaskPollFrames = 5;
    std::string privacyMaskMode = "mosaic";                  // "blackout" | "blur" | "mosaic"; default = mosaic (chat-window-mosaic feature)
    int privacyMaskMosaicBlockPx = 16;
    int notificationPaddingPx = 4;

    // Async analyzer bridge (P2) + mock slow analyzer wiring. Default off;
    // opt-in via `--mock-analyzer` CLI or `AETHERFLOW_MOCK_ANALYZER=1` env.
    // Cross-platform: the bridge and mock are pure C++ and build on macOS
    // unchanged.
    //
    // Bridge-hardening (Phase 4 P0 prerequisite) adds two opt-in tuning
    // knobs that mirror the Windows path. They are evaluated regardless of
    // mockAnalyzerEnabled but only have an effect when the bridge is wired
    // in (i.e. mockAnalyzerEnabled == true on the mock path; future real
    // analyzers will check a separate flag):
    //   - analyzerBridgeIntervalFrames: how often the bridge calls
    //     SubmitFrame on the analyzer (1 = every frame). Set via
    //     `--analyzer-bridge-interval-frames=N` /
    //     `AETHERFLOW_ANALYZER_BRIDGE_INTERVAL_FRAMES`.
    //   - mockAnalyzerInferenceMs: nominal compute delay simulated by
    //     MockSlowAnalyzer. Set via
    //     `AETHERFLOW_MOCK_ANALYZER_INFERENCE_MS`.
    bool mockAnalyzerEnabled = false;
    int analyzerBridgeIntervalFrames = 1;
    int mockAnalyzerInferenceMs = 200;
};

struct MacosRunResult {
    int capturedFrames = 0;
    int encodedFrames = 0;
    int encodeFailureFrames = 0;
    int parseErrors = 0;
    double durationSeconds = 0.0;
    std::string outputPath;
    std::string permissionState;          // "granted" | "denied" | "unknown"
    std::string captureBackend = "ScreenCaptureKit";
    std::string encoderBackend = "VideoToolbox";
    std::vector<std::string> errors;

    // Privacy-mask accounting. `privacyMaskTotal` is the sum of every
    // FrameRegion of purpose PrivacyMask emitted across the run; the shim
    // owns this counter from the producer-emitted count. The applied / fallback
    // counters are populated by the encoder agent's compositor in stage 2 and
    // stay at zero in the producer-only slice (masks are emitted but not yet
    // composited onto the encoded frame).
    int privacyMaskTotal = 0;
    int privacyMaskAppliedTotal = 0;
    int privacyMaskFallbackFrames = 0;
};

// Returns 0 on success, nonzero on permission/init failure.
int RunMacosPipeline(const MacosRunOptions& options, MacosRunResult* outResult);

} // namespace mac
} // namespace platform
} // namespace AetherFlow
