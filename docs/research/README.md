# CrimsonDesertTools -- Reverse-Engineering Knowledge Base

Project knowledge base for mod runtime internals. Each entry is a long-lived reference document anchored to a specific binary version; entries are updated (not replaced) when the game binary shifts.

- Game binary: `BlackDesertCrimson.exe`
- Current verified version: **Crimson Desert v1.05.01** (FileVersion `1.0.0.1070`)
- Image base: `0x140000000`

## CrimsonDesertLiveTransmog

| Document | Role |
|----------|------|
| [live-transmog-architecture.md](live-transmog-architecture.md) | Runtime pipeline: hook points, apply state machine, memory write surface, event-driven transitions. Cross-references `CrimsonDesertLiveTransmog/src/` files. |
| [live-transmog-source-of-truth.md](live-transmog-source-of-truth.md) | Byte-level reference: WS chain offsets, carrier descriptor layout, hook RVAs, `charClassBypass` site. Section 1.3 covers the `CDCore::current_controlled_character()` resolver shared with EquipHide. Cross-references the AOB cascade audit in section 9 of the same doc. |
| [live-transmog-prefab-wrapper-swap.md](live-transmog-prefab-wrapper-swap.md) | Body-mesh pointer-swap feature: catalog enumeration, two-hook substitution mechanism (`sub_140352AA0` primary swap and `sub_142711DF0` natural-pipeline patch), preset persistence, and slot-tag taxonomy. |

## CrimsonDesert (combat / HUD)

| Document | Role |
|----------|------|
| [combat-state-research.md](combat-state-research.md) | Self-contained reference for the combat-state oracle on the HUD CSS swap method. Two-candidate AOB pair, hook contract, and the rejected `g_battleStateByte` oscillator hypothesis. |

## CrimsonDesertEquipHide

Research artifacts for the EquipHide mod will land in this same directory once lifted into the shared knowledge base format. Naming convention:

```
equip-hide-architecture.md       -- runtime pipeline
equip-hide-source-of-truth.md    -- byte-level anchors
equip-hide-<topic>.md            -- topic-scoped deep dives
```

## Conventions

- Entries list semantic role first, then concrete offsets / bytes / AOBs.
- Every absolute address is paired with an RVA (`CrimsonDesert.exe + 0x...`) so it survives re-basing.
- Cross-reference source files by path (`module/src/file.cpp`) with optional line-range hints.
- No em-dashes; use `--` or restructure.
- When a doc references a hardcoded RVA that has since been replaced by an AOB cascade, point at the cascade table in [live-transmog-source-of-truth.md §9](live-transmog-source-of-truth.md#9-aob-cascade-audit-2026-05-08) rather than duplicating the bytes here.
