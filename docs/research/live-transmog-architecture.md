# Live Transmog -- Runtime Architecture

Reverse-engineering reference for the Live Transmog mod's runtime pipeline. This document maps the apply pipeline to concrete functions, offsets, and source locations; byte-level anchors (AOBs, struct offsets) live in `live-transmog-source-of-truth.md`. The cross-character body-mesh swap layered on top of this pipeline lives in `live-transmog-prefab-wrapper-swap.md`.

Verified against **Crimson Desert v1.05.01** (image base `0x140000000`, FileVersion `1.0.0.1070`).

---

## 1. Pipeline summary

The engine owns its own rules for which visual mesh is drawn per armor slot (helm, chest, cloak, gloves, boots). Live Transmog does not rewrite those rules. It operates by:

1. Asking the engine to remove the real armor's mesh from the live scene graph (the SafeTearDown path).
2. Asking the engine to run its own "show this item" path (`SlotPopulator`) with a substituted item id.

The auth table at `*(a1+0x78)` (what is actually equipped, and what is serialised to the save file) is never mutated. Mod state lives in DLL memory and is released on quit.

### Why this design

The straightforward "rewrite the engine's part list" approach was rejected because the engine re-derives the list from the auth table on most state transitions (zone load, save load, character swap, glide exit, dismount). Any direct mutation has to be re-applied on every transition or it is wiped. Driving the engine through its own SlotPopulator entry point makes the engine treat the substituted mesh as "the item it just decided to equip", which gives us correct cleanup semantics for free across every path the engine itself supports.

The carrier hybrid (section 4.3) is the secondary consequence: cross-class items are rejected at the engine's equip-type gate, so the mod patches the carrier descriptor's equip-type byte (`+0x42`) into the target descriptor for the duration of one `SlotPopulator` call. Patching exactly one field, rather than constructing an item from scratch, keeps the mod compatible with future changes to the descriptor layout that do not touch `+0x42`.

---

## 2. Terminology

| Term | Plain English | Technical meaning |
|------|---------------|-------------------|
| **Real item** | Whatever the character actually has equipped in the inventory screen. | Entry in the auth table at `*(a1+0x78)`. Written to the save file. |
| **Fake item** | The mesh the mod is asking the engine to draw instead. | `slot_mappings[i].targetItemId` in mod memory. Not persisted in save file. |
| **Carrier** | Helper item the mod uses to pass the engine's class check. | Member of `k_kliffCarriers` / `k_oongkaCarriers` / `k_damianeCarriers` in `transmog_apply.cpp`. |
| **Auth table** | Authoritative equipment record (server-synced). | Stride-`0xC8` array at `*(a1+0x78)+0x08`, count at `+0x10`. |
| **PartDef / scene graph cache** | Live in-RAM list of meshes the renderer walks this frame. | Dispatch cache at `a1+0x1D8` (basePtr), `+0x1E0` (count), `+0x1E4` (cap) on v1.04+; v1.03.01 had the triple at `+0x1B8 / +0x1C0 / +0x1C4`. |
| **Tear-down** | Scene-graph remove: "stop drawing this mesh." | Call path passing through `SafeTearDown` (resolved via cascade). |
| **a1** | Equip-wrapper pointer passed back into engine functions. | Player-component pointer; engine reads many offsets from it. |
| **Dispatch** | One full tear-down-and-reapply pass. | Body of `apply_all_transmog()` in `transmog_apply.cpp`. |

The dispatch-cache offset shift between v1.03.01 and v1.04+ is documented in `transmog_apply.cpp` as `k_compSlotCacheBasePtrOffset = 0x1D8`. Reading the old offsets on v1.04+ returns zero for count and silently makes apply a no-op; writing the old offsets corrupts the component a slot at a time. The mod uses the v1.04+ offsets unconditionally on the current build of record (v1.05.01).

---

## 3. End-to-end lifecycle

