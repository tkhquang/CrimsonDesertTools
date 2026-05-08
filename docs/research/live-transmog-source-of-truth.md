# Live Transmog -- Source of Truth

Byte-level reference for every address, offset, and AOB the Live Transmog mod depends on in the Crimson Desert binary. Structured so that when a game patch shifts an address, the hook point can be re-found from surrounding byte patterns. Each section lists the semantic role first, then the concrete offsets / bytes / AOBs.

Verified against **Crimson Desert v1.05.01** (image base `0x140000000`, FileVersion `1.0.0.1070`). Many anchors below were originally probed on v1.03.01; cross-build deltas are called out inline. The seven AOB cascades (StringInfoRegistry, StringInfoVtable, LoaderRegistry, ApptContainerVtable, NaturalPipeline, ApptNameLookup, CharClassBypass) were authored against v1.05.01 and live in section 9 below ("AOB cascade audit (2026-05-08)").

---

## 1. WorldSystem chain: resolving the currently-controlled actor

### 1.1 Static entry point

The mod scans for the WorldSystem global-pointer holder via `CDCore::Anchors::k_worldSystemCandidates` (the WORLD_SYSTEM_P*_SmallFunc family of patterns shared between LT and EH). On v1.03.01 the holder resolved to:

```text
v1.03.01 holder : CrimsonDesert.exe + 0x05D2AEE8   (absolute 0x145D2AEE8)
v1.05.01 holder : CrimsonDesert.exe + 0x05EF15B0   (absolute 0x145EF15B0)
```

`*WS_holder` -> WorldSystem instance pointer (lives on the game heap; value changes per session).

The static slot moved between v1.03.01 and v1.05.01 (RVA shifted by `+0x1C66C8`). The cascade in `CDCore::Anchors::k_worldSystemCandidates` (P1 SmallFunc whole-function anchor) resolves it dynamically regardless; the v1.05.01 absolute is recorded here for cross-build delta tracking.

### 1.2 Chain to the currently-controlled character actor

```text
WS holder          : <cascade-resolved>      (scanned via AOB at init)
  +0x30            -> ActorManager           (singleton, updated on world load)
    +0x28          -> UserActor              (singleton, ClientUserActor class;
                                              the POINTER itself does NOT swap
                                              when the player switches chars)
      +0xD0        -> "primary" body actor   (stable = Kliff in current
                                              story progression)
      +0xD8        -> CURRENTLY-CONTROLLED   (this is the useful one;
                     body actor              switches when user swaps char)
        +0x68      -> sub-component
          +0x38    -> wrapper / "player component"
                    (what the apply pipeline accepts as `a1`)
        +0x88      -> typeEntry pointer
          +1 (u8)  -> role byte (see section 2.2)
```

`user+0xD8` falls back to Kliff's actor when Kliff is the active character (so `+0xD0` and `+0xD8` coincide). Reading `+0xD8` unconditionally is therefore safe for all three playable characters.

### 1.3 Controlled-character resolver (`CDCore`)

Authoritative reference for the resolver implemented in `CrimsonDesertCore/src/controlled_char.cpp`. The resolver answers a single question: which of the three playable protagonists is the user currently controlling? It returns one of `Kliff`, `Damiane`, `Oongka`, or `Unknown`. Two consumer mods (`CrimsonDesertLiveTransmog`, `CrimsonDesertEquipHide`) drive per-character behaviour off the result.

#### 1.3.1 Chain walk

The resolver walks five pointer dereferences plus two byte reads on every query. No caching of intermediate pointers; the chain is cheap and any heap pointer in it can be invalidated by a save load or character swap, so re-walking is the only correctness-preserving strategy.

```text
WorldSystem holder static    : <cascade-resolved>    (= CrimsonDesert.exe + 0x05D2AEE8 on v1.03.01)
  +0x00 (qword)              -> WorldSystem instance        [session heap]
    +0x30 (qword)            -> ActorManager singleton
      +0x28 (qword)          -> UserActor singleton (ClientUserActor)
        +0xD0 (qword)        -> primary actor slot           [always Kliff]
        +0xD8 (qword)        -> controlled client actor      [rotates on swap]
          +0x60 (u8)         -> per-actor slot-index byte    [allocation order]
```

Notes on each step:

* The WorldSystem holder static slot is session-dependent (allocated during world load). The consumer mods AOB-scan for this address and hand it to Core via `set_world_system_holder()`; Core does not scan for it itself.
* `ws + 0x30` is the ActorManager singleton. Reallocated on every save load.
* `am + 0x28` is the UserActor singleton (RTTI class `.?AVClientUserActor@pa@@`). The pointer at this slot is stable for the lifetime of a save: in-session character swaps do NOT rewrite it. Save loads do reallocate it; consumers use the change of this pointer as the trigger for cache invalidation.
* `user + 0xD0` is the "primary" actor slot. Always points at Kliff's actor in the current story progression, regardless of which character is currently controlled. This is a structural invariant that the decode below relies on.
* `user + 0xD8` is the currently-controlled client actor (RTTI class `.?AVClientChildOnlyInGameActor@pa@@`). Coincides with `user + 0xD0` when Kliff is controlled, and rotates to a Damiane or Oongka slot when one of them is controlled.

Every intermediate pointer is rejected if it falls below the 64 KiB guard region (`k_minValidPtr = 0x10000`). The whole walk runs inside a single `__try` block; any access fault returns an invalid probe which the caller treats as "no live identity, fall back to cache".

#### 1.3.2 Decode rules

**Rule 1: Kliff via structural invariant.** When `*(user + 0xD0) == *(user + 0xD8)` the primary slot coincides with the controlled slot, which only happens when Kliff is controlled. This is a pure pointer comparison with no dependence on numeric fields that could shift across saves or patches.

**Rule 2: companions via slot-index diff.** When the two slots differ, a companion is controlled. The byte at `actor + 0x60` is a per-actor slot index assigned at allocation time in a fixed character order (Kliff, then Damiane, then Oongka). The absolute value varies per save because the slot-index pool is shared with other actors, but within any given save the diff between the controlled slot and the primary slot uniquely identifies the companion:

| Companion diff | Character |
|----------------|-----------|
| `+1` | Damiane |
| `+2` | Oongka |
| anything else | Unknown |

Why structural compare instead of a numeric identity field: prior resolver attempts used a packed `(party_class << 32) | char_kind` key (the u32s at `actor + 0xDC` and `+0xEC`). That key shifts with party composition and produces ambiguous values across companions in the same save. The structural `D0 == D8` compare plus slot-diff is more robust because it depends on engine allocation order rather than serialized numeric tags.

#### 1.3.3 Observed values

| Build | Save shape | Kliff `+0x60` | Damiane `+0x60` | Oongka `+0x60` |
|-------|------------|---------------|-----------------|----------------|
| v1.03.01 | Kliff + Damiane | `1` | `2` | (absent) |
| v1.03.01 | 3 protagonists | `6` | `7` | `8` |
| v1.05.01 | Kliff + Damiane | `1` | `2` | (pending) |

The `+0x60` byte stays stable when a character is backgrounded. On a v1.03.01 save, reading it on Oongka after the user swapped away from him left the value `8` across the swap. On v1.05.01, an in-session swap from Kliff to Damiane left the WS / ActorManager / UserActor pointers identical and the primary slot byte unchanged. So the decode produces the same result regardless of which character is currently controlled, as long as the primary slot stays Kliff.

