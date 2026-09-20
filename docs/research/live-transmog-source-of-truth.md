# Live Transmog - Source of Truth

Memory geometry the Live Transmog and EquipHide mods depend on. It covers the chain walks that reach a body, the struct offsets they read, and the roles the anchor registry resolves. The pipeline that consumes all of it is in [live-transmog-architecture.md](live-transmog-architecture.md).

Verified against **Crimson Desert 2.03.00** (image base `0x140000000`, FileVersion `1.0.0.2944`).

**No absolute address appears in this document.** Every code address the mods use resolves at startup from a byte signature. A written-down address rots on the next patch and misleads the next reader. Some runtime data offsets below quote the live instruction that states them. Where no instruction appears, re-derive the offset from a live read instead of a guess.

---

## 1. Reaching a body

Two roots reach the same `pa::ClientActorManager`. They are independent anchors. A patch that breaks one leaves the other intact.

### 1.1 Static root (CDCore resolver)

The ladder that resolves this slot lives in `cdcore/anchors.hpp`. It is not the `PlayerStatic` anchor, which roots a separate server-side chain the helm-audio filter walks.

```text
[ClientActorManagerGlobal]              anchor-resolved module-static slot
  +0x00  -> pa::ClientActorManager
    +0x58  -> pa::ClientUserActor
      +0x08  -> CCOIA sub-manager
        +0x30  -> Kliff CCOIA            always-present anchor body
        +0x38  -> controlled CCOIA       rotates on a body swap
```

### 1.2 WorldSystem root (mod workers)

```text
[WorldSystem]                           anchor-resolved module-static slot
  +0x00  -> WorldSystem instance
    +0x30  -> pa::ClientActorManager
      +0x58  -> pa::ClientUserActor
        +0xD8  -> controlled CCOIA
```

The three offsets are owned once, by `CDCore::actor_chain_offsets` in `cdcore/controlled_char.hpp`. `ACTOR_MANAGER_TO_USER_ACTOR` is the field most likely to move on a patch. The CDCore walk re-derives it at runtime through an RTTI dissection of the live manager. The WorldSystem-rooted walks read the constant and do not self-heal.

### 1.3 CCOIA to the equip-slot component

```text
CCOIA
  +0x68  -> component container
    +0x38  -> pa::ClientEquipSlotActorComponent      the mod's `a1`
```

The engine states this walk itself, immediately before it walks the authoritative table:

```text
mov rcx,[<ccoia>+68]
mov rax,[rcx+38]
```

The walk returns zero on an inactive body, where the engine zeroes the slot.

### 1.4 Character identity

Identity comes from the appearance-config asset path, not from a numeric field:

```text
CCOIA + 0x68 -> component pointer table
  RTTI name match pa::ClientCharacterControlActorComponent
    +0x40 -> +0x38    std::string
```

| Character | Codename in the path |
|-----------|----------------------|
| Kliff | `cd_phm_macduff` |
| Damiane | `cd_phw_damian` |
| Oongka | `cd_phm_oongka` |

The component table is sparse, and the class slot moves between cold load and steady state. The walk therefore matches the class by RTTI name, not by a fixed slot offset. The path embeds the codename twice, in the subfolder and in the filename. The engine binds the appearance config at actor spawn. It does not change with outfit, animation or save load, so the classification is stable across sessions. A humanoid NPC carries no config at this path and classifies as `Unknown`.

The codename is character-specific rather than a skeleton archetype. A future protagonist on Damiane's skeleton therefore classifies as `Unknown` instead of as Damiane.

A `+0x63` high-byte fast path answers Kliff when the appearance chain is mid-teardown. Every walk is guarded. A fault yields `Unknown`, which callers treat as "no live identity".

---

## 2. Equip-slot component

Three independent field groups hang off `a1`. **Each sits at its own depth and drifts by its own amount.** Never scale one from another's shift.

### 2.1 Authoritative equip table

Owned by `auth_table.hpp`. One copy for the whole mod, because the geometry moves as a unit.

```text
component + 0x90   -> container
  container + 0x08  -> entry array base
  container + 0x10  -> live entry count (dword)
  base + index * 0xD0 -> entry
    entry + 0x08     item id word, 0xFFFF or 0 marks empty
    entry + 0x10     live-entry gate, non-zero on a live entry
    entry + 0x78     dye-record vector, 16-byte records, read-only, moves with the stride
    entry + 0xC8     slot tag
```

The engine states the whole walk in one run, and the entry search states the stride and the tag offset literally:

