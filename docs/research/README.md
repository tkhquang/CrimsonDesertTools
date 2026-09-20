# CrimsonDesertTools - Reverse-Engineering Knowledge Base

Long-lived reference for the engine internals the mods in this repository depend on. Each entry states a mechanism or the memory geometry behind it. An entry is updated in place when the game binary shifts.

- Game binary: `CrimsonDesert.exe`
- Current verified version: **Crimson Desert 2.03.00** (FileVersion `1.0.0.2944`)
- Image base: `0x140000000`

## Scope

These entries describe the **engine** side: the chains a mod walks, the structs it reads and the seams it hooks. They answer "what does the game do here". They do not describe the mod's own code.

A contract that a header already states belongs in that header. An entry here points at it rather than restates it.

## CrimsonDesertLiveTransmog

| Document | Role |
|----------|------|
| [live-transmog-architecture.md](live-transmog-architecture.md) | Runtime pipeline: the carrier-plus-substitution mechanism, the game-thread execution model, one apply pass, the write surface. |
| [live-transmog-source-of-truth.md](live-transmog-source-of-truth.md) | Memory geometry: chain walks to a body, equip-slot component offsets, prefab wrapper layout, slot tags, anchor roles. |
| [live-transmog-prefab-wrapper-swap.md](live-transmog-prefab-wrapper-swap.md) | The substitution layer: the five hooks, the hash-keyed swap map, the per-character ledger, the catalog, weapon handling. |

## CrimsonDesert (combat / HUD)

| Document | Role |
|----------|------|
| [combat-state-research.md](combat-state-research.md) | Combat-state oracle on the HUD class-swap method. Anchored to an earlier build and not consumed by any shipped mod. |

## CrimsonDesertEquipHide

EquipHide has no entry here yet.

Naming convention when an entry does land here:

```text
equip-hide-architecture.md       - runtime pipeline
equip-hide-source-of-truth.md    - memory geometry
equip-hide-<topic>.md            - topic-scoped deep dive
```

## Conventions

- State the semantic role first. State the concrete offset second.
- **Write no absolute address.** Every code address the mods use resolves at startup from a byte signature. A pinned address rots on the next patch and misleads the next reader. Name the anchor role instead.
- A runtime data offset cannot be scanned. Write it down, and name the live instruction that states it, so the next reader can re-derive the value.
- State each invariant once, at the declaration that owns it. Point to it everywhere else.
- Record no change history, no migration narrative and no audit trail. Git holds that.
- Cross-reference source by path, for example `module/src/file.cpp`, with an optional line hint.
- Use a single `-` in place of an em dash or an en dash. Do not use the superseded double-hyphen pair.
