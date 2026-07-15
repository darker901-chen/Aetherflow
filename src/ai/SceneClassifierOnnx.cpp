#include "AetherFlow/ai/SceneClassifierOnnx.h"

#ifdef AETHERFLOW_ENABLE_SCENE_CLASSIFIER

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <vector>

#include <onnxruntime_cxx_api.h>
#if __has_include(<dml_provider_factory.h>)
#include <dml_provider_factory.h>
#define AETHERFLOW_HAVE_DML_FACTORY 1
#endif

namespace AetherFlow {

namespace {

constexpr int kInputH = 224;
constexpr int kInputW = 224;
constexpr int kInputC = 3;
constexpr int kNumClasses = 5;

// CLIP normalization constants. *Not* ImageNet. Must match
// tools/export_clip_zeroshot.py.
constexpr float kClipMean[3] = {0.48145466f, 0.4578275f, 0.40821073f};
constexpr float kClipStd[3]  = {0.26862954f, 0.26130258f, 0.27577711f};

} // namespace

SceneClassifierOnnx::SceneClassifierOnnx(std::string modelPath, Provider providerPreference)
    : m_modelPath(std::move(modelPath)),
      m_providerPreference(providerPreference),
      m_inputBuffer(static_cast<size_t>(1 * kInputC * kInputH * kInputW), 0.0f),
      m_outputBuffer(kNumClasses, 0.0f) {
}

SceneClassifierOnnx::~SceneClassifierOnnx() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_shutdown.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
#if defined(_WIN32)
    // Safe to release D3D handles after the worker is joined: the immediate /
    // video context + the 224x224 scale-target/staging-ring textures are only
    // touched on the producer thread, which is no longer calling SubmitFrame
    // by the time the dtor runs. Release children before parents (output view
    // -> processor -> enumerator -> video device).
    if (m_vpOutView)  { m_vpOutView->Release();  m_vpOutView = nullptr; }
    if (m_scaleTarget){ m_scaleTarget->Release();m_scaleTarget = nullptr; }
    for (int s = 0; s < kStagingRing; ++s) {
        if (m_stagingRing[s]) { m_stagingRing[s]->Release(); m_stagingRing[s] = nullptr; }
    }
    if (m_vp)         { m_vp->Release();         m_vp = nullptr; }
    if (m_vpEnum)     { m_vpEnum->Release();     m_vpEnum = nullptr; }
    if (m_videoContext){ m_videoContext->Release(); m_videoContext = nullptr; }
    if (m_videoDevice){ m_videoDevice->Release();m_videoDevice = nullptr; }
    if (m_immediate)  { m_immediate->Release();  m_immediate = nullptr; }
    m_device = nullptr;  // not owned
#endif
}

bool SceneClassifierOnnx::Initialize(ID3D11Device* device, int width, int height) {
    (void)width;
    (void)height;

#if defined(_WIN32)
    // Store the device + grab its immediate context for the producer-thread
    // GPU-downscale + tiny readback in SubmitFrame (§4.z). The VideoProcessor
    // and the fixed 224x224 scale-target/staging textures are created lazily
    // on first SubmitFrame: the VideoProcessor enumerator must key off the
    // actual source texture desc (capture dims/format), not from width/height
    // here. Idempotent: re-Initialize keeps the first valid device/context.
    if (device && !m_device) {
        m_device = device;
        m_device->GetImmediateContext(&m_immediate);  // AddRef'd; released in dtor
    }
#else
    (void)device;
#endif

    // Idempotent: the explicit ctor-time Initialize and the
    // FramePolicyEngine::Initialize call both land here. Skip session
    // re-creation on the second call to avoid loading the model + DirectML
    // device twice (the ORT session compile is the most expensive cold-start
    // we pay; doing it twice would also confuse the [SceneClassifierOnnx]
    // load banner).
    if (m_sessionReady.load(std::memory_order_acquire)) {
        return true;
    }

    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "aetherflow.scene_classifier");
        m_sessionOptions = std::make_unique<Ort::SessionOptions>();
        m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_sessionOptions->SetIntraOpNumThreads(2);

        bool dmlAppended = false;
