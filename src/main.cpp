#if defined(_WIN32)
#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <string>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <atlbase.h>
#include <Windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <numeric>
#include <cstdlib>
#include <cctype>
#include <timeapi.h>
#include <filesystem>
#include <fstream>
#include <sstream>

#pragma comment(lib, "winmm.lib")

#include "AetherFlow/Config.h"
#include "AetherFlow/NvencRoiDefaults.h"
#include "AetherFlow/IH264Encoder.h"
#include "AetherFlow/IAIFrameAnalyzer.h"
#include "AetherFlow/AsyncAnalyzerBridgeModule.h"
#include "AetherFlow/MockSlowAnalyzer.h"
#include "AetherFlow/policy/PolicyDecision.h"
#include "AetherFlow/policy/PolicyEngine.h"
#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
#include "AetherFlow/ai/SceneClassifierOnnx.h"
#endif
#include "AetherFlow/PasswordFieldPrivacyMaskModule.h"
#include "AetherFlow/SceneDemoActionModule.h"
#include "AetherFlow/PrivacyMaskCompositor.h"
#include "AetherFlow/NotificationProducerModule.h"
#include "AetherFlow/VplH264Wrapper.h"
#if defined(AETHERFLOW_ENABLE_NVENC)
#include "AetherFlow/NvencH264Wrapper.h"
#endif
#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
#include "AetherFlow/streaming/SrtStreamOutput.h"
#endif
#include "AetherFlow/app/PipelineRunner.h"
#include "AetherFlow/ScreenCapture.h"

#pragma comment(lib, "d3d11.lib")

static bool HasIntelAdapter() {
    CComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) {
        return false;
    }

    for (UINT i = 0;; i++) {
        CComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;

        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.VendorId == 0x8086) return true; // Intel
    }
    return false;
}

static bool HasNvidiaAdapter() {
    CComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) {
        return false;
    }

    for (UINT i = 0;; i++) {
        CComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;

        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.VendorId == 0x10DE) return true; // NVIDIA
    }
    return false;
}

static bool HasNvencRuntime() {
    HMODULE lib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!lib) return false;
    auto proc = GetProcAddress(lib, "NvEncodeAPICreateInstance");
    FreeLibrary(lib);
    return proc != nullptr;
}

static int GetEnvInt(const char* name, int defaultValue) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return defaultValue;
    }
    return std::atoi(value);
}

static bool GetEnvBool(const char* name, bool defaultValue) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return defaultValue;
    }

    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return defaultValue;
}

class RightCtrlPanicMaskHotkey final {
public:
    explicit RightCtrlPanicMaskHotkey(std::chrono::milliseconds latchDuration)
        : m_latchDuration(latchDuration) {}

    bool PollActive() {
        const auto now = Clock::now();
        if ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0) {
            m_activeUntil = now + m_latchDuration;
            return true;
        }
        return now < m_activeUntil;
    }

private:
    using Clock = std::chrono::steady_clock;

    std::chrono::milliseconds m_latchDuration;
    Clock::time_point m_activeUntil = (Clock::time_point::min)();
};

static AetherFlow::FrameRegion MakePrivacyMaskRegion(
    int left,
    int top,
    int right,
    int bottom,
    const std::string& source) {
    AetherFlow::FrameRegion region;
    region.purpose = AetherFlow::FrameRegionPurpose::PrivacyMask;
    region.left = left;
    region.top = top;
    region.right = right;
    region.bottom = bottom;
    region.confidence = 1.0f;
    region.source = source;
    region.debugLabel = "ManualPrivacyMaskModule";
    return region;
}

static bool ParsePrivacyMaskSpec(
    const std::string& spec,
    const std::string& source,
    AetherFlow::FrameRegion* region) {
    if (!region) {
        return false;
    }

    std::stringstream ss(spec);
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    char c1 = 0;
    char c2 = 0;
    char c3 = 0;
    if (!(ss >> left >> c1 >> top >> c2 >> right >> c3 >> bottom) ||
        c1 != ',' || c2 != ',' || c3 != ',') {
        return false;
    }
    ss >> std::ws;
    if (!ss.eof() || right <= left || bottom <= top) {
        return false;
    }

    *region = MakePrivacyMaskRegion(left, top, right, bottom, source);
    return true;
}

static void ParsePrivacyMaskList(
    const std::string& value,
    const std::string& source,
    std::vector<AetherFlow::FrameRegion>* masks) {
    if (!masks) {
        return;
    }

    std::stringstream list(value);
    std::string item;
    while (std::getline(list, item, ';')) {
        AetherFlow::FrameRegion region;
        if (ParsePrivacyMaskSpec(item, source, &region)) {
            masks->push_back(region);
        } else if (!item.empty()) {
            std::cerr << "[LiveShareGuard] Ignoring invalid privacy mask spec: " << item << "\n";
        }
    }
}

static std::filesystem::path GetExeDir() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path).parent_path();
}

static std::filesystem::path GetOutputDir() {
    if (const char* env = std::getenv("AETHERFLOW_OUTPUT_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    return GetExeDir() / ".." / ".." / "output";
}

static std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch))
                    << std::dec << std::setfill(' ');
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

// D3D11 Video Processor based converter: BGRA → NV12 (GPU only, handles downscaling)
// This replaces the compute-shader approach. CopySubresourceRegion from R8/R8G8 to
// NV12 subresources silently fails on D3D11 (format incompatibility), so the CS
// path produced a zeroed NV12 buffer → green screen. VideoProcessorBlt writes
// NV12 natively and is the canonical D3D11 way to do BGRA→NV12.
class D3D11VideoConverter {
public:
    CComPtr<ID3D11Device>                     m_dev;
    CComPtr<ID3D11DeviceContext>              m_ctx;
    CComPtr<ID3D11VideoDevice>                m_videoDevice;
    CComPtr<ID3D11VideoContext>               m_videoCtx;
    CComPtr<ID3D11VideoProcessorEnumerator>   m_vpEnum;
    CComPtr<ID3D11VideoProcessor>             m_vp;
    CComPtr<ID3D11Texture2D>                  m_nv12Out;
    CComPtr<ID3D11VideoProcessorOutputView>   m_outView;
    int m_srcW = 0, m_srcH = 0;

    D3D11VideoConverter(ID3D11Device* dev, ID3D11DeviceContext* ctx) : m_dev(dev), m_ctx(ctx) {}

    bool Initialize(int srcW, int srcH, int dstW, int dstH) {
        m_srcW = srcW; m_srcH = srcH;

        HRESULT hr = m_dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&m_videoDevice);
        if (FAILED(hr)) { std::cerr << "[VideoConv] No ID3D11VideoDevice (hr=" << std::hex << hr << ")\n"; return false; }
        hr = m_ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&m_videoCtx);
        if (FAILED(hr)) { std::cerr << "[VideoConv] No ID3D11VideoContext\n"; return false; }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpDesc = {};
        vpDesc.InputFrameFormat  = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        vpDesc.InputWidth  = static_cast<UINT>(srcW);
        vpDesc.InputHeight = static_cast<UINT>(srcH);
        vpDesc.OutputWidth = static_cast<UINT>(dstW);
        vpDesc.OutputHeight= static_cast<UINT>(dstH);
        vpDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = m_videoDevice->CreateVideoProcessorEnumerator(&vpDesc, &m_vpEnum);
        if (FAILED(hr)) { std::cerr << "[VideoConv] CreateVideoProcessorEnumerator failed\n"; return false; }
        hr = m_videoDevice->CreateVideoProcessor(m_vpEnum, 0, &m_vp);
        if (FAILED(hr)) { std::cerr << "[VideoConv] CreateVideoProcessor failed\n"; return false; }

        // NV12 intermediate output texture (RENDER_TARGET required for VP output view)
        D3D11_TEXTURE2D_DESC td = {};
        td.Width  = static_cast<UINT>(dstW);
        td.Height = static_cast<UINT>(dstH);
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = m_dev->CreateTexture2D(&td, nullptr, &m_nv12Out);
        if (FAILED(hr)) { std::cerr << "[VideoConv] CreateTexture2D NV12(RT) failed, trying BindFlags=0\n"; }
        if (FAILED(hr)) {
            td.BindFlags = 0;
            hr = m_dev->CreateTexture2D(&td, nullptr, &m_nv12Out);
        }
        if (FAILED(hr)) { std::cerr << "[VideoConv] CreateTexture2D NV12 failed\n"; return false; }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
        ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = m_videoDevice->CreateVideoProcessorOutputView(m_nv12Out, m_vpEnum, &ovDesc, &m_outView);
        if (FAILED(hr)) { std::cerr << "[VideoConv] CreateVideoProcessorOutputView failed (hr=" << std::hex << hr << ")\n"; return false; }

        std::cout << "[VideoConv] OK: " << srcW << "x" << srcH << " BGRA -> " << dstW << "x" << dstH << " NV12 (VideoProcessorBlt)\n";
        return true;
    }

    // Converts pSrc (BGRA or any VP-supported format) to NV12 m_nv12Out via VideoProcessorBlt.
    bool Convert(ID3D11Texture2D* pSrc) {
        if (!pSrc) return false;
        CComPtr<ID3D11VideoProcessorInputView> pInputView;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
        ivDesc.FourCC = 0;
        ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(pSrc, m_vpEnum, &ivDesc, &pInputView);
        if (FAILED(hr)) {
            std::cerr << "[VideoConv] CreateVideoProcessorInputView failed: 0x" << std::hex << hr << std::dec << "\n";
            return false;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = pInputView;
        hr = m_videoCtx->VideoProcessorBlt(m_vp, m_outView, 0, 1, &stream);
        return SUCCEEDED(hr);
    }

    ID3D11Texture2D* GetNV12Texture() { return m_nv12Out; }
};

// ============================================================
//  Phase 0/1 infrastructure: telemetry helpers + async pipeline
// ============================================================

// Compute percentile from a PRE-SORTED vector (p in 0..100).
static double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p / 100.0 * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    double frac = idx - static_cast<double>(lo);
    if (lo + 1 < sorted.size())
        return sorted[lo] * (1.0 - frac) + sorted[lo + 1] * frac;
    return sorted[lo];
}

// Thread-safe bounded queue.  Blocks on push when full, on pop when empty.
template<typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t cap) : m_cap(cap) {}

    bool push(T item) {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_notFull.wait(lk, [&] { return m_q.size() < m_cap || m_closed; });
        if (m_closed) return false;
        m_q.push(std::move(item));
        m_notEmpty.notify_one();
        return true;
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_notEmpty.wait(lk, [&] { return !m_q.empty() || m_closed; });
        if (m_q.empty()) return false;
        out = std::move(m_q.front());
        m_q.pop();
        m_notFull.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    std::queue<T> m_q;
    std::mutex    m_mtx;
    std::condition_variable m_notEmpty, m_notFull;
    size_t m_cap;
    bool   m_closed = false;
};

// Small pool of NV12 GPU textures so Convert and Encode can overlap.
class NV12TexturePool {
public:
    bool Init(ID3D11Device* dev, int w, int h, int count) {
        for (int i = 0; i < count; i++) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width  = static_cast<UINT>(w);
            td.Height = static_cast<UINT>(h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_NV12;
            td.SampleDesc.Count = 1;
            td.Usage  = D3D11_USAGE_DEFAULT;
            CComPtr<ID3D11Texture2D> tex;
            HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
            if (FAILED(hr)) return false;
            m_pool.push_back(tex);
            m_free.push(tex.p);
        }
        return true;
    }

    ID3D11Texture2D* Acquire() {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait(lk, [&] { return !m_free.empty(); });
        auto* t = m_free.front();
        m_free.pop();
        return t;
    }

    void Release(ID3D11Texture2D* t) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_free.push(t);
        m_cv.notify_one();
    }

private:
    std::vector<CComPtr<ID3D11Texture2D>> m_pool;
    std::queue<ID3D11Texture2D*>          m_free;
    std::mutex                            m_mtx;
    std::condition_variable               m_cv;
};

// Data that flows through the 4-stage pipeline.
struct PipelineFrame {
    using Clock = std::chrono::high_resolution_clock;
    using TP    = Clock::time_point;

    int              index     = 0;
    ID3D11Texture2D* bgraTex   = nullptr;  // WGC tex; Release() after Convert
    ID3D11Texture2D* nv12Tex   = nullptr;  // From pool; returned after Encode
    int              mouseX    = 0;
    int              mouseY    = 0;
    bool             encodeOk  = false;
    AetherFlow::FrameDecision decision;
    int              privacyMaskAppliedCount = 0;
    bool             privacyMaskFallbackUsed = false;
    bool             panicMaskActive = false;
    std::string      privacyMaskPath = "none";

    // Bridge-hardening per-frame analyzer state (only consumed when the
    // bridge is active; trace writer gates emission on mockAnalyzerEnabled).
    bool             analyzerSubmitted = false;
    bool             analyzerContributed = false;
    double           analyzerInferenceMs = 0.0;
    int              analyzerStalenessFrames = 0;

    // Phase 4 P0.1 per-frame policy state. Strategy-A conditional emission:
    // populated only when the scene classifier is active; the trace writer
    // skips these fields otherwise so classifier-inactive runs stay
    // byte-equivalent to a Bridge Hardening baseline trace.
    std::string      sceneClass;            // canonical 5-class name (e.g. "code_text")
    float            sceneClassConfidence = 0.0f;
    std::string      policyMode;            // PolicyDecision::mode_label
    std::string      policyReason;          // PolicyDecision::reason

    // Capture-timing root-fix PD1/PD4: the REAL per-frame capture timestamp
    // (WGC Direct3D11CaptureFrame.SystemRelativeTime in 100ns units, or a QPC
    // fallback). This is DELIBERATELY separate from the telemetry-only
    // frameStart/captureEnd high_resolution_clock stamps below — those measure
    // when the loop reached/finished the frame (latency telemetry), NOT when
    // WGC presented the captured content. Only this field reflects true
    // content time and is the basis for the effective-capture-fps diagnostic.
    int64_t          captureSystemRelativeTime100ns = 0;
    bool             captureTimestampFromWgc = false;
    // Real inter-frame capture interval in ms vs the previous accepted frame
    // (this frame's captureSystemRelativeTime100ns - previous frame's),
    // computed in the producer. First accepted frame = 0.0. This is the real
    // WGC delivery cadence, NOT the synthetic frameIndex/AETHERFLOW_FPS one.
    double           captureDeltaMs = 0.0;

