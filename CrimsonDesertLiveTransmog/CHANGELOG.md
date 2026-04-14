# Changelog

All notable changes to the CrimsonDesertLiveTransmog mod will be documented in this file.

## [0.3.1] - Cross-Character Armor Support & Carrier System Improvements

- NPC armor from other characters (Oongka, etc.) can now be worn by Kliff
- Items with non-player equip types are automatically detected and handled via the carrier system

## [0.3.0] - Instant Apply and Overlay Improvements

- Added **Instant Apply** mode - preview armor in real-time by hovering over items in the picker, no need to click Apply All
- Clicking an item or toggling a slot checkbox now applies immediately when Instant Apply is on
- Added **Keep Search Text** option - search field preserves your text between picker opens
- Added **X** button next to each slot for quick one-click clear
- Added **navigation buttons** (^/v) in the item picker for browsing without scrolling
- Changing one armor slot no longer causes all other slots to briefly flicker
- Unticking a slot then unequipping that armor in the inventory now correctly removes the visual
- Fixed a bug where clearing a slot could save "Pyeonjeon_Arrow" in the preset file instead of leaving it empty

## [0.2.0] - NPC and Damaged Armor Support

- **NPC and damaged armor variants now work** - items previously greyed out or invisible (like Antra, Aant, and other NPC-specific armors) can now be equipped as transmog. The mod automatically handles the conversion behind the scenes.
- **New item picker badges** - NPC/damaged items show a cyan `(carrier)` tag in the picker so you know they use the new system. Truly incompatible items (horse tack, pet armor) remain red.
- **New filter: "Hide variants"** - toggle visibility of NPC/damaged items in the picker. Off by default so you can see everything.
- **"Safe only" filter renamed** - the old "Hide variants" filter is now "Safe only" and only hides items that would crash the game (non-humanoid gear).
- **Fixed: real armor not hiding on game load** - when slots were set to active with no item selected, real armor would briefly show on load. Fixed.
- **Fixed: unticking a slot now restores real armor** - previously unticking a slot checkbox could leave the slot empty. Now the real equipped item reappears.
- **Fixed: switching presets with NPC items** - switching from an NPC preset to a normal preset no longer leaves ghost meshes from the previous preset.

## [0.1.0] - Initial release (BETA)

- Runtime armor transmog via SlotPopulator hook with ReShade overlay GUI, preset system, safety filters, and per-slot control
- Modular code split: `transmog_apply` (apply/clear logic), `transmog_hooks` (VEC/BatchEquip callbacks), `transmog_worker` (debounce/load-detect/nametable threads), `shared_state` (cross-TU atomics)
- Docs clarify ReShade is required for GUI; without it, users edit JSON manually via Capture hotkey

[0.3.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.3.1
[0.3.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.3.0
[0.2.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.2.0
[0.1.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.1.0