```mermaid
flowchart TD
    A[Game loads the world] --> B[Load-detect thread wakes<br/>every 1000 ms]
    B --> C{WS chain resolves<br/>a valid component?}
    C -- no --> B
    C -- yes --> D[Resolver reads<br/>CDCore::current_controlled_character<br/>Kliff / Damiane / Oongka / Unknown]
    D --> E{Current char differs from<br/>PresetManager active?}
    E -- yes --> F[Switch UI preset list<br/>rebuild slot_mappings<br/>reset drop-detection]
    E -- no --> G[Idle]
    F --> H[Schedule a transmog pass]
    G --> H

    subgraph userActions [user triggers]
        U1[User equips gear<br/>BatchEquip hook fires]
        U2[User toggles a preset<br/>in the overlay]
        U3[User hotkey: apply]
    end
    U1 --> H
    U2 --> H
    U3 --> H

    H --> W[Debounce worker wakes<br/>~125 ms later]
    W --> X[sync_active_char_to_live<br/>reads live char identity<br/>rebuilds mappings if needed]
    X --> Y[apply_all_transmog a1]
    Y --> Z[Tear-down + apply pipeline<br/>see section 4]
    Z --> B
```

Components:

- **Load-detect thread** (`transmog_worker.cpp :: load_detect_thread_fn`). Wakes once per second. Resolves the player component via the `WorldSystem -> ActorManager -> UserActor -> +0xD8` chain, and queries the shared `CDCore::current_controlled_character()` resolver to decide whether to switch the active preset on character swap. The chain walk and decode rules live in `live-transmog-source-of-truth.md` section 1.
- **Hooks** (`transmog_hooks.cpp`). Two apply triggers: `BatchEquip` and `VisualEquipChange (VEC)`. On fire they record the passed-in `a1` and schedule a pass. Resolution is via shared CDCore cascades (`k_batchEquipCandidates`, `k_visualEquipChangeCandidates`).
- **Debounce worker** (`transmog_worker.cpp :: run_debounced_apply`). Single thread consuming scheduled apply requests, coalescing bursts so multi-slot inventory commits become one apply pass.

The rest of this document focuses on one apply pass (node `Y`).

---

## 4. One apply pass

`apply_all_transmog()` in `transmog_apply.cpp` executes as a short state machine:

```mermaid
stateDiagram-v2
    [*] --> Guard
    Guard: Enter guarded region
    Guard: in_transmog = true
    Guard: suppress_vec = true
    Guard --> SnapshotReal

    SnapshotReal: Snapshot real item ids per slot
    SnapshotReal: walks auth table at *(a1+0x78)
    SnapshotReal: realItemId[k] filled via IndexedStringLookup
    SnapshotReal --> ClearCache

    ClearCache: Clear dispatch cache entries
    ClearCache: a1+0x1D8 array, reset subCount for our slots only
    ClearCache: leaves other slots untouched
    ClearCache --> PhaseA

    PhaseA: Phase A tear-down previous fake
    PhaseA: for every slot that had a fake applied before
    PhaseA: call tear_down_by_item_id(a1, prevFakeId, slotTag)
    PhaseA: if the previous fake used a carrier also tear down the carrier
    PhaseA: briefly flips charClassBypass JZ to JMP so the NPC item resolves
    PhaseA --> PhaseB

    PhaseB: Phase B tear-down real item
    PhaseB: for every active slot
    PhaseB: call tear_down_real_part(a1, slotTag)
    PhaseB: marks real_damaged[slot] = true
    PhaseB --> Apply

    Apply: Apply the fake
    Apply: for each active slot with a target item
    Apply: decide direct apply or carrier-patch
    Apply: call SlotPopulator(a1, itemData, swapEntry)
    Apply --> Restore

    Restore: Restore real items for unticked slots
    Restore: if user unticked a slot that we had torn down
    Restore: call apply_transmog(a1, realId) to put the mesh back
    Restore --> SuppressSetup

    SuppressSetup: Arm PartAddShow suppression mask
    SuppressSetup: bit per slot where fake differs from real
    SuppressSetup: stops glide-exit and landing anims from showing real helm
    SuppressSetup --> Unguard

    Unguard: Exit guarded region
    Unguard: suppress_vec = false
    Unguard: in_transmog = false
    Unguard --> [*]
```

The `__finally` around this state machine clears `in_transmog` and `suppress_vec` even if a step throws an SEH exception; without it, a fault would leave hooks gagged permanently and no further equip events would fire through the mod.

