// feed_render_dump.h -- portrait detection + diagnostic render dumper. Registered at start-up when
// portrait_mode=1 (detection only, lean) or render_dump=1 (detection + logging + census); changing
// either needs a game restart. Included by dlss5-feed.cpp after Log/Cfg/FormatName.
//
// ReShade's add-on events fire for the game's own Vulkan device, so this sees every pipeline,
// descriptor update and draw CK3 issues -- no ck3.exe hooks, nothing pinned to a game build.
//
//   1. Pipelines are tagged from the names DXC leaves in the SPIR-V:
//        GUI portrait composite  -- cbuffer member "PortraitUVOffset" (jomini gui_portrait.shader)
//        3D character mesh       -- "PatternColorMasks_Texture" (portrait.shader, court_scene.shader)
//      For the GUI portrait, the set/binding and member offsets of the cbuffer holding
//      WidgetPos/WidgetSize/PortraitTextureSize are parsed out of the SPIR-V.
//   2. Each GUI-portrait draw resolves the buffer + offset backing that cbuffer (descriptor
//      table + dynamic offset, push descriptor, or push constants).
//   3. At present the bytes are copied into a readback ring and decoded 3 frames later, so the
//      log shows the real on-screen portrait rectangles whatever way the game uploaded them.
//   4. Ctrl+Shift+F11 logs one frame's render-target census: every render pass / RT bind with
//      its size, format and draw count, plus how many of those draws were tagged pipelines.
//
// Frontier 3 additions, render_dump=1 only (portrait_mode alone stays lean):
//   5. The set/binding of the four textures the portrait shader samples (Frame, Mask, Portrait,
//      Background) come from the SPIR-V of both stages. Image descriptor updates and texture
//      view lifetimes are tracked, so every portrait draw records which texture/view/mip it
//      sampled; each texture is described once in the log (T#id) and per-draw lines name it.
//   6. The census hotkey also probes the Portrait and Mask textures of that frame: they are
//      copied to a readback buffer, alpha statistics are logged for the exact texel rectangle
//      each draw samples (PortraitUVOffset/Scale), and PNGs are written to dlss5-feed-dump\.
//      To make that copy legal, dump mode adds copy_source to sampled 2D textures at creation.
//   7. GetPortraits' data carries its frame of origin (PortraitAge) for frame association.

#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "feed_dump_probe.h"

