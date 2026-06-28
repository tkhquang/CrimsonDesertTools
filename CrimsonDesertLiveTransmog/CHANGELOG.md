# Changelog

All notable changes to the CrimsonDesertLiveTransmog mod will be documented in this file.

## [0.12.7] - Companion transmog fix and stability update

- Fixed companions (such as Oongka and Damiane) showing their default gear instead of your chosen outfit after they leave and return to the scene; their transmog now reapplies on its own
- Refreshed the color and dye data for game version 1.12.02
- Updated the underlying framework (DetourModKit) to 3.9.0 for better stability and future compatibility
- More resilient to future game updates: some internal addresses now self-correct after a patch
- Added an advanced INI setting (SelfHealWindow) to help the mod recover after a game update
- Standardised the source-code formatting and added automatic style enforcement for future development
- Tidied developer comments and documentation for consistency, with no change to in-game behaviour

## [0.12.6] - Reliable body-mesh prefab picks

- Fixed body-mesh prefab picks not applying when the character had no saved preset

## [0.12.5] - Updated for 1.12.00

- Updated for 1.12.00

## [0.12.4] - Crimson Desert 1.10.00 support

- Updated the mod to work with Crimson Desert version 1.10.00.
- Fixed saved outfits not applying to your characters after the game update.
- Fixed items whose names use special characters (such as Roman numerals) being missing from the item catalog.
- Fixed the item catalog showing the wrong model for some character-specific gear (male, female, and orc versions).
- Hardened the mod under the hood for better stability and resilience to game updates.
- Updated the mod's underlying framework (DetourModKit) to v3.4.0 for better stability and future compatibility.

## [0.12.3] - Sturdier helmet voice-unmuffle

- Improved Unmuffle Helm Voice so it better survives future game updates.

## [0.12.2] - Crimson Desert 1.09.00 support

- Updated the mod to work with Crimson Desert version 1.09.00.
- Added Item Catalog data for game version 1.09.00.
- Fixed the item picker showing some armor under the wrong body type, or hiding it entirely.
- Fixed the optional helmet voice-unmuffle setting for the new game patch.

## [0.12.1] - Item catalog

- Added a game-version switcher to the online Item Catalog.
- Added Item Catalog data for game version 1.08.00.
- Item Catalog now lists items that were previously missing.
- Fixed companions sometimes not receiving their saved transmog in crowded areas.

## [0.12.0] - Helm voice unmuffle and reliability fixes

- Optional setting to stop plate and heavy helmets from muffling your protagonist's voice -- enable `UnmuffleHelmVoice` under `[Experimental]` (off by default; NPC voices still muffle as in vanilla).
- Upgraded DetourModKit dependency to v3.3.0.
- More resilient internal checks against the game's loaded modules.
- Reduced duplicated crash-protection code across modules.
- Fixed first-apply errors after a cold load by waiting until the character is fully ready.

## [0.11.3] - Save-load and Color Override fixes

- Fixed a burst of transient errors that could appear in the log during the first few seconds after loading a save.
- Transmog now waits for the game to finish setting up your character before applying, instead of retrying through errors.
- Internal refactor: identify character components by class name so the mod keeps working if the game shuffles their order.
- Fixed an FPS drop when applying transmog while Color Override is enabled.

## [0.11.2] - Picker quality-of-life and preset polish

- Prefab picker now shows non-human NPC body parts (dwarves, orcs, etc.). The "Exact" toggle still keeps things tidy per slot.
- Prefab picker header now shows live counts (e.g. "All Prefabs across all slots (3854)" or "(142 / 3854)" when filtering).
- Append now ticks only the five armor slots (Helm/Chest/Cloak/Gloves/Boots) by default, so items like the lantern aren't accidentally hidden.
- Refreshed 119 item display names with minor spelling and wording fixes.
- Pickers now open scrolled to your current pick instead of starting at the top of the list.
- Up and Down arrow keys now move the picker highlight; Enter commits the highlighted row.
- Prefab picker has a new "Keep open" toggle so you can audition several prefabs without re-opening the popup.
- Prefab picker has a new in-popup "Apply" button that works the same as Apply All, handy when Instant Apply is off.
- Reset Slot in the color override panel now saves an empty slot into the active preset (previously the wipe was silently reverted on Save).

## [0.11.1] - Instant Apply and preset-hide fixes

- Fix: real Mask/Necklace (and other accessory slots) now hide correctly when switching to a preset with all slots set to (none). Previously only the manual transmog→none path worked; preset switches left the real mesh visible on first-claim hide.
- Fix: Instant Apply now works correctly for body-mesh prefab picks on Damiane and Oongka. Previously, Instant Apply only affected the controlled character, so editing another character's outfit reverted to their original armor.

