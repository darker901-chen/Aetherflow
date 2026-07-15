#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <atlbase.h>

#include <string>
#include <vector>

#include "AetherFlow/IAIFrameAnalyzer.h"

namespace AetherFlow {

enum class PrivacyMaskMode {
    Blackout,
    Blur,
    Mosaic,
    Grayscale
};

struct PrivacyMaskApplyStats {
    int requestedCount = 0;
    int appliedCount = 0;
    bool fallbackUsed = false;
    std::string path = "none";
    std::string failureReason;
};

class PrivacyMaskCompositor final {
public:
    bool Initialize(
        ID3D11Device* device,
        int captureWidth,
        int captureHeight,
        int decisionWidth,
        int decisionHeight);

    // Backward-compatible blackout path. Delegates to ApplyMask with
    // PrivacyMaskMode::Blackout.
    bool ApplyBlackout(
        ID3D11Texture2D* sourceBgra,
        const FrameDecision& decision,
        ID3D11Texture2D** maskedBgra,
        PrivacyMaskApplyStats* stats);

    // Mode-aware mask application. Blackout stays on the existing
    // ID3D11DeviceContext1::ClearView fast path. Blur and Mosaic share a
    // single shader pipeline that draws a fullscreen triangle and uses a
    // scissor rect per mask region to limit output. If the shader pipeline
    // is unavailable on this device, blur/mosaic fall back to blackout and
    // PrivacyMaskApplyStats::fallbackUsed is set.
    bool ApplyMask(
        ID3D11Texture2D* sourceBgra,
        const FrameDecision& decision,
        PrivacyMaskMode mode,
        ID3D11Texture2D** maskedBgra,
        PrivacyMaskApplyStats* stats);

private:
    bool BuildRects(const FrameDecision& decision, std::vector<D3D11_RECT>* rects) const;
    bool EnsureShaderPipeline();
    bool ApplyBlackoutInternal(
        ID3D11Texture2D* sourceBgra,
        const std::vector<D3D11_RECT>& rects,
        ID3D11Texture2D** maskedBgra,
        PrivacyMaskApplyStats* stats);
    bool ApplyShaderMode(
        ID3D11Texture2D* sourceBgra,
        const std::vector<D3D11_RECT>& rects,
        PrivacyMaskMode mode,
        ID3D11Texture2D** maskedBgra,
        PrivacyMaskApplyStats* stats);

    CComPtr<ID3D11Device> m_device;
    CComPtr<ID3D11DeviceContext> m_context;
    CComPtr<ID3D11DeviceContext1> m_context1;
    CComPtr<ID3D11Texture2D> m_maskedBgra;
    CComPtr<ID3D11RenderTargetView> m_maskedBgraRtv;

    // Shader pipeline (lazily built on first blur/mosaic use). All optional;
    // failure to build any of these forces a blackout fallback for that call.
    bool m_shaderPipelineAttempted = false;
    bool m_shaderPipelineReady = false;
    CComPtr<ID3D11VertexShader> m_fullscreenVS;
    CComPtr<ID3D11PixelShader> m_blurPS;
    CComPtr<ID3D11PixelShader> m_mosaicPS;
    CComPtr<ID3D11PixelShader> m_grayscalePS;
    CComPtr<ID3D11SamplerState> m_linearClampSampler;
    CComPtr<ID3D11SamplerState> m_pointClampSampler;
    CComPtr<ID3D11Buffer> m_paramsCb;
    CComPtr<ID3D11RasterizerState> m_scissorRasterizer;
    CComPtr<ID3D11BlendState> m_opaqueBlend;
    CComPtr<ID3D11DepthStencilState> m_noDepth;

    int m_captureWidth = 0;
    int m_captureHeight = 0;
    int m_decisionWidth = 0;
    int m_decisionHeight = 0;
};

} // namespace AetherFlow
