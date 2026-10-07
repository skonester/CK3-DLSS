# Frontier 2: portrait pipeline optimization

Implemented and exercised locally on 2026-10-07. In this repository, Frontier 2 is the
next CK3 rendering milestone after [Frontier 1](frontier.md). This change optimizes its
existing feather blend and atlas allocation; the portrait alpha-mask fix remains
outstanding.

## Result

The old pass copied the entire neural-rendered atlas, searched every portrait block
for every atlas pixel, and rewrote the entire atlas. The new pass schedules only the
feather bands. On devices that support typed UAV loads for the output format, it
reads and writes the neural result in place, eliminating the scratch texture and
the atlas copy.

GPU timestamp measurements on **NVIDIA GeForce RTX 3060, 12 GB, driver 610.88**:

| Output format | Frontier 1 blend | Edge jobs with copy fallback | Edge jobs in place | Blend speedup |
|---|---:|---:|---:|---:|
| RGBA8 UNORM | 0.276784 ms | 0.017546 ms | 0.003897 ms | 71.02x |
| RGBA16 FLOAT | 0.276687 ms | 0.028893 ms | 0.003821 ms | 72.41x |

The fixture uses a 640x640 atlas with ten non-overlapping portraits: one 496x456
block and nine 104x104 blocks. A 16-pixel feather, with one screen-edge exemption,
requires **73,344 edge pixels and 591 thread groups** instead of processing all
409,600 atlas pixels. The typed path also eliminates a 1.56 MiB RGBA8 or 3.13 MiB
FP16 scratch atlas, excluding allocation alignment.

These are **warm-cache blend microbenchmarks**, not CK3 frame-rate measurements.
Each path receives 16 warm-up iterations; the reported value is the median of five
128-iteration GPU batches, with the path order rotated between trials. GPU
timestamps include the pass's barriers, copy where applicable, and dispatch. The
fixture uses the same shared-heap and simultaneous-access resource flags as the
feeder. It does not evaluate NGX, perform Vulkan transport, or run CK3. Neural
rendering still dominates the cost reported in Frontier 1, so these ratios must
not be interpreted as whole-frame speedups.

## Implementation

`src/feed_portrait_blend.h` keeps the existing input block interface and requires
no configuration changes:

- Each block contributes up to four disjoint edge bands. Corners and overlapping
  feathers on tiny portraits still receive exactly one writer.
- One dispatch processes all bands in 16x8 thread groups. A group-uniform binary
  search assigns the band from cumulative group tickets, replacing the divergent
  per-pixel block scan. Partial tiles are checked before any texture access.
- Dispatches larger than 65,535 groups spill into a second dispatch dimension.
- Capability checks include `TypedUAVLoadAdditionalFormats` and the output format's
  typed UAV load/store support. Unsupported formats use the SRV copy fallback,
  which still schedules only edge pixels.
- Portrait interiors retain the neural result bit for bit. Atlas padding is left
  untouched: the Vulkan paste-back only copies portrait rectangles.
- UAV barriers order evaluation, blending, and subsequent consumers. Each in-flight
  constant-buffer slot is 4 KiB, and the feeder's existing allocator fence protects
  reuse. Target release and device shutdown release scratch resources and both
  cached pipeline variants.

Microsoft documents the capability requirements in its
[typed UAV load specification](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/UAVTypedLoad.md)
and UAV ordering in the
[D3D12 UAV barrier reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_uav_barrier).

## Verification and reproduction

From a checkout with the dependencies described in README.md:

```cmd
build-local-current-tree.cmd
tests\test-portrait-blend.cmd --benchmark
tests\test-portrait-atlas.cmd tests\fixtures\frontier2-session-crops.tsv
build\compat-tests\portrait-atlas.exe tests\fixtures\frontier2-trace-crops.tsv
tests\test-feeder-compat.cmd
```

`tests/portrait-blend.cpp` executes the shipped shader and checks its pixels
against an independent CPU feather calculation on both WARP and the RTX 3060.
It covers RGBA8 UNORM, RGB10A2 UNORM, FP16 including negative and HDR color values,
24 tiny portraits, oversized and zero feathers, odd atlas dimensions, empty
inputs, all three upload slots, and both shader variants. Interiors and padding
must remain bit-exact. Edge values must match within one storage step.

The CPU scheduler additionally checks 128 deterministic randomized layouts,
unique pixel ownership, out-of-bounds and overlapping block rejection, and the
maximum texture dimensions. A 4096x2057 hardware fixture exercises 66,048 thread
groups across two dispatch rows and a partial final tile on both shader variants.

The current-tree feeder build, pixel tests, and existing feeder submission,
color-layout, configuration, and Vulkan present-order regression tests passed.
The D3D12 debug layer was unavailable on this machine; pixel readback and device
completion were checked, and the test enables debug validation when installed.

