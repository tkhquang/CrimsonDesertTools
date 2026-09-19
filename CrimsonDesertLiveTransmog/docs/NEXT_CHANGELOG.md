## Languages, overlay and fixes

- Item names now available in all 15 of the game's languages, and the mod's own text in Simplified Chinese, from two dropdowns at the top of the Transmog tab
- Both come from a new optional language pack download, so the main download is unchanged in size
- Non-Latin text now renders in both the standalone overlay and the ReShade tab, with a new **FontPath** setting in the INI if you want a different font
- **UI Scale** now works in the ReShade tab too, as a free slider instead of fixed steps
- UI Scale, **Instant Apply** and **Keep Search Text** are remembered between sessions
- The preset list is now scrollable and resizable, so a long list no longer pushes the buttons off screen
- Tooltips now wrap instead of running off the screen, and slot columns follow the text size
- You can rename individual items yourself with an override file that an update will not overwrite
- The Status line is gone. A warning now appears only when the mod cannot apply anything on your game build
- Fixed a crash when switching presets while several transmogged pieces were being removed
- Fixed real items vanishing, or staying on screen after you unequipped them, around presets with every slot unticked
- Fixed transmogged earrings and rings ending up on the wrong side, and only one of a pair coming back
- Fixed changing an overlay setting quietly saving unsaved dye edits into the active preset
- Fixed a blank row appearing in the character dropdown
- Made the mod's game-code detection more tolerant of game updates
