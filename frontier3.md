# Frontier 3: portrait mask and live validation

Handoff for the next agent, written 2026-10-07. Continue from the current working
tree and the installed Frontier 2 build. Frontier is the CK3 rendering milestone
name in this repository. Read [Frontier 2](frontier2.md) for the completed work and
[Frontier 1](frontier.md) for the original portrait investigation.

## Progress on 2026-10-09: offline work done, live steps waiting

Everything below that can be built and verified without playing CK3 is done. Sections 0
and 1 need a live session, and the install is not on the profile Frontier 3 needs. No new
build has been deployed to the game folder.

### The install changed after this handoff

On 2026-10-08 the install was switched to the experimental **NativeStreamline** profile.
The feeder is still `ck3-frontier2-atlas.1` (SHA-256 `c34f6abf…`), but the active config
has no portrait settings (`preset=0`, no `portrait_mode`), so the feeder evaluates the full
frame. In that session ReShade.log shows `feature 18 create failed with 0xbad00001` and no
"evaluation succeeded" line: DLSS 5 neural rendering did not run. Frontier 3 measures
portrait mode on **DLSS5Extended**. Switching back is the user's decision. Reinstalling
DLSS5Extended restores the saved profile config from `binaries/dlss-cache/profiles/
DLSS5Extended/dlss5-feed.cfg` (preset 13, portrait_mode 1, feather 16, budget 400) and puts
the Frontier 2 payload feeder back into `dlss-active`.

### What was built (build `ck3-frontier3-diag.1`, file version still 0.6.0.0)

All new behaviour is off by default. With the default config the only differences from
Frontier 2 are the build name, reset-cause bookkeeping and an inert present callback.

| Area | Change | Files |
|---|---|---|
| Section 0/2: render dump | With `render_dump=1`: the set/binding of the Frame, Mask, Portrait and Background textures is parsed from both shader stages; image descriptors and texture/view lifetimes are tracked; every portrait draw logs its textures as `T#id` (each texture described once: size, format, mips, usage, view) plus PortraitUVOffset/Scale, PopOutThreshold and IsGrayscale. Ctrl+Shift+F11 now also probes that frame's Portrait/Mask/Frame textures: alpha statistics over the exact texel rectangle each draw samples, an "alpha looks like character coverage" verdict, and PNGs in `binaries/dlss-active/dlss5-feed-dump/`. To make the copy legal, dump mode adds copy_source to sampled 2D textures at creation. | `src/feed_render_dump.h`, `src/feed_dump_probe.h` |
| Section 1: baseline | `frame_stats=1` (live, no restart) writes one CSV row per game Vulkan present to `dlss5-feed-frames.csv`, evaluated or not: interval, evaluated/skip reason, atlas size, blocks, portrait count, rectangle age, reset and reset causes, feature builds, feeder CPU time. Every 600 presents the log gets median/p95/p99/max, hitch counts and skip/reset breakdowns. `tools/frame-stats.ps1` summarises a capture with warm-up and post-build exclusion, a time window, and `-Compare` for matched runs. | `src/feed_frame_stats.h`, `tools/frame-stats.ps1` |
| Section 3: resets | Every reset carries its causes: canvas, new_crop, moved_slot, translated, resized (atlas geometry), plus build, resume, reset_every and failure. GetPortraits' data now has a frame of origin (`rect_age` in the CSV). Behaviour is unchanged: resets stay conservative. | `src/feed_portrait_atlas.h`, `src/dlss5-feed.cpp` |
| Section 2: composite | The blend pass accepts an R8 coverage atlas. A block flagged in `Cb::coverage_blocks` is one job over the whole block, interior included: `src + (neural - src) * feather * coverage`, with coverage 0 giving the original pixel bit-exactly and 1 the neural pixel. Flags are ignored when no coverage is bound, so a missing mask falls back to the legacy edge feather. The feeder does not bind coverage yet: no verified mask producer exists. | `src/feed_portrait_blend.h` |
| Deployment | `tools/Frontier3-Live.ps1`: Status, Deploy (refuses unless DLSS5Extended and CK3 closed; hash-verified backup, both copies, receipt), Configure (render_dump, frame_stats, portrait_*), Collect (logs, CSV, dump PNGs into `build/frontier3-<label>-<stamp>/`), Restore. | `tools/Frontier3-Live.ps1` |