#ifdef AETHERFLOW_HAVE_DML_FACTORY
        if (m_providerPreference == Provider::DirectML) {
            try {
                // DML requires single-thread executor & no memory pattern.
                m_sessionOptions->DisableMemPattern();
                m_sessionOptions->SetExecutionMode(ORT_SEQUENTIAL);
                OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_DML(*m_sessionOptions, 0);
                if (status == nullptr) {
                    dmlAppended = true;
                    m_providerName = "DirectML";
                } else {
                    Ort::GetApi().ReleaseStatus(status);
                    std::cerr << "[SceneClassifierOnnx] DirectML EP append failed; falling back to CPU.\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[SceneClassifierOnnx] DirectML EP threw '" << e.what()
                          << "'; falling back to CPU.\n";
            }
        }
#else
        if (m_providerPreference == Provider::DirectML) {
            std::cerr << "[SceneClassifierOnnx] DirectML factory header not available at build time; using CPU.\n";
        }
#endif
        if (!dmlAppended) {
            m_providerName = "CPU";
        }

#if defined(_WIN32)
        // ORT on Windows wants the model path as wchar_t*. Convert UTF-8 -> UTF-16 ourselves to
        // avoid pulling in <codecvt> (deprecated) and to match how the existing project handles
        // narrow paths.
        std::wstring wpath;
        wpath.reserve(m_modelPath.size());
        for (char c : m_modelPath) {
            wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        m_session = std::make_unique<Ort::Session>(*m_env, wpath.c_str(), *m_sessionOptions);
#else
        m_session = std::make_unique<Ort::Session>(*m_env, m_modelPath.c_str(), *m_sessionOptions);
#endif

        m_memInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        m_sessionReady.store(true, std::memory_order_release);
        std::cout << "[SceneClassifierOnnx] Loaded model='" << m_modelPath
                  << "' provider=" << m_providerName
                  << " input=[1," << kInputC << "," << kInputH << "," << kInputW << "]"
                  << " output=[1," << kNumClasses << "]\n";

        // Reset cached state for re-Initialize during a hot restart.
        std::lock_guard<std::mutex> lk(m_mutex);
        m_submitCount = 0;
        m_cached = {};
        m_hasCached = false;
        m_queue.clear();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[SceneClassifierOnnx] Initialize failed: " << e.what() << "\n";
        m_sessionReady.store(false, std::memory_order_release);
        return false;
    }
}

void SceneClassifierOnnx::EnsureWorkerStarted() {
    if (m_workerStarted) return;
    m_workerStarted = true;
    m_worker = std::thread([this]() { this->WorkerLoop(); });
}

void SceneClassifierOnnx::SubmitFrame(ID3D11Texture2D* sourceTexture,
                                      int frameIndex,
                                      double elapsedSeconds) {
    if (!m_sessionReady.load(std::memory_order_acquire)) {
        return;
    }

    // Drop-when-busy check first so we never pay the Blt/Copy cost on a frame
    // the worker can't consume. We snapshot queue-emptiness under the lock;
    // the GPU work + the non-blocking ring Map (§4.z.8 rev2: NO synchronous
    // GPU wait) then happen *outside* the lock so we never hold m_mutex
    // across D3D calls.
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_queue.empty()) {
            // Worker still busy with the previous job — drop this frame.
            return;
        }
    }

#if defined(_WIN32)
    if (!sourceTexture || !m_immediate) {
        return;  // nothing to read back (non-opt-in / non-Windows path)
    }
    std::vector<unsigned char> bgra;
    int srcW = 0;
    int srcH = 0;
    // §4.z.8 rev2: GPU-scale the source -> 224x224 BGRA (Blt + ring Copy),
    // then NON-BLOCKING Map of the *previous* cycle's slot. Returns false on
    // a no-contribution cycle (cold-start cycle 0, WAS_STILL_DRAWING skip, or
    // any D3D failure) — treated as a dropped frame; the producer thread never
    // synchronously waited. srcW/srcH come back fixed 224/224 on success.
    if (!ScaleAndReadback224(sourceTexture, &bgra, &srcW, &srcH)) {
        return;  // no contribution this cycle — drop silently (non-blocking contract)
    }
#else
    (void)sourceTexture;
    return;  // no D3D readback available off-Windows
#endif

