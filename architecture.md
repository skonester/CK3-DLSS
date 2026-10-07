# DLSS5 Feeder Architecture

## Overview

This document describes the architectural design of the Crusader Kings III DLSS Vulkan integration package. The system enables DLSS 4.5/5 rendering for CK3 by injecting a package-local Vulkan layer, DXGI bootstrap, and ReShade framework that work together to deliver DLSS-enhanced rendering.

## Core Components

### 1. App-Local DXGI Bridge (`binaries\dxgi.dll`)

The `dxgi.dll` is a custom x64 DLL that serves as the foundation for the entire DLSS pipeline. It is **not** a replacement Vulkan driver and does not translate all CK3 rendering to D3D12.

**Key functions:**

1. **Loads package's private ReShade64.dll** through its DXGI exports
2. **Creates a hidden D3D12 device, command queue, and DXGI swapchain**
3. **Performs an initial `Present`** so ReShade and RenoDX receive the initialization events they expect
4. **Keeps the hidden runtime alive** while the feeder creates its private D3D12 device and DLSS feature
5. **Leaves the existing Vulkan/D3D12 shared-texture and shared-fence transport** responsible for moving CK3's frame to DLSS and returning the processed result to Vulkan

**Why it's required:** CK3's Vulkan renderer does not naturally load a DXGI proxy or create a D3D12 presentation runtime. The RenoDX DLSS 5 add-on hooks D3D12 NGX entry points, and when RHI installs ReShade as `dxgi.dll` for a D3D12 game, ReShade starts early and RenoDX observes the D3D12 device lifecycle before the game creates or evaluates its DLSS feature. Loading ReShade only as a Vulkan layer discovers the add-ons, but by itself does not guarantee that those D3D12 hooks are armed.

### 2. Vulkan Layer (`layer\VkLayer_feed_vk.dll`)

The Vulkan layer is the primary entry point that intercepts CK3's Vulkan calls. Key aspects:

