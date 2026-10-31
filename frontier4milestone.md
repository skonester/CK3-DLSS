# Frontier 4 milestone: 95% complete (release 1.0.6)

Written 2026-10-31. DLSS 5 neural rendering in CK3 now enhances the character portraits with no
visible rectangle around them, including while hovering, on an RTX 3060. About 95% of the work is
done. This file records what it took to get here and what is left. Details are in
[Frontier 1](frontier.md), [Frontier 2](frontier2.md) and [Frontier 3](frontier3.md).

## Where we are

| | Start (full frame) | Frontier 1 | Frontier 2 | Now (1.0.6) |
|---|---|---|---|---|
| What gets neural rendering | whole 1080p frame | screen crops around portraits | same, packed atlas | the portrait textures themselves |
| Frame rate (feeder summary, RTX 3060) | 15.5 fps | ~40 fps | not measured | ~42 fps steady |
| Start-up | 266 s | 20 s | 20 s | 20 s |
| Box around characters | n/a | yes | yes | **none** |
| Hover over the ruler portrait | n/a | box | box | **stays enhanced** |

Final live session (build `ck3-frontier3-tex.2`, DLSS5Extended, preset 13): in steady play
3,600 of 3,600 portrait draws per 1,800 frames used the enhanced textures; zero evaluate
failures. The frame rates are the feeder's own 600-frame summaries, not a controlled benchmark
(see the remaining work).

## What it took

1. **Make DLSS 5 playable (Frontier 1).** Full-frame NR ran CK3 at 15 fps. A render dumper found
   the GUI portrait pipeline and its `WidgetPos`/`WidgetSize` constants; NR was limited to those
   rectangles (400k-pixel budget), with zero motion vectors and fixed exposure for stability.
2. **Make the portrait pass cheap and stable (Frontier 2).** Feather blend only on edge bands, in
   place (about 70x faster blend), and a MaxRects atlas allocator that keeps slots, enforces the
   budget and avoids feature rebuilds.
3. **Find the character's real outline (Frontier 3, section 0).** A texture probe in the render
   dumper showed that each Portrait texture's alpha is a clean silhouette (edge ring 68-91% zero,
   under 2% partial), and the shipped shader source gave the exact UV rule
   (`(UV1 - PortraitUVOffset) / PortraitUVScale`).
4. **Try a screen-space mask, and learn why it can't work (cover.1-cover.3).** Blitting that alpha
   into a coverage mask removed the box on the load screen but not on the map. Portrait positions
   come from a GPU readback 3 frames late, and the animated ruler portrait is re-rendered into a new
   texture faster than that, so the mask kept dropping out. Patches on top of that design didn't
   fix it. (An earlier attempt to feed a mask into the NR network itself had already been rejected:
   it broke DLSS 5 and depends on an undocumented NVIDIA interface.)
5. **Rewrite in texture space (tex.1).** Instead of processing the screen, the feeder now:
   - copies every Portrait texture the frame samples into an atlas (cells planned by the Frontier 2
     allocator), runs NR, and restores the game's alpha with a small finish pass;
   - keeps each result in an "enhanced" texture of its own;
   - in the next frame, re-issues each GUI portrait draw with a copy of its descriptor set that
     points at the enhanced texture, then puts the game's set back.

   The game's own shader still does all compositing, so the map and UI are never touched and the
   3-frame position lag no longer matters. This removed the box.
6. **Survive the hover zoom (tex.2).** On hover the game re-renders the portrait at a new size every
   frame (455 → 473 px). Slots now reserve 5% headroom and only grow, a re-rendered portrait inherits
   its slot, and a brand-new texture borrows last frame's enhanced copy from the same draw position
   and widget. Hover no longer rebuilds DLSS or falls back to the original.
7. **Ship it (1.0.6).** The packaged feeder is the tested build (file version 1.0.6.0). New DLSS 5
   profiles get `portrait_texture=1`, saved portrait profiles from older versions gain it on
   reinstall, and the release workflow fails if a default profile lacks it.

Every step had automated checks: CPU and GPU pixel oracles for the blend, coverage blits and finish
pass (WARP and RTX 3060), allocator and planner invariants, and mutation runs proving the tests catch
a broken implementation. The parts that only exist inside CK3 (the draw re-issue, texture tracking)
were validated live from the logs.

## Releasing 1.0.6

Changed for the release: `installer-gui/build.gradle.kts` (1.0.6), `src/version.rc` (1.0.6.0),
`ck3-package/binaries/dlss-payload/dlss5-feed.addon64` (SHA-256 `c09c72b5…`),
`ck3-package/DLSS-Runtime-Setup.ps1` (default and migrated `portrait_texture=1`) and
`.github/workflows/installer-gui.yml` (package check). Commit, push, and run the Installer GUI
workflow with release tag `v1.0.6`.

## The remaining 5%

- **Controlled performance baseline.** Frontier 3 section 1 was never run: matched
  `frame_stats=1` captures (median/p95/p99, hitches) with portraits off, Frontier 2 and texture mode.
- **Load-screen portraits stay unenhanced.** Their textures are 400x1400, larger than an atlas cell
  and the 400k budget. They show the game's original image (no box). Enhancing only the visible part
  would fix it.
- **Other profiles.** Texture mode was live-tested on DLSS5Extended only. DLSS5 and
  NativeStreamline need a session each (NativeStreamline previously failed to create NR, 0xbad00001).
- **Known trade-offs.** Enhanced portraits are one frame behind the animation; a portrait's first
  frame on screen is the original. Retired descriptor sets and enhanced textures are freed after a
  fixed number of frames, not a fence.
- **Cleanup.** The screen-space coverage path (`portrait_coverage`) is superseded; remove it or keep
  it as a documented fallback. README, `architecture.md` and `frontier3.md` still describe the
  screen-space design in places.