    TP frameStart, captureEnd, decisionEnd, maskEnd, convertEnd, encodeEnd;
};

static double MsBetween(PipelineFrame::TP end, PipelineFrame::TP start) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static void WriteFrameTraceJson(std::ofstream& out, const PipelineFrame& f, bool emitAnalyzerFields, bool emitSceneClassifierFields) {
    if (!out.is_open()) {
        return;
    }

    const auto& regions = f.decision.qualityRegions;
    const auto& masks = f.decision.privacyMasks;
    const char* source = regions.empty() ? "none" : regions.front().source.c_str();
    const char* label = regions.empty() ? "" : regions.front().debugLabel.c_str();
    const char* maskSource = masks.empty() ? "none" : masks.front().source.c_str();
    const char* maskLabel = masks.empty() ? "" : masks.front().debugLabel.c_str();
    const char* sceneType = AetherFlow::FrameSceneTypeName(f.decision.scene.type);
    const char* sceneSource = f.decision.scene.source.empty() ? "none" : f.decision.scene.source.c_str();
    const char* sceneLabel = f.decision.scene.debugLabel.empty() ? "" : f.decision.scene.debugLabel.c_str();

    out << "{"
        << "\"frameIndex\":" << f.index
        << ",\"encodeOk\":" << (f.encodeOk ? "true" : "false")
        << ",\"sceneType\":\"" << JsonEscape(sceneType) << "\""
        << ",\"sceneSource\":\"" << JsonEscape(sceneSource) << "\""
        << ",\"sceneConfidence\":" << std::fixed << std::setprecision(3) << f.decision.scene.confidence
        << ",\"sceneDebugLabel\":\"" << JsonEscape(sceneLabel) << "\""
        << ",\"roiCenterX\":" << f.mouseX
        << ",\"roiCenterY\":" << f.mouseY
        << ",\"qualityRegionCount\":" << regions.size()
        << ",\"privacyMaskCount\":" << masks.size()
        << ",\"privacyMaskAppliedCount\":" << f.privacyMaskAppliedCount
        << ",\"privacyMaskSource\":\"" << JsonEscape(maskSource) << "\""
        << ",\"privacyMaskDebugLabel\":\"" << JsonEscape(maskLabel) << "\""
        << ",\"privacyMaskPath\":\"" << JsonEscape(f.privacyMaskPath) << "\""
        << ",\"privacyMaskFallbackUsed\":" << (f.privacyMaskFallbackUsed ? "true" : "false")
        << ",\"panicMaskActive\":" << (f.panicMaskActive ? "true" : "false")
        << ",\"decisionSource\":\"" << JsonEscape(source) << "\""
        << ",\"debugLabel\":\"" << JsonEscape(label) << "\""
        << ",\"captureMs\":" << MsBetween(f.captureEnd, f.frameStart)
        << ",\"decisionMs\":" << MsBetween(f.decisionEnd, f.captureEnd)
        << ",\"maskMs\":" << MsBetween(f.maskEnd, f.decisionEnd)
        << ",\"convertMs\":" << MsBetween(f.convertEnd, f.maskEnd)
        << ",\"encodeSubmitMs\":" << MsBetween(f.encodeEnd, f.convertEnd)
        << ",\"totalMs\":" << MsBetween(f.encodeEnd, f.frameStart)
        // Capture-timing root-fix PD4: the REAL inter-frame capture interval
        // (this frame's WGC SystemRelativeTime minus the previous accepted
        // frame's), in ms. NOT the synthetic frameIndex/AETHERFLOW_FPS
        // cadence. ALWAYS emitted (measurement-only, ~zero cost, additive
        // field — schema-v3 parsers ignore unknown keys exactly like prior
        // strategy-A additions; canonical output bytes are unchanged).
        << ",\"captureDeltaMs\":" << std::fixed << std::setprecision(3) << f.captureDeltaMs;
    if (emitAnalyzerFields) {
        // Strategy A: only emit these fields when the bridge is active. Keeps
        // the analyzer_bridge_no_mock regression byte-equivalent to its
        // pre-bridge-hardening baseline.
        out << ",\"analyzerSubmitted\":" << (f.analyzerSubmitted ? "true" : "false")
            << ",\"analyzerContributed\":" << (f.analyzerContributed ? "true" : "false")
            << ",\"analyzerInferenceMs\":" << std::fixed << std::setprecision(3) << f.analyzerInferenceMs
            << ",\"analyzerStalenessFrames\":" << f.analyzerStalenessFrames;
    }
    if (emitSceneClassifierFields) {
        // Strategy A: emit only when the scene classifier is active. Keeps
        // every other run (including analyzer_bridge_no_mock) byte-equivalent
        // to its pre-P0.1 baseline.
        //
        // Note: the per-frame trace already carries `sceneConfidence` (the
        // post-merge FrameDecision.scene.confidence). The classifier-specific
        // confidence emitted here is the raw softmax probability from the
        // ONNX session, named `sceneClassConfidence` to avoid colliding with
        // the existing field. When the classifier won the FrameDecision merge
        // these will agree; when a deterministic 1.0 producer overrode it,
        // `sceneConfidence` shows 1.0 while `sceneClassConfidence` shows the
        // classifier's lower number.
        out << ",\"sceneClass\":\"" << JsonEscape(f.sceneClass) << "\""
            << ",\"sceneClassConfidence\":" << std::fixed << std::setprecision(3) << f.sceneClassConfidence
            << ",\"policyMode\":\"" << JsonEscape(f.policyMode) << "\""
            << ",\"policyReason\":\"" << JsonEscape(f.policyReason) << "\"";
    }
    out << "}\n";
}

// Graceful-stop latch for Ctrl+C / Ctrl+Break / console close. The handler
// only sets a flag; the producer loop exits at the next frame boundary and the
// normal closeout path (encoder flush, performance report, [SRT] summary) runs
// instead of the process being torn down mid-write. A second Ctrl+C falls
// through to the default handler (immediate termination) as an escape hatch.
// CTRL_CLOSE (window X) gets the same clean path best-effort — Windows grants
// roughly five seconds before force-killing the process.
// Default whitelist for common messengers. An empty effective whitelist still
// disables the producer (normalized in RunPipelineOnce), but when the caller —
// CLI env/flags or the Studio UI — supplies none, this safe default makes the
// demo path mask something.
static const std::vector<std::string> kDefaultNotificationProcessWhitelist = {
    "LINE.exe",
    "Slack.exe",
    "Discord.exe",
    // Teams has classic and packaged executable identities. The producer
    // also expands these to packaged-app identity tokens.
    "Teams.exe",
    "ms-teams.exe",
    "MSTeams.exe",
    "Telegram.exe",
    "WhatsApp.exe",
};

static std::atomic<bool> g_stopRequested{false};
static BOOL WINAPI ConsoleStopHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (!g_stopRequested.exchange(true)) {
            std::cout << "\n[STOP] stop requested - finishing in-flight frames, writing "
                         "summaries, then exiting cleanly. Press Ctrl+C again to force quit.\n";
            return TRUE;  // handled; keep running for the clean shutdown
        }
        return FALSE;  // second signal: let the default handler terminate
    default:
        return FALSE;
    }
}

