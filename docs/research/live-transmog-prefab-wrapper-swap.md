# Body-Mesh Pointer Swap -- Implementation Reference

This document is the reference for the body-mesh-swap feature in `Transmog::PrefabWrapperSwap`. It is a reference for the *current* working code only. Read it alongside the source files in `CrimsonDesertLiveTransmog/src/`:

- `prefab_wrapper_swap.{hpp,cpp}` -- feature implementation
- `aob_resolver.hpp` -- AOB candidate definitions used by this feature plus the wider mod
- `transmog_map.cpp` -- slot enum to engine slot tag mapping

Verified against **Crimson Desert v1.05.01** (image base `0x140000000`, FileVersion `1.0.0.1070`). The full AOB-cascade audit covering every hardcoded RVA replaced in the 2026-05-08 patchproofing pass lives in section 9 of [live-transmog-source-of-truth.md §9](live-transmog-source-of-truth.md#9-aob-cascade-audit-2026-05-08); this document references that table rather than duplicating it.

---

## 1. What it does

Re-skin the player body's five cosmetic slots (Helm, Chest, Cloak, Gloves, Boots) onto **any prefab the engine has loaded**, including cross-character NPC prefabs (e.g. `cd_nhw_no_ub_20027` on Kliff). The swap operates on the engine's resource binding pipeline rather than on item-level transmog: the player still has Kliff's helm equipped at the inventory level, but the body component renders the picked prefab.

Source mesh names per slot are hardcoded to the canonical Kliff plate set (`cd_phm_00_*_00_0435*`); the user picks a target via the per-slot dropdown in the overlay. Targets persist in the preset JSON as `prefabName` and auto-apply on load. There is no INI configuration and no toggle hotkey; the feature is always installed, and activation happens implicitly on Apply when at least one slot has a target selection.

---

## 2. Architecture

Two hooks compose the feature:

```text
Apply path -----------------> sub_140352AA0  (primary swap)            ---+
                                                                         | All gated on
Engine teardown -------------> sub_142711DF0  (natural-pipeline patch) ---+ s_active
```

Each hook runs only when `s_active.load() == true`. When the feature is inactive (no body-mesh selections), both hooks become zero-impact passthroughs. The substitution path also cross-checks `Transmog::in_transmog()` so the engine's own real-item flow is never affected.

### Why two hooks

The substitution at `sub_140352AA0` installs the target wrapper into the destination record (`parent+88`'s record list). On preset switch the engine's content-keyed teardown pipeline (`sub_142711DF0` and downstream unlink primitives) walks `parent+88` looking for a wrapper to unlink, but the search target it derives from `safeTearDown -> ExpandToMeshes` is the *Kliff* wrapper that the inventory still holds, not the substituted target wrapper sitting in the record. Without the second hook, no record matches, no unlink fires, and the helm / cloak ghost-leaks across preset switches.

The natural-pipeline hook (Hook 2) substitutes Kliff src wrappers with target wrappers in the input list at hook entry, so the engine matches the substituted target in `parent+88` and unlinks correctly. After the trampoline returns, the originals are restored so the caller's refcount-release loop pairs with the same wrapper it incremented during list construction.

---

## 3. Boot-time catalog population

When `init()` runs it installs the two hooks and spawns a detached thread that polls `Transmog::is_world_ready()` every 500 ms (no timeout; the sleeping detached thread is reaped on process exit). Once the world is ready the thread calls `populate_slot_catalogs()` to build the per-slot prefab dropdowns, then runs:

```cpp
Transmog::PresetManager::instance().apply_to_state();
Transmog::manual_apply();
```

This is the load-time auto-apply path: presets parsed before the heap walk completed have their `prefabName` values unresolved (catalog was empty); this retroactive `apply_to_state` resolves each name against the now-populated catalog and the subsequent `manual_apply` activates the swap via `notify_apply_starting -> reactivate_with_selections`.

The catalog is also re-populated by `transmog_worker.cpp`'s save-load auto-detect: save loads can rotate the AppearanceTableLoader registry's resident wrapper set as zone- and archetype-specific assets stream in / out.

---

## 4. Catalog enumeration

`populate_slot_catalogs()` builds the per-slot prefab dropdowns from two sources, merged into one per-slot vector inside `s_slotCatalogs[k_slotN]`.

### 4.1 Source 1: StringInfo walk (Phase-1 fast walk)

`walk_string_info(prefix, visitor)` runs with a broad prefix `"cd_"` (3 chars). For each entry that has vtable equal to the StringInfo body-entry vtable AND has an inline name containing one of the slot tags `_hel_00_`, `_ub_00_`, `_cloak_00_`, `_hand_00_`, `_foot_00_`, the visitor records `(name, wrapper-ptr from entry+0x18, hash from entry+0x00)` into the slot's local vector. Both `cd_phm_*` (player male) and `cd_phw_*` (player female) variants are admitted.

The walk uses bulk-copy (`bulk_copy_seh`) over the entry-pointer array plus a per-entry 128-byte header copy. Completes in ~5 ms across all ~30,000 entries.

The StringInfo registry layout (verified live):

| Address | Field |
|---|---|
| `s_stringInfoRegistry` (resolved at init) | `regPtr` (singleton struct ptr) |
| `regPtr+0x08` | `count` u32 |
| `regPtr+0x50` | `array_ptr` (u64 array of entry pointers, length = `count`) |

Each entry's relevant fields:

| Offset | Field |
|---|---|
| `+0x00` | u32 hash (engine asset name hash) |
| `+0x08` | vtable pointer (matched against the resolved StringInfoVtable for body-mesh entries) |
| `+0x18` | wrapper-ptr (the value LT stores in `s_swapMap`) |
| `+0x20` | inline name, NUL-terminated |

The registry singleton address and the body-entry vtable both arrive through 3-anchor cascades resolved at init (`k_stringInfoRegistryCandidates` and `k_stringInfoVtableCandidates` in `aob_resolver.hpp`). Until the cascade resolves, every consumer reads zero and short-circuits, so a cascade failure degrades to "feature inert" rather than "feature mis-pointers".

Per-slot heap walk for partprefab pool wrappers is then merged into each entry's `wrappers` vector (same 32-byte stride records the natural pipeline operates on, in heap pools observed at `0x4104A*` / `0x4104E*` on this build).

### 4.2 Source 2: AppearanceTableLoader registry

The engine maintains a singleton name->entry registry (resolved at init via `k_loaderRegistryCandidates`). Its hash-table struct lives at `+0x50` and contains ~15,300 pre-allocated entries at boot covering every body-mesh prefab the engine knows about, both player AND NPC variants, regardless of whether the asset's data is currently resident.

| Offset (within `+0x50` table struct) | Field |
|---|---|
| `+0x00` | `bucket_count` u32 |
| `+0x04` | `count` u32 |
| `+0x08` | `capacity` u32 |
| `+0x10` | `bucket_array_ptr` (256-byte stride per bucket) |
| `+0x18` | `data_array_ptr` (8-byte pointer per entry) |

Each entry pointed to by the data array is 24 bytes:

| Offset (within entry) | Field |
|---|---|
| `+0x00` | `(hash u32, region u32)` packed |
| `+0x08` | KEY wrapper-ptr (the partprefabdyeslot wrapper carrying the inline name at `+0x18`) |
| `+0x10` | VALUE wrapper-ptr (metadata struct, different layout, not used here) |
| `+0x18` | zeros / padding |

`enumerate_loader_registry_into_catalog()` walks `data_array[0..count-1]`, dereferences `entry+0x08` to get the KEY wrapper, reads the inline name at `wrapper+0x18`, classifies by slot tag, and merges into the catalog. Names already present from the StringInfo walk get their wrapper appended to the existing entry's `wrappers` vector with sort + dedup. Fresh names create new `PrefabEntry` records.

Why `entry+0x08` and not `entry+0x10`: the KEY wrapper at `+0x08` carries the prefab name in the standard partprefabdyeslot format that the body-mesh hook already substitutes by pointer equality. The VALUE wrapper at `+0x10` is a metadata struct (counts / IDs) with a different vtable.

Order matters: registry enumeration runs after the StringInfo walk and the partprefab heap merge, but before the metadata enrichment pass and the auto-seed of source selections.

1. StringInfo entries are seeded first (fast path).
2. Registry adds the rest, merges by name (dedupe).
3. Each slot's catalog is re-sorted with the full superset.
4. Source-selection auto-seed runs last: for each slot, finds the hardcoded default Kliff source name in the sorted catalog and stores its index in `s_selSrcIdx[slot]` (only when not already set, so user picks are not overwritten on refresh).

### 4.3 Hardcoded Kliff source defaults

```text
Helm   -> cd_phm_00_hel_00_0435_d
Chest  -> cd_phm_00_ub_00_0435
Cloak  -> cd_phm_00_cloak_00_0435_s
Gloves -> cd_phm_00_hand_00_0435
Boots  -> cd_phm_00_foot_00_0435
```

These are the canonical "Kairos plate" mesh names every protagonist transmog rides on. Oongka and Damiane fall back to the same Kliff defaults until per-character source selection lands.

### 4.4 Per-character body-mesh family

Each protagonist has a top-level appearance key registered in `stringinfo.pabgb`; the engine resolves it via `sub_1402F9340` plus `metadata+0x92`:

| Character | Appearance key | Slot-prefab family root |
|---|---|---|
| Kliff | `cd_phm_macduff` | `cd_phm_00_*` |
| Damiane | `cd_phw_damian` | `cd_phw_00_*` |
| Oongka | `cd_phm_oongka` | `cd_phm_00_*` (same as Kliff) |

`cd_phm_*` = player-human-male; `cd_phw_*` = player-human-female. Oongka shares Kliff's family root despite a different equip-class; the runtime classifier rejects Kliff-equip-type items at his equip gate, but the underlying body-mesh prefabs come from the same `cd_phm_00_*` pool.

### 4.5 Per-character carrier sets (`transmog_apply.cpp`)

Three character-specific carrier tables drive `default_carrier_for_slot()`. Every entry must pass that character's equip class-gate; mismatches fall back to direct equip and may silently fail. The Kliff-plate `_0435` source defaults above are reusable across characters because every protagonist body resolves through the same partprefab registry walk; the canonical "plate" variant per character differs and the deterministic carrier->prefab binding is not statically extractable from the offline pabgb dumps (iteminfo.pabgb stores it as a binary-serialized appearance struct that resolves via stringinfo hashes at runtime).

| Character | Helm | Chest | Cloak | Gloves | Boots |
|---|---|---|---|---|---|
| Kliff | `Kliff_PlateArmor_Helm` | `Kliff_PlateArmor_Armor` | `Kliff_PlateArmor_Cloak` | `Kliff_PlateArmor_Gloves` | `Kliff_Plate_Boots` |
| Damiane | `Demian_PlateArmor_Helm_I` | `Demian_Leather_Armor` | `Demian_Leather_Cloak` | `Demian_Leather_Gloves_I` | `Demian_Plate_Boots_III` |
| Oongka | `Oongka_PlateArmor_Helm_II` | `Oongka_Basic_Leather_Armor` | `Oongka_Basic_Leather_Cloak` | `Oongka_Basic_Leather_Gloves` | `Oongka_PlateArmor_Boots_II` |

Each carrier's actual prefab variant requires the runtime catalog walk plus a live equip event with that character on the active slot. The offline pabgb dumps in `D:\Tools\CrimsonForge\Out` do not contain a text-level item-name to prefab-name mapping; the link is hashed through stringinfo. Available `cd_phw_00_*` (Damiane) variants per slot, sourced from `partprefabdyeslotinfo.pabgb`:

- `hel`: 100, 141, 145, 146, 151, 160, 161, 162, 167, 168, 169, 170, 203
- `ub`:  1, 43, 73, 145, 146, 151, 160, 161, 162, 163, 166, 170, 180
- `cloak`: 65, 141, 145, 146, 151, 160, 161, 162, 163, 166, 170, 180, 201
- `hand`: 116, 137, 141, 145, 146, 151, 160, 161, 166, 170
- `foot`: 20, 145, 151, 160, 161, 163, 166, 170, 180

### 4.6 Naming conventions across the catalog

The two enumeration sources expose names in different forms, both valid:

- **Player prefabs (StringInfo)**: full form with `_00_` markers, e.g. `cd_phm_00_hel_00_0395_c`, `cd_phw_00_ub_00_0163_index02`.
- **NPC prefabs (registry)**: short form, no `_00_` markers, e.g. `cd_nhw_no_ub_20027`, `cd_nhm_de_ub_0002`. Region segment: `no` (north), `so` (south), `de` (desert). Sub-tier sometimes preserved: `cd_nhw_so_ub_10_10047`.

---

## 5. Apply-only activation lifecycle

Body-mesh swap follows the carrier hybrid pattern: picker mutations only update `s_selSrcIdx[slot]` / `s_selTgtIdx[slot]` (pending UI state). Actual swap-map rebuild and activation happens at apply time via `notify_apply_starting()`, which calls `reactivate_with_selections()`:

- `has_any_selection() == true` -> `deactivate_for_clear()` (if active), then `apply_selections_to_swap_map()`, then `s_active = true`.
- `has_any_selection() == false` -> `deactivate_for_clear()` (if active), stay inactive.

This means:

- Picking a prefab does not immediately fire the engine; nothing changes visually until the user clicks Apply All (or auto-apply triggers).
- Clearing all selections (e.g. picker -> "(none) prefab" or X) deactivates cleanly on the next apply.
- A preset switch that introduces or removes prefab selections is just a state transition that `notify_apply_starting` syncs.

---

## 6. Picker UX flow

Each slot row in the overlay has a single picker button that, when clicked, opens a popup with two sections:

1. **Real items**: the existing item catalog (carrier-driven transmog).
2. **Body-Mesh Prefabs**: the slot's per-slot catalog from the merged enumeration above.

Picking from either section commits a different state on the slot.

### 6.1 Real-item pick

Sets `m.targetItemId = N`, clears any prior body-mesh prefab override on the same slot via `BMP::set_selection(slot, curSrc, -1)`, and (when auto-apply mode is on) triggers `manual_apply_slot(i)`. The body-mesh hook is dormant for this slot until the user picks a prefab again.

### 6.2 Prefab pick

Updates pending state only:

1. `BMP::set_selection(slot, curSrc, prefabIdx)`: store the new target index. Source is the auto-seeded Kliff default.
2. Update `s_slotUI[i].pickedPrefabName` (UI label) from the catalog name.
3. `force_kairos_carrier_for_picked_slots()`: for slots with a prefab pick, force `m.targetItemId` to the Kliff carrier itemId resolved via `default_carrier_for_slot(slot, "Kliff")` from `transmog_apply.cpp::k_kliffCarriers[]`. Slots without a prefab pick keep the user's current preset's carrier.
4. If auto-apply is enabled, `manual_apply_slot(i)` fires (which routes through `notify_apply_starting -> reactivate_with_selections`, swap activates). Otherwise the change stays pending until Apply All.

### 6.3 Why force the Kliff carrier on prefab pick

The body-mesh hook substitutes by pointer equality against `s_swapMap` keys. The keys are Kliff's natural source wrappers. When the user is on a non-Kliff preset (e.g. Wellsknight), the engine renders that preset's items using *their* wrappers, which are not in `s_swapMap`. The hook never matches, no substitution happens.

Forcing the carrier to Kliff's hardcoded armor makes the engine equip Kliff's plate armor, which uses the `cd_phm_00_*_00_0435*` wrappers. Those ARE in `s_swapMap`. Hook fires. Substitution happens. NPC mesh visualizes.

The choice of Kliff specifically (rather than reading from PresetManager's "Kairos preset") is deliberate: a user's saved Kairos preset may be empty or unconfigured. The hardcoded carrier set is the authoritative ground truth.

### 6.4 Slot button display when a prefab is picked

When `s_slotUI[i].pickedPrefabName` is non-empty, the slot button shows the prefab name with a `[body-mesh]` suffix in cyan, e.g.

```text
[ cd_nhw_no_ub_20027 [body-mesh] ]
```

The hex carrier id input next to the picker is disabled with an explanatory tooltip: the body-mesh prefab is the source of truth, and the hex is just a peek at the underlying carrier plumbing. Clearing the prefab via the picker's "(none) prefab" re-enables the hex for direct editing.

The slot row also includes a lazy two-way sync between `BMP::selection_tgt_index(slot)` and `s_slotUI[i].pickedPrefabName`: when the BMP target is set but the UI label is empty (e.g. right after preset load on boot), the row picks up the prefab name from the catalog. When BMP target clears, the label clears too.

### 6.5 Mutually exclusive states per slot

Per slot, exactly one of these is active at a time:

| State | `pickedPrefabName` | `m.targetItemId` | body-mesh `tgt_idx` |
|---|---|---|---|
| Empty / clear | empty | 0 | -1 |
| Real-item carrier | empty | itemId | -1 |
| Body-mesh prefab override | non-empty | Kliff carrier itemId | >= 0 |

Picking a real item or `(none) item` from the popup clears `pickedPrefabName` and the body-mesh selection. Picking a prefab clears any prior real-item by forcing the carrier to Kliff's, and sets `pickedPrefabName`. The two states cannot coexist on a single slot, but DIFFERENT slots can be in DIFFERENT states (e.g. helm = Wellsknight real-item, chest = NPC prefab override).

---

## 7. Preset persistence and auto-apply

`PresetSlot` stores `prefabName` alongside `itemName` in the preset JSON. For slots with a body-mesh override, `itemName` is intentionally not written: the carrier itemId is internal plumbing that's re-derived at load time from `default_carrier_for_slot(slot, activeCharacter)`.

```json
{ "active": true, "prefabName": "cd_nhw_no_ub_20027" }
```

For plain carrier slots, the existing format is unchanged:

```json
{ "active": true, "itemName": "Wellsknight_Helm" }
```

`PresetManager::apply_to_state` walks each slot:

1. Writes `(active, itemId)` from the resolved preset slot.
2. If `prefabName` is set but `itemId == 0` (no `itemName` in JSON), looks up the default carrier via `default_carrier_for_slot(slot, m_activeCharacter)`. Kliff / Damiane / Oongka each get their own carrier set.
3. Resolves `prefabName` against `BMP::slot_catalog(slot)` and calls `BMP::set_selection(slot, curSrc, foundIdx)`.

`PresetManager::capture_from_state` reverses this on Save / Capture / Duplicate: reads `BMP::selection_tgt_index(slot)` -> catalog name -> stores `prefabName`. When `prefabName` is set, `itemName` is cleared.

---

## 8. Preset switch transition

When the user clicks a different preset in the Presets panel:

1. **`clear_all_picked_prefabs_and_deactivate()`**: for every slot, clears `s_slotUI[i].pickedPrefabName`, drops the BMP target via `BMP::set_selection(slot, curSrc, -1)`, and returns a per-slot `hadPick[]` bitmask. Does NOT touch `last_applied_ids`.

2. **`pm.set_active_preset(i) + pm.apply_to_state()`**: writes the new preset's `(active, itemId, prefabName)` into `slot_mappings()` and re-syncs BMP selections from the new `prefabName` values.

3. **Conditional `last_applied_ids[i] = 0`**: for each slot in `hadPick`, if `mods[i].targetItemId == lastIds[i]` (the early-out case where `apply_single_slot_transmog` would skip teardown), zero `lastIds[i]` so the apply pass tears down the prior body-mesh fake. When carriers differ (e.g. helm `0x1521 -> 0x0000`) `lastIds` stays intact and the regular tear_down_fake path handles cleanup naturally; the natpipe-hook fires during teardown and substitutes src to tgt so the engine unlinks correctly.

4. **`manual_apply()`**: triggers `apply_all_transmog`. The next `notify_apply_starting` rebuilds the swap map for whatever the new preset selected (or deactivates if it has no body-mesh slots).

5. **`pm.save()`**: persists.

---

## 9. Hook 1: Primary swap at `sub_140352AA0`

**Function**: 96-byte struct copy operator. Engine calls it during the slot-populator pipeline to copy a per-slot resource record from a source struct (`a2`) into the destination record (`a1`). The intermediate-wrapper pointer sits at `+0x18` of the source.

**Decompile shape:** the function zero-inits `*a1`, copies `*(QWORD*)(a2 + 0x00)` into `*a1`, then writes the StringInfoVtable sentinel back into `*(QWORD*)(a2 + 0x00)` (so the engine's eventual destruct of `a2` is a no-op for the moved-from wrapper). Subsequent fields at +0x08, +0x09, +0x0A, +0x0C, +0x10, +0x18 are likewise moved-from-into. The wrapper-of-interest sits at `+0x18` of the source struct.

**Hook**: `on_struct_copy(a1, a2)`. At entry:

1. Bail out unless `s_active && Transmog::in_transmog()`.
2. Read `srcWrapper = *(QWORD*)(a2 + 0x18)`.
3. Look up `srcWrapper` in `s_swapMap`. If absent: passthrough.
4. If present, the looked-up `tgtWrapper` is the cross-class prefab to install. `_InterlockedIncrement(tgtWrapper + 0x10)` to balance the destination's eventual destruction-time refcount-decrement.
5. Write `tgtWrapper` to `a2 + 0x18` *in the source struct*. The trampoline's subsequent copy will then land the target into the destination.
6. Run trampoline.
7. Append `(destAddr, origSrcWrapper)` to `s_substLog` (per-apply diagnostic).

The "source +1 leak" (Kliff's refcount stays bumped from the engine's pre-copy work) is acceptable: these are global-lifetime asset wrappers and the engine holds them anyway.

---

## 10. Hook 2: Natural-pipeline patch at `sub_142711DF0`

This hook is the fix that closed the helm / cloak ghost leak.

After the primary swap installs the target wrapper into the destination record, the engine's content-keyed teardown pipeline (`sub_142711DF0` plus downstream unlink primitives) needs to find that same wrapper in `parent+88`'s record list and unlink it on preset-switch. The pipeline gets its *search target* from `safeTearDown` (`sub_14078BB20`), which:

1. Calls `ExpandToMeshes` (`sub_141E0A710`) with the inventory-resolved character ID and slot tag, returns `u16` mesh-prefab IDs in a parts table.
2. Resolves each `u16` via `sub_1402F7FA0(u16) -> entry -> +0x18 -> wrapper-ptr`.
3. Builds a `(wrapper, flag)` list at 16-byte stride.
4. Calls `sub_142711DF0(parent_body, &wrapper_list, &empty_collection)`.
5. The pipeline walks `parent+88` records, comparing each record's wrapper to each entry in the list, unlinking on match.

Without this hook: the inventory has Kliff helm equipped, so `ExpandToMeshes` returns Kliff's `u16`. That resolves to Kliff's wrapper, which is what the pipeline searches for. But `parent+88` contains the substituted target wrapper (Hook 1 installed it). No match, no unlink, ghost helm persists.

**Decompile shape:** two iteration loops over RDX (`a2`) and R8 (`a3`) wrapper lists, both filtering against `0x145BC4638` (the StringInfoVtable sentinel) and using `_InterlockedIncrement` on `wrapper+0x10` for refcount balance. First loop strides 16 bytes (`v7 += 2`); second loop strides 96 bytes (`v17 += 12`). Inner walk is `parent+128 -> records list with stride 8` looking for `*(record+32)+24 == wrapper` (the `parent+88` content-key compare). On match, the unlink helper at `sub_140301720(wrapper)` releases the previous content's refcount.

**Hook signature**: `on_natural_pipeline(a1, a2, a3)`. Calling convention:

| Reg | Param | Meaning |
|---|---|---|
| RCX | `a1` | parent body component pointer |
| RDX | `a2` | pointer to `{data: u64*, count: u32}`, the wrapper list |
| R8  | `a3` | pointer to a second list (empty in `safeTearDown`) |

At entry:

1. Bail if `s_active == false`.
2. Read `listData = *a2` and `listCount = *(u32*)(a2 + 8)` (SEH-guarded).
3. **Empty-list fast path**: if `listData == null || listCount == 0`, trampoline straight through. The engine fires this function from many call sites (e.g. render / animation tick) with empty lists; logging every such call would flood the log.
4. For each entry `i in [0..listCount)`:
   - Read `orig = listData[i*2]` (16-byte stride; wrapper at `+0`).
   - Look up `orig` in `s_swapMap`. If found, write `tgt` to that slot and remember `orig` in a stack-local `saved[]` array.
5. Call trampoline. Engine walks `parent+88` searching for substituted wrappers. Finds tgt (which Hook 1 installed). Unlinks correctly.
6. **Restore originals** from `saved[]`: write each saved src wrapper back over the substituted slot.

The restore step is critical for refcount accounting. The caller iterates the same list after the pipeline returns and calls `sub_140301720(*v19)` on each entry to release the refcount it incremented during list construction. If the entries still held the substituted target values, the wrong wrapper's refcount would be decremented. Restoring keeps the inc / dec pairs balanced.

The hook is `__fastcall` 3-arg; SEH-guarded reads / writes throughout. List size is bounded by `k_maxEntries = 64` (defensive cap; observed lists are 1-3 entries).

**Logging gate**: the hook only logs at `trace` level, and only when at least one substitution happens. PASSTHROUGH and empty-list calls are silent. A successful substitution emits one trace line per entry plus one post-call summary:

```text
[natpipe-hook] hit#N entry[i] SUBST 0x... -> 0x... (src -> tgt) caller_ra=0x...
[natpipe-hook] hit#N done: substituted N restored M result=0x...
```

`write FAULTED` is logged at `warning` for visibility on real anomalies.

**Address resolution**: cascade-driven via `k_naturalPipelineCandidates` (3 anchors registered in `aob_resolver.hpp`; bytes and recovery recipe in section 9.5 of [live-transmog-source-of-truth.md §9](live-transmog-source-of-truth.md#9-aob-cascade-audit-2026-05-08)). On install, `sanity_check_function_prologue(natpipeAbs)` runs as a second gate. If either the cascade or the prologue check fails, the install is skipped with a warning and the helm leak returns; other swap features remain active.

---

## 11. Slot mapping

The engine identifies cosmetic slots by a small `int16_t` "game slot tag", distinct from LT's internal `TransmogSlot` enum. Mapping (`transmog_map.cpp::game_slot_from_transmog`):

| TransmogSlot | engine tag |
|---|---:|
| Helm   | `3` |
| Chest  | `4` |
| Gloves | `5` |
| Boots  | `6` |
| Cloak  | `16` |

These tags appear as `R8` at `sub_141E0A710` entry (per the safeTearDown chain) and as the game-slot field in the inventory's per-slot records. The full 22-slot engine taxonomy (including weapons, accessories, mask, backpack, glasses, etc.) is documented in the project memory `reference_engine_slot_taxonomy_2026-05-07`; LT covers 5 of those 22.

---

## 12. State

The module's hot-path state, all with documented thread-safety:

| Symbol | Purpose | Safety |
|---|---|---|
| `s_active` | Master gate, `bool atomic` | acq / rel; load on every hook entry |
| `s_selSrcIdx[k_slotN]` | per-slot source catalog index | guarded by `s_catalogMtx`; UI-thread mutations only |
| `s_selTgtIdx[k_slotN]` | per-slot target catalog index | guarded by `s_catalogMtx` |
| `s_slotCatalogs[k_slotN]` | per-slot prefab catalog | guarded by `s_catalogMtx`; immutable on hot path post-publish |
| `s_swapMap` | `unordered_map<src_wrapper, tgt_wrapper>` | guarded by `s_mapMtx`; read-only on hot path while `s_active` |
| `s_targetWrappers` | `unordered_set<tgt_wrapper>` | mirrors `s_swapMap` values; read-only on hot path |
| `s_orig` / `s_origNaturalPipeline` | trampoline pointers | atomic stores at install; read on every hook fire |
| `s_lastApplyItems[5]` | last-apply itemId snapshot | guarded by `s_lastApplyMtx`; diagnostic only |
| `s_stringInfoRegistry` / `s_stringInfoVtable` / `s_loaderRegistrySingleton` / `s_apptContainerVtable` | runtime-resolved data globals | `std::atomic<uintptr_t>`, populated from cascades at init |

The hot path (Hook 1 firing per slot during apply, Hook 2 firing per teardown call) does an `acq` load of `s_active`, an `unordered_map::find` under a short-held lock, and one `Interlocked` op. The `find` is read-only because `s_swapMap` is only mutated at apply-time (never from a hook callback), and the `s_active` gate ensures hooks see a stable map.

---

## 13. Activation / deactivation lifecycle

```text
init()                          install hooks; spawn detached boot thread
  |
  +- Hook install: sub_140352AA0 (always installed, gated on s_active)
  +- Hook install: sub_142711DF0 (natural pipeline; cascade + prologue gate)
  |
  v
[boot thread]                   wait for is_world_ready();
  |                             populate_slot_catalogs();
  |                             PresetManager::apply_to_state();
  |                             manual_apply();
  |
  v
[user picks prefab in overlay]  set_selection(slot, src, tgtIdx)
  |                             (pending state; no engine effect yet)
  |
  v
[user clicks Apply All]         apply_all_transmog
  |                             |
  |                             notify_apply_starting -> reactivate_with_selections
  |                             |
  |                             apply_selections_to_swap_map (rebuild s_swapMap)
  |                             |
  |                             s_active = true
  |                             |
  |                             sub_140352AA0 fires per slot during equip
  |                             |
  |                             swap src -> tgt at dest+0x18
  |                             |
  |                             body component renders target prefab
  |
  v
[user switches preset]          clear_all_picked_prefabs_and_deactivate
  |                             (clears UI labels + BMP selections)
  |                             |
  |                             pm.set_active_preset + pm.apply_to_state
  |                             (writes new preset; BMP selections re-set
  |                              from new prefabName values)
  |                             |
  |                             Conditional last_applied_ids[i]=0 only when
  |                             new carrier == prev carrier (early-out case)
  |                             |
  |                             manual_apply
  |
  v
[engine teardown runs]          sub_142711DF0 hook fires
  |                             |
  |                             substitute matching src wrappers -> tgt
  |                             |
  |                             trampoline runs natural pipeline
  |                             |
  |                             pipeline matches tgt in parent+88, unlinks
  |                             |
  |                             restore originals (refcount balance)
  |
  v
[next apply]                    notify_apply_starting -> reactivate_with_selections
                                rebuilds map for new selections (or deactivates)
```

---

## 14. Resolved-address constants (v1.05.01)

| Symbol | Address | Resolution |
|---|---|---|
| `sub_140352AA0` (primary swap) | `0x140352AA0` | AOB cascade |
| `sub_142711DF0` (natural pipeline) | `0x142711DF0` (RVA `0x2711DF0`) | AOB cascade (was hardcoded RVA pre-2026-05-08) |
| StringInfo registry singleton | resolved from `k_stringInfoRegistryCandidates` (was `MEMORY[0x145EF1DE8]`) | AOB cascade |
| StringInfo entry vtable | resolved from `k_stringInfoVtableCandidates` (was `0x145BC4638`) | AOB cascade |
| AppearanceTableLoader registry root | resolved from `k_loaderRegistryCandidates` (was `MEMORY[0x145DDF8B0]`) | AOB cascade |
| Appearance container vtable | resolved from `k_apptContainerVtableCandidates` (was `0x144D24308`) | AOB cascade |
| Appearance name lookup primitive | resolved from `k_apptNameLookupCandidates` (was `sub_1424DF420`) | AOB cascade |

All seven cascades are documented with their three anchor patterns, hit counts, and recovery recipes in section 9 of [live-transmog-source-of-truth.md §9](live-transmog-source-of-truth.md#9-aob-cascade-audit-2026-05-08).

---

## 15. Why this design

A few decisions that aren't obvious from the code alone:

- **Hook the engine's natural pipeline rather than overriding it**. The engine already has correct cleanup semantics for the wrapper it expects to find. Substituting the search target is a 1-line intervention; reimplementing the cleanup ourselves would be hundreds of lines and version-fragile.

- **Restore originals after the trampoline returns**. The caller (`safeTearDown`) refcount-releases each entry in the list after the call. If we left the substituted values in place, refcounts would drift over many apply / switch cycles. The save-and-restore is per-call O(N) on a small list (1-3 entries typically; bounded by 64).

- **Preserve `s_swapMap` across deactivate**. Most users will toggle selections frequently within a session. Wiping the map on every deactivate would force a re-walk of the source name set on every Apply. Persisting until shutdown amortizes the cost.

- **Apply-only activation**. Picker mutations are pending; the engine doesn't see anything until Apply runs. Mirrors the carrier hybrid flow and prevents the UI from triggering visual churn for every dropdown click. Single integration point (`notify_apply_starting`) handles activation, deactivation, and preset transitions.

- **`prefabName` lives in PresetSlot, not as session-only state**. Saved presets persist body-mesh overrides on disk. On boot, the catalog walk feeds into `apply_to_state` and `manual_apply` to re-apply the active preset automatically: same UX as plain carrier transmogs.

- **Hardcoded Kliff carrier set, not user-configurable**. The carrier is internal plumbing (forces the engine to load the wrappers `s_swapMap` keys expect). Letting the user pick a non-Kliff carrier would silently break the substitution (different wrapper class). The hex carrier id input is disabled when a prefab is active, with a tooltip explaining why.

- **`k_maxEntries = 64` for the natpipe stack-saved array**. Observed lists during teardown are 1-3 entries. 64 gives 50x headroom while staying on the stack (no heap allocation in a hook). Entries beyond 64 are skipped (graceful degradation).

- **natpipe hook logs at `trace` level only on actual substitutions**. The hook fires hundreds of times per second from various render-path call sites; logging every call would flood the trace stream. Empty-list and no-substitution calls are silent. SUBST and post-call summary lines surface only when real cleanup work happened.

- **Cascade + prologue gate at install**. Either of the two failing makes the install a no-op rather than scribbling at the wrong address. The cascade's prologue-fallback already covers most one-byte register-tail repurposes; the explicit `sanity_check_function_prologue` is a second gate against a cascade that picks up a fragment of an unrelated function on a future patch reshuffle.

---

## 16. Files of interest

| File | Purpose |
|---|---|
| `prefab_wrapper_swap.cpp` | Hooks 1 + 2, swap map, catalog enumeration helpers (`walk_string_info`, `enumerate_loader_registry_into_catalog`), cascade-driven init for the four runtime-resolved data globals, lifecycle |
| `prefab_wrapper_swap.hpp` | Public API: `is_active`, `is_target_wrapper`, catalog accessors |
| `aob_resolver.hpp` | Cascading AOB definitions (mod-local + CDCore-shared aliases) |
| `transmog_map.cpp` | `game_slot_from_transmog` slot enum mapping |
| `transmog.cpp` | Boot-time orchestration; calls `register_config` + `init` |
| `transmog_apply.cpp` | Apply-path orchestration; invokes `notify_apply_*`; `default_carrier_for_slot` and `k_kliffCarriers` live here |
| `preset_manager.cpp` | `prefabName` JSON ser / de + `apply_to_state` / `capture_from_state` |
| `overlay_ui.inl` | Picker UI, slot row layout, preset switch wiring |

---

## 17. Diagnostic log signatures

Look for these in `CrimsonDesertLiveTransmog.log` to understand a session at a glance:

```text
[body-mesh-ptr-swap] installed at 0x140352AA0 (INACTIVE -- ...)
[body-mesh-ptr-swap] NaturalPipeline hook INSTALLED at 0x142711DF0
[body-mesh-ptr-swap] boot-scan: world ready, populating per-slot catalog...
[body-mesh-ptr-swap] Catalog populated: helm=N chest=N cloak=N gloves=N boots=N (Nms)
[body-mesh-ptr-swap] boot-scan: preset prefabs re-synced and apply scheduled.

[body-mesh-ptr-swap]   slot[0] RESOLVED "cd_phm_00_hel_00_0435_d" (2 src) -> "cd_nhw_no_ub_20027" (0x...)
[body-mesh-ptr-swap] reactivated via UI selections (1 slot(s) bound)
[body-mesh-ptr-swap] SWAP src=0x... -> tgt=0x... (subst #N)

[body-mesh-ptr-swap] DEACTIVATED -- swap map RETAINED for next activation. Engine cleanup deferred to natural-pipeline hook.
```

At `trace` log level the natpipe-hook also surfaces successful substitutions:

```text
[natpipe-hook] hit#N entry[i] SUBST 0x... -> 0x... (src -> tgt) caller_ra=0x...
[natpipe-hook] hit#N done: substituted N restored M result=0x...
```