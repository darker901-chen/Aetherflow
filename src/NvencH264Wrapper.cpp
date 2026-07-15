#include "AetherFlow/NvencH264Wrapper.h"
#include "AetherFlow/Config.h"
#include "AetherFlow/IEncodedFrameSink.h"
#include "AetherFlow/NvencRoiDefaults.h"

#include <Windows.h>
#include <dxgi1_2.h>
#include <d3d11_4.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

constexpr UINT kNvidiaVendorId = 0x10DE;
constexpr uint32_t kNumBuffers = 4;

std::filesystem::path GetExeDir() {
    char buf[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0 || len >= sizeof(buf)) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::string(buf, buf + len)).parent_path();
}

std::filesystem::path GetOutputDir() {
    if (const char* env = std::getenv("AETHERFLOW_OUTPUT_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    return GetExeDir() / ".." / ".." / "output";
}

} // namespace

NvencH264Wrapper::NvencH264Wrapper() {}

NvencH264Wrapper::~NvencH264Wrapper() {
    Cleanup();
}

bool NvencH264Wrapper::Initialize(int width, int height, int fps) {
    Cleanup();

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_mbWidth = (m_width + 15) / 16;
    m_mbHeight = (m_height + 15) / 16;
    m_qpDeltaMap.assign(static_cast<size_t>(m_mbWidth) * static_cast<size_t>(m_mbHeight), 0);
    m_frameIndex = 0;
    m_telemetryStartTs = std::chrono::steady_clock::now();
    m_captureTimelineInitialized = false;
    m_captureTimelineBaseUs = 0;

    m_dropCount = 0;
    m_encoderBusyCount = 0;
    m_lockBusyCount = 0;
    m_mapFailureCount = 0;
    m_unmapFailureCount = 0;
    m_maxInflightSlotsObserved = 0;
    m_encodedFrameCount = 0;
    m_dropReasonCounts.fill(0);
    m_latencySampleCount = 0;
    m_submitAgeSumMs = 0.0;
    m_outputAgeSumMs = 0.0;
    m_queueSumMs = 0.0;
    m_submitAgeMinMs = (std::numeric_limits<double>::max)();
    m_submitAgeMaxMs = 0.0;
    m_outputAgeMinMs = (std::numeric_limits<double>::max)();
    m_outputAgeMaxMs = 0.0;
    m_queueMinMs = (std::numeric_limits<double>::max)();
    m_queueMaxMs = 0.0;

    bool telemetryLog = m_frameTelemetryLogEnabled;
    if (const char* telemetryEnv = std::getenv("AETHERFLOW_NVENC_FRAME_TELEMETRY")) {
        telemetryLog = (std::strcmp(telemetryEnv, "0") != 0);
    }
    m_frameTelemetryLogEnabled = telemetryLog;

    bool telemetryVerbose = m_frameTelemetryVerbose;
    if (const char* telemetryVerboseEnv = std::getenv("AETHERFLOW_NVENC_FRAME_TELEMETRY_VERBOSE")) {
        telemetryVerbose = (std::strcmp(telemetryVerboseEnv, "0") != 0);
    }
    m_frameTelemetryVerbose = telemetryVerbose;

    if (const char* telemetryIntervalEnv = std::getenv("AETHERFLOW_NVENC_FRAME_TELEMETRY_INTERVAL")) {
        const int parsed = std::atoi(telemetryIntervalEnv);
        if (parsed > 0) {
            m_frameTelemetryLogInterval = static_cast<uint32_t>(parsed);
        }
    }

    if (const char* telemetryAlertEnv = std::getenv("AETHERFLOW_NVENC_FRAME_TELEMETRY_ALERT_MS")) {
        const double parsed = std::atof(telemetryAlertEnv);
        if (parsed > 0.0) {
            m_frameTelemetryAlertMs = parsed;
        }
    }

    if (const char* writeBitstreamEnv = std::getenv("AETHERFLOW_NVENC_WRITE_BITSTREAM")) {
        m_writeBitstreamEnabled = (std::strcmp(writeBitstreamEnv, "0") != 0);
    }

    m_timedRecording = false;
    if (const char* timedEnv = std::getenv("AETHERFLOW_TIMED_RECORDING")) {
        m_timedRecording = (std::strcmp(timedEnv, "0") != 0);
    }
    m_havePtsBase = false;
    m_ptsBase100ns = 0;
    m_pendingCaptureStamp100ns = 0;
    m_lastSidecarMs = 0.0;

    if (!CreateNvidiaDevice()) {
        std::cerr << "[NVENC] Failed to create NVIDIA D3D11 device.\n";
        return false;
    }

    if (!LoadNvencApi()) {
        std::cerr << "[NVENC] Failed to load NVENC API.\n";
        return false;
    }

    if (!OpenEncoderSession()) {
        std::cerr << "[NVENC] Failed to open NVENC session.\n";
        return false;
    }

    if (!InitializeEncoder()) {
        std::cerr << "[NVENC] Failed to initialize encoder.\n";
        return false;
    }

    if (!CreateBuffers()) {
        std::cerr << "[NVENC] Failed to create output buffers.\n";
        return false;
    }

    if (m_writeBitstreamEnabled) {
        auto outDir = GetOutputDir();
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        auto outPath = outDir / "output_encoded.h264";
        m_outputFile.open(outPath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!m_outputFile.is_open()) {
            std::cerr << "[NVENC] Failed to open output file: " << outPath.string() << "\n";
            return false;
        }

        if (m_timedRecording) {
            auto sidecarPath = outDir / "output_encoded.timestamps.txt";
            m_sidecarFile.open(sidecarPath, std::ios::out | std::ios::trunc);
            if (!m_sidecarFile.is_open()) {
                std::cerr << "[NVENC] Failed to open PTS sidecar: " << sidecarPath.string() << "\n";
                return false;
            }
            // mkvmerge timecodes v2: header then one ms timestamp per frame.
            m_sidecarFile << "# timestamp format v2\n";
            std::cout << "[NVENC] Timed recording ON: PTS sidecar -> "
                      << sidecarPath.string() << "\n";
        }
    }

    if (!StartWriterThread()) {
        std::cerr << "[NVENC] Failed to start bitstream writer thread.\n";
        return false;
    }

    if (!StartDrainThread()) {
        std::cerr << "[NVENC] Failed to start drain thread.\n";
        return false;
    }

    std::cout << "[NVENC] Initialized (" << m_width << "x" << m_height << " @ " << m_fps << ")\n";
    std::cout << "[NVENC] ROI QP map configured: radius=" << m_roiRadius
              << "px, roiDeltaQP=" << m_roiDeltaQp
              << ", bgDeltaQP=" << m_bgDeltaQp
              << " (applied only when cursor ROI is enabled)\n";
    std::cout << "[NVENC] Full-GPU input path: encoder-owned registered D3D11 NV12 texture pool\n";
    std::cout << "[NVENC] Drain path: background thread locks bitstream and releases input slots\n";
    std::cout << "[NVENC] inputPitch policy: use inputWidth when GPU texture row pitch is not queryable "
              << "(per NV_ENC_PIC_PARAMS documentation).\n";
    std::cout << "[NVENC] Frame telemetry: " << (m_frameTelemetryLogEnabled ? "enabled" : "disabled")
              << " (interval=" << m_frameTelemetryLogInterval
              << ", alertMs=" << m_frameTelemetryAlertMs
              << ", verbose=" << (m_frameTelemetryVerbose ? "1" : "0") << ")\n";
    std::cout << "[NVENC] Bitstream write: " << (m_writeBitstreamEnabled ? "enabled" : "disabled")
              << " (code default)\n";
    std::cout << "[NVENC] Encode mode: " << (m_asyncEncode ? "asynchronous" : "synchronous fallback") << "\n";
    return true;
}

bool NvencH264Wrapper::CreateNvidiaDevice() {
    CComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        return false;
    }

    CComPtr<IDXGIAdapter1> nvidiaAdapter;
    for (UINT i = 0;; i++) {
        CComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if (desc.VendorId == kNvidiaVendorId) {
            nvidiaAdapter = adapter;
            break;
        }
    }

    if (!nvidiaAdapter) {
        return false;
    }

    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(
        nvidiaAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &m_pDevice,
        nullptr,
        &m_pContext);
    if (FAILED(hr) || !m_pDevice || !m_pContext) {
        return false;
    }

    CComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(m_pContext->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
    }

    return true;
}

