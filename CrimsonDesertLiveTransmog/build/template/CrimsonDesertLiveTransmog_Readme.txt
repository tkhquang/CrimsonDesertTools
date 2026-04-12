CrimsonDesertLiveTransmog
=========================
Runtime visual armor swap (transmog) for Crimson Desert.
Changes the appearance of equipped armor without affecting stats.

BETA: This mod injects visual data into the game's equipment pipeline
at runtime. Use it for aesthetics and screenshots  - side effects may
exist. Install at your own risk.

Author:  tkhquang
Source:  https://github.com/tkhquang/CrimsonDesertTools

Requirements
------------
- Crimson Desert (PC, Steam)
- ReShade (for the overlay GUI  - mod works via hotkeys without it)
- ASI Loader (e.g. Ultimate ASI Loader)

Installation
------------
1. Install ReShade for Crimson Desert (https://reshade.me/)
2. Copy "CrimsonDesertLiveTransmog.asi" and
   "CrimsonDesertLiveTransmog.ini" into:
     <Game Folder>/bin64/
3. Launch the game. Press Home to open ReShade, find the Transmog tab.

Usage
-----
1. Open ReShade overlay (Home key), find the Transmog tab
2. Click a slot picker (Helm, Chest, etc.) to browse armor
3. Search by name, select an item  - changes stay pending
4. Click "Apply All" to commit all slot changes
5. Use "Append" to save your look as a preset

Configuration
-------------
Edit CrimsonDesertLiveTransmog.ini for global settings and hotkeys.
All hotkeys are DISABLED by default  - the ReShade overlay is the
primary interface. Set bindings in the INI if you prefer keyboard
shortcuts.

Presets are stored in CrimsonDesertLiveTransmog_presets.json
(auto-generated alongside the INI).

Logs
----
Check CrimsonDesertLiveTransmog.log for troubleshooting.
Set LogLevel = Debug or Trace for detailed output.

Uninstall
---------
Delete CrimsonDesertLiveTransmog.asi, .ini, .log, and
_presets.json from your game folder.

Known Limitations
-----------------
- BETA: side effects may exist  - use for aesthetics/screenshots
- Some armor in the picker may not render (other body types, damaged
  variants). Safety filters are on by default.
- Wrong-slot or non-equipment items will crash the game. Do not
  disable safety filters unless you know what you are doing.
- Dye is not supported yet.
- Special-effect armor (e.g. particle helmets) may have visual quirks.
- Hair may clip through some helmets.
- Major game updates may break the mod.
- Only tested with the Steam version.

Third-Party Notices
-------------------
See CrimsonDesertLiveTransmog_Acknowledgements.txt for third-party
software licenses.
