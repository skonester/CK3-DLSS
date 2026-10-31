// Frontier 3, texture-space portraits (portrait_texture=1). NR runs on the Portrait textures
// themselves -- the off-screen character renders whose alpha is the silhouette -- packed into an
// atlas, and the game's GUI shader composites the enhanced copies (feed_render_dump.h re-issues
// the draws). The map and UI behind a portrait are never processed, so there is no rectangle to
// hide, and no screen geometry (3 frames late from the readback) is involved at all.
//
// Planning reuses the Frontier 2 allocator: each texture gets a virtual "screen" cell, so
// portrait::Allocate's slot retention, hard budget and resize hysteresis apply unchanged. An
// animated portrait is re-rendered into a fresh texture every few hundred ms, and a hovered one
// into a fresh texture of a new size every frame of its zoom (455..473 px live). A slot reserves
// kHeadroom over its texture, the texture sits at the slot's top-left, and a texture replacing a
// vanished one at the same draw position within kSimilar of its size inherits the cell; the slot
// only grows. The atlas layout (and the feature) survive the swap and the zoom; only a size change
// resets NR history.
//
// After evaluation a finish pass keeps the game's alpha: out = (min(nr.rgb, a), a), and pixels
// with a == 0 stay exactly the original, so the premultiplied texture stays premultiplied.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "feed_portrait_atlas.h"

namespace ptex
{
static constexpr int kCellSize = 1024, kCellsPerRow = 16, kCells = kCellsPerRow * kCellsPerRow;
static constexpr uint32_t kSpace = uint32_t(kCellSize * kCellsPerRow);   // virtual screen for the allocator

// One Portrait texture sampled this frame; index = its first draw among the frame's portrait draws.
struct Tex { uint64_t res, gen; uint32_t w, h, index, level, layer; };
// sw x sh: the reserved slot (>= the texture, grow-only while the slot lives).
struct Slot { Tex tex; int cell; uint32_t sw, sh; };

// 5% headroom: 455 -> 478 covers the whole live hover zoom (455..473) in one slot.
static inline uint32_t Headroom(uint32_t texels) { return (std::min)(uint32_t(kCellSize), (texels * 21 + 19) / 20); }
// The same portrait re-rendered at another size: within 10% on both axes.
static inline bool Similar(uint32_t a_w, uint32_t a_h, uint32_t b_w, uint32_t b_h)
{
    auto within = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) * 10 <= (std::max)(a, b); };
    return within(a_w, b_w) && within(a_h, b_h);
}

static inline portrait::Rect CellRect(const Slot &s)
{
    return { (s.cell % kCellsPerRow) * kCellSize, (s.cell / kCellsPerRow) * kCellSize, int(s.sw), int(s.sh) };
}

// A slot keeps its reservation while the texture fits, otherwise grows (with headroom again).
static inline uint32_t Grow(uint32_t reserved, uint32_t texels) { return texels <= reserved ? reserved : Headroom(texels); }

// Next frame's slots: a known texture keeps its cell, a replacement inherits one, others get the
// lowest free cell. Textures larger than a cell are left out (they exceed any sane budget).
// Returns true when an inherited slot's texture changed size (its NR history no longer matches).
static inline bool AssignCells(std::vector<Slot> &slots, const std::vector<Tex> &textures)
{
    bool resized = false;
    std::vector<Slot> next;
    std::vector<bool> taken(slots.size(), false), used(kCells, false);
    std::vector<const Tex *> pending;
    for (const Tex &t : textures)
    {
        if (t.w == 0 || t.h == 0 || t.w > uint32_t(kCellSize) || t.h > uint32_t(kCellSize)) continue;
        size_t j = 0;
        while (j < slots.size() && !(slots[j].tex.res == t.res && slots[j].tex.gen == t.gen)) ++j;
        if (j < slots.size() && !taken[j])
        {
            taken[j] = true; used[size_t(slots[j].cell)] = true;
            next.push_back({ t, slots[j].cell, Grow(slots[j].sw, t.w), Grow(slots[j].sh, t.h) });
        }
        else pending.push_back(&t);
    }
    std::vector<const Tex *> fresh;
    for (const Tex *t : pending)
    {
        size_t j = 0;
        while (j < slots.size() && (taken[j] || slots[j].tex.index != t->index || !Similar(slots[j].tex.w, slots[j].tex.h, t->w, t->h))) ++j;
        if (j < slots.size())
        {
            taken[j] = true; used[size_t(slots[j].cell)] = true;
            next.push_back({ *t, slots[j].cell, Grow(slots[j].sw, t->w), Grow(slots[j].sh, t->h) });
            resized |= slots[j].tex.w != t->w || slots[j].tex.h != t->h;
        }
        else fresh.push_back(t);
    }
    for (const Tex *t : fresh)
    {
        int cell = 0;
        while (cell < kCells && used[size_t(cell)]) ++cell;
        if (cell == kCells) break;
        used[size_t(cell)] = true;
        next.push_back({ *t, cell, Headroom(t->w), Headroom(t->h) });
    }
    slots.swap(next);
    return resized;
}

