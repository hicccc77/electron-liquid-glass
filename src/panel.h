// Glass panel: a popup window with no redirection bitmap + a DirectComposition
// premultiplied-alpha swapchain. The window is fully click-through, never
// steals focus, stays out of the taskbar, and can optionally be excluded from
// screen capture (prevents self-capture feedback loops).
// All methods must be called on the worker thread (the window belongs to the
// message queue of its creating thread).
#pragma once
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct GlassParams {
    float cornerRadius = 20.0f;       // physical pixels
    float blurSigma = 5.0f;           // physical pixels
    float displacementScale = 70.0f;
    float aberrationIntensity = 2.0f;
    float saturation = 1.4f;
    // Offset of the sampled area relative to the panel (physical pixels):
    // 0 for regular glass; non-zero makes the panel show content from
    // elsewhere (e.g. feedback-free observation windows in latency tests)
    float sourceOffsetX = 0.0f;
    float sourceOffsetY = 0.0f;
};

class GlassPanel {
public:
    ~GlassPanel();

    // Create the window and composition chain (bounds in screen physical pixels)
    HRESULT Create(ID3D11Device* device, const RECT& bounds, const GlassParams& params,
                   bool excludeFromCapture);

    void SetBounds(const RECT& bounds);
    void SetParams(const GlassParams& params) { params_ = params; }
    void Show();
    void Hide();
    // Place the panel right below the anchor window in z-order; topmost when
    // the anchor is invalid
    void AnchorBelow(HWND anchor);
    void SetExcludeFromCapture(bool exclude);

    // Fade: target opacity + duration (the worker thread advances in ~16ms steps)
    void BeginFade(float target, UINT fadeMs) {
        fadeTarget_ = target;
        fadeStep_ = fadeMs <= 16 ? 1.0f : 16.0f / static_cast<float>(fadeMs);
    }
    bool FadeStep();  // returns true while still transitioning (keep redrawing)

    HWND hwnd() const { return hwnd_; }
    const RECT& bounds() const { return bounds_; }
    const GlassParams& params() const { return params_; }
    float opacity() const { return opacity_; }
    bool visible() const { return visible_; }

    ID3D11RenderTargetView* AcquireBackBuffer();  // rebuilds swapchain buffers on resize
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