Initial blend deployment: `build/dlss5-feed.addon64`. On 2026-10-07, that add-on was deployed to
`C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III` in both
`binaries/dlss-active` and `binaries/dlss-payload`, so reinstalling a profile uses
the new feeder too. Both copies match SHA-256
`3519b02239d168737091db521e8c03efd580466b3e7689c26b650d46fc0527aa`.
The active runtime receipt records this build. The DLSS 5 Extended profile and its
existing portrait settings (`portrait_mode=1`, feather 16, budget 400) were retained.
The launcher's graphics, package, runtime, and Vulkan-renderer validation passed.
The repository's release payload was not updated.

Previous add-ons, runtime receipt, and portrait configuration are backed up under
`binaries/dlss-backups/frontier2-20261007T065736Z-3519e8bf` in the game installation.

To test, double-click `Launch CK3 with DLSS.cmd` in that CK3 folder and open a
character window. The new blend logs `portrait blend ready (..., edge jobs, ...)`
in `binaries/dlss-active/dlss5-feed.log`; its message identifies either the in-place
UAV path or the SRV copy fallback. Press F6 to toggle neural rendering for comparison.

## Live inspection on 2026-10-07

Inspected the running CK3 process through its existing ReShade/feeder diagnostics,
read its loaded rendering modules, sampled GPU/process telemetry, and captured
the foreground game client without changing focus or sending game input. The
user requested inspection of the current scene. The captured view showed a paused
map with a character panel and ruler portrait badges on the map.

The loaded feeder is the deployed build, and live logs confirm the BGRA8 output
uses `Aquarius in-place UAV: no atlas copy`. The trace recorded no feeder shader,
pipeline, evaluation, or overlapping-block errors.

| Observation | Recorded value |
|---|---:|
| Trace duration / GPU samples | 73.38 seconds / 24 samples |
| Atlas layout changes during trace | 109 |
| Feature rebuilds during trace | 9 |
| Median / maximum build-to-blend-ready interval | 238 / 327 ms |
| Largest atlas in trace | 704x896 = 630,784 pixels |
| Portrait block pixels in that atlas | 377,216 pixels (59.8% occupancy) |
| Largest atlas in the session snapshot | 704x1024 = 720,896 pixels |
| Portrait block pixels in that atlas | 390,400 pixels (54.2% occupancy) |
| GPU utilization, mean / peak | 83.7% / 100% |
| GPU memory use, peak | 7,393 MiB of 12,288 MiB |
| GPU temperature, peak / clock range | 60 C / 1920-1927 MHz |

At the time of inspection, the pixel budget limited the sum of selected portrait rectangles,
**not the allocated atlas area**. The session's largest atlas was 80.2% above
the configured 400,000-pixel budget even though its blocks fit that budget.
ReShade's NR creation log confirms that NR receives the full atlas dimensions.
Packing waste therefore remains real neural-rendering work after the edge blend
optimization.

That atlas builder also invalidated history whenever `SameAtlasLayout` changed,
including screen coordinates, and atlas dimension changes rebuild the DLSS/NR
feature. Live logs show a small portrait block moving across the screen while
other blocks retain their positions. Combined with the visible ruler badges,
this suggests that moving map portraits contribute to layout churn; identifying
individual source textures would be needed to confirm which draw each block is.

Stable earlier windows in this session reported 58.3-62.0 fps with 9.10-9.78 ms
of feeder wall time. The capture itself included three 600-frame summaries,
reporting 13.1, 37.5, and 35.7 fps. These are observational, mixed-scene samples:
the feeder's wall time includes GPU waits, and its interval can span gaps when
portrait evaluation is skipped. They do not constitute a controlled comparison
with Frontier 1 or an independent measurement of every game present. The logged
build-to-ready intervals likewise include allocation, feature creation, and
other intervening work; they are not isolated GPU timer measurements.

Those observations motivated persistent atlas placement with a limit on the
actual atlas area, implemented below. The separate alpha-mask fix remains
necessary for portrait background edges.

Capture artifacts are under
`build/frontier2-live-20261007T070339Z/`: `game-scene.png`, `telemetry.csv`,
`loaded-modules.json`, the start/end feeder snapshots, `feeder-trace.log`,
`reshade-end.log`, configuration, and `analysis.json`. These local artifacts
are ignored by Git. The capture changed no running-game configuration.
After capture, CK3 logged `Quit: Quit from inside game` and the feeder logged
normal runtime/device destruction. The session was closed from the game.

## Persistent atlas allocation

`src/feed_portrait_atlas.h` replaces shelf packing with guarded MaxRects packing.
`portrait_budget=400` now limits **width times height to 400,000 pixels**, including
alignment, guards and packing holes. It cannot silently allocate a 720,896-pixel
NR input. Portraits that cannot fit keep their original game rendering.

- Surviving crops reserve their old atlas addresses before new crops are placed.
  A fresh packing is used when it improves portrait coverage. Large portraits
  have priority; the remaining space favors covered pixels, then portrait count.
- The initial compact canvas follows the requested crop area, with packing
  allowance capped at 75% of the configured budget. One small portrait therefore
  uses a small canvas. The allocator can grow within the hard ceiling when more
  portraits need space, preferring expansion that retains current addresses.