The Oongka `+0x60` byte on a v1.05.01 save where Oongka is in the playable party is unverified; the captured save above only had Kliff + Damiane available.

#### 1.3.4 Known assumption

The decode assumes the engine's character allocation order is always `Kliff, Damiane, Oongka`. A save containing only Kliff + Oongka (no Damiane) would allocate Oongka at `Kliff + 1` and would be decoded as Damiane. No such save has been observed in the current game content; if one arises, the consumer mod's overlay allows manual override and the misdetection is a UX hiccup, not a correctness failure.

#### 1.3.5 Last-known-good cache

The resolver keeps one atomic `s_lastGoodKey` that holds the most recent identity key matching one of the three known values. Each query:

1. Walks the chain (SEH-isolated).
2. If the resulting key is one of `{Kliff, Damiane, Oongka}`, refreshes `s_lastGoodKey` with it and uses it as the effective key.
3. Otherwise (zero from a faulted walk, torn read mid-swap, unknown future value) ignores the live key and uses whatever is currently in `s_lastGoodKey`. The cache may itself be empty (zero), in which case the resolver returns `Unknown`.

The cache exists to absorb torn reads at the moment the engine is rewriting `user + 0xD8` during a swap. Without it, a query that landed mid-rewrite would briefly resolve to `Unknown` and any consumer holding state per-character (preset selection, parts override) would flap.

`invalidate_controlled_character()` zeroes the cache. Consumers MUST call it on save-load transitions:

* **Trigger:** the UserActor pointer at `am + 0x28` changes value.
* **Why:** the previous save's identity key may still reflect the old character whose actor lived in heap that has just been freed and reallocated.
* **Why not on character swap:** the UserActor pointer does not change on swaps; only `user + 0xD8` rotates. Invalidating on swap would wipe a cache the resolver could have re-populated within the same query.

Both `transmog_worker.cpp` (LiveTransmog load-detect thread) and `player_detection.cpp` (EquipHide background tick) implement this trigger. The first-boot path is gated by a `prevUser != 0` check so the very first tick after world load does not invalidate a cache the resolver may have just populated.

EquipHide additionally has an atomic-swap save-load detector that drives the same `invalidate_controlled_character()` call via `radial_swap_pending()` on CDCore (see project memory `project_eh_atomic_save_load_detection_2026-05-03`); its purpose is to disambiguate "user pressed radial-swap button" from "engine atomically replaced the controlled actor across save-load" so that LT does not commit the wrong preset on the new world.

#### 1.3.6 Consumer init contract

The Core resolver does not own its own AOB scan. Consumers AOB-scan for the WorldSystem holder (each mod has a `WorldSystem` AOB candidate set in `CDCore::Anchors`) and publish the resolved address via:

```cpp
CDCore::set_world_system_holder(addrs.worldSystem);
```

Both consumers can publish the same address; later writers win, which is the intended behaviour after a re-resolve. Until at least one consumer has called `set_world_system_holder()` with a non-zero address, every resolver query short-circuits to `Unknown`.

The first call also emits a single info log line:

```text
ControlledChar: WorldSystem holder published at 0x...
```

A separate one-shot diagnostic logs each unique probe signature (the `(primary_slot, controlled_slot)` pair) the resolver observes:

```text
ControlledChar: probe primary_slot=6 controlled_slot=8 diff=2 -> Oongka
```

The trailing name is the decoded character (`Kliff` / `Damiane` / `Oongka` / `Unknown`). This is the only signal that the resolver is wired up correctly on a future patch; the four-slot seen-set covers the three expected diff combinations plus one transient anomaly without log spam.

### 1.4 How the mod resolves `a1` at apply time

```cpp
// transmog_worker.cpp :: resolve_player_component()
ws       = *(WS_holder)
am       = *(ws  + 0x30)
user     = *(am  + 0x28)
actor    = *(user + 0xD8)              // per-character
te       = *(actor + 0x88)             // pointer-valid check only
sub1     = *(actor + 0x68)
a1       =  *(sub1 + 0x38)             // passes into SlotPopulator
```

If the binary shifts and offsets change, start by re-finding the chain via:

* `WORLD_SYSTEM_P*` AOBs in `CDCore::Anchors::k_worldSystemCandidates`.
* The RTTI class name on the singleton at `am+0x28` should read `.?AVClientUserActor@pa@@`.
* `user+0xD0` / `+0xD8` slots both hold `.?AVClientChildOnlyInGameActor@pa@@` pointers.

---

## 2. Character identification

### 2.1 Body-kind classifier sets

Every armor's rule list (`desc+0x248`, stride `0x38`, count at `desc+0x250`) contains classifier arrays (ptr `rule+0x20`, count `rule+0x28`, u16 tokens). The tokens partition the catalog into disjoint body families:

| Role | Token set |
|------|-----------|
| **Male humanoid** | `0x0018, 0x0058, 0x02E3` |
| **Female humanoid** | `0x0072, 0x0382, 0x0300` |
| Kliff-specific | `0x0015, 0x039D` |
| Oongka-specific | `0x0028` |
| Damiane / Demian-specific | `0x0021` |
| Horse family | `0x142B, 0x0037, 0x14A5, 0x14D6, 0x1501` |
| Pet cat | `0x183F, 0x22C7, 0x22B4, 0x22BA, 0x22AE` |
| Pet dog | `0x0D69, 0x4397, 0x2214, 0x0D74` |

Any token `>= 0x1000` is classified by the mod as non-humanoid (`k_nonHumanoidTokenThreshold` in `item_name_table.cpp`). Humanoid tokens observed so far all fit below `0x0400`.

### 2.2 Role byte: NOT a reliable identifier

The `typeEntry+1` byte is role-based (controlled vs backgrounded), not character-based. Companion actors share the same typeEntry pool slot when controlled, so multiple characters decode to the same byte value and cannot be distinguished here. Identity decoding belongs to the resolver documented in section 1.3, which compares the primary and controlled actor pointers at `user+0xD0` and `user+0xD8` for the Kliff case and uses the allocation-order slot-index byte at `actor+0x60` to disambiguate companions.

`*(actor+0x88)+1` is **not** a player / NPC flag despite older code treating it as one. Observed values:

| Character state | Byte |
|--------------------------------------------------|-------------|
| Kliff, controlled (ClientUserActor) | 0 |
| Kliff, backgrounded (ClientChildOnlyInGameActor) | 1 |
| Damiane, controlled | 0 |
| Damiane, backgrounded | 4 |
| Oongka | also varies |

Treat pointer-validity as the only safe gate. The in-hook filter (`is_player_actor` in `transmog_hooks.cpp`) accepts the byte being in `{0, 1}` since both forms of the same humanoid actor show up on hook paths; deeper filters use the classifier-token sets above.

### 2.3 Equip-type byte at `desc+0x42` (u16)

| Character | Expected equip-type |
|-----------|---------------------|
| Kliff | `0x0004` |
| Oongka | `0x0001` |
| Damiane | `0x0001` |
| Most NPCs | `0x0001` |