bool NvencH264Wrapper::LoadNvencApi() {
    m_nvencDll = LoadLibraryA("nvEncodeAPI64.dll");
    if (!m_nvencDll) {
        return false;
    }

    using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance = reinterpret_cast<CreateInstanceFn>(
        GetProcAddress(m_nvencDll, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        return false;
    }

    memset(&m_api, 0, sizeof(m_api));
    m_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = createInstance(&m_api);
    if (st != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] NvEncodeAPICreateInstance failed: " << StatusToString(st) << "\n";
        return false;
    }

    return true;
}

bool NvencH264Wrapper::OpenEncoderSession() {
    if (!m_api.nvEncOpenEncodeSessionEx) {
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = {};
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    openParams.device = m_pDevice;
    openParams.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_api.nvEncOpenEncodeSessionEx(&openParams, &m_encoder);
    if (st != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncOpenEncodeSessionEx failed: " << StatusToString(st) << "\n";
        return false;
    }

    return true;
}

bool NvencH264Wrapper::InitializeEncoder() {
    if (!m_api.nvEncInitializeEncoder ||
        !m_api.nvEncCreateBitstreamBuffer ||
        !m_api.nvEncRegisterResource ||
        !m_api.nvEncMapInputResource ||
        !m_api.nvEncUnmapInputResource ||
        !m_api.nvEncUnregisterResource) {
        std::cerr << "[NVENC] Missing required API entry points for D3D11 zero-copy path.\n";
        return false;
    }

    NV_ENC_PRESET_CONFIG presetConfig = {};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS st = NV_ENC_ERR_GENERIC;
    if (m_api.nvEncGetEncodePresetConfigEx) {
        st = m_api.nvEncGetEncodePresetConfigEx(
            m_encoder,
            NV_ENC_CODEC_H264_GUID,
            NV_ENC_PRESET_P4_GUID,
            NV_ENC_TUNING_INFO_LOW_LATENCY,
            &presetConfig);
    } else if (m_api.nvEncGetEncodePresetConfig) {
        st = m_api.nvEncGetEncodePresetConfig(
            m_encoder,
            NV_ENC_CODEC_H264_GUID,
            NV_ENC_PRESET_P4_GUID,
            &presetConfig);
    }
    if (st != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] Failed to query preset config: " << StatusToString(st) << "\n";
        return false;
    }

    if (m_api.nvEncGetEncodeCaps) {
        NV_ENC_CAPS_PARAM capsParam = {};
        capsParam.version = NV_ENC_CAPS_PARAM_VER;
        capsParam.capsToQuery = NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP;
        int capsValue = 0;
        NVENCSTATUS capsSt = m_api.nvEncGetEncodeCaps(
            m_encoder,
            NV_ENC_CODEC_H264_GUID,
            &capsParam,
            &capsValue);
        if (capsSt == NV_ENC_SUCCESS) {
            std::cout << "[NVENC] CAPS_SUPPORT_EMPHASIS_LEVEL_MAP: " << capsValue << "\n";
        } else {
            std::cout << "[NVENC] CAPS query failed: " << StatusToString(capsSt) << "\n";
        }
    }

    const int bitrateKbps = (m_targetBitrateKbps > 0) ? m_targetBitrateKbps : AETHERFLOW_BITRATE;

    m_encConfig = presetConfig.presetCfg;
    m_encConfig.version = NV_ENC_CONFIG_VER;
    m_encConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    m_encConfig.gopLength = static_cast<uint32_t>(m_fps * AETHERFLOW_GOP_SECONDS);
    m_encConfig.frameIntervalP = 1;
    m_encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    m_encConfig.rcParams.averageBitRate = static_cast<uint32_t>(bitrateKbps) * 1000;
    m_encConfig.rcParams.maxBitRate = static_cast<uint32_t>(bitrateKbps) * 1000;
    m_encConfig.rcParams.vbvBufferSize = static_cast<uint32_t>(bitrateKbps) * 1000;
    m_encConfig.rcParams.vbvInitialDelay = static_cast<uint32_t>(bitrateKbps) * 500;
    m_encConfig.rcParams.enableAQ = 0;
    m_encConfig.rcParams.enableTemporalAQ = 0;
    m_encConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;

    // Default to synchronous mode for deterministic latency telemetry.
    // Async mode can improve throughput but may inflate queueing jitter.
    bool wantAsync = false;
    m_asyncEncode = wantAsync && m_api.nvEncRegisterAsyncEvent && m_api.nvEncUnregisterAsyncEvent;

    NV_ENC_INITIALIZE_PARAMS initParams = {};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P4_GUID;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    initParams.encodeWidth = static_cast<uint32_t>(m_width);
    initParams.encodeHeight = static_cast<uint32_t>(m_height);
    initParams.darWidth = static_cast<uint32_t>(m_width);
    initParams.darHeight = static_cast<uint32_t>(m_height);
    initParams.frameRateNum = static_cast<uint32_t>(m_fps);
    initParams.frameRateDen = 1;
    initParams.enableEncodeAsync = m_asyncEncode ? 1u : 0u;
    initParams.enablePTD = 1;
    initParams.encodeConfig = &m_encConfig;

    st = m_api.nvEncInitializeEncoder(m_encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncInitializeEncoder failed: " << StatusToString(st) << "\n";
        return false;
    }

    std::cout << "[NVENC] Target bitrate: " << bitrateKbps << " kbps\n";
    return true;
}

