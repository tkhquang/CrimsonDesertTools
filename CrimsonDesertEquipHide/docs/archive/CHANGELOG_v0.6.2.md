## Multi-Protagonist Support and 1.04.00 Reliability

- Per-character armor-hide settings now stick when you swap protagonists; hiding gear on one party member no longer flips back when you switch to another.
- Per-character Parts overrides are now honoured by the live mid-hook (not just the resync pass), so a hash excluded from one protagonist's `[Section:CharName]` list will no longer be momentarily hidden on that protagonist's frames.
- INI template now lists the exact `CD_*` part names alongside each per-character override comment, so users can copy-paste names without guessing what "R-daggers" or "fire can" map to.
- Detect all three party members on game version 1.04.00 (the third character was previously missing the per-character hide mask).
- Toggle hotkeys reliably apply right after a character swap; presses are no longer silently dropped on internal lock contention.
- Equipment hash table now retries until your world finishes loading, so worlds with extremely sparse table state at startup still converge to the full hide list.
- Hot-reload during development no longer hangs the game on shutdown.
- Quieter logs: per-binding registration and per-part trace lines moved off the default INFO/DEBUG output.
- Migrated to DetourModKit 3.2.3 lifecycle helpers; existing INI files keep working unchanged.
- INI auto-reload (default on) -- most edits to category Enabled/DefaultHidden/Parts/per-character overrides take effect within a single frame, no relaunch required. Disable via `[General] AutoReloadConfig=false`.
- Categories sharing the same `ToggleHotkey` now flip independently (each on its own state). Bind the same combo to `[General] HideAll`/`ShowAll` for a synchronised toggle.
- Cleaner shutdown: full DetourModKit teardown removes every hook and restores patched bytes, so dev hot-reload starts each cycle from a clean state.