## [0.11.0] - Multi-character world-entry auto-apply

- Every visible protagonist now has their saved transmog preset applied automatically on world entry, without needing to cycle to each character first.
- Followers freshly summoned during play also get their preset applied as soon as their body finishes loading.
- Sturdier save-load detection: the active character's preset is re-applied immediately on load instead of needing a short settle window.
- Switching the editing character in the dropdown no longer leaves stale fake items installed on the body.
- Added an optional Dump Item-to-Prefab TSV diagnostic for one-shot capture of every item's icon mesh name.
- Added an optional Dump Item Catalog TSV diagnostic, gated by its own INI key instead of trace-level logging.
- Hardened the two underlying engine-registry address lookups so the diagnostics keep working after game patches.
- Fixed brief part flashes during animation transitions that could persist for the whole session when the mod was loaded before reaching the main menu.
- New "Apply To Selected" checkbox next to the character dropdown: edits while pinned to a non-controlled character now render on that character's body, not on the one you control. Untick to keep the old cross-body behavior.
- Preset swaps on a non-controlled character now correctly remove the previous preset's items before installing the new ones, instead of stacking outfits on top of each other.
- Compatibility fix for Crimson Desert game version 1.08.00

## [0.10.1] - Overlay Unification

- Standalone and ReShade overlays now share a single rendering path, eliminating a class of widget-mismatch glitches when both ReShade and the standalone window were used in the same session.
- Updated bundled Dear ImGui to the latest stable release for smoother input handling and minor visual refinements.
- `ForceStandaloneOverlay = true` now strictly disables the ReShade addon tab and opens only the standalone window. Leave it false to keep the ReShade addon tab when ReShade is installed.
- Internal cleanup of overlay UI code (no functional change).
- The standalone overlay is much more responsive: clicks, hovers, and preset switches no longer lag. The overlay now repaints only the area Dear ImGui actually drew (instead of the full game window) and bumps its frame rate up while you're interacting with it.
- Fixed: Color Override swatch circles now reliably appear after game load (previously needed a preset switch to show up).
- Fixed: Switching presets no longer risks wiping saved Color Override entries when those entries had not yet finished loading.
- Fixed: Overlay window no longer shrinks then re-expands when switching between presets with and without Dye / Color Override applied.
- Fixed: The hex item-id field next to each slot picker no longer clips its last digit at larger UI scales.
- Picker now lists items whose internal names contain special characters (a handful of Goblin Merchant gear that was previously missing).
- Refreshed the bundled display-name table to match the latest in-game text (about 119 entries updated, e.g. crafting recipe names and currency labels).

## [0.10.0] - [Experimental] Color Override

- New experimental "Color Override" picker paints transmog items (including outfits the in-game dye merchant refuses) with a custom colour per visible region.
- Color Override is OFF by default; enable it by setting `ColorOverride = true` under a new `[Experimental]` section in the INI, then relaunch the game.
- New per-slot "Sync from live" button captures the dye you applied to a real item in-game and stores it in the active preset for that slot.
- Cleanup pass on the existing dye apply path so unset slots no longer silently inherit your real item's colours.
- New "Save as New" button in the Presets section forks your pending edits into a brand-new preset without overwriting the one you started from.
- Copy now clones the active preset's saved state only; pending picker edits are discarded so you get a clean baseline. Use "Save as New" if you want to keep your pending edits.
- Prefab picker's Exact filter is stricter: knowledge images and other UI assets that happened to share a body-mesh name no longer leak into the list.
- More reliable character detection on save-load and radial swap; the active preset now follows your real protagonist across reloads without needing a manual swap-and-back.

## [0.9.4] - Cross-character preset editing

- You can now pick a different character in the Character dropdown to edit (and wear) their preset on whoever you are currently controlling, with an Unpin button to snap back to following the controlled character.

## [0.9.3] - v1.06.00 patch support

- Compatibility fix for Crimson Desert game version 1.06.00. Restored radial-character swap detection so transmog stays correct after switching characters.

## [0.9.2] - Fixed item picker filtering

- Restored item picker filtering for Cloak, Bracelet, Glasses, Mask, and Backpack on v1.06.00, which renumbered their type-codes and left those pickers appearing empty under the "Exact" filter.

## [0.9.1] - Capture Real Dye, Disabled-Mode Cleanup, Standalone Overlay Polish

