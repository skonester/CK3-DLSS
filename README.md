# Crusader Kings III DLSS 5

<p align="center">
  <img src="resources/CK3DLSS5.jpg" alt="CK3 DLSS 5" width="720">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/release-1.0.6-76B900?style=for-the-badge" alt="Release 1.0.6">
  <img src="https://img.shields.io/badge/Vulkan-1.3-blue?style=for-the-badge&logo=vulkan" alt="Vulkan 1.3">
  <img src="https://img.shields.io/badge/ReShade-6.8-blue?style=for-the-badge" alt="ReShade">
  <img src="https://img.shields.io/badge/DLSS-NVIDIA-76B900?style=for-the-badge&logo=nvidia" alt="DLSS">
</p>

**Version 1.0.6, out of beta.** NVIDIA DLSS 5 Neural Rendering for the character portraits in
**Crusader Kings III**: more lifelike faces, hair and clothing on every portrait, from your ruler
in the corner of the map to the council and character windows.

> [!TIP]
> **For the portrait enhancements, pick the DLSS 5 Extended profile in the installer.** The app
> shows it with an *Experimental* tag because it uses a community DLSS 5 runtime, but it's the
> profile this release was built and tested on.

## What's new in 1.0.6

- **Portrait mode is finished.** DLSS 5 now enhances the portrait images themselves, and the game
  draws them as usual. The square box that used to surround characters on the map and in windows
  is gone.
- **Hovering stays enhanced.** Moving the mouse over your ruler's portrait no longer switches
  DLSS 5 off for a moment.
- **Playable on mid-range cards.** About 42 fps on an RTX 3060 with DLSS 5 on, where running it on
  the whole screen gave about 15 fps. CK3 still starts in seconds.

## What you need

- Crusader Kings III (Steam, Windows)
- An NVIDIA RTX graphics card (RTX 20 series or newer)
- An up-to-date NVIDIA driver

## Install

1. Close Crusader Kings III and the Paradox launcher.
2. Download **`CK3-DLSS.zip`** from the [latest release](../../releases/latest) and extract it
   anywhere, for example your Downloads folder.
3. Open the **`tools`** folder, then **`CK3-DLSS-Installer`**, and double-click
   **`CK3 DLSS Installer.exe`**. Everything else is done from this app.
4. Check that the app found your CK3 folder, select **DLSS 5 Extended**, tick the license box and
   press **Install**. The app copies everything into your CK3 folder for you.
5. Press **Launch CK3**.

You don't need to copy any files yourself.

## Play

Start the game from the installer with **Launch CK3**, or with **`Launch CK3 with DLSS.cmd`** in
your CK3 folder.

> [!NOTE]
> Steam's normal **Play** button starts CK3 without DLSS.

| Key | What it does |
|---|---|
| **F6** | Turns DLSS 5 on or off, so you can compare. |
| **Home** | Opens the ReShade menu. DLSS settings are on its **Add-ons** tab. |
| **F5** | Takes a screenshot through the DLSS 5 add-on. |

DLSS 5 shows on the portraits only; the map and interface keep CK3's own look. The enhanced
portraits appear as soon as a portrait is on screen. On the loading screen, the very tall
portraits stay as the game draws them.

## Profiles

| Profile in the app | What it does |
|---|---|
| **DLSS 5 Extended** | **Use this one.** DLSS 5 Neural Rendering on the character portraits. Runs on more cards than NVIDIA's preview through the ShortFuse runtime (tested on an RTX 3060). |
| DLSS 4.5 | Model M anti-aliasing across the whole screen at native resolution, no portrait enhancement. The app tags it *Recommended* as the safe fallback if DLSS 5 doesn't run on your card. |
| DLSS 5 | NVIDIA's signed DLSS 5 runtime, for cards its preview supports. |
| Native Streamline | Developer testing only. Not for normal play. |

To change profiles, close CK3, open the installer again, pick another profile and press
**Install**.

## Uninstall

Open the installer and press **Uninstall**. It puts CK3's original renderer setting back and
removes every file it added. Your saves and the game's own files are not touched.

## Troubleshooting

| Problem | Fix |
|---|---|
| No ReShade menu, or portraits look unchanged | Start the game with **Launch CK3** in the installer, not Steam's Play button. Press **F6** in case DLSS 5 was switched off. |
| The installer can't find CK3 | Point it at the folder Steam opens from **Crusader Kings III → Manage → Browse local files**. |
| "Validation failed" | Close CK3, press **Install** again in the installer. |
| Low frame rate | Make sure you picked **DLSS 5 Extended**, which only processes the portraits. If your card still struggles, press **F6** or switch to the DLSS 4.5 profile. |
| RHI or ReShade asks to replace `dxgi.dll` | Say no for CK3. This package's `dxgi.dll` is required. RHI itself can stay installed. |
| You want your game back right away | Press **Uninstall** in the installer, then play from Steam. |

Don't combine this with OptiScaler, NVIDIA Smooth Motion, or another DLSS or ReShade injector for
CK3.

If you report a problem, please include these files from your CK3 `binaries` folder:
`dlss-active\dlss5-feed.log`, `dlss5-vulkan\feed-vk-layer.log`, `dlss5-dxgi.log` and `ReShade.log`.

## Good to know

- It switches CK3's renderer to **Vulkan**. Uninstall puts your previous setting back.
- It does not replace `ck3.exe`, installs nothing system-wide, and does not affect other games.

## For developers

- [frontier4milestone.md](frontier4milestone.md): the current state (1.0.6) and what it took to get
  here.
- [frontier.md](frontier.md), [frontier2.md](frontier2.md), [frontier3.md](frontier3.md): the
  portrait-mode work, measurements and test logs, step by step.
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

Outputs: `build\dlss5-feed.addon64`, `layer\VkLayer_feed_vk.dll`, `layer\dxgi.dll`. Releases are
built by the **Installer GUI** workflow from `ck3-package` and `installer-gui`.

## Credits and licenses

Built on DLSS5 Feeder, ReShade, VORT shaders, RenoDX and RHI, Lilium HDR shaders, and NVIDIA DLSS.
See [`THIRD-PARTY-NOTICES.md`](ck3-package/THIRD-PARTY-NOTICES.md) and
[`THIRD-PARTY-LICENSES`](ck3-package/THIRD-PARTY-LICENSES).
