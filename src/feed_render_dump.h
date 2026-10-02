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

#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dump
{
using namespace reshade::api;

enum : uint8_t { TAG_NONE = 0, TAG_GUI_PORTRAIT = 1, TAG_CHAR3D = 2 };

// ---------------------------------------------------------------------------
// SPIR-V scan
// ---------------------------------------------------------------------------

struct CbInfo
{
    bool     found = false;
    bool     push_constant = false;   // the cbuffer is a push-constant block, not a descriptor
    uint32_t set = 0, binding = 0;
    uint32_t off_pos = 0, off_size = 8, off_tex = 48;
};

static bool BytesContain(const void *code, size_t size, const char *needle)
{
    const size_t n = strlen(needle);
    const char *p = static_cast<const char *>(code);
    for (size_t i = 0; i + n <= size; ++i)
        if (p[i] == needle[0] && memcmp(p + i, needle, n) == 0) return true;
    return false;
}

static const char *SpvString(const uint32_t *w, uint32_t words)
{
    const char *s = reinterpret_cast<const char *>(w);
    return strnlen(s, words * 4u) < words * 4u ? s : "";
}

// Finds the uniform/push-constant block whose struct has WidgetPos + WidgetSize members.
// Also logs every named descriptor variable once (first GUI-portrait shader only).
static CbInfo SpvFindPortraitCb(const uint32_t *w, size_t count, bool log_bindings)
{
    CbInfo out;
    if (count < 5 || w[0] != 0x07230203u) return out;

    struct Member { uint32_t index; std::string name; };
    std::unordered_map<uint32_t, std::vector<Member>> member_names;   // struct id -> names
    std::unordered_map<uint64_t, uint32_t> member_offsets;            // (struct<<32)|member -> offset
    std::unordered_map<uint32_t, std::string> names;                  // id -> OpName
    std::unordered_map<uint32_t, uint32_t> sets, bindings;            // id -> decoration value
    std::unordered_map<uint32_t, uint32_t> ptr_pointee;               // pointer type -> pointee
    struct Var { uint32_t type, id, storage; };
    std::vector<Var> vars;

    for (size_t i = 5; i < count;)
    {
        const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        if (wc == 0 || i + wc > count) break;
        const uint32_t *o = w + i;
        switch (op)
        {
        case 5:  if (wc >= 3) names[o[1]] = SpvString(o + 2, wc - 2); break;                          // OpName
        case 6:  if (wc >= 4) member_names[o[1]].push_back({ o[2], SpvString(o + 3, wc - 3) }); break; // OpMemberName
        case 71: if (wc >= 4) { if (o[2] == 34) sets[o[1]] = o[3]; else if (o[2] == 33) bindings[o[1]] = o[3]; } break; // OpDecorate
        case 72: if (wc >= 5 && o[3] == 35) member_offsets[(uint64_t(o[1]) << 32) | o[2]] = o[4]; break; // OpMemberDecorate Offset
        case 32: if (wc >= 4) ptr_pointee[o[1]] = o[3]; break;                                        // OpTypePointer
        case 59: if (wc >= 4) vars.push_back({ o[1], o[2], o[3] }); break;                            // OpVariable
        }
        i += wc;
    }

    uint32_t block = 0, pos = UINT32_MAX, size = UINT32_MAX, tex = UINT32_MAX;
    for (const auto &kv : member_names)
    {
        uint32_t p = UINT32_MAX, s = UINT32_MAX, t = UINT32_MAX;
        for (const Member &m : kv.second)
        {
            if (m.name == "WidgetPos") p = m.index;
            else if (m.name == "WidgetSize") s = m.index;
            else if (m.name == "PortraitTextureSize") t = m.index;
        }
        if (p != UINT32_MAX && s != UINT32_MAX) { block = kv.first; pos = p; size = s; tex = t; break; }
    }

    if (log_bindings)
        for (const Var &v : vars)
            if (sets.count(v.id) || bindings.count(v.id))
            {
                const auto nm = names.find(v.id);
                Log("[dump]   binding set=%u binding=%u storage=%u %s", sets.count(v.id) ? sets[v.id] : 0,
                    bindings.count(v.id) ? bindings[v.id] : 0, v.storage, nm != names.end() ? nm->second.c_str() : "?");
            }

    if (block == 0) return out;
    for (const Var &v : vars)
    {
        const auto pp = ptr_pointee.find(v.type);
        if (pp == ptr_pointee.end() || pp->second != block) continue;
        out.found = true;
        out.push_constant = v.storage == 9;   // StorageClass PushConstant
        out.set = sets.count(v.id) ? sets[v.id] : 0;
        out.binding = bindings.count(v.id) ? bindings[v.id] : 0;
        auto off = [&](uint32_t m, uint32_t def) {
            const auto it = member_offsets.find((uint64_t(block) << 32) | m);
            return m != UINT32_MAX && it != member_offsets.end() ? it->second : def;
        };
        out.off_pos  = off(pos, 0);
        out.off_size = off(size, 8);
        out.off_tex  = off(tex, 48);
        break;
    }
    return out;
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

static uint64_t DescKey(uint64_t table, uint32_t binding) { return table * 0x9E3779B97F4A7C15ull ^ binding; }

static constexpr int kMaxSets = 8, kMaxDyn = 16, kMaxPush = 4;

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

// Copy of the latest portrait rectangles; returns how many decodes in a row they have been stable.
static uint32_t GetPortraits(std::vector<PRect> &out)
{
    std::lock_guard<std::mutex> lock(g_pub_mx);
    out = g_pub;
    return g_pub_stable;
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

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static bool OnCreateResource(device *dev, resource_desc &desc, subresource_data *, resource_usage)
{
    // Uniform buffers need TRANSFER_SRC so the present-time readback copy is legal (game's Vulkan device only).
    if (dev->get_api() == device_api::vulkan && desc.type == resource_type::buffer && (desc.usage & resource_usage::constant_buffer) != 0 &&
        (desc.usage & resource_usage::copy_source) == 0)
    {
        desc.usage |= resource_usage::copy_source;
        return true;
    }
    return false;
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

static void OnInitPipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subs, pipeline pipe)
{
    ++g_pipes_seen;
    uint8_t tag = TAG_NONE;
    CbInfo cb;
    bool any_code = false;
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
                tag = TAG_GUI_PORTRAIT;
                if (!cb.found)
                {
                    const bool first = g_pipes_tagged.load() == 0;
                    if (first) Log("[dump] first GUI-portrait shader bindings:");
                    cb = SpvFindPortraitCb(static_cast<const uint32_t *>(sd[j].code), sd[j].code_size / 4, first);
                }
            }
            else if (tag == TAG_NONE && BytesContain(sd[j].code, sd[j].code_size, "PatternColorMasks_Texture"))
                tag = TAG_CHAR3D;
        }
    }
    if (!any_code) ++g_pipes_no_code;
    if (tag == TAG_NONE) return;

    const uint32_t n = ++g_pipes_tagged;
    if (tag == TAG_GUI_PORTRAIT && n <= 8)
        Log("[dump] GUI-portrait pipeline %016llX: cbuffer %s set=%u binding=%u offsets pos=%u size=%u tex=%u",
            static_cast<unsigned long long>(pipe.handle), cb.found ? (cb.push_constant ? "push-constant" : "found") : "NOT FOUND",
            cb.set, cb.binding, cb.off_pos, cb.off_size, cb.off_tex);
    std::unique_lock<std::shared_mutex> lock(g_pipe_mx);
    g_pipes[pipe.handle] = { tag, cb };
}