bool NvencH264Wrapper::CreateBuffers() {
    if (!m_api.nvEncCreateBitstreamBuffer || !m_api.nvEncDestroyBitstreamBuffer) {
        return false;
    }
    if (m_asyncEncode && (!m_api.nvEncRegisterAsyncEvent || !m_api.nvEncUnregisterAsyncEvent)) {
        return false;
    }

    m_buffers.clear();
    m_registeredInputs.clear();
    m_buffers.reserve(kNumBuffers);
    m_registeredInputs.reserve(kNumBuffers);

    D3D11_TEXTURE2D_DESC inputDesc = {};
    inputDesc.Width = static_cast<UINT>(m_width);
    inputDesc.Height = static_cast<UINT>(m_height);
    inputDesc.MipLevels = 1;
    inputDesc.ArraySize = 1;
    inputDesc.Format = DXGI_FORMAT_NV12;
    inputDesc.SampleDesc.Count = 1;
    inputDesc.Usage = D3D11_USAGE_DEFAULT;
    inputDesc.BindFlags = 0;
    inputDesc.CPUAccessFlags = 0;
    inputDesc.MiscFlags = 0;

    for (uint32_t i = 0; i < kNumBuffers; ++i) {
        BufferPair pair = {};
        pair.inputIndex = i;

        NV_ENC_CREATE_BITSTREAM_BUFFER out = {};
        out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        NVENCSTATUS st = m_api.nvEncCreateBitstreamBuffer(m_encoder, &out);
        if (st != NV_ENC_SUCCESS || !out.bitstreamBuffer) {
            std::cerr << "[NVENC] nvEncCreateBitstreamBuffer failed: " << StatusToString(st) << "\n";
            return false;
        }
        pair.output = out.bitstreamBuffer;

        RegisteredInput input = {};
        HRESULT hr = m_pDevice->CreateTexture2D(&inputDesc, nullptr, &input.texture);
        if (FAILED(hr) || !input.texture) {
            std::cerr << "[NVENC] Failed to create input pool texture.\n";
            return false;
        }

        NV_ENC_REGISTER_RESOURCE reg = {};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = static_cast<uint32_t>(m_width);
        reg.height = static_cast<uint32_t>(m_height);
        reg.pitch = 0;
        reg.subResourceIndex = 0;
        reg.resourceToRegister = input.texture;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        st = m_api.nvEncRegisterResource(m_encoder, &reg);
        if (st != NV_ENC_SUCCESS || !reg.registeredResource) {
            std::cerr << "[NVENC] nvEncRegisterResource(input pool) failed: " << StatusToString(st) << "\n";
            return false;
        }
        input.handle = reg.registeredResource;

        if (m_asyncEncode) {
            pair.completionEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!pair.completionEvent) {
                std::cerr << "[NVENC] CreateEvent failed for completion event.\n";
                return false;
            }

            NV_ENC_EVENT_PARAMS ev = {};
            ev.version = NV_ENC_EVENT_PARAMS_VER;
            ev.completionEvent = pair.completionEvent;
            st = m_api.nvEncRegisterAsyncEvent(m_encoder, &ev);
            if (st != NV_ENC_SUCCESS) {
                std::cerr << "[NVENC] nvEncRegisterAsyncEvent failed: " << StatusToString(st) << "\n";
                return false;
            }
        }

        m_registeredInputs.push_back(input);
        m_buffers.push_back(pair);
    }

    m_frameIndex = 0;
    return true;
}

bool NvencH264Wrapper::StartDrainThread() {
    StopDrainThread();

    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_drainThreadStop = false;
    }

    m_drainThread = std::thread([this]() {
        while (true) {
            bool retired = false;
            bool anyInFlight = false;
            {
                std::unique_lock<std::mutex> lk(m_stateMutex);
                if (m_drainThreadStop) {
                    break;
                }

                for (auto& buffer : m_buffers) {
                    if (!buffer.inFlight) {
                        continue;
                    }
                    if (!DrainSlot(buffer, false)) {
                        buffer.eos = false;
                        buffer.inFlight = false;
                        buffer.reserved = false;
                        buffer.mappedInput = nullptr;
                        buffer.telemetry = {};
                        retired = true;
                    } else if (!buffer.inFlight) {
                        // DrainSlot retired this frame (cleared inFlight).
                        retired = true;
                    } else {
                        // Still encoding (LOCK_BUSY); back off below instead
                        // of hot-spinning nvEncLockBitstream().
                        anyInFlight = true;
                    }
                }
            }

            if (retired) {
                // A slot was freed: bitstream was already handed to the
                // writer thread inside DrainSlot. Just wake the producer and
                // re-poll immediately. The drain thread NEVER touches disk.
                m_inputAvailableCv.notify_all();
                continue;
            }

            std::unique_lock<std::mutex> lk(m_stateMutex);
            if (m_drainThreadStop) {
                break;
            }
            if (anyInFlight) {
                // Synchronous NVENC has no completion signal. Poll on a short
                // fixed backoff so the drain thread does not burn a core and
                // thrash m_stateMutex while a frame is on the GPU.
                m_inputAvailableCv.wait_for(lk, std::chrono::milliseconds(1),
                                            [this]() { return m_drainThreadStop; });
            } else {
                // Nothing in flight: sleep until a new frame is submitted.
                m_inputAvailableCv.wait(lk, [this]() {
                    if (m_drainThreadStop) {
                        return true;
                    }
                    for (const auto& buffer : m_buffers) {
                        if (buffer.inFlight) {
                            return true;
                        }
                    }
                    return false;
                });
            }
        }
    });

    return m_drainThread.joinable();
}

void NvencH264Wrapper::StopDrainThread() {
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_drainThreadStop = true;
    }
    m_inputAvailableCv.notify_all();
    if (m_drainThread.joinable()) {
        m_drainThread.join();
    }
}

