#include "AetherFlow/VplH264Wrapper.h"
#include "AetherFlow/IEncodedFrameSink.h"
#include <iostream>
#include <cstdlib>
#include <filesystem>

#include <Windows.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "libvpl.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")


static std::filesystem::path GetExeDir() {
    char buf[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::string(buf, buf + len)).parent_path();
}

static std::filesystem::path GetOutputDir() {
    if (const char* env = std::getenv("AETHERFLOW_OUTPUT_DIR"); env && *env) {
        return std::filesystem::path(env);
    }

    // Default: repo-root `output/` when running from `build/<Config>/AetherFlow.exe`.
    return GetExeDir() / ".." / ".." / "output";
}

static std::filesystem::path MakeOutputPath(const char* fileName) {
    auto outDir = GetOutputDir();
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    return outDir / fileName;
}

// ============================================================
// D3D11 Frame Allocator callbacks
// VPL VIDEO_MEMORY mode requires allocator callbacks. We intentionally
// disable CPU lock/unlock to keep Intel path GPU-only and avoid any
// staging Map()/copy behavior.
// ============================================================
static mfxStatus MFX_CDECL D3D11Alloc(mfxHDL pthis,
                                       mfxFrameAllocRequest*  req,
                                       mfxFrameAllocResponse* resp)
{
    auto* ctx = static_cast<VplD3D11AllocCtx*>(pthis);
    if (!ctx || !req || !resp) return MFX_ERR_NULL_PTR;
    if (req->Type & MFX_MEMTYPE_SYSTEM_MEMORY) return MFX_ERR_UNSUPPORTED;

    const int n = req->NumFrameSuggested;
    ctx->pools.emplace_back();
    VplAllocPool& pool = ctx->pools.back();
    pool.textures.resize(n);
    pool.mids.resize(n);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = req->Info.Width;
    desc.Height           = req->Info.Height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET;  // Intel QSV 需要 RT access

    for (int i = 0; i < n; i++) {
        HRESULT hr = ctx->device->CreateTexture2D(&desc, nullptr, &pool.textures[i]);
        if (FAILED(hr)) {
            desc.BindFlags = 0;  // some drivers reject RENDER_TARGET on NV12
            hr = ctx->device->CreateTexture2D(&desc, nullptr, &pool.textures[i]);
        }
        if (FAILED(hr)) {
            std::cerr << "[D3D11Alloc] CreateTexture2D["<<i<<"] failed\n";
            return MFX_ERR_MEMORY_ALLOC;
        }
        pool.mids[i] = (mfxMemId)pool.textures[i].p;
    }
    resp->mids           = pool.mids.data();
    resp->NumFrameActual = static_cast<mfxU16>(n);
    return MFX_ERR_NONE;
}

// GetHDL: mid IS the D3D11 texture pointer (both for allocator-managed and
// app-created surfaces). Trivially return it as the native handle.
static mfxStatus MFX_CDECL D3D11GetHDL(mfxHDL /*pthis*/, mfxMemId mid, mfxHDL* handle)
{
    if (!mid || !handle) return MFX_ERR_NULL_PTR;
    *handle = mid;
    return MFX_ERR_NONE;
}

static mfxStatus MFX_CDECL D3D11Lock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr)
{
    (void)pthis;
    (void)mid;
    // GPU-only path: keep lock as a no-op for runtime compatibility.
    // We intentionally do not map/copy any surface to CPU memory.
    if (ptr) {
        ptr->Pitch = 0;
        ptr->Y = ptr->U = ptr->V = ptr->UV = nullptr;
    }
    return MFX_ERR_NONE;
}

static mfxStatus MFX_CDECL D3D11Unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr)
{
    (void)pthis;
    (void)mid;
    if (ptr) ptr->Y = ptr->U = ptr->V = ptr->UV = nullptr;
    return MFX_ERR_NONE;
}

static mfxStatus MFX_CDECL D3D11Free(mfxHDL pthis, mfxFrameAllocResponse* resp)
{
    auto* ctx = static_cast<VplD3D11AllocCtx*>(pthis);
    if (!ctx) return MFX_ERR_NULL_PTR;
    if (resp && resp->mids) {
        for (auto it = ctx->pools.begin(); it != ctx->pools.end(); ++it) {
            if (!it->mids.empty() && it->mids.data() == resp->mids) {
                ctx->pools.erase(it); break;
            }
        }
    }
    return MFX_ERR_NONE;
}

mfxStatus SimpleCreateHWDevice(mfxSession session, mfxHDL* devHandle, ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext) {
    std::cout << "[VPL] Creating D3D11 Device for HW Acceleration...\n";

    // Prefer Intel adapter when present (hybrid laptops often default to NVIDIA in D3D11CreateDevice(nullptr,...)).
    CComPtr<IDXGIFactory1> factory;
    CComPtr<IDXGIAdapter1> intelAdapter;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) && factory) {
        for (UINT i = 0;; i++) {
            CComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            if (!adapter) continue;

            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x8086) { // Intel
                intelAdapter = adapter;
                break;
            }
        }
    }

    HRESULT hr = E_FAIL;
    if (intelAdapter) {
        hr = D3D11CreateDevice(
            intelAdapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            ppDevice,
            nullptr,
            ppContext);
    } else {
        // Fallback: default adapter.
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            ppDevice,
            nullptr,
            ppContext);
    }
    
    if (FAILED(hr)) return MFX_ERR_DEVICE_FAILED;

    CComPtr<ID3D11Multithread> pMultiThread;
    hr = (*ppDevice)->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMultiThread);
    if (SUCCEEDED(hr) && pMultiThread) {
        pMultiThread->SetMultithreadProtected(TRUE);
    }

    *devHandle = (mfxHDL)*ppDevice;
    return MFX_ERR_NONE;  // 不在這裡設置 handle，稍後設置
}

VplH264Wrapper::VplH264Wrapper() {}
VplH264Wrapper::~VplH264Wrapper() { Cleanup(); }

