## Initial release (BETA)

- Runtime armor transmog via SlotPopulator hook with ReShade overlay GUI, preset system, safety filters, and per-slot control
- Modular code split: `transmog_apply` (apply/clear logic), `transmog_hooks` (VEC/BatchEquip callbacks), `transmog_worker` (debounce/load-detect/nametable threads), `shared_state` (cross-TU atomics)
- Docs clarify ReShade is required for GUI; without it, users edit JSON manually via Capture hotkey
