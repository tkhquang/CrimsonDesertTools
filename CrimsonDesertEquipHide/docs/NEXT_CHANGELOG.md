## BaldFix Rework & CascadeFix

- Added `CascadeFix` option (disabled by default, experimental): reduces pants flickering when hiding chest armor, allowing bare-chested gameplay without visual glitches. Known limitations: hiding legs while chest is visible may not stick, and toggling pants independently may not work correctly.
- BaldFix reworked: now uses a per-call priority bitmask override instead of permanent state changes, hair-hiding rules are suppressed only for the player context when Helm, Cloak, or Mask is hidden, eliminating NPC hair clipping entirely
- Improved performance when toggling equipment visibility
- Reorganized internal code for easier maintenance and future updates
- Adopted newer DetourModKit APIs for faster and more reliable pattern scanning
