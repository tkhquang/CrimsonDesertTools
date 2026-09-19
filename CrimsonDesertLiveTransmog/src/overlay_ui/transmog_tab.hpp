// overlay_ui/transmog_tab.hpp
//
// Depth-1 sections at the top of the Transmog tab: the header (mod name, UI-scale combo, pending and unsaved
// badges), the global toggles (Enabled, Instant Apply, Keep Search Text), the character picker (per-character preset
// routing and body-kind override), and the preset management section (Append, Copy, Save as New, Remove, Prev, Next,
// rename).
//
// Each function ends with the trailing ImGui::Separator that visually closes its block, which frees the caller of
// separator bookkeeping.

#ifndef TRANSMOG_OVERLAY_UI_TRANSMOG_TAB_HPP
#define TRANSMOG_OVERLAY_UI_TRANSMOG_TAB_HPP

namespace Transmog
{
    /**
     * @brief Forget every font this mod added to the host atlas.
     * @details Call it once the host has cleared its atlas. Every cached pointer is dangling from that moment, and
     *          pushing one hangs the game.
     */
    void invalidate_host_font_cache();

    class PresetManager;

    /**
     * @brief Draws the tab header: mod name, UI-scale combo and the pending and unsaved badges.
     * @param pending True when the overlay holds uncommitted target edits.
     * @param pending_save True when the overlay holds edits no Save wrote to JSON.
     */
    void draw_header(bool pending, bool pending_save);

    /** @brief Draws the Enabled, Instant Apply and Keep Search Text toggles. */
    void draw_global_toggles();

    /**
     * @brief Draws the character picker: per-character preset routing plus the body-kind override.
     * @param pm The preset manager the picker routes through.
     */
    void draw_character_picker(PresetManager &pm);

    /**
     * @brief Draws the preset management section: Append, Copy, Save as New, Remove, Prev, Next and rename.
     * @param pm The preset manager the section acts on.
     */
    void draw_presets_section(PresetManager &pm);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_TRANSMOG_TAB_HPP