- Capture Outfit now also snapshots the live dye and material state on every captured slot, so loading a captured preset paints each item in the colours it had at capture time instead of the factory palette.
- Toggling Live Transmog off (or hitting Clear) now correctly tears down NPC carrier visuals on slots that had no real backing item, so disabled mode no longer leaves a ghost helm or cloak painted on the actor.
- Restoring a real item after unticking a slot, or after toggling Live Transmog off, now keeps the item's current inventory dye instead of resetting it to the factory palette.
- New per-slot Sparse toggle in the dye picker. Sparse (default) emits only the channels you set and lets the engine paint the rest with its own defaults; turn it off when doing cross-class fake transmog so every channel uses your colour and the carrier's default palette is fully suppressed.
- Standalone overlay font is now built at the target pixel size (sharper text on 1440p and 4K) instead of being stretched, and the window, item picker, slot label column, and picker button widths all auto-fit the live font so 4K screens no longer show wide empty bands.
- Standalone overlay mouse clicks now use rising-edge detection over the overlay window: clicks that begin inside the game and drag onto the overlay no longer fire phantom widget events, and clicks that begin on the overlay still complete cleanly even if the mouse leaves the overlay before release.

## [0.9.0] - Dye picker

- New per-slot dye picker recolors transmog armor in real time, independent of the dye on your real items.
- Fixed mouse clicks being ignored in the standalone overlay on some setups (hover already worked; clicks now follow it).
- Standalone overlay UI tightened up: bigger color swatches, button labels no longer get clipped, and the dye popup laid out so nothing runs off the edge.
- Fresh slots now highlight the "Default" material and show 100% repair, matching the engine's natural state.

## [0.8.0] - Expanded Slot Coverage and Body-Mesh Prefab Picker

- Slot picker now covers 10 equipment slots (helm, chest, cloak, gloves, boots, necklace, lantern, glasses, mask, backpack) instead of the 5 armor slots only. Weapon, ring, earring, and bracelet slots are not yet supported.
- New body-mesh prefab picker lets you pick a prefab variant to swap onto a slot, in addition to the regular item picker
- Body-mesh prefab selections persist with your preset (saved as a new `prefabName` field; older preset files load unchanged)
- Preset JSON format updated: slots are now stored as a keyed object (by slot name) instead of a positional array, so future slot additions will not shift older saves. Legacy array-format presets load automatically and are rewritten in the new format on next save.
- Optional cross-slot prefab browser mode: list every body-mesh prefab in one place and apply it to its native slot from any picker
- Removed the hard world-load timeout in the standalone overlay path. The mod now waits indefinitely for the game world to become ready (with a heartbeat log every minute) instead of giving up and warning, which prevented the overlay from initializing on slow boots.

## [0.7.0] - v1.05.00 patch support + multi-protagonist fixes

- Updated for game version 1.05.00: transmog correctly identifies carrier-required versus direct-wear items on the latest patch.
- Updated for game version 1.05.00: real-equipment capture and restore now read the live equipment table on the latest patch.
- Restored radial character-swap detection on game version 1.05.00.
- Updated item display names to match the v1.05.00 catalog.
- Radial-menu character switching is now reliably tracked, so each protagonist's preset applies after a swap.
- Fixed the wrong outfit briefly appearing on a teammate after a save-load.
- Fixed coexistence with Equip Hide: both mods can now be loaded together without breaking radial-menu tracking.

## [0.6.4] - Game Patch 1.04.00 Capture Outfit Fix

- Capture Outfit and the equipment restore after Clear work again on game version 1.04.00.

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

[0.12.7]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.7
[0.12.6]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.6
[0.12.5]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.5
[0.12.4]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.4
[0.12.3]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.3
[0.12.2]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.2
[0.12.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.1
[0.12.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.12.0
[0.11.3]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.11.3
[0.11.2]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.11.2
[0.11.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.11.1
[0.11.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.11.0
[0.10.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.10.1
[0.10.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.10.0
[0.9.4]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.9.4
[0.9.3]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.9.3
[0.9.2]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.9.2
[0.9.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.9.1
[0.9.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.9.0
[0.8.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.8.0
[0.7.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.7.0
[0.6.4]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.4
[0.6.3]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.3
[0.6.2]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.2
[0.6.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.1
[0.6.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.6.0
[0.5.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.5.0
[0.3.1]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.3.1
[0.3.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.3.0
[0.2.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.2.0
[0.1.0]: https://github.com/tkhquang/CrimsonDesertTools/releases/tag/live-transmog/v0.1.0
