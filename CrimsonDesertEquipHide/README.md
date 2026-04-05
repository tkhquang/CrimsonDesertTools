# Crimson Desert - Equip Hide

## Overview

**CrimsonDesertEquipHide** is an ASI plugin for Crimson Desert that allows players to hide equipped gear visually using customizable hotkeys. Toggle visibility of weapons, shields, bows, armor, tools, and lanterns per category. Useful for cleaner screenshots, aesthetic character builds, or simply enjoying the world without gear clutter.

## Features

- Toggle, force-show, or force-hide equipment visibility per category with hotkeys
- Global show-all / hide-all hotkeys to control every category at once
- Weapon categories: One-Hand Weapons, Two-Hand Weapons, Shields, Bows, Special Weapons, Tools, Lanterns
- Armor categories: Helm, Chest, Legs, Gloves, Boots, Cloak, Shoulder, Mask, Glasses
- Accessory categories: Earrings, Rings, Necklace, Bags
- User Presets: 10 custom part groups with independent visibility control (overrides built-in categories)
- Per-category configuration: enable/disable, toggle/show/hide hotkeys, default hidden state, part list
- IndependentToggle mode: categories sharing a hotkey flip their own state individually, preserving mixed visible/hidden configurations
- BaldFix: runtime hair-visibility fix when helmet/cloak is hidden — no PAZ patching needed. Hair/beard may occasionally disappear; re-toggling the helmet (show then hide) restores it
- GlidingFix: prevents hidden equipment from briefly flashing visible during state transitions (e.g. exiting gliding, cutscene transitions)
- ForceShow mode for compatibility with mods that alter default equipment visibility via PAZ patching (e.g. character replacers, armor replacers, transmog mods)
- Hot-reload cleanup: visibility bytes restored on DLL unload
- SEH-protected hook callback to prevent crashes if mod is outdated
- Fully customizable settings via INI configuration

## Installation

### Step 1: Install an ASI Loader

You need an ASI Loader to load `.asi` mods. If you already have one (e.g. from another mod), skip to Step 2.

Download the **x64** build of [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) and place one of the following DLLs into your `bin64` folder:

| DLL name       | Notes                                    |
| -------------- | ---------------------------------------- |
| `winmm.dll`    | **Recommended**, works for most setups   |
| `version.dll`  | Alternative if `winmm.dll` conflicts     |
| `dinput8.dll`  | Another alternative                      |

> **How to verify:** After completing Step 2, launch the game once. If no `CrimsonDesertEquipHide.log` file appears in `bin64`, the ASI Loader is not loading. Try renaming the DLL to one of the other variants listed above.

### Step 2: Install the mod

