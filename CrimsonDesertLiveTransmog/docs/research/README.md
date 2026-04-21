# CrimsonDesertTools -- Reverse-Engineering Knowledge Base

Project knowledge base for mod runtime internals. Each entry is a long-lived reference document anchored to a specific binary version; entries are updated (not replaced) when the game binary shifts.

- Game binary: `BlackDesertCrimson.exe`
- Current verified version: **Crimson Desert v1.03.01**
- Image base: `0x140000000`

## CrimsonDesertLiveTransmog

| Document | Role |
|----------|------|
| [live-transmog-architecture.md](live-transmog-architecture.md) | Runtime pipeline: hook points, apply state machine, memory write surface, event-driven transitions. Cross-references `CrimsonDesertLiveTransmog/src/` files. |
| [live-transmog-source-of-truth.md](live-transmog-source-of-truth.md) | Byte-level reference: WS chain offsets, carrier descriptor layout, hook RVAs, `charClassBypass` site, IDA verification log. |

## CrimsonDesertEquipHide

Research artifacts for the EquipHide mod will live under `CrimsonDesertEquipHide/docs/research/` once lifted into this format. Naming convention:

```
equip-hide-architecture.md       -- runtime pipeline
equip-hide-source-of-truth.md    -- byte-level anchors
equip-hide-<topic>.md            -- topic-scoped deep dives
```

## Conventions

- Entries list semantic role first, then concrete offsets / bytes / AOBs.
- Every absolute address is paired with an RVA (`CrimsonDesert.exe + 0x...`) so it survives re-basing.
- IDA verification status goes in a dedicated section near the end.
- Cross-reference source files by path (`module/src/file.cpp`) with optional line-range hints.
- No em-dashes; use `--` or restructure.