namespace dump
{
using namespace reshade::api;
using dprobe::CbInfo;
using dprobe::IMG_COUNT;
using dprobe::IMG_PORTRAIT;
using dprobe::IMG_MASK;

enum : uint8_t { TAG_NONE = 0, TAG_GUI_PORTRAIT = 1, TAG_CHAR3D = 2 };

// ---------------------------------------------------------------------------
// SPIR-V scan (parsing lives in feed_dump_probe.h)
// ---------------------------------------------------------------------------

static bool BytesContain(const void *code, size_t size, const char *needle)
{
    const size_t n = strlen(needle);
    const char *p = static_cast<const char *>(code);
    for (size_t i = 0; i + n <= size; ++i)
        if (p[i] == needle[0] && memcmp(p + i, needle, n) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Tracked state
// ---------------------------------------------------------------------------

struct PipeInfo { uint8_t tag; CbInfo cb; };
struct RangeInfo { uint32_t binding, count; descriptor_type type; };
struct ParamInfo { pipeline_layout_param_type type; std::vector<RangeInfo> ranges; };

static std::shared_mutex g_pipe_mx;
static std::unordered_map<uint64_t, PipeInfo> g_pipes;                  // pipeline -> tag
static std::unordered_map<uint64_t, std::vector<ParamInfo>> g_layouts;  // pipeline_layout -> params

struct DescEntry { buffer_range range; descriptor_type type; };
static std::mutex g_desc_mx;
static std::unordered_map<uint64_t, DescEntry> g_desc;  // hash(table, binding) -> constant-buffer range
static std::unordered_map<uint64_t, uint64_t>  g_img;   // hash(table, binding) -> texture view (render_dump=1 only)

static uint64_t DescKey(uint64_t table, uint32_t binding) { return table * 0x9E3779B97F4A7C15ull ^ binding; }

// Texture and view lifetimes (render_dump=1 only). Descriptor handles are only resolved
// through these maps, never by asking ReShade about a handle that may have been destroyed.
struct ViewInfo { uint64_t resource; resource_view_desc desc; };
static std::mutex g_res_mx;
static std::unordered_map<uint64_t, resource_desc> g_textures;   // texture -> desc
static std::unordered_map<uint64_t, ViewInfo>      g_views;      // texture view -> resource + desc
static std::unordered_map<uint64_t, uint32_t>      g_tex_ids;    // texture -> T#id (new id after destroy)
static uint32_t                                    g_next_tex_id = 1;
static std::atomic<uint32_t> g_copy_src_added{ 0 };

static constexpr int kMaxSets = 8, kMaxDyn = 16, kMaxPush = 4, kMaxPushImg = 8;

struct __declspec(uuid("4c2a6f1e-8d3b-4b7a-9e2f-1a6c5d7e9b30")) CmdState
{
    uint64_t pipeline = 0;
    uint8_t  tag = TAG_NONE;
    CbInfo   cb;
    uint64_t tables[kMaxSets] = {};
    uint64_t set_layout[kMaxSets] = {};   // pipeline layout each set was bound with
    uint32_t dyn[kMaxSets][kMaxDyn] = {};
    uint32_t dyn_count[kMaxSets] = {};
    struct Push { uint32_t set, binding; buffer_range range; } push[kMaxPush] = {};
    uint32_t push_count = 0;
    struct PushImg { uint32_t set, binding; uint64_t view; } push_img[kMaxPushImg] = {};
    uint32_t push_img_count = 0;
    uint8_t  push_constants[128] = {};
    bool     push_constants_valid = false;
    viewport vp = {};
    rect     scissor = {};
    uint32_t rt_w = 0, rt_h = 0, rt_fmt = 0;
    int      census_entry = -1;
};

static CmdState *State(command_list *cl)
{
    CmdState *s = cl->get_private_data<CmdState>();
    return s != nullptr ? s : cl->create_private_data<CmdState>();
}

struct PortraitDraw
{
    buffer_range range;            // where the cbuffer bytes live (offset already includes dynamic offset)
    uint8_t      bytes[64];        // filled directly for push constants
    bool         cpu_bytes;
    int          source;           // 0 table, 1 push descriptor, 2 push constants
    uint32_t     dyn_index;        // which dynamic offset was applied (source 0)
    uint32_t     rt_w, rt_h;
    viewport     vp;
    rect         scissor;
    uint64_t     views[IMG_COUNT];    // render_dump=1: texture views bound at the shader's bindings
    uint32_t     tex_id[IMG_COUNT];   // render_dump=1: T#id of each view's texture (0 unknown)
};

static std::mutex g_frame_mx;
static std::vector<PortraitDraw> g_frame_draws;
static uint32_t g_frame_char3d = 0;
static std::vector<uint64_t> g_frame_char3d_rts;   // (w<<32)|h of targets 3D characters were drawn into

struct CensusEntry { uint32_t w, h, fmt, ds_w, ds_h, draws, gui_portrait, char3d; };
static std::atomic<uint64_t> g_frame{ 0 };
static std::atomic<uint64_t> g_census_frame{ UINT64_MAX };
static std::vector<CensusEntry> g_census;   // guarded by g_frame_mx

static std::atomic<uint32_t> g_pipes_seen{ 0 }, g_pipes_no_code{ 0 }, g_pipes_tagged{ 0 };
static bool g_verbose = false;   // render_dump=1: logging, census, 3D-character stats

// Published portrait rectangles (screen pixels, unclipped), decoded kSlots-1 frames after the draw.
struct PRect { int x, y, w, h; };
static std::mutex         g_pub_mx;
static std::vector<PRect> g_pub;
static uint32_t           g_pub_stable = 0;   // consecutive decodes with an unchanged set
static uint64_t           g_pub_frame = 0;    // frame the published draws were recorded in

// Copy of the latest portrait rectangles; returns how many decodes in a row they have been stable.
static uint32_t GetPortraits(std::vector<PRect> &out)
{
    std::lock_guard<std::mutex> lock(g_pub_mx);
    out = g_pub;
    return g_pub_stable;
}

// Frames between the draws behind GetPortraits' rectangles and now; -1 before the first decode.
static int64_t PortraitAge()
{
    std::lock_guard<std::mutex> lock(g_pub_mx);
    return g_pub_frame == 0 ? -1 : static_cast<int64_t>(g_frame.load()) - static_cast<int64_t>(g_pub_frame);
}

// readback ring
static constexpr uint32_t kSlots = 4, kMaxPerFrame = 32, kStride = 256;
static device  *g_rb_dev = nullptr;
static resource g_rb = { 0 };
static std::vector<PortraitDraw> g_slot_meta[kSlots];
static CbInfo   g_slot_cb[kSlots];
static uint64_t g_slot_frame[kSlots] = {};
static CbInfo   g_last_cb;   // offsets of the most recently bound GUI-portrait cbuffer
static std::string g_last_logged;
static uint64_t g_last_log_frame = 0;

// Census-frame texture probe (render_dump=1). Decoded kProbeDelay presents later: a frame
// count, not a fence -- acceptable for this diagnostic only (the copies are a few MB).
static constexpr uint64_t kProbeDelay = 16, kProbeMaxBytes = 128ull << 20;
struct ProbeTex
{
    uint64_t resource; uint32_t id, kind, fmt, w, h, level, subresource, row_pitch; uint64_t offset;
};
struct ProbeDraw
{
    bool decoded = false;
    float pos[2] = {}, size[2] = {}, uvoff[2] = {}, uvscale[2] = {}, popout = 0, gray = 0;
    uint32_t tex_id[IMG_COUNT] = {};
    rect scissor = {};
};
struct Probe
{
    bool active = false;
    uint64_t frame = 0;
    device *dev = nullptr;
    resource buf = { 0 };
    std::vector<ProbeTex> tex;
    std::vector<ProbeDraw> draws;
};
static Probe g_probe;

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static bool OnCreateResource(device *dev, resource_desc &desc, subresource_data *, resource_usage)
{
    if (dev->get_api() != device_api::vulkan) return false;
    // Uniform buffers need TRANSFER_SRC so the present-time readback copy is legal (game's Vulkan device only).
    if (desc.type == resource_type::buffer && (desc.usage & resource_usage::constant_buffer) != 0 &&
        (desc.usage & resource_usage::copy_source) == 0)
    {
        desc.usage |= resource_usage::copy_source;
        return true;
    }
    // Dump mode only: sampled 2D textures get TRANSFER_SRC so the census probe may copy one.
    // Never transient/depth/multisampled images (they never carry shader_resource here anyway).
    if (g_verbose && desc.type == resource_type::texture_2d && (desc.usage & resource_usage::shader_resource) != 0 &&
        (desc.usage & resource_usage::depth_stencil) == 0 && (desc.usage & resource_usage::copy_source) == 0 &&
        desc.texture.samples <= 1)
    {
        desc.usage |= resource_usage::copy_source;
        ++g_copy_src_added;
        return true;
    }
    return false;
}

static void OnInitResource(device *dev, const resource_desc &desc, const subresource_data *, resource_usage, resource res)
{
    if (dev->get_api() != device_api::vulkan || desc.type != resource_type::texture_2d) return;
    std::lock_guard<std::mutex> lock(g_res_mx);
    g_textures[res.handle] = desc;
    g_tex_ids.erase(res.handle);   // a recycled handle is a new texture
}

static void OnDestroyResource(device *, resource res)
{
    std::lock_guard<std::mutex> lock(g_res_mx);
    g_textures.erase(res.handle);
    g_tex_ids.erase(res.handle);
}

static void OnInitResourceView(device *dev, resource res, resource_usage, const resource_view_desc &desc, resource_view view)
{
    if (dev->get_api() != device_api::vulkan || desc.type == resource_view_type::buffer ||
        desc.type == resource_view_type::acceleration_structure || desc.type == resource_view_type::unknown) return;
    std::lock_guard<std::mutex> lock(g_res_mx);
    g_views[view.handle] = { res.handle, desc };
}

static void OnDestroyResourceView(device *, resource_view view)
{
    std::lock_guard<std::mutex> lock(g_res_mx);
    g_views.erase(view.handle);
}

static void OnInitPipelineLayout(device *, uint32_t count, const pipeline_layout_param *params, pipeline_layout layout)
{
    std::vector<ParamInfo> out(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const pipeline_layout_param &p = params[i];
        out[i].type = p.type;
        switch (p.type)
        {
        case pipeline_layout_param_type::descriptor_table:
        case pipeline_layout_param_type::push_descriptors_with_ranges:
            for (uint32_t r = 0; r < p.descriptor_table.count; ++r)
                out[i].ranges.push_back({ p.descriptor_table.ranges[r].binding, p.descriptor_table.ranges[r].count, p.descriptor_table.ranges[r].type });
            break;
        case pipeline_layout_param_type::descriptor_table_with_flags:
        case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
            for (uint32_t r = 0; r < p.descriptor_table_with_flags.count; ++r)
                out[i].ranges.push_back({ p.descriptor_table_with_flags.ranges[r].binding, p.descriptor_table_with_flags.ranges[r].count, p.descriptor_table_with_flags.ranges[r].type });
            break;
        case pipeline_layout_param_type::push_descriptors:
            out[i].ranges.push_back({ p.push_descriptors.binding, p.push_descriptors.count, p.push_descriptors.type });
            break;
        default: break;
        }
    }
    std::unique_lock<std::shared_mutex> lock(g_pipe_mx);
    g_layouts[layout.handle] = std::move(out);
}

static void OnDestroyPipelineLayout(device *, pipeline_layout layout)
{
    std::unique_lock<std::shared_mutex> lock(g_pipe_mx);
    g_layouts.erase(layout.handle);
}

static void LogLines(const std::string &text)
{
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) Log("[dump]   %s", line.c_str());
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

static void OnInitPipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subs, pipeline pipe)
{
    ++g_pipes_seen;
    uint8_t tag = TAG_NONE;
    CbInfo cb;
    bool any_code = false;
    const bool first = g_pipes_tagged.load() == 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (subs[i].type != pipeline_subobject_type::vertex_shader && subs[i].type != pipeline_subobject_type::pixel_shader)
            continue;
        const shader_desc *sd = static_cast<const shader_desc *>(subs[i].data);
        for (uint32_t j = 0; sd != nullptr && j < subs[i].count; ++j)
        {
            if (sd[j].code == nullptr || sd[j].code_size < 20) continue;
            any_code = true;
            if (BytesContain(sd[j].code, sd[j].code_size, "PortraitUVOffset"))
            {
                // Both stages: the vertex stage carries the cbuffer, the pixel stage the textures.
                tag = TAG_GUI_PORTRAIT;
                std::string bindings;
                const CbInfo part = dprobe::SpvParsePortrait(static_cast<const uint32_t *>(sd[j].code), sd[j].code_size / 4,
                                                             first ? &bindings : nullptr);
                if (first)
                {
                    Log("[dump] first GUI-portrait shader bindings (%s stage):",
                        subs[i].type == pipeline_subobject_type::vertex_shader ? "vertex" : "pixel");
                    LogLines(bindings);
                }
                dprobe::MergeCb(cb, part);
            }
            else if (tag == TAG_NONE && BytesContain(sd[j].code, sd[j].code_size, "PatternColorMasks_Texture"))
                tag = TAG_CHAR3D;
        }
    }
    if (!any_code) ++g_pipes_no_code;
    if (tag == TAG_NONE) return;

    const uint32_t n = ++g_pipes_tagged;
    if (tag == TAG_GUI_PORTRAIT && n <= 8)
    {
        char img[160] = "";
        int len = 0;
        for (int k = 0; k < IMG_COUNT; ++k)
            len += _snprintf_s(img + len, sizeof(img) - len, _TRUNCATE, " %c=%s", dprobe::kImgLetter[k],
                               cb.img[k].found ? (std::to_string(cb.img[k].set) + "/" + std::to_string(cb.img[k].binding)).c_str() : "?");
        Log("[dump] GUI-portrait pipeline %016llX: cbuffer %s set=%u binding=%u offsets pos=%u size=%u pop=%u uvoff=%u uvscale=%u tex=%u gray=%u; textures%s (%s)",
            static_cast<unsigned long long>(pipe.handle), cb.found ? (cb.push_constant ? "push-constant" : "found") : "NOT FOUND",
            cb.set, cb.binding, cb.off_pos, cb.off_size, cb.off_popout, cb.off_uvoff, cb.off_uvscale, cb.off_tex, cb.off_gray, img,
            cb.img_by_name ? "by name" : cb.img[IMG_PORTRAIT].found ? "binding order" : "NOT FOUND");
    }
    std::unique_lock<std::shared_mutex> lock(g_pipe_mx);
    g_pipes[pipe.handle] = { tag, cb };
}

static void OnDestroyPipeline(device *, pipeline pipe)
{
    std::unique_lock<std::shared_mutex> lock(g_pipe_mx);
    g_pipes.erase(pipe.handle);
}

static uint64_t ImageViewOf(const descriptor_table_update &u, uint32_t k)
{
    if (u.type == descriptor_type::sampler_with_resource_view)
        return static_cast<const sampler_with_resource_view *>(u.descriptors)[k].view.handle;
    return static_cast<const resource_view *>(u.descriptors)[k].handle;
}

static bool OnUpdateDescriptorTables(device *, uint32_t count, const descriptor_table_update *updates)
{
    std::lock_guard<std::mutex> lock(g_desc_mx);
    for (uint32_t i = 0; i < count; ++i)
    {
        const descriptor_table_update &u = updates[i];
        if (u.descriptors == nullptr) continue;
        if (u.type == descriptor_type::constant_buffer || u.type == descriptor_type::constant_buffer_with_dynamic_offset)
        {
            const buffer_range *r = static_cast<const buffer_range *>(u.descriptors);
            for (uint32_t k = 0; k < u.count; ++k)
                g_desc[DescKey(u.table.handle, u.binding + u.array_offset + k)] = { r[k], u.type };
        }
        else if (g_verbose && (u.type == descriptor_type::sampler_with_resource_view || u.type == descriptor_type::shader_resource_view))
        {
            for (uint32_t k = 0; k < u.count; ++k)
                g_img[DescKey(u.table.handle, u.binding + u.array_offset + k)] = ImageViewOf(u, k);
        }
    }
    if (g_desc.size() > (1u << 20)) g_desc.clear();   // handles are recycled; never grow unbounded
    if (g_img.size() > (1u << 20)) g_img.clear();
    return false;
}

static bool OnCopyDescriptorTables(device *, uint32_t count, const descriptor_table_copy *copies)
{
    std::lock_guard<std::mutex> lock(g_desc_mx);
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t k = 0; k < copies[i].count; ++k)
        {
            const uint64_t src = DescKey(copies[i].source_table.handle, copies[i].source_binding + copies[i].source_array_offset + k);
            const uint64_t dst = DescKey(copies[i].dest_table.handle, copies[i].dest_binding + copies[i].dest_array_offset + k);
            const auto it = g_desc.find(src);
            if (it != g_desc.end()) g_desc[dst] = it->second;
            if (g_verbose)
            {
                const auto im = g_img.find(src);
                if (im != g_img.end()) g_img[dst] = im->second;
            }
        }
    return false;
}

