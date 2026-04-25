## [Title for next release]

- Migrated to DetourModKit 3.2.3 lifecycle helpers. `Bootstrap` collapses
  the dllmain process gate, instance mutex, and async-logger setup into
  a single call; the lifecycle worker thread is now owned by DMK.
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
- New optional INI key `[General] AutoReloadConfig=true` (default true)
  enables filesystem-watcher hot-reload of the INI. Setters re-fire on
  every save so most tweaks no longer require relaunching the game. Set
  to `false` to opt out.
- Atomic-bool config items use upstream `Config::register_atomic<bool>`,
  and the log-level lambda has been replaced with
  `Config::register_log_level`. User-visible behaviour is unchanged.
- AOB resolution now uses `DetourModKit::Scanner::resolve_cascade`
  directly. Mod behaviour is unchanged; the wrapper layer is shorter.
- `Transmog::shutdown()` calls `DMK_Shutdown()` for full DetourModKit
  teardown: every managed hook (`BatchEquip`, `VEC`, `PartAddShow`)
  is removed so SafetyHook restores the original prologue bytes, the
  InputManager poller is stopped and its bindings cleared, the
  `ConfigWatcher` is stopped, and the `Config` registered-items list
  is wiped. Workers are joined before the teardown so no background
  thread is mid-call into a SafetyHook trampoline when the trampoline
  pages are reclaimed. Each LT detour body (`on_vec`, `on_batch_equip`,
  `on_part_add_show`) snapshots its trampoline pointer at entry and
  bails to a benign default if the snapshot is null, defending the
  brief drain window between hook removal and DLL unmap. The next
  Logic-DLL `Init()` re-runs `Logger::configure`, `Config::register_*`,
  `HookManager::create_*_hook`, and `InputManager::start()` from a
  fresh DMK state, so dev hot reload starts each cycle from clean
  singletons.
