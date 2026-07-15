#pragma once

// Phase 1 stub: phase 2 will replace Evaluate with AXUIElement detection of
// macOS NSSecureTextField (and equivalent password fields) so we can produce
// privacy mask regions analogous to the Windows PasswordFieldPrivacyMaskModule.
//
// In phase 1 this module is wired into the FramePolicyEngine but never emits
// masks. Keeping the slot reserved means the macOS trace already records a
// dedicated decision source name and the encoder agent / benchmark agent do
// not have to renumber decision_sources later.
//
// THREADING CONTRACT FOR PHASE 2 (do not regress): the AXUIElement focus
// walk is a cross-process accessibility query that costs tens of ms -- the
// macOS analogue of the Windows UIA FindAll(TreeScope_Subtree) call. It MUST
// run on a dedicated background poll thread (mirroring the off-thread model
// of the Windows PasswordFieldPrivacyMaskModule and of
// MacosNotificationProducerModule); Evaluate() may only copy a
// mutex-protected cached snapshot. Doing the AX scan inline on the
// producer/capture thread would reintroduce the exact periodic capture
// stall / recorded-video judder the Windows side already fixed.

#include "AetherFlow/IAIFrameAnalyzer.h"

namespace AetherFlow {
namespace platform {
namespace mac {

class MacosSecureTextFieldPrivacyMaskModule final : public IFrameDecisionModule {
public:
    bool Initialize(ID3D11Device* device, int width, int height) override;
    void Evaluate(const FrameContext& context, FrameDecision* decision) override;
    const char* Name() const override;

private:
    bool m_valid = false;
};

} // namespace mac
} // namespace platform
} // namespace AetherFlow
