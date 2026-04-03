## Performance & Stability Improvements

### Fixed

- Reduced micro-stuttering caused by frequent memory safety checks in the hot path
- Fixed game thread stalling when pressing hotkeys during heavy scenes
- Fixed equipment parts not updating after editing the INI and hot-reloading (dev builds)
- Fixed shutdown corrupting visibility state of unrelated equipment parts
- Fixed repeated log entries for already-resolved parts during startup scan

### Changed

- Removed flight cloak parts (CD_Cloak_Flight) from default Cloak category to prevent visual artifacts and squished hair when toggling cloak visibility
- Unified address format in log output to uppercase hex
- Added per-part trace logging when toggling visibility (visible at Trace log level)
- Added category summary after config load (visible at Trace log level)

### Added

- Expanded user presets from 3 to 10 slots
- Added workaround tip in INI for dagger clipping issue
- Documented known issues: flight cloak artifacts, chest/legs flickering, dagger clipping