static void OnBindPipeline(command_list *cl, pipeline_stage stages, pipeline pipe)
{
    if ((stages & pipeline_stage::all_graphics) == 0) return;
    CmdState *s = State(cl);
    s->pipeline = pipe.handle;
    s->tag = TAG_NONE;
    std::shared_lock<std::shared_mutex> lock(g_pipe_mx);
    const auto it = g_pipes.find(pipe.handle);
    if (it != g_pipes.end()) { s->tag = it->second.tag; s->cb = it->second.cb; }
}

// Dynamic descriptors are counted per set in binding order (Vulkan rule).
static uint32_t DynamicCount(const std::vector<ParamInfo> *params, uint32_t set)
{
    if (params == nullptr || set >= params->size()) return 0;
    uint32_t n = 0;
    for (const RangeInfo &r : (*params)[set].ranges)
        if (r.type == descriptor_type::constant_buffer_with_dynamic_offset || r.type == descriptor_type::shader_storage_buffer_with_dynamic_offset)
            n += r.count;
    return n;
}

static void OnBindDescriptorTables(command_list *cl, shader_stage, pipeline_layout layout, uint32_t first, uint32_t count,
                                   const descriptor_table *tables, uint32_t dyn_count, const uint32_t *dyn)
{
    CmdState *s = State(cl);
    const std::vector<ParamInfo> *params = nullptr;
    std::shared_lock<std::shared_mutex> lock(g_pipe_mx);
    const auto it = g_layouts.find(layout.handle);
    if (it != g_layouts.end()) params = &it->second;
    uint32_t used = 0;
    for (uint32_t i = 0; i < count && first + i < kMaxSets; ++i)
    {
        const uint32_t set = first + i;
        s->tables[set] = tables[i].handle;
        s->set_layout[set] = layout.handle;
        uint32_t n = params != nullptr ? DynamicCount(params, set) : (i == 0 ? dyn_count : 0);
        n = (std::min)(n, (std::min)(static_cast<uint32_t>(kMaxDyn), dyn_count - (std::min)(used, dyn_count)));
        for (uint32_t k = 0; k < n; ++k) s->dyn[set][k] = dyn[used + k];
        s->dyn_count[set] = n;
        used += n;
    }
}

