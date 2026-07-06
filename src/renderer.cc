#include "renderer.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

#include "session.h"

namespace {

constexpr float kBlurMarginCss = 40.0f;

const char kShaderSource[] = R"hlsl(
cbuffer P : register(b0) {
  float2 srcOrigin; float2 srcSpan;
  float2 outSize;   float2 dir;
  float2 glassSize; float radius; float bezel;
  float maxBend; float dispFactor; float aberration; float saturation;
  float margin; float alpha; float2 pad;
};

Texture2D tex : register(t0);
SamplerState smp : register(s0);

// 全屏三角形
float4 VSMain(uint id : SV_VertexID) : SV_Position {
  float2 pos = float2((id << 1) & 2, id & 2);
  return float4(pos * float2(2, -2) + float2(-1, 1), 0, 1);
}

// 下采样 + 单方向高斯
float4 PSBlur(float4 pos : SV_Position) : SV_Target {
  float2 t = pos.xy / outSize;
  float2 uv = srcOrigin + t * srcSpan;
  return tex.Sample(smp, uv) * 0.4026
    + (tex.Sample(smp, uv + dir) + tex.Sample(smp, uv - dir)) * 0.2442
    + (tex.Sample(smp, uv + dir * 2.0) + tex.Sample(smp, uv - dir * 2.0)) * 0.0545;
}

float sdRoundRect(float2 p, float2 halfSize, float r) {
  float2 q = abs(p) - halfSize + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// 平滑 max(q,0)：法线方向在角区连续旋转，消除对角线上的方向折痕
float2 softClamp(float2 q, float soft) {
  return 0.5 * (q + sqrt(q * q + soft * soft));
}

float2 toBlurUV(float2 px) {
  return (px + margin) / (glassSize + margin * 2.0);
}

// 透镜主着色：SDF 位移 + RGB 色散 + 饱和度，输出预乘 alpha（圆角 AA 覆盖 × 面板不透明度）
float4 PSLens(float4 pos : SV_Position) : SV_Target {
  float2 css = pos.xy;
  float2 halfSize = glassSize * 0.5;
  float2 p = css - halfSize;
  float r = min(radius, min(halfSize.x, halfSize.y));

  float sd = sdRoundRect(p, halfSize, r);
  float coverage = saturate(0.5 - sd);
  if (coverage <= 0.0) return float4(0, 0, 0, 0);

  // 凸透镜边缘（外向折射）：玻璃边缘把边界外的内容压缩折进边缘带，与
  // iOS 液态玻璃/玻璃球边缘的光学一致。剖面 (1-t)²：边界处向外看得最远
  //（maxBend），沿深度平滑衰减到 0（C¹ 衔接平整中心）。映射斜率
  // = 1 + 2·maxBend/bezel·(1-t) ≥ 1：处处压缩，结构上不存在拉伸/鬼影
  float depth = -sd;
  float2 disp = float2(0, 0);
  float2 nrm = float2(0, 0);
  float humpV = 0.0;
  if (depth > 0.0 && depth < bezel) {
    float2 q = abs(p) - halfSize + r;
    // 法线方向平滑过角（软化系数取圆角半径量级），位移在角区连续旋转
    float2 qs = softClamp(q, max(r * 0.8, 1.0));
    nrm = (qs / max(length(qs), 1e-4)) * sign(p + float2(1e-6, 1e-6));
    float t = depth / bezel;
    humpV = (1.0 - t) * (1.0 - t);
    disp = nrm * (humpV * maxBend) * dispFactor;
  }

  // 压缩带内做 5 tap 足迹积分（径向 + 切向拉丝）：边缘带是 2~3 倍缩小
  // 映射，点采样会产生锯齿/摩尔纹；径向扩散随压缩强度自适应加宽做
  // 抗锯齿，同时保留可辨认的压缩内容（苹果边缘的"流动光带"质感）
  float3 c;
  float dispLen2 = dot(disp, disp);
  if (dispLen2 > 0.25) {
    float2 tangent = float2(nrm.y, -nrm.x) * sqrt(dispLen2);
    float rs = 0.05 + 0.10 * humpV;
    float radial[5] = { 1.0 - 2.0 * rs, 1.0 - rs, 1.0, 1.0 + rs, 1.0 + 2.0 * rs };
    const float lateral[5] = { -0.36, 0.18, 0.0, -0.18, 0.36 };
    const float wts[5] = { 0.14, 0.22, 0.28, 0.22, 0.14 };
    c = float3(0, 0, 0);
    [unroll] for (int i = 0; i < 5; ++i) {
      float2 d = disp * radial[i] + tangent * lateral[i];
      c += wts[i] * float3(
          tex.Sample(smp, toBlurUV(css + d)).r,
          tex.Sample(smp, toBlurUV(css + d * (1.0 - aberration * 0.05))).g,
          tex.Sample(smp, toBlurUV(css + d * (1.0 - aberration * 0.1))).b);
    }
  } else {
    c = tex.Sample(smp, toBlurUV(css + disp)).rgb;
  }
  float lum = dot(c, float3(0.213, 0.715, 0.072));
  c = lerp(lum.xxx, c, saturation);

  float a = coverage * alpha;
  return float4(c * a, a);
}
)hlsl";

struct ShaderParams {
    float srcOrigin[2];
    float srcSpan[2];
    float outSize[2];
    float dir[2];
    float glassSize[2];
    float radius;
    float bezel;
    float maxBend;
    float dispFactor;
    float aberration;
    float saturation;
    float margin;
    float alpha;
    float pad[2];
};

HRESULT CompileShader(const char* entry, const char* target, ComPtr<ID3DBlob>* blob) {
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            blob->GetAddressOf(), &errors);
    return hr;
}

}  // namespace

