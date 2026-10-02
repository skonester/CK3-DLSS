# Frontier 1: DLSS 5 on an RTX 3060, portraits only

Log of the 2026-10-02 work session that took the DLSS 5 Neural Rendering profile on an
**RTX 3060 12 GB** from an unplayable **~15 fps with a 4.5-minute start-up** to **~40 fps with a
~20-second start-up**. We did it by applying neural rendering only to the character portraits
instead of the whole 1080p frame.

| | Start of session | End of Frontier 1 |
|---|---|---|
| Frame rate with DLSS 5 active | 15.5 fps | ~40 fps (40.8 fps measured, steady state) |
| Feeder cost per frame | 45–63 ms | ~13.5 ms |
| CK3 start-up ("Total startup duration") | 266.5 s | 19.6 s |
| Pixels sent through neural rendering | 2.07 M (full 1920×1080 frame) | ≤ 400 k (portraits only, capped) |
| Neural rendering on the map / loading screens | always on | off |

---

## Test setup

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 3060 12 GB |
| Resolution | 1920×1080, CK3 on the Vulkan renderer |
| Profile | DLSS 5 Extended (DLSS 310.8.0 + ShortFuse NR 310.8.SF-v2) |
| Add-ons | RenoDX DLSS 5 add-on (pinned 4.55 archive), ReShade 6.8.0, VORT motion vectors |
| DLSS preset | 13 (Model M), DLAA |
| Game | CK3 with a playset that includes script mods (unrelated script errors in `error.log`) |

All frame rates come from the feeder's own 600-frame summaries in
`binaries\dlss-active\dlss5-feed.log` (`feed CPU … ms/frame | frame interval … ms (… fps)`).
"Feed CPU" includes the time spent waiting on the GPU for the DLSS and neural-rendering work.
Start-up times come from CK3's `Documents\Paradox Interactive\Crusader Kings III\logs\debug.log`.

---

## Starting point

The current tree was built and installed with `Build-CK3-Complete-Test.ps1` (current-tree feeder
and layer, deployed to the game folder). On the DLSS 5 profile, the main menu took about
4.5 minutes and seemed stuck on "Initialize game".

What the logs showed (06:05–06:10):

- Everything loaded correctly. The D3D12 session opened, NGX initialized, and RenoDX reported
  `inline feature 18 evaluation succeeded` every frame, so DLSS 5 NR was running.
- **The feeder cost 45 ms per frame, 71% of the frame, holding everything at 15.5 fps.** That
  includes the menus and the loading screen.
- `debug.log`: `Total startup duration: 266.5 seconds`. Most of it was a single 154-second silent
  stretch after database loading (map and shader preparation), with the GPU saturated by NR.
- RenoDX runs NR at **whatever size our DLSS feature is** (`NR input 1920x1080 (guides 1920x1080)`),
  and the feeder always created that feature at the full backbuffer size.

Lowering the game resolution and graphics settings had not helped in earlier testing. CK3 keeps the
swapchain at the display resolution, so NR still processed 1920×1080 regardless.

**Conclusion:** the cost was neural rendering over every pixel of the frame. Shrinking what goes
through NR was the only real lever.

---

## Step 1: Evaluating ck3_accelerator

ck3_accelerator (a GPLv3 CK3 engine-hooking framework, F4SE-style) was evaluated as a way to get
at the renderer.

- It injects through a `winmm.dll` proxy and hooks `ck3.exe` by byte signatures pinned to
  **CK3 1.19.0.6**. Its shipped plugins only speed up CPU simulation work (trigger caches, family
  lists).
- Its only rendering code is a demo overlay that hooks **D3D11** `Present`. With CK3 on Vulkan,
  that path is never called.
- `ck3.exe` already statically imports `dxgi.dll`, and our app-local `binaries\dxgi.dll` bridge
  already gives us the same early, F4SE-style foothold.

**Decision:** not used. Everything below runs inside our own ReShade add-on, with no `ck3.exe`
hooks and nothing pinned to a game build. Its runtime-dumper idea was adopted in our own form
(Step 2).

---

## Step 2: Render dumper (`render_dump=1`)

New file: `src/feed_render_dump.h`. ReShade's add-on events fire for every pipeline, descriptor
update and draw CK3 issues on its Vulkan device, so the dumper sees the real frame from inside it.

How it works:

1. **Pipeline tagging from SPIR-V names.** CK3's DXC-compiled Vulkan shaders keep their member
   names; this was verified in the player's `shadercache\vulkan`.
   - GUI portrait composite: cbuffer member `PortraitUVOffset` (`jomini/gfx/FX/jomini/gui_portrait.shader`).
   - 3D characters: `PatternColorMasks_Texture` (only `portrait.shader` and `court_scene.shader`).
2. **Portrait screen rectangles.** The GUI portrait shader receives `WidgetPos` / `WidgetSize`
   (screen pixels) and `PortraitTextureSize` in cbuffer `pdx_hlsl_cb38`. The dumper parses that
   block's set, binding and member offsets out of the SPIR-V. It tracks descriptor updates,
   dynamic offsets, push descriptors and push constants per command list, copies the bound bytes
   into a readback ring at present, and decodes them 3 frames later.
