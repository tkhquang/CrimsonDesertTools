# Live Transmog -- Source of Truth

Byte-level reference for every address, offset, and AOB the Live Transmog mod depends on in the Crimson Desert binary. Structured so that when a game patch shifts an address the hook point can be re-found from surrounding byte patterns. Each section lists the semantic role first, then the concrete offsets / bytes / AOBs.

Verified against **Crimson Desert v1.03.01** (image base `0x140000000`). IDA anchors spot-checked 2026-04-21 (see §8).

---

## 1. WorldSystem chain -- resolving the currently-controlled actor

### 1.1 Static entry point

The mod scans for the WorldSystem global-pointer holder via `signatures::WORLD_SYSTEM_P1_SmallFunc` and its fallbacks. On v1.03.01 the holder resolves to:

```
WS holder : CrimsonDesert.exe + 0x05D2AEE8   (absolute 0x145D2AEE8)
```

`*WS_holder` -> `WorldSystem` instance pointer (lives on the game heap; value changes per session).

### 1.2 Chain to the currently-controlled character actor

```
WS holder          : 0x145D2AEE8           (scanned via AOB at init)
  +0x30            -> ActorManager          (singleton, updated on world load)
    +0x28          -> UserActor             (singleton, ClientUserActor class;
                                             the POINTER itself does NOT swap
                                             when the player switches chars)
      +0xD0        -> "primary" body actor  (stable = Kliff in the current
                                             story progression)
      +0xD8        -> CURRENTLY-CONTROLLED  (this is the useful one --
                     body actor            switches when user swaps char)
        +0x68      -> sub-component
          +0x38    -> wrapper / "player component"
                    (what the apply pipeline accepts as `a1`)
        +0x88      -> typeEntry pointer
          +1 (u8)  -> role byte (see §2.2)
```

`user+0xD8` falls back to Kliff's actor when Kliff is the active character (so `+0xD0` and `+0xD8` coincide). Reading `+0xD8` unconditionally is therefore safe for all three playable characters.

### 1.3 Character index byte

```
ActorManager +0x30 (first byte, u8)  :  1 = Kliff, 2 = Damiane, 3 = Oongka
```

This byte is the single cheapest signal for "who is controlled right now". The mod reads it in `read_controlled_char_idx_seh()` (`shared_state.cpp`) and polls it once per second in `load_detect_thread_fn` (`transmog_worker.cpp`).

### 1.4 How the mod resolves `a1` at apply time

```cpp
// transmog_worker.cpp :: resolve_player_component() (around line 139)
ws       = *(0x145D2AEE8)
am       = *(ws  + 0x30)
user     = *(am  + 0x28)
actor    = *(user + 0xD8)              // <-- per-character
te       = *(actor + 0x88)             // pointer-valid check only
sub1     = *(actor + 0x68)
a1       =  *(sub1 + 0x38)             // passes into SlotPopulator
```

The pre-2026-04-21 version walked `+0xD0` and rejected actors whose role byte was not `1` -- both wrong for companions. If the binary shifts and offsets change, start by re-finding the chain via:

* `WORLD_SYSTEM_P*` AOBs in `constants.hpp` / `aob_resolver.hpp` (shared with CrimsonDesertCore).
* The RTTI class name on the singleton at `am+0x28` should read `.?AVClientUserActor@pa@@`.
* `user+0xD0` / `+0xD8` slots both hold `.?AVClientChildOnlyInGameActor@pa@@` pointers.

---

## 2. Character identification

### 2.1 Body-kind classifier sets (CE-verified 2026-04-21)

Every armor's rule list (`desc+0x248`, stride `0x38`, count at `desc+0x250`) contains classifier arrays (ptr `rule+0x20`, count `rule+0x28`, u16 tokens). The tokens partition the catalog into disjoint body families:

