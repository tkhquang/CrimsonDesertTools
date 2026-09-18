// overlay_ui/footer.hpp
//
// Bottom-of-Transmog-tab UI: the action buttons (Apply All, Clear All, Capture Outfit, Save) and the status footer
// (slot-populator state, active-slot count). This sits at depth 1 of draw_overlay_content. It closes only over the
// pending, pending_save and preset-manager handles that the caller already computes for its own use.

#ifndef TRANSMOG_OVERLAY_UI_FOOTER_HPP
#define TRANSMOG_OVERLAY_UI_FOOTER_HPP

namespace Transmog
{

    class PresetManager;

    /**
     * @brief Draws the Apply All, Clear All, Capture Outfit and Save buttons.
     * @param pending True when the overlay holds uncommitted target edits, which enables Apply All.
     * @param pending_save True when the overlay holds edits no Save wrote to JSON, which tints the Save button.
     * @param pm The preset manager the buttons act on.
     */
    void draw_action_buttons(bool pending, bool pending_save, PresetManager &pm);

    /**
     * @brief Draws the status line: slot-populator state and the active-slot count.
     */
    void draw_status_footer();

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_FOOTER_HPP
