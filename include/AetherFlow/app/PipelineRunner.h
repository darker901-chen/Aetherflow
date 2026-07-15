#pragma once

// Windows pipeline entry, extracted from main.cpp's monolithic main() so two
// front-ends can drive the SAME capture→decide→mask→encode(→SRT) session:
//
//   AetherFlow.exe        — headless CLI (agent harness contract, unchanged):
//                           parse argv/env → PipelineOptions → RunPipelineOnce
//   AetherFlowStudio.exe  — settings UI (spec Delta B): builds PipelineOptions
//                           from the window controls, runs RunPipelineOnce on a
//                           worker thread, Stop sets `externalStop`.
//
// One pipeline implementation, two entry points — behavior cannot fork.
// Compile-time Config.h values remain the defaults: any Options field left at
// its zero/negative sentinel resolves to the historical constant, so the
// canonical headless run is unchanged byte-for-byte.

#if defined(_WIN32)

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "AetherFlow/IAIFrameAnalyzer.h"      // FrameRegion (manual masks)
#include "AetherFlow/PrivacyMaskCompositor.h" // PrivacyMaskMode

namespace AetherFlow {

enum class EncoderPreference {
    Auto,    // NVENC when present, else oneVPL (historical behavior)
    Nvenc,   // NVENC only; fail if unavailable
    OneVpl,  // skip NVENC even when present
};

struct SrtOutputOptions {
    bool enabled = false;
    int port = 8888;
    int latencyMs = 120;
    std::string passphrase;  // empty = none; else 10-79 chars (validated upstream)
};

struct PipelineOptions {
    // 0 (or negative) = compile-time Config.h default.
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrateKbps = 0;
    int monitorIndex = 0;                    // ScreenCapture::EnumerateMonitors order
    EncoderPreference encoder = EncoderPreference::Auto;
    // -1 = honor the AETHERFLOW_MAX_FRAMES env / Config.h default (CLI path);
    //  0 = no frame limit (run until stopped);  >0 = exact frame count.
    int maxFrames = -1;

    bool cursorRoiEnabled = false;
    int staticRoiX = -1;
    int staticRoiY = -1;

    bool startupPanicMask = false;
    bool rightCtrlPanicHotkeyEnabled = true;
    bool passwordFieldMaskEnabled = true;
    int passwordFieldMaskPollFrames = 5;
    bool notificationMaskEnabled = true;
    int notificationMaskPollFrames = 5;
    // Empty = built-in messenger default list (LINE/Slack/Discord/Teams/...).
    std::vector<std::string> notificationProcessWhitelist;
    std::vector<AetherFlow::FrameRegion> manualPrivacyMasks;
    AetherFlow::PrivacyMaskMode privacyMaskMode = AetherFlow::PrivacyMaskMode::Blur;

    bool mockAnalyzerEnabled = false;
    int mockAnalyzerInferenceMs = 200;
    int analyzerBridgeIntervalFrames = 1;
    bool analyzerBridgeIntervalExplicit = false;
    bool timedRecording = false;
    std::string sceneClassifierOnnxModel;
    std::string sceneClassifierProvider = "directml";
    bool sceneClassifierDemoAction = false;

    SrtOutputOptions srt;

    // External controls (Studio UI); both may be null on the CLI path.
    // externalStop: OR-ed with the process-wide Ctrl+C latch each frame.
    // panicLatch:   while true, every frame gets a full-screen panic mask
    //               (source "studio-panic-mask"), same fail-closed path as
    //               --panic-mask.
    std::atomic<bool>* externalStop = nullptr;
    std::atomic<bool>* panicLatch = nullptr;
};

// What a privacy mask on the current frame came from (status readout).
enum class MaskSourceKind : int {
    None = 0,
    PasswordField = 1,
    Notification = 2,
    Panic = 3,
    Manual = 4,
    DemoAction = 5,
    Other = 6,
};

// Canonical scene-class order of the CLIP zero-shot export
// (tools/export_clip_zeroshot.py). Index MUST match the class mapping in
// include/AetherFlow/ai/SceneClassifierOnnx.h ("0 -> code_text ... 4 ->
// sensitive_surface"); duplicated here because that mapping is a private
// implementation detail and the Studio UI needs the names for its AI scene
// indicator without pulling in ORT headers.
inline constexpr const char* kSceneClassNames[5] = {
    "code_text", "slides", "video", "mixed_ui", "sensitive_surface"};

inline int SceneClassNameToIndex(const std::string& name) {
    for (int i = 0; i < 5; ++i) {
        if (name == kSceneClassNames[i]) return i;
    }
    return -1;  // "unknown" / empty (classifier not active or pre-policy)
}

// Live status for the Studio UI. All fields are atomics written by the
// pipeline threads and read by the UI thread; the UI never touches pipeline
// objects directly.
struct PipelineStatus {
    std::atomic<bool> running{false};
    std::atomic<uint64_t> encodedFrames{0};
    std::atomic<double> effectiveFps{0.0};
    std::atomic<bool> maskActive{false};
    std::atomic<int> maskSource{0};  // MaskSourceKind
    std::atomic<bool> srtListening{false};
    std::atomic<bool> srtClientConnected{false};
    std::atomic<uint32_t> srtConnections{0};
    std::atomic<uint64_t> srtBytesSent{0};

    // AI scene classifier readouts (Studio "AI scene" indicator; advisory
    // display only — nothing here feeds back into masks or the encoder).
    // sceneClassifierState: 0 = not requested this session, 1 = active on
    // DirectML, 2 = active on CPU, 3 = requested but failed to initialize
    // (the session continues without AI). sceneClassIndex: -1 until the
    // policy layer stamps a class, else kSceneClassNames order. sceneSource-
    // Kind: 1 = the classifier's verdict won the confidence merge this frame;
    // 2 = a deterministic 1.0 producer (panic/password/notification/manual)
    // out-ranked it (the shown class is the producer's implied class);
    // 3 = baseline/low-confidence fallback holds the merge (classifier has no
    // confident verdict yet — cold start or mid-session fallback), so the UI
    // must NOT attribute the class to the AI.
    std::atomic<int> sceneClassifierState{0};
    std::atomic<int> sceneClassIndex{-1};
    std::atomic<float> sceneClassConfidence{0.0f};
    std::atomic<int> sceneSourceKind{0};
};

// Runs one full capture→encode session (blocking; typically minutes). Returns
// a process-style exit code (0 = clean). Reentrant sequentially, not
// concurrently (one capture/encode session per process at a time).
int RunPipelineOnce(const PipelineOptions& options, PipelineStatus* status);

}  // namespace AetherFlow

#endif  // _WIN32
