#ifndef TRANSMOG_TRANSMOG_APPLY_HPP
#define TRANSMOG_TRANSMOG_APPLY_HPP

#include "shared_state.hpp"
#include "slot_metadata.hpp"

#include <cstdint>
#include <string_view>

namespace Transmog
{
    /**
     * @brief Applies transmog through a direct SlotPopulator call with the target item.
     * @param a1 The live equip-slot component.
     * @param targetId Item the engine equips.
     * @details It builds the 16-byte item data structure and the empty swap entry internally.
     */
    void apply_transmog(__int64 a1, std::uint16_t targetId);

    /**
     * @brief Rebuild ONE slot in place - no tear-down, no equip, no SlotPopulator.
     *
     * @details Publishes the slot's dye state exactly as a full apply does and binds the slot for the setter
     *          substitute, then drives the engine's own per-slot rebuild with both apply windows open. The prefab
     *          swap keeps substituting (so the transmogged mesh survives), the dye injector sees its state on the
     *          DyeCopier call the rebuild drives, and the routing sends Color Override's material writes to the
     *          chosen color.
     *
     *          This is the ONLY correct way to rebuild a slot from outside this file. The bare engine rebuild it
     *          wraps is deliberately not exported. With no dye published, it makes the engine re-emit its natural
     *          records and silently strips the color the preceding apply injected.
     * @return false when the rebuild is unavailable. The caller must fall back to a full apply.
     */
    bool refresh_slot_appearance(std::size_t slot_idx);

    /**
     * @brief Equips the carrier AS ITSELF.
     *
     * @details The transmog visual comes from the prefab-wrapper swap. The carrier is a legitimately equippable item,
     *          so the call defeats no equip gate and falsifies no descriptor. The swap map binds the carrier's own
     *          prefab to the target item's prefab, which is what makes the target mesh render.
     * @param a1 The live equip-slot component.
     * @param carrier_id Item the wearer can equip in this slot. 0 falls back to a direct equip of targetId.
     * @param targetId Item whose visual the user wants. The log lines and the swap-map target derivation read it.
     * @param slot_sel Engine slot to equip into, or @ref NO_GAME_TAG to let the engine derive it from the item. The
     *        derivation is unambiguous only for slots whose item type maps to exactly one slot. Paired slots
     *        (Ring1/Ring2, Earring1/Earring2) share one type, so a derived equip always resolves to the first of the
     *        pair and the second can never be filled - pass the slot tag for those.
     * @param exclude_tag Slot to hide from the engine's item -> slot resolution for the duration of this equip, or
     *        @ref NO_GAME_TAG. Reaches a paired slot's second half when naming the destination outright is not
     *        possible because that slot owns no part record yet.
     */
    void apply_transmog_with_carrier(
        __int64 a1,
        std::uint16_t carrier_id,
        std::uint16_t targetId,
        std::uint16_t slot_sel = NO_GAME_TAG,
        std::uint16_t exclude_tag = NO_GAME_TAG
    );

    /**
     * @brief Resolves the default carrier item_id for one transmog slot on one character.
     * @param slot Transmog slot the carrier fills.
     * @param char_name Character the carrier must be equippable on.
     * @return The carrier item_id, or 0 when the name cannot be resolved.
     * @details Each character needs carriers from its own equippable pool. The engine class-gate rejects Kliff's
     *          plate base items on Damiane, and the reverse.
     */
    std::uint16_t default_carrier_for_slot(TransmogSlot slot, std::string_view char_name);

    /**
     * @brief Tears down and re-applies one slot.
     * @param a1 The live equip-slot component.
     * @param slot_idx Index of the slot to rebuild.
     * @details Hover-preview calls it to avoid full-gear flicker. It clears only the dispatch-cache entries whose
     *          game tag matches this slot.
     */
    void apply_single_slot_transmog(__int64 a1, std::size_t slot_idx);

    /**
     * @brief Runs the full apply pass over every active slot.
     * @param a1 The live equip-slot component.
     * @details It performs the two-phase tear-down, then one SlotPopulator call per active slot, then updates the
     *          dispatch cache, the suppress mask and the last-applied state.
     */
    void apply_all_transmog(__int64 a1);

    /**
     * @brief Clears every transmog in two passes.
     * @param a1 The live equip-slot component.
     * @details Pass A tears down orphan fakes. Pass B re-applies the real equipment the authoritative entry table
     *          records.
     */
    void clear_all_transmog(__int64 a1);

} // namespace Transmog

#endif // TRANSMOG_TRANSMOG_APPLY_HPP
