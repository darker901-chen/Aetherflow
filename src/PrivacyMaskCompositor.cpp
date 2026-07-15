#include "AetherFlow/PrivacyMaskCompositor.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <iostream>

#pragma comment(lib, "d3dcompiler.lib")

namespace AetherFlow {

namespace {

int ScaleFloor(int value, int srcExtent, int dstExtent) {
    return static_cast<int>((static_cast<long long>(value) * dstExtent) / srcExtent);
}

int ScaleCeil(int value, int srcExtent, int dstExtent) {
    return static_cast<int>(
        (static_cast<long long>(value) * dstExtent + srcExtent - 1) / srcExtent);
}

// Shared fullscreen-triangle vertex shader. Three verts, no vertex buffer.
// Coverage is restricted by a per-rect scissor rect set by the caller.
constexpr const char* kFullscreenVsHlsl = R"HLSL(
struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut VSMain(uint vid : SV_VertexID) {
    VsOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)HLSL";

// Box blur. Samples a 9x9 grid stepped by 2 texels for ~17px effective
// radius. Heavy enough to make text unreadable, cheap enough to stay well
// inside the 4 ms mask budget for the rect sizes we expect.
constexpr const char* kBlurPsHlsl = R"HLSL(
Texture2D<float4>    srcTex     : register(t0);
SamplerState         srcSampler : register(s0);

cbuffer Params : register(b0) {
    float2 textureSizePixels;
    float2 cellSizePixels; // unused for blur
};

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 texel = float2(2.0, 2.0) / textureSizePixels;
    float4 sum = float4(0, 0, 0, 0);
    [unroll] for (int dy = -4; dy <= 4; ++dy) {
        [unroll] for (int dx = -4; dx <= 4; ++dx) {
            sum += srcTex.Sample(srcSampler, uv + float2(dx, dy) * texel);
        }
    }
    return sum / 81.0;
}
)HLSL";

// Mosaic. Snaps each fragment's UV to the centre of a cellSizePixels grid so
// an N x N block of pixels collapses to one colour.
constexpr const char* kMosaicPsHlsl = R"HLSL(
Texture2D<float4>    srcTex     : register(t0);
SamplerState         srcSampler : register(s0);

cbuffer Params : register(b0) {
    float2 textureSizePixels;
    float2 cellSizePixels;
};

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 cellSizeUV = cellSizePixels / textureSizePixels;
    float2 snappedUV  = (floor(uv / cellSizeUV) + 0.5) * cellSizeUV;
    return srcTex.Sample(srcSampler, snappedUV);
}
)HLSL";

// Grayscale. Desaturates each pixel to its luma so colour is removed but the
// image stays sharp -- visually distinct from blur (fuzzy) and mosaic
// (blocky). Used for the `slides` demo class so all five scene classes map to
// a distinguishable effect. The cbuffer is declared for pipeline layout
// compatibility (shared MaskShaderParams) but unused here.
constexpr const char* kGrayscalePsHlsl = R"HLSL(
Texture2D<float4>    srcTex     : register(t0);
SamplerState         srcSampler : register(s0);

cbuffer Params : register(b0) {
    float2 textureSizePixels; // unused for grayscale
    float2 cellSizePixels;    // unused for grayscale
};

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = srcTex.Sample(srcSampler, uv);
    float g = dot(c.rgb, float3(0.299, 0.587, 0.114));
    return float4(g, g, g, c.a);
}
)HLSL";

struct alignas(16) MaskShaderParams {
    float textureSizePixels[2];
    float cellSizePixels[2];
};

bool CompileShader(const char* source, const char* entry, const char* profile, ID3DBlob** blob) {
    if (!source || !entry || !profile || !blob) {
        return false;
    }
    *blob = nullptr;
    CComPtr<ID3DBlob> err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(
        source,
        strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        profile,
        flags,
        0,
        blob,
        &err);
    if (FAILED(hr)) {
        if (err) {
            std::cerr << "[PrivacyMaskCompositor] Shader compile failed ("
                      << entry << " " << profile << "): "
                      << static_cast<const char*>(err->GetBufferPointer()) << "\n";
        } else {
            std::cerr << "[PrivacyMaskCompositor] Shader compile failed ("
                      << entry << " " << profile << "): hr=" << std::hex << hr << "\n";
        }
        return false;
    }
    return *blob != nullptr;
}

} // namespace

