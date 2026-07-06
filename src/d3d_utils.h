#pragma once
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Create a hardware D3D11 device (BGRA support for DComp interop); falls back to WARP
HRESULT CreateD3DDevice(ComPtr<ID3D11Device>* device, ComPtr<ID3D11DeviceContext>* context);
