// feed_portrait_blend.h -- portrait-mode edge blend, recorded on our private D3D12 list right after
// the evaluate. out = lerp(input, NR result, w), where w ramps 0 -> 1 over `feather` pixels inside
// each atlas block (except on sides that touch the screen edge). The NR'd portrait then fades into
// the untouched UI around it instead of ending in a hard square. Included by dlss5-feed.cpp.

#pragma once

#include <d3dcompiler.h>

namespace pblend
{
static constexpr int kMaxBlocks = 24;
static constexpr UINT kCbSlot = 1024;   // per in-flight frame, 256-byte aligned

struct Block { uint32_t x, y, w, h; uint32_t feather_l, feather_t, feather_r, feather_b; };
struct Cb { uint32_t count, pad0, pad1, pad2; Block blocks[kMaxBlocks]; };
static_assert(sizeof(Cb) <= kCbSlot, "blend constant buffer slot too small");

static const char kHlsl[] = R"(
struct Block { uint4 rect; uint4 feather; };
cbuffer Blocks : register(b0) { uint count; uint3 pad; Block blocks[24]; };
Texture2D<float4>   nr   : register(t0);
Texture2D<float4>   src  : register(t1);
RWTexture2D<float4> outp : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float w = 0.0;
    [loop] for (uint i = 0; i < count; ++i)
    {
        int4 r = int4(blocks[i].rect);
        int2 p = int2(id.xy) - r.xy;
        if (p.x < 0 || p.y < 0 || p.x >= r.z || p.y >= r.w) continue;
        float4 f = max(float4(blocks[i].feather), 1.0);
        float4 d = float4(p.x, p.y, r.z - 1 - p.x, r.w - 1 - p.y) + 0.5;
        float4 k = blocks[i].feather > 0 ? saturate(d / f) : 1.0;
        w = min(min(k.x, k.y), min(k.z, k.w));
    }
    outp[id.xy] = lerp(src[id.xy], nr[id.xy], w);
}
)";

static ID3D12RootSignature  *g_rs;
static ID3D12PipelineState  *g_pso;
static ID3D12DescriptorHeap *g_heap;
static ID3D12Resource       *g_nr_copy;
static ID3D12Resource       *g_cb;
static uint8_t              *g_cb_ptr;
static ID3D12Resource       *g_bound_color, *g_bound_output;   // what the descriptors point at
static bool                  g_failed;

// Size-dependent part only (feature rebuilds); the pipeline survives.
static void ReleaseTargets()
{
    SafeRelease(g_nr_copy);
    g_bound_color = g_bound_output = nullptr;
}

static void Release()
{
    if (g_cb != nullptr && g_cb_ptr != nullptr) g_cb->Unmap(0, nullptr);
    g_cb_ptr = nullptr;
    SafeRelease(g_cb);
    SafeRelease(g_nr_copy);
    SafeRelease(g_heap);
    SafeRelease(g_pso);
    SafeRelease(g_rs);
    g_bound_color = g_bound_output = nullptr;
    g_failed = false;
}

static bool InitPipeline(ID3D12Device *dev)
{
    if (g_pso != nullptr) return true;

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d12 != nullptr ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr) { Log("[feed] portrait blend: d3dcompiler/d3d12 entry points missing"); return false; }

    ID3DBlob *cs = nullptr, *err = nullptr;
    if (FAILED(compile(kHlsl, sizeof(kHlsl) - 1, "portrait_blend", nullptr, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err)))
    {
        Log("[feed] portrait blend: shader compile failed: %s", err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2; ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;

    ID3DBlob *sig = nullptr;
    HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    SafeRelease(err);
    if (SUCCEEDED(hr))
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                      reinterpret_cast<void **>(&g_rs));
    SafeRelease(sig);
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_rs;
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g_pso));
    }
    SafeRelease(cs);
    if (FAILED(hr)) { Log("[feed] portrait blend: pipeline creation failed 0x%08X", hr); return false; }
    return true;
}

// (Re)creates the NR copy + descriptors whenever the shared textures were rebuilt.
static bool EnsureResources(ID3D12Device *dev, ID3D12Resource *color, ID3D12Resource *output,
                            UINT w, UINT h, DXGI_FORMAT color_fmt, DXGI_FORMAT out_fmt)
{
    if (g_failed) return false;
    if (!InitPipeline(dev)) { g_failed = true; return false; }
    if (g_bound_color == color && g_bound_output == output && g_nr_copy != nullptr) return true;

    SafeRelease(g_nr_copy);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = out_fmt; rd.SampleDesc.Count = 1;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_nr_copy));
    if (SUCCEEDED(hr) && g_heap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_heap));
    }
    if (SUCCEEDED(hr) && g_cb == nullptr)
    {
        D3D12_HEAP_PROPERTIES up = {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = UINT64(kCbSlot) * Feed::kFrames; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_cb));
        if (SUCCEEDED(hr)) hr = g_cb->Map(0, nullptr, reinterpret_cast<void **>(&g_cb_ptr));
    }
    if (FAILED(hr))
    {
        Log("[feed] portrait blend: resource creation failed 0x%08X; portraits pasted without feathering", hr);
        g_failed = true;
        return false;
    }

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = out_fmt;
    dev->CreateShaderResourceView(g_nr_copy, &sv, h0);
    h0.ptr += inc;
    sv.Format = color_fmt;
    dev->CreateShaderResourceView(color, &sv, h0);
    h0.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = out_fmt;
    dev->CreateUnorderedAccessView(output, nullptr, &uv, h0);

    g_bound_color = color;
    g_bound_output = output;
    Log("[feed] portrait blend ready (%ux%u, feathered edges)", w, h);
    return true;
}

// Precondition: color in NON_PIXEL_SHADER_RESOURCE, output in UNORDERED_ACCESS (as left by the
// evaluate). Leaves them in the same states.
static void Record(ID3D12GraphicsCommandList *list, ID3D12Resource *output, int slot, UINT w, UINT h, const Cb &cb)
{
    memcpy(g_cb_ptr + UINT64(slot) * kCbSlot, &cb, sizeof(cb));

    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = output;
    list->ResourceBarrier(1, &uav);

    Barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(g_nr_copy, output);
    Barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(g_nr_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_rs);
    list->SetPipelineState(g_pso);
    list->SetComputeRootDescriptorTable(0, g_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootConstantBufferView(1, g_cb->GetGPUVirtualAddress() + UINT64(slot) * kCbSlot);
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    list->ResourceBarrier(1, &uav);
    Barrier(g_nr_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}
}  // namespace pblend

static void PortraitBlendReleaseTargets() { pblend::ReleaseTargets(); }
static void PortraitBlendRelease()        { pblend::Release(); }
