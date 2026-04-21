CrimsonDesertLiveTransmog
=========================
Runtime visual armor swap (transmog) for Crimson Desert.
Changes the appearance of equipped armor without affecting stats.
Supports Kliff, Damiane, and Oongka with per-character presets
that swap automatically when you change who you control.

BETA: This mod injects visual data into the game's equipment pipeline
at runtime. Use it for aesthetics and screenshots -- side effects may
exist. Install at your own risk.

Author:   tkhquang
Source:   https://github.com/tkhquang/CrimsonDesertTools
Nexus:    https://www.nexusmods.com/crimsondesert/mods/25
Issues:   https://github.com/tkhquang/CrimsonDesertTools/issues

Requirements
------------
- Crimson Desert (PC, Steam)
- ASI Loader (e.g. Ultimate ASI Loader)
  https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases
- ReShade is OPTIONAL. If present, the overlay registers as a ReShade
  addon tab. If not, the mod provides its own standalone overlay.
  https://reshade.me/

Installation
------------
1. Install an ASI Loader (skip if you already have one).
2. Copy ALL of the following into <Game Folder>/bin64/:
     - CrimsonDesertLiveTransmog.asi
     - CrimsonDesertLiveTransmog.ini
     - CrimsonDesertLiveTransmog_display_names.tsv
     - CrimsonDesertLiveTransmog_Acknowledgements.txt (optional)
     - CrimsonDesertLiveTransmog_Readme.txt          (optional)
3. Launch the game. Press Home to open the overlay.

IMPORTANT: The .tsv file MUST sit next to the .asi. It maps internal
item ids to human-readable display names in the picker. Without it,
the picker shows raw ids only.

Usage
-----
1. Press Home to open the overlay (ReShade tab or standalone window).
2. Click a slot picker (Helm, Chest, Cloak, Gloves, Boots) to browse
   armor. Use the search box to filter by name.
3. Select an item -- changes stay pending until you click "Apply All".
4. Click "Apply All" to commit all slot changes at once.
5. Use "Append" in the Presets section to save the current look as a
   preset. Use "Prev" / "Next" to cycle presets.

Tip: enable "Instant Apply" in the overlay to preview items on hover
without clicking Apply All.

Without the overlay: use the online preset builder
https://tkhquang.github.io/CrimsonDesertTools/live-transmog/
or edit CrimsonDesertLiveTransmog_presets.json by hand.

Configuration
-------------
Edit CrimsonDesertLiveTransmog.ini for global settings and optional
hotkeys. All hotkeys are DISABLED by default -- the overlay is the
primary interface. Set bindings in the INI if you prefer keyboard
shortcuts.

Presets are stored in CrimsonDesertLiveTransmog_presets.json, which
is auto-generated alongside the INI on first launch.

Logs
----
Check CrimsonDesertLiveTransmog.log for troubleshooting.
Set LogLevel = Debug or Trace for detailed output.

Uninstall
---------
Delete the following from your bin64 folder:
  - CrimsonDesertLiveTransmog.asi
  - CrimsonDesertLiveTransmog.ini
  - CrimsonDesertLiveTransmog.log
  - CrimsonDesertLiveTransmog_presets.json
  - CrimsonDesertLiveTransmog_display_names.tsv
  - CrimsonDesertLiveTransmog_Acknowledgements.txt
  - CrimsonDesertLiveTransmog_Readme.txt

Known Limitations
-----------------
- BETA: side effects may exist -- use for aesthetics/screenshots.
- Some armor in the picker may not render (other body types, damaged
  variants). Safety filters are on by default.
- Wrong-slot or non-equipment items will crash the game. Do not
  disable safety filters unless you know what you are doing.
- Non-humanoid items (horse tack, pet armor) crash the mesh binder.
  The "Safe only" filter hides these by default.
- Dye is not supported yet.
- Special-effect armor (e.g. particle helmets) may have visual quirks.
- Hair may clip through some helmets.
- Major game updates may break the mod until a new version ships.
- Only tested with the Steam version.

Third-Party Notices
-------------------
See CrimsonDesertLiveTransmog_Acknowledgements.txt for third-party
software licenses (SafetyHook, Zydis, SimpleIni, Dear ImGui,
DirectXMath, ReShade addon SDK).
