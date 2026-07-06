// electron-liquid-glass 原生入口（Windows）：N-API 绑定 + 会话生命周期
#include <napi.h>
#include <windows.h>

#include <utility>
#include <vector>

#include "capture.h"
#include "d3d_utils.h"
#include "panel.h"
#include "session.h"

namespace {

DWORD GetWindowsBuildNumber() {
    // GetVersionEx 受兼容性清单影响，RtlGetVersion 始终返回真实版本
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) return 0;
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) return 0;
    return info.dwBuildNumber;
}

// WDA_EXCLUDEFROMCAPTURE（防自采集反馈回路）要求 Win10 2004 (build 19041)+
Napi::Value IsSupported(const Napi::CallbackInfo& info) {
    return Napi::Boolean::New(info.Env(), GetWindowsBuildNumber() >= 19041);
}

Napi::Value OsBuild(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), static_cast<double>(GetWindowsBuildNumber()));
}

// —— 参数解析辅助 ——

LONG GetInt(Napi::Object obj, const char* key, LONG fallback = 0) {
    Napi::Value v = obj.Get(key);
    return v.IsNumber() ? v.As<Napi::Number>().Int32Value() : fallback;
}

float GetFloat(Napi::Object obj, const char* key, float fallback) {
    Napi::Value v = obj.Get(key);
    return v.IsNumber() ? v.As<Napi::Number>().FloatValue() : fallback;
}

bool GetBool(Napi::Object obj, const char* key, bool fallback) {
    Napi::Value v = obj.Get(key);
    return v.IsBoolean() ? v.As<Napi::Boolean>().Value() : fallback;
}

RECT BoundsFromObject(Napi::Object obj) {
    const LONG x = GetInt(obj, "x");
    const LONG y = GetInt(obj, "y");
    return RECT{ x, y, x + GetInt(obj, "width"), y + GetInt(obj, "height") };
}

GlassParams ParamsFromObject(Napi::Object obj, const GlassParams& base) {
    GlassParams p = base;
    p.cornerRadius = GetFloat(obj, "cornerRadius", p.cornerRadius);
    p.blurSigma = GetFloat(obj, "blurSigma", p.blurSigma);
    p.displacementScale = GetFloat(obj, "displacementScale", p.displacementScale);
    p.aberrationIntensity = GetFloat(obj, "aberrationIntensity", p.aberrationIntensity);
    p.saturation = GetFloat(obj, "saturation", p.saturation);
    p.sourceOffsetX = GetFloat(obj, "sourceOffsetX", p.sourceOffsetX);
    p.sourceOffsetY = GetFloat(obj, "sourceOffsetY", p.sourceOffsetY);
    return p;
}

HWND HwndFromValue(Napi::Value value) {
    if (!value.IsBuffer()) return nullptr;
    auto buffer = value.As<Napi::Buffer<uint8_t>>();
    if (buffer.Length() < sizeof(void*)) return nullptr;
    HWND hwnd = nullptr;
    memcpy(&hwnd, buffer.Data(), sizeof(void*));
    return hwnd;
}

std::vector<LumaBand> BandsFromValue(Napi::Value value) {
    std::vector<LumaBand> bands;
    if (!value.IsArray()) return bands;
    auto arr = value.As<Napi::Array>();
    for (uint32_t i = 0; i < arr.Length(); i++) {
        Napi::Value item = arr.Get(i);
        if (!item.IsObject()) continue;
        auto obj = item.As<Napi::Object>();
        LumaBand band;
        band.id = GetInt(obj, "id", static_cast<LONG>(i));
        band.rect = BoundsFromObject(obj);
        bands.push_back(band);
    }
    return bands;
}

// —— 面板 API ——

Napi::Value CreatePanel(const Napi::CallbackInfo& info) {
    auto opts = info[0].As<Napi::Object>();
    PanelConfig config;
    config.bounds = BoundsFromObject(opts);
    config.params = ParamsFromObject(opts, GlassParams{});
    config.dpr = GetFloat(opts, "dpr", 1.0f);
    config.excludeFromCapture = GetBool(opts, "excludeFromCapture", true);
    config.anchor = HwndFromValue(opts.Get("anchorHwnd"));
    const int id = GlassSession::Instance().CreatePanel(config);
    return Napi::Number::New(info.Env(), id);
}

