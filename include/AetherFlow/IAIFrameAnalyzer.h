#pragma once

#if defined(_WIN32)
  #include <d3d11.h>
#else
  // macOS / non-Windows: D3D11 is unavailable. The runtime never dereferences
  // these pointers on non-Windows; they exist only for cross-platform shared
  // structs (FrameContext::captureTextureBgra, FrameContext::nv12Texture,
  // IFrameDecisionModule::Initialize).
  struct ID3D11Device;
  struct ID3D11Texture2D;
  struct ID3D11DeviceContext;
#endif

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace AetherFlow {

enum class FrameSceneType {
    Unknown,
    GenericScreen,
    TextUi,
    Slides,
    VideoContent,
    GameMotion,
    SensitiveSurface
};

inline const char* FrameSceneTypeName(FrameSceneType type) {
    switch (type) {
    case FrameSceneType::GenericScreen: return "generic-screen";
    case FrameSceneType::TextUi: return "text-ui";
    case FrameSceneType::Slides: return "slides";
    case FrameSceneType::VideoContent: return "video-content";
    case FrameSceneType::GameMotion: return "game-motion";
    case FrameSceneType::SensitiveSurface: return "sensitive-surface";
    case FrameSceneType::Unknown:
    default:
        return "unknown";
    }
}

enum class FrameRegionPurpose {
    QualityRoi,
    PrivacyMask
};

struct FrameScene {
    FrameSceneType type = FrameSceneType::Unknown;
    float confidence = 0.0f;
    std::string source;
    std::string debugLabel;
};

struct FrameRegion {
    FrameRegionPurpose purpose = FrameRegionPurpose::QualityRoi;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    float confidence = 0.0f;
    std::string source;
    std::string debugLabel;

    int CenterX() const { return left + (right - left) / 2; }
    int CenterY() const { return top + (bottom - top) / 2; }
};

struct FrameDecision {
    int frameIndex = 0;
    double elapsedSeconds = 0.0;
    FrameScene scene;
    std::vector<FrameRegion> qualityRegions;
    std::vector<FrameRegion> privacyMasks;

    // The hysteresis-smoothed canonical scene class from PolicyEngine (5 s +
    // 3-consecutive). Empty until the policy has a stable class. Downstream
    // consumers that want a STABLE class (e.g. SceneDemoActionModule, so the
    // demo effect doesn't flicker on noisy ~1 Hz classifier output) read this
    // instead of the raw per-inference `scene`. The raw `scene` is preserved
    // for honest trace logging of what the classifier actually saw.
    std::string policyStableClass;

    bool HasScene() const { return scene.type != FrameSceneType::Unknown; }
    bool HasQualityRoi() const { return !qualityRegions.empty(); }
    bool HasPrivacyMask() const { return !privacyMasks.empty(); }

    // Confidence-based scene merge (resolves P1). Modules call ProposeScene
    // instead of writing decision->scene directly. The proposed scene wins iff
    // no scene is set yet, or its confidence is strictly higher than the
    // currently-set scene's confidence. Strict greater-than (`>`) preserves
    // first-writer-wins on ties so deterministic producers retain stable
    // ordering when they share the same confidence (e.g. 1.0 for explicit
    // user/CLI actions). Baseline keeps a low confidence so any producer with
    // confidence > 0.5 (deterministic 1.0, async analyzer 0.6+) overrides it.
    void ProposeScene(const FrameScene& proposed) {
        if (scene.type == FrameSceneType::Unknown ||
            proposed.confidence > scene.confidence) {
            scene = proposed;
        }
    }
};

struct FrameContext {
    // The current frame's source image texture at the deterministic-decision
    // stage. This is the *captured BGRA* texture (WGC delivers BGRA;
    // NV12 conversion is a later Stage-4 pipeline product). Async analyzers
    // (e.g. SceneClassifierOnnx) read this; deterministic producers ignore it.
    // Null on non-Windows and when no analyzer that consumes it is wired.
    ID3D11Texture2D* captureTextureBgra = nullptr;
    // Latent / dead at the deterministic-decision stage: NV12 is a Stage-4
    // product, so this is always nullptr here today. Kept for a future genuine
    // NV12-stage analyzer; removing it is a deferred cleanup (out of scope of
    // the P0.1 real-pixel wiring to keep blast radius small).
    ID3D11Texture2D* nv12Texture = nullptr;
    int frameIndex = 0;
    double elapsedSeconds = 0.0;
    int width = 0;
    int height = 0;
    int captureLeft = 0;
    int captureTop = 0;
    int captureWidth = 0;
    int captureHeight = 0;
    bool hasCursor = false;
    bool cursorIsStatic = false;
    int cursorX = 0;
    int cursorY = 0;
    bool panicMaskActive = false;
    std::string panicMaskSource;
    std::string panicMaskDebugLabel;
};

class IFrameDecisionModule {
public:
    virtual ~IFrameDecisionModule() = default;

    virtual bool Initialize(ID3D11Device* device, int width, int height) = 0;
    virtual void Evaluate(const FrameContext& context, FrameDecision* decision) = 0;
    virtual const char* Name() const = 0;

    // Optional one-shot warmup hook called on the producer thread before the
    // pipeline's first frame, to let modules pre-pay any thread-local cold
    // start cost (COM init, UIAutomation proxy, ONNX session warmup, etc.).
    // Default is no-op so existing modules do not need to override.
    virtual void Warmup(const FrameContext& warmupContext) { (void)warmupContext; }
};

class BaselineSceneModule final : public IFrameDecisionModule {
public:
    bool Initialize(ID3D11Device* device, int width, int height) override {
        (void)device;
        m_valid = width > 0 && height > 0;
        return m_valid;
    }

    void Evaluate(const FrameContext& context, FrameDecision* decision) override {
        (void)context;
        if (!decision || !m_valid) {
            return;
        }

        FrameScene proposed;
        proposed.type = FrameSceneType::GenericScreen;
        proposed.confidence = 0.5f;
        proposed.source = "baseline";
        proposed.debugLabel = "BaselineSceneModule";
        decision->ProposeScene(proposed);
    }

    const char* Name() const override { return "BaselineSceneModule"; }

private:
    bool m_valid = false;
};

class CursorFocusModule final : public IFrameDecisionModule {
public:
    explicit CursorFocusModule(int radiusPixels) : m_radius(radiusPixels) {}

    bool Initialize(ID3D11Device* device, int width, int height) override {
        (void)device;
        m_width = width;
        m_height = height;
        return m_width > 0 && m_height > 0;
    }

    void Evaluate(const FrameContext& context, FrameDecision* decision) override {
        if (!decision || !context.hasCursor || m_width <= 0 || m_height <= 0) {
            return;
        }

        const int safeX = (std::max)(0, (std::min)(m_width - 1, context.cursorX));
        const int safeY = (std::max)(0, (std::min)(m_height - 1, context.cursorY));

        FrameRegion region;
        region.purpose = FrameRegionPurpose::QualityRoi;
        region.left = (std::max)(0, safeX - m_radius);
        region.top = (std::max)(0, safeY - m_radius);
        region.right = (std::min)(m_width, safeX + m_radius);
        region.bottom = (std::min)(m_height, safeY + m_radius);
        region.confidence = 1.0f;
        region.source = context.cursorIsStatic ? "static-cursor" : "cursor";
        region.debugLabel = "CursorFocusModule";
        decision->qualityRegions.push_back(region);
    }

    const char* Name() const override { return "CursorFocusModule"; }

private:
    int m_radius = 0;
    int m_width = 0;
    int m_height = 0;
};

class ManualPrivacyMaskModule final : public IFrameDecisionModule {
public:
    explicit ManualPrivacyMaskModule(std::vector<FrameRegion> masks)
        : m_requestedMasks(std::move(masks)) {}

    bool Initialize(ID3D11Device* device, int width, int height) override {
        (void)device;
        m_width = width;
        m_height = height;
        m_masks.clear();

        if (m_width <= 0 || m_height <= 0) {
            return false;
        }

        for (auto mask : m_requestedMasks) {
            mask.purpose = FrameRegionPurpose::PrivacyMask;
            mask.left = (std::max)(0, (std::min)(m_width, mask.left));
            mask.top = (std::max)(0, (std::min)(m_height, mask.top));
            mask.right = (std::max)(0, (std::min)(m_width, mask.right));
            mask.bottom = (std::max)(0, (std::min)(m_height, mask.bottom));
            if (mask.right <= mask.left || mask.bottom <= mask.top) {
                continue;
            }
            if (mask.confidence <= 0.0f) {
                mask.confidence = 1.0f;
            }
            if (mask.source.empty()) {
                mask.source = "manual-privacy-mask";
            }
            if (mask.debugLabel.empty()) {
                mask.debugLabel = "ManualPrivacyMaskModule";
            }
            m_masks.push_back(mask);
        }

        return true;
    }

    void Evaluate(const FrameContext& context, FrameDecision* decision) override {
        (void)context;
        if (!decision || m_masks.empty()) {
            return;
        }

        FrameScene proposed;
        proposed.type = FrameSceneType::SensitiveSurface;
        proposed.confidence = 1.0f;
        proposed.source = "manual-privacy-mask";
        proposed.debugLabel = "ManualPrivacyMaskModule";
        decision->ProposeScene(proposed);

        decision->privacyMasks.insert(
            decision->privacyMasks.end(),
            m_masks.begin(),
            m_masks.end());
    }

    const char* Name() const override { return "ManualPrivacyMaskModule"; }

private:
    std::vector<FrameRegion> m_requestedMasks;
    std::vector<FrameRegion> m_masks;
    int m_width = 0;
    int m_height = 0;
};

class PanicPrivacyMaskModule final : public IFrameDecisionModule {
public:
    bool Initialize(ID3D11Device* device, int width, int height) override {
        (void)device;
        m_width = width;
        m_height = height;
        return m_width > 0 && m_height > 0;
    }

    void Evaluate(const FrameContext& context, FrameDecision* decision) override {
        if (!decision || !context.panicMaskActive || m_width <= 0 || m_height <= 0) {
            return;
        }

        FrameRegion mask;
        mask.purpose = FrameRegionPurpose::PrivacyMask;
        mask.left = 0;
        mask.top = 0;
        mask.right = m_width;
        mask.bottom = m_height;
        mask.confidence = 1.0f;
        mask.source = context.panicMaskSource.empty() ? "panic-privacy-mask" : context.panicMaskSource;
        mask.debugLabel = context.panicMaskDebugLabel.empty()
            ? "PanicPrivacyMaskModule"
            : context.panicMaskDebugLabel;

        FrameScene proposed;
        proposed.type = FrameSceneType::SensitiveSurface;
        proposed.confidence = 1.0f;
        proposed.source = mask.source;
        proposed.debugLabel = mask.debugLabel;
        decision->ProposeScene(proposed);

        decision->privacyMasks.push_back(mask);
    }

    const char* Name() const override { return "PanicPrivacyMaskModule"; }

private:
    int m_width = 0;
    int m_height = 0;
};

class FramePolicyEngine final {
public:
    void AddModule(IFrameDecisionModule* module) {
        if (module) {
            m_modules.push_back(module);
        }
    }

    bool Initialize(ID3D11Device* device, int width, int height) {
        for (auto* module : m_modules) {
            if (!module->Initialize(device, width, height)) {
                return false;
            }
        }
        return true;
    }

    // Call once on the producer thread before the main frame loop so modules
    // can pay thread-local COM/UIA/ONNX cold-start costs up front. The result
    // of warmup is intentionally discarded; modules must not produce
    // user-visible side effects from a warmup call.
    void Warmup(const FrameContext& warmupContext) {
        for (auto* module : m_modules) {
            module->Warmup(warmupContext);
        }
    }

    FrameDecision Evaluate(const FrameContext& context) {
        FrameDecision decision;
        decision.frameIndex = context.frameIndex;
        decision.elapsedSeconds = context.elapsedSeconds;

        for (auto* module : m_modules) {
            module->Evaluate(context, &decision);
        }

        return decision;
    }

private:
    std::vector<IFrameDecisionModule*> m_modules;
};

// One source's contribution to a frame decision, produced asynchronously by an
// IAIFrameAnalyzer (e.g. ONNX scene classifier, OCR/NER, sensitive-surface
// detector). Scene is the primary signal; qualityRegions and privacyMasks are
// scene-derived action hints. Multiple analyzers can coexist; the
// FramePolicyEngine merges their AiFrameAnalysis values into a single
// FrameDecision under a precedence/confidence policy (TODO).
//
// Latency-tracking fields populated by the analyzer worker (bridge-hardening,
// Phase 4 P0 prerequisite):
//   - submitFrameIndex: the frameIndex at which the producer SubmitFrame'd the
//     inputs that yielded this AiFrameAnalysis. Defaults to -1 (no result).
//     The bridge uses (context.frameIndex - submitFrameIndex) to compute
//     staleness in frames.
//   - inferenceMs: measured wall-clock duration of the inference that
//     produced this AiFrameAnalysis (queue-wait + compute). Defaults to 0.0.
//     The verifier consumes per-frame samples to derive p50/p95/p99.
struct AiFrameAnalysis {
    int frameIndex = 0;
    double elapsedSeconds = 0.0;
    FrameScene scene;
    std::vector<FrameRegion> qualityRegions;
    std::vector<FrameRegion> privacyMasks;
    int submitFrameIndex = -1;
    double inferenceMs = 0.0;
};

// GPU-first hook for future scene/text/sensitive-content detection.
// Implementations should consume the D3D11 texture without CPU readback. If an
// implementation needs the texture after SubmitFrame returns, it must take its
// own reference and synchronize before the texture is reused by the pipeline.
//
// SubmitFrame's first parameter is the current frame's source image texture
// (BGRA at the deterministic-decision stage; NV12 conversion is a later
// pipeline stage). The historical parameter name `nv12Texture` is retained
// only to avoid churning every implementer's override declaration — the bridge
// forwards FrameContext::captureTextureBgra into it. Treat the pointer as the
// captured BGRA texture; query its actual D3D11_TEXTURE2D_DESC for
// width/height/format rather than assuming a fixed size or format.
//
// Contract: scene-first. TryGetLatest must populate AiFrameAnalysis::scene
// when it returns true. Regions are optional and represent actions chosen
// from the scene decision, not classification inputs.
//
// Threading contract (bridge-hardening, Phase 4 P0 prerequisite):
//   - SubmitFrame MUST be non-blocking. Implementations that need to run
//     compute on a worker thread are responsible for enqueueing work and
//     returning immediately. If the implementation cannot accept the frame
//     (worker busy, queue full), it must drop the frame silently rather than
//     block the producer.
//   - TryGetLatest MUST be non-blocking. It returns the most recent completed
//     AiFrameAnalysis (or false if none is available yet). Implementations
//     must not wait on a condition variable, mutex contention beyond a brief
//     copy, or any I/O.
//   - The implementer owns thread safety. Internal state shared between the
//     producer-thread Submit/TryGet entry points and any worker thread MUST
//     be guarded (mutex / atomic). Returning std::string / std::vector by
//     value from TryGetLatest requires holding the lock during the copy.
//   - The bridge (AsyncAnalyzerBridgeModule) invokes SubmitFrame and
//     TryGetLatest on the producer thread, once per frame, subject to the
//     bridge's sub-sampling interval (submitEveryNFrames). TryGetLatest is
//     always polled even when SubmitFrame was skipped, so a stale-but-valid
//     result can still contribute to the current frame's decision.
class IAIFrameAnalyzer {
public:
    virtual ~IAIFrameAnalyzer() = default;

    virtual bool Initialize(ID3D11Device* device, int width, int height) = 0;
    virtual void SubmitFrame(ID3D11Texture2D* nv12Texture, int frameIndex, double elapsedSeconds) = 0;
    virtual bool TryGetLatest(AiFrameAnalysis* result) = 0;
};

class NullAIFrameAnalyzer final : public IAIFrameAnalyzer {
public:
    bool Initialize(ID3D11Device* device, int width, int height) override {
        (void)device;
        (void)width;
        (void)height;
        return true;
    }

    void SubmitFrame(ID3D11Texture2D* nv12Texture, int frameIndex, double elapsedSeconds) override {
        (void)nv12Texture;
        (void)frameIndex;
        (void)elapsedSeconds;
    }

    bool TryGetLatest(AiFrameAnalysis* result) override {
        if (result) *result = {};
        return false;
    }
};

} // namespace AetherFlow
