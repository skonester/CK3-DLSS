// Frontier 2 atlas allocator. Aquarius charges every allocated pixel to the budget;
// Sagittarius keeps surviving portraits in their seats instead of shuffling history.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace portrait
{
static constexpr int kMaxItems = 24, kPad = 8, kMaxDimension = 16384;
static constexpr uint32_t kShrinkFrames = 180;
struct Rect { int x, y, w, h; };
struct Item { int sx, sy, dx, dy, w, h, edges; };
struct Atlas
{
    uint32_t w = 0, h = 0, compact_frames = 0;
    uint64_t budget = 0;
    std::vector<Item> items;
};
struct Limits { int min_size, feather; uint64_t pixels; };
struct Candidate { Rect widget, crop; };
struct Canvas { int w = 0, h = 0; };
struct Plan
{
    Atlas atlas;
    std::vector<bool> accepted;
    uint32_t retained = 0;
    size_t priority_count = 1;
    uint64_t selected_pixels = 0;
};

static inline int64_t Area(const Rect &r) { return int64_t(r.w) * r.h; }
static inline uint64_t Area(Canvas c) { return uint64_t(c.w) * c.h; }
static inline bool Overlap(const Rect &a, const Rect &b)
{ return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
static inline bool Contains(const Rect &outer, const Rect &inner)
{ return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h; }
static inline bool Equal(const Rect &a, const Rect &b)
{ return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
static inline Rect Unite(const Rect &a, const Rect &b)
{
    const int x = (std::min)(a.x, b.x), y = (std::min)(a.y, b.y);
    return { x, y, (std::max)(a.x + a.w, b.x + b.w) - x, (std::max)(a.y + a.h, b.y + b.h) - y };
}
static inline int Up16(int v) { return (v + 15) & ~15; }

// Pixel ceilings apply to the complete canvas, including guards, alignment and holes.
static inline Canvas ChooseCanvas(uint64_t pixels, int need_w, int need_h)
{
    need_w = (std::max)(64, Up16(need_w)); need_h = (std::max)(64, Up16(need_h));
    if (need_w > kMaxDimension || need_h > kMaxDimension || uint64_t(need_w) * need_h > pixels) return {};
    int w = (std::max)(need_w, (int(std::sqrt(double(pixels))) + 63) / 64 * 64);
    w = (std::min)(w, kMaxDimension);
    int h = int((pixels / w) / 16 * 16);
    h = (std::min)(h, kMaxDimension);
    if (h < need_h)
    {
        h = need_h;
        w = (std::min)(kMaxDimension, int((pixels / h) / 16 * 16));
    }
    return w >= need_w && h >= need_h ? Canvas{ w, h } : Canvas{};
}

// MaxRects splitting retains every usable sub-rectangle; containment pruning keeps
// this small (at most 24 occupied slots). Reservations include the inter-slot guard.
static inline void Reserve(std::vector<Rect> &free, const Rect &used)
{
    std::vector<Rect> next;
    next.reserve(free.size() * 2 + 4);
    for (const Rect &r : free)
    {
        if (!Overlap(r, used)) { next.push_back(r); continue; }
        if (used.x > r.x) next.push_back({ r.x, r.y, used.x - r.x, r.h });
        if (used.x + used.w < r.x + r.w) next.push_back({ used.x + used.w, r.y, r.x + r.w - used.x - used.w, r.h });
        if (used.y > r.y) next.push_back({ r.x, r.y, r.w, used.y - r.y });
        if (used.y + used.h < r.y + r.h) next.push_back({ r.x, used.y + used.h, r.w, r.y + r.h - used.y - used.h });
    }
    for (size_t i = 0; i < next.size();)
    {
        bool redundant = false;
        for (size_t j = 0; j < next.size(); ++j)
            if (i != j && Contains(next[j], next[i]) && (!Equal(next[j], next[i]) || j < i)) { redundant = true; break; }
        if (redundant) next.erase(next.begin() + i);
        else ++i;
    }
    free.swap(next);
}

static inline bool Place(const std::vector<Rect> &free, int w, int h, Rect &out)
{
    bool found = false;
    int best_short = 0, best_long = 0;
    for (const Rect &r : free)
    {
        if (w > r.w || h > r.h) continue;
        const int a = r.w - w, b = r.h - h;
        const int short_side = (std::min)(a, b), long_side = (std::max)(a, b);
        if (!found || short_side < best_short || (short_side == best_short &&
            (long_side < best_long || (long_side == best_long && (r.y < out.y || (r.y == out.y && r.x < out.x))))))
        {
            out = { r.x, r.y, w, h }; best_short = short_side; best_long = long_side; found = true;
        }
    }
    return found;
}

static inline bool SameItem(const Item &a, const Item &b)
{
    return a.sx == b.sx && a.sy == b.sy && a.dx == b.dx && a.dy == b.dy && a.w == b.w && a.h == b.h && a.edges == b.edges;
}
static inline bool SameLayout(const Atlas &a, const Atlas &b)
{
    if (a.w != b.w || a.h != b.h || a.items.size() != b.items.size()) return false;
    for (size_t i = 0; i < a.items.size(); ++i) if (!SameItem(a.items[i], b.items[i])) return false;
    return true;
}
// Removing a slot cannot invalidate the pixels of a surviving slot. New or changed
// crops still reset the feature: NGX/NR has no per-slot history-reset contract here.
static inline bool SameHistory(const Atlas &previous, const Atlas &next)
{
    if (previous.w != next.w || previous.h != next.h) return false;
    for (const Item &item : next.items)
        if (std::none_of(previous.items.begin(), previous.items.end(), [&](const Item &old) { return SameItem(item, old); })) return false;
    return true;
}

static inline Plan Pack(Canvas canvas, const std::vector<Candidate> &candidates,
                        uint32_t sw, uint32_t sh, uint64_t pixels, const Atlas *previous)
{
    Plan plan;
    plan.atlas.w = canvas.w; plan.atlas.h = canvas.h;
    plan.accepted.resize(candidates.size());
    plan.priority_count = (std::max)(size_t(1), size_t(std::count_if(candidates.begin(), candidates.end(),
        [&](const Candidate &c) { return uint64_t(Area(c.crop)) * 4 >= pixels; })));
    if (canvas.w == 0 || canvas.h == 0) return plan;
    std::vector<Rect> free = { { kPad, kPad, canvas.w - kPad, canvas.h - kPad } };
    auto append = [&](size_t index, const Rect &slot) {
        const Rect &q = candidates[index].crop;
        const int edges = (q.x == 0 ? 1 : 0) | (q.y == 0 ? 2 : 0) |
            (q.x + q.w == int(sw) ? 4 : 0) | (q.y + q.h == int(sh) ? 8 : 0);
        plan.atlas.items.push_back({ q.x, q.y, slot.x, slot.y, q.w, q.h, edges });
        plan.accepted[index] = true;
        plan.selected_pixels += Area(q);
        Reserve(free, slot);
    };
    // Reserve surviving slots before a new portrait can steal their history address.
    if (previous != nullptr)
        for (const Item &old : previous->items)
        {
            if (plan.atlas.items.size() == kMaxItems) break;
            const Rect slot = { old.dx, old.dy, old.w + kPad, old.h + kPad };
            if (slot.x < kPad || slot.y < kPad || slot.w <= 0 || slot.h <= 0 ||
                slot.x > canvas.w - slot.w || slot.y > canvas.h - slot.h) continue;
            if (std::any_of(plan.atlas.items.begin(), plan.atlas.items.end(), [&](const Item &it) {
                    return Overlap(slot, { it.dx, it.dy, it.w + kPad, it.h + kPad }); })) continue;
            for (size_t i = 0; i < candidates.size(); ++i)
                if (!plan.accepted[i] && Equal(candidates[i].crop, { old.sx, old.sy, old.w, old.h }))
                {
                    append(i, slot); ++plan.retained; break;
                }
        }
    for (size_t i = 0; i < candidates.size() && plan.atlas.items.size() < kMaxItems; ++i)
    {
        if (plan.accepted[i]) continue;
        Rect slot;
        if (Place(free, candidates[i].crop.w + kPad, candidates[i].crop.h + kPad, slot)) append(i, slot);
    }
    return plan;
}

// Large portraits win by descending crop area; the remainder wins by covered
// pixels and count. Then preserve slots and prefer fewer allocated pixels.
static inline bool BetterCoverage(const Plan &a, const Plan &b)
{
    for (size_t i = 0; i < (std::min)(a.accepted.size(), a.priority_count); ++i)
        if (a.accepted[i] != b.accepted[i]) return a.accepted[i];
    if (a.selected_pixels != b.selected_pixels) return a.selected_pixels > b.selected_pixels;
    return a.atlas.items.size() > b.atlas.items.size();
}
static inline bool BetterPlan(const Plan &a, const Plan &b)
{
    if (BetterCoverage(a, b)) return true;
    if (BetterCoverage(b, a)) return false;
    if (a.retained != b.retained) return a.retained > b.retained;
    return uint64_t(a.atlas.w) * a.atlas.h < uint64_t(b.atlas.w) * b.atlas.h;
}
static inline Plan BestPack(Canvas canvas, const std::vector<Candidate> &candidates,
                            uint32_t sw, uint32_t sh, uint64_t pixels, const Atlas *previous)
{
    Plan stable = Pack(canvas, candidates, sw, sh, pixels, previous);
    if (previous != nullptr)
    {
        Plan fresh = Pack(canvas, candidates, sw, sh, pixels, nullptr);
        if (BetterCoverage(fresh, stable)) return fresh;
    }
    return stable;
}

static inline bool Allocate(std::vector<Candidate> candidates, uint32_t sw, uint32_t sh,
                            uint64_t pixels, const Atlas *previous, Atlas &out)
{
    out = {};
    pixels = (std::min)(pixels, uint64_t(kMaxDimension) * kMaxDimension);
    if (sw == 0 || sh == 0 || sw > kMaxDimension || sh > kMaxDimension || pixels < 4096) return false;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate &c) {
        return c.crop.x < 0 || c.crop.y < 0 || c.crop.w <= 0 || c.crop.h <= 0 ||
            int64_t(c.crop.x) + c.crop.w > sw || int64_t(c.crop.y) + c.crop.h > sh;
    }), candidates.end());
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate &c) {
        const int w = Up16(c.crop.w + 2 * kPad), h = Up16(c.crop.h + 2 * kPad);
        return w > kMaxDimension || h > kMaxDimension || uint64_t(w) * h > pixels;
    }), candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (Area(a.crop) != Area(b.crop)) return Area(a.crop) > Area(b.crop);
        if (a.crop.y != b.crop.y) return a.crop.y < b.crop.y;
        if (a.crop.x != b.crop.x) return a.crop.x < b.crop.x;
        if (a.crop.h != b.crop.h) return a.crop.h > b.crop.h;
        return a.crop.w > b.crop.w;
    });
    if (candidates.empty()) return false;
    const int need_w = candidates.front().crop.w + 2 * kPad, need_h = candidates.front().crop.h + 2 * kPad;
    uint64_t demand = 0;
    for (size_t i = 0; i < (std::min)(candidates.size(), size_t(kMaxItems)); ++i)
        demand += uint64_t(Up16(candidates[i].crop.w + 2 * kPad)) * Up16(candidates[i].crop.h + 2 * kPad);
    Canvas compact = ChooseCanvas((std::min)(pixels * 3 / 4, demand * 5 / 4), need_w, need_h);
    if (compact.w == 0) compact = ChooseCanvas(pixels, need_w, need_h);
    const bool valid_previous = previous != nullptr && previous->w >= 64 && previous->h >= 64 &&
        previous->w <= kMaxDimension && previous->h <= kMaxDimension && previous->w % 16 == 0 && previous->h % 16 == 0 &&
        uint64_t(previous->w) * previous->h <= pixels;
    const Canvas current = valid_previous ? Canvas{ int(previous->w), int(previous->h) } : compact;
    Plan chosen = BestPack(current, candidates, sw, sh, pixels, previous);
    const std::vector<bool> current_coverage = chosen.accepted;
    Plan compact_plan = BestPack(compact, candidates, sw, sh, pixels, previous);
    const auto same_canvas = [&](const Plan &p) { return p.atlas.w == uint32_t(current.w) && p.atlas.h == uint32_t(current.h); };

    auto consider = [&](Canvas canvas) {
        if (canvas.w == 0 || canvas.h == 0 || Area(canvas) > pixels) return;
        Plan next = BestPack(canvas, candidates, sw, sh, pixels, previous);
        if (valid_previous && (canvas.w < current.w || canvas.h < current.h))
        {
            // Reshaping a full canvas costs a feature rebuild. A tiny map head
            // cannot demand that toll. Only admitting the largest missing crop
            // or a portrait occupying >= one quarter of the budget earns it.
            size_t gain = 0;
            while (gain < next.accepted.size() && next.accepted[gain] == current_coverage[gain]) ++gain;
            if (gain == next.accepted.size() || !next.accepted[gain] ||
                (gain != 0 && uint64_t(Area(candidates[gain].crop)) * 4 < pixels)) return;
        }
        // Growth must buy portrait coverage, not just a larger mostly empty texture.
        if (BetterCoverage(next, chosen) || (!same_canvas(chosen) && !BetterCoverage(chosen, next) && BetterPlan(next, chosen)))
            chosen = std::move(next);
    };
    if (valid_previous)
    {
        // Expand a single axis first so all current addresses can survive growth.
        const int wide = (std::min)(kMaxDimension, int((pixels / previous->h) / 16 * 16));
        const int tall = (std::min)(kMaxDimension, int((pixels / previous->w) / 16 * 16));
        if (wide >= int(previous->w)) consider({ wide, int(previous->h) });
        if (tall >= int(previous->h)) consider({ int(previous->w), tall });
    }
    consider(ChooseCanvas(pixels, need_w, need_h));
    if (std::any_of(chosen.accepted.begin(), chosen.accepted.end(), [](bool placed) { return !placed; }))
    {
        // Nearby aspect ratios can fit two large portraits where a square cannot.
        // Probe only on placement pressure; an unchanged fitting scene stays cheap.
        const int begin_w = (need_w + 63) / 64 * 64;
        const int end_w = (std::min)(kMaxDimension, (std::max)(begin_w, int(std::sqrt(double(pixels)) * 2) / 64 * 64));
        for (int w = begin_w; w <= end_w; w += 64)
        {
            const int h = (std::min)(kMaxDimension, int((pixels / w) / 16 * 16));
            if (h >= need_h) consider({ w, h });
        }
    }
    if (chosen.atlas.items.empty()) return false;
    if (valid_previous && chosen.atlas.w == previous->w && chosen.atlas.h == previous->h &&
        Area(compact) < uint64_t(chosen.atlas.w) * chosen.atlas.h && !BetterCoverage(chosen, compact_plan))
    {
        const uint32_t age = (std::min)(previous->compact_frames, kShrinkFrames - 1) + 1;
        if (age == kShrinkFrames) chosen = std::move(compact_plan);
        else chosen.atlas.compact_frames = age;
    }
    chosen.atlas.budget = pixels;
    out = std::move(chosen.atlas);
    return true;
}