int NvencH264Wrapper::FindRegisteredInputIndex(ID3D11Texture2D* texture) const {
    if (!texture) {
        return -1;
    }

    for (size_t i = 0; i < m_registeredInputs.size(); ++i) {
        if (m_registeredInputs[i].texture == texture) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool NvencH264Wrapper::AcquireInputTexture(ID3D11Texture2D** texture) {
    if (!texture) {
        return false;
    }
    *texture = nullptr;

    std::unique_lock<std::mutex> lk(m_stateMutex);
    m_inputAvailableCv.wait(lk, [this]() {
        if (m_drainThreadStop || m_buffers.empty()) {
            return true;
        }
        for (const auto& buffer : m_buffers) {
            if (!buffer.reserved && !buffer.inFlight) {
                return true;
            }
        }
        return false;
    });

    if (m_drainThreadStop || m_buffers.empty()) {
        return false;
    }

    for (size_t i = 0; i < m_buffers.size(); ++i) {
        auto& buffer = m_buffers[i];
        if (buffer.reserved || buffer.inFlight) {
            continue;
        }
        if (i >= m_registeredInputs.size() || !m_registeredInputs[i].texture) {
            return false;
        }
        buffer.reserved = true;
        *texture = m_registeredInputs[i].texture;
        return true;
    }

    return false;
}

void NvencH264Wrapper::ReleaseInputTexture(ID3D11Texture2D* texture) {
    const int idx = FindRegisteredInputIndex(texture);
    if (idx < 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        if (static_cast<size_t>(idx) < m_buffers.size() && !m_buffers[idx].inFlight) {
            m_buffers[idx].reserved = false;
            m_buffers[idx].telemetry = {};
        }
    }
    m_inputAvailableCv.notify_all();
}

uint64_t NvencH264Wrapper::NowTelemetryUs() const {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - m_telemetryStartTs).count());
}

uint64_t NvencH264Wrapper::CaptureTimestampUsFromElapsed(double elapsedSeconds, uint64_t convertDoneUs) {
    const double elapsedUsDouble = (std::max)(0.0, elapsedSeconds) * 1000000.0;
    const uint64_t elapsedUs = static_cast<uint64_t>(elapsedUsDouble);

    if (!m_captureTimelineInitialized) {
        m_captureTimelineBaseUs = (convertDoneUs > elapsedUs) ? (convertDoneUs - elapsedUs) : 0;
        m_captureTimelineInitialized = true;
    }

    return m_captureTimelineBaseUs + elapsedUs;
}

void NvencH264Wrapper::UpdateInflightStats() {
    uint64_t inFlight = 0;
    for (const auto& buffer : m_buffers) {
        if (buffer.inFlight) {
            ++inFlight;
        }
    }
    m_maxInflightSlotsObserved = (std::max)(m_maxInflightSlotsObserved, inFlight);
}

const char* NvencH264Wrapper::DropReasonToString(DropReason reason) const {
    switch (reason) {
    case DropReason::SlotBusyAfterPreDrain: return "slot_busy_after_pre_drain";
    case DropReason::EncoderBusy: return "encoder_busy";
    case DropReason::MapFailed: return "map_failed";
    case DropReason::InvalidState: return "invalid_state";
    case DropReason::InvalidInput: return "invalid_input";
    default: return "unknown";
    }
}

void NvencH264Wrapper::LogDrop(DropReason reason, uint32_t slot, uint32_t frameIndex, const char* detail) {
    ++m_dropCount;
    const size_t reasonIdx = static_cast<size_t>(reason);
    if (reasonIdx < m_dropReasonCounts.size()) {
        ++m_dropReasonCounts[reasonIdx];
    }

    std::cerr << "[NVENC][DROP] frame=" << frameIndex
              << " slot=" << slot
              << " reason=" << DropReasonToString(reason)
              << " dropCount=" << m_dropCount;
    if (detail && *detail) {
        std::cerr << " detail=" << detail;
    }
    std::cerr << "\n";
}

bool NvencH264Wrapper::ShouldLogFrameTelemetry(const FrameTelemetry& telemetry) const {
    if (!m_frameTelemetryLogEnabled || !telemetry.valid) {
        return false;
    }

    const uint32_t interval = m_frameTelemetryLogInterval == 0u ? 1u : m_frameTelemetryLogInterval;
    if ((telemetry.frameIndex % interval) == 0u) {
        return true;
    }

    return telemetry.ageAtSubmitMs >= m_frameTelemetryAlertMs ||
           telemetry.ageAtOutputMs >= m_frameTelemetryAlertMs;
}

void NvencH264Wrapper::LogFrameTelemetry(const FrameTelemetry& telemetry) const {
    if (!telemetry.valid) {
        return;
    }

    const double queueMs = telemetry.ageAtOutputMs >= telemetry.ageAtSubmitMs
        ? (telemetry.ageAtOutputMs - telemetry.ageAtSubmitMs)
        : 0.0;
    const double targetFrameMs = (m_fps > 0) ? (1000.0 / static_cast<double>(m_fps)) : 33.33;
    const double submitHeadroomMs = targetFrameMs - telemetry.ageAtSubmitMs;
    const double e2eHeadroomMs = targetFrameMs - telemetry.ageAtOutputMs;

    const auto oldFlags = std::cout.flags();
    const auto oldPrecision = std::cout.precision();
    std::cout << std::fixed << std::setprecision(2);

    if (m_frameTelemetryVerbose) {
        std::cout << "[NVENC][TEL][V] Frame " << std::setw(4) << telemetry.frameIndex
                  << " | Cap90k: " << telemetry.captureTimestamp90k
                  << " | CaptureUs: " << telemetry.captureTimestampUs
                  << " | ConvertDoneUs: " << telemetry.convertDoneTimestampUs
                  << " | SubmitUs: " << telemetry.encodeSubmitTimestampUs
                  << " | OutputUs: " << telemetry.encodeOutputTimestampUs
                  << " | SubmitAge: " << std::setw(6) << telemetry.ageAtSubmitMs << "ms"
                  << " | OutputAge: " << std::setw(6) << telemetry.ageAtOutputMs << "ms"
                  << " | Queue: " << std::setw(6) << queueMs << "ms"
                  << " | SubmitHeadroom: " << std::setw(6) << submitHeadroomMs << "ms"
                  << " | E2EHeadroom: " << std::setw(6) << e2eHeadroomMs << "ms"
                  << "\n";
        std::cout.flags(oldFlags);
        std::cout.precision(oldPrecision);
        return;
    }

    std::cout << "[NVENC][TEL] Frame " << std::setw(4) << telemetry.frameIndex
              << " | SubmitAge: " << std::setw(6) << telemetry.ageAtSubmitMs << "ms"
              << " | OutputAge: " << std::setw(6) << telemetry.ageAtOutputMs << "ms"
              << " | Queue: " << std::setw(6) << queueMs << "ms"
              << " | SubmitHeadroom: " << std::setw(6) << submitHeadroomMs << "ms"
              << " | E2EHeadroom: " << std::setw(6) << e2eHeadroomMs << "ms"
              << "\n";
    std::cout.flags(oldFlags);
    std::cout.precision(oldPrecision);
}

