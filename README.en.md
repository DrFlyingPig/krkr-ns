<div align="center">

# KRKR-ns

[中文](README.md) | English

**KiriKiri visual novel engine port for Nintendo Switch**

**`⚠️ Experimental project; stability is not guaranteed. ⚠️`**

Porting baselines: [krkrsdl2](https://github.com/krkrsdl2/krkrsdl2) `bf207f2` and [krkrz](https://github.com/krkrsdl2/krkrz) `b11c43`; compatibility work is informed by Kirikiroid2.

[![Release](https://img.shields.io/github/v/release/DrFlyingPig/krkr-ns?style=flat-square)](https://github.com/DrFlyingPig/krkr-ns/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red?style=flat-square)

**[⬇️ Download the NRO](https://github.com/DrFlyingPig/krkr-ns/releases/latest)**

</div>

---

## 📖 Overview

KRKR-ns is a Nintendo Switch port of the KiriKiri / KRKR visual novel engine. It is based on krkrsdl2 and uses Kirikiroid2 as a compatibility reference. The launcher currently uses `.xp3` files as game entries and can read resource archives and files in the game directory. Some plugins, formats, and APIs remain subject to compatibility limits.

The built-in light-themed library supports controller and touch input. Each game can have its own preview image, icon, and display name. Browsing and changing the selection only updates the library; game resources are loaded after you choose **Start Game**. Selecting **Exit Game** returns to the library so you can choose another title.

## ⬇️ Installation and use

> ⚠️ **Important: paths on a real Switch must not contain Chinese characters.**
>
> The game directory, the `.xp3` entry filename, and the actual resource path on the SD card must use ASCII names such as English letters, numbers, and underscores. For example: `sdmc:/switch/KRKR-ns/Game/Game01/data.xp3`.
>
> Renaming a game directory changes how saves and artwork settings are matched. Back up saves before renaming it. Avoid renaming resources inside an XP3 archive; when loose resources must be renamed, also check every script reference.

1. Download `krkrsdl2.nro` from [Releases](https://github.com/DrFlyingPig/krkr-ns/releases/latest) and place it in `sdmc:/switch/KRKR-ns/`. When upgrading, replace the NRO while keeping your games, saves, and artwork settings.
2. **Real Switch:** open HBMenu in full-memory mode. In most setups, hold `R` while launching an installed game and then load the NRO; launching from the Album generally provides less memory. The entry button can be changed through the [Atmosphère configuration](https://github.com/Atmosphere-NX/Atmosphere/blob/master/config_templates/override_config.ini).
3. **Emulator:** load the NRO directly. The SD-card paths below refer to the emulator's virtual SD-card directory.
4. Put each game under `sdmc:/switch/KRKR-ns/Game/<GameFolder>/` using the naming rules above. Keep the original directory structure and script references. Select a game in the library and press `A` to start it; press `X` to choose an `.xp3` entry. Other `.xp3` files in the same directory are mounted as resource archives, and the last selected entry is preferred on the next launch.
5. Saves are separated by game directory under `sdmc:/switch/KRKR-ns/saves/<GameFolder>/`. Multiple entry archives in one game directory share that save directory.
6. Put custom artwork in `sdmc:/switch/KRKR-ns/Artwork/`. Open the artwork settings for a selected game to choose a preview image and icon from the game or from custom artwork; the choice is saved automatically.

> **Entry requirement:** the launcher currently requires an `.xp3` entry. A loose directory containing only `startup.tjs`, without an `.xp3`, cannot yet be launched directly from the library.
>
> **Display names:** add `gameAliases["Game01"] = "中文显示名称";` to `sdmc:/switch/KRKR-ns/Names.tjs` to change a library label without changing the directory name.

Diagnostic logs are stored in `sdmc:/switch/KRKR-ns/log/`, with the three most recent logs kept automatically. Runtime switches and source-level compatibility notes are documented in [PATCHES.md](docs/PATCHES.md).

To build locally, prepare devkitPro / devkitA64, the Switch dependencies, CMake/Ninja, and FFmpeg, then configure the tool paths in the build scripts. The FFmpeg build script is [build_ffmpeg_switch.sh](tools/build_ffmpeg_switch.sh). Run `build_nro.sh` to create `build-switch/krkrsdl2.nro`; add `--no-emu-copy` to skip copying it to the emulator.

## ✅ Implemented features

See [KIRIKIROID2_PORTING_PLAN.md](docs/KIRIKIROID2_PORTING_PLAN.md) for compatibility differences, architecture notes, and the porting plan.

**Library and interface**

- Light-themed game library with controller and touch input; `A` confirms, `B` goes back, and paging, entry selection, and rescanning are supported.
- Automatically selects and remembers a game's entry file, with optional custom display names.
- Browsing and switching games does not scan game resources; loading starts after **Start Game** is selected.
- Preview images and icons can come from the game or from SD-card artwork, with paging, automatic cropping, reset-to-default, and persistent settings.
- Exiting a game returns to the library so another title can be selected; low-memory startup provides a retry path.

**Game execution and saves**

- Supports common KRKR / KAG games and multiple resource archives in a game directory.
- Supports game settings, choices, confirmation dialogs, and quick-save / quick-load confirmation flows; the exact behavior still depends on game compatibility.
- Provides popup menus for games that use `MenuItem`, including submenus, checkboxes, radio items, disabled/hidden states, per-window menus, and shortcuts through controller, keyboard, or touch input.
- Keeps saves separate per game and rotates quick-save backups.
- Loads patches and translation resources with priority and resolves resources by directory to avoid mixing same-named images from different games.
- Improves Chinese and Japanese script and font compatibility; games that provide a decryption script may be able to read encrypted archives through that script.

**Graphics, animation, and audio/video**

- Supports backgrounds, character art, text, choices, and common transitions while preserving the game's aspect ratio.
- Supports E-mote animated characters and improves the return to the title screen after limited animations.
- Supports transparent AlphaMovie playback, looping, and frame skipping.
- Includes the native LayerExRaster plugin for script-driven ripple effects used by some scene transitions.
- Music and voice support WAV, OGG, Opus, and some AAC/M4A and MP3 content, including files whose extension does not match the actual audio format. Video support includes parts of WMV, MP4, and MPEG, with volume control for video playback.
- Video can seek by time or frame and loop a selected segment while retaining its playing or paused state.
- Variable-speed playback and multi-track switching are not currently supported; an audio codec that works in video does not necessarily work as a standalone audio file.
- Includes common text rendering, script parsing, save, and font plugins; some plugins and formats are still unsupported.

**Performance and stability**

- Caches displayed text, images, and game resources to reduce repeated reads and drawing during library and menu use.
- Reduces repeated work for large E-mote animations and parallelizes parts of transitions, falling back to synchronous drawing when thread resources are unavailable.
- Reports image loading and decoding errors, with a custom-artwork alternative for images that cannot be previewed.
- Fixes audio underruns in some video playback cases and improves end-of-video audio handling and resource cleanup.
- Cleans up windows, audio, timers, and events when leaving a game, improving recovery after script cleanup errors and return to the library.
- Fixes object-cleanup crashes in quick-load flows for some games and improves resource release when videos are closed and reopened.
- Keeps the launcher usable after a startup-script failure.
- Automatically retains the three most recent diagnostic logs.

## 🗂 Architecture

The project is organized into a launcher, engine core, Switch platform layer, built-in plugins, and script compatibility layer:

| Component | Responsibility |
| --- | --- |
| Launcher | `data/startup.tjs` provides the game list, entry selection, and artwork UI; the `LauncherArtwork` module scans images, creates thumbnails, and saves choices. |
| Engine core | `external/krkrz/` provides TJS2 execution, layers and bitmaps, and resource archives; resource lookup is implemented in `base/StorageIntf.cpp`. |
| Switch platform layer | `src/core/` connects windows, input, drawing, files, and audio. SDL2 and libnx adapt the engine to Switch; FAudio outputs audio, while FFmpeg is the fallback decoder for video, video audio, and standalone audio. |
| Built-in plugins | `src/plugins/` provides E-mote, AlphaMovie, LayerExRaster, text rendering, and save support. These plugins are compiled into the NRO and registered by the engine or enabled on demand. |
| Script compatibility layer | `compat-patches/system/` contains platform compatibility scripts packaged in RomFS with the launcher and font; SD-card patches can override the bundled versions. |

The launcher and games share one engine. Browsing the library updates only the UI; the selected entry and its resources are loaded after **Start Game**. Exiting a game rebuilds the engine state before returning to the library.

Repository layout:

```text
KRKR-ns/
├── krkrsdl2/                       # Engine source
│   ├── external/
│   │   ├── krkrz/                  # KRKRZ core: scripts, layers, bitmaps, archives
│   │   └── ...                     # SDL2, FAudio, simde, zlib, and other dependencies
│   ├── src/
│   │   ├── core/sdl2/              # Entry point, presentation, logs, artwork handling
│   │   │   ├── SDLEntrypoint.cpp
│   │   │   ├── SDLApplication.cpp
│   │   │   └── LauncherArtwork*    # Artwork scanning, decoding, thumbnails, settings
│   │   ├── core/base/sdl2/         # Files, storage, scripts, plugin loading
│   │   ├── core/visual/sdl2/       # Windows, drawing, layers, video playback
│   │   ├── core/sound/sdl2/        # Audio output and decoding
│   │   ├── core/environ/sdl2/      # Lifecycle, events, threads, system information
│   │   ├── core/msg/sdl2/          # Messages and dialogs
│   │   ├── core/utils/sdl2/        # Clipboard and platform utilities
│   │   ├── plugins/                # E-mote, AlphaMovie, LayerExRaster, text, saves
│   │   ├── resources/nswitch/      # NRO platform resources
│   │   └── config/                 # Build source manifests
│   ├── data/
│   │   ├── startup.tjs             # Game-library launcher
│   │   ├── launcher/               # Launcher logo and default artwork
│   │   └── notosanssc.ttf          # Bundled Chinese font
│   ├── CMakeLists.txt              # Switch build configuration
│   └── meson.build                 # Upstream build configuration
├── compat-patches/system/          # TJS compatibility scripts packed into RomFS
├── design/launcher-preview/        # Launcher designs and previews
├── docs/                           # Compatibility, patch, and development notes
├── tools/                          # Build, packaging, and debugging tools
├── tests/                          # Engine, save, animation, and other checks
├── out/                            # Local FFmpeg dependencies and release NRO; ignored by Git
└── build_nro.sh                    # Build, package, and emulator deployment entry point
```

> Documentation index: [docs/README.md](docs/README.md). The porting plan is [KIRIKIROID2_PORTING_PLAN.md](docs/KIRIKIROID2_PORTING_PLAN.md), module notes are [MODULES.md](docs/MODULES.md), source patches are [PATCHES.md](docs/PATCHES.md), performance and failure notes are [RUNTIME_NOTES.md](docs/RUNTIME_NOTES.md), and the upstream delta is [UPSTREAM_DELTA.md](docs/UPSTREAM_DELTA.md), generated by `tools/upstream_delta.sh`.

## 📄 License

The project's own code is released under the [MIT license used by krkrsdl2](LICENSE). Upstream and third-party components, including krkrz, FAudio, SDL2, FFmpeg, simde, zlib, FreeType, libjpeg-turbo, libpng, libogg/libvorbis, and libopus, retain their original licenses and notices in their component directories.

Licensing for the E-mote, AlphaMovie, and LayerExRaster ports is documented in [E-mote license](krkrsdl2/src/plugins/emoteplayer/UPSTREAM-LICENSE.txt), [AlphaMovie license](krkrsdl2/src/plugins/alphamovie/LICENSE.krkrsdl3), and [LayerExRaster license](krkrsdl2/src/plugins/layerexraster/LICENSE.krkrsdl3). Archive support and its dependencies retain the notices provided with their source directories. FFmpeg source versions and build options are documented in [build_ffmpeg_switch.sh](tools/build_ffmpeg_switch.sh).

**This project does not include or distribute any commercial game assets.**
