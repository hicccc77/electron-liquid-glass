// 非 Windows 平台占位实现：模块可加载，isSupported 恒为 false，
// 调用方据此回退到自己的跨平台方案（如 Chromium 屏幕采集管线）
#include <napi.h>

namespace {

Napi::Value IsSupported(const Napi::CallbackInfo& info) {
    return Napi::Boolean::New(info.Env(), false);
}

Napi::Value OsBuild(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), 0);
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("isSupported", Napi::Function::New(env, IsSupported));
    exports.Set("osBuild", Napi::Function::New(env, OsBuild));
    return exports;
}

NODE_API_MODULE(liquid_glass, Init)
