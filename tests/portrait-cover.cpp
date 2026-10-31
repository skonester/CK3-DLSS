// Frontier 3 coverage atlas: the planner against fixed and randomized scenes, then the shipped
// clear + blit helpers on a real Vulkan device, checked pixel by pixel against an oracle that
// samples the synthetic portrait textures the way gui_portrait.shader does.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgiformat.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "../src/feed_vk.h"
#include "../src/feed_portrait_cover.h"

static void Log(const char *fmt, ...)
{ va_list args; va_start(args, fmt); std::vprintf(fmt, args); va_end(args); std::puts(""); }
static void Require(bool ok, const char *what)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); } }
static void Check(VkResult r) { Require(r == VK_SUCCESS, "Vulkan operation"); }

using pcover::Blit;
using pcover::Source;
using pcover::Widget;

static Source Identity(const Widget &w, uint32_t tw, uint32_t th) { return { w, 0, 0, 1, 1, tw, th }; }

static void PlannerTests()
{
    const portrait::Item item = { 96, 48, 400, 32, 160, 192, 0 };   // screen 96,48 -> atlas 400,32, 160x192
    std::vector<Blit> out;
    {
        const std::vector<Widget> w = { { 112, 64, 128, 160 } };
        const std::vector<Source> s = { Identity(w[0], 128, 160) };
        Require(pcover::PlanItem(item, w, s, 1920, 1080, out) == pcover::kPlanned && out.size() == 1, "identity widget plans one blit");
        const Blit &b = out[0];
        Require(b.sx0 == 0 && b.sy0 == 0 && b.sx1 == 128 && b.sy1 == 160, "identity samples the whole texture");
        Require(b.dx0 == 416 && b.dy0 == 48 && b.dx1 == 544 && b.dy1 == 208, "identity lands at the widget's atlas position");
    }
    {
        const std::vector<Widget> w = { { 112, 64, 128, 160 } };
        const std::vector<Source> s = { { w[0], 1, 1, -1, -1, 128, 160 } };
        Require(pcover::PlanItem(item, w, s, 1920, 1080, out) == pcover::kPlanned && out[0].sx0 == 128 && out[0].sx1 == 0 &&
                out[0].sy0 == 160 && out[0].sy1 == 0, "mirrored axes (offset 1, scale -1) reverse the source offsets");
    }
    {
        // uv = (t - 0.25) / 0.5: the texture fills the middle half of the widget; the rest stays zero.
        const std::vector<Widget> w = { { 112, 64, 128, 160 } };
        const std::vector<Source> s = { { w[0], .25f, .25f, .5f, .5f, 64, 80 } };
        Require(pcover::PlanItem(item, w, s, 1920, 1080, out) == pcover::kPlanned && out[0].sx0 == 0 && out[0].sx1 == 64 &&
                out[0].dx0 == 448 && out[0].dx1 == 512 && out[0].dy0 == 88 && out[0].dy1 == 168, "a shrunken texture covers only its part");
        // uv = (t + 0.25) / 1.5: zoomed in, the widget shows texels 1/6 .. 5/6.
        const std::vector<Source> z = { { w[0], -.25f, -.25f, 1.5f, 1.5f, 120, 120 } };
        Require(pcover::PlanItem(item, w, z, 1920, 1080, out) == pcover::kPlanned && out[0].sx0 == 20 && out[0].sx1 == 100 &&
                out[0].dx0 == 416 && out[0].dx1 == 544, "a zoomed texture maps its visible texels over the whole widget");
    }
    {
        const std::vector<Widget> w = { { 112, 64, 128, 160 } };
        const std::vector<Source> s = { { w[0], 2, 0, 1, 1, 128, 160 } };
        Require(pcover::PlanItem(item, w, s, 1920, 1080, out) == pcover::kPlanned && out.empty(),
                "a texture shifted out of the widget plans an all-zero block");
    }
    {
        const portrait::Item edge = { 0, 0, 0, 0, 96, 96, 3 };
        const std::vector<Widget> w = { { -20, -10, 80, 100 } };
        const std::vector<Source> s = { Identity(w[0], 80, 100) };
        Require(pcover::PlanItem(edge, w, s, 1920, 1080, out) == pcover::kPlanned && out[0].sx0 == 20 && out[0].sy0 == 10 &&
                out[0].dx0 == 0 && out[0].dy0 == 0 && out[0].dx1 == 60 && out[0].dy1 == 90, "screen clipping offsets the texels");
    }
    {
        const std::vector<Widget> w = { { 112, 64, 64, 64 }, { 150, 100, 64, 64 } };
        const std::vector<Source> s = { Identity(w[0], 64, 64), Identity(w[1], 64, 64) };
        Require(pcover::PlanItem(item, w, s, 1920, 1080, out) == pcover::kOverlap && out.empty(), "overlapping widgets fall back (no union by blit)");
    }
    {
        const std::vector<Widget> w = { { 112, 64, 40, 40 }, { 180, 150, 40, 40 } };
        const std::vector<Source> s = { Identity(w[0], 40, 40) };
        Require(pcover::PlanItem(item, w, s, 1920, 1080, out) == pcover::kNoSource, "a widget without a source fails its block");
        const std::vector<Source> unusable = { s[0], Identity(w[1], 0, 40) };
        Require(pcover::PlanItem(item, w, unusable, 1920, 1080, out) == pcover::kUnusable, "an unusable source (tex_w 0) fails its block");
        const std::vector<Source> both = { s[0], Identity(w[1], 40, 40) };
        Require(pcover::PlanItem(item, w, both, 1920, 1080, out) == pcover::kPlanned && out.size() == 2, "disjoint widgets both blit");
    }
    {
        const std::vector<Widget> w = { { 900, 900, 40, 40 } };
        const std::vector<Source> s = { Identity(w[0], 40, 40) };
        Require(pcover::PlanItem(item, w, s, 1920, 1080, out) == pcover::kNoWidget, "a block with no widget gets no mask");
    }
    {
        // 4 texels across 400 pixels: the item clips a 1-pixel sliver that still needs one texel.
        const portrait::Item thin = { 255, 0, 0, 0, 1, 16, 0 };
        const std::vector<Widget> w = { { 0, 0, 400, 16 } };
        for (bool mirror : { false, true })
        {
            const std::vector<Source> s = { { w[0], mirror ? 1.f : 0.f, 0, mirror ? -1.f : 1.f, 1, 4, 16 } };
            Require(pcover::PlanItem(thin, w, s, 1920, 1080, out) == pcover::kPlanned && std::abs(out[0].sx1 - out[0].sx0) == 1,
                    "a sliver samples exactly one texel");
            Require(std::min(out[0].sx0, out[0].sx1) == (mirror ? 1 : 2), "the sliver samples the texel it lies on");
        }
    }

    std::mt19937 rng(20261031);
    auto next = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    auto real = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    int planned = 0, refused = 0;
    for (int trial = 0; trial < 4000; ++trial)
    {
        const uint32_t sw = uint32_t(next(64, 640)), sh = uint32_t(next(64, 480));
        std::vector<Widget> w;
        std::vector<Source> s;
        for (int i = next(0, 6); i > 0; --i)
        {
            const Widget r = { next(-60, int(sw)), next(-60, int(sh)), next(1, 200), next(1, 200) };
            w.push_back(r);
            const float scale_x = (next(0, 1) ? 1.f : -1.f) * real(.2f, 3.f), scale_y = (next(0, 1) ? 1.f : -1.f) * real(.2f, 3.f);
            if (next(0, 9) != 0)
                s.push_back({ r, real(-1.5f, 1.5f), real(-1.5f, 1.5f), scale_x, scale_y, uint32_t(next(0, 300)), uint32_t(next(1, 300)) });
        }
        const portrait::Item it = { next(-20, int(sw)), next(-20, int(sh)), next(0, 500), next(0, 500), next(1, 300), next(1, 300), 0 };
        if (pcover::PlanItem(it, w, s, sw, sh, out) != pcover::kPlanned) { Require(out.empty(), "a refused block plans nothing"); ++refused; continue; }
        ++planned;
        const int ax0 = it.dx + std::max(0, -it.sx), ay0 = it.dy + std::max(0, -it.sy);
        const int ax1 = it.dx + std::min(it.w, int(sw) - it.sx), ay1 = it.dy + std::min(it.h, int(sh) - it.sy);
        for (size_t i = 0; i < out.size(); ++i)
        {
            const Blit &b = out[i];
            const Source &src = s[b.source];
            Require(b.dx0 < b.dx1 && b.dy0 < b.dy1, "destination regions are non-empty");
            Require(b.dx0 >= ax0 && b.dy0 >= ay0 && b.dx1 <= ax1 && b.dy1 <= ay1, "blits stay inside the item's on-screen part");
            Require(b.sx0 != b.sx1 && b.sy0 != b.sy1, "source regions are non-empty");
            Require(std::min(b.sx0, b.sx1) >= 0 && std::max(b.sx0, b.sx1) <= int(src.tex_w) &&
                    std::min(b.sy0, b.sy1) >= 0 && std::max(b.sy0, b.sy1) <= int(src.tex_h), "source regions stay inside the texture");
            Require((b.sx0 > b.sx1) == (src.scale_x < 0) && (b.sy0 > b.sy1) == (src.scale_y < 0), "mirroring follows the scale sign");
            for (size_t j = 0; j < i; ++j)
                Require(b.dx0 >= out[j].dx1 || out[j].dx0 >= b.dx1 || b.dy0 >= out[j].dy1 || out[j].dy0 >= b.dy1,
                        "every coverage pixel has one writer");
        }
    }
    Require(planned > 300 && refused > 300, "randomized scenes exercise both outcomes");
    std::printf("PASS planner: identity, mirror, shrunk/zoomed/shifted UV, screen clip, overlap/missing/unusable fallbacks, slivers; "
                "%d planned and %d refused random blocks\n", planned, refused);
}