| Role                   | Token set                                 |
|------------------------|-------------------------------------------|
| **Male humanoid**      | `0x0018, 0x0058, 0x02E3`                  |
| **Female humanoid**    | `0x0072, 0x0382, 0x0300`                  |
| Kliff-specific         | `0x0015, 0x039D`                          |
| Oongka-specific        | `0x0028`                                  |
| Damiane/Demian-specific| `0x0021`                                  |
| Horse family           | `0x142B, 0x0037, 0x14A5, 0x14D6, 0x1501`  |
| Pet cat                | `0x183F, 0x22C7, 0x22B4, 0x22BA, 0x22AE`  |
| Pet dog                | `0x0D69, 0x4397, 0x2214, 0x0D74`          |

Any token `>= 0x1000` is classified by the mod as **non-humanoid** (`k_nonHumanoidTokenThreshold` in `item_name_table.cpp`). Humanoid tokens observed so far all fit below `0x0400`.

### 2.2 Role byte -- NOT a reliable identifier

`*(actor+0x88)+1` is **not** a player/NPC flag despite older code treating it as one. Observed values:

| Character state                                  | Byte        |
|--------------------------------------------------|-------------|
| Kliff, controlled (ClientUserActor)              | 0           |
| Kliff, backgrounded (ClientChildOnlyInGameActor) | 1           |
| Damiane, controlled                              | 0           |
| Damiane, backgrounded                            | 4           |
| Oongka                                           | also varies |

Treat pointer-validity as the only safe gate. The in-hook filter (`is_player_actor` in `transmog_hooks.cpp`) accepts the byte being in `{0, 1}` since both forms of the same humanoid actor show up on hook paths; deeper filters use the classifier-token sets above.

### 2.3 Equip-type byte at `desc+0x42` (u16)

| Character | Expected equip-type |
|-----------|---------------------|
| Kliff     | `0x0004`            |
| Oongka    | `0x0001`            |
| Damiane   | `0x0001`            |
| Most NPCs | `0x0001`            |

An item whose equip-type differs from the active character's expected value needs the carrier-patch path (see §3). This is what `needs_carrier(itemId, charName)` checks in `transmog_apply.cpp`.

### 2.4 Picker body-kind UI classification

