#include "d3d_utils.h"

HRESULT CreateD3DDevice(ComPtr<ID3D11Device>* device, ComPtr<ID3D11DeviceContext>* context) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        device->GetAddressOf(), nullptr, context->GetAddressOf());
    if (SUCCEEDED(hr)) return hr;
    return D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        device->GetAddressOf(), nullptr, context->GetAddressOf());
}