### 4.1 Phase A -- "forget the old fake"

When the mod applies a new preset, the previous preset is still on screen. Phase A walks the slots the mod wrote to last time (`last_applied_ids`) and asks the engine to remove those meshes from the scene graph:

```text
tear_down_by_item_id(a1, prevFakeId, slotTag)
  -> IndexedStringLookup(&prevFakeId)           // descriptor hash
  -> SafeTearDown(a1, hash, slotTag)            // cascade-resolved
                                                // (sub_14075F9E0 / SafeTearDown
                                                // candidates in aob_resolver.hpp)
       -> internal scene-graph detach           // detaches mesh, particle
                                                // emitters, anim controllers
                                                // from the scene
```

No write to the auth table at `*(a1+0x78)`. No server-side notification. Save file bytes unchanged across the call. SafeTearDown is a scene-graph operation only -- it removes the mesh from the render list and drops the renderer's internal refs.

If the previous apply used a carrier, the mod briefly flips `charClassBypass` from `0x74` (`jz short`) to `0xEB` (`jmp short`) so the engine's class check accepts the NPC carrier during tear-down. The byte is restored to `0x74` the moment tear-down returns. The address is resolved by `k_charClassBypassCandidates` (3-anchor cascade, see [live-transmog-source-of-truth.md §9](live-transmog-source-of-truth.md#9-aob-cascade-audit-2026-05-08)). The mod refuses to install if the resolved byte is anything other than `0x74`.

### 4.2 Phase B -- remove the real mesh so the fake can take its place

Phase B calls a different tear-down path that walks the auth table directly:

```text
tear_down_real_part(a1, slotTag)
  -> container = *(a1 + 0x78)
  -> base     = *(container + 0x08)              // stride 0xC8 entries (k_compEntryStride = 0xD0
  -> count    = *(container + 0x10)              //   in the live source; legacy 0xC8 noted below)
  -> for i in 0..count
       -> entry = base + i * stride
       -> if *(entry + slotTagOffset) == slotTag // slot tag at +0xC8 (k_compEntrySlotTagOffset)
              realItemId = *(entry + 0x88)       // alt first
              if realItemId == 0xFFFF:
                  realItemId = *(entry + 0x08)   // fall back to primary
              -> IndexedStringLookup(&realItemId)
              -> SafeTearDown(a1, hash, slotTag)
```

The walk reads entries to find the real item id; it does not write to any entry. Tear-down is renderer-side only.

After Phase B, `real_damaged[slotIdx] = true` records that the real mesh is currently missing from the scene graph. That flag is consulted in Restore if the user unticks a slot in the overlay.

The historical stride-`0xC8` value is documented in older notes; the live source at `transmog_apply.cpp` declares `k_compEntryStride = 0xD0`.

### 4.3 Apply -- draw the fake mesh

For every active slot with a target item, the mod calls SlotPopulator (the same function the engine itself uses to attach armor). Two flavours:

**Direct apply** when the fake is something the active character can normally wear:

```text
apply_transmog(a1, targetItemId)
  itemData = { word targetId at +0, defaults elsewhere }
  swapEntry = zero-initialised via InitSwapEntry
  SlotPopulator(a1, &itemData, &swapEntry)
```

**Carrier-patch apply** when the fake belongs to another character or an NPC. The engine's equip gate checks a `u16` at `desc+0x42` (equip-type, `0x0004` for Kliff, `0x0001` for Oongka / Damiane / NPCs). If the fake's byte does not match the active character's expected value, direct apply is rejected. The mod builds a hybrid descriptor:

```mermaid
flowchart LR
    T[Target descriptor<br/>0x400 bytes total] -->|memcpy| H[Hybrid buffer<br/>VirtualAlloc'd]
    C[Carrier descriptor] -->|overlay +0x42 u16| H
    H -->|InterlockedExchange64<br/>into carrier slot| Slot[Carrier slot pointer<br/>in catalog]
    Slot --> SP[SlotPopulator<br/>reads target meshes<br/>sees carrier equip-type]
    SP -->|on return<br/>InterlockedExchange64| Restore[Original carrier<br/>descriptor pointer]
    Restore --> Free[VirtualFree hybrid buffer]
```