static void OnPushDescriptors(command_list *cl, shader_stage, pipeline_layout, uint32_t param, const descriptor_table_update &u)
{
    if (u.descriptors == nullptr) return;
    if (g_verbose && (u.type == descriptor_type::sampler_with_resource_view || u.type == descriptor_type::shader_resource_view))
    {
        CmdState *s = State(cl);
        for (uint32_t k = 0; k < u.count; ++k)
        {
            const uint32_t binding = u.binding + u.array_offset + k;
            uint32_t slot = 0;
            while (slot < s->push_img_count && !(s->push_img[slot].set == param && s->push_img[slot].binding == binding)) ++slot;
            if (slot == s->push_img_count) { if (s->push_img_count == kMaxPushImg) slot = 0; else ++s->push_img_count; }
            s->push_img[slot] = { param, binding, ImageViewOf(u, k) };
        }
        return;
    }
    if (u.type != descriptor_type::constant_buffer && u.type != descriptor_type::constant_buffer_with_dynamic_offset) return;
    CmdState *s = State(cl);
    const buffer_range *r = static_cast<const buffer_range *>(u.descriptors);
    for (uint32_t k = 0; k < u.count; ++k)
    {
        uint32_t slot = 0;
        while (slot < s->push_count && !(s->push[slot].set == param && s->push[slot].binding == u.binding + k)) ++slot;
        if (slot == s->push_count) { if (s->push_count == kMaxPush) slot = 0; else ++s->push_count; }
        s->push[slot] = { param, u.binding + k, r[k] };
    }
}

static void OnPushConstants(command_list *cl, shader_stage, pipeline_layout, uint32_t, uint32_t first, uint32_t count, const void *values)
{
    CmdState *s = State(cl);
    const uint32_t off = first * 4, bytes = count * 4;
    if (off >= sizeof(s->push_constants)) return;
    memcpy(s->push_constants + off, values, (std::min)(bytes, static_cast<uint32_t>(sizeof(s->push_constants)) - off));
    s->push_constants_valid = true;
}

static void OnBindViewports(command_list *cl, uint32_t first, uint32_t count, const viewport *vps)
{
    if (first == 0 && count > 0) State(cl)->vp = vps[0];
}

static void OnBindScissors(command_list *cl, uint32_t first, uint32_t count, const rect *rects)
{
    if (first == 0 && count > 0) State(cl)->scissor = rects[0];
}

static void TrackTarget(command_list *cl, resource_view rtv, resource_view dsv)
{
    CmdState *s = State(cl);
    s->census_entry = -1;
    if (!g_verbose) return;
    device *dev = cl->get_device();
    s->rt_w = s->rt_h = s->rt_fmt = 0;
    uint32_t ds_w = 0, ds_h = 0;
    if (rtv.handle != 0)
    {
        const resource_desc d = dev->get_resource_desc(dev->get_resource_from_view(rtv));
        s->rt_w = d.texture.width; s->rt_h = d.texture.height; s->rt_fmt = static_cast<uint32_t>(d.texture.format);
    }
    if (dsv.handle != 0)
    {
        const resource_desc d = dev->get_resource_desc(dev->get_resource_from_view(dsv));
        ds_w = d.texture.width; ds_h = d.texture.height;
    }
    s->census_entry = -1;
    if (g_census_frame.load(std::memory_order_relaxed) == g_frame.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(g_frame_mx);
        if (g_census.size() < 4096)
        {
            s->census_entry = static_cast<int>(g_census.size());
            g_census.push_back({ s->rt_w, s->rt_h, s->rt_fmt, ds_w, ds_h, 0, 0, 0 });
        }
    }
}

static bool OnBeginRenderPass(command_list *cl, uint32_t count, const render_pass_render_target_desc *rts,
                              const render_pass_depth_stencil_desc *ds, render_pass_flags)
{
    TrackTarget(cl, count > 0 ? rts[0].view : resource_view{ 0 }, ds != nullptr ? ds->view : resource_view{ 0 });
    return false;
}

static void OnBindRenderTargets(command_list *cl, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    TrackTarget(cl, count > 0 ? rtvs[0] : resource_view{ 0 }, dsv);
}

// The view bound at (set, binding) for the current draw: push descriptor first, then the bound table.
static uint64_t ResolveImage(const CmdState *s, uint32_t set, uint32_t binding)
{
    for (uint32_t i = 0; i < s->push_img_count; ++i)
        if (s->push_img[i].set == set && s->push_img[i].binding == binding) return s->push_img[i].view;
    if (set >= kMaxSets || s->tables[set] == 0) return 0;
    std::lock_guard<std::mutex> lock(g_desc_mx);
    const auto it = g_img.find(DescKey(s->tables[set], binding));
    return it != g_img.end() ? it->second : 0;
}

static void RecordPortrait(CmdState *s)
{
    PortraitDraw d = {};
    d.rt_w = s->rt_w; d.rt_h = s->rt_h; d.vp = s->vp; d.scissor = s->scissor;
    const CbInfo &cb = s->cb;
    if (!cb.found) return;
    if (cb.push_constant)
    {
        if (!s->push_constants_valid) return;
        memcpy(d.bytes, s->push_constants, sizeof(d.bytes));
        d.cpu_bytes = true;
        d.source = 2;
    }
    else
    {
        bool have = false;
        for (uint32_t i = 0; i < s->push_count && !have; ++i)
            if (s->push[i].set == cb.set && s->push[i].binding == cb.binding) { d.range = s->push[i].range; d.source = 1; have = true; }
        if (!have && cb.set < kMaxSets && s->tables[cb.set] != 0)
        {
            DescEntry e = {};
            {
                std::lock_guard<std::mutex> lock(g_desc_mx);
                const auto it = g_desc.find(DescKey(s->tables[cb.set], cb.binding));
                if (it == g_desc.end()) return;
                e = it->second;
            }
            d.range = e.range;
            d.source = 0;
            if (e.type == descriptor_type::constant_buffer_with_dynamic_offset)
            {
                // Index of this binding among the set's dynamic descriptors: Vulkan orders them by binding.
                uint32_t idx = 0;
                {
                    std::shared_lock<std::shared_mutex> lock(g_pipe_mx);
                    const auto it = g_layouts.find(s->set_layout[cb.set]);
                    if (it != g_layouts.end() && cb.set < it->second.size())
                        for (const RangeInfo &r : it->second[cb.set].ranges)
                            if (r.binding < cb.binding && (r.type == descriptor_type::constant_buffer_with_dynamic_offset ||
                                                           r.type == descriptor_type::shader_storage_buffer_with_dynamic_offset))
                                idx += r.count;
                }
                d.dyn_index = idx;
                if (idx < s->dyn_count[cb.set]) d.range.offset += s->dyn[cb.set][idx];
            }
            have = true;
        }
        if (!have) return;
    }
    if (g_verbose)
        for (int k = 0; k < IMG_COUNT; ++k)
            if (cb.img[k].found) d.views[k] = ResolveImage(s, cb.img[k].set, cb.img[k].binding);
    std::lock_guard<std::mutex> lock(g_frame_mx);
    g_last_cb = cb;
    if (g_frame_draws.size() < kMaxPerFrame) g_frame_draws.push_back(d);
}