void NvencH264Wrapper::LogStats(const char* phase) const {
    std::cout << "[NVENC][STATS] phase=" << (phase ? phase : "unknown")
              << " encodedFrameCount=" << m_encodedFrameCount
              << " dropCount=" << m_dropCount
              << " encoderBusyCount=" << m_encoderBusyCount
              << " lockBusyCount=" << m_lockBusyCount
              << " mapFailures=" << m_mapFailureCount
              << " unmapFailures=" << m_unmapFailureCount
              << " maxInflightSlotsObserved=" << m_maxInflightSlotsObserved
              << "\n";

    std::cout << "[NVENC][STATS] dropReasons"
              << " slot_busy_after_pre_drain=" << m_dropReasonCounts[static_cast<size_t>(DropReason::SlotBusyAfterPreDrain)]
              << " encoder_busy=" << m_dropReasonCounts[static_cast<size_t>(DropReason::EncoderBusy)]
              << " map_failed=" << m_dropReasonCounts[static_cast<size_t>(DropReason::MapFailed)]
              << " invalid_state=" << m_dropReasonCounts[static_cast<size_t>(DropReason::InvalidState)]
              << " invalid_input=" << m_dropReasonCounts[static_cast<size_t>(DropReason::InvalidInput)]
              << "\n";

    if (m_latencySampleCount > 0) {
        const double sampleCount = static_cast<double>(m_latencySampleCount);
        const double avgSubmitMs = m_submitAgeSumMs / sampleCount;
        const double avgOutputMs = m_outputAgeSumMs / sampleCount;
        const double avgQueueMs = m_queueSumMs / sampleCount;
        const double targetFrameMs = (m_fps > 0) ? (1000.0 / static_cast<double>(m_fps)) : 33.33;
        const double avgSubmitHeadroomMs = targetFrameMs - avgSubmitMs;
        const double avgE2EHeadroomMs = targetFrameMs - avgOutputMs;

        const auto oldFlags = std::cout.flags();
        const auto oldPrecision = std::cout.precision();
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "[NVENC][LAT] samples=" << m_latencySampleCount
                  << " submitAge(avg/min/max)=" << avgSubmitMs << "/" << m_submitAgeMinMs << "/" << m_submitAgeMaxMs << "ms"
                  << " outputAge(avg/min/max)=" << avgOutputMs << "/" << m_outputAgeMinMs << "/" << m_outputAgeMaxMs << "ms"
                  << " queue(avg/min/max)=" << avgQueueMs << "/" << m_queueMinMs << "/" << m_queueMaxMs << "ms"
                  << "\n";
        std::cout << "[NVENC][LAT] targetFrameMs=" << targetFrameMs
                  << " avgSubmitHeadroomMs=" << avgSubmitHeadroomMs
                  << " avgE2EHeadroomMs=" << avgE2EHeadroomMs
                  << "\n";
        std::cout.flags(oldFlags);
        std::cout.precision(oldPrecision);
    }
}

bool NvencH264Wrapper::UnmapInputResourceChecked(NV_ENC_INPUT_PTR mappedInput, const char* context) {
    if (!mappedInput) {
        return true;
    }
    if (!m_api.nvEncUnmapInputResource || !m_encoder) {
        ++m_unmapFailureCount;
        std::cerr << "[NVENC] nvEncUnmapInputResource unavailable";
        if (context && *context) {
            std::cerr << " (" << context << ")";
        }
        std::cerr << ".\n";
        return false;
    }

    const NVENCSTATUS st = m_api.nvEncUnmapInputResource(m_encoder, mappedInput);
    if (st != NV_ENC_SUCCESS) {
        ++m_unmapFailureCount;
        std::cerr << "[NVENC] nvEncUnmapInputResource failed";
        if (context && *context) {
            std::cerr << " (" << context << ")";
        }
        std::cerr << ": " << StatusToString(st) << "\n";
        return false;
    }
    return true;
}

bool NvencH264Wrapper::DrainSlot(BufferPair& buffer, bool waitForEvent) {
    if (!buffer.inFlight) {
        if (buffer.mappedInput) {
            if (!UnmapInputResourceChecked(buffer.mappedInput, "idle cleanup")) {
                return false;
            }
            buffer.mappedInput = nullptr;
        }
        return true;
    }

    if (m_asyncEncode && buffer.completionEvent) {
        const DWORD waitMs = waitForEvent ? 5000u : 0u;
        const DWORD wr = WaitForSingleObject(buffer.completionEvent, waitMs);
        if (wr == WAIT_TIMEOUT) {
            if (!waitForEvent) {
                return true;
            }
            std::cerr << "[NVENC] Timeout waiting for encode completion event.\n";
            return false;
        }
        if (wr != WAIT_OBJECT_0) {
            std::cerr << "[NVENC] WaitForSingleObject failed while draining bitstream.\n";
            return false;
        }
    }

    NV_ENC_LOCK_BITSTREAM lockBit = {};
    lockBit.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBit.outputBitstream = buffer.output;
    lockBit.doNotWait = waitForEvent ? 0u : 1u;

    NVENCSTATUS st = m_api.nvEncLockBitstream(m_encoder, &lockBit);
    if (!waitForEvent && (st == NV_ENC_ERR_LOCK_BUSY || st == NV_ENC_ERR_OUT_OF_MEMORY)) {
        ++m_lockBusyCount;
        return true;
    }
    if (st != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncLockBitstream failed: " << StatusToString(st) << "\n";
        return false;
    }

    if (m_outputFile.is_open() && lockBit.bitstreamSizeInBytes > 0 && lockBit.bitstreamBufferPtr) {
        EnqueueBitstream(
            reinterpret_cast<const char*>(lockBit.bitstreamBufferPtr),
            lockBit.bitstreamSizeInBytes);
        if (m_timedRecording && !buffer.eos) {
            // Exactly one sidecar PTS per access unit written to the .h264,
            // so the timecodes stay 1:1 with encoded frames across drops.
            EnqueueSidecarPts(buffer.telemetry.captureStamp100ns);
        }
    }

    // SRT/TS live sink tap. Deliberately NOT gated on m_outputFile so
    // AETHERFLOW_NVENC_WRITE_BITSTREAM=0 still streams. Runs on the drain
    // thread (never the producer); the sink copies into its bounded queue and
    // returns without blocking. outputTimeStamp round-trips the 90 kHz
    // inputTimeStamp90k submitted by EncodeFromYUVWithROI.
    if (m_encodedSink && !buffer.eos &&
        lockBit.bitstreamSizeInBytes > 0 && lockBit.bitstreamBufferPtr) {
        AetherFlow::EncodedAccessUnit au;
        au.data = static_cast<const uint8_t*>(lockBit.bitstreamBufferPtr);
        au.size = lockBit.bitstreamSizeInBytes;
        au.pts90k = static_cast<uint64_t>(lockBit.outputTimeStamp);
        au.keyframe = (lockBit.pictureType == NV_ENC_PIC_TYPE_IDR) ||
                      (lockBit.pictureType == NV_ENC_PIC_TYPE_I);
        m_encodedSink->OnEncodedAccessUnit(au);
    }

    st = m_api.nvEncUnlockBitstream(m_encoder, buffer.output);
    if (st != NV_ENC_SUCCESS) {
        std::cerr << "[NVENC] nvEncUnlockBitstream failed: " << StatusToString(st) << "\n";
        return false;
    }

    if (buffer.mappedInput) {
        if (!UnmapInputResourceChecked(buffer.mappedInput, "drain slot")) {
            return false;
        }
        buffer.mappedInput = nullptr;
    }

    if (!buffer.eos) {
        ++m_encodedFrameCount;
    }
    if (buffer.telemetry.valid) {
        buffer.telemetry.encodeOutputTimestampUs = NowTelemetryUs();
        if (buffer.telemetry.encodeOutputTimestampUs >= buffer.telemetry.captureTimestampUs) {
            buffer.telemetry.ageAtOutputMs =
                static_cast<double>(buffer.telemetry.encodeOutputTimestampUs - buffer.telemetry.captureTimestampUs) / 1000.0;
        } else {
            buffer.telemetry.ageAtOutputMs = 0.0;
        }
        const double queueMs = buffer.telemetry.ageAtOutputMs >= buffer.telemetry.ageAtSubmitMs
            ? (buffer.telemetry.ageAtOutputMs - buffer.telemetry.ageAtSubmitMs)
            : 0.0;
        ++m_latencySampleCount;
        m_submitAgeSumMs += buffer.telemetry.ageAtSubmitMs;
        m_outputAgeSumMs += buffer.telemetry.ageAtOutputMs;
        m_queueSumMs += queueMs;
        m_submitAgeMinMs = (std::min)(m_submitAgeMinMs, buffer.telemetry.ageAtSubmitMs);
        m_submitAgeMaxMs = (std::max)(m_submitAgeMaxMs, buffer.telemetry.ageAtSubmitMs);
        m_outputAgeMinMs = (std::min)(m_outputAgeMinMs, buffer.telemetry.ageAtOutputMs);
        m_outputAgeMaxMs = (std::max)(m_outputAgeMaxMs, buffer.telemetry.ageAtOutputMs);
        m_queueMinMs = (std::min)(m_queueMinMs, queueMs);
        m_queueMaxMs = (std::max)(m_queueMaxMs, queueMs);
        if (ShouldLogFrameTelemetry(buffer.telemetry)) {
            LogFrameTelemetry(buffer.telemetry);
        }
    }

    buffer.telemetry = {};
    buffer.eos = false;
    buffer.inFlight = false;
    buffer.reserved = false;
    UpdateInflightStats();
    return true;
}

