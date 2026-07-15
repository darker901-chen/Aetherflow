#pragma once

// SRT/MPEG-TS live output stage (spec Delta A).
//
// Consumes encoded H.264 access units through the IEncodedFrameSink seam and
// serves them as an MPEG-TS stream on a local SRT *listener* — any device on
// the LAN can watch with `ffplay/VLC srt://<host-ip>:<port>`. v1 contract:
// video-only, one viewer at a time, reconnect loop (viewer leaves → listener
// reopens), default port 8888 / latency 120 ms / no passphrase.
//
// Threading model (realtime discipline):
// - OnEncodedAccessUnit() is called on the encoder's drain/consumer thread.
//   It only copies the AU into a bounded drop-oldest queue — it never touches
//   the network and never blocks beyond a short mutex hold.
// - A dedicated worker thread owns ALL libavformat/SRT state: blocking
//   listener accept (interruptible for shutdown), mux, send, reconnect.
// - The capture/producer thread is never involved.
//
// This header deliberately leaks no FFmpeg types; only the .cpp includes the
// vendored SDK (third_party/ffmpeg/SOURCE.md), and the whole component is
// compiled only when CMake defines AETHERFLOW_ENABLE_SRT_OUTPUT.

#include "AetherFlow/IEncodedFrameSink.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AetherFlow {

class SrtStreamOutput : public IEncodedFrameSink {
public:
    struct Options {
        int port = 8888;
        int latencyMs = 120;        // SRT latency; converted to µs for FFmpeg
        std::string passphrase;     // empty = no encryption
        int fps = 30;
        int width = 1920;
        int height = 1080;
    };

    struct Stats {
        uint64_t enqueued = 0;                 // AUs accepted from the encoder
        uint64_t sent = 0;                     // AUs written to a viewer
        uint64_t bytesSent = 0;
        uint64_t droppedQueueFull = 0;         // bounded-queue overflow (oldest dropped)
        uint64_t droppedAwaitingKeyframe = 0;  // dropped until a safe stream entry point
        uint32_t connections = 0;              // accepted viewer connections
        bool listening = false;
        bool clientConnected = false;
    };

    explicit SrtStreamOutput(const Options& options);
    ~SrtStreamOutput() override;

    SrtStreamOutput(const SrtStreamOutput&) = delete;
    SrtStreamOutput& operator=(const SrtStreamOutput&) = delete;

    // Spawns the worker thread. Returns false when the linked FFmpeg lacks
    // the `srt` protocol (nothing is spawned; streaming is disabled).
    bool Start();
    // Stops the worker (interrupts a blocking listen) and joins it. After
    // Stop(), OnEncodedAccessUnit() is a no-op. Called by the destructor.
    void Stop();

    // IEncodedFrameSink — encoder drain-thread side; copy + return.
    void OnEncodedAccessUnit(const EncodedAccessUnit& au) override;

    Stats GetStats() const;
    std::string ListenUrl() const;  // srt://0.0.0.0:<port>

    // Viewer URLs (`srt://127.0.0.1:<port>` first, then one per local IPv4)
    // so the user knows what to paste into VLC. Windows-only helper; lives
    // here so callers do not need winsock includes.
    static std::vector<std::string> ShareUrls(int port);
    // Prints the above as `[SRT] ...` console lines (CLI path).
    static void PrintShareUrls(int port);

private:
    struct QueuedAu {
        std::vector<uint8_t> data;
        uint64_t pts90k = 0;
        bool keyframe = false;
    };

    void WorkerLoop();
    // One listen → serve → disconnect cycle. Returns true when a viewer was
    // actually served (used to pick the retry backoff).
    bool OpenAndServeOnce();
    static int InterruptCb(void* opaque);

    Options m_options;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<QueuedAu> m_queue;
    size_t m_queueBytes = 0;

    // Cached SPS/PPS, guarded by m_queueMutex. Captured on the ENQUEUE side
    // (OnEncodedAccessUnit scans every keyframe AU) — not in the serve loop —
    // because the serve loop clears the queue when a viewer connects, and the
    // encoders emit in-band parameter sets only on the FIRST IDR. A cache
    // filled only from served AUs would miss that first IDR and every
    // mid-stream join would get an undecodable stream (found live: viewer
    // received sent=frames-60 AUs and decoded none).
    std::vector<uint8_t> m_cachedSpsPps;

    std::thread m_worker;
    std::atomic<bool> m_stop{false};

    mutable std::mutex m_statsMutex;
    Stats m_stats;
};

}  // namespace AetherFlow