- **Package-local**: Does not register as a global Vulkan layer
- **Exports `DLSS5Bootstrap`**: The dxgi.dll calls this export before Vulkan device creation
- **Compatibility shim**: Keeps the system Vulkan loader handle expected by CK3 and owns `vkGetInstanceProcAddr` plus `vkGetDeviceProcAddr`
- **Routes calls strategically**:
  - Device and swapchain calls continue through Streamline (or the package's interposer)
  - Win32 surface creation, destruction, presentation support, and capability queries use the system Vulkan loader
- **Failure handling**: If initialization fails, CK3 uses the normal Vulkan loader without interception
- **Logging**: Records results in `binaries\dlss5-dxgi.log`

### 3. DLSS Feeder (`build\dlss5-feed.addon64`)

The feeder is the NGX-based DLSS integration component:

- **Built from**: `src\dlss5-feed.cpp` and `src\dlss5-feed32.cpp`
- **Output**: `build\dlss5-feed.addon64`
- **Function**: Feeds color frame, depth, and motion vectors to the NVIDIA DLSS runtime
- **Motion vector estimation**: Uses VORT shaders since CK3 has no native DLSS motion-vector integration
- **Profile support**: Supports multiple DLSS runtime profiles (DLSS 4.5 Model M, DLSS 5 Stock, DLSS 5 Extended)

### 4. ReShade Integration

ReShade provides the graphics overlay framework:

- **Activated via**: Press **Home** in game
- **DLSS preset**: Contains `vort_MotionEffects`, `DLSS5_Feed`, `vort_StaticEffects` (motion effect must be above DLSS5_Feed)
- **Add-ons tab**: DLSS preset selection and RenoDX/Neural Rendering controls appear here
- **Lilium HDR Shaders**: 2026.02.28 suite bundled for HDR analysis, tone mapping, inverse tone mapping, black-floor correction, and HDR-aware sharpening

### 5. RenoDX / RHI Runtime Integration

RenoDX enables DLSS 5's Neural Rendering extension:

- **Hooked through**: The app-local `dxgi.dll` creates a hidden D3D12 device
- **Initialization sequence**: Hidden Present → ReShade/RenoDX initialization → feeder device creation
- **Profile dependencies**:
  - DLSS 5 Stock: Requires RenoDX
  - DLSS 5 Extended: Requires RenoDX with modified ShortFuse runtime
  - DLSS 4.5: Does not require RenoDX

### 6. Profile System

Four runtime profiles are supported, each with different component combinations:

| Profile | Components | Intended use |
|---|---|---|
| **DLSS 4.5 Neural Reconstruction / DLAA** | Local feeder + NVIDIA DLSS runtime | RTX 20/30/40 baseline; Model M neural reconstruction |
| **DLSS 5 Stock** | Feeder + RenoDX + signed DLSS/Neural Rendering pair | Hardware supported by the stock preview runtime |
| **DLSS 5 Extended** | Feeder + RenoDX + modified ShortFuse Neural Rendering runtime | Experimental compatibility testing |
| **Native Streamline Vulkan** | NVIDIA Streamline interposer + DLSS/DLSS-RR plugins + stock NGX fallback | Experimental Vulkan interception and integration testing |

### 7. Installer Architecture

The installer (`Open CK3 DLSS Installer.cmd`) performs these operations:

1. **Validates** the bundled x64 renderer and runtime files
2. **Creates** `binaries\dlss-active` containing only the selected profile's files
3. **Backs up** CK3's current renderer setting
4. **Changes** `Graphics.renderer` to `Vulkan`
5. **Installs** the package-owned `binaries\dxgi.dll` bootstrap
6. **Requires license acknowledgement** before enabling installation

**Profile switching**: Close CK3 and reopen `Open CK3 DLSS Installer.cmd` to change profiles. Each switch rebuilds `binaries\dlss-active` so files from the previous profile are not left loaded.

### 8. Launcher (`Launch CK3 with DLSS.cmd`)

The launcher supplies the package-local Vulkan-layer environment:

- **Required**: Steam's normal Play button does not activate this package
- **Function**: Sets environment variables and launches CK3 with the correct Vulkan layer path
- **RHI suppression**: Disables RHI/global ReShade Vulkan layer only for the CK3 process

### 9. Disable/Restore Mechanism (`Disable CK3 DLSS.cmd`)

Restores CK3 to its pre-installation state:

1. **Restores** the recorded renderer setting
2. **Deactivates** portable Vulkan layers when CK3 is launched normally
3. **Package files remain available** but are inactive

## Technical Flow

```
User launches CK3 via Launch CK3 with DLSS.cmd
    ↓
Launcher sets Vulkan layer environment variables
    ↓
CK3 loads package's VkLayer_feed_vk.dll
    ↓
Vulkan layer calls DLSS5Bootstrap in dxgi.dll
    ↓
dxgi.dll loads ReShade64.dll and creates hidden D3D12 runtime
    ↓
Hidden Present triggers ReShade/RenoDX initialization
    ↓
dxgi.dll creates feeder's private D3D12 device
    ↓
DLSS feeder (dlss5-feed.addon64) processes frame
    ↓
DLSS runtime enhances frame
    ↓
Processed result returned to Vulkan for presentation
```

## Build Outputs

```
build\dlss5-feed.addon64          (DLSS feeder addon)
layer\VkLayer_feed_vk.dll         (Vulkan layer)
layer\dxgi.dll                    (DXGI bootstrap)
```

## Known Constraints

- **No native DLSS motion-vector integration**: CK3 has no native motion vectors; VORT estimates motion, causing potential ghosting during fast map movement, UI elements, smoke, and transparency
- **Experimental status**: This is prototype software with known limitations
- **Profile compatibility**: Do not combine the active package with OptiScaler, Smooth Motion, or a second DLSS/Streamline injector
- **Streamline identity**: The donor Streamline runtime does not have a valid NVIDIA application identity for CK3, so direct NGX features may remain disabled until supported project identity and resource tagging are added

## Native Streamline Bridge Details

For the opt-in Native Streamline profile, the bridge calls `slInit` for Vulkan with the DLSS and DLSS-RR plugins when CK3 dynamically loads `vulkan-1.dll`. It returns the normal system Vulkan loader handle, tracks surviving instances across CK3's temporary probes, and routes proc-address lookups through typed wrappers. Surface operations (Win32 surface creation, destruction, presentation support, capability queries) use the system Vulkan loader to avoid the donor interposer's unbound surface thunk during CK3's device-first startup; device and swapchain operations remain routed through Streamline. If initialization fails, CK3 uses the normal Vulkan loader without interception. The result is recorded in `binaries\dlss5-dxgi.log`.

The feeder's existing NGX path remains the evaluator fallback; direct Streamline resource tagging and feature evaluation are not yet implemented.

## RHI Coexistence

RHI may remain installed. The launcher sets `DISABLE_VK_LAYER_reshade_1=1` only for the CK3 process to suppress a separately registered global ReShade Vulkan layer, while the package manifest uses its own disable key and remains active. RHI or another injector must not replace this package's `binaries\dxgi.dll`: it contains the custom `DLSS5Bootstrap` entry point that the Vulkan layer requires.

## Portrait Mode (Frontier 1, unreleased)

`portrait_mode=1` restricts DLSS and DLSS 5 Neural Rendering to the CK3 GUI portraits. `src/feed_render_dump.h` tags the GUI portrait pipeline from SPIR-V member names and reads each portrait's on-screen rectangle from its constant buffer. The Vulkan transport packs the visible rectangles into a pixel-budgeted atlas, evaluates only the atlas, and `src/feed_portrait_blend.h` feathers the result back into the frame. See [frontier.md](frontier.md) for the full design log and measurements.

The [Frontier 2 blend optimization](frontier2.md) schedules only disjoint feather bands in one compute dispatch. Where the output format supports typed UAV loads, it blends in place without a scratch atlas or full-atlas copy; other formats retain an SRV copy fallback. Portrait interiors and unused atlas padding are untouched. The existing frame-slot fences protect the mapped blend constants, and UAV barriers order the pass after NGX evaluation. The alpha-aware portrait mask remains outstanding.

`src/feed_portrait_atlas.h` provides persistent guarded slots using MaxRects splitting. The budget covers the full allocated texture, including holes and alignment. Initial compact dimensions follow crop demand; expansion buys coverage, aspect changes require a missing large portrait, and a 180-update delay controls shrinkage. Surviving crops reserve their old addresses. Removals retain history when remaining crops and addresses match; new or changed crops still reset the feature globally. Color and optional mask inputs are cleared before crop copies, with a transfer write barrier, so retired slots and guards are deterministic. Crops that cannot fit retain native rendering. This affects portrait mode on Vulkan; other transport paths keep their existing full-frame behavior.

## Package Layout

```text
Crusader Kings III\
  Open CK3 DLSS Installer.cmd
  Install CK3 DLSS.cmd
  Install CK3 DLSS 4.5 RTX 3060 Test.cmd
  Install CK3 DLSS 5 Stock Test.cmd
  Install CK3 DLSS 5 Extended Test.cmd
  Install CK3 DLSS Native Streamline Experimental.cmd
  Configure CK3 DLSS Runtime.cmd
  Launch CK3 with DLSS.cmd
  Disable CK3 DLSS.cmd
  DLSS5-CK3.ps1
  DLSS-Runtime-Setup.ps1
  Graphics-Dependency-Setup.ps1
  tools\
    CK3-DLSS-Installer\
      CK3 DLSS Installer.exe
    RHI-Setup.exe
  binaries\
    ck3.exe                              (provided by CK3)
    dxgi.dll                             (this package's DXGI/D3D12 bootstrap)
    ReShade.ini
    DLSS5-CK3.ini
    dlss5-vulkan\
      ReShade64.dll
      ReShade64.json
      VkLayer_feed_vk.dll
      VkLayer_feed_vk.json
    dlss-payload\
      dlss5-feed.addon64
      runtimes\DLSS45\...
      runtimes\DLSS5\...
      runtimes\DLSS5Extended\...
      runtimes\NativeStreamline\...
    dlss-active\                         (created by the installer)
    reshade-shaders\Shaders\
      DLSS5_Feed.fx
    third-party\vort_Shaders\
```

## Packaging

CK3 packaging scripts and the drag-and-drop template are under `ck3-package`. Build and stage the self-contained installer app (JDK 17) before creating a release package:

```powershell
.\Build-Installer-GUI.ps1
```