static void OnAnyDraw(command_list *cl)
{
    CmdState *s = cl->get_private_data<CmdState>();
    if (s == nullptr) return;
    if (s->census_entry >= 0)
    {
        std::lock_guard<std::mutex> lock(g_frame_mx);
        if (static_cast<size_t>(s->census_entry) < g_census.size())
        {
            CensusEntry &e = g_census[s->census_entry];
            ++e.draws;
            if (s->tag == TAG_GUI_PORTRAIT) ++e.gui_portrait;
            if (s->tag == TAG_CHAR3D) ++e.char3d;
        }
    }
    if (s->tag == TAG_GUI_PORTRAIT) RecordPortrait(s);
    else if (s->tag == TAG_CHAR3D && g_verbose)
    {
        std::lock_guard<std::mutex> lock(g_frame_mx);
        ++g_frame_char3d;
        const uint64_t key = (uint64_t(s->rt_w) << 32) | s->rt_h;
        if (g_frame_char3d_rts.size() < 16 &&
            std::find(g_frame_char3d_rts.begin(), g_frame_char3d_rts.end(), key) == g_frame_char3d_rts.end())
            g_frame_char3d_rts.push_back(key);
    }
}

static bool OnDraw(command_list *cl, uint32_t, uint32_t, uint32_t, uint32_t) { OnAnyDraw(cl); return false; }
static bool OnDrawIndexed(command_list *cl, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { OnAnyDraw(cl); return false; }
static bool OnDrawIndirect(command_list *cl, indirect_command, resource, uint64_t, uint32_t, uint32_t) { OnAnyDraw(cl); return false; }

static void OnDestroyCommandList(command_list *cl)
{
    if (cl->get_private_data<CmdState>() != nullptr) cl->destroy_private_data<CmdState>();
}

static void OnDestroyDevice(device *dev)
{
    if (dev == g_rb_dev && g_rb.handle != 0) { dev->destroy_resource(g_rb); g_rb = { 0 }; g_rb_dev = nullptr; }
    if (dev == g_probe.dev && g_probe.buf.handle != 0) { dev->destroy_resource(g_probe.buf); g_probe = Probe(); }
}

static float F(const uint8_t *b, uint32_t off) { float v = 0; if (off + 4 <= 64) memcpy(&v, b + off, 4); return v; }

static void Publish(const uint8_t *base, const std::vector<PortraitDraw> &meta, const CbInfo &cb, uint64_t frame)
{
    std::vector<PRect> rects;
    for (size_t i = 0; i < meta.size(); ++i)
    {
        const uint8_t *b = meta[i].cpu_bytes ? meta[i].bytes : base + i * kStride;
        if (b == nullptr) continue;
        const float x = F(b, cb.off_pos), y = F(b, cb.off_pos + 4), w = F(b, cb.off_size), h = F(b, cb.off_size + 4);
        if (!(w >= 1.0f && h >= 1.0f && w <= 8192.0f && h <= 8192.0f && x > -8192.0f && x < 8192.0f && y > -8192.0f && y < 8192.0f))
            continue;   // also rejects NaN
        rects.push_back({ static_cast<int>(lroundf(x)), static_cast<int>(lroundf(y)),
                          static_cast<int>(lroundf(w)), static_cast<int>(lroundf(h)) });
    }
    std::lock_guard<std::mutex> lock(g_pub_mx);
    bool same = rects.size() == g_pub.size();
    for (size_t i = 0; same && i < rects.size(); ++i)
        same = abs(rects[i].x - g_pub[i].x) <= 1 && abs(rects[i].y - g_pub[i].y) <= 1 &&
               rects[i].w == g_pub[i].w && rects[i].h == g_pub[i].h;
    g_pub_stable = same ? g_pub_stable + 1 : 0;
    g_pub.swap(rects);
    g_pub_frame = frame;
}

static void DecodeAndLog(const uint8_t *base, const std::vector<PortraitDraw> &meta, const CbInfo &cb, uint64_t frame)
{
    char line[6000];
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "n=%u", static_cast<unsigned>(meta.size()));
    for (size_t i = 0; i < meta.size() && len > 0 && len < static_cast<int>(sizeof(line)) - 220; ++i)
    {
        const uint8_t *b = meta[i].cpu_bytes ? meta[i].bytes : base + i * kStride;
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " | %.0f,%.0f %.0fx%.0f tex %.0fx%.0f",
                           F(b, cb.off_pos), F(b, cb.off_pos + 4), F(b, cb.off_size), F(b, cb.off_size + 4),
                           F(b, cb.off_tex), F(b, cb.off_tex + 4));
        if (g_verbose)
        {
            len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " uv %.3f,%.3f/%.3f,%.3f pop %.2f%s",
                               F(b, cb.off_uvoff), F(b, cb.off_uvoff + 4), F(b, cb.off_uvscale), F(b, cb.off_uvscale + 4),
                               F(b, cb.off_popout), F(b, cb.off_gray) > 0.5f ? " gray" : "");
            for (int k = 0; k < IMG_COUNT; ++k)
                if (meta[i].tex_id[k] != 0)
                    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %c#%u", dprobe::kImgLetter[k], meta[i].tex_id[k]);
                else if (meta[i].views[k] != 0)
                    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %c?", dprobe::kImgLetter[k]);
        }
    }
    std::string s = line;
    const bool changed = s != g_last_logged;
    if (!changed && frame - g_last_log_frame < 600) return;
    g_last_logged = s;
    g_last_log_frame = frame;
    if (!meta.empty())
    {
        const PortraitDraw &m = meta[0];
        Log("[dump] frame %llu portraits %s  (src=%d dyn=%u rt %ux%u vp %.0f,%.0f %.0fx%.0f scissor %d,%d-%d,%d buf %016llX+%llu)",
            static_cast<unsigned long long>(frame), line, m.source, m.dyn_index, m.rt_w, m.rt_h, m.vp.x, m.vp.y, m.vp.width, m.vp.height,
            m.scissor.left, m.scissor.top, m.scissor.right, m.scissor.bottom,
            static_cast<unsigned long long>(m.range.buffer.handle), static_cast<unsigned long long>(m.range.offset));
    }
    else
        Log("[dump] frame %llu portraits n=0", static_cast<unsigned long long>(frame));
}

