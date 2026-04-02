## Full gear hider with armor, accessories, and user presets

### Added

- Armor hiding — 9 new categories: Helm, Chest, Legs, Gloves, Boots, Cloak, Shoulder, Mask, Glasses
- Accessory hiding — 4 new categories: Earrings, Rings, Necklace, Bags (belt/bag parts)
- User Presets — 3 custom part groups (UserPreset1–3) with independent visibility control; presets take priority over built-in categories when enabled
- [Experimental] BaldFix — runtime hair-visibility fix when helmet or cloak is hidden; hooks the postfix rule evaluator to suppress hair-hiding rules (`_a`, `_c`, `_d`, `_f`, `_i`, `_q`, `_v`) without PAZ patching; enabled by default via `BaldFix = true` in `[General]`. Hair/beard may occasionally disappear; re-toggling the helmet (show then hide) restores it
- Gliding flash fix — inline hook on PartAddShow prevents shield/equipment from briefly flashing visible when exiting glide; controllable via `GlidingFix = true/false` in `[General]`
- Hot-reload cleanup — visibility bytes are restored to original values on DLL unload
- ForceShow fix — vis byte restore now works correctly even with `ForceShow = false`

### Improved

- Flat lookup table and classification bitset for O(1) part lookups
- Primary player cache to reduce per-frame pointer chain walks
- Default config ships with only Shields, Helm, and Mask enabled (`DefaultHidden = true`, `ToggleHotkey = V`)

### Changed

- Renamed from "Weapon Hider" to "Gear Hider" to reflect expanded scope (20 built-in categories + 3 user presets = 23 total)
- Updated DetourModKit to latest version
