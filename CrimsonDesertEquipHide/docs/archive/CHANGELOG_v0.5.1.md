## Upgrade to DetourModKit v3.0.0

### Internal

- Upgraded DetourModKit dependency from v2.2.0 to v3.0.0 (std::expected API, typed hook errors, AVX2 scanner, deterministic shutdown)
- Bumped CMake minimum to 3.25; added SYSTEM subdirectory to suppress dependency header warnings
- Fixed [[nodiscard]] violation on memory cache initialization
- Fixed linker warning (LNK4075) from INCREMENTAL/ICF conflict in dev builds
- Reduced amour flashing issues when CascadeFix is on
