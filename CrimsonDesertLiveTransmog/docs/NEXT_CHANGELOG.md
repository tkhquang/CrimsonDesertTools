## Overlay Unification

- Standalone and ReShade overlays now share a single rendering path, eliminating a class of widget-mismatch glitches when both ReShade and the standalone window were used in the same session.
- Updated bundled Dear ImGui to the latest stable release for smoother input handling and minor visual refinements.
- `ForceStandaloneOverlay = true` now strictly disables the ReShade addon tab and opens only the standalone window. Leave it false to keep the ReShade addon tab when ReShade is installed.
