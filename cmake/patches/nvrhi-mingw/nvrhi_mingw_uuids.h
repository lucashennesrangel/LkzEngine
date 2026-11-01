#pragma once

// MinGW: o __uuidof das interfaces do D3D12 (IID_PPV_ARGS) vem do dxguids.h do
// DirectX-Headers, que só cobre os headers já incluídos. Incluir depois do d3d12.h.
#include <d3d12sdklayers.h>
#include <dxguids/dxguids.h>

#if defined(__CRT_UUID_DECL)
// Interfaces de prévia que o dxguids.h ainda não cobre (valores do d3d12.h).
__CRT_UUID_DECL(ID3D12DevicePreview, 0x55ea41d3, 0x6bf5, 0x4332, 0xbb, 0xf9, 0x90, 0x5e, 0x6b, 0x4e, 0x29, 0x30)
__CRT_UUID_DECL(ID3D12GraphicsCommandListPreview, 0x536d9bb6, 0x9eee, 0x4c75, 0x86, 0xe8, 0xe2, 0x9e, 0x29, 0xe0, 0x8e, 0xd3)
#endif
