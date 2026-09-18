#ifndef TRANSMOG_REAL_PART_TEAR_DOWN_HPP
#define TRANSMOG_REAL_PART_TEAR_DOWN_HPP

#include <cstdint>

namespace Transmog::real_part_tear_down
{
    /**
     * @brief Publish the pre-resolved helper addresses this module calls into.
     * @details Reads them from ResolvedAddresses. Call it once during init, AFTER the SafeTearDown anchor resolves
     *          and AFTER ItemNameTable::build() has cached its chain walk, which yields indexed_string_lookup. On a
     *          resolution or sanity-check failure the caller treats the feature as disabled.
     * @return true when both helpers are published and is_ready() turns true.
     */
    bool resolve_helpers() noexcept;

    /**
     * @brief Remove the real part the engine currently shows in one slot.
     * @details Walks the authoritative equip table at *(a1+CONTAINER_PTR_OFFSET), locates the entry whose slot_tag
     *          matches @p game_slot_tag (helm = 0x0003), extracts its item descriptor hash through IndexedStringA, then
     *          calls the AOB-resolved SafeTearDown engine helper. It does NOT touch the auth table.
     * @param a1 ClientEquipSlotActorComponent pointer.
     * @param game_slot_tag Engine slot tag to tear down.
     * @return true when the engine helper ran for this slot; false on any failure.
     * @note Thread affinity: SafeTearDown walks the scene graph. The game normally drives it from its equip/render
     *       thread, and LT calls it from whatever thread apply_all_transmog runs on (currently the deferred-apply
     *       worker). Accepted risk.
     */
    bool tear_down_real_part(void *a1, std::uint16_t game_slot_tag) noexcept;

    /**
     * @brief Same scene-graph tear-down as @ref tear_down_real_part, keyed by an explicit item id.
     * @details Bypasses the auth-table walk. It removes a previously applied fake transmog mesh whose item_id LT
     *          tracked in last_ids but which never appeared in the auth table. The pipeline is
     *          item_id -> IndexedStringLookup -> DWORD hash -> SafeTearDown(a1, hash, slot_tag).
     * @param a1 ClientEquipSlotActorComponent pointer.
     * @param item_id Item id whose mesh comes off.
     * @param game_slot_tag Engine slot tag to tear down.
     * @return true when the engine helper ran; false on any failure, a hash lookup miss included.
     */
    bool tear_down_by_item_id(void *a1, std::uint16_t item_id, std::uint16_t game_slot_tag) noexcept;

    /**
     * @brief Read the real item currently equipped in one slot.
     * @details Walks the authoritative equip table at *(a1+CONTAINER_PTR_OFFSET) and returns the raw item word, not
     *          the hash-transformed value. Callers compare it against the fake transmog item_id and skip a tear-down
     *          that would strip layer-2 effects such as particles and hair-hide.
     * @param a1 ClientEquipSlotActorComponent pointer.
     * @param game_slot_tag Engine slot tag to read.
     * @return The raw item word, or 0 when the slot holds no entry or any read fails.
     */
    std::uint16_t get_real_item_id(void *a1, std::uint16_t game_slot_tag) noexcept;

    /**
     * @brief True once @ref resolve_helpers has succeeded.
     * @return true when both helper pointers are published.
     * @note Callers gate the feature on this instead of re-checking individual function pointers.
     */
    bool is_ready() noexcept;

    /**
     * @brief Read-only probe for "is this actor ready to receive tear_down/apply".
     * @details Two-stage check for the load-detect retry loop. During world load the engine briefly parks the
     *          equip-slot component on a placeholder whose downstream sub-handlers are not yet wired. A call to
     *          `apply_all_transmog` in that window consistently faults inside `SafeTearDown`'s deep dereferences and
     *          produces dozens of log lines per attempt. A readiness probe first lets the retry loop skip those
     *          attempts at microsecond cost without shrinking the overall retry budget.
     *
     *          Stage 1, structural. Walks the same container chain that `tear_down_real_part` dereferences
     *          (`*(a1+CONTAINER_PTR_OFFSET) -> container`, `container+0x08 -> array_base`,
     *          `container+0x10 -> count`) and verifies each link reads cleanly and `count` is in
     *          `[1, MAX_PLAUSIBLE_ENTRIES]`.
     *
     *          Stage 2, engine readiness. Locates the `pa::ClientCharacterControlActorComponent` (CCC) for this actor
     *          through RTTI, so component slot drift in the CCOIA component table does not affect the probe, then
     *          checks that the sub-handler pointer at `CCC + 0x130` is non-null. That is precisely the field
     *          `SafeTearDown` dereferences as `v15 = *(v7 + 304)` and unconditionally re-dereferences as `*v15`. The
     *          field is null during cold-load and holds a heap-allocated sub-object pointer at the moment
     *          `SafeTearDown` stops faulting, which makes it both a necessary and an empirically sufficient readiness
     *          gate.
     *
     * @param a1 ClientEquipSlotActorComponent pointer, the same value the SlotPopulator pipeline carries.
     * @return true when both stages pass; false otherwise.
     */
    bool is_actor_apply_ready(void *a1) noexcept;
} // namespace Transmog::real_part_tear_down

#endif // TRANSMOG_REAL_PART_TEAR_DOWN_HPP