```text
mov rax,[<comp>+00000090]
mov rdx,[rax+08]
mov eax,[rax+10]
imul rcx,rax,000000D0
...
cmp [rdx+000000C8],r8w
add rdx,000000D0
```

The stride and the tag offset always move together by 8. The engine alternates between two shapes: stride `0xC8` with the tag at `0xC0`, and stride `0xD0` with the tag at `0xC8`. A static assertion encodes the pairing, so an edit of one alone fails the build.

A stale container offset fails silently. The neighboring slot holds a packed scalar. On most instances the plausibility guard rejects it and the apply reports zero slots forever. On others the scalar passes the guard and the walk reads garbage.

### 2.2 Dispatch cache

```text
component + 0x1F8   base pointer
component + 0x200   count
component + 0x204   capacity
entry stride        24 bytes
entry + 0x10        blob sub-count
```

`SlotPopulator` forms one base pointer and reads the other two members off it:

```text
lea rsi,[<comp>+000001F8]
mov r8d,[rsi+08]
mov r9,[rsi]
```

The grow check pins the capacity directly rather than by adjacency:

```text
mov edx,[rsi+08] / cmp [rsi+0C],edx / ja
```

The sub-count is append-only and the engine never lowers it. A pass that releases a slot must zero it by hand. Otherwise the stale blob still reaches the visual dispatch.

Check the triple against a live component, not against the disassembly alone. A correct base reads a populated triple. A stale base reads zeroes and surfaces as `post-apply live_count=0` on every apply.

### 2.3 Candidate-exclusion pair

```text
component + 104 (0x68)   pointer to a WORD array of excluded slot tags
component + 112 (0x70)   count
```

The item-to-slot resolver walks the character's candidate slots and returns the first that validates. The validator rejects any candidate listed here. The pair is empty in normal play, which is what makes it safe to borrow for one equip. Re-derive it from the validator's own live read of the array and the count.

---

## 3. Prefab wrapper

The engine's handle for one named mesh asset.

```text
wrapper + 0x00   string pointer
wrapper + 0x08   length
wrapper + 0x0C   prefab name hash
wrapper + 0x10   refcount
```

The hash is `lookup3` `hashlittle` over the full suffixed name with the engine's seed. Every wrapper instance of one name carries the same value. That is why the swap map keys on the hash and not on a pointer. The catalog and the engine draw from different pools, which a pointer key can never bridge.

The wrapper for a StringInfo entry lives at `entry + 0x18`. The `StringInfoRegistry` anchor resolves the registry the walk starts from, with the count at `+0x08` and the entry-array pointer at `+0x58`.

---

## 4. Slot tags

`SLOT_METADATA` in `slot_metadata.hpp` is the only table. It states the tag, the display name, the part-show hash key and the enable flag per slot.

| Tag | Slot | Tag | Slot |
|-----|------|-----|------|
| `0x00` | MainHand | `0x0C` | SubWeapon |
| `0x01` | OffHand | `0x0D` | TwoHandWeapon |
| `0x02` | Ranged | `0x0E` | Tool |
| `0x03` | Helm | `0x0F` | Lantern |
| `0x04` | Chest | `0x10` | Cloak |
| `0x05` | Gloves | `0x11` | Glasses |
| `0x06` | Boots | `0x12` | Mask |
| `0x07` | Earring1 | `0x13` | Backpack |
| `0x08` | Earring2 | `0x14` | Bracelet |
| `0x09` | Necklace | `0x17` | OffHand2 (shield) |
| `0x0A` | Ring1 | `0x18` | Ranged2 |
| `0x0B` | Ring2 | `0x15` | Oongka rocket helm, not managed |

A row in the table is not the same as a live slot. Four rows ship with the enable flag clear: Tool `0x0E`, Bracelet `0x14`, OffHand2 `0x17` and Ranged2 `0x18`. The picker hides a disabled slot, and the apply and clear dispatcher skips it, so a preset that names one is inert.

Tag values are a gameplay-design constant. No tag changed across any version the mod shipped against, and every patch so far only appended. Only the tag's position inside an entry shifts.

The values are written down rather than derived, because the authoritative table lists filled slots only and can never produce the whole table. What is derived is a **check**. The tear-down walk compares every live pair against the catalog's name-derived classification and warns on disagreement, so a renumbered column reports itself.

Three properties follow the tag rather than the slot index:

- A **paired** slot shares its item type with a sibling, so the equip names the destination explicitly. Ring1, Ring2, Earring1, Earring2 and OffHand all name it. The second half also excludes the first half: Ring2 excludes Ring1, Earring2 excludes Earring1, and OffHand excludes MainHand.
- A **weapon-family** slot needs its target prefab to declare the same part name as the mesh it replaces. The draw and stow mover finds a weapon by part name, so a cross-type look otherwise never reaches the hand.
- The **shield** has no row of its own at apply time. The engine keeps it in tag `0x01` on a one-hand loadout, and the OffHand row dresses it there. Tag `0x17` takes the shield only during dual-wield, and that row ships disabled.

