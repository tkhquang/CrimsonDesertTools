# Prefab Wrapper Swap - Implementation Reference

The substitution layer that makes a slot render the target mesh while the engine believes the wearer equipped a carrier item. It sits under the apply pipeline in [live-transmog-architecture.md](live-transmog-architecture.md). Struct geometry is in [live-transmog-source-of-truth.md](live-transmog-source-of-truth.md).

Verified against **Crimson Desert 2.03.00** (image base `0x140000000`, FileVersion `1.0.0.2944`).

---

## 1. Core mechanism

Every part the engine builds is a descriptor whose first field is a **prefab wrapper**, and every wrapper carries the hash of its prefab name. The swap rewrites that one pointer.

The swap map keys on the **name hash**, never on a wrapper pointer. Every instance of a name carries the same hash, so the engine can hand over any pool instance and the lookup still resolves. The catalog and the engine draw from different pools, which is what a pointer key can never bridge.

The layer installs unconditionally at boot and declares no INI key. It stays inert until a selection binds a source to a target.

---

## 2. The five hooks

| Hook | Key | Job |
|------|-----|-----|
| `StructCopy` | Source name hash | Rewrite the wrapper of a staging descriptor as the engine copies it into its part list. |
| `PartDescriptorBuild` | Socket, from the slot-tag argument | Rewrite every descriptor the build appended, and lend the build the preset's dye records. Owned by `socket_mesh_override.cpp`. |
| `PartListMerge` | Assembly node | Publish which protagonist the nested copies belong to. Observational. |
| `NaturalPipeline` | Source name hash | Rewrite the unlink list so the engine's content-keyed search finds what the mod installed. |
| `UnlinkByWrapper` | Wrapper | Observe a claim removal. The sweep also calls it directly to erase a stale claim. |

Four of them install from `prefab_wrapper_swap.cpp`. `PartDescriptorBuild` installs from `socket_mesh_override.cpp`.

### Why two substitution keys

`StructCopy` sits where nothing states which slot is under construction, so it can only match by source mesh. That is exactly what a carrier supplies: the mod chose the carrier, so it knows the source name.

`PartDescriptorBuild` receives the slot tag, so it can match by socket and needs no source registration. That is what covers the engine's own equips, where the incoming item is arbitrary. It also removes the flash, because the target mesh is built in the first place.

Both remain. The socket override handles the engine's builds. The hash-keyed copy handles the records the mod's own apply drives and any build path the socket override does not reach.

### Scope

`on_struct_copy` picks its bucket from the protagonist `PartListMerge` published, or from the bound character index while the mod drives the call, and refuses everything else. A non-protagonist assembly is refused outright, which is what keeps inventory previews and NPCs untouched.

Two further guards sit on the write. A hit is confirmed against the wrapper's inline name before it counts, and a name collision logs and passes through. The destination must lie inside the calling thread's own stack range, because the copy target is a staging vector slot on that stack.

---

## 3. Per-character state

The module keeps one bucket per protagonist, not one union:

- Selection rows, one per protagonist, so a dropdown flip does not drop the outgoing character's uncommitted picks.
- Swap map and target wrappers, one bucket per protagonist, rebuilt for the active character alone on each apply.
- A per-slot target table, which answers what one socket must wear. The socket override reads it.

Per-character keying is required, not a convenience. Protagonists share carrier items. All three default to the same Mask carrier and the same Glasses carrier, so a union map puts one character's selection on another character's body.

The target table holds one character's targets at a time. Anything that installs a target onto a body must compare the table's owner against the body's own protagonist. Without that check it dresses whichever body it is handed with whichever table is current.

---

## 4. Catalog

The picker browses every body-mesh prefab the engine knows. Three sources merge into one per-slot catalog.

1. **StringInfo walk.** One pass with a `cd_` prefix, filtered by the StringInfo vtable sentinel. These are the wrappers resident in the player pipeline, so a selection renders now with no asynchronous load.
2. **AppearanceTableLoader registry.** Every prefab the loader parsed, resident or not, merged in by slot tag. A prefab present here but absent from StringInfo has no resident wrapper, so a pick is best-effort.
3. **Heap scan.** Fills in parallel-pool wrapper instances for each name, which the first two sources do not carry.

`slot_for_prefab_name` classifies a name into a slot. It matches on substrings, so every role prefix works. That covers `cd_phm_*` and `cd_phw_*` for protagonists, `cd_nhm_*` and `cd_ndm_*` for NPCs, and the prefix-free accessory and mount families. An ambiguous weapon tag such as `sword` or `axe` falls through to the numeric role chunk. A future `cd_xxx_02_sword_*` family therefore inherits the two-hand classification with no code change.

