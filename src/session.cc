#include "session.h"

#include <algorithm>

namespace {

// 无可见面板持续该时长后释放 DDA 会话（不白占系统采集资源）
constexpr ULONGLONG kCaptureIdleReleaseMs = 3000;
// 亮度采样由重绘驱动（桌面内容变化才推送），此值仅节流高频变化，上限约 60Hz
// （GetTickCount64 分辨率 ~15.6ms，阈值取 15 恰好逐帧不跳帧）
constexpr ULONGLONG kLumaMinGapMs = 15;
constexpr ULONGLONG kAnchorReassertMs = 500;

bool RectIntersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

}  // namespace

GlassSession& GlassSession::Instance() {
    static GlassSession session;
    return session;
}

void GlassSession::EnsureThread() {
    if (running_.load()) return;
    running_ = true;
    thread_ = std::thread([this] { ThreadMain(); });
}

void GlassSession::Post(std::function<void()> cmd) {
    EnsureThread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(std::move(cmd));
    }
    cv_.notify_one();
}

int GlassSession::CreatePanel(const PanelConfig& config) {
    const int id = nextId_.fetch_add(1);
    Post([this, id, config] {
        PanelEntry entry;
        entry.panel = std::make_unique<GlassPanel>();
        entry.renderer = std::make_unique<GlassRenderer>();
        entry.config = config;
        if (FAILED(entry.panel->Create(device_.Get(), config.bounds, config.params,
                                       config.excludeFromCapture))) {
            return;
        }
        if (FAILED(entry.renderer->Initialize(device_.Get()))) return;
        if (config.anchor) entry.panel->AnchorBelow(config.anchor);
        panels_[id] = std::move(entry);
    });
    return id;
}

void GlassSession::DestroyPanel(int id) {
    Post([this, id] { panels_.erase(id); });
}

void GlassSession::ShowPanel(int id, UINT fadeMs) {
    Post([this, id, fadeMs] {
        auto it = panels_.find(id);
        if (it == panels_.end()) return;
        it->second.panel->Show();
        it->second.panel->BeginFade(1.0f, fadeMs);
        it->second.fading = true;
        it->second.needsInitialPaint = true;
    });
}

void GlassSession::HidePanel(int id, UINT fadeMs) {
    Post([this, id, fadeMs] {
        auto it = panels_.find(id);
        if (it == panels_.end()) return;
        if (fadeMs == 0) {
            it->second.panel->Hide();
            it->second.fading = false;
        } else {
            it->second.panel->BeginFade(0.0f, fadeMs);
            it->second.fading = true;
        }
    });
}

void GlassSession::SetPanelBounds(int id, const RECT& bounds) {
    Post([this, id, bounds] {
        auto it = panels_.find(id);
        if (it == panels_.end()) return;
        it->second.config.bounds = bounds;
        it->second.panel->SetBounds(bounds);
        it->second.needsInitialPaint = true;
    });
}

void GlassSession::SetPanelParams(int id, const GlassParams& params) {
    Post([this, id, params] {
        auto it = panels_.find(id);
        if (it == panels_.end()) return;
        it->second.config.params = params;
        it->second.panel->SetParams(params);
        it->second.needsInitialPaint = true;
    });
}

void GlassSession::AnchorPanel(int id, HWND anchor) {
    Post([this, id, anchor] {
        auto it = panels_.find(id);
        if (it == panels_.end()) return;
        it->second.config.anchor = anchor;
        it->second.panel->AnchorBelow(anchor);
    });
}

void GlassSession::SetLumaBands(int id, std::vector<LumaBand> bands) {
    Post([this, id, bands = std::move(bands)]() mutable {
        auto it = panels_.find(id);
        if (it == panels_.end()) return;
        it->second.lumaBands = std::move(bands);
        it->second.lastLumaTick = 0;
    });
}

void GlassSession::SetLumaCallback(LumaCallback cb) {
    std::lock_guard<std::mutex> lock(lumaMutex_);
    lumaCallback_ = std::move(cb);
}