// render_dump=1: give every sampled texture a short T#id and describe each once.
static void AnnotateTextures(std::vector<PortraitDraw> &draws)
{
    std::vector<std::string> fresh;
    {
        std::lock_guard<std::mutex> lock(g_res_mx);
        for (PortraitDraw &d : draws)
            for (int k = 0; k < IMG_COUNT; ++k)
            {
                d.tex_id[k] = 0;
                if (d.views[k] == 0) continue;
                const auto v = g_views.find(d.views[k]);
                if (v == g_views.end()) continue;
                const auto t = g_textures.find(v->second.resource);
                if (t == g_textures.end()) continue;
                auto id = g_tex_ids.find(v->second.resource);
                if (id == g_tex_ids.end())
                {
                    id = g_tex_ids.emplace(v->second.resource, g_next_tex_id++).first;
                    const resource_desc &rd = t->second;
                    const resource_view_desc &vd = v->second.desc;
                    char buf[512];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                                "[dump] texture T#%u (first seen as %s): resource %016llX %ux%u %s levels=%u layers=%u usage=0x%X heap=%u; "
                                "view %016llX %s mips %u+%d layers %u+%d",
                                id->second, dprobe::kImgName[k], static_cast<unsigned long long>(v->second.resource),
                                rd.texture.width, rd.texture.height, FormatName(static_cast<DXGI_FORMAT>(rd.texture.format)),
                                rd.texture.levels, rd.texture.depth_or_layers, static_cast<unsigned>(rd.usage), static_cast<unsigned>(rd.heap),
                                static_cast<unsigned long long>(d.views[k]), FormatName(static_cast<DXGI_FORMAT>(vd.format)),
                                vd.texture.first_level, vd.texture.levels == UINT32_MAX ? -1 : static_cast<int>(vd.texture.levels),
                                vd.texture.first_layer, vd.texture.layers == UINT32_MAX ? -1 : static_cast<int>(vd.texture.layers));
                    fresh.push_back(buf);
                }
                d.tex_id[k] = id->second;
            }
    }
    for (const std::string &l : fresh) Log("%s", l.c_str());
}

static void DumpDir(char *out)
{
    strcpy_s(out, MAX_PATH, g_log_path);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "dlss5-feed-dump");
    CreateDirectoryA(out, nullptr);
}

