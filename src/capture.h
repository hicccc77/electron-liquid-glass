// DXGI Desktop Duplication capturer: zero-copy access to the desktop GPU
// texture. Frames arrive only when the screen changes (a static desktop costs
// nothing); dirty-region metadata lets callers skip irrelevant updates.
#pragma once
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

struct CaptureFrameInfo {
    bool desktopUpdated = false;  // desktop pixels changed (as opposed to pointer-only updates)
    RECT dirtyBounds{};           // bounding box of dirty + move regions (output-local coords), valid when desktopUpdated
    uint32_t accumulatedFrames = 0;
};

class DesktopCapturer {
public:
    // Create a duplication session on the given device for the monitor
    // containing the center of `monitorRect`
    HRESULT Initialize(ID3D11Device* device, const RECT& monitorRect);
    void Shutdown();

    // Wait for the next frame; S_OK = frame available (must ReleaseFrame when
    // done), S_FALSE = timeout, DXGI_ERROR_ACCESS_LOST etc. = rebuild session.
    // selfRects (virtual-desktop coords): the caller's own panel window rects.
    // After a WDA_EXCLUDEFROMCAPTURE-excluded panel Presents, DWM still
    // reports the panel rect as dirty even though the captured content did not
    // change. Dirty rects nearly equal to a self rect are dropped — otherwise
    // "repaint → dirty → repaint" becomes a self-sustained loop.
    HRESULT AcquireFrame(UINT timeoutMs, const std::vector<RECT>& selfRects,
                         CaptureFrameInfo* info, ID3D11Texture2D** texture);
    void ReleaseFrame();

    bool initialized() const { return dupl_ != nullptr; }
    // This output's rect within the virtual desktop (physical pixels)
    const RECT& desktopRect() const { return desktopRect_; }

private:
    ComPtr<IDXGIOutputDuplication> dupl_;
    RECT desktopRect_{};
    std::vector<uint8_t> metadata_;
    bool frameHeld_ = false;
    bool firstFrame_ = true;
};
