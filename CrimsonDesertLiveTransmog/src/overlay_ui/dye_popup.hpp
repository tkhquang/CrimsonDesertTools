// overlay_ui/dye_popup.hpp
//
// Per-slot dye and color-override popup, including the BeginPopup/EndPopup pair and the Dye / Color Override tab
// bar. It owns the function-local static arrays that carry one-shot tab-selection hand-offs from the row-chip widgets
// and the per-slot mod expansion state.

#ifndef TRANSMOG_OVERLAY_UI_DYE_POPUP_HPP
#define TRANSMOG_OVERLAY_UI_DYE_POPUP_HPP

#include <cstddef>

namespace Transmog
{

    /**
     * @brief Draws the dye and color-override popup for one slot.
     * @param slot Per-slot loop index, the TransmogSlot value cast to std::size_t.
     * @details The function owns the BeginPopup/EndPopup pair, so the caller only has to reach the matching id.
     */
    void draw_dye_popup(std::size_t slot);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_DYE_POPUP_HPP
