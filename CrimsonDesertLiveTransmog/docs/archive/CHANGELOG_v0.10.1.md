## Overlay Unification

- Standalone and ReShade overlays now share a single rendering path, eliminating a class of widget-mismatch glitches when both ReShade and the standalone window were used in the same session.
- Updated bundled Dear ImGui to the latest stable release for smoother input handling and minor visual refinements.
- `ForceStandaloneOverlay = true` now strictly disables the ReShade addon tab and opens only the standalone window. Leave it false to keep the ReShade addon tab when ReShade is installed.
- Internal cleanup of overlay UI code (no functional change).
- The standalone overlay is much more responsive: clicks, hovers, and preset switches no longer lag. The overlay now repaints only the area Dear ImGui actually drew (instead of the full game window) and bumps its frame rate up while you're interacting with it.
- Fixed: Color Override swatch circles now reliably appear after game load (previously needed a preset switch to show up).
- Fixed: Switching presets no longer risks wiping saved Color Override entries when those entries had not yet finished loading.
- Fixed: Overlay window no longer shrinks then re-expands when switching between presets with and without Dye / Color Override applied.
- Fixed: The hex item-id field next to each slot picker no longer clips its last digit at larger UI scales.
- Picker now lists items whose internal names contain special characters (a handful of Goblin Merchant gear that was previously missing).
- Refreshed the bundled display-name table to match the latest in-game text (about 119 entries updated, e.g. crafting recipe names and currency labels).
