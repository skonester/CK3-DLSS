# FSR 3.1 on the 5700G iGPU alongside CK3 DLSS 5

Updated 2026-11-06. The current result is the next section. Everything after it is the earlier investigation, whose whole-frame bridge now survives only as the Vega mode.

## Current result: FSR before the UI, in-process

`layer/ck3_fsr_layer.cpp` (`VK_LAYER_ck3_fsr`) runs FSR 3.1.3 on the game's device and command buffer. It is loaded only for measurement launches. Same save and camera, 1920x1080 window, DLSS 5 portraits on, game input frozen, 30 seconds, paused map ([comparison](binaries/fsr-measurements/comparison-20261106-104707.json) in the game folder):

| Mode | Median FPS | Median ms | p95 ms | p99 ms |
| --- | --- | --- | --- | --- |
| Native 1080p | 46.3 | 21.59 | 24.20 | 27.88 |
| 720p, game's bilinear upscale | 63.8 | 15.68 | 17.93 | 20.94 |
| 720p + FSR 3.1.3, in-process | 59.7 | 16.76 | 17.94 | 19.35 |
| Vega, old whole-frame bridge (one earlier run, view not verified) | 18.5 | 54.03 | 77.96 | 292.71 |

FSR's GPU time is 0.91 ms on the RTX 3060 and 7.80 ms on Vega, so moving it to Vega could save at most 0.91 ms of NVIDIA time. Offloading cannot pay for itself on this machine.

What CK3's frame looks like at `render_scale<1`, from the census: scene into render-size colour + D24S8, bloom/exposure, tonemap into a render-size BGRA8 target, then a first swapchain pass (`LOAD`) holding one fullscreen upscale draw followed by the GUI at native resolution, then a second swapchain pass with more GUI and portraits. All draws use dynamic viewports and the frame records into one command buffer. The layer dispatches FSR just before that first swapchain pass, jitters the render-size depth-tested passes through their viewports, copies the result to the swapchain and skips the game's upscale draw.

Facts that would otherwise be rediscovered:

- CK3 has `upscale`/`upscale_quality` settings and `dlss`/`fsr2`/`xess` names, but ships no backend. `upscale="fsr2"` is rewritten to `DISABLED` on load and the frame does not change.
- The pinned FSR provider ignores both non-linear colour flags, so input must be linear. Same-class copies into sRGB images plus blits do the conversion without a shader.
- The game reads the mouse with its window disabled and in the background, so unattended runs drop input in its window procedure (`CK3_FSR_FREEZE_INPUT`). It still polls the cursor position, so a tooltip can appear under a stationary mouse.
- Measurement sessions turn autosave off and load a private copy of `binaries/fsr-measurements/scene.ck3`, so they never write to the player's saves. Delete that file to take a new scene from the current last save.

### Portraits and motion vectors

The engine renders portraits at render scale too, so at 720p DLSS 5 and the GUI get them at 2/3 size (atlas 384x352 against 576x512 at native). That is the soft look, with or without FSR. `-PortraitScale` sets the size of the portrait targets in FSR mode, defaulting to 1.5. Same scene with only the ruler portrait, 30 seconds:

| Portrait scale | Atlas | Median FPS | Median ms | Look |
| --- | --- | --- | --- | --- |
| 1.0 | 384x352 | 59.7 | 16.76 | soft |
| 1.25 | 512x416 | 54.6 | 18.32 | in between |
| 1.5 | 576x512 | 50.5 | 19.79 | matches native |

The cost comes from larger portraits, in rendering and DLSS 5 together; the two have not been measured separately.

Event windows and the barbershop put many or large portraits on screen, and the feeder's `portrait_budget` caps the atlas in pixels. At 1.5x with the old 400k budget, typically 2 of 13-19 portraits were enhanced, and the rest fell back to the game's originals. FSR mode now scales the budget with the square of the portrait scale (`-PortraitBudget` overrides it). At 1.5x and 900k, a short play session reached 6 of 14 to 13 of 13 portraits at 26-31 fps while those windows were open, compared with 44-46 fps before. A 1024x586 portrait can still crowd out the small ones. A play session at 1.25x and 625k froze the game when a portrait was opened; the cause has not been found, so the default went back to 1.5x and 900k. Input-frozen runs can't fire events, so judge these windows from a session someone plays.

