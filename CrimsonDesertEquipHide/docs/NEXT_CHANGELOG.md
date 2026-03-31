## Thread safety and hot-path performance improvements

### Fixed

- Fix potential permanent mutex deadlock in `apply_direct_vis_write` on SEH unwind — switched to manual `try_lock`/`unlock`
- Fix TOCTOU race on player slot caching in d8 fallback path — use `compare_exchange_weak`
- Fix hotkey toggle causing `apply_direct_vis_write` to run on every subsequent hook call indefinitely
- Fix silent `catch(...)` swallowing malformed hex IDs in INI parsing — now logs a warning

### Improved

- Avoid `lock xchg` (~25 cycles) on every hook call via load-before-exchange on `s_needsDirectWrite`
- Background threads now exit cleanly on DLL unload via `s_shutdownRequested` flag

### Added

- CMakePresets.json (msvc-release, mingw-release, msvc-dev, mingw-dev)
- Dev build two-DLL hot-reload infrastructure (loader stub + logic DLL)
