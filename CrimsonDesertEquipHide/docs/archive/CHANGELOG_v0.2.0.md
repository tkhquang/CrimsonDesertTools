## Player-Only Mode, Show/Hide Hotkeys, and Reliable Hook Init

### Added
- Player-only filtering mode that restricts equipment hiding to player characters using a global pointer chain with d8-based fallback
- Per-category `ShowHotkey` / `HideHotkey` bindings alongside existing toggle
- Global `ShowAllHotkey` / `HideAllHotkey` overrides for one-press show/hide
- Direct-write visibility updates via map lookup for immediate visual feedback on hotkey press
- Complete equipment part registry covering all known part hashes, removing hardcoded defaults
- Process-wide executable memory scan (VirtualQuery over all PAGE_EXECUTE_READ regions) to find hook targets in VirtualAlloc'd code outside the main module
- Per-process named mutex to detect duplicate ASI instances
- SyncFallback async logger overflow policy to prevent message drops
- Fail-fast when hook installation fails (no hotkeys, no input thread)
- Centralized constants into `constants.hpp`

### Fixed
- Suppress spurious empty-parts warning during config registration
- Hook init no longer relies on SizeOfImage polling or module-only AOB scan, which failed because the game's protector unpacks code into regions outside the 0.6 MB main module stub
- Gate on process name before logger init to prevent `crashpad_handler` and other non-game processes from truncating the log file

### Changed
- Upgraded DetourModKit to v2.1.1
- Removed redundant `InitDelayMs` config and retry loop

### Docs
- Updated installation docs with ASI loader instructions
