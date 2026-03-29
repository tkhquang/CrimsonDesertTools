## Runtime hash resolution for patch-proof equipment detection

### Added

- Runtime IndexedStringA table scan at init — part name-to-hash resolution is now automatic, eliminating the need to manually update hardcoded hash IDs after each game patch
- Targeted outlier probe for parts with non-deterministic hash slots (e.g. CD_Tool_Book), searching a ±0x100 window around fallback hashes
- Dynamic range filter with automatic outlier detection — contiguous hash block bounds and outlier set are computed from resolved hashes at startup
- Background thread for deferred table scan with retry logic — avoids blocking the game thread during loading

### Improved

- Replaced per-entry `is_readable` (VirtualQuery) calls with SEH exception handling in the table scanner — scan time reduced from 300-900ms to <1ms
- Thread-safe double-buffered part lookup map — eliminates data race between background scan thread and hook callback

### Fixed

- Fix equipment hide not working on game v1.01.00 — the patch inserted ~60 new entries into the IndexedStringA table, shifting all 98 equipment part hash IDs
- Fix DefaultHidden intermittently not applying on cold boot — added 60-second startup direct-write enforcement that proactively writes visibility bytes after init, covering the race where the game processes initial equipment visibility before the hook is installed or the PlayerOnly filter has transitional pointer data
- Direct-write visibility updates now work in d8-based fallback mode, improving hotkey responsiveness when the global pointer chain is unavailable
- Fix toggling a category to visible forcing all its parts to always-show (vis=0) — the direct-write path now caches each part's original vis byte before overriding and restores it on un-hide, preserving the game's per-part-type render routing (vis=0 for shields rendered through the PartInOut path, vis=3 for weapons rendered by the animation system). This fixes daggers/knives becoming permanently visible after any hide/show toggle cycle
