// Allocator invariants and captured-geometry replay. No game/runtime DLLs loaded.
#include "../src/feed_portrait_atlas.h"
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static void Require(bool ok, const char *message)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

static void Validate(const portrait::Atlas &atlas, uint32_t sw, uint32_t sh, uint64_t budget)
{
    Require(atlas.w >= 64 && atlas.h >= 64 && atlas.w <= 16384 && atlas.h <= 16384, "legal canvas extent");
    Require(atlas.w % 16 == 0 && atlas.h % 16 == 0, "aligned canvas");
    Require(uint64_t(atlas.w) * atlas.h <= budget, "allocated pixels obey the hard budget");
    Require(atlas.items.size() <= portrait::kMaxItems && !atlas.items.empty(), "valid slot count");
    for (size_t i = 0; i < atlas.items.size(); ++i)
    {
        const auto &a = atlas.items[i];
        Require(a.sx >= 0 && a.sy >= 0 && a.w > 0 && a.h > 0 && a.sx + a.w <= int(sw) && a.sy + a.h <= int(sh), "source crop is clipped to screen");
        Require(a.dx >= 8 && a.dy >= 8 && a.dx + a.w + 8 <= int(atlas.w) && a.dy + a.h + 8 <= int(atlas.h), "slot has an outer guard");
        const int edges = (a.sx == 0 ? 1 : 0) | (a.sy == 0 ? 2 : 0) |
            (a.sx + a.w == int(sw) ? 4 : 0) | (a.sy + a.h == int(sh) ? 8 : 0);
        Require(a.edges == edges, "screen-border feather exemption is accurate");
        for (size_t j = 0; j < i; ++j)
        {
            const auto &b = atlas.items[j];
            Require(!portrait::Overlap({a.dx, a.dy, a.w + 8, a.h + 8}, {b.dx, b.dy, b.w + 8, b.h + 8}), "slots and their guards never overlap");
        }
    }
}

static const portrait::Item *Find(const portrait::Atlas &atlas, int sx, int sy)
{
    for (const auto &item : atlas.items) if (item.sx == sx && item.sy == sy) return &item;
    return nullptr;
}

