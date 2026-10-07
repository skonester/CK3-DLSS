// Run the shipped atlas clear/copy helpers and check every input pixel on Vulkan.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgiformat.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <vector>
#include "../src/feed_vk.h"

static void Log(const char *fmt, ...)
{ va_list args; va_start(args, fmt); std::vprintf(fmt, args); va_end(args); std::puts(""); }
static void Require(bool ok, const char *what)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); } }
static void Check(VkResult r) { Require(r == VK_SUCCESS, "Vulkan operation"); }

int main()
{
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll"); Require(loader != nullptr, "Vulkan loader");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    Require(gipa != nullptr, "instance dispatch");
    auto CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "portrait-atlas-clear-test";
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
    DEVICE(CmdClearColorImage); DEVICE(CmdCopyImage); DEVICE(CmdCopyImageToBuffer);
    DEVICE(CreateBuffer); DEVICE(GetBufferMemoryRequirements); DEVICE(BindBufferMemory); DEVICE(DestroyBuffer);
    DEVICE(MapMemory); DEVICE(UnmapMemory); DEVICE(CreateCommandPool); DEVICE(DestroyCommandPool);
    DEVICE(AllocateCommandBuffers); DEVICE(BeginCommandBuffer); DEVICE(EndCommandBuffer); DEVICE(ResetCommandPool);
    DEVICE(QueueSubmit); DEVICE(CreateFence); DEVICE(WaitForFences); DEVICE(ResetFences); DEVICE(DestroyFence); DEVICE(DestroyDevice);
    FeedVk vk = {}; vk.CmdPipelineBarrier = CmdPipelineBarrier; vk.CmdClearColorImage = CmdClearColorImage; vk.CmdCopyImage = CmdCopyImage;
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
    constexpr uint32_t w = 192, h = 128;
    VkImage images[4]; VkDeviceMemory image_memory[4];
    for (int i = 0; i < 4; ++i)
    {
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO }; info.imageType = VK_IMAGE_TYPE_2D;
        info.format = i % 2 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM; info.extent = {w,h,1};
        info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(CreateImage(dev, &info, nullptr, &images[i]));
        VkMemoryRequirements req; GetImageMemoryRequirements(dev, images[i], &req);
        image_memory[i] = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); Check(BindImageMemory(dev, images[i], image_memory[i], 0));
    }
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bci.size = w * h * 5;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer; Check(CreateBuffer(dev, &bci, nullptr, &buffer));
    VkMemoryRequirements req; GetBufferMemoryRequirements(dev, buffer, &req);
    VkDeviceMemory buffer_memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(BindBufferMemory(dev, buffer, buffer_memory, 0));
    uint8_t *mapped; Check(MapMemory(dev, buffer_memory, 0, bci.size, 0, reinterpret_cast<void **>(&mapped)));
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pci.queueFamilyIndex = family;
    VkCommandPool pool; Check(CreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb; Check(AllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; Check(CreateFence(dev, &fci, nullptr, &fence));
    for (int frame = 0; frame < 3; ++frame)
    {
        const bool mask = frame != 1;
        const int dx = frame == 0 ? 8 : 104, dy = frame == 0 ? 8 : 72;
        const uint32_t cw = frame == 0 ? 80 : 16, ch = frame == 0 ? 48 : 16;
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; Check(BeginCommandBuffer(cb, &begin));
        for (auto image : images) FeedVkBarrier(&vk, cb, image, frame ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        if (frame == 0)
            for (int i = 0; i < 2; ++i) { FeedVkClear(&vk, cb, images[i], .75f); FeedVkBarrier(&vk, cb, images[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL); }
        FeedVkClearAtlas(&vk, cb, images[2], mask ? images[3] : VK_NULL_HANDLE);
        for (int i = 0; i < (mask ? 2 : 1); ++i)
        {
            FeedVkCopyRegion(&vk, cb, images[i], VK_IMAGE_LAYOUT_GENERAL, images[i + 2], VK_IMAGE_LAYOUT_GENERAL, 16,16,dx,dy,cw,ch);
            FeedVkBarrier(&vk, cb, images[i + 2], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            VkBufferImageCopy read = {}; read.bufferOffset = i ? w * h * 4 : 0;
            read.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 }; read.imageExtent = {w,h,1};
            CmdCopyImageToBuffer(cb, images[i + 2], VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &read);
        }
        VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        Check(EndCommandBuffer(cb));
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
        Check(QueueSubmit(queue, 1, &submit, fence)); Check(WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
        for (int i = 0; i < (mask ? 2 : 1); ++i)
            for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t expected = x >= uint32_t(dx) && y >= uint32_t(dy) && x < dx + cw && y < dy + ch ? 191 : 0;
                for (uint32_t c = 0; c < (i ? 1u : 4u); ++c)
                    Require(mapped[(i ? w * h * 4 : 0) + (y * w + x) * (i ? 1 : 4) + c] == expected, "active crops survive, guards and retired pixels are zero");
            }
        Check(ResetFences(dev, 1, &fence)); Check(ResetCommandPool(dev, pool, 0));
    }
    std::puts("PASS Vulkan atlas: RGBA8 color/R8 mask, optional mask, clear-before-copy, guards and retired-slot pixel readback");
    DestroyFence(dev, fence, nullptr); DestroyCommandPool(dev, pool, nullptr);
    UnmapMemory(dev, buffer_memory); DestroyBuffer(dev, buffer, nullptr); FreeMemory(dev, buffer_memory, nullptr);
    for (int i = 0; i < 4; ++i) { DestroyImage(dev, images[i], nullptr); FreeMemory(dev, image_memory[i], nullptr); }
    DestroyDevice(dev, nullptr); DestroyInstance(instance, nullptr); FreeLibrary(loader);
}