### Evidence, with its limits

Reset causes on the recorded geometry replays (crop geometry only; one reset can have
several causes):

| Replay | Resets | canvas | new_crop | moved_slot | translated | resized |
|---|---|---|---|---|---|---|
| `frontier2-session-crops.tsv`, 405 layouts | 274 | 11 | 105 | 64 | 62 | 193 |
| `frontier2-trace-crops.tsv`, 109 layouts | 75 | 1 | 23 | 6 | 28 | 36 |

"resized" dominates: crops that overlap a previous crop but changed rectangle. These are
geometry labels, not identity: a same-place character swap is invisible to them.

Blend microbenchmark, RTX 3060, 640x640 council atlas, ten blocks (blend pass only):

| Pass | RGBA8 | RGBA16F |
|---|---|---|
| Frontier 1 full-atlas reference | 0.284 ms | 0.282 ms |
| Frontier 2 edge bands, in place | 0.0040 ms | 0.0040 ms |
| Frontier 3 coverage, whole blocks, in place | 0.0143 ms | 0.0254 ms |

Coverage processes 323,520 block pixels instead of 73,344 edge pixels. Its extra memory is
one R8 atlas, at most about 400 KB under the 400k-pixel budget. None of this is a game
frame-time result.

Tests added or extended, all passing on 2026-10-09: `tests/render-dump-probe.cpp`
(hand-assembled SPIR-V, texel decode, alpha statistics, UV mapping, PNG checked against
Python's zlib), reset attribution in `tests/portrait-atlas.cpp` (agrees with SameHistory on
2,000 scenes), coverage scenarios in `tests/portrait-blend.cpp` (full, partial, zero and
interior holes; mixed coverage/legacy blocks; coverage flags with nothing bound; an empty
mask; three formats, typed and copy paths, three frame slots, WARP and RTX 3060; a mutation
that drops the coverage multiply fails the test), `tests/Test-FrameStats.ps1` and
`tests/Test-Frontier3Live.ps1` (fake game folder only). The dumper's in-game paths cannot
run outside CK3 and are unverified until the first dump session.

### Next steps, in order

1. **User decision:** switch back to DLSS5Extended (`Install CK3 DLSS 5 Extended Test.cmd`
   in the game folder, or `DLSS5-CK3.ps1 -Action Install -Profile DLSS5Extended`). Confirm
   with `.\tools\Frontier3-Live.ps1 -Action Status`: profile DLSS5Extended, portrait settings
   back, NeuralUplift 1.
2. Launch once with the Frontier 2 feeder and confirm DLSS 5 evaluates (Status shows
   "evaluation succeeded" lines and no failures). This separates profile problems from
   Frontier 3 changes.
3. Deploy: `.\tools\Frontier3-Live.ps1 -Action Deploy -Addon .\build\dlss5-feed.addon64`.
   Launch normally and confirm the log says `ck3-frontier3-diag.1` and DLSS 5 still evaluates.
4. Section 0: `-Action Configure -Set render_dump=1`, launch, capture the scenes in section 0
   with Ctrl+Shift+F11 in each, close, `-Action Collect -Label dump`, then
   `-Action Configure -Set render_dump=0`. The probe lines answer the section 2 question:
   does the Portrait texture's alpha describe the character?
5. Section 1: `-Action Configure -Set frame_stats=1` (can also be toggled live), repeat the
   scenes, `-Action Collect -Label baseline`, then summarise with `tools/frame-stats.ps1`.
6. Then the mask producer (below), using what step 4 found.

### Still open

- **Mask producer (section 2).** Build the coverage atlas on the GPU from the draw that the
  dump identifies, in the same frame as the colour crop, transport it like `SLOT_MASK` but as
  its own slot, and set `coverage_blocks` only for crops whose coverage matches. If the probe
  shows opaque portrait alpha, trace the real coverage source first. Pixel tests for clipping
  and overlapping source draws belong with the producer.