template<class InputRect>
static bool Build(const std::vector<InputRect> &input, uint32_t sw, uint32_t sh,
                  Limits limits, const Atlas *previous, Atlas &out)
{
    out = {};
    if (sw == 0 || sh == 0 || sw > kMaxDimension || sh > kMaxDimension) return false;
    limits.min_size = (std::max)(1, limits.min_size);
    limits.feather = (std::clamp)(limits.feather, 0, 64);
    limits.pixels = (std::min)(limits.pixels, uint64_t(kMaxDimension) * kMaxDimension);
    if (limits.pixels < 4096) return false;
    std::vector<Candidate> candidates;
    for (const InputRect &p : input)
    {
        if (p.w <= 0 || p.h <= 0) continue;
        const int x0 = int((std::clamp)(int64_t(p.x), int64_t(0), int64_t(sw)));
        const int y0 = int((std::clamp)(int64_t(p.y), int64_t(0), int64_t(sh)));
        const int x1 = int((std::clamp)(int64_t(p.x) + p.w, int64_t(0), int64_t(sw)));
        const int y1 = int((std::clamp)(int64_t(p.y) + p.h, int64_t(0), int64_t(sh)));
        if (x1 - x0 < limits.min_size || y1 - y0 < limits.min_size) continue;
        const Rect widget = { x0, y0, x1 - x0, y1 - y0 };
        const int cx = (std::max)(0, (x0 - limits.feather) & ~15);
        const int cy = (std::max)(0, (y0 - limits.feather) & ~15);
        Rect crop = { cx, cy, (std::min)(int(sw), Up16(x1 + limits.feather)) - cx,
                             (std::min)(int(sh), Up16(y1 + limits.feather)) - cy };
        if (previous != nullptr)
            for (const Item &old : previous->items)
            {
                const Rect old_crop = { old.sx, old.sy, old.w, old.h };
                if (old_crop.x >= 0 && old_crop.y >= 0 && old_crop.w > 0 && old_crop.h > 0 &&
                    old_crop.x + old_crop.w <= int(sw) && old_crop.y + old_crop.h <= int(sh) &&
                    Contains(old_crop, widget) && Area(old_crop) * 2 <= Area(crop) * 3)
                { crop = old_crop; break; }
            }
        candidates.push_back({ widget, crop });
    }
    // Merge real widget overlap; margins alone must never join an entire council row.
    for (bool merged = true; merged;)
    {
        merged = false;
        for (size_t i = 0; i < candidates.size() && !merged; ++i)
            for (size_t j = i + 1; j < candidates.size() && !merged; ++j)
                if (Overlap(candidates[i].widget, candidates[j].widget) || Equal(candidates[i].crop, candidates[j].crop))
                {
                    candidates[i] = { Unite(candidates[i].widget, candidates[j].widget), Unite(candidates[i].crop, candidates[j].crop) };
                    candidates.erase(candidates.begin() + j); merged = true;
                }
    }
    return Allocate(std::move(candidates), sw, sh, limits.pixels, previous, out);
}

} // namespace portrait