static void RegressionTests()
{
    const portrait::Limits limits = { 48, 16, 400000 };
    std::vector<portrait::Rect> rects = { {32, 32, 380, 421}, {600, 100, 80, 100}, {800, 100, 80, 100} };
    portrait::Atlas first, second;
    Require(portrait::Build(rects, 1920, 1080, limits, nullptr, first), "initial scene builds");
    Validate(first, 1920, 1080, limits.pixels);
    Require(first.items.size() == 3, "simple scene retains all portraits");
    std::reverse(rects.begin(), rects.end());
    Require(portrait::Build(rects, 1920, 1080, limits, &first, second) && portrait::SameLayout(first, second), "draw order changes preserve slots and history");
    rects.pop_back(); // remove the largest; remaining heads must not shuffle
    Require(portrait::Build(rects, 1920, 1080, limits, &first, second), "removal builds");
    Validate(second, 1920, 1080, limits.pixels);
    Require(portrait::SameHistory(first, second), "removing a portrait preserves surviving histories");
    for (const auto &item : second.items)
    { const auto *old = Find(first, item.sx, item.sy); Require(old && portrait::SameItem(*old, item), "survivor retains its exact crop and atlas address"); }
    auto previous = second;
    rects.push_back({1100, 300, 80, 100});
    Require(portrait::Build(rects, 1920, 1080, limits, &previous, second), "addition builds");
    Validate(second, 1920, 1080, limits.pixels);
    Require(!portrait::SameHistory(previous, second), "new slot requests a safe global reset");
    for (const auto &item : previous.items)
    { const auto *now = Find(second, item.sx, item.sy); Require(now && portrait::SameItem(item, *now), "addition retains existing addresses"); }

    rects = {{32, 32, 380, 421}};
    Require(portrait::Build(rects, 1920, 1080, limits, nullptr, first), "hover initial scene");
    rects[0].w -= 5; rects[0].h -= 5;
    Require(portrait::Build(rects, 1920, 1080, limits, &first, second) && portrait::SameHistory(first, second), "sticky crop absorbs hover shrink");
    rects = {{-70, -104, 380, 421}, {1910, 1060, 80, 100}, {INT_MAX, INT_MAX, INT_MAX, INT_MAX}, {0,0,-1,100}};
    Require(portrait::Build(rects, 1920, 1080, limits, nullptr, second), "clipped and malformed inputs are safe");
    Validate(second, 1920, 1080, limits.pixels);
    Require(second.items.size() == 1 && second.items[0].edges == 3, "off-screen/small portraits are omitted, screen edges retained");
    rects = {{0,0,1900,1060}};
    Require(!portrait::Build(rects, 1920, 1080, limits, nullptr, second), "oversized portrait cannot violate budget");
    rects.clear(); Require(!portrait::Build(rects, 1920, 1080, limits, &first, second), "empty scene skips evaluation");

    rects = {{32,32,80,100}, {60,60,80,100}};
    Require(portrait::Build(rects, 1920, 1080, limits, nullptr, second) && second.items.size() == 1, "overlapping widgets merge");
    rects = {{32,32,80,100}, {112,32,80,100}};
    Require(portrait::Build(rects, 1920, 1080, limits, nullptr, second) && second.items.size() == 2, "touching widget margins remain separate");

    // A compact scene grows when council heads need space, holds that allocation
    // through a transient removal, and shrinks once after the explicit delay.
    rects = {{0,0,496,456}};
    Require(portrait::Build(rects, 1920, 1080, {48,0,400000}, nullptr, first), "compact tier builds");
    const auto compact = first;
    for (int i = 0; i < 12; ++i) rects.push_back({600 + (i % 6) * 160, 100 + (i / 6) * 180, 104, 104});
    Require(portrait::Build(rects, 1920, 1080, {48,0,400000}, &first, second), "council scene grows within budget");
    Validate(second, 1920, 1080, 400000);
    Require(uint64_t(second.w) * second.h > uint64_t(compact.w) * compact.h, "growth actually buys portrait slots");
    auto grown = second; rects.resize(1);
    for (uint32_t frame = 1; frame < portrait::kShrinkFrames; ++frame)
    {
        previous = second;
        Require(portrait::Build(rects, 1920, 1080, {48,0,400000}, &previous, second), "shrink delay builds");
        Require(second.w == grown.w && second.h == grown.h, "transient removal does not rebuild atlas");
    }
    previous = second;
    Require(portrait::Build(rects, 1920, 1080, {48,0,400000}, &previous, second), "delayed shrink builds");
    Require(uint64_t(second.w) * second.h < uint64_t(grown.w) * grown.h, "sustained compact scene shrinks once");
    Require(portrait::Build(rects, 1920, 1080, {48,0,270000}, &grown, second), "runtime budget reduction builds");
    Validate(second, 1920, 1080, 270000);
    Require(!portrait::Build(rects, 1920, 1080, {48,0,50000}, &grown, second), "budget too small for the portrait skips it");

    rects = {{32,32,48,48}};
    Require(portrait::Build(rects, 1920, 1080, limits, nullptr, first), "sparse scene builds");
    Validate(first, 1920, 1080, limits.pixels);
    Require(uint64_t(first.w) * first.h <= 20000, "one small portrait does not pay for a mostly empty atlas");

    // Deliberately use a tall, full-budget atlas. More small heads could fit in
    // another aspect ratio, but their movement cannot force a feature rebuild.
    first = {}; first.w = 288; first.h = 1376; first.budget = 400000;
    first.items.push_back({0,0,8,8,256,256,3});
    for (int frame = 0; frame < 32; ++frame)
    {
        std::vector<portrait::Candidate> crops = {{{0,0,256,256},{0,0,256,256}}};
        for (int i = 0; i < 12; ++i)
        {
            const portrait::Rect crop = {400 + (i % 4) * 320 + frame, (i / 4) * 280, 168, 240};
            crops.push_back({crop,crop});
        }
        Require(portrait::Allocate(crops, 1920, 1080, 400000, &first, second), "moving head scene builds");
        Validate(second, 1920, 1080, 400000);
        Require(second.w == first.w && second.h == first.h, "small moving heads cannot reshape a full atlas");
        const auto alternate = portrait::BestPack({640,624}, crops, 1920, 1080, 400000, &first);
        Require(alternate.atlas.items.size() > second.items.size(), "fixture actually pressures aspect-ratio policy");
        first = second;
    }
    const portrait::Rect large = {500,500,600,400};
    Require(portrait::Allocate({{large,large}}, 1920, 1080, 400000, &first, second), "new large portrait admits a necessary reshape");
    Validate(second, 1920, 1080, 400000);
    Require(second.w != first.w && second.items[0].w == 600, "large portrait wins over the previous narrow allocation");
    std::puts("PASS allocator regressions: persistent slots, history, growth/shrink, sparse scenes, reshape policy, clipping, hard budget");
}

