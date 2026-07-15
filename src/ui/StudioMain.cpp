// AetherFlowStudio — the spec Delta B settings UI.
//
// A Dear ImGui (Win32 + D3D11) shell around the SAME pipeline the headless
// CLI runs: Start builds AetherFlow::PipelineOptions from the window controls
// and calls RunPipelineOnce on a worker thread (maxFrames = 0, until Stop);
// Stop flips the shared externalStop atomic and the pipeline shuts down
// through its normal clean path (flush → reports → [SRT] summary). Settings
// persist to aetherflow_studio.ini beside the exe (src/ui/AppConfig.*).
//
// Zero mandatory command-line arguments (spec hard rule). One hidden flag:
//   --ui-smoke   create the window+device hidden, render 3 frames, exit 0 —
//                the deterministic CI/agent gate for this target.

#include <winsock2.h>  // must precede windows.h (SrtStreamOutput.h dependency chain)
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "AetherFlow/app/PipelineRunner.h"
#include "AetherFlow/ScreenCapture.h"
#include "AppConfig.h"
#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
#include "AetherFlow/streaming/SrtStreamOutput.h"
#endif

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ── D3D11 swapchain plumbing (standard ImGui win32+dx11 shell) ─────────────
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_mainRtv = nullptr;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_mainRtv);
        backBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRtv) {
        g_mainRtv->Release();
        g_mainRtv = nullptr;
    }
}

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = {};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    }
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI StudioWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g_resizeWidth = LOWORD(lparam);
            g_resizeHeight = HIWORD(lparam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;  // no ALT menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ── Pipeline session driven from the UI ─────────────────────────────────────
struct StudioSession {
    std::thread worker;
    std::atomic<bool> stopFlag{false};
    std::atomic<bool> panicLatch{false};
    std::atomic<int> lastExitCode{0};
    // Set by the worker lambda as its LAST statement — the only safe join
    // signal. status.running turns true only once RunPipelineOnce begins, so
    // "joinable && !running" would join (and freeze the UI forever on an
    // unlimited session) if the just-spawned thread had not been scheduled
    // yet. Join on proof of completion instead. (Code-review blocker,
    // srt_ui_v1 review.)
    std::atomic<bool> workerDone{false};
    AetherFlow::PipelineStatus status;

    bool Running() const { return status.running.load(std::memory_order_relaxed); }

    void Start(const AetherFlow::AppConfig& cfg,
               const std::vector<ScreenCapture::MonitorInfo>& monitors,
               int maxFramesOverride = 0 /* 0 = until Stop */,
               const std::string& aiModelPath = {} /* empty = AI off */) {
        JoinIfFinished();
        if (worker.joinable()) return;  // one session at a time
        stopFlag.store(false);
        status.encodedFrames.store(0);
        status.effectiveFps.store(0.0);
        // Clear the rest too, or the panel shows ~1 s of stale
        // previous-session values until the first 30-frame refresh.
        status.maskActive.store(false);
        status.maskSource.store(0);
        status.srtListening.store(false);
        status.srtClientConnected.store(false);
        status.srtConnections.store(0);
        status.srtBytesSent.store(0);
        status.sceneClassifierState.store(0);
        status.sceneClassIndex.store(-1);
        status.sceneClassConfidence.store(0.0f);
        status.sceneSourceKind.store(0);

        AetherFlow::PipelineOptions opt;
        opt.monitorIndex = cfg.monitorIndex;
        opt.encoder = static_cast<AetherFlow::EncoderPreference>(cfg.encoder);
        if (cfg.resolutionPreset == 0) {
            // Native = the selected monitor's desktop size (converter scales
            // capture→encode 1:1); fall back to the compile-time default when
            // the monitor list is stale.
            const int mi = (cfg.monitorIndex < static_cast<int>(monitors.size()))
                               ? cfg.monitorIndex : 0;
            if (mi < static_cast<int>(monitors.size())) {
                const RECT r = monitors[static_cast<size_t>(mi)].rect;
                opt.width = static_cast<int>(r.right - r.left);
                opt.height = static_cast<int>(r.bottom - r.top);
            }
        } else if (cfg.resolutionPreset == 2) {
            opt.width = 1280;
            opt.height = 720;
        } else {
            opt.width = 1920;
            opt.height = 1080;
        }
        opt.fps = cfg.fps;
        opt.bitrateKbps = cfg.bitrateKbps;
        opt.passwordFieldMaskEnabled = cfg.passwordFieldMask;
        opt.notificationMaskEnabled = cfg.notificationMask;
        opt.privacyMaskMode =
            (cfg.maskMode == 0) ? AetherFlow::PrivacyMaskMode::Blackout
            : (cfg.maskMode == 2) ? AetherFlow::PrivacyMaskMode::Mosaic
                                  : AetherFlow::PrivacyMaskMode::Blur;
        opt.srt.enabled = cfg.srtEnabled;
        opt.srt.port = cfg.srtPort;
        opt.srt.latencyMs = cfg.srtLatencyMs;
        opt.srt.passphrase = cfg.srtPassphrase;
        // AI scene detection: both the persisted toggle AND a model found on
        // disk at launch are required (a stale saved `true` with no model is
        // inert). Provider stays the pipeline default (DirectML, CPU fallback).
        if (cfg.aiSceneDetection && !aiModelPath.empty()) {
            opt.sceneClassifierOnnxModel = aiModelPath;
        }
        opt.maxFrames = maxFramesOverride;  // 0 = run until Stop
        opt.externalStop = &stopFlag;
        opt.panicLatch = &panicLatch;

        workerDone.store(false, std::memory_order_relaxed);
        worker = std::thread([this, opt] {
            lastExitCode.store(AetherFlow::RunPipelineOnce(opt, &status),
                               std::memory_order_relaxed);
            workerDone.store(true, std::memory_order_release);
        });
    }

    void RequestStop() { stopFlag.store(true); }

    void JoinIfFinished() {
        if (worker.joinable() && workerDone.load(std::memory_order_acquire)) {
            worker.join();
            workerDone.store(false, std::memory_order_relaxed);
        }
    }

    void StopAndJoin() {
        RequestStop();
        if (worker.joinable()) worker.join();
        workerDone.store(false, std::memory_order_relaxed);
    }
};

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::filesystem::path(buf).parent_path()
                                       : std::filesystem::current_path();
}