static void OnDestroyPipeline(device *, pipeline pipe)
{
    std::unique_lock<std::shared_mutex> lock(g_pipe_mx);
    g_pipes.erase(pipe.handle);
}

static bool OnUpdateDescriptorTables(device *, uint32_t count, const descriptor_table_update *updates)
{
    std::lock_guard<std::mutex> lock(g_desc_mx);
    for (uint32_t i = 0; i < count; ++i)
    {
        const descriptor_table_update &u = updates[i];
        if (u.type != descriptor_type::constant_buffer && u.type != descriptor_type::constant_buffer_with_dynamic_offset)
            continue;
        const buffer_range *r = static_cast<const buffer_range *>(u.descriptors);
        for (uint32_t k = 0; r != nullptr && k < u.count; ++k)
            g_desc[DescKey(u.table.handle, u.binding + u.array_offset + k)] = { r[k], u.type };
    }
    if (g_desc.size() > (1u << 20)) g_desc.clear();   // handles are recycled; never grow unbounded
    return false;
}

static bool OnCopyDescriptorTables(device *, uint32_t count, const descriptor_table_copy *copies)
{
    std::lock_guard<std::mutex> lock(g_desc_mx);
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t k = 0; k < copies[i].count; ++k)
        {
            const auto it = g_desc.find(DescKey(copies[i].source_table.handle, copies[i].source_binding + copies[i].source_array_offset + k));
            if (it != g_desc.end())
                g_desc[DescKey(copies[i].dest_table.handle, copies[i].dest_binding + copies[i].dest_array_offset + k)] = it->second;
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
    if (u.type != descriptor_type::constant_buffer && u.type != descriptor_type::constant_buffer_with_dynamic_offset) return;
    CmdState *s = State(cl);
    const buffer_range *r = static_cast<const buffer_range *>(u.descriptors);
    for (uint32_t k = 0; r != nullptr && k < u.count; ++k)
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
}

