// overlay_ui/item_picker_popup.hpp
//
// Search-filterable picker popup that the per-slot loop opens. It handles item selection plus the cross-slot
// body-mesh prefab browser. When `auto_apply` is true, a hover over an item starts a debounce timer. Once that timer
// expires the slot-scoped apply fires through manual_apply_slot, so only the hovered slot re-equips.

#ifndef TRANSMOG_OVERLAY_UI_ITEM_PICKER_POPUP_HPP
#define TRANSMOG_OVERLAY_UI_ITEM_PICKER_POPUP_HPP

#include "overlay_ui/state.hpp"
#include "transmog_map.hpp"

#include <cstddef>
#include <cstdint>

namespace Transmog
{

    /**
     * @brief Draws the item and body-mesh prefab picker popup for one slot.
     * @param popup_id ImGui popup id the caller opened.
     * @param ui The slot's SlotUIState, which holds the search buffer, filters and hover-debounce state.
     * @param slot_category The slot's category, which selects the item catalog and the body-aware filters.
     * @param target_item_id In-out carrier item id. A prefab pick clears it.
     * @param auto_apply True to apply the hovered item once the debounce timer expires.
     * @param slot_idx Per-slot loop index, used for the slot-scoped apply.
     * @param out_prefab_idx Optional prefab-pick out-parameter. Pass nullptr to ignore prefab picks. Otherwise
     *        initialize it to -1 and read it after a commit: a value >= 0 is the body-mesh catalog index the user
     *        picked, and -1 means the user picked a real item or "(none)".
     * @return True on the frame the user commits a pick.
     * @note A prefab pick also clears @p target_item_id, so the caller's carrier-clear path runs on its own. When
     *       @p out_prefab_idx reads -1 after a commit, the caller must clear any prior body-mesh selection, because
     *       the two states are mutually exclusive.
     */
    [[nodiscard]] bool draw_item_picker_popup(
        const char *popup_id,
        SlotUIState &ui,
        Transmog::TransmogSlot slot_category,
        std::uint16_t &target_item_id,
        bool auto_apply,
        std::size_t slot_idx,
        int *out_prefab_idx = nullptr
    );

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_ITEM_PICKER_POPUP_HPP
