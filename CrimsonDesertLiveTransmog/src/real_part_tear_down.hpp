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

    // Walks the authoritative equip table at *(a1+k_containerPtrOffset),
    // locates the entry whose slotTag matches `gameSlotTag` (helm = 0x0003),
    // extracts its item descriptor hash via IndexedStringA, then calls the
    // AOB-resolved SafeTearDown engine helper. Does NOT touch the auth
    // table. SEH-guarded; returns false on any failure.
    //
    // Thread affinity: SafeTearDown eventually walks the scene graph via
    // sub_1425EBAE0. The game normally invokes this from its equip/render
    // thread; we call it from whatever thread apply_all_transmog is
    // running on (currently the deferred-apply worker). Accepted risk.
    bool tear_down_real_part(void *a1, std::uint16_t gameSlotTag) noexcept;

    // Same scene-graph tear-down as `tear_down_real_part`, but bypasses
    // the auth-table walk and uses an explicit itemId. Intended for
    // removing previously-applied fake transmog meshes whose itemId we
    // tracked in lastIds but which never appeared in the auth table.
    //
    // Pipeline: itemId -> IndexedStringLookup -> DWORD hash ->
    // SafeTearDown(a1, hash, slotTag). SEH-guarded; returns false on
    // any failure (including hash lookup miss).
    bool tear_down_by_item_id(void *a1, std::uint16_t itemId,
                              std::uint16_t gameSlotTag) noexcept;

    // Walks the authoritative equip table at *(a1+k_containerPtrOffset)
    // and returns the raw
    // item word (not the hash-transformed value) for the given gameSlotTag.
    // Returns 0 if no entry for the slot or on any failure. Used by callers
    // to detect "fake transmog itemId equals real equipped itemId" and skip
    // tear-down that would strip layer-2 effects (particles, hair-hide).
    std::uint16_t get_real_item_id(void *a1,
                                   std::uint16_t gameSlotTag) noexcept;

    // True iff resolve_helpers() succeeded. Used by callers to gate the
    // feature without re-checking individual function pointers.
    bool is_ready() noexcept;

    /** @brief Read-only probe for "is this actor ready to receive
     *         tear_down/apply".
     *  @details Two-stage check, both SEH-guarded. Designed for the
     *           load-detect retry loop -- during world load the
     *           engine briefly parks the equip-slot component on a
     *           placeholder whose downstream sub-handlers are not
     *           yet wired. Calling `apply_all_transmog` in that
     *           window consistently SEH-faults inside `SafeTearDown`'s
     *           deep dereferences and produces dozens of log lines
     *           per attempt. Probing readiness first lets the retry
     *           loop skip those attempts at microsecond cost without
     *           shrinking the overall retry budget.
     *
     *           Stage 1 -- structural. Walks the same container
     *           chain that `tear_down_real_part` dereferences
     *           (`*(a1+k_containerPtrOffset) -> container`,
     *           `container+0x08 -> arrayBase`, `container+0x10 ->
     *           count`) and verifies each link reads cleanly and
     *           `count` is in `[1, k_maxPlausibleEntries]`.
     *
     *           Stage 2 -- engine readiness. Locates the
     *           `pa::ClientCharacterControlActorComponent` (CCC)
     *           for this actor via RTTI (so component slot drift in
     *           the CCOIA component table does not affect the
     *           probe), then checks the sub-handler pointer at
     *           `CCC + 0x130` is non-null. That is precisely the
     *           field `SafeTearDown` dereferences as
     *           `v15 = *(v7 + 304)` and unconditionally
     *           re-dereferences as `*v15`. The field is null during
     *           cold-load and becomes a heap-allocated sub-object
     *           pointer at the moment `SafeTearDown` stops faulting,
     *           making it both a necessary and empirically
     *           sufficient readiness gate.
     *
     *  @param a1 ClientEquipSlotActorComponent pointer -- the same
     *            value the SlotPopulator pipeline carries.
     *  @returns true when both stages pass; false otherwise. */
    bool is_actor_apply_ready(void *a1) noexcept;
}