An item whose equip-type differs from the active character's expected value needs the carrier-patch path (see section 3). This is what `needs_carrier(itemId, charName)` checks in `transmog_apply.cpp`.

### 2.4 Picker body-kind UI classification

```text
BodyKind::Generic      -- no classifier tokens (rule-less cosmetics)
BodyKind::Male         -- has >=1 male-body token, no female / non-humanoid
BodyKind::Female       -- has >=1 female-body token, no male / non-humanoid
BodyKind::Both         -- rare: both male AND female tokens
BodyKind::Ambiguous    -- has humanoid-range tokens but none in male / female
                          set (e.g. Antumbra / Badran / Luka gloves with
                          only `0x012F`). Picker colors these amber;
                          render fidelity is inconsistent.
BodyKind::NonHumanoid  -- any token >= 0x1000 (horse, pet, wagon, etc.).
                          Never rendered on a human body.
```

Derivation lives in `item_name_table.cpp :: item_body_bits()`.

---

## 3. Carrier system (cross-character transmog)

### 3.1 Why carriers exist

The game's equip pipeline rejects an item that does not match the active character's equip-type / classifier rules. To render an NPC or cross-character visual, the mod uses a **carrier** (an item the active character CAN equip) and patches its descriptor to substitute the target's visuals while keeping the carrier's class signature at `desc+0x42`.

### 3.2 Hybrid descriptor patch

```text
hybrid        = copy of target descriptor   (target's rule list, meshes, etc.)
hybrid[+0x42] = carrier[+0x42]              (equip-type; the only gated field)
```

Stride between descriptors observed at `0x400` bytes (`k_descBufSize` in `transmog_apply.cpp`). The buffer is SEH-guarded; the mod over-reads a full page and the engine only cares about the bytes actually read.

### 3.3 Per-character carrier sets

Defined in `transmog_apply.cpp`:

```cpp
k_kliffCarriers    = { Kliff_PlateArmor_Helm, _Armor, _Cloak, _Gloves, Kliff_Plate_Boots }
k_oongkaCarriers   = { Oongka_PlateArmor_Helm_II, Oongka_Basic_Leather_Armor,
                       Oongka_Basic_Leather_Cloak, Oongka_Basic_Leather_Gloves,
                       Oongka_PlateArmor_Boots_II }
k_damianeCarriers  = { Demian_PlateArmor_Helm_I, Demian_Leather_Armor,
                       Demian_Leather_Cloak, Demian_Leather_Gloves_I,
                       Demian_Plate_Boots_III }
```

Kliff and Oongka share three male-body tokens but the engine still rejects Kliff's equip-type `0x0004` at Oongka's slot gate, so Oongka needs his own carrier set matching his `0x0001` type. Falling back to `k_kliffCarriers` is the last-chance path when a character-specific name fails to resolve.

### 3.4 `charClassBypass` byte toggle

```text
Address   : resolved via k_charClassBypassCandidates (3-anchor cascade in aob_resolver.hpp)
            (observed absolute on v1.03.01 = 0x141D5F538; the v1.04 / v1.05 patches
            shifted this address; the cascade picks it up dynamically and
            verifies the byte equals 0x74 before activating)
Original  : 0x74  (jz short -- class check required)
Bypass    : 0xEB  (jmp short -- class check skipped, forces acceptance)
```

The instruction is a `jz short` inside a `cmp r9w,[r8+rcx*2]; jz; inc ecx; cmp ecx,edx; jb` classifier-scan loop. The cascade's three anchors target progressively weaker pieces of that loop; the audit doc lists each anchor's hit count and recovery recipe.

Toggled during Phase A tear-down (in `RealPartTearDown::tear_down_by_item_id` via `carrier_swap_and_call`) so carrier-bearing fakes can be torn down on characters whose class normally rejects the carrier item. Restored to `0x74` immediately after.

The mod sanity-checks the byte on resolve (`byte=0x74 OK` log line); if the landed byte is anything else the mod refuses to install. The cascade-driven address survives the v1.05.01 build (see section 9.7 below).

If a future patch shifts this address again:

* The cascade auto-resolves; check the audit doc's "if this breaks" recipe.
* Manual: re-scan for the `movzx eax,[...]; cmp al,<class>; jz <short>` loop. The `jz`'s 2-byte jump is the toggle site; the first byte (`0x74` opcode) is what we overwrite.

---

## 4. Hook points

All hook entry points are resolved through CDCore-shared cascades aliased into `aob_resolver.hpp`. The historical absolute addresses are documentation only.

### 4.1 BatchEquip (game-side inventory commit)

```text
Cascade    : k_batchEquipCandidates (CDCore-shared)
Historical : CrimsonDesert.exe + 0x007605B0  (sub_1407605B0 on v1.03.01, size 0x87A)
Arg0 (rcx) : a1       (equip wrapper; *(a1+8) = actor)
Purpose    : fires when the game commits a batch of equipment changes;
             the mod captures `a1` into shared_state::player_a1().
```

Filter: `is_player_actor(a1)` gate in `transmog_hooks.cpp`. Checks `*(a1+8) -> +0x88 -> +1` byte is in `{0, 1}` and the typeEntry pointer is readable. Any humanoid controlled / backgrounded actor passes.

### 4.2 VisualEquipChange (VEC)

```text
Cascade    : k_visualEquipChangeCandidates (CDCore-shared, aliased as k_vecCandidates)
Historical : CrimsonDesert.exe + 0x007733B0  (sub_1407733B0 on v1.03.01, size 0x42A)
Args (rcx, dx, r8w, r9)
           = (a1, slotId, itemId, unused)
Purpose    : fires on every visual-equip transition. Captures `a1`,
             filters to armor slot tags, schedules a transmog pass.
```

Armor slot tags the mod cares about:

```text
0x0003 = Helm
0x0004 = Chest
0x0005 = Gloves
0x0006 = Boots
0x0010 = Cloak
```

These match the body-mesh hook's `game_slot_from_transmog` mapping (Helm=3, Chest=4, Gloves=5, Boots=6, Cloak=16).

Other tags (weapons, rings, shoulders, belts) are captured as `a1` but do not trigger a transmog pass. Full 22-slot taxonomy is in the project memory `reference_engine_slot_taxonomy_2026-05-07`.

### 4.3 SlotPopulator (direct call, not hooked)

```text
Cascade    : k_slotPopulatorCandidates (mod-local in aob_resolver.hpp)
Historical : CrimsonDesert.exe + 0x007727F0  (sub_1407727F0 on v1.03.01, size 0x367)
Prototype  : __int64 slotPop(__int64 a1, uint16_t* itemData, __int64 swapEntry)
```

The mod calls this directly to push a target / carrier item through the visual pipeline. Sanity: if this address is wrong, expect immediate CTD on first apply. The resolver logs `SlotPopulator resolved via '...' at 0x...` at init.

### 4.4 InitSwapEntry

```text
Cascade   : k_initSwapEntryCandidates (mod-local)
Historical: CrimsonDesert.exe + 0x01D56620  (sub_141D56620 on v1.03.01, size 0xB3)
Role      : zero-init a SlotPopulator "swap entry" before passing it
            into slotPop.
```

### 4.5 Body-mesh pointer-swap hooks