Exactly one field is patched: the equip-type at `+0x42`. Every other byte in the hybrid comes from the target (meshes, textures, tints, rule classifiers). SlotPopulator reads the patched byte during its visual-config matching loop, accepts the carrier's class identity, then renders the target's visual data.

During the call, `charClassBypass` is again flipped to `0xEB` so any deeper class check also passes. Bypass and descriptor pointer are both restored inside a `__finally` so an SEH fault cannot leave the catalog corrupted. The hybrid buffer is freed immediately after.

Source: `CrimsonDesertLiveTransmog/src/transmog_apply.cpp` (`k_descBufSize = 0x400` constant; hybrid assembly + bypass flips inside `apply_transmog_carrier_path` and `tear_down_by_item_id`).

### 4.4 Restore -- put the real item back if the user unticked

If the user turns off a slot in the overlay, the mod needs to put the real mesh back. This is a SlotPopulator call with the real item id the mod snapshotted during the real-id sweep at the top of the dispatch.

### 4.5 PartAddShow suppression

The engine occasionally bypasses the regular scene-graph path and calls `PartAddShow` directly, most notably during glide exits, horse dismounts, and some action-chart landings. If that happens while the mod has torn down the real mesh, the real helm flashes visible for a frame or two. The mod hooks PartAddShow (cascade `k_partAddShowCandidates`) and keeps a small table of suppressed part hashes; while a slot is in the torn-down state, any add-show call for one of that slot's part hashes returns zero immediately without touching the scene graph.

Source: `CrimsonDesertLiveTransmog/src/part_show_suppress.cpp`.

---

## 5. Memory write surface

Every absolute write the mod performs:

| Address | Size | Frequency | Reason | Restore |
|---------|------|-----------|--------|---------|
| `charClassBypass` byte (cascade-resolved) | 1 byte | during carrier-apply / tear-down | Flip `JZ` (`0x74`) to `JMP` (`0xEB`) so NPC carriers pass the class check | Restored within the same function under `__finally` |
| `a1 + dispatch-cache triple` (basePtr / count / cap) | ~16 bytes per slot | during apply | Invalidate stale cache entries for mod-owned slots | Natural overwrite by SlotPopulator on the next apply |
| Mod-allocated heap buffer (`VirtualAlloc`) | `0x400` bytes per carrier apply | during carrier-apply | Build hybrid descriptor | Freed via `VirtualFree` before the function returns |
| Carrier slot pointer in item catalog | 8 bytes via `InterlockedExchange64` | during carrier-apply | Redirect catalog lookup to the hybrid for one call | Swapped back to the original within the same function under `__finally` |

No writes to the auth table at `*(a1+0x78)` or its entries. No writes to save-file-backed state. On DLL unload, all mod-side arrays are released; the hybrid buffer is freed; `charClassBypass` is `0x74` because every flip is paired with a restore in a `__finally`.

