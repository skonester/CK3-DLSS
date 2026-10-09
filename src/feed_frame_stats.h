// feed_frame_stats.h -- Frontier 3 section 1: per-present frame timing (frame_stats=1).
//
// Every present of the game's own swapchain is counted, including frames where the feeder
// skipped evaluation, so the numbers describe the game, not just the evaluated frames. The
// DXGI bridge's hidden D3D12 runtime is filtered out: on a Vulkan game only Vulkan runtimes
// count. Times are CPU-side present-to-present intervals (QueryPerformanceCounter at
// ReShade's reshade_present event). feed_cpu_ms is the feeder's own CPU wall time inside
// the frame; GPU work is not timed here and must not be inferred from it.
// The feeder-side columns (evaluated, skip, atlas, resets) are filled by the Vulkan transport
// only; on D3D11/D3D12 games they read as not_run while the timing columns stay valid.
//
// Raw rows go to dlss5-feed-frames.csv next to the add-on (tools/frame-stats.ps1 summarises
// them); every 600 counted presents a summary line goes to dlss5-feed.log.
// Included by dlss5-feed.cpp after the Feed state, the config and feed_render_dump.h.

#pragma once

#include <algorithm>
#include <cstdio>
#include <vector>

namespace fstats
{
// Why a frame was not evaluated. NOT_RUN: the feeder was not called (technique off,
// effects off, feeder disabled); NOT_READY: session/feature/input not ready or held.
enum Skip : uint8_t { SKIP_NONE = 0, SKIP_NOT_RUN, SKIP_NO_PORTRAITS, SKIP_TOO_SMALL, SKIP_NOT_READY, SKIP_FAILED, SKIP_COUNT };
static const char *kSkipName[SKIP_COUNT] = { "", "not_run", "no_portraits", "too_small", "not_ready", "failed" };

// Reset causes: the low bits are portrait::kHistory* (atlas geometry); these are feeder-level.
enum : uint32_t
{
    kResetBuild   = 1u << 8,    // feature (re)created: NGX always starts without history
    kResetResume  = 1u << 9,    // portraits returned after skipped frames
    kResetEvery   = 1u << 10,   // reset_every=1 diagnostic
    kResetFailure = 1u << 11,   // a submission failed
};

static const char *ResetName(uint32_t bit)
{
    switch (bit)
    {
    case kResetBuild:   return "build";
    case kResetResume:  return "resume";
    case kResetEvery:   return "reset_every";
    case kResetFailure: return "failure";
    default:            return portrait::HistoryChangeName(bit);
    }
}

// Filled by FeedFrameVk on the present thread; consumed by the reshade_present event that
// follows it in the same frame (ReShade raises reshade_present after effects and overlay).
struct Note
{
    bool     ran = false;
    bool     evaluated = false;
    uint8_t  skip = SKIP_NOT_RUN;
    bool     build = false;
    int      reset = 0;
    uint32_t reset_why = 0;
    uint32_t atlas_w = 0, atlas_h = 0, blocks = 0, portraits = 0;
    int64_t  rect_age = -1;   // frames between the portrait draws and their use here
    double   cpu_ms = 0.0;
};
static Note g_note;

// RAII: marks the feeder as having run this frame and charges its CPU wall time.
struct Scope
{
    LARGE_INTEGER t0;
    Scope() { g_note.ran = true; g_note.skip = SKIP_NOT_READY; QueryPerformanceCounter(&t0); }
    ~Scope()
    {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        g_note.cpu_ms += 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
    }
};

static FILE    *g_csv = nullptr;
static bool     g_csv_created = false;
static LONGLONG g_qpf = 0, g_t_start = 0, g_t_last = 0;
static uint64_t g_presents = 0;
static bool     g_seen_vulkan = false;

struct Window
{
    std::vector<double> dt;
    uint32_t evaluated = 0, skips[SKIP_COUNT] = {}, resets = 0, builds = 0;
    uint32_t reasons[16] = {};
    double   cpu_ms = 0.0;
    void clear() { *this = Window(); }
};
static Window g_window;

static void CsvPath(char *out)
{
    strcpy_s(out, MAX_PATH, g_log_path);
    if (char *s = strrchr(out, '\\'))
        strcpy_s(s + 1, MAX_PATH - (s + 1 - out), "dlss5-feed-frames.csv");
}

static void Close()
{
    if (g_csv != nullptr) { fflush(g_csv); fclose(g_csv); g_csv = nullptr; }
    g_t_last = 0;
}

static bool Open()
{
    if (g_csv != nullptr) return true;
    char path[MAX_PATH];
    CsvPath(path);
    // First enable in this process starts a fresh file; a later re-enable appends.
    if (fopen_s(&g_csv, path, g_csv_created ? "a" : "w") != 0 || g_csv == nullptr)
    {
        static bool said = false;
        if (!said) Log("[stats] cannot open %s; frame statistics disabled for now", path);
        said = true;
        g_csv = nullptr;
        return false;
    }
    setvbuf(g_csv, nullptr, _IOFBF, 1 << 20);
    if (!g_csv_created)
    {
        fprintf(g_csv, "present,t_ms,dt_ms,api,ran,evaluated,skip,atlas_w,atlas_h,blocks,portraits,rect_age,reset,reset_why,build,feed_cpu_ms\n");
        Log("[stats] frame statistics ON (frame_stats=1): every game present -> %s", path);
    }
    else
        Log("[stats] frame statistics resumed (appending; t_ms continues, dt of the first row is blank)");
    g_csv_created = true;
    return true;
}

static double Percentile(std::vector<double> &v, double p)
{
    if (v.empty()) return 0.0;
    const size_t k = static_cast<size_t>(p * double(v.size() - 1) + 0.5);
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

static void Summarise()
{
    std::vector<double> v = g_window.dt;
    if (v.empty()) return;
    const double median = Percentile(v, 0.50), p95 = Percentile(v, 0.95), p99 = Percentile(v, 0.99);
    const double worst = *std::max_element(v.begin(), v.end());
    uint32_t hitches = 0, severe = 0;
    for (double d : g_window.dt) { if (d > 2.0 * median) ++hitches; if (d > 50.0) ++severe; }
    char reasons[256] = "";
    int len = 0;
    for (int bit = 0; bit < 16; ++bit)
        if (g_window.reasons[bit] != 0 && len >= 0 && len < static_cast<int>(sizeof(reasons)) - 32)
            len += _snprintf_s(reasons + len, sizeof(reasons) - len, _TRUNCATE, " %s=%u", ResetName(1u << bit), g_window.reasons[bit]);
    char skips[256] = "";
    len = 0;
    for (int s = 1; s < SKIP_COUNT; ++s)
        if (g_window.skips[s] != 0 && len >= 0 && len < static_cast<int>(sizeof(skips)) - 32)
            len += _snprintf_s(skips + len, sizeof(skips) - len, _TRUNCATE, " %s=%u", kSkipName[s], g_window.skips[s]);
    Log("[stats] %u presents: median %.2f ms (%.1f fps) p95 %.2f p99 %.2f max %.2f | hitches >2x median %u, >50 ms %u | "
        "evaluated %u, skipped:%s | resets %u:%s | builds %u | feed CPU %.2f ms/present (CPU wall time only)",
        static_cast<unsigned>(g_window.dt.size()), median, median > 0 ? 1000.0 / median : 0.0, p95, p99, worst, hitches, severe,
        g_window.evaluated, skips[0] ? skips : " none", g_window.resets, reasons[0] ? reasons : " none", g_window.builds,
        g_window.cpu_ms / double(g_window.dt.size()));
    g_window.clear();
}

static void OnPresent(reshade::api::effect_runtime *rt)
{
    const reshade::api::device_api api = rt->get_device()->get_api();
    if (api == reshade::api::device_api::vulkan) g_seen_vulkan = true;
    // On a Vulkan game the bridge's hidden D3D12 swapchain is not the game's frame; its
    // present must not consume the note FeedFrameVk left for the game's own present.
    if (g_seen_vulkan && api != reshade::api::device_api::vulkan) return;
    const Note note = g_note;
    g_note = Note();
    if (!g_cfg.frame_stats)
    {
        if (g_csv != nullptr) { Summarise(); Close(); Log("[stats] frame statistics OFF"); }
        return;
    }
    if (!Open()) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_qpf == 0)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpf = f.QuadPart;
        g_t_start = now.QuadPart;
    }
    const double t_ms = 1000.0 * double(now.QuadPart - g_t_start) / double(g_qpf);
    const bool has_dt = g_t_last != 0;
    const double dt_ms = has_dt ? 1000.0 * double(now.QuadPart - g_t_last) / double(g_qpf) : 0.0;
    g_t_last = now.QuadPart;
    ++g_presents;

