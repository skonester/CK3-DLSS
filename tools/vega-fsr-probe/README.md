# Vega FSR Preflight

A Windows Python/Flet diagnostic and optional real FSR 3.1.3 benchmark for a separate stage on the Ryzen 7 5700G's integrated Vega GPU. It leaves CK3, OptiScaler, drivers and system settings untouched.

## Run

Double-click **`run.cmd`** in this folder, or `Run Vega FSR Preflight.cmd` in the repository root. It creates a local `.venv` and installs pinned Flet/NumPy dependencies if needed. Python 3.11+ **64-bit** and a Vulkan-capable Windows GPU driver are required. The first desktop launch may also download the matching Flet desktop client from Flet's GitHub release. Installed scans/tests work offline.

1. The app scans Vulkan hardware automatically.
2. Check that **Processing GPU** is AMD Radeon Graphics and the transfer partner is the NVIDIA GPU. The app matches GPU UUIDs, not a fragile enumeration index. It does not silently replace a missing AMD target with NVIDIA.
3. Press **Run GPU checks**. The default payload is 16 MiB in each direction, with three warmups and 20 measured samples. Select **No transfer partner** for a compute-only check.
4. Press **Export report** to save JSON and Markdown under `reports/`.

## Actual FSR 3.1 benchmark

The optional runtime is already installed in this workspace. For a fresh checkout, run:

```powershell
.\Setup-FSRRuntime.ps1
```

This downloads AMD's official signed Vulkan DLL from [SDK v1.1.3, pinned commit](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/54fbaafdc34716811751bea5032700e78f5a0f33), verifies its SHA-256, and retains the MIT license. Runtime files stay under ignored `runtime/fsr31/`; they are not shipped in this repository or installed system-wide. `AMD-FSR-LICENSE.txt` accompanies the API declarations.

Press **Benchmark FSR 3.1** in the app. It explicitly selects the runtime's **3.1.3 provider**; it never substitutes FSR 2 or FSR 4. Choose 720p, 1080p or 1440p output. Default 1080p uses 1280x720 input and 1920x1080 output.

Three paths are measured with identical synthetic inputs and five warmups per path:

1. FSR on the rendering GPU, with input images already resident there.
2. FSR on the processing GPU, with input images already resident there.
3. Full serial offload: pack rendering-GPU color/depth/motion images, read back, copy through host staging, upload to the processing GPU, execute FSR, read back output, copy through host staging, and upload the returned image to the rendering GPU.

Both per-stage **wall time** and **FSR GPU timestamps** are recorded. Wall time includes Python/CPU dispatch recording, submissions and waits. Input scene generation/upload is excluded from all paths, modeling images that a game already rendered. The full path includes image packing/copies and FSR itself. The offload table's GPU column measures its FSR portion, not all transfers. At 1080p, the input payload is 14,745,600 bytes (RGBA16F color, R32F depth, RG16F motion), and output is 16,588,800 bytes (RGBA16F).

The synthetic scene is a jittered static analytic gradient/checker plane, constant depth, zero unjittered motion, auto exposure and no sharpening. Context/shader setup is excluded and reported separately. Output checks reject nonfinite/unwritten pixels, check known red/green gradients, compare cross-GPU RGB errors, and verify every byte of every offloaded image returned to the rendering GPU. Preview PNGs are saved under `reports/`; comparisons use the original float16 data, not PNGs.

This tests actual FSR execution, not CK3 temporal quality, concurrent rendering/DLSS workloads, peer-to-peer transfers, or pipelined overlap. The benchmark runs in the isolated worker with a 180-second deadline. Preflight-only workers retain the 90-second deadline. The pinned SDK needs its KHR memory-requirements alias and `VK_EXT_debug_utils`; these are enabled and checked before dispatch.

## What it checks

