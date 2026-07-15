#pragma once

#include <d3d11.h>
#include "AetherFlow/IAIFrameAnalyzer.h"

namespace AetherFlow {
class IEncodedFrameSink;  // see AetherFlow/IEncodedFrameSink.h
}

struct EncodeFrameRequest {
    ID3D11Texture2D* nv12Texture = nullptr;
    double elapsedSeconds = 0.0;
    // Real capture presentation time (WGC SystemRelativeTime, 100ns units) for
    // this frame. 0 = unknown. Used only for the opt-in timed-recording PTS
    // sidecar; elapsedSeconds (synthetic) still drives encode/telemetry so the
    // canonical bitstream stays byte-stable.
    int64_t captureTimestamp100ns = 0;
    AetherFlow::FrameDecision decision;
    int fallbackRoiCenterX = 0;
    int fallbackRoiCenterY = 0;
};

// Minimal interface so we can swap encoder backends (oneVPL / NVENC / etc.)
class IH264Encoder {
public:
    virtual ~IH264Encoder() = default;

    virtual bool Initialize(int width, int height, int fps) = 0;

    // Input is the same as the current pipeline: two GPU planes (Y + UV) produced by our converter.
    // elapsedSeconds is used to generate timestamps.
    virtual bool EncodeFromYUVWithROI(
        ID3D11Texture2D* pY,
        ID3D11Texture2D* pUV,
        double elapsedSeconds,
        int mouseX,
        int mouseY) = 0;

    // Decision-layer entry point. Existing backends can keep overriding only
    // EncodeFromYUVWithROI(); this wrapper preserves the current ABI surface
    // while callers migrate from mouse-only ROI to structured frame decisions.
    virtual bool EncodeFrame(const EncodeFrameRequest& request) {
        int roiX = request.fallbackRoiCenterX;
        int roiY = request.fallbackRoiCenterY;
        if (request.decision.HasQualityRoi()) {
            const auto& roi = request.decision.qualityRegions.front();
            roiX = roi.CenterX();
            roiY = roi.CenterY();
        }
        return EncodeFromYUVWithROI(
            request.nv12Texture,
            nullptr,
            request.elapsedSeconds,
            roiX,
            roiY);
    }

    // Enable/disable the cursor-tracking ROI QP delta. Default no-op so
    // backends without ROI keep working. When disabled, a backend must apply a
    // uniform QP (no per-region boost). main.cpp sets this from the
    // --cursor-roi flag (default off; static --roi-x/--roi-y implies on).
    virtual void SetRoiEnabled(bool enabled) { (void)enabled; }

    // Optional tap on the encoded output (SRT/TS live streaming). Backends
    // that support it call sink->OnEncodedAccessUnit(...) from their
    // drain/writer-side thread for every encoded access unit. Default no-op
    // so backends without streaming support keep working. Set before the
    // first EncodeFrame; the sink must outlive the encoder.
    virtual void SetEncodedFrameSink(AetherFlow::IEncodedFrameSink* sink) { (void)sink; }

    // Optional runtime bitrate override in kbps. Must be called BEFORE
    // Initialize(); backends fall back to the compile-time AETHERFLOW_BITRATE
    // when unset (<= 0). Default no-op so backends without the knob keep
    // working (same pattern as SetRoiEnabled).
    virtual void SetTargetBitrateKbps(int kbps) { (void)kbps; }

    virtual void Flush() = 0;

    virtual ID3D11Device* GetDevice() const = 0;
    virtual ID3D11DeviceContext* GetContext() const = 0;

    // Optional fast path: an encoder can expose pre-registered GPU input
    // textures. The caller copies NV12 directly into the acquired texture and
    // passes that same pointer to EncodeFromYUVWithROI(). On success, the
    // encoder owns the texture until its internal drain path releases it.
    virtual bool HasInputTexturePool() const { return false; }
    virtual bool AcquireInputTexture(ID3D11Texture2D** texture) {
        if (texture) *texture = nullptr;
        return false;
    }
    virtual void ReleaseInputTexture(ID3D11Texture2D* texture) {
        (void)texture;
    }
};

