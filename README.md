# Crusader Kings III DLSS Vulkan

<p align="center">
  <img src="resources/CK3DLSS5.jpg" alt="CK3 DLSS 5" width="720">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Vulkan-1.3-blue?style=for-the-badge&logo=vulkan" alt="Vulkan 1.3">
  <img src="https://img.shields.io/badge/ReShade-6.8-blue?style=for-the-badge" alt="ReShade">
  <img src="https://img.shields.io/badge/DLSS-NVIDIA-76B900?style=for-the-badge&logo=nvidia" alt="DLSS">
</p>

NVIDIA DLSS for **Crusader Kings III** on Windows: sharper, more stable images and DLSS 5 Neural
Rendering on character faces. Drop it into your CK3 folder, pick a profile, and play. It only
affects CK3, and you can switch it off at any time.

> [!IMPORTANT]
> **Current work: [Frontier 1](frontier.md), not in a release yet.**
> DLSS 5 Neural Rendering used to run at about **15 fps on an RTX 3060** because it processed the
> whole screen. Frontier 1 applies it **only to the character portraits**, which brings that to
> **about 40 fps**, and CK3 now starts in seconds instead of minutes.
> There is still a slight square box around portraits, most visible while zooming the map.
> **That is planned to be fixed in Frontier 2, and a new release will follow then.** Until then,
> the download below is the previous release. [Read the full Frontier 1 log](frontier.md).

## What you need

- Crusader Kings III (Steam, Windows)
- An NVIDIA RTX graphics card (RTX 20 series or newer)
- An up-to-date NVIDIA driver

## Install

1. Close Crusader Kings III and the Paradox launcher.
2. Download **`CK3-DLSS-Vulkan-All-Profiles.zip`** from the
   [latest release](../../releases/latest).
3. In Steam, right-click **Crusader Kings III**, then choose **Manage → Browse local files**.
4. Copy everything from the ZIP into that folder. `Launch CK3 with DLSS.cmd` should end up next to
   the `binaries`, `game` and `launcher` folders, not inside `binaries`.
5. Double-click **`Open CK3 DLSS Installer.cmd`**, pick a profile (see below), accept the licenses,
   and install.

## Play

Start the game with **`Launch CK3 with DLSS.cmd`**.

> [!NOTE]
> Steam's normal **Play** button starts CK3 without DLSS. Always use `Launch CK3 with DLSS.cmd`.

## Which profile should I pick?

| Profile | Pick it if… |
|---|---|
| **DLSS 4.5 (Model M)** | You have an RTX 20, 30 or 40 card. **Recommended.** Smooth, stable image across the whole screen. |
| **DLSS 5 Stock** | Your card is supported by NVIDIA's DLSS 5 preview. Adds Neural Rendering, which is heavy on the GPU. |
| **DLSS 5 Extended** | You want to try DLSS 5 on cards the stock version doesn't support. Experimental and may be unstable. |

To change profiles later, close CK3 and run `Open CK3 DLSS Installer.cmd` again.

A fourth, developer-only option (**Native Streamline**) exists for testing and is not meant for
normal play.

## In-game controls

| Key | What it does |
|---|---|
| **Home** | Opens the ReShade menu. DLSS settings are on its **Add-ons** tab. |
| **F6** | Turns DLSS 5 Neural Rendering on or off (DLSS 5 profiles). |
| **F5** | Takes a screenshot through the DLSS 5 add-on (DLSS 5 profiles). |

In the ReShade menu, keep **`vort_MotionEffects` above `DLSS5_Feed`** in the effect list. The
included preset is already in the right order.

## Uninstall or turn it off

Run **`Disable CK3 DLSS.cmd`**, then start CK3 normally from Steam. This restores your original
renderer setting. To remove the package completely, delete the files you copied in.

## Troubleshooting

| Problem | Fix |
|---|---|
| `binaries\ck3.exe was not found` | You copied the files one folder too deep. Copy them into the main CK3 folder instead. |
| No ReShade menu or no DLSS | Start the game with `Launch CK3 with DLSS.cmd`, not Steam's Play button. |
| "Validation failed" | Close CK3, copy the ZIP contents in again (overwrite everything), and reinstall your profile. |
| Very low fps on DLSS 5 | DLSS 5 Neural Rendering is heavy. Press **F6** to turn it off, or switch to the DLSS 4.5 profile. The Frontier 1 work (above) fixes this for a future release. |
| Ghosting when panning the map, around UI, smoke or water | CK3 has no built-in motion data for DLSS, so it is estimated. Some ghosting during fast movement is expected. |
| RHI or ReShade asks to replace `dxgi.dll` | Say no for CK3. This package's `dxgi.dll` is required. RHI itself can stay installed. |
| Something is broken and you want your game back | Run `Disable CK3 DLSS.cmd`, then launch CK3 from Steam. |

Don't combine this with OptiScaler, NVIDIA Smooth Motion, or another DLSS or ReShade injector for
CK3.

If you report a problem, please include these log files from the `binaries` folder:
`dlss-active\dlss5-feed.log`, `dlss5-vulkan\feed-vk-layer.log`, `dlss5-dxgi.log` and `ReShade.log`.

## Good to know

- This is **experimental** software.
- It changes CK3's renderer to **Vulkan**. `Disable CK3 DLSS.cmd` puts your previous setting back.
- The default DLSS 4.5 profile runs at your native resolution for image quality (DLAA). It is not
  a performance upscaler.
- It does not replace `ck3.exe`, does not install anything system-wide, and does not affect other
  games.

## For developers

- [frontier.md](frontier.md): current work (portrait-only DLSS 5) with measurements.
- [architecture.md](architecture.md): how the DXGI bridge, Vulkan layer, feeder and profiles fit
  together, plus the package layout.
- [UPSTREAM-COMPAT-TESTING.md](UPSTREAM-COMPAT-TESTING.md): upstream fixes ported and how they
  were verified.

Build the feeder and layer from this checkout (Visual Studio 2022 Build Tools, NGX SDK under
`external\ngx`, Vulkan headers under `external\vulkan`):

```cmd
build-local-current-tree.cmd
layer\build-layer-local.cmd
```

Outputs: `build\dlss5-feed.addon64`, `layer\VkLayer_feed_vk.dll`, `layer\dxgi.dll`.

## Credits and licenses

Built on DLSS5 Feeder, ReShade, VORT shaders, RenoDX and RHI, Lilium HDR shaders, and NVIDIA DLSS.
See [`THIRD-PARTY-NOTICES.md`](ck3-package/THIRD-PARTY-NOTICES.md) and
[`THIRD-PARTY-LICENSES`](ck3-package/THIRD-PARTY-LICENSES).