```
BodyKind::Generic      -- no classifier tokens (rule-less cosmetics)
BodyKind::Male         -- has >=1 male-body token, no female/non-humanoid
BodyKind::Female       -- has >=1 female-body token, no male/non-humanoid
BodyKind::Both         -- rare: both male AND female tokens
BodyKind::Ambiguous    -- has humanoid-range tokens but none in male/female
                          set (e.g. Antumbra/Badran/Luka gloves with
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

```
hybrid        = copy of target descriptor   (target's rule list, meshes, etc.)
hybrid[+0x42] = carrier[+0x42]              (equip-type -- the only gated field)
```

Stride between descriptors observed at `0x400` bytes (see `k_descBufSize` in `transmog_apply.cpp`, line ~199). Buffer is SEH-guarded; the mod over-reads a full page and the engine only cares about the bytes actually read.

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

```
Address   : resolved via signatures::CharClassBypass_P1_MovzxCmpLoop
            (observed absolute on v1.03.01 = 0x141D5F538)
Original  : 0x74  (jz short -- class check required)
Bypass    : 0xEB  (jmp short -- class check skipped, forces acceptance)
```

IDA verification (2026-04-21): the instruction at `0x141D5F538` is `jz short loc_141D5F542` inside a `cmp r9w,[r8+rcx*2]; jz; inc ecx; cmp ecx,edx; jb` classifier-scan loop (host function `sub_141D5F470`).

Toggled during Phase A tear-down (in `RealPartTearDown::tear_down_by_item_id` via `carrier_swap_and_call`) so carrier-bearing fakes can be torn down on characters whose class normally rejects the carrier item. Restored to `0x74` immediately after. The mod sanity-checks the byte on resolve (`byte=0x74 OK` log line); if the landed byte is anything else the mod refuses to install.

If a patch shifts this address:
* Re-scan for the `movzx eax,[...]; cmp al,<class>; jz <short>` loop.
* The `jz`'s 2-byte jump is the toggle site; the first byte (`0x74` opcode) is what we overwrite.

---

## 4. Hook points

### 4.1 BatchEquip (game-side inventory commit)

```
Signature  : signatures::BatchEquip_P1_FullPrologue
Resolved   : CrimsonDesert.exe + 0x007605B0   (IDA: sub_1407605B0, size 0x87A)
Arg0 (rcx) : a1       (equip wrapper; *(a1+8) = actor)
Purpose    : fires when the game commits a batch of equipment changes;
             the mod captures `a1` into shared_state::player_a1().
```

Filter: `is_player_actor(a1)` gate in `transmog_hooks.cpp` -- checks `*(a1+8) -> +0x88 -> +1` byte is in `{0, 1}` and the typeEntry pointer is readable. Any humanoid controlled/backgrounded actor passes.

### 4.2 VisualEquipChange (VEC)

```
Signature  : signatures::VisualEquipChange_P1_FullPrologue
Resolved   : CrimsonDesert.exe + 0x007733B0   (IDA: sub_1407733B0, size 0x42A)
Args (rcx, dx, r8w, r9)
           = (a1, slotId, itemId, unused)
Purpose    : fires on every visual-equip transition. Captures `a1`,
             filters to armor slot tags, schedules a transmog pass.
```

Armor slot tags the mod cares about (CE-verified via HW BP on `sub_148EB6700`):

```
0x0003 = Helm
0x0004 = Chest
0x0005 = Gloves
0x0006 = Boots
0x0010 = Cloak
```

Other tags (weapons, rings, shoulders, belts) are captured as `a1` but do not trigger a transmog pass.

### 4.3 SlotPopulator (direct call, not hooked)

```
Signature  : signatures::SlotPopulator_P1_FullPrologue
Resolved   : CrimsonDesert.exe + 0x007727F0   (IDA: sub_1407727F0, size 0x367)
Prototype  : __int64 slotPop(__int64 a1, uint16_t* itemData, __int64 swapEntry)
```

The mod calls this directly to push a target/carrier item through the visual pipeline. Sanity: if this address is wrong, expect immediate CTD on first apply. The resolver logs `SlotPopulator resolved via '...' at 0x...` at init.

### 4.4 InitSwapEntry

```
Signature : signatures::InitSwapEntry_P1_FullPrologue
Resolved  : CrimsonDesert.exe + 0x01D56620    (IDA: sub_141D56620, size 0xB3)
Role      : zero-init a SlotPopulator "swap entry" before passing it
            into slotPop.
```

### 4.5 PartAddShow, SafeTearDown, SubTranslator, MapLookup

Support hooks for the tear-down / name-lookup pipeline. See the `[dispatch] tear_down helpers resolved` line at init for resolved addresses. Signatures in `constants.hpp` / `aob_resolver.hpp`.

SafeTearDown call label sits at `0x14075FE60` inside `sub_14075F9E0` (size `0x5CD`). The internal detach path at `0x1425EBAE0` sits inside `sub_1425EACA0` (size `0xEC6`). These are internal call labels, not function starts -- resolve via the AOB, not by address.

---

## 5. Item filter rules (picker UI)

Implemented in `overlay_ui.inl`. An item shows in the slot picker when:

1. **Slot match** (if "Exact" toggle is on) -- `category_of(itemId)` reads the canonical item-type code at `desc+0x44` captured at catalog-build time:
   ```
   0x04 -> Helm    0x05 -> Chest    0x06 -> Gloves
   0x07 -> Boots   0x45 -> Cloak
   ```
Every other code (shields `0x20`, horse armor `0x53`, quest `0xFFFF`, etc.) maps to `TransmogSlot::Count` and the item is hidden as non-equipment. No name parsing is performed; the engine itself uses this byte as the item category so we trust it.

2. **Not flagged non-humanoid** -- `BodyKind::NonHumanoid` items are always hidden (no toggle to show them; they never render on a humanoid skeleton).

3. **Body-kind match** (if "Hide cross-body" toggle is on -- default): item's `BodyKind` matches the active character's body (Male for Kliff/Oongka, Female for Damiane), OR is Generic / Both. Per-char override available in the overlay Body dropdown.

4. **Variant / incompatibility** filters -- existing `hideIncompatible` and `hideVariants` checkboxes still apply; the former is now equivalent to "hide non-humanoid + non-slot-matched".

Color tiers on rows:

| Color   | Condition                              | Meaning                          |
|---------|----------------------------------------|----------------------------------|
| red     | `!bodyMatches && !hasVariantMeta`      | Crash risk -- engine rejects     |
| orange  | `!bodyMatches && hasVariantMeta`       | Carrier path works but body      |
|         |                                        | mismatch; mesh may break         |
| amber   | `BodyKind::Ambiguous && hasVariantMeta`| Render outcome inconsistent      |
| blue    | `hasVariantMeta && body matches`       | Normal NPC-variant carrier       |
| default | everything else                        | Clean base item                  |

---

## 6. Character-swap auto-detect

Implemented in `transmog_worker.cpp :: load_detect_thread_fn`.

1. Poll `read_controlled_char_idx_seh()` once per second.
2. On change between known characters, call `pm.set_active_character(live)`, wipe `slot_mappings`, run `apply_to_state()` so `slot_mappings` reflects the live character's active preset, reset drop-detection state, save.
3. Schedule a transmog apply if the mod is enabled.

`run_debounced_apply` additionally calls `sync_active_char_to_live()` at the start of every apply pass so that even UI-driven applies (preset clicks, character picker) target the correct character regardless of what `pm.active_character()` was momentarily set to by the UI.

---

## 7. Known caveats

* `UserActor+0xD8` was verified on three characters only; if Pearl Abyss adds a fourth playable party member, the slot may or may not extend to `+0xE0`, `+0xE8`, etc. Worth re-probing after any DLC / major patch.
* The role byte at `typeEntry+1` is **per-character**, not a clean player/NPC flag. Do not gate on specific values.
* Heap-pool addresses (`ActorManager`, `UserActor`, wrappers) change per session -- never hard-code them. Always resolve via the static `WS_holder` chain or a fresh AOB scan.
* `charClassBypass` is a single `JZ` byte; if the game ever moves it to a `CMOV` or uses IBT-hardened code, that instruction may change size/encoding. Watch for resolve-time `CharClassBypass byte mismatch` warnings.

---

## 8. IDA verification status (2026-04-21)

Spot-checked against the v1.03.01 IDB:

| Anchor | Status |
|--------|--------|
| BatchEquip `+0x007605B0` (`sub_1407605B0`, size 0x87A) | VERIFIED |
| VisualEquipChange `+0x007733B0` (`sub_1407733B0`, size 0x42A) | VERIFIED |
| SlotPopulator `+0x007727F0` (`sub_1407727F0`, size 0x367) | VERIFIED |
| InitSwapEntry `+0x01D56620` (`sub_141D56620`, size 0xB3) | VERIFIED |
| `charClassBypass` at `0x141D5F538` original byte `0x74` | VERIFIED (bytes `74 08 FF C1`; `jz short loc_141D5F542` inside `sub_141D5F470`) |
| `WS holder` at `0x145D2AEE8` | Address in `.data` confirmed; live pointer value session-dependent |
| SafeTearDown call label `0x14075FE60` | INTERNAL LABEL (inside `sub_14075F9E0`, size 0x5CD). Not a function entry; resolve via AOB. |
| Scene-graph detach label `0x1425EBAE0` | INTERNAL LABEL (inside `sub_1425EACA0`, size 0xEC6). Not a function entry. |
| SlotPopulator visual-config match label `0x14076CAED` | INTERNAL LABEL (inside `sub_14076C7F0`, size 0x71B). Not a function entry. |

Deferred verification (not spot-checked this pass -- recommend a follow-up if any of these are edited):

* AOB strings inside `constants.hpp` / `aob_resolver.hpp`.
* Auth-table stride `0xC8`, slot-tag offset `+0xC0`, primary/alt item id offsets `+0x08` / `+0x88`.
* Dispatch cache layout at `a1+0x1B8..+0x1C4`.
* Descriptor `+0x42` equip-type byte semantics (Kliff 0x0004 vs others 0x0001) -- rely on in-code `needs_carrier()` truth.
* Rule list layout `desc+0x248` stride `0x38` count `desc+0x250`.

---

Maintained alongside the source; bump the "Verified game version" line at the top whenever an address or AOB is re-probed against a new patch.
