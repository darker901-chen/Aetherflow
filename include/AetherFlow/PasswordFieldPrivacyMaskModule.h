#pragma once

#include "AetherFlow/IAIFrameAnalyzer.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Forward-declare the COM interface so this header does not pull in
// <UIAutomation.h> for every translation unit that consumes the module. The
// implementation file (.cpp) includes the full COM headers and uses the
// pointer through them.
struct IUIAutomation;

namespace AetherFlow {

class PasswordFieldPrivacyMaskModule final : public IFrameDecisionModule {
public:
    explicit PasswordFieldPrivacyMaskModule(int pollEveryFrames = 5, int paddingPixels = 4);
    ~PasswordFieldPrivacyMaskModule() override;

    bool Initialize(ID3D11Device* device, int width, int height) override;
    void Warmup(const FrameContext& warmupContext) override;
    void Evaluate(const FrameContext& context, FrameDecision* decision) override;
    const char* Name() const override { return "PasswordFieldPrivacyMaskModule"; }

private:
    // EnsureAutomation + RefreshMasks run on the dedicated poll thread only.
    // The UIA FindAll(TreeScope_Subtree) walk is a cross-process COM call that
    // costs tens of ms; keeping it off the producer thread is the whole point
    // of this module's threading model.
    bool EnsureAutomation();
    void RefreshMasks(const FrameContext& context, std::vector<FrameRegion>& out);

    void StartPollThreadOnce();
    void StopPollThread();
    void PollLoop();
    void PublishGeometry(const FrameContext& context);

    int m_width = 0;
    int m_height = 0;
    int m_paddingPixels = 4;
    int m_pollIntervalMs = 166;

    // Snapshot handed producer<->poll. m_mutex guards m_cachedMasks + m_geom.
    std::mutex m_mutex;
    std::vector<FrameRegion> m_cachedMasks;
    FrameContext m_geom = {};
    bool m_geomReady = false;

    std::thread m_pollThread;
    std::condition_variable m_pollCv;
    std::atomic<bool> m_pollStop{false};
    std::atomic<bool> m_pollStarted{false};

    // Touched only on the poll thread.
    IUIAutomation* m_automation = nullptr;
    bool m_comInitOwned = false;
};

} // namespace AetherFlow
