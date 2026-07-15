#pragma once

// PolicyEngine — Phase 4 P0.1 cross-platform policy layer.
//
// Consumes the post-merge `FrameDecision` (scene + confidence resolved by
// `FrameDecision::ProposeScene`'s strict-`>` merge) and emits a
// `PolicyDecision { mask_mode, encode_hint, mode_label, reason }`. The engine
// lives on the producer thread (per docs/archive/PHASE4_P0_PLAN.md §9.2 default) so
// it sees the same FrameContext + decision the trace writer logs.
//
// Hysteresis (docs/archive/PHASE4_P0_PLAN.md §3):
//   - Minimum kMinFramesBetweenSwitches=150 frames (5 s at 30 fps) between
//     mode switches. Raised from 60 (2 s) on 2026-05-23 so the policy mode
//     (and the demo effect that follows it) cannot flip more than ~once per
//     5 s. This both removes visible flicker and makes the verifier's
//     `mode_switches_le_1_per_5s` gate pass: with a 5 s floor the worst-case
//     switch rate is <= 1 per 5 s at run lengths that are multiples of 150
//     frames (the durations run_full_test uses: 900 / 1800 / 5400 ...). At
//     off-multiple lengths a pathologically flapping classifier could still
//     exceed it by one switch — which is exactly the thrash the gate exists
//     to catch, not normal interactive content (real runs sit near 0.2-0.4).
//   - kConsecutiveSameClassToSwitch=3 consecutive same-class verdicts before
//     a switch.
// Fallback:
//   - `sceneConfidence < kLowConfidenceThreshold` (0.6) → keep previous stable
//     decision, reason="low_confidence_fallback".
//   - Panic mode (detected from FrameContext.panicMaskActive or the
//     "panic-privacy-mask"-family scene source) → emit
//     `{mask_mode=Blackout, encode_hint=balanced, reason=panic_override}` AND
//     reset hysteresis state (so the engine doesn't carry "I was inside a
//     pre-panic switch-floor state out the other side).
//
// **P0.1 is advisory only.** Product consumers are the trace writer and Studio
// status. The opt-in SceneDemoActionModule also reads policyStableClass for a
// non-product compositor preview. Product mask_mode and encoder QP still come
// from their existing deterministic inputs; P1.1 is the real policy-action
// wiring milestone.
//
// Cross-platform: pure C++. No D3D11 / Foundation includes. Same TU compiles
// on Windows (P0.1) and macOS (P0.2).

#include "AetherFlow/IAIFrameAnalyzer.h"
#include "AetherFlow/policy/PolicyDecision.h"

#include <string>

namespace AetherFlow {

class PolicyEngine final : public IFrameDecisionModule {
public:
    static constexpr int   kMinFramesBetweenSwitches = 150;  // 5 s @ 30 fps (was 60 = 2 s)
    static constexpr int   kConsecutiveSameClassToSwitch = 3;
    static constexpr float kLowConfidenceThreshold = 0.6f;

    PolicyEngine() = default;

    bool Initialize(ID3D11Device* device, int width, int height) override;
    void Evaluate(const FrameContext& context, FrameDecision* decision) override;
    const char* Name() const override { return "PolicyEngine"; }

    // The most recent decision. Stable across hysteresis-pinned frames. Used
    // by the trace writer to emit `policyMode` / `policyReason`.
    const PolicyDecision& LastDecision() const { return m_last; }

    // Total number of mode_label transitions since Initialize. Exposed for
    // trace summary aggregation (`policy_mode_switches`).
    int ModeSwitches() const { return m_modeSwitches; }

    // Number of frames so far that hit the `low_confidence_fallback` path.
    int LowConfidenceFrames() const { return m_lowConfidenceFrames; }

private:
    // Map a FrameScene (post-merge) into the 5-class policy taxonomy. Returns
    // an opaque string from the canonical set
    // {`code_text`, `slides`, `video`, `mixed_ui`, `sensitive_surface`,
    //  `unknown`}. `unknown` triggers the low-confidence fallback path.
    static std::string ClassifyScene(const FrameScene& scene);

    static bool IsPanicSource(const std::string& source);

    void ApplyMapping(const std::string& policyClass, PolicyDecision* out) const;

    PolicyDecision m_last{};
    std::string m_lastPolicyClass;       // policy class behind m_last (stable)
    int m_lastSwitchFrame = -1;          // frame index of most recent switch
    std::string m_pendingClass;          // candidate not yet promoted by hysteresis
    int m_pendingCount = 0;              // consecutive frames seeing m_pendingClass
    int m_modeSwitches = 0;
    int m_lowConfidenceFrames = 0;
    bool m_initialized = false;
};

} // namespace AetherFlow
