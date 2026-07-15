#pragma once

// NVENC backend (optional). This file is only compiled when AETHERFLOW_ENABLE_NVENC is ON.
//
// Note: We intentionally do not vendor NVIDIA headers in this repo. To enable NVENC, point
// CMake to the NVIDIA Video Codec SDK include path so we can include nvEncodeAPI.h.

#include "AetherFlow/IH264Encoder.h"
#include "AetherFlow/NvencRoiDefaults.h"
#include <nvEncodeAPI.h>

#include <atlbase.h>
#include <d3d11.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NvencH264Wrapper : public IH264Encoder {
public:
    NvencH264Wrapper();
    ~NvencH264Wrapper() override;

    bool Initialize(int width, int height, int fps) override;
    bool EncodeFrame(const EncodeFrameRequest& request) override;
    bool EncodeFromYUVWithROI(
        ID3D11Texture2D* pY,
        ID3D11Texture2D* pUV,
        double elapsedSeconds,
        int mouseX,
        int mouseY) override;
    void Flush() override;

    ID3D11Device* GetDevice() const override { return m_pDevice; }
    ID3D11DeviceContext* GetContext() const override { return m_pContext; }
    bool HasInputTexturePool() const override { return true; }
    bool AcquireInputTexture(ID3D11Texture2D** texture) override;
    void ReleaseInputTexture(ID3D11Texture2D* texture) override;
    void SetRoiEnabled(bool enabled) override { m_roiEnabled = enabled; }
    void SetEncodedFrameSink(AetherFlow::IEncodedFrameSink* sink) override { m_encodedSink = sink; }
    void SetTargetBitrateKbps(int kbps) override { m_targetBitrateKbps = kbps; }