RECT GlassRenderer::RegionForPanel(const RECT& panelBounds, const GlassParams& params, float dpr) {
    const LONG margin = static_cast<LONG>(std::lround(kBlurMarginCss * dpr));
    const LONG offsetX = static_cast<LONG>(std::lround(params.sourceOffsetX));
    const LONG offsetY = static_cast<LONG>(std::lround(params.sourceOffsetY));
    return RECT{ panelBounds.left + offsetX - margin, panelBounds.top + offsetY - margin,
                 panelBounds.right + offsetX + margin, panelBounds.bottom + offsetY + margin };
}

HRESULT GlassRenderer::Initialize(ID3D11Device* device) {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> blurBlob;
    ComPtr<ID3DBlob> lensBlob;
    HRESULT hr = CompileShader("VSMain", "vs_5_0", &vsBlob);
    if (FAILED(hr)) return hr;
    hr = CompileShader("PSBlur", "ps_5_0", &blurBlob);
    if (FAILED(hr)) return hr;
    hr = CompileShader("PSLens", "ps_5_0", &lensBlob);
    if (FAILED(hr)) return hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(), nullptr, &blurPs_);
    if (FAILED(hr)) return hr;
    hr = device->CreatePixelShader(lensBlob->GetBufferPointer(), lensBlob->GetBufferSize(), nullptr, &lensPs_);
    if (FAILED(hr)) return hr;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = device->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ShaderParams);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return device->CreateBuffer(&bd, nullptr, &cb_);
}