3. **Ctrl+Shift+F11 census:** one frame's list of render passes with size, format, draw count and
   tagged draws.

What it found:

- Portrait data arrives through **descriptor set 0, binding 2**, from one constant-buffer ring.
  Rectangles decoded correctly on the first run, from the bookmark screen to in-game.
- The in-game frame has 46 render-target binds. The animated player portrait is rendered
  **off-screen at 380×421 (RGBA16F) with its own depth buffer** (4 character draws). It then goes
  through a small post chain (190×210 and 95×105 bloom, 8-bit tonemap). Finally the GUI composites
  12 portraits onto the backbuffer: the 380×421 player portrait at (-70,-104), partly off-screen,
  and about ten 80×100 council and court heads, which are cached rather than redrawn every frame.
- All portraits on screen add up to **~240 k pixels, about 12% of the frame**.
- Side finding for later: the 3D shaders receive the camera's `ViewProjectionMatrix` in
  cbuffer `pdx_hlsl_cb53`. That is a possible source of real map motion vectors instead of VORT
  estimates.

The dumper costs CPU time on every draw (that diagnostic run took ~5 minutes to reach the game
and pushed the CPU to 82 °C). It is a diagnostic tool only and stays fully off unless
`render_dump=1`.

---

## Step 3: Portrait mode v1 (`portrait_mode=1`)

- A lean, detection-only build of the dumper runs in normal play: no logging, no census, and no
  per-render-pass resource queries.
- Each frame, the visible portrait rectangles are clipped to the screen, overlaps merged, and
  shelf-packed into one small **atlas**. Color is copied in by region (`FeedVkCopyRegion`). DLSS
  and RenoDX's NR run on the atlas only, and each processed block is copied back to its exact
  screen spot.
- With no portraits on screen (map, loading screens), **nothing is evaluated**: the frame passes
  through untouched.
- The DLSS feature, and with it NR, is created at the atlas size.

**Result (06:48):** **45–57 fps**, with the feeder at ~10 ms per frame (512×640 atlas). The user
reported the game reaching the map dramatically faster, with DLSS visible on the portraits.

Problems seen:

