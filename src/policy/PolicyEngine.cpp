#include "AetherFlow/policy/PolicyEngine.h"

namespace AetherFlow {

bool PolicyEngine::Initialize(ID3D11Device* device, int width, int height) {
    (void)device;
    (void)width;
    (void)height;
    m_last = {};
    m_last.mask_mode = PolicyMaskMode::None;
    m_last.encode_hint = "balanced";
    m_last.mode_label = "initial->balanced";
    m_last.reason = "initial";
    m_lastPolicyClass.clear();
    m_lastSwitchFrame = -1;
    m_pendingClass.clear();
    m_pendingCount = 0;
    m_modeSwitches = 0;
    m_lowConfidenceFrames = 0;
    m_initialized = true;
    return true;
}

bool PolicyEngine::IsPanicSource(const std::string& source) {
    // Panic producers (startup --panic-mask, right-ctrl hotkey, future
    // platform panic surfaces) all stamp the scene/mask source with a
    // "panic"-bearing token. Be permissive so any future producer naming
    // its source like `panic-privacy-mask`, `right-ctrl-panic-mask`, etc. is
    // detected. Avoids coupling PolicyEngine to a specific producer name.
    return source.find("panic") != std::string::npos;
}

std::string PolicyEngine::ClassifyScene(const FrameScene& scene) {
    // The PolicyEngine taxonomy is the canonical 5-class set documented in
    // docs/archive/PHASE4_P0_PLAN.md §2.2.A. Existing FrameSceneType enum values are
    // mapped onto it; the classifier emits a `sceneClass` string directly
    // (set on the scene producer side) which is recovered preferentially.
    //
    // 1) If the producer left a source token like "scene-classifier-onnx" and
    //    a debug label that *is* one of the canonical names, prefer that.
    //    This way the canonical 5-class taxonomy round-trips through the
    //    deterministic FrameSceneType + scene.source/debugLabel without
    //    needing to extend the enum.
    const std::string& src = scene.source;
    const std::string& dbg = scene.debugLabel;
    auto isCanonical = [](const std::string& s) {
        return s == "code_text" || s == "slides" || s == "video" ||
               s == "mixed_ui" || s == "sensitive_surface";
    };
    if (isCanonical(dbg)) return dbg;
    // Some classifier producers may stamp it into `source` instead.
    if (isCanonical(src)) return src;

    switch (scene.type) {
    case FrameSceneType::TextUi:           return "code_text";
    case FrameSceneType::Slides:           return "slides";
    case FrameSceneType::VideoContent:     return "video";
    case FrameSceneType::GenericScreen:    return "mixed_ui";
    case FrameSceneType::SensitiveSurface: return "sensitive_surface";
    case FrameSceneType::GameMotion:       return "video";  // closest action mapping
    case FrameSceneType::Unknown:
    default:
        return "unknown";
    }
}

void PolicyEngine::ApplyMapping(const std::string& policyClass, PolicyDecision* out) const {
    if (!out) return;
    if (policyClass == "code_text") {
        out->mask_mode = PolicyMaskMode::Mosaic;
        out->encode_hint = "text_safe";
        out->mode_label = "code_text->text_safe";
    } else if (policyClass == "slides") {
        out->mask_mode = PolicyMaskMode::Blur;
        out->encode_hint = "detail_preserving";
        out->mode_label = "slides->detail_preserving";
    } else if (policyClass == "video") {
        out->mask_mode = PolicyMaskMode::Blur;
        out->encode_hint = "motion_preserving";
        out->mode_label = "video->motion_preserving";
    } else if (policyClass == "mixed_ui") {
        out->mask_mode = PolicyMaskMode::Mosaic;
        out->encode_hint = "balanced";
        out->mode_label = "mixed_ui->balanced";
    } else if (policyClass == "sensitive_surface") {
        out->mask_mode = PolicyMaskMode::Blackout;
        out->encode_hint = "balanced";
        out->mode_label = "sensitive_surface->balanced";
    } else {
        // Unknown -- caller is responsible for choosing the fallback path
        // (typically holding the previous stable decision via the
        // low_confidence_fallback reason).
        out->mask_mode = PolicyMaskMode::None;
        out->encode_hint = "balanced";
        out->mode_label = "unknown->balanced";
    }
}

void PolicyEngine::Evaluate(const FrameContext& context, FrameDecision* decision) {
    if (!m_initialized || !decision) {
        return;
    }

    // Publish the hysteresis-smoothed stable class onto the decision so
    // downstream consumers (e.g. SceneDemoActionModule) can follow a class
    // that changes at most ~once per 5 s instead of the raw per-inference
    // scene. Default to the current stable class; the panic and promotion
    // paths below mutate m_lastPolicyClass and re-stamp it.
    decision->policyStableClass = m_lastPolicyClass;

    // Panic override path. Independent of classifier output and hysteresis.
    // Detection prefers the explicit FrameContext flag (used by the panic
    // producers); also covers the case where the merged scene source carries
    // a "panic" token even though the FrameContext flag was set later in the
    // frame.
    const bool panicByContext = context.panicMaskActive;
    const bool panicByScene = IsPanicSource(decision->scene.source);
    if (panicByContext || panicByScene) {
        PolicyDecision panic;
        panic.mask_mode = PolicyMaskMode::Blackout;
        panic.encode_hint = "balanced";
        panic.mode_label = "panic->blackout";
        panic.reason = "panic_override";

        if (m_last.mode_label != panic.mode_label) {
            ++m_modeSwitches;
            m_lastSwitchFrame = context.frameIndex;
        }
        m_last = panic;
        m_lastPolicyClass = "panic";
        decision->policyStableClass = m_lastPolicyClass;
        // Reset hysteresis state so when panic releases, the next class needs
        // to satisfy the 3-consecutive + 150-frame (5 s) rules from scratch.
        m_pendingClass.clear();
        m_pendingCount = 0;
        return;
    }

    const std::string policyClass = ClassifyScene(decision->scene);
    const float confidence = decision->scene.confidence;

    // Low-confidence path: hold the previous stable decision. Counts toward
    // policy_low_confidence_frames trace aggregate.
    if (confidence < kLowConfidenceThreshold || policyClass == "unknown") {
        ++m_lowConfidenceFrames;
        // Hold previous stable decision but rewrite reason so the trace can
        // detect the fallback explicitly.
        if (m_lastPolicyClass.empty()) {
            // No prior stable decision (first frames before classifier warmup).
            PolicyDecision initial;
            ApplyMapping("unknown", &initial);
            initial.reason = "low_confidence_fallback";
            m_last = initial;
            // mode_label transitions from "initial->balanced" to
            // "unknown->balanced" are not a *policy* switch, so do not
            // increment ModeSwitches here.
        } else {
            m_last.reason = "low_confidence_fallback";
        }
        // Reset pending: a low-confidence frame breaks the 3-consecutive run.
        m_pendingClass.clear();
        m_pendingCount = 0;
        return;
    }

    // Confidence >= threshold. Apply hysteresis: only switch when the new
    // class is seen at least kConsecutiveSameClassToSwitch frames in a row
    // AND at least kMinFramesBetweenSwitches frames have passed since the
    // last switch.
    if (policyClass == m_lastPolicyClass) {
        // Same class as already-stable. Reaffirm the existing decision.
        m_pendingClass.clear();
        m_pendingCount = 0;
        // mode_label stays, but mark as classifier_high_confidence so the
        // trace shows the active path.
        m_last.reason = "classifier_high_confidence";
        return;
    }

    // policyClass differs from the stable class. Accumulate pending votes.
    if (m_pendingClass == policyClass) {
        ++m_pendingCount;
    } else {
        m_pendingClass = policyClass;
        m_pendingCount = 1;
    }

    const int framesSinceSwitch = (m_lastSwitchFrame < 0)
        ? kMinFramesBetweenSwitches  // pretend "long enough" so first-ever switch is unconstrained
        : (context.frameIndex - m_lastSwitchFrame);

    const bool consecutiveMet = m_pendingCount >= kConsecutiveSameClassToSwitch;
    const bool gapMet = framesSinceSwitch >= kMinFramesBetweenSwitches;

    if (consecutiveMet && gapMet) {
        // Promote the pending class.
        PolicyDecision promoted;
        ApplyMapping(policyClass, &promoted);
        promoted.reason = "classifier_high_confidence";

        if (m_last.mode_label != promoted.mode_label) {
            ++m_modeSwitches;
            m_lastSwitchFrame = context.frameIndex;
        }
        m_last = promoted;
        m_lastPolicyClass = policyClass;
        decision->policyStableClass = m_lastPolicyClass;
        m_pendingClass.clear();
        m_pendingCount = 0;
        return;
    }

    // Hysteresis pin: keep the stable decision; reason explains why.
    m_last.reason = "hysteresis_pin";
}

} // namespace AetherFlow
