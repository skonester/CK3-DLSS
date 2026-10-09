// Frontier 2: spend bandwidth on portrait edges, where blending actually changes a pixel.
// Aquarius refuses the full-atlas copy tax; Sagittarius gives each edge pixel one owner.
// Recorded after evaluation on the private D3D12 list. Atlas padding is never pasted home.
//
// Frontier 3 (section 2): a block flagged in Cb::coverage_blocks is composited with a character
// coverage atlas (R8, same coordinates as the colour atlas): one job covers the whole block,
// interior included, and out = src + (neural - src) * feather_weight * coverage. Coverage 0
// restores the original pixel bit-exactly and coverage 1 keeps the neural pixel bit-exactly.
// Unflagged blocks keep the legacy edge-band feather. Flags are ignored while no coverage
// resource is bound, so a missing mask can only ever fall back to the legacy path.

#pragma once

#include <d3dcompiler.h>

namespace pblend
{
static constexpr int kMaxBlocks = 24;
static constexpr int kMaxJobs = kMaxBlocks * 4;
static constexpr UINT kGroupWidth = 16, kGroupHeight = 8;
static constexpr UINT kCbSlot = 4096;   // per retired frame slot, 256-byte aligned

struct Block { uint32_t x, y, w, h; uint32_t feather_l, feather_t, feather_r, feather_b; };
// coverage_blocks: bit i set = block i is composited with the coverage atlas (Frontier 3).
struct Cb { uint32_t count, coverage_blocks, pad1, pad2; Block blocks[kMaxBlocks]; };
static_assert(kMaxBlocks <= 32, "coverage_blocks is a 32-bit mask");
// flags bit 0: whole-block coverage job.
struct EdgeJob { uint32_t x, y, w, h; uint32_t block, group_end, groups_x, flags; };
struct DispatchCb
{
    uint32_t job_count, group_count, dispatch_width, pad;
    Block blocks[kMaxBlocks];
    EdgeJob jobs[kMaxJobs];
};
static_assert(sizeof(Block) == 32 && sizeof(EdgeJob) == 32, "HLSL layout mismatch");
static_assert(sizeof(DispatchCb) <= kCbSlot, "blend constant buffer slot too small");

// Disjoint bands give each pixel exactly one writer, including tiny portraits whose
// feathers overlap. The CPU submits at most 96 jobs; the GPU never scans 24 blocks per pixel.
// A coverage block is one job over the whole block: its interior changes too.
static bool BuildDispatch(const Cb &cb, UINT w, UINT h, DispatchCb &out)
{
    out = {};
    if (cb.count > kMaxBlocks || w == 0 || h == 0 ||
        w > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || h > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
    for (uint32_t i = 0; i < cb.count; ++i)
    {
        const Block &b = cb.blocks[i];
        if (b.w == 0 || b.h == 0 || b.x >= w || b.y >= h || b.w > w - b.x || b.h > h - b.y) return false;
        for (uint32_t j = 0; j < i; ++j)
        {
            const Block &a = cb.blocks[j];
            if (b.x < a.x + a.w && a.x < b.x + b.w && b.y < a.y + a.h && a.y < b.y + b.h) return false;
        }
        out.blocks[i] = b;
        auto add = [&](uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, uint32_t flags) {
            if (rw == 0 || rh == 0) return;
            const uint32_t gx = (rw - 1) / kGroupWidth + 1, gy = (rh - 1) / kGroupHeight + 1;
            out.group_count += gx * gy;
            out.jobs[out.job_count++] = { x, y, rw, rh, i, out.group_count, gx, flags };
        };
        if ((cb.coverage_blocks >> i) & 1u) { add(b.x, b.y, b.w, b.h, 1); continue; }
        const uint32_t top = (std::min)(b.feather_t, b.h);
        const uint32_t bottom = (std::min)(b.feather_b, b.h - top);
        const uint32_t middle = b.h - top - bottom;
        const uint32_t left = (std::min)(b.feather_l, b.w);
        const uint32_t right = (std::min)(b.feather_r, b.w - left);
        add(b.x, b.y, b.w, top, 0);
        add(b.x, b.y + b.h - bottom, b.w, bottom, 0);
        add(b.x, b.y + top, left, middle, 0);
        add(b.x + b.w - right, b.y + top, right, middle, 0);
    }
    out.dispatch_width = (std::min)(out.group_count, UINT(D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION));
    return true;
}

static const char kHlsl[] = R"(
struct Block { uint4 rect; uint4 feather; };
struct EdgeJob { uint4 rect; uint4 schedule; };
cbuffer Edges : register(b0)
{
    uint job_count; uint group_count; uint dispatch_width; uint pad;
    Block blocks[24]; EdgeJob jobs[96];
};
#if !COSMIC_IN_PLACE
Texture2D<float4>   nr   : register(t0);
#endif
Texture2D<float4>   src  : register(t1);
Texture2D<float>    cover: register(t2);   // character coverage atlas (null descriptor reads 0)
RWTexture2D<float4> outp : register(u0);

[numthreads(16, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID)
{
    uint ticket = group.x + group.y * dispatch_width;
    if (ticket >= group_count) return;
    // Every lane in the group takes the same search path. No divergent portrait hunt.
    uint lo = 0, hi = job_count;
    [loop] while (lo < hi)
    {
        uint mid = (lo + hi) / 2;
        if (ticket < jobs[mid].schedule.y) hi = mid;
        else lo = mid + 1;
    }
    EdgeJob job = jobs[lo];
    uint local = ticket - (lo == 0 ? 0 : jobs[lo - 1].schedule.y);
    uint2 p = uint2(local % job.schedule.z, local / job.schedule.z) * uint2(16, 8) + lane.xy;
    if (any(p >= job.rect.zw)) return;
    uint2 pixel = job.rect.xy + p;
    Block block = blocks[job.schedule.x];
    uint2 offset = pixel - block.rect.xy;
    float4 distance = float4(offset, block.rect.zw - 1 - offset) + 0.5;
    float4 feather = max(float4(block.feather), 1.0);
    float4 k = block.feather > 0 ? saturate(distance / feather) : 1.0;
    float weight = min(min(k.x, k.y), min(k.z, k.w));
    if (job.schedule.w & 1) weight *= saturate(cover[pixel]);
#if COSMIC_IN_PLACE
    float4 neural = outp[pixel];
#else
    float4 neural = nr[pixel];
#endif
    float4 source = src[pixel];
    // Exact end points: no coverage is the original pixel, full coverage the neural one.
    outp[pixel] = weight >= 1.0 ? neural : weight <= 0.0 ? source : lerp(source, neural, weight);
}
)";

static ID3D12RootSignature  *g_rs;
static ID3D12PipelineState  *g_pso[2];   // SRV fallback, typed UAV load
static ID3D12DescriptorHeap *g_heap;
static ID3D12Resource       *g_nr_copy;
static ID3D12Resource       *g_cb;
static uint8_t              *g_cb_ptr;
static ID3D12Resource       *g_bound_color, *g_bound_output;   // what the descriptors point at
static ID3D12Resource       *g_bound_cover;                   // coverage atlas, or nullptr (null descriptor)
static bool                  g_failed;
static bool                  g_in_place;

// Size-dependent part only (feature rebuilds); the pipeline survives.
static void ReleaseTargets()
{
    SafeRelease(g_nr_copy);
    g_bound_color = g_bound_output = g_bound_cover = nullptr;
}

static void Release()
{
    if (g_cb != nullptr && g_cb_ptr != nullptr) g_cb->Unmap(0, nullptr);
    g_cb_ptr = nullptr;
    SafeRelease(g_cb);
    SafeRelease(g_nr_copy);
    SafeRelease(g_heap);
    SafeRelease(g_pso[0]);
    SafeRelease(g_pso[1]);
    SafeRelease(g_rs);
    g_bound_color = g_bound_output = g_bound_cover = nullptr;
    g_failed = false;
    g_in_place = false;
}

static bool InitPipeline(ID3D12Device *dev, bool in_place)
{
    const int variant = in_place ? 1 : 0;
    if (g_pso[variant] != nullptr) return true;

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d12 != nullptr ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr)
    {
        if (dc != nullptr) FreeLibrary(dc);
        Log("[feed] portrait blend: d3dcompiler/d3d12 entry points missing");
        return false;
    }

    ID3DBlob *cs = nullptr, *err = nullptr;
    const D3D_SHADER_MACRO macros[] = { { "COSMIC_IN_PLACE", in_place ? "1" : "0" }, { nullptr, nullptr } };
    const HRESULT compiled = compile(kHlsl, sizeof(kHlsl) - 1, "portrait_blend", macros, nullptr, "main", "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err);
    if (FAILED(compiled))
    {
        Log("[feed] portrait blend: shader compile failed: %s", err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "?");
        SafeRelease(err);
        SafeRelease(cs);
        FreeLibrary(dc);
        return false;
    }
    SafeRelease(err);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 3; ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
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
    HRESULT hr = S_OK;
    if (g_rs == nullptr) hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    SafeRelease(err);
    if (SUCCEEDED(hr) && g_rs == nullptr)
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                      reinterpret_cast<void **>(&g_rs));
    SafeRelease(sig);
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_rs;
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g_pso[variant]));
    }
    SafeRelease(cs);
    FreeLibrary(dc);   // shader/error blobs must be released before their DLL can unload
    if (FAILED(hr)) { Log("[feed] portrait blend: pipeline creation failed 0x%08X", hr); return false; }
    return true;
}

