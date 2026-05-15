// overlay_ui/item_picker_popup.hpp
//
// Search-filterable picker popup used by the per-slot loop.  Handles
// item selection plus the cross-slot body-mesh prefab browser.  When
// `autoApply` is true, hovering an item starts a debounce timer; once
// it expires the slot-scoped apply fires via manual_apply_slot so
// only the hovered slot re-equips.

#ifndef TRANSMOG_OVERLAY_UI_ITEM_PICKER_POPUP_HPP
#define TRANSMOG_OVERLAY_UI_ITEM_PICKER_POPUP_HPP

#include "overlay_ui/state.hpp"

#include "transmog_map.hpp"

#include <cstddef>
#include <cstdint>

namespace Transmog
{

// outPrefabIdx contract:
//   nullptr  : caller doesn't care about prefab picks (legacy path).
//   non-null : initialized to -1 by the caller; set to a body-mesh
//              catalog index (>= 0) when the user picks a prefab in
//              the popup. The function also clears targetItemId on
//              prefab pick so the caller's existing carrier-clear
//              path runs naturally; -1 on commit means a real item
//              (or "(none)") was picked, in which case the caller
//              should clear any prior body-mesh selection so the two
//              states stay mutually exclusive.
[[nodiscard]] bool draw_item_picker_popup(const char *popupId,
                                          SlotUIState &ui,
                                          Transmog::TransmogSlot slotCategory,
                                          std::uint16_t &targetItemId,
                                          bool autoApply,
                                          std::size_t slotIdx,
                                          int *outPrefabIdx = nullptr);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_ITEM_PICKER_POPUP_HPP