The body-mesh pointer-swap feature (separate document) adds two more writes (one to a struct copy operator's intermediate slot, one to a wrapper-list during natural-pipeline teardown), both at well-defined hook entry / exit boundaries with the same `__finally` discipline.

---

## 6. Event-driven state transitions

| Event | What the mod sees | Mod response |
|-------|-------------------|--------------|
| Zone transition | `BatchEquip` hook fires with a different `a1`. | Wipes `real_damaged` and `last_applied_real_ids` (arrays referenced previous world's memory). Schedules a fresh dispatch. |
| Save-load | Same as zone transition. Server resends auth table, BatchEquip fires, mod re-applies. | Preset file unchanged. Real gear unchanged. Fake gear re-applies within a second. The CDCore resolver invalidates its last-known-good cache when the UserActor singleton pointer changes. |
| Character control swap (Kliff -> Damiane) | Load-detect thread reads a different identity from the controlled-character resolver. | Switch UI preset list, rebuild `slot_mappings` from Damiane's active preset, clear drop-detection, schedule a dispatch. |
| World reload (death, fast travel, manual reload) | `resolve_player_component()` returns a new component pointer. | Same flow as zone transition. |
| Uninstall the mod | DLL unloaded on next launch. | No-op. Save file was never touched. |

Source: `CrimsonDesertLiveTransmog/src/transmog_worker.cpp` (`resolve_player_component` and the load-detect thread).

---

## 7. Recursion guards

`in_transmog` and `suppress_vec` look similar but serve different scopes.

- **`in_transmog`** is scoped to a single SlotPopulator call. Set true on entry, cleared on return. Without it, SlotPopulator's own internal equip-event fan-out would re-enter the VEC hook, which would schedule another apply, which would call SlotPopulator again. One guard, one call, ~50 ms.
- **`suppress_vec`** wraps the entire dispatch. Set true at the top of `apply_all_transmog`, cleared at the bottom. A dispatch touches multiple slots and therefore calls SlotPopulator multiple times. Each SlotPopulator call can fire VEC internally. Without the outer guard, each VEC fire would schedule another dispatch behind the current one, producing visible flicker and cascading applies that never converge.

Both guards are `std::atomic<bool>` in `shared_state.cpp` and are paired with `__finally` restores so a fault during apply cannot leave them stuck muted. The body-mesh pointer-swap feature reads `Transmog::in_transmog()` on its own hook entry to keep its substitution gated to the mod's own SlotPopulator drives, never the engine's organic equip flows.

---

## 8. Verification checklist

To confirm the mod does not touch persistent state:

1. **Compare saves.** Make a save while transmogged. Quit. Remove the mod. Relaunch. Load the save. Real gear is present; transmog mesh is gone. Save-on-disk bytes are identical to a save made with the mod never installed.
2. **Inspect the auth table in Cheat Engine.** Walk `*(a1 + 0x78) + 0x08`, stride `0xD0` (or `0xC8` if the legacy stride is what is live), and compare the item word at `+0x08` / `+0x88` before and after an apply. Bytes are identical. Only the dispatch cache at `a1 + 0x1D8` and the renderer-side mesh refs change.
3. **Read the log.** With `LogLevel = Trace` in the INI, every dispatch prints the source and target of every tear-down and every apply call, the carrier ids used, and the hybrid descriptor writes.

---

## 9. Patch-migration anchors

If the game binary shifts, the highest-churn items are:

- SlotPopulator / BatchEquip / VEC entry points. Re-scan via the AOBs in `aob_resolver.hpp` (mod-local for SlotPopulator / SafeTearDown / SubTranslator; CDCore-shared for BatchEquip / VEC / WorldSystem). The cascading scanner with prologue-fallback handles the common shifts; full audit is at [live-transmog-source-of-truth.md §9](live-transmog-source-of-truth.md#9-aob-cascade-audit-2026-05-08).
- The `charClassBypass` JZ byte. Cascade tries 3 anchors; on resolve, the mod sanity-checks the byte equals `0x74`. If a future patch retunes the classifier scan loop into a `CMOV` or rewrites it as a vtable dispatch, the cascade fails and the mod refuses to install rather than scribbling at the wrong site.
- Auth-table stride and slot-tag offset (`k_compEntryStride`, `k_compEntrySlotTagOffset` in `transmog_apply.cpp`).
- Dispatch-cache triple offsets (`k_compSlotCacheBasePtrOffset` etc.). These shifted by `+0x20` between v1.03.01 and v1.04.00; the live source is at the v1.04+ values.
- Descriptor byte `+0x42` (equip-type). If the engine starts gating equip on additional bytes, the patch list in the carrier-hybrid path will need to grow.

---

## 10. Related documents

- `live-transmog-source-of-truth.md` -- byte-level reference for hook RVAs, struct offsets, the WorldSystem chain, and the controlled-character resolver.
- `live-transmog-prefab-wrapper-swap.md` -- body-mesh pointer-swap feature layered on top of this pipeline (cross-character body re-skin via wrapper-pointer substitution at the slot-populator copy operator).
- [live-transmog-source-of-truth.md §9](live-transmog-source-of-truth.md#9-aob-cascade-audit-2026-05-08) -- section 9 of the byte-level reference; AOB cascade audit covering every hardcoded RVA that was replaced by a 3-anchor cascade. Includes per-cascade hit counts and recovery recipes.
