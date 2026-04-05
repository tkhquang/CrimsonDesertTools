## Improvements

- BaldFix reworked: now uses a per-call priority bitmask override instead of permanent state changes, hair-hiding rules are suppressed only for the player context when Helm or Cloak is hidden, eliminating NPC hair clipping entirely
- Improved performance when toggling equipment visibility
- Reorganized internal code for easier maintenance and future updates
- Adopted newer DetourModKit APIs for faster and more reliable pattern scanning