1. Download the latest release from the [GitHub Releases page](https://github.com/tkhquang/CrimsonDesertTools/releases)
2. Extract `CrimsonDesertEquipHide.asi` and `CrimsonDesertEquipHide.ini` into your `bin64` folder

### Step 3: Launch and play

Launch the game. Press `V` (default) to toggle equipment visibility. By default, the INI is configured to hide Shields, Helms, and Masks on game load.

### File placement

```text
<Crimson Desert>/bin64/
├── CrimsonDesert.exe              (Game executable)
├── winmm.dll                      (ASI Loader)
├── CrimsonDesertEquipHide.asi     (This mod)
├── CrimsonDesertEquipHide.ini     (This mod's configuration)
└── ...
```

### Using with OptiScaler

If you use [OptiScaler](https://github.com/cdozdil/OptiScaler) for frame generation or upscaling, the standard ASI Loader (`version.dll`) will conflict with OptiScaler's own `dxgi.dll`. Follow these steps instead:

1. **Delete** `winmm.dll` (or whichever ASI Loader DLL you placed) from `bin64`. OptiScaler will handle mod loading instead
2. Open `OptiScaler.ini`, find the `[Plugins]` section, and set:

   ```ini
   LoadAsiPlugins = true
   ```

3. Create a `plugins` folder inside `bin64`
4. Move `CrimsonDesertEquipHide.asi` and `CrimsonDesertEquipHide.ini` into the `plugins` folder

```text
<Crimson Desert>/bin64/
├── CrimsonDesert.exe
├── OptiScaler.ini                 (LoadAsiPlugins = true)
├── dxgi.dll                       (OptiScaler)
├── plugins/
│   ├── CrimsonDesertEquipHide.asi
│   ├── CrimsonDesertEquipHide.ini
│   └── ...                        (Other ASI mods go here too)
└── ...
```

> This applies to any ASI mod, not just this one. If you have other `.asi` mods (e.g. CDSprintHold), move them into the `plugins` folder as well.

## Configuration

The mod is configured via the `CrimsonDesertEquipHide.ini` file:

```ini
[General]
; Log level: Trace, Debug, Info, Warning, Error
LogLevel = Info
; Apply only to player characters
PlayerOnly = true
; Set to true if you use mods that alter the default visibility of equipment
; via PAZ patching (e.g. character replacers like 'Playing Kliff as Damiane',
; armor replacers, transmog mods). ForceShow overrides their changes so
; toggling equipment back to visible still works.
; Safe to leave false if you are not using such mods.
ForceShow = false
; Prevents baldness when hiding helmets/cloaks
; Hair/beard may occasionally disappear; re-toggling the helmet (show then hide) restores it
BaldFix = true
; Prevents hidden equipment from briefly flashing during state transitions
GlidingFix = true
; When multiple categories share the same ToggleHotkey, this controls
; whether they toggle as a synchronized group (false, default) or each
; category flips its own state independently (true).
; Use true when categories on the same key have different DefaultHidden
; values and you want to preserve mixed visible/hidden states.
IndependentToggle = false
; Force all categories visible / hidden at once (empty = disabled)
ShowAllHotkey =
HideAllHotkey =

[Shields]
Enabled = true
ToggleHotkey = V
ShowHotkey =
HideHotkey =
DefaultHidden = true
Parts = CD_MainWeapon_Shield_L, CD_MainWeapon_Shield_R, ...

[Helm]
Enabled = true
ToggleHotkey = V
ShowHotkey =
HideHotkey =
DefaultHidden = true
Parts = CD_Helm, CD_Helm_Acc, ...

[UserPreset1]
Enabled = false
ToggleHotkey =
ShowHotkey =
HideHotkey =
DefaultHidden = false
Parts =
```

### Hotkey Types

Each category supports three independent hotkey bindings:

| Key | Behavior |
|-----|----------|
| `ToggleHotkey` | Flips between hidden and visible |
| `ShowHotkey` | Always forces visible |
| `HideHotkey` | Always forces hidden |

Global overrides in `[General]`:

| Key | Behavior |
|-----|----------|
| `ShowAllHotkey` | Forces ALL categories visible |
| `HideAllHotkey` | Forces ALL categories hidden |

### Hotkey Format

- Named keys: `V`, `F3`, `Numpad1`, `Mouse4`, `Gamepad_A`, etc.
- Modifiers: `Ctrl`, `Shift`, `Alt` (or `LCtrl`, `RCtrl`, etc.)
- Multiple combos: `V,Gamepad_LB+Gamepad_Y` (V alone OR hold LB + press Y)
- Empty value disables the binding

See the full list at the [Supported Input Names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) reference.

### Using with Controllers

DetourModKit supports gamepad input natively via the **XInput** API:

```ini
ToggleHotkey = Gamepad_Y
ToggleHotkey = Gamepad_LB+Gamepad_Y
ToggleHotkey = V,Gamepad_LB+Gamepad_Y
```

> **XInput only:** Xbox controllers work natively. For PS4/PS5/Switch controllers, use an XInput translation layer (e.g. DS4Windows, DualSenseX, BetterJoy) or Steam Input. See [Gamepad Compatibility](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#gamepad-compatibility) for details.

## How It Works

1. **AOB Pattern Scanning** - Cascading patterns scan the game's memory to locate the visibility decision function and related game structures
2. **Mid-Hook** - Intercepts the visibility check and forces `Visible=2` (Out-only) for hidden weapon categories
3. **Inline Hook** - When GlidingFix is enabled, a secondary hook on the PartAddShow function prevents hidden parts from briefly flashing visible during state transitions (e.g. exiting glide)
4. **Armor Injection** - Armor parts have no vanilla visibility entries, so the mod injects new map entries via the game's own internal functions
5. **BaldFix Hook** - Hooks the postfix rule evaluator and temporarily sets a priority bitmask override on equipped items per-call, making hair-hiding rules see them as inactive so hair stays visible. Only applies to the player context; NPCs are unaffected. Hair/beard may occasionally disappear; re-toggling the helmet (show then hide) restores it
6. **Input Management** - DetourModKit's input system handles keyboard, mouse, and gamepad with modifier key combos
7. **SEH Protection** - Hook callback is wrapped in structured exception handling (MSVC) to prevent crashes

## Troubleshooting

If you encounter issues:

1. Set `LogLevel = Debug` in the INI file
2. Check `CrimsonDesertEquipHide.log` in your game directory
3. After a game update, the mod may stop working due to memory layout changes

Common issues:

- **Mod doesn't load** – Ensure the files are in the correct location and ASI Loader is installed
- **Toggle doesn't work** – Check log file for AOB pattern errors
- **Game crashes** – Check log file for errors; try updating to the latest version

> **Still stuck?** [Open a GitHub issue](https://github.com/tkhquang/CrimsonDesertTools/issues/new?assignees=&labels=bug&template=bug_report.yaml) and include your INI config, log output, and game version.

## User Presets

The INI file includes three `UserPreset` sections (UserPreset1, UserPreset2, UserPreset3) that let you create custom part groups with independent visibility control.

When a preset is **enabled** and contains parts, it takes **full control** of those parts:

- Only the preset's own hotkey controls its parts
- Built-in category toggles (e.g., `V` for Shields/Helm/Mask) will **not** affect parts owned by an active preset
- Parts can appear in both a built-in category and a preset -- the preset wins when enabled
- Set `DefaultHidden = true` on a preset if you want its parts hidden on startup

```ini
[UserPreset1]
Enabled = true
ToggleHotkey = F5
DefaultHidden = true
Parts = CD_Helm, CD_Cloak, CD_Shoulder
```

In this example, pressing `V` would still toggle Shields and Masks, but Helm would only respond to `F5` because UserPreset1 owns it.

## Compatibility with Replacer Mods

Set `ForceShow = true` if you use mods that alter the default visibility of equipment via PAZ patching (e.g. character replacers like "Playing Kliff as Damiane", armor replacers, transmog mods). ForceShow overrides their changes so toggling equipment back to visible still works:

```ini
[General]
ForceShow = true
```

Safe to leave `false` if you are not using such mods.

## Known Issues

| Issue | Details | Workaround |
| ------- | --------- | ------------ |
| **Stuttering or FPS drops** | Some users report performance issues in crowded areas. *Not reproducible on my end.* | Try a different ASI Loader DLL (e.g. `version.dll` instead of `winmm.dll`) |
| **Flight cloak visual artifacts** | `CD_Cloak_Flight` variants cause artifacts and squished hair. Removed from defaults. | Re-add in INI at your own risk (see `[Cloak]` comment) |
| **Chest/Legs flickering** | Toggling chest and legs independently can cause pants to flicker | Bind both to the same hotkey, or use `DefaultHidden` instead of runtime toggling |
| **Dagger stays visible** | Daggers may remain visible and clip after hiding one-hand weapons | *Will be addressed in a future update* |
| **Toggle delay** | Visibility changes can take 1-3 seconds | -- |
| **Game updates** | Major updates may break the mod until a new version is released | -- |
| **Steam only** | Only tested with the Steam version | -- |

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for a detailed history of updates.

## Dependencies

This mod requires:

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by [**ThirteenAG**](https://github.com/ThirteenAG) – `winmm.dll` recommended (see [Installation](#installation) for alternatives)
- [DetourModKit](https://github.com/tkhquang/DetourModKit) – A lightweight C++ toolkit for game modding

## Building from Source

### Prerequisites

- [Visual Studio 2022](https://visualstudio.microsoft.com/) (MSVC with C++23 support)
- [CMake](https://cmake.org/) (3.16 or newer)
- Git (to fetch submodules)

### Release Build

```bash
git submodule update --init --recursive
cd CrimsonDesertEquipHide
cmake --preset msvc-release
cmake --build build/release-msvc --config Release --parallel
```

The output binary (`CrimsonDesertEquipHide.asi`) will be placed in the build directory.

### Dev Build (Hot-Reload)

The dev build produces a loader ASI + logic DLL pair, allowing you to rebuild and hot-reload the logic DLL without restarting the game.

```bash
cmake --preset msvc-dev
cmake --build build/dev-msvc --config RelWithDebInfo --parallel
```

The loader (`CrimsonDesertEquipHide.asi`) and logic DLL (`CrimsonDesertEquipHide_Logic.dll`) are deployed directly to the game's plugin directory via a post-build step. Press **Numpad 0** in-game to trigger a reload after rebuilding.

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) – for the Ultimate ASI Loader
- [cursey](https://github.com/cursey) – for SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) – for SimpleIni
- Pearl Abyss – for Crimson Desert

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
