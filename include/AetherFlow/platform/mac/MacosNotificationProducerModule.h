#pragma once

// MacosNotificationProducerModule
//
// macOS counterpart of AetherFlow::NotificationProducerModule (Windows).
// Deterministic visible-window producer that emits PrivacyMask regions for
// every visible top-level window whose owner application name is in the
// configured whitelist. No AI / OCR / ML - purely a CoreGraphics window list
// enumeration (kCGWindowListOptionOnScreenOnly).
//
// Semantic differences vs Windows:
//   - The whitelist matches against kCGWindowOwnerName (the application's
//     localized owner string -- e.g. "Slack", "Microsoft Teams", "Messages"),
//     NOT against the executable leaf filename. macOS bundles do not expose a
//     stable per-window exe path the way Win32 does, so owner-name matching is
//     the deterministic equivalent.
//   - No TCC / Screen Recording permission is required to enumerate window
//     bounds + owner names via CGWindowListCopyWindowInfo at this layer; the
//     producer therefore works even if AetherFlow has not yet been granted
//     Screen Recording. (Actual pixel masking still depends on the screen
//     capture path which does require Screen Recording.)
//   - Spaces / multi-display limitation: CGWindowList only reports windows on
//     the current Space, and bounds are in the global desktop coordinate
//     space. The producer clips against FrameContext capture-space coords; if
//     a whitelisted window is on another Space it simply will not be returned
//     by the enumeration and no mask is emitted.

#include "AetherFlow/IAIFrameAnalyzer.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AetherFlow {
namespace platform {
namespace mac {

class MacosNotificationProducerModule final : public IFrameDecisionModule {
public:
    explicit MacosNotificationProducerModule(
        std::vector<std::string> ownerNameWhitelist,
        int pollEveryFrames = 5,
        int paddingPixels = 4);
    ~MacosNotificationProducerModule() override;

    bool Initialize(ID3D11Device* device, int width, int height) override;
    void Warmup(const FrameContext& warmupContext) override;
    void Evaluate(const FrameContext& context, FrameDecision* decision) override;
    const char* Name() const override { return "MacosNotificationProducerModule"; }

private:
    // RefreshMasks (CGWindowListCopyWindowInfo + per-window dictionary scan +
    // occluder subtraction) runs on the dedicated poll thread only, never on
    // the producer/capture thread. CGWindowListCopyWindowInfo is a
    // window-server round trip; keeping it off the producer thread is the
    // whole point of this module's threading model -- it mirrors the Windows
    // NotificationProducerModule off-thread fix that removed the periodic
    // capture stall / recorded-video judder.
    void RefreshMasks(const FrameContext& context, std::vector<FrameRegion>& out);

    void StartPollThreadOnce();
    void StopPollThread();
    void PollLoop();
    void PublishGeometry(const FrameContext& context);

    std::vector<std::string> m_whitelistLower;  // lowercased, trimmed, deduped
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
};

} // namespace mac
} // namespace platform
} // namespace AetherFlow