- Changing a full canvas's aspect ratio requires admitting a missing largest
  portrait or one occupying at least a quarter of the pixel budget. Small moving
  map heads cannot demand a feature rebuild to gain a different canvas shape.
- A smaller canvas must retain the same or better portrait coverage for 180
  consecutive portrait updates before shrinking. Empty scenes skip evaluation.
- Removing a slot preserves surviving histories when their crops and addresses
  are unchanged. New crops, changed source coordinates, moved slots and changed
  dimensions still request a global reset. Source texture identity is unavailable,
  so screen translation cannot safely retain a portrait's history.
- Vulkan clears input color and optional mask each frame, with a transfer write
  dependency before region copies. Guards and retired slots are zero instead of
  stale faces. Paste-back still touches only accepted portrait rectangles.

Recorded geometry replay at the existing 400,000-pixel budget:

| Measurement | Session: captured / new | 73-second trace: captured / new |
|---|---:|---:|
| Recorded layouts | 405 | 109 |
| Peak allocated pixels | 720,896 / 399,360 | 630,784 / 399,360 |
| Mean allocated pixels per recorded layout | 534,927 / 399,087 | 482,839 / 399,304 |
| Dimension changes, excluding initial allocation | 64 / 11 | 9 / 1 |
| Selected portrait slots | 2,849 / 2,002 | 676 / 510 |
| Selected crop pixels | 114,738,944 / 97,145,600 | 28,225,920 / 25,235,840 |
| New allocator's history resets, excluding initial allocation | 274 | 75 |

The session replay reduces mean allocated area by **25.4%**, peak area by **44.6%**,
and size changes by **82.8%**. It processes **84.7% of the original crop pixels**
(70.3% of slots); the stricter physical budget and stable geometry leave some
smaller portraits at native quality. A larger `portrait_budget` trades more NR
work for greater coverage. Dense and sparse scenes can make different tradeoffs;
holding a previous large canvas through the shrink delay can cost more than the
old tight canvas temporarily.

The tracked fixtures contain processed crops from the actual logs, with no
character data. Replay calls the same allocation stage after crop preparation.
It has one update per recorded layout, excludes unlogged unchanged frames, and
does not simulate draw detection, NGX, feature creation time or GPU evaluation.
Means are weighted by recorded layouts, not elapsed time. Sticky widget cropping
and the 180-update delay are exercised separately in regression tests. These
results establish geometry and resource bounds; a new in-game frame-time and
visual comparison is still needed.

Validation passed:

- 2,000 deterministic randomized scenes with varied budgets, clipping and sizes;
  focused cases for draw-order changes, history, removal/addition, growth, delayed
  shrink, budget reduction, sparse scenes and aspect-ratio stability.
- The actual allocator's dense and retired-slot layouts through the shipped GPU
  blend on WARP and RTX 3060, in three formats and both shader variants, checked
  against the independent pixel oracle.
- Vulkan RGBA8 color and R8 mask readback through the shipped clear/copy helpers,
  checking every pixel in active crops, guards and retired slots, including frames
  without a mask.
- Current-tree add-on build and existing feeder/configuration/present-order checks.

The replay harness took about 0.02 ms per layout when run alone, including parsing
and validation. D3D12 debug validation remains unavailable; GPU readback and
device completion passed. This is a complete implementation of the atlas change;
the separate portrait alpha/depth work is still outstanding.

## Current CK3 deployment

The combined blend/atlas build `ck3-frontier2-atlas.1` was installed on 2026-10-07
at 07:46 UTC while CK3 was closed. Both `binaries/dlss-active/dlss5-feed.addon64`
and `binaries/dlss-payload/dlss5-feed.addon64` match the tested local build's SHA-256:
`c34f6abf42e807258ad116ff56617f374159487c5114790b5c511450cf9d9ed3`.
The runtime receipt records the build and hash. The DLSS 5 Extended profile and
portrait configuration remain at feather 16 and budget 400.

The previous add-ons, runtime receipt and configuration have hash-verified backups
under `binaries/dlss-backups/frontier2-atlas-20261007T074619Z-3519b022`; that directory
also contains the deployment manifest. Graphics dependencies, package, runtime
and Vulkan renderer validation passed. The packaged release in this checkout is
unchanged; the game's local payload contains the new add-on.

Use `Launch CK3 with DLSS.cmd` in the game installation. The new feeder's startup
line identifies `ck3-frontier2-atlas.1`. Layout changes also log
`atlas allocation: actual/budget pixels, retained slot(s), history reset/retained`
in `binaries/dlss-active/dlss5-feed.log`. No new in-game FPS result is claimed for
this build yet.

## Remaining milestone work

The next agent's priorities, implementation hazards, verification and deployment
steps are in [the Frontier 3 handoff](frontier3.md).

This optimization preserves the existing feather appearance. Eliminating the
portrait background box still needs an alpha-aware mask captured from CK3's
portrait texture. Persistent slots and a hard allocation ceiling are now implemented.
Portrait depth and in-game visual and frame-time verification remain as described in
[the Frontier 1 issue list](frontier.md#known-issues-carried-into-frontier-2).
