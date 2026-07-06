// Placeholder implementation for non-Windows platforms: the module loads,
// isSupported is always false, and callers fall back to their own
// cross-platform approach (e.g. a Chromium screen-capture pipeline)
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
