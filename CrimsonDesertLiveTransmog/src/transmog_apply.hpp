#pragma once

#include "shared_state.hpp"

#include <cstdint>

namespace Transmog
{
    /**
     * Applies transmog by calling SlotPopulator directly with target item. Builds the 16-byte item data structure and
     * empty swap entry internally.
     */
    void apply_transmog(__int64 a1, std::uint16_t targetId);

    /**
     * Equips the carrier AS ITSELF; the transmog visual comes from the prefab-wrapper swap.
     *
     * The carrier is a legitimately equippable item, so no equip gate has to be defeated and no descriptor is
     * falsified. The swap map binds the carrier's own prefab to the target item's prefab, which is what makes the
     * target mesh render.
     *
     * @param carrierId  Item the wearer can equip in this slot. 0 falls back to equipping targetId directly.
     * @param targetId   Item whose visual is wanted; used for logging and by the swap-map target derivation.
     */
    /**
     * @param slotSel Engine slot to equip into, or 0xFFFF to let the engine derive it from the item.
     *
     * Deriving is only unambiguous for slots whose item type maps to exactly one slot. Paired slots (Ring1/Ring2,
     * Earring1/Earring2) share one type, so a derived equip always resolves to the first of the pair and the second
     * can never be filled. Pass the slot tag for those.
     */
    /**
     * @brief Rebuild ONE slot's visual in place -- no tear-down, no equip, no SlotPopulator.
     *
     * Calls the engine's own per-slot rebuild with both apply windows open, so the prefab swap keeps substituting
     * (the transmogged mesh survives) and material writes are routed to the chosen colour. The swap entry passed is
     * empty on purpose: the engine then rebuilds from whatever is already registered for the slot.
     *
     * Intended for per-slot visual changes that today cost a full tear-down and re-apply -- a dye or Color Override
     * edit being the obvious one.
     *
     * @return false when an anchor is unresolved, the slot has no live part record, or the call faulted.
     */
    bool refresh_slot_visual(TransmogSlot slot);

    void apply_transmog_with_carrier(__int64 a1, std::uint16_t carrierId, std::uint16_t targetId,
                                     std::uint16_t slotSel = 0xFFFF);

    /**
     * Resolve the default carrier itemId for a given transmog slot and currently-active character. Each character needs
     * carriers from its own equippable pool -- Kliff's plate base items are rejected by the engine class-gate on
     * Damiane (and vice versa). Returns 0 if the name can't be resolved.
     */
    std::uint16_t default_carrier_for_slot(TransmogSlot slot, const std::string &charName);


    /**
     * Single-slot apply: tears down and re-applies only the given slot. Used by hover-preview to avoid full-gear
     * flicker. Only clears dispatch cache entries matching this slot's game tag.
     */
    void apply_single_slot_transmog(__int64 a1, std::size_t slotIdx);

    /**
     * Full apply pass: two-phase tear-down + SlotPopulator for all active slots. Updates dispatch cache, suppress mask,
     * and last-applied state.
     */
    void apply_all_transmog(__int64 a1);

    /**
     * Two-pass clear: tears down orphan fakes (pass A), then re-applies real equipment from the authoritative entry
     * table (pass B).
     */
    void clear_all_transmog(__int64 a1);

} // namespace Transmog
