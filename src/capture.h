// DXGI Desktop Duplication 采集器：零拷贝获取桌面 GPU 纹理。
// 帧只在画面变化时到达（静止桌面零开销），脏区元数据用于跳过无关更新。
#pragma once
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

struct CaptureFrameInfo {
    bool desktopUpdated = false;  // 桌面像素有更新（区别于仅指针移动）
    RECT dirtyBounds{};           // 脏区+移动区包围盒（输出本地坐标），desktopUpdated 时有效
    uint32_t accumulatedFrames = 0;
};

class DesktopCapturer {
public:
    // 在给定设备上为包含 rect 中心的显示器建立复制会话
    HRESULT Initialize(ID3D11Device* device, const RECT& monitorRect);
    void Shutdown();

    // 等待下一帧；S_OK=有帧（用毕必须 ReleaseFrame），S_FALSE=超时无帧，
    // DXGI_ERROR_ACCESS_LOST 等错误须重建会话
    HRESULT AcquireFrame(UINT timeoutMs, CaptureFrameInfo* info, ID3D11Texture2D** texture);
    void ReleaseFrame();

    bool initialized() const { return dupl_ != nullptr; }
    // 该输出在虚拟桌面中的矩形（物理像素）
    const RECT& desktopRect() const { return desktopRect_; }

private:
    ComPtr<IDXGIOutputDuplication> dupl_;
    RECT desktopRect_{};
    std::vector<uint8_t> metadata_;
    bool frameHeld_ = false;
    bool firstFrame_ = true;
};
