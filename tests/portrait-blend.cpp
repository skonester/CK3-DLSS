// Execute the shipped blend on WARP and the local GPU, with an independent CPU oracle.
// The Frontier 1 shader is retained only as a timestamp benchmark reference.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>

using Microsoft::WRL::ComPtr;
struct Feed { static constexpr int kFrames = 3; };
template<class T> static void SafeRelease(T *&p) { if (p) p->Release(); p = nullptr; }
static void Log(const char *fmt, ...)
{
    va_list args; va_start(args, fmt); std::vprintf(fmt, args); va_end(args); std::puts("");
}
static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}
static ID3D12GraphicsCommandList *g_test_list;
static void Barrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{ Barrier(g_test_list, resource, before, after); }
#include "../src/feed_portrait_blend.h"
#include "../src/feed_portrait_atlas.h"

static void Require(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void Check(HRESULT hr, const char *message)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s (0x%08lX)\n", message, hr); std::exit(1); }
}

struct Device
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> messages;
    HANDLE event = nullptr;
    UINT64 serial = 0;

    explicit Device(IDXGIAdapter *adapter)
    {
        Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "create D3D12 device");
        D3D12_COMMAND_QUEUE_DESC qd = {};
        Check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "create queue");
        Check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "create allocator");
        Check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "create list");
        Check(list->Close(), "close initial list");
        Check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(event != nullptr, "create fence event");
        dev.As(&messages);
    }
    ~Device() { CloseHandle(event); }
    void Begin()
    {
        Check(allocator->Reset(), "reset allocator");
        Check(list->Reset(allocator.Get(), nullptr), "reset list");
        g_test_list = list.Get();
    }
    void Submit()
    {
        Check(list->Close(), "close list");
        ID3D12CommandList *lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++serial), "signal fence");
        Check(fence->SetEventOnCompletion(serial, event), "arm fence");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU completes in ten seconds");
        Check(dev->GetDeviceRemovedReason(), "device remains usable");
    }
    void CheckMessages()
    {
        if (!messages) return;
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            Check(messages->GetMessage(i, nullptr, &size), "size debug message");
            std::vector<uint8_t> storage(size);
            auto *m = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            Check(messages->GetMessage(i, m, &size), "read debug message");
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                std::fprintf(stderr, "%s\n", m->pDescription);
                Require(false, "D3D12 debug validation");
            }
        }
    }
};

static ComPtr<ID3D12Resource> Buffer(Device &d, UINT64 bytes, D3D12_HEAP_TYPE heap)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1;
    rd.DepthOrArraySize = rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Check(d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)), "create transfer buffer");
    return result;
}

static UINT PixelBytes(DXGI_FORMAT format) { return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4; }
static void Encode(uint8_t *dest, const float values[4], DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        for (int c = 0; c < 4; ++c)
        {
            const uint16_t half = DirectX::PackedVector::XMConvertFloatToHalf(values[c]);
            std::memcpy(dest + c * 2, &half, 2);
        }
    }
    else if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        uint32_t packed = 0;
        for (int c = 0; c < 4; ++c)
        {
            const uint32_t scale = c == 3 ? 3 : 1023;
            packed |= uint32_t(std::lround(values[c] * scale)) << (c * 10);
        }
        std::memcpy(dest, &packed, 4);
    }
    else for (int c = 0; c < 4; ++c) dest[c] = uint8_t(std::lround(values[c] * 255));
}
static void Decode(const uint8_t *source, float values[4], DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        for (int c = 0; c < 4; ++c)
        {
            uint16_t half; std::memcpy(&half, source + c * 2, 2);
            values[c] = DirectX::PackedVector::XMConvertHalfToFloat(half);
        }
    }
    else if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        uint32_t packed; std::memcpy(&packed, source, 4);
        for (int c = 0; c < 4; ++c)
        {
            const uint32_t scale = c == 3 ? 3 : 1023;
            values[c] = float((packed >> (c * 10)) & scale) / scale;
        }
    }
    else for (int c = 0; c < 4; ++c) values[c] = float(source[c]) / 255;
}