// Census frame: copy each distinct Portrait/Mask texture's sampled mip to a readback buffer.
static void StartProbe(device *dev, command_list *cl, const std::vector<PortraitDraw> &draws, uint64_t frame)
{
    if (g_probe.active) { Log("[dump] probe: previous probe still pending; not probing frame %llu", static_cast<unsigned long long>(frame)); return; }
    Probe p;
    p.frame = frame;
    p.dev = dev;
    p.draws.resize(draws.size());
    uint64_t total = 0;
    std::vector<std::string> notes;
    {
        std::lock_guard<std::mutex> lock(g_res_mx);
        for (size_t i = 0; i < draws.size(); ++i)
        {
            memcpy(p.draws[i].tex_id, draws[i].tex_id, sizeof(p.draws[i].tex_id));
            p.draws[i].scissor = draws[i].scissor;
            static const int kProbeKinds[] = { dprobe::IMG_PORTRAIT, dprobe::IMG_MASK, dprobe::IMG_FRAME };
            for (int k : kProbeKinds)
            {
                if (draws[i].tex_id[k] == 0) continue;
                if (std::any_of(p.tex.begin(), p.tex.end(), [&](const ProbeTex &t) { return t.id == draws[i].tex_id[k]; })) continue;
                const auto v = g_views.find(draws[i].views[k]);
                if (v == g_views.end()) continue;
                const auto t = g_textures.find(v->second.resource);
                if (t == g_textures.end()) continue;
                const resource_desc &rd = t->second;
                const uint32_t fmt = static_cast<uint32_t>(rd.texture.format);
                const uint32_t bpp = dprobe::BytesPerTexel(fmt);
                char why[160] = "";
                if (bpp == 0) _snprintf_s(why, sizeof(why), _TRUNCATE, "format %s not decodable", FormatName(static_cast<DXGI_FORMAT>(fmt)));
                else if ((rd.usage & resource_usage::copy_source) == 0) _snprintf_s(why, sizeof(why), _TRUNCATE, "no copy_source usage (0x%X)", static_cast<unsigned>(rd.usage));
                const uint32_t levels = (std::max)(1u, static_cast<uint32_t>(rd.texture.levels));
                const uint32_t level = (std::min)(v->second.desc.texture.first_level, levels - 1);
                const uint32_t layer = v->second.desc.texture.first_layer < rd.texture.depth_or_layers ? v->second.desc.texture.first_layer : 0;
                const uint32_t w = (std::max)(1u, rd.texture.width >> level), h = (std::max)(1u, rd.texture.height >> level);
                const uint64_t bytes = uint64_t(w) * h * bpp;
                if (!why[0] && (w > 4096 || h > 4096 || total + bytes > kProbeMaxBytes)) _snprintf_s(why, sizeof(why), _TRUNCATE, "too large (%ux%u)", w, h);
                if (why[0])
                {
                    char n[256];
                    _snprintf_s(n, sizeof(n), _TRUNCATE, "[dump] probe frame %llu: T#%u (%s) not probed: %s", static_cast<unsigned long long>(frame),
                                draws[i].tex_id[k], dprobe::kImgName[k], why);
                    notes.push_back(n);
                    continue;
                }
                ProbeTex pt = { v->second.resource, draws[i].tex_id[k], static_cast<uint32_t>(k), fmt, w, h, level, level + layer * levels, w * bpp, total };
                p.tex.push_back(pt);
                total += (bytes + 255) & ~uint64_t(255);
            }
        }
    }
    for (const std::string &n : notes) Log("%s", n.c_str());
    if (p.tex.empty()) { Log("[dump] probe frame %llu: no probe-able portrait textures", static_cast<unsigned long long>(frame)); return; }
    if (!dev->create_resource(resource_desc(total, memory_heap::readback, resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &p.buf))
    {
        Log("[dump] probe frame %llu: readback buffer (%llu bytes) creation failed", static_cast<unsigned long long>(frame), static_cast<unsigned long long>(total));
        return;
    }
    // The game sampled these in its GUI pass this frame, so they are in shader-read state.
    for (const ProbeTex &t : p.tex)
    {
        const resource res = { t.resource };
        cl->barrier(res, resource_usage::shader_resource, resource_usage::copy_source);
        cl->copy_texture_to_buffer(res, t.subresource, nullptr, p.buf, t.offset, 0, 0);
        cl->barrier(res, resource_usage::copy_source, resource_usage::shader_resource);
    }
    p.active = true;
    Log("[dump] probe frame %llu: copying %u texture(s), %llu bytes; results in %llu presents", static_cast<unsigned long long>(frame),
        static_cast<unsigned>(p.tex.size()), static_cast<unsigned long long>(total), static_cast<unsigned long long>(kProbeDelay));
    g_probe = std::move(p);
}

static double Pct(uint64_t a, uint64_t n) { return n ? 100.0 * double(a) / double(n) : 0.0; }

static void FinishProbe()
{
    Probe &p = g_probe;
    void *mapped = nullptr;
    if (!p.dev->map_buffer_region(p.buf, 0, UINT64_MAX, map_access::read_only, &mapped) || mapped == nullptr)
        Log("[dump] probe frame %llu: map failed", static_cast<unsigned long long>(p.frame));
    else
    {
        const uint8_t *base = static_cast<const uint8_t *>(mapped);
        char dir[MAX_PATH];
        DumpDir(dir);
        for (const ProbeTex &t : p.tex)
        {
            const uint8_t *data = base + t.offset;
            const dprobe::AlphaStats s = dprobe::Measure(data, t.row_pitch, t.fmt, 0, 0, int(t.w), int(t.h));
            std::vector<uint8_t> rgba, alpha;
            char png[MAX_PATH] = "", png_a[MAX_PATH] = "";
            if (dprobe::ToRgba8(data, t.row_pitch, t.fmt, t.w, t.h, rgba, alpha))
            {
                _snprintf_s(png, sizeof(png), _TRUNCATE, "%s\\f%llu-T%u-%s.png", dir, static_cast<unsigned long long>(p.frame), t.id, dprobe::kImgName[t.kind]);
                _snprintf_s(png_a, sizeof(png_a), _TRUNCATE, "%s\\f%llu-T%u-%s-alpha.png", dir, static_cast<unsigned long long>(p.frame), t.id, dprobe::kImgName[t.kind]);
                if (!dprobe::WritePng(png, t.w, t.h, rgba.data(), 4)) png[0] = '\0';
                if (!dprobe::WritePng(png_a, t.w, t.h, alpha.data(), 1)) png_a[0] = '\0';
            }
            Log("[dump] probe frame %llu T#%u %s %ux%u mip %u %s: alpha zero %.1f%% full %.1f%% partial %.1f%% mean %.3f range %.3f..%.3f; "
                "edge ring zero %.1f%%; not premultiplied %.2f%% -> %s",
                static_cast<unsigned long long>(p.frame), t.id, dprobe::kImgName[t.kind], t.w, t.h, t.level,
                FormatName(static_cast<DXGI_FORMAT>(t.fmt)), Pct(s.zero, s.n), Pct(s.full, s.n), Pct(s.partial, s.n),
                s.n ? s.sum_a / double(s.n) : 0.0, s.n ? s.min_a : 0.0f, s.n ? s.max_a : 0.0f, Pct(s.ring_zero, s.ring_n),
                Pct(s.premult_violations, s.n), png[0] ? png : "(no png)");
        }
        // Per draw: the texel rectangle of the Portrait texture the widget actually samples.
        for (size_t i = 0; i < p.draws.size(); ++i)
        {
            const ProbeDraw &d = p.draws[i];
            if (!d.decoded) continue;
            const auto pt = std::find_if(p.tex.begin(), p.tex.end(), [&](const ProbeTex &t) { return t.id == d.tex_id[IMG_PORTRAIT] && d.tex_id[IMG_PORTRAIT] != 0; });
            char what[400] = "Portrait texture not probed";
            if (pt != p.tex.end())
            {
                int x0, y0, x1, y1;
                if (dprobe::UvTexelRect(d.uvoff[0], d.uvoff[1], d.uvscale[0], d.uvscale[1], pt->w, pt->h, x0, y0, x1, y1))
                {
                    const dprobe::AlphaStats s = dprobe::Measure(base + pt->offset, pt->row_pitch, pt->fmt, x0, y0, x1, y1);
                    const bool coverage_like = Pct(s.ring_zero, s.ring_n) > 50.0 && s.full > 0 && s.zero > 0;
                    _snprintf_s(what, sizeof(what), _TRUNCATE,
                                "T#%u texels %d,%d-%d,%d: alpha zero %.1f%% full %.1f%% partial %.1f%%, edge ring zero %.1f%%, not premultiplied %.2f%% => %s",
                                pt->id, x0, y0, x1, y1, Pct(s.zero, s.n), Pct(s.full, s.n), Pct(s.partial, s.n), Pct(s.ring_zero, s.ring_n),
                                Pct(s.premult_violations, s.n), coverage_like ? "alpha looks like character coverage" : "alpha does NOT look like coverage");
                }
                else
                    _snprintf_s(what, sizeof(what), _TRUNCATE, "T#%u: degenerate UV transform", pt->id);
            }
            Log("[dump] probe frame %llu draw %u: widget %.0f,%.0f %.0fx%.0f uv off %.4f,%.4f scale %.4f,%.4f pop %.2f%s M#%u F#%u B#%u scissor %d,%d-%d,%d: %s",
                static_cast<unsigned long long>(p.frame), static_cast<unsigned>(i), d.pos[0], d.pos[1], d.size[0], d.size[1],
                d.uvoff[0], d.uvoff[1], d.uvscale[0], d.uvscale[1], d.popout, d.gray > 0.5f ? " gray" : "",
                d.tex_id[IMG_MASK], d.tex_id[dprobe::IMG_FRAME], d.tex_id[dprobe::IMG_BACKGROUND],
                d.scissor.left, d.scissor.top, d.scissor.right, d.scissor.bottom, what);
        }
        p.dev->unmap_buffer_region(p.buf);
    }
    p.dev->destroy_resource(p.buf);
    g_probe = Probe();
}

static void OnPresent(effect_runtime *rt)
{
    // The DXGI bridge's hidden D3D12 swapchain has its own runtime; only the game's Vulkan one counts.
    if (rt->get_device()->get_api() != device_api::vulkan) return;
    if (g_rb_dev != nullptr && g_rb_dev != rt->get_device()) { g_rb = { 0 }; g_rb_dev = nullptr; }

    const uint64_t frame = g_frame.fetch_add(1) + 1;   // draws recorded from now on belong to `frame`

    // Census hotkey: Ctrl+Shift+F11 arms the next frame.
    static bool key_down = false;
    const bool down = g_verbose && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (down && !key_down) { g_census_frame = frame; Log("[dump] census armed for frame %llu", static_cast<unsigned long long>(frame)); }
    key_down = down;

    std::vector<PortraitDraw> draws;
    std::vector<CensusEntry> census;
    std::vector<uint64_t> char3d_rts;
    uint32_t char3d = 0;
    bool census_now = false;
    CbInfo cb;
    {
        std::lock_guard<std::mutex> lock(g_frame_mx);
        draws.swap(g_frame_draws);
        char3d_rts.swap(g_frame_char3d_rts);
        char3d = g_frame_char3d; g_frame_char3d = 0;
        cb = g_last_cb;
        if (g_census_frame.load() == frame - 1) { census.swap(g_census); g_census_frame = UINT64_MAX; census_now = true; }
    }
    if (g_verbose) AnnotateTextures(draws);

    if (!census.empty())
    {
        Log("[dump] ===== census of frame %llu: %u render-target binds =====", static_cast<unsigned long long>(frame - 1),
            static_cast<unsigned>(census.size()));
        for (size_t i = 0; i < census.size(); ++i)
        {
            const CensusEntry &e = census[i];
            if (e.draws == 0) continue;
            Log("[dump]  #%03u rt %ux%u %s ds %ux%u draws=%u gui_portrait=%u char3d=%u", static_cast<unsigned>(i), e.w, e.h,
                FormatName(static_cast<DXGI_FORMAT>(e.fmt)), e.ds_w, e.ds_h, e.draws, e.gui_portrait, e.char3d);
        }
        Log("[dump] ===== end census (pipelines seen=%u tagged=%u without SPIR-V=%u; copy_source added to %u sampled textures) =====",
            g_pipes_seen.load(), g_pipes_tagged.load(), g_pipes_no_code.load(), g_copy_src_added.load());
    }

    device *dev = rt->get_device();
    if (g_rb.handle == 0)
    {
        if (!dev->create_resource(resource_desc(uint64_t(kSlots) * kMaxPerFrame * kStride, memory_heap::readback, resource_usage::copy_dest),
                                  nullptr, resource_usage::copy_dest, &g_rb))
        {
            static bool said = false;
            if (!said) Log("[dump] readback buffer creation failed; portrait rectangles from descriptors unavailable");
            said = true;
            g_rb = { 0 };
        }
        else g_rb_dev = dev;
    }

    // Decode the slot written kSlots-1 frames ago (its copies have retired by now).
    const uint32_t read_slot = static_cast<uint32_t>((frame + 1) % kSlots);
    if (g_slot_frame[read_slot] != 0)
    {
        void *mapped = nullptr;
        const std::vector<PortraitDraw> &meta = g_slot_meta[read_slot];
        const bool need_map = std::any_of(meta.begin(), meta.end(), [](const PortraitDraw &d) { return !d.cpu_bytes; });
        if (!need_map || (g_rb.handle != 0 && dev->map_buffer_region(g_rb, uint64_t(read_slot) * kMaxPerFrame * kStride,
                                                                     uint64_t(kMaxPerFrame) * kStride, map_access::read_only, &mapped)))
        {
            const uint8_t *base = static_cast<const uint8_t *>(mapped);
            const CbInfo &scb = g_slot_cb[read_slot];
            Publish(base, meta, scb, g_slot_frame[read_slot]);
            if (g_verbose) DecodeAndLog(base, meta, scb, g_slot_frame[read_slot]);
            if (g_probe.active && g_probe.frame == g_slot_frame[read_slot])
                for (size_t i = 0; i < meta.size() && i < g_probe.draws.size(); ++i)
                {
                    const uint8_t *b = meta[i].cpu_bytes ? meta[i].bytes : base != nullptr ? base + i * kStride : nullptr;
                    if (b == nullptr) continue;
                    ProbeDraw &d = g_probe.draws[i];
                    d.pos[0] = F(b, scb.off_pos); d.pos[1] = F(b, scb.off_pos + 4);
                    d.size[0] = F(b, scb.off_size); d.size[1] = F(b, scb.off_size + 4);
                    d.uvoff[0] = F(b, scb.off_uvoff); d.uvoff[1] = F(b, scb.off_uvoff + 4);
                    d.uvscale[0] = F(b, scb.off_uvscale); d.uvscale[1] = F(b, scb.off_uvscale + 4);
                    d.popout = F(b, scb.off_popout); d.gray = F(b, scb.off_gray);
                    d.decoded = true;
                }
            if (mapped != nullptr) dev->unmap_buffer_region(g_rb);
        }
        g_slot_frame[read_slot] = 0;
    }

    if (g_probe.active && frame >= g_probe.frame + kProbeDelay) FinishProbe();

    // Queue this frame's copies.
    const uint32_t slot = static_cast<uint32_t>(frame % kSlots);
    g_slot_meta[slot] = draws;
    g_slot_cb[slot] = cb;
    g_slot_frame[slot] = frame;
    if (g_rb.handle != 0 || (census_now && g_verbose))
    {
        command_list *cl = rt->get_command_queue()->get_immediate_command_list();
        if (g_rb.handle != 0)
            for (size_t i = 0; i < draws.size(); ++i)
            {
                const PortraitDraw &d = draws[i];
                if (d.cpu_bytes || d.range.buffer.handle == 0) continue;
                uint64_t size = 64;
                if (d.range.size != UINT64_MAX && d.range.size < size) size = d.range.size;
                cl->barrier(d.range.buffer, resource_usage::constant_buffer, resource_usage::copy_source);
                cl->copy_buffer_region(d.range.buffer, d.range.offset, g_rb, (uint64_t(slot) * kMaxPerFrame + i) * kStride, size);
                cl->barrier(d.range.buffer, resource_usage::copy_source, resource_usage::constant_buffer);
            }
        if (census_now && g_verbose && !draws.empty()) StartProbe(dev, cl, draws, frame);
    }

    // 3D character summary is logged immediately (no GPU data needed).
    if (char3d > 0)
    {
        static std::string last;
        static uint64_t last_frame = 0;
        char rts[256] = {};
        int l = 0;
        for (uint64_t k : char3d_rts)
            l += _snprintf_s(rts + l, sizeof(rts) - l, _TRUNCATE, " %ux%u", static_cast<unsigned>(k >> 32), static_cast<unsigned>(k & 0xFFFFFFFFu));
        if (last != rts || frame - last_frame >= 600)
        {
            Log("[dump] frame %llu 3D character draws=%u into targets:%s", static_cast<unsigned long long>(frame), char3d, rts);
            last = rts;
            last_frame = frame;
        }
    }
}

static void Register(bool verbose)
{
    g_verbose = verbose;
    if (verbose)
        Log("[dump] render dumper ON (render_dump=1): tagging GUI-portrait + 3D-character pipelines; Ctrl+Shift+F11 = one-frame census + portrait texture probe");
    else
        Log("[dump] portrait detection ON (portrait_mode=1)");
    reshade::register_event<reshade::addon_event::create_resource>(OnCreateResource);
    if (verbose)
    {
        reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
        reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
        reshade::register_event<reshade::addon_event::init_resource_view>(OnInitResourceView);
        reshade::register_event<reshade::addon_event::destroy_resource_view>(OnDestroyResourceView);
    }
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(OnInitPipelineLayout);
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyPipelineLayout);
    reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(OnUpdateDescriptorTables);
    reshade::register_event<reshade::addon_event::copy_descriptor_tables>(OnCopyDescriptorTables);
    reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OnBindDescriptorTables);
    reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
    reshade::register_event<reshade::addon_event::push_constants>(OnPushConstants);
    reshade::register_event<reshade::addon_event::bind_viewports>(OnBindViewports);
    reshade::register_event<reshade::addon_event::bind_scissor_rects>(OnBindScissors);
    reshade::register_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
    reshade::register_event<reshade::addon_event::draw>(OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(OnDrawIndirect);
    reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
    reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
}