bool PrivacyMaskCompositor::Initialize(
    ID3D11Device* device,
    int captureWidth,
    int captureHeight,
    int decisionWidth,
    int decisionHeight) {
    if (!device || captureWidth <= 0 || captureHeight <= 0 || decisionWidth <= 0 || decisionHeight <= 0) {
        return false;
    }

    m_device = device;
    m_captureWidth = captureWidth;
    m_captureHeight = captureHeight;
    m_decisionWidth = decisionWidth;
    m_decisionHeight = decisionHeight;

    m_device->GetImmediateContext(&m_context);
    if (!m_context) {
        return false;
    }

    if (FAILED(m_context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&m_context1))) ||
        !m_context1) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(m_captureWidth);
    desc.Height = static_cast<UINT>(m_captureHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_maskedBgra)) || !m_maskedBgra) {
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = desc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(m_device->CreateRenderTargetView(m_maskedBgra, &rtvDesc, &m_maskedBgraRtv)) ||
        !m_maskedBgraRtv) {
        return false;
    }

    // Eagerly compile the blur/mosaic shader pipeline. Doing this in
    // Initialize() means the first frame that uses a non-blackout mode does
    // not pay the HLSL compile + state object creation cost (observed at
    // ~48 ms on a 1080p panic frame). If compile fails, the compositor still
    // works in Blackout mode and ApplyShaderMode falls back per call.
    EnsureShaderPipeline();
    return true;
}

bool PrivacyMaskCompositor::BuildRects(const FrameDecision& decision, std::vector<D3D11_RECT>* rects) const {
    if (!rects || m_captureWidth <= 0 || m_captureHeight <= 0 || m_decisionWidth <= 0 || m_decisionHeight <= 0) {
        return false;
    }

    rects->clear();
    rects->reserve(decision.privacyMasks.size());
    for (const auto& mask : decision.privacyMasks) {
        int left = ScaleFloor(mask.left, m_decisionWidth, m_captureWidth);
        int top = ScaleFloor(mask.top, m_decisionHeight, m_captureHeight);
        int right = ScaleCeil(mask.right, m_decisionWidth, m_captureWidth);
        int bottom = ScaleCeil(mask.bottom, m_decisionHeight, m_captureHeight);

        left = (std::max)(0, (std::min)(m_captureWidth, left));
        top = (std::max)(0, (std::min)(m_captureHeight, top));
        right = (std::max)(0, (std::min)(m_captureWidth, right));
        bottom = (std::max)(0, (std::min)(m_captureHeight, bottom));

        if (right <= left || bottom <= top) {
            continue;
        }

        D3D11_RECT rect = {};
        rect.left = static_cast<LONG>(left);
        rect.top = static_cast<LONG>(top);
        rect.right = static_cast<LONG>(right);
        rect.bottom = static_cast<LONG>(bottom);
        rects->push_back(rect);
    }

    return !rects->empty();
}

bool PrivacyMaskCompositor::EnsureShaderPipeline() {
    if (m_shaderPipelineReady) {
        return true;
    }
    if (m_shaderPipelineAttempted) {
        return false;
    }
    m_shaderPipelineAttempted = true;

    CComPtr<ID3DBlob> vsBlob, blurBlob, mosaicBlob, grayscaleBlob;
    if (!CompileShader(kFullscreenVsHlsl, "VSMain", "vs_4_0", &vsBlob)) return false;
    if (!CompileShader(kBlurPsHlsl, "PSMain", "ps_4_0", &blurBlob)) return false;
    if (!CompileShader(kMosaicPsHlsl, "PSMain", "ps_4_0", &mosaicBlob)) return false;
    if (!CompileShader(kGrayscalePsHlsl, "PSMain", "ps_4_0", &grayscaleBlob)) return false;

    if (FAILED(m_device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fullscreenVS)) ||
        !m_fullscreenVS) {
        return false;
    }
    if (FAILED(m_device->CreatePixelShader(
            blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(), nullptr, &m_blurPS)) ||
        !m_blurPS) {
        return false;
    }
    if (FAILED(m_device->CreatePixelShader(
            mosaicBlob->GetBufferPointer(), mosaicBlob->GetBufferSize(), nullptr, &m_mosaicPS)) ||
        !m_mosaicPS) {
        return false;
    }
    if (FAILED(m_device->CreatePixelShader(
            grayscaleBlob->GetBufferPointer(), grayscaleBlob->GetBufferSize(), nullptr, &m_grayscalePS)) ||
        !m_grayscalePS) {
        return false;
    }

    D3D11_SAMPLER_DESC sampLinear = {};
    sampLinear.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampLinear.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampLinear.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampLinear.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampLinear.MinLOD = 0;
    sampLinear.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&sampLinear, &m_linearClampSampler))) {
        return false;
    }

    D3D11_SAMPLER_DESC sampPoint = sampLinear;
    sampPoint.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(m_device->CreateSamplerState(&sampPoint, &m_pointClampSampler))) {
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(MaskShaderParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, &m_paramsCb))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.ScissorEnable = TRUE;
    rsDesc.DepthClipEnable = TRUE;
    if (FAILED(m_device->CreateRasterizerState(&rsDesc, &m_scissorRasterizer))) {
        return false;
    }

    D3D11_BLEND_DESC bsDesc = {};
    bsDesc.RenderTarget[0].BlendEnable = FALSE;
    bsDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&bsDesc, &m_opaqueBlend))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = FALSE;
    dsDesc.StencilEnable = FALSE;
    if (FAILED(m_device->CreateDepthStencilState(&dsDesc, &m_noDepth))) {
        return false;
    }

    m_shaderPipelineReady = true;
    return true;
}

