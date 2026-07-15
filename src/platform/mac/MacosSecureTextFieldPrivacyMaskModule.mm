// PHASE-1 STUB — phase 2 will fill Evaluate with AXUIElement detection of
// macOS secure text fields and emit privacy mask regions.

#include "AetherFlow/platform/mac/MacosSecureTextFieldPrivacyMaskModule.h"

namespace AetherFlow {
namespace platform {
namespace mac {

bool MacosSecureTextFieldPrivacyMaskModule::Initialize(ID3D11Device* device,
                                                       int width,
                                                       int height) {
    (void)device;
    m_valid = width > 0 && height > 0;
    return m_valid;
}

void MacosSecureTextFieldPrivacyMaskModule::Evaluate(const FrameContext& context,
                                                     FrameDecision* decision) {
    (void)context;
    (void)decision;
    // Phase 1: no-op. Phase 2 will populate decision->privacyMasks based on
    // AXUIElement queries against the focused application.
}

const char* MacosSecureTextFieldPrivacyMaskModule::Name() const {
    return "MacosSecureTextFieldPrivacyMaskModule";
}

} // namespace mac
} // namespace platform
} // namespace AetherFlow