Documented separately in `live-transmog-prefab-wrapper-swap.md`. Brief reference:

* `sub_140352AA0` (primary swap, AOB cascade): 96-byte struct copy operator, hook substitutes `*(a2+0x18)` wrapper.
* `sub_142711DF0` (natural-pipeline patch, AOB cascade per `k_naturalPipelineCandidates`): wrapper-list walker; hook substitutes src wrappers with target wrappers in the input list at hook entry, restores after trampoline.

The StringInfoVtable sentinel `0x145BC4638` literal is in both function bodies.

### 4.6 PartAddShow, SafeTearDown, SubTranslator, MapLookup

Support hooks for the tear-down / name-lookup pipeline. See the `[dispatch] tear_down helpers resolved` line at init for resolved addresses. Cascades in `aob_resolver.hpp` (`k_partAddShowCandidates`, `k_safeTearDownCandidates`, `k_subTranslatorCandidates`, `k_mapLookupCandidates`).

SafeTearDown call label sits at `0x14075FE60` inside `sub_14075F9E0` (size `0x5CD` on v1.03.01). The internal detach path at `0x1425EBAE0` sits inside `sub_1425EACA0` (size `0xEC6` on v1.03.01). These are internal call labels, not function starts; resolve via the AOB, not by address.

---

## 5. Item filter rules (picker UI)

Implemented in `overlay_ui.inl`. An item shows in the slot picker when:

1. **Slot match** (if "Exact" toggle is on): `category_of(itemId)` reads the canonical item-type code at `desc+0x44` captured at catalog-build time:

   ```text
   0x04 -> Helm    0x05 -> Chest    0x06 -> Gloves
   0x07 -> Boots   0x45 -> Cloak
   ```

   Every other code (shields `0x20`, horse armor `0x53`, quest `0xFFFF`, etc.) maps to `TransmogSlot::Count` and the item is hidden as non-equipment. No name parsing is performed; the engine itself uses this byte as the item category so we trust it.

2. **Not flagged non-humanoid**: `BodyKind::NonHumanoid` items are always hidden (no toggle to show them; they never render on a humanoid skeleton).

3. **Body-kind match** (if "Hide cross-body" toggle is on, default): item's `BodyKind` matches the active character's body (Male for Kliff / Oongka, Female for Damiane), OR is Generic / Both. Per-char override available in the overlay Body dropdown.

4. **Variant / incompatibility** filters: existing `hideIncompatible` and `hideVariants` checkboxes still apply; the former is now equivalent to "hide non-humanoid + non-slot-matched".

Color tiers on rows:

| Color | Condition | Meaning |
|---------|----------------------------------------|----------------------------------|
| red | `!bodyMatches && !hasVariantMeta` | Crash risk; engine rejects |
| orange | `!bodyMatches && hasVariantMeta` | Carrier path works but body mismatch; mesh may break |
| amber | `BodyKind::Ambiguous && hasVariantMeta`| Render outcome inconsistent |
| blue | `hasVariantMeta && body matches` | Normal NPC-variant carrier |
| default | everything else | Clean base item |

---

## 6. Character-swap auto-detect

Implemented in `transmog_worker.cpp :: load_detect_thread_fn`.

1. Poll `CDCore::current_controlled_character_name()` once per second (resolver internals in section 1.3).
2. Apply a **settle window** (`k_charSwapSettleMs = 1000`): a candidate identity must remain stable across at least one full second of polling before the swap commits. The first tick that observes a new candidate records `(pendingCharName, pendingFirstSeenTick)`; subsequent ticks confirm the candidate, and the swap fires when `GetTickCount64() - pendingFirstSeenTick >= 1000`. A back-and-forth (A -> B -> A) within the window cancels the pending candidate, so phantom commits do not leak through.
3. On confirmed change between known characters, call `pm.set_active_character(live)`, wipe `slot_mappings`, run `apply_to_state()` so `slot_mappings` reflects the live character's active preset, reset drop-detection state, save.
4. Schedule a transmog apply if the mod is enabled.

The settle window exists because the engine rotates `user+0xD8` through party members during world-load wiring (observed sequence on a Damiane save: Kliff -> Oongka -> Damiane over ~30 seconds). Each rotation is a real engine state, but only the final identity is the user's intended controlled character; firing the swap on each transition would commit the wrong preset and incur ~3x wasted apply work plus a brief visual flicker. 1s is chosen to coalesce sub-second torn reads at the cost of an equal amount of latency on real user-initiated swaps. Multi-second engine wiring sequences are not coalesced; the user will still see one transmog apply per stable identity.

A separate save-load detector observes the `am+0x28` UserActor pointer; when it changes the worker calls `CDCore::invalidate_controlled_character()` so the resolver's last-known-good cache does not return the previous save's character against freshly-reallocated heap. The settle-window state (`pendingCharName`, `pendingFirstSeenTick`) is also cleared on this transition.

The same `CDCore::invalidate_controlled_character()` is called when `player_component` changes (not just on UserActor change). A character swap rotates `user+0xD8` to a different `ClientChildOnlyInGameActor` without touching the UserActor singleton; the LKG identity from the previous controlled actor is no longer applicable to the new wrapper. Without this second invalidation point, the resolver's torn-read absorption returns the prior character's name on any chain walk that lands on the new wrapper before its `+0x60` slot-index byte has been written, and the load-detect retry path commits the prior character's preset onto the new actor. Cost: at most one outer-loop tick (~1 second) of additional latency on swaps where the chain walk transiently returns Unknown right after the rotation; the safety-gate defers until the chain lands on a known identity.

`run_debounced_apply` additionally calls `sync_active_char_to_live()` at the start of every apply pass so that even UI-driven applies (preset clicks, character picker) target the correct character regardless of what `pm.active_character()` was momentarily set to by the UI.

### 6.1 Actor-readiness probe (placeholder-wrapper filter)

When `Load detect: player component changed` fires, the load-detect retry loop schedules up to 20 attempts (backoff 2s, 2s, 3s, 4s, then 5s x 16, ~95s total). The 20-attempt budget exists because on low-end PCs the engine takes 60+ seconds to fully wire the controlled actor's part-registry after a save load, and a shorter budget would give up before legitimate slow loads complete.

The cost of that budget on machines where the engine parks `user+0xD8` on a *placeholder* wrapper for the entire load was ~5 SEH-faulted `tear_down` log lines plus an apply fault per attempt, ~100 noisy faults until the engine swaps the wrapper to the real actor. Mitigation: a cheap actor-readiness probe in front of each scheduled apply.

**Probe** (`RealPartTearDown::is_actor_apply_ready`):

```text
SEH-guarded read-only chain on a1 (the player wrapper):
  *(a1 + 0x78) -> container         (must be >= 0x10000)
    +0x08 -> arrayBase              (must be >= 0x10000)
    +0x10 -> count (u32)            (must be in [1, 0x1000])
```

