// Frontier 3 texture-space portraits: cell planning through the real allocator (stable slots, a
// re-rendered texture inheriting its slot without a history reset), and the D3D12 finish pass
// against an exact CPU oracle on WARP and every hardware adapter.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using Microsoft::WRL::ComPtr;
template <class T> static void SafeRelease(T *&p) { if (p != nullptr) { p->Release(); p = nullptr; } }
static void Log(const char *fmt, ...) { va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a); std::puts(""); }
static const char *FormatName(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8_UNORM" : "?"; }
#include "../src/feed_portrait_tex.h"

static void Require(bool ok, const char *what) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); } }
static void Check(HRESULT hr, const char *what) { if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s (0x%08lX)\n", what, hr); std::exit(1); } }

static portrait::Atlas Plan(std::vector<ptex::Slot> &slots, const std::vector<ptex::Tex> &tex, const portrait::Atlas *previous)
{
    ptex::AssignCells(slots, tex);
    portrait::Atlas atlas;
    Require(portrait::Allocate(ptex::Candidates(slots), ptex::kSpace, ptex::kSpace, 400000, previous, atlas), "texture atlas allocates");
    for (const auto &it : atlas.items) Require(ptex::SlotOf(slots, it) != nullptr, "every atlas item maps back to its texture");
    return atlas;
}

static void PlannerTests()
{
    // The map scene: the animated ruler portrait (473x473) and one 90x112 head.
    std::vector<ptex::Slot> slots;
    std::vector<ptex::Tex> frame = { { 0xA1, 1, 473, 473, 0, 0, 0 }, { 0xB1, 2, 90, 112, 1, 0, 0 } };
    portrait::Atlas a = Plan(slots, frame, nullptr);
    Require(a.items.size() == 2, "both portraits are placed");
    portrait::Atlas b = Plan(slots, frame, &a);
    Require(portrait::SameLayout(a, b) && portrait::SameHistory(a, b), "an unchanged frame keeps the layout and history");

    // The ruler portrait is re-rendered into a new texture: same draw position and size.
    std::vector<ptex::Tex> swapped = { { 0xA2, 7, 473, 473, 0, 0, 0 }, { 0xB1, 2, 90, 112, 1, 0, 0 } };
    portrait::Atlas c = Plan(slots, swapped, &b);
    Require(portrait::SameHistory(b, c), "a re-rendered portrait inherits its slot: no history reset");
    Require(slots.size() == 2 && std::any_of(slots.begin(), slots.end(), [](const ptex::Slot &s) { return s.tex.res == 0xA2; }) &&
            std::none_of(slots.begin(), slots.end(), [](const ptex::Slot &s) { return s.tex.res == 0xA1; }), "the slot now names the new texture");

    // Hover zoom, as logged live: a fresh texture of a new size every frame, 455..473 and back.
    // One slot (473 + 5% headroom) holds them all: layout survives; each size change asks for a reset.
    {
        portrait::Atlas prev = c;
        uint64_t res = 0xD000;
        for (uint32_t size : { 455u, 457u, 462u, 466u, 470u, 473u, 471u, 466u, 457u, 455u })
        {
            std::vector<ptex::Tex> zoom = { { ++res, 1, size, size, 0, 0, 0 }, { 0xB1, 2, 90, 112, 1, 0, 0 } };
            const std::vector<ptex::Slot> before = slots;
            const bool resized = ptex::AssignCells(slots, zoom);
            portrait::Atlas z;
            Require(portrait::Allocate(ptex::Candidates(slots), ptex::kSpace, ptex::kSpace, 400000, &prev, z), "zoom frame allocates");
            Require(portrait::SameLayout(prev, z) && portrait::SameHistory(prev, z), "hover zoom keeps the atlas layout (no feature rebuild)");
            const uint32_t old = before[0].tex.res == 0xB1 ? before[1].tex.w : before[0].tex.w;
            Require(resized == (old != size), "a size change (and only that) requests a history reset");
            prev = z;
        }
        c = prev;
    }

    // A replacement of a clearly different size (beyond ptex::Similar) is a new portrait, not an inheritance.
    std::vector<ptex::Tex> other = { { 0xA3, 9, 380, 421, 0, 0, 0 }, { 0xB1, 2, 90, 112, 1, 0, 0 } };
    portrait::Atlas d = Plan(slots, other, &c);
    Require(!portrait::SameHistory(c, d), "a different portrait resets history");

    // A council opens: twelve 80x100 heads join; surviving portraits keep their atlas address.
    std::vector<ptex::Tex> council = other;
    for (uint32_t i = 0; i < 12; ++i) council.push_back({ 0x100 + i, 1, 80, 100, 2 + i, 0, 0 });
    portrait::Atlas e = Plan(slots, council, &d);
    Require(e.items.size() == 14, "all fourteen textures fit the budget");
    std::vector<int> cells;
    for (const auto &s : slots) cells.push_back(s.cell);
    std::sort(cells.begin(), cells.end());
    Require(std::adjacent_find(cells.begin(), cells.end()) == cells.end(), "cells are unique");

    // Oversized textures (the 400x1400 load-screen portraits) are left to the game.
    std::vector<ptex::Slot> s2;
    ptex::AssignCells(s2, { { 0xC1, 1, 400, 1400, 0, 0, 0 }, { 0xC2, 1, 240, 240, 1, 0, 0 } });
    Require(s2.size() == 1 && s2[0].tex.res == 0xC2, "a texture larger than a cell is not planned");

    // Randomized: identity is kept, cells stay unique, every texture that fits a cell gets one.
    std::mt19937 rng(31);
    std::vector<ptex::Slot> live;
    std::vector<ptex::Tex> pool;
    for (int trial = 0; trial < 3000; ++trial)
    {
        if (pool.size() < 30 && (rng() % 3) == 0)
            pool.push_back({ 0x1000u + trial, uint64_t(trial), 16 + rng() % 1100, 16 + rng() % 1100, uint32_t(rng() % 40), 0, 0 });
        if (!pool.empty() && (rng() % 4) == 0) pool.erase(pool.begin() + rng() % pool.size());
        const std::vector<ptex::Slot> before = live;
        ptex::AssignCells(live, pool);
        size_t fitting = 0;
        for (const auto &t : pool) fitting += t.w <= 1024 && t.h <= 1024;
        Require(live.size() == fitting, "every fitting texture has a cell");
        std::vector<int> used;
        for (const auto &s : live)
        {
            used.push_back(s.cell);
            for (const auto &o : before)
                if (o.tex.res == s.tex.res && o.tex.gen == s.tex.gen) Require(o.cell == s.cell, "a known texture keeps its cell");
        }
        std::sort(used.begin(), used.end());
        Require(std::adjacent_find(used.begin(), used.end()) == used.end(), "cells stay unique");
    }
    std::puts("PASS planner: stable slots, swap inheritance without reset, new portraits reset, council growth, oversize, 3000 random frames");
}

struct Device
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    explicit Device(IUnknown *adapter)
    {
        Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "create device");
        D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        Check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
        Check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        Check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    }
    void Submit()
    {
        Check(list->Close(), "close");
        ID3D12CommandList *lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++value), "signal");
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(fence->SetEventOnCompletion(value, e), "wait");
        WaitForSingleObject(e, INFINITE); CloseHandle(e);
        Check(dev->GetDeviceRemovedReason(), "device completed the work");
        Check(alloc->Reset(), "reset allocator");
        Check(list->Reset(alloc.Get(), nullptr), "reset list");
    }
};