#if !defined(AETHERFLOW_STUDIO_BUILD)
int main(int argc, char* argv[]) {
    // Timer resolution is raised inside RunPipelineOnce; main() only wires the
    // console Ctrl+C handler (the Studio front-end has its own Stop button).
    SetConsoleCtrlHandler(ConsoleStopHandler, TRUE);

    // Parse optional static ROI args: --roi-x <X> --roi-y <Y>
    // When both are supplied, ROI is fixed at that position (no mouse tracking).
    // Useful for benchmark reproducibility: roi_benchmark.ps1 -Static passes these.
    int staticRoiX = -1, staticRoiY = -1;
    // Cursor-tracking ROI (mouse-follow NVENC/VPL QP delta). Retired as a
    // product feature; default OFF so the default screen-share path applies no
    // ROI QP boost. Opt back in with --cursor-roi / AETHERFLOW_CURSOR_ROI=1. A
    // static ROI (--roi-x/--roi-y, used by roi_benchmark) implies cursor ROI ON
    // so the engineering benchmark still measures the QP path.
    bool cursorRoiEnabled = GetEnvBool("AETHERFLOW_CURSOR_ROI", false);
    std::vector<AetherFlow::FrameRegion> manualPrivacyMasks;
    bool startupPanicPrivacyMask = GetEnvBool("AETHERFLOW_PRIVACY_PANIC_MASK", false);
    bool rightCtrlPanicMaskHotkeyEnabled = GetEnvBool("AETHERFLOW_PRIVACY_PANIC_HOTKEY", true);
    // Live Share Guard defaults: every deterministic detector is on, with
    // blur as the visual mode. Each detector is still opt-out via its
    // --no-... CLI flag or AETHERFLOW_*=0 env override; this just makes the
    // baseline `AetherFlow.exe` invocation a working demo without flags.
    bool passwordFieldPrivacyMaskEnabled = GetEnvBool("AETHERFLOW_PASSWORD_FIELD_MASK", true);
    int passwordFieldMaskPollFrames = GetEnvInt("AETHERFLOW_PASSWORD_FIELD_MASK_POLL_FRAMES", 5);
    bool notificationMaskEnabled = GetEnvBool("AETHERFLOW_NOTIFICATION_MASK", true);
    int notificationMaskPollFrames = GetEnvInt("AETHERFLOW_NOTIFICATION_MASK_POLL_FRAMES", 5);
    // SRT live output (spec Delta A). Default OFF — the canonical smoke path
    // is byte-identical when unset. When ON, the encoded H.264 stream is
    // additionally muxed to MPEG-TS and served on a local SRT listener; the
    // deterministic mask pipeline is untouched (masks are composited BEFORE
    // encode, so the outgoing stream is pre-masked by construction).
    bool srtOutputEnabled = GetEnvBool("AETHERFLOW_SRT_OUTPUT", false);
    int srtPort = GetEnvInt("AETHERFLOW_SRT_PORT", 8888);
    int srtLatencyMs = GetEnvInt("AETHERFLOW_SRT_LATENCY_MS", 120);
    std::string srtPassphrase;
    if (const char* env = std::getenv("AETHERFLOW_SRT_PASSPHRASE"); env && *env) {
        srtPassphrase = env;
    }
    // Async analyzer bridge (P2) + mock slow analyzer wiring. Both default
    // off — the deterministic Live Share Guard path stays untouched unless
    // the user explicitly opts in via env or CLI.
    bool mockAnalyzerEnabled = GetEnvBool("AETHERFLOW_MOCK_ANALYZER", false);
    // Capture-timing root-fix PD3: opt-in timed-recording flag plumbing only.
    // Default OFF. For THIS measure-first step the flag is INERT — it changes
    // NO output bytes and does NOT gate the always-on PD1 capture-timestamp
    // propagation or the PD4 effective-fps diagnostic (those are
    // measurement-only and needed regardless). When ON it only prints a
    // one-line notice that the sidecar/mux are not yet implemented. The
    // sidecar + PTS-honoring mux are a deferred follow-up gated on what this
    // measurement shows.
    bool timedRecordingRequested = GetEnvBool("AETHERFLOW_TIMED_RECORDING", false);
    // Bridge-hardening (Phase 4 P0 prerequisite): sub-sampling interval and
    // mock analyzer nominal inference latency. Defaults match the recorded
    // plan in docs/PROJECT_STATUS.md — 1 frame interval (every-frame submit
    // for backward parity) and 200 ms simulated compute.
    int analyzerBridgeIntervalFrames = GetEnvInt("AETHERFLOW_ANALYZER_BRIDGE_INTERVAL_FRAMES", 1);
    // Track whether the bridge interval was *explicitly* set by the user
    // (env or CLI). When the scene classifier is active and the user did not
    // explicitly override this, the interval defaults to ~1 Hz so the
    // producer-thread readback cost is paid roughly once per second (D4).
    bool analyzerBridgeIntervalExplicit = false;
    if (const char* env = std::getenv("AETHERFLOW_ANALYZER_BRIDGE_INTERVAL_FRAMES"); env && *env) {
        analyzerBridgeIntervalExplicit = true;
    }
    int mockAnalyzerInferenceMs = GetEnvInt("AETHERFLOW_MOCK_ANALYZER_INFERENCE_MS", 200);

    // Phase 4 P0.1 scene classifier opt-in. Defaults off; user opts in via
    // `--scene-classifier-onnx-model=<path>` or the
    // AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL env var. Classifier-active runs
    // emit `sceneClass`/`sceneClassConfidence`/`policyMode`/`policyReason`
    // strategy-A trace fields and register the PolicyEngine module.
    std::string sceneClassifierOnnxModel;
    if (const char* env = std::getenv("AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL"); env && *env) {
        sceneClassifierOnnxModel = env;
    }
    std::string sceneClassifierProvider = "directml";
    if (const char* env = std::getenv("AETHERFLOW_SCENE_CLASSIFIER_PROVIDER"); env && *env) {
        sceneClassifierProvider = env;
    }
    // Phase 4 P0.1 §4.y visible scene demo action. Opt-in, default OFF. Only
    // does anything when the scene classifier is also active. NOT P1.1, NOT
    // product behavior — a crude visual proxy so detection is eyeball-visible
    // in the encoded output (full-screen effect chosen per detected class).
    bool sceneClassifierDemoAction =
        GetEnvBool("AETHERFLOW_SCENE_CLASSIFIER_DEMO_ACTION", false);
    std::vector<std::string> notificationProcessWhitelist;
    if (const char* envList = std::getenv("AETHERFLOW_NOTIFICATION_PROCESS_LIST"); envList && *envList) {
        std::string s(envList);
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == ',' || s[i] == ';') {
                if (i > start) {
                    auto entry = s.substr(start, i - start);
                    while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.erase(entry.begin());
                    while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) entry.pop_back();
                    if (!entry.empty()) notificationProcessWhitelist.push_back(entry);
                }
                start = i + 1;
            }
        }
    }
    AetherFlow::PrivacyMaskMode privacyMaskMode = AetherFlow::PrivacyMaskMode::Blur;
    if (const char* envMode = std::getenv("AETHERFLOW_PRIVACY_MASK_MODE"); envMode && *envMode) {
        std::string m(envMode);
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (m == "blur") privacyMaskMode = AetherFlow::PrivacyMaskMode::Blur;
        else if (m == "mosaic") privacyMaskMode = AetherFlow::PrivacyMaskMode::Mosaic;
        else if (m == "blackout") privacyMaskMode = AetherFlow::PrivacyMaskMode::Blackout;
    }
    if (const char* envMasks = std::getenv("AETHERFLOW_PRIVACY_MASKS"); envMasks && *envMasks) {
        ParsePrivacyMaskList(envMasks, "manual-privacy-mask-env", &manualPrivacyMasks);
    }
    for (int a = 1; a < argc; ++a) {
        const std::string arg(argv[a]);
        if (arg == "--roi-x" && a + 1 < argc) {
            staticRoiX = std::atoi(argv[++a]);
        } else if (arg == "--roi-y" && a + 1 < argc) {
            staticRoiY = std::atoi(argv[++a]);
        } else if (arg == "--cursor-roi" || arg == "--roi") {
            cursorRoiEnabled = true;
        } else if (arg == "--no-cursor-roi" || arg == "--no-roi") {
            cursorRoiEnabled = false;
        } else if (arg == "--privacy-mask" && a + 1 < argc) {
            AetherFlow::FrameRegion region;
            if (!ParsePrivacyMaskSpec(argv[++a], "manual-privacy-mask-cli", &region)) {
                std::cerr << "[LiveShareGuard] Invalid --privacy-mask. Expected left,top,right,bottom\n";
                return -4;
            }
            manualPrivacyMasks.push_back(region);
        } else if (arg.rfind("--privacy-mask=", 0) == 0) {
            AetherFlow::FrameRegion region;
            if (!ParsePrivacyMaskSpec(arg.substr(15), "manual-privacy-mask-cli", &region)) {
                std::cerr << "[LiveShareGuard] Invalid --privacy-mask. Expected left,top,right,bottom\n";
                return -4;
            }
            manualPrivacyMasks.push_back(region);
        } else if (arg == "--panic-mask" || arg == "--privacy-mask-fullscreen") {
            startupPanicPrivacyMask = true;
        } else if (arg == "--panic-mask-hotkey") {
            rightCtrlPanicMaskHotkeyEnabled = true;
        } else if (arg == "--no-panic-mask-hotkey" || arg == "--disable-panic-mask-hotkey") {
            rightCtrlPanicMaskHotkeyEnabled = false;
        } else if (arg == "--password-field-mask") {
            passwordFieldPrivacyMaskEnabled = true;
        } else if (arg == "--no-password-field-mask" || arg == "--disable-password-field-mask") {
            passwordFieldPrivacyMaskEnabled = false;
        } else if (arg == "--notification-mask") {
            notificationMaskEnabled = true;
        } else if (arg == "--no-notification-mask" || arg == "--disable-notification-mask") {
            notificationMaskEnabled = false;
        } else if (arg == "--mock-analyzer") {
            mockAnalyzerEnabled = true;
        } else if (arg == "--no-mock-analyzer") {
            mockAnalyzerEnabled = false;
        } else if (arg == "--record-timed") {
            // PD3 plumbing only (measure-first step): parsed without error,
            // no behavior change. The PD1/PD4 measurement is always active
            // and independent of this flag.
            timedRecordingRequested = true;
        } else if (arg == "--no-record-timed") {
            timedRecordingRequested = false;
        } else if (arg.rfind("--analyzer-bridge-interval-frames=", 0) == 0) {
            const int parsed = std::atoi(arg.substr(34).c_str());
            if (parsed > 0) {
                analyzerBridgeIntervalFrames = parsed;
                analyzerBridgeIntervalExplicit = true;
            } else {
                std::cerr << "[LiveShareGuard] Invalid --analyzer-bridge-interval-frames; expected positive int\n";
                return -4;
            }
        } else if (arg.rfind("--scene-classifier-onnx-model=", 0) == 0) {
            sceneClassifierOnnxModel = arg.substr(30);
        } else if (arg.rfind("--scene-classifier-provider=", 0) == 0) {
            sceneClassifierProvider = arg.substr(28);
        } else if (arg == "--scene-classifier-demo-action") {
            sceneClassifierDemoAction = true;
        } else if (arg == "--no-scene-classifier-demo-action") {
            sceneClassifierDemoAction = false;
        } else if (arg == "--srt-output") {
            srtOutputEnabled = true;
        } else if (arg == "--no-srt-output") {
            srtOutputEnabled = false;
        } else if (arg.rfind("--srt-port=", 0) == 0) {
            const int parsed = std::atoi(arg.substr(11).c_str());
            if (parsed >= 1 && parsed <= 65535) {
                srtPort = parsed;
                srtOutputEnabled = true;  // setting a port implies streaming on
            } else {
                std::cerr << "[SRT] Invalid --srt-port; expected 1-65535\n";
                return -4;
            }
        } else if (arg.rfind("--srt-latency-ms=", 0) == 0) {
            const int parsed = std::atoi(arg.substr(17).c_str());
            if (parsed >= 0) {
                srtLatencyMs = parsed;
            } else {
                std::cerr << "[SRT] Invalid --srt-latency-ms; expected >= 0\n";
                return -4;
            }
        } else if (arg.rfind("--srt-passphrase=", 0) == 0) {
            srtPassphrase = arg.substr(17);
        } else if (arg.rfind("--notification-mask-process=", 0) == 0) {
            std::string v = arg.substr(28);
            if (!v.empty()) notificationProcessWhitelist.push_back(v);
        } else if (arg.rfind("--privacy-mask-mode=", 0) == 0) {
            std::string m = arg.substr(20);
            std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (m == "blur") privacyMaskMode = AetherFlow::PrivacyMaskMode::Blur;
            else if (m == "mosaic") privacyMaskMode = AetherFlow::PrivacyMaskMode::Mosaic;
            else if (m == "blackout") privacyMaskMode = AetherFlow::PrivacyMaskMode::Blackout;
            else {
                std::cerr << "[LiveShareGuard] Invalid --privacy-mask-mode. Expected blackout|blur|mosaic\n";
                return -4;
            }
        }
    }
    passwordFieldMaskPollFrames = (std::max)(1, passwordFieldMaskPollFrames);
    notificationMaskPollFrames = (std::max)(1, notificationMaskPollFrames);
    analyzerBridgeIntervalFrames = (std::max)(1, analyzerBridgeIntervalFrames);
    mockAnalyzerInferenceMs = (std::max)(1, mockAnalyzerInferenceMs);
    // Env-supplied SRT values get the same bounds as the CLI-validated ones.
    srtPort = (std::min)(65535, (std::max)(1, srtPort));
    srtLatencyMs = (std::max)(0, srtLatencyMs);
    // libsrt rejects passphrases outside 10-79 chars at connect time, which
    // would otherwise surface as an endless listener open-fail/retry loop.
    // Refuse up front instead of silently altering a security value. Only
    // checked when streaming is enabled so a stray AETHERFLOW_SRT_PASSPHRASE
    // env var cannot fail unrelated (non-SRT) runs.
    if (srtOutputEnabled && !srtPassphrase.empty() &&
        (srtPassphrase.size() < 10 || srtPassphrase.size() > 79)) {
        std::cerr << "[SRT] Invalid SRT passphrase length (" << srtPassphrase.size()
                  << "); libsrt requires 10-79 characters, or empty for no encryption\n";
        return -4;
    }
    // Hand everything to the shared pipeline entry (also used by
    // AetherFlowStudio). Whitelist-default / panic-clears-manual normalization
    // happens inside RunPipelineOnce so both front-ends get it.
    AetherFlow::PipelineOptions pipelineOptions;
    pipelineOptions.cursorRoiEnabled = cursorRoiEnabled;
    pipelineOptions.staticRoiX = staticRoiX;
    pipelineOptions.staticRoiY = staticRoiY;
    pipelineOptions.startupPanicMask = startupPanicPrivacyMask;
    pipelineOptions.rightCtrlPanicHotkeyEnabled = rightCtrlPanicMaskHotkeyEnabled;
    pipelineOptions.passwordFieldMaskEnabled = passwordFieldPrivacyMaskEnabled;
    pipelineOptions.passwordFieldMaskPollFrames = passwordFieldMaskPollFrames;
    pipelineOptions.notificationMaskEnabled = notificationMaskEnabled;
    pipelineOptions.notificationMaskPollFrames = notificationMaskPollFrames;
    pipelineOptions.notificationProcessWhitelist = notificationProcessWhitelist;
    pipelineOptions.manualPrivacyMasks = manualPrivacyMasks;
    pipelineOptions.privacyMaskMode = privacyMaskMode;
    pipelineOptions.mockAnalyzerEnabled = mockAnalyzerEnabled;
    pipelineOptions.mockAnalyzerInferenceMs = mockAnalyzerInferenceMs;
    pipelineOptions.analyzerBridgeIntervalFrames = analyzerBridgeIntervalFrames;
    pipelineOptions.analyzerBridgeIntervalExplicit = analyzerBridgeIntervalExplicit;
    pipelineOptions.timedRecording = timedRecordingRequested;
    pipelineOptions.sceneClassifierOnnxModel = sceneClassifierOnnxModel;
    pipelineOptions.sceneClassifierProvider = sceneClassifierProvider;
    pipelineOptions.sceneClassifierDemoAction = sceneClassifierDemoAction;
    pipelineOptions.srt.enabled = srtOutputEnabled;
    pipelineOptions.srt.port = srtPort;
    pipelineOptions.srt.latencyMs = srtLatencyMs;
    pipelineOptions.srt.passphrase = srtPassphrase;
    return AetherFlow::RunPipelineOnce(pipelineOptions, nullptr);
}
#endif  // !AETHERFLOW_STUDIO_BUILD