void GlassSession::Shutdown() {
    if (!running_.load()) return;
    running_ = false;
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void GlassSession::ThreadMain() {
    if (FAILED(CreateD3DDevice(&device_, &context_))) {
        running_ = false;
        return;
    }

    while (running_.load()) {
        PumpCommandsAndMessages();
        if (!running_.load()) break;
        RenderTick();
    }

    panels_.clear();
    capturer_.Shutdown();
    desktopCache_.Reset();
    context_.Reset();
    device_.Reset();
}

void GlassSession::PumpCommandsAndMessages() {
    std::deque<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(commands_);
    }
    for (auto& cmd : pending) cmd();

    // 泵窗口消息（面板窗口从属本线程）
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// 把最新桌面帧留存到持久缓存：新面板在静止桌面上也能立即取到像素完成首绘
bool GlassSession::UpdateDesktopCache(ID3D11Texture2D* frameTex) {
    D3D11_TEXTURE2D_DESC desc{};
    frameTex->GetDesc(&desc);
    if (!desktopCache_ || cacheW_ != desc.Width || cacheH_ != desc.Height) {
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, desktopCache_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        cacheW_ = desc.Width;
        cacheH_ = desc.Height;
    }
    context_->CopyResource(desktopCache_.Get(), frameTex);
    cacheValid_ = true;
    return true;
}

void GlassSession::RenderTick() {
    const ULONGLONG now = GetTickCount64();

    bool anyVisible = false;
    bool anyFading = false;
    const PanelEntry* firstVisible = nullptr;
    for (auto& [id, entry] : panels_) {
        if (entry.panel->visible()) {
            anyVisible = true;
            if (!firstVisible) firstVisible = &entry;
        }
        if (entry.fading) anyFading = true;
    }

    if (!anyVisible && !anyFading) {
        if (capturer_.initialized() && now - lastActiveTick_ > kCaptureIdleReleaseMs) {
            capturer_.Shutdown();
            cacheValid_ = false;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(80),
                     [this] { return !commands_.empty() || !running_.load(); });
        return;
    }
    lastActiveTick_ = now;

    // 采集会话跟随首个可见面板所在显示器
    if (firstVisible) {
        const RECT& b = firstVisible->config.bounds;
        if (!capturer_.initialized() || !RectIntersects(capturer_.desktopRect(), b)) {
            capturer_.Shutdown();
            cacheValid_ = false;
            if (FAILED(capturer_.Initialize(device_.Get(), b))) {
                // 初始化失败（如安全桌面期间）：稍后重试，面板保持上一帧内容
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(300));
                return;
            }
        }
    }

    // 等待新帧（短超时保证命令与淡入淡出及时推进）
    CaptureFrameInfo frameInfo;
    bool newFrame = false;
    RECT dirtyVirtual{};
    {
        ComPtr<ID3D11Texture2D> desktopTex;
        const HRESULT hr = capturer_.AcquireFrame(8, &frameInfo, &desktopTex);
        if (hr == S_OK) {
            if (frameInfo.desktopUpdated && UpdateDesktopCache(desktopTex.Get())) {
                newFrame = true;
                const RECT& dr = capturer_.desktopRect();
                dirtyVirtual = RECT{ frameInfo.dirtyBounds.left + dr.left,
                                     frameInfo.dirtyBounds.top + dr.top,
                                     frameInfo.dirtyBounds.right + dr.left,
                                     frameInfo.dirtyBounds.bottom + dr.top };
            }
            capturer_.ReleaseFrame();
        } else if (hr != S_FALSE) {
            // ACCESS_LOST 等：重建会话（分辨率切换、全屏独占、安全桌面切换）
            capturer_.Shutdown();
            return;
        }
    }

    const bool fadeTickDue = now - lastFadeTick_ >= 16;
    for (auto& [id, entry] : panels_) {
        if (!entry.panel->visible()) continue;
        const RECT region = GlassRenderer::RegionForPanel(
            entry.config.bounds, entry.config.params, entry.config.dpr);
        const bool dirtyHit = newFrame && RectIntersects(dirtyVirtual, region);
        bool rendered = false;

        if (cacheValid_ && (dirtyHit || entry.needsInitialPaint)) {
            if (SUCCEEDED(entry.renderer->RenderFromDesktop(
                    context_.Get(), desktopCache_.Get(), capturer_.desktopRect(),
                    *entry.panel, entry.config.dpr))) {
                entry.needsInitialPaint = false;
                rendered = true;
            }
        }

        // 淡入淡出推进（无新帧时用上帧区域纹理重绘）
        if (entry.fading && fadeTickDue) {
            const bool continuing = entry.panel->FadeStep();
            if (!rendered) entry.renderer->Redraw(context_.Get(), *entry.panel, entry.config.dpr);
            if (!continuing) {
                entry.fading = false;
                if (entry.panel->opacity() <= 0.0f) entry.panel->Hide();
            }
        }

        // 亮度带采样（自适应反色）：由重绘驱动——本 tick 重绘过说明面板下方内容
        // 刚变化，立即回读推送（上限 ~60Hz），反色与画面同帧；桌面静止时零推送。
        // lastLumaTick=0 表示带配置刚更新，即使无重绘也用现存纹理补采一次
        if (!entry.lumaBands.empty() && entry.renderer->hasFrame() &&
            (rendered || entry.lastLumaTick == 0) &&
            now - entry.lastLumaTick >= kLumaMinGapMs) {
            entry.lastLumaTick = now;
            SampleLuma(id, entry);
        }

        // 周期性重申 z 序锚定（防其他置顶窗口插队）
        if (entry.config.anchor && now - entry.lastAnchorTick >= kAnchorReassertMs) {
            entry.lastAnchorTick = now;
            entry.panel->AnchorBelow(entry.config.anchor);
        }
    }
    if (fadeTickDue) lastFadeTick_ = now;
}

void GlassSession::SampleLuma(int panelId, PanelEntry& entry) {
    std::vector<LumaBandStats> result;
    if (FAILED(entry.renderer->SampleBands(context_.Get(), entry.lumaBands,
                                           entry.config.dpr, &result)) ||
        result.empty()) {
        return;
    }
    LumaCallback cb;
    {
        std::lock_guard<std::mutex> lock(lumaMutex_);
        cb = lumaCallback_;
    }
    if (cb) cb(panelId, std::move(result));
}
