## [Title for next release]

- Migrated to DetourModKit 3.2.3 lifecycle helpers. `Bootstrap` collapses
  the dllmain process gate, instance mutex, and async-logger setup into
  a single call; the three background workers run on `StoppableWorker`
  with cooperative `std::stop_token` shutdown.
- Hotkey bindings go through `DMK::Config::register_press_combo`, which
  fuses the INI key registration with `InputManager::register_press()`.
  Existing user INI hotkey values continue to work without changes. An
  empty INI value or the literal `NONE` (case-insensitive, whole-string
  only) is recognised as a silent opt-out sentinel by DMK at parse time,
  so unbound bindings produce no log noise on any `Config::reload()`
  cycle. A live INI edit that assigns a real combo still goes live on
  the next reload, and a non-empty value whose every comma-separated
  token fails to parse still trips DMK's INI-typo WARNING with the
  offending raw string.
- Behaviour change: when multiple categories share a Toggle hotkey,
  each category now flips its own state independently. The previous
  `IndependentToggle=false` "flip all together" semantics depended on a
  manual de-dupe pass that DetourModKit's per-binding cancellation
  guards have superseded. Bind the same combo to `[General] HideAll` /
  `[General] ShowAll` for a synchronised toggle.
- New optional INI key `[General] AutoReloadConfig=true` (default true)
  enables filesystem-watcher hot-reload of the INI. Setters re-fire on
  every save so most tweaks no longer require relaunching the game.
  Set to `false` to opt out. The reload callback re-derives the cached
  hidden-state masks and hands the actual vis-byte commit to the game
  thread via the same needs-direct-write primitive that drives
  character-swap re-application, so edits to `[Helm]/[Chest]/...`
  `Enabled` / `DefaultHidden` flags, and to `Parts=` lists (including
  per-character overrides such as `[Chest:Damiane] Parts=NONE`), take
  effect within one mid-hook tick of the save. The `Parts=` setter
  writes into the inactive lookup buffer; the reload tail calls
  `rebuild_part_lookup()` to atomically publish the rebuilt buffer so
  the mid-hook classifier and the direct-write pass see the new part
  list on the next tick.
- Atomic-bool config items use upstream `Config::register_atomic<bool>`,
  and the log-level lambda has been replaced with
  `Config::register_log_level`. User-visible behaviour is unchanged.
- AOB resolution now uses `DetourModKit::Scanner::resolve_cascade`
  directly. Mod behaviour is unchanged; the wrapper layer is shorter.
  The `EquipVisCheck` mid-hook target is routed through
  `Scanner::resolve_cascade_with_prologue_fallback`, matching every
  other AOB call site, so when SafetyHook's prior detour-jump still
  occupies the mid-body bytes the resolver retries each candidate
  with the first five byte-tokens replaced by the near-JMP signature
  and lands on the same cmp instruction. Production first-load is
  unchanged: the original-bytes patterns still win the cascade.
- `EquipHide::shutdown()` calls `DMK_Shutdown()` for full DetourModKit
  teardown: every managed hook (`EquipVisCheck`, `PartAddShow`,
  `PostfixEval`, `VisualEquipChange`, `VisualEquipSwap`) is removed
  so SafetyHook restores the original bytes at each patched site,
  the InputManager poller is stopped and its bindings cleared, the
  `ConfigWatcher` is stopped, and the `Config` registered-items list
  is wiped. Background workers are joined before the teardown so no
  thread is mid-call into a SafetyHook trampoline when the trampoline
  pages are reclaimed; visibility-byte cleanup also runs before
  `DMK_Shutdown()` so the cleanup walk completes while its host code
  is still mapped. Each detour body snapshots its trampoline pointer
  at entry and bails to a safe default if the snapshot is null,
  defending the brief drain window between hook removal and DLL
  unmap. The next Logic-DLL `Init()` re-runs `Logger::configure`,
  `Config::register_*`, `HookManager::create_*_hook`, and
  `InputManager::start()` from a fresh DMK state, so dev hot reload
  starts each cycle from clean singletons.
