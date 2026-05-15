// overlay_ui/color_override.hpp
//
// Body of the per-slot "Color Override" tab inside the dye-popup tab
// bar.  Drawn once per slot when ReShade-side dye state has been
// captured and the user has the popup open on that slot.  The call
// site keeps `BeginTabItem` / `EndTabItem` around the call so the
// function body operates inside an already-open tab item.

#ifndef TRANSMOG_OVERLAY_UI_COLOR_OVERRIDE_HPP
#define TRANSMOG_OVERLAY_UI_COLOR_OVERRIDE_HPP

#include "overlay_ui/state.hpp"

#include "color_override/color_swatch_table.hpp"

#include <cstddef>

namespace Transmog
{

// Draws the Color Override tab body for slot `slot`.  Captures from the
// surrounding per-slot loop:
//   slot            : per-slot loop index (== TransmogSlot cast to size_t)
//   detected        : detected-region count for this slot (from the
//                     reinit metric used to decide auto-reinit)
//   detectedReady   : detected > 0 sentinel cached by the caller
//   ui              : the slot's SlotUIState (s_slotUI[slot]) reference
//   dyeSlot         : the slot's DyeSlot row from ColorOverride::dye_state()
//
// The function does NOT call ImGui::EndTabItem -- the caller owns the
// BeginTabItem / EndTabItem pair.
void draw_color_override_tab_body(std::size_t slot,
                                  std::size_t detected,
                                  bool detectedReady,
                                  SlotUIState &ui,
                                  ColorOverride::DyeSlot &dyeSlot);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_COLOR_OVERRIDE_HPP