    char why[160] = "";
    int len = 0;
    for (int bit = 0; bit < 16; ++bit)
        if ((note.reset_why & (1u << bit)) != 0 && len >= 0 && len < static_cast<int>(sizeof(why)) - 24)
            len += _snprintf_s(why + len, sizeof(why) - len, _TRUNCATE, "%s%s", len ? "|" : "", ResetName(1u << bit));
    const char *api_name = api == reshade::api::device_api::vulkan ? "vk" : api == reshade::api::device_api::d3d12 ? "d3d12" :
                           api == reshade::api::device_api::d3d11 ? "d3d11" : "other";
    if (has_dt)
        fprintf(g_csv, "%llu,%.3f,%.3f,%s,%d,%d,%s,%u,%u,%u,%u,%lld,%d,%s,%d,%.3f\n",
                static_cast<unsigned long long>(g_presents), t_ms, dt_ms, api_name, note.ran ? 1 : 0, note.evaluated ? 1 : 0,
                kSkipName[note.skip < SKIP_COUNT ? note.skip : 0], note.atlas_w, note.atlas_h, note.blocks, note.portraits,
                static_cast<long long>(note.rect_age), note.reset, why, note.build ? 1 : 0, note.cpu_ms);
    else
        fprintf(g_csv, "%llu,%.3f,,%s,%d,%d,%s,%u,%u,%u,%u,%lld,%d,%s,%d,%.3f\n",
                static_cast<unsigned long long>(g_presents), t_ms, api_name, note.ran ? 1 : 0, note.evaluated ? 1 : 0,
                kSkipName[note.skip < SKIP_COUNT ? note.skip : 0], note.atlas_w, note.atlas_h, note.blocks, note.portraits,
                static_cast<long long>(note.rect_age), note.reset, why, note.build ? 1 : 0, note.cpu_ms);

    if (has_dt) g_window.dt.push_back(dt_ms);
    if (note.evaluated) ++g_window.evaluated;
    else ++g_window.skips[note.skip < SKIP_COUNT ? note.skip : 0];
    if (note.reset) ++g_window.resets;
    for (int bit = 0; bit < 16; ++bit) if (note.reset_why & (1u << bit)) ++g_window.reasons[bit];
    if (note.build) ++g_window.builds;
    g_window.cpu_ms += note.cpu_ms;
    if (g_window.dt.size() >= 600) { Summarise(); fflush(g_csv); }
}

static void Register()   { reshade::register_event<reshade::addon_event::reshade_present>(OnPresent); }
static void Unregister() { reshade::unregister_event<reshade::addon_event::reshade_present>(OnPresent); Close(); }
}  // namespace fstats