namespace AetherFlow {

int RunPipelineOnce(const PipelineOptions& opt, PipelineStatus* status) {
    // Sleep(1) ≈ 1ms for the whole session (same rationale as the old main()).
    timeBeginPeriod(1);
    struct TimerPeriodGuard { ~TimerPeriodGuard() { timeEndPeriod(1); } } _timerGuard;
    struct StatusRunGuard {
        PipelineStatus* s;
        explicit StatusRunGuard(PipelineStatus* st) : s(st) {
            if (s) s->running.store(true, std::memory_order_relaxed);
        }
        ~StatusRunGuard() {
            if (s) s->running.store(false, std::memory_order_relaxed);
        }
    } _runGuard(status);

    // ── Option normalization shared by both front-ends ──────────────────────
    std::vector<std::string> notificationWhitelistStorage = opt.notificationProcessWhitelist;
    bool notificationMaskEnabled = opt.notificationMaskEnabled;
    if (notificationMaskEnabled && notificationWhitelistStorage.empty()) {
        // No whitelist supplied -- fall back to the bundled default so the
        // demo path masks common messengers automatically.
        notificationWhitelistStorage = kDefaultNotificationProcessWhitelist;
    }
    if (notificationWhitelistStorage.empty()) {
        notificationMaskEnabled = false;
    }
    std::vector<AetherFlow::FrameRegion> manualMasksStorage = opt.manualPrivacyMasks;
    if (opt.startupPanicMask) {
        manualMasksStorage.clear();  // panic supersedes manual regions (historical)
    }

    // ── Alias block ──────────────────────────────────────────────────────────
    // The pipeline body below is main()'s former body; these aliases keep that
    // large block textually unchanged. Encode geometry falls back to the
    // compile-time Config.h constants so the canonical CLI run is identical.
    const bool cursorRoiEnabled = opt.cursorRoiEnabled;
    const int staticRoiX = opt.staticRoiX;
    const int staticRoiY = opt.staticRoiY;
    const std::vector<AetherFlow::FrameRegion>& manualPrivacyMasks = manualMasksStorage;
    const bool startupPanicPrivacyMask = opt.startupPanicMask;
    const bool rightCtrlPanicMaskHotkeyEnabled = opt.rightCtrlPanicHotkeyEnabled;
    const bool passwordFieldPrivacyMaskEnabled = opt.passwordFieldMaskEnabled;
    const int passwordFieldMaskPollFrames = (std::max)(1, opt.passwordFieldMaskPollFrames);
    const int notificationMaskPollFrames = (std::max)(1, opt.notificationMaskPollFrames);
    const std::vector<std::string>& notificationProcessWhitelist = notificationWhitelistStorage;
    const bool mockAnalyzerEnabled = opt.mockAnalyzerEnabled;
    const int mockAnalyzerInferenceMs = (std::max)(1, opt.mockAnalyzerInferenceMs);
    int analyzerBridgeIntervalFrames = (std::max)(1, opt.analyzerBridgeIntervalFrames);
    const bool analyzerBridgeIntervalExplicit = opt.analyzerBridgeIntervalExplicit;
    const bool timedRecordingRequested = opt.timedRecording;
    const std::string& sceneClassifierOnnxModel = opt.sceneClassifierOnnxModel;
    const std::string& sceneClassifierProvider = opt.sceneClassifierProvider;
    const bool sceneClassifierDemoAction = opt.sceneClassifierDemoAction;
    const AetherFlow::PrivacyMaskMode privacyMaskMode = opt.privacyMaskMode;
    const bool srtOutputEnabled = opt.srt.enabled;
    const int srtPort = opt.srt.port;
    const int srtLatencyMs = opt.srt.latencyMs;
    const std::string& srtPassphrase = opt.srt.passphrase;
    const int encWidth = (opt.width > 0) ? opt.width : AETHERFLOW_WIDTH;
    const int encHeight = (opt.height > 0) ? opt.height : AETHERFLOW_HEIGHT;
    const int encFps = (opt.fps > 0) ? opt.fps : AETHERFLOW_FPS;
    const int bitrateKbpsEffective = (opt.bitrateKbps > 0) ? opt.bitrateKbps : AETHERFLOW_BITRATE;

    const bool useStaticRoi = (staticRoiX >= 0 && staticRoiY >= 0);
    // Cursor ROI is opt-in (default off). A static ROI position (benchmark)
    // forces it on so roi_benchmark still exercises the QP path.
    const bool cursorRoiActive = cursorRoiEnabled || useStaticRoi;
    const bool privacyMaskFastPathEnabled =
        startupPanicPrivacyMask ||
        rightCtrlPanicMaskHotkeyEnabled ||
        passwordFieldPrivacyMaskEnabled ||
        notificationMaskEnabled ||
        !manualPrivacyMasks.empty();

    std::cout << "--- AetherFlow Performance Benchmark Start ---\n";
    std::cout << "[Config] " << encWidth << "x" << encHeight
              << " @ " << encFps << " fps, "
              << bitrateKbpsEffective << " kbps"
              << ", monitor " << opt.monitorIndex << "\n";
    if (timedRecordingRequested) {
        // PD2a/PD3: real per-frame capture timestamps are propagated to the
        // encoder, which emits a mkvmerge-v2 PTS sidecar next to
        // output_encoded.h264. run_scene_test.sh muxes honoring it. The
        // canonical verify harness does not set this, so its bitstream stays
        // byte-stable. The PD1/PD4 capture diagnostic runs regardless.
        std::cout << "[CAPTURE] timed recording ON — encoder emits PTS sidecar; "
                     "demo mux honors real capture timing\n";
    }

    const bool hasIntel = HasIntelAdapter();
    const bool hasNvidia = HasNvidiaAdapter();
    const bool hasNvencRuntime = hasNvidia && HasNvencRuntime();
    std::cout << "[ENV] Intel GPU detected: " << (hasIntel ? "YES" : "NO") << "\n";
    std::cout << "[ENV] NVIDIA GPU detected: " << (hasNvidia ? "YES" : "NO") << "\n";
    std::cout << "[ENV] NVENC runtime detected: " << (hasNvencRuntime ? "YES" : "NO") << "\n";

#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
    // Declared BEFORE `enc` so the sink outlives the encoder: the encoder's
    // drain/cleanup path may still push access units while it is destroyed,
    // and locals are destroyed in reverse declaration order.
    std::unique_ptr<AetherFlow::SrtStreamOutput> srtOutput;
#endif

    // Backend selection: Auto keeps the historical NVENC-first order; an
    // explicit preference (Studio dropdown) narrows the candidates instead of
    // silently falling back to a backend the user did not pick.
    const bool allowNvenc = (opt.encoder != EncoderPreference::OneVpl);
    const bool allowVpl = (opt.encoder != EncoderPreference::Nvenc);
    std::unique_ptr<IH264Encoder> enc;
#if defined(AETHERFLOW_ENABLE_NVENC)
    if (allowNvenc && hasNvencRuntime) {
        std::cout << "[Step 1] Initializing NVENC backend...\n";
        enc = std::make_unique<NvencH264Wrapper>();
        if (opt.bitrateKbps > 0) enc->SetTargetBitrateKbps(opt.bitrateKbps);
        if (!enc->Initialize(encWidth, encHeight, encFps)) {
            enc.reset();
            std::cout << "[ENV] NVENC init failed. ";
            if (allowVpl && hasIntel) {
                std::cout << "Falling back to Intel oneVPL." << std::endl;
            } else {
                std::cout << "Exiting as unsupported." << std::endl;
                return -3;
            }
        }
    }
#endif

    if (!enc && allowVpl && hasIntel) {
        std::cout << "[Step 1] Initializing oneVPL...\n";
        enc = std::make_unique<VplH264Wrapper>();
        if (opt.bitrateKbps > 0) enc->SetTargetBitrateKbps(opt.bitrateKbps);
        if (!enc->Initialize(encWidth, encHeight, encFps)) {
            std::cerr << "VPL Init Failed\n";
            return -1;
        }
    }

    if (!enc) {
        if (opt.encoder == EncoderPreference::Nvenc) {
            std::cout << "[ENV] NVENC was requested but is unavailable on this machine." << std::endl;
        } else if (opt.encoder == EncoderPreference::OneVpl) {
            std::cout << "[ENV] oneVPL was requested but no Intel adapter was found." << std::endl;
        }
#if defined(AETHERFLOW_ENABLE_NVENC)
        std::cout << "[ENV] No usable encoder backend found. Exiting as unsupported." << std::endl;
#else
        std::cout << "[ENV] This build currently supports Intel Quick Sync (oneVPL) only." << std::endl;
        std::cout << "[ENV] No Intel adapter found. Exiting as unsupported." << std::endl;
#endif
        return -2;
    }

#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
    if (srtOutputEnabled) {
        AetherFlow::SrtStreamOutput::Options srtOptions;
        srtOptions.port = srtPort;
        srtOptions.latencyMs = srtLatencyMs;
        srtOptions.passphrase = srtPassphrase;
        srtOptions.fps = encFps;
        srtOptions.width = encWidth;
        srtOptions.height = encHeight;
        srtOutput = std::make_unique<AetherFlow::SrtStreamOutput>(srtOptions);
        if (srtOutput->Start()) {
            enc->SetEncodedFrameSink(srtOutput.get());
            std::cout << "[SRT] live output enabled: port " << srtPort
                      << ", latency " << srtLatencyMs << " ms, "
                      << (srtPassphrase.empty() ? "no passphrase" : "passphrase set (AES-128)")
                      << ", video-only, single viewer\n";
            AetherFlow::SrtStreamOutput::PrintShareUrls(srtPort);
        } else {
            std::cerr << "[SRT] listener start failed; continuing without SRT output.\n";
            srtOutput.reset();
        }
    }
#else
    if (srtOutputEnabled) {
        std::cerr << "[SRT] --srt-output requested but this build was compiled without "
                     "AETHERFLOW_ENABLE_SRT_OUTPUT. Run `python tools/fetch_ffmpeg.py` to vendor "
                     "the FFmpeg SDK (see third_party/ffmpeg/SOURCE.md), reconfigure CMake, and rebuild.\n";
    }
#endif

    // unique_ptr: RunPipelineOnce is called repeatedly from Studio, and the
    // mid-init early returns must not leak a live WGC session per failed Start.
    std::unique_ptr<ScreenCapture> capOwner = std::make_unique<ScreenCapture>();
    ScreenCapture* cap = capOwner.get();
    std::cout << "[Step 2] Initializing Screen Capture (WGC)...\n";
    if (!cap->Init(encWidth, encHeight, enc->GetDevice(), opt.monitorIndex)) {
        std::cerr << "Capture Init Failed\n";
        return -1;
    }

    std::cout << "[Step 3] Setting up D3D11 Video Converter (VideoProcessorBlt BGRA→NV12)...\n";
    D3D11VideoConverter conv(enc->GetDevice(), enc->GetContext());
    if (!conv.Initialize(cap->GetCaptureWidth(), cap->GetCaptureHeight(),
                         encWidth, encHeight)) {
        std::cerr << "[ERROR] D3D11VideoConverter init failed\n";
        return -1;
    }
    std::cout << "[OK] Color converter ready (GPU VideoProcessorBlt pipeline)\n";

    // WGC ?�身：�?待第一幀準�?就�?
    std::cout << "[Step 3.5] Waiting for WGC to warm up...\n";
    ID3D11Texture2D* testTex = nullptr;
    for (int retry = 0; retry < 100 && !testTex; retry++) {
        testTex = cap->CaptureTexture();
        if (!testTex) Sleep(50); // 等�? 50ms
    }
    if (testTex) {
        std::cout << "[WGC] First frame captured successfully!\n";
        testTex->Release();  // ?�放測試幀
        Sleep(100);  // �?WGC ?��??��?下�?幀
    } else {
        std::cerr << "[WGC ERROR] Failed to capture first frame after warmup\n";
        return -1;
    }

    // ============================================================
    // ============================================================
    // ROI (Region of Interest)
    // ============================================================
    // Cursor-tracking ROI is opt-in (default off). When off, the encoder
    // applies no QP delta and CursorFocusModule is not registered below.
    // Radius/DeltaQP are compile-time defaults from NvencRoiDefaults.h.
    const int roiRadiusRuntime = AetherFlow::kNvencRoiDefaultRadius;
    const int roiDeltaQpRuntime = AetherFlow::kNvencRoiDefaultDeltaQp;
    enc->SetRoiEnabled(cursorRoiActive);
    if (!cursorRoiActive) {
        std::cout << "[ROI] Cursor ROI disabled (default); no QP delta applied. "
                     "Enable with --cursor-roi (or --roi-x/--roi-y for static).\n";
    } else {
        if (useStaticRoi) {
            std::cout << "[Step 3.6] ROI Mode: STATIC at (" << staticRoiX << "," << staticRoiY << ")\n";
        } else {
            std::cout << "[Step 3.6] ROI Mode: Mouse tracking\n";
        }
        // kNvencRoiDefaultDeltaQp = AETHERFLOW_ROI_DELTA_QP (from Config.h)
        std::cout << "[ROI] Radius: " << roiRadiusRuntime << "px | DeltaQP: "
                  << roiDeltaQpRuntime << " (High quality around mouse)\n";
        std::cout << "[ROI] Capture space: " << cap->GetCaptureWidth() << "x" << cap->GetCaptureHeight()
                  << " at (" << cap->GetCaptureLeft() << "," << cap->GetCaptureTop() << ")"
                  << " -> Encode space: " << encWidth << "x" << encHeight << "\n";
    }

    AetherFlow::PanicPrivacyMaskModule panicPrivacyMask;
    AetherFlow::ManualPrivacyMaskModule manualPrivacyMask(manualPrivacyMasks);
    AetherFlow::PasswordFieldPrivacyMaskModule passwordFieldPrivacyMask(passwordFieldMaskPollFrames);
    AetherFlow::NotificationProducerModule notificationProducer(notificationProcessWhitelist, notificationMaskPollFrames);
    AetherFlow::BaselineSceneModule baselineScene;
    AetherFlow::CursorFocusModule cursorFocus(roiRadiusRuntime);

    // Phase 4 P0.1 scene classifier. Constructed lazily so the
    // classifier-inactive path is byte-equivalent to the pre-P0.1 binary
    // (no ORT session created, no DirectML device queried). The classifier
    // shares the same IAIFrameAnalyzer seam as MockSlowAnalyzer.
    bool sceneClassifierActive = false;
    std::string sceneClassifierProviderActual = "n/a";
#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
    std::unique_ptr<AetherFlow::SceneClassifierOnnx> sceneClassifierOnnx;
    if (!sceneClassifierOnnxModel.empty()) {
        // Choose provider preference.
        std::string p = sceneClassifierProvider;
        std::transform(p.begin(), p.end(), p.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto providerPref = AetherFlow::SceneClassifierOnnx::Provider::DirectML;
        if (p == "cpu") {
            providerPref = AetherFlow::SceneClassifierOnnx::Provider::CPU;
        } else if (p != "directml" && !p.empty()) {
            std::cerr << "[SceneClassifier] Unknown --scene-classifier-provider='" << p
                      << "'; defaulting to directml.\n";
        }
        sceneClassifierOnnx = std::make_unique<AetherFlow::SceneClassifierOnnx>(
            sceneClassifierOnnxModel, providerPref);
        if (sceneClassifierOnnx->Initialize(enc->GetDevice(), encWidth, encHeight)) {
            sceneClassifierActive = true;
            sceneClassifierProviderActual = sceneClassifierOnnx->ProviderName();
            if (status) {
                status->sceneClassifierState.store(
                    sceneClassifierProviderActual == "CPU" ? 2 : 1,
                    std::memory_order_relaxed);
            }
        } else {
            std::cerr << "[SceneClassifier] Initialize failed; classifier path disabled this run.\n";
            sceneClassifierOnnx.reset();
            if (status) {
                status->sceneClassifierState.store(3, std::memory_order_relaxed);
            }
        }
    }
#else
    if (!sceneClassifierOnnxModel.empty()) {
        std::cerr << "[SceneClassifier] --scene-classifier-onnx-model='" << sceneClassifierOnnxModel
                  << "' requested but this build was compiled without AETHERFLOW_ENABLE_SCENE_CLASSIFIER. "
                  << "See third_party/onnxruntime/SOURCE.md to vendor ONNX Runtime.\n";
        if (status) {
            status->sceneClassifierState.store(3, std::memory_order_relaxed);
        }
    }
#endif

    // Async analyzer bridge (P2). The bridge is only registered when an
    // analyzer impl is wired in; today the mock-analyzer CLI/env flag is the
    // only producer. Bridge runs *after* deterministic producers so its
    // confidence-based scene merge competes against an already-populated
    // baseline / 1.0 deterministic claim.
    AetherFlow::MockSlowAnalyzer mockSlowAnalyzer(mockAnalyzerInferenceMs);
    AetherFlow::IAIFrameAnalyzer* activeAnalyzer = nullptr;
    if (mockAnalyzerEnabled) {
        activeAnalyzer = &mockSlowAnalyzer;
    }
#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
    if (sceneClassifierActive) {
        // Classifier wins over the mock when both are requested: the mock is
        // only a wiring stand-in. Document this in the startup banner.
        activeAnalyzer = sceneClassifierOnnx.get();
        if (mockAnalyzerEnabled) {
            std::cout << "[SceneClassifier] Both --mock-analyzer and --scene-classifier-onnx-model "
                      << "set; using classifier (mock disabled this run).\n";
        }
    }
#endif

    // D4 default sub-sample wiring: when the scene classifier is the active
    // analyzer and the user did NOT explicitly set the bridge interval, default
    // to ~1 Hz (every encFps frames). The producer-thread BGRA readback
    // in SceneClassifierOnnx::SubmitFrame is ~8 MB CopyResource+Map; paying it
    // once per second keeps the fast path off the 33 ms budget. An explicit
    // CLI/env override always wins (do not stomp the user's choice).
#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
    if (sceneClassifierActive && !analyzerBridgeIntervalExplicit) {
        analyzerBridgeIntervalFrames = (std::max)(1, static_cast<int>(encFps));
        std::cout << "[SceneClassifier] No --analyzer-bridge-interval-frames override; "
                  << "defaulting bridge submit interval to " << analyzerBridgeIntervalFrames
                  << " frames (~1 Hz readback at " << encFps << " fps).\n";
    }
#endif

    // §4.y DA3/DA4: the visible scene demo action only does anything when the
    // classifier is also active. Resolve the effective enable state here so
    // the module is only ever registered when (flag ON && classifier active).
    // OFF (or classifier inactive) => module never registered => byte-equiv.
    const bool sceneDemoActionEffective =
        sceneClassifierDemoAction && sceneClassifierActive;
    if (sceneClassifierDemoAction && !sceneClassifierActive) {
        // Asked for but inert: warn and continue (do not error).
        std::cout << "[DEMO] --scene-classifier-demo-action requested but the scene "
                     "classifier is not active (no --scene-classifier-onnx-model); "
                     "demo action is a no-op this run.\n";
    }
    if (sceneDemoActionEffective) {
        std::cout << "[DEMO] --scene-classifier-demo-action active: full-screen mask "
                     "mode chosen per detected scene class; NOT P1.1 policy, NOT "
                     "product behavior (opt-in visual proxy only).\n";
    }

    AetherFlow::AsyncAnalyzerBridgeModule asyncAnalyzerBridge(
        activeAnalyzer,
        analyzerBridgeIntervalFrames,
        /*dropWhenBusy=*/true);

    // Phase 4 P0.1 policy engine. Cross-platform pure C++; lives on the
    // producer thread; advisory (mask_mode / encode_hint trace-only) for
    // P0.1. Registered after the scene-merging modules + bridge so it sees
    // the post-merge FrameDecision.scene.
    AetherFlow::PolicyEngine policyEngine;
    const bool policyEngineActive = sceneClassifierActive;

    // §4.y DA1: visible scene demo action module. Constructed with the
    // effective enable flag; registered AFTER policyEngine so it can read the
    // hysteresis-smoothed FrameDecision.policyStableClass that PolicyEngine
    // stamps. Internally no-ops when disabled, when a deterministic privacy
    // mask already exists, or when the smoothed class maps to passthrough
    // (mixed_ui / unknown / pre-warmup "").
    AetherFlow::SceneDemoActionModule sceneDemoAction(sceneDemoActionEffective);

    AetherFlow::FramePolicyEngine framePolicy;
    if (startupPanicPrivacyMask || rightCtrlPanicMaskHotkeyEnabled) {
        framePolicy.AddModule(&panicPrivacyMask);
    }
    if (!manualPrivacyMasks.empty()) {
        framePolicy.AddModule(&manualPrivacyMask);
    }
    if (passwordFieldPrivacyMaskEnabled) {
        framePolicy.AddModule(&passwordFieldPrivacyMask);
    }
    if (notificationMaskEnabled) {
        framePolicy.AddModule(&notificationProducer);
    }
    framePolicy.AddModule(&baselineScene);
    if (cursorRoiActive) {
        framePolicy.AddModule(&cursorFocus);
    }
    const bool analyzerBridgeActive = (activeAnalyzer != nullptr);
    if (analyzerBridgeActive) {
        framePolicy.AddModule(&asyncAnalyzerBridge);
    }
    // PolicyEngine runs last on the producer thread so it sees the
    // post-merge scene (deterministic 1.0 producers, baseline, or
    // classifier-via-bridge).
    if (policyEngineActive) {
        framePolicy.AddModule(&policyEngine);
    }
    // §4.y DA1: demo action registers LAST — after policyEngine — so it sees
    // the final merged scene. Only registered when (flag ON && classifier
    // active); OFF => not in the module chain => byte-equivalent to HEAD.
    if (sceneDemoActionEffective) {
        framePolicy.AddModule(&sceneDemoAction);
    }
    if (!framePolicy.Initialize(enc->GetDevice(), encWidth, encHeight)) {
        std::cerr << "[Runtime] Frame policy init failed\n";
        return -1;
    }
    std::cout << "[Runtime] Deterministic layer: "
              << ((startupPanicPrivacyMask || rightCtrlPanicMaskHotkeyEnabled) ? "PanicPrivacyMaskModule + " : "")
              << (manualPrivacyMasks.empty() ? "" : "ManualPrivacyMaskModule + ")
              << (passwordFieldPrivacyMaskEnabled ? "PasswordFieldPrivacyMaskModule + " : "")
              << (notificationMaskEnabled ? "NotificationProducerModule + " : "")
              << "BaselineSceneModule" << (cursorRoiActive ? " + CursorFocusModule" : "") << " enabled\n";
    if (analyzerBridgeActive) {
        std::cout << "[Runtime] AsyncAnalyzerBridgeModule active: "
                  << "submit-every-n-frames=" << analyzerBridgeIntervalFrames
                  << ", drop-when-busy=on";
        if (sceneClassifierActive) {
            std::cout << ", analyzer=scene-classifier-onnx"
                      << ", provider=" << sceneClassifierProviderActual
                      << ", model=" << sceneClassifierOnnxModel;
        } else {
            std::cout << ", analyzer=mock-slow-analyzer"
                      << ", mock-inference-ms=" << mockAnalyzerInferenceMs;
        }
        std::cout << "\n";
    }
    if (policyEngineActive) {
        std::cout << "[Runtime] PolicyEngine active (advisory; mask/encode wiring is P1.1): "
                  << "min-frames-between-switches=" << AetherFlow::PolicyEngine::kMinFramesBetweenSwitches
                  << ", consecutive-same-class-to-switch=" << AetherFlow::PolicyEngine::kConsecutiveSameClassToSwitch
                  << ", low-confidence-threshold=" << AetherFlow::PolicyEngine::kLowConfidenceThreshold
                  << "\n";
    }

    AetherFlow::PrivacyMaskCompositor privacyMaskCompositor;
    if (privacyMaskFastPathEnabled) {
        if (!privacyMaskCompositor.Initialize(
                enc->GetDevice(),
                cap->GetCaptureWidth(),
                cap->GetCaptureHeight(),
                encWidth,
                encHeight)) {
            std::cerr << "[LiveShareGuard] Failed to initialize privacy mask compositor\n";
            return -1;
        }
        std::cout << "[LiveShareGuard] Privacy mask fast path enabled: manual="
                  << manualPrivacyMasks.size()
                  << " region(s), startup-panic="
                  << (startupPanicPrivacyMask ? "on" : "off")
                  << ", right-ctrl-hotkey="
                  << (rightCtrlPanicMaskHotkeyEnabled ? "on" : "off")
                  << ", password-field="
                  << (passwordFieldPrivacyMaskEnabled ? "on" : "off")
                  << ", password-field-poll-frames="
                  << passwordFieldMaskPollFrames
                  << ", notification="
                  << (notificationMaskEnabled ? "on" : "off")
                  << ", notification-process-list-size="
                  << notificationProcessWhitelist.size()
                  << ", mode="
                  << (privacyMaskMode == AetherFlow::PrivacyMaskMode::Mosaic ? "mosaic"
                      : privacyMaskMode == AetherFlow::PrivacyMaskMode::Blur ? "blur"
                      : privacyMaskMode == AetherFlow::PrivacyMaskMode::Grayscale ? "grayscale"
                      : "blackout")
                  << ", AI detector disabled\n";
    } else {
        std::cout << "[LiveShareGuard] Privacy mask fast path disabled; AI detector disabled\n";
    }

    // Enable D3D11 multithread protection for pipeline stages
    {
        CComPtr<ID3D11Multithread> d3dMT;
        enc->GetDevice()->QueryInterface(__uuidof(ID3D11Multithread), (void**)&d3dMT);
        if (d3dMT) d3dMT->SetMultithreadProtected(TRUE);
    }

    // Run length: an explicit Options value (Studio) wins; otherwise honor the
    // AETHERFLOW_MAX_FRAMES env override (used by run_full_test.sh
    // --seconds/--minutes/--frames), defaulting to the Config.h value.
    // 0 = no frame limit: run until stopped (Ctrl+C / console close / the
    // Studio Stop button via opt.externalStop). This is the intended mode for
    // long SRT streaming sessions.
    const int maxFrames = (opt.maxFrames >= 0)
        ? opt.maxFrames
        : GetEnvInt("AETHERFLOW_MAX_FRAMES",
                    AETHERFLOW_MAX_FRAMES > 0 ? AETHERFLOW_MAX_FRAMES : 500);
    const bool runUntilStopped = maxFrames <= 0;
    // Stats retention cap for unlimited runs: the run keeps encoding/streaming
    // past this, but the end-of-run percentile report covers the first ~55 min
    // at 30 fps so memory stays bounded.
    const size_t kMaxCompletedFramesRetained = 100000;
    const double targetFrameTime = 1000.0 / encFps;
    const int liveLogInterval = GetEnvInt("AETHERFLOW_LIVE_LOG_INTERVAL", 60);
    const bool encoderOwnsInputTextures = enc->HasInputTexturePool();

    // ── GPU texture pool: producer copies NV12 here so consumer can encode
    //    while producer captures the next frame (decouples Convert↔Encode stages).
    NV12TexturePool pool;
    if (!encoderOwnsInputTextures && !pool.Init(enc->GetDevice(), encWidth, encHeight, 4)) {
        std::cerr << "[ERROR] NV12TexturePool init failed\n";
        return -1;
    }
    std::cout << "[Pipeline] Input texture ownership: "
              << (encoderOwnsInputTextures ? "encoder registered pool" : "pipeline pool") << "\n";

    const auto traceDir = GetOutputDir() / "traces";
    std::error_code traceEc;
    std::filesystem::create_directories(traceDir, traceEc);
    const auto tracePath = traceDir / "frame_trace.jsonl";
    std::ofstream frameTrace(tracePath, std::ios::out | std::ios::trunc);
    if (frameTrace.is_open()) {
        std::cout << "[Trace] Frame trace: " << tracePath.string() << "\n";
    } else {
        std::cerr << "[Trace] Failed to open frame trace: " << tracePath.string() << "\n";
    }

    // ── Dual-threaded producer-consumer pipeline ──────────────────────────────
    //
    //  Thread 1 (producer): frame pacing + Capture + Convert + CopyResource
    //  Thread 2 (consumer): EncodeFromYUVWithROI + pool.Release
    //
    //  BoundedQueue depth=4 matches the encoder async depth so the GPU encode
    //  pipeline stays full without unbounded queueing.
    //
    //  D3D11 immediate context is shared between threads, protected by
    //  SetMultithreadProtected(TRUE) (set above).  An explicit cross-engine
    //  GPU fence in VplH264Wrapper::MergeYUVtoNV12_GPU() ensures the
    //  CopyResource from the pool into the VPL surface completes before
    //  EncodeFrameAsync reads it — mirrors what nvEncMapInputResource() does
    //  internally for the NVENC path.
    BoundedQueue<PipelineFrame> q(4);

    std::vector<PipelineFrame> completedFrames;
    completedFrames.reserve(runUntilStopped ? 4096 : maxFrames);
    std::atomic<int> captureFailCount{0};
    // Uncapped count of frames that completed the pipeline (encodeOk). The
    // per-frame stats vector below is retention-capped for unlimited runs, so
    // report denominators must NOT derive from its size (code-review risk 3).
    std::atomic<uint64_t> deliveredFrames{0};
    std::atomic<bool> privacyMaskFatal{false};
    RightCtrlPanicMaskHotkey rightCtrlPanicMaskHotkey(std::chrono::milliseconds(1000));

    if (runUntilStopped) {
        std::cout << "[Step 4] Starting dual-threaded producer-consumer pipeline "
                     "(no frame limit - press Ctrl+C to stop cleanly)...\n";
    } else {
        std::cout << "[Step 4] Starting dual-threaded producer-consumer pipeline ("
                  << maxFrames << " frames)...\n";
    }
    std::cout << "[Performance] Target: " << encFps << "fps = "
              << std::fixed << std::setprecision(2) << targetFrameTime << "ms per frame\n";
    std::cout << "========================================\n";

    auto benchStart = std::chrono::high_resolution_clock::now();
    auto benchEnd   = benchStart;  // updated after consumer.join()

    // ── Thread 1: Producer (Capture + Convert + CopyResource → queue) ─────────
    std::thread producer([&]() {
        auto frameTimer = benchStart;
        HANDLE hFrameEvent = cap->GetFrameEvent();  // WGC FrameArrived auto-reset event

        // PD4 capture-delta state: the previous ACCEPTED frame's real capture
        // timestamp (100ns units). Used to compute the per-frame
        // captureDeltaMs (real WGC inter-frame interval). Producer-thread
        // local — only this thread captures, so no synchronization needed.
        int64_t prevCaptureStamp100ns = 0;
        bool haveCapturePrevStamp = false;

        // One-shot producer-thread warmup. Lets modules pre-pay any thread-local
        // cold-start cost (COM init, UIAutomation proxy, etc.) before frame 0
        // hits Evaluate, so the first user frame's decisionMs does not include
        // that one-time overhead. Modules that do not need warmup are no-ops.
        {
            AetherFlow::FrameContext warmupContext;
            warmupContext.frameIndex = -1;
            warmupContext.elapsedSeconds = 0.0;
            warmupContext.width = encWidth;
            warmupContext.height = encHeight;
            warmupContext.captureLeft = cap->GetCaptureLeft();
            warmupContext.captureTop = cap->GetCaptureTop();
            warmupContext.captureWidth = cap->GetCaptureWidth();
            warmupContext.captureHeight = cap->GetCaptureHeight();
            framePolicy.Warmup(warmupContext);
        }

        const auto stopRequested = [&opt] {
            return g_stopRequested.load(std::memory_order_relaxed) ||
                   (opt.externalStop && opt.externalStop->load(std::memory_order_relaxed));
        };
        for (int frameIdx = 0;
             (runUntilStopped || frameIdx < maxFrames) && !stopRequested(); ) {
            // Frame pacing: wait until the scheduled slot for this frame.
            // Uses WaitForSingleObject(frameEvent, remaining) so the loop wakes
            // the instant WGC delivers a frame — aligns our wake-up with the WGC
            // delivery cadence so CaptureTexture() finds a frame immediately.
            {
                auto target = frameTimer + std::chrono::microseconds(
                    static_cast<long long>(frameIdx * targetFrameTime * 1000));
                // If we fell more than one frame behind the grid, the old code
                // free-ran (no wait) and drained the WGC pool in a burst, then
                // stalled — a self-inflicted non-uniform capture cadence. Re-
                // anchor the grid to "now" so pacing resumes next frame instead
                // of perpetually trying to catch up by bursting.
                auto nowTs = std::chrono::high_resolution_clock::now();
                if (target + std::chrono::microseconds(
                        static_cast<long long>(targetFrameTime * 1000)) < nowTs) {
                    frameTimer = nowTs - std::chrono::microseconds(
                        static_cast<long long>(frameIdx * targetFrameTime * 1000));
                    target = nowTs;
                }
                while (true) {
                    auto remaining = target - std::chrono::high_resolution_clock::now();
                    if (remaining.count() <= 0) break;
                    auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
                    if (remainingMs <= 0) break;
                    if (hFrameEvent)
                        WaitForSingleObject(hFrameEvent, static_cast<DWORD>(remainingMs));
                    else
                        Sleep(static_cast<DWORD>(remainingMs));
                }
            }

            PipelineFrame f;
            f.index = frameIdx;
            f.frameStart = std::chrono::high_resolution_clock::now();

            // ---- Stage 1: Capture ----
            f.bgraTex = cap->CaptureTexture();
            f.captureEnd = std::chrono::high_resolution_clock::now();
            if (!f.bgraTex) {
                captureFailCount.fetch_add(1, std::memory_order_relaxed);
                continue;  // retry same frameIdx; target is past, no extra sleep
            }

            // PD1/PD4: pull the REAL capture timestamp for this frame, read
            // by ScreenCapture immediately after the WGC frame was acquired
            // (accessor is valid only until the next capture call on this
            // thread; this is the only thread that captures). Compute the
            // real inter-frame delta vs the previous ACCEPTED frame. This is
            // the genuine WGC delivery cadence — independent of the synthetic
            // frameIdx/encFps timeline used downstream.
            f.captureSystemRelativeTime100ns = cap->LastFrameSystemRelativeTime();
            f.captureTimestampFromWgc = cap->LastFrameTimestampFromWgc();
            if (haveCapturePrevStamp && f.captureSystemRelativeTime100ns > 0 &&
                prevCaptureStamp100ns > 0) {
                const int64_t d100ns =
                    f.captureSystemRelativeTime100ns - prevCaptureStamp100ns;
                // Guard against a non-monotonic stamp (should not happen with
                // QPC/SystemRelativeTime, but never emit a negative delta).
                f.captureDeltaMs = (d100ns > 0) ? (d100ns / 10000.0) : 0.0;
            } else {
                f.captureDeltaMs = 0.0;  // first accepted frame
            }
            prevCaptureStamp100ns = f.captureSystemRelativeTime100ns;
            haveCapturePrevStamp = true;

            const double elapsed = frameIdx / static_cast<double>(encFps);

            // ---- Stage 2: Deterministic frame decision layer ----
            AetherFlow::FrameContext frameContext;
            // P0.1 real-pixel wiring: hand the captured BGRA source texture to
            // the deterministic-decision layer. f.bgraTex was captured at
            // Stage 1 (above) and is not released until after Stage 4
            // (f.bgraTex->Release() further down), so it stays valid for the
            // entire framePolicy.Evaluate(frameContext) call below — the async
            // analyzer's SubmitFrame does a synchronous staging readback before
            // returning, so it never holds this pointer past Evaluate.
            frameContext.captureTextureBgra = f.bgraTex;
            // nv12Texture stays nullptr at the deterministic-decision stage:
            // NV12 is a Stage-4 product. Removing the dead field is a deferred
            // cleanup (kept out of the P0.1 real-pixel wiring scope).
            frameContext.nv12Texture = nullptr;
            frameContext.frameIndex = frameIdx;
            frameContext.elapsedSeconds = elapsed;
            frameContext.width = encWidth;
            frameContext.height = encHeight;
            frameContext.captureLeft = cap->GetCaptureLeft();
            frameContext.captureTop = cap->GetCaptureTop();
            frameContext.captureWidth = cap->GetCaptureWidth();
            frameContext.captureHeight = cap->GetCaptureHeight();
            const bool studioPanicActive =
                opt.panicLatch && opt.panicLatch->load(std::memory_order_relaxed);
            if (startupPanicPrivacyMask || studioPanicActive) {
                frameContext.panicMaskActive = true;
                frameContext.panicMaskSource =
                    (studioPanicActive && !startupPanicPrivacyMask) ? "studio-panic-mask"
                                                                    : "panic-privacy-mask";
                frameContext.panicMaskDebugLabel = "PanicPrivacyMaskModule";
            } else if (rightCtrlPanicMaskHotkeyEnabled && rightCtrlPanicMaskHotkey.PollActive()) {
                frameContext.panicMaskActive = true;
                frameContext.panicMaskSource = "right-ctrl-panic-mask";
                frameContext.panicMaskDebugLabel = "RightCtrlPanicMaskModule";
            }
            f.panicMaskActive = frameContext.panicMaskActive;

            if (useStaticRoi) {
                frameContext.hasCursor = true;
                frameContext.cursorIsStatic = true;
                frameContext.cursorX = (std::max)(0, (std::min)(encWidth - 1, staticRoiX));
                frameContext.cursorY = (std::max)(0, (std::min)(encHeight - 1, staticRoiY));
            } else {
                POINT cursorPos;
                if (GetCursorPos(&cursorPos)) {
                    const int capW = (std::max)(1, cap->GetCaptureWidth());
                    const int capH = (std::max)(1, cap->GetCaptureHeight());
                    const int relX = cursorPos.x - cap->GetCaptureLeft();
                    const int relY = cursorPos.y - cap->GetCaptureTop();
                    frameContext.hasCursor = true;
                    frameContext.cursorX = (relX * encWidth) / capW;
                    frameContext.cursorY = (relY * encHeight) / capH;
                    frameContext.cursorX = (std::max)(0, (std::min)(encWidth - 1, frameContext.cursorX));
                    frameContext.cursorY = (std::max)(0, (std::min)(encHeight - 1, frameContext.cursorY));
                }
            }

            f.decision = framePolicy.Evaluate(frameContext);
            if (f.decision.HasQualityRoi()) {
                const auto& roi = f.decision.qualityRegions.front();
                f.mouseX = (std::max)(0, (std::min)(encWidth - 1, roi.CenterX()));
                f.mouseY = (std::max)(0, (std::min)(encHeight - 1, roi.CenterY()));
            } else {
                f.mouseX = frameContext.hasCursor ? frameContext.cursorX : (encWidth / 2);
                f.mouseY = frameContext.hasCursor ? frameContext.cursorY : (encHeight / 2);
            }
            // Bridge-hardening: capture the per-frame analyzer accessors after
            // the policy engine has evaluated. Always populated; trace writer
            // only emits them when analyzerBridgeActive is true (strategy A).
            f.analyzerSubmitted = asyncAnalyzerBridge.LastSubmitted();
            f.analyzerContributed = asyncAnalyzerBridge.LastContributed();
            f.analyzerInferenceMs = asyncAnalyzerBridge.LastInferenceMs();
            f.analyzerStalenessFrames = asyncAnalyzerBridge.LastStalenessFrames();
            // Phase 4 P0.1 scene classifier / policy capture. Strategy-A
            // emission: trace writer only writes these fields when the
            // classifier is active. We always populate them so the values
            // are usable in-process; they default to empty otherwise.
            if (policyEngineActive) {
                const auto& policyDecision = policyEngine.LastDecision();
                f.policyMode = policyDecision.mode_label;
                f.policyReason = policyDecision.reason;
                // sceneClass / sceneClassConfidence come from the classifier's
                // contribution — recover from the FrameDecision merged scene
                // when the classifier won, otherwise stamp the canonical name
                // implied by FrameSceneType so the trace still records what
                // the policy decided on.
                const auto& mergedScene = f.decision.scene;
                if (mergedScene.source == "scene-classifier-onnx") {
                    f.sceneClass = mergedScene.debugLabel;  // canonical name stamped by classifier
                    f.sceneClassConfidence = mergedScene.confidence;
                } else {
                    // A deterministic 1.0 producer (panic / password / notification /
                    // manual) overrode the classifier. Report what the policy saw at
                    // the merge layer; sceneClass tracks the deterministic producer's
                    // implied class so the trace shows "sensitive_surface" even
                    // though the classifier may have voted otherwise.
                    if (mergedScene.type == AetherFlow::FrameSceneType::SensitiveSurface) {
                        f.sceneClass = "sensitive_surface";
                    } else if (mergedScene.type == AetherFlow::FrameSceneType::TextUi) {
                        f.sceneClass = "code_text";
                    } else if (mergedScene.type == AetherFlow::FrameSceneType::Slides) {
                        f.sceneClass = "slides";
                    } else if (mergedScene.type == AetherFlow::FrameSceneType::VideoContent) {
                        f.sceneClass = "video";
                    } else if (mergedScene.type == AetherFlow::FrameSceneType::GenericScreen) {
                        f.sceneClass = "mixed_ui";
                    } else {
                        f.sceneClass = "unknown";
                    }
                    f.sceneClassConfidence = mergedScene.confidence;
                }
            }
            f.decisionEnd = std::chrono::high_resolution_clock::now();

            // ---- Stage 3: Live Share Guard privacy mask fast path ----
            ID3D11Texture2D* convertInput = f.bgraTex;
            ID3D11Texture2D* maskedBgra = nullptr;
            if (f.decision.HasPrivacyMask()) {
                AetherFlow::PrivacyMaskApplyStats maskStats;
                // §4.y DA2: per-frame mode selection for the demo action.
                // privacyMaskMode is a single per-frame variable already
                // consumed by ApplyMask. When the decision carries a
                // scene-demo-action region (only possible when the demo flag
                // is ON), select the visual mode from the canonical merged
                // f.sceneClass per the §4.y.2 table. When no demo region is
                // present, frameMaskMode == privacyMaskMode (unchanged
                // behavior; the configured/default mode is used exactly as
                // before — the shared variable is never mutated, so there is
                // zero leakage into deterministic-producer frames).
                AetherFlow::PrivacyMaskMode frameMaskMode = privacyMaskMode;
                bool demoRegionPresent = false;
                for (const auto& m : f.decision.privacyMasks) {
                    if (m.source == "scene-demo-action") {
                        demoRegionPresent = true;
                        break;
                    }
                }
                if (demoRegionPresent) {
                    // Use the SAME hysteresis-smoothed class the demo module
                    // used to decide whether to emit (decision.policyStableClass,
                    // set by PolicyEngine before the demo module ran), NOT the
                    // raw per-inference f.sceneClass — so the emit decision and
                    // the mode selection always agree and the effect doesn't
                    // flicker. Mapping (2026-05-23): sensitive_surface->Blackout,
                    // video->Mosaic, code_text->Blur, slides->Grayscale.
                    // mixed_ui (generic desktop) never reaches here — the module
                    // suppresses it so a plain desktop is never obscured.
                    const std::string& demoClass = f.decision.policyStableClass;
                    if (demoClass == "sensitive_surface") {
                        frameMaskMode = AetherFlow::PrivacyMaskMode::Blackout;
                    } else if (demoClass == "video") {
                        frameMaskMode = AetherFlow::PrivacyMaskMode::Mosaic;
                    } else if (demoClass == "code_text") {
                        frameMaskMode = AetherFlow::PrivacyMaskMode::Blur;
                    } else if (demoClass == "slides") {
                        frameMaskMode = AetherFlow::PrivacyMaskMode::Grayscale;
                    }
                    // else: defensive — keep configured mode (should not occur
                    // because SceneDemoActionModule only emits for the four
                    // mapped classes).
                }
                if (!privacyMaskCompositor.ApplyMask(f.bgraTex, f.decision, frameMaskMode, &maskedBgra, &maskStats)) {
                    std::cerr << "[LiveShareGuard] Privacy mask apply failed: "
                              << (maskStats.failureReason.empty() ? "unknown" : maskStats.failureReason)
                              << "\n";
                    privacyMaskFatal.store(true, std::memory_order_relaxed);
                    f.bgraTex->Release();
                    f.bgraTex = nullptr;
                    break;
                }
                f.privacyMaskAppliedCount = maskStats.appliedCount;
                f.privacyMaskFallbackUsed = maskStats.fallbackUsed;
                f.privacyMaskPath = maskStats.path;
                convertInput = maskedBgra;
            }
            f.maskEnd = std::chrono::high_resolution_clock::now();

            // ---- Stage 4: Convert (BGRA -> NV12, VideoProcessorBlt, GPU) ----
            conv.Convert(convertInput);
            if (maskedBgra) {
                maskedBgra->Release();
                maskedBgra = nullptr;
            }
            f.bgraTex->Release();
            f.bgraTex = nullptr;
            f.convertEnd = std::chrono::high_resolution_clock::now();

            // ---- Stage 4b: Copy NV12 into pool texture ----
            // Decouples the converter's single output surface from the encoder's
            // multi-slot async pipeline; without this copy both threads would race
            // on the same conv.GetNV12Texture() pointer.
            if (encoderOwnsInputTextures) {
                if (!enc->AcquireInputTexture(&f.nv12Tex)) {
                    break;
                }
            } else {
                f.nv12Tex = pool.Acquire();  // blocks only when all 4 slots in flight
            }
            enc->GetContext()->CopyResource(f.nv12Tex, conv.GetNV12Texture());

            ID3D11Texture2D* queuedNv12Tex = f.nv12Tex;
            if (!q.push(std::move(f))) {
                if (encoderOwnsInputTextures) {
                    enc->ReleaseInputTexture(queuedNv12Tex);
                } else {
                    pool.Release(queuedNv12Tex);
                }
                break;
            }  // queue closed by consumer - stop
            frameIdx++;
        }
        q.close();  // signal consumer that no more frames are coming
    });

    // ── Thread 2: Consumer (Encode + pool.Release) ────────────────────────────
    std::thread consumer([&]() {
        int consecutiveFails = 0;

        PipelineFrame f;
        while (q.pop(f)) {
            // ---- Stage 4: Encode (async GPU submit) ----
            double elapsed = f.index / static_cast<double>(encFps);
            EncodeFrameRequest request;
            request.nv12Texture = f.nv12Tex;
            request.elapsedSeconds = elapsed;
            request.captureTimestamp100ns = f.captureSystemRelativeTime100ns;
            request.decision = f.decision;
            request.fallbackRoiCenterX = f.mouseX;
            request.fallbackRoiCenterY = f.mouseY;
            f.encodeOk = enc->EncodeFrame(request);
            f.encodeEnd = std::chrono::high_resolution_clock::now();
            WriteFrameTraceJson(frameTrace, f, analyzerBridgeActive, policyEngineActive);

            // Encoder-owned input textures are released by the encoder drain thread after success.
            if (encoderOwnsInputTextures) {
                if (!f.encodeOk) {
                    enc->ReleaseInputTexture(f.nv12Tex);
                }
            } else {
                pool.Release(f.nv12Tex);
            }
            f.nv12Tex = nullptr;

            // Per-frame live log; set AETHERFLOW_LIVE_LOG_INTERVAL=0 to disable.
            if (liveLogInterval > 0 && f.index % liveLogInterval == 0) {
                double capMs = std::chrono::duration<double, std::milli>(
                    f.captureEnd - f.frameStart).count();
                double decisionMs = std::chrono::duration<double, std::milli>(
                    f.decisionEnd - f.captureEnd).count();
                double maskMs = std::chrono::duration<double, std::milli>(
                    f.maskEnd - f.decisionEnd).count();
                double cvtMs = std::chrono::duration<double, std::milli>(
                    f.convertEnd - f.maskEnd).count();
                double encMs = std::chrono::duration<double, std::milli>(
                    f.encodeEnd - f.convertEnd).count();
                double totMs = std::chrono::duration<double, std::milli>(
                    f.encodeEnd - f.frameStart).count();
                double headroom = targetFrameTime - totMs;
                std::cout << "Frame " << std::setw(3) << f.index
                          << " | Cap: " << std::fixed << std::setprecision(2)
                          << std::setw(5) << capMs << "ms"
                          << " | Decision: " << std::setw(5) << decisionMs << "ms"
                          << " | Mask: " << std::setw(5) << maskMs << "ms"
                          << " | Conv: " << std::setw(5) << cvtMs << "ms"
                          << " | EncSubmit: " << std::setw(5) << encMs << "ms"
                          << " | Total: " << std::setw(6) << totMs << "ms"
                          << " | Headroom: " << std::setw(6) << headroom << "ms\n";
            }

            if (f.encodeOk) {
                deliveredFrames.fetch_add(1, std::memory_order_relaxed);
            }
            if (completedFrames.size() < kMaxCompletedFramesRetained) {
                completedFrames.push_back(f);
            }

            // Studio status readouts (atomics only; no-op on the CLI path).
            if (status && f.encodeOk) {
                const uint64_t encoded =
                    status->encodedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool maskOn =
                    f.decision.HasPrivacyMask() && f.privacyMaskAppliedCount > 0;
                status->maskActive.store(maskOn, std::memory_order_relaxed);
                int sourceKind = static_cast<int>(AetherFlow::MaskSourceKind::None);
                if (maskOn && !f.decision.privacyMasks.empty()) {
                    const std::string& src = f.decision.privacyMasks.front().source;
                    if (src.find("password") != std::string::npos) {
                        sourceKind = static_cast<int>(AetherFlow::MaskSourceKind::PasswordField);
                    } else if (src.find("notification") != std::string::npos) {
                        sourceKind = static_cast<int>(AetherFlow::MaskSourceKind::Notification);
                    } else if (src.find("panic") != std::string::npos) {
                        sourceKind = static_cast<int>(AetherFlow::MaskSourceKind::Panic);
                    } else if (src.find("manual") != std::string::npos) {
                        sourceKind = static_cast<int>(AetherFlow::MaskSourceKind::Manual);
                    } else if (src.find("scene-demo") != std::string::npos) {
                        sourceKind = static_cast<int>(AetherFlow::MaskSourceKind::DemoAction);
                    } else {
                        sourceKind = static_cast<int>(AetherFlow::MaskSourceKind::Other);
                    }
                }
                status->maskSource.store(sourceKind, std::memory_order_relaxed);
                // AI scene indicator readouts. f.sceneClass/-Confidence are
                // only populated when the policy engine ran (i.e. the
                // classifier was active this session); sourceKind tells the
                // UI whether the classifier's verdict actually won the merge
                // or a deterministic 1.0 producer out-ranked it.
                if (policyEngineActive) {
                    status->sceneClassIndex.store(
                        AetherFlow::SceneClassNameToIndex(f.sceneClass),
                        std::memory_order_relaxed);
                    status->sceneClassConfidence.store(f.sceneClassConfidence,
                                                       std::memory_order_relaxed);
                    // 1 = classifier verdict; 3 = baseline / low-confidence
                    // fallback (cold start included) — NOT the AI speaking;
                    // 2 = a deterministic producer out-ranked the classifier.
                    const std::string& mergedSrc = f.decision.scene.source;
                    status->sceneSourceKind.store(
                        mergedSrc == "scene-classifier-onnx" ? 1
                        : (mergedSrc == "baseline" ? 3 : 2),
                        std::memory_order_relaxed);
                }
                if ((encoded % 30) == 0) {
                    const double secs =
                        std::chrono::duration<double>(f.encodeEnd - benchStart).count();
                    if (secs > 0.5) {
                        status->effectiveFps.store(
                            static_cast<double>(encoded) / secs, std::memory_order_relaxed);
                    }
#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
                    if (srtOutput) {
                        const auto srtStats = srtOutput->GetStats();
                        status->srtListening.store(srtStats.listening, std::memory_order_relaxed);
                        status->srtClientConnected.store(srtStats.clientConnected,
                                                         std::memory_order_relaxed);
                        status->srtConnections.store(srtStats.connections,
                                                     std::memory_order_relaxed);
                        status->srtBytesSent.store(srtStats.bytesSent, std::memory_order_relaxed);
                    }
#endif
                }
            }

            // Stop early if GPU device is lost (4 consecutive encode failures).
            if (!f.encodeOk) {
                if (++consecutiveFails >= 4) {
                    std::cerr << "[PIPELINE] Encoder failed 4 consecutive frames. Stopping benchmark.\n";
                    q.close();
                    break;
                }
            } else {
                consecutiveFails = 0;
            }
        }
    });

    producer.join();
    consumer.join();
    if (frameTrace.is_open()) {
        frameTrace.flush();
    }
    benchEnd = std::chrono::high_resolution_clock::now();
    if (privacyMaskFatal.load(std::memory_order_relaxed)) {
        std::cerr << "[LiveShareGuard] Fatal privacy mask failure; refusing unmasked encode path\n";
        enc->Flush();
        return -5;
    }

    // ============================================================
    // Phase 0: p50 / p95 / p99 latency telemetry
    // ============================================================
    double totalSec = std::chrono::duration<double>(benchEnd - benchStart).count();

    std::vector<double> allCapMs, allDecisionMs, allMaskMs, allCvtMs, allEncMs, allTotMs;
    double totalCapture = 0, totalDecision = 0, totalMask = 0, totalConvert = 0, totalEncode = 0, totalLat = 0;
    double minLat = 9999, maxLat = 0;
    for (auto& f : completedFrames) {
        if (!f.encodeOk) continue;
        double capMs = std::chrono::duration<double, std::milli>(f.captureEnd - f.frameStart).count();
        double decisionMs = std::chrono::duration<double, std::milli>(f.decisionEnd - f.captureEnd).count();
        double maskMs = std::chrono::duration<double, std::milli>(f.maskEnd - f.decisionEnd).count();
        double cvtMs = std::chrono::duration<double, std::milli>(f.convertEnd - f.maskEnd).count();
        double encMs = std::chrono::duration<double, std::milli>(f.encodeEnd - f.convertEnd).count();
        double totMs = std::chrono::duration<double, std::milli>(f.encodeEnd  - f.frameStart).count();
        allCapMs.push_back(capMs);
        allDecisionMs.push_back(decisionMs);
        allMaskMs.push_back(maskMs);
        allCvtMs.push_back(cvtMs);
        allEncMs.push_back(encMs);
        allTotMs.push_back(totMs);
        totalCapture += capMs;
        totalDecision += decisionMs;
        totalMask += maskMs;
        totalConvert += cvtMs;
        totalEncode  += encMs;
        totalLat     += totMs;
        minLat = (std::min)(minLat, totMs);
        maxLat = (std::max)(maxLat, totMs);
    }
    std::sort(allCapMs.begin(), allCapMs.end());
    std::sort(allDecisionMs.begin(), allDecisionMs.end());
    std::sort(allMaskMs.begin(), allMaskMs.end());
    std::sort(allCvtMs.begin(), allCvtMs.end());
    std::sort(allEncMs.begin(), allEncMs.end());
    std::sort(allTotMs.begin(), allTotMs.end());

    int successCount = static_cast<int>(allTotMs.size());
    int failCount    = captureFailCount.load();
    // successCount is the (possibly retention-capped) percentile SAMPLE size;
    // deliveredTotal is the true session frame count and feeds every
    // denominator so unlimited runs past the cap do not skew the report.
    const uint64_t deliveredTotal = deliveredFrames.load(std::memory_order_relaxed);
    const bool statsSampled = deliveredTotal > static_cast<uint64_t>(successCount);

    std::cout << "\n========================================\n";
    std::cout << "     AETHERFLOW PERFORMANCE REPORT      \n";
    std::cout << "========================================\n";
    // For unlimited runs the denominator is the frames actually delivered
    // (there is no fixed target); bounded runs keep the historical
    // "fails / target" semantic that agent_verify parses.
    std::cout << "Capture Failures: " << failCount << " / "
              << (runUntilStopped ? static_cast<long long>(deliveredTotal)
                                  : static_cast<long long>(maxFrames)) << "\n";
    if (statsSampled) {
        std::cout << "NOTE: per-stage percentiles below are sampled from the first "
                  << successCount << " frames of " << deliveredTotal
                  << " delivered (stats retention cap).\n";
    }
    if (successCount > 0) {
        double avgCapture  = totalCapture / successCount;
        double avgDecision = totalDecision / successCount;
        double avgMask     = totalMask / successCount;
        double avgConvert  = totalConvert / successCount;
        double avgEncode   = totalEncode  / successCount;
        double avgPipeline = avgCapture + avgDecision + avgMask + avgConvert + avgEncode;
        double avgAIBuffer = targetFrameTime - avgPipeline;

        std::cout << "Total Frames:     " << static_cast<long long>(deliveredTotal) << " / "
                  << (runUntilStopped ? static_cast<long long>(deliveredTotal)
                                      : static_cast<long long>(maxFrames)) << "\n";
        std::cout << "\n--- Overall Latency ---\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Avg: " << (totalLat / successCount) << " ms\n";
        std::cout << "  Min: " << minLat << " ms   Max: " << maxLat << " ms\n";
        std::cout << "  p50: " << Percentile(allTotMs, 50) << " ms\n";
        std::cout << "  p95: " << Percentile(allTotMs, 95) << " ms\n";
        std::cout << "  p99: " << Percentile(allTotMs, 99) << " ms\n";
        std::cout << "  Avg FPS: " << (static_cast<double>(deliveredTotal) / totalSec) << "\n";

        std::cout << "\n--- Per-Stage Breakdown (ms) ---\n";
        std::cout << "  Note: Capture includes wait-for-next-frame time in WGC polling path.\n";
        std::cout << "  Note: EncodeSubmit measures submit latency, not full encode completion latency.\n";
        std::cout << "  Stage         Avg     p50     p95     p99     Min     Max\n";
        std::cout << "  Capture  " << std::setw(7) << avgCapture
                  << std::setw(8) << Percentile(allCapMs, 50)
                  << std::setw(8) << Percentile(allCapMs, 95)
                  << std::setw(8) << Percentile(allCapMs, 99)
                  << std::setw(8) << allCapMs.front()
                  << std::setw(8) << allCapMs.back() << "\n";
        std::cout << "  Decision " << std::setw(7) << avgDecision
                  << std::setw(8) << Percentile(allDecisionMs, 50)
                  << std::setw(8) << Percentile(allDecisionMs, 95)
                  << std::setw(8) << Percentile(allDecisionMs, 99)
                  << std::setw(8) << allDecisionMs.front()
                  << std::setw(8) << allDecisionMs.back() << "\n";
        std::cout << "  Mask     " << std::setw(7) << avgMask
                  << std::setw(8) << Percentile(allMaskMs, 50)
                  << std::setw(8) << Percentile(allMaskMs, 95)
                  << std::setw(8) << Percentile(allMaskMs, 99)
                  << std::setw(8) << allMaskMs.front()
                  << std::setw(8) << allMaskMs.back() << "\n";
        std::cout << "  Convert  " << std::setw(7) << avgConvert
                  << std::setw(8) << Percentile(allCvtMs, 50)
                  << std::setw(8) << Percentile(allCvtMs, 95)
                  << std::setw(8) << Percentile(allCvtMs, 99)
                  << std::setw(8) << allCvtMs.front()
                  << std::setw(8) << allCvtMs.back() << "\n";
        std::cout << "  EncodeSubmit" << std::setw(6) << avgEncode
                  << std::setw(8) << Percentile(allEncMs, 50)
                  << std::setw(8) << Percentile(allEncMs, 95)
                  << std::setw(8) << Percentile(allEncMs, 99)
                  << std::setw(8) << allEncMs.front()
                  << std::setw(8) << allEncMs.back() << "\n";
        std::cout << "  Pipeline Total: " << avgPipeline << " ms\n";

        std::cout << "\n--- AI Processing Budget ---\n";
        std::cout << "Target Frame Time: " << targetFrameTime << " ms (" << encFps << "fps)\n";
        std::cout << "Pipeline Usage:    " << avgPipeline << " ms\n";
        std::cout << "AI Buffer:         " << avgAIBuffer << " ms ("
                  << (avgAIBuffer / targetFrameTime * 100) << "% available)\n";
        if (avgAIBuffer < 10) {
            std::cout << "  WARNING: AI buffer < 10ms, may drop frames!\n";
        } else {
            std::cout << "  OK: AI buffer sufficient for frame decisions\n";
        }
    } else {
        std::cout << "No frames were successfully encoded.\n";
    }

    // ============================================================
    // Capture-timing root-fix PD4: effective-capture-fps self-report.
    //
    // This is the DECISIVE diagnostic for the recorded-video judder. It
    // answers: is the judder purely the fixed-30fps mux forcing uniform
    // spacing onto a steady ~30fps capture (a PTS sidecar + mux fix fully
    // solves it), OR is background WGC capture genuinely starved (few unique
    // frames; a mux fix alone will NOT make a smooth-30fps demo — needs a
    // capture-method change)?
    //
    // Uses the REAL WGC SystemRelativeTime deltas (captureDeltaMs), NOT the
    // synthetic frameIndex/encFps timeline. Measurement-only: always
    // computed, never gated on --record-timed, changes no output bytes.
    // ============================================================
    {
        std::vector<double> capDeltas;        // accepted-frame inter-deltas (>0)
        capDeltas.reserve(completedFrames.size());
        double sumDeltaMs = 0.0;
        double maxDeltaMs = 0.0;
        int nearZeroDeltaFrames = 0;          // duplicate / stale (WGC returned same frame)
        int bigJumpFrames = 0;                // > 2x the 33.33ms/30fps budget
        int wgcSourceFrames = 0;              // stamp came from WGC SystemRelativeTime
        int qpcFallbackFrames = 0;            // stamp came from QPC fallback
        int stampedFrames = 0;                // frames with a real timestamp at all
        const double targetDeltaMs = 1000.0 / static_cast<double>(encFps);

        for (const auto& f : completedFrames) {
            if (f.captureSystemRelativeTime100ns > 0) {
                ++stampedFrames;
                if (f.captureTimestampFromWgc) ++wgcSourceFrames;
                else ++qpcFallbackFrames;
            }
            // f.index == 0 is the first accepted frame: captureDeltaMs is 0.0
            // by construction (no previous frame), exclude from the cadence.
            if (f.index == 0) continue;
            const double d = f.captureDeltaMs;
            // A near-zero delta means WGC handed back the same content again
            // (no new unique frame) — the starvation signal.
            if (d <= 1.0) {
                ++nearZeroDeltaFrames;
            }
            if (d > 2.0 * targetDeltaMs) {
                ++bigJumpFrames;
            }
            if (d > 0.0) {
                capDeltas.push_back(d);
                sumDeltaMs += d;
                maxDeltaMs = (std::max)(maxDeltaMs, d);
            }
        }

        std::sort(capDeltas.begin(), capDeltas.end());
        const size_t nDeltas = capDeltas.size();
        const double meanDeltaMs = nDeltas ? (sumDeltaMs / static_cast<double>(nDeltas)) : 0.0;
        const double p50DeltaMs = Percentile(capDeltas, 50);
        const double p95DeltaMs = Percentile(capDeltas, 95);
        // Effective capture fps = (frames-1) / (sum of real deltas in seconds).
        // nDeltas already == (accepted frames - 1) excluding zero/negative.
        const double effFps =
            (sumDeltaMs > 0.0) ? (static_cast<double>(nDeltas) / (sumDeltaMs / 1000.0)) : 0.0;

        std::cout << "\n========================================\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "[CAPTURE] effective capture fps = " << effFps
                  << " (mean delta " << meanDeltaMs << "ms"
                  << ", p50 " << p50DeltaMs << "ms"
                  << ", p95 " << p95DeltaMs << "ms"
                  << ", max " << maxDeltaMs << "ms)"
                  << " over " << stampedFrames << " frames"
                  << " — target " << targetDeltaMs << "ms/" << encFps << "fps\n";
        std::cout << "[CAPTURE] near-zero-delta (duplicate/stale) frames = "
                  << nearZeroDeltaFrames << " / " << stampedFrames
                  << "; >2x-budget jumps = " << bigJumpFrames << "\n";
        std::cout << "[CAPTURE] timestamp source: WGC SystemRelativeTime = "
                  << wgcSourceFrames << ", QPC fallback = " << qpcFallbackFrames
                  << " (of " << stampedFrames << " stamped frames)\n";
    }

    std::cout << "========================================\n";
    // CRITICAL: Flush encoder to retrieve buffered frames
    std::cout << "\n[Step 5] Flushing encoder...\n";
    enc->Flush();

#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
    if (srtOutput) {
        // Run-end evidence line (consumed from console.log by humans and
        // agents). Teardown itself is destructor-ordered: `enc` goes first,
        // then `srtOutput` stops its worker.
        const auto srtStats = srtOutput->GetStats();
        std::cout << "[SRT] summary: connections=" << srtStats.connections
                  << " sent=" << srtStats.sent
                  << " bytes=" << srtStats.bytesSent
                  << " enqueued=" << srtStats.enqueued
                  << " dropped_queue_full=" << srtStats.droppedQueueFull
                  << " dropped_awaiting_keyframe=" << srtStats.droppedAwaitingKeyframe
                  << "\n";
    }
#endif

    return 0;  // capOwner tears down the capture session
}

}  // namespace AetherFlow

#elif defined(__APPLE__)

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "AetherFlow/platform/mac/MacosPlatformShim.h"

namespace {

// macOS-only copies of small env / CLI helpers. We deliberately do NOT move
// the Windows GetEnvBool / GetEnvInt / ParseCommaList helpers here -- those
// live inside the #if defined(_WIN32) block and editing them risks breaking
// Windows. These local copies are scoped to the macOS branch.

std::string DefaultMacosOutputDir() {
    if (const char* env = std::getenv("AETHERFLOW_OUTPUT_DIR"); env && *env) {
        return std::string(env);
    }
    return "output";
}

int ParseIntArg(const char* value, int fallback) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<int>(parsed);
}

bool GetEnvBoolMac(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return fallback;
}

int GetEnvIntMac(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return ParseIntArg(v, fallback);
}

std::vector<std::string> ParseCommaListMac(const char* value) {
    std::vector<std::string> out;
    if (!value || !*value) return out;
    std::string s(value);
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',' || s[i] == ';') {
            if (i > start) {
                auto entry = s.substr(start, i - start);
                while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.erase(entry.begin());
                while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) entry.pop_back();
                if (!entry.empty()) out.push_back(std::move(entry));
            }
            start = i + 1;
        }
    }
    return out;
}

