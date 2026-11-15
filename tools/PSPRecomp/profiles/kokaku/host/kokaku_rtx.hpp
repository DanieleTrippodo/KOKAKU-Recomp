#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <d3d12.h>
#include <d3dcompiler.h>

namespace kokaku {
struct RtxGeometry {
    unsigned mask{};
    std::vector<std::array<float, 3>> vertices; // triangle list, in world space
};
struct RtxImpl;
class RtxRenderer {
    std::unique_ptr<RtxImpl> impl;
public:
    RtxRenderer();
    ~RtxRenderer();
    bool initialize(ID3D12Device *device, std::string &error);
    bool compile(const char *source, const wchar_t *entry, const wchar_t *profile, ID3DBlob **result, std::string &error);
    // The caller waits for this frame slot's fence before rebuilding/reusing it.
    bool build(ID3D12GraphicsCommandList *list, unsigned slot, std::span<const RtxGeometry> geometry,
               size_t draw_count, std::string &error);
    D3D12_VERTEX_BUFFER_VIEW normals(unsigned slot, std::span<const std::array<float,3>> values);
    D3D12_GPU_VIRTUAL_ADDRESS scene(unsigned slot) const;
    D3D12_GPU_VIRTUAL_ADDRESS pixel_constants(unsigned slot, size_t draw, const void *data, size_t size);
};
}
#endif