Napi::Value DestroyPanel(const Napi::CallbackInfo& info) {
    GlassSession::Instance().DestroyPanel(info[0].As<Napi::Number>().Int32Value());
    return info.Env().Undefined();
}

Napi::Value ShowPanel(const Napi::CallbackInfo& info) {
    const int id = info[0].As<Napi::Number>().Int32Value();
    const UINT fadeMs = info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 120;
    GlassSession::Instance().ShowPanel(id, fadeMs);
    return info.Env().Undefined();
}

Napi::Value HidePanel(const Napi::CallbackInfo& info) {
    const int id = info[0].As<Napi::Number>().Int32Value();
    const UINT fadeMs = info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 100;
    GlassSession::Instance().HidePanel(id, fadeMs);
    return info.Env().Undefined();
}

Napi::Value SetPanelBounds(const Napi::CallbackInfo& info) {
    const int id = info[0].As<Napi::Number>().Int32Value();
    GlassSession::Instance().SetPanelBounds(id, BoundsFromObject(info[1].As<Napi::Object>()));
    return info.Env().Undefined();
}

Napi::Value SetPanelParams(const Napi::CallbackInfo& info) {
    const int id = info[0].As<Napi::Number>().Int32Value();
    // JS 侧维护完整参数对象（缺省字段保留默认值语义由 JS 保证）
    GlassSession::Instance().SetPanelParams(
        id, ParamsFromObject(info[1].As<Napi::Object>(), GlassParams{}));
    return info.Env().Undefined();
}

Napi::Value AnchorPanel(const Napi::CallbackInfo& info) {
    const int id = info[0].As<Napi::Number>().Int32Value();
    GlassSession::Instance().AnchorPanel(id, HwndFromValue(info[1]));
    return info.Env().Undefined();
}

Napi::Value SetLumaBands(const Napi::CallbackInfo& info) {
    const int id = info[0].As<Napi::Number>().Int32Value();
    GlassSession::Instance().SetLumaBands(id, BandsFromValue(info[1]));
    return info.Env().Undefined();
}

// —— 亮度回调（工作线程 → JS 主线程，TSFN 转发）——

using LumaPayload = std::pair<int, std::vector<LumaBandStats>>;
Napi::ThreadSafeFunction g_lumaTsfn;

Napi::Value SetLumaCallback(const Napi::CallbackInfo& info) {
    if (g_lumaTsfn) {
        g_lumaTsfn.Release();
        g_lumaTsfn = Napi::ThreadSafeFunction();
        GlassSession::Instance().SetLumaCallback(nullptr);
    }
    if (info.Length() > 0 && info[0].IsFunction()) {
        g_lumaTsfn = Napi::ThreadSafeFunction::New(
            info.Env(), info[0].As<Napi::Function>(), "liquid-glass-luma", 4, 1);
        g_lumaTsfn.Unref(info.Env());  // 不阻止进程退出
        GlassSession::Instance().SetLumaCallback([](int panelId, std::vector<LumaBandStats> bands) {
            if (!g_lumaTsfn) return;
            auto* payload = new LumaPayload(panelId, std::move(bands));
            // 采样最高 ~60Hz：JS 忙时队列（容量 4）可能拒收，必须回收 payload；
            // 丢弃的是中间态样本，最新样本随下一次重绘再来
            const napi_status status = g_lumaTsfn.NonBlockingCall(
                payload, [](Napi::Env env, Napi::Function cb, LumaPayload* data) {
                    Napi::Object bands = Napi::Object::New(env);
                    for (const LumaBandStats& s : data->second) {
                        Napi::Object stat = Napi::Object::New(env);
                        stat.Set("r", Napi::Number::New(env, s.r));
                        stat.Set("g", Napi::Number::New(env, s.g));
                        stat.Set("b", Napi::Number::New(env, s.b));
                        stat.Set("darkTail", Napi::Number::New(env, s.darkTail));
                        stat.Set("lightTail", Napi::Number::New(env, s.lightTail));
                        bands.Set(std::to_string(s.id), stat);
                    }
                    cb.Call({ Napi::Number::New(env, data->first), bands });
                    delete data;
                });
            if (status != napi_ok) delete payload;
        });
    }
    return info.Env().Undefined();
}