static void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r;
    x.Transition.StateBefore = a; x.Transition.StateAfter = b; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &x);
}

static void FinishTest(Device &d)
{
    constexpr UINT w = 203, h = 117;   // partial thread groups on both axes
    const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!ptex::Supports(d.dev.Get(), fmt)) { std::puts("SKIP finish pass: no typed UAV load of R8G8B8A8_UNORM on this adapter"); return; }
    std::mt19937 rng(7);
    std::vector<uint8_t> src(size_t(w) * h * 4), nr(src.size());
    for (size_t i = 0; i < size_t(w) * h; ++i)
    {
        const uint8_t a = (rng() % 4 == 0) ? 0 : (rng() % 3 == 0) ? 255 : uint8_t(rng() % 256);
        for (int c = 0; c < 3; ++c) src[i * 4 + c] = a ? uint8_t(rng() % (a + 1)) : uint8_t(rng() % 7);   // premultiplied, a few stray texels
        src[i * 4 + 3] = a;
        for (int c = 0; c < 4; ++c) nr[i * 4 + c] = uint8_t(rng() % 256);
    }
    D3D12_HEAP_PROPERTIES def = {}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> color, output;
    Check(d.dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&color)), "color");
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Check(d.dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&output)), "output");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 plane = 0;
    d.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &plane);
    D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = plane * 2; bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload, readback;
    Check(d.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload");
    bd.Width = plane;
    Check(d.dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
    uint8_t *p = nullptr; Check(upload->Map(0, nullptr, reinterpret_cast<void **>(&p)), "map upload");
    for (UINT y = 0; y < h; ++y)
    {
        std::memcpy(p + y * fp.Footprint.RowPitch, src.data() + size_t(y) * w * 4, w * 4);
        std::memcpy(p + plane + y * fp.Footprint.RowPitch, nr.data() + size_t(y) * w * 4, w * 4);
    }
    upload->Unmap(0, nullptr);
    auto copy_in = [&](ID3D12Resource *dst, UINT64 offset) {
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = fp; from.PlacedFootprint.Offset = offset;
        to.pResource = dst; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    };
    copy_in(color.Get(), 0); copy_in(output.Get(), plane);
    Transition(d.list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(d.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Require(ptex::EnsureResources(d.dev.Get(), color.Get(), output.Get(), fmt, fmt), "finish pass resources");
    ptex::Record(d.list.Get(), output.Get(), w, h);
    Transition(d.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = output.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    d.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    d.Submit();
    Check(readback->Map(0, nullptr, reinterpret_cast<void **>(&p)), "map readback");
    size_t transparent = 0, clamped = 0;
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
    {
        const uint8_t *s = src.data() + (size_t(y) * w + x) * 4, *n = nr.data() + (size_t(y) * w + x) * 4;
        const uint8_t *o = p + y * fp.Footprint.RowPitch + x * 4;
        uint8_t want[4];
        if (s[3] == 0) { std::memcpy(want, s, 4); ++transparent; }
        else
        {
            for (int c = 0; c < 3; ++c) { want[c] = std::min(n[c], s[3]); clamped += n[c] > s[3]; }
            want[3] = s[3];
        }
        if (std::memcmp(o, want, 4) != 0)
        {
            std::fprintf(stderr, "pixel %u,%u: got %u,%u,%u,%u want %u,%u,%u,%u\n", x, y, o[0], o[1], o[2], o[3], want[0], want[1], want[2], want[3]);
            Require(false, "finish pass: original alpha, NR colour clamped to it, transparent texels untouched");
        }
    }
    readback->Unmap(0, nullptr);
    Require(transparent > 1000 && clamped > 1000, "the fixture exercises transparent and clamped texels");
    ptex::Release();
    std::printf("PASS finish pass: %ux%u exact (%zu transparent texels kept, %zu colour channels clamped to alpha)\n", w, h, transparent, clamped);
}

int main()
{
    PlannerTests();
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> warp; Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    { std::puts("Testing WARP"); Device d(warp.Get()); FinishTest(d); }
    for (UINT i = 0; ; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc; Check(adapter->GetDesc1(&desc), "adapter");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        std::printf("Testing GPU: %ls\n", desc.Description);
        Device d(adapter.Get()); FinishTest(d);
    }
    std::puts("PASS portrait textures: planner and finish pass");
}
