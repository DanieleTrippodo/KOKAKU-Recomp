#include "kokaku_rtx.hpp"
#ifdef _WIN32
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <dxcapi.h>
#include <wrl/client.h>

namespace kokaku {
using Microsoft::WRL::ComPtr;
namespace {
void checked(HRESULT hr, const char *where) {
    if (FAILED(hr)) throw std::runtime_error(std::string(where) + " HRESULT=" + std::to_string(unsigned(hr)));
}
struct Buffer {
    ComPtr<ID3D12Resource> resource;
    UINT64 capacity{};
    unsigned char *mapped{};
    void ensure(ID3D12Device *device, UINT64 bytes, bool upload, bool acceleration = false) {
        if (resource && capacity >= bytes) return;
        capacity = (std::max<UINT64>(bytes, 256) + 65535) & ~UINT64(65535);
        resource.Reset(); mapped = nullptr;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = capacity; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : acceleration ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                                                    : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&resource)), "RTX buffer");
        if (upload) {
            D3D12_RANGE read{0,0};
            checked(resource->Map(0, &read, reinterpret_cast<void **>(&mapped)), "RTX map");
        }
    }
    auto address() const { return resource->GetGPUVirtualAddress(); }
};
struct Frame {
    std::array<Buffer, 8> blas;
    Buffer vertices, instances, tlas, scratch, pixels, normals;
};
void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    list->ResourceBarrier(1, &b);
}
}
struct RtxImpl {
    ComPtr<ID3D12Device5> device;
    ComPtr<IDxcCompiler3> compiler;
    HMODULE module{};
    std::array<Frame, 2> frames;
    ~RtxImpl() { compiler.Reset(); if (module) FreeLibrary(module); }
};
RtxRenderer::RtxRenderer() : impl(std::make_unique<RtxImpl>()) {}
RtxRenderer::~RtxRenderer() = default;
bool RtxRenderer::initialize(ID3D12Device *device, std::string &error) {
    try {
        checked(device->QueryInterface(IID_PPV_ARGS(&impl->device)), "RTX device interface");
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        const auto path = std::filesystem::path(executable).parent_path()/L"dxcompiler.dll";
        impl->module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!impl->module) throw std::runtime_error("RTX: dxcompiler.dll mancante accanto all'eseguibile");
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(impl->module, "DxcCreateInstance"));
        if (!create) throw std::runtime_error("RTX: compilatore DXC non valido");
        checked(create(CLSID_DxcCompiler, IID_PPV_ARGS(&impl->compiler)), "RTX compiler");
        return true;
    } catch (const std::exception &e) { error = e.what(); return false; }
}
bool RtxRenderer::compile(const char *source, const wchar_t *entry, const wchar_t *profile,
                          ID3DBlob **result, std::string &error) {
    try {
        const wchar_t *args[]{L"-E",entry,L"-T",profile,L"-D",L"KOKAKU_RTX=1",L"-O3",L"-WX"};
        DxcBuffer buffer{source, std::strlen(source), DXC_CP_UTF8};
        ComPtr<IDxcResult> output;
        checked(impl->compiler->Compile(&buffer, args, UINT(std::size(args)), nullptr, IID_PPV_ARGS(&output)), "DXC compile");
        HRESULT status{}; checked(output->GetStatus(&status), "DXC status");
        if (FAILED(status)) {
            ComPtr<IDxcBlobUtf8> errors;
            output->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            throw std::runtime_error(errors ? errors->GetStringPointer() : "RTX shader compilation failed");
        }
        ComPtr<IDxcBlob> shader;
        checked(output->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr), "DXC shader");
        checked(D3DCreateBlob(shader->GetBufferSize(), result), "RTX shader buffer");
        std::memcpy((*result)->GetBufferPointer(), shader->GetBufferPointer(), shader->GetBufferSize());
        return true;
    } catch (const std::exception &e) { error = e.what(); return false; }
}
bool RtxRenderer::build(ID3D12GraphicsCommandList *commands, unsigned slot,
                        std::span<const RtxGeometry> groups, size_t draw_count, std::string &error) {
    try {
        auto &f = impl->frames.at(slot);
        auto *device = impl->device.Get();
        ComPtr<ID3D12GraphicsCommandList4> list;
        checked(commands->QueryInterface(IID_PPV_ARGS(&list)), "RTX command list");
        if (groups.size() > 8 || draw_count > 65536) throw std::runtime_error("RTX: limite scena superato");
        size_t vertex_count = 0;
        for (const auto &g : groups) vertex_count += g.vertices.size();
        if (vertex_count > 3000000) throw std::runtime_error("RTX: limite di un milione di triangoli superato");
        f.vertices.ensure(device, vertex_count * 12, true);
        f.pixels.ensure(device, draw_count * 256, true);
        f.instances.ensure(device, 8 * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), true);
        std::array<D3D12_RAYTRACING_GEOMETRY_DESC, 8> geometry{};
        std::array<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, 8> inputs{};
        std::array<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO, 8> info{};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
        top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.NumDescs = UINT(groups.size());
        top.InstanceDescs = f.instances.address();
        top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO top_info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&top, &top_info);
        UINT64 scratch = top_info.ScratchDataSizeInBytes;
        size_t first = 0;
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto &group = groups[i];
            std::memcpy(f.vertices.mapped + first * 12, group.vertices.data(), group.vertices.size() * 12);
            auto &g = geometry[i];
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexCount = UINT(group.vertices.size());
            g.Triangles.VertexBuffer = {f.vertices.address() + first * 12, 12};
            auto &in = inputs[i];
            in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            in.NumDescs = 1; in.pGeometryDescs = &g;
            in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info[i]);
            f.blas[i].ensure(device, info[i].ResultDataMaxSizeInBytes, false, true);
            scratch = std::max(scratch, info[i].ScratchDataSizeInBytes);
            first += group.vertices.size();
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
            instance.InstanceMask = group.mask;
            instance.AccelerationStructure = f.blas[i].address();
            std::memcpy(f.instances.mapped + i * sizeof(instance), &instance, sizeof(instance));
        }
        f.scratch.ensure(device, scratch, false);
        f.tlas.ensure(device, top_info.ResultDataMaxSizeInBytes, false, true);
        for (size_t i = 0; i < groups.size(); ++i) {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = inputs[i];
            build.DestAccelerationStructureData = f.blas[i].address();
            build.ScratchAccelerationStructureData = f.scratch.address();
            list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            barrier(list.Get(), f.blas[i].resource.Get());
            barrier(list.Get(), f.scratch.resource.Get());
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = top;
        build.DestAccelerationStructureData = f.tlas.address();
        build.ScratchAccelerationStructureData = f.scratch.address();
        list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        barrier(list.Get(), f.tlas.resource.Get());
        return true;
    } catch (const std::exception &e) { error = e.what(); return false; }
}
D3D12_VERTEX_BUFFER_VIEW RtxRenderer::normals(unsigned slot, std::span<const std::array<float,3>> values) {
    auto &buffer = impl->frames.at(slot).normals;
    buffer.ensure(impl->device.Get(), values.size_bytes(), true);
    if (!values.empty()) std::memcpy(buffer.mapped, values.data(), values.size_bytes());
    return {buffer.address(), UINT(values.size_bytes()), 12};
}
D3D12_GPU_VIRTUAL_ADDRESS RtxRenderer::scene(unsigned slot) const { return impl->frames.at(slot).tlas.address(); }
D3D12_GPU_VIRTUAL_ADDRESS RtxRenderer::pixel_constants(unsigned slot, size_t draw, const void *data, size_t size) {
    auto &buffer = impl->frames.at(slot).pixels;
    if (size > 256 || draw * 256 + size > buffer.capacity) throw std::runtime_error("RTX pixel constants overflow");
    std::memcpy(buffer.mapped + draw * 256, data, size);
    return buffer.address() + draw * 256;
}
}
#endif