---

## 5. Item descriptor and catalog

```text
descriptor + 0x42   item type code (u16), 0xFFFF for an item with no equip slot
```

The type code indexes `EquipTypeInfo`. That band grows whenever content adds a weapon family, and it moved in both directions across patches. The catalog therefore **learns** the code-to-slot join at runtime and pins nothing. `ItemNameTable` classifies each item from its `ItemGroupInfo` membership plus that learned join.

A wrong type-code offset no longer mis-slots items silently. It yields scattered keys that nothing votes for twice, the learned table collapses, and the catalog histogram drops to the group-only counts.

Wearer-body restriction (`BodyKind`) comes from the shipped display-names table, keyed by the lowercase internal name. It answers whether a body can wear an item. It is not a classifier-token derivation.

The runtime prefab solver `variant_meshes_for_item` reads an item's per-body variant entry list and returns the mesh names the item emits on each wearer body. This is how a carrier's source prefab and an item-driven target are derived. A hardcoded prefab column drifts from the item name every patch. The variant list is always exact.

---

## 6. Anchor roles

`aob_resolver.hpp` declares every role the mod resolves. `resolve_all_anchors()` resolves the table in one parallel pass at startup and records each result. `anchor_address()` hands out the address, or zero on a miss.

Roles shared with EquipHide live in `cdcore/anchors.hpp`. Every ladder resolves under `require_unique`. An ambiguous pattern is skipped rather than resolved blindly, and a full miss is a clean zero rather than a guess.

| Group | Roles |
|-------|-------|
| Equip and part build | `SlotPopulator`, `PartSlotRefresh`, `SlotTagToHandle`, `InitSwapEntry`, `PartDescriptorBuild` |
| Tear-down and naming | `SafeTearDown`, `PartAddShow`, `MapLookup`, `SubTranslator` |
| Mesh substitution | `StructCopy`, `NaturalPipeline`, `UnlinkByWrapper`, `PartListMerge` |
| Dye and color | `DyeCopy`, `DyeCopier`, `ColorPublisher`, `SetterByte`, `ColorTokenInterner`, `HostScopeVfunc1`, `HostScopeVfunc2` |
| Audio | `HelmAudioRegistrar`, `GameAudioEffectVtable` |
| Frame and world | `FrameUpdate`, `FrameUpdateXref`, `WorldSystem`, `PlayerStatic` |
| Registries | `StringInfoRegistry`, `StringInfoVtable`, `LoaderRegistry` |
| Vtable witnesses | `HostScopeVfunc1Vtable`, `HostScopeVfunc2Vtable`, `SetterByteVtable` |

Two validators stand behind the table. Each entry rejects a value outside the host image, and a code entry additionally rejects a site whose first byte cannot begin an instruction. Three hooked functions carry an RTTI witness: the registry resolves the class vtable by mangled name and corroborates the byte ladder against the vtable slot. A disagreement is logged.

The startup summary line reports the resolved count, the failures and the unsupported entries. Read it first on patch day.

---

## 7. Authoring rules for a new ladder

Follow [the DetourModKit AOB guide](../../CrimsonDesertCore/external/DetourModKit/docs/misc/aob-signatures.md). The rules that matter most here:

1. Sign code, not data. Anchor on instruction semantics, never on linker output.
2. Wildcard every immediate, RIP displacement, jump target and compiler-renumbered struct offset.
3. Keep a row long enough to be unique and no longer. Seven to sixteen bytes is the common range.
4. Never anchor on a short `Jcc rel8`. Use a bounded gap, because a compiler flips between the 2-byte and the 6-byte form across patches.
5. Carry the function's own identity. A row built only from a stack allocation and register moves identifies nothing. The wrong function then returns harmlessly, and the visual symptom looks like mod logic.
6. Prefer the loss of a ladder to a match on the wrong function.

---

## 8. Caveats

- Heap addresses rotate every session. Resolve through a chain walk or an anchor. Never pin one.
- The controlled-CCOIA slot covers three protagonists. Re-probe it after any content patch that adds a playable character.
- `shutdown()` returns false unless every hooked prologue is proved restored. A hook that cannot prove a restore pins its own backend, so no stale trampoline stays live. The shipped build logs that failure. Only the dev loader turns the verdict into an unload refusal.