void NvencH264Wrapper::EnqueueBitstream(const char* data, size_t count) {
    if (!data || count == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_writerMutex);
        m_writerBuf.insert(m_writerBuf.end(), data, data + count);
    }
    m_writerCv.notify_one();
}

void NvencH264Wrapper::EnqueueSidecarPts(int64_t captureStamp100ns) {
    if (!m_timedRecording || !m_sidecarFile.is_open()) {
        return;
    }
    // m_havePtsBase / m_ptsBase100ns / m_lastSidecarMs are touched only on the
    // single logical drain path (drain thread, or single-threaded flush).
    if (captureStamp100ns > 0 && !m_havePtsBase) {
        m_ptsBase100ns = captureStamp100ns;
        m_havePtsBase = true;
    }
    double ms = m_lastSidecarMs;
    if (m_havePtsBase && captureStamp100ns > 0) {
        ms = static_cast<double>(captureStamp100ns - m_ptsBase100ns) / 10000.0;
    }
    if (ms < m_lastSidecarMs) {
        ms = m_lastSidecarMs;  // mkvmerge timecodes v2 must be non-decreasing
    }
    m_lastSidecarMs = ms;

    char line[32];
    int n = std::snprintf(line, sizeof(line), "%.3f\n", ms);
    if (n <= 0 || n >= static_cast<int>(sizeof(line))) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_writerMutex);
        m_writerSidecar.append(line, static_cast<size_t>(n));
    }
    m_writerCv.notify_one();
}

bool NvencH264Wrapper::StartWriterThread() {
    StopWriterThread();
    {
        std::lock_guard<std::mutex> lk(m_writerMutex);
        m_writerStop = false;
        m_writerBuf.clear();
    }
    m_writerThread = std::thread([this]() {
        while (true) {
            std::vector<char> bytes;
            std::string sidecar;
            {
                std::unique_lock<std::mutex> lk(m_writerMutex);
                m_writerCv.wait(lk, [this]() {
                    return m_writerStop || !m_writerBuf.empty() || !m_writerSidecar.empty();
                });
                if (m_writerBuf.empty() && m_writerSidecar.empty() && m_writerStop) {
                    break;
                }
                bytes.swap(m_writerBuf);
                sidecar.swap(m_writerSidecar);
            }
            // Disk writes run on this dedicated thread only. They hold NO
            // pipeline lock, so however slow they are, they never block the
            // drain thread, slot recycling, or the producer.
            if (m_outputFile.is_open() && !bytes.empty()) {
                m_outputFile.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
            if (m_sidecarFile.is_open() && !sidecar.empty()) {
                m_sidecarFile.write(sidecar.data(), static_cast<std::streamsize>(sidecar.size()));
            }
        }
    });
    return m_writerThread.joinable();
}

void NvencH264Wrapper::StopWriterThread() {
    if (m_writerThread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_writerMutex);
            m_writerStop = true;
        }
        m_writerCv.notify_all();
        m_writerThread.join();
    }
    // Final synchronous drain: covers bytes enqueued after the thread exited
    // (e.g. Cleanup's DrainCompletedFrames running after Flush already
    // stopped the writer).
    std::vector<char> rest;
    std::string restSidecar;
    {
        std::lock_guard<std::mutex> lk(m_writerMutex);
        rest.swap(m_writerBuf);
        restSidecar.swap(m_writerSidecar);
    }
    if (m_outputFile.is_open() && !rest.empty()) {
        m_outputFile.write(rest.data(), static_cast<std::streamsize>(rest.size()));
    }
    if (m_sidecarFile.is_open() && !restSidecar.empty()) {
        m_sidecarFile.write(restSidecar.data(), static_cast<std::streamsize>(restSidecar.size()));
    }
}

bool NvencH264Wrapper::DrainCompletedFrames(bool waitForAll) {
    bool ok = true;
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        for (auto& buffer : m_buffers) {
            if (!buffer.inFlight) {
                continue;
            }

            if (!DrainSlot(buffer, waitForAll)) {
                ok = false;
            }
        }
        UpdateInflightStats();
    }
    m_inputAvailableCv.notify_all();
    return ok;
}