Two structural rejections keep the lists clean. A name with any uppercase letter is a UI, knowledge or icon asset that embeds a real prefab. A name that does not start with `cd_` is not a prefab at all.

### Source defaults

A slot's **source** is derived, never hardcoded. It comes from the character's carrier item through the runtime prefab solver, which reads the item's per-body variant entry list. A hardcoded prefab column drifts from the item name every patch.

Each carrier must render on its own. The swap redirects the carrier's own mesh to supply the visual. A carrier whose prefab resolves to no live wrapper therefore produces an empty slot, not a swapped one.

Two slots on one character must not share a carrier. The map is keyed by source name, so a shared carrier collapses into one entry and the second slot is lost.

---

## 5. Install and sweep lifecycle

The layer tracks what it installed, so it can remove exactly what the player deselects.

**Full apply.** `notify_apply_starting` parks every installed target and rebuilds the active character's bucket from the current selections. The apply re-installs what is still selected. `notify_apply_finished` sweeps the difference: it detaches each parked wrapper the apply did not re-install, then compacts the claim vector.

**Single-slot apply.** The park-everything cycle cannot run. It sweeps the other slots' live targets off the body, because only one slot re-installs. That path parks only the target it replaces, arms the map, leaves the ledger alone, and runs the same sweep afterwards.

Two rules govern the single-slot path:

- Park the previous target **before** the arm, and only when the target actually changes. An id the apply re-installs parks the wrappers the sweep must read as live.
- Arm only when the slot **installs** a fake. On a tear-down the map must stay as it is. An armed map rewrites the wrapper the engine's unlink looks for, the unlink misses, and the old mesh stays on the body.

**Direct fakes.** When the carrier is the target item, the slot equips it as itself and no substitution happens. Such a slot never passes through `StructCopy`, so nothing lands in the installed set and the sweep finds no victim. The apply registers the item's prefabs explicitly, which makes the park-then-subtract machinery cover the case. One exception holds. The sweep keeps a parked wrapper that a released slot wears as its real item. A detach there takes the restored real item down with it.

**Refcounts.** The target wrapper is incremented before substitution, so the destination's eventual destruction stays balanced. The source keeps its leftover increment. The result is one increment per substitution on the source, which is acceptable for wrapper allocations the engine keeps alive for the session.

---

## 6. Weapons

A weapon slot dresses through the same carrier pipeline as armor, with two additions.

**Part name.** The engine moves a weapon between its stowed and drawn sockets by part name, and it derives that name from whichever mesh the record carries. A war hammer substituted over a sword emits a hammer part while the mover looks for a sword part. The weapon never reaches the hand. The override rewrites the target's prefab metadata to declare the part name of the **real equipped weapon**. It reads that name live from the authoritative table, not from the carrier.

The metadata search recognizes an entry by shape, and shape alone matches unrelated objects. `publish_part_name_ids` supplies the real `CD_*` name ids the string sweep found. The search then requires that the value it replaces is itself a part name. Until that publish lands, the override is inert.

**The case.** A scabbard belongs to the part, not to the item, so the engine builds the carrier's case beside the weapon. The case reaches the copy hook under its own name, so it needs a swap row of its own. A target that owns a case gets that row. A target that owns none has the carrier's case retracted after the slot rebuilds. Retraction must run **after** the rebuild, because the carrier equip recreates the case.

**The pair.** The hands are a paired slot. Each hand needs its own carrier. OffHand needs both a named destination and the first-half exclusion. The opposite side needs its own part-name override. Without it the look keeps its own part, and the engine mounts it in the main hand.

**Sides.** A paired slot's second half emits the other side's descriptor. `target_wrapper_for_socket` returns the opposite-side wrapper only when the source and the primary carry opposite side tokens. It never consults the swap map. A source-keyed lookup drags a target along with a carrier the engine re-seated on the other socket, and the pair then swaps sides. A mesh with no side token serves either side, which is how a shield is shaped.

---

## 7. Files

| File | Role |
|------|------|
| `prefab_wrapper_swap.cpp` | Catalog, per-character buckets, four of the five hooks, the ledger and the sweep. |
| `prefab_wrapper_swap.hpp` | The contract for every entry point, plus the call-order warnings. |
| `socket_mesh_override.cpp` | The `PartDescriptorBuild` override. |
| `carrier_defaults.hpp` | Carrier item per character and per slot. |
| `itemmesh_dumper.cpp` | The runtime prefab solver and the item-to-prefab dump. |
| `slot_metadata.hpp` | Slot tags, the paired and weapon-family flags, name-to-slot classification. |
