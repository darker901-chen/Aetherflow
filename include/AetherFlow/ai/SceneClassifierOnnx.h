#pragma once

// SceneClassifierOnnx — Phase 4 P0.1 Stage A scene classifier.
//
// Runs a CLIP ViT-B/32 zero-shot classifier (5 classes) exported by
// tools/export_clip_zeroshot.py. Implements the IAIFrameAnalyzer contract:
// non-blocking SubmitFrame, non-blocking TryGetLatest, real-thread worker
// (mirrors MockSlowAnalyzer's threading model). Drop-when-busy queue depth 1.
//
// Build gating: `#ifdef AETHERFLOW_ENABLE_SCENE_CLASSIFIER`. When ORT is not
// available the type compiles to an empty body so the rest of the binary still
// links. Callers should also `#ifdef` the construction site.
//
// Provider preference: DirectML first (strongly preferred for laptops with
// integrated GPUs), CPU fallback when DirectML construction throws. The EP
// actually loaded is printed to stderr at construction time and reported via
// `ProviderName()` for the bridge startup banner.
//
// Input handling (P0.1 real-pixel wiring, 2026-05-15; GPU-downscale jitter
// fix §4.z DG1a, 2026-05-16; ring-buffer async readback §4.z.8 revision 2,
// 2026-05-16): the IAIFrameAnalyzer's SubmitFrame parameter (historically
// named `nv12Texture` for signature stability) carries the captured *BGRA*
// source texture forwarded by the bridge from
// FrameContext::captureTextureBgra. SubmitFrame runs on the producer thread
// (per the IAIFrameAnalyzer contract), so it may safely use the device's
// immediate context. Per §4.z (DG1a/DG2/DG3) the producer thread does the
// 224x224 downscale ON THE GPU before any CPU readback: an
// SceneClassifierOnnx-owned ID3D11VideoProcessor (input rect = full source,
// output rect = 224x224) does a BGRA->BGRA VideoProcessorBlt into a fixed
// 224x224 BGRA scale-target texture (GPU-async, no CPU sync).
//
// §4.z.8 revision 2 (DZ1–DZ5): the DG1a downscale alone still stalled the
// producer ~19 ms on a 2560x1440 source because the same-cycle Map(READ)
// synchronously waited on the Blt to drain. The readback is now decoupled
// from the Blt by exactly one sub-sample cycle through a K=3 ring of 224x224
// STAGING textures. Per submit cycle i: VideoProcessorBlt source ->
// (shared) m_scaleTarget; CopyResource m_scaleTarget -> m_stagingRing[i%3]
// (issue only, NOT Map'd this cycle); then for i>=1 attempt
// Map(m_stagingRing[(i-1)%3], D3D11_MAP_FLAG_DO_NOT_WAIT). On S_OK:
// row-pitch-safe memcpy of the 224x224 BGRA into the worker buffer + enqueue.
// On DXGI_ERROR_WAS_STILL_DRAWING: skip this cycle's contribution — no block,
// no spin-retry (DZ3). The producer thread never synchronously waits on the
// GPU. Cold start (DZ2): cycle 0 has no i-1 slot so the first contribution
// lands at cycle 1; a ~1-cycle (~1 s at 1 Hz) warm-up with no classifier
// contribution is acceptable because the deterministic baseline scene already
// covers cold-start frames. The contributed result is exactly one sub-sample
// interval staler than DG1a; that is within the bridge's existing
// `analyzerStalenessFrames` tolerance (no bridge change). The worker then
// does ONLY BGRA->RGB + CLIP-normalize -> ONNX -> softmax -> argmax (the
// resize is already done on the GPU). When the texture pointer is null
// (non-Windows, no analyzer wired) the frame is dropped.
//
// CLIP normalization (per the export — NOT ImageNet):
//   mean = [0.48145466, 0.4578275, 0.40821073]
//   std  = [0.26862954, 0.26130258, 0.27577711]
//
// Class mapping (canonical, must match every artifact):
//   0 -> code_text
//   1 -> slides
//   2 -> video
//   3 -> mixed_ui
//   4 -> sensitive_surface
// A UI-facing copy of this order lives in include/AetherFlow/app/
// PipelineRunner.h (kSceneClassNames) — keep BOTH in sync when re-exporting.

#include "AetherFlow/IAIFrameAnalyzer.h"

#ifdef AETHERFLOW_ENABLE_SCENE_CLASSIFIER

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Forward-declare Ort types so the public header does not leak
// <onnxruntime_cxx_api.h> into every translation unit.
namespace Ort {
class Env;
class Session;
class SessionOptions;
class MemoryInfo;
}

