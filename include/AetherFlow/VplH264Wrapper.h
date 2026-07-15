#pragma once

#define ONEVPL_EXPERIMENTAL  // Enable VPL 2.x experimental APIs

#include <d3d11.h>
#include <d3d11_4.h>
#include <atlbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vpl/mfxdefs.h>
#include <vpl/mfxvideo.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxmemory.h>
#include <vector>
#include <deque>

#include "AetherFlow/IH264Encoder.h"
#include "AetherFlow/VplDefaults.h"

// ── D3D11 Frame Allocator ─────────────────────────────────────────────────
// Required for MFX_IOPATTERN_IN_VIDEO_MEMORY (zero-copy GPU pipeline).
// Without this VPL returns MFX_ERR_UNDEFINED_BEHAVIOR (-16) on every frame.
// ─────────────────────────────────────────────────────────────────────────
struct VplAllocPool {
    std::vector<CComPtr<ID3D11Texture2D>> textures;
    std::vector<mfxMemId>                 mids;
};
struct VplD3D11AllocCtx {
    CComPtr<ID3D11Device>        device;
    CComPtr<ID3D11DeviceContext> context;
    std::vector<VplAllocPool>    pools;  // one entry per Alloc() call
};

class VplH264Wrapper : public IH264Encoder {
public:
    VplH264Wrapper();
    ~VplH264Wrapper() override;

    bool Initialize(int width, int height, int fps) override;
    bool EncodeFromYUVWithROI(ID3D11Texture2D* pY, ID3D11Texture2D* pUV, double elapsedSeconds, int mouseX, int mouseY) override;
    void Flush() override;

    ID3D11Device* GetDevice() const override { return m_pDevice; }
    ID3D11DeviceContext* GetContext() const override { return m_pContext; }
    void SetRoiEnabled(bool enabled) override { m_roiEnabled = enabled; }
    void SetEncodedFrameSink(AetherFlow::IEncodedFrameSink* sink) override { m_encodedSink = sink; }
    void SetTargetBitrateKbps(int kbps) override { m_targetBitrateKbps = kbps; }

private:
    struct PendingBitstreamSlot {
        mfxBitstream bs = {};
        std::vector<mfxU8> storage;
        mfxSyncPoint sync = nullptr;
        bool inUse = false;
    };

    mfxLoader m_loader = nullptr;
    mfxSession m_session = nullptr;
    mfxVideoParam m_mfxEncParams = {};

    CComPtr<ID3D11Device> m_pDevice;
    CComPtr<ID3D11DeviceContext> m_pContext;
    mfxHDL m_devHandle = nullptr;
    
    mfxMemoryInterface* m_memoryInterface = nullptr;  // VPL 2.x memory interface
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;  // Frame rate parameter
    int m_targetBitrateKbps = 0;  // <=0 = compile-time AETHERFLOW_BITRATE default
    
    // D3D11 zero-copy surface pool (VIDEO_MEMORY mode)
    bool m_useD3D11VideoMemory = false;
    std::vector<CComPtr<ID3D11Texture2D>> m_d3d11Surfaces;
    std::vector<mfxFrameSurface1> m_d3d11VplSurfaces;
    
    // ROI (Region of Interest) Rectangle 支援
    mfxExtEncoderROI m_extEncoderROI = {};
    int m_roiRadius = AetherFlow::kVplRoiDefaultRadius;
    bool m_roiEnabled = false;

    // MP4 output via Media Foundation SinkWriter
    CComPtr<IMFSinkWriter> m_sinkWriter;
    DWORD m_mfStreamIndex = 0;
    LONGLONG m_frameIndex = 0;   // counts encoded frames for PTS calculation
    int m_asyncDepth = 4;
    // Optional live-stream tap (SRT/TS). Set once before encoding starts and
    // read on the encode/drain side in WriteBitstreamSample; the sink outlives
    // the encoder and only copies + returns. NOTE: code-complete but not yet
    // exercised on Intel hardware (this repo's canonical runs are NVENC).
    AetherFlow::IEncodedFrameSink* m_encodedSink = nullptr;
    bool m_srtKeyframeHeuristicLogged = false;  // one-shot diagnostic, see WriteBitstreamSample

    std::vector<PendingBitstreamSlot> m_bsSlots;
    std::deque<size_t> m_pendingSlots;

    int m_debugEncodedCount = 0;
    size_t m_debugTotalBytes = 0;
    bool m_deviceFailed = false;  // set on first MFX_ERR_DEVICE_FAILED; silences cascade
    bool m_warmupDone    = false;  // first frame submitted without ROI to warm up Intel QSV

    // D3D11→QSV inter-engine GPU fence: ensures CopyResource finishes before
    // the Intel Video Encode Engine reads the surface via EncodeFrameAsync.
    CComPtr<ID3D11Query> m_d3dSyncQuery;

    void Cleanup();
    mfxFrameSurface1* GetFreeSurface();
    bool MergeYUVtoNV12_GPU(ID3D11Texture2D* pY, ID3D11Texture2D* pUV, ID3D11Texture2D* pDstNV12);
    bool InitBitstreamSlots(int packetSize, int slotCount);
    bool DrainOnePending(mfxU32 waitMs);
    int DrainReadyPending(mfxU32 waitMsPerFrame, int maxFrames);
    bool WriteBitstreamSample(const mfxBitstream& bs);

    // D3D11 frame allocator (required for VIDEO_MEMORY zero-copy)
    VplD3D11AllocCtx  m_allocCtx;
    mfxFrameAllocator m_allocator = {};
};
