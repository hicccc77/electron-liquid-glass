#pragma once
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// 创建硬件 D3D11 设备（BGRA 支持，供 DComp 合成互操作）；失败时尝试 WARP
HRESULT CreateD3DDevice(ComPtr<ID3D11Device>* device, ComPtr<ID3D11DeviceContext>* context);
