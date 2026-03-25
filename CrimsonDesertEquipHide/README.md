# Crimson Desert - Equip Hide

## Overview

**CrimsonDesertEquipHide** is an ASI plugin for Crimson Desert that allows players to hide equipped gear visually using customizable hotkeys. Toggle visibility of weapons, shields, bows, tools, and lanterns per category — useful for cleaner screenshots, aesthetic character builds, or simply enjoying the world without gear clutter.

## Features

- Toggle equipment visibility per category with hotkeys (default: V)
- Categories: One-Hand Weapons, Two-Hand Weapons, Shields, Bows, Special Weapons, Tools, Lanterns
- Per-category configuration: enable/disable, hotkey, default hidden state, part list
- Cascading AOB pattern scanning for resilience across game updates
- SEH-protected hook callback to prevent crashes if mod is outdated
- Fully customizable settings via INI configuration

## Installation

1. Make sure you have an ASI Loader installed for Crimson Desert. If you don't have one, download `version.dll` from the [Ultimate ASI Loader releases](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) and place it in your Crimson Desert game directory (where the game executable is located).
2. Download the latest release from the [GitHub Releases page](https://github.com/tkhquang/CrimsonDesertTools/releases)
3. Extract all files to your Crimson Desert game directory:

   ```text
   <Crimson Desert installation folder>/bin64/
   ```

   Example: `D:\Games\SteamLibrary\steamapps\common\Crimson Desert\bin64`

4. Launch the game and use the configured hotkey (default: V) to toggle equipment visibility

## Configuration

The mod is configured via the `CrimsonDesertEquipHide.ini` file:

```ini
[General]
; Delay in milliseconds before scanning for hooks (game needs time to unpack)
InitDelayMs = 3000
; Log level: Trace, Debug, Info, Warning, Error
LogLevel = Info

[OneHandWeapons]
Enabled = true
Hotkey = V
DefaultHidden = false
Parts = CD_MainWeapon_Sword_R, CD_MainWeapon_Sword_IN_R, ...

[Shields]
Enabled = true
Hotkey = V
DefaultHidden = false
Parts = CD_MainWeapon_Shield_L, CD_MainWeapon_Shield_R, ...
```

### Hotkey Format

- Named keys: `V`, `F3`, `Numpad1`, `Mouse4`, `Gamepad_A`, etc.
- Modifiers: `Ctrl`, `Shift`, `Alt` (or `LCtrl`, `RCtrl`, etc.)
- Multiple combos: `V,Gamepad_LB+Gamepad_Y` (V alone OR hold LB + press Y)
- Empty `Hotkey =` disables toggle for that category

See the full list at the [Supported Input Names](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#supported-input-names) reference.

### Using with Controllers

DetourModKit supports gamepad input natively via the **XInput** API:

```ini
Hotkey = Gamepad_Y
Hotkey = Gamepad_LB+Gamepad_Y
Hotkey = V,Gamepad_LB+Gamepad_Y
```

> **XInput only:** Xbox controllers work natively. For PS4/PS5/Switch controllers, use an XInput translation layer (e.g. DS4Windows, DualSenseX, BetterJoy) or Steam Input. See [Gamepad Compatibility](https://github.com/tkhquang/DetourModKit?tab=readme-ov-file#gamepad-compatibility) for details.

## How It Works

1. **AOB Pattern Scanning** – Cascading patterns scan the game's memory to locate the visibility decision function
2. **Mid-Hook** – Intercepts the visibility check and forces `Visible=2` (Out-only) for hidden equipment categories
3. **Input Management** – DetourModKit's input system handles keyboard, mouse, and gamepad with modifier key combos
4. **SEH Protection** – Hook callback is wrapped in structured exception handling (MSVC) to prevent crashes

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

## Known Limitations

- Toggle may take 1-3 seconds to take effect — the game caches mesh visibility and re-evaluates periodically, not per-frame
- After a major game update, AOB patterns may need updating
- Currently only tested with the Steam version of the game

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for a detailed history of updates.

## Dependencies

This mod requires:

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by [**ThirteenAG**](https://github.com/ThirteenAG) – `version.dll` variant required
- [DetourModKit](https://github.com/tkhquang/DetourModKit) – A lightweight C++ toolkit for game modding

## Building from Source

### Prerequisites

- [MinGW-w64](https://www.mingw-w64.org/) (GCC 12+) or [Visual Studio 2022](https://visualstudio.microsoft.com/) (MSVC with C++23 support)
- [CMake](https://cmake.org/) (3.16 or newer)
- [Ninja](https://ninja-build.org/) (recommended)
- Git (to fetch submodules)

### Building with MinGW (Recommended)

```bash
git submodule update --init --recursive
cd CrimsonDesertEquipHide
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build --config Release --parallel
```

### Building with MSVC

```bash
git submodule update --init --recursive
cd CrimsonDesertEquipHide
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

The output binary (`CrimsonDesertEquipHide.asi`) will be placed in the build directory.

## Credits

- [ThirteenAG](https://github.com/ThirteenAG) – for the Ultimate ASI Loader
- [cursey](https://github.com/cursey) – for SafetyHook
- [Brodie Thiesfield](https://github.com/brofield) – for SimpleIni
- Pearl Abyss – for Crimson Desert

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