- A **square box** around portraits that faded in and out.
- The player portrait alternated between 383×442 and 392×451 (CK3's hover pop-out). Every change
  counted as a new layout and reset NR's history.
- The atlas flipped between 512×512, 512×640 and 640×640 as windows opened, and each flip
  rebuilt the DLSS and NR feature.
- A "wait until stable" check paused NR while portraits moved, which made it blink.

---

## Step 4: Feathered paste-back, and a regression

New file: `src/feed_portrait_blend.h`. This is a small D3D12 compute pass recorded right after
the evaluate. It sets `out = lerp(input, NR, w)`, where `w` ramps from 0 to 1 over a margin inside
each block (sides on the screen border are not faded). The NR result then fades into the
untouched UI instead of ending in a hard edge. The shader is compiled at runtime with
`d3dcompiler_47` and was verified offline with `fxc`.

Also in this step: each block grew by a 24 px margin and was snapped to a 32 px grid; the
stability gate was removed; and the atlas was kept large for 10 s before shrinking.

**Result (06:56):** the edges improved, but **frame rate dropped to 10–29 fps**. The log showed
NR cost scaling almost linearly with atlas pixels, about **20 ns per pixel plus ~3 ms fixed**:

| Atlas | Pixels | Feeder cost | fps |
|---|---|---|---|
| 512×640 (Step 3) | 0.33 M | ~10 ms | 45–57 |
| 768×1024 | 0.79 M | 17–24 ms | 22–29 |
| 1152×1408 (bookmark screen) | 1.6 M | ~35 ms | 10 |
| Full 1920×1080 (start) | 2.07 M | ~45 ms | 15 |

Causes:

- Margins made neighbouring heads overlap, so they merged into big rectangles of mostly empty
  UI. One was 1632×1080 on the bookmark screen.
- The 10-second shrink delay kept a 1.6 M-pixel atlas running while only two small portraits
  were visible.
- Snapping alone did not absorb the hover pop-out.

---

## Step 5: Pixel budget and sticky blocks

- **Pixel budget** (`portrait_budget`, default 400 k pixels ≈ 11 ms of NR on a 3060). The
  largest portraits are processed first; the rest are passed through untouched instead of
  costing frame rate.
- **Merge only when the portraits themselves overlap**, not their margins.
- **Sticky blocks.** While a portrait still fits inside its current block (the hover pop-out
  shrinking back, sub-pixel drift), that block is kept, so the layout and NR's history hold
  still. This is capped so a portrait never inherits a block much larger than it needs.
- Margin reduced to 16 px with a 16 px snap. The atlas shrinks after ~1.5 s (90 frames).

**Result (07:03):** **~40 fps** (40.8 fps measured steady state, feeder at 13.5 ms) with no
errors. The user judged this acceptable for a 3060.

- The hover pop is gone: the player portrait keeps one block (`0,624 496x456`).
- Layout changes fell from ~150 to 33 in a comparable session.
- The council view packs as 11 small blocks instead of merged rectangles.

---

## Step 6: Zoom fix (zero motion vectors, fixed exposure)

The user noticed the box was more visible **when zooming the map**.

- GUI portraits don't move on screen, but the feeder was passing VORT's motion vectors for those
  pixels. Those vectors describe the map behind the portrait, so zooming or panning dragged the
  portrait's history around inside its block. Portrait mode now **clears the motion-vector atlas
  to zero**. VORT's validation mask is still passed, so DLSS favours the current frame where the
  map really moves inside a margin.
- DLSS auto-exposure was metering a few UI blocks whose margins change with the map. In portrait
  mode it is now **off**, with a fixed exposure of 1.0 (`flags=10` in the log instead of `74`).

**Result (07:07):** the build runs. The log confirms `flags=10` and the zero-motion-vector path,
and CK3 reported **`Total startup duration: 19.6 seconds`**. This build has only had a short
test, and its visual effect on the box during zoom is not yet assessed.

---

## Known issues, carried into Frontier 2

- **Slight square box around portraits.** It is now faint, but still visible, and more noticeable
  while zooming the map. The root cause is that NR changes the tone of everything inside a block,
  including the portrait's background and frame, while the pixels just outside stay untouched.
  Feathering softens the edge but does not remove it. **This is planned to be resolved in
  Frontier 2.** The planned fix is an **alpha-aware mask** built from the portrait texture's own
  transparency, which is available at the GUI draw. With it, NR is applied only to the character's
  pixels, so no rectangle exists to see.
- **Feature rebuilds when switching windows.** There were about 10 rebuilds in 50 s of quick
  window switching, each a brief hitch. Plan: one fixed-size atlas (e.g. 640×640, matching the
  budget), so DLSS and NR are never rebuilt.
- **History resets when the atlas repacks.** Opening or closing a window moves existing blocks
  inside the atlas, and NR restarts its history (a one-frame flash). Plan: persistent atlas slots,
  where blocks that stay on screen keep their position and only new ones are placed.
- **The map no longer gets DLAA** in portrait mode. Outside portraits, the frame is CK3's
  original image.
- **Flat depth for portraits.** NR receives a constant far-plane depth for the atlas, because the
  scene depth under a GUI portrait belongs to the map. The portrait's own 380×421 depth buffer
  (found in Step 2) could be fed in instead.
- **Detection latency.** Rectangles are read back 3 frames late. Crop and paste always use the
  same screen location in the current frame, so content is never misplaced, but NR briefly
  touches the area a window just vacated.
- **Measurement caveats.** The 19.6 s start-up was measured on a later launch with warm file
  caches. A cold-start comparison against the 266.5 s baseline is still to be done.

---

## Configuration (`binaries\dlss-active\dlss5-feed.cfg`)

| Key | Default | Meaning |
|---|---|---|
| `portrait_mode` | 0 (set to 1 for this work) | DLSS/NR only on GUI portraits. Detection is registered at start-up, so changing it needs a game restart. |
| `portrait_budget` | 400 | Maximum atlas pixels, in thousands. NR cost is ~linear in pixels. |
| `portrait_feather` | 16 | Margin in px that each block grows by, and over which NR fades in. 0 gives a hard edge. |
| `portrait_min` | 48 | Portraits smaller than this, in visible width or height, are skipped. |
| `render_dump` | 0 | Diagnostic dumper (logging plus the Ctrl+Shift+F11 census). Expensive; needs a restart. |

---

## Files

| File | Change |
|---|---|
| `src/feed_render_dump.h` | **New.** Pipeline tagging, portrait-rectangle decoding, readback ring, census. |
| `src/feed_portrait_blend.h` | **New.** D3D12 compute edge blend after the NR evaluate. |
| `src/feed_vk.h` | Region copy, region blit and clear helpers (`FeedVkCopyRegion`, `FeedVkBlitRegion`, `FeedVkClear`). |
| `src/dlss5-feed.cpp` | New config keys; atlas builder (budget, sticky blocks, hysteresis); portrait path in the Vulkan transport; zero MVs and fixed exposure in portrait mode; config reload counted per call. |

## Reproducing

```cmd
build-local-current-tree.cmd
layer\build-layer-local.cmd
```

Copy `build\dlss5-feed.addon64` to `<CK3>\binaries\dlss-payload\`, rerun the profile install
(`DLSS5-CK3.ps1 -Action Install -Profile DLSS5Extended`), then set `portrait_mode=1` in
`binaries\dlss-active\dlss5-feed.cfg`. Launch with `Launch CK3 with DLSS.cmd`. Compare the
`feed CPU` lines on the map, in your own character window, and in the council view.
