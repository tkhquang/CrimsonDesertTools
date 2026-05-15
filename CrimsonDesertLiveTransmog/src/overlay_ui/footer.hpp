// overlay_ui/footer.hpp
//
// Bottom-of-Transmog-tab UI: action buttons (Apply All / Clear All /
// Capture Outfit / Save) and the status footer (slot-populator state,
// active-slot count).  Sits at depth 1 of draw_overlay_content; closes
// only over the pending / pendingSave / preset-manager handles which
// the caller already computes for its own use.

#ifndef TRANSMOG_OVERLAY_UI_FOOTER_HPP
#define TRANSMOG_OVERLAY_UI_FOOTER_HPP

namespace Transmog
{

class PresetManager;

void draw_action_buttons(bool pending, bool pendingSave, PresetManager &pm);
void draw_status_footer();

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_FOOTER_HPP