static float F(const uint8_t *b, uint32_t off) { float v = 0; if (off + 4 <= 64) memcpy(&v, b + off, 4); return v; }

static void Publish(const uint8_t *base, const std::vector<PortraitDraw> &meta, const CbInfo &cb)
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
}

static void DecodeAndLog(const uint8_t *base, const std::vector<PortraitDraw> &meta, const CbInfo &cb, uint64_t frame)
{
    char line[1900];
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "n=%u", static_cast<unsigned>(meta.size()));
    for (size_t i = 0; i < meta.size() && len > 0 && len < static_cast<int>(sizeof(line)) - 120; ++i)
    {
        const uint8_t *b = meta[i].cpu_bytes ? meta[i].bytes : base + i * kStride;
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " | %.0f,%.0f %.0fx%.0f tex %.0fx%.0f",
                           F(b, cb.off_pos), F(b, cb.off_pos + 4), F(b, cb.off_size), F(b, cb.off_size + 4),
                           F(b, cb.off_tex), F(b, cb.off_tex + 4));
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
    CbInfo cb;
    {
        std::lock_guard<std::mutex> lock(g_frame_mx);
        draws.swap(g_frame_draws);
        char3d_rts.swap(g_frame_char3d_rts);
        char3d = g_frame_char3d; g_frame_char3d = 0;
        cb = g_last_cb;
        if (g_census_frame.load() == frame - 1) { census.swap(g_census); g_census_frame = UINT64_MAX; }
    }

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
        Log("[dump] ===== end census (pipelines seen=%u tagged=%u without SPIR-V=%u) =====",
            g_pipes_seen.load(), g_pipes_tagged.load(), g_pipes_no_code.load());
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
            Publish(static_cast<const uint8_t *>(mapped), meta, g_slot_cb[read_slot]);
            if (g_verbose) DecodeAndLog(static_cast<const uint8_t *>(mapped), meta, g_slot_cb[read_slot], g_slot_frame[read_slot]);
            if (mapped != nullptr) dev->unmap_buffer_region(g_rb);
        }
        g_slot_frame[read_slot] = 0;
    }

    // Queue this frame's copies.
    const uint32_t slot = static_cast<uint32_t>(frame % kSlots);
    g_slot_meta[slot] = draws;
    g_slot_cb[slot] = cb;
    g_slot_frame[slot] = frame;
    if (g_rb.handle != 0)
    {
        command_list *cl = rt->get_command_queue()->get_immediate_command_list();
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
        Log("[dump] render dumper ON (render_dump=1): tagging GUI-portrait + 3D-character pipelines; Ctrl+Shift+F11 = one-frame census");
    else
        Log("[dump] portrait detection ON (portrait_mode=1)");
    reshade::register_event<reshade::addon_event::create_resource>(OnCreateResource);
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