Napi::Value ShutdownSession(const Napi::CallbackInfo& info) {
    GlassSession::Instance().SetLumaCallback(nullptr);
    GlassSession::Instance().Shutdown();
    if (g_lumaTsfn) {
        g_lumaTsfn.Release();
        g_lumaTsfn = Napi::ThreadSafeFunction();
    }
    return info.Env().Undefined();
}

// —— 诊断探针（测试/排障用，见 tmp/native-step*.cjs）——

Napi::Value ProbeCapture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const UINT durationMs = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 2000;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Napi::Object result = Napi::Object::New(env);
    if (FAILED(CreateD3DDevice(&device, &context))) {
        result.Set("error", Napi::String::New(env, "CreateD3DDevice failed"));
        return result;
    }

    DesktopCapturer capturer;
    const RECT primary{ 0, 0, 1, 1 };
    HRESULT hr = capturer.Initialize(device.Get(), primary);
    if (FAILED(hr)) {
        char buf[64];
        sprintf_s(buf, "DuplicateOutput failed hr=0x%08lX", hr);
        result.Set("error", Napi::String::New(env, buf));
        return result;
    }

    UINT frames = 0;
    UINT desktopUpdates = 0;
    UINT timeouts = 0;
    double sumIntervalMs = 0;
    UINT intervals = 0;
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    LARGE_INTEGER lastFrameTs{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    for (;;) {
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart >= durationMs) break;
        CaptureFrameInfo frameInfo;
        ComPtr<ID3D11Texture2D> texture;
        hr = capturer.AcquireFrame(30, &frameInfo, &texture);
        if (hr == S_FALSE) {
            timeouts++;
            continue;
        }
        if (FAILED(hr)) break;
        frames++;
        if (frameInfo.desktopUpdated) {
            desktopUpdates++;
            QueryPerformanceCounter(&now);
            if (lastFrameTs.QuadPart != 0) {
                sumIntervalMs += (now.QuadPart - lastFrameTs.QuadPart) * 1000.0 / freq.QuadPart;
                intervals++;
            }
            lastFrameTs = now;
        }
        capturer.ReleaseFrame();
    }
    const RECT& dr = capturer.desktopRect();
    result.Set("frames", Napi::Number::New(env, frames));
    result.Set("desktopUpdates", Napi::Number::New(env, desktopUpdates));
    result.Set("timeouts", Napi::Number::New(env, timeouts));
    result.Set("avgUpdateIntervalMs",
               Napi::Number::New(env, intervals ? sumIntervalMs / intervals : -1));
    result.Set("desktopW", Napi::Number::New(env, dr.right - dr.left));
    result.Set("desktopH", Napi::Number::New(env, dr.bottom - dr.top));
    capturer.Shutdown();
    return result;
}

Napi::Value ProbePanel(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object opts = info[0].As<Napi::Object>();
    const float alpha = GetFloat(opts, "alpha", 0.5f);
    const bool exclude = GetBool(opts, "exclude", false);
    const UINT durationMs = opts.Get("durationMs").As<Napi::Number>().Uint32Value();

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Napi::Object result = Napi::Object::New(env);
    if (FAILED(CreateD3DDevice(&device, &context))) {
        result.Set("error", Napi::String::New(env, "CreateD3DDevice failed"));
        return result;
    }

    GlassPanel panel;
    HRESULT hr = panel.Create(device.Get(), BoundsFromObject(opts), GlassParams{}, exclude);
    if (FAILED(hr)) {
        char buf[64];
        sprintf_s(buf, "Panel.Create failed hr=0x%08lX", hr);
        result.Set("error", Napi::String::New(env, buf));
        return result;
    }

    ID3D11RenderTargetView* rtv = panel.AcquireBackBuffer();
    if (!rtv) {
        result.Set("error", Napi::String::New(env, "AcquireBackBuffer failed"));
        return result;
    }
    const float color[4] = { alpha, 0.0f, 0.0f, alpha };
    context->ClearRenderTargetView(rtv, color);
    panel.Present();
    panel.Show();

    const ULONGLONG end = GetTickCount64() + durationMs;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    result.Set("ok", Napi::Boolean::New(env, true));
    return result;
}

