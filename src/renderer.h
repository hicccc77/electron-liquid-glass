// 玻璃渲染器
// 趟1/2：面板区域(+边距)半分辨率可分离高斯模糊
// 趟3：解析式圆角矩形 SDF 透镜位移 + RGB 色散 + 饱和度 + 圆角 AA 覆盖 alpha
// 输入为 DDA 桌面纹理的区域拷贝（区域拷贝持久保留，供淡入淡出无新帧时重绘）
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <utility>
#include <vector>

#include "panel.h"

using Microsoft::WRL::ComPtr;

struct LumaBand;
struct LumaBandStats;

class GlassRenderer {
public:
    HRESULT Initialize(ID3D11Device* device);

    // 从桌面帧纹理拷出面板区域后执行三趟绘制并 Present
    HRESULT RenderFromDesktop(ID3D11DeviceContext* ctx, ID3D11Texture2D* desktopTex,
                              const RECT& desktopRect, GlassPanel& panel, float dpr);
    // 用最近一次的区域纹理重绘（无新帧时推进淡入淡出）
    HRESULT Redraw(ID3D11DeviceContext* ctx, GlassPanel& panel, float dpr);

    // 从模糊纹理回读各亮度带统计（均值色 + luma p15/p85）；带矩形为面板本地物理像素
    HRESULT SampleBands(ID3D11DeviceContext* ctx, const std::vector<LumaBand>& bands,
                        float dpr, std::vector<LumaBandStats>* out);

    bool hasFrame() const { return hasFrame_; }
    // 面板采样区域（含模糊边距、含采样源偏移）在虚拟桌面中的矩形
    static RECT RegionForPanel(const RECT& panelBounds, const GlassParams& params, float dpr);

private:
    HRESULT EnsureTargets(ID3D11Device* device, UINT regionW, UINT regionH, UINT panelW, UINT panelH);
    void RunPasses(ID3D11DeviceContext* ctx, GlassPanel& panel, float dpr);

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> blurPs_;
    ComPtr<ID3D11PixelShader> lensPs_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> cb_;

    // 区域纹理（桌面裁剪快照，全分辨率）
    ComPtr<ID3D11Texture2D> regionTex_;
    ComPtr<ID3D11ShaderResourceView> regionSrv_;
    // 半分辨率模糊乒乓目标
    ComPtr<ID3D11Texture2D> blurTexA_;
    ComPtr<ID3D11RenderTargetView> blurRtvA_;
    ComPtr<ID3D11ShaderResourceView> blurSrvA_;
    ComPtr<ID3D11Texture2D> blurTexB_;
    ComPtr<ID3D11RenderTargetView> blurRtvB_;
    ComPtr<ID3D11ShaderResourceView> blurSrvB_;
    // 亮度带回读用 staging（懒创建，尺寸同模糊目标）
    ComPtr<ID3D11Texture2D> stagingTex_;

    UINT regionW_ = 0;
    UINT regionH_ = 0;
    UINT blurW_ = 0;
    UINT blurH_ = 0;
    bool hasFrame_ = false;
};
