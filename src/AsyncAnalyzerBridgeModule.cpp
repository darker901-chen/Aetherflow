#include "AetherFlow/AsyncAnalyzerBridgeModule.h"

namespace AetherFlow {

bool AsyncAnalyzerBridgeModule::Initialize(ID3D11Device* device, int width, int height) {
    if (!m_analyzer) {
        // Bridge wired with no analyzer is a no-op; still report success so
        // the policy engine doesn't fail to initialize when the user opts the
        // bridge in but the analyzer construction was skipped (e.g. CLI flag
        // present but analyzer impl disabled).
        return true;
    }
    return m_analyzer->Initialize(device, width, height);
}

void AsyncAnalyzerBridgeModule::Warmup(const FrameContext& warmupContext) {
    (void)warmupContext;
    // Analyzers that need cold-start warmup (ONNX session compile, CoreML
    // graph build, OCR dictionary load) should expose their own warmup
    // through SubmitFrame on warmupContext if needed. The bridge itself has
    // no thread-local state to pre-pay.
}

void AsyncAnalyzerBridgeModule::Evaluate(const FrameContext& context, FrameDecision* decision) {
    // Reset per-frame state up front. Even when the bridge is unwired or the
    // decision pointer is null we want stale accessor reads to return zeros
    // rather than the previous frame's values.
    m_lastSubmitted = false;
    m_lastContributed = false;
    m_lastInferenceMs = 0.0;
    m_lastStalenessFrames = 0;

    if (!decision || !m_analyzer) {
        return;
    }

    // Sub-sampling: SubmitFrame fires at most once every submitEveryNFrames
    // frames. m_lastSubmitFrame starts at -1 so the first frame (frameIndex
    // = 0) always submits.
    const bool dueToSubmit = (m_lastSubmitFrame < 0) ||
        ((context.frameIndex - m_lastSubmitFrame) >= m_submitEveryNFrames);
    if (dueToSubmit) {
        // Forward the captured BGRA source texture (the only image texture
        // that exists at the deterministic-decision stage). The analyzer's
        // SubmitFrame param is named `nv12Texture` for historical signature
        // stability only — see IAIFrameAnalyzer contract doc.
        m_analyzer->SubmitFrame(context.captureTextureBgra, context.frameIndex, context.elapsedSeconds);
        m_lastSubmitFrame = context.frameIndex;
        m_lastSubmitted = true;
        ++m_submittedFrames;
    }

    AiFrameAnalysis latest;
    if (!m_analyzer->TryGetLatest(&latest)) {
        return;
    }
    if (latest.submitFrameIndex < 0) {
        // Worker has not yet finished a real inference (cold start). The
        // analyzer published nothing actionable — do not contribute.
        return;
    }

    // Confidence-based merge: bridge's contribution wins iff strictly higher
    // than whatever the deterministic producers already proposed. This is
    // how P2 (analyzer ↔ engine bridge) + P1 (confidence merge) plug
    // together: deterministic 1.0 producers (panic / manual / password /
    // notification) keep precedence over an analyzer at 0.6-0.92; baseline
    // (0.5) is always overridden by a real analyzer result.
    if (latest.scene.type != FrameSceneType::Unknown) {
        decision->ProposeScene(latest.scene);
    }
    decision->qualityRegions.insert(
        decision->qualityRegions.end(),
        latest.qualityRegions.begin(),
        latest.qualityRegions.end());
    decision->privacyMasks.insert(
        decision->privacyMasks.end(),
        latest.privacyMasks.begin(),
        latest.privacyMasks.end());

    m_lastContributed = true;
    m_lastInferenceMs = latest.inferenceMs;
    m_lastStalenessFrames = context.frameIndex - latest.submitFrameIndex;
    ++m_contributedFrames;
}

} // namespace AetherFlow
