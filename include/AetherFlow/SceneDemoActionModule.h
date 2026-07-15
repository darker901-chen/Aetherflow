#pragma once

#include "AetherFlow/IAIFrameAnalyzer.h"

namespace AetherFlow {

// Phase 4 P0.1 §4.y — Visible Scene Demo Action (opt-in, default OFF).
//
// This is a *deliberately crude visual proxy of P1.1*, NOT P1.1 itself and
// NOT product behavior. Its only job is to make scene detection visually
// unmistakable in the encoded output so the user can run the §7 Stage A→B
// accuracy review by eyeball instead of reading frame_trace.jsonl.
//
// It is registered AFTER PolicyEngine (see src/main.cpp) so it observes the
// final merged FrameDecision.scene. When enabled, and only when no
// deterministic privacy mask (panic / password / notification / manual) is
// already on the decision, it emits ONE full-screen PrivacyMask FrameRegion
// (source="scene-demo-action") whose visual *mode* main.cpp then selects per
// detected class. It does NOT call ProposeScene — it is not a scene producer,
// only a downstream demo-action emitter.
//
// Class-name source of truth (updated 2026-05-23): this module keys off
// FrameDecision.policyStableClass — the hysteresis-smoothed canonical 5-class
// string that PolicyEngine stamps onto the decision. Because PolicyEngine is
// registered BEFORE this module, that field is already set this frame when we
// run. Using the SMOOTHED class (not the raw per-inference scene) means the
// demo effect changes at most ~once per 5 s instead of flickering on noisy
// ~1 Hz classifier output (e.g. a momentary mixed_ui blip on a mouse move).
// Emit decision per the table:
//
//   canonical class      demo effect          emit?
//   -------------------  -------------------  -----
//   sensitive_surface    Blackout             yes
//   video                Mosaic               yes
//   code_text            Blur                 yes
//   slides               Grayscale            yes
//   mixed_ui             none (passthrough)   no
//   unknown / "" (warmup) none (passthrough)  no
//
// Mapping rationale (2026-05-23): a generic desktop (mixed_ui) must NOT be
// obscured — it stays clean. The other four classes each get a visually
// distinct effect so the demo is legible: blackout / mosaic / blur /
// grayscale.
//
// The per-frame PrivacyMaskMode (Blackout / Mosaic / Blur / Grayscale) is
// selected in main.cpp Stage 3 from the SAME FrameDecision.policyStableClass
// before ApplyMask — this header only decides whether to emit the full-screen
// region at all. Both read one smoothed class, so emit and mode always agree.
class SceneDemoActionModule final : public IFrameDecisionModule {
public:
    explicit SceneDemoActionModule(bool enabled) : m_enabled(enabled) {}

    bool Initialize(ID3D11Device* device, int width, int height) override {
        (void)device;
        m_width = width;
        m_height = height;
        return m_width > 0 && m_height > 0;
    }

    // Returns true when the (hysteresis-smoothed) canonical scene class maps
    // to a visible demo effect (i.e. should emit a full-screen mask). mixed_ui
    // (generic desktop) / unknown / "" (pre-warmup) / panic map to passthrough
    // (no mask) so a plain desktop is never obscured.
    //   sensitive_surface -> Blackout, video -> Mosaic, code_text -> Blur,
    //   slides -> Grayscale  (mode selected in main.cpp Stage 3).
    static bool ClassMapsToDemoEffect(const std::string& cls) {
        return cls == "sensitive_surface" || cls == "video" ||
               cls == "code_text" || cls == "slides";
    }

    void Evaluate(const FrameContext& context, FrameDecision* decision) override {
        (void)context;
        if (!m_enabled || !decision || m_width <= 0 || m_height <= 0) {
            return;
        }
        // Deterministic producers (panic / password / notification / manual)
        // stay dominant and untouched. If a real privacy mask already exists
        // on this decision, the demo must NOT fight it — suppress entirely.
        if (decision->HasPrivacyMask()) {
            return;
        }
        // Follow PolicyEngine's hysteresis-smoothed class (set on the decision
        // before this module runs), NOT the raw per-inference scene, so the
        // demo effect changes at most ~once per 5 s instead of flickering on
        // noisy ~1 Hz classifier output (e.g. a momentary mixed_ui blip on a
        // mouse move). The raw scene is still logged in the trace for honesty.
        if (!ClassMapsToDemoEffect(decision->policyStableClass)) {
            return; // mixed_ui / unknown / pre-warmup -> emit nothing.
        }

        FrameRegion mask;
        mask.purpose = FrameRegionPurpose::PrivacyMask;
        mask.left = 0;
        mask.top = 0;
        mask.right = m_width;
        mask.bottom = m_height;
        // Confidence is informational here; the demo region is unconditional
        // once the class maps to an effect. It is NOT merged into the scene.
        mask.confidence = decision->scene.confidence;
        mask.source = "scene-demo-action";
        mask.debugLabel = "SceneDemoActionModule";
        decision->privacyMasks.push_back(mask);
        // Intentionally NO ProposeScene: this module is a downstream demo
        // action emitter, not a scene producer.
    }

    const char* Name() const override { return "SceneDemoActionModule"; }

private:
    bool m_enabled = false;
    int m_width = 0;
    int m_height = 0;
};

} // namespace AetherFlow