void NvencH264Wrapper::BuildQpDeltaMap(int mouseX, int mouseY) {
    if (!m_roiEnabled) {
        // Cursor ROI disabled (default): uniform zero delta = no per-region QP
        // boost. Keeps qpMapMode / pic structure identical to the ROI-on path.
        std::fill(m_qpDeltaMap.begin(), m_qpDeltaMap.end(), static_cast<int8_t>(0));
        return;
    }
    std::fill(m_qpDeltaMap.begin(), m_qpDeltaMap.end(), static_cast<int8_t>(m_bgDeltaQp));
    if (m_qpDeltaMap.empty()) {
        return;
    }

    const int safeX = (std::max)(0, (std::min)(m_width - 1, mouseX));
    const int safeY = (std::max)(0, (std::min)(m_height - 1, mouseY));

    const int left = (std::max)(0, safeX - m_roiRadius);
    const int top = (std::max)(0, safeY - m_roiRadius);
    const int right = (std::min)(m_width - 1, safeX + m_roiRadius);
    const int bottom = (std::min)(m_height - 1, safeY + m_roiRadius);

    const int mbLeft = left / 16;
    const int mbTop = top / 16;
    const int mbRight = right / 16;
    const int mbBottom = bottom / 16;

    for (int my = mbTop; my <= mbBottom; ++my) {
        for (int mx = mbLeft; mx <= mbRight; ++mx) {
            const size_t idx = static_cast<size_t>(my) * m_mbWidth + static_cast<size_t>(mx);
            if (idx < m_qpDeltaMap.size()) {
                m_qpDeltaMap[idx] = static_cast<int8_t>(m_roiDeltaQp);
            }
        }
    }
}

bool NvencH264Wrapper::EncodeFrame(const EncodeFrameRequest& request) {
    // Stash the real capture stamp so EncodeFromYUVWithROI can attach it to
    // this frame's telemetry for the opt-in PTS sidecar. elapsedSeconds is
    // left synthetic so encode/telemetry/bitstream stay byte-stable.
    m_pendingCaptureStamp100ns = request.captureTimestamp100ns;
    return IH264Encoder::EncodeFrame(request);
}

bool NvencH264Wrapper::EncodeFromYUVWithROI(
    ID3D11Texture2D* pY,
    ID3D11Texture2D* pUV,
    double elapsedSeconds,
    int mouseX,
    int mouseY) {
    if (!m_encoder || m_buffers.empty() || !pY) {
        LogDrop(DropReason::InvalidState, 0, m_frameIndex, "encoder/buffer/input not ready");
        return false;
    }

    if (pUV != nullptr) {
        std::cerr << "[NVENC] Full-GPU NVENC path expects NV12 input texture (pUV must be nullptr).\n";
        LogDrop(DropReason::InvalidInput, 0, m_frameIndex, "pUV must be nullptr for NV12");
        return false;
    }

    const int registeredIdx = FindRegisteredInputIndex(pY);
    if (registeredIdx < 0) {
        LogDrop(DropReason::InvalidInput, 0, m_frameIndex, "input texture is not from NVENC registered pool");
        return false;
    }

    const uint32_t slot = static_cast<uint32_t>(registeredIdx);
    if (static_cast<size_t>(registeredIdx) >= m_buffers.size() ||
        static_cast<size_t>(registeredIdx) >= m_registeredInputs.size()) {
        LogDrop(DropReason::InvalidState, slot, m_frameIndex, "registered input index out of range");
        ReleaseInputTexture(pY);
        return false;
    }

    auto& input = m_registeredInputs[registeredIdx];
    if (!input.texture || !input.handle) {
        LogDrop(DropReason::InvalidState, slot, m_frameIndex, "missing registered input");
        ReleaseInputTexture(pY);
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    pY->GetDesc(&srcDesc);
    if (srcDesc.Format != DXGI_FORMAT_NV12) {
        std::cerr << "[NVENC] Input texture must be NV12.\n";
        LogDrop(DropReason::InvalidInput, slot, m_frameIndex, "input texture format is not NV12");
        ReleaseInputTexture(pY);
        return false;
    }
    if (static_cast<int>(srcDesc.Width) != m_width || static_cast<int>(srcDesc.Height) != m_height) {
        std::cerr << "[NVENC] Input texture size mismatch. Expected "
                  << m_width << "x" << m_height << ", got "
                  << srcDesc.Width << "x" << srcDesc.Height << ".\n";
        LogDrop(DropReason::InvalidInput, slot, m_frameIndex, "input texture size mismatch");
        ReleaseInputTexture(pY);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        auto& pair = m_buffers[registeredIdx];
        if (!pair.reserved || pair.inFlight) {
            LogDrop(DropReason::InvalidState, slot, m_frameIndex, "input slot was not acquired or is still in flight");
            if (pair.reserved && !pair.inFlight) {
                pair.reserved = false;
                m_inputAvailableCv.notify_all();
            }
            return false;
        }
    }

    const uint64_t convertDoneUs = NowTelemetryUs();
    const uint64_t captureTimestampUs = CaptureTimestampUsFromElapsed(elapsedSeconds, convertDoneUs);
    const uint64_t inputTimeStamp90k =
        static_cast<uint64_t>((std::max)(0.0, elapsedSeconds) * 90000.0);

    FrameTelemetry telemetry = {};
    telemetry.frameIndex = m_frameIndex;
    telemetry.captureTimestamp90k = inputTimeStamp90k;
    telemetry.captureTimestampUs = captureTimestampUs;
    telemetry.convertDoneTimestampUs = convertDoneUs;
    telemetry.captureStamp100ns = m_pendingCaptureStamp100ns;
    telemetry.valid = true;

    NV_ENC_MAP_INPUT_RESOURCE mappedInput = {};
    mappedInput.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mappedInput.registeredResource = input.handle;

    NVENCSTATUS st = m_api.nvEncMapInputResource(m_encoder, &mappedInput);
    if (st != NV_ENC_SUCCESS || !mappedInput.mappedResource) {
        ++m_mapFailureCount;
        std::cerr << "[NVENC] nvEncMapInputResource failed: " << StatusToString(st) << "\n";
        LogDrop(DropReason::MapFailed, slot, m_frameIndex, StatusToString(st));
        ReleaseInputTexture(pY);
        return false;
    }

    BuildQpDeltaMap(mouseX, mouseY);

    const uint64_t submitUs = NowTelemetryUs();
    telemetry.encodeSubmitTimestampUs = submitUs;
    if (submitUs >= telemetry.captureTimestampUs) {
        telemetry.ageAtSubmitMs = static_cast<double>(submitUs - telemetry.captureTimestampUs) / 1000.0;
    }

    // D3D11 default-usage NV12 textures do not expose row pitch to CPU. NVENC documentation states:
    // "If pitch value is not known, set this to inputWidth."
    const uint32_t inputPitch = static_cast<uint32_t>(srcDesc.Width);

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = mappedInput.mappedResource;
    pic.outputBitstream = m_buffers[registeredIdx].output;
    pic.bufferFmt = mappedInput.mappedBufferFmt;
    pic.inputWidth = static_cast<uint32_t>(m_width);
    pic.inputHeight = static_cast<uint32_t>(m_height);
    pic.inputPitch = inputPitch;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = inputTimeStamp90k;
    pic.frameIdx = m_frameIndex;
    pic.qpDeltaMap = m_qpDeltaMap.data();
    pic.qpDeltaMapSize = static_cast<uint32_t>(m_qpDeltaMap.size());
    pic.completionEvent = m_asyncEncode ? m_buffers[registeredIdx].completionEvent : nullptr;

    st = m_api.nvEncEncodePicture(m_encoder, &pic);

    if (st == NV_ENC_ERR_ENCODER_BUSY) {
        ++m_encoderBusyCount;
        UnmapInputResourceChecked(mappedInput.mappedResource, "encode busy");
        LogDrop(DropReason::EncoderBusy, slot, m_frameIndex, StatusToString(st));
        ReleaseInputTexture(pY);
        return false;
    }

    if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        if (!UnmapInputResourceChecked(mappedInput.mappedResource, "need more input")) {
            ReleaseInputTexture(pY);
            return false;
        }
        std::cout << "[NVENC] nvEncEncodePicture returned NV_ENC_ERR_NEED_MORE_INPUT for frame="
                  << m_frameIndex << ".\n";
        ++m_frameIndex;
        ReleaseInputTexture(pY);
        return true;
    }

    if (st != NV_ENC_SUCCESS) {
        UnmapInputResourceChecked(mappedInput.mappedResource, "encode failed");
        std::cerr << "[NVENC] nvEncEncodePicture failed: " << StatusToString(st) << "\n";
        LogDrop(DropReason::InvalidState, slot, m_frameIndex, StatusToString(st));
        ReleaseInputTexture(pY);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        auto& pair = m_buffers[registeredIdx];
        pair.inFlight = true;
        pair.reserved = false;
        pair.mappedInput = mappedInput.mappedResource;
        pair.telemetry = telemetry;
        ++m_frameIndex;
        UpdateInflightStats();
    }
    m_inputAvailableCv.notify_all();

    return true;
}