#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // Re-check drop-when-busy: the worker may have started a new job
        // between the snapshot above and here. Keep depth-1 semantics.
        if (!m_queue.empty()) {
            return;
        }
        PendingJob job;
        job.frameIndex = frameIndex;
        job.elapsedSeconds = elapsedSeconds;
        job.submitTime = Clock::now();
        job.bgra = std::move(bgra);
        job.srcWidth = srcW;
        job.srcHeight = srcH;
        m_queue.push_back(std::move(job));
        ++m_submitCount;
        EnsureWorkerStarted();
    }
    m_cv.notify_one();
#endif
}

#if defined(_WIN32)
bool SceneClassifierOnnx::ScaleAndReadback224(ID3D11Texture2D* sourceTexture,
                                              std::vector<unsigned char>* out,
                                              int* outWidth,
                                              int* outHeight) {
    if (!sourceTexture || !m_immediate || !m_device || !out) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    sourceTexture->GetDesc(&srcDesc);
    if (srcDesc.Width == 0 || srcDesc.Height == 0) {
        return false;
    }

    // ---- DG1a: lazily build the SceneClassifierOnnx-owned VideoProcessor for
    // a BGRA->BGRA scale (input rect = full source, output rect = 224x224).
    // The enumerator is keyed off the actual source dims/format, so a changed
    // capture region rebuilds only the processor; the 224x224 textures are
    // size-invariant and created exactly once. Mirrors D3D11VideoConverter
    // (src/main.cpp:258) but BGRA->BGRA instead of BGRA->NV12. ----
    const bool needVp =
        !m_vp ||
        m_vpSrcWidth  != static_cast<int>(srcDesc.Width) ||
        m_vpSrcHeight != static_cast<int>(srcDesc.Height) ||
        m_vpSrcFormat != static_cast<unsigned int>(srcDesc.Format);
    if (needVp) {
        // Rebuild the processor chain for the new input size. The 224x224
        // scale-target/staging are NOT rebuilt (size-invariant).
        if (m_vpOutView) { m_vpOutView->Release(); m_vpOutView = nullptr; }
        if (m_vp)        { m_vp->Release();        m_vp = nullptr; }
        if (m_vpEnum)    { m_vpEnum->Release();    m_vpEnum = nullptr; }

        if (!m_videoDevice) {
            HRESULT hr = m_device->QueryInterface(__uuidof(ID3D11VideoDevice),
                                                  reinterpret_cast<void**>(&m_videoDevice));
            if (FAILED(hr) || !m_videoDevice) { m_videoDevice = nullptr; return false; }
        }
        if (!m_videoContext) {
            HRESULT hr = m_immediate->QueryInterface(__uuidof(ID3D11VideoContext),
                                                     reinterpret_cast<void**>(&m_videoContext));
            if (FAILED(hr) || !m_videoContext) { m_videoContext = nullptr; return false; }
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpDesc{};
        vpDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        vpDesc.InputWidth   = srcDesc.Width;
        vpDesc.InputHeight  = srcDesc.Height;
        vpDesc.OutputWidth  = static_cast<UINT>(kInputW);
        vpDesc.OutputHeight = static_cast<UINT>(kInputH);
        vpDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&vpDesc, &m_vpEnum);
        if (FAILED(hr) || !m_vpEnum) { m_vpEnum = nullptr; return false; }
        hr = m_videoDevice->CreateVideoProcessor(m_vpEnum, 0, &m_vp);
        if (FAILED(hr) || !m_vp) { m_vp = nullptr; return false; }

        m_vpSrcWidth  = static_cast<int>(srcDesc.Width);
        m_vpSrcHeight = static_cast<int>(srcDesc.Height);
        m_vpSrcFormat = static_cast<unsigned int>(srcDesc.Format);
    }

    // ---- DG2: one-time creation of the fixed 224x224 BGRA scale target
    // (VideoProcessorBlt dest; RENDER_TARGET so a VideoProcessorOutputView can
    // be created) and the fixed 224x224 BGRA STAGING texture. Reused every
    // submit; released in the dtor. ----
    if (!m_scaleTarget) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width  = static_cast<UINT>(kInputW);
        td.Height = static_cast<UINT>(kInputH);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_scaleTarget);
        if (FAILED(hr) || !m_scaleTarget) {
            // Some drivers reject RENDER_TARGET on a VP output; the VP output
            // view only strictly needs the texture to be a valid VP target.
            td.BindFlags = 0;
            hr = m_device->CreateTexture2D(&td, nullptr, &m_scaleTarget);
        }
        if (FAILED(hr) || !m_scaleTarget) { m_scaleTarget = nullptr; return false; }
    }
    // §4.z.8 rev2 DZ1: lazily create the K=3 ring of 224x224 BGRA STAGING
    // textures (created exactly once, size-invariant, reused every submit,
    // all released in the dtor). One shared scale-target above feeds all
    // three slots via the per-cycle CopyResource.
    for (int s = 0; s < kStagingRing; ++s) {
        if (m_stagingRing[s]) {
            continue;
        }
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width  = static_cast<UINT>(kInputW);
        sd.Height = static_cast<UINT>(kInputH);
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hr = m_device->CreateTexture2D(&sd, nullptr, &m_stagingRing[s]);
        if (FAILED(hr) || !m_stagingRing[s]) { m_stagingRing[s] = nullptr; return false; }
    }
    if (!m_vpOutView) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
        ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        HRESULT hr = m_videoDevice->CreateVideoProcessorOutputView(
            m_scaleTarget, m_vpEnum, &ovDesc, &m_vpOutView);
        if (FAILED(hr) || !m_vpOutView) { m_vpOutView = nullptr; return false; }
    }

    // ---- GPU-async BGRA->BGRA downscale (no CPU sync). Input rect = full
    // source frame, output rect = 224x224 (set implicitly by the content desc
    // OutputWidth/Height + the 224x224 output texture). ----
    ID3D11VideoProcessorInputView* inputView = nullptr;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
    ivDesc.FourCC = 0;
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(
        sourceTexture, m_vpEnum, &ivDesc, &inputView);
    if (FAILED(hr) || !inputView) {
        if (inputView) inputView->Release();
        return false;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView;
    hr = m_videoContext->VideoProcessorBlt(m_vp, m_vpOutView, 0, 1, &stream);
    inputView->Release();
    if (FAILED(hr)) {
        return false;
    }

    // ---- §4.z.8 rev2 (DZ1–DZ3): async ring readback. The producer thread
    // MUST NOT synchronously wait on the GPU here. ----
    //
    // Cycle i = m_submitCycle:
    //  (2) CopyResource the just-Blt'd scale-target into slot i%K. Issue only
    //      — do NOT Map slot i%K this cycle (it would block on the Blt that
    //      was issued microseconds ago: the exact ~19 ms DG1a stall).
    //  (3) For i>=1, Map slot (i-1)%K with D3D11_MAP_FLAG_DO_NOT_WAIT. Its
    //      CopyResource was issued a full sub-sample cycle (~1 s at 1 Hz)
    //      ago, so the GPU has long finished it and the non-blocking Map
    //      returns S_OK immediately. On WAS_STILL_DRAWING: skip this cycle's
    //      contribution (no block, no spin-retry — DZ3).
    //  (4) ++m_submitCycle always (cold start: cycle 0 contributes nothing).
    const unsigned long long i = m_submitCycle;
    const int curSlot  = static_cast<int>(i % static_cast<unsigned long long>(kStagingRing));
    m_immediate->CopyResource(m_stagingRing[curSlot], m_scaleTarget);

    bool contributed = false;
    if (i >= 1) {
        const int readSlot =
            static_cast<int>((i - 1ULL) % static_cast<unsigned long long>(kStagingRing));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT mapHr = m_immediate->Map(m_stagingRing[readSlot], 0, D3D11_MAP_READ,
                                         D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (mapHr == DXGI_ERROR_WAS_STILL_DRAWING) {
            // GPU has not yet finished the (i-1) CopyResource. Skip this
            // cycle's contribution entirely: no memcpy, no enqueue, NO
            // blocking, NO spin-retry (DZ3). The bridge tolerates a missed
            // contribution + the resulting extra staleness (DZ2).
        } else if (FAILED(mapHr) || !mapped.pData) {
            // Any other Map failure: drop this cycle (non-blocking contract).
        } else {
            const int w = kInputW;
            const int h = kInputH;
            const size_t rowBytes = static_cast<size_t>(w) * 4u;  // BGRA = 4 bytes/px
            out->resize(rowBytes * static_cast<size_t>(h));

            const unsigned char* src = static_cast<const unsigned char*>(mapped.pData);
            unsigned char* dst = out->data();
            if (mapped.RowPitch == rowBytes) {
                std::memcpy(dst, src, rowBytes * static_cast<size_t>(h));
            } else {
                // Row-pitch padded: copy row by row, stripping the pad so the
                // worker sees a tight 224*4 stride.
                for (int y = 0; y < h; ++y) {
                    std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                                src + static_cast<size_t>(y) * mapped.RowPitch,
                                rowBytes);
                }
            }
            m_immediate->Unmap(m_stagingRing[readSlot], 0);
            if (outWidth) *outWidth = w;
            if (outHeight) *outHeight = h;
            contributed = true;
        }
    }
    // (4) Advance the cycle unconditionally — every Blt-path call consumes a
    // ring slot regardless of whether it contributed (cold start / skip).
    ++m_submitCycle;

    // false on cold-start cycle 0 or a WAS_STILL_DRAWING skip: SubmitFrame
    // treats that as a dropped frame (non-blocking contract). The producer
    // thread never blocked.
    return contributed;
}
#endif

