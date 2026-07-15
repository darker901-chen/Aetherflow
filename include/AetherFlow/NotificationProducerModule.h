#pragma once

#include "AetherFlow/IAIFrameAnalyzer.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AetherFlow {

// Deterministic visible-window producer that masks visible top-level / popup
// window rects whose executable filename is in the configured whitelist. No
// AI / OCR / ML - purely Win32 (EnumWindows, GetWindowRect,
// QueryFullProcessImageName).
//
// v0.2 scope is intentionally deterministic: full visible-window mask, clipped
// against higher z-order windows to avoid blurring unrelated windows that cover
// a messenger. Future versions can add UIA-based sub-region targeting (sender
// name + first message line) without changing this producer's role.
//
// Process whitelist is case-insensitive on exact executable leaf filenames by
// default (no path component). Examples: "LINE.exe", "Slack.exe",
// "Discord.exe". Teams entries additionally expand to known classic/current
// Teams leaf names and packaged-app identity tokens so packaged ms-teams.exe
// does not miss when configured as Teams.exe. An empty whitelist disables the
// producer.
class NotificationProducerModule final : public IFrameDecisionModule {
public:
    explicit NotificationProducerModule(
        std::vector<std::string> processWhitelist,
        int pollEveryFrames = 5,
        int paddingPixels = 4);
    ~NotificationProducerModule() override;

    bool Initialize(ID3D11Device* device, int width, int height) override;
    void Warmup(const FrameContext& warmupContext) override;
    void Evaluate(const FrameContext& context, FrameDecision* decision) override;
    const char* Name() const override { return "NotificationProducerModule"; }

private:
    // RefreshMasks (EnumWindows + per-process image-name queries) runs on the
    // dedicated poll thread only, never on the producer thread.
    void RefreshMasks(const FrameContext& context, std::vector<FrameRegion>& out);
    bool MatchesWhitelist(unsigned long processId) const;

    void StartPollThreadOnce();
    void StopPollThread();
    void PollLoop();
    void PublishGeometry(const FrameContext& context);

    std::vector<std::string> m_whitelist;          // lowercase exact exe leaf filenames
    std::vector<std::string> m_identityTokens;     // lowercase packaged-app identity substrings
    int m_width = 0;
    int m_height = 0;
    int m_paddingPixels = 4;
    int m_pollIntervalMs = 166;

    std::mutex m_mutex;                             // guards m_cachedMasks + m_geom
    std::vector<FrameRegion> m_cachedMasks;
    FrameContext m_geom = {};
    bool m_geomReady = false;

    std::thread m_pollThread;
    std::condition_variable m_pollCv;
    std::atomic<bool> m_pollStop{false};
    std::atomic<bool> m_pollStarted{false};
};

} // namespace AetherFlow