// ---------------------------------------------------------------------------
// GPU: real Vulkan blits through the shipped helpers.
// ---------------------------------------------------------------------------

struct Tex { uint32_t w, h, layers; VkFormat fmt; Widget widget; float off_x, off_y, scale_x, scale_y; uint32_t layer; bool source; };

static uint8_t AlphaAt(int t, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (layer == 0 && t == 2) return 255;   // decoy layer: a wrong layer turns the whole widget opaque
    const float cx = (float(x) + .5f) / float(w) - .5f, cy = (float(y) + .5f) / float(h) - .45f;
    const float r = std::sqrt(cx * cx * 3.2f + cy * cy * 2.4f);
    if (r > .62f) return 0;
    if (r > .52f) return uint8_t(255 * (.62f - r) / .1f);   // soft hair edge
    return ((x * 7 + y * 3 + t * 11) % 23 == 0) ? 96 : 255;   // interior detail, not uniform
}

static void GpuTest()
{
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll"); Require(loader != nullptr, "Vulkan loader");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    Require(gipa != nullptr, "instance dispatch");
    auto CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "portrait-cover-test";
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
    VkInstance instance; Check(CreateInstance(&ici, nullptr, &instance));
#define INSTANCE(name) auto name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name)); Require(name != nullptr, #name)
    INSTANCE(EnumeratePhysicalDevices); INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(GetPhysicalDeviceMemoryProperties); INSTANCE(GetPhysicalDeviceProperties);
    INSTANCE(CreateDevice); INSTANCE(GetDeviceProcAddr); INSTANCE(DestroyInstance);
    uint32_t count = 0; Check(EnumeratePhysicalDevices(instance, &count, nullptr)); Require(count != 0, "GPU");
    std::vector<VkPhysicalDevice> devices(count); Check(EnumeratePhysicalDevices(instance, &count, devices.data()));
    const auto phys = devices.front();
    VkPhysicalDeviceProperties props; GetPhysicalDeviceProperties(phys, &props); std::printf("GPU: %s\n", props.deviceName);
    GetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); GetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    Require(family != UINT32_MAX, "graphics queue");
    float priority = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; Check(CreateDevice(phys, &dci, nullptr, &dev));