void NvencH264Wrapper::Flush() {
    if (!m_encoder || m_buffers.empty()) {
        LogStats("flush_skipped");
        return;
    }

    StopDrainThread();

    if (!DrainCompletedFrames(true)) {
        return;
    }

    auto& pair = m_buffers[m_frameIndex % static_cast<uint32_t>(m_buffers.size())];
    if (pair.inFlight && !DrainSlot(pair, true)) {
        return;
    }

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    pic.outputBitstream = pair.output;
    pic.completionEvent = m_asyncEncode ? pair.completionEvent : nullptr;

    NVENCSTATUS st = m_api.nvEncEncodePicture(m_encoder, &pic);
    if (st == NV_ENC_SUCCESS) {
        pair.inFlight = true;
        pair.eos = true;  // EOS marker frame: drained but not counted as a real frame
    } else if (st != NV_ENC_ERR_NEED_MORE_INPUT) {
        std::cerr << "[NVENC] Flush nvEncEncodePicture(EOS) failed: " << StatusToString(st) << "\n";
    }

    DrainCompletedFrames(true);
    StopWriterThread();
    if (m_outputFile.is_open()) {
        m_outputFile.flush();
    }
    if (m_sidecarFile.is_open()) {
        m_sidecarFile.flush();
    }
    LogStats("flush");
}

const char* NvencH264Wrapper::StatusToString(NVENCSTATUS status) const {
    switch (status) {
    case NV_ENC_SUCCESS: return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE: return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST: return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PARAM: return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL: return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_ENCODER_BUSY: return "NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_NEED_MORE_INPUT: return "NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_LOCK_BUSY: return "NV_ENC_ERR_LOCK_BUSY";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_GENERIC: return "NV_ENC_ERR_GENERIC";
    default: return "NV_ENC_ERR_UNKNOWN";
    }
}

void NvencH264Wrapper::Cleanup() {
    StopDrainThread();

    if (m_encoder) {
        DrainCompletedFrames(true);

        if (m_encodedFrameCount > 0 || m_dropCount > 0 || m_mapFailureCount > 0 || m_unmapFailureCount > 0) {
            LogStats("cleanup");
        }

        for (auto& reg : m_registeredInputs) {
            if (reg.handle && m_api.nvEncUnregisterResource) {
                m_api.nvEncUnregisterResource(m_encoder, reg.handle);
            }
            if (reg.texture) {
                reg.texture->Release();
                reg.texture = nullptr;
            }
            reg.handle = nullptr;
        }
        m_registeredInputs.clear();

        for (auto& b : m_buffers) {
            if (b.mappedInput && m_api.nvEncUnmapInputResource) {
                UnmapInputResourceChecked(b.mappedInput, "cleanup");
            }
            if (b.output && m_api.nvEncDestroyBitstreamBuffer) {
                m_api.nvEncDestroyBitstreamBuffer(m_encoder, b.output);
            }
            if (b.completionEvent) {
                if (m_asyncEncode && m_api.nvEncUnregisterAsyncEvent) {
                    NV_ENC_EVENT_PARAMS ev = {};
                    ev.version = NV_ENC_EVENT_PARAMS_VER;
                    ev.completionEvent = b.completionEvent;
                    m_api.nvEncUnregisterAsyncEvent(m_encoder, &ev);
                }
                CloseHandle(b.completionEvent);
            }
            b.output = nullptr;
            b.mappedInput = nullptr;
            b.completionEvent = nullptr;
            b.inFlight = false;
            b.telemetry = {};
        }
        m_buffers.clear();

        if (m_api.nvEncDestroyEncoder) {
            m_api.nvEncDestroyEncoder(m_encoder);
        }
        m_encoder = nullptr;
    } else {
        for (auto& reg : m_registeredInputs) {
            if (reg.texture) {
                reg.texture->Release();
                reg.texture = nullptr;
            }
            reg.handle = nullptr;
        }
        m_registeredInputs.clear();

        for (auto& b : m_buffers) {
            if (b.completionEvent) {
                CloseHandle(b.completionEvent);
            }
            b.mappedInput = nullptr;
            b.telemetry = {};
        }
        m_buffers.clear();
    }

    StopWriterThread();
    if (m_outputFile.is_open()) {
        m_outputFile.flush();
        m_outputFile.close();
    }
    if (m_sidecarFile.is_open()) {
        m_sidecarFile.flush();
        m_sidecarFile.close();
    }

    if (m_nvencDll) {
        FreeLibrary(m_nvencDll);
        m_nvencDll = nullptr;
    }

    m_qpDeltaMap.clear();
    m_pContext.Release();
    m_pDevice.Release();
    memset(&m_api, 0, sizeof(m_api));
}