// Typed loads are format-specific. A fast path earns its place through device capabilities.
static bool SupportsInPlace(ID3D12Device *dev, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = format;
    return SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
           options.TypedUAVLoadAdditionalFormats &&
           SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0 &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

// Rebind after a feature rebuild. Only devices without typed loads pay for a scratch atlas.
// `cover` (optional) is the R8_UNORM coverage atlas; it is bound with the other atlas resources
// because the descriptor heap is shared by all frame slots and may only change on a rebuild.
static bool EnsureResources(ID3D12Device *dev, ID3D12Resource *color, ID3D12Resource *output,
                            UINT w, UINT h, DXGI_FORMAT color_fmt, DXGI_FORMAT out_fmt, ID3D12Resource *cover = nullptr)
{
    if (g_failed) return false;
    if (g_bound_color == color && g_bound_output == output && g_bound_cover == cover && g_cb_ptr != nullptr) return true;
    g_in_place = SupportsInPlace(dev, out_fmt);
    if (!InitPipeline(dev, g_in_place)) { g_failed = true; return false; }

    SafeRelease(g_nr_copy);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = out_fmt; rd.SampleDesc.Count = 1;
    HRESULT hr = S_OK;
    if (!g_in_place) hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_nr_copy));
    if (SUCCEEDED(hr) && g_heap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4;
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
        const D3D12_RANGE no_read = { 0, 0 };
        if (SUCCEEDED(hr)) hr = g_cb->Map(0, &no_read, reinterpret_cast<void **>(&g_cb_ptr));
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
    sv.Format = DXGI_FORMAT_R8_UNORM;
    dev->CreateShaderResourceView(cover, &sv, h0);   // nullptr: a null descriptor that reads 0
    h0.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = out_fmt;
    dev->CreateUnorderedAccessView(output, nullptr, &uv, h0);

    g_bound_color = color;
    g_bound_output = output;
    g_bound_cover = cover;
    Log("[feed] portrait blend ready (%ux%u, edge jobs, %s%s)", w, h,
        g_in_place ? "Aquarius in-place UAV: no atlas copy" : "SRV copy fallback",
        cover != nullptr ? ", coverage atlas bound" : "");
    return true;
}

