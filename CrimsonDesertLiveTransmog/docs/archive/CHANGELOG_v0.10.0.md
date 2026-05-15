## [Experimental] Color Override

- New experimental "Color Override" picker paints transmog items (including outfits the in-game dye merchant refuses) with a custom colour per visible region.
- Color Override is OFF by default; enable it by setting `ColorOverride = true` under a new `[Experimental]` section in the INI, then relaunch the game.
- New per-slot "Sync from live" button captures the dye you applied to a real item in-game and stores it in the active preset for that slot.
- Cleanup pass on the existing dye apply path so unset slots no longer silently inherit your real item's colours.
- New "Save as New" button in the Presets section forks your pending edits into a brand-new preset without overwriting the one you started from.
- Copy now clones the active preset's saved state only; pending picker edits are discarded so you get a clean baseline. Use "Save as New" if you want to keep your pending edits.
- Prefab picker's Exact filter is stricter: knowledge images and other UI assets that happened to share a body-mesh name no longer leak into the list.
- More reliable character detection on save-load and radial swap; the active preset now follows your real protagonist across reloads without needing a manual swap-and-back.