std::filesystem::path IniPath() { return ExeDir() / L"aetherflow_studio.ini"; }

#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
// Locate the optional CLIP scene-classifier model. Checked ONCE at launch
// (cheap + deterministic; the grayed-out hint says so). Search order:
//   1. AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL env override (CLI parity)
//   2. <exe>/models/scene_classifier_v1.onnx   (portable --with-model layout)
//   3. <exe>/../../models/scene_classifier_v1.onnx (dev tree: build/Release)
// Returned as a narrow string because PipelineOptions::sceneClassifierOnnxModel
// is narrow (same limitation as the CLI flag); a path the ACP cannot represent
// is treated as "not found" rather than crashing.
std::string FindSceneClassifierModel() {
    std::vector<std::filesystem::path> candidates;
    // An explicit env override is authoritative: use it or report "not found"
    // (grayed toggle) — never silently fall through to a DIFFERENT model than
    // the one the user pointed at (parity with the CLI, where a bad path
    // fails loudly instead of degrading). Values >= 32k chars cannot be env
    // values on Windows, so the fixed buffer is safe.
    wchar_t envBuf[32768] = {};
    if (GetEnvironmentVariableW(L"AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL",
                                envBuf, 32768) > 0) {
        candidates.emplace_back(envBuf);
    } else {
        candidates.push_back(ExeDir() / L"models" / L"scene_classifier_v1.onnx");
        candidates.push_back(ExeDir() / L".." / L".." / L"models" /
                             L"scene_classifier_v1.onnx");
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(c, ec)) continue;
        std::filesystem::path resolved = std::filesystem::weakly_canonical(c, ec);
        if (ec || resolved.empty()) resolved = c;  // canonicalize is cosmetic
        try {
            return resolved.string();
        } catch (...) {
            // Not representable in the ACP — skip (documented limitation).
        }
    }
    return {};
}
#endif

