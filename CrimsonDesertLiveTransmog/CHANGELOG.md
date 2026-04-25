# Changelog

All notable changes to the CrimsonDesertLiveTransmog mod will be documented in this file.

## [0.6.3] - DetourModKit 3.2.3 refresh

- Faster, lighter mod startup and shutdown.
- INI hotkey changes now apply on save without relaunching the game.
- Set a hotkey to empty or `NONE` to leave it unbound, no warning.
- New `[General] AutoReloadConfig` toggle (on by default) for live INI reloads.
- Item picker no longer cuts off long lists. Every matching item shows up,
  including the full Chest catalog past the letter K.

## [0.6.2] - Crimson Desert 1.04.00 Support

- Restored full functionality on game version 1.04.00. Transmog apply, real-armor teardown, the picker filter, and the PlayerSafe check all work again after the patch
- Display name coverage is now complete: the picker shows a friendly name for every 1.04.00 item (was missing ~300 names after the patch)
- Append now creates a blank preset with every slot set to hide (ticked + none) and applies it immediately, instead of copying your current outfit
- New Copy button duplicates the currently-visible picker rows into a new preset so you can fork without overwriting
- Unsaved picker edits now show an orange `[UNSAVED -- click Save]` banner at the top of the overlay and tint the Save button orange with a `*` marker
- Developer hot-reload is more resilient: reloading a rebuilt logic DLL no longer leaves stale hooks behind

## [0.6.1] - Load-Time Auto-Apply Robustness

- Character-swap auto-detect now uses a 1 second settle window so the engine's load-time party rotation (Kliff -> Oongka -> Damiane) collapses into a single transition instead of three back-to-back applies
- Load-detect retry loop now runs a cheap actor-readiness probe before each scheduled apply, suppressing the noisy SEH-faulted log spam previously produced when the engine parked the player wrapper on a placeholder during long world loads
- Load-detect retry loop now aborts mid-budget if the engine swaps the player wrapper beneath it (placeholder -> real actor), handing off to the outer loop instead of burning the remainder of a ~95 second retry budget against a dead pointer
- Cuts the visible-mismatch window after a slow load from up to ~95 seconds down to ~3-7 seconds on affected systems
- Fixed: switching characters could apply the previous character's preset onto the new actor when the controlled-char resolver's last-known-good cache had not yet caught up to the swap (e.g. swapping to Damiane on a save with no Oongka could briefly render Oongka's transmog). The cache is now invalidated on every player-component change, not only on save load
- Fixed: auto-apply could fail entirely on saves whose Kliff actor was constructed with different metadata (e.g. a chapter where Kliff reads as `(party_class=3, char_kind=3)` instead of the previously-verified `(4, 3)`). The controlled-character resolver now identifies Kliff via the structural invariant `*(user+0xD0) == *(user+0xD8)` and the two companions via the slot-index byte at `actor+0x60` (Damiane = primary slot + 1, Oongka = primary slot + 2), making detection robust across saves regardless of party composition

## [0.6.0] - Multi-Character Support (Kliff, Damiane, Oongka)

- Preset lists are now stored per character. The active list swaps automatically when you change who you are controlling in-game, and each character remembers its own "last applied" preset ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- New body-kind (male / female) filter in the picker so mismatched-body armor is hidden by default. Per-character override available in the overlay for body-swap mod users ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- Display name table is now loaded from `CrimsonDesertLiveTransmog_display_names.tsv`. The `.tsv` must be installed alongside the `.asi`; without it the picker falls back to raw item ids ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- Added `LICENSE` (MIT) to the mod folder ([#34](https://github.com/tkhquang/CrimsonDesertTools/pull/34))
- Shared AOB utilities moved into the new `CrimsonDesertCore` static library. No behavioural change ([#33](https://github.com/tkhquang/CrimsonDesertTools/pull/33))
- README, Nexus bbcode, template readme, and Acknowledgements refreshed for the multi-character release

## [0.5.0] - Built-in Overlay (Removed ReShade Hard Requirement)

- The mod now includes its own overlay window. ReShade is no longer needed to use the in-game GUI
- If ReShade is installed, the mod still registers as a ReShade addon tab automatically
- New `ForceStandaloneOverlay` setting in INI to use the standalone window alongside ReShade
- Overlay toggle key is now configurable via `OverlayToggleHotkey` in INI (default: Home)
- Standalone overlay: press Esc to close, window follows game focus and hides on alt-tab
- Compatible with OptiScaler and other DXGI wrappers (no swap chain hooking)

## Human-Readable Item Names & Overlay Improvements

- Item picker now shows human-readable display names (e.g. "Kliff's Leather Armor" instead of "Kliff_Leather_Armor") with the internal name shown below in gray
- Search now matches both display names and internal names
- Item slot detection rewritten to be more accurate across the full catalog
- Name table scan no longer gives up after 15 attempts, it keeps retrying until the game is ready

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

[0.6.3]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.3
[0.6.2]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.2
[0.6.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.1
[0.6.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.0
[0.5.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.5.0
[0.3.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.3.1
[0.3.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.3.0
[0.2.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.2.0
[0.1.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.1.0
