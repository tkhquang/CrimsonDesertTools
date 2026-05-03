CRIMSON DESERT - EQUIP HIDE
Version 0.7.1

REQUIREMENTS:
- Crimson Desert (Steam version)
- Ultimate ASI Loader (x64) from:
  https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases

FEATURES:
- Toggle, force-show, or force-hide equipment visibility per category
- Weapon categories: One-Hand Weapons, Two-Hand Weapons, Shields,
  Bows, Special Weapons, Tools, Lanterns
- Armor categories: Helm, Chest, Legs, Gloves, Boots, Cloak,
  Shoulder, Mask, Glasses, Accessories
- 3 User Presets: custom part groups combining parts from any category
- Global show-all / hide-all hotkeys
- Fully customizable settings through INI configuration

INSTALLATION:

  Step 1: Install an ASI Loader
  -----------------------------
  Download the x64 build of Ultimate ASI Loader and place one of the
  following DLLs into your bin64 folder:

    winmm.dll   - Recommended, works for most setups
    version.dll - Alternative if winmm.dll conflicts
    dinput8.dll - Another alternative

  How to verify: After completing Step 2, launch the game once. If no
  CrimsonDesertEquipHide.log file appears in bin64, the ASI Loader is
  not loading. Try renaming the DLL to one of the other variants
  listed above.

  Step 2: Install the mod
  -----------------------
  Extract CrimsonDesertEquipHide.asi and CrimsonDesertEquipHide.ini
  into your bin64 folder.

  Step 3: Launch and play
  -----------------------
  Launch the game. Press V (default) to toggle equipment visibility.
  By default, the INI is configured to hide Shields, Helms, and Masks
  on game load.

  File placement:

    <Crimson Desert>/bin64/
    ├── CrimsonDesert.exe              (Game executable)
    ├── winmm.dll                      (ASI Loader)
    ├── CrimsonDesertEquipHide.asi     (This mod)
    ├── CrimsonDesertEquipHide.ini     (This mod's configuration)
    └── ...

USING WITH OPTISCALER:

  If you use OptiScaler for frame generation or upscaling, the standard
  ASI Loader (e.g. winmm.dll) will conflict with OptiScaler's dxgi.dll.
  Follow these steps instead:

  1. Delete winmm.dll (or whichever ASI Loader DLL you placed) from
     bin64 -- OptiScaler will handle mod loading instead
  2. Open OptiScaler.ini, find the [Plugins] section, and set
     LoadAsiPlugins = true
  3. Create a plugins folder inside bin64
  4. Move CrimsonDesertEquipHide.asi and CrimsonDesertEquipHide.ini
     into the plugins folder

    <Crimson Desert>/bin64/
    ├── CrimsonDesert.exe
    ├── OptiScaler.ini                 (LoadAsiPlugins = true)
    ├── dxgi.dll                       (OptiScaler)
    ├── plugins/
    │   ├── CrimsonDesertEquipHide.asi
    │   ├── CrimsonDesertEquipHide.ini
    │   └── ...                        (Other ASI mods go here too)
    └── ...

  This applies to any ASI mod. If you have other .asi mods (e.g.
  CDSprintHold), move them into the plugins folder as well.

CONFIGURATION:
Edit CrimsonDesertEquipHide.ini to customize hotkeys, categories,
and default visibility. See the full documentation at:
https://github.com/tkhquang/CrimsonDesertTools

TROUBLESHOOTING:
- Set LogLevel = Debug in the INI file
- Check CrimsonDesertEquipHide.log for details
- If hooks fail to install, the mod will log an error and remain inert
  (the game continues normally without equipment hiding)

SOURCE & SUPPORT:
https://github.com/tkhquang/CrimsonDesertTools