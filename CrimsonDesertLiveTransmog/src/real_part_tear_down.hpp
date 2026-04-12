#pragma once

#include <cstdint>

namespace Transmog::RealPartTearDown
{
    // Reads pre-resolved helper addresses from ResolvedAddresses and
    // performs a first-byte prologue sanity check. Must be called once
    // during init, AFTER the AOB scan for safeTearDown and AFTER
    // ItemNameTable::build() has cached its chain walk (which yields
    // indexedStringLookup). Returns false on any resolution or sanity-
    // check failure; the caller should treat the feature as disabled in
    // that case.
    bool resolve_helpers() noexcept;

    // Walks the authoritative equip table at *(a1+0x78), locates the entry
    // whose slotTag matches `gameSlotTag` (helm = 0x0003), extracts its
    // item descriptor hash via IndexedStringA, then calls the safe
    // scene-graph tear-down (sub_14075FE60). Does NOT touch the auth table.
    // SEH-guarded; returns false on any failure.
    //
    // Thread affinity: sub_14075FE60 eventually walks the scene graph via
    // sub_1425EBAE0. The game normally invokes this from its equip/render
    // thread. This POC calls it from whatever thread apply_all_transmog is
    // running on (currently the deferred-apply worker). Accepted risk for POC.
    bool tear_down_real_part(void *a1, std::uint16_t gameSlotTag) noexcept;

    // Same scene-graph tear-down as `tear_down_real_part`, but bypasses the
    // auth-table walk and uses an explicit itemId. Intended for removing
    // previously-applied fake transmog meshes whose itemId we tracked in
    // lastIds but which never appeared in the auth table.
    //
    // Pipeline: itemId -> sub_1402D75D0 (IndexedStringA lookup) -> DWORD hash
    // -> sub_14075FE60(a1, hash, slotTag). SEH-guarded; returns false on any
    // failure (including hash lookup miss).
    bool tear_down_by_item_id(void *a1, std::uint16_t itemId,
                              std::uint16_t gameSlotTag) noexcept;

    // Walks the authoritative equip table at *(a1+0x78) and returns the raw
    // item word (not the hash-transformed value) for the given gameSlotTag.
    // Returns 0 if no entry for the slot or on any failure. Used by callers
    // to detect "fake transmog itemId equals real equipped itemId" and skip
    // tear-down that would strip layer-2 effects (particles, hair-hide).
    std::uint16_t get_real_item_id(void *a1,
                                   std::uint16_t gameSlotTag) noexcept;

    // True iff resolve_helpers() succeeded. Used by callers to gate the
    // feature without re-checking individual function pointers.
    bool is_ready() noexcept;
}