bool SceneClassifierOnnx::TryGetLatest(AiFrameAnalysis* result) {
    if (!result) return false;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_hasCached) return false;
    *result = m_cached;
    return true;
}

void SceneClassifierOnnx::BuildInputFromBgra(const unsigned char* bgra,
                                             int srcWidth,
                                             int srcHeight) {
    // §4.z (DG3): the GPU (ScaleAndReadback224) already scaled to 224x224, so
    // the input is a tightly-packed 224x224 BGRA buffer (stride 224*4). NO
    // resize here any more — just a straight per-pixel BGRA->RGB channel swap
    // + CLIP mean/std normalize -> NCHW float32 in m_inputBuffer. The CLIP
    // constants (kClipMean/kClipStd) and the BGRA->RGB channel order are
    // UNCHANGED from §4.x; the only semantic change is "resize happens on the
    // GPU now, not on the CPU worker".
    const int planeSize = kInputH * kInputW;
    float* base = m_inputBuffer.data();

    // srcWidth/srcHeight are always 224/224 post-§4.z; treat any mismatch
    // (incl. null) as a readback failure and zero-fill (valid logits, no UB).
    if (!bgra || srcWidth != kInputW || srcHeight != kInputH) {
        std::fill(m_inputBuffer.begin(), m_inputBuffer.end(), 0.0f);
        return;
    }

    const size_t srcStride = static_cast<size_t>(kInputW) * 4u;
    for (int y = 0; y < kInputH; ++y) {
        const unsigned char* srcRow = bgra + static_cast<size_t>(y) * srcStride;
        for (int x = 0; x < kInputW; ++x) {
            const unsigned char* px = srcRow + static_cast<size_t>(x) * 4u;
            // Captured texture is BGRA (B=px[0], G=px[1], R=px[2]). CLIP wants
            // RGB. Normalize to [0,1] then apply CLIP mean/std per channel.
            const float r = static_cast<float>(px[2]) / 255.0f;
            const float g = static_cast<float>(px[1]) / 255.0f;
            const float b = static_cast<float>(px[0]) / 255.0f;
            const int idx = y * kInputW + x;
            base[0 * planeSize + idx] = (r - kClipMean[0]) / kClipStd[0];
            base[1 * planeSize + idx] = (g - kClipMean[1]) / kClipStd[1];
            base[2 * planeSize + idx] = (b - kClipMean[2]) / kClipStd[2];
        }
    }
}

