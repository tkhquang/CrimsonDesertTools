## [Experimental] Color Override

- New experimental "Color Override" picker paints transmog items (including outfits the in-game dye merchant refuses) with a custom colour per visible region.
- Color Override is OFF by default; enable it by setting `ColorOverride = true` under a new `[Experimental]` section in the INI, then relaunch the game.
- New per-slot "Sync from live" button captures the dye you applied to a real item in-game and stores it in the active preset for that slot.
- Cleanup pass on the existing dye apply path so unset slots no longer silently inherit your real item's colours.
