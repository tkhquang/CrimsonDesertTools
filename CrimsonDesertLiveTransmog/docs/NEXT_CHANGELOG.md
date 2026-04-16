## Built-in Overlay (Removed ReShade Hard Requirement)

- The mod now includes its own overlay window. ReShade is no longer needed to use the in-game GUI
- If ReShade is installed, the mod still registers as a ReShade addon tab automatically
- New `ForceStandaloneOverlay` setting in INI to use the standalone window alongside ReShade
- Overlay toggle key is now configurable via `OverlayToggleHotkey` in INI (default: Home)
- Standalone overlay: press Esc to close, window follows game focus and hides on alt-tab
- Compatible with OptiScaler and other DXGI wrappers (no swap chain hooking)

## Human-Readable Item Names & Overlay Improvements

- Item picker now shows human-readable display names (e.g. "Kliff's Leather Armor" instead of "Kliff_Leather_Armor") with the internal name shown below in gray
- Search now matches both display names and internal names
- Item slot detection rewritten to be more accurate across the full catalog
- Name table scan no longer gives up after 15 attempts, it keeps retrying until the game is ready