// libsrt accepts 10-79 chars. The CLI refuses invalid values with exit -4;
// the UI must hold the SAME line — Start is refused with inline red text
// instead of silently stripping a security value (code-review risk 1).
bool PassphraseValid(const std::string& p) {
    return p.empty() || (p.size() >= 10 && p.size() <= 79);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    bool uiSmoke = false;
    bool uiSelftest = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--ui-smoke") uiSmoke = true;
            if (std::wstring(argv[i]) == L"--ui-selftest") uiSelftest = true;
        }
        if (argv) LocalFree(argv);
    }
    const bool headlessGate = uiSmoke || uiSelftest;

    // Keep recordings inside the app's own folder (portable zip = next to the
    // exe) instead of the CLI's exe/../../output convention, which escapes an
    // extracted folder. Respect an explicit user override. (Code-review risk 2.)
    // _wputenv_s, NOT SetEnvironmentVariableW: the pipeline reads this via the
    // CRT's getenv, whose snapshot does not see Win32-only runtime changes.
    if (!GetEnvironmentVariableW(L"AETHERFLOW_OUTPUT_DIR", nullptr, 0)) {
        _wputenv_s(L"AETHERFLOW_OUTPUT_DIR", (ExeDir() / L"output").c_str());
    }

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, StudioWndProc, 0, 0,
                      hInstance, nullptr, nullptr, nullptr, nullptr,
                      L"AetherFlowStudio", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"AetherFlow Studio",
                              WS_OVERLAPPEDWINDOW, 100, 100, 760, 640,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, headlessGate ? SW_HIDE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Fixed full-viewport layout — nothing for imgui.ini to remember, and a
    // stray ini beside the exe (or repo root) is just litter.
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    AetherFlow::AppConfig cfg;
    AetherFlow::AppConfig::LoadFromFile(IniPath(), &cfg);  // missing file = defaults
    cfg.Normalize();
    const auto monitors = ScreenCapture::EnumerateMonitors();
    if (cfg.monitorIndex >= static_cast<int>(monitors.size())) cfg.monitorIndex = 0;

    // AI scene detection availability — model looked up once at launch. Empty
    // means the toggle renders grayed out and sessions never get a model path.
    // aiEnvOverride: the env var is authoritative when set, so the grayed-out
    // hint must point at the env value, not at the exe-adjacent models\ folder
    // (delta-review nit).
    std::string aiModelPath;
    bool aiEnvOverride = false;
#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
    aiModelPath = FindSceneClassifierModel();
    aiEnvOverride = GetEnvironmentVariableW(
                        L"AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL", nullptr, 0) > 0;
