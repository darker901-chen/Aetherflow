#pragma once

// PolicyDecision is the (advisory) output of the Phase 4 P0.1 PolicyEngine.
//
// The engine consumes the post-merge `FrameScene` (whatever
// `FrameDecision::ProposeScene` resolved on the producer thread; see strict-`>`
// confidence merge in IAIFrameAnalyzer.h) and emits a stable, human-readable
// recommendation for the privacy-mask compositor (mask_mode) and the encoder
// backend (encode_hint).
//
// **P0.1 scope is annotation-only.** PolicyDecision is logged into
// frame_trace.jsonl and printed in the bridge startup banner so the
// trace-verifier can prove the policy mapping is deterministic, but it does
// NOT yet drive the compositor / encoder. Wiring is P1.1. Treat the fields
// here as truth surfaces for trace + diagnostics, not as runtime levers.
//
// Cross-platform: pure C++, no Windows / macOS headers. Same translation unit
// compiles on Windows (P0.1) and macOS (P0.2).

#include <string>

namespace AetherFlow {

// PolicyMaskMode mirrors the PrivacyMaskCompositor enum but lives in this
// pure-C++ header so the policy layer compiles on every platform. The values
// map 1:1 onto PrivacyMaskCompositor::PrivacyMaskMode at the
// runtime-wiring layer (P1.1).
enum class PolicyMaskMode {
    None = 0,
    Blackout,
    Blur,
    Mosaic
};

inline const char* PolicyMaskModeName(PolicyMaskMode mode) {
    switch (mode) {
    case PolicyMaskMode::None:     return "none";
    case PolicyMaskMode::Blackout: return "blackout";
    case PolicyMaskMode::Blur:     return "blur";
    case PolicyMaskMode::Mosaic:   return "mosaic";
    }
    return "none";
}

struct PolicyDecision {
    // What the compositor *should* do (P1.1 will wire this).
    PolicyMaskMode mask_mode = PolicyMaskMode::None;

    // Free-form encoder rate-control hint string. P0.1 keeps these as opaque
    // labels (`text_safe`, `detail_preserving`, `motion_preserving`,
    // `balanced`) because the encoder backend wiring is P1.1.
    std::string encode_hint;

    // Short human-readable label combining the policy class and the action.
    // Examples: `code_text->text_safe`, `slides->detail_preserving`,
    // `panic->blackout`, `low_confidence_fallback->balanced`.
    std::string mode_label;

    // Trace-grade reason string. Drives the `policyReason` trace field. One of:
    //   `classifier_high_confidence`  — classifier confidence >= 0.6, applied
    //   `hysteresis_pin`              — within hysteresis window, holding prev
    //   `low_confidence_fallback`     — confidence < 0.6, holding prev stable
    //   `panic_override`              — panic mask active; bypasses policy
    //   `initial`                     — engine has no prior decision yet
    std::string reason;
};

} // namespace AetherFlow