bool VplH264Wrapper::Initialize(int width, int height, int fps) {
    m_width = width;
    m_height = height;
    
    std::cout << "[VPL] Creating loader...\n";
    m_loader = MFXLoad();
    if (!m_loader) {
        std::cerr << "[VPL ERROR] MFXLoad failed\n";
        return false;
    }

    // 配置 Hardware Implementation
    // ✅ 恢復硬體編碼 (ROI Rectangle 支援硬體加速)
    bool USE_SOFTWARE_FOR_MBQP_TEST = false;  // ROI Rectangle 用硬體
    
    mfxConfig cfg = MFXCreateConfig(m_loader);
    mfxVariant impl_value;
    impl_value.Type = MFX_VARIANT_TYPE_U32;
    impl_value.Data.U32 = USE_SOFTWARE_FOR_MBQP_TEST ? MFX_IMPL_TYPE_SOFTWARE : MFX_IMPL_TYPE_HARDWARE;
    MFXSetConfigFilterProperty(cfg, (mfxU8*)"mfxImplDescription.Impl", impl_value);
    
    if (!USE_SOFTWARE_FOR_MBQP_TEST) {
        // 配置 D3D11 加速模式（僅硬體模式）
        cfg = MFXCreateConfig(m_loader);
        impl_value.Data.U32 = MFX_ACCEL_MODE_VIA_D3D11;
        MFXSetConfigFilterProperty(cfg, (mfxU8*)"mfxImplDescription.AccelerationMode", impl_value);
    }

    std::cout << "[VPL] Creating session...\n";
    mfxStatus sts = MFXCreateSession(m_loader, 0, &m_session);
    if (sts != MFX_ERR_NONE) {
        std::cerr << "[VPL ERROR] MFXCreateSession failed, status=" << sts << "\n";
        return false;
    }
    
    // Print implementation details
    mfxIMPL impl;
    MFXQueryIMPL(m_session, &impl);
    std::cout << "[VPL] Session created with implementation: ";
    if (impl & MFX_IMPL_HARDWARE) std::cout << "HARDWARE ";
    if (impl & MFX_IMPL_SOFTWARE) std::cout << "SOFTWARE ";
    if (impl & MFX_IMPL_VIA_D3D11) std::cout << "VIA_D3D11 ";
    std::cout << "\n";
    
    mfxVersion ver;
    MFXQueryVersion(m_session, &ver);
    std::cout << "[VPL] VPL Runtime Version: " << ver.Major << "." << ver.Minor << "\n";

    std::cout << "[VPL] Creating D3D11 Device...\n";
    if (SimpleCreateHWDevice(m_session, &m_devHandle, &m_pDevice.p, &m_pContext.p) != MFX_ERR_NONE) {
        std::cerr << "[VPL ERROR] SimpleCreateHWDevice failed\n";
        return false;
    }
    
    // Get adapter info
    CComPtr<IDXGIDevice> pDXGIDevice;
    m_pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
    if (pDXGIDevice) {
        CComPtr<IDXGIAdapter> pAdapter;
        pDXGIDevice->GetAdapter(&pAdapter);
        if (pAdapter) {
            DXGI_ADAPTER_DESC adapterDesc;
            pAdapter->GetDesc(&adapterDesc);
            std::wcout << L"[VPL] GPU Adapter: " << adapterDesc.Description << L"\n";
            std::cout << "[VPL] Dedicated Video Memory: " << (adapterDesc.DedicatedVideoMemory / 1024 / 1024) << " MB\n";
            std::cout << "[VPL] Vendor ID: 0x" << std::hex << adapterDesc.VendorId << std::dec << "\n";
        }
    }
    
    // 設置 device handle
    sts = MFXVideoCORE_SetHandle(m_session, MFX_HANDLE_D3D11_DEVICE, m_devHandle);
    if (sts != MFX_ERR_NONE) {
        std::cerr << "[VPL ERROR] MFXVideoCORE_SetHandle failed, status=" << sts << "\n";
        return false;
    }

    memset(&m_mfxEncParams, 0, sizeof(m_mfxEncParams));
    m_mfxEncParams.mfx.CodecId = MFX_CODEC_AVC;
    m_mfxEncParams.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    
    // 碼率控制：從 Config.h 讀取 AETHERFLOW_BITRATE（與 NVENC 統一）
    // 碼率控制：CBR 是讓 ROI 視覺效果最明顯的模式。
    // Intel QSV CBR 有最低碼率限制（1080p30 約 1074 kbps），低於此值 driver 會強制提高。
    // AETHERFLOW_BITRATE=1500 高於此限制，設定會被 driver 接受並生效。
    // CBR 下 bits 預算固定：ROI 搶走 bits → rate controller 自動壓低背景 → 對比明顯。
    // VBR 讓 ROI 和背景都獲得更多 bits → 對比消失。
    // CQP 在 Intel UHD + VIDEO_MEMORY 組合下觸發 MFX_ERR_DEVICE_FAILED (-17)。
    m_mfxEncParams.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    // TargetKbps is mfxU16 — clamp instead of silently truncating a >65535
    // request (70000 would wrap to ~4464 kbps). Values that large would need
    // BRCParamMultiplier, which nothing here requests yet.
    int targetKbps = (m_targetBitrateKbps > 0) ? m_targetBitrateKbps : AETHERFLOW_BITRATE;
    if (targetKbps > 65535) {
        std::cout << "[VPL] WARNING: requested bitrate " << targetKbps
                  << " kbps exceeds the mfxU16 field; clamping to 65535 kbps\n";
        targetKbps = 65535;
    }
    m_mfxEncParams.mfx.TargetKbps = static_cast<mfxU16>(targetKbps);
    m_mfxEncParams.mfx.MaxKbps    = 0;  // CBR: must be 0
    std::cout << "[VPL] Bitrate: " << targetKbps << " kbps (CBR"
              << (m_targetBitrateKbps > 0 ? ", runtime override" : ", AETHERFLOW_BITRATE default")
              << ")\n";
    m_mfxEncParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12; 
    m_mfxEncParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    m_mfxEncParams.mfx.FrameInfo.Width = (width + 15) / 16 * 16;
    m_mfxEncParams.mfx.FrameInfo.Height = (height + 15) / 16 * 16;
    m_mfxEncParams.mfx.FrameInfo.CropW = width;
    m_mfxEncParams.mfx.FrameInfo.CropH = height;
    m_mfxEncParams.mfx.FrameInfo.FrameRateExtN = fps;
    m_mfxEncParams.mfx.FrameInfo.FrameRateExtD = 1;
    m_mfxEncParams.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    
    // 儲存 fps 參數（用於 TimeStamp 計算）
    m_width = width;
    m_height = height;
    m_fps = fps;
    
    // GOP structure: use config-defined GOP length (AETHERFLOW_GOP_SECONDS = 2s = 60 frames @ 30fps).
    // Previously used fps/2 (0.5s = 15 frames) which caused ~25 IDR frames per 363 frames,
    // a pattern associated with driver-level MFX_ERR_DEVICE_FAILED on Intel UHD.
    // With GOP=60, only ~6 IDR frames occur before frame 363, greatly reducing encoder stress.
    m_mfxEncParams.mfx.GopPicSize = static_cast<mfxU16>(AETHERFLOW_GOP_SIZE);  // 2s GOP
    m_mfxEncParams.mfx.GopRefDist = 1;         // No B-frames (低延遲優先)
    std::cout << "[VPL] GOP size: " << m_mfxEncParams.mfx.GopPicSize
              << " frames (" << (m_mfxEncParams.mfx.GopPicSize / fps) << "s), DeltaQP="
              << AetherFlow::kVplRoiDefaultDeltaQp << "\n";
    
    // VIDEO_MEMORY: zero-copy GPU pipeline. Requires mfxFrameAllocator (set below).
    m_mfxEncParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    m_mfxEncParams.AsyncDepth = 4;
    
    // ============================================================
    // ROI 編碼功能檢測 - 測試所有可能的擴展
    // ============================================================
    std::cout << "\n========== ROI CAPABILITY DETECTION ==========\n";
    
    // 測試 1: ROI Rectangle
    mfxExtEncoderROI testROI = {};
    testROI.Header.BufferId = MFX_EXTBUFF_ENCODER_ROI;
    testROI.Header.BufferSz = sizeof(testROI);
    testROI.NumROI = 1;
    testROI.ROIMode = MFX_ROI_MODE_QP_DELTA;
    testROI.ROI[0] = { 0, 0, 256, 256, -10 };
    
    mfxExtBuffer* testBuf1[1] = { (mfxExtBuffer*)&testROI };
    mfxVideoParam testParams = m_mfxEncParams;
    testParams.ExtParam = testBuf1;
    testParams.NumExtParam = 1;
    mfxVideoParam testOut = testParams;
    mfxStatus roiSts = MFXVideoENCODE_Query(m_session, &testParams, &testOut);
    std::cout << "[Test] ROI Rectangle: " 
              << (roiSts >= MFX_ERR_NONE ? "SUPPORTED" : "NOT SUPPORTED") 
              << " (status=" << roiSts << ")\n";
    
    // 測試 2: Dirty Rectangles (快速編碼用)
    mfxExtDirtyRect testDirty = {};
    testDirty.Header.BufferId = MFX_EXTBUFF_DIRTY_RECTANGLES;
    testDirty.Header.BufferSz = sizeof(testDirty);
    testDirty.NumRect = 1;
    testDirty.Rect[0] = { 0, 0, 256, 256 };
    
    mfxExtBuffer* testBuf2[1] = { (mfxExtBuffer*)&testDirty };
    testParams = m_mfxEncParams;
    testParams.ExtParam = testBuf2;
    testParams.NumExtParam = 1;
    testOut = testParams;
    mfxStatus dirtySts = MFXVideoENCODE_Query(m_session, &testParams, &testOut);
    std::cout << "[Test] Dirty Rectangles: " 
              << (dirtySts >= MFX_ERR_NONE ? "SUPPORTED" : "NOT SUPPORTED") 
              << " (status=" << dirtySts << ")\n";
    
    // 測試 3: MBQP (Per-Macroblock QP)
    mfxExtCodingOption3 testCO3 = {};
    testCO3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
    testCO3.Header.BufferSz = sizeof(testCO3);
    testCO3.EnableMBQP = MFX_CODINGOPTION_ON;
    
    mfxExtBuffer* testBuf3[1] = { (mfxExtBuffer*)&testCO3 };
    testParams = m_mfxEncParams;
    testParams.ExtParam = testBuf3;
    testParams.NumExtParam = 1;
    testOut = testParams;
    mfxExtCodingOption3 testCO3Out = testCO3;
    mfxExtBuffer* testBufOut3[1] = { (mfxExtBuffer*)&testCO3Out };
    testOut.ExtParam = testBufOut3;
    mfxStatus mbqpSts = MFXVideoENCODE_Query(m_session, &testParams, &testOut);
    std::cout << "[Test] MBQP (EnableMBQP): " 
              << (mbqpSts >= MFX_ERR_NONE && testCO3Out.EnableMBQP == MFX_CODINGOPTION_ON ? "SUPPORTED" : "NOT SUPPORTED")
              << " (status=" << mbqpSts << ", EnableMBQP=" << (int)testCO3Out.EnableMBQP << ")\n";
    
    std::cout << "===============================================\n\n";
    
    // 根據檢測結果決定是否啟用 ROI
    bool roiSupported = (roiSts >= MFX_ERR_NONE);
    
    // ⚠️ 強制開關 - 用於 A/B 測試
    // 設為 true 啟用 ROI，設為 false 關閉 ROI 做對比
    bool FORCE_ENABLE_ROI = true;  // 🟢 ROI enabled (per-frame ctrl only)
    
    if (roiSupported && FORCE_ENABLE_ROI) {
        // ROI Rectangle 支援，初始化結構
        std::cout << "[VPL] ✓ ROI Rectangle ENABLED for hardware acceleration\n";
        std::cout << "[VPL] ROI Radius: " << m_roiRadius << " pixels\n";
        
        memset(&m_extEncoderROI, 0, sizeof(m_extEncoderROI));
        m_extEncoderROI.Header.BufferId = MFX_EXTBUFF_ENCODER_ROI;
        m_extEncoderROI.Header.BufferSz = sizeof(m_extEncoderROI);
        m_extEncoderROI.NumROI = 1;
        m_extEncoderROI.ROIMode = MFX_ROI_MODE_QP_DELTA;
        
        // 初始化預設 ROI 區域 (螢幕中央)
        int centerX = width / 2;
        int centerY = height / 2;
        m_extEncoderROI.ROI[0].Left = ((centerX - m_roiRadius) / 16) * 16;
        m_extEncoderROI.ROI[0].Top = ((centerY - m_roiRadius) / 16) * 16;
        m_extEncoderROI.ROI[0].Right = (((centerX + m_roiRadius) + 15) / 16) * 16;
        m_extEncoderROI.ROI[0].Bottom = (((centerY + m_roiRadius) + 15) / 16) * 16;
        
        // DeltaQP: 使用 VplDefaults 常數 (kVplRoiDefaultDeltaQp = -30)
        // 範圍說明: -5 微小 / -10 輕微 / -15 推薦 / -20 顯著 / -30 預設 / -51 極端(會閃)
        m_extEncoderROI.ROI[0].DeltaQP = AetherFlow::kVplRoiDefaultDeltaQp;
        
        std::cout << "[VPL] Initial ROI: (" << m_extEncoderROI.ROI[0].Left << "," 
                  << m_extEncoderROI.ROI[0].Top << ") to (" 
                  << m_extEncoderROI.ROI[0].Right << "," 
                  << m_extEncoderROI.ROI[0].Bottom << "), DeltaQP=" 
                  << m_extEncoderROI.ROI[0].DeltaQP << "\n";
        
        // Do NOT attach ROI to m_mfxEncParams for Init.
        // Per-VPL spec, session-level ROI in Init can interfere with the driver's
        // per-frame ROI path via mfxEncodeCtrl on some Intel UHD configurations
        // (causes MFX_ERR_DEVICE_FAILED on the first EncodeFrameAsync).
        // ROI is instead supplied per-frame via mfxEncodeCtrl only, which is the
        // correct dynamic ROI usage pattern.
        m_mfxEncParams.ExtParam = nullptr;
        m_mfxEncParams.NumExtParam = 0;
        m_roiEnabled = true;
    } else {
        // 不支援或關閉 ROI，使用普通編碼
        std::cout << "[VPL] ✗ ROI Rectangle DISABLED\n";
        if (!roiSupported) std::cout << "  Reason: Hardware not supported\n";
        if (!FORCE_ENABLE_ROI) std::cout << "  Reason: Manually disabled for A/B test\n";
        m_mfxEncParams.ExtParam = nullptr;
        m_mfxEncParams.NumExtParam = 0;
    }
    
    // Store framerate for later use
    m_width = width;
    m_height = height;

    // CRITICAL: Query encoder to validate and adjust parameters
    std::cout << "[VPL] Validating encoder parameters with Query...\n";
    mfxVideoParam queriedParams = m_mfxEncParams;
    sts = MFXVideoENCODE_Query(m_session, &m_mfxEncParams, &queriedParams);
    
    if (sts == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
        std::cout << "[VPL] Warning: Some parameters were adjusted by Query:\n";
        std::cout << "  Original Width=" << m_mfxEncParams.mfx.FrameInfo.Width 
                  << ", Queried Width=" << queriedParams.mfx.FrameInfo.Width << "\n";
        std::cout << "  Original Height=" << m_mfxEncParams.mfx.FrameInfo.Height 
                  << ", Queried Height=" << queriedParams.mfx.FrameInfo.Height << "\n";
        std::cout << "  Original IOPattern=" << m_mfxEncParams.IOPattern 
                  << ", Queried IOPattern=" << queriedParams.IOPattern << "\n";
        std::cout << "  Original TargetKbps=" << m_mfxEncParams.mfx.TargetKbps
                  << ", Queried TargetKbps=" << queriedParams.mfx.TargetKbps << "\n";
        // Preserve bitrate: driver may zero TargetKbps in queriedParams for CBR.
        const mfxU16 savedTargetKbps = m_mfxEncParams.mfx.TargetKbps;
        const mfxU16 savedRateCtrl   = m_mfxEncParams.mfx.RateControlMethod;
        m_mfxEncParams = queriedParams;  // Use queried parameters
        // Restore bitrate fields that driver may have cleared/changed
        if (m_mfxEncParams.mfx.TargetKbps == 0) {
            m_mfxEncParams.mfx.TargetKbps = savedTargetKbps;
            m_mfxEncParams.mfx.RateControlMethod = savedRateCtrl;
            std::cout << "  [VPL] Restored TargetKbps=" << savedTargetKbps << " (driver had cleared it)\n";
        }
        m_mfxEncParams.mfx.MaxKbps = 0;  // CBR
        m_mfxEncParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
        sts = MFX_ERR_NONE;
    }
    if (sts != MFX_ERR_NONE) {
        std::cerr << "[VPL ERROR] MFXVideoENCODE_Query failed, status=" << sts << "\n";
        return false;
    }
    
    // Query IOSurf requirements (just for logging before Init)
    {
        mfxFrameAllocRequest tmp = {};
        if (MFXVideoENCODE_QueryIOSurf(m_session, &m_mfxEncParams, &tmp) == MFX_ERR_NONE)
            std::cout << "[VPL] QueryIOSurf: NumFrameSuggested=" << tmp.NumFrameSuggested << "\n";
    }

    // ── Set up D3D11 frame allocator BEFORE MFXVideoENCODE_Init ──────────────────
    // Without this, VIDEO_MEMORY mode cannot lock/unlock D3D11 surfaces
    // and returns MFX_ERR_UNDEFINED_BEHAVIOR (-16) on every EncodeFrameAsync.
    m_allocCtx.device  = m_pDevice;
    m_allocCtx.context = m_pContext;
    m_allocator.pthis  = &m_allocCtx;
    m_allocator.Alloc  = D3D11Alloc;
    m_allocator.Lock   = D3D11Lock;
    m_allocator.Unlock = D3D11Unlock;
    m_allocator.GetHDL = D3D11GetHDL;
    m_allocator.Free   = D3D11Free;
    sts = MFXVideoCORE_SetFrameAllocator(m_session, &m_allocator);
    if (sts != MFX_ERR_NONE) {
        std::cerr << "[VPL ERROR] MFXVideoCORE_SetFrameAllocator failed, status=" << sts << "\n";
        return false;
    }
    std::cout << "[VPL] ✓ D3D11 frame allocator registered\n";

    std::cout << "[VPL] Initializing Encoder (AVC NV12, " << width << "x" << height << " @" << fps << "fps)...\n";
    sts = MFXVideoENCODE_Init(m_session, &m_mfxEncParams);
    if (sts != MFX_ERR_NONE) {
        std::cerr << "[VPL ERROR] MFXVideoENCODE_Init failed, status=" << sts << "\n";
        if (sts == MFX_ERR_INVALID_VIDEO_PARAM) std::cerr << "  -> Invalid video parameters\n";
        if (sts == MFX_ERR_UNSUPPORTED)        std::cerr << "  -> Configuration not supported by hardware\n";
        return false;
    }
    
    // CRITICAL: Get actual encoder parameters after Init (ffmpeg 的關鍵步驟)
    // ⚠️ 先解除 ExtParam，避免 GetVideoParam 覆寫 m_extEncoderROI
    // (ROI 是 per-frame 控制，session 層不保存，GetVideoParam 會把 NumROI 清為 0)
    m_mfxEncParams.ExtParam = nullptr;
    m_mfxEncParams.NumExtParam = 0;
    sts = MFXVideoENCODE_GetVideoParam(m_session, &m_mfxEncParams);
    if (sts != MFX_ERR_NONE) {
        std::cerr << "[VPL ERROR] MFXVideoENCODE_GetVideoParam failed, status=" << sts << "\n";
        return false;
    }
    // 確保 IOPattern 維持 VIDEO_MEMORY
    m_mfxEncParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;

    // Calculate correct packet size
    // NOTE: Per VPL spec, BRCParamMultiplier==0 means multiplier is 1.
    mfxU16 brcMult = (m_mfxEncParams.mfx.BRCParamMultiplier == 0) ? 1 : m_mfxEncParams.mfx.BRCParamMultiplier;
    int packet_size = (int)m_mfxEncParams.mfx.BufferSizeInKB * brcMult * 1000;
    if (packet_size <= 0) packet_size = 4 * 1024 * 1024;
    // Effective bitrate = TargetKbps * BRCParamMultiplier (driver may scale them)
    const int effectiveKbps = (int)m_mfxEncParams.mfx.TargetKbps * (int)brcMult;
    std::cout << "[VPL] After GetVideoParam: TargetKbps=" << m_mfxEncParams.mfx.TargetKbps
              << ", BRCParamMultiplier=" << brcMult
              << " => effective=" << effectiveKbps << " kbps";
    const int requestedKbps = (m_targetBitrateKbps > 0) ? m_targetBitrateKbps : AETHERFLOW_BITRATE;
    if (effectiveKbps != requestedKbps) {
        std::cout << "  *** WARNING: differs from requested " << requestedKbps << " kbps ***";
    }
    std::cout << "\n";
    std::cout << "[VPL] BufferSizeInKB=" << m_mfxEncParams.mfx.BufferSizeInKB
              << " => packet_size=" << packet_size << " bytes\n";

    // ── Build D3D11 input surface pool (Zero-Copy GPU pipeline) ─────────────────
    // QueryIOSurf after Init gives us the actual NumFrameSuggested.
    // App manages its own input pool; GetHDL trivially returns Data.MemId,
    // so these textures don't need to go through D3D11Alloc.
    {
        mfxFrameAllocRequest encReq = {};
        const int numSurfaces = (MFXVideoENCODE_QueryIOSurf(m_session, &m_mfxEncParams, &encReq) == MFX_ERR_NONE)
                                 ? (std::max)(4, (int)encReq.NumFrameSuggested) : 4;
        int alignedW = m_mfxEncParams.mfx.FrameInfo.Width;
        int alignedH = m_mfxEncParams.mfx.FrameInfo.Height;

        m_d3d11Surfaces.resize(numSurfaces);
        m_d3d11VplSurfaces.resize(numSurfaces);

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width            = static_cast<UINT>(alignedW);
        texDesc.Height           = static_cast<UINT>(alignedH);
        texDesc.MipLevels        = 1;
        texDesc.ArraySize        = 1;
        texDesc.Format           = DXGI_FORMAT_NV12;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage            = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET;  // Intel QSV encoder input

        for (int i = 0; i < numSurfaces; i++) {
            HRESULT hr = m_pDevice->CreateTexture2D(&texDesc, nullptr, &m_d3d11Surfaces[i]);
            if (FAILED(hr)) {
                texDesc.BindFlags = 0;  // driver fallback
                hr = m_pDevice->CreateTexture2D(&texDesc, nullptr, &m_d3d11Surfaces[i]);
            }
            if (FAILED(hr)) {
                std::cerr << "[VPL ERROR] CreateTexture2D surface pool [" << i << "] failed\n";
                return false;
            }
            memset(&m_d3d11VplSurfaces[i], 0, sizeof(mfxFrameSurface1));
            m_d3d11VplSurfaces[i].Info       = m_mfxEncParams.mfx.FrameInfo;
            m_d3d11VplSurfaces[i].Data.MemId = (mfxMemId)m_d3d11Surfaces[i].p;
        }
        m_useD3D11VideoMemory = true;
        std::cout << "[VPL] ✓ Zero-Copy D3D11 surface pool: " << numSurfaces
                  << " x " << alignedW << "x" << alignedH << " NV12\n";
    }

    m_asyncDepth = (std::max)(1, static_cast<int>(m_mfxEncParams.AsyncDepth));
    const int slotCount = (std::max)(4, m_asyncDepth + 2);
    if (!InitBitstreamSlots(packet_size, slotCount)) {
        std::cerr << "[VPL ERROR] Failed to allocate async bitstream slots\n";
        return false;
    }
    
    std::cout << "[VPL] Encoder ready (VIDEO_MEMORY / Zero-Copy mode)\n";
    
    // ── Create IMFSinkWriter for .mp4 output ─────────────────────────────────
    // Output path: repo-root output/output.mp4
    const auto mp4Path = MakeOutputPath("output.mp4");
    const std::wstring mp4Wide(mp4Path.wstring());

    MFStartup(MF_VERSION);  // idempotent; safe to call each Initialize

    CComPtr<IMFAttributes> pAttr;
    MFCreateAttributes(&pAttr, 2);
    pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    pAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hrMF = MFCreateSinkWriterFromURL(mp4Wide.c_str(), nullptr, pAttr, &m_sinkWriter);
    if (FAILED(hrMF)) {
        std::cerr << "[VPL ERROR] MFCreateSinkWriterFromURL failed: 0x" << std::hex << hrMF << std::dec << "\n";
        return false;
    }

    // Configure video stream (pre-encoded H.264 annex-B → MP4)
    CComPtr<IMFMediaType> pVideoType;
    MFCreateMediaType(&pVideoType);
    pVideoType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pVideoType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(pVideoType, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    MFSetAttributeRatio(pVideoType, MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1U);
    MFSetAttributeRatio(pVideoType, MF_MT_PIXEL_ASPECT_RATIO, 1U, 1U);
    pVideoType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // Target bitrate hint for MP4 header (bps). Nominal hint only, so the
    // container is valid; honors the runtime override when set.
    pVideoType->SetUINT32(MF_MT_AVG_BITRATE,
        static_cast<UINT32>((m_targetBitrateKbps > 0) ? m_targetBitrateKbps
                                                      : AETHERFLOW_BITRATE) * 1000u);

    hrMF = m_sinkWriter->AddStream(pVideoType, &m_mfStreamIndex);
    if (FAILED(hrMF)) {
        std::cerr << "[VPL ERROR] SinkWriter AddStream failed: 0x" << std::hex << hrMF << std::dec << "\n";
        return false;
    }
    // SetInputMediaType == same as stream type (no transcoding, just muxing)
    hrMF = m_sinkWriter->SetInputMediaType(m_mfStreamIndex, pVideoType, nullptr);
    if (FAILED(hrMF)) {
        std::cerr << "[VPL ERROR] SinkWriter SetInputMediaType failed: 0x" << std::hex << hrMF << std::dec << "\n";
        return false;
    }
    hrMF = m_sinkWriter->BeginWriting();
    if (FAILED(hrMF)) {
        std::cerr << "[VPL ERROR] SinkWriter BeginWriting failed: 0x" << std::hex << hrMF << std::dec << "\n";
        return false;
    }

    m_frameIndex = 0;
    m_debugEncodedCount = 0;
    m_debugTotalBytes = 0;
    std::cout << "[VPL] MP4 output: " << mp4Path.string() << "\n";

    // Create a D3D11_QUERY_EVENT for cross-engine GPU synchronization.
    // D3D11 CopyResource (3D engine) and Intel QSV EncodeFrameAsync (Video Encode
    // engine) are separate hardware units on Intel UHD.  Without an explicit GPU
    // fence between them, QSV may start reading the surface before the copy is
    // done, causing MFX_ERR_DEVICE_FAILED (-17) on the first EncodeFrameAsync.
    {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_EVENT;
        HRESULT hrQ = m_pDevice->CreateQuery(&qd, &m_d3dSyncQuery);
        if (FAILED(hrQ)) {
            std::cerr << "[VPL] Warning: D3D11_QUERY_EVENT creation failed (hr=0x"
                      << std::hex << hrQ << std::dec
                      << "); will fall back to Flush() for GPU sync.\n";
        } else {
            std::cout << "[VPL] ✓ D3D11 GPU sync query created (cross-engine fence)\n";
        }
    }

    return true;
}


// ============================================================
// ROI 編碼：使用自定義 QP map 進行逐幀質量控制
// ============================================================
bool VplH264Wrapper::EncodeFromYUVWithROI(
    ID3D11Texture2D* pY,           // Y plane texture (Luminance)
    ID3D11Texture2D* pUV,          // UV plane texture (Chrominance)
    double elapsedSeconds,          // 精確時間戳 (秒)
    int mouseX, int mouseY)         // 滑鼠位置 (ROI 中心)
{
    if (!m_session) return false;

    if (m_deviceFailed) return false;  // device already lost; stop silently
    
    // ====== 1. 計算 ROI Rectangle 位置 ======
    // 將圓形 ROI (滑鼠周圍 m_roiRadius 半徑) 轉為矩形邊界框
    // ⚠️ 座標必須對齊 16 的倍數 (AVC macroblock boundary)
    
    // 先限制滑鼠在有效範圍內
    int safeMouseX = (std::max)(0, (std::min)(m_width, mouseX));
    int safeMouseY = (std::max)(0, (std::min)(m_height, mouseY));
    
    int roiLeft = (std::max)(0, safeMouseX - m_roiRadius);
    int roiTop = (std::max)(0, safeMouseY - m_roiRadius);
    int roiRight = (std::min)(m_width, safeMouseX + m_roiRadius);
    int roiBottom = (std::min)(m_height, safeMouseY + m_roiRadius);
    
    // 對齊到 16 的倍數
    roiLeft = (roiLeft / 16) * 16;
    roiTop = (roiTop / 16) * 16;
    roiRight = ((roiRight + 15) / 16) * 16;  // 向上取整
    roiBottom = ((roiBottom + 15) / 16) * 16;
    
    // 再次限制不超過螢幕邊界
    int maxWidth = (m_width + 15) / 16 * 16;
    int maxHeight = (m_height + 15) / 16 * 16;
    roiRight = (std::min)(roiRight, maxWidth);
    roiBottom = (std::min)(roiBottom, maxHeight);
    
    // 確保 ROI 有效（至少 16x16），+16 後必須再次 clamp 防止超出邊界
    if (roiRight <= roiLeft) { roiLeft = (std::max)(0, roiLeft - 16); roiRight = roiLeft + 16; }
    if (roiBottom <= roiTop) { roiTop  = (std::max)(0, roiTop  - 16); roiBottom = roiTop  + 16; }
    roiRight  = (std::min)(roiRight,  maxWidth);
    roiBottom = (std::min)(roiBottom, maxHeight);
    
    // 更新 ROI 區域結構（每幀都完整設定，避免 GetVideoParam 清除後殘留零值）
    m_extEncoderROI.NumROI = 1;
    m_extEncoderROI.ROIMode = MFX_ROI_MODE_QP_DELTA;
    m_extEncoderROI.ROI[0].Left = static_cast<mfxU32>(roiLeft);
    m_extEncoderROI.ROI[0].Top = static_cast<mfxU32>(roiTop);
    m_extEncoderROI.ROI[0].Right = static_cast<mfxU32>(roiRight);
    m_extEncoderROI.ROI[0].Bottom = static_cast<mfxU32>(roiBottom);
    m_extEncoderROI.ROI[0].DeltaQP = AetherFlow::kVplRoiDefaultDeltaQp;  // -30
    
    // 🔍 第一幀：輸出 ROI 資訊驗證
    static bool firstFrameDebug = true;
    if (firstFrameDebug) {
        firstFrameDebug = false;
        int roiWidth = roiRight - roiLeft;
        int roiHeight = roiBottom - roiTop;
        float coverage = (roiWidth * roiHeight * 100.0f) / (m_width * m_height);
        
        std::cout << "\n========== ROI RECTANGLE VERIFICATION ==========\n";
        std::cout << "Mouse Position: (" << mouseX << ", " << mouseY << ")\n";
        std::cout << "ROI Rectangle: (" << roiLeft << "," << roiTop << ") to ("
                  << roiRight << "," << roiBottom << ")\n";
        std::cout << "ROI Size: " << roiWidth << "x" << roiHeight << " pixels\n";
        std::cout << "Coverage: " << coverage << "%\n";
        std::cout << "DeltaQP: " << m_extEncoderROI.ROI[0].DeltaQP << " (Background QP → ROI QP)\n";
        std::cout << "Zero-Copy Mode: " << (m_useD3D11VideoMemory ? "ENABLED" : "DISABLED") << "\n";
        std::cout << "================================================\n\n";
    }
    
    // ====== 2. 找到可用的 surface ======
    mfxFrameSurface1* pInputSurface = GetFreeSurface();
    if (!pInputSurface) {
        std::cerr << "[VPL ERROR] No free surface available (all locked)\n";
        return false;
    }

    // ====== 3. Zero-Copy: GPU→GPU merge Y+UV → NV12 ======
    // Data.MemId == D3D11 NV12 texture pointer; D3D11GetHDL returns it directly.
    // No CPU involvement — entirely on GPU.
    ID3D11Texture2D* pDstNV12 = static_cast<ID3D11Texture2D*>(pInputSurface->Data.MemId);
    if (!pDstNV12) {
        std::cerr << "[VPL ERROR] Surface MemId is null\n";
        return false;
    }
    if (!MergeYUVtoNV12_GPU(pY, pUV, pDstNV12)) {
        std::cerr << "[VPL ERROR] GPU merge Y+UV -> NV12 failed\n";
        return false;
    }
    
    // ====== 4. 設置時間戳 (精確 PTS) ======
    // 時間戳單位：90kHz (H.264 標準)
    pInputSurface->Data.TimeStamp = static_cast<mfxU64>(elapsedSeconds * 90000);
    
    // ====== 9. 附加 ROI Rectangle 擴展到 mfxEncodeCtrl ======
    // 檢查硬體是否支援 ROI (NumExtParam=0 表示不支援)
    mfxEncodeCtrl ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    
    mfxExtBuffer* extBuffersEnc[1] = { (mfxExtBuffer*)&m_extEncoderROI };

    // Intel QSV on cold GPU: the very first EncodeFrameAsync with ROI ctrl
    // triggers MFX_ERR_DEVICE_FAILED (-17) because the encoder's lazy
    // initialization hasn't completed yet.  Submit the first frame plain
    // (no ROI) to finish the warm-up, then enable ROI from frame 2 onward.
    const bool applyRoi = m_roiEnabled && m_warmupDone;
    if (!m_warmupDone) m_warmupDone = true;

    if (applyRoi) {
        ctrl.NumExtParam = 1;
        ctrl.ExtParam = extBuffersEnc;
    }
    // Otherwise ctrl stays zeroed → plain encode (warm-up frame or ROI disabled)
    
    // ====== 10. 執行非同步編碼 (submit first, drain later) ======
    // Try to drain already-finished frames first so slots can be reused.
    DrainReadyPending(0, 2);

    size_t slotIdx = static_cast<size_t>(-1);
    for (size_t i = 0; i < m_bsSlots.size(); ++i) {
        if (!m_bsSlots[i].inUse) {
            slotIdx = i;
            break;
        }
    }

    if (slotIdx == static_cast<size_t>(-1)) {
        // Backpressure: wait and drain at least one frame.
        if (!DrainOnePending(1000)) {
            std::cerr << "[VPL ERROR] No free async bitstream slot available\n";
            return false;
        }
        for (size_t i = 0; i < m_bsSlots.size(); ++i) {
            if (!m_bsSlots[i].inUse) {
                slotIdx = i;
                break;
            }
        }
    }
    if (slotIdx == static_cast<size_t>(-1)) {
        std::cerr << "[VPL ERROR] Async slot allocation failed after drain\n";
        return false;
    }

    auto& slot = m_bsSlots[slotIdx];
    slot.bs.DataLength = 0;
    slot.bs.DataOffset = 0;
    slot.sync = nullptr;

    for (;;) {
        mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(m_session, &ctrl, pInputSurface, &slot.bs, &slot.sync);
        if (sts == MFX_ERR_NONE) {
            slot.inUse = true;
            m_pendingSlots.push_back(slotIdx);
            break;
        }
        if (sts == MFX_ERR_MORE_DATA) {
            break;
        }
        if (sts == MFX_WRN_DEVICE_BUSY) {
            Sleep(1);
            continue;
        }
        if (sts == -17 && !m_deviceFailed) {  // MFX_ERR_DEVICE_FAILED
            m_deviceFailed = true;
            std::cerr << "[VPL ERROR] GPU device lost during encode (status=-17). Stopping encoder.\n";
        } else if (sts != -17) {
            std::cerr << "[VPL ERROR] EncodeFrameAsync failed, status=" << sts << "\n";
        }
        return false;
    }

    // Keep pipeline flowing: lightly drain ready frames without blocking long.
    if (m_pendingSlots.size() >= static_cast<size_t>(m_asyncDepth)) {
        DrainOnePending(1);
    } else {
        DrainReadyPending(0, 1);
    }

    return true;
}
bool VplH264Wrapper::InitBitstreamSlots(int packetSize, int slotCount) {
    m_bsSlots.clear();
    m_pendingSlots.clear();
    m_bsSlots.resize(static_cast<size_t>(slotCount));

    for (auto& slot : m_bsSlots) {
        slot.storage.resize(static_cast<size_t>(packetSize));
        if (slot.storage.empty()) return false;
        memset(&slot.bs, 0, sizeof(slot.bs));
        slot.bs.MaxLength = static_cast<mfxU32>(packetSize);
        slot.bs.Data = slot.storage.data();
        slot.bs.DataOffset = 0;
        slot.bs.DataLength = 0;
        slot.sync = nullptr;
        slot.inUse = false;
    }
    return true;
}

bool VplH264Wrapper::WriteBitstreamSample(const mfxBitstream& bs) {
    if (bs.DataLength == 0) return true;

    const bool isKeyFrame = (m_frameIndex == 0) ||
        (m_mfxEncParams.mfx.GopPicSize > 0 &&
         (m_frameIndex % m_mfxEncParams.mfx.GopPicSize) == 0);

    // SRT/TS live sink tap (same seam as the NVENC DrainSlot tap). Runs on
    // the encode/consumer thread, never the capture thread; the sink copies
    // into its bounded queue and returns. FrameType is authoritative when the
    // driver set it; otherwise fall back to the GOP-cadence heuristic used
    // for the MP4 CleanPoint below.
    if (m_encodedSink) {
        AetherFlow::EncodedAccessUnit au;
        au.data = bs.Data + bs.DataOffset;
        au.size = bs.DataLength;
        // Real 90 kHz capture pts round-trips through the driver from the
        // input surface stamp (Data.TimeStamp at submit) — same provenance as
        // the NVENC tap. Frame-index synthesis is only the fallback when the
        // driver reports no timestamp; synthetic pts drift against wall clock
        // whenever capture drops frames.
        if (bs.TimeStamp != static_cast<mfxU64>(MFX_TIMESTAMP_UNKNOWN)) {
            au.pts90k = bs.TimeStamp;
        } else {
            const unsigned fpsSafe = (m_fps > 0) ? static_cast<unsigned>(m_fps) : 30u;
            au.pts90k = static_cast<uint64_t>(m_frameIndex) * (90000ull / fpsSafe);
        }
        au.keyframe = bs.FrameType
            ? ((bs.FrameType & (MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_I)) != 0)
            : isKeyFrame;
        if (bs.FrameType == 0 && !m_srtKeyframeHeuristicLogged) {
            // Intel-hardware validation checklist item: if this prints, the
            // driver is not populating FrameType and mid-stream joins depend
            // on the GOP-cadence heuristic above being exact.
            std::cout << "[VPL][SRT] driver did not populate FrameType; "
                         "using GOP-cadence keyframe heuristic for the live stream\n";
            m_srtKeyframeHeuristicLogged = true;
        }
        m_encodedSink->OnEncodedAccessUnit(au);
    }

    if (!m_sinkWriter) return true;

    CComPtr<IMFMediaBuffer> pBuf;
    MFCreateMemoryBuffer(bs.DataLength, &pBuf);
    BYTE* pDst = nullptr;
    pBuf->Lock(&pDst, nullptr, nullptr);
    memcpy(pDst, bs.Data + bs.DataOffset, bs.DataLength);
    pBuf->Unlock();
    pBuf->SetCurrentLength(bs.DataLength);

    CComPtr<IMFSample> pSample;
    MFCreateSample(&pSample);
    pSample->AddBuffer(pBuf);

    const LONGLONG frameDuration = (m_fps > 0) ? (10000000LL / m_fps) : 333333LL;
    pSample->SetSampleTime(m_frameIndex * frameDuration);
    pSample->SetSampleDuration(frameDuration);

    if (isKeyFrame) pSample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

    m_sinkWriter->WriteSample(m_mfStreamIndex, pSample);

    m_debugTotalBytes += bs.DataLength;
    m_debugEncodedCount++;
    if (m_debugEncodedCount == 1 || m_debugEncodedCount == 100 || m_debugEncodedCount == 300) {
        std::cout << "[ROI VERIFY] Frame " << m_debugEncodedCount
                  << ": bitstream=" << bs.DataLength << " bytes"
                  << ", avg=" << (m_debugTotalBytes / static_cast<size_t>(m_debugEncodedCount))
                  << " bytes/frame\n";
    }

    m_frameIndex++;
    return true;
}

bool VplH264Wrapper::DrainOnePending(mfxU32 waitMs) {
    if (m_pendingSlots.empty()) return false;

    const size_t slotIdx = m_pendingSlots.front();
    auto& slot = m_bsSlots[slotIdx];
    if (!slot.inUse || !slot.sync) {
        slot.inUse = false;
        slot.sync = nullptr;
        slot.bs.DataOffset = 0;
        slot.bs.DataLength = 0;
        m_pendingSlots.pop_front();
        return false;
    }

    mfxStatus syncSts = MFXVideoCORE_SyncOperation(m_session, slot.sync, waitMs);
    if (syncSts == MFX_WRN_IN_EXECUTION || syncSts == MFX_WRN_DEVICE_BUSY) {
        return false;
    }
    if (syncSts != MFX_ERR_NONE) {
        if (syncSts == -17 && !m_deviceFailed) {  // MFX_ERR_DEVICE_FAILED
            m_deviceFailed = true;
            std::cerr << "[VPL ERROR] GPU device lost (status=-17). Stopping encoder.\n";
        } else if (syncSts != -17) {
            std::cerr << "[VPL ERROR] SyncOperation failed, status=" << syncSts << "\n";
        }
    } else {
        WriteBitstreamSample(slot.bs);
    }

    slot.bs.DataOffset = 0;
    slot.bs.DataLength = 0;
    slot.sync = nullptr;
    slot.inUse = false;
    m_pendingSlots.pop_front();
    return (syncSts == MFX_ERR_NONE);
}

int VplH264Wrapper::DrainReadyPending(mfxU32 waitMsPerFrame, int maxFrames) {
    int drained = 0;
    while (!m_pendingSlots.empty() && drained < maxFrames) {
        if (!DrainOnePending(waitMsPerFrame)) {
            break;
        }
        drained++;
    }
    return drained;
}

void VplH264Wrapper::Flush() {
    std::cout << "[VPL] Flushing encoder (retrieving remaining frames)...\n";

    for (;;) {
        size_t freeIdx = static_cast<size_t>(-1);
        for (size_t i = 0; i < m_bsSlots.size(); ++i) {
            if (!m_bsSlots[i].inUse) {
                freeIdx = i;
                break;
            }
        }
        if (freeIdx == static_cast<size_t>(-1)) {
            DrainOnePending(60000);
            continue;
        }

        auto& slot = m_bsSlots[freeIdx];
        slot.bs.DataOffset = 0;
        slot.bs.DataLength = 0;
        slot.sync = nullptr;

        mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(m_session, nullptr, nullptr, &slot.bs, &slot.sync);
        if (sts == MFX_ERR_MORE_DATA) {
            break;
        }
        if (sts == MFX_WRN_DEVICE_BUSY) {
            Sleep(1);
            continue;
        }
        if (sts != MFX_ERR_NONE) {
            std::cerr << "[VPL ERROR] Flush EncodeFrameAsync failed, status=" << sts << "\n";
            break;
        }

        slot.inUse = true;
        m_pendingSlots.push_back(freeIdx);
    }

    int flushedCount = 0;
    while (!m_pendingSlots.empty()) {
        if (DrainOnePending(60000)) {
            flushedCount++;
        }
    }

    if (m_sinkWriter) {
        m_sinkWriter->Finalize();
        m_sinkWriter.Release();
        MFShutdown();
    }

    std::cout << "[VPL] Flushed " << flushedCount << " buffered frames\n";
    std::cout << "[VPL] MP4 output: " << MakeOutputPath("output.mp4").string() << "\n";
}

void VplH264Wrapper::Cleanup() {
    m_d3d11Surfaces.clear();
    m_d3d11VplSurfaces.clear();
    m_pendingSlots.clear();
    m_bsSlots.clear();
    if (m_session) MFXVideoENCODE_Close(m_session);
    if (m_session) MFXClose(m_session);
    if (m_loader) MFXUnload(m_loader);
    m_memoryInterface = nullptr;
    // IMFSinkWriter is finalized in Flush(); release here if Flush was not called
    if (m_sinkWriter) {
        m_sinkWriter.Release();
        MFShutdown();
    }
}





// ============================================================
// 獲取可用的 surface（零拷貝模式或 legacy 模式）
// ============================================================
mfxFrameSurface1* VplH264Wrapper::GetFreeSurface() {
    for (auto& surf : m_d3d11VplSurfaces) {
        if (surf.Data.Locked == 0) {
            return &surf;
        }
    }
    std::cerr << "[VPL] No free surface available (all locked)\n";
    return nullptr;
}

// ============================================================
// GPU 端合併 Y + UV → NV12 (零拷貝關鍵函數)
// 使用 CopySubresourceRegion 在 GPU 內直接複製，無 CPU 介入
// ============================================================
bool VplH264Wrapper::MergeYUVtoNV12_GPU(ID3D11Texture2D* pY, ID3D11Texture2D* pUV, ID3D11Texture2D* pDstNV12) {
    if (!pY || !pDstNV12 || !m_pContext) {
        std::cerr << "[D3D11] Invalid parameters for MergeYUVtoNV12_GPU\n";
        return false;
    }

    D3D11_TEXTURE2D_DESC descY, descDst;
    pY->GetDesc(&descY);
    pDstNV12->GetDesc(&descDst);

    // Fast path: pY is already NV12 (output from D3D11VideoConverter).
    // CopyResource requires IDENTICAL dimensions.  The VideoConverter output
    // (e.g. 1920×1080) can differ from the VPL encoder surface which is
    // height-aligned to 16 (e.g. 1920×1088).  When sizes differ, use
    // CopySubresourceRegion with an explicit source box on both NV12 planes;
    // this copies only the valid pixels and leaves the extra padding rows
    // untouched (encoder ignores them via CropH).
    if (descY.Format == DXGI_FORMAT_NV12 && descDst.Format == DXGI_FORMAT_NV12) {
        if (descY.Width == descDst.Width && descY.Height == descDst.Height) {
            // Exact match — fast single-call copy.
            m_pContext->CopyResource(pDstNV12, pY);
        } else {
            // Y plane (subresource 0): copy descY.Width × descY.Height texels.
            D3D11_BOX boxY = { 0u, 0u, 0u, descY.Width, descY.Height, 1u };
            m_pContext->CopySubresourceRegion(pDstNV12, 0, 0, 0, 0, pY, 0, &boxY);
            // UV plane (subresource 1): NV12 UV is half-height (descY.Height/2).
            D3D11_BOX boxUV = { 0u, 0u, 0u, descY.Width, descY.Height / 2u, 1u };
            m_pContext->CopySubresourceRegion(pDstNV12, 1, 0, 0, 0, pY, 1, &boxUV);
        }

        // Submit all pending D3D11 commands to the GPU hardware before
        // EncodeFrameAsync accesses the surface via the Video Encode Engine.
        m_pContext->Flush();

        // Cross-engine GPU fence: D3D11 3D engine (CopyResource) and Intel QSV
        // Video Encode Engine are separate hardware units.  Flush() pushes the
        // copy commands but does NOT wait for completion.  We spin on
        // D3D11_QUERY_EVENT (created in Initialize) to confirm the copy is done
        // before EncodeFrameAsync reads the surface — equivalent to what
        // nvEncMapInputResource() does internally for NVENC.
        // Intel GPU copy typically completes in <10 µs; spin overhead is negligible.
        if (m_d3dSyncQuery) {
            m_pContext->End(m_d3dSyncQuery);
            BOOL done = FALSE;
            while (m_pContext->GetData(m_d3dSyncQuery, &done,
                                       sizeof(done),
                                       D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done)
                ;
        }
        return true;
    }

    // Legacy path: separate R8_UNORM Y + R8G8_UNORM UV planes.
    // NOTE: CopySubresourceRegion from R8/R8G8 to NV12 subresources is format-
    // incompatible and silently produces zeros (green frame). This path is kept
    // only as a fallback; prefer the NV12 input path above.
    if (descY.Format == DXGI_FORMAT_R8_UNORM && pUV && descDst.Format == DXGI_FORMAT_NV12) {
        m_pContext->CopySubresourceRegion(pDstNV12, 0, 0, 0, 0, pY,  0, nullptr);
        m_pContext->CopySubresourceRegion(pDstNV12, 1, 0, 0, 0, pUV, 0, nullptr);
        return true;
    }

    std::cerr << "[D3D11] Unsupported format combination: Y=" << descY.Format
              << ", Dst=" << descDst.Format << "\n";
    return false;
}