#endif

    StudioSession session;
    int smokeFramesLeft = uiSmoke ? 3 : -1;
    // --ui-selftest: programmatically drive the flagship path the plain smoke
    // never touches — Start a short real session (60 frames, SRT off, primary
    // monitor), wait for it to finish, assert frames actually encoded, exit
    // 0/1. This is the deterministic Delta B gate (and it exercises the
    // StudioSession thread lifecycle where the review found the join race).
    // When a scene-classifier model is on disk, a second 150-frame leg runs
    // with the AI toggle on and additionally asserts the classifier came up
    // AND won the scene merge at least once (pixels → ONNX → merge → status).
    // No model ⇒ the AI leg is skipped and the gate stays green (portable/CI).
    bool selftestStarted = false;
    int selftestStage = 0;          // 0 = default leg, 1 = AI leg
    bool selftestSawAiWin = false;  // AI leg: sceneSourceKind==1 observed
    int selftestResult = -1;  // -1 pending, 0 pass, 1 fail
    auto selftestLegBegin = std::chrono::steady_clock::now();
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                       DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateRenderTarget();
        }

        session.JoinIfFinished();

        if (uiSelftest) {
            if (!selftestStarted) {
                selftestStarted = true;
                selftestLegBegin = std::chrono::steady_clock::now();
                AetherFlow::AppConfig testCfg;   // defaults, forced safe:
                testCfg.srtEnabled = false;      // no port dependency
                testCfg.monitorIndex = 0;
                session.Start(testCfg, monitors, /*maxFramesOverride=*/60);
            } else if (selftestResult < 0) {
                // AI leg: sample the merge-win latch at render rate (~2x the
                // pipeline frame rate; plus a final read at leg end below).
                if (selftestStage == 1 &&
                    session.status.sceneSourceKind.load(std::memory_order_relaxed) == 1) {
                    selftestSawAiWin = true;
                }
                if (!session.Running() && !session.worker.joinable()) {
                    const auto frames =
                        session.status.encodedFrames.load(std::memory_order_relaxed);
                    const bool legPass =
                        session.lastExitCode.load(std::memory_order_relaxed) == 0 &&
                        frames >= 30;
                    if (!legPass) {
                        selftestResult = 1;
                        done = true;
                    } else if (selftestStage == 0 && !aiModelPath.empty()) {
                        selftestStage = 1;
                        selftestLegBegin = std::chrono::steady_clock::now();
                        AetherFlow::AppConfig aiCfg;  // defaults, forced safe
                        aiCfg.srtEnabled = false;
                        aiCfg.monitorIndex = 0;
                        aiCfg.aiSceneDetection = true;
                        session.Start(aiCfg, monitors, /*maxFramesOverride=*/150,
                                      aiModelPath);
                    } else if (selftestStage == 1) {
                        const int st = session.status.sceneClassifierState.load(
                            std::memory_order_relaxed);
                        selftestResult =
                            ((st == 1 || st == 2) && selftestSawAiWin) ? 0 : 1;
                        done = true;
                    } else {
                        selftestResult = 0;  // no model on disk: AI leg skipped
                        done = true;
                    }
                } else if (std::chrono::steady_clock::now() - selftestLegBegin >
                           std::chrono::seconds(120)) {
                    // Likely capture starvation (static desktop) or a hang.
                    session.RequestStop();
                    selftestResult = 1;
                    done = true;
                }
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("AetherFlow Studio", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        const bool running = session.Running();

        // ── Capture / encode settings ───────────────────────────────────────
        ImGui::SeparatorText("Capture & Encode");
        ImGui::BeginDisabled(running);
        {
            std::string monitorLabel;
            {
                char buf[96];
                const int mi = cfg.monitorIndex;
                if (mi < static_cast<int>(monitors.size())) {
                    const RECT r = monitors[static_cast<size_t>(mi)].rect;
                    std::snprintf(buf, sizeof(buf), "Monitor %d (%ldx%ld%s)", mi,
                                  r.right - r.left, r.bottom - r.top,
                                  monitors[static_cast<size_t>(mi)].primary ? ", primary" : "");
                } else {
                    std::snprintf(buf, sizeof(buf), "Monitor %d", mi);
                }
                monitorLabel = buf;
            }
            if (ImGui::BeginCombo("Capture source", monitorLabel.c_str())) {
                for (const auto& m : monitors) {
                    char item[96];
                    std::snprintf(item, sizeof(item), "Monitor %d (%ldx%ld%s)", m.index,
                                  m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
                                  m.primary ? ", primary" : "");
                    if (ImGui::Selectable(item, cfg.monitorIndex == m.index)) {
                        cfg.monitorIndex = m.index;
                    }
                }
                ImGui::EndCombo();
            }

            const char* encoders[] = {"Auto (NVENC if available, else oneVPL)",
                                      "NVIDIA NVENC", "Intel oneVPL"};
            ImGui::Combo("Encoder", &cfg.encoder, encoders, 3);
            const char* resolutions[] = {"Native (monitor size)", "1080p", "720p"};
            ImGui::Combo("Resolution", &cfg.resolutionPreset, resolutions, 3);
            const char* fpsItems[] = {"15", "30", "60"};
            int fpsIdx = (cfg.fps == 15) ? 0 : (cfg.fps == 60) ? 2 : 1;
            if (ImGui::Combo("FPS", &fpsIdx, fpsItems, 3)) {
                cfg.fps = (fpsIdx == 0) ? 15 : (fpsIdx == 2) ? 60 : 30;
            }
            ImGui::SliderInt("Bitrate (kbps)", &cfg.bitrateKbps, 500, 20000);
        }

        // ── Privacy ────────────────────────────────────────────────────────
        ImGui::SeparatorText("Privacy masks (deterministic, applied before encode)");
        ImGui::Checkbox("Password fields (UIA)", &cfg.passwordFieldMask);
        ImGui::SameLine();
        ImGui::Checkbox("Messenger windows (LINE/Teams/...)", &cfg.notificationMask);
        const char* modes[] = {"Blackout", "Blur", "Mosaic"};
        ImGui::Combo("Mask style", &cfg.maskMode, modes, 3);

        // ── AI scene detection (advisory) ──────────────────────────────────
        ImGui::SeparatorText("AI scene detection (advisory, on-device)");
#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
        if (!aiModelPath.empty()) {
            ImGui::Checkbox("Detect scene type (CLIP ONNX, DirectML)",
                            &cfg.aiSceneDetection);
            if (cfg.aiSceneDetection) {
                ImGui::TextDisabled(
                    "Shows what the AI sees in Status below. Advisory only: "
                    "masks and encoding stay deterministic.");
            }
        } else {
            bool off = false;  // render-only; the real cfg value is preserved
            ImGui::BeginDisabled(true);
            ImGui::Checkbox("Detect scene type (model not found)", &off);
            ImGui::EndDisabled();
            if (aiEnvOverride) {
                ImGui::TextDisabled(
                    "AETHERFLOW_SCENE_CLASSIFIER_ONNX_MODEL is set but that "
                    "file was not found (checked at launch).");
            } else {
                ImGui::TextDisabled(
                    "Put scene_classifier_v1.onnx into a models\\ folder next "
                    "to the exe (checked at launch).");
            }
        }
#else
        ImGui::TextDisabled("This build has no ONNX scene classifier support.");
#endif

        // ── SRT ────────────────────────────────────────────────────────────
        ImGui::SeparatorText("SRT live stream");
#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
        ImGui::Checkbox("Stream over SRT", &cfg.srtEnabled);
        ImGui::InputInt("Port", &cfg.srtPort);
        ImGui::InputInt("Latency (ms)", &cfg.srtLatencyMs);
        {
            char pass[80] = {};
            std::snprintf(pass, sizeof(pass), "%s", cfg.srtPassphrase.c_str());
            if (ImGui::InputText("Passphrase (empty = none, else 10-79 chars)", pass,
                                 sizeof(pass), ImGuiInputTextFlags_Password)) {
                cfg.srtPassphrase = pass;
            }
        }
#else
        ImGui::TextDisabled("This build has no SRT support (run tools/fetch_ffmpeg.py and rebuild).");
#endif
        ImGui::EndDisabled();

        // ── Start / Stop / Panic ───────────────────────────────────────────
        ImGui::SeparatorText("Session");
        const bool passphraseOk = !cfg.srtEnabled || PassphraseValid(cfg.srtPassphrase);
        if (!passphraseOk) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                               "SRT passphrase must be 10-79 characters (or empty) - Start disabled.");
        }
        if (!running) {
            ImGui::BeginDisabled(!passphraseOk);
            if (ImGui::Button("Start", ImVec2(140, 34))) {
                cfg.Normalize();
                cfg.SaveToFile(IniPath());  // remember choices (spec §5)
                session.Start(cfg, monitors, /*maxFramesOverride=*/0, aiModelPath);
            }
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Stop", ImVec2(140, 34))) {
                session.RequestStop();
            }
        }
        ImGui::SameLine();
        {
            const bool panicOn = session.panicLatch.load(std::memory_order_relaxed);
            if (panicOn) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button(panicOn ? "PANIC ON (click to clear)" : "Panic (mask everything)",
                              ImVec2(220, 34))) {
                session.panicLatch.store(!panicOn, std::memory_order_relaxed);
            }
            if (panicOn) ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("global hotkey: Right Ctrl");
        }

        // ── Status ─────────────────────────────────────────────────────────
        ImGui::SeparatorText("Status");
        {
            // Every session records the encoded output + trace here (CLI
            // parity); say so instead of writing gigabytes invisibly.
            static std::string recordingDir = [] {
                wchar_t buf[MAX_PATH] = {};
                GetEnvironmentVariableW(L"AETHERFLOW_OUTPUT_DIR", buf, MAX_PATH);
                return std::filesystem::path(buf).u8string();
            }();
            ImGui::TextDisabled("Recording to: %s", recordingDir.c_str());
        }
        if (running) {
            ImGui::Text("Pipeline: RUNNING");
            ImGui::Text("Encoded frames: %llu",
                        static_cast<unsigned long long>(
                            session.status.encodedFrames.load(std::memory_order_relaxed)));
            ImGui::Text("Effective fps: %.1f",
                        session.status.effectiveFps.load(std::memory_order_relaxed));
            const bool maskOn = session.status.maskActive.load(std::memory_order_relaxed);
            const int kind = session.status.maskSource.load(std::memory_order_relaxed);
            const char* kindNames[] = {"none", "password field", "messenger window",
                                       "panic", "manual region", "AI demo", "other"};
            ImGui::Text("Masking now: %s%s%s", maskOn ? "YES (" : "no",
                        maskOn ? kindNames[(kind >= 0 && kind <= 6) ? kind : 6] : "",
                        maskOn ? ")" : "");
#if defined(AETHERFLOW_ENABLE_SCENE_CLASSIFIER)
            // AI scene indicator — the "symbol on screen" row the user asked
            // for. A per-class colored dot + label so a human can see at a
            // glance what the classifier thinks the screen is. Display only;
            // never feeds back into the pipeline.
            {
                const int aiState = session.status.sceneClassifierState.load(
                    std::memory_order_relaxed);
                if (aiState == 3) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.25f, 1.0f),
                                       "AI: failed to start - session continues without it");
                } else if (aiState > 0) {
                    const int idx = session.status.sceneClassIndex.load(
                        std::memory_order_relaxed);
                    // kSceneClassNames order: code_text, slides, video,
                    // mixed_ui, sensitive_surface.
                    static const ImVec4 kClassColors[5] = {
                        ImVec4(0.30f, 0.80f, 0.40f, 1.0f),   // code_text: green
                        ImVec4(0.30f, 0.55f, 0.90f, 1.0f),   // slides: blue
                        ImVec4(0.70f, 0.40f, 0.90f, 1.0f),   // video: purple
                        ImVec4(0.85f, 0.75f, 0.25f, 1.0f),   // mixed_ui: yellow
                        ImVec4(0.90f, 0.20f, 0.20f, 1.0f),   // sensitive: red
                    };
                    const int srcKind = session.status.sceneSourceKind.load(
                        std::memory_order_relaxed);
                    // Attribute a class to the AI only when the classifier's
                    // verdict actually holds the merge (kind 1) or a
                    // deterministic producer overrode it (kind 2). Kind 3 =
                    // baseline/low-confidence fallback — the AI has no
                    // confident verdict, so show a neutral line instead of
                    // dressing baseline up as an AI verdict (review risk 1).
                    const bool aiSpeaking = (idx >= 0 && srcKind != 3);
                    const ImVec4 dot = (aiSpeaking && idx < 5)
                                           ? kClassColors[idx]
                                           : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                    const float lh = ImGui::GetTextLineHeight();
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(p.x + lh * 0.5f, p.y + lh * 0.55f), lh * 0.38f,
                        ImGui::ColorConvertFloat4ToU32(dot));
                    ImGui::Dummy(ImVec2(lh * 1.2f, lh));
                    ImGui::SameLine();
                    if (aiSpeaking) {
                        const float conf = session.status.sceneClassConfidence.load(
                            std::memory_order_relaxed);
                        ImGui::Text("AI scene: %s (%.0f%%) via %s%s",
                                    AetherFlow::kSceneClassNames[idx],
                                    static_cast<double>(conf) * 100.0,
                                    (aiState == 2) ? "CPU" : "DirectML",
                                    (srcKind == 2) ? "  [deterministic override]" : "");
                    } else {
                        ImGui::Text("AI scene: no confident verdict yet...");
                    }
                }
            }