The game-upscale mode now defaults to render scale 0.7 and a 700k budget. At 400k, the ruler portrait dropped out of the atlas whenever an event-sized set of portraits appeared, which reads as DLSS 5 switching off until the event closes. Frame time stayed high through those windows, so the neural pass itself was still running. The 700k budget has not been tested in play yet.

That mode also replaces the game's bilinear upscale with FSR 1 EASU + RCAS and sharpens each portrait with RCAS before DLSS 5 reads it (`-NoSpatial` turns both off). It uses AMD's MIT-licensed `layer/ffx_fsr1/` headers and this repo's own shaders, with no OptiScaler GPL code. As of 1.0.7 this has only been compiled, never run in the game.

Facts for the scaling code: a portrait is identified by its multisampled images, since nothing else in the frame is multisampled. The final R8G8B8A8 target is created before those, so it gets a shadow image that framebuffers, descriptors and barriers are redirected to. Making the pipelines' baked scissor dynamic was also required; without it the portrait was clipped at the old size and came out empty, and DLSS 5 turned that blank input into a grey stone texture.

Motion vectors come from the `PdxCamera` constant buffer (set 0, binding 1; near 10, far 100000, FOV 60 degrees) through `layer/ck3_fsr_motion.comp`. The matrices are stored column-major. Moving objects such as units and ships get camera motion only. Mid-pan frames show less smearing with the vectors than with zero motion. This rests on one frame from each of two runs at different map positions, and panning has only been tested with a held key.

Not done yet: zoom and animated objects have not been tested in motion. Portrait-open and zoomed-in scenes have not been measured. Mip bias is not adjusted.

## Earlier finding: serial offload

Written 2026-11-05 about the synthetic benchmark and the whole-frame bridge.

**Implementation decision: keep FSR on the RTX 3060 for this machine.** Actual FSR 3.1.3 works on Vega, but the measured serial offload route is substantially slower. At 1280x720 → 1920x1080, direct NVIDIA FSR took **1.30 ms** median wall time; Vega FSR alone took **7.69 ms**; the complete NVIDIA → Vega FSR → NVIDIA route took **19.00 ms**, about **14.6x** the direct time.

The workspace now contains a standalone actual FSR benchmark and cross-GPU buffer/image transport. The CK3 package still has no integrated split between NVIDIA DLSS 5 and AMD FSR, and OptiScaler does not automatically provide that split. The current measurements do not justify implementing it as a CK3 performance feature. They establish the cost of this synthetic serial route, not CK3 FPS or the performance of a future overlapping/shared-memory design.

## Hardware checked on this machine

Windows reports an AMD Ryzen 7 5700G, AMD Radeon(TM) Graphics, and NVIDIA GeForce RTX 3060. `vulkaninfo --summary` and `vulkaninfo --text` both enumerate the AMD and NVIDIA devices.

| Capability | NVIDIA RTX 3060 | AMD integrated graphics |
| --- | --- | --- |
| Vulkan API | 1.4.341 | 1.3.217 |
| Driver reported by Vulkan | 610.88 | 22.20.27.09 |
| Subgroup size | 32 | 64 |
| `shaderFloat16` / `shaderInt16` | true / true | true / true |
| `storageBuffer16BitAccess` | true | true |
| `timelineSemaphore` | true | true |
| Win32 external memory and semaphore extensions | present | present |
| Valid device LUID | yes | yes |

These are advertised capabilities. The later actual benchmark separately proves FSR 3.1.3 context creation, dispatch and synthetic output checks on both GPUs, plus host-staged image transport. Cross-vendor shared-image import and acceptable game frame times remain unproven. The loader also reports installed overlay layers; the snapshot is the existing machine environment.