The chain mirrors the preamble of `tear_down_real_part` exactly. On a placeholder wrapper the `*(a1+0x78)` read SEH-faults (the placeholder's container slot is unallocated). On a real actor the chain reads cleanly and `count` is the live PartDef entry count (14 in a representative reproduction; the protagonist always has at least one equipped slot once wiring completes).

**Wiring in the retry loop:**

* Attempt 0 always invokes the apply path (so a fast-load case where the actor was already wired at the moment load-detect fired still applies immediately, with no probe-only delay).
* Attempts 1+ run the probe first. If the probe returns false, log one `Load detect: actor not ready -- deferring apply (attempt N)` line, increment a `notReadyStreak` counter, and continue. Subsequent failed probes within the same streak are silent (no log spam).
* On the first probe that passes after a streak, log `Load detect: actor became ready after N deferred attempts`, reset the streak, and proceed to schedule a real apply.

**Result:** on a slow load where the engine holds the placeholder for 90+ seconds, the log shows two debug lines (defer + recovery) instead of ~100 SEH-fault traces, and the eventual apply still fires on the first attempt after the engine swaps the wrapper. The 20-attempt budget itself is unchanged so legitimate slow-but-in-place wiring is still tolerated.

**Why `count >= 1` is safe even for a naked character:** `count` is the slot-entry count (helm / chest / cloak / gloves / boots / weapons / accessories / ...), NOT the equipped-item count. Empty slots persist as `primary == 0xFFFF` sentinels and are skipped *inside* the iteration loop, not by lowering `count`. A naked save still reports a full slot count (the representative reproduction shows `count = 14` even for slots that turned out empty during the walk). `count == 0` with a readable container only occurs during the brief window where the container struct is allocated but its slot entries have not yet been initialised, which is exactly the placeholder / mid-wiring state we want to filter out.

### 6.2 Mid-retry wrapper-change abort

The retry loop also re-resolves the player component at the top of every attempt and aborts the current budget when the wrapper has been swapped beneath us:

```cpp
const auto curComp = resolve_player_component();
if (curComp > 0x10000 && curComp != comp)
{
    logger.info("Load detect: wrapper changed mid-retry "
                "({:#x} -> {:#x}), aborting current budget",
                comp, curComp);
    wrapperChanged = true;
    break;
}
```

The engine sometimes parks `user+0xD8` on a *placeholder* wrapper for 60+ seconds before deallocating it and allocating the real character actor at a different address. Without this check the inner retry loop would burn its full ~95s budget against the dead placeholder, then return to the outer load-detect tick which would only then notice the new wrapper and start a *second* full retry budget. Net delay until the real wrapper got a transmog apply: ~95s + 1s outer-loop tick + 2s first-attempt delay = ~98s of visible-mismatch render.

With the abort the inner loop bails within at most one attempt's delay (2-5s) of the swap. The outer loop's next 1s tick observes `comp != prevComp`, advances `prevComp` and starts a fresh 20-attempt budget against the real wrapper, which typically succeeds on attempt 1 because the freshly-allocated real actor is already wired by the time the engine swaps to it. End-to-end visible-mismatch window collapses from ~95s to ~3-7s.

The "auto-apply failed after N attempts" warning is suppressed when the inner loop exits via `wrapperChanged = true`: that exit is a planned hand-off to the outer loop, not a failure.

---

## 7. Known caveats

* `UserActor+0xD8` covers three characters; if Pearl Abyss adds a fourth playable party member, the slot may or may not extend to `+0xE0`, `+0xE8`, etc. Worth re-probing after any DLC / major patch.
* The role byte at `typeEntry+1` is per-character, not a clean player / NPC flag. Do not gate on specific values.
* Heap-pool addresses (`ActorManager`, `UserActor`, wrappers) change per session; never hard-code them. Always resolve via the static WS holder chain or a fresh AOB scan.
* `charClassBypass` is a single `JZ` byte. If the game ever moves it to a `CMOV` or uses IBT-hardened code, that instruction may change size / encoding. Watch for resolve-time `CharClassBypass byte mismatch` warnings.
* Auth-table stride: live source declares `k_compEntryStride = 0xD0`; legacy notes referenced `0xC8`. If a future patch widens the entry, update both `transmog_apply.cpp` and the iteration loop in `tear_down_real_part`.

---

## 9. AOB cascade audit (2026-05-08)

Audit pass that replaced every hardcoded `0x14......` literal in `CrimsonDesertLiveTransmog/src/` that the runtime actually dereferences with a 3-anchor AOB cascade resolved through DetourModKit's `resolve_cascade_with_prologue_fallback`. Comment-only references to historical addresses are kept as documentation.

Game build of record: v1.05.01 (FileVersion 1.0.0.1070).

The "if this breaks" recipe at the bottom of each entry is intentionally prescriptive: the same recipe was used to author the candidate set, so a future engineer can re-run it exactly. Per-cascade ordering follows the DetourModKit rule that the most-specific anchor leads each cascade; otherwise `resolve_cascade` returns on the first match and a non-unique leader will shadow the unique fallbacks.

### 9.1 StringInfoRegistry (`MEMORY[0x145EF1DE8]`)

Engine registry struct that PrefabWrapperSwap walks to resolve prefab names to wrapper-ptrs (`+0x08` count u32, `+0x50` entry-array). Single abs-address load, used in `walk_string_info` hot path.

Live values: `*0x145EF1DE8` is session-dependent, `count = 30095` (matches the documented ~30k), `arrayPtr` lives in the heap arena. Filtering the array for body-mesh prefixes (`cd_phm_`, `cd_phw_`, `cd_m000_`) yields **2926** entries; every one of them has its `+0x18` wrapper-ptr in a single pool prefix (top-24 bits identical across all 2926). Heap arena positions are session-specific so the literal prefix is not load-bearing; the invariant is single-band clustering, which lets the natural-pipeline hook range-test for sentinel filtering. Per-slot heap-walk wrappers (the second pool documented in v1.03.01 era memory as `0x4104A*` / `0x4104E*`) live in a different arena populated by the mod's init-time enumeration.

Cascade: `Transmog::k_stringInfoRegistryCandidates` (3 entries, `ResolveMode::RipRelative`).

| Pattern | Anchor caller | Walk-back |
|---|---|---|
| P1 LoadAddCallSite | `sub_141D81F90` @ `0x141D8215E` | disp32@9, instrEnd@13 |
| P2 OuterScopeFrame | `sub_142144B10` @ deep parser | disp32@12, instrEnd@16 |
| P3 CondLoadStore | `sub_14074DD40` | disp32@13, instrEnd@17 |

Disasm anchor (P1): `mov eax, [rbp-0x50]; mov [rbp+0x58], eax; mov rcx, [rip+disp32]; add rcx, 0x60; lea rdx, [rbp+0x58]`.

If this breaks: each candidate names its host function. Re-find the function in IDA, locate the `mov reg, [rip+disp32]` that loads the registry, copy 16-32 bytes around it, wildcard the disp32 only, and verify uniqueness in the module.

### 9.2 StringInfoVtable (`0x145BC4638`)

Vtable sentinel used as the `+0x08` filter on every StringInfo entry. Filters non-StringInfo heap rows during `walk_string_info`.

Cascade: `Transmog::k_stringInfoVtableCandidates` (`ResolveMode::RipRelative`).

| Pattern | Anchor caller | Walk-back |
|---|---|---|
| P1 LargeMethodAssign | `sub_1402F58A0` (0x17B0-byte fn) | disp32@14, instrEnd@18 |
| P2 R13InitBlock | `sub_1403174F0` | disp32@13, instrEnd@17 |
| P3 TailLeaRdi | `sub_14031AC50` | disp32@11, instrEnd@15 |

Three independent hosts so one recompile cannot break all three.

If this breaks: the vtable layout itself is far more stable than the load-instruction encoding. Search for any `lea reg, [rip+disp32]` that resolves to the vtable address, pick three from different functions.

### 9.3 LoaderRegistry (`MEMORY[0x145DDF8B0]`)

Engine partprefab name-to-wrapper registry singleton. Dereferenced at `+0x50` by the AppT name-lookup function. Read at LT init for the heap-walk enumeration.

Live values: `*0x145DDF8B0` is session-dependent. Capacity = `15298` entries (read as the hi-half u32 at `instance+0x50` and `instance+0x58`; both match). `instance+0x50 lo` ~= `877` (likely active count), `instance+0x58 lo` ~= `18203` (likely a parallel counter). Bucket array at `instance+0x60`, alternating `(small_id u64, heap_ptr u64)` qword pairs consistent with a name-to-wrapper hash table.

Cascade: `Transmog::k_loaderRegistryCandidates` (`ResolveMode::RipRelative`).

| Pattern | Anchor caller | Walk-back |
|---|---|---|
| P1 AddD0CallSite | `sub_1424E44A0` @ `0x1424E4771` | disp32@3, instrEnd@7 |
| P2 R14ReadAfterStore | `sub_1424E44A0` @ `0x1424E47AA` | disp32@7, instrEnd@11 |
| P3 InitStoreSite | `sub_142D1E220` @ `0x142D1E7F1` | disp32@6, instrEnd@10 |

P3 is unusual: it is the engine-init STORE that initializes the singleton, not a load. The disp32 still resolves to the same target.

If this breaks: any function that reads `MEMORY[0x145DDF8B0]` is a candidate. The `+0xD0` walk-offset in P1 and the `EB 03` short-jmp context in P3 are stable game-struct features.

### 9.4 ApptContainerVtable (`0x144D24358`)

Vtable of `partPrefabDataContainer`. Used for gating in `lookup_prefab_metadata`. Single-xref; resolved indirectly by AOB-ing the ctor (`sub_141E2DBB0`) prologue, then walking its body for the SECOND `lea rax, [rip+disp32]; mov [rdi], rax` pair (the FIRST is the intermediate vtable `0x144D242B8`).

Cascade: `Transmog::k_apptLoaderCtorCandidates` (`ResolveMode::Direct`). Walk-forward done in C++ via `prefab_wrapper_swap.cpp::resolve_appt_container_vtable`.

| Pattern | Anchor | Walk-back |
|---|---|---|
| P1 FullPrologue | full 8-callee-saved + alloc | 0 |
| P2 FieldInitChain | post-arg-shuffle XOR + zero stores | -0x1D |
| P3 PayloadCopy | inline qword, qword, byte from `*a3` | -0x2F |

If this breaks: re-AOB `sub_141E2DBB0` by its prologue, then in IDA find the second `48 8D 05 ?? ?? ?? ?? 48 89 07` pair inside the function. The walk-forward is bounded to the function's first 0x400 bytes.

### 9.5 NaturalPipeline (`sub_142711DF0`)

Engine pre-unlink wrapper-list walker. Hooked by PrefabWrapperSwap to substitute Kliff src wrappers with target wrappers in the engine's unlink list (helm and cloak ghost cleanup). Critical for clean teardown.

Cascade: `Transmog::k_naturalPipelineCandidates` (`ResolveMode::Direct`). Function is 6kB with all 8 callee-saved registers pushed, so the prologue is highly distinctive.

| Pattern | Anchor | Walk-back |
|---|---|---|
| P1 FullPrologueChkstk | 4 arg-spills + 7 pushes + lea rbp + B8 0x1880 + chkstk | 0 |
| P2 PostArgSpill | drop first arg-spill, anchor on 3-spill + pushes + chkstk | -0x05 |
| P3 PostChkstkArgShuffle | chkstk call + sub rsp,rax + 4D 8B E8 / 48 8B DA / 4C 8B F9 / 41 83 78 | -0x27 |

Disasm anchor: prologue at `0x142711DF0` ends with `B8 80 18 00 00 / E8 chkstk / 48 2B E0 / 4D 8B E8 / 48 8B DA / 4C 8B F9`. The `0x1880` stack reservation is unique enough to anchor on.

If this breaks: the `B8 imm32` only changes if stack-allocation grows past 4kB. If that happens, search for any `__chkstk` call where the preceding mov-eax-imm matches the new stack size and the post-`48 2B E0` register shuffle is the same 4-saved-args + first-call sequence.

### 9.6 ApptNameLookup (`sub_1424DF420`)

Self-contained name-to-wrapper primitive. Lowercases input, interns it, queries `MEMORY[0x145DDF8B0]+0x50`. Called directly (not hooked).

Cascade: `Transmog::k_apptNameLookupCandidates` (`ResolveMode::Direct`).

| Pattern | Anchor | Walk-back |
|---|---|---|
| P1 FullPrologue | 2 arg-spills + 3 pushes + lea rbp,[rsp-0x60] + sub rsp,0x160 + mov rsi,[rip+disp32] + xor edi,edi + mov [rbp+0x28],rdi | 0 |
| P2 PostArgSpill | drop arg-spill prefix, anchor on pushes + frame setup + registry load | -0x0A |
| P3 LocalInitConstant | frame setup + registry load + xor edi,edi + mov [rbp+0x28],rdi + 48 C7 45 (mov qword imm32) | -0x0D |

The `48 81 EC 60 01 00 00` (sub rsp, 0x160) is a uniquely-sized stack frame. The immediately-following `48 8B 35 ?? ?? ?? ?? 33 FF` (load LoaderRegistry singleton + xor edi,edi) is the function's signature move.

If this breaks: the function exists to query the LoaderRegistry, so locate any function that reads `MEMORY[0x145DDF8B0]` AND lowercases its input AND calls `sub_1403016B0` (string interner). This is a singleton in the engine. Verify the prologue starts with `48 89 5C 24 10 48 89 4C 24 08`.

### 9.7 CharClassBypass patch site

Single-byte runtime patch in CondPrefab evaluator. Toggle `74` (jz rel8) <-> `EB` (jmp rel8). Forces NPC items past the character-class hash check.

| Build | Patch byte address | RVA | Host function |
|-------|--------------------|-----|---------------|
| v1.03.01 | `0x141D5F538` | `0x01D5F538` | `sub_141D5F470 + 0xC8` |
| v1.05.01 | `0x141E0A7D8` | `0x01E0A7D8` | host shifted by `+0x0AB2A0`; cascade re-resolves |

Cascade: `Transmog::k_charClassBypassCandidates` (`ResolveMode::Direct`, return = patch byte address).

P1 cascade matches at `0x141E0A7C7`, patch byte at `match+0x11 = 0x141E0A7D8` reads `0x74` (baseline). Disassembly:

```text
0x141E0A7C7  mov   r8, [rbx-0x08]               ; allowed-class hash array base
0x141E0A7CB  movzx r9d, word ptr [rax+0xAC]     ; current item's class hash
0x141E0A7D3  cmp   r9w, [r8+rcx*2]              ; vs allowed[ecx]
0x141E0A7D8  74 08    je  0x141E0A7E2           ; <-- patch byte (early-out on match)
0x141E0A7DA  FF C1    inc ecx
0x141E0A7DC  3B CA    cmp ecx, edx
0x141E0A7DE  72 F3    jb  0x141E0A7D3           ; loop back
0x141E0A7E0  EB 72    jmp 0x141E0A854           ; rejection (loop fell through)
0x141E0A7E2  ...                                ; matched path
```

Flipping `74 -> EB` forces the first comparison into the matched path regardless of hash equality, so any item's class hash satisfies the check.

| Pattern | Anchor | Walk-forward |
|---|---|---|
| P1 MovzxCmpLoop | inner loop with `4C 8B 43` + `44 0F B7 88 disp16` + `66 45 3B 0C 48` + `74 ??` + `FF C1` + `3B CA` + `72 ??` + `EB` | +0x11 |
| P2 TestMovzxCmp | adds preceding `85 D2 74 ??` (test edx, edx; jz) | +0x15 |
| P3 XorTestCmpLoop | adds preceding `33 C9 85 D2 74 ??` (xor ecx, ecx; test; jz) | +0x17 |

CRITICAL CONSTRAINT: the `74` rel8 in these patterns IS the byte the mod flips at runtime, so it MUST stay literal (not wildcarded). A future compiler that emits this jz in its 6-byte `0F 84 rel32` form would require a full re-RE; every candidate would stop matching and the feature would fail-soft via the scanner's null return. Documented inline in `aob_resolver.hpp`.

If this breaks: re-find `sub_141D5F470`, locate the secondary-hash inner loop (its outer is at `+0x70`). Pattern shape: `4C 8B 43 ??` + `44 0F B7 88 ?? ??` + `66 45 3B 0C 48` + `74` + `FF C1` + `3B CA` + `72 ?? EB`.

### 9.8 Files touched by the audit

Modified:

* `src/aob_resolver.hpp` -- six new cascade tables added (sections 9.1 through 9.6 above; 9.7 was already present from a prior pass).
* `src/prefab_wrapper_swap.cpp` -- replaced `k_stringInfoRegistryAbs`, `k_stringInfoVtable`, `k_loaderRegistrySingleton`, `k_apptContainerVtable` static constants with atomic globals populated from cascades at `init()`. Replaced `k_naturalPipelineRva` and `k_apptNameLookupRva` RVA constants + `GetModuleHandleW` arithmetic with cascade calls.

Verified comment-only (no load-bearing literals):

* `src/shared_state.hpp` -- `0x141D5F538` mentioned in the comment of the `charClassBypass` `ResolvedAddresses` field. Field is initialized to `0` and populated at runtime from `k_charClassBypassCandidates`.
* `src/item_name_table.cpp` -- mentions `0x145BC3638` (sentinel) and `0x140799CB9` (subTranslator anchor) in documentation comments only. Runtime resolution uses `parse_aob` bounded scans inside the AOB-resolved subTranslator function body.
* `src/prefab_wrapper_swap.hpp` -- mentions `0x145EF1DE8` and `0x144D24308` in interface docstrings only.

### 9.9 ApptResMgrInit (`sub_1408AF8F0`)

One-shot capture-hook target. PrefabWrapperSwap installs an inline entry hook that runs the trampoline and then snapshots ResMgr at `a1[5]` (= `a1+0x28`), the loader at `ResMgr+0x58`, and the partprefab container at `loader+0x08`. The hook is one-shot; subsequent calls are pass-throughs.

Live re-verification (Cheat Engine + IDA, v1.05.01 .text, 2026-05-08):

| Anchor | Hits | Walk-back | Notes |
|---|---|---|---|
| `P1_FullPrologue` | 1 | 0 | 8 callee-saved pushes + lea rbp + direct 0xF8 alloc + arg shuffle. No RIP-rel inside window. |
| `P2_PostPushAlloca` | 1 | -0x0D | Survives a future build that re-orders early callee-save pushes (lea-rbp marker pins frame setup). |
| `P3_TlsScratchSetup` | 1 | -0x2D | TLS-canary load body anchor (`mov r12d, 204h ; mov rax, gs:58h`). Anchors past prologue entirely. |

Migrated out of `prefab_wrapper_swap.cpp` (was a single-anchor `inline constexpr AddrCandidate[]`). Consumer call site at `prefab_wrapper_swap.cpp::install_struct_copy_hook`.

If this breaks: the `40 55 53 56 57 41 54 41 55 41 56 41 57` push run + the chkstk-free `48 81 EC F8 00 00 00` direct alloc are the two distinguishing prologue features. Re-anchor by combining either with one of the early-body markers (TLS slot 0x58 read or the constant `204h` payload size).

### 9.10 ApptInnerLookup (`sub_140350910`)

Partprefab container hashtable lookup primitive (pure read). Signature: `__int64(*)(table_struct*, key_wrapper_ptr_ptr*)` where `table_struct = container + 0x70`. Returns 0 on miss or `entry+0x10` on hit (a 24-byte metadata payload pointer).

Critical context: this function has a **byte-identical sibling clone at `sub_1430C4880`** (UI/render subsystem). Both implement the same primitive but only `sub_140350910` is wired to the partprefab `container+0x70` shape. The two clones share their ENTIRE prologue and most of their body -- the only differences are the `disp32` operands of two internal `E8` calls (which point to the same helper `sub_140FBA3B0` but encoded relative to different addresses). The function-prologue cascade alone matches BOTH copies.

Per `feedback_aob_cascade_ordering`: any P1 with hit count >=2 shadows unique fallbacks. Resolution: P1 leads with a **RipRelative call-site anchor** (in `sub_140347BB0` at `0x1403495D5`) that walks an `E8 disp32` to the canonical target. The call-site anchor IS unique because the caller-side instruction window includes a stable game-struct field offset (`+0x70` walk).

Live re-verification (Cheat Engine + IDA, v1.05.01 .text, 2026-05-08):

| Anchor | Hits | Type | Notes |
|---|---|---|---|
| `P1_CallSiteRipRel` | 1 | RipRelative | Window in `sub_140347BB0`: `mov ecx,[rax+disp32] ; add rcx,0x70 ; mov rdx,rbx ; call sub_140350910`. The `48 83 C1 70` is a SEMANTIC offset kept literal. |
| `P2_FullPrologueEarlyExit` | 2 | Direct | Function prologue + early-return path. Shared with sibling clone -- relies on linear-scan first-match returning the canonical target. |
| `P3_PrologueBodyChain` | 2 | Direct | Same as P2 plus one more basic block. Same multi-hit caveat. |

Migrated out of `prefab_wrapper_swap.cpp`. Renamed from `k_apptLookupCandidates` for clarity. Consumer call site at `prefab_wrapper_swap.cpp::install_struct_copy_hook` (the AppearanceTableLoader integration block).

If this breaks: re-find an xref to `sub_140350910` from gameplay-side code (`sub_140347BB0` at `+0x1A25` or `+0x1B98` in v1.05.01) and grab the 12-byte window before the `E8 disp32` for a fresh RipRelative anchor.

### 9.11 ApptStringIntern (`sub_1403016B0`)

String-intern primitive. Signature: `handle_t(*)(const char* utf8)`. Lowercases nothing -- just returns the engine's interned-string handle expected by `sub_141D38810` and `sub_140350910`. Returns 0 for null/empty input.

Like ApptInnerLookup, this function has a templated sibling clone (linker-emitted from a header). The full prologue is unique in v1.05.01 so P1 stays direct; P2 and P3 are body anchors that fall back if the prologue shifts.

Live re-verification (Cheat Engine + IDA, v1.05.01 .text, 2026-05-08):

| Anchor | Hits | Walk-back | Notes |
|---|---|---|---|
| `P1_FullPrologue` | 1 | 0 | Full prologue + null/empty short-circuit + strlen-loop init (`mov rbx, -1`). |
| `P2_StrlenLoopBody` | 1 | -0x13 | Mid-body strlen-loop interior + back-jump (`75 F7`). Survives a prologue-shuffle that drops short-jumps in favour of `0F 84 rel32`. |
| `P3_HeadShortPair` | 1 | 0 | Truncated prologue (no `mov rbx, -1`). Survives a build that re-orders the spill/strlen-init pair. |

Migrated out of `prefab_wrapper_swap.cpp` (was a single-anchor `inline constexpr AddrCandidate[]`). Consumer call site at `prefab_wrapper_swap.cpp::install_struct_copy_hook` (the AppearanceTableLoader integration block).

If this breaks: re-anchor on the unique `48 C7 C3 FF FF FF FF` (mov rbx, -1 = strlen-counter init) + the `strncpy_s` import-call `FF 15 ?? ?? ?? ??` shape -- the import slot is a `__ImageImpDir` entry whose location is build-stable.

### 9.12 StructCopy (`sub_140352AA0`)

0x40-byte struct-copy hot path. Signature: `__int64(*)(dst, src)`. The function copies a partprefab wrapper-related struct field-by-field. PrefabWrapperSwap installs an **inline hook** here and (when LT-active) substitutes Kliff source wrappers with target wrappers for the duration of the copy. This is the single most-active hook in the LT-active path; it must compile to a clean inline.

Function reads the engine's StringInfo vtable sentinel `0x145BC4638` via a `lea rax, [rip+disp32]` early in the body; that single RIP-rel byte is wildcarded. All other bytes in the patterns below are stable.

Live re-verification (Cheat Engine + IDA, v1.05.01 .text, 2026-05-08):

| Anchor | Hits | Walk-back | Notes |
|---|---|---|---|
| `P1_FullPrologueWithVtable` | 1 | 0 | Full prologue + first qword copy + `lea rax, [vtable]`. The disp32 wildcard absorbs the only patch-volatile byte run. |
| `P2_PrologueNoVtable` | -- | 0 | Truncated prologue (no RIP-rel). Survives a future build that moves the vtable lea later. WARNING: pattern fragment also matches `sub_140355210` -- relies on `sanity_check_function_prologue` post-resolve. |
| `P3_ByteTransferBlock` | 1 | -0x2F | Unique 4-byte payload copy (`movzx eax, byte ptr [rdx+8/9/A] ; mov [rcx+8/9/A], al` x3) + dword tail + `mov [rcx+0x10], rbp`. Strong tell of this exact function -- byte-by-byte transfer means struct-alignment-1 (packed). |

Migrated out of `prefab_wrapper_swap.cpp` (was a single-anchor cascade). Consumer call site at `prefab_wrapper_swap.cpp::install_struct_copy_hook`.

If this breaks: re-anchor on the byte-transfer block (P3) -- it is the most function-specific shape and the least likely to shuffle. The function's signature is `dst,src -> mov [dst], 0 ; copy src->dst ; lea rax, [vtable] ; mov [src], rax ; movzx-byte transfers from [src+8..src+0xA] into [dst+8..]`.

### 9.13 ItemNameTable bounded-window anchors

These are NOT cascades: they are pattern strings handed to `DMK::Scanner::find_pattern` for a 0x40--0x80-byte LOCAL scan inside a function whose start has already been resolved (via `k_subTranslatorCandidates`, see section 9.0 and `aob_resolver.hpp` line 146). They live in `aob_resolver.hpp` as `inline constexpr const char*` named pattern constants alongside the cascade tables, per the audit policy of keeping all byte-pattern string literals in one place.

The `|` glyph marks the point where `parse_aob` should compute its `pattern.offset` for downstream `match + offset` arithmetic (DMK v3.0.2+ applies offset internally during `find_pattern`).

Three anchors:

| Constant | Function searched | Hits in window |
|---|---|---|
| `k_nametableSubTxV105Anchor` | sub_14076D950 (0x80 window) | 1 (locally) |
| `k_nametableSubTxV104Anchor` | same | 0 on v1.05; v1.04 fallback |
| `k_nametableItemAccessorAnchor` | sub_1402D75D0 (0x40 window) | 1 (locally) |

Consumed by `ItemNameTable::resolve_chain` in `item_name_table.cpp`. Global-uniqueness does not apply because the scan is locally bounded; the function start is the cascade-resolved `sub_14076D950` (or `sub_1402D75D0`), so the search has at most 0x80 bytes of code to walk. Inert v1.04/v1.05 encoding shifts cause the v105 anchor to miss; the consumer falls back to the v104 anchor inside the same window.

Migrated out of `item_name_table.cpp` (lines 653, 655, 705 were inline string literals fed to `parse_aob`). Per the audit policy "any byte-pattern string handed to `parse_aob` must move into `aob_resolver.hpp`" these qualify even though their containing scan is sub-function rather than module-wide.

If these break: re-RE the encoding shift inside the resolved subTranslator function. The `41 B8 01 00 00 00` (mov r8d, 1) is a SEMANTIC argument flag (the `compileBuffer` flag passed to `sub_141D45270`) and must remain literal. Stack disp8 wildcards absorb compiler register-allocation drift. For step-3, the `0F B7 39` (movzx edi, word ptr [rcx]) reads the 16-bit item-id and is the function's signature opcode; if a future build switches to a 32-bit item-id, that opcode shifts and the anchor needs a new variant.

### 9.14 Files touched by the audit (part 2)

Modified:

* `src/aob_resolver.hpp` -- four new function-target cascade tables (`k_apptResMgrInitCandidates`, `k_apptInnerLookupCandidates`, `k_apptStringInternCandidates`, `k_structCopyCandidates`) plus three named pattern constants (`k_nametableSubTxV105Anchor`, `k_nametableSubTxV104Anchor`, `k_nametableItemAccessorAnchor`).
* `src/prefab_wrapper_swap.cpp` -- removed 4 inline `AddrCandidate[]` arrays (former lines 125, 165, 198, 220). Renamed consumer call from `k_apptLookupCandidates` to `k_apptInnerLookupCandidates` for clarity. The 4 cascades are function-target resolves consumed by `resolve_address()` at install-time -- no atomic-store-init wiring needed (function pointers stored directly into existing `s_apptResMgrInitOrig` / `s_apptStringIntern` / `s_apptLookup` / `s_orig` statics).
* `src/item_name_table.cpp` -- replaced 3 inline pattern strings with references to the named constants in `aob_resolver.hpp`. Added `#include "aob_resolver.hpp"`.
