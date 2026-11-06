// VK_LAYER_ck3_fsr - measurement-only Vulkan layer. Loaded solely by CK3-FSR-Measurement.ps1 through
// VK_LAYER_PATH / VK_INSTANCE_LAYERS for one launch; never installed for normal play.
//
// CK3's frame at render_scale<1 (established by the census below):
//   shadow + portrait passes -> scene at render size (colour + D24S8) -> bloom/exposure ->
//   tonemap into a render-size BGRA8 target -> first swapchain pass: one fullscreen upscale draw,
//   then the GUI at native resolution -> second swapchain pass: more GUI, portraits.
// All viewports are dynamic and the frame records into one primary command buffer.
//
// CK3_FSR_MODE=Dedicated runs FSR 3.1.3 on the game's own device and command buffer, right before
// the first swapchain pass: input = the tonemapped render-size target, the scene depth and subpixel
// jitter applied through the scene passes' viewports. Its output is copied to the swapchain image
// (which that pass LOADs) and the game's upscale draw is skipped, so the GUI is drawn at native
// resolution over it. Any failure leaves the game's own upscale in place.
//
// The engine also renders portraits at render scale, so at 720p they reach the GUI (and DLSS 5) at 2/3 size. In the
// same mode the layer creates the portrait targets at native size and scales their render area, viewports, scissors
// and resolves to match; everything samples them through normalized UVs. CK3_FSR_PORTRAIT_SCALE=1 turns this off.
// The final R8G8B8A8 target is created before anything identifies it as a portrait, so it gets a native-size shadow
// image once the multisampled target of its size appears; framebuffers, descriptors and barriers follow the shadow.
//
// Motion vectors come from the camera: the matrices are read from the game's PdxCamera constant buffer as the scene
// is recorded, and ck3_fsr_motion.comp turns depth into per-pixel motion. Moving objects get camera motion only.
//
// CK3_FSR_MODE=Off keeps the game's reduced scene but replaces its bilinear upscale with FSR 1 EASU + RCAS, and sharpens
// each portrait with RCAS right after the game renders it, before DLSS 5 reads it. CK3_FSR_SPATIAL=0 gives the plain game.
//
// Other modes only time presents. In every mode except Integrated, frames.csv is written here.
// CK3_FSR_CENSUS=1 additionally writes the structure of the game's frames to census-*.txt.

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <share.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "ck3_fsr_motion.spv.h"
#include "ck3_fsr_easu.spv.h"
#include "ck3_fsr_rcas.spv.h"

// AMD's MIT-licensed constant setup for EASU and RCAS.
#define A_CPU 1
#include "ffx_fsr1/ffx_fsr1.h"

