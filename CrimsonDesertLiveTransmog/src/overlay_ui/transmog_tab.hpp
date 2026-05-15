// overlay_ui/transmog_tab.hpp
//
// Depth-1 sections at the top of the Transmog tab: header (mod name
// + UI-scale combo + pending/unsaved badges), global toggles
// (Enabled / Instant Apply / Keep Search Text), character picker
// (per-character preset routing + body-kind override), and the preset
// management section (Append / Copy / Save as New / Remove / Prev /
// Next + rename).
//
// Each function ends with the trailing ImGui::Separator that visually
// closes its block, keeping the caller free of separator bookkeeping.

#ifndef TRANSMOG_OVERLAY_UI_TRANSMOG_TAB_HPP
#define TRANSMOG_OVERLAY_UI_TRANSMOG_TAB_HPP

namespace Transmog
{

class PresetManager;

void draw_header(bool pending, bool pendingSave);
void draw_global_toggles();
void draw_character_picker(PresetManager &pm);
void draw_presets_section(PresetManager &pm);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_TRANSMOG_TAB_HPP
