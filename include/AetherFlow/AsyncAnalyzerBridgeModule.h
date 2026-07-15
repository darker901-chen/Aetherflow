#pragma once

// AsyncAnalyzerBridgeModule bridges a slow-path IAIFrameAnalyzer (OCR / QR /
// ONNX / CoreML / future VLM) into the deterministic FramePolicyEngine. Each
// frame the bridge does two things on the producer thread:
//
//   1. SubmitFrame(captureTextureBgra, ...) — fire-and-forget the current
//      frame's captured BGRA source texture to the analyzer. The analyzer is
//      responsible for running asynchronously and must not block. Submission
//      is sub-sampled: the bridge only calls SubmitFrame every
//      `submitEveryNFrames` frames so a real 200 ms-class classifier at 30 fps
//      target cadence does not flood the analyzer queue.
//   2. TryGetLatest(&analysis) — non-blocking snapshot of the most recent
//      completed analyzer result. Polled every frame regardless of the
//      submit cadence so a stale-but-valid result can still contribute to
//      the decision. When a fresh result is available, the bridge merges its
//      scene via FrameDecision::ProposeScene (P1 confidence-based merge) and
//      appends its qualityRegions / privacyMasks.
//
// The bridge tracks per-frame staleness (currentFrameIndex - submitFrameIndex
// of the returned result) and inferenceMs, which the trace writer emits as
// `analyzerStalenessFrames` and `analyzerInferenceMs`. These power the
// bridge-level verifier gates added in the Phase 4 P0 prerequisite ("Bridge
// Hardening").
//
// The bridge owns no analyzer state; the caller manages the IAIFrameAnalyzer
// lifetime (mirrors how producers receive raw module pointers via
// FramePolicyEngine::AddModule). On non-Windows platforms the captured BGRA
// texture pointer is null; analyzer implementations must tolerate that.

#include "AetherFlow/IAIFrameAnalyzer.h"

namespace AetherFlow {

class AsyncAnalyzerBridgeModule final : public IFrameDecisionModule {
public:
    AsyncAnalyzerBridgeModule() = default;
    explicit AsyncAnalyzerBridgeModule(IAIFrameAnalyzer* analyzer)
        : AsyncAnalyzerBridgeModule(analyzer, 1, true) {}

    AsyncAnalyzerBridgeModule(IAIFrameAnalyzer* analyzer,
                              int submitEveryNFrames,
                              bool dropWhenBusy)
        : m_analyzer(analyzer),
          m_submitEveryNFrames(submitEveryNFrames > 0 ? submitEveryNFrames : 1),
          m_dropWhenBusy(dropWhenBusy) {}

    void SetAnalyzer(IAIFrameAnalyzer* analyzer) { m_analyzer = analyzer; }
    IAIFrameAnalyzer* GetAnalyzer() const { return m_analyzer; }

    bool Initialize(ID3D11Device* device, int width, int height) override;
    void Warmup(const FrameContext& warmupContext) override;
    void Evaluate(const FrameContext& context, FrameDecision* decision) override;
    const char* Name() const override { return "AsyncAnalyzerBridgeModule"; }

    // Counters exposed for trace / verifier wiring. Not thread-safe; the
    // bridge is only touched on the producer thread.
    int SubmittedFrames() const { return m_submittedFrames; }
    int ContributedFrames() const { return m_contributedFrames; }

    // Per-frame accessors reflecting the most recent Evaluate() call. Used by
    // the trace writer to emit `analyzerSubmitted`, `analyzerContributed`,
    // `analyzerInferenceMs`, `analyzerStalenessFrames`.
    bool LastSubmitted() const { return m_lastSubmitted; }
    bool LastContributed() const { return m_lastContributed; }
    double LastInferenceMs() const { return m_lastInferenceMs; }
    int LastStalenessFrames() const { return m_lastStalenessFrames; }

    int SubmitEveryNFrames() const { return m_submitEveryNFrames; }
    bool DropWhenBusy() const { return m_dropWhenBusy; }

private:
    IAIFrameAnalyzer* m_analyzer = nullptr;
    int m_submitEveryNFrames = 1;
    bool m_dropWhenBusy = true;

    int m_submittedFrames = 0;
    int m_contributedFrames = 0;
    int m_lastSubmitFrame = -1;       // frameIndex of the most recent SubmitFrame; -1 means none yet

    bool m_lastSubmitted = false;
    bool m_lastContributed = false;
    double m_lastInferenceMs = 0.0;
    int m_lastStalenessFrames = 0;
};

} // namespace AetherFlow
