#include "capture.h"

namespace {

// 找到与矩形中心相交的 DXGI 输出（显示器）
HRESULT FindOutputForRect(ID3D11Device* device, const RECT& rect, ComPtr<IDXGIOutput>* result) {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) return hr;
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return hr;

    const POINT center{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
    ComPtr<IDXGIOutput> firstOutput;
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
        if (!firstOutput) firstOutput = output;
        const RECT& r = desc.DesktopCoordinates;
        if (center.x >= r.left && center.x < r.right && center.y >= r.top && center.y < r.bottom) {
            *result = output;
            return S_OK;
        }
    }
    if (firstOutput) {
        *result = firstOutput;
        return S_OK;
    }
    return DXGI_ERROR_NOT_FOUND;
}

}  // namespace

HRESULT DesktopCapturer::Initialize(ID3D11Device* device, const RECT& monitorRect) {
    Shutdown();
    firstFrame_ = true;

    ComPtr<IDXGIOutput> output;
    HRESULT hr = FindOutputForRect(device, monitorRect, &output);
    if (FAILED(hr)) return hr;

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    desktopRect_ = desc.DesktopCoordinates;

    // 优先 DuplicateOutput1：显式声明 BGRA，避免 HDR 显示器返回 FP16 纹理
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output.As(&output5))) {
        const DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
        hr = output5->DuplicateOutput1(device, 0, 1, formats, &dupl_);
        if (SUCCEEDED(hr)) return S_OK;
    }
    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) return hr;
    return output1->DuplicateOutput(device, &dupl_);
}

void DesktopCapturer::Shutdown() {
    ReleaseFrame();
    dupl_.Reset();
}

HRESULT DesktopCapturer::AcquireFrame(UINT timeoutMs, CaptureFrameInfo* info, ID3D11Texture2D** texture) {
    if (!dupl_) return DXGI_ERROR_ACCESS_LOST;
    ReleaseFrame();

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return S_FALSE;
    if (FAILED(hr)) return hr;
    frameHeld_ = true;

    // 会话首帧是累积桌面快照，LastPresentTime 可能为 0，必须视为有效更新，
    // 否则静止桌面上的新面板永远等不到首绘内容
    info->desktopUpdated = frameInfo.LastPresentTime.QuadPart != 0 || firstFrame_;
    firstFrame_ = false;
    info->accumulatedFrames = frameInfo.AccumulatedFrames;
    info->dirtyBounds = RECT{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };

    if (info->desktopUpdated) {
        // 合并移动区与脏区为包围盒；元数据超缓冲/失败时保守视为全屏脏
        bool haveMeta = false;
        if (frameInfo.TotalMetadataBufferSize > 0) {
            metadata_.resize(frameInfo.TotalMetadataBufferSize);
            UINT moveBytes = 0;
            UINT dirtyBytes = 0;
            HRESULT mhr = dupl_->GetFrameMoveRects(
                static_cast<UINT>(metadata_.size()),
                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data()), &moveBytes);
            if (SUCCEEDED(mhr)) {
                const auto* moves = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data());
                for (UINT i = 0; i < moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT); i++) {
                    UnionRect(&info->dirtyBounds, &info->dirtyBounds, &moves[i].DestinationRect);
                }
                UINT remaining = static_cast<UINT>(metadata_.size()) - moveBytes;
                HRESULT dhr = dupl_->GetFrameDirtyRects(
                    remaining, reinterpret_cast<RECT*>(metadata_.data() + moveBytes), &dirtyBytes);
                if (SUCCEEDED(dhr)) {
                    const auto* dirty = reinterpret_cast<RECT*>(metadata_.data() + moveBytes);
                    for (UINT i = 0; i < dirtyBytes / sizeof(RECT); i++) {
                        UnionRect(&info->dirtyBounds, &info->dirtyBounds, &dirty[i]);
                    }
                    haveMeta = true;
                }
            }
        }
        if (!haveMeta || info->dirtyBounds.left > info->dirtyBounds.right) {
            info->dirtyBounds = RECT{ 0, 0, desktopRect_.right - desktopRect_.left,
                                      desktopRect_.bottom - desktopRect_.top };
        }
    }

    return resource->QueryInterface(IID_PPV_ARGS(texture));
}

void DesktopCapturer::ReleaseFrame() {
    if (frameHeld_ && dupl_) {
        dupl_->ReleaseFrame();
        frameHeld_ = false;
    }
}
