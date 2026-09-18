// overlay_ui/color_override.hpp
//
// Body of the per-slot "Color Override" tab inside the dye-popup tab bar. It draws once per slot, after the capture
// pass records ReShade-side dye state for that slot and while the user holds the popup open on it. The call site puts
// `BeginTabItem` and `EndTabItem` around the call, so the body runs inside an already-open tab item.

#ifndef TRANSMOG_OVERLAY_UI_COLOR_OVERRIDE_HPP
#define TRANSMOG_OVERLAY_UI_COLOR_OVERRIDE_HPP

#include "overlay_ui/state.hpp"

#include "color_override/color_swatch_table.hpp"

#include <cstddef>

namespace Transmog
{

    /**
     * @brief Draws the Color Override tab body for one slot.
     * @param slot Per-slot loop index, the TransmogSlot value cast to std::size_t.
     * @param detected Detected-region count for this slot, taken from the reinit metric that drives auto-reinit.
     * @param detectedReady The `detected > 0` sentinel the caller already cached.
     * @param ui The slot's SlotUIState, that is s_slotUI[slot].
     * @param dyeSlot The slot's DyeSlot row from ColorOverride::dye_state().
     * @note The function never calls ImGui::EndTabItem. The caller owns the BeginTabItem and EndTabItem pair.
     */
    void draw_color_override_tab_body(
        std::size_t slot,
        std::size_t detected,
        bool detectedReady,
        SlotUIState &ui,
        ColorOverride::DyeSlot &dyeSlot
    );

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_COLOR_OVERRIDE_HPP