// Evaluate distance to each edge independently of the GPU scheduler and its jobs.
// `coverage` (optional, one byte per atlas pixel) applies to blocks flagged in coverage_blocks.
static float Weight(const pblend::Cb &cb, UINT x, UINT y, const std::vector<uint8_t> *coverage = nullptr, UINT stride = 0)
{
    for (uint32_t i = 0; i < cb.count; ++i)
    {
        const auto &b = cb.blocks[i];
        if (x < b.x || y < b.y || x - b.x >= b.w || y - b.y >= b.h) continue;
        const UINT distance[] = { x - b.x, y - b.y, b.w - 1 - (x - b.x), b.h - 1 - (y - b.y) };
        const UINT feather[] = { b.feather_l, b.feather_t, b.feather_r, b.feather_b };
        float weight = 1;
        for (int side = 0; side < 4; ++side)
            if (feather[side]) weight = (std::min)(weight, (float(distance[side]) + .5f) / feather[side]);
        if (coverage != nullptr && ((cb.coverage_blocks >> i) & 1u))
            weight *= float((*coverage)[size_t(y) * stride + x]) / 255;
        return weight;
    }
    return 1;   // padding has no consumer and must retain the neural result
}

// Character coverage fixture: opaque centres, transparent corners and an interior hole,
// a soft ramp, and every byte value somewhere, so zero/full/partial are all exercised.
static uint8_t CoverageAt(UINT x, UINT y)
{
    if ((x / 5 + y / 3) % 11 == 0) return 0;                  // interior holes
    if ((x + 2 * y) % 17 == 0) return uint8_t((x * 31 + y * 7) % 256);   // all byte values
    const int band = int((x + y) % 64);
    return band < 20 ? 255 : band < 30 ? 0 : uint8_t(band * 4);
}

struct Image
{
    Device &d;
    UINT w, h, bytes;
    DXGI_FORMAT format;
    ComPtr<ID3D12Resource> color, output, upload, readback, cover, cover_upload;
    std::vector<uint8_t> coverage;   // one byte per pixel, tightly packed
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 plane_size = 0, neural_offset = 0;
    std::vector<uint8_t> originals;

