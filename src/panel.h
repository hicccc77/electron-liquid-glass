// 玻璃面板：无重定向位图的弹出窗口 + DirectComposition 预乘 alpha 交换链。
// 窗口完全点击穿透、不抢焦点、不进任务栏，可选从屏幕捕获中排除（防自采集回路）。
// 所有方法必须在工作线程调用（窗口从属创建线程的消息队列）。
#pragma once
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct GlassParams {
    float cornerRadius = 20.0f;       // 物理像素
    float blurSigma = 5.0f;           // 物理像素
    float displacementScale = 70.0f;
    float aberrationIntensity = 2.0f;
    float saturation = 1.4f;
    // 采样区相对面板的偏移（物理像素）：常规玻璃为 0；
    // 非 0 时面板显示别处内容（延迟测试的无反馈观测窗等场景）
    float sourceOffsetX = 0.0f;
    float sourceOffsetY = 0.0f;
};

class GlassPanel {
public:
    ~GlassPanel();

    // 创建窗口与合成链（bounds 为屏幕物理像素）
    HRESULT Create(ID3D11Device* device, const RECT& bounds, const GlassParams& params,
                   bool excludeFromCapture);

    void SetBounds(const RECT& bounds);
    void SetParams(const GlassParams& params) { params_ = params; }
    void Show();
    void Hide();
    // 把面板置于 anchor 窗口正下方（z 序），anchor 无效时置顶
    void AnchorBelow(HWND anchor);
    void SetExcludeFromCapture(bool exclude);

    // 淡入淡出：目标不透明度 + 过渡时长（工作线程按 ~16ms 步进推进）
    void BeginFade(float target, UINT fadeMs) {
        fadeTarget_ = target;
        fadeStep_ = fadeMs <= 16 ? 1.0f : 16.0f / static_cast<float>(fadeMs);
    }
    bool FadeStep();  // 返回 true 表示仍在过渡中（需要继续重绘）

    HWND hwnd() const { return hwnd_; }
    const RECT& bounds() const { return bounds_; }
    const GlassParams& params() const { return params_; }
    float opacity() const { return opacity_; }
    bool visible() const { return visible_; }

    ID3D11RenderTargetView* AcquireBackBuffer();  // 尺寸变化时自动重建交换链缓冲
    void Present();

private:
    HRESULT EnsureSwapchain(ID3D11Device* device, UINT width, UINT height);

    HWND hwnd_ = nullptr;
    RECT bounds_{};
    GlassParams params_{};
    bool visible_ = false;
    float opacity_ = 0.0f;
    float fadeTarget_ = 0.0f;
    float fadeStep_ = 0.12f;

    ComPtr<ID3D11Device> device_;
    ComPtr<IDCompositionDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> dcompVisual_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    UINT swapW_ = 0;
    UINT swapH_ = 0;
};
