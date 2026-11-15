#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdlib>
#include <string>
#include <string_view>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace kokaku {
inline bool rtx_requested() noexcept {
    const auto *value = std::getenv("PSPRECOMP_RTX");
    return value && std::string_view(value) == "1";
}
inline bool select_gpu(IDXGIFactory6 *factory, Microsoft::WRL::ComPtr<IDXGIAdapter1> &adapter,
                       std::string &error, bool require_rtx = rtx_requested()) {
    const auto *preference = std::getenv("PSPRECOMP_GPU");
    const bool nvidia = require_rtx || (preference && std::string_view(preference) == "NVIDIA");
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        const auto hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { error = "Impossibile enumerare le GPU DirectX 12"; return false; }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            (nvidia && desc.VendorId != 0x10de)) continue;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) continue;
        if (require_rtx) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
            D3D12_FEATURE_DATA_SHADER_MODEL shader{D3D_SHADER_MODEL_6_5};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
                options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 ||
                FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader, sizeof(shader))) ||
                shader.HighestShaderModel < D3D_SHADER_MODEL_6_5) continue;
        }
        adapter = candidate;
        return true;
    }
    error = require_rtx ? "RTX richiede una GPU NVIDIA con DXR 1.1 e Shader Model 6.5. Controllare GPU e driver."
                        : nvidia ? "GPU NVIDIA DirectX 12 non disponibile" : "GPU DirectX 12 non disponibile";
    return false;
}
}
#endif