// Precondition: color (and the coverage atlas, when bound) in NON_PIXEL_SHADER_RESOURCE, output
// in UNORDERED_ACCESS (as left by the evaluate). Leaves them in the same states.
static void Record(ID3D12GraphicsCommandList *list, ID3D12Resource *output, int slot, UINT w, UINT h, const Cb &request)
{
    Cb cb = request;
    if (g_bound_cover == nullptr) cb.coverage_blocks = 0;   // no mask: legacy edge feather only
    DispatchCb dispatch;
    if (slot < 0 || slot >= Feed::kFrames || !BuildDispatch(cb, w, h, dispatch))
    {
        Log("[feed] portrait blend: invalid or overlapping atlas blocks; retaining neural output");
        return;
    }
    if (dispatch.group_count == 0) return;
    memcpy(g_cb_ptr + UINT64(slot) * kCbSlot, &dispatch, sizeof(dispatch));

    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = output;
    list->ResourceBarrier(1, &uav);

    if (!g_in_place)
    {
        Barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(g_nr_copy, output);
        Barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(g_nr_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_rs);
    list->SetPipelineState(g_pso[g_in_place ? 1 : 0]);
    list->SetComputeRootDescriptorTable(0, g_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootConstantBufferView(1, g_cb->GetGPUVirtualAddress() + UINT64(slot) * kCbSlot);
    list->Dispatch(dispatch.dispatch_width, (dispatch.group_count - 1) / dispatch.dispatch_width + 1, 1);

    list->ResourceBarrier(1, &uav);
    if (!g_in_place) Barrier(g_nr_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}
}  // namespace pblend

static void PortraitBlendReleaseTargets() { pblend::ReleaseTargets(); }
static void PortraitBlendRelease()        { pblend::Release(); }
