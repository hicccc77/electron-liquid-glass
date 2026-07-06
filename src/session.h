// 玻璃会话：独占工作线程运行「采集 → 渲染 → 上屏」闭环。
// - 面板窗口在工作线程创建（窗口消息队列从属该线程）
// - 无可见面板时不采集（休眠等命令）；桌面静止时 AcquireNextFrame 超时空转，零渲染
// - 脏区与面板区域不相交时跳过该面板的重绘
// - JS 侧所有调用经命令队列异步投递，无像素数据跨线程
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "capture.h"
#include "d3d_utils.h"
#include "panel.h"
#include "renderer.h"

struct PanelConfig {
    RECT bounds{};            // 屏幕物理像素
    GlassParams params{};     // 物理像素单位
    float dpr = 1.0f;
    bool excludeFromCapture = true;
    HWND anchor = nullptr;    // 面板贴于该窗口下方（z 序）
};

// 亮度带：面板本地物理像素矩形
struct LumaBand {
    int id = 0;
    RECT rect{};
};

// 亮度带统计：均值色 + 明暗分位数（自适应反色算法的完整输入）
struct LumaBandStats {
    int id = 0;
    float r = 0;
    float g = 0;
    float b = 0;
    float darkTail = 0;   // luma p15（0~255）
    float lightTail = 0;  // luma p85
};

class GlassSession {
public:
    using LumaCallback = std::function<void(int panelId, std::vector<LumaBandStats>)>;

    static GlassSession& Instance();

    int CreatePanel(const PanelConfig& config);
    void DestroyPanel(int id);
    void ShowPanel(int id, UINT fadeMs);
    void HidePanel(int id, UINT fadeMs);
    void SetPanelBounds(int id, const RECT& bounds);
    void SetPanelParams(int id, const GlassParams& params);
    void AnchorPanel(int id, HWND anchor);
    void SetLumaBands(int id, std::vector<LumaBand> bands);
    void SetLumaCallback(LumaCallback cb);  // 工作线程上调用（调用方负责线程安全投递）

    void Shutdown();

private:
    GlassSession() = default;

    struct PanelEntry {
        std::unique_ptr<GlassPanel> panel;
        std::unique_ptr<GlassRenderer> renderer;
        PanelConfig config;
        std::vector<LumaBand> lumaBands;
        bool needsInitialPaint = true;
        bool fading = false;
        ULONGLONG lastLumaTick = 0;
        ULONGLONG lastAnchorTick = 0;
    };

    void EnsureThread();
    void Post(std::function<void()> cmd);
    void ThreadMain();
    void PumpCommandsAndMessages();
    void RenderTick();
    bool UpdateDesktopCache(ID3D11Texture2D* frameTex);
    void SampleLuma(int panelId, PanelEntry& entry);

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> commands_;

    // 以下成员仅工作线程访问
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    DesktopCapturer capturer_;
    ComPtr<ID3D11Texture2D> desktopCache_;  // 最新桌面帧留存（新面板静止桌面首绘）
    UINT cacheW_ = 0;
    UINT cacheH_ = 0;
    bool cacheValid_ = false;
    std::map<int, PanelEntry> panels_;
    ULONGLONG lastFadeTick_ = 0;
    ULONGLONG lastActiveTick_ = 0;

    std::mutex lumaMutex_;
    LumaCallback lumaCallback_;
    std::atomic<int> nextId_{ 1 };
};