int SceneClassifierOnnx::ArgmaxSoftmax(const float* logits, float* confidence) const {
    // Numerically-stable softmax: subtract max before exp.
    float maxLogit = logits[0];
    for (int i = 1; i < kNumClasses; ++i) {
        if (logits[i] > maxLogit) maxLogit = logits[i];
    }
    float sum = 0.0f;
    std::array<float, kNumClasses> exps{};
    for (int i = 0; i < kNumClasses; ++i) {
        exps[i] = std::exp(logits[i] - maxLogit);
        sum += exps[i];
    }
    if (sum <= 0.0f) sum = 1e-9f;
    int argmax = 0;
    float best = -1.0f;
    for (int i = 0; i < kNumClasses; ++i) {
        const float p = exps[i] / sum;
        if (p > best) {
            best = p;
            argmax = i;
        }
    }
    if (confidence) *confidence = best;
    return argmax;
}

FrameSceneType SceneClassifierOnnx::MapClassIndexToFrameSceneType(int classIndex) {
    switch (classIndex) {
    case 0: return FrameSceneType::TextUi;
    case 1: return FrameSceneType::Slides;
    case 2: return FrameSceneType::VideoContent;
    case 3: return FrameSceneType::GenericScreen;   // mixed_ui
    case 4: return FrameSceneType::SensitiveSurface;
    default: return FrameSceneType::Unknown;
    }
}