bool PrivacyMaskCompositor::ApplyBlackoutInternal(
    ID3D11Texture2D* sourceBgra,
    const std::vector<D3D11_RECT>& rects,
    ID3D11Texture2D** maskedBgra,
    PrivacyMaskApplyStats* stats) {
    m_context->CopyResource(m_maskedBgra, sourceBgra);
    const FLOAT black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_context1->ClearView(
        m_maskedBgraRtv,
        black,
        rects.data(),
        static_cast<UINT>(rects.size()));

    m_maskedBgra.p->AddRef();
    *maskedBgra = m_maskedBgra.p;

    if (stats) {
        stats->appliedCount = static_cast<int>(rects.size());
        if (stats->path.empty() || stats->path == "none") {
            stats->path = "d3d11-bgra-clearview";
        }
    }
    return true;
}

bool PrivacyMaskCompositor::ApplyShaderMode(
    ID3D11Texture2D* sourceBgra,
    const std::vector<D3D11_RECT>& rects,
    PrivacyMaskMode mode,
    ID3D11Texture2D** maskedBgra,
    PrivacyMaskApplyStats* stats) {
    if (!EnsureShaderPipeline()) {
        if (stats) {
            stats->fallbackUsed = true;
            if (stats->failureReason.empty()) {
                stats->failureReason = "shader pipeline unavailable; fell back to blackout";
            }
            stats->path = "d3d11-bgra-clearview-fallback";
        }
        return ApplyBlackoutInternal(sourceBgra, rects, maskedBgra, stats);
    }

    // Unmasked pixels: copy source verbatim into maskedBgra. The shader pass
    // below only writes inside scissor rects, so everything outside stays as
    // original capture data.
    m_context->CopyResource(m_maskedBgra, sourceBgra);

    // Build an SRV over the source BGRA so the shader can sample it. WGC
    // textures observed so far carry D3D11_BIND_SHADER_RESOURCE; if a future
    // adapter does not, SRV creation fails and we fall back to blackout.
    CComPtr<ID3D11ShaderResourceView> srv;
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        HRESULT hr = m_device->CreateShaderResourceView(sourceBgra, &srvDesc, &srv);
        if (FAILED(hr) || !srv) {
            if (stats) {
                stats->fallbackUsed = true;
                stats->failureReason = "source BGRA does not expose an SRV; fell back to blackout";
                stats->path = "d3d11-bgra-clearview-fallback";
            }
            return ApplyBlackoutInternal(sourceBgra, rects, maskedBgra, stats);
        }
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_paramsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            if (stats) {
                stats->fallbackUsed = true;
                stats->failureReason = "failed to map shader params; fell back to blackout";
                stats->path = "d3d11-bgra-clearview-fallback";
            }
            return ApplyBlackoutInternal(sourceBgra, rects, maskedBgra, stats);
        }
        MaskShaderParams params = {};
        params.textureSizePixels[0] = static_cast<float>(m_captureWidth);
        params.textureSizePixels[1] = static_cast<float>(m_captureHeight);
        params.cellSizePixels[0] = 16.0f;
        params.cellSizePixels[1] = 16.0f;
        std::memcpy(mapped.pData, &params, sizeof(params));
        m_context->Unmap(m_paramsCb, 0);
    }

    ID3D11RenderTargetView* rtvs[1] = { m_maskedBgraRtv };
    m_context->OMSetRenderTargets(1, rtvs, nullptr);
    m_context->OMSetDepthStencilState(m_noDepth, 0);
    const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_context->OMSetBlendState(m_opaqueBlend, blendFactor, 0xFFFFFFFFu);
    m_context->RSSetState(m_scissorRasterizer);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = static_cast<float>(m_captureWidth);
    vp.Height = static_cast<float>(m_captureHeight);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    m_context->RSSetViewports(1, &vp);

    m_context->IASetInputLayout(nullptr);
    ID3D11Buffer* nullVb[1] = { nullptr };
    UINT zeroStride = 0;
    UINT zeroOffset = 0;
    m_context->IASetVertexBuffers(0, 1, nullVb, &zeroStride, &zeroOffset);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_context->VSSetShader(m_fullscreenVS, nullptr, 0);
    ID3D11PixelShader* ps =
        (mode == PrivacyMaskMode::Mosaic)    ? m_mosaicPS.p
        : (mode == PrivacyMaskMode::Grayscale) ? m_grayscalePS.p
        : m_blurPS.p;
    m_context->PSSetShader(ps, nullptr, 0);
    ID3D11SamplerState* sampler =
        (mode == PrivacyMaskMode::Mosaic) ? m_pointClampSampler.p : m_linearClampSampler.p;
    ID3D11SamplerState* samplers[1] = { sampler };
    m_context->PSSetSamplers(0, 1, samplers);
    ID3D11ShaderResourceView* srvs[1] = { srv.p };
    m_context->PSSetShaderResources(0, 1, srvs);
    ID3D11Buffer* cbs[1] = { m_paramsCb.p };
    m_context->PSSetConstantBuffers(0, 1, cbs);

    int applied = 0;
    for (const auto& rect : rects) {
        D3D11_RECT scissor = rect;
        m_context->RSSetScissorRects(1, &scissor);
        m_context->Draw(3, 0);
        ++applied;
    }

    // Unbind SRV so the same texture can be re-bound as input next frame
    // without a hazard warning.
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    m_context->PSSetShaderResources(0, 1, nullSrv);
    ID3D11RenderTargetView* nullRtv[1] = { nullptr };
    m_context->OMSetRenderTargets(1, nullRtv, nullptr);

    m_maskedBgra.p->AddRef();
    *maskedBgra = m_maskedBgra.p;

    if (stats) {
        stats->appliedCount = applied;
        stats->path = (mode == PrivacyMaskMode::Mosaic)
            ? "d3d11-bgra-mosaic-shader"
            : (mode == PrivacyMaskMode::Grayscale)
            ? "d3d11-bgra-grayscale-shader"
            : "d3d11-bgra-blur-shader";
        stats->fallbackUsed = false;
    }
    return true;
}