namespace {

std::wstring EnvW(const wchar_t *name)
{
    wchar_t value[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(value) : std::wstring();
}

std::string Fmt(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    return line;
}

std::wstring g_out_dir;

void Log(const char *fmt, ...)
{
    if (g_out_dir.empty()) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE *f = nullptr;
    if (_wfopen_s(&f, (g_out_dir + L"\\layer.log").c_str(), L"a") == 0 && f != nullptr)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// FidelityFX API declarations (SDK v1.1.3 headers; see AMD-FSR-LICENSE.txt)
// ---------------------------------------------------------------------------

struct FfxHeader { uint64_t type; FfxHeader *pNext; };
struct FfxBackendVK { FfxHeader header; VkDevice device; VkPhysicalDevice gpu; PFN_vkGetDeviceProcAddr gdpa; };
struct FfxOverrideVersion { FfxHeader header; uint64_t version; };
struct FfxGetVersions { FfxHeader header; uint64_t createDescType; void *device; uint64_t *count; uint64_t *ids; const char **names; };
struct FfxDims { uint32_t w, h; };
struct FfxCoords { float x, y; };
using FfxMessage = void (*)(uint32_t, const wchar_t *);
struct FfxCreateUpscale { FfxHeader header; uint32_t flags; FfxDims maxRender, maxUpscale; FfxMessage message; };
struct FfxDesc { uint32_t type, format, width, height, depth, mips, flags, usage; };
struct FfxResource { void *resource; FfxDesc desc; uint32_t state; };
struct FfxDispatchUpscale {
    FfxHeader header;
    void *commandList;
    FfxResource color, depth, motion, exposure, reactive, composition, output;
    FfxCoords jitter, motionScale;
    FfxDims renderSize, upscaleSize;
    bool sharpen;
    float sharpness, frameTimeDelta, preExposure;
    bool reset;
    float cameraNear, cameraFar, cameraFov, metersFactor;
    uint32_t flags;
};
struct FfxJitterPhases { FfxHeader header; uint32_t renderWidth, displayWidth; int32_t *out; };
struct FfxJitterOffset { FfxHeader header; int32_t index, phases; float *x, *y; };

constexpr uint32_t kFfxTexture2D = 2, kFfxRgba16F = 4, kFfxR32Uint = 8, kFfxRg16F = 18;
constexpr uint32_t kFfxUsageUav = 2, kFfxUsageDepth = 4, kFfxUsageStencil = 32;
constexpr uint32_t kFfxStateUav = 2, kFfxStateComputeRead = 4;
constexpr uint32_t kFfxAutoExposure = 1u << 5, kFfxDebugChecking = 1u << 7;

using PfnFfxCreate = uint32_t (*)(void **, FfxHeader *, const void *);
using PfnFfxDestroy = uint32_t (*)(void **, const void *);
using PfnFfxQuery = uint32_t (*)(void **, FfxHeader *);
using PfnFfxDispatch = uint32_t (*)(void **, const FfxHeader *);

// ---------------------------------------------------------------------------
// Next-layer entry points
// ---------------------------------------------------------------------------

PFN_vkGetDeviceProcAddr g_next_gdpa;
PFN_vkGetInstanceProcAddr g_next_gipa;
VkDevice g_device;
VkPhysicalDevice g_gpu;
std::vector<VkInstance> g_instances;

#define NEXT_FUNCS(X)                  \
    X(CreateImage)                     \
    X(DestroyImage)                    \
    X(CreateImageView)                 \
    X(DestroyImageView)                \
    X(CreateRenderPass)                \
    X(CreateRenderPass2)               \
    X(CreateFramebuffer)               \
    X(DestroyFramebuffer)              \
    X(CreateShaderModule)              \
    X(DestroyShaderModule)             \
    X(CreateGraphicsPipelines)         \
    X(CreateSwapchainKHR)              \
    X(GetSwapchainImagesKHR)           \
    X(DestroyDevice)                   \
    X(DeviceWaitIdle)                  \
    X(BeginCommandBuffer)              \
    X(CmdBeginRenderPass)              \
    X(CmdBeginRenderPass2)             \
    X(CmdEndRenderPass)                \
    X(CmdEndRenderPass2)               \
    X(CmdBeginRendering)               \
    X(CmdEndRendering)                 \
    X(CmdBindPipeline)                 \
    X(CmdSetViewport)                  \
    X(CmdSetScissor)                   \
    X(CmdCopyBufferToImage)            \
    X(UpdateDescriptorSets)            \
    X(CreateDescriptorSetLayout)       \
    X(DestroyDescriptorSetLayout)      \
    X(AllocateDescriptorSets)          \
    X(CreateDescriptorPool)            \
    X(DestroyDescriptorPool)           \
    X(CreatePipelineLayout)            \
    X(DestroyPipelineLayout)           \
    X(CreateComputePipelines)          \
    X(DestroyPipeline)                 \
    X(CreateSampler)                   \
    X(DestroySampler)                  \
    X(CmdBindDescriptorSets)           \
    X(CmdPushConstants)                \
    X(UpdateDescriptorSetWithTemplate) \
    X(CmdPushDescriptorSetKHR)         \
    X(CmdClearAttachments)             \
    X(CmdDraw)                         \
    X(CmdDrawIndexed)                  \
    X(CmdDrawIndirect)                 \
    X(CmdDrawIndexedIndirect)          \
    X(CmdDispatch)                     \
    X(CmdBlitImage)                    \
    X(CmdCopyImage)                    \
    X(CmdResolveImage)                 \
    X(CmdExecuteCommands)              \
    X(CmdPipelineBarrier)              \
    X(CmdPipelineBarrier2)             \
    X(CmdClearColorImage)              \
    X(CmdWriteTimestamp)               \
    X(CmdResetQueryPool)               \
    X(CreateQueryPool)                 \
    X(DestroyQueryPool)                \
    X(GetQueryPoolResults)             \
    X(AllocateMemory)                  \
    X(FreeMemory)                      \
    X(BindImageMemory)                 \
    X(GetImageMemoryRequirements)      \
    X(EndCommandBuffer)                \
    X(CmdCopyImageToBuffer)            \
    X(CreateBuffer)                    \
    X(DestroyBuffer)                   \
    X(GetBufferMemoryRequirements)     \
    X(BindBufferMemory)                \
    X(MapMemory)                       \
    X(UnmapMemory)                     \
    X(QueueSubmit)                     \
    X(QueueSubmit2)                    \
    X(QueuePresentKHR)

struct Next {
#define X(name) PFN_vk##name name = nullptr;
    NEXT_FUNCS(X)
#undef X
} next;

// ---------------------------------------------------------------------------
// Tracked objects
// ---------------------------------------------------------------------------

bool g_census, g_track, g_fsr_mode, g_csv_mode;
std::mutex g_mx;   // object tables, frame state and the census text

struct ImageInfo { uint32_t id, w, h, fmt, usage, samples, mips, layers; bool swap; VkImageLayout layout; float sx = 1, sy = 1; };
struct AttDesc { uint32_t fmt, load, store, initial_layout, final_layout, samples; };

std::unordered_map<VkImage, ImageInfo> g_images;
std::unordered_map<VkImageView, VkImage> g_views;
std::unordered_map<VkRenderPass, std::vector<AttDesc>> g_passes;
std::unordered_map<VkFramebuffer, std::vector<VkImageView>> g_fbs;
std::unordered_map<VkShaderModule, uint32_t> g_module_tags;
// Where a shader keeps the camera: from the names DXC leaves in the SPIR-V.
struct CameraBlock { bool found = false; uint32_t set = 0, binding = 0, vp = 0, inv_vp = 0, z_near = 0, z_far = 0, fov = 0; };
struct PipelineInfo { uint32_t tags; bool dyn_viewport; bool has_scissor = false; VkRect2D scissor{}; CameraBlock camera{}; };
std::unordered_map<VkPipeline, PipelineInfo> g_pipelines;
std::unordered_map<VkShaderModule, CameraBlock> g_module_cameras;

// Enough of the game's descriptor state to find the bytes behind a uniform buffer binding.
struct BufferBinding { VkBuffer buffer; VkDeviceSize offset; };
struct LayoutBinding { uint32_t binding, count; VkDescriptorType type; };
std::unordered_map<VkDescriptorSetLayout, std::vector<LayoutBinding>> g_set_layouts;
std::unordered_map<VkDescriptorSet, VkDescriptorSetLayout> g_set_layout_of;
std::unordered_map<VkDescriptorSet, std::unordered_map<uint32_t, BufferBinding>> g_set_buffers;
struct BufferMemory { VkDeviceMemory memory; VkDeviceSize offset; };
std::unordered_map<VkBuffer, BufferMemory> g_buffer_memory;
struct MappedMemory { char *base; };
std::unordered_map<VkDeviceMemory, MappedMemory> g_mapped;

CameraBlock ParseCamera(const uint32_t *w, size_t count)
{
    CameraBlock out;
    if (w == nullptr || count < 5 || w[0] != 0x07230203u) return out;
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> member_index;   // struct -> name -> member
    std::unordered_map<uint64_t, uint32_t> member_offset;
    std::unordered_map<uint32_t, uint32_t> sets, bindings, pointee;
    struct Var { uint32_t type, id; };
    std::vector<Var> vars;
    for (size_t i = 5; i < count;)
    {
        const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        if (wc == 0 || i + wc > count) break;
        const uint32_t *o = w + i;
        switch (op)
        {
        case 6:  if (wc >= 4) member_index[o[1]][reinterpret_cast<const char *>(o + 3)] = o[2]; break;           // OpMemberName
        case 71: if (wc >= 4) { if (o[2] == 34) sets[o[1]] = o[3]; else if (o[2] == 33) bindings[o[1]] = o[3]; } break;   // OpDecorate
        case 72: if (wc >= 5 && o[3] == 35) member_offset[(uint64_t(o[1]) << 32) | o[2]] = o[4]; break;          // OpMemberDecorate Offset
        case 32: if (wc >= 4) pointee[o[1]] = o[3]; break;                                                       // OpTypePointer
        case 59: if (wc >= 4) vars.push_back({ o[1], o[2] }); break;                                             // OpVariable
        }
        i += wc;
    }
    for (const auto &[block, members] : member_index)
    {
        const auto vp = members.find("ViewProjectionMatrix"), inv = members.find("InvViewProjectionMatrix");
        if (vp == members.end() || inv == members.end()) continue;
        const auto offset = [&](const char *name) -> uint32_t {
            const auto m = members.find(name);
            if (m == members.end()) return UINT32_MAX;
            const auto it = member_offset.find((uint64_t(block) << 32) | m->second);
            return it != member_offset.end() ? it->second : UINT32_MAX;
        };
        for (const Var &v : vars)
        {
            const auto pointer = pointee.find(v.type);
            if (pointer == pointee.end() || pointer->second != block || !bindings.count(v.id)) continue;
            out.found = true;
            out.set = sets.count(v.id) ? sets[v.id] : 0;
            out.binding = bindings[v.id];
            out.vp = offset("ViewProjectionMatrix");
            out.inv_vp = offset("InvViewProjectionMatrix");
            out.z_near = offset("ZNear");
            out.z_far = offset("ZFar");
            out.fov = offset("CameraFoV");
            return out;
        }
    }
    return out;
}
uint32_t g_next_image = 1;

// Names DXC leaves in CK3's SPIR-V, used to label draws by shader family.
const char *const kTagNames[] = { "PortraitUVOffset", "PdxCamera", "ViewProjectionMatrix", "LeftTop_WidthHeight",
                                  "Gui", "Tonemap", "Bloom", "HdrLuma", "DepthBuffer", "MapObject", "Terrain",
                                  "Shadow", "Fxaa", "Exposure" };
constexpr uint32_t kTagCount = sizeof(kTagNames) / sizeof(kTagNames[0]);

uint32_t ScanTags(const void *code, size_t bytes)
{
    uint32_t tags = 0;
    const char *const begin = static_cast<const char *>(code), *const end = begin + bytes;
    for (uint32_t i = 0; i < kTagCount; ++i)
    {
        const size_t n = strlen(kTagNames[i]);
        for (const char *p = begin; p + n <= end; ++p)
        {
            p = static_cast<const char *>(memchr(p, kTagNames[i][0], end - p));
            if (p == nullptr || p + n > end) break;
            if (memcmp(p, kTagNames[i], n) == 0) { tags |= 1u << i; break; }
        }
    }
    return tags;
}

std::string TagText(uint32_t tags)
{
    if (tags == 0) return "-";
    std::string s;
    for (uint32_t i = 0; i < kTagCount; ++i)
        if (tags & (1u << i)) { if (!s.empty()) s += '+'; s += kTagNames[i]; }
    return s;
}

bool IsDepthFormat(uint32_t f) { return f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }

// ---------------------------------------------------------------------------
// Per command buffer state
// ---------------------------------------------------------------------------

struct PassStats {
    std::string head;
    std::unordered_map<uint32_t, uint32_t> draws_by_tags;
    std::unordered_map<VkPipeline, int> pipelines;
    uint32_t draws = 0, dyn_draws = 0;
};

struct CbState {
    std::string text;
    bool in_pass = false;
    PassStats pass;
    uint32_t tags = 0;
    bool dyn_viewport = false;
    VkViewport viewport{};       // as the game set it
    VkRect2D scissor{};          // as the game set it
    float pass_sx = 1, pass_sy = 1;   // scale of the attachments of the open render pass
    bool jitter_pass = false;    // inside a scene pass at render size
    bool scene_pass = false;     // inside a depth-tested pass at scene size or larger
    VkImage swap_image = VK_NULL_HANDLE;   // swapchain image this command buffer rendered to
    bool camera_pass = false;              // inside the pass whose depth FSR will use
    VkPipeline pipeline = VK_NULL_HANDLE;
    struct BoundSet { VkDescriptorSet set = VK_NULL_HANDLE; std::vector<uint32_t> dynamic_offsets; };
    std::vector<BoundSet> bound_sets;
    std::vector<VkImage> scaled_images;    // CK3_FSR_DUMP_PORTRAIT: single-sample scaled targets, in order of use
    VkImage portrait_final = VK_NULL_HANDLE;   // the open pass writes this portrait's final image
    std::vector<std::pair<VkImage, VkImageLayout>> pass_finals;
    uint32_t dispatches = 0;
};

std::unordered_map<VkCommandBuffer, CbState *> g_cbs;

CbState *Cb(VkCommandBuffer cb)
{
    thread_local VkCommandBuffer cached = VK_NULL_HANDLE;
    thread_local CbState *cached_state = nullptr;
    if (cached == cb) return cached_state;
    std::lock_guard lock(g_mx);
    CbState *&slot = g_cbs[cb];
    if (slot == nullptr) slot = new CbState();
    cached = cb;
    cached_state = slot;
    return slot;
}

std::string ImageText(VkImage image)
{
    const auto it = g_images.find(image);
    if (it == g_images.end()) return "?";
    const ImageInfo &i = it->second;
    return Fmt("#%u%s %ux%u f%u u%x s%u m%u l%u", i.id, i.swap ? "(swap)" : "", i.w, i.h, i.fmt, i.usage, i.samples, i.mips, i.layers);
}

void FlushDispatches(CbState *s)
{
    if (s->dispatches == 0) return;
    s->text += Fmt("  DISPATCH x%u\n", s->dispatches);
    s->dispatches = 0;
}

// ---------------------------------------------------------------------------
// FSR state
// ---------------------------------------------------------------------------

struct FrameState {
    VkImage color = VK_NULL_HANDLE;   // render-size tonemapped target
    VkImage depth = VK_NULL_HANDLE;   // sampled scene depth at render size
    bool ran = false;
    bool camera_read = false;
};

FrameState g_frame_state;
uint32_t g_render_w = 1280, g_render_h = 720;
float g_sharpness = 0.5f;
float g_portrait_scale = 1;
bool g_dump_portrait;
bool g_spatial_mode, g_spatial_failed;
float g_rcas_scene = 0.25f, g_rcas_portrait = 0.35f;   // stops of sharpness reduction: 0 is the strongest
int g_scaled_pass_logs;
std::set<std::pair<uint32_t, uint32_t>> g_portrait_sizes;
bool g_ffx_debug;

struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Fsr {
    HMODULE dll = nullptr;
    PfnFfxCreate create = nullptr;
    PfnFfxDestroy destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    void *context = nullptr;
    FfxBackendVK backend{};
    FfxOverrideVersion override_version{};
    bool ready = false, failed = false;
    uint32_t out_w = 0, out_h = 0;
    OwnedImage color_srgb, color_linear, motion, output, output_srgb;
    bool motion_cleared = false;
    VkQueryPool queries = VK_NULL_HANDLE;
    double timestamp_ns = 1;
    int32_t phases = 0;
    uint32_t jitter_index = 0;
    float jx = 0, jy = 0;
    uint64_t last_run_frame = 0;
    uint64_t dispatches = 0;
    bool logged_draw = false;
} g_fsr;

struct Camera { bool ok = false; float vp[16]{}, inv_vp[16]{}; float z_near = 20, z_far = 5000, fov = 0.7f; };
Camera g_camera, g_previous_camera;
bool g_no_motion;
bool g_camera_logged;

struct MotionPass {
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageView motion_view = VK_NULL_HANDLE;
    VkDescriptorSet sets[4]{};
    std::unordered_map<VkImage, VkImageView> depth_views;
    bool ready = false;
} g_motion;

VkCommandBuffer g_skip_cb = VK_NULL_HANDLE;   // the next fullscreen draw here is the game's upscale
uint64_t g_present_count;

LARGE_INTEGER g_qpf, g_csv_start, g_last_present;
double g_delta_ms = 16.6667;
double g_record_ms;
double g_gpu_ms;
uint32_t g_scene_draws;   // this frame; lets the measurement script tell the map from menus and loading
uint32_t g_fsr_success;
uint32_t g_swap_w, g_swap_h;
FILE *g_csv;
std::string g_gpu_name;

uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties props{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_next_gipa(g_instances.back(), "vkGetPhysicalDeviceMemoryProperties"))(g_gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

bool MakeImage(OwnedImage &out, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (next.CreateImage(g_device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    next.GetImageMemoryRequirements(g_device, out.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (next.AllocateMemory(g_device, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    return next.BindImageMemory(g_device, out.image, out.memory, 0) == VK_SUCCESS;
}

void FreeImage(OwnedImage &i)
{
    if (i.image) next.DestroyImage(g_device, i.image, nullptr);
    if (i.memory) next.FreeMemory(g_device, i.memory, nullptr);
    i = {};
}

void VKAPI_CALL NoopLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
void VKAPI_CALL NoopEndLabel(VkCommandBuffer) {}
VkResult VKAPI_CALL NoopName(VkDevice, const VkDebugUtilsObjectNameInfoEXT *) { return VK_SUCCESS; }

// FFX resolves device functions through this. The SDK calls the debug-label functions unconditionally
// and the KHR aliases of promoted functions; CK3's device has neither, so both are answered here.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FfxDeviceProcAddr(VkDevice device, const char *name)
{
    if (const PFN_vkVoidFunction f = g_next_gdpa(device, name)) return f;
    if (strcmp(name, "vkCmdBeginDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(NoopLabel);
    if (strcmp(name, "vkCmdEndDebugUtilsLabelEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(NoopEndLabel);
    if (strcmp(name, "vkSetDebugUtilsObjectNameEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(NoopName);
    std::string alias(name);
    if (alias.size() > 3 && alias.compare(alias.size() - 3, 3, "KHR") == 0)
    {
        alias.resize(alias.size() - 3);
        return g_next_gdpa(device, alias.c_str());
    }
    return nullptr;
}

void FfxLogMessage(uint32_t type, const wchar_t *message)
{
    char text[512];
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text), nullptr, nullptr);
    Log("[ffx %u] %s", type, text);
}

// FFX calls vkGetPhysicalDevice* straight through the loader, which wants the handle the application
// sees, not the one layers receive. Find it through the loader by the device UUID.
VkPhysicalDevice LoaderPhysicalDevice()
{
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    if (loader == nullptr) return VK_NULL_HANDLE;
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetProcAddress(loader, "vkEnumeratePhysicalDevices"));
    const auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(GetProcAddress(loader, "vkGetPhysicalDeviceProperties2"));
    if (enumerate == nullptr || props2 == nullptr) return VK_NULL_HANDLE;

    VkPhysicalDeviceIDProperties mine{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 wrap{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &mine };
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(g_next_gipa(g_instances.back(), "vkGetPhysicalDeviceProperties2"))(g_gpu, &wrap);

    for (auto it = g_instances.rbegin(); it != g_instances.rend(); ++it)
    {
        uint32_t n = 0;
        if (enumerate(*it, &n, nullptr) != VK_SUCCESS || n == 0) continue;
        std::vector<VkPhysicalDevice> gpus(n);
        if (enumerate(*it, &n, gpus.data()) != VK_SUCCESS) continue;
        for (VkPhysicalDevice gpu : gpus)
        {
            VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 p{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
            props2(gpu, &p);
            if (memcmp(id.deviceUUID, mine.deviceUUID, VK_UUID_SIZE) == 0) return gpu;
        }
    }
    return VK_NULL_HANDLE;
}

void FailFsr(const char *why)
{
    g_fsr.failed = true;
    Log("FSR disabled; the game's own upscale stays in place: %s", why);
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
void Barriers(VkCommandBuffer cb, std::initializer_list<VkImageMemoryBarrier> list);

bool InitMotionPass()
{
    MotionPass &m = g_motion;
    VkShaderModuleCreateInfo module{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    module.codeSize = sizeof(kCameraMotionSpv);
    module.pCode = kCameraMotionSpv;
    if (next.CreateShaderModule(g_device, &module, nullptr, &m.module) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (next.CreateSampler(g_device, &sampler, nullptr, &m.sampler) != VK_SUCCESS) return false;

    const VkDescriptorSetLayoutBinding bindings[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &m.sampler },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo set_layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set_layout.bindingCount = 2;
    set_layout.pBindings = bindings;
    if (next.CreateDescriptorSetLayout(g_device, &set_layout, nullptr, &m.set_layout) != VK_SUCCESS) return false;

    const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 72 };
    VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (next.CreatePipelineLayout(g_device, &layout, nullptr, &m.layout) != VK_SUCCESS) return false;

    VkComputePipelineCreateInfo pipeline{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeline.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = m.module;
    pipeline.stage.pName = "main";
    pipeline.layout = m.layout;
    if (next.CreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &m.pipeline) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 } };
    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 4;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (next.CreateDescriptorPool(g_device, &pool, nullptr, &m.pool) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout layouts[4] = { m.set_layout, m.set_layout, m.set_layout, m.set_layout };
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = m.pool;
    allocate.descriptorSetCount = 4;
    allocate.pSetLayouts = layouts;
    if (next.AllocateDescriptorSets(g_device, &allocate, m.sets) != VK_SUCCESS) return false;

    VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = g_fsr.motion.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R16G16_SFLOAT;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (next.CreateImageView(g_device, &view, nullptr, &m.motion_view) != VK_SUCCESS) return false;
    m.ready = true;
    return true;
}

void DestroyMotionPass()
{
    MotionPass &m = g_motion;
    for (const auto &[image, view] : m.depth_views) next.DestroyImageView(g_device, view, nullptr);
    if (m.motion_view) next.DestroyImageView(g_device, m.motion_view, nullptr);
    if (m.pool) next.DestroyDescriptorPool(g_device, m.pool, nullptr);
    if (m.pipeline) next.DestroyPipeline(g_device, m.pipeline, nullptr);
    if (m.layout) next.DestroyPipelineLayout(g_device, m.layout, nullptr);
    if (m.set_layout) next.DestroyDescriptorSetLayout(g_device, m.set_layout, nullptr);
    if (m.sampler) next.DestroySampler(g_device, m.sampler, nullptr);
    if (m.module) next.DestroyShaderModule(g_device, m.module, nullptr);
    m = MotionPass{};
}

// Row-major 4x4: a * b, in the sense of HLSL mul(a, b).
void Multiply(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = sum;
        }
}

// Depth is in SHADER_READ_ONLY when this runs. Returns false to fall back on all-zero motion.
bool RecordCameraMotion(VkCommandBuffer cb, VkImage depth, uint32_t slot)
{
    if (g_no_motion || !g_motion.ready || !g_camera.ok || !g_previous_camera.ok) return false;
    VkImageView &depth_view = g_motion.depth_views[depth];
    if (depth_view == VK_NULL_HANDLE)
    {
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = depth;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_D24_UNORM_S8_UINT;
        view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (next.CreateImageView(g_device, &view, nullptr, &depth_view) != VK_SUCCESS) { depth_view = VK_NULL_HANDLE; return false; }
    }
    const VkDescriptorImageInfo depth_info{ VK_NULL_HANDLE, depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo motion_info{ VK_NULL_HANDLE, g_motion.motion_view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    writes[0].dstSet = writes[1].dstSet = g_motion.sets[slot];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &depth_info;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &motion_info;
    next.UpdateDescriptorSets(g_device, 2, writes, 0, nullptr);

    struct { float reproject[16]; float size[2]; } constants{};
    Multiply(g_previous_camera.vp, g_camera.inv_vp, constants.reproject);
    constants.size[0] = static_cast<float>(g_render_w);
    constants.size[1] = static_cast<float>(g_render_h);

    const VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    Barriers(cb, { Barrier(g_fsr.motion.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, all) });
    next.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_motion.pipeline);
    next.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_motion.layout, 0, 1, &g_motion.sets[slot], 0, nullptr);
    next.CmdPushConstants(cb, g_motion.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    next.CmdDispatch(cb, (g_render_w + 7) / 8, (g_render_h + 7) / 8, 1);
    Barriers(cb, { Barrier(g_fsr.motion.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, all, all) });
    return true;
}

bool InitFsr(uint32_t out_w, uint32_t out_h)
{
    if (g_fsr.failed) return false;
    if (g_fsr.ready) return g_fsr.out_w == out_w && g_fsr.out_h == out_h;

    const std::wstring runtime = EnvW(L"CK3_FSR_RUNTIME");
    g_fsr.dll = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (g_fsr.dll == nullptr) { FailFsr(Fmt("cannot load the FSR runtime (Win32 %lu)", GetLastError()).c_str()); return false; }
    g_fsr.create = reinterpret_cast<PfnFfxCreate>(GetProcAddress(g_fsr.dll, "ffxCreateContext"));
    g_fsr.destroy = reinterpret_cast<PfnFfxDestroy>(GetProcAddress(g_fsr.dll, "ffxDestroyContext"));
    g_fsr.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_fsr.dll, "ffxQuery"));
    g_fsr.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_fsr.dll, "ffxDispatch"));
    if (!g_fsr.create || !g_fsr.destroy || !g_fsr.query || !g_fsr.dispatch) { FailFsr("runtime exports missing"); return false; }

    const VkPhysicalDevice loader_gpu = LoaderPhysicalDevice();
    if (loader_gpu == VK_NULL_HANDLE) { FailFsr("could not resolve the loader's physical device"); return false; }

    // Select the pinned 3.1.3 provider explicitly; there is no fallback.
    uint64_t count = 0;
    FfxGetVersions versions{ { 4, nullptr }, 0x10000, nullptr, &count, nullptr, nullptr };
    if (g_fsr.query(nullptr, &versions.header) != 0 || count == 0 || count > 32) { FailFsr("cannot enumerate FSR providers"); return false; }
    uint64_t ids[32] = {};
    const char *names[32] = {};
    versions.ids = ids;
    versions.names = names;
    if (g_fsr.query(nullptr, &versions.header) != 0) { FailFsr("cannot enumerate FSR providers"); return false; }
    uint64_t provider = 0;
    for (uint64_t i = 0; i < count; ++i)
        if (strcmp(names[i], "3.1.3") == 0) provider = ids[i];
    if (provider == 0) { FailFsr("FSR 3.1.3 provider not reported"); return false; }

    // VK_IMAGE_USAGE values spelled out where each image's use is obvious from the sequence in RunFsr.
    const VkImageUsageFlags xfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!MakeImage(g_fsr.color_srgb, g_render_w, g_render_h, VK_FORMAT_B8G8R8A8_SRGB, xfer) ||
        !MakeImage(g_fsr.color_linear, g_render_w, g_render_h, VK_FORMAT_R16G16B16A16_SFLOAT, xfer | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !MakeImage(g_fsr.motion, g_render_w, g_render_h, VK_FORMAT_R16G16_SFLOAT, xfer | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT) ||
        !MakeImage(g_fsr.output, out_w, out_h, VK_FORMAT_R16G16B16A16_SFLOAT, xfer | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT) ||
        !MakeImage(g_fsr.output_srgb, out_w, out_h, VK_FORMAT_B8G8R8A8_SRGB, xfer))
    {
        FailFsr("could not allocate FSR images");
        return false;
    }

    VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 8;
    if (next.CreateQueryPool(g_device, &qi, nullptr, &g_fsr.queries) != VK_SUCCESS) g_fsr.queries = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(g_next_gipa(g_instances.back(), "vkGetPhysicalDeviceProperties"))(g_gpu, &props);
    g_fsr.timestamp_ns = props.limits.timestampPeriod;

    g_fsr.override_version = { { 5, nullptr }, provider };
    g_fsr.backend = { { 3, &g_fsr.override_version.header }, g_device, loader_gpu, FfxDeviceProcAddr };
    FfxCreateUpscale ci{ { 0x10000, &g_fsr.backend.header }, kFfxAutoExposure | (g_ffx_debug ? kFfxDebugChecking : 0u),
                         { g_render_w, g_render_h }, { out_w, out_h }, FfxLogMessage };
    const auto start = std::chrono::steady_clock::now();
    const uint32_t rc = g_fsr.create(&g_fsr.context, &ci.header, nullptr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (rc != 0) { FailFsr(Fmt("ffxCreateContext returned %u", rc).c_str()); return false; }

    FfxJitterPhases phases{ { 0x10004, nullptr }, g_render_w, out_w, &g_fsr.phases };
    if (g_fsr.query(&g_fsr.context, &phases.header) != 0 || g_fsr.phases <= 0) { FailFsr("jitter phase query failed"); return false; }

    g_fsr.out_w = out_w;
    g_fsr.out_h = out_h;
    g_fsr.ready = true;
    if (!InitMotionPass()) Log("the camera motion pass could not be created; FSR runs with zero motion");
    Log("FSR 3.1.3 context created in %.1f ms: %ux%u -> %ux%u, %d jitter phases, sharpness %.2f", ms, g_render_w, g_render_h, out_w, out_h, g_fsr.phases, g_sharpness);
    return true;
}

void DestroyFsr()
{
    if (g_fsr.context != nullptr)
    {
        next.DeviceWaitIdle(g_device);
        g_fsr.destroy(&g_fsr.context, nullptr);
        g_fsr.context = nullptr;
    }
    if (g_device == VK_NULL_HANDLE) return;
    DestroyMotionPass();
    for (OwnedImage *i : { &g_fsr.color_srgb, &g_fsr.color_linear, &g_fsr.motion, &g_fsr.output, &g_fsr.output_srgb }) FreeImage(*i);
    if (g_fsr.queries) next.DestroyQueryPool(g_device, g_fsr.queries, nullptr);
    g_fsr.queries = VK_NULL_HANDLE;
    g_fsr.ready = false;
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                             VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    return b;
}

void Barriers(VkCommandBuffer cb, std::initializer_list<VkImageMemoryBarrier> list)
{
    next.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                            static_cast<uint32_t>(list.size()), list.begin());
}

FfxResource FfxImage(VkImage image, uint32_t format, uint32_t w, uint32_t h, uint32_t usage, uint32_t state)
{
    FfxResource r{};
    r.resource = image;
    r.desc = { kFfxTexture2D, format, w, h, 1, 1, 0, usage };
    r.state = state;
    return r;
}

// Recorded on the game's command buffer, outside any render pass, right before the first swapchain pass.
bool RunFsr(VkCommandBuffer cb, VkImage swap, VkImageLayout swap_layout, uint32_t out_w, uint32_t out_h)
{
    VkImage color, depth;
    VkImageLayout color_layout, depth_layout;
    {
        std::lock_guard lock(g_mx);
        color = g_frame_state.color;
        depth = g_frame_state.depth;
        color_layout = g_images[color].layout;
        depth_layout = g_images[depth].layout;
    }
    if (swap_layout == VK_IMAGE_LAYOUT_UNDEFINED || color_layout == VK_IMAGE_LAYOUT_UNDEFINED || depth_layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        Log("skipping a frame: untracked layout (swap %d colour %d depth %d)", swap_layout, color_layout, depth_layout);
        return false;
    }
    if (!InitFsr(out_w, out_h)) return false;

    LARGE_INTEGER begin, end;
    QueryPerformanceCounter(&begin);

    const VkAccessFlags all_write = VK_ACCESS_MEMORY_WRITE_BIT, all_read = VK_ACCESS_MEMORY_READ_BIT;
    const VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    if (!g_fsr.motion_cleared)
    {
        Barriers(cb, { Barrier(g_fsr.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, all_write | all_read) });
        g_fsr.motion_cleared = true;
    }

    // Colour in: the target holds sRGB-encoded values in a UNORM image and FSR wants linear input.
    // A same-class copy into an sRGB image followed by a blit to float decodes it on the GPU.
    Barriers(cb, { Barrier(color, color_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all_write, all_read),
                   Barrier(depth, depth_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, all_write, all_read, depth_aspect),
                   Barrier(g_fsr.color_srgb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, all_write),
                   Barrier(g_fsr.color_linear.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, all_write) });
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.extent = { g_render_w, g_render_h, 1 };
    next.CmdCopyImage(cb, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fsr.color_srgb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barriers(cb, { Barrier(g_fsr.color_srgb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all_write, all_read) });
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = blit.dstOffsets[1] = { static_cast<int32_t>(g_render_w), static_cast<int32_t>(g_render_h), 1 };
    next.CmdBlitImage(cb, g_fsr.color_srgb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fsr.color_linear.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    Barriers(cb, { Barrier(g_fsr.color_linear.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, all_write, all_read),
                   Barrier(color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_layout, all_read, all_read | all_write) });

    const uint32_t frame_slot = static_cast<uint32_t>(g_fsr.dispatches % 4);
    if (!RecordCameraMotion(cb, depth, frame_slot))
    {
        // No camera yet (or motion disabled): all-zero motion, right for a static view.
        Barriers(cb, { Barrier(g_fsr.motion.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, all_write) });
        const VkClearColorValue zero{};
        const VkImageSubresourceRange all{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        next.CmdClearColorImage(cb, g_fsr.motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        Barriers(cb, { Barrier(g_fsr.motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, all_write, all_read) });
    }
    const uint32_t slot = frame_slot * 2;
    if (g_fsr.queries != VK_NULL_HANDLE)
    {
        next.CmdResetQueryPool(cb, g_fsr.queries, slot, 2);
        next.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_fsr.queries, slot);
    }

    const bool reset = g_fsr.dispatches == 0 || g_present_count > g_fsr.last_run_frame + 1;
    FfxDispatchUpscale d{};
    d.header = { 0x10001, nullptr };
    d.commandList = cb;
    d.color = FfxImage(g_fsr.color_linear.image, kFfxRgba16F, g_render_w, g_render_h, 0, kFfxStateComputeRead);
    d.depth = FfxImage(depth, kFfxR32Uint, g_render_w, g_render_h, kFfxUsageDepth | kFfxUsageStencil, kFfxStateComputeRead);
    d.motion = FfxImage(g_fsr.motion.image, kFfxRg16F, g_render_w, g_render_h, 0, kFfxStateComputeRead);
    d.output = FfxImage(g_fsr.output.image, kFfxRgba16F, out_w, out_h, kFfxUsageUav, kFfxStateUav);
    d.jitter = { g_fsr.jx, g_fsr.jy };
    d.motionScale = { 1, 1 };
    d.renderSize = { g_render_w, g_render_h };
    d.upscaleSize = { out_w, out_h };
    d.sharpen = g_sharpness > 0;
    d.sharpness = g_sharpness;
    d.frameTimeDelta = static_cast<float>(std::clamp(g_delta_ms, 1.0, 250.0));
    d.preExposure = 1;
    d.reset = reset;
    d.cameraNear = g_camera.z_near;
    d.cameraFar = g_camera.z_far;
    d.cameraFov = g_camera.fov;
    d.metersFactor = 1;
    const uint32_t rc = g_fsr.dispatch(&g_fsr.context, &d.header);

    if (g_fsr.queries != VK_NULL_HANDLE)
        next.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_fsr.queries, slot + 1);

    Barriers(cb, { Barrier(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth_layout, all_read, all_read | all_write, depth_aspect) });
    if (rc != 0)
    {
        FailFsr(Fmt("ffxDispatch returned %u", rc).c_str());
        return false;
    }

    // Colour out: the mirror image of the way in, ending on the swapchain image in the layout the pass expects.
    Barriers(cb, { Barrier(g_fsr.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all_write, all_read),
                   Barrier(g_fsr.output_srgb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, all_write) });
    blit.srcOffsets[1] = blit.dstOffsets[1] = { static_cast<int32_t>(out_w), static_cast<int32_t>(out_h), 1 };
    next.CmdBlitImage(cb, g_fsr.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fsr.output_srgb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    Barriers(cb, { Barrier(g_fsr.output_srgb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all_write, all_read),
                   Barrier(swap, swap_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, all_read | all_write, all_write),
                   Barrier(g_fsr.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, all_read, all_read | all_write) });
    copy.extent = { out_w, out_h, 1 };
    next.CmdCopyImage(cb, g_fsr.output_srgb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barriers(cb, { Barrier(swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, swap_layout, all_write, all_read | all_write) });

    QueryPerformanceCounter(&end);
    g_record_ms = 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) / static_cast<double>(g_qpf.QuadPart);
    g_fsr.last_run_frame = g_present_count;
    ++g_fsr.dispatches;
    g_fsr_success = 1;
    if (g_fsr.dispatches == 1 || g_fsr.dispatches % 3600 == 0)
        Log("FSR dispatched %llu times; jitter %.3f,%.3f; record %.2f ms; last GPU %.2f ms", g_fsr.dispatches, g_fsr.jx, g_fsr.jy, g_record_ms, g_gpu_ms);
    return true;
}

// ---------------------------------------------------------------------------
// FSR 1 spatial pass
// ---------------------------------------------------------------------------

struct SpatialPush { uint32_t con[16]; uint32_t size[4]; };

struct Spatial {
    VkShaderModule easu_module = VK_NULL_HANDLE, rcas_module = VK_NULL_HANDLE;
    VkPipeline easu = VK_NULL_HANDLE, rcas = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet sets[256]{};
    uint32_t next_set = 0;
    OwnedImage upscaled, sharpened;
    VkImageView upscaled_view = VK_NULL_HANDLE, sharpened_view = VK_NULL_HANDLE;
    uint32_t out_w = 0, out_h = 0;
    std::unordered_map<VkImage, VkImageView> views;   // the game's images we read
    struct Scratch { OwnedImage image; VkImageView view = VK_NULL_HANDLE; };
    std::unordered_map<uint64_t, Scratch> portrait_scratch;   // by size
    bool ready = false;
    uint64_t frames = 0;
    uint64_t portraits = 0;
    uint32_t portraits_this_frame = 0;
} g_spatial;

VkImageView MakeColorView(VkImage image, VkFormat format)
{
    VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView out = VK_NULL_HANDLE;
    return next.CreateImageView(g_device, &view, nullptr, &out) == VK_SUCCESS ? out : VK_NULL_HANDLE;
}

bool MakeComputePipeline(const uint32_t *code, size_t bytes, VkShaderModule &module, VkPipeline &pipeline)
{
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = bytes;
    info.pCode = code;
    if (next.CreateShaderModule(g_device, &info, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = g_spatial.layout;
    return next.CreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) == VK_SUCCESS;
}

bool InitSpatial()
{
    Spatial &m = g_spatial;
    if (m.ready) return true;
    if (g_spatial_failed) return false;
    g_spatial_failed = true;   // until everything below succeeds

    VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (next.CreateSampler(g_device, &sampler, nullptr, &m.sampler) != VK_SUCCESS) return false;

    const VkDescriptorSetLayoutBinding bindings[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &m.sampler },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo set_layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set_layout.bindingCount = 2;
    set_layout.pBindings = bindings;
    if (next.CreateDescriptorSetLayout(g_device, &set_layout, nullptr, &m.set_layout) != VK_SUCCESS) return false;

    const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SpatialPush) };
    VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (next.CreatePipelineLayout(g_device, &layout, nullptr, &m.layout) != VK_SUCCESS) return false;
    if (!MakeComputePipeline(kEasuSpv, sizeof(kEasuSpv), m.easu_module, m.easu)) return false;
    if (!MakeComputePipeline(kRcasSpv, sizeof(kRcasSpv), m.rcas_module, m.rcas)) return false;

    constexpr uint32_t set_count = sizeof(m.sets) / sizeof(m.sets[0]);
    const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, set_count }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, set_count } };
    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = set_count;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (next.CreateDescriptorPool(g_device, &pool, nullptr, &m.pool) != VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayout> layouts(set_count, m.set_layout);
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = m.pool;
    allocate.descriptorSetCount = set_count;
    allocate.pSetLayouts = layouts.data();
    if (next.AllocateDescriptorSets(g_device, &allocate, m.sets) != VK_SUCCESS) return false;

    g_spatial_failed = false;
    m.ready = true;
    Log("FSR 1 spatial pass ready: scene RCAS %.2f, portrait RCAS %.2f stops", g_rcas_scene, g_rcas_portrait);
    return true;
}

void DestroySpatial()
{
    Spatial &m = g_spatial;
    if (g_device == VK_NULL_HANDLE || !m.ready) return;
    next.DeviceWaitIdle(g_device);
    for (const auto &[image, view] : m.views) next.DestroyImageView(g_device, view, nullptr);
    for (auto &[size, scratch] : m.portrait_scratch)
    {
        next.DestroyImageView(g_device, scratch.view, nullptr);
        FreeImage(scratch.image);
    }
    if (m.upscaled_view) next.DestroyImageView(g_device, m.upscaled_view, nullptr);
    if (m.sharpened_view) next.DestroyImageView(g_device, m.sharpened_view, nullptr);
    FreeImage(m.upscaled);
    FreeImage(m.sharpened);
    next.DestroyDescriptorPool(g_device, m.pool, nullptr);
    next.DestroyPipeline(g_device, m.easu, nullptr);
    next.DestroyPipeline(g_device, m.rcas, nullptr);
    next.DestroyShaderModule(g_device, m.easu_module, nullptr);
    next.DestroyShaderModule(g_device, m.rcas_module, nullptr);
    next.DestroyPipelineLayout(g_device, m.layout, nullptr);
    next.DestroyDescriptorSetLayout(g_device, m.set_layout, nullptr);
    next.DestroySampler(g_device, m.sampler, nullptr);
    m = Spatial{};
}

// One compute pass. The input must be in SHADER_READ_ONLY and the output in GENERAL.
void SpatialDispatch(VkCommandBuffer cb, VkPipeline pipeline, VkImageView input, VkImageView output, const SpatialPush &push, uint32_t w, uint32_t h)
{
    Spatial &m = g_spatial;
    const VkDescriptorSet set = m.sets[m.next_set];
    m.next_set = (m.next_set + 1) % (sizeof(m.sets) / sizeof(m.sets[0]));
    const VkDescriptorImageInfo input_info{ VK_NULL_HANDLE, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo output_info{ VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    writes[0].dstSet = writes[1].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &input_info;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &output_info;
    next.UpdateDescriptorSets(g_device, 2, writes, 0, nullptr);
    next.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    next.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &set, 0, nullptr);
    next.CmdPushConstants(cb, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    next.CmdDispatch(cb, (w + 15) / 16, (h + 15) / 16, 1);
}

VkImageView SpatialView(VkImage image, VkFormat format)
{
    VkImageView &view = g_spatial.views[image];
    if (view == VK_NULL_HANDLE) view = MakeColorView(image, format);
    return view;
}

// Replaces the game's bilinear upscale. Recorded right before the first swapchain pass.
bool RunSpatial(VkCommandBuffer cb, VkImage swap, VkImageLayout swap_layout, uint32_t out_w, uint32_t out_h)
{
    VkImage color;
    VkImageLayout color_layout;
    uint32_t in_w, in_h;
    {
        std::lock_guard lock(g_mx);
        color = g_frame_state.color;
        const ImageInfo &info = g_images[color];
        color_layout = info.layout;
        in_w = info.w;
        in_h = info.h;
    }
    if (swap_layout == VK_IMAGE_LAYOUT_UNDEFINED || color_layout == VK_IMAGE_LAYOUT_UNDEFINED || !InitSpatial()) return false;
    Spatial &m = g_spatial;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (m.out_w != out_w || m.out_h != out_h)
    {
        next.DeviceWaitIdle(g_device);
        if (m.upscaled_view) next.DestroyImageView(g_device, m.upscaled_view, nullptr);
        if (m.sharpened_view) next.DestroyImageView(g_device, m.sharpened_view, nullptr);
        FreeImage(m.upscaled);
        FreeImage(m.sharpened);
        m.upscaled_view = m.sharpened_view = VK_NULL_HANDLE;
        if (!MakeImage(m.upscaled, out_w, out_h, VK_FORMAT_R8G8B8A8_UNORM, usage) || !MakeImage(m.sharpened, out_w, out_h, VK_FORMAT_R8G8B8A8_UNORM, usage)) { g_spatial_failed = true; Log("could not allocate the spatial pass images"); return false; }
        m.upscaled_view = MakeColorView(m.upscaled.image, VK_FORMAT_R8G8B8A8_UNORM);
        m.sharpened_view = MakeColorView(m.sharpened.image, VK_FORMAT_R8G8B8A8_UNORM);
        m.out_w = out_w;
        m.out_h = out_h;
    }
    const VkImageView color_view = SpatialView(color, VK_FORMAT_B8G8R8A8_UNORM);
    if (color_view == VK_NULL_HANDLE || m.upscaled_view == VK_NULL_HANDLE || m.sharpened_view == VK_NULL_HANDLE) return false;

    LARGE_INTEGER begin, end;
    QueryPerformanceCounter(&begin);
    const VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    Barriers(cb, { Barrier(color, color_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, all, all),
                   Barrier(m.upscaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, all) });
    SpatialPush easu{};
    FsrEasuCon(&easu.con[0], &easu.con[4], &easu.con[8], &easu.con[12], static_cast<AF1>(in_w), static_cast<AF1>(in_h),
               static_cast<AF1>(in_w), static_cast<AF1>(in_h), static_cast<AF1>(out_w), static_cast<AF1>(out_h));
    easu.size[0] = out_w;
    easu.size[1] = out_h;
    SpatialDispatch(cb, m.easu, color_view, m.upscaled_view, easu, out_w, out_h);

    Barriers(cb, { Barrier(m.upscaled.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, all, all),
                   Barrier(m.sharpened.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, all),
                   Barrier(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_layout, all, all) });
    SpatialPush rcas{};
    FsrRcasCon(&rcas.con[0], g_rcas_scene);
    rcas.con[4] = out_w;
    rcas.con[5] = out_h;
    SpatialDispatch(cb, m.rcas, m.upscaled_view, m.sharpened_view, rcas, out_w, out_h);

    Barriers(cb, { Barrier(m.sharpened.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, all),
                   Barrier(swap, swap_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, all, all) });
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = blit.dstOffsets[1] = { static_cast<int32_t>(out_w), static_cast<int32_t>(out_h), 1 };
    next.CmdBlitImage(cb, m.sharpened.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    Barriers(cb, { Barrier(swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, swap_layout, all, all) });

    QueryPerformanceCounter(&end);
    g_record_ms = 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) / static_cast<double>(g_qpf.QuadPart);
    if (++m.frames == 1) Log("FSR 1 scene upscale: %ux%u -> %ux%u", in_w, in_h, out_w, out_h);
    return true;
}

// Sharpens a portrait right after its final pass. Bounded per frame so the descriptor ring stays safe.
void SharpenPortrait(VkCommandBuffer cb, VkImage image)
{
    if (!InitSpatial() || g_spatial.portraits_this_frame >= 48) return;
    Spatial &m = g_spatial;
    uint32_t w, h;
    VkImageLayout layout;
    {
        std::lock_guard lock(g_mx);
        const auto it = g_images.find(image);
        if (it == g_images.end()) return;
        w = it->second.w;
        h = it->second.h;
        layout = it->second.layout;
    }
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
    Spatial::Scratch &scratch = m.portrait_scratch[(static_cast<uint64_t>(w) << 32) | h];
    if (scratch.view == VK_NULL_HANDLE)
    {
        if (!MakeImage(scratch.image, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return;
        scratch.view = MakeColorView(scratch.image.image, VK_FORMAT_R8G8B8A8_UNORM);
        if (scratch.view == VK_NULL_HANDLE) return;
    }
    const VkImageView view = SpatialView(image, VK_FORMAT_R8G8B8A8_UNORM);
    if (view == VK_NULL_HANDLE) return;

    const VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    Barriers(cb, { Barrier(image, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, all, all),
                   Barrier(scratch.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, all) });
    SpatialPush rcas{};
    FsrRcasCon(&rcas.con[0], g_rcas_portrait);
    rcas.con[4] = w;
    rcas.con[5] = h;
    SpatialDispatch(cb, m.rcas, view, scratch.view, rcas, w, h);
    Barriers(cb, { Barrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, all, all),
                   Barrier(scratch.image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, all) });
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.extent = { w, h, 1 };
    next.CmdCopyImage(cb, scratch.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barriers(cb, { Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, all, all) });
    ++g_spatial.portraits_this_frame;
    if (++m.portraits == 1) Log("FSR 1 portrait sharpening active (first portrait %ux%u)", w, h);
}

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------

struct PassAtt { VkImage image; ImageInfo info; AttDesc desc; };

std::vector<PassAtt> ResolveAttachments(const VkRenderPassBeginInfo *info)
{
    std::vector<PassAtt> out;
    const auto rp = g_passes.find(info->renderPass);
    std::vector<VkImageView> views;
    const auto fb = g_fbs.find(info->framebuffer);
    if (fb != g_fbs.end()) views = fb->second;
    for (const auto *s = static_cast<const VkBaseInStructure *>(info->pNext); s != nullptr; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
        {
            const auto *a = reinterpret_cast<const VkRenderPassAttachmentBeginInfo *>(s);
            views.assign(a->pAttachments, a->pAttachments + a->attachmentCount);
        }
    for (size_t i = 0; i < views.size(); ++i)
    {
        const auto view = g_views.find(views[i]);
        if (view == g_views.end()) continue;
        const auto image = g_images.find(view->second);
        if (image == g_images.end()) continue;
        PassAtt att{ view->second, image->second, {} };
        if (rp != g_passes.end() && i < rp->second.size()) att.desc = rp->second[i];
        out.push_back(att);
    }
    return out;
}

void CensusImage(const char *event, VkImage image, const VkImageCreateInfo *ci);

VkViewport ScaledViewport(const CbState *s, VkViewport v)
{
    v.x *= s->pass_sx;
    v.y *= s->pass_sy;
    v.width *= s->pass_sx;
    v.height *= s->pass_sy;
    return v;
}

VkRect2D ScaledRect(const CbState *s, const VkRect2D &r)
{
    const float x0 = static_cast<float>(r.offset.x) * s->pass_sx, y0 = static_cast<float>(r.offset.y) * s->pass_sy;
    const float x1 = static_cast<float>(r.offset.x + static_cast<int32_t>(r.extent.width)) * s->pass_sx;
    const float y1 = static_cast<float>(r.offset.y + static_cast<int32_t>(r.extent.height)) * s->pass_sy;
    VkRect2D out{};
    out.offset = { static_cast<int32_t>(x0), static_cast<int32_t>(y0) };
    out.extent = { static_cast<uint32_t>(std::ceil(x1)) - static_cast<uint32_t>(out.offset.x), static_cast<uint32_t>(std::ceil(y1)) - static_cast<uint32_t>(out.offset.y) };
    return out;
}

// What the pass means for FSR. Called before the pass begins, which is the last moment compute can be recorded.
void BeforePass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, std::string *census_head)
{
    std::vector<PassAtt> atts;
    bool scene = false;
    VkImage swap = VK_NULL_HANDLE;
    VkImageLayout swap_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t swap_w = 0, swap_h = 0;
    bool run = false, run_spatial = false;
    CbState *const cb_state = Cb(cb);   // before the lock: Cb takes it on a cache miss
    {
        std::lock_guard lock(g_mx);
        atts = ResolveAttachments(info);
        for (const PassAtt &a : atts) CensusImage("attach", a.image, nullptr);
        const uint32_t w = info->renderArea.extent.width, h = info->renderArea.extent.height;
        bool has_depth = false;
        for (const PassAtt &a : atts) has_depth |= IsDepthFormat(a.info.fmt);
        cb_state->scene_pass = has_depth && w >= g_render_w && h >= g_render_h;
        if (g_dump_portrait)
            for (const PassAtt &a : atts)
                if (a.info.sx != 1 && a.info.samples == 1) cb_state->scaled_images.push_back(a.image);
        cb_state->pass_sx = atts.empty() ? 1.0f : atts[0].info.sx;
        cb_state->pass_sy = atts.empty() ? 1.0f : atts[0].info.sy;
        for (const PassAtt &a : atts)
            if (a.info.sx != cb_state->pass_sx || a.info.sy != cb_state->pass_sy)
                Log("render pass mixes scaled and unscaled attachments: %s", ImageText(a.image).c_str());
        if (!atts.empty() && atts[0].info.swap) cb_state->swap_image = atts[0].image;
        cb_state->portrait_final = VK_NULL_HANDLE;
        if (g_spatial_mode && !has_depth && atts.size() == 1 && !atts[0].info.swap && atts[0].info.samples == 1)
        {
            if (atts[0].info.fmt == VK_FORMAT_B8G8R8A8_UNORM && w * 2 >= g_swap_w && h * 2 >= g_swap_h && g_swap_w != 0)
                g_frame_state.color = atts[0].image;
            else if (atts[0].info.fmt == VK_FORMAT_R8G8B8A8_UNORM && g_portrait_sizes.count({ w, h }) != 0)
                cb_state->portrait_final = atts[0].image;
        }
        if (g_fsr_mode && w == g_render_w && h == g_render_h)
        {
            if (has_depth)
            {
                scene = true;
                for (const PassAtt &a : atts)
                    if (IsDepthFormat(a.info.fmt) && (a.info.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
                    {
                        g_frame_state.depth = a.image;
                        cb_state->camera_pass = true;
                    }
            }
            else if (atts.size() == 1 && atts[0].info.fmt == VK_FORMAT_B8G8R8A8_UNORM)
                g_frame_state.color = atts[0].image;
        }
        if (!atts.empty() && atts[0].info.swap)
        {
            g_swap_w = atts[0].info.w;
            g_swap_h = atts[0].info.h;
            if (g_fsr_mode && !g_frame_state.ran && g_frame_state.color != VK_NULL_HANDLE && g_frame_state.depth != VK_NULL_HANDLE &&
                atts[0].desc.load == VK_ATTACHMENT_LOAD_OP_LOAD)
            {
                run = true;
                swap = atts[0].image;
                swap_layout = static_cast<VkImageLayout>(atts[0].desc.initial_layout);
                swap_w = atts[0].info.w;
                swap_h = atts[0].info.h;
                g_frame_state.ran = true;
            }
            if (g_spatial_mode && !g_spatial_failed && !g_frame_state.ran && g_frame_state.color != VK_NULL_HANDLE && atts[0].desc.load == VK_ATTACHMENT_LOAD_OP_LOAD)
            {
                run_spatial = true;
                swap = atts[0].image;
                swap_layout = static_cast<VkImageLayout>(atts[0].desc.initial_layout);
                swap_w = atts[0].info.w;
                swap_h = atts[0].info.h;
                g_frame_state.ran = true;
            }
        }
        if (census_head != nullptr)
        {
            std::string head = Fmt("  PASS %ux%u", w, h);
            for (const PassAtt &a : atts)
                head += " {" + ImageText(a.image) + Fmt(" load%u store%u init%u final%u}", a.desc.load, a.desc.store, a.desc.initial_layout, a.desc.final_layout);
            *census_head = std::move(head);
        }
    }

    CbState *s = (g_census || g_fsr_mode || g_spatial_mode) ? Cb(cb) : nullptr;
    if (s != nullptr)
    {
        s->pass_finals.clear();
        for (const PassAtt &a : atts) s->pass_finals.emplace_back(a.image, static_cast<VkImageLayout>(a.desc.final_layout));
        s->jitter_pass = scene;
    }
    if (run && RunFsr(cb, swap, swap_layout, swap_w, swap_h)) g_skip_cb = cb;
    if (run_spatial && RunSpatial(cb, swap, swap_layout, swap_w, swap_h)) g_skip_cb = cb;
}

void AfterPassBegin(VkCommandBuffer cb, std::string *census_head)
{
    if (!g_census && !g_fsr_mode) return;
    CbState *s = Cb(cb);
    if (g_census)
    {
        FlushDispatches(s);
        s->pass = PassStats{};
        s->pass.head = std::move(*census_head);
        s->in_pass = true;
    }
    if (s->jitter_pass && s->viewport.width == static_cast<float>(g_render_w) && s->viewport.height == static_cast<float>(g_render_h))
    {
        VkViewport v = s->viewport;
        v.x += g_fsr.jx;
        v.y += g_fsr.jy;
        next.CmdSetViewport(cb, 0, 1, &v);
    }
    if (s->pass_sx != 1 || s->pass_sy != 1)
    {
        if (s->viewport.width > 0)
        {
            const VkViewport v = ScaledViewport(s, s->viewport);
            next.CmdSetViewport(cb, 0, 1, &v);
        }
        if (s->scissor.extent.width > 0)
        {
            const VkRect2D r = ScaledRect(s, s->scissor);
            next.CmdSetScissor(cb, 0, 1, &r);
        }
        if (g_dump_portrait && g_scaled_pass_logs < 12)
        {
            ++g_scaled_pass_logs;
            Log("scaled pass x%.3f: viewport %.1f,%.1f %.1fx%.1f depth %.2f-%.2f; scissor %d,%d %ux%u", s->pass_sx, s->viewport.x, s->viewport.y, s->viewport.width, s->viewport.height,
                s->viewport.minDepth, s->viewport.maxDepth, s->scissor.offset.x, s->scissor.offset.y, s->scissor.extent.width, s->scissor.extent.height);
        }
    }
}

void PassEnded(VkCommandBuffer cb)
{
    if (!g_track) return;
    CbState *s = Cb(cb);
    {
        std::lock_guard lock(g_mx);
        for (const auto &[image, layout] : s->pass_finals)
        {
            const auto it = g_images.find(image);
            if (it != g_images.end()) it->second.layout = layout;
        }
    }
    s->pass_finals.clear();
    s->scene_pass = false;
    s->camera_pass = false;
    if (s->portrait_final != VK_NULL_HANDLE)
    {
        const VkImage image = s->portrait_final;
        s->portrait_final = VK_NULL_HANDLE;
        SharpenPortrait(cb, image);
    }
    if (s->pass_sx != 1 || s->pass_sy != 1)
    {
        s->pass_sx = s->pass_sy = 1;
        if (s->viewport.width > 0) next.CmdSetViewport(cb, 0, 1, &s->viewport);
        if (s->scissor.extent.width > 0) next.CmdSetScissor(cb, 0, 1, &s->scissor);
    }
    if (s->jitter_pass)
    {
        s->jitter_pass = false;
        if (s->viewport.width > 0) next.CmdSetViewport(cb, 0, 1, &s->viewport);
    }
    if (g_census && s->in_pass)
    {
        s->in_pass = false;
        std::string line = s->pass.head + Fmt(" draws=%u dynvp=%u pipelines=%zu lastvp=%.1f,%.1f %.0fx%.0f",
            s->pass.draws, s->pass.dyn_draws, s->pass.pipelines.size(), s->viewport.x, s->viewport.y, s->viewport.width, s->viewport.height);
        for (const auto &[tags, count] : s->pass.draws_by_tags)
            line += Fmt(" [%s:%u]", TagText(tags).c_str(), count);
        s->text += line + "\n";
    }
}

// The swapchain pass after RunFsr: its first draw is the game's fullscreen upscale, now redundant.
bool SkipThisDraw(VkCommandBuffer cb, uint32_t count, bool indexed)
{
    if (g_skip_cb != cb) return false;
    g_skip_cb = VK_NULL_HANDLE;
    const uint32_t tags = Cb(cb)->tags;
    const bool skip = tags == 0 && count <= 6;
    if (!g_fsr.logged_draw)
    {
        g_fsr.logged_draw = true;
        Log("first swapchain draw: tags=%s count=%u indexed=%d -> %s", TagText(tags).c_str(), count, indexed, skip ? "skipped (FSR output is already there)" : "kept");
    }
    return skip;
}

// Called at a draw in the scene's depth pass, once per frame: the bytes behind the bound PdxCamera constant buffer.
void ReadCamera(const CbState *s)
{
    std::lock_guard lock(g_mx);
    const auto pipeline = g_pipelines.find(s->pipeline);
    if (pipeline == g_pipelines.end() || !pipeline->second.camera.found) return;
    const CameraBlock &block = pipeline->second.camera;
    if (block.set >= s->bound_sets.size() || block.vp == UINT32_MAX || block.inv_vp == UINT32_MAX) return;
    const CbState::BoundSet &bound = s->bound_sets[block.set];
    const auto buffers = g_set_buffers.find(bound.set);
    if (buffers == g_set_buffers.end()) return;
    const auto binding = buffers->second.find(block.binding);
    if (binding == buffers->second.end()) return;
    VkDeviceSize dynamic = 0;
    const auto layout_of = g_set_layout_of.find(bound.set);
    if (layout_of != g_set_layout_of.end())
    {
        const auto layout = g_set_layouts.find(layout_of->second);
        if (layout != g_set_layouts.end())
        {
            uint32_t index = 0;
            for (const LayoutBinding &b : layout->second)
            {
                const bool is_dynamic = b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                if (b.binding == block.binding)
                {
                    if (is_dynamic && index < bound.dynamic_offsets.size()) dynamic = bound.dynamic_offsets[index];
                    break;
                }
                if (is_dynamic) index += b.count;
            }
        }
    }
    const auto memory = g_buffer_memory.find(binding->second.buffer);
    if (memory == g_buffer_memory.end()) return;
    const auto mapped = g_mapped.find(memory->second.memory);
    if (mapped == g_mapped.end()) return;
    const char *bytes = mapped->second.base + memory->second.offset + binding->second.offset + dynamic;

    float vp[16], inv_vp[16];
    memcpy(vp, bytes + block.vp, sizeof(vp));
    memcpy(inv_vp, bytes + block.inv_vp, sizeof(inv_vp));
    // The bottom row of a perspective view-projection is the camera's unit forward vector, which tells the storage order.
    const float row = std::sqrt(vp[12] * vp[12] + vp[13] * vp[13] + vp[14] * vp[14]);
    const float column = std::sqrt(vp[3] * vp[3] + vp[7] * vp[7] + vp[11] * vp[11]);
    const bool transpose = std::fabs(column - 1.0f) < std::fabs(row - 1.0f);
    Camera camera;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            camera.vp[r * 4 + c] = transpose ? vp[c * 4 + r] : vp[r * 4 + c];
            camera.inv_vp[r * 4 + c] = transpose ? inv_vp[c * 4 + r] : inv_vp[r * 4 + c];
        }
    if (block.z_near != UINT32_MAX) memcpy(&camera.z_near, bytes + block.z_near, 4);
    if (block.z_far != UINT32_MAX) memcpy(&camera.z_far, bytes + block.z_far, 4);
    if (block.fov != UINT32_MAX) memcpy(&camera.fov, bytes + block.fov, 4);
    float identity[16];
    Multiply(camera.vp, camera.inv_vp, identity);
    const float error = std::fabs(identity[0] - 1) + std::fabs(identity[5] - 1) + std::fabs(identity[10] - 1) + std::fabs(identity[15] - 1) + std::fabs(identity[3]) + std::fabs(identity[12]);
    camera.ok = error < 0.05f && camera.z_far > camera.z_near && camera.z_near > 0;
    if (!g_camera_logged)
    {
        g_camera_logged = true;
        Log("camera: set %u binding %u, row norm %.3f column norm %.3f, transpose=%d, near %.2f far %.1f fov %.3f, vp*inv error %.5f, usable=%d",
            block.set, block.binding, row, column, transpose, camera.z_near, camera.z_far, camera.fov, error, camera.ok);
    }
    if (camera.ok) g_camera = camera;
}

void CountDraw(VkCommandBuffer cb)
{
    if (!g_track) return;
    CbState *s = Cb(cb);
    if (s->scene_pass) ++g_scene_draws;
    if (s->camera_pass && g_fsr_mode && !g_frame_state.camera_read)
    {
        ReadCamera(s);
        g_frame_state.camera_read = true;
    }
    if (!g_census) return;
    if (!s->in_pass) { s->text += "  DRAW outside pass\n"; return; }
    ++s->pass.draws;
    if (s->dyn_viewport) ++s->pass.dyn_draws;
    ++s->pass.draws_by_tags[s->tags];
}

// ---------------------------------------------------------------------------
// Object tracking hooks
// ---------------------------------------------------------------------------

void CensusImage(const char *event, VkImage image, const VkImageCreateInfo *ci)
{
    static std::set<std::pair<VkImage, std::string>> seen;
    if (!g_census || !seen.insert({ image, event }).second) return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, (g_out_dir + L"\\images.txt").c_str(), L"a") != 0 || f == nullptr) return;
    if (ci != nullptr)
        fprintf(f, "%llu %s %p %ux%u f%u u%x flags%x s%u m%u l%u tiling%u layout%u\n", g_present_count, event, image, ci->extent.width, ci->extent.height, ci->format, ci->usage, ci->flags, ci->samples, ci->mipLevels, ci->arrayLayers, ci->tiling, ci->initialLayout);
    else
    {
        const auto it = g_images.find(image);
        if (it != g_images.end()) fprintf(f, "%llu %s %p %ux%u f%u u%x s%u\n", g_present_count, event, image, it->second.w, it->second.h, it->second.fmt, it->second.usage, it->second.samples);
    }
    fclose(f);
}

// The portrait chain has a distinctive signature in the census: the only multisampled images in the frame
// (anti-aliasing is off for the scene), plus R8G8B8A8 finals and R8 ambient-occlusion targets, and the float
// image its MSAA resolves into, which has the size of a multisampled image. Map-sized atlases are excluded by size.
bool IsPortraitTarget(const VkImageCreateInfo *ci)
{
    const uint32_t longest = (std::max)(ci->extent.width, ci->extent.height);
    if (ci->imageType != VK_IMAGE_TYPE_2D || ci->mipLevels != 1 || ci->arrayLayers != 1 || longest > 1500 || longest < 16) return false;
    constexpr VkImageUsageFlags target = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const std::pair<uint32_t, uint32_t> size{ ci->extent.width, ci->extent.height };
    if (ci->samples != VK_SAMPLE_COUNT_1_BIT)
    {
        g_portrait_sizes.insert(size);
        return true;
    }
    if (ci->usage != target || g_portrait_sizes.count(size) == 0) return false;
    return ci->format == VK_FORMAT_R8G8B8A8_UNORM || ci->format == VK_FORMAT_R8_UNORM || ci->format == VK_FORMAT_R16G16B16A16_SFLOAT;
}

// A portrait final created before its size was known. Other R8G8B8A8 images with this usage are ordinary GUI
// textures and must stay as they are, so the decision waits for the multisampled image of the same size.
struct FinalCandidate { VkImage image; uint32_t w, h; VkFormat format; VkImageUsageFlags usage; uint64_t present; };
std::vector<FinalCandidate> g_final_candidates;
struct ShadowImage { OwnedImage owned; };
std::unordered_map<VkImage, ShadowImage> g_shadows;
std::unordered_map<VkImageView, VkImageViewCreateInfo> g_candidate_views;
std::unordered_map<VkImageView, VkImageView> g_shadow_views;

VkImageView MakeShadowView(VkImageViewCreateInfo info, VkImage shadow)
{
    info.image = shadow;
    VkImageView view = VK_NULL_HANDLE;
    return next.CreateImageView(g_device, &info, nullptr, &view) == VK_SUCCESS ? view : VK_NULL_HANDLE;
}

void AnchorPortraitFinals(uint32_t w, uint32_t h)
{
    for (auto it = g_final_candidates.begin(); it != g_final_candidates.end();)
    {
        if (it->w != w || it->h != h || it->present + 1 < g_present_count) { ++it; continue; }
        ShadowImage shadow;
        const uint32_t sw = static_cast<uint32_t>(std::ceil(static_cast<float>(w) * g_portrait_scale));
        const uint32_t sh = static_cast<uint32_t>(std::ceil(static_cast<float>(h) * g_portrait_scale));
        if (MakeImage(shadow.owned, sw, sh, it->format, it->usage))
        {
            ImageInfo &info = g_images[it->image];
            info.w = sw;
            info.h = sh;
            info.sx = static_cast<float>(sw) / static_cast<float>(w);
            info.sy = static_cast<float>(sh) / static_cast<float>(h);
            for (const auto &[view, view_info] : g_candidate_views)
                if (view_info.image == it->image)
                    if (const VkImageView shadow_view = MakeShadowView(view_info, shadow.owned.image)) g_shadow_views[view] = shadow_view;
            g_shadows[it->image] = shadow;
        }
        else
            Log("could not create a native-size portrait target (%ux%u)", sw, sh);
        it = g_final_candidates.erase(it);
    }
}

// The image the GPU should really use for one the game names.
VkImage RealImage(VkImage image)
{
    if (g_shadows.empty()) return image;
    std::lock_guard lock(g_mx);
    const auto it = g_shadows.find(image);
    return it != g_shadows.end() ? it->second.owned.image : image;
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    VkImageCreateInfo scaled = *ci;
    float sx = 1, sy = 1;
    if (g_spatial_mode && ci->samples != VK_SAMPLE_COUNT_1_BIT)
    {
        std::lock_guard lock(g_mx);
        g_portrait_sizes.insert({ ci->extent.width, ci->extent.height });
    }
    if (g_portrait_scale > 1)
    {
        std::lock_guard lock(g_mx);
        if (IsPortraitTarget(ci))
        {
            scaled.extent.width = static_cast<uint32_t>(std::ceil(static_cast<float>(ci->extent.width) * g_portrait_scale));
            scaled.extent.height = static_cast<uint32_t>(std::ceil(static_cast<float>(ci->extent.height) * g_portrait_scale));
            sx = static_cast<float>(scaled.extent.width) / static_cast<float>(ci->extent.width);
            sy = static_cast<float>(scaled.extent.height) / static_cast<float>(ci->extent.height);
        }
    }
    const VkResult r = next.CreateImage(d, &scaled, a, out);
    if (r == VK_SUCCESS && g_track)
    {
        std::lock_guard lock(g_mx);
        ImageInfo info{ g_next_image++, scaled.extent.width, scaled.extent.height, static_cast<uint32_t>(ci->format), ci->usage,
                        static_cast<uint32_t>(ci->samples), ci->mipLevels, ci->arrayLayers, false, ci->initialLayout };
        info.sx = sx;
        info.sy = sy;
        g_images[*out] = info;
        CensusImage("create", *out, ci);
        if (g_portrait_scale > 1)
        {
            if (ci->samples != VK_SAMPLE_COUNT_1_BIT) AnchorPortraitFinals(ci->extent.width, ci->extent.height);
            else if (sx == 1 && ci->format == VK_FORMAT_R8G8B8A8_UNORM && ci->mipLevels == 1 && ci->arrayLayers == 1 &&
                     ci->usage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
                     (std::max)(ci->extent.width, ci->extent.height) <= 1500)
                g_final_candidates.push_back({ *out, ci->extent.width, ci->extent.height, ci->format, ci->usage, g_present_count });
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a)
{
    if (g_track && i)
    {
        std::lock_guard lock(g_mx);
        g_images.erase(i);
        const auto spatial_view = g_spatial.views.find(i);
        if (spatial_view != g_spatial.views.end())
        {
            next.DestroyImageView(d, spatial_view->second, nullptr);
            g_spatial.views.erase(spatial_view);
        }
        g_final_candidates.erase(std::remove_if(g_final_candidates.begin(), g_final_candidates.end(), [&](const FinalCandidate &c) { return c.image == i; }), g_final_candidates.end());
        const auto shadow = g_shadows.find(i);
        if (shadow != g_shadows.end())
        {
            FreeImage(shadow->second.owned);
            g_shadows.erase(shadow);
        }
    }
    next.DestroyImage(d, i, a);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
    const VkResult r = next.CreateImageView(d, ci, a, out);
    if (r == VK_SUCCESS && g_track)
    {
        std::lock_guard lock(g_mx);
        g_views[*out] = ci->image;
        if (g_portrait_scale > 1)
        {
            const auto shadow = g_shadows.find(ci->image);
            if (shadow != g_shadows.end())
            {
                if (const VkImageView view = MakeShadowView(*ci, shadow->second.owned.image)) g_shadow_views[*out] = view;
            }
            else if (std::any_of(g_final_candidates.begin(), g_final_candidates.end(), [&](const FinalCandidate &c) { return c.image == ci->image; }))
                g_candidate_views[*out] = *ci;
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{
    if (g_track && v)
    {
        std::lock_guard lock(g_mx);
        g_views.erase(v);
        g_candidate_views.erase(v);
        const auto shadow = g_shadow_views.find(v);
        if (shadow != g_shadow_views.end())
        {
            next.DestroyImageView(d, shadow->second, a);
            g_shadow_views.erase(shadow);
        }
    }
    next.DestroyImageView(d, v, a);
}
template <typename T>
void StoreRenderPass(VkRenderPass pass, const T *ci)
{
    std::vector<AttDesc> atts;
    for (uint32_t i = 0; i < ci->attachmentCount; ++i)
    {
        const auto &x = ci->pAttachments[i];
        atts.push_back({ static_cast<uint32_t>(x.format), static_cast<uint32_t>(x.loadOp), static_cast<uint32_t>(x.storeOp),
                         static_cast<uint32_t>(x.initialLayout), static_cast<uint32_t>(x.finalLayout), static_cast<uint32_t>(x.samples) });
    }
    std::lock_guard lock(g_mx);
    g_passes[pass] = std::move(atts);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
    const VkResult r = next.CreateRenderPass(d, ci, a, out);
    if (r == VK_SUCCESS && g_track) StoreRenderPass(*out, ci);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
    const VkResult r = next.CreateRenderPass2(d, ci, a, out);
    if (r == VK_SUCCESS && g_track) StoreRenderPass(*out, ci);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out)
{
    VkFramebufferCreateInfo sized = *ci;
    std::vector<VkImageView> real_views;
    if (g_portrait_scale > 1 && !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments != nullptr && ci->attachmentCount > 0)
    {
        std::lock_guard lock(g_mx);
        real_views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        for (VkImageView &v : real_views)
        {
            const auto shadow = g_shadow_views.find(v);
            if (shadow != g_shadow_views.end()) v = shadow->second;
        }
        sized.pAttachments = real_views.data();
        const auto view = g_views.find(ci->pAttachments[0]);
        const auto image = view != g_views.end() ? g_images.find(view->second) : g_images.end();
        if (image != g_images.end() && (image->second.sx != 1 || image->second.sy != 1))
        {
            sized.width = image->second.w;
            sized.height = image->second.h;
        }
    }
    const VkResult r = next.CreateFramebuffer(d, &sized, a, out);
    if (r == VK_SUCCESS && g_track)
    {
        std::vector<VkImageView> views;
        if (!(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments != nullptr)
            views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        std::lock_guard lock(g_mx);
        g_fbs[*out] = std::move(views);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *a)
{
    if (g_track && f) { std::lock_guard lock(g_mx); g_fbs.erase(f); }
    next.DestroyFramebuffer(d, f, a);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{
    const VkResult r = next.CreateShaderModule(d, ci, a, out);
    if (r == VK_SUCCESS && g_track)
    {
        const uint32_t tags = ScanTags(ci->pCode, ci->codeSize);
        const CameraBlock camera = g_fsr_mode ? ParseCamera(ci->pCode, ci->codeSize / 4) : CameraBlock{};
        std::lock_guard lock(g_mx);
        g_module_tags[*out] = tags;
        if (camera.found) g_module_cameras[*out] = camera;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL HookDestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    next.DestroyShaderModule(d, m, a);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                           const VkAllocationCallbacks *a, VkPipeline *out)
{
    // The game bakes its scissor into each pipeline, which would clip a native-size portrait target to the old size.
    // With native-size portraits the scissor becomes dynamic, and HookCmdBindPipeline supplies the baked value.
    std::vector<VkGraphicsPipelineCreateInfo> dynamic_scissor;
    std::vector<std::vector<VkDynamicState>> states;
    std::vector<VkPipelineDynamicStateCreateInfo> dynamic_infos;
    std::vector<char> baked(n, 0);
    if (g_portrait_scale > 1)
    {
        dynamic_scissor.assign(ci, ci + n);
        states.reserve(n);
        dynamic_infos.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            const auto *dyn = ci[i].pDynamicState;
            std::vector<VkDynamicState> list;
            if (dyn != nullptr) list.assign(dyn->pDynamicStates, dyn->pDynamicStates + dyn->dynamicStateCount);
            const bool already = std::find(list.begin(), list.end(), VK_DYNAMIC_STATE_SCISSOR) != list.end() ||
                                 std::find(list.begin(), list.end(), VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) != list.end();
            if (already || ci[i].pViewportState == nullptr || ci[i].pViewportState->pScissors == nullptr || ci[i].pViewportState->scissorCount != 1) continue;
            list.push_back(VK_DYNAMIC_STATE_SCISSOR);
            states.push_back(std::move(list));
            VkPipelineDynamicStateCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            info.dynamicStateCount = static_cast<uint32_t>(states.back().size());
            info.pDynamicStates = states.back().data();
            dynamic_infos.push_back(info);
            dynamic_scissor[i].pDynamicState = &dynamic_infos.back();
            baked[i] = 1;
        }
    }
    const VkResult r = next.CreateGraphicsPipelines(d, c, n, dynamic_scissor.empty() ? ci : dynamic_scissor.data(), a, out);
    if (g_track)
    {
        std::lock_guard lock(g_mx);
        for (uint32_t i = 0; i < n; ++i)
        {
            if (out[i] == VK_NULL_HANDLE) continue;
            PipelineInfo info{ 0, false };
            for (uint32_t s = 0; s < ci[i].stageCount; ++s)
            {
                const auto it = g_module_tags.find(ci[i].pStages[s].module);
                if (it != g_module_tags.end()) info.tags |= it->second;
                const auto camera = g_module_cameras.find(ci[i].pStages[s].module);
                if (camera != g_module_cameras.end() && !info.camera.found) info.camera = camera->second;
            }
            if (const auto *dyn = ci[i].pDynamicState)
                for (uint32_t k = 0; k < dyn->dynamicStateCount; ++k)
                    if (dyn->pDynamicStates[k] == VK_DYNAMIC_STATE_VIEWPORT || dyn->pDynamicStates[k] == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT)
                        info.dyn_viewport = true;
            if (baked[i])
            {
                info.has_scissor = true;
                info.scissor = ci[i].pViewportState->pScissors[0];
            }
            g_pipelines[out[i]] = info;
        }
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out)
{
    const VkResult r = next.CreateSwapchainKHR(d, ci, a, out);
    if (r == VK_SUCCESS && g_track)
    {
        uint32_t n = 0;
        next.GetSwapchainImagesKHR(d, *out, &n, nullptr);
        std::vector<VkImage> images(n);
        next.GetSwapchainImagesKHR(d, *out, &n, images.data());
        std::lock_guard lock(g_mx);
        for (VkImage image : images)
            g_images[image] = { g_next_image++, ci->imageExtent.width, ci->imageExtent.height, static_cast<uint32_t>(ci->imageFormat),
                                ci->imageUsage, 1, 1, ci->imageArrayLayers, true, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    return r;
}

// ---------------------------------------------------------------------------
// Command recording hooks
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL HookBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info)
{
    if (g_track)
    {
        CbState *s = Cb(cb);
        s->text.clear();
        s->in_pass = false;
        s->jitter_pass = false;
        s->scene_pass = false;
        s->camera_pass = false;
        s->portrait_final = VK_NULL_HANDLE;
        s->pipeline = VK_NULL_HANDLE;
        s->bound_sets.clear();
        s->swap_image = VK_NULL_HANDLE;
        s->scaled_images.clear();
        s->tags = 0;
        s->dyn_viewport = false;
        s->dispatches = 0;
        s->viewport = {};
    }
    return next.BeginCommandBuffer(cb, info);
}
// Screenshot on request: the game's own final image, read from its swapchain at the end of the command
// buffer that drew it. Only the game's frame can ever end up in a report, whatever else is on the desktop.
enum class ShotState { Idle, Armed, Recorded };
struct Shot {
    ShotState state = ShotState::Idle;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, wait = 0;
} g_shot;

bool RecordShot(VkCommandBuffer cb, VkImage swap)
{
    VkImageLayout layout;
    {
        std::lock_guard lock(g_mx);
        const auto it = g_images.find(swap);
        if (it == g_images.end()) return false;
        g_shot.w = it->second.w;
        g_shot.h = it->second.h;
        layout = it->second.layout;
    }
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(g_shot.w) * g_shot.h * 4;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (next.CreateBuffer(g_device, &bi, nullptr, &g_shot.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    next.GetBufferMemoryRequirements(g_device, g_shot.buffer, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || next.AllocateMemory(g_device, &ai, nullptr, &g_shot.memory) != VK_SUCCESS ||
        next.BindBufferMemory(g_device, g_shot.buffer, g_shot.memory, 0) != VK_SUCCESS) return false;
    const VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    Barriers(cb, { Barrier(swap, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, VK_ACCESS_TRANSFER_READ_BIT) });
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { g_shot.w, g_shot.h, 1 };
    next.CmdCopyImageToBuffer(cb, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shot.buffer, 1, &region);
    Barriers(cb, { Barrier(swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT, all) });
    return true;
}

void ReleaseShot()
{
    if (g_shot.buffer) next.DestroyBuffer(g_device, g_shot.buffer, nullptr);
    if (g_shot.memory) next.FreeMemory(g_device, g_shot.memory, nullptr);
    g_shot = Shot{};
}

// Called once per present. screen.raw = width, height (uint32), then BGRA rows; screen.done marks it complete.
void ServiceShot()
{
    if (g_out_dir.empty()) return;
    if (g_shot.state == ShotState::Idle)
    {
        const std::wstring request = g_out_dir + L"\\screen.request";
        if (g_present_count % 15 == 0 && GetFileAttributesW(request.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileW(request.c_str());
            g_shot.state = ShotState::Armed;
        }
        return;
    }
    if (g_shot.state != ShotState::Recorded || ++g_shot.wait < 4) return;
    void *mapped = nullptr;
    if (next.DeviceWaitIdle(g_device) == VK_SUCCESS &&
        next.MapMemory(g_device, g_shot.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
    {
        FILE *f = nullptr;
        if (_wfopen_s(&f, (g_out_dir + L"\\screen.raw").c_str(), L"wb") == 0 && f != nullptr)
        {
            fwrite(&g_shot.w, 4, 1, f);
            fwrite(&g_shot.h, 4, 1, f);
            fwrite(mapped, 4, static_cast<size_t>(g_shot.w) * g_shot.h, f);
            fclose(f);
            if (_wfopen_s(&f, (g_out_dir + L"\\screen.done").c_str(), L"w") == 0 && f != nullptr) fclose(f);
        }
        next.UnmapMemory(g_device, g_shot.memory);
    }
    ReleaseShot();
}

// One-shot diagnostic: every single-sample scaled portrait target of one frame, as raw texels.
struct PortraitDump { VkBuffer buffer; VkDeviceMemory memory; uint32_t w, h, fmt, bytes; int index; };
std::vector<PortraitDump> g_portrait_dumps;
enum class DumpState { Waiting, Recorded, Done };
DumpState g_dump_state = DumpState::Waiting;
uint32_t g_dump_wait;

void RecordPortraitDump(VkCommandBuffer cb, const CbState *s)
{
    int index = 0;
    for (const VkImage image : s->scaled_images)
    {
        ImageInfo info;
        {
            std::lock_guard lock(g_mx);
            const auto it = g_images.find(image);
            if (it == g_images.end()) continue;
            info = it->second;
        }
        const uint32_t bytes = info.fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : info.fmt == VK_FORMAT_R8_UNORM ? 1 : 4;
        PortraitDump d{ VK_NULL_HANDLE, VK_NULL_HANDLE, info.w, info.h, info.fmt, bytes, index++ };
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = static_cast<VkDeviceSize>(info.w) * info.h * bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (next.CreateBuffer(g_device, &bi, nullptr, &d.buffer) != VK_SUCCESS) continue;
        VkMemoryRequirements req{};
        next.GetBufferMemoryRequirements(g_device, d.buffer, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || next.AllocateMemory(g_device, &ai, nullptr, &d.memory) != VK_SUCCESS ||
            next.BindBufferMemory(g_device, d.buffer, d.memory, 0) != VK_SUCCESS) continue;
        const VkImage real = RealImage(image);
        const VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        Barriers(cb, { Barrier(real, info.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, VK_ACCESS_TRANSFER_READ_BIT) });
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { info.w, info.h, 1 };
        next.CmdCopyImageToBuffer(cb, real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.buffer, 1, &region);
        Barriers(cb, { Barrier(real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, info.layout, VK_ACCESS_TRANSFER_READ_BIT, all) });
        g_portrait_dumps.push_back(d);
    }
    Log("portrait dump recorded: %zu of %zu scaled targets", g_portrait_dumps.size(), s->scaled_images.size());
}

void ServicePortraitDump()
{
    if (g_dump_state != DumpState::Recorded || ++g_dump_wait < 4) return;
    next.DeviceWaitIdle(g_device);
    for (const PortraitDump &d : g_portrait_dumps)
    {
        void *mapped = nullptr;
        if (next.MapMemory(g_device, d.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
        {
            const std::wstring path = g_out_dir + L"\\dump-" + std::to_wstring(d.index) + L"-" + std::to_wstring(d.w) + L"x" + std::to_wstring(d.h) + L"-f" + std::to_wstring(d.fmt) + L".raw";
            FILE *f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f != nullptr)
            {
                fwrite(mapped, d.bytes, static_cast<size_t>(d.w) * d.h, f);
                fclose(f);
            }
            next.UnmapMemory(g_device, d.memory);
        }
        next.DestroyBuffer(g_device, d.buffer, nullptr);
        next.FreeMemory(g_device, d.memory, nullptr);
    }
    g_portrait_dumps.clear();
    g_dump_state = DumpState::Done;
}

VKAPI_ATTR VkResult VKAPI_CALL HookEndCommandBuffer(VkCommandBuffer cb)
{
    if (g_dump_portrait && g_dump_state == DumpState::Waiting && g_fsr.dispatches > 900)
    {
        const CbState *s = Cb(cb);
        if (!s->scaled_images.empty() && s->swap_image != VK_NULL_HANDLE)
        {
            RecordPortraitDump(cb, s);
            g_dump_state = DumpState::Recorded;
        }
    }
    if (g_track && g_shot.state == ShotState::Armed)
    {
        const VkImage swap = Cb(cb)->swap_image;
        if (swap != VK_NULL_HANDLE)
        {
            if (RecordShot(cb, swap)) g_shot.state = ShotState::Recorded;
            else { Log("screenshot could not be recorded"); ReleaseShot(); }
        }
    }
    return next.EndCommandBuffer(cb);
}

VKAPI_ATTR void VKAPI_CALL HookCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, VkSubpassContents contents)
{
    std::string head;
    if (g_track) BeforePass(cb, info, g_census ? &head : nullptr);
    VkRenderPassBeginInfo sized = *info;
    if (g_track) { const CbState *s = Cb(cb); if (s->pass_sx != 1 || s->pass_sy != 1) sized.renderArea = ScaledRect(s, info->renderArea); }
    next.CmdBeginRenderPass(cb, &sized, contents);
    if (g_track) AfterPassBegin(cb, &head);
}
VKAPI_ATTR void VKAPI_CALL HookCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, const VkSubpassBeginInfo *sub)
{
    std::string head;
    if (g_track) BeforePass(cb, info, g_census ? &head : nullptr);
    VkRenderPassBeginInfo sized = *info;
    if (g_track) { const CbState *s = Cb(cb); if (s->pass_sx != 1 || s->pass_sy != 1) sized.renderArea = ScaledRect(s, info->renderArea); }
    next.CmdBeginRenderPass2(cb, &sized, sub);
    if (g_track) AfterPassBegin(cb, &head);
}
VKAPI_ATTR void VKAPI_CALL HookCmdEndRenderPass(VkCommandBuffer cb)
{
    next.CmdEndRenderPass(cb);
    PassEnded(cb);
}
VKAPI_ATTR void VKAPI_CALL HookCmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *info)
{
    next.CmdEndRenderPass2(cb, info);
    PassEnded(cb);
}
VKAPI_ATTR void VKAPI_CALL HookCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *info)
{
    if (g_census)
    {
        std::string head = Fmt("  RENDERING %ux%u", info->renderArea.extent.width, info->renderArea.extent.height);
        {
            std::lock_guard lock(g_mx);
            auto add = [&](const VkRenderingAttachmentInfo *a, const char *kind) {
                if (a == nullptr || a->imageView == VK_NULL_HANDLE) return;
                const auto view = g_views.find(a->imageView);
                head += Fmt(" {%s ", kind) + (view != g_views.end() ? ImageText(view->second) : std::string("?")) +
                        Fmt(" load%u store%u}", a->loadOp, a->storeOp);
            };
            for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) add(&info->pColorAttachments[i], "color");
            add(info->pDepthAttachment, "depth");
        }
        CbState *s = Cb(cb);
        FlushDispatches(s);
        s->pass = PassStats{};
        s->pass.head = std::move(head);
        s->in_pass = true;
    }
    next.CmdBeginRendering(cb, info);
}
VKAPI_ATTR void VKAPI_CALL HookCmdEndRendering(VkCommandBuffer cb)
{
    next.CmdEndRendering(cb);
    PassEnded(cb);
}
VKAPI_ATTR void VKAPI_CALL HookCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline)
{
    next.CmdBindPipeline(cb, point, pipeline);
    if (g_track && point == VK_PIPELINE_BIND_POINT_GRAPHICS)
    {
        CbState *s = Cb(cb);
        s->pipeline = pipeline;
        std::lock_guard lock(g_mx);
        const auto it = g_pipelines.find(pipeline);
        s->tags = it != g_pipelines.end() ? it->second.tags : 0;
        s->dyn_viewport = it != g_pipelines.end() && it->second.dyn_viewport;
        if (g_census && s->in_pass) ++s->pass.pipelines[pipeline];
        if (it != g_pipelines.end() && it->second.has_scissor)
        {
            s->scissor = it->second.scissor;
            const VkRect2D rect = (s->pass_sx != 1 || s->pass_sy != 1) ? ScaledRect(s, s->scissor) : s->scissor;
            next.CmdSetScissor(cb, 0, 1, &rect);
            if (g_dump_portrait && g_scaled_pass_logs < 40)
            {
                ++g_scaled_pass_logs;
                Log("baked scissor %d,%d %ux%u -> %d,%d %ux%u at x%.3f", s->scissor.offset.x, s->scissor.offset.y, s->scissor.extent.width, s->scissor.extent.height, rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height, s->pass_sx);
            }
        }
    }
}
VKAPI_ATTR void VKAPI_CALL HookCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *v)
{
    if (g_track && count == 1 && first == 0)
    {
        CbState *s = Cb(cb);
        s->viewport = v[0];
        if (s->pass_sx != 1 || s->pass_sy != 1)
        {
            if (g_dump_portrait && g_scaled_pass_logs < 24)
            {
                ++g_scaled_pass_logs;
                Log("in scaled pass, game viewport %.1f,%.1f %.1fx%.1f", v[0].x, v[0].y, v[0].width, v[0].height);
            }
            const VkViewport scaled = ScaledViewport(s, v[0]);
            next.CmdSetViewport(cb, 0, 1, &scaled);
            return;
        }
        if (s->jitter_pass && v[0].width == static_cast<float>(g_render_w) && v[0].height == static_cast<float>(g_render_h))
        {
            VkViewport jittered = v[0];
            jittered.x += g_fsr.jx;
            jittered.y += g_fsr.jy;
            next.CmdSetViewport(cb, 0, 1, &jittered);
            return;
        }
    }
    next.CmdSetViewport(cb, first, count, v);
}
// Writes with any shadowed image view, redirected. Storage for the replaced infos lives in `infos`.
std::vector<VkWriteDescriptorSet> RedirectWrites(uint32_t n, const VkWriteDescriptorSet *writes, std::vector<std::vector<VkDescriptorImageInfo>> &infos)
{
    std::vector<VkWriteDescriptorSet> out(writes, writes + n);
    std::lock_guard lock(g_mx);
    for (VkWriteDescriptorSet &w : out)
    {
        if (w.pImageInfo == nullptr || (w.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && w.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)) continue;
        infos.emplace_back(w.pImageInfo, w.pImageInfo + w.descriptorCount);
        for (VkDescriptorImageInfo &info : infos.back())
        {
            const auto shadow = g_shadow_views.find(info.imageView);
            if (shadow != g_shadow_views.end()) info.imageView = shadow->second;
        }
        w.pImageInfo = infos.back().data();
    }
    return out;
}
void TrackBufferWrites(uint32_t n, const VkWriteDescriptorSet *writes)
{
    std::lock_guard lock(g_mx);
    for (uint32_t i = 0; i < n; ++i)
    {
        const VkWriteDescriptorSet &w = writes[i];
        if ((w.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && w.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) || w.pBufferInfo == nullptr) continue;
        for (uint32_t k = 0; k < w.descriptorCount; ++k)
            g_set_buffers[w.dstSet][w.dstBinding + k] = { w.pBufferInfo[k].buffer, w.pBufferInfo[k].offset };
    }
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    const VkResult r = next.CreateDescriptorSetLayout(d, ci, a, out);
    if (r == VK_SUCCESS && g_fsr_mode)
    {
        std::vector<LayoutBinding> bindings;
        for (uint32_t i = 0; i < ci->bindingCount; ++i)
            bindings.push_back({ ci->pBindings[i].binding, ci->pBindings[i].descriptorCount, ci->pBindings[i].descriptorType });
        std::sort(bindings.begin(), bindings.end(), [](const LayoutBinding &x, const LayoutBinding &y) { return x.binding < y.binding; });
        std::lock_guard lock(g_mx);
        g_set_layouts[*out] = std::move(bindings);
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL HookAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    const VkResult r = next.AllocateDescriptorSets(d, ai, out);
    if (r == VK_SUCCESS && g_fsr_mode)
    {
        std::lock_guard lock(g_mx);
        for (uint32_t i = 0; i < ai->descriptorSetCount; ++i)
        {
            g_set_layout_of[out[i]] = ai->pSetLayouts[i];
            g_set_buffers.erase(out[i]);
        }
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL HookBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o)
{
    if (g_fsr_mode) { std::lock_guard lock(g_mx); g_buffer_memory[b] = { m, o }; }
    return next.BindBufferMemory(d, b, m, o);
}
VKAPI_ATTR VkResult VKAPI_CALL HookMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
    const VkResult r = next.MapMemory(d, m, o, size, flags, out);
    if (r == VK_SUCCESS && g_fsr_mode) { std::lock_guard lock(g_mx); g_mapped[m] = { static_cast<char *>(*out) - o }; }
    return r;
}
VKAPI_ATTR void VKAPI_CALL HookUnmapMemory(VkDevice d, VkDeviceMemory m)
{
    if (g_fsr_mode) { std::lock_guard lock(g_mx); g_mapped.erase(m); }
    next.UnmapMemory(d, m);
}
VKAPI_ATTR void VKAPI_CALL HookCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t dn, const uint32_t *offsets)
{
    next.CmdBindDescriptorSets(cb, point, layout, first, n, sets, dn, offsets);
    if (!g_fsr_mode || point != VK_PIPELINE_BIND_POINT_GRAPHICS || g_frame_state.camera_read) return;
    CbState *s = Cb(cb);
    if (s->bound_sets.size() < first + n) s->bound_sets.resize(first + n);
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        CbState::BoundSet &bound = s->bound_sets[first + i];
        bound.set = sets[i];
        bound.dynamic_offsets.clear();
        uint32_t count = 0;
        {
            std::lock_guard lock(g_mx);
            const auto layout_of = g_set_layout_of.find(sets[i]);
            const auto found = layout_of != g_set_layout_of.end() ? g_set_layouts.find(layout_of->second) : g_set_layouts.end();
            if (found != g_set_layouts.end())
                for (const LayoutBinding &b : found->second)
                    if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) count += b.count;
        }
        for (uint32_t k = 0; k < count && used < dn; ++k) bound.dynamic_offsets.push_back(offsets[used++]);
    }
}
VKAPI_ATTR void VKAPI_CALL HookUpdateDescriptorSets(VkDevice d, uint32_t n, const VkWriteDescriptorSet *writes, uint32_t cn, const VkCopyDescriptorSet *copies)
{
    if (g_fsr_mode && n > 0) TrackBufferWrites(n, writes);
    if (g_portrait_scale > 1 && n > 0)
    {
        std::vector<std::vector<VkDescriptorImageInfo>> infos;
        infos.reserve(n);
        const auto redirected = RedirectWrites(n, writes, infos);
        next.UpdateDescriptorSets(d, n, redirected.data(), cn, copies);
        return;
    }
    next.UpdateDescriptorSets(d, n, writes, cn, copies);
}
VKAPI_ATTR void VKAPI_CALL HookCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set, uint32_t n, const VkWriteDescriptorSet *writes)
{
    if (g_portrait_scale > 1 && n > 0)
    {
        std::vector<std::vector<VkDescriptorImageInfo>> infos;
        infos.reserve(n);
        const auto redirected = RedirectWrites(n, writes, infos);
        next.CmdPushDescriptorSetKHR(cb, point, layout, set, n, redirected.data());
        return;
    }
    next.CmdPushDescriptorSetKHR(cb, point, layout, set, n, writes);
}
VKAPI_ATTR void VKAPI_CALL HookUpdateDescriptorSetWithTemplate(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data)
{
    static bool logged;
    if (!logged && g_portrait_scale > 1) { logged = true; Log("the game updates descriptors through templates, which are not redirected to native-size portraits"); }
    next.UpdateDescriptorSetWithTemplate(d, set, t, data);
}
VKAPI_ATTR void VKAPI_CALL HookCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *r)
{
    if (g_track && count == 1 && first == 0)
    {
        CbState *s = Cb(cb);
        s->scissor = r[0];
        if (s->pass_sx != 1 || s->pass_sy != 1)
        {
            const VkRect2D scaled = ScaledRect(s, r[0]);
            if (g_dump_portrait && g_scaled_pass_logs < 24)
            {
                ++g_scaled_pass_logs;
                Log("in scaled pass, game scissor %d,%d %ux%u -> %d,%d %ux%u", r[0].offset.x, r[0].offset.y, r[0].extent.width, r[0].extent.height, scaled.offset.x, scaled.offset.y, scaled.extent.width, scaled.extent.height);
            }
            next.CmdSetScissor(cb, 0, 1, &scaled);
            return;
        }
    }
    next.CmdSetScissor(cb, first, count, r);
}
VKAPI_ATTR void VKAPI_CALL HookCmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment *atts, uint32_t rc, const VkClearRect *rects)
{
    if (g_track)
    {
        const CbState *s = Cb(cb);
        if (s->pass_sx != 1 || s->pass_sy != 1)
        {
            std::vector<VkClearRect> scaled(rects, rects + rc);
            for (VkClearRect &x : scaled) x.rect = ScaledRect(s, x.rect);
            next.CmdClearAttachments(cb, n, atts, rc, scaled.data());
            return;
        }
    }
    next.CmdClearAttachments(cb, n, atts, rc, rects);
}
VKAPI_ATTR void VKAPI_CALL HookCmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (SkipThisDraw(cb, a, false)) return;
    next.CmdDraw(cb, a, b, c, d);
    CountDraw(cb);
}
VKAPI_ATTR void VKAPI_CALL HookCmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e)
{
    if (SkipThisDraw(cb, a, true)) return;
    next.CmdDrawIndexed(cb, a, b, c, d, e);
    CountDraw(cb);
}
VKAPI_ATTR void VKAPI_CALL HookCmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
    next.CmdDrawIndirect(cb, b, o, n, s);
    CountDraw(cb);
}
VKAPI_ATTR void VKAPI_CALL HookCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
    next.CmdDrawIndexedIndirect(cb, b, o, n, s);
    CountDraw(cb);
}
VKAPI_ATTR void VKAPI_CALL HookCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{
    next.CmdDispatch(cb, x, y, z);
    if (g_census) ++Cb(cb)->dispatches;
}
void ImageScale(VkImage image, float &sx, float &sy)
{
    sx = sy = 1;
    if (g_portrait_scale <= 1) return;
    std::lock_guard lock(g_mx);
    const auto it = g_images.find(image);
    if (it != g_images.end()) { sx = it->second.sx; sy = it->second.sy; }
}

void CensusLine(VkCommandBuffer cb, const char *kind, VkImage src, VkImage dst)
{
    if (!g_census) return;
    CbState *s = Cb(cb);
    FlushDispatches(s);
    std::lock_guard lock(g_mx);
    s->text += std::string("  ") + kind + " " + ImageText(src) + " -> " + ImageText(dst) + "\n";
}
VKAPI_ATTR void VKAPI_CALL HookCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{
    float ssx, ssy, dsx, dsy;
    ImageScale(src, ssx, ssy);
    ImageScale(dst, dsx, dsy);
    src = RealImage(src);
    dst = RealImage(dst);
    if (ssx != 1 || ssy != 1 || dsx != 1 || dsy != 1)
    {
        std::vector<VkImageBlit> scaled(r, r + n);
        for (VkImageBlit &x : scaled)
            for (int k = 0; k < 2; ++k)
            {
                x.srcOffsets[k].x = static_cast<int32_t>(std::ceil(static_cast<float>(x.srcOffsets[k].x) * ssx));
                x.srcOffsets[k].y = static_cast<int32_t>(std::ceil(static_cast<float>(x.srcOffsets[k].y) * ssy));
                x.dstOffsets[k].x = static_cast<int32_t>(std::ceil(static_cast<float>(x.dstOffsets[k].x) * dsx));
                x.dstOffsets[k].y = static_cast<int32_t>(std::ceil(static_cast<float>(x.dstOffsets[k].y) * dsy));
            }
        next.CmdBlitImage(cb, src, sl, dst, dl, n, scaled.data(), f);
    }
    else
        next.CmdBlitImage(cb, src, sl, dst, dl, n, r, f);
    CensusLine(cb, "BLIT", src, dst);
}
VKAPI_ATTR void VKAPI_CALL HookCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{
    float ssx, ssy, dsx, dsy;
    ImageScale(src, ssx, ssy);
    ImageScale(dst, dsx, dsy);
    src = RealImage(src);
    dst = RealImage(dst);
    if (ssx != dsx || ssy != dsy)
    {
        static bool logged;
        if (!logged) { logged = true; Log("copy between differently scaled images: %.2f -> %.2f; regions left as they are", ssx, dsx); }
    }
    if (ssx != 1 && ssx == dsx && ssy == dsy)
    {
        std::vector<VkImageCopy> scaled(r, r + n);
        for (VkImageCopy &x : scaled)
        {
            x.srcOffset.x = static_cast<int32_t>(static_cast<float>(x.srcOffset.x) * ssx);
            x.srcOffset.y = static_cast<int32_t>(static_cast<float>(x.srcOffset.y) * ssy);
            x.dstOffset.x = static_cast<int32_t>(static_cast<float>(x.dstOffset.x) * ssx);
            x.dstOffset.y = static_cast<int32_t>(static_cast<float>(x.dstOffset.y) * ssy);
            x.extent.width = static_cast<uint32_t>(std::ceil(static_cast<float>(x.extent.width) * ssx));
            x.extent.height = static_cast<uint32_t>(std::ceil(static_cast<float>(x.extent.height) * ssy));
        }
        next.CmdCopyImage(cb, src, sl, dst, dl, n, scaled.data());
    }
    else
        next.CmdCopyImage(cb, src, sl, dst, dl, n, r);
    if (g_census) { std::lock_guard lock(g_mx); CensusImage("copysrc", src, nullptr); CensusImage("copydst", dst, nullptr); }
    CensusLine(cb, "COPY", src, dst);
}
VKAPI_ATTR void VKAPI_CALL HookCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage dst, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r)
{
    next.CmdCopyBufferToImage(cb, b, dst, l, n, r);
    if (g_census) { std::lock_guard lock(g_mx); CensusImage("upload", dst, nullptr); }
}
VKAPI_ATTR void VKAPI_CALL HookCmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r)
{
    float ssx, ssy, dsx, dsy;
    ImageScale(src, ssx, ssy);
    ImageScale(dst, dsx, dsy);
    if (g_dump_portrait && dsx != 1) Cb(cb)->scaled_images.push_back(dst);
    if (ssx != 1 || ssy != 1)
    {
        if (ssx != dsx || ssy != dsy) Log("resolve between differently scaled images: %.2f -> %.2f", ssx, dsx);
        std::vector<VkImageResolve> scaled(r, r + n);
        for (VkImageResolve &x : scaled)
        {
            x.srcOffset.x = static_cast<int32_t>(static_cast<float>(x.srcOffset.x) * ssx);
            x.srcOffset.y = static_cast<int32_t>(static_cast<float>(x.srcOffset.y) * ssy);
            x.dstOffset.x = static_cast<int32_t>(static_cast<float>(x.dstOffset.x) * dsx);
            x.dstOffset.y = static_cast<int32_t>(static_cast<float>(x.dstOffset.y) * dsy);
            x.extent.width = static_cast<uint32_t>(std::ceil(static_cast<float>(x.extent.width) * ssx));
            x.extent.height = static_cast<uint32_t>(std::ceil(static_cast<float>(x.extent.height) * ssy));
        }
        next.CmdResolveImage(cb, src, sl, dst, dl, n, scaled.data());
    }
    else
        next.CmdResolveImage(cb, src, sl, dst, dl, n, r);
    CensusLine(cb, "RESOLVE", src, dst);
}
VKAPI_ATTR void VKAPI_CALL HookCmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *list)
{
    next.CmdExecuteCommands(cb, n, list);
    if (g_census)
    {
        CbState *s = Cb(cb);
        for (uint32_t i = 0; i < n; ++i) s->text += Cb(list[i])->text;
    }
}
VKAPI_ATTR void VKAPI_CALL HookCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
                                                  uint32_t mc, const VkMemoryBarrier *m, uint32_t bc, const VkBufferMemoryBarrier *b,
                                                  uint32_t ic, const VkImageMemoryBarrier *i)
{
    std::vector<VkImageMemoryBarrier> both;
    if (!g_shadows.empty() && ic > 0)
    {
        both.assign(i, i + ic);
        for (uint32_t k = 0; k < ic; ++k)
        {
            const VkImage real = RealImage(i[k].image);
            if (real != i[k].image) { both.push_back(i[k]); both.back().image = real; }
        }
    }
    if (both.size() > ic) next.CmdPipelineBarrier(cb, src, dst, flags, mc, m, bc, b, static_cast<uint32_t>(both.size()), both.data());
    else next.CmdPipelineBarrier(cb, src, dst, flags, mc, m, bc, b, ic, i);
    if (g_track && ic > 0)
    {
        std::lock_guard lock(g_mx);
        for (uint32_t k = 0; k < ic; ++k)
        {
            const auto it = g_images.find(i[k].image);
            if (it != g_images.end()) it->second.layout = i[k].newLayout;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL HookCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *info)
{
    std::vector<VkImageMemoryBarrier2> both;
    if (!g_shadows.empty() && info->imageMemoryBarrierCount > 0)
    {
        both.assign(info->pImageMemoryBarriers, info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
        for (uint32_t k = 0; k < info->imageMemoryBarrierCount; ++k)
        {
            const VkImage real = RealImage(info->pImageMemoryBarriers[k].image);
            if (real != info->pImageMemoryBarriers[k].image) { both.push_back(info->pImageMemoryBarriers[k]); both.back().image = real; }
        }
    }
    if (both.size() > info->imageMemoryBarrierCount)
    {
        VkDependencyInfo extended = *info;
        extended.imageMemoryBarrierCount = static_cast<uint32_t>(both.size());
        extended.pImageMemoryBarriers = both.data();
        next.CmdPipelineBarrier2(cb, &extended);
    }
    else
        next.CmdPipelineBarrier2(cb, info);
    if (g_track && info->imageMemoryBarrierCount > 0)
    {
        std::lock_guard lock(g_mx);
        for (uint32_t k = 0; k < info->imageMemoryBarrierCount; ++k)
        {
            const auto it = g_images.find(info->pImageMemoryBarriers[k].image);
            if (it != g_images.end()) it->second.layout = info->pImageMemoryBarriers[k].newLayout;
        }
    }
}

// ---------------------------------------------------------------------------
// Submission, frame end
// ---------------------------------------------------------------------------

std::string g_frame_text;
uint32_t g_census_seq;
auto g_census_next = std::chrono::steady_clock::now() + std::chrono::seconds(10);

void AppendSubmit(uint32_t count, const VkCommandBuffer *cbs)
{
    std::lock_guard lock(g_mx);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto it = g_cbs.find(cbs[i]);
        if (it == g_cbs.end()) continue;
        FlushDispatches(it->second);
        g_frame_text += Fmt("CB %p\n", cbs[i]) + it->second->text;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *submits, VkFence fence)
{
    if (g_census)
        for (uint32_t i = 0; i < n; ++i)
            AppendSubmit(submits[i].commandBufferCount, submits[i].pCommandBuffers);
    return next.QueueSubmit(q, n, submits, fence);
}
VKAPI_ATTR VkResult VKAPI_CALL HookQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *submits, VkFence fence)
{
    if (g_census)
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; ++k)
                AppendSubmit(1, &submits[i].pCommandBufferInfos[k].commandBuffer);
    return next.QueueSubmit2(q, n, submits, fence);
}

void WriteCensus()
{
    std::lock_guard lock(g_mx);
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_census_next && g_census_seq < 40)
    {
        g_census_next = now + std::chrono::seconds(10);
        const std::wstring path = g_out_dir + L"\\census-" + std::to_wstring(++g_census_seq) + L".txt";
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f != nullptr)
        {
            fprintf(f, "present %llu images=%zu pipelines=%zu\n%s", g_present_count, g_images.size(), g_pipelines.size(), g_frame_text.c_str());
            fclose(f);
        }
    }
    g_frame_text.clear();
}

// Timestamps of a dispatch two frames back; never waits.
void ReadFsrGpuTime()
{
    if (g_fsr.queries == VK_NULL_HANDLE || g_fsr.dispatches < 3) return;
    const uint32_t slot = static_cast<uint32_t>((g_fsr.dispatches - 3) % 4) * 2;
    uint64_t ticks[2] = {};
    if (next.GetQueryPoolResults(g_device, g_fsr.queries, slot, 2, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        g_gpu_ms = static_cast<double>(ticks[1] - ticks[0]) * g_fsr.timestamp_ns / 1e6;
}

void AdvanceJitter()
{
    if (!g_fsr_mode || !g_fsr.ready) return;
    float x = 0, y = 0;
    FfxJitterOffset q{ { 0x10005, nullptr }, static_cast<int32_t>(g_fsr.jitter_index % static_cast<uint32_t>(g_fsr.phases)), g_fsr.phases, &x, &y };
    if (g_fsr.query(&g_fsr.context, &q.header) == 0)
    {
        g_fsr.jx = x;
        g_fsr.jy = y;
    }
    ++g_fsr.jitter_index;
}

// Unattended runs (CK3_FSR_FREEZE_INPUT=1): the player keeps using the PC, and the game reads the mouse through
// raw input even with its window disabled and behind others. Dropping input in its window procedure stops it all.
WNDPROC g_game_wndproc;
HWND g_game_window;
bool g_freeze_input, g_input_frozen;
uint64_t g_pan_until;
UINT g_key_down;
uint64_t g_key_up_at;

constexpr UINT kPanMessage = WM_APP + 77;

LRESULT CALLBACK FreezeWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == kPanMessage)
    {
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(wparam), MAPVK_VK_TO_VSC);
        const LPARAM bits = lparam ? (1 | (static_cast<LPARAM>(scan) << 16)) : (1 | (static_cast<LPARAM>(scan) << 16) | (1LL << 30) | (1LL << 31));
        return CallWindowProcW(g_game_wndproc, hwnd, lparam ? WM_KEYDOWN : WM_KEYUP, wparam, bits);
    }
    if (message == WM_INPUT) return DefWindowProcW(hwnd, message, wparam, lparam);
    if ((message >= WM_KEYFIRST && message <= WM_KEYLAST) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_NCMOUSEMOVE && message <= WM_NCXBUTTONDBLCLK) || message == WM_MOUSELEAVE || message == WM_MOUSEHOVER)
        return 0;
    return CallWindowProcW(g_game_wndproc, hwnd, message, wparam, lparam);
}

BOOL CALLBACK FreezeGameWindow(HWND hwnd, LPARAM)
{
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    g_game_window = hwnd;
    g_game_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FreezeWndProc)));
    g_input_frozen = g_game_wndproc != nullptr;
    Log("game input %s", g_input_frozen ? "frozen for this unattended run" : "could not be frozen");
    return FALSE;
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info)
{
    if (g_freeze_input && !g_input_frozen) EnumWindows(FreezeGameWindow, 0);
    if (g_input_frozen && g_present_count % 15 == 0)
    {
        const std::wstring request = g_out_dir + L"\\pan.request";
        if (g_pan_until == 0 && GetFileAttributesW(request.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileW(request.c_str());
            g_pan_until = g_present_count + 300;
            PostMessageW(g_game_window, kPanMessage, 'D', 1);
            Log("camera pan started");
        }
    }
    if (g_input_frozen && g_key_down == 0 && g_present_count % 15 == 0)
    {
        const std::wstring request = g_out_dir + L"\\key.request";
        FILE *f = nullptr;
        if (GetFileAttributesW(request.c_str()) != INVALID_FILE_ATTRIBUTES && _wfopen_s(&f, request.c_str(), L"r") == 0 && f != nullptr)
        {
            unsigned key = 0;
            const int read = fscanf_s(f, "%u", &key);
            fclose(f);
            DeleteFileW(request.c_str());
            if (read == 1 && key != 0)
            {
                g_key_down = key;
                g_key_up_at = g_present_count + 6;
                PostMessageW(g_game_window, kPanMessage, key, 1);
                Log("pressed key %u", key);
            }
        }
    }
    if (g_key_down != 0 && g_present_count >= g_key_up_at)
    {
        PostMessageW(g_game_window, kPanMessage, g_key_down, 0);
        g_key_down = 0;
    }
    if (g_pan_until != 0 && g_present_count >= g_pan_until)
    {
        g_pan_until = 0;
        PostMessageW(g_game_window, kPanMessage, 'D', 0);
        Log("camera pan ended");
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double dt = g_last_present.QuadPart ? 1000.0 * static_cast<double>(now.QuadPart - g_last_present.QuadPart) / static_cast<double>(g_qpf.QuadPart) : 0;
    if (dt > 0) g_delta_ms = dt;
    g_last_present = now;

    if (g_csv != nullptr)
    {
        ReadFsrGpuTime();
        fprintf(g_csv, "%.4f,%llu,%.4f,%d,%u,%.4f,0,%.4f,%u,%u,%u,%u,%s,0,0,0,0,0,%u\n",
                1000.0 * static_cast<double>(now.QuadPart - g_csv_start.QuadPart) / static_cast<double>(g_qpf.QuadPart),
                g_present_count, dt, g_fsr_mode ? 1 : 0, g_fsr_success, g_record_ms, g_gpu_ms, g_render_w, g_render_h, g_swap_w, g_swap_h,
                g_fsr_mode ? g_gpu_name.c_str() : "Off", g_scene_draws);
        fflush(g_csv);
    }
    g_scene_draws = 0;
    if (g_census) WriteCensus();

    {
        std::lock_guard lock(g_mx);
        g_frame_state = FrameState{};
        if (g_camera.ok) g_previous_camera = g_camera;
        g_camera.ok = false;
    }
    g_fsr_success = 0;
    g_record_ms = 0;
    g_spatial.portraits_this_frame = 0;
    g_skip_cb = VK_NULL_HANDLE;
    ++g_present_count;
    AdvanceJitter();
    if (g_track) ServiceShot();
    if (g_dump_portrait) ServicePortraitDump();
    return next.QueuePresentKHR(q, info);
}

VKAPI_ATTR void VKAPI_CALL HookDestroyDevice(VkDevice d, const VkAllocationCallbacks *a)
{
    if (d == g_device)
    {
        DestroySpatial();
        DestroyFsr();
        if (g_csv != nullptr) { fclose(g_csv); g_csv = nullptr; }
        g_device = VK_NULL_HANDLE;
        g_track = g_census = g_fsr_mode = false;
    }
    next.DestroyDevice(d, a);
}

// ---------------------------------------------------------------------------
// Layer plumbing
// ---------------------------------------------------------------------------

template <typename T>
const T *FindChainInfo(const void *pNext, VkStructureType type)
{
    for (const auto *s = static_cast<const VkBaseInStructure *>(pNext); s != nullptr; s = s->pNext)
    {
        const auto *c = reinterpret_cast<const T *>(s);
        if (s->sType == type && c->function == VK_LAYER_LINK_INFO) return c;
    }
    return nullptr;
}

PFN_vkDestroyInstance g_next_destroy_instance;

VKAPI_ATTR void VKAPI_CALL HookDestroyInstance(VkInstance instance, const VkAllocationCallbacks *a)
{
    g_instances.erase(std::remove(g_instances.begin(), g_instances.end(), instance), g_instances.end());
    if (g_next_destroy_instance != nullptr) g_next_destroy_instance(instance, a);
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
    const auto *chain = FindChainInfo<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const_cast<VkLayerInstanceCreateInfo *>(chain)->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    const VkResult r = create(ci, a, out);
    if (r == VK_SUCCESS)
    {
        g_next_gipa = gipa;
        g_instances.push_back(*out);
        g_next_destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out, "vkDestroyInstance"));
    }
    return r;
}

// Everything the pinned FSR shaders need that CK3's device does not enable on its own.
// Flags are flipped in the game's own feature structs when it chains one that covers them, since
// Vulkan forbids a struct next to the 1.1/1.2 aggregate that contains the same feature.
struct FsrDeviceFeatures {
    std::vector<const char *> extensions;
    VkPhysicalDeviceShaderFloat16Int8Features float16{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
    VkPhysicalDevice16BitStorageFeatures storage16{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures subgroup{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES };
    VkPhysicalDeviceFeatures core{};
};

void AddFsrFeatures(VkPhysicalDevice gpu, VkInstance instance, PFN_vkGetInstanceProcAddr gipa, VkDeviceCreateInfo &ci, FsrDeviceFeatures &own)
{
    uint32_t n = 0;
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    std::vector<VkExtensionProperties> props;
    if (enumerate != nullptr && enumerate(gpu, nullptr, &n, nullptr) == VK_SUCCESS)
    {
        props.resize(n);
        enumerate(gpu, nullptr, &n, props.data());
    }
    own.extensions.assign(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
    for (const char *want : { "VK_KHR_shader_float16_int8", "VK_KHR_storage_buffer_storage_class", "VK_KHR_16bit_storage",
                              "VK_KHR_shader_subgroup_extended_types", "VK_EXT_subgroup_size_control" })
    {
        bool have = false, offered = false;
        for (const char *e : own.extensions) have |= strcmp(e, want) == 0;
        for (const auto &p : props) offered |= strcmp(p.extensionName, want) == 0;
        if (!have && offered) own.extensions.push_back(want);
    }
    ci.enabledExtensionCount = static_cast<uint32_t>(own.extensions.size());
    ci.ppEnabledExtensionNames = own.extensions.data();

    VkPhysicalDeviceShaderFloat16Int8Features sf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
    VkPhysicalDevice16BitStorageFeatures ss{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &sf };
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures se{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, &ss };
    VkPhysicalDeviceFeatures2 sup{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &se };
    reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"))(gpu, &sup);

    auto core_flags = [&](VkPhysicalDeviceFeatures &f) {
        f.shaderInt16 |= sup.features.shaderInt16;
        f.shaderImageGatherExtended |= sup.features.shaderImageGatherExtended;
        f.shaderStorageImageExtendedFormats |= sup.features.shaderStorageImageExtendedFormats;
        f.shaderStorageImageWriteWithoutFormat |= sup.features.shaderStorageImageWriteWithoutFormat;
        f.shaderStorageImageReadWithoutFormat |= sup.features.shaderStorageImageReadWithoutFormat;
    };

    bool float16_done = false, storage_done = false, subgroup_done = false, core_done = false;
    for (auto *s = const_cast<VkBaseOutStructure *>(reinterpret_cast<const VkBaseOutStructure *>(ci.pNext)); s != nullptr; s = s->pNext)
        switch (s->sType)
        {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            core_flags(reinterpret_cast<VkPhysicalDeviceFeatures2 *>(s)->features);
            core_done = true;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            reinterpret_cast<VkPhysicalDeviceVulkan11Features *>(s)->storageBuffer16BitAccess |= ss.storageBuffer16BitAccess;
            storage_done = true;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            reinterpret_cast<VkPhysicalDeviceVulkan12Features *>(s)->shaderFloat16 |= sf.shaderFloat16;
            reinterpret_cast<VkPhysicalDeviceVulkan12Features *>(s)->shaderSubgroupExtendedTypes |= se.shaderSubgroupExtendedTypes;
            float16_done = subgroup_done = true;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            reinterpret_cast<VkPhysicalDeviceShaderFloat16Int8Features *>(s)->shaderFloat16 |= sf.shaderFloat16;
            float16_done = true;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
            reinterpret_cast<VkPhysicalDevice16BitStorageFeatures *>(s)->storageBuffer16BitAccess |= ss.storageBuffer16BitAccess;
            storage_done = true;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
            reinterpret_cast<VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures *>(s)->shaderSubgroupExtendedTypes |= se.shaderSubgroupExtendedTypes;
            subgroup_done = true;
            break;
        default:
            break;
        }

    auto chain = [&](auto &own_struct) {
        own_struct.pNext = const_cast<void *>(ci.pNext);
        ci.pNext = &own_struct;
    };
    if (!float16_done) { own.float16.shaderFloat16 = sf.shaderFloat16; chain(own.float16); }
    if (!storage_done) { own.storage16.storageBuffer16BitAccess = ss.storageBuffer16BitAccess; chain(own.storage16); }
    if (!subgroup_done) { own.subgroup.shaderSubgroupExtendedTypes = se.shaderSubgroupExtendedTypes; chain(own.subgroup); }
    if (!core_done)
    {
        if (ci.pEnabledFeatures != nullptr) own.core = *ci.pEnabledFeatures;
        core_flags(own.core);
        ci.pEnabledFeatures = &own.core;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out)
{
    const auto *chain = FindChainInfo<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(g_instances.empty() ? nullptr : g_instances.back(), "vkCreateDevice"));
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const_cast<VkLayerDeviceCreateInfo *>(chain)->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    g_out_dir = EnvW(L"CK3_FSR_OUTPUT");
    const std::wstring mode = EnvW(L"CK3_FSR_MODE");
    const bool want_fsr = mode == L"Dedicated" && !g_out_dir.empty();

    VkDeviceCreateInfo modified = *ci;
    FsrDeviceFeatures own;
    if (want_fsr && !g_instances.empty()) AddFsrFeatures(gpu, g_instances.back(), gipa, modified, own);
    VkResult r = create(gpu, &modified, a, out);
    if (r != VK_SUCCESS && want_fsr)
    {
        Log("vkCreateDevice failed (%d) with the FSR features; retrying with the game's own request; FSR off", r);
        r = create(gpu, ci, a, out);
        if (r == VK_SUCCESS) g_fsr.failed = true;
    }
    if (r != VK_SUCCESS) return r;

    g_next_gdpa = gdpa;
    g_device = *out;
    g_gpu = gpu;
#define X(name) next.name = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name));
    NEXT_FUNCS(X)
#undef X

    g_census = EnvW(L"CK3_FSR_CENSUS") == L"1" && !g_out_dir.empty();
    g_fsr_mode = want_fsr && !g_fsr.failed;
    g_csv_mode = !g_out_dir.empty() && mode != L"Integrated" && !mode.empty();
    g_track = g_census || g_fsr_mode || g_csv_mode;
    if (const std::wstring sharp = EnvW(L"CK3_FSR_SHARPNESS"); !sharp.empty()) g_sharpness = static_cast<float>(_wtof(sharp.c_str()));
    g_ffx_debug = EnvW(L"CK3_FSR_DEBUG") == L"1";
    g_freeze_input = EnvW(L"CK3_FSR_FREEZE_INPUT") == L"1" && g_csv_mode;
    g_dump_portrait = EnvW(L"CK3_FSR_DUMP_PORTRAIT") == L"1" && g_fsr_mode;
    g_spatial_mode = mode == L"Off" && g_csv_mode && EnvW(L"CK3_FSR_SPATIAL") != L"0";
    g_no_motion = EnvW(L"CK3_FSR_NO_MOTION") == L"1";
    g_portrait_scale = 1;
    if (g_fsr_mode)
    {
        const std::wstring scale = EnvW(L"CK3_FSR_PORTRAIT_SCALE");
        g_portrait_scale = scale.empty() ? 1.5f : static_cast<float>(_wtof(scale.c_str()));
    }

    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_csv_start);
    if (g_csv_mode && (g_csv = _wfsopen((g_out_dir + L"\\frames.csv").c_str(), L"w", _SH_DENYNO)) != nullptr)
    {
        setvbuf(g_csv, nullptr, _IOFBF, 4096);
        fprintf(g_csv, "t_ms,frame,dt_ms,fsr_enabled,fsr_success,fsr_cpu_ms,worker_ms,fsr_gpu_ms,input_w,input_h,output_w,output_h,gpu,readback_wait_ms,input_copy_ms,worker_wait_ms,output_copy_ms,upload_wait_ms,scene_draws\n");
        fflush(g_csv);
    }
    VkPhysicalDeviceProperties props{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(g_instances.back(), "vkGetPhysicalDeviceProperties"))(gpu, &props);
    g_gpu_name = props.deviceName;
    char mode_text[32] = {};
    WideCharToMultiByte(CP_UTF8, 0, mode.c_str(), -1, mode_text, sizeof(mode_text), nullptr, nullptr);
    Log("device created on %s; mode=%s census=%d fsr=%d portrait scale=%.2f spatial=%d", props.deviceName, mode_text, g_census, g_fsr_mode, g_portrait_scale, g_spatial_mode);
    return r;
}

// Hooks are offered only for functions the next layer exposes; alias names of promoted entry
// points resolve to the same hook.
PFN_vkVoidFunction Lookup(const char *name)
{
    if (!g_track && !g_csv_mode) return nullptr;
#define H(api, hook) if (strcmp(name, api) == 0) return next.hook != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(Hook##hook) : nullptr;
    H("vkQueuePresentKHR", QueuePresentKHR) H("vkDestroyDevice", DestroyDevice)
    if (!g_track) return nullptr;
    H("vkCreateImage", CreateImage) H("vkDestroyImage", DestroyImage)
    H("vkCreateImageView", CreateImageView) H("vkDestroyImageView", DestroyImageView)
    H("vkCreateRenderPass", CreateRenderPass) H("vkCreateRenderPass2", CreateRenderPass2) H("vkCreateRenderPass2KHR", CreateRenderPass2)
    H("vkCreateFramebuffer", CreateFramebuffer) H("vkDestroyFramebuffer", DestroyFramebuffer)
    H("vkCreateShaderModule", CreateShaderModule) H("vkDestroyShaderModule", DestroyShaderModule)
    H("vkCreateGraphicsPipelines", CreateGraphicsPipelines) H("vkCreateSwapchainKHR", CreateSwapchainKHR)
    H("vkBeginCommandBuffer", BeginCommandBuffer) H("vkEndCommandBuffer", EndCommandBuffer)
    H("vkCmdBeginRenderPass", CmdBeginRenderPass) H("vkCmdBeginRenderPass2", CmdBeginRenderPass2) H("vkCmdBeginRenderPass2KHR", CmdBeginRenderPass2)
    H("vkCmdEndRenderPass", CmdEndRenderPass) H("vkCmdEndRenderPass2", CmdEndRenderPass2) H("vkCmdEndRenderPass2KHR", CmdEndRenderPass2)
    H("vkCmdBeginRendering", CmdBeginRendering) H("vkCmdBeginRenderingKHR", CmdBeginRendering)
    H("vkCmdEndRendering", CmdEndRendering) H("vkCmdEndRenderingKHR", CmdEndRendering)
    H("vkCmdBindPipeline", CmdBindPipeline) H("vkCmdSetViewport", CmdSetViewport)
    H("vkCmdSetScissor", CmdSetScissor) H("vkCmdClearAttachments", CmdClearAttachments)
    H("vkCmdCopyBufferToImage", CmdCopyBufferToImage)
    H("vkUpdateDescriptorSets", UpdateDescriptorSets) H("vkCmdPushDescriptorSetKHR", CmdPushDescriptorSetKHR)
    H("vkCreateDescriptorSetLayout", CreateDescriptorSetLayout) H("vkAllocateDescriptorSets", AllocateDescriptorSets)
    H("vkBindBufferMemory", BindBufferMemory) H("vkMapMemory", MapMemory) H("vkUnmapMemory", UnmapMemory)
    H("vkCmdBindDescriptorSets", CmdBindDescriptorSets)
    H("vkUpdateDescriptorSetWithTemplate", UpdateDescriptorSetWithTemplate) H("vkUpdateDescriptorSetWithTemplateKHR", UpdateDescriptorSetWithTemplate)
    H("vkCmdDraw", CmdDraw) H("vkCmdDrawIndexed", CmdDrawIndexed) H("vkCmdDrawIndirect", CmdDrawIndirect) H("vkCmdDrawIndexedIndirect", CmdDrawIndexedIndirect)
    H("vkCmdDispatch", CmdDispatch) H("vkCmdBlitImage", CmdBlitImage) H("vkCmdCopyImage", CmdCopyImage) H("vkCmdResolveImage", CmdResolveImage)
    H("vkCmdExecuteCommands", CmdExecuteCommands)
    H("vkCmdPipelineBarrier", CmdPipelineBarrier) H("vkCmdPipelineBarrier2", CmdPipelineBarrier2) H("vkCmdPipelineBarrier2KHR", CmdPipelineBarrier2)
    H("vkQueueSubmit", QueueSubmit) H("vkQueueSubmit2", QueueSubmit2) H("vkQueueSubmit2KHR", QueueSubmit2)
#undef H
    return nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Ck3FsrGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Ck3FsrGetDeviceProcAddr);
    if (const PFN_vkVoidFunction hook = Lookup(name)) return hook;
    return g_next_gdpa != nullptr ? g_next_gdpa(device, name) : nullptr;
}

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Ck3FsrGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(HookCreateInstance);
    if (strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(HookDestroyInstance);
    if (strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(HookCreateDevice);
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Ck3FsrGetInstanceProcAddr);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Ck3FsrGetDeviceProcAddr);
    if (g_device != VK_NULL_HANDLE)
        if (const PFN_vkVoidFunction hook = Lookup(name)) return hook;
    return g_next_gipa != nullptr ? g_next_gipa(instance, name) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v == nullptr || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = Ck3FsrGetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = Ck3FsrGetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