// The slot an atlas item was allocated for (items carry the cell rectangle as their screen crop).
static inline const Slot *SlotOf(const std::vector<Slot> &slots, const portrait::Item &it)
{
    for (const Slot &s : slots)
    {
        const portrait::Rect r = CellRect(s);
        if (r.x == it.sx && r.y == it.sy && r.w == it.w && r.h == it.h) return &s;
    }
    return nullptr;
}

static inline std::vector<portrait::Candidate> Candidates(const std::vector<Slot> &slots)
{
    std::vector<portrait::Candidate> out;
    for (const Slot &s : slots) out.push_back({ CellRect(s), CellRect(s) });
    return out;
}
}  // namespace ptex

#ifdef __d3d12_h__
#include <d3dcompiler.h>

// D3D12 finish pass: alpha of the original portrait, colour of the NR output (in place).
namespace ptex
{
static const char kFinishHlsl[] = R"(
Texture2D<float4>   src  : register(t0);
RWTexture2D<float4> outp : register(u0);
cbuffer Size : register(b0) { uint width; uint height; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    float4 s = src[id.xy];
    float4 n = outp[id.xy];
    // Transparent texels stay exactly the original; elsewhere NR colour, the game's alpha, premultiplied.
    outp[id.xy] = s.a <= 0.0 ? s : float4(min(n.rgb, s.a), s.a);
}
)";

static ID3D12RootSignature  *g_rs;
static ID3D12PipelineState  *g_pso;
static ID3D12DescriptorHeap *g_heap;
static ID3D12Resource       *g_bound_src, *g_bound_out;
static bool                  g_failed;

static void ReleaseTargets() { g_bound_src = g_bound_out = nullptr; }
static void Release()
{
    SafeRelease(g_heap); SafeRelease(g_pso); SafeRelease(g_rs);
    g_bound_src = g_bound_out = nullptr;
    g_failed = false;
}

// Typed UAV loads of the output format are required (in-place read-modify-write).
static bool Supports(ID3D12Device *dev, DXGI_FORMAT format)
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

static bool EnsureResources(ID3D12Device *dev, ID3D12Resource *src, ID3D12Resource *out, DXGI_FORMAT src_fmt, DXGI_FORMAT out_fmt)
{
    if (g_failed) return false;
    if (g_bound_src == src && g_bound_out == out && g_pso != nullptr) return true;
    if (!Supports(dev, out_fmt))
    {
        Log("[feed] texture-space portraits: no typed UAV load for %s; enhanced portraits are not used", FormatName(out_fmt));
        g_failed = true;
        return false;
    }
    if (g_pso == nullptr)
    {
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
        auto serialize = d12 != nullptr ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d12, "D3D12SerializeRootSignature")) : nullptr;
        ID3DBlob *cs = nullptr, *err = nullptr, *sig = nullptr;
        HRESULT hr = compile != nullptr && serialize != nullptr ?
            compile(kFinishHlsl, sizeof(kFinishHlsl) - 1, "portrait_finish", nullptr, nullptr, "main", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err) : E_FAIL;
        SafeRelease(err);
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2; params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 2;
        D3D12_ROOT_SIGNATURE_DESC rsd = {}; rsd.NumParameters = 2; rsd.pParameters = params;
        if (SUCCEEDED(hr)) hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        SafeRelease(err);
        if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                                         reinterpret_cast<void **>(&g_rs));
        SafeRelease(sig);
        if (SUCCEEDED(hr))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = g_rs; pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
            hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g_pso));
        }
        SafeRelease(cs);
        if (dc != nullptr) FreeLibrary(dc);
        if (SUCCEEDED(hr) && g_heap == nullptr)
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_heap));
        }
        if (FAILED(hr)) { Log("[feed] texture-space portraits: finish pass setup failed 0x%08X", hr); Release(); g_failed = true; return false; }
    }
    // Rebinding is only legal once every frame slot that used the heap has retired: callers rebind
    // on a feature rebuild, after the transport has waited for the device.
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1; sv.Format = src_fmt;
    dev->CreateShaderResourceView(src, &sv, h);
    h.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = out_fmt;
    dev->CreateUnorderedAccessView(out, nullptr, &uv, h);
    g_bound_src = src; g_bound_out = out;
    Log("[feed] texture-space portraits: finish pass ready (%s -> %s, in place)", FormatName(src_fmt), FormatName(out_fmt));
    return true;
}

// Precondition: src in NON_PIXEL_SHADER_RESOURCE, out in UNORDERED_ACCESS (as the evaluate leaves them).
static void Record(ID3D12GraphicsCommandList *list, ID3D12Resource *out, UINT w, UINT h)
{
    D3D12_RESOURCE_BARRIER uav = {}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = out;
    list->ResourceBarrier(1, &uav);
    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_rs);
    list->SetPipelineState(g_pso);
    list->SetComputeRootDescriptorTable(0, g_heap->GetGPUDescriptorHandleForHeapStart());
    const UINT size[2] = { w, h };
    list->SetComputeRoot32BitConstants(1, 2, size, 0);
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    list->ResourceBarrier(1, &uav);
}
}  // namespace ptex
#endif