- Vulkan version, vendor/device identity, driver, all core feature bits and selected extended features.
- Compute queues, wave/subgroup operations, FP16 arithmetic, 16-bit storage and timeline semaphore availability.
- Sampled/storage support for RGBA8, RGBA16 UNORM/float, RG16 float and R32 float.
- Memory types/heaps, compute limits, LUID, Win32 memory/semaphore extensions and enabled-device entry points.
- Actual creation of a separate logical device on the selected adapter.
- Actual compute pipeline creation and dispatch: 65,536 transformed integers are compared byte-for-byte with a CPU oracle. The included small SPIR-V shader is compiled from `shaders/compute_check.comp`.
- An actual rendering-GPU → processing-GPU → rendering-GPU buffer round trip using host staging. Every sample changes its pattern, and every returned byte is checked. Readback memory prefers `HOST_CACHED` where available; noncoherent memory is flushed/invalidated when required.

The feature scan needs no Vulkan SDK or `vulkaninfo.exe`: Python's standard-library `ctypes` talks to the system Vulkan loader. Native driver calls run in a child process with a Cancel button; each GPU fence wait has a five-second timeout. Cancellation stops the worker process; it is not a driver-reset mechanism.

## How to interpret FSR results

**FSR is not a driver capability that can be enumerated by version.** The scan reports conservative hardware candidates. Only the separate actual FSR benchmark can promote the FSR 3.1 row to **FSR 3.1.3 synthetic test passed**, with matching-adapter dispatch and validated-output evidence. It does not promote other versions.

| Scan-only result for the 5700G family | Meaning |
| --- | --- |
| FSR 1 candidate | Basic spatial-upscaler compute/format checks pass; algorithm untested. |
| FSR 2 / FSR 3.1 upscaling candidate | Inspected compute/API/storage-format prerequisites pass; exact shader requirements, FSR context creation, dispatch, temporal inputs and image quality are untested. |
| FSR 3.1 frame generation outside published range | Vega is outside AMD's listed RX 5000-class range. It is separate from FSR upscaling. |
| FSR 4 no official Vega support | The 5700G is absent from the published FSR 4 hardware list. Newer supported Ryzen APUs do not imply Vega support. |

The rules were checked against [AMD's SDK requirements](https://gpuopen.com/amd-fsr-sdk/) and [temporal-upscaler integration guide](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/) on 2026-11-05. The app records its source URLs in each report. Unsupported or unrecognized GPUs are not assigned a Vega-specific FSR 4 verdict.

The transfer result is a **serial host-staged baseline**, including Python/CPU scheduling, four GPU copies/submissions, two CPU staging copies, waits and cache maintenance. Pattern seeding and the final validation copy are outside timing. It does not test optimal-image packing, cross-vendor shared-handle import, peer-to-peer access or a pipelined implementation. Six 16 MiB buffers are allocated across the two devices at the default setting.

At 60 FPS a whole frame is 16.67 ms. A generic transfer result below that budget only establishes available room before adding FSR and the game's existing work; it does not establish a net speedup. The generic compute shader is never presented as FSR. Use the actual benchmark for real FSR timing; the optional runtime is installed separately.

## Command line and development

From this folder:

```powershell
.venv\Scripts\python.exe probe.py --mode scan --output reports\scan.json
.venv\Scripts\python.exe probe.py --mode test --mib 16 --samples 20 --fps 60 --output reports\test.json
.venv\Scripts\python.exe probe.py --mode fsr --resolution 1080p --samples 20 --output reports\fsr-test.json
.venv\Scripts\python.exe -m unittest discover -s tests -v
```

Use `--target <UUID>` and `--source <UUID>` to override adapter selection, or `--source none` to skip transport. The JSON contains actual per-sample timings and memory-selection flags. Process exit code is nonzero for failed tests; a low-performance transfer that is correct is still a passed transport test, with a separate frame-budget verdict.

To rebuild the bundled generic test shader (developer tool only):

```powershell
glslangValidator -V shaders\compute_check.comp -o shaders\compute_check.spv
```

On this machine, the recorded 1080p comparison passed output validation but measured 1.30 ms median for rendering-GPU FSR versus 19.00 ms for the full offload route. See `FSR31-VEGA-OFFLOAD.md` in the repository root for the recorded findings and limits.