namespace AetherFlow {

class SceneClassifierOnnx final : public IAIFrameAnalyzer {
public:
    enum class Provider {
        DirectML,
        CPU
    };

    SceneClassifierOnnx(std::string modelPath, Provider providerPreference);
    ~SceneClassifierOnnx() override;

    SceneClassifierOnnx(const SceneClassifierOnnx&) = delete;
    SceneClassifierOnnx& operator=(const SceneClassifierOnnx&) = delete;

    bool Initialize(ID3D11Device* device, int width, int height) override;
    void SubmitFrame(ID3D11Texture2D* nv12Texture, int frameIndex, double elapsedSeconds) override;
    bool TryGetLatest(AiFrameAnalysis* result) override;

    // True iff Initialize succeeded (ONNX session loaded). When false, the
    // analyzer behaves as a no-op (TryGetLatest returns false). Used by the
    // CLI/env wiring path to skip registering the bridge when the model is
    // missing or fails to load.
    bool IsReady() const { return m_sessionReady; }

    // Returns "DirectML" or "CPU" depending on which EP the session actually
    // loaded. Reported in the bridge startup banner so the trace consumer can
    // attribute inference latency to a backend.
    const char* ProviderName() const { return m_providerName.c_str(); }

private:
    using Clock = std::chrono::high_resolution_clock;

    struct PendingJob {
        int frameIndex = 0;
        double elapsedSeconds = 0.0;
        Clock::time_point submitTime{};
        // Worker-owned tightly-packed BGRA pixels read back from the captured
        // texture on the producer thread. §4.z (DG3): the GPU already scaled
        // to 224x224, so this is now ALWAYS exactly kInputW*kInputH*4 bytes
        // (224*224*4 = ~196 KB) with a tight 224*4 stride (RowPitch padding
        // stripped during readback). srcWidth/srcHeight are kept for the
        // worker's defensive assertion and are always 224/224.
        std::vector<unsigned char> bgra;
        int srcWidth = 0;
        int srcHeight = 0;
    };

    void EnsureWorkerStarted();
    void WorkerLoop();

    // Convert a tightly-packed 224x224 BGRA frame (stride 224*4, already
    // GPU-scaled by ScaleAndReadback224) into a CLIP-normalized
    // [1,3,224,224] float32 NCHW RGB tensor. §4.z (DG3): NO resize here any
    // more — just a straight per-pixel BGRA->RGB channel swap + CLIP mean/std
    // normalization. Output is the pre-allocated m_inputBuffer. The
    // srcWidth/srcHeight params are kept (always 224/224) for a defensive
    // size check; CLIP constants + channel order are UNCHANGED from §4.x.
    void BuildInputFromBgra(const unsigned char* bgra, int srcWidth, int srcHeight);

#if defined(_WIN32)
    // §4.z (DG1a/DG2) + §4.z.8 rev2 (DZ1–DZ3): producer-thread GPU-downscale
    // + K=3 ring async readback. Lazily creates (once) the
    // SceneClassifierOnnx-owned ID3D11VideoProcessor (input rect = full source
    // desc, output rect = 224x224), the shared fixed 224x224 BGRA scale-target
    // texture (VideoProcessorBlt output, owns a VideoProcessorOutputView) and
    // a ring of 3 fixed 224x224 D3D11_USAGE_STAGING textures. Per submit cycle
    // i = m_submitCycle: (1) VideoProcessorBlt source -> m_scaleTarget
    // (GPU-async); (2) CopyResource m_scaleTarget -> m_stagingRing[i%3] (issue
    // only, NOT Map'd this cycle); (3) if i>=1, Map(m_stagingRing[(i-1)%3],
    // D3D11_MAP_FLAG_DO_NOT_WAIT): on S_OK row-pitch-safe memcpy 224x224x4 ->
    // `out` and report a contribution (returns true with *out filled); on
    // DXGI_ERROR_WAS_STILL_DRAWING return false with *contributed=false (skip,
    // no block, no retry — DZ3). (4) ++m_submitCycle always. Returns false
    // (and drops the frame, non-blocking contract) on any D3D failure or
    // no-contribution cycle (cold start cycle 0, or WAS_STILL_DRAWING). The
    // VideoProcessor enumerator is keyed off the actual source dims so a
    // changed capture region still scales correctly; the scale-target and the
    // ring slots are permanently 224x224. All handles released in the dtor.
    bool ScaleAndReadback224(ID3D11Texture2D* sourceTexture,
                             std::vector<unsigned char>* out,
                             int* outWidth,
                             int* outHeight);
#endif

