#pragma once

// MockSlowAnalyzer is a deterministic, real-threaded stand-in for a future
// async analyzer (ONNX / CoreML / OCR / QR). Bridge-hardening (Phase 4 P0
// prerequisite) reworked the mock to actually exercise the IAIFrameAnalyzer
// non-blocking contract by running its work on a dedicated std::thread, so the
// AsyncAnalyzerBridgeModule + confidence-based scene merge (P1+P2) are
// verified end-to-end under the same threading model a real classifier will
// use. Behavior:
//
//   - SubmitFrame() enqueues a job (frameIndex, elapsedSeconds, submitTime)
//     for the worker. Non-blocking by contract; if a prior job is still in
//     flight, the older one is dropped and replaced with the latest input
//     (drop-when-busy is built in because a queue depth > 1 would only add
//     staleness without improving freshness for a single classifier).
//   - The worker thread is lazily started on the first SubmitFrame call so
//     Initialize() failures cannot leave a half-constructed worker behind.
//   - The worker pops the latest job, sleeps for `inferenceMs` to simulate
//     model compute (sleep is on the shutdown CV so the destructor can join
//     promptly), then publishes an AiFrameAnalysis into `m_cached` under the
//     mutex. `inferenceMs` on the published analysis is the measured
//     wall-clock time from SubmitFrame to publication (queue-wait + compute),
//     not the configured nominal delay.
//   - Cold-start gate (kColdStartFrames = 6) is now applied on the worker
//     side: the worker still performs the sleep so the latency cost is
//     observable in the bridge's per-frame samples, but the result is
//     discarded rather than cached. This matches how a real warmup-bound
//     classifier behaves.
//   - TryGetLatest() takes the mutex briefly to copy m_cached. Non-blocking by
//     contract.
//
// The mock continues to publish a FrameScene with type VideoContent,
// confidence 0.92, source "mock-slow-analyzer", every kPublishCadenceFrames
// post-cold-start submissions. This is calibrated to:
//   - beat the baseline (0.5) so confidence-based merge can be observed in
//     the trace summary;
//   - lose to deterministic producers (1.0) so trace shows precedence
//     ordering is correct when both deterministic and analyzer sources are
//     present.
//
// Cross-platform: pure C++, no D3D11 / Foundation deps.

#include "AetherFlow/IAIFrameAnalyzer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace AetherFlow {

class MockSlowAnalyzer final : public IAIFrameAnalyzer {
public:
    static constexpr int kColdStartFrames = 6;     // ~200ms at 30 fps
    static constexpr int kPublishCadenceFrames = 30;
    static constexpr float kSceneConfidence = 0.92f;

    explicit MockSlowAnalyzer(int inferenceMs = 200);
    ~MockSlowAnalyzer() override;

    MockSlowAnalyzer(const MockSlowAnalyzer&) = delete;
    MockSlowAnalyzer& operator=(const MockSlowAnalyzer&) = delete;

    bool Initialize(ID3D11Device* device, int width, int height) override;
    void SubmitFrame(ID3D11Texture2D* nv12Texture, int frameIndex, double elapsedSeconds) override;
    bool TryGetLatest(AiFrameAnalysis* result) override;

private:
    using Clock = std::chrono::high_resolution_clock;

    struct PendingJob {
        int frameIndex = 0;
        double elapsedSeconds = 0.0;
        Clock::time_point submitTime{};
    };

    void EnsureWorkerStarted();
    void WorkerLoop();

    int m_inferenceMs;
    int m_submitCount = 0;          // protected by m_mutex; counts SubmitFrame calls since Initialize
    int m_publishCount = 0;         // protected by m_mutex; counts post-cold-start results actually cached
    AiFrameAnalysis m_cached{};     // protected by m_mutex
    bool m_hasCached = false;       // protected by m_mutex
    std::deque<PendingJob> m_queue; // protected by m_mutex; depth-1 by drop-when-busy policy

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
    bool m_workerStarted = false;   // protected by m_mutex
};

} // namespace AetherFlow