- **Frame association (section 3).** Rectangles still come from a readback three frames late;
  the dump will show whether that age matters. Identity-aware fixtures and slot-preserving
  resets wait on real draw identity.
- **Depth (section 4).** Untouched; still after mask and association work.
- **GPU stage timing.** Not added; the CSV is CPU-side only and says so.

## Objective and priority

Start with a render dump of the current build ([section 0](#0-render-dump-of-the-current-state-do-this-first)).
Then remove the visible portrait background rectangle while preserving character detail
and the original map/UI. Establish the current build's actual frame times before
making more performance claims. Then reduce stale portrait detection and unnecessary
history resets using correctly associated draw metadata. Portrait depth is a later
experiment, dependent on identifying the right resources.

The user has already requested real graphics improvements, live-game inspection,
and installation into their CK3 folder for easier testing. The planned Frontier 3
work below has not started. One side experiment (a network-side character mask)
was built, deployed, rejected and fully reverted on 2026-10-07; see
[Rejected: network-side control mask](#rejected-network-side-control-mask). Do not
repeat it.

## Starting state

| Item | Current state |
|---|---|
| Workspace | `C:\Users\admin\Documents\GitHub\DLSS5-Feeder#install-for-a-64-bit-game` |
| Game root | `C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III` |
| GPU at last test | RTX 3060, 12 GB, driver 610.88 |
| Renderer / profile | Vulkan / DLSS5Extended |
| Installed feeder | `ck3-frontier2-atlas.1`, file version `0.6.0.0` |
| Build output | `build/dlss5-feed.addon64` |
| Installed copies | `binaries/dlss-active/dlss5-feed.addon64` and `binaries/dlss-payload/dlss5-feed.addon64` |
| Launch | `Launch CK3 with DLSS.cmd` in the game root |
| Settings | enabled 1, mode 2, portrait_mode 1, portrait_min 48, portrait_feather 16, portrait_budget 400, preset 13, render_dump 0 |

Installed SHA-256:
`c34f6abf42e807258ad116ff56617f374159487c5114790b5c511450cf9d9ed3`.
The receipt is `binaries/dlss-active/CK3-DLSS-RUNTIME.json`. The latest backup is
`binaries/dlss-backups/frontier2-atlas-20261007T074619Z-3519b022`, including the
previous active/payload add-ons, receipt, configuration and deployment manifest.
Recheck the process, hashes and settings before working; this records the handoff
baseline rather than guaranteeing that a later session has the same state.

The working tree has uncommitted changes, including new files. Preserve the
Frontier 2 source, tests, fixtures and documentation. The repository's packaged
release payload has **not** been refreshed; the game's local payload has. A new
agent on another checkout must obtain these working-tree files before proceeding.

## What is already finished

- The feather blend schedules disjoint edge bands, with one writer per pixel and
  one dispatch. Typed UAV support permits blending in place; the SRV copy fallback
  remains available. Frame-slot fences, UAV ordering and device cleanup are tested.
- The guarded MaxRects allocator retains surviving slot addresses, enforces the
  budget on the entire allocated texture, grows when coverage benefits, limits
  aspect changes, and delays shrinkage for 180 portrait updates.
- Removal preserves history when surviving crops and addresses match. New/changed
  crops still request a global reset. Unselected portraits retain native rendering.
- Vulkan clears color and optional motion-validation mask before crop copies;
  transfer ordering and retired-slot pixels have a GPU readback test.

Do not replace this with an always-full atlas or reinstate full-atlas feather
processing without measurements justifying the extra work.

Evidence to preserve, with its limits:

| Test | Result | Meaning |
|---|---|---|
| Blend microbenchmark, RTX 3060 | About 0.277 ms to 0.0038-0.0039 ms | Blend only; excludes NGX, Vulkan transport and CK3 |
| 405 recorded session layouts | Mean atlas area 534,927 to 399,087 pixels; peak 720,896 to 399,360 | Geometry replay, not a frame-time comparison |
| Same replay | Dimension changes 64 to 11; history resets still 274 | Rebuild opportunities reduced; reset churn remains |
| Same replay | 84.7% of original crop pixels, 70.3% of original slot instances selected | Some small portraits stay native under the stricter budget |

The latest atlas build has no controlled in-game FPS result. The old feeder's
timing summaries count evaluated frames, include GPU waits and can span skipped
evaluation periods. They must not be presented as independent game FPS.

## 0. Render dump of the current state (do this first)

Before any Frontier 3 change, run the render dumper (`render_dump=1`, see
[Frontier 1, step 2](frontier.md#step-2-render-dumper-render_dump1)) against the
installed Frontier 2 build. Frontier 1's single dump found the essential facts:
the GUI portrait pipeline, the `WidgetPos`/`WidgetSize` rectangles in
`pdx_hlsl_cb38`, the off-screen 380x421 RGBA16F player portrait with its own depth
buffer, the bloom/tonemap chain, the cached 80x100 council heads, and
`ViewProjectionMatrix` in `pdx_hlsl_cb53`. Sections 2 and 4 depend on those
resources, and the game, Frontier 2's atlas and the scenes in use have all changed
since then, so check the current picture first.

Steps:

1. Confirm DLSS 5 is on: `NeuralUplift=1` in `ReShade.ini`. Without the dumper,
   `ReShade.log` should show "inline feature 18 evaluation succeeded" and no
   "evaluate failed".
2. With CK3 closed, set `render_dump=1` in `binaries/dlss-active/dlss5-feed.cfg`.
   It is read at start-up only. Change no other setting.
3. Launch and load the usual save. Capture the same scenes section 1 will use: a
   character panel with a large portrait, council, hover expansion, and the map
   with ruler badges. In each, press **Ctrl+Shift+F11** for a one-frame census of
   render passes (size, format, draw count, tagged draws).
4. Copy `dlss5-feed.log`, `ReShade.log` and the config into
   `build/frontier3-dump-<UTC stamp>/` before the next launch overwrites them.
5. Close CK3, set `render_dump=0`, and confirm the next launch logs normally.

Cautions: the dumper costs CPU on every draw. In Frontier 1 the game took about 5
minutes to reach the map and the CPU reached 82 °C. Never take section 1 timings
with it on, and keep the dump session separate from baseline runs. It shows passes,
targets and tagged draws, but it does not yet record which texture a portrait draw
samples, or that texture's alpha. Section 2 adds that.

What to compare against Frontier 1: whether the 380x421 RGBA16F portrait target and
its depth buffer still exist and at what size (section 4); which portraits are
redrawn each frame and which are cached (section 3); the GUI portrait draw order and
overlaps (section 2); and how the feeder's atlas pass appears in the frame.
Summarise the differences here before starting section 1.

## 1. Establish a live baseline first

Confirm the installed startup log identifies `ck3-frontier2-atlas.1` and the expected
profile. The prior live inspection captured a paused map with a character panel
and ruler badges. For additional views or movement, coordinate with the user; the
earlier request was to inspect the current scene.

Use repeatable scenes: map without eligible portraits, one character panel, council,
hover expansion, window changes, and map pan/zoom with ruler badges. Keep the save,
resolution, settings and scene timing the same between variants. Separate warm-up
and feature creation from steady-state intervals. Repeat runs and retain raw data.

Measure every game Vulkan present, including frames where portrait evaluation is
skipped. Filter out the bridge's hidden D3D12 runtime. Report median and p95/p99
frame times and hitch counts, alongside evaluated/skipped frames, atlas dimensions,
selected coverage, feature rebuilds and reset reasons. F6 is useful for a visual NR
comparison, but toggle transients should be excluded from steady-state measurements.
Record the measurement method; CPU feeder wall time and GPU work are separate costs.

Use asynchronous GPU timestamps if adding stage timing. Avoid a new CPU/GPU wait
per frame. Separate transport, NGX/NR evaluation and blending where the available
hooks allow it; otherwise label a combined interval honestly. Capture screenshots
for static edges and a short sequence for flashing/ghosting while moving.

Acceptance: a reproducible current-build baseline and matched candidate comparison,
with raw captures and clear limits. Do not infer a whole-game speedup from the blend
microbenchmark or from fewer pixels in a geometry replay.

## 2. Capture a real character coverage mask and composite with it

Start in `src/feed_render_dump.h`. GUI portrait pipelines are tagged using SPIR-V
names, including `PortraitUVOffset`; uniform/push-constant decoding currently reads
`WidgetPos`, `WidgetSize` and `PortraitTextureSize`. `DescEntry` tracks constant
buffers, and published `PRect` contains only x/y/w/h. Image descriptors, character
coverage and stable portrait identity are not currently published to the feeder.

Extend draw inspection to identify the actual sampled portrait resource/view,
descriptor array element, subresource, UV transform, viewport and scissor. Verify
the texture's format and channel values in a captured draw. Frontier 1 proposed
using portrait transparency; useful alpha is **not yet verified by this code**.
If that texture has opaque alpha, trace the actual GUI coverage source or capture
coverage from the relevant draw instead of guessing from color or screen depth.
Associate overlapping layers and final draw order with their effective coverage.

Create the mask on the GPU using the same draw/crop metadata and frame association
as the color it will affect. Account for texture-space to widget-space mapping,
clipping, scaling, portrait animation and partially transparent hair/clothes. Keep
resource generations and lifetimes valid until submitted GPU work retires. Verify
the game's image supports any proposed copy/sample usage; inspect resource creation
and state tracking before adding usage flags or changing layouts.

Transport character coverage as its own atlas resource if it is consumed on the
private D3D12 device. Preserve Vulkan/ReShade layout tracking, cross-API fence order,
frame-slot retirement and teardown/rebuild behavior.

**`SLOT_MASK` is not character alpha.** It is the shader's R8 motion-vector validation
mask, passed to NGX as `pInBiasCurrentColorMask`. Keep that meaning intact. A new
silhouette/coverage resource has a different purpose and needs explicit ownership.

The final composition should restore original pixels wherever character coverage
is zero and apply NR where coverage is one, with a defined soft transition for
intermediate coverage. A useful reference RGB formula is
`original + (neural - original) * coverage * feather_weight`, evaluated against the
already composited current game frame. Define output alpha behavior explicitly;
do not composite the portrait texture over the background a second time.

**The current scheduler visits only outer feather bands.** Sampling a mask only in
those jobs will leave neural-altered background inside the crop untouched. The new
pass must cover interior pixels where coverage is below one as well. Use active
portrait regions or measured mask-aware tile jobs, retaining unique writers and
the typed-UAV/fallback capability checks. Measure the added work and memory.

Missing, stale or mismatched coverage must have an explicit fallback: skip that
crop or use a documented legacy path. Never apply another portrait's mask. An
entirely empty mask should not produce a neural rectangle in the map/UI.

Acceptance: no visible rectangle around characters during static view or map
pan/zoom; original background pixels survive even in crop interiors; transparent
edges retain detail; unrelated UI stays unchanged. Add an independent pixel oracle
for zero/full/partial coverage, interior holes, screen clipping, overlapping source
draws, mask mismatch and all supported output formats. Exercise both shader paths,
three frame slots and actual Vulkan mask transport/readback.

## 3. Associate current draws correctly and reduce history resets

The detector has four readback slots and currently publishes data from three frames
earlier. `GetPortraits` exposes rectangles and stability, not the originating frame
or source identity. A just-closed window can briefly leave a stale processed crop.
Color is copied from the current frame, so adding a mask from an older draw without
matching it would make this problem more visible.

Publish frame/age, source view/subresource/UV region, resource generation and the
relevant draw metadata together. Prefer current CPU-visible descriptor or
push-constant data where available. For GPU-only constants, prove completion before
mapping/reusing a slot; elapsed frame count alone is not a synchronization contract.
If matching current coverage cannot be established, skip or invalidate that crop.
Keep the solution asynchronous rather than forcing a synchronous readback.

Propagate reliable identity into atlas placement. A resource handle alone can be
recycled or contain several portraits; equal rectangle dimensions also do not mean
the same character. Source content changes can require reset even at the same
screen coordinates. Pure screen translation might preserve history only when the
source, crop mapping and atlas address are demonstrably unchanged. Keep conservative
resets until that association is proven. NGX/NR has no per-slot reset contract in
the current implementation; do not merely remove the global reset flag.

Log reset reasons separately: new identity, changed content/mapping, moved slot,
dimension change, stale detection and session/resource rebuild. Improve coverage
under the existing hard budget only after measurements; do not silently raise 400
or let tiny map badges reshape a full atlas again.

Acceptance: closing/changing windows cannot process a vacated background with an
old mask; same-size character replacements cannot inherit unrelated history; stable
or verified translated portraits retain their slots safely. Compare reset/rebuild
counts and visual stability in live frame sequences. New identity-aware fixtures
are needed: the existing crop-only TSV files cannot test these properties.

## 4. Investigate portrait depth after mask and frame association work

Portrait mode currently clears depth to a flat far plane, clears motion vectors to
zero and fixes exposure to 1.0. The map's depth and motion behind a GUI portrait are
not the character's geometry. Keep those choices until replacement inputs are
verified.

Frontier 1 observed a 380x421 portrait render target/depth candidate. Identify the
actual per-character resource, view, projection, depth convention and frame, then
map it to the displayed widget and atlas crop. Do not assume that a similarly sized
depth image belongs to the right character or contains linear depth. Keep the flat
fallback when association is unavailable. If character animation needs vectors,
derive them from the portrait rendering path rather than reusing map vectors.

Acceptance: demonstrable visual benefit without regressions, correct foreground
coverage and depth convention, and measured GPU/memory cost. This is an experiment
until resource association and benefit are established.

## Rejected: network-side control mask

Tried and abandoned on 2026-10-07. The idea was to hand the neural rendering
network a character mask so it would leave the portrait background alone itself,
instead of the post-evaluation composite in section 2. It produced no usable
result, broke DLSS 5 in the live install twice, and the user directed that it be
dropped. All code was removed and the Frontier 2 build was restored. Section 2's
composite remains the plan.

### Where the idea came from

The DLSS5-NR AMD project (`dlss5-nr-amd-custom`, `runtime/common/ngx_cuda.h`) lists the
NR parameter `DLSSNR.ControlMask`; it never sets it and always sends
`DLSSNR.UseAutoMask=1`. Strings in the installed `nvngx_dlssnr.dll` (310.8, ShortFuse
build) confirm it accepts `DLSSNR.ControlMask` with its own subrect parameters and
has `..._control_mask` kernels. Nothing documents the mask's format, value meaning,
or whether it is honoured when the automatic mask is off. RenoDX in this install
sends `UseAutoMask=0` and supplies no mask.

### What was built and what failed

1. **The mask cannot travel on the feeder's contract.** RenoDX allocates its own
   NR parameter block ("DLSS-NR AllocateParameters failed" in its binary), so a
   value set on `g.params` never reaches the network. The only injection point
   found was the NR runtime's own `NVSDK_NGX_D3D12_EvaluateFeature` export
   (arguments: command list, handle, parameter block, callback).
2. **Build `ck3-frontier3-nrmask.1`: a MinHook detour on that export broke NR
   completely.** The runtime checks the module of its *return address* and
   rejects any caller but NGX. With a calling detour, every NR evaluate failed
   with `0xBAD00002` and RenoDX latched NR off. This happened even in observe-only
   mode, which changed no parameters. That session also ended with a
   `CreateFeature` access violation (`0xC0000005`), cause unconfirmed. The
   disassembly had been misread: `[rsp+0x288]` is the return address, not a fifth
   argument.
3. **Build `ck3-frontier3-nrmask.2`: a hand-written x64 pass-through stub.** It
   saved the argument registers, called a pre-hook, restored them and jumped to
   the trampoline. An offline test showed MinHook also rejected a non-executable
   stub page (`MH_ERROR_NOT_EXECUTABLE`), which was fixed. Live, observe mode
   kept NR working: 3,600+ evaluates, zero failures. With an all-zero mask
   injected, the user saw degraded output and no proper DLSS 5. That observation
   is **not a clean result**. An all-zero mask was expected to suppress NR if
   honoured, and the user also used F6 during that window, which turns NR off.
   No comparison between mask values was completed, so whether the network
   honours the mask remains unknown.
4. **Side effect: F6 persists.** F6 is RenoDX's NR toggle and it saves its state
   (`NeuralUplift` in `ReShade.ini`). Suggesting it for a visual comparison left
   NR off at the next launch, which looked like a regression of the restored
   build. Never use F6 as a test step without restoring and confirming
   `NeuralUplift=1` afterwards.

### Why it will not work long term

- **It depends on an internal, undocumented interface.** The hook targets the
  NR runtime's snippet export, which NVIDIA does not intend anyone but NGX to
  call. It relies on the exact argument layout of one modified, unsigned build
  (`310.8.SF.0`). Any runtime, driver or RenoDX update can move or change it
  silently.
- **It works against a deliberate caller check.** The runtime verifies who calls
  it. Working around that needs machine code with no unwind information inside
  NVIDIA's call path, and an exception crossing it cannot be caught. That
  fragility is not acceptable in a released package.
- **It competes with RenoDX for the same call path.** RenoDX uses Detours on NGX
  modules, re-attaches hooks on every present and patches the NR runtime's import
  table. A second hooker in the same path is untestable against future RenoDX
  versions. Both live attempts changed NR behaviour.
- **The mask's meaning is unknown.** The texture format, value convention
  (apply vs protect), resolution, and interaction with `UseAutoMask` would all
  have to be reverse engineered and re-verified per runtime build.
- **It does not remove the hard part.** Either way, Frontier 3 must find each
  character's real coverage (section 2). A network-side mask only changes who
  applies it, so success would have saved the composite pass and nothing else.
- **It cannot be shipped or supported.** Patching a third-party NVIDIA binary
  in memory is outside what the installer and profiles can validate, and a
  failure presents as "DLSS 5 is gone", which users cannot diagnose.

**Reopen only if** NVIDIA documents `DLSSNR.ControlMask` (format, value meaning,
interaction with `UseAutoMask`) and it becomes settable through a supported
interface, such as NGX/Streamline or a RenoDX setting that forwards it. Until then,
this path is exhausted from the outside.

### State after the revert

- Installed feeder: `ck3-frontier2-atlas.1`, SHA-256
  `c34f6abf42e807258ad116ff56617f374159487c5114790b5c511450cf9d9ed3`, in both
  `dlss-active` and `dlss-payload`. Config and receipt are restored from
  `binaries/dlss-backups/frontier3-nrmask-20261007T081250Z`, the pre-experiment backup.
  `frontier3-nrmask2-20261007T093602Z` holds the rejected nrmask.1 build only for
  reference; do not restore it.
- Source: `src/feed_nr_mask.h`, `tests/nr-mask-stub.cpp`,
  `tests/test-nr-mask-stub.cmd` and the `nr_mask`/`nr_auto_mask` config keys are
  deleted. The working tree matches the Frontier 2 state, and the existing tests pass.
- `ReShade.ini` was left at `NeuralUplift=0` by F6. The user turns NR back on with
  F6; confirm `NeuralUplift=1` before any live measurement.
- Verification rule for any live test: `ReShade.log` must show "inline feature
  18 evaluation succeeded" and no "evaluate failed" lines before a build is
  called working.

## Files and verification commands

| File | Responsibility |
|---|---|
| `src/feed_render_dump.h` | Pipeline/descriptor tracking, draw metadata, readback and rectangle publication; dump-mode texture identity and census probe |
| `src/feed_dump_probe.h` | Game-independent probe helpers: SPIR-V bindings, texel decode, alpha statistics, UV mapping, PNG |
| `src/feed_frame_stats.h` | `frame_stats=1` per-present CSV and log summaries |
| `src/feed_portrait_atlas.h` | Candidate preparation, guarded placement, hard budget and history comparison |
| `src/feed_portrait_blend.h` | GPU composition, scheduling, format capability checks and resources |
| `src/feed_vk.h` | Vulkan imports, clears, copy/blit helpers and barriers |
| `src/dlss5-feed.cpp` | Configuration, `FeedFrameVk`, shared slots, NGX contract, resets and lifecycle |
| `tests/portrait-atlas.cpp`, `tests/vk-portrait-atlas.cpp` | Geometry invariants/replay and input pixel readback |
| `tests/portrait-blend.cpp` | Independent CPU reference versus actual GPU shader output, including coverage compositing |
| `tests/render-dump-probe.cpp` | Probe helpers against hand-assembled SPIR-V and synthetic textures |
| `tools/frame-stats.ps1`, `tests/Test-FrameStats.ps1` | Capture summary/comparison and its fixture test |
| `tools/Frontier3-Live.ps1`, `tests/Test-Frontier3Live.ps1` | Live-install helper and its fake-install test |
| `tests/feeder-compat.cpp`, `tests/vk-present-order.cpp` | Submission failure handling, formats/config and present dependency ordering |

From a checkout with the current dependencies and Visual Studio 2022 Build Tools:

```powershell
cmd /c .\build-local-current-tree.cmd
cmd /c tests\test-portrait-atlas.cmd tests\fixtures\frontier2-session-crops.tsv
.\build\compat-tests\portrait-atlas.exe tests\fixtures\frontier2-trace-crops.tsv
cmd /c tests\test-portrait-blend.cmd
cmd /c tests\test-feeder-compat.cmd
cmd /c tests\test-render-dump-probe.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File tests\Test-FrameStats.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\Test-Frontier3Live.ps1
git diff --check
```

`--benchmark` is optional for the blend test when the pass changes. The checked
results include 2,000 randomized allocator scenes, clipping/guards/hysteresis,
WARP and RTX 3060 shader pixels, copy/typed variants, three output formats and
dispatches spanning two rows. D3D12 debug validation was unavailable locally;
readback and device completion were checked. Enable debug validation when available.

Local inspection artifacts are in `build/frontier2-live-20261007T070339Z/`:
`game-scene.png`, `telemetry.csv`, loaded modules, feeder/ReShade snapshots and
`analysis.json`. These are ignored and may be absent in another checkout. The two
TSV fixtures in `tests/fixtures/` contain only processed crop geometry. They replay
one update per logged layout, excluding unchanged frames and actual GPU work.

## Deployment and completion

Build and test before replacing the add-on. Confirm CK3 is closed before copying.
Back up the active/payload add-ons, active config and runtime receipt outside the
add-on search directory; verify backup hashes. Copy the tested feeder to both game
locations, verify SHA-256 against the build, and update the receipt's feeder source,
hash, file version and deployment time. Give the feeder a distinct build identifier
so live logs distinguish Frontier 3. Preserve the user's profile and settings.

Validate using the installed launcher script:

```powershell
& 'C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III\DLSS5-CK3.ps1' `
    -Action Validate `
    -GameRoot 'C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III'
```

Profile reinstall may overwrite local testing settings or files; use a deliberate
deployment rather than running a broad installer as a shortcut. A release/package
refresh is separate work: inspect the builder and packaging tests, include all new
source/tests/fixtures, and verify fresh install/reinstall behavior before claiming
that downloaded packages contain the optimization. Publishing is not part of this
documentation request.

Frontier 3 is ready for review when the character-mask fix is implemented and tested,
matched live captures demonstrate its appearance and measured costs, stale metadata
cannot affect the wrong scene, and deployment is identifiable and recoverable.
Record any deferred depth or identity work explicitly. Update this file,
`frontier2.md`, `architecture.md` and README with the measured final state; avoid
promising a released box fix while it is still only a plan.