#endif
#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
            if (cfg.srtEnabled) {
                const bool viewer =
                    session.status.srtClientConnected.load(std::memory_order_relaxed);
                ImGui::Text("SRT: %s | viewers: %d | sent: %.1f MB",
                            viewer ? "viewer connected"
                                   : (session.status.srtListening.load(std::memory_order_relaxed)
                                          ? "listening (no viewer)"
                                          : "off"),
                            viewer ? 1 : 0,
                            static_cast<double>(session.status.srtBytesSent.load(
                                std::memory_order_relaxed)) /
                                (1024.0 * 1024.0));
            }
#endif
        } else {
            ImGui::Text("Pipeline: stopped");
            if (session.lastExitCode.load(std::memory_order_relaxed) != 0) {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Last session exit code: %d",
                                   session.lastExitCode.load(std::memory_order_relaxed));
            }
        }
#if defined(AETHERFLOW_ENABLE_SRT_OUTPUT)
        if (cfg.srtEnabled) {
            ImGui::SeparatorText("Viewer URLs (paste into VLC / ffplay on any LAN device)");
            // Cached: ShareUrls does a winsock+getaddrinfo round trip — fine
            // once, not at render rate (code-review risk 5).
            static int cachedUrlPort = -1;
            static std::vector<std::string> cachedUrls;
            if (cachedUrlPort != cfg.srtPort) {
                cachedUrls = AetherFlow::SrtStreamOutput::ShareUrls(cfg.srtPort);
                cachedUrlPort = cfg.srtPort;
            }
            for (const auto& url : cachedUrls) {
                ImGui::TextUnformatted(url.c_str());
                ImGui::SameLine();
                ImGui::PushID(url.c_str());
                if (ImGui::SmallButton("copy")) {
                    ImGui::SetClipboardText(url.c_str());
                }
                ImGui::PopID();
            }
            ImGui::TextDisabled("First picture ~2 s after connect (keyframe wait). "
                                "High delay in VLC? Lower its network-caching to ~200 ms.");
        }
#endif

        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_mainRtv, nullptr);
        g_context->ClearRenderTargetView(g_mainRtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);

        if (smokeFramesLeft > 0 && --smokeFramesLeft == 0) {
            done = true;  // --ui-smoke: proved we can init + render; exit clean
        }
    }

    // Window closing stops any live session through the pipeline's normal
    // clean-shutdown path (flush, summaries) before teardown.
    session.StopAndJoin();
    cfg.Normalize();
    cfg.SaveToFile(IniPath());

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (uiSelftest) {
        return (selftestResult == 0) ? 0 : 1;
    }
    return 0;
}