    Image(Device &device, UINT width, UINT height, DXGI_FORMAT fmt, int coverage_mode = 0)
        : d(device), w(width), h(height), bytes(PixelBytes(fmt)), format(fmt)
    {
        // coverage_mode: 0 none bound, 1 fixture pattern, 2 entirely empty mask
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h;
        rd.DepthOrArraySize = rd.MipLevels = 1; rd.Format = format; rd.SampleDesc.Count = 1;
        // Match MakeSharedPair: cross-API images enter and leave D3D12 in COMMON.
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        Check(d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color)), "create shared color texture");
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output)), "create shared neural texture");
        d.dev->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, nullptr, nullptr, &plane_size);
        neural_offset = (plane_size + 511) & ~UINT64(511);
        originals.resize(size_t(neural_offset + plane_size));
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
        {
            float src[4], nr[4];
            for (UINT c = 0; c < 4; ++c)
            {
                src[c] = float((x * 13 + y * 29 + c * 53) % 251) / 250;
                nr[c] = float((x * 7 + y * 11 + c * 31 + 43) % 241) / 240;
                if (format == DXGI_FORMAT_R16G16B16A16_FLOAT && c != 3)
                {
                    src[c] = (src[c] - .25f) * 4;
                    nr[c] = (nr[c] - .35f) * 7;
                }
            }
            const size_t offset = size_t(y) * footprint.Footprint.RowPitch + x * bytes;
            Encode(originals.data() + offset, src, format);
            Encode(originals.data() + neural_offset + offset, nr, format);
        }
        upload = Buffer(d, originals.size(), D3D12_HEAP_TYPE_UPLOAD);
        readback = Buffer(d, plane_size, D3D12_HEAP_TYPE_READBACK);
        uint8_t *ptr; const D3D12_RANGE no_read = { 0, 0 };
        Check(upload->Map(0, &no_read, reinterpret_cast<void **>(&ptr)), "map upload");
        std::memcpy(ptr, originals.data(), originals.size()); upload->Unmap(0, nullptr);
        d.Begin();
        Barrier(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        Copy(color.Get(), 0); Copy(output.Get(), neural_offset);
        Barrier(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        d.Submit();
        if (coverage_mode != 0) MakeCoverage(coverage_mode);
        Require(pblend::EnsureResources(d.dev.Get(), color.Get(), output.Get(), w, h, format, format, cover.Get()),
                "prepare production blend");
        if (pblend::g_in_place) Require(pblend::g_nr_copy == nullptr, "typed path allocates no scratch atlas");
    }
    ~Image() { pblend::ReleaseTargets(); }
    // The coverage atlas is created like the other shared atlas textures (enters D3D12 in COMMON).
    void MakeCoverage(int mode)
    {
        coverage.resize(size_t(w) * h);
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) coverage[size_t(y) * w + x] = mode == 1 ? CoverageAt(x, y) : 0;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h;
        rd.DepthOrArraySize = rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8_UNORM; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        Check(d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&cover)), "create shared coverage texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 size = 0;
        d.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &size);
        cover_upload = Buffer(d, size, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t *ptr; const D3D12_RANGE no_read = { 0, 0 };
        Check(cover_upload->Map(0, &no_read, reinterpret_cast<void **>(&ptr)), "map coverage upload");
        for (UINT y = 0; y < h; ++y) std::memcpy(ptr + size_t(y) * fp.Footprint.RowPitch, coverage.data() + size_t(y) * w, w);
        cover_upload->Unmap(0, nullptr);
        d.Begin();
        Barrier(cover.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = cover_upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = fp;
        to.pResource = cover.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Barrier(cover.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        d.Submit();
    }
    void Copy(ID3D12Resource *dest, UINT64 offset)
    {
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprint; from.PlacedFootprint.Offset = offset;
        to.pResource = dest; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    void Reset()
    {
        Barrier(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (cover) Barrier(cover.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        Copy(output.Get(), neural_offset);
        Barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // Exercise the compatibility path by selecting its private pipeline in this test process.
    void CopyPath()
    {
        Require(pblend::InitPipeline(d.dev.Get(), false), "compile fallback shader");
        if (!pblend::g_nr_copy)
        {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            auto rd = output->GetDesc(); rd.Flags = D3D12_RESOURCE_FLAG_NONE;
            Check(d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pblend::g_nr_copy)), "create fallback scratch");
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = format;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.dev->CreateShaderResourceView(pblend::g_nr_copy, &sv, pblend::g_heap->GetCPUDescriptorHandleForHeapStart());
        }
        pblend::g_in_place = false;
    }
    void Verify(const pblend::Cb &cb)
    {
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = output.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        Barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (cover) Barrier(cover.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        d.Submit();
        uint8_t *ptr; const D3D12_RANGE range = { 0, size_t(plane_size) };
        Check(readback->Map(0, &range, reinterpret_cast<void **>(&ptr)), "map result");
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
        {
            const size_t offset = size_t(y) * footprint.Footprint.RowPitch + x * bytes;
            // Without a bound coverage resource the production pass must ignore coverage flags.
            const float weight = Weight(cb, x, y, cover ? &coverage : nullptr, w);
            if (weight >= 1)
                Require(std::memcmp(ptr + offset, originals.data() + neural_offset + offset, bytes) == 0,
                        "interiors, full coverage and atlas padding remain bit-exact neural");
            else if (weight <= 0)
                Require(std::memcmp(ptr + offset, originals.data() + offset, bytes) == 0,
                        "zero coverage restores the original pixel bit-exactly");
            else
            {
                float source[4], neural[4], actual[4];
                Decode(originals.data() + offset, source, format);
                Decode(originals.data() + neural_offset + offset, neural, format);
                Decode(ptr + offset, actual, format);
                for (int c = 0; c < 4; ++c)
                {
                    const float expected = source[c] + (neural[c] - source[c]) * weight;
                    const float tolerance = format == DXGI_FORMAT_R16G16B16A16_FLOAT ?
                        (std::max)(1.f / 16777216, std::abs(expected) / 1024) :
                        format == DXGI_FORMAT_R10G10B10A2_UNORM ? (c == 3 ? 1.f / 3 : 1.f / 1023) : 1.f / 255;
                    if (std::abs(actual[c] - expected) > tolerance + .000001f)
                    {
                        std::fprintf(stderr, "pixel %u,%u component %d: expected %.8f got %.8f\n", x, y, c, expected, actual[c]);
                        Require(false, "GPU feather matches CPU oracle within one storage step");
                    }
                }
            }
        }
        const D3D12_RANGE no_write = { 0, 0 }; readback->Unmap(0, &no_write);
    }
};

static pblend::Cb Council()
{
    pblend::Cb cb = {}; cb.blocks[cb.count++] = { 8, 8, 496, 456, 0, 16, 16, 16 };
    for (UINT y = 8; y < 456; y += 112) cb.blocks[cb.count++] = { 512, y, 104, 104, 16, 16, 16, 16 };
    for (UINT x = 8; x < 560; x += 112) cb.blocks[cb.count++] = { x, 472, 104, 104, 16, 16, 16, 16 };
    return cb;
}

static void SchedulerTests()
{
    uint32_t random = 0xA9A25177;
    auto next = [&]() { random = random * 1664525 + 1013904223; return random; };
    for (int trial = 0; trial < 128; ++trial)
    {
        constexpr UINT w = 137, h = 93;
        pblend::Cb cb = {}; cb.count = next() % 25;
        cb.coverage_blocks = trial % 3 == 0 ? 0u : next();   // Frontier 3: mixed coverage and legacy blocks
        for (UINT i = 0; i < cb.count; ++i)
            cb.blocks[i] = { (i % 8) * 17, (i / 8) * 31, 1 + next() % 16, 1 + next() % 30,
                             next() % 65, next() % 65, next() % 65, next() % 65 };
        pblend::DispatchCb plan;
        Require(pblend::BuildDispatch(cb, w, h, plan), "valid randomized scheduler input");
        std::vector<UINT> visits(w * h);
        UINT end = 0;
        for (UINT i = 0; i < plan.job_count; ++i)
        {
            const auto &job = plan.jobs[i];
            Require(job.group_end > end && job.block < cb.count, "monotonic group tickets");
            const auto &owner = cb.blocks[job.block];
            const bool whole = ((cb.coverage_blocks >> job.block) & 1u) != 0;
            Require(job.flags == (whole ? 1u : 0u), "coverage flag follows the block");
            Require(!whole || (job.x == owner.x && job.y == owner.y && job.w == owner.w && job.h == owner.h),
                    "a coverage block is one whole-block job");
            const UINT groups = job.groups_x * ((job.h - 1) / pblend::kGroupHeight + 1);
            Require(job.group_end - end == groups, "ticket count covers every edge tile");
            end = job.group_end;
            for (UINT y = job.y; y < job.y + job.h; ++y) for (UINT x = job.x; x < job.x + job.w; ++x)
            { Require(x < w && y < h, "jobs remain within atlas"); ++visits[y * w + x]; }
        }
        Require(end == plan.group_count, "final ticket matches dispatch count");
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x)
        {
            UINT expected = Weight(cb, x, y) < 1 ? 1u : 0u;
            for (UINT i = 0; i < cb.count; ++i)
            {
                const auto &b = cb.blocks[i];
                if (((cb.coverage_blocks >> i) & 1u) && x >= b.x && y >= b.y && x - b.x < b.w && y - b.y < b.h) expected = 1;
            }
            Require(visits[y * w + x] == expected, "each feather or coverage pixel has exactly one owner");
        }
    }
    pblend::Cb cb = {}; pblend::DispatchCb plan;
    cb.count = 1; cb.blocks[0] = { 0, 0, 16384, 16384, 16384, 16384, 16384, 16384 };
    Require(pblend::BuildDispatch(cb, 16384, 16384, plan) && plan.group_count == 2097152 &&
            plan.dispatch_width == 65535, "large jobs spill into a legal two-dimensional dispatch");
    cb.count = 2; cb.blocks[1] = cb.blocks[0];
    Require(!pblend::BuildDispatch(cb, 16384, 16384, plan), "overlap rejected before in-place writes");
    cb.count = 1; cb.blocks[0].x = 1;
    Require(!pblend::BuildDispatch(cb, 16384, 16384, plan), "out-of-bounds block rejected");
    cb.count = 25;
    Require(!pblend::BuildDispatch(cb, 16384, 16384, plan), "excess block count rejected");
    cb.count = 0;
    Require(pblend::BuildDispatch(cb, 137, 93, plan) && plan.group_count == 0, "empty atlas has no dispatch");
    std::puts("PASS scheduler: 128 randomized layouts with mixed coverage blocks, unique pixel ownership, bounds, large dispatch");
}

static const char kReference[] = R"(
struct Block { uint4 rect; uint4 feather; };
cbuffer Blocks : register(b0) { uint count; uint3 pad; Block blocks[24]; };
Texture2D<float4> nr : register(t0);
Texture2D<float4> src : register(t1);
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

static void RecordReference(Image &image, ID3D12PipelineState *pso, const pblend::Cb &cb)
{
    auto *list = image.d.list.Get();
    std::memcpy(pblend::g_cb_ptr, &cb, sizeof(cb));
    D3D12_RESOURCE_BARRIER uav = {}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = image.output.Get();
    list->ResourceBarrier(1, &uav);
    Barrier(image.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(pblend::g_nr_copy, image.output.Get());
    Barrier(image.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(pblend::g_nr_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap *heaps[] = { pblend::g_heap }; list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(pblend::g_rs); list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, pblend::g_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootConstantBufferView(1, pblend::g_cb->GetGPUVirtualAddress());
    list->Dispatch((image.w + 7) / 8, (image.h + 7) / 8, 1);
    list->ResourceBarrier(1, &uav);
    Barrier(pblend::g_nr_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}

static void Benchmark(Device &d, DXGI_FORMAT format)
{
    Image image(d, 640, 640, format, 1); const auto cb = Council();   // coverage bound; used only by mode 3
    auto covered = cb; covered.coverage_blocks = (1u << cb.count) - 1;
    Require(pblend::SupportsInPlace(d.dev.Get(), format), "hardware benchmark supports typed loads");
    image.CopyPath();
    ComPtr<ID3DBlob> code, error;
    const HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    Require(compile != nullptr, "load reference compiler");
    Check(compile(kReference, sizeof(kReference) - 1, "frontier1_reference", nullptr, nullptr, "main", "cs_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error), "compile Frontier 1 reference");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = pblend::g_rs;
    pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    ComPtr<ID3D12PipelineState> reference;
    Check(d.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&reference)), "create reference pipeline");
    code.Reset(); error.Reset(); FreeLibrary(compiler);
    D3D12_QUERY_HEAP_DESC qd = {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2;
    ComPtr<ID3D12QueryHeap> query;
    Check(d.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&query)), "create timestamp heap");
    auto timestamps = Buffer(d, 16, D3D12_HEAP_TYPE_READBACK);
    UINT64 frequency; Check(d.queue->GetTimestampFrequency(&frequency), "timestamp frequency");
    auto run = [&](int mode, UINT count) {
        d.Begin(); image.Reset(); pblend::g_in_place = mode >= 2;
        d.list->EndQuery(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (UINT i = 0; i < count; ++i)
        {
            if (mode == 0) RecordReference(image, reference.Get(), cb);
            else pblend::Record(d.list.Get(), image.output.Get(), 0, image.w, image.h, mode == 3 ? covered : cb);
        }
        d.list->EndQuery(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        d.list->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestamps.Get(), 0);
        Barrier(image.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(image.color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(image.cover.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        d.Submit();
        UINT64 *ticks; const D3D12_RANGE range = { 0, 16 };
        Check(timestamps->Map(0, &range, reinterpret_cast<void **>(&ticks)), "read GPU timestamps");
        const double ms = double(ticks[1] - ticks[0]) * 1000 / double(frequency) / count;
        const D3D12_RANGE no_write = { 0, 0 }; timestamps->Unmap(0, &no_write);
        return ms;
    };
    for (int mode = 0; mode < 4; ++mode) run(mode, 16);
    std::array<std::array<double, 5>, 4> samples;
    for (int trial = 0; trial < 5; ++trial)
        for (int step = 0; step < 4; ++step)
        { const int mode = (step + trial) % 4; samples[mode][trial] = run(mode, 128); }
    for (auto &sample : samples) std::sort(sample.begin(), sample.end());
    pblend::DispatchCb plan; Require(pblend::BuildDispatch(cb, image.w, image.h, plan), "benchmark dispatch");
    UINT pixels = 0; for (UINT i = 0; i < plan.job_count; ++i) pixels += plan.jobs[i].w * plan.jobs[i].h;
    pblend::DispatchCb whole; Require(pblend::BuildDispatch(covered, image.w, image.h, whole), "coverage benchmark dispatch");
    UINT covered_pixels = 0; for (UINT i = 0; i < whole.job_count; ++i) covered_pixels += whole.jobs[i].w * whole.jobs[i].h;
    std::printf("BENCH format=%d atlas=640x640 blocks=%u edge_pixels=%u groups=%u median_ms: "
                "frontier1=%.6f copy_edges=%.6f in_place_edges=%.6f speedup=%.2fx\n",
                int(format), cb.count, pixels, plan.group_count, samples[0][2], samples[1][2], samples[2][2],
                samples[0][2] / samples[2][2]);
    std::printf("BENCH format=%d coverage (Frontier 3): whole-block pixels=%u groups=%u in_place_coverage_ms=%.6f "
                "(%.2fx the edge-only pass); extra memory: one R8 atlas, %u bytes at this size\n",
                int(format), covered_pixels, whole.group_count, samples[3][2], samples[3][2] / samples[2][2], image.w * image.h);
}

static void GpuTests(Device &d, bool benchmark, bool hardware = false)
{
    const DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
    for (auto format : formats)
    {
        // 6-9: Frontier 3 coverage. 6 council, all coverage; 7 tiny blocks, alternate coverage
        // and legacy (a legacy block never reads the mask under it); 8 coverage flags with no
        // coverage bound (must equal legacy); 9 entirely empty mask (no neural rectangle).
        for (int scenario = 0; scenario < 10; ++scenario)
        {
            UINT w = (scenario == 0 || scenario == 6 || scenario == 9) ? 640 : 137;
            UINT h = (scenario == 0 || scenario == 6 || scenario == 9) ? 640 : 93;
            pblend::Cb cb = {};
            int coverage_mode = 0;
            if (scenario == 0 || scenario == 6 || scenario == 9) cb = Council();
            if (scenario == 6 || scenario == 9) { cb.coverage_blocks = (1u << cb.count) - 1; coverage_mode = scenario == 6 ? 1 : 2; }
            if (scenario == 7 || scenario == 8)
            {
                cb.count = 24;
                for (UINT i = 0; i < cb.count; ++i)
                    cb.blocks[i] = { (i % 8) * 17, (i / 8) * 31, 1 + (i * 7) % 16, 1 + (i * 13) % 30,
                                     i % 3 ? 64u : 0u, i % 5 ? 3u : 0u, i % 7 ? 16u : 0u, i % 2 ? 0u : 64u };
                cb.coverage_blocks = scenario == 7 ? 0x00AAAAAAu : 0x00FFFFFFu;
                coverage_mode = scenario == 7 ? 1 : 0;
            }
            else if (scenario == 1)
            {
                cb.count = 24;
                for (UINT i = 0; i < cb.count; ++i)
                    cb.blocks[i] = { (i % 8) * 17, (i / 8) * 31, 1 + (i * 7) % 16, 1 + (i * 13) % 30,
                                     i % 3 ? 64u : 0u, i % 5 ? 3u : 0u, i % 7 ? 16u : 0u, i % 2 ? 0u : 64u };
            }
            else if (scenario == 2) { cb.count = 1; cb.blocks[0] = { 0, 0, w, h, 0, 0, 0, 0 }; }
            else if (scenario == 4 || scenario == 5)
            {
                std::vector<portrait::Rect> rects = {{0,0,380,421}};
                for (int i = 0; i < 12; ++i) rects.push_back({600 + (i % 6) * 160, 100 + (i / 6) * 180, 80, 100});
                portrait::Atlas atlas;
                Require(portrait::Build(rects, 1920, 1080, {48,16,400000}, nullptr, atlas), "GPU allocator fixture builds");
                if (scenario == 5)
                {
                    auto previous = atlas;
                    std::vector<portrait::Candidate> survivors;
                    for (size_t i = 1; i < previous.items.size(); ++i)
                    {
                        const auto &it = previous.items[i];
                        const portrait::Rect crop = {it.sx,it.sy,it.w,it.h};
                        survivors.push_back({crop,crop});
                    }
                    Require(portrait::Allocate(survivors, 1920, 1080, 400000, &previous, atlas) &&
                            portrait::SameHistory(previous, atlas), "GPU fixture retains sparse survivor addresses");
                }
                w = atlas.w; h = atlas.h;
                for (const auto &it : atlas.items)
                    cb.blocks[cb.count++] = { UINT(it.dx), UINT(it.dy), UINT(it.w), UINT(it.h),
                        (it.edges & 1) ? 0u : 16u, (it.edges & 2) ? 0u : 16u,
                        (it.edges & 4) ? 0u : 16u, (it.edges & 8) ? 0u : 16u };
            }
            Image image(d, w, h, format, coverage_mode);
            const bool typed = pblend::g_in_place;
            for (int mode = 0; mode < (typed ? 2 : 1); ++mode)
            {
                if (mode || !typed) image.CopyPath();
                for (int slot = 0; slot < Feed::kFrames; ++slot)
                {
                    d.Begin(); image.Reset();
                    pblend::Record(d.list.Get(), image.output.Get(), slot, w, h, cb);
                    image.Verify(cb);
                }
            }
        }
        std::printf("PASS GPU format=%d: council, 24 tiny blocks, zero feathers, empty, allocated and retired slots; "
                    "coverage: full/partial/zero/holes, mixed legacy blocks, unbound fallback, empty mask; three frame slots; typed=%d and copy fallback\n",
                    int(format), pblend::SupportsInPlace(d.dev.Get(), format));
    }
    if (hardware)
    {
        constexpr UINT w = 4096, h = 2057;
        pblend::Cb cb = {}; cb.count = 1; cb.blocks[0] = { 0, 0, w, h, 0, h, 0, 0 };
        pblend::DispatchCb plan;
        Require(pblend::BuildDispatch(cb, w, h, plan) && plan.group_count > 65535, "GPU case spans dispatch rows");
        Image image(d, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        const bool typed = pblend::g_in_place;
        for (int mode = 0; mode < (typed ? 2 : 1); ++mode)
        {
            if (mode || !typed) image.CopyPath();
            d.Begin(); image.Reset(); pblend::Record(d.list.Get(), image.output.Get(), 2, w, h, cb); image.Verify(cb);
        }
        std::puts("PASS GPU: 66048 thread groups across two dispatch rows, with a partial final tile");
    }
    if (benchmark) { Benchmark(d, DXGI_FORMAT_R8G8B8A8_UNORM); Benchmark(d, DXGI_FORMAT_R16G16B16A16_FLOAT); }
    pblend::Release(); d.CheckMessages();
}

int main(int argc, char **argv)
{
    const bool benchmark = argc == 2 && std::strcmp(argv[1], "--benchmark") == 0;
    Require(argc == 1 || benchmark, "usage: portrait-blend.exe [--benchmark]");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); std::puts("D3D12 debug validation enabled"); }
    else std::puts("D3D12 debug layer unavailable; GPU pixels and device completion are checked");
    SchedulerTests();
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
    ComPtr<IDXGIAdapter> warp; Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "enumerate WARP");
    { std::puts("Testing WARP"); Device d(warp.Get()); GpuTests(d, false); }
    bool hardware = false;
    for (UINT i = 0; ; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerated = factory->EnumAdapters1(i, &adapter);
        if (enumerated == DXGI_ERROR_NOT_FOUND) break;
        Check(enumerated, "enumerate adapter");
        DXGI_ADAPTER_DESC1 desc; Check(adapter->GetDesc1(&desc), "adapter description");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        std::printf("Testing GPU: %ls\n", desc.Description);
        Device d(adapter.Get()); GpuTests(d, benchmark, true); hardware = true;
    }
    Require(!benchmark || hardware, "benchmark requires a hardware GPU");
    std::puts("PASS portrait blend: CPU coverage oracle, GPU pixel oracle, capability fallback and shared resources");
}
