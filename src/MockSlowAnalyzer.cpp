#include "AetherFlow/MockSlowAnalyzer.h"

namespace AetherFlow {

MockSlowAnalyzer::MockSlowAnalyzer(int inferenceMs)
    : m_inferenceMs(inferenceMs > 0 ? inferenceMs : 200) {}

MockSlowAnalyzer::~MockSlowAnalyzer() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_shutdown.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool MockSlowAnalyzer::Initialize(ID3D11Device* device, int width, int height) {
    (void)device;
    (void)width;
    (void)height;
    // Reset producer-visible counters and the cached result. The worker is
    // lazily started on the first SubmitFrame so a re-Initialize after a hot
    // restart does not need special teardown of the worker thread; we keep
    // any pre-existing thread and just clear shared state under the mutex.
    std::lock_guard<std::mutex> lk(m_mutex);
    m_submitCount = 0;
    m_publishCount = 0;
    m_cached = {};
    m_hasCached = false;
    m_queue.clear();
    return true;
}

void MockSlowAnalyzer::EnsureWorkerStarted() {
    // m_mutex must already be held by the caller. Worker creation is lazy so
    // an Initialize() that fails before any SubmitFrame leaves the analyzer
    // in a clean, joinable-free state.
    if (m_workerStarted) {
        return;
    }
    m_workerStarted = true;
    m_worker = std::thread([this]() { this->WorkerLoop(); });
}

void MockSlowAnalyzer::SubmitFrame(ID3D11Texture2D* nv12Texture, int frameIndex, double elapsedSeconds) {
    (void)nv12Texture;

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // Drop-when-busy: keep at most the latest pending job. A real classifier
        // running at 200ms/inference cannot benefit from queue depth > 1 — older
        // entries just go stale. Mirrors how the bridge will drive a real
        // analyzer at 1 Hz.
        if (!m_queue.empty()) {
            m_queue.pop_front();
        }

        PendingJob job;
        job.frameIndex = frameIndex;
        job.elapsedSeconds = elapsedSeconds;
        job.submitTime = Clock::now();
        m_queue.push_back(job);

        ++m_submitCount;

        EnsureWorkerStarted();
    }
    m_cv.notify_one();
}

void MockSlowAnalyzer::WorkerLoop() {
    while (true) {
        PendingJob job;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this]() {
                return m_shutdown.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_shutdown.load(std::memory_order_acquire) && m_queue.empty()) {
                return;
            }
            job = m_queue.front();
            m_queue.pop_front();
        }

        // Simulate model compute. Wait on the shutdown CV so the destructor's
        // join() does not stall up to m_inferenceMs on a tail-end frame.
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            const auto waitFor = std::chrono::milliseconds(m_inferenceMs);
            m_cv.wait_for(lk, waitFor, [this]() {
                return m_shutdown.load(std::memory_order_acquire);
            });
            if (m_shutdown.load(std::memory_order_acquire)) {
                return;
            }
        }

        const auto completionTime = Clock::now();
        const double measuredMs =
            std::chrono::duration<double, std::milli>(completionTime - job.submitTime).count();

        // Cold-start gate: worker still pays the latency cost so the bridge's
        // per-frame latency samples reflect a warmup-bound classifier, but the
        // result is not cached. Cadence gate runs only post-cold-start so the
        // mock continues to publish on the same frame-index schedule as the
        // pre-bridge-hardening version.
        std::lock_guard<std::mutex> lk(m_mutex);
        if (job.frameIndex < kColdStartFrames) {
            continue;
        }
        const bool firstPostCold = (m_publishCount == 0);
        const bool cadenceTick =
            !firstPostCold && (m_publishCount % kPublishCadenceFrames == 0);
        if (!firstPostCold && !cadenceTick && m_hasCached) {
            // Between cadence ticks: refresh the latency stamps so per-frame
            // telemetry reflects the most-recent inference, but keep the
            // existing scene/regions (matches how an analyzer's last result
            // stays valid between fresh inferences).
            m_cached.frameIndex = job.frameIndex;
            m_cached.elapsedSeconds = job.elapsedSeconds;
            m_cached.submitFrameIndex = job.frameIndex;
            m_cached.inferenceMs = measuredMs;
            ++m_publishCount;
            continue;
        }

        m_cached.frameIndex = job.frameIndex;
        m_cached.elapsedSeconds = job.elapsedSeconds;
        m_cached.scene.type = FrameSceneType::VideoContent;
        m_cached.scene.confidence = kSceneConfidence;
        m_cached.scene.source = "mock-slow-analyzer";
        m_cached.scene.debugLabel = "MockSlowAnalyzer";
        m_cached.qualityRegions.clear();
        m_cached.privacyMasks.clear();
        m_cached.submitFrameIndex = job.frameIndex;
        m_cached.inferenceMs = measuredMs;
        m_hasCached = true;
        ++m_publishCount;
    }
}

bool MockSlowAnalyzer::TryGetLatest(AiFrameAnalysis* result) {
    if (!result) {
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_hasCached) {
        return false;
    }
    *result = m_cached;
    return true;
}

} // namespace AetherFlow
