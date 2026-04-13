# Crimson Desert - Live Transmog

**This mod is in BETA** and edge cases may exist. Use at your own risk. Please report any issues you find! **Currently only works with Kliff** - other playable characters are not supported yet.

[![Live Transmog Demo](https://img.youtube.com/vi/B4xX3LkgXhs/maxresdefault.jpg)](https://www.youtube.com/watch?v=B4xX3LkgXhs)

## Overview

**CrimsonDesertLiveTransmog** is an ASI plugin for Crimson Desert that lets you change the visual appearance of equipped armor in real time without affecting stats. Browse the full item catalog through a ReShade overlay GUI, pick the armor you want to see, build presets, and apply instantly.

## Features

- In-game item browser with search-filterable dropdown, auto-categorized by slot (Helm, Chest, Cloak, Gloves, Boots)
- ReShade overlay GUI for full point-and-click control (Home key)
- Preset system: save, load, rename, and cycle through multiple transmog presets per character
- NPC armor variants and damaged variants render via character-class bypass
- Safety filters: non-player gear (horse tack, pet armor) and wrong-slot items are hidden by default
- Per-slot control: enable/disable transmog independently for each equipment slot
- Optional hotkeys: all disabled by default, configurable via INI
- Open-source with full transparency

## Requirements

- Crimson Desert (PC, Steam)
- [ReShade](https://reshade.me/) — **required for the in-game GUI**. The transmog overlay is a ReShade addon; without ReShade installed, there is no GUI. You can still use the mod via hotkeys and by editing `CrimsonDesertLiveTransmog_presets.json` manually, but the item browser, preset management, and per-slot controls are only available through the ReShade overlay.
- ASI Loader (e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases))

## Installation

### Step 1: Install ReShade

Download and install [ReShade](https://reshade.me/) for Crimson Desert. The transmog overlay registers as a ReShade addon tab.

### Step 2: Install an ASI Loader

You need an ASI Loader to load `.asi` mods. If you already have one, skip to Step 3.

Download the **x64** build of [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) and place one of the following DLLs into your `bin64` folder:

| DLL name       | Notes                                    |
| -------------- | ---------------------------------------- |
| `winmm.dll`    | **Recommended**, works for most setups   |
| `version.dll`  | Alternative if `winmm.dll` conflicts     |
| `dinput8.dll`  | Another alternative                      |

> **How to verify:** After completing Step 3, launch the game once. If no `CrimsonDesertLiveTransmog.log` file appears in `bin64`, the ASI Loader is not loading. Try renaming the DLL to one of the other variants listed above.

### Step 3: Install the mod

1. Download the latest release from the [GitHub Releases page](https://github.com/tkhquang/CrimsonDesertTools/releases)
2. Extract `CrimsonDesertLiveTransmog.asi` and `CrimsonDesertLiveTransmog.ini` into your `bin64` folder

### Step 4: Launch and play

Launch the game. Press **Home** to open the ReShade menu, then find the **Transmog** tab.

### File placement

```text
<Crimson Desert>/bin64/
├── CrimsonDesert.exe                          (Game executable)
├── winmm.dll                                  (ASI Loader)
├── CrimsonDesertLiveTransmog.asi              (This mod)
├── CrimsonDesertLiveTransmog.ini              (Configuration)
├── CrimsonDesertLiveTransmog_presets.json     (Auto-generated preset file)
└── ...
```

### Using with OptiScaler

If you use [OptiScaler](https://github.com/cdozdil/OptiScaler) for frame generation or upscaling, you need to make sure that both ReShade, OptiScaler, and an ASI Loader can coexist. There are multiple valid setups depending on which DLL names each tool uses (e.g. ReShade as `dxgi.dll`, OptiScaler renamed to `winmm.dll` with `LoadAsiPlugins = true`, or a separate Ultimate ASI Loader alongside both).

I cannot provide a definitive guide here because the correct setup depends on your specific combination of tools and versions. You may need to experiment with DLL names and load order. One example that has worked:

```text
<Crimson Desert>/bin64/
├── CrimsonDesert.exe
├── winmm.dll                                  (OptiScaler, renamed)
├── dxgi.dll                                   (ReShade)
├── OptiScaler.ini                             (LoadAsiPlugins = true)
├── plugins/
│   ├── CrimsonDesertLiveTransmog.asi
│   ├── CrimsonDesertLiveTransmog.ini
│   └── ...
└── ...
```

If you run into issues, check the OptiScaler and ReShade documentation for their respective DLL naming and compatibility options.

> This applies to any ASI mod, not just this one. If you have other `.asi` mods, move them into the `plugins` folder as well.

## Usage

> **The in-game GUI is only available through ReShade.** If you do not have ReShade installed, use the **[online Preset Builder](https://tkhquang.github.io/CrimsonDesertTools/live-transmog/)** or edit the JSON file manually. See the [Without ReShade](#without-reshade-web-preset-builder) section below.

### With ReShade (recommended)

1. Open the ReShade overlay (**Home** key) and find the **Transmog** tab
2. In **Slot Details**, click the picker button next to a slot to browse available armor
3. Use the search box to filter by name
4. Select an item - changes stay **pending** until you click **Apply All**
5. Click **Apply All** to commit all slot changes at once
6. Use **Append** in the Presets section to save your current look
7. Use **Prev** / **Next** to cycle through saved presets

### Without ReShade (web preset builder)

If you cannot or do not want to use ReShade, use the **[online Preset Builder](https://tkhquang.github.io/CrimsonDesertTools/live-transmog/)** to create presets from your browser. It mirrors the in-game GUI - browse the full item catalog, pick armor per slot, save multiple presets, then download the JSON file.

Alternatively, you can edit `CrimsonDesertLiveTransmog_presets.json` by hand:

1. Launch the game once so the mod generates a default `CrimsonDesertLiveTransmog_presets.json`
2. Close the game and open the JSON file in a text editor
3. Each preset contains slot entries with `itemName` (string identifier). Set `active: true` and fill in the item name for each slot you want to transmog
4. Set hotkeys in the INI (e.g. `ApplyHotkey = F5`, `ClearHotkey = F6`, `CaptureHotkey = F7`) to apply/clear/capture in-game
5. Launch the game - the mod loads presets on startup and applies them automatically

> **Tip: Use Capture to get item IDs.** Equip the armor you want in-game, then press your **Capture** hotkey. This snapshots your currently equipped gear into the active preset's slot mappings. Check `CrimsonDesertLiveTransmog_presets.json` after - the captured item names will be filled in for each slot. You can then copy these to build other presets manually.

### Item Catalog

The full item catalog for the current game version can be browsed on the **[online Preset Builder](https://tkhquang.github.io/CrimsonDesertTools/live-transmog/)** under the "Item Catalog" section. It shows all item IDs, slot categories, variant/safety flags, and names in a searchable, sortable table.

## Configuration

Edit `CrimsonDesertLiveTransmog.ini` for global settings and optional hotkey bindings. All hotkeys are **disabled by default** - the ReShade overlay is the primary interface.

```ini
[General]
LogLevel = Info
Enabled = true
; All hotkeys disabled by default - set via INI if needed
ToggleHotkey =
ApplyHotkey =
ClearHotkey =
CaptureHotkey =

[Presets]
AppendHotkey =
ReplaceHotkey =
RemoveHotkey =
NextHotkey =
PrevHotkey =
```

Presets are stored in `CrimsonDesertLiveTransmog_presets.json` (auto-generated alongside the INI).

### Hotkey Format

- Named keys: `T`, `F3`, `Numpad1`, `Mouse4`, `Gamepad_A`, etc.
- Modifiers: `Ctrl`, `Shift`, `Alt` (or `LCtrl`, `RCtrl`, etc.)
- Multiple combos: `T,Gamepad_LB+Gamepad_Y` (T alone OR hold LB + press Y)
- Empty value = disabled

See the full list at the [Supported Input Names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) reference.

## Known Limitations

- **Item catalog may be incomplete** - the armor list is filtered to exclude known-broken items. Some wearable items may be missing. Report missing items in the Bugs or Posts tab.
- **NPC armor variants and damaged variants render via bypass** — items tagged `(NPC variant)` in the picker use a character-class bypass to render. Most work; a few may still produce empty slots depending on the item's internal skeleton bindings.
- **Non-humanoid items crash** — horse tack, pet armor, and wagon gear crash the mesh binder. The "Safe only" filter hides these by default.
- **Wrong-slot or non-equipment items will crash** - selecting a chest piece for the helm slot, or non-armor items (dog armor, recipes, etc.) crashes the game. Safety filters prevent this by default. Do not disable them unless you know what you are doing.
- **Dyeing is not supported yet** - transmog items use their default colors.
- **Special-effect armor may have visual quirks** - armor with particle effects (e.g. Marni Laser Helm) may not render particles correctly. Hair may clip through some helmets. I haven't been able to make these work yet.
- Major game updates may break the mod until a new version is released.
- Only tested with the Steam version.

## Troubleshooting

If you encounter issues:

1. Set `LogLevel = Debug` in the INI file
2. Check `CrimsonDesertLiveTransmog.log` in your game directory
3. After a game update, the mod may stop working due to memory layout changes

> **Still stuck?** [Open a GitHub issue](https://github.com/tkhquang/CrimsonDesertTools/issues/new?assignees=&labels=bug&template=bug_report.yaml) and include your INI config, log output, and game version.

## Building from Source

### Prerequisites

- [Visual Studio 2022](https://visualstudio.microsoft.com/) (MSVC with C++23 support)
- [CMake](https://cmake.org/) (3.16 or newer)
- Git (to fetch submodules)

### Release Build

```bash
git submodule update --init --recursive
cd CrimsonDesertLiveTransmog
cmake --preset msvc-release
cmake --build build/release-msvc --config Release --parallel
```

The output binary (`CrimsonDesertLiveTransmog.asi`) will be placed in the build directory.

### Dev Build (Hot-Reload)

The dev build produces a loader ASI + logic DLL pair, allowing you to rebuild and hot-reload the logic DLL without restarting the game.

```bash
cmake --preset msvc-dev
cmake --build build/dev-msvc --config RelWithDebInfo --parallel
```

Press **Numpad 0** in-game to trigger a reload after rebuilding.

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) - for the Ultimate ASI Loader
- [cursey](https://github.com/cursey) - for SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) - for SimpleIni
- [crosire](https://reshade.me/) - for ReShade
- Pearl Abyss - for Crimson Desert

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for a detailed history of updates.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
