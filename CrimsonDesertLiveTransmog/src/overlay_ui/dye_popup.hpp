// overlay_ui/dye_popup.hpp
//
// Per-slot dye + color-override popup, including BeginPopup/EndPopup management and the Dye / Color Override tab bar.
// Owns three function-local static arrays (s_dyePopupJumpToColor / s_dyePopupJumpToDye / s_expandedMod) for one-shot
// tab-selection hand-offs from the row-chip widgets and per-slot mod expansion.

#ifndef TRANSMOG_OVERLAY_UI_DYE_POPUP_HPP
#define TRANSMOG_OVERLAY_UI_DYE_POPUP_HPP

#include <cstddef>

namespace Transmog
{

    void draw_dye_popup(std::size_t slot);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_DYE_POPUP_HPP