static void Unregister()
{
    reshade::unregister_event<reshade::addon_event::create_resource>(OnCreateResource);
    if (g_verbose)
    {
        reshade::unregister_event<reshade::addon_event::init_resource>(OnInitResource);
        reshade::unregister_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
        reshade::unregister_event<reshade::addon_event::init_resource_view>(OnInitResourceView);
        reshade::unregister_event<reshade::addon_event::destroy_resource_view>(OnDestroyResourceView);
    }
    reshade::unregister_event<reshade::addon_event::init_pipeline_layout>(OnInitPipelineLayout);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyPipelineLayout);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
    reshade::unregister_event<reshade::addon_event::update_descriptor_tables>(OnUpdateDescriptorTables);
    reshade::unregister_event<reshade::addon_event::copy_descriptor_tables>(OnCopyDescriptorTables);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(OnBindDescriptorTables);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
    reshade::unregister_event<reshade::addon_event::push_constants>(OnPushConstants);
    reshade::unregister_event<reshade::addon_event::bind_viewports>(OnBindViewports);
    reshade::unregister_event<reshade::addon_event::bind_scissor_rects>(OnBindScissors);
    reshade::unregister_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
    reshade::unregister_event<reshade::addon_event::draw>(OnDraw);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(OnDrawIndirect);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
    reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::reshade_present>(OnPresent);
}
}  // namespace dump
