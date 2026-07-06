#include "capture.h"

#include <cstdlib>

namespace {

// Find the DXGI output (monitor) containing the rect's center
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

    // Prefer DuplicateOutput1 with an explicit BGRA request so HDR displays
    // don't hand back FP16 textures
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

namespace {

// Near-equality (±4px tolerance): DWM reports our own panel's dirty region as
// exactly the panel window rect; the tolerance absorbs edge rounding. The odds
// of third-party content coinciding with the panel rect on all four edges are
// negligible.
bool NearlyEqualRect(const RECT& a, const RECT& b) {
    constexpr LONG kTolerance = 4;
    return std::abs(a.left - b.left) <= kTolerance && std::abs(a.top - b.top) <= kTolerance &&
           std::abs(a.right - b.right) <= kTolerance && std::abs(a.bottom - b.bottom) <= kTolerance;
}

}  // namespace

HRESULT DesktopCapturer::AcquireFrame(UINT timeoutMs, const std::vector<RECT>& selfRects,
                                      CaptureFrameInfo* info, ID3D11Texture2D** texture) {
    if (!dupl_) return DXGI_ERROR_ACCESS_LOST;
    ReleaseFrame();

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return S_FALSE;
    if (FAILED(hr)) return hr;
    frameHeld_ = true;

    // The session's first frame is the accumulated desktop snapshot and may
    // carry LastPresentTime == 0; it must count as a valid update, otherwise a
    // new panel on a static desktop would never receive first-paint content
    info->desktopUpdated = frameInfo.LastPresentTime.QuadPart != 0 || firstFrame_;
    info->accumulatedFrames = frameInfo.AccumulatedFrames;
    info->dirtyBounds = RECT{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };

    // Convert our own panel rects to output-local coords for dirty self-filtering
    std::vector<RECT> selfLocal;
    selfLocal.reserve(selfRects.size());
    for (const RECT& r : selfRects) {
        selfLocal.push_back(RECT{ r.left - desktopRect_.left, r.top - desktopRect_.top,
                                  r.right - desktopRect_.left, r.bottom - desktopRect_.top });
    }
    const auto isSelfRect = [&](const RECT& r) {
        for (const RECT& s : selfLocal) {
            if (NearlyEqualRect(r, s)) return true;
        }
        return false;
    };

    if (info->desktopUpdated && !firstFrame_) {
        // Union move and dirty rects into one bounding box; on metadata
        // overflow/failure fall back conservatively to full-screen dirty
        bool haveMeta = false;
        bool anyRealDamage = false;
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
                    anyRealDamage = true;
                }
                UINT remaining = static_cast<UINT>(metadata_.size()) - moveBytes;
                HRESULT dhr = dupl_->GetFrameDirtyRects(
                    remaining, reinterpret_cast<RECT*>(metadata_.data() + moveBytes), &dirtyBytes);
                if (SUCCEEDED(dhr)) {
                    const auto* dirty = reinterpret_cast<RECT*>(metadata_.data() + moveBytes);
                    for (UINT i = 0; i < dirtyBytes / sizeof(RECT); i++) {
                        if (isSelfRect(dirty[i])) continue;
                        UnionRect(&info->dirtyBounds, &info->dirtyBounds, &dirty[i]);
                        anyRealDamage = true;
                    }
                    haveMeta = true;
                }
            }
        }
        if (haveMeta && !anyRealDamage) {
            // Every dirty rect this frame came from our own panels' Presents:
            // the desktop content did not actually change
            info->desktopUpdated = false;
        } else if (!haveMeta || info->dirtyBounds.left > info->dirtyBounds.right) {
            info->dirtyBounds = RECT{ 0, 0, desktopRect_.right - desktopRect_.left,
                                      desktopRect_.bottom - desktopRect_.top };
        }
    } else if (info->desktopUpdated) {
        // First frame: full-screen dirty
        info->dirtyBounds = RECT{ 0, 0, desktopRect_.right - desktopRect_.left,
                                  desktopRect_.bottom - desktopRect_.top };
    }
    firstFrame_ = false;

    return resource->QueryInterface(IID_PPV_ARGS(texture));
}

void DesktopCapturer::ReleaseFrame() {
    if (frameHeld_ && dupl_) {
        dupl_->ReleaseFrame();
        frameHeld_ = false;
    }
}
