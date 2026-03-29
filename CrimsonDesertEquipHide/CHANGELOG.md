# Changelog

All notable changes to the CrimsonDesertEquipHide mod will be documented in this file.

## [0.2.2] - Runtime hash resolution for patch-proof equipment detection

### Added

- Runtime IndexedStringA table scan at init — part name-to-hash resolution is now automatic, eliminating the need to manually update hardcoded hash IDs after each game patch
- Targeted outlier probe for parts with non-deterministic hash slots (e.g. CD_Tool_Book), searching a ±0x100 window around fallback hashes
- Dynamic range filter with automatic outlier detection — contiguous hash block bounds and outlier set are computed from resolved hashes at startup
- Background thread for deferred table scan with retry logic — avoids blocking the game thread during loading

### Improved

- Replaced per-entry is_readable (VirtualQuery) calls with SEH exception handling in the table scanner — scan time reduced from 300-900ms to <1ms
- Thread-safe double-buffered part lookup map — eliminates data race between background scan thread and hook callback

### Fixed

- Fix equipment hide not working on game v1.01.00 — the patch inserted ~60 new entries into the IndexedStringA table, shifting all 98 equipment part hash IDs
- Fix DefaultHidden intermittently not applying on cold boot — added 60-second startup direct-write enforcement that proactively writes visibility bytes after init, covering the race where the game processes initial equipment visibility before the hook is installed or the PlayerOnly filter has transitional pointer data
- Direct-write visibility updates now work in d8-based fallback mode, improving hotkey responsiveness when the global pointer chain is unavailable
- Fix toggling a category to visible forcing all its parts to always-show (vis=0) — the direct-write path now caches each part's original vis byte before overriding and restores it on un-hide, preserving the game's per-part-type render routing (vis=0 for shields rendered through the PartInOut path, vis=3 for weapons rendered by the animation system). This fixes daggers/knives becoming permanently visible after any hide/show toggle cycle

## [0.2.1] - Fixed equipment hash IDs for game v1.01.00

- Fix equipment hide not working on game v1.01.00 — the patch inserted ~60 new entries (bags, accessories) into the IndexedStringA table, shifting all 98 equipment part hash IDs upward and causing most categories (1H weapons, shields, bows, special weapons, lanterns) to fail silently while tools and 2H weapons appeared to work by coincidence
- Update range filter bounds from

## [0.2.0] - Player-Only Mode, Show/Hide Hotkeys, and Reliable Hook Init

### Added
- Player-only filtering mode that restricts equipment hiding to player characters using a global pointer chain with d8-based fallback
- Per-category ShowHotkey / HideHotkey bindings alongside existing toggle
- Global ShowAllHotkey / HideAllHotkey overrides for one-press show/hide
- Direct-write visibility updates via map lookup for immediate visual feedback on hotkey press
- Complete equipment part registry covering all known part hashes, removing hardcoded defaults
- Process-wide executable memory scan (VirtualQuery over all PAGE_EXECUTE_READ regions) to find hook targets in VirtualAlloc'd code outside the main module
- Per-process named mutex to detect duplicate ASI instances
- SyncFallback async logger overflow policy to prevent message drops
- Fail-fast when hook installation fails (no hotkeys, no input thread)
- Centralized constants into constants.hpp

### Fixed
- Suppress spurious empty-parts warning during config registration
- Hook init no longer relies on SizeOfImage polling or module-only AOB scan, which failed because the game's protector unpacks code into regions outside the 0.6 MB main module stub
- Gate on process name before logger init to prevent crashpad_handler and other non-game processes from truncating the log file

### Changed
- Upgraded DetourModKit to v2.1.1
- Removed redundant InitDelayMs config and retry loop

### Docs
- Updated installation docs with ASI loader instructions

## [0.1.0] - Initial Release

- Hide equipped weapons, shields, bows, tools, and lanterns with a configurable hotkey
- Per-category toggle with independent hotkey bindings
- Cascading AOB pattern scanning for resilience across game updates
- SEH-protected hook callback to prevent crashes if mod is outdated
- Full keyboard, mouse, and gamepad (XInput) hotkey support with modifier combos
- Customizable part lists per category via INI configuration
- Configurable init delay and log level

[0.2.2]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/v0.2.2
[0.2.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/v0.2.1
[0.2.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/v0.2.0
[0.1.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/v0.1.0