#define DEVICE(name) auto name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(dev, "vk" #name)); Require(name != nullptr, #name)
    DEVICE(GetDeviceQueue); DEVICE(CreateImage); DEVICE(GetImageMemoryRequirements); DEVICE(BindImageMemory);
    DEVICE(AllocateMemory); DEVICE(FreeMemory); DEVICE(DestroyImage); DEVICE(CmdPipelineBarrier);
    DEVICE(CmdClearColorImage); DEVICE(CmdCopyImage); DEVICE(CmdBlitImage); DEVICE(CmdCopyImageToBuffer); DEVICE(CmdCopyBufferToImage);
    DEVICE(CreateBuffer); DEVICE(GetBufferMemoryRequirements); DEVICE(BindBufferMemory); DEVICE(DestroyBuffer);
    DEVICE(MapMemory); DEVICE(UnmapMemory); DEVICE(CreateCommandPool); DEVICE(DestroyCommandPool);
    DEVICE(AllocateCommandBuffers); DEVICE(BeginCommandBuffer); DEVICE(EndCommandBuffer); DEVICE(ResetCommandPool);
    DEVICE(QueueSubmit); DEVICE(CreateFence); DEVICE(WaitForFences); DEVICE(ResetFences); DEVICE(DestroyFence); DEVICE(DestroyDevice);
    FeedVk vk = {};
    vk.CmdPipelineBarrier = CmdPipelineBarrier; vk.CmdClearColorImage = CmdClearColorImage;
    vk.CmdCopyImage = CmdCopyImage; vk.CmdBlitImage = CmdBlitImage;
    VkQueue queue; GetDeviceQueue(dev, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory; GetPhysicalDeviceMemoryProperties(phys, &memory);
    auto allocate = [&](const VkMemoryRequirements &req, VkMemoryPropertyFlags flags) {
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < memory.memoryTypeCount; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (memory.memoryTypes[t].propertyFlags & flags) == flags) { type = t; break; }
        Require(type != UINT32_MAX, "memory type");
        VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; info.allocationSize = req.size; info.memoryTypeIndex = type;
        VkDeviceMemory result; Check(AllocateMemory(dev, &info, nullptr, &result)); return result;
    };
    auto make_image = [&](uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt, VkImage &img, VkDeviceMemory &mem) {
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO }; info.imageType = VK_IMAGE_TYPE_2D;
        info.format = fmt; info.extent = { w, h, 1 }; info.mipLevels = 1; info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        Check(CreateImage(dev, &info, nullptr, &img));
        VkMemoryRequirements req; GetImageMemoryRequirements(dev, img, &req);
        mem = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); Check(BindImageMemory(dev, img, mem, 0));
    };

    // Screen 480x200. A: identity, clipped at the left edge. B: mirrored X, BGRA. C: scaled 50->49,
    // mirrored Y, layer 1 of 2, reaching into B's crop margin. D+E overlap (refused). F: no source.
    // G: hover-style zoom, uv = (t + 0.1) / 1.2 (texels 1/12 .. 11/12 fill the widget).
    // H: shrunken, uv = (t - 0.2) / 0.6 (the texture fills the middle 60%, zero around it).
    constexpr uint32_t sw = 480, sh = 200;
    std::vector<Tex> tex = {
        { 96, 120, 1, VK_FORMAT_R8G8B8A8_UNORM, { -20, 40, 96, 120 }, 0, 0, 1, 1, 0, true },
        { 60, 80, 1, VK_FORMAT_B8G8R8A8_UNORM, { 150, 30, 60, 80 }, 1, 0, -1, 1, 0, true },
        { 50, 50, 2, VK_FORMAT_R8G8B8A8_UNORM, { 230, 120, 49, 49 }, 0, 1, 1, -1, 1, true },
        { 40, 40, 1, VK_FORMAT_R8G8B8A8_UNORM, { 100, 140, 40, 40 }, 0, 0, 1, 1, 0, true },
        { 40, 40, 1, VK_FORMAT_R8G8B8A8_UNORM, { 120, 150, 40, 40 }, 0, 0, 1, 1, 0, true },
        { 40, 40, 1, VK_FORMAT_R8G8B8A8_UNORM, { 280, 10, 32, 32 }, 0, 0, 1, 1, 0, false },
        { 72, 72, 1, VK_FORMAT_R8G8B8A8_UNORM, { 400, 20, 72, 72 }, -.1f, -.1f, 1.2f, 1.2f, 0, true },
        { 30, 30, 1, VK_FORMAT_R8G8B8A8_UNORM, { 330, 120, 60, 60 }, .2f, .2f, .6f, .6f, 0, true },
    };
    std::vector<Widget> widgets;
    std::vector<Source> sources;
    std::vector<size_t> source_tex;
    for (size_t i = 0; i < tex.size(); ++i)
    {
        widgets.push_back(tex[i].widget);
        if (tex[i].source)
        {
            sources.push_back({ tex[i].widget, tex[i].off_x, tex[i].off_y, tex[i].scale_x, tex[i].scale_y, tex[i].w, tex[i].h });
            source_tex.push_back(i);
        }
    }
    portrait::Atlas atlas;
    Require(portrait::Build(widgets, sw, sh, { 16, 16, 400000 }, nullptr, atlas), "fixture atlas builds");

    std::vector<VkImage> images(tex.size()); std::vector<VkDeviceMemory> image_mem(tex.size());
    for (size_t i = 0; i < tex.size(); ++i) make_image(tex[i].w, tex[i].h, tex[i].layers, tex[i].fmt, images[i], image_mem[i]);
    VkImage cover; VkDeviceMemory cover_mem; make_image(atlas.w, atlas.h, 1, VK_FORMAT_R8G8B8A8_UNORM, cover, cover_mem);

    uint64_t upload_bytes = 0;
    for (const Tex &t : tex) upload_bytes += uint64_t(t.w) * t.h * 4 * t.layers;
    const uint64_t read_bytes = uint64_t(atlas.w) * atlas.h * 4;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bci.size = upload_bytes + read_bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer; Check(CreateBuffer(dev, &bci, nullptr, &buffer));
    VkMemoryRequirements req; GetBufferMemoryRequirements(dev, buffer, &req);
    VkDeviceMemory buffer_memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(BindBufferMemory(dev, buffer, buffer_memory, 0));
    uint8_t *mapped; Check(MapMemory(dev, buffer_memory, 0, bci.size, 0, reinterpret_cast<void **>(&mapped)));
    std::vector<uint64_t> offsets;
    {
        uint64_t at = 0;
        for (size_t i = 0; i < tex.size(); ++i)
        {
            offsets.push_back(at);
            const Tex &t = tex[i];
            const bool bgra = t.fmt == VK_FORMAT_B8G8R8A8_UNORM;
            for (uint32_t l = 0; l < t.layers; ++l)
                for (uint32_t y = 0; y < t.h; ++y) for (uint32_t x = 0; x < t.w; ++x)
                {
                    uint8_t *p = mapped + at + ((uint64_t(l) * t.h + y) * t.w + x) * 4;
                    const uint8_t a = AlphaAt(int(i), l, x, y, t.w, t.h);
                    p[bgra ? 2 : 0] = uint8_t(255 - a); p[1] = uint8_t(x * 9 + y); p[bgra ? 0 : 2] = uint8_t(y * 5); p[3] = a;
                }
            at += uint64_t(t.w) * t.h * 4 * t.layers;
        }
    }

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pci.queueFamilyIndex = family;
    VkCommandPool pool; Check(CreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb; Check(AllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; Check(CreateFence(dev, &fci, nullptr, &fence));
    auto layer_barrier = [&](VkImage img, uint32_t layers, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // Two frames: the second starts from a dirty atlas to prove the clear covers retired pixels.
    std::vector<int> planned_items;
    for (int frame = 0; frame < 2; ++frame)
    {
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; Check(BeginCommandBuffer(cb, &begin));
        if (frame == 0)
        {
            for (size_t i = 0; i < tex.size(); ++i)
            {
                layer_barrier(images[i], tex[i].layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy up = {}; up.bufferOffset = offsets[i];
                up.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, tex[i].layers }; up.imageExtent = { tex[i].w, tex[i].h, 1 };
                CmdCopyBufferToImage(cb, buffer, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
                // The production path finds them in shader-read state and moves them to transfer-source.
                layer_barrier(images[i], tex[i].layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            FeedVkBarrier(&vk, cb, cover, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            FeedVkClear(&vk, cb, cover, 1.0f);   // dirty: everything must be cleared or overwritten
            FeedVkBarrier(&vk, cb, cover, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        FeedVkClearAtlas(&vk, cb, cover, VK_NULL_HANDLE);
        for (size_t i = 0; i < tex.size(); ++i)
            layer_barrier(images[i], tex[i].layers, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        planned_items.clear();
        std::vector<Blit> blits;
        for (size_t it = 0; it < atlas.items.size(); ++it)
        {
            if (pcover::PlanItem(atlas.items[it], widgets, sources, sw, sh, blits) != pcover::kPlanned) continue;
            planned_items.push_back(int(it));
            for (const Blit &b : blits)
            {
                const size_t t = source_tex[b.source];
                FeedVkBlitScaled(&vk, cb, images[t], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, tex[t].layer, cover, VK_IMAGE_LAYOUT_GENERAL,
                                 b.sx0, b.sy0, b.sx1, b.sy1, b.dx0, b.dy0, b.dx1, b.dy1);
            }
        }
        for (size_t i = 0; i < tex.size(); ++i)
            layer_barrier(images[i], tex[i].layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        FeedVkBarrier(&vk, cb, cover, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy read = {}; read.bufferOffset = upload_bytes;
        read.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; read.imageExtent = { atlas.w, atlas.h, 1 };
        CmdCopyImageToBuffer(cb, cover, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &read);
        VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        Check(EndCommandBuffer(cb));
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
        Check(QueueSubmit(queue, 1, &submit, fence)); Check(WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));

        // Which atlas item holds which widget, from the fixture geometry.
        auto item_of = [&](size_t t) {
            for (size_t it = 0; it < atlas.items.size(); ++it)
            {
                const auto &a = atlas.items[it];
                const Widget &w = tex[t].widget;
                if (std::max(w.x, 0) >= a.sx && std::max(w.y, 0) >= a.sy && std::min(w.x + w.w, int(sw)) <= a.sx + a.w &&
                    std::min(w.y + w.h, int(sh)) <= a.sy + a.h) return int(it);
            }
            return -1;
        };
        const auto has = [&](int it) { return std::find(planned_items.begin(), planned_items.end(), it) != planned_items.end(); };
        Require(has(item_of(0)) && has(item_of(1)) && has(item_of(2)), "identity, mirrored and scaled portraits get coverage");
        Require(has(item_of(6)) && has(item_of(7)), "zoomed and shrunken portraits get coverage");
        Require(item_of(3) == item_of(4) && !has(item_of(3)), "the merged overlapping pair keeps the legacy feather");
        Require(!has(item_of(5)), "a portrait without a source keeps the legacy feather");
        Require(atlas.items[size_t(item_of(1))].sx + atlas.items[size_t(item_of(1))].w > tex[2].widget.x &&
                atlas.items[size_t(item_of(1))].sy + atlas.items[size_t(item_of(1))].h > tex[2].widget.y,
                "fixture: the scaled portrait reaches into the mirrored portrait's block");

        const uint8_t *out = mapped + upload_bytes;
        uint64_t checked = 0, inside = 0;
        for (uint32_t y = 0; y < atlas.h; ++y) for (uint32_t x = 0; x < atlas.w; ++x)
        {
            const uint8_t got = out[(uint64_t(y) * atlas.w + x) * 4 + 3];
            int owner = -1;
            for (int it : planned_items)
            {
                const auto &a = atlas.items[size_t(it)];
                if (int(x) >= a.dx && int(y) >= a.dy && int(x) < a.dx + a.w && int(y) < a.dy + a.h) owner = it;
            }
            ++checked;
            if (owner < 0) { Require(got == 0, "pixels outside covered blocks are zero (cleared, never written)"); continue; }
            const auto &a = atlas.items[size_t(owner)];
            const int px = a.sx + int(x) - a.dx, py = a.sy + int(y) - a.dy;
            int lo = 0, hi = 0;   // expected alpha range
            for (size_t t = 0; t < tex.size(); ++t)
            {
                const Tex &T = tex[t];
                const Widget &w = T.widget;
                if (!T.source || px < w.x || py < w.y || px >= w.x + w.w || py >= w.y + w.h || px >= int(sw) || py >= int(sh)) continue;
                // The shader's mapping at the pixel centre, in texel units (texel centres at k + 0.5).
                const double u = ((px - w.x + .5) / w.w - T.off_x) / T.scale_x * T.w;
                const double v = ((py - w.y + .5) / w.h - T.off_y) / T.scale_y * T.h;
                const bool unit = std::fabs(std::fabs(T.scale_x) - 1) < 1e-6 && std::fabs(std::fabs(T.scale_y) - 1) < 1e-6 &&
                                  T.w == uint32_t(w.w) && T.h == uint32_t(w.h);
                if (unit)
                {
                    ++inside;
                    lo = hi = AlphaAt(int(t), T.layer, uint32_t(std::floor(u)), uint32_t(std::floor(v)), T.w, T.h);   // 1:1 blit is exact
                    continue;
                }
                const bool outside = u < 0 || v < 0 || u > T.w || v > T.h;
                const bool near_edge = u < 1 || v < 1 || u > T.w - 1.0 || v > T.h - 1.0;
                if (outside && !near_edge) { lo = hi = 0; continue; }   // texture not visible here: original pixels
                ++inside;
                // Scaled: within the texels around the mapped point (rounded region edges shift it < 1 texel).
                lo = near_edge ? 0 : 255; hi = 0;
                for (int dy = -2; dy <= 1; ++dy) for (int dx = -2; dx <= 1; ++dx)
                {
                    const int tx = std::clamp(int(std::floor(u)) + dx + 1, 0, int(T.w) - 1);
                    const int ty = std::clamp(int(std::floor(v)) + dy + 1, 0, int(T.h) - 1);
                    const int s = AlphaAt(int(t), T.layer, uint32_t(tx), uint32_t(ty), T.w, T.h);
                    lo = std::min(lo, s); hi = std::max(hi, s);
                }
            }
            if (got < lo - 1 || got > hi + 1)
            {
                std::fprintf(stderr, "atlas %u,%u (screen %d,%d): alpha %u, expected %d..%d\n", x, y, px, py, got, lo, hi);
                Require(false, "coverage equals the portrait alpha under each widget and zero elsewhere");
            }
        }
        Require(inside > 10000, "the oracle checked a substantial number of covered pixels");
        Check(ResetFences(dev, 1, &fence)); Check(ResetCommandPool(dev, pool, 0));
        if (frame == 1)
            std::printf("PASS Vulkan coverage blits: identity, screen clip, mirrored BGRA, scaled array layer, hover zoom, shrunken texture, "
                        "neighbour in a crop margin, refused overlap and missing source, dirty-atlas clear; %llu atlas pixels, %llu under widgets\n",
                        static_cast<unsigned long long>(checked), static_cast<unsigned long long>(inside));
    }
    DestroyFence(dev, fence, nullptr); DestroyCommandPool(dev, pool, nullptr);
    UnmapMemory(dev, buffer_memory); DestroyBuffer(dev, buffer, nullptr); FreeMemory(dev, buffer_memory, nullptr);
    for (size_t i = 0; i < tex.size(); ++i) { DestroyImage(dev, images[i], nullptr); FreeMemory(dev, image_mem[i], nullptr); }
    DestroyImage(dev, cover, nullptr); FreeMemory(dev, cover_mem, nullptr);
    DestroyDevice(dev, nullptr); DestroyInstance(instance, nullptr); FreeLibrary(loader);
}

int main()
{
    PlannerTests();
    GpuTest();
    std::puts("PASS portrait coverage: planner invariants and Vulkan pixel oracle");
}