HRESULT GlassRenderer::EnsureTargets(ID3D11Device* device, UINT regionW, UINT regionH,
                                     UINT panelW, UINT panelH) {
    (void)panelW;
    (void)panelH;
    if (regionTex_ && regionW == regionW_ && regionH == regionH_) return S_OK;

    regionW_ = regionW;
    regionH_ = regionH;
    blurW_ = (regionW + 1) / 2;
    blurH_ = (regionH + 1) / 2;
    stagingTex_.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = regionW;
    td.Height = regionH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, regionTex_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = device->CreateShaderResourceView(regionTex_.Get(), nullptr, regionSrv_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    td.Width = blurW_;
    td.Height = blurH_;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    hr = device->CreateTexture2D(&td, nullptr, blurTexA_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = device->CreateRenderTargetView(blurTexA_.Get(), nullptr, blurRtvA_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = device->CreateShaderResourceView(blurTexA_.Get(), nullptr, blurSrvA_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = device->CreateTexture2D(&td, nullptr, blurTexB_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = device->CreateRenderTargetView(blurTexB_.Get(), nullptr, blurRtvB_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    return device->CreateShaderResourceView(blurTexB_.Get(), nullptr, blurSrvB_.ReleaseAndGetAddressOf());
}

HRESULT GlassRenderer::RenderFromDesktop(ID3D11DeviceContext* ctx, ID3D11Texture2D* desktopTex,
                                         const RECT& desktopRect, GlassPanel& panel, float dpr) {
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);

    const RECT region = RegionForPanel(panel.bounds(), panel.params(), dpr);
    const UINT regionW = region.right - region.left;
    const UINT regionH = region.bottom - region.top;
    const RECT& pb = panel.bounds();
    HRESULT hr = EnsureTargets(device.Get(), regionW, regionH, pb.right - pb.left, pb.bottom - pb.top);
    if (FAILED(hr)) return hr;

    // 区域拷贝：桌面纹理坐标 = 虚拟桌面坐标 - 输出原点；越界部分裁剪（目标偏移保持对齐）
    RECT clamped{
        std::max(region.left, desktopRect.left), std::max(region.top, desktopRect.top),
        std::min(region.right, desktopRect.right), std::min(region.bottom, desktopRect.bottom)
    };
    if (clamped.right <= clamped.left || clamped.bottom <= clamped.top) return S_FALSE;

    D3D11_BOX box{};
    box.left = clamped.left - desktopRect.left;
    box.top = clamped.top - desktopRect.top;
    box.right = clamped.right - desktopRect.left;
    box.bottom = clamped.bottom - desktopRect.top;
    box.back = 1;
    ctx->CopySubresourceRegion(regionTex_.Get(), 0,
                               clamped.left - region.left, clamped.top - region.top, 0,
                               desktopTex, 0, &box);
    hasFrame_ = true;
    RunPasses(ctx, panel, dpr);
    return S_OK;
}

HRESULT GlassRenderer::Redraw(ID3D11DeviceContext* ctx, GlassPanel& panel, float dpr) {
    if (!hasFrame_) return S_FALSE;
    RunPasses(ctx, panel, dpr);
    return S_OK;
}

HRESULT GlassRenderer::SampleBands(ID3D11DeviceContext* ctx, const std::vector<LumaBand>& bands,
                                   float dpr, std::vector<LumaBandStats>* out) {
    if (!hasFrame_ || !blurTexB_) return S_FALSE;
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);

    if (!stagingTex_) {
        D3D11_TEXTURE2D_DESC td{};
        blurTexB_->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, stagingTex_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return hr;
    }
    ctx->CopyResource(stagingTex_.Get(), blurTexB_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    const auto* pixels = static_cast<const uint8_t*>(mapped.pData);

    const float margin = kBlurMarginCss * dpr;
    uint32_t histogram[256];
    for (const LumaBand& band : bands) {
        // 面板本地物理像素 → 模糊纹理坐标（区域含边距，半分辨率）
        const LONG x0 = std::clamp<LONG>(static_cast<LONG>((band.rect.left + margin) / 2), 0, blurW_ - 1);
        const LONG x1 = std::clamp<LONG>(static_cast<LONG>((band.rect.right + margin) / 2), 1, blurW_);
        const LONG y0 = std::clamp<LONG>(static_cast<LONG>((band.rect.top + margin) / 2), 0, blurH_ - 1);
        const LONG y1 = std::clamp<LONG>(static_cast<LONG>((band.rect.bottom + margin) / 2), 1, blurH_);
        if (x1 <= x0 || y1 <= y0) continue;

        memset(histogram, 0, sizeof(histogram));
        double sumB = 0;
        double sumG = 0;
        double sumR = 0;
        for (LONG y = y0; y < y1; y++) {
            const uint8_t* row = pixels + y * mapped.RowPitch;
            for (LONG x = x0; x < x1; x++) {
                const uint8_t* px = row + x * 4;  // BGRA
                sumB += px[0];
                sumG += px[1];
                sumR += px[2];
                const int luma = static_cast<int>((px[2] * 54 + px[1] * 183 + px[0] * 19) >> 8);
                histogram[luma > 255 ? 255 : luma]++;
            }
        }
        const uint32_t count = static_cast<uint32_t>((x1 - x0) * (y1 - y0));
        // 直方图求 luma p15 / p85
        const auto percentile = [&](float q) -> float {
            const uint32_t target = static_cast<uint32_t>(count * q);
            uint32_t acc = 0;
            for (int v = 0; v < 256; v++) {
                acc += histogram[v];
                if (acc >= target) return static_cast<float>(v);
            }
            return 255.0f;
        };
        LumaBandStats stats;
        stats.id = band.id;
        stats.r = static_cast<float>(sumR / count);
        stats.g = static_cast<float>(sumG / count);
        stats.b = static_cast<float>(sumB / count);
        stats.darkTail = percentile(0.15f);
        stats.lightTail = percentile(0.85f);
        out->push_back(stats);
    }
    ctx->Unmap(stagingTex_.Get(), 0);
    return S_OK;
}

void GlassRenderer::RunPasses(ID3D11DeviceContext* ctx, GlassPanel& panel, float dpr) {
    const GlassParams& gp = panel.params();
    const RECT& pb = panel.bounds();
    const float panelW = static_cast<float>(pb.right - pb.left);
    const float panelH = static_cast<float>(pb.bottom - pb.top);
    const float margin = kBlurMarginCss * dpr;

    // 与 WebGL 版一致的透镜几何（物理像素）。
    // maxBend = 边界处向外看的最远距离（外向折射无单调性约束，可以做强）；
    // 上限受模糊纹理外扩边距（kBlurMarginCss）限制，防止采样越界钳制
    const float halfMin = std::min(panelW, panelH) / 2.0f;
    const float bezel = std::min(24.0f * dpr, halfMin * 0.55f);
    const float maxBend = std::min({ 16.0f * dpr, bezel * 0.6f, margin * 0.8f });

    ShaderParams params{};
    params.glassSize[0] = panelW;
    params.glassSize[1] = panelH;
    params.radius = gp.cornerRadius;
    params.bezel = bezel;
    params.maxBend = maxBend;
    params.dispFactor = gp.displacementScale / 70.0f;
    params.aberration = gp.aberrationIntensity;
    params.saturation = gp.saturation;
    params.margin = margin;
    params.alpha = panel.opacity();

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());

    const auto setViewport = [&](float w, float h) {
        D3D11_VIEWPORT vp{ 0, 0, w, h, 0, 1 };
        ctx->RSSetViewports(1, &vp);
    };
    ID3D11ShaderResourceView* nullSrv = nullptr;

    // 趟1：区域全分辨率 → 半分辨率 + 水平高斯
    params.srcOrigin[0] = 0;
    params.srcOrigin[1] = 0;
    params.srcSpan[0] = 1;
    params.srcSpan[1] = 1;
    params.outSize[0] = static_cast<float>(blurW_);
    params.outSize[1] = static_cast<float>(blurH_);
    params.dir[0] = gp.blurSigma / static_cast<float>(regionW_);
    params.dir[1] = 0;
    ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
    ctx->OMSetRenderTargets(1, blurRtvA_.GetAddressOf(), nullptr);
    setViewport(static_cast<float>(blurW_), static_cast<float>(blurH_));
    ctx->PSSetShader(blurPs_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, regionSrv_.GetAddressOf());
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    // 趟2：垂直高斯
    params.dir[0] = 0;
    params.dir[1] = gp.blurSigma / static_cast<float>(regionH_);
    ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
    ctx->OMSetRenderTargets(1, blurRtvB_.GetAddressOf(), nullptr);
    ctx->PSSetShaderResources(0, 1, blurSrvA_.GetAddressOf());
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    // 趟3：透镜着色 → 面板背缓冲
    ID3D11RenderTargetView* backBuffer = panel.AcquireBackBuffer();
    if (!backBuffer) return;
    params.outSize[0] = panelW;
    params.outSize[1] = panelH;
    ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
    ctx->OMSetRenderTargets(1, &backBuffer, nullptr);
    setViewport(panelW, panelH);
    ctx->PSSetShader(lensPs_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, blurSrvB_.GetAddressOf());
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);

    panel.Present();
}