bool PrivacyMaskCompositor::ApplyMask(
    ID3D11Texture2D* sourceBgra,
    const FrameDecision& decision,
    PrivacyMaskMode mode,
    ID3D11Texture2D** maskedBgra,
    PrivacyMaskApplyStats* stats) {
    if (stats) {
        *stats = {};
        stats->requestedCount = static_cast<int>(decision.privacyMasks.size());
    }
    if (maskedBgra) {
        *maskedBgra = nullptr;
    }

    if (!sourceBgra || !maskedBgra || decision.privacyMasks.empty()) {
        return false;
    }
    if (!m_context || !m_context1 || !m_maskedBgra || !m_maskedBgraRtv) {
        if (stats) {
            stats->failureReason = "privacy mask compositor is not initialized";
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    sourceBgra->GetDesc(&srcDesc);
    if (srcDesc.Width != static_cast<UINT>(m_captureWidth) ||
        srcDesc.Height != static_cast<UINT>(m_captureHeight) ||
        srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        srcDesc.SampleDesc.Count != 1) {
        if (stats) {
            stats->failureReason = "capture texture is not single-sample BGRA at expected size";
        }
        return false;
    }

    std::vector<D3D11_RECT> rects;
    if (!BuildRects(decision, &rects)) {
        if (stats) {
            stats->failureReason = "no valid privacy mask rectangles";
        }
        return false;
    }

    if (mode == PrivacyMaskMode::Blackout) {
        return ApplyBlackoutInternal(sourceBgra, rects, maskedBgra, stats);
    }
    return ApplyShaderMode(sourceBgra, rects, mode, maskedBgra, stats);
}

bool PrivacyMaskCompositor::ApplyBlackout(
    ID3D11Texture2D* sourceBgra,
    const FrameDecision& decision,
    ID3D11Texture2D** maskedBgra,
    PrivacyMaskApplyStats* stats) {
    return ApplyMask(sourceBgra, decision, PrivacyMaskMode::Blackout, maskedBgra, stats);
}

} // namespace AetherFlow