The full local snapshot is `build/fsr-offload-vulkaninfo.txt` (ignored build output).

## What the OptiScaler source actually does

Source folder: `C:/Users/admin/Desktop/OptiScaler-master/OptiScaler-master`.

- `README.md` describes replacing existing DLSS/FSR/XeSS integrations. It lists FSR 3.1 as a Vulkan output. It is not a generic second-GPU scheduler.
- `OptiScaler/upscalers/ffx/FFXFeature_Vk.cpp` passes the existing `Device` and `PhysicalDevice` into `ffxCreateBackendVKDesc`. It does not select the AMD iGPU independently.
- `OptiScaler/upscalers/IFeature_VkwDx12.cpp`, `VulkanDeviceLuid()` and `CreateDx12Device()`, deliberately match the D3D12 adapter to the Vulkan adapter. The source explicitly says: "Shared resources only work on the Vulkan device's GPU". This describes its current sharing path, not a universal prohibition on custom multi-GPU systems.
- That interop path creates ordinary shared D3D12 textures. It does not implement cross-adapter staging and scheduling.
- The OptiScaler source archive has empty FidelityFX SDK submodule directories. The probe now separately installs a pinned official FSR Vulkan runtime under its own ignored `runtime/fsr31/` directory. This does not complete OptiScaler's SDK submodules or change its build.

Changing an adapter index alone would leave textures and command buffers owned by the other GPU. The resources must be transferred and recreated in the receiving device's context.

## What the CK3 feeder provides, and what is missing

- `src/dlss5-feed.cpp`, `InitSessionVk()`, creates a private D3D12 device with a null/default adapter. It does not explicitly select AMD for a second processing stage.
- `MakeSharedTexVk()` uses `D3D12_HEAP_FLAG_SHARED`, an unspecified driver texture layout, and ordinary shared fences. `src/feed_vk.h` imports these textures into Vulkan with optimal tiling. This is the existing cross-API transport; it is not a cross-vendor transport implementation.
- `CreateFeature()` configures native-size DLAA. The evaluate paths supply zero jitter. The current code does not establish a lower-resolution game scene and a larger FSR output.
- `shaders/DLSS5_Feed.fx` provides depth and estimated motion from providers such as VORT. Those are not demonstrated engine motion vectors or engine camera jitter. The portrait path intentionally uses simplified guides.
- The released DLSS 5 profile enhances portrait textures, then lets CK3 composite them through its own UI. `frontier4milestone.md` records approximately 42 fps, but explicitly says that this was not a controlled benchmark. It does not measure a potential FSR saving.
- The package already owns `dxgi.dll`, Vulkan interception, and the NGX/RenoDX call sequence. Installing OptiScaler alongside it as another injector is not an established compatible configuration. Replacing the NGX backend also cannot be assumed to preserve the DLSS 5 neural-rendering hook.