// Default messenger / notification whitelist for macOS. owner-name semantics
// (kCGWindowOwnerName), NOT executable leaf -- macOS bundles do not expose a
// stable per-window exe path. Includes the Windows demo six plus the macOS
// native Messages app. Empty whitelist disables the producer (handled below
// after env+CLI resolution).
const std::vector<std::string> kDefaultMacosNotificationOwnerWhitelist = {
    "LINE",
    "Slack",
    "Discord",
    "Microsoft Teams",
    "Teams",
    "Telegram",
    "WhatsApp",
    "Messages",
};

} // namespace

int main(int argc, char* argv[]) {
    AetherFlow::platform::mac::MacosRunOptions options;
    options.targetFps = 30;
    options.durationFrames = 500;
    options.cursorRoiRadiusPx = 200;

    std::string outputDir = DefaultMacosOutputDir();
    std::string traceJsonlPath;
    std::string macosSmokeJsonPath;

    // Env-driven privacy / notification producer defaults (Windows parity).
    bool notificationMaskEnabled = GetEnvBoolMac("AETHERFLOW_NOTIFICATION_MASK", true);
    int notificationMaskPollFrames = GetEnvIntMac("AETHERFLOW_NOTIFICATION_MASK_POLL_FRAMES", 5);
    std::vector<std::string> notificationOwnerWhitelist =
        ParseCommaListMac(std::getenv("AETHERFLOW_NOTIFICATION_PROCESS_LIST"));
    std::string privacyMaskMode = "mosaic"; // default mode on macOS = mosaic (chat-window-mosaic feature).
    if (const char* envMode = std::getenv("AETHERFLOW_PRIVACY_MASK_MODE"); envMode && *envMode) {
        std::string m(envMode);
        std::transform(m.begin(), m.end(), m.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (m == "blur" || m == "mosaic" || m == "blackout") {
            privacyMaskMode = m;
        } else {
            std::cerr << "[AetherFlow][macOS] Unknown AETHERFLOW_PRIVACY_MASK_MODE='" << envMode
                      << "', keeping default 'mosaic'.\n";
        }
    }
    int privacyMaskMosaicBlockPx = GetEnvIntMac("AETHERFLOW_PRIVACY_MASK_MOSAIC_BLOCK_PX", 16);
    bool mockAnalyzerEnabled = GetEnvBoolMac("AETHERFLOW_MOCK_ANALYZER", false);
    // Bridge-hardening (Phase 4 P0 prerequisite): Windows parity for the two
    // bridge-tuning env vars. Default to every-frame submission and 200 ms
    // simulated compute.
    int analyzerBridgeIntervalFrames = GetEnvIntMac("AETHERFLOW_ANALYZER_BRIDGE_INTERVAL_FRAMES", 1);
    int mockAnalyzerInferenceMs = GetEnvIntMac("AETHERFLOW_MOCK_ANALYZER_INFERENCE_MS", 200);

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        auto next = [&](int& idx) -> const char* {
            if (idx + 1 >= argc) return nullptr;
            return argv[++idx];
        };
        if (arg == "--macos-output-dir") {
            const char* v = next(i);
            if (v) outputDir = v;
        } else if (arg == "--macos-trace-path") {
            const char* v = next(i);
            if (v) traceJsonlPath = v;
        } else if (arg == "--macos-smoke-path") {
            const char* v = next(i);
            if (v) macosSmokeJsonPath = v;
        } else if (arg == "--target-fps") {
            options.targetFps = ParseIntArg(next(i), options.targetFps);
        } else if (arg == "--duration-frames") {
            options.durationFrames = ParseIntArg(next(i), options.durationFrames);
        } else if (arg == "--cursor-radius") {
            options.cursorRoiRadiusPx = ParseIntArg(next(i), options.cursorRoiRadiusPx);
        } else if (arg == "--capture-width") {
            options.captureWidth = ParseIntArg(next(i), options.captureWidth);
        } else if (arg == "--capture-height") {
            options.captureHeight = ParseIntArg(next(i), options.captureHeight);
        } else if (arg == "--notification-mask") {
            notificationMaskEnabled = true;
        } else if (arg == "--no-notification-mask" || arg == "--disable-notification-mask") {
            notificationMaskEnabled = false;
        } else if (arg == "--mock-analyzer") {
            mockAnalyzerEnabled = true;
        } else if (arg == "--no-mock-analyzer") {
            mockAnalyzerEnabled = false;
        } else if (arg.rfind("--analyzer-bridge-interval-frames=", 0) == 0) {
            const int parsed = ParseIntArg(arg.substr(34).c_str(), 0);
            if (parsed > 0) {
                analyzerBridgeIntervalFrames = parsed;
            } else {
                std::cerr << "[AetherFlow][macOS] Invalid --analyzer-bridge-interval-frames; expected positive int\n";
                return -4;
            }
        } else if (arg.rfind("--notification-mask-process=", 0) == 0) {
            std::string v = arg.substr(28);
            if (!v.empty()) notificationOwnerWhitelist.push_back(std::move(v));
        } else if (arg.rfind("--privacy-mask-mode=", 0) == 0) {
            std::string m = arg.substr(20);
            std::transform(m.begin(), m.end(), m.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (m == "blur" || m == "mosaic" || m == "blackout") {
                privacyMaskMode = m;
            } else {
                std::cerr << "[AetherFlow][macOS] Invalid --privacy-mask-mode='" << m
                          << "', expected blackout|blur|mosaic; keeping default '"
                          << privacyMaskMode << "'.\n";
            }
        }
    }

    notificationMaskPollFrames = std::max(1, notificationMaskPollFrames);
    if (notificationMaskEnabled && notificationOwnerWhitelist.empty()) {
        // No env / CLI whitelist supplied -- fall back to the macOS default
        // so the demo path masks common messengers automatically.
        notificationOwnerWhitelist = kDefaultMacosNotificationOwnerWhitelist;
    }
    if (notificationOwnerWhitelist.empty()) {
        notificationMaskEnabled = false;
    }

    if (traceJsonlPath.empty()) {
        std::filesystem::path p(outputDir);
        p /= "traces";
        p /= "frame_trace.jsonl";
        traceJsonlPath = p.string();
    }
    if (macosSmokeJsonPath.empty()) {
        std::filesystem::path p(outputDir);
        p /= "macos_smoke.json";
        macosSmokeJsonPath = p.string();
    }

    options.outputDir = outputDir;
    options.traceJsonlPath = traceJsonlPath;
    options.macosSmokeJsonPath = macosSmokeJsonPath;
    options.notificationMaskEnabled = notificationMaskEnabled;
    options.notificationOwnerWhitelist = notificationOwnerWhitelist;
    options.notificationMaskPollFrames = notificationMaskPollFrames;
    options.privacyMaskMode = privacyMaskMode;
    options.privacyMaskMosaicBlockPx = privacyMaskMosaicBlockPx;
    options.notificationPaddingPx = 4;
    options.mockAnalyzerEnabled = mockAnalyzerEnabled;
    options.analyzerBridgeIntervalFrames = std::max(1, analyzerBridgeIntervalFrames);
    options.mockAnalyzerInferenceMs = std::max(1, mockAnalyzerInferenceMs);

    AetherFlow::platform::mac::MacosRunResult result;
    int rc = AetherFlow::platform::mac::RunMacosPipeline(options, &result);
    if (rc != 0) {
        std::cerr << "[AetherFlow] macOS pipeline failed with code " << rc << "\n";
    }
    return rc;
}

#else

#include <iostream>

int main(int /*argc*/, char** /*argv*/) {
    std::cerr << "AetherFlow: unsupported platform\n";
    return 2;
}

#endif