static void RandomizedTests()
{
    uint32_t random = 0xA0A5CAFE;
    auto next = [&]() { random = random * 1664525 + 1013904223; return random; };
    portrait::Atlas previous;
    for (int scene = 0; scene < 2000; ++scene)
    {
        const uint32_t sw = 480 + next() % 3361, sh = 320 + next() % 1841;
        const uint64_t budget = 50000 + next() % 950001;
        const portrait::Limits limits = {48, int(next() % 65), budget};
        std::vector<portrait::Rect> rects;
        for (uint32_t n = next() % 49; n > 0; --n)
            rects.push_back({int(next() % (sw + 500)) - 250, int(next() % (sh + 500)) - 250,
                int(24 + next() % 550), int(24 + next() % 600)});
        portrait::Atlas current;
        const bool built = portrait::Build(rects, sw, sh, limits, &previous, current);
        if (built) Validate(current, sw, sh, budget);
        else Require(current.items.empty(), "skipped frame has no paste-back rectangles");
        previous = std::move(current);
    }
    std::puts("PASS 2000 randomized scenes: screen bounds, guards, slot count, varied budgets and dimensions");
}

// TSV rows contain the real processed crop rectangles, so replay calls the same
// allocation stage after crop preparation instead of inventing missing widgets.
static void Replay(const char *path)
{
    std::ifstream stream(path); Require(bool(stream), "open recorded geometry");
    portrait::Atlas previous;
    uint64_t old_pixels = 0, new_pixels = 0, old_peak = 0, new_peak = 0, slots = 0, old_slots = 0;
    uint64_t captured_crops = 0, selected_crops = 0;
    uint32_t rows = 0, old_resizes = 0, new_resizes = 0, resets = 0, prior_w = 0, prior_h = 0;
    std::string line;
    const auto start = std::chrono::steady_clock::now();
    while (std::getline(stream, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        uint32_t w, h, count; Require(bool(fields >> w >> h >> count), "parse geometry row");
        std::vector<portrait::Candidate> candidates;
        for (uint32_t i = 0; i < count; ++i)
        {
            portrait::Rect crop;
            Require(bool(fields >> crop.x >> crop.y >> crop.w >> crop.h), "parse crop");
            candidates.push_back({crop, crop});
            captured_crops += portrait::Area(crop);
        }
        portrait::Atlas current;
        Require(portrait::Allocate(std::move(candidates), 1920, 1080, 400000, &previous, current), "captured layout allocates");
        Validate(current, 1920, 1080, 400000);
        const uint64_t old_area = uint64_t(w) * h, new_area = uint64_t(current.w) * current.h;
        old_pixels += old_area; new_pixels += new_area;
        old_peak = (std::max)(old_peak, old_area); new_peak = (std::max)(new_peak, new_area);
        if (rows && (w != prior_w || h != prior_h)) ++old_resizes;
        if (rows && (current.w != previous.w || current.h != previous.h)) ++new_resizes;
        if (rows && !portrait::SameHistory(previous, current)) ++resets;
        slots += current.items.size(); old_slots += count;
        for (const auto &item : current.items) selected_crops += uint64_t(item.w) * item.h;
        prior_w = w; prior_h = h; previous = std::move(current); ++rows;
    }
    Require(rows > 0 && new_peak <= 400000, "replayed peak obeys physical budget");
    const double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("REPLAY rows=%u peak_pixels: captured=%llu new=%llu; average_pixels: captured=%.0f new=%.0f; "
                "size_changes: captured=%u new=%u; global_resets=%u; selected_slots=%llu/%llu; "
                "selected_crop_pixels=%llu/%llu; CPU_harness=%.3f ms/update\n",
                rows, static_cast<unsigned long long>(old_peak), static_cast<unsigned long long>(new_peak),
                double(old_pixels) / rows, double(new_pixels) / rows, old_resizes, new_resizes, resets,
                static_cast<unsigned long long>(slots), static_cast<unsigned long long>(old_slots),
                static_cast<unsigned long long>(selected_crops), static_cast<unsigned long long>(captured_crops), ms / rows);
}

int main(int argc, char **argv)
{
    Require(argc <= 2, "usage: portrait-atlas.exe [captured-layouts.tsv]");
    RegressionTests(); RandomizedTests(); if (argc == 2) Replay(argv[1]);
}