[AMD's FSR integration guide](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/) documents color, depth, motion-vector inputs and camera jitter. A useful CK3 integration would need to establish these inputs and the scene/UI processing order. Processing the completed screen instead would also process text and UI. FSR 3.1 upscaling is separate from FSR frame generation; this assessment does not validate an iGPU frame-generation path.

## A viable experimental design

The standalone prototype keeps source images on NVIDIA and creates a separately selected AMD device with its own queue, FSR context, images and history. It moves the inputs to AMD, runs FSR, and returns the output image to NVIDIA. CK3 rendering and portrait DLSS 5 have not been modified.

Two transport approaches are relevant:

1. **Independent Vulkan devices with host-visible staging.** Both buffers and actual synthetic FSR images now pass this route. CK3 images and temporal integration remain unimplemented. Device-local images on each GPU and staging copies add transfers and synchronization. Shared Win32 extension support alone does not establish that one driver's optimal image memory can be imported by the other.
2. **A D3D12 cross-adapter staging bridge.** Test cross-adapter shared buffers or supported row-major textures between NVIDIA and AMD D3D12 devices, retaining same-adapter Vulkan/D3D12 interop where needed. This has more components. [Microsoft documents](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps) special heap/resource flags, system-memory placement, layout restrictions, and explicit cross-adapter synchronization. These facilities do not make the current shared images portable automatically.

Do not assume the GPUs can overlap dependent stages of the same frame. Pipelining across frames requires careful input/history matching and may add latency. FSR can also run on NVIDIA; AMD's vendor identity is not itself a reason to move it off NVIDIA.

For scale only: 1280x720 input with RGBA16F color, R32F depth and RG16F motion, plus 1920x1080 RGBA16F output, represents 31,334,400 bytes (29.9 MiB) of input/output payload per frame, or about 1.88 GB/s at 60 fps. This excludes masks, alignment, redundant staging copies and synchronization. It is arithmetic, not measured bandwidth or latency.

## Experiment status and acceptance criteria

1. **Completed:** pinned official FSR 3.1.3 context creation and dispatch on both adapters, with analytic synthetic inputs, output checks, GPU timestamps and wall timings.
2. **Completed:** buffer transport and actual color/depth/motion/output image transport, including packing/copies, waits and full-byte return validation.
3. **Completed; performance criterion failed:** the complete offload route is slower than identical FSR processing on NVIDIA alone at all three tested output resolutions. These results do not justify a performance feature. The user subsequently requested the diagnostic CK3 experiment described below.
4. **Native engine integration not attempted:** CK3's engine temporal inputs and native UI composition remain outside this experiment. The separate whole-frame measurement bridge preserves the portrait feeder and uses estimated motion instead.
5. **Matched in-game benchmarks not attempted:** initial live NVIDIA smoke captures are recorded below. Offloading FSR does not by itself reduce the portrait neural-rendering workload or solve a CPU simulation bottleneck.

The initial capability investigation changed no game installation, driver setting, packaged DLL, or production source. The later opt-in live experiment below temporarily changes game settings and loads an independent measurement add-on, with a restore receipt.

## Follow-up: basic Flet preflight app

Launch `Run Vega FSR Preflight.cmd` from the repository root. The implementation and instructions are in [tools/vega-fsr-probe](tools/vega-fsr-probe/README.md).

The app queries the Vulkan driver directly and offers generic GPU checks plus **Benchmark FSR 3.1**. The benchmark uses a separately installed, pinned official runtime and promotes only the tested FSR 3.1.3 row after matching-adapter execution and validated output. JSON/Markdown reports and preview PNGs remain local.

### Recorded findings from the user's report

Source: [vega-fsr-20261105-224910-426850.json](tools/vega-fsr-probe/reports/vega-fsr-20261105-224910-426850.json), captured **2026-11-05 at 22:49:02 CST** (`2026-11-06T04:49:02.866730+00:00`). This records the earlier generic buffer baseline. The actual FSR measurements below determine the current implementation decision. Raw reports are local, ignored diagnostic artifacts; the findings are preserved here.

- **Separate AMD device: passed.** A logical Vulkan device and compute queue were created on AMD Radeon(TM) Graphics, PCI `1002:1638`, rather than on the RTX 3060.
- **Compute correctness: passed.** All **65,536 uint32 results** matched the CPU oracle. Submission wall time was **0.1587 ms**; this includes submission/wait overhead and is not an FSR execution time.
- **Round-trip correctness: passed.** RTX 3060 → Vega → RTX 3060 transfers returned every byte correctly across changing sample patterns.
- **Preflight checks: all 11 present**, including the five required checks. Vega exposes Vulkan **1.3.217**, driver **22.20.27.09**, wave size **64**, FP16 arithmetic, 16-bit storage, extended storage-image formats, timeline semaphores and a valid LUID.
- **Image formats:** RGBA8 UNORM, RGBA16 UNORM, RGBA16 float, RG16 float and R32 float all advertise sampled-image, storage-image, linear-filter and transfer support. These were queried, not exercised by the buffer test.
- **Compute limits:** **64 KiB** shared memory, **1,024** invocations per workgroup, maximum workgroup dimensions **1,024 × 1,024 × 1,024** subject to that invocation limit, and maximum dispatch counts **65,535 × 65,535 × 65,535**.

| Serial host-staged round trip | Measured result |
| --- | --- |
| Payload | 16 MiB each direction; 32 MiB total |
| Warmups / measured samples | 3 / 20 |
| Median | **7.9868 ms** |
| p95 / p99 | **9.1801 / 9.2617 ms** |
| Minimum / maximum | 7.6338 / 9.2821 ms |
| Effective aggregate payload rate | 4.2012 GB/s; not a PCIe bandwidth measurement |
| Target / whole-frame budget | 60 FPS / 16.6667 ms |
| Transfer share of budget | **47.92% median; 55.08% at p95** |
| Readback memory on both GPUs | `0x0E`: host-visible, host-coherent, host-cached |

Timing includes four GPU copies/submissions, two CPU staging copies, cache maintenance and waits. Initial seeding and final validation are excluded. This measures serial buffers, with CPU/Python scheduling overhead; it excludes FSR, image packing, CK3 integration, external shared-handle imports and pipelined overlap.

The enabled AMD device also returned entry points for `vkCmdDispatch`, `vkCmdCopyBuffer`, `vkCreateComputePipelines`, `vkGetMemoryWin32HandleKHR`, `vkGetMemoryWin32HandlePropertiesKHR`, `vkImportSemaphoreWin32HandleKHR` and `vkGetSemaphoreWin32HandleKHR`. Availability of the Win32 entry points does not establish cross-vendor shared-handle interoperability.

| FSR assessment recorded by the probe | Status |
| --- | --- |
| FSR 1 spatial upscaling | Hardware candidate; runtime untested |
| FSR 2 temporal upscaling | Hardware candidate; runtime untested |
| FSR 3.1 upscaling | Hardware candidate; runtime untested |
| FSR 3.1 frame generation | Outside the published hardware range assessed by the probe; untested |
| FSR 4 ML upscaling | No official Vega support in the probe's referenced requirements; untested |

This earlier report explicitly records `fsr_runtime_executed=false`: it justified the isolated actual FSR comparison below, rather than proving an offload speedup.

## Actual FSR 3.1.3 comparison and decision

Primary source: [fsr-1080p-comparison.json](tools/vega-fsr-probe/reports/fsr-1080p-comparison.json), captured **2026-11-05 at 23:08:59 CST**. This report records `fsr_runtime_executed=true` and passed all three paths. The official signed Vulkan DLL is from [AMD SDK v1.1.3, pinned commit](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/54fbaafdc34716811751bea5032700e78f5a0f33). The **3.1.3 provider is selected explicitly**, with no FSR 2/4 fallback. Runtime SHA-256: `04a455bb7c01af1adc630f3be41bd85689b6a8565199e25b09696240750f6d1e`.

The primary workload is 1280x720 input → 1920x1080 output, **20 measured samples after five warmups per path**, automatic exposure enabled and sharpening disabled. Inputs are a jittered static analytic gradient/checker plane, constant depth and zero unjittered motion. Input scene preparation/upload and output validation are outside timing. Context/shader creation is excluded and reported separately.

| Path | Median wall time | p95 wall time | p99 wall time | Median FSR GPU time |
| --- | --- | --- | --- | --- |
| RTX 3060 FSR alone | **1.3006 ms** | 1.4165 ms | 1.4204 ms | **0.8052 ms** |
| Vega FSR alone | **7.6945 ms** | 7.9869 ms | 8.0572 ms | **7.2296 ms** |
| Full NVIDIA → Vega FSR → NVIDIA path | **18.9956 ms** | 22.5829 ms | 47.9935 ms | 7.9707 ms (FSR portion only) |

Wall timings include CPU/Python recording, submissions and waits. The complete offload measurement additionally includes packing color/depth/motion images, host-staged transfers, AMD FSR, and uploading the returned output image onto NVIDIA. Actual payloads are **14,745,600 input bytes** and **16,588,800 output bytes**. This is an image benchmark, not an estimate formed by adding the earlier generic buffer timing to FSR timing.

A [desktop-app verification rerun](tools/vega-fsr-probe/reports/vega-fsr-20261105-231754-994585.json) measured **1.46 ms** direct versus **17.45 ms** full-offload median, again passing all output checks. The variation does not change the result: offloading was about **11.9x** slower in that run.

**Output validation passed:** all output values were finite; red/green analytic gradients passed their checks; cross-GPU RGB mean absolute error was **0.000395**, with p99 absolute error **0.001953** (tolerances 0.005 / 0.025). NVIDIA and AMD output need not be bit-identical, but **every byte of every AMD output returned to NVIDIA matched**. [NVIDIA preview](tools/vega-fsr-probe/reports/fsr-1080p-comparison/previews/nvidia-fsr.png) and [offloaded preview](tools/vega-fsr-probe/reports/fsr-1080p-comparison/previews/offloaded-fsr.png) are sRGB display conversions; validation uses float16 output.

Additional preset validation used ten measured samples after five warmups per path:

| Output | Direct rendering-GPU FSR median | Full offload median | Offload/direct ratio |
| --- | --- | --- | --- |
| 720p (854x480 → 1280x720) | 0.8251 ms | 7.9660 ms | 9.65x |
| 1440p (1706x960 → 2560x1440) | 1.9050 ms | 32.2942 ms | 16.95x |

Sources: [720p validation](tools/vega-fsr-probe/reports/fsr-720p-validation.json), [1440p validation](tools/vega-fsr-probe/reports/fsr-1440p-validation.json). Both passed dispatch, analytic output, cross-GPU tolerance and returned-image checks.

**Conclusion:** the tested 1080p serial offload route adds **17.6951 ms** median versus direct NVIDIA FSR, making that stage **14.61x slower**. Even Vega's isolated FSR GPU work is about nine times the RTX's. The offload median alone exceeds a 60 FPS frame's 16.67 ms budget before the game's existing work is counted. Keep the benchmark available, but do not implement this route as an optional CK3 performance feature on the tested 5700G/RTX 3060 system.

These are short synthetic runs in the current desktop environment; the p99 from twenty samples is sensitive to individual stalls. No CK3 FPS, concurrent game/DLSS workload, temporal game image quality, peer-to-peer/shared-image path, or pipelined overlap was measured. A substantially different design would need its own measurements; these results support rejecting the measured serial implementation.

## Live CK3 measurement prototype and the 2 FPS regression

The user requested a whole-frame experiment, including the interface. The separate `ck3-fsr-measure.addon64` now reads CK3's final composite and the existing feeder's depth/estimated motion, resamples them to 1280x720, dispatches the pinned FSR 3.1.3 provider on the selected GPU, and copies its 1920x1080 result back to CK3. The portrait feeder remains separate. This is a serial host-staged diagnostic, with no native engine jitter or engine motion vectors; it is not a production temporal-upscaling integration or a frame-generation feature.

CK3 is configured for a **1920x1080 window with `render_scale=0.666667`**. This requests a 720p scene inside a 1080p composite. The final interface/composite is also downsampled to 720p for FSR, so UI text can soften and CK3's own scaling remains in the path. Frame logs verify the FSR input/output sizes; they do not independently prove the dimensions of every internal engine render target.

The initial live **Dedicated NVIDIA** run caused the reported approximately 2 FPS experience. Profiling reproduced **212 ms** of CPU color conversion per input/output pair: about 75 ms for the input and 136 ms for the output. The old worker repeatedly evaluated sRGB powers, float conversions and full-image NumPy allocations on the CPU, while the game waited synchronously. This was an avoidable prototype regression, not evidence that the FSR shaders themselves require hundreds of milliseconds or that Vega was selected.

The fix uses `ck3-fsr-convert.dll`: precomputed sRGB/half-float lookup tables and native loops write directly between the shared packet and Vulkan's mapped buffers. A local twenty-sample check measured **6.06 ms median** for the two conversions together. Native output matches the original float formulas, including every finite half-float pattern; nonfinite depth, motion and RGB output still fail validation. There is no silent Python conversion fallback in the live path.

A subsequent five-second live smoke capture saved **107 verified FSR frames**:

| Fixed NVIDIA run | Measured result |
| --- | --- |
| Median present interval / reciprocal FPS | **44.10 ms / 22.67 FPS** |
| p95 / p99 present interval | 56.30 / 93.96 ms |
| Complete serial FSR bridge median | **27.38 ms** |
| Python worker median, including conversion/copies/submission | **10.10 ms** |
| FSR GPU timestamp median | **0.65 ms** |
| Bridge readback and queue wait median | **15.25 ms** |
| Verified FSR input / output | 1280x720 / 1920x1080 |
| Rendering / processing GPU | RTX 3060 / RTX 3060 |

Source: [capture](tools/vega-fsr-probe/reports/ck3-cpu-conversion-20261106/measurement-20261106-000125.json), [diagnosis and older run summary](tools/vega-fsr-probe/reports/ck3-cpu-conversion-20261106/diagnosis.json), [bridge frames](tools/vega-fsr-probe/reports/ck3-cpu-conversion-20261106/after-frames.csv), [worker stage timings](tools/vega-fsr-probe/reports/ck3-cpu-conversion-20261106/after-worker-timings.csv). Raw diagnostic artifacts remain local and ignored by Git. The capture occurred at 00:01 CST on November 6, 2026 (`06:01Z`). Component medians need not sum to the median total, and readback waiting can include preceding game GPU work.

These are **uncontrolled smoke captures**, with changing camera/portrait state and desktop activity. They diagnose the severe CPU conversion stall and verify live delivery; they are not a matched gameplay speedup measurement. The full bridge still consumes substantial frame time even on NVIDIA. A useful performance feature needs GPU-side conversion and an in-process NVIDIA path that retains images on the GPU. A separate Vega route still needs its own live comparison; its earlier synthetic result remains unfavorable.

## Running the measurement controls

Use `Run CK3 FSR Measurement.cmd` in this checkout to open the Kotlin app with the local measurement bundle selected. Under the active **DLSS 5 Extended** profile, **Run comparison** measures Native, 720p and FSR unattended on the same scene and restores settings afterwards; the frames and JSON land under **Reports**. To look at one mode yourself, pick it and press **Launch mode**. Normal launch and profile changes are disabled while measurement settings are pending restoration.

From PowerShell: `CK3-FSR-Measurement.ps1 -Action Compare -GameRoot <CK3 folder>`, with `-Modes Native,Off,Dedicated,Integrated` to choose modes. A row with `ViewChanged` true is not comparable.

Reports are saved under `binaries/fsr-measurements/<session>/` in the game directory. `frames.csv` includes readback, worker wait, CPU copies and return-upload timing; `worker-timings.csv` separates conversion, submission/wait and FSR GPU timestamps. Capture rejects missing FSR frames and wrong output dimensions. An optional portrait-log snapshot being locked by CK3 no longer invalidates the saved core report.

After closing CK3, press **Restore settings**. The script restores only the graphics/portrait keys it changed and the prior measurement add-on state; it preserves unrelated preferences and reports. The same actions are available in [CK3-FSR-Measurement.ps1](ck3-package/CK3-FSR-Measurement.ps1) using `-Action Start/Capture/Restore`, `-Mode Off/Dedicated/Integrated`, and `-GameRoot <CK3 folder>`.

To rebuild locally, run `build-ck3-fsr-measure.cmd` and `Stage-CK3-FsrMeasurement.ps1`; the latter requires the existing probe Python environment and pinned official runtime. The staged bundle references that local Python executable and is not a portable public installer. Rebuild the Kotlin app with `Build-Installer-GUI.ps1` after UI/backend changes. Validation for this fix passed 31 Python tests, 16 Kotlin backend tests, the native builds and the settings round-trip checks.