// 单次渲染探针：采集一帧 → 玻璃管线渲染一次 → 保持显示。
// 不进入持续采集循环，因此 exclude=false 也不会产生自采集反馈，
// 用于对着色器输出做视觉回归（生产路径始终 exclude=true）。
Napi::Value ProbeGlassShot(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object opts = info[0].As<Napi::Object>();
    const UINT durationMs = opts.Get("durationMs").As<Napi::Number>().Uint32Value();

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Napi::Object result = Napi::Object::New(env);
    if (FAILED(CreateD3DDevice(&device, &context))) {
        result.Set("error", Napi::String::New(env, "CreateD3DDevice failed"));
        return result;
    }

    const RECT bounds = BoundsFromObject(opts);
    DesktopCapturer capturer;
    HRESULT hr = capturer.Initialize(device.Get(), bounds);
    if (FAILED(hr)) {
        result.Set("error", Napi::String::New(env, "DuplicateOutput failed"));
        return result;
    }

    // 等一帧真实桌面内容
    CaptureFrameInfo frameInfo;
    ComPtr<ID3D11Texture2D> desktopTex;
    const ULONGLONG deadline = GetTickCount64() + 1500;
    for (;;) {
        hr = capturer.AcquireFrame(50, &frameInfo, desktopTex.ReleaseAndGetAddressOf());
        if (hr == S_OK && frameInfo.desktopUpdated) break;
        if (hr == S_OK) capturer.ReleaseFrame();
        if (FAILED(hr) || GetTickCount64() > deadline) {
            result.Set("error", Napi::String::New(env, "no desktop frame"));
            return result;
        }
    }

    GlassPanel panel;
    hr = panel.Create(device.Get(), bounds, ParamsFromObject(opts, GlassParams{}), false);
    if (FAILED(hr)) {
        result.Set("error", Napi::String::New(env, "Panel.Create failed"));
        capturer.ReleaseFrame();
        return result;
    }
    panel.BeginFade(1.0f, 16);
    panel.FadeStep();

    GlassRenderer renderer;
    if (FAILED(renderer.Initialize(device.Get()))) {
        result.Set("error", Napi::String::New(env, "Renderer init failed"));
        capturer.ReleaseFrame();
        return result;
    }
    hr = renderer.RenderFromDesktop(context.Get(), desktopTex.Get(), capturer.desktopRect(),
                                    panel, GetFloat(opts, "dpr", 1.0f));
    capturer.ReleaseFrame();
    capturer.Shutdown();
    if (FAILED(hr)) {
        result.Set("error", Napi::String::New(env, "Render failed"));
        return result;
    }
    panel.Show();

    const ULONGLONG end = GetTickCount64() + durationMs;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    result.Set("ok", Napi::Boolean::New(env, true));
    return result;
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("isSupported", Napi::Function::New(env, IsSupported));
    exports.Set("osBuild", Napi::Function::New(env, OsBuild));
    exports.Set("createPanel", Napi::Function::New(env, CreatePanel));
    exports.Set("destroyPanel", Napi::Function::New(env, DestroyPanel));
    exports.Set("showPanel", Napi::Function::New(env, ShowPanel));
    exports.Set("hidePanel", Napi::Function::New(env, HidePanel));
    exports.Set("setPanelBounds", Napi::Function::New(env, SetPanelBounds));
    exports.Set("setPanelParams", Napi::Function::New(env, SetPanelParams));
    exports.Set("anchorPanel", Napi::Function::New(env, AnchorPanel));
    exports.Set("setLumaBands", Napi::Function::New(env, SetLumaBands));
    exports.Set("setLumaCallback", Napi::Function::New(env, SetLumaCallback));
    exports.Set("shutdown", Napi::Function::New(env, ShutdownSession));
    exports.Set("_probeCapture", Napi::Function::New(env, ProbeCapture));
    exports.Set("_probePanel", Napi::Function::New(env, ProbePanel));
    exports.Set("_probeGlassShot", Napi::Function::New(env, ProbeGlassShot));

    // 进程退出前回收工作线程，避免持有窗口/设备时被强杀
    env.AddCleanupHook([] { GlassSession::Instance().Shutdown(); });
    return exports;
}

NODE_API_MODULE(liquid_glass, Init)