private:
    enum class DropReason : uint8_t {
        SlotBusyAfterPreDrain = 0,
        EncoderBusy,
        MapFailed,
        InvalidState,
        InvalidInput,
        Count
    };

    struct FrameTelemetry {
        uint32_t frameIndex = 0;
        uint64_t captureTimestamp90k = 0;
        uint64_t captureTimestampUs = 0;
        uint64_t convertDoneTimestampUs = 0;
        uint64_t encodeSubmitTimestampUs = 0;
        uint64_t encodeOutputTimestampUs = 0;
        double ageAtSubmitMs = 0.0;
        double ageAtOutputMs = 0.0;
        int64_t captureStamp100ns = 0;  // real WGC stamp for the PTS sidecar
        bool valid = false;
    };

    struct BufferPair {
        uint32_t inputIndex = 0;
        NV_ENC_OUTPUT_PTR output = nullptr;
        NV_ENC_INPUT_PTR mappedInput = nullptr;
        HANDLE completionEvent = nullptr;
        bool reserved = false;
        bool inFlight = false;
        bool eos = false;
        FrameTelemetry telemetry = {};
    };

    struct RegisteredInput {
        ID3D11Texture2D* texture = nullptr;
        NV_ENC_REGISTERED_PTR handle = nullptr;
    };

    bool CreateNvidiaDevice();
    bool LoadNvencApi();
    bool OpenEncoderSession();
    bool InitializeEncoder();
    bool CreateBuffers();
    bool StartDrainThread();
    void StopDrainThread();
    bool DrainSlot(BufferPair& buffer, bool waitForEvent);
    bool DrainCompletedFrames(bool waitForAll);
    bool StartWriterThread();
    void StopWriterThread();
    void EnqueueBitstream(const char* data, size_t count);
    void EnqueueSidecarPts(int64_t captureStamp100ns);
    int FindRegisteredInputIndex(ID3D11Texture2D* texture) const;
    bool UnmapInputResourceChecked(NV_ENC_INPUT_PTR mappedInput, const char* context);
    void UpdateInflightStats();
    uint64_t NowTelemetryUs() const;
    uint64_t CaptureTimestampUsFromElapsed(double elapsedSeconds, uint64_t convertDoneUs);
    void LogDrop(DropReason reason, uint32_t slot, uint32_t frameIndex, const char* detail = nullptr);
    bool ShouldLogFrameTelemetry(const FrameTelemetry& telemetry) const;
    void LogFrameTelemetry(const FrameTelemetry& telemetry) const;
    void LogStats(const char* phase) const;
    const char* DropReasonToString(DropReason reason) const;
    void BuildQpDeltaMap(int mouseX, int mouseY);
    const char* StatusToString(NVENCSTATUS status) const;
    void Cleanup();

    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;

    int m_targetBitrateKbps = 0;  // <=0 = compile-time AETHERFLOW_BITRATE default
    int m_roiRadius = AetherFlow::kNvencRoiDefaultRadius;
    int m_roiDeltaQp = AetherFlow::kNvencRoiDefaultDeltaQp;
    int m_bgDeltaQp = AetherFlow::kNvencBgDefaultDeltaQp;
    bool m_roiEnabled = false;  // cursor ROI QP delta OFF unless SetRoiEnabled(true); main.cpp drives it from --cursor-roi
    int m_mbWidth = 0;
    int m_mbHeight = 0;

    CComPtr<ID3D11Device> m_pDevice;
    CComPtr<ID3D11DeviceContext> m_pContext;

    HMODULE m_nvencDll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_api = {};
    void* m_encoder = nullptr;
    NV_ENC_CONFIG m_encConfig = {};
    bool m_asyncEncode = true;

    std::vector<BufferPair> m_buffers;
    std::vector<RegisteredInput> m_registeredInputs;
    std::vector<int8_t> m_qpDeltaMap;
    std::ofstream m_outputFile;
    // Optional live-stream tap (SRT/TS). Set once before encoding starts and
    // read on the drain thread; the sink outlives the encoder (main.cpp
    // declares it earlier in scope) and only copies + returns.
    AetherFlow::IEncodedFrameSink* m_encodedSink = nullptr;
    // Bitstream is handed to a dedicated writer thread so disk I/O never runs
    // on the drain thread (or under m_stateMutex). A slow write only backs up
    // m_writerBuf in RAM; it can never stall slot recycling or the producer.
    std::mutex m_writerMutex;
    std::condition_variable m_writerCv;
    std::vector<char> m_writerBuf;
    std::string m_writerSidecar;            // pending mkvmerge-v2 timecode lines
    std::thread m_writerThread;
    bool m_writerStop = false;

    // Opt-in timed recording (PD2a/PD3): emit a per-encoded-frame PTS sidecar
    // (mkvmerge timecodes v2) next to output_encoded.h264. Off by default ⇒
    // canonical bitstream + verifier gates are byte-identical.
    bool m_timedRecording = false;
    std::ofstream m_sidecarFile;
    int64_t m_pendingCaptureStamp100ns = 0; // stashed by EncodeFrame()
    int64_t m_ptsBase100ns = 0;
    bool m_havePtsBase = false;
    double m_lastSidecarMs = 0.0;
    uint32_t m_frameIndex = 0;
    std::mutex m_stateMutex;
    std::condition_variable m_inputAvailableCv;
    std::thread m_drainThread;
    bool m_drainThreadStop = false;

    std::chrono::steady_clock::time_point m_telemetryStartTs = {};
    bool m_captureTimelineInitialized = false;
    uint64_t m_captureTimelineBaseUs = 0;
    bool m_frameTelemetryLogEnabled = false;
    bool m_frameTelemetryVerbose = false;
    uint32_t m_frameTelemetryLogInterval = 60;
    double m_frameTelemetryAlertMs = 999999.0;
    // Default to writing the encoded bitstream so an unflagged AetherFlow.exe
    // run leaves a playable artifact in <output>/output_encoded.h264. Disable
    // explicitly with AETHERFLOW_NVENC_WRITE_BITSTREAM=0 for pure encode-latency
    // benchmarks.
    bool m_writeBitstreamEnabled = true;

    uint64_t m_dropCount = 0;
    uint64_t m_encoderBusyCount = 0;
    uint64_t m_lockBusyCount = 0;
    uint64_t m_mapFailureCount = 0;
    uint64_t m_unmapFailureCount = 0;
    uint64_t m_maxInflightSlotsObserved = 0;
    uint64_t m_encodedFrameCount = 0;
    std::array<uint64_t, static_cast<size_t>(DropReason::Count)> m_dropReasonCounts = {};

    uint64_t m_latencySampleCount = 0;
    double m_submitAgeSumMs = 0.0;
    double m_outputAgeSumMs = 0.0;
    double m_queueSumMs = 0.0;
    double m_submitAgeMinMs = (std::numeric_limits<double>::max)();
    double m_submitAgeMaxMs = 0.0;
    double m_outputAgeMinMs = (std::numeric_limits<double>::max)();
    double m_outputAgeMaxMs = 0.0;
    double m_queueMinMs = (std::numeric_limits<double>::max)();
    double m_queueMaxMs = 0.0;
};