const char* SceneClassifierOnnx::MapClassIndexToCanonicalName(int classIndex) {
    switch (classIndex) {
    case 0: return "code_text";
    case 1: return "slides";
    case 2: return "video";
    case 3: return "mixed_ui";
    case 4: return "sensitive_surface";
    default: return "unknown";
    }
}

void SceneClassifierOnnx::WorkerLoop() {
    // Input + output tensor descriptors. The model has one input named
    // "image" and one output named "logits" (see tools/export_clip_zeroshot.py).
    const std::array<int64_t, 4> inputShape{1, kInputC, kInputH, kInputW};
    const std::array<int64_t, 2> outputShape{1, kNumClasses};
    const std::array<const char*, 1> inputNames{"image"};
    const std::array<const char*, 1> outputNames{"logits"};

    while (true) {
        PendingJob job;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this]() {
                return m_shutdown.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_shutdown.load(std::memory_order_acquire) && m_queue.empty()) return;
            job = m_queue.front();
            m_queue.pop_front();
        }
        if (m_shutdown.load(std::memory_order_acquire)) return;

        // ---- Preprocess: real pixels, GPU-scaled to 224x224 on the producer
        // thread (§4.z), so this is just BGRA->RGB + CLIP-normalize. ----
        BuildInputFromBgra(job.bgra.data(), job.srcWidth, job.srcHeight);

        // ---- Inference ----
        float bestConfidence = 0.0f;
        int classIndex = -1;
        bool inferenceOk = false;
        try {
            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                *m_memInfo,
                m_inputBuffer.data(),
                m_inputBuffer.size(),
                inputShape.data(),
                inputShape.size());

            Ort::Value outputTensor = Ort::Value::CreateTensor<float>(
                *m_memInfo,
                m_outputBuffer.data(),
                m_outputBuffer.size(),
                outputShape.data(),
                outputShape.size());

            Ort::RunOptions runOptions;
            m_session->Run(
                runOptions,
                inputNames.data(),
                &inputTensor,
                1,
                outputNames.data(),
                &outputTensor,
                1);

            classIndex = ArgmaxSoftmax(m_outputBuffer.data(), &bestConfidence);
            inferenceOk = true;
        } catch (const std::exception& e) {
            std::cerr << "[SceneClassifierOnnx] Run failed at frame " << job.frameIndex
                      << ": " << e.what() << "\n";
        }

        const auto completionTime = Clock::now();
        const double measuredMs =
            std::chrono::duration<double, std::milli>(completionTime - job.submitTime).count();

        if (!inferenceOk) {
            // Refresh latency stamps but do not change the cached scene; the
            // bridge will keep using the prior result and the next successful
            // inference will recover.
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_hasCached) {
                m_cached.frameIndex = job.frameIndex;
                m_cached.elapsedSeconds = job.elapsedSeconds;
                m_cached.submitFrameIndex = job.frameIndex;
                m_cached.inferenceMs = measuredMs;
            }
            continue;
        }

        // ---- Publish ----
        AiFrameAnalysis next;
        next.frameIndex = job.frameIndex;
        next.elapsedSeconds = job.elapsedSeconds;
        next.scene.type = MapClassIndexToFrameSceneType(classIndex);
        next.scene.confidence = bestConfidence;
        next.scene.source = "scene-classifier-onnx";
        // Stamp the canonical 5-class name into debugLabel so the
        // PolicyEngine can recover the policy class even though
        // FrameSceneType conflates code_text -> TextUi etc.
        next.scene.debugLabel = MapClassIndexToCanonicalName(classIndex);
        next.qualityRegions.clear();
        next.privacyMasks.clear();
        next.submitFrameIndex = job.frameIndex;
        next.inferenceMs = measuredMs;

        std::lock_guard<std::mutex> lk(m_mutex);
        m_cached = next;
        m_hasCached = true;
    }
}

} // namespace AetherFlow

#endif // AETHERFLOW_ENABLE_SCENE_CLASSIFIER