    // Convert raw logits to softmax probabilities + argmax. Returns the class
    // index [0,5) and stores its softmax probability in *confidence.
    int ArgmaxSoftmax(const float* logits, float* confidence) const;

    static FrameSceneType MapClassIndexToFrameSceneType(int classIndex);
    static const char* MapClassIndexToCanonicalName(int classIndex);

    const std::string m_modelPath;
    const Provider m_providerPreference;

#if defined(_WIN32)
    // D3D11 handles for the producer-thread GPU-downscale + tiny readback
    // (§4.z DG1a/DG2). Stored at Initialize; the immediate/video context is
    // only ever touched on the producer thread (SubmitFrame), which the
    // IAIFrameAnalyzer contract guarantees is the same thread that drives
    // capture + convert. Raw pointers, manual AddRef/Release: the device
    // outlives this analyzer (owned by the encoder, not owned here); every
    // other handle below is owned here and released in the dtor.
    ID3D11Device* m_device = nullptr;            // not owned
    ID3D11DeviceContext* m_immediate = nullptr;  // not owned (AddRef'd; released in dtor)

    // DG1a: SceneClassifierOnnx-owned VideoProcessor for BGRA->BGRA scale.
    ID3D11VideoDevice* m_videoDevice = nullptr;               // owned
    ID3D11VideoContext* m_videoContext = nullptr;             // owned
    ID3D11VideoProcessorEnumerator* m_vpEnum = nullptr;       // owned
    ID3D11VideoProcessor* m_vp = nullptr;                     // owned
    ID3D11VideoProcessorOutputView* m_vpOutView = nullptr;    // owned
    // Shared fixed 224x224 BGRA scale target (VideoProcessorBlt dest,
    // RENDER_TARGET). NOT ringed (DZ1): the Blt overwrites it each cycle and
    // the per-cycle CopyResource snapshots it into the ring slot before the
    // next cycle's Blt, so a single shared scale-target is correct.
    ID3D11Texture2D* m_scaleTarget = nullptr;                 // owned
    // §4.z.8 rev2 DZ1: K=3 ring of fixed 224x224 BGRA STAGING textures. Slot
    // i%3 receives cycle i's CopyResource; slot (i-1)%3 is Map'd next cycle
    // with D3D11_MAP_FLAG_DO_NOT_WAIT (decoupled from the Blt by one
    // sub-sample cycle). K=3 (not 2): keeps the current Blt + the previous
    // cycle's Copy in flight plus one safety slot. Created lazily once,
    // reused every submit, all 3 released in the dtor.
    static constexpr int kStagingRing = 3;
    ID3D11Texture2D* m_stagingRing[kStagingRing] = {nullptr, nullptr, nullptr}; // owned
    // Monotonically-increasing submit counter. Incremented ONLY on a cycle
    // that runs the Blt path (a real bridge SubmitFrame), NOT every producer
    // frame. Drives the ring slot index and the cold-start (i==0 has no i-1
    // slot) gate.
    unsigned long long m_submitCycle = 0;
    // Source dims the VideoProcessor enumerator was built for. If the capture
    // region changes, the VideoProcessor is rebuilt for the new input size;
    // the 224x224 scale-target/ring slots are size-invariant and never
    // rebuilt.
    int m_vpSrcWidth = 0;
    int m_vpSrcHeight = 0;
    unsigned int m_vpSrcFormat = 0;              // DXGI_FORMAT of the source
#endif

    // ORT runtime state. unique_ptr so the public header does not need the
    // ORT C++ headers. All access on the worker thread except read-only
    // m_sessionReady (atomic) for the producer thread short-circuit.
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memInfo;
    std::atomic<bool> m_sessionReady{false};
    std::string m_providerName = "uninitialized";

    // Input/output tensor buffers, owned by this analyzer so the worker does
    // not malloc per frame.
    std::vector<float> m_inputBuffer;   // [1*3*224*224] CLIP-normalized
    std::vector<float> m_outputBuffer;  // [1*5] raw logits

    // Worker thread state (mirrors MockSlowAnalyzer pattern).
    int m_submitCount = 0;          // protected by m_mutex
    AiFrameAnalysis m_cached{};     // protected by m_mutex
    bool m_hasCached = false;       // protected by m_mutex
    std::deque<PendingJob> m_queue; // protected by m_mutex; depth-1
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
    bool m_workerStarted = false;   // protected by m_mutex
};

} // namespace AetherFlow

#endif // AETHERFLOW_ENABLE_SCENE_CLASSIFIER
