// Frontier 3 (section 2): plan the character coverage atlas. Each GUI portrait widget samples a
// Portrait texture whose alpha is the character silhouette (2026-10-31 probe). Blitting that alpha
// into the widget's screen rectangle, translated into the atlas item, gives the blend pass its
// coverage. Game-independent so tests/portrait-cover.cpp can check it.
//
// The portrait shaders sample at uv = (t - PortraitUVOffset) / PortraitUVScale, t being the 0..1
// position across the widget, with clamped addressing. The blit covers the part of the widget
// where uv lies inside the texture; outside it the coverage stays zero (original pixels).
//
// An item gets coverage only when it is exact: every widget that reaches into the item has a
// source, and no two of them overlap inside it (a blit overwrites, it cannot take the union).
// Otherwise the item keeps the legacy edge feather; it never gets another portrait's mask.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "feed_portrait_atlas.h"

namespace pcover
{
struct Widget { int x, y, w, h; };
// tex_w == 0 marks a source that is known but unusable this frame (stale or unsupported texture).
struct Source { Widget widget; float off_x, off_y, scale_x, scale_y; uint32_t tex_w, tex_h; };
// Blit of one source: texel offsets (reversed on a mirrored axis) to atlas pixels.
struct Blit { size_t source; int sx0, sy0, sx1, sy1; int dx0, dy0, dx1, dy1; };

enum Result { kPlanned = 0, kNoWidget, kOverlap, kNoSource, kUnusable, kResultCount };
static const char *const kResultName[kResultCount] = { "planned", "no_widget", "overlap", "no_source", "unusable" };

static inline bool SameWidget(const Widget &a, const Widget &b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

// One axis of the widget -> texture mapping.
struct Axis
{
    int origin, extent; float off, scale; uint32_t texels;
    double Texel(double screen) const { return ((screen - origin) / extent - off) / scale * texels; }
    // Screen interval [lo, hi) whose texel coordinate lies inside the texture, clipped to [v0, v1).
    bool Valid(int v0, int v1, int &lo, int &hi) const
    {
        const double a = origin + double(extent) * off, b = origin + double(extent) * (off + scale);
        lo = (std::max)(v0, int(std::ceil((std::min)(a, b) - 1e-6)));
        hi = (std::min)(v1, int(std::floor((std::max)(a, b) + 1e-6)));
        return hi > lo;
    }
    // Rounded texel offsets for [lo, hi); a sliver narrower than one texel samples the texel under its centre.
    void Offsets(int lo, int hi, int &s0, int &s1) const
    {
        auto at = [&](double screen) {
            const double t = std::floor(Texel(screen) + 0.5);
            return int((std::clamp)(t, 0.0, double(texels)));
        };
        s0 = at(lo); s1 = at(hi);
        if (s0 != s1) return;
        const int texel = int((std::clamp)(std::floor(Texel((lo + hi) * 0.5)), 0.0, double(texels) - 1));
        if (scale > 0) { s0 = texel; s1 = texel + 1; }
        else           { s0 = texel + 1; s1 = texel; }
    }
};

// widgets: every rectangle the atlas was built from. sources: those with a known texture.
static inline Result PlanItem(const portrait::Item &item, const std::vector<Widget> &widgets, const std::vector<Source> &sources,
                              uint32_t screen_w, uint32_t screen_h, std::vector<Blit> &out)
{
    out.clear();
    const int cx0 = (std::max)(item.sx, 0), cy0 = (std::max)(item.sy, 0);
    const int cx1 = (std::min)(item.sx + item.w, int(screen_w)), cy1 = (std::min)(item.sy + item.h, int(screen_h));
    if (cx1 <= cx0 || cy1 <= cy0) return kNoWidget;
    struct Visible { int x0, y0, x1, y1; };
    std::vector<Visible> seen;
    for (const Widget &w : widgets)
    {
        if (w.w <= 0 || w.h <= 0) continue;
        const Visible v = { (std::max)(w.x, cx0), (std::max)(w.y, cy0), (std::min)(w.x + w.w, cx1), (std::min)(w.y + w.h, cy1) };
        if (v.x1 <= v.x0 || v.y1 <= v.y0) continue;
        for (const Visible &o : seen)
            if (v.x0 < o.x1 && o.x0 < v.x1 && v.y0 < o.y1 && o.y0 < v.y1) { out.clear(); return kOverlap; }
        seen.push_back(v);
        size_t s = 0;
        while (s < sources.size() && !SameWidget(sources[s].widget, w)) ++s;
        if (s == sources.size()) { out.clear(); return kNoSource; }
        const Source &src = sources[s];
        if (src.tex_w == 0 || src.tex_h == 0) { out.clear(); return kUnusable; }
        const Axis ax = { w.x, w.w, src.off_x, src.scale_x, src.tex_w }, ay = { w.y, w.h, src.off_y, src.scale_y, src.tex_h };
        int x0, x1, y0, y1;
        if (!ax.Valid(v.x0, v.x1, x0, x1) || !ay.Valid(v.y0, v.y1, y0, y1)) continue;   // texture not visible here: zero
        Blit b = { s, 0, 0, 0, 0, item.dx + x0 - item.sx, item.dy + y0 - item.sy, item.dx + x1 - item.sx, item.dy + y1 - item.sy };
        ax.Offsets(x0, x1, b.sx0, b.sx1);
        ay.Offsets(y0, y1, b.sy0, b.sy1);
        out.push_back(b);
    }
    if (seen.empty()) return kNoWidget;   // nothing known about this item: no mask, legacy path
    return kPlanned;
}
}  // namespace pcover
