// overlay_ui/dye_popup.cpp
//
// Per-slot dye + color-override popup. Drives the merged dye-record picker (16 ARMOR_MOD rows + material picker + top
// action bar) and hosts the Color Override tab item that delegates to draw_color_override_tab_body. Called once per
// slot from the per-slot loop in draw_overlay_content when the slot's dye chip is clicked (popup is owned here --
// BeginPopup/EndPopup pair stays internal to the function).

#include "overlay_ui/dye_popup.hpp"
#include "overlay_ui/color_override.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"

#include "color_override/color_pending_overrides.hpp"
#include "color_override/color_reinit.hpp"
#include "color_override/color_swatch_table.hpp"
#include "color_override/color_token_table.hpp"
#include "color_override/setter_substitute.hpp"

#include "generated/dye_color_table.hpp"
#include "generated/material_palette_table.hpp"

#include "dye_record_inject.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "prefab_wrapper_swap.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Transmog
{

    void draw_dye_popup(std::size_t slot)
    {
        auto &ui = s_slotUI[slot];

        const auto curSlot = static_cast<TransmogSlot>(slot);
        const bool isArmorSlot = curSlot == TransmogSlot::Helm || curSlot == TransmogSlot::Chest ||
                                 curSlot == TransmogSlot::Cloak || curSlot == TransmogSlot::Gloves ||
                                 curSlot == TransmogSlot::Boots;
        // The caller manages the per-slot ImGui::PushID/PopID pair, so we just bail out -- non-armor slots (weapons,
        // accessories) skip the dye chip entirely.
        if (!isArmorSlot)
            return;
        using namespace Transmog::DyeColorTable;

        // Pull the active preset's slot dye state. We mutate it in place; the dispatch loop reads the same memory next
        // apply.
        Preset *editPreset = PresetManager::instance().active_preset_mut();
        SlotDyeChannels *slotDye = editPreset ? &editPreset->slots[slot].dye : nullptr;

        // Declared in the dye-picker outer scope so the
        // Color Override tab below can read them without a separate top-level lookup block.
        auto &dyeSlot = Transmog::ColorOverride::dye_state()[slot];
        const std::size_t detected =
            Transmog::ColorOverride::SwatchTable::detected_swatch_count(static_cast<int>(slot));
        const bool detectedReady = detected > 0;

        ImGui::SameLine();

        // Find first active channel (for adjacent swatch).
        const ChannelDye *firstActiveCh = nullptr;
        int activeChCount = 0;
        if (slotDye)
        {
            for (const auto &c : *slotDye)
                if (c.active())
                {
                    if (!firstActiveCh)
                        firstActiveCh = &c;
                    ++activeChCount;
                }
        }

        // Per-popup tab jump flags. The Dye button + square dye chip force the popup onto the Dye tab; the circular
        // override chip forces it onto Color
        // Override. Without this, ImGui's tab-bar remembers the last-selected tab across popup opens, so a user who
        // visited Color Override once would re-land there when they next hit the Dye button.
        static bool s_dyePopupJumpToColor[k_slotCount] = {};
        static bool s_dyePopupJumpToDye[k_slotCount] = {};

        // Dye button: always plain "Dye" with no background color override -- keeps text legible regardless of dye
        // state. Active state is signalled by a small color-swatch chip rendered next to the button. Auto-sized so the
        // label fits at any font scale.
        const bool dyeBtn = ImGui::Button("Dye##dyeBtn", ImVec2(0, 0));

        // Constant row geometry: every slot reserves space for the dye chip + CO chip + reload icon even when inactive.
        // Without this, switching from a preset with dye/CO active to one without makes the standalone window
        // auto-shrink, and switching back forces it to expand again. Pinning the reserved width keeps the window stable
        // across preset switches.
        const float chip_h = ImGui::GetFrameHeight();
        ImGui::SameLine(0.0f, 4.0f);
        if (firstActiveCh)
        {
            ImVec4 sw(firstActiveCh->r / 255.0f, firstActiveCh->g / 255.0f, firstActiveCh->b / 255.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, sw);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sw);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, sw);
            if (ImGui::Button("##dyeSw", ImVec2(chip_h, chip_h)))
            {
                ImGui::OpenPopup("##dye_picker");
                s_dyePopupJumpToDye[slot] = true;
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
            {
                char tip[48];
                std::snprintf(tip, sizeof(tip), "%d active mod%s", activeChCount, activeChCount == 1 ? "" : "s");
                ui_tooltip(tip);
            }
        }
        else
        {
            ImGui::Dummy(ImVec2(chip_h, chip_h));
        }

        // Color-override chip: circular (to distinguish from the square dye chip), shows the first active override
        // colour for this slot. Clicking opens the popup AND jumps to the Color Override tab.
        const Transmog::ColorOverride::SwatchTable::SwatchOverride *firstOv = nullptr;
        int overrideCount = 0;
        for (std::size_t k = 0; k < detected; ++k)
        {
            const auto &sw = dyeSlot.swatches[k];
            if (sw.override_active)
            {
                if (!firstOv)
                    firstOv = &sw;
                ++overrideCount;
            }
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (firstOv)
        {
            const ImVec2 cur = ImGui::GetCursorScreenPos();
            const ImVec2 center(cur.x + chip_h * 0.5f, cur.y + chip_h * 0.5f);
            const float radius = chip_h * 0.5f - 1.0f;
            const ImVec4 colv(firstOv->r / 255.0f, firstOv->g / 255.0f, firstOv->b / 255.0f, 1.0f);
            const bool clicked = ImGui::InvisibleButton("##coSw", ImVec2(chip_h, chip_h));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(colv), 32);
            dl->AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_Border), 32, 1.0f);
            if (clicked)
            {
                ImGui::OpenPopup("##dye_picker");
                s_dyePopupJumpToColor[slot] = true;
            }
            if (hovered)
            {
                char tip[64];
                std::snprintf(tip, sizeof(tip), "%d active override%s", overrideCount, overrideCount == 1 ? "" : "s");
                ui_tooltip(tip);
            }

            // Reload icon: manual single-pass commit-retick for the slot's color overrides. Only shown when at least
            // one override is active (guarded by the outer `if (firstOv)` block). Coalesced if a reinit is already
            // running.
            //
            // Drawn with ImDrawList primitives (3/4 arc + arrowhead) rather than a font glyph because the overlay loads
            // only the default ImGui font (ASCII range) -- a Unicode reload glyph would render as a missing-glyph box.
            ImGui::SameLine(0.0f, 4.0f);
            const float reload_d = ImGui::GetFrameHeight();
            const ImVec2 reload_p = ImGui::GetCursorScreenPos();
            const bool reloadClicked = ImGui::InvisibleButton("##coReload", ImVec2(reload_d, reload_d));
            const bool reloadHovered = ImGui::IsItemHovered();
            {
                ImDrawList *rl_dl = ImGui::GetWindowDrawList();
                const ImVec2 rl_center(reload_p.x + reload_d * 0.5f, reload_p.y + reload_d * 0.5f);
                const float rl_radius = reload_d * 0.32f;
                const float thickness = (std::max)(1.0f, reload_d * 0.10f);
                // Slightly highlight on hover so the icon reads as interactive without needing a background frame.
                const ImU32 col = ImGui::GetColorU32(reloadHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                // 3/4 arc from 45 deg (top-right) sweeping clockwise through 315 deg (right). The gap sits at the 0-45
                // deg slice where the arrow tip points back along the circle's tangent.
                constexpr float kPi = 3.14159265358979f;
                const float a0 = -kPi * 0.25f; // -45
                const float a1 = kPi * 1.5f;   // +270
                rl_dl->PathArcTo(rl_center, rl_radius, a0, a1, 24);
                rl_dl->PathStroke(col, ImDrawFlags_None, thickness);
                // Arrowhead at the arc's terminus (a0). The tangent at angle a is (-sin(a), cos(a)) (CCW direction);
                // since we drew CW we negate to get the head pointing in the sweep direction.
                const float ax = rl_center.x + std::cos(a0) * rl_radius;
                const float ay = rl_center.y + std::sin(a0) * rl_radius;
                const float tx = std::sin(a0);
                const float ty = -std::cos(a0);
                // Outward (radial) unit vector for the head's perpendicular spread.
                const float ox = std::cos(a0);
                const float oy = std::sin(a0);
                const float head = (std::max)(2.0f, reload_d * 0.22f);
                const ImVec2 p_tip(ax + tx * head, ay + ty * head);
                const ImVec2 p_in(ax - ox * head * 0.7f, ay - oy * head * 0.7f);
                const ImVec2 p_out(ax + ox * head * 0.7f, ay + oy * head * 0.7f);
                rl_dl->AddTriangleFilled(p_tip, p_in, p_out, col);
            }
            if (reloadHovered)
                ui_tooltip("Re-tick this slot once so color\n"
                           "overrides commit. Use if a colour\n"
                           "edit didn't take.");
            if (reloadClicked && !Transmog::ColorOverride::Reinit::any_slot_reinit_active())
            {
                Transmog::flag_enabled().store(true, std::memory_order_relaxed);
                Transmog::ColorOverride::Reinit::schedule_color_commit_retick(static_cast<int>(slot));
            }
        }
        else
        {
            // Reserve the CO chip + reload icon slots so the row's footprint matches an active-CO row exactly.
            ImGui::Dummy(ImVec2(chip_h, chip_h));
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::Dummy(ImVec2(chip_h, chip_h));
        }

        if (dyeBtn)
        {
            // If the editing character has no preset yet, mint one from the current state so the popup has somewhere to
            // write instead of dropping the click. Re-fetch slotDye off the freshly-created preset so the popup body
            // below (and this frame's OpenPopup) observe it.
            if (!slotDye)
            {
                editPreset = PresetManager::instance().active_preset_mut_or_create();
                slotDye = editPreset ? &editPreset->slots[slot].dye : nullptr;
            }
            if (slotDye)
            {
                ImGui::OpenPopup("##dye_picker");
                s_dyePopupJumpToDye[slot] = true;
            }
        }

        // 10 color groups, ordered by HSL hue (red -> rose). Anchored by `string_key` (the data file's _stringKey
        // field). Hash + sample swatch are looked up at render time from the dye_color_table -- nothing in this UI
        // hardcodes a `_key` integer, so the picker survives a game patch that renumbers `_key` (we just regen the
        // dye_color_table from the new dump).
        //
        // Sample swatch = each group's saturated end-of-row shade (idx 18 for most groups, idx 17 for Bar_I). We pick
        // idx 18 universally; the visual difference for Bar_I is one row of saturation, acceptable.
        struct GroupRow
        {
            const char *string_key;
            const char *label; // UI-only display text
        };
        static constexpr GroupRow kGroupRows[] = {
            {"Her_Color_Group_I", "Red"},  {"Tom_Color_Group_I", "Orange"}, {"Por_Color_Group_I", "Yellow"},
            {"Bar_Color_Group_I", "Lime"}, {"Cal_Color_Group_I", "Green"},  {"Kwe_Color_Group_I", "Teal"},
            {"Del_Color_Group_I", "Cyan"}, {"Dem_Color_Group_III", "Blue"}, {"Dem_Color_Group_II", "Magenta"},
            {"Dem_Color_Group_I", "Rose"},
        };
        constexpr std::uint32_t kSampleShadeIdx = 18;

        // Per-slot UI state: which mod row is expanded (only one at a time; clicking a row collapses siblings). Static
        // so it persists across frames.
        static int s_expandedMod[k_slotCount] = {};
        static bool s_expandedInit = false;
        if (!s_expandedInit)
        {
            for (auto &v : s_expandedMod)
                v = -1;
            s_expandedInit = true;
        }

        // Dye changes are visual-only with no side effects, so we always re-apply on edit (regardless of the global
        // auto-apply toggle). force_apply_pending bypasses the dispatcher's no-change skip; without it a pure dye edit
        // (item id unchanged) would never re-fire the slotpop chain. flag_enabled is set so the user sees their pick
        // even if LT was toggled off when they opened the picker.
        auto reapply_now = [slot]()
        {
            force_apply_pending()[slot] = true;
            flag_enabled().store(true, std::memory_order_relaxed);
            // Mark unsaved -- picker writes dye directly into the active preset, so a Save button check against
            // slot_mappings can't catch it.
            dye_dirty().store(true, std::memory_order_relaxed);
            manual_apply_slot(slot);
        };

        // Helper lambda: render the picker body for one specific channel index. Used inside the expanded mod row.
        auto draw_channel_picker = [&](std::size_t chIdx, ChannelDye &ch)
        {
            ImGui::PushID(static_cast<int>(chIdx) + 9000);

            // Scale swatch button dims by the current font size so the picker block stays proportional to surrounding
            // text. Without this, fixed-pixel buttons look tiny when the host (standalone DPI-scaled atlas / user
            // override / ReShade host scale) inflates everything else.
            const float pickerScale = ImGui::GetFontSize() / 13.0f;

            // Group buttons row -- color swatches only, hovering shows the family name. Selected group gets a thin
            // border so the user knows which palette they're picking shades from. Hash + sample BGRA looked up
            // dynamically from the data table: zero hardcoded `_key` integers in this UI.
            for (std::size_t g = 0; g < std::size(kGroupRows); ++g)
            {
                if (g != 0)
                    ImGui::SameLine();
                const auto &row = kGroupRows[g];
                const Group *grp = find_group_by_name(row.string_key);
                // Sample = saturated shade idx 18 (or shade 0 if group missing -- shouldn't happen on a matched build,
                // but guards against patch mismatch).
                const std::uint32_t sampleBgra = (grp && grp->shades && grp->shade_count > kSampleShadeIdx)
                                                     ? grp->shades[kSampleShadeIdx].bgra
                                                     : 0xFF888888u;
                const float br = ((sampleBgra >> 16) & 0xFF) / 255.0f;
                const float bg = ((sampleBgra >> 8) & 0xFF) / 255.0f;
                const float bb = ((sampleBgra) & 0xFF) / 255.0f;
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(br, bg, bb, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(br * 1.15f, bg * 1.15f, bb * 1.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(br, bg, bb, 1.0f));
                const bool selected = !ch.group_name.empty() && ch.group_name == row.string_key;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                char gid[16];
                std::snprintf(gid, sizeof(gid), "##g%zu", g);
                if (ImGui::Button(gid, ImVec2(17.0f * pickerScale, 14.0f * pickerScale)))
                {
                    // string_key is the source-of-truth; hash is derived. If the group is missing (patch mismatch) we
                    // leave hash at 0 and the dye won't apply, but the picker state is preserved.
                    ch.group_name = row.string_key;
                    ch.group_hash = grp ? grp->key : 0;
                }
                if (selected)
                    ImGui::PopStyleColor();
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered())
                    ui_tooltip(row.label);
            }

            // Shade grid for the chosen group.
            if (ch.group_hash != 0)
            {
                const Group *grp = find_group(ch.group_hash);
                if (grp != nullptr && grp->shades != nullptr)
                {
                    const std::uint32_t maxIdx = grp->shade_count;

                    ui_text("Neutrals:");
                    for (std::uint32_t s = 0; s < 9 && s < maxIdx; ++s)
                    {
                        const std::uint32_t bgra = grp->shades[s].bgra;
                        const float r = ((bgra >> 16) & 0xFF) / 255.0f;
                        const float g2 = ((bgra >> 8) & 0xFF) / 255.0f;
                        const float b = ((bgra) & 0xFF) / 255.0f;
                        if (s != 0)
                            ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(r, g2, b, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(r, g2, b, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(r, g2, b, 1.0f));
                        char nlabel[16];
                        std::snprintf(nlabel, sizeof(nlabel), "##n%u", s);
                        if (ImGui::Button(nlabel, ImVec2(14.0f * pickerScale, 14.0f * pickerScale)))
                        {
                            ch.r = (bgra >> 16) & 0xFF;
                            ch.g = (bgra >> 8) & 0xFF;
                            ch.b = bgra & 0xFF;
                            reapply_now();
                        }
                        ImGui::PopStyleColor(3);
                        if (ImGui::IsItemHovered())
                        {
                            char tip[64];
                            std::snprintf(tip, sizeof(tip), "shade %u  RGB=(%u,%u,%u)", s, (bgra >> 16) & 0xFF,
                                          (bgra >> 8) & 0xFF, bgra & 0xFF);
                            ui_tooltip(tip);
                        }
                    }

                    ui_text("Hues:");
                    int col = 0;
                    for (std::uint32_t s = 9; s < maxIdx; ++s)
                    {
                        const std::uint32_t bgra = grp->shades[s].bgra;
                        const float r = ((bgra >> 16) & 0xFF) / 255.0f;
                        const float g2 = ((bgra >> 8) & 0xFF) / 255.0f;
                        const float b = ((bgra) & 0xFF) / 255.0f;
                        if (col != 0)
                            ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(r, g2, b, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(r, g2, b, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(r, g2, b, 1.0f));
                        char slabel[32];
                        std::snprintf(slabel, sizeof(slabel), "##s%u", s);
                        if (ImGui::Button(slabel, ImVec2(14.0f * pickerScale, 14.0f * pickerScale)))
                        {
                            ch.r = (bgra >> 16) & 0xFF;
                            ch.g = (bgra >> 8) & 0xFF;
                            ch.b = bgra & 0xFF;
                            reapply_now();
                        }
                        ImGui::PopStyleColor(3);
                        if (ImGui::IsItemHovered())
                        {
                            char tip[64];
                            std::snprintf(tip, sizeof(tip), "shade %u  RGB=(%u,%u,%u)", s, (bgra >> 16) & 0xFF,
                                          (bgra >> 8) & 0xFF, bgra & 0xFF);
                            ui_tooltip(tip);
                        }
                        ++col;
                        if (col >= 10)
                            col = 0;
                    }
                }
            }
            else
            {
                ui_text_disabled("(pick a group above)");
            }

            // Repair slider (offset +11 of dye record). The engine stores wear as a 0..127 byte; 0 means pristine, 127
            // means max wear. We surface the inverse as "Repair %" so 100 = pristine and 0 = max wear, matching the
            // conventional reading. 0xFF is the legacy "no override" sentinel and is treated as 100% on read for old
            // presets.
            int repairPct = (ch.repair_byte == 0xFF) ? 100 : 100 - ((ch.repair_byte * 100 + 63) / 127);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderInt("Repair %", &repairPct, 0, 100))
            {
                if (repairPct >= 100)
                    ch.repair_byte = 0;
                else if (repairPct <= 0)
                    ch.repair_byte = 127;
                else
                    ch.repair_byte = static_cast<std::uint8_t>(((100 - repairPct) * 127) / 100);
                reapply_now();
            }

            // --- Material picker (template u16 at +4..+5) ---
            //
            // The dye record's u16 at +4..+5 is a TEMPLATE INDEX (1..10) into partprefabdyetexturepalleteinfo. Each
            // template specifies texture variants per cat_code slot (cat1=cloth, cat2=secondary fabric, cat3=metal).
            // The engine resolves each channel's cat_code per-item and pulls the variant from the template. So template
            // 5 picks DIFFERENT textures for cat_001/002/003 simultaneously, not "variant 5 of one category."
            namespace MPT = Transmog::MaterialPaletteTable;
            ui_text("Material template:");
            // Width sized to the widest two-digit label so the "10" button is not cut off; all rows align at the same
            // width regardless of single- vs two-digit.
            const float mat_btn_w = ImGui::CalcTextSize("10").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
            for (std::uint16_t v = 1; v <= 10; ++v)
            {
                if (v != 1)
                    ImGui::SameLine();
                const bool selected = ch.material_id == v;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
                char mlabel[16];
                std::snprintf(mlabel, sizeof(mlabel), "%u##m%u", v, v);
                if (ImGui::Button(mlabel, ImVec2(mat_btn_w, 0.0f)))
                {
                    ch.material_id = v;
                    reapply_now();
                }
                if (selected)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    const auto *t = MPT::find(v);
                    if (t)
                    {
                        char tip[512];
                        int n = std::snprintf(tip, sizeof(tip), "Template %u (blend %.2f)\n", t->idx, t->blend);
                        for (std::size_t s = 0; s < t->slot_count && n < (int)sizeof(tip) - 1; ++s)
                        {
                            const auto &slot = t->slots[s];
                            if (slot.alias)
                                n += std::snprintf(tip + n, sizeof(tip) - n, "  cat%u %s/%s -> %u\n", slot.cat_code,
                                                   slot.label, slot.alias, slot.variant);
                            else
                                n += std::snprintf(tip + n, sizeof(tip) - n, "  cat%u %s -> %u\n", slot.cat_code,
                                                   slot.label, slot.variant);
                        }
                        ui_tooltip(tip);
                    }
                    else
                    {
                        char tip[64];
                        std::snprintf(tip, sizeof(tip), "Template %u (out of range)", v);
                        ui_tooltip(tip);
                    }
                }
            }
            ImGui::SameLine();
            {
                const bool selected = ch.material_id == 0xFFFF;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
                // Auto-sized so the label always fits regardless of overlay font scale; prior fixed widths truncated
                // the text on chunky bitmap fonts.
                if (ImGui::Button("Default##mFFFF", ImVec2(0, 0)))
                {
                    ch.material_id = 0xFFFF;
                    reapply_now();
                }
                if (selected)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ui_tooltip("0xFFFF -- engine picks the "
                               "natural variant for this channel.");
            }
            // Render the label on the left so the trailing ImGui auto-label does not clip on the popup's right edge in
            // the standalone overlay.
            int rawMat = static_cast<int>(ch.material_id);
            ui_text("Raw u16:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputInt("##matRaw", &rawMat, 1, 16,
                                ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (rawMat < 0)
                    rawMat = 0;
                if (rawMat > 0xFFFF)
                    rawMat = 0xFFFF;
                ch.material_id = static_cast<std::uint16_t>(rawMat);
                reapply_now();
            }

            if (ImGui::Button("Clear this mod", ImVec2(0, 0)))
            {
                ch = ChannelDye{};
                reapply_now();
            }

            ImGui::PopID();
        };

        // Per-popup-session auto-trigger flag for the Color
        // Override tab's first-activation 1-pass reinit. One entry per slot; reset on popup close so the next open
        // re-fires on still-empty slots. Declared in overlay_ui/state.hpp so color_override.cpp can read it too. Pin a
        // min size so the popup doesn't visibly collapse + re-expand while the 1-pass reinit runs (swatches arrive
        // ~1.5s after the tab is selected). Scaled by font size so the floor stays sensible on chunky DPI / overlay
        // scale.
        {
            const float fs = ImGui::GetFontSize();
            ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 36.0f, fs * 28.0f), ImVec2(FLT_MAX, FLT_MAX));
        }
        if (ImGui::BeginPopup("##dye_picker"))
        {
            // Two-tab popup: Dye (engine partprefab dye-record picker) and Color Override (post-binding matInst colour
            // injection). Distinct pipelines, unified entry point.
            if (ImGui::BeginTabBar("##dye_tabs"))
            {
                // Honor "jump to Dye tab" set by the Dye button + square dye chip on the row. One-shot: clear after
                // consuming so subsequent tab clicks behave normally.
                ImGuiTabItemFlags dyeTabFlags = s_dyePopupJumpToDye[slot] ? ImGuiTabItemFlags_SetSelected : 0;
                s_dyePopupJumpToDye[slot] = false;
                if (ImGui::BeginTabItem("Dye", nullptr, dyeTabFlags))
                {
                    // --- Top action bar ---
                    if (ImGui::Button("Mirror Mod 0 to all", ImVec2(0, 0)))
                    {
                        if (slotDye)
                        {
                            const auto src = (*slotDye)[0];
                            for (auto &ch : *slotDye)
                                ch = src;
                            reapply_now();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear all mods", ImVec2(0, 0)))
                    {
                        if (slotDye)
                            *slotDye = SlotDyeChannels{};
                        reapply_now();
                    }
                    ImGui::SameLine();
                    // Per-slot "sync from live" -- pulls the engine's current dye records for this slot (e.g. after the
                    // user applied dye at an in-game dye station) into the active preset, overwriting any picker-set
                    // channels on this slot only. The real-item restore path already auto-mirrors on
                    // picker -> real transitions; this button covers
                    // the case where the slot stays bound to a fake but the underlying real item's dye changed in-game.
                    if (ImGui::Button("Sync from live##dye_sync", ImVec2(0, 0)))
                    {
                        if (Transmog::sync_live_dye_for_slot(slot))
                            reapply_now();
                    }
                    if (ImGui::IsItemHovered())
                        ui_tooltip("Re-read this slot's dye from the engine's\n"
                                   "current auth table and overwrite the\n"
                                   "preset's saved channels. Use after you\n"
                                   "apply dye at an in-game dye station and\n"
                                   "want those bytes saved into this preset\n"
                                   "without re-running Capture Outfit.\n"
                                   "\n"
                                   "No-op if the slot has no live auth-table\n"
                                   "entry (nothing equipped) or no dye records.");
                    ImGui::SameLine();
                    if (ImGui::Button("Close", ImVec2(0, 0)))
                        ImGui::CloseCurrentPopup();

                    // Per-slot dye-inject mode toggle. See PresetSlot::dyeSparse in preset_manager.hpp for the full
                    // semantics. Toggling reapplies immediately so the user sees the visual change without an extra
                    // Apply All click.
                    if (editPreset != nullptr && slot < editPreset->slots.size())
                    {
                        bool sparse = editPreset->slots[slot].dyeSparse;
                        if (ImGui::Checkbox("Sparse##dye_sparse", &sparse))
                        {
                            editPreset->slots[slot].dyeSparse = sparse;
                            dye_dirty().store(true, std::memory_order_release);
                            reapply_now();
                        }
                        if (ImGui::IsItemHovered())
                            ui_tooltip("Sparse (default, matches the merchant dye UI):\n"
                                       "emit only the channels you set; engine paints\n"
                                       "the rest with its own defaults.\n"
                                       "\n"
                                       "Turn off for cross-class fake transmog where\n"
                                       "every channel needs your colour to suppress\n"
                                       "the carrier's default palette.");
                    }

                    ui_text_disabled("Tip: items typically use mods (dye slots) 1-12; "
                                     "rest no-ops.\n"
                                     "This mod cannot tell which item is dyeable or "
                                     "has many slots.");
                    ImGui::Separator();

                    // --- 16 mod rows, each inline-expandable ---
                    if (slotDye)
                    {
                        for (std::size_t k = 0; k < slotDye->size(); ++k)
                        {
                            ChannelDye &ch = (*slotDye)[k];
                            ImGui::PushID(static_cast<int>(k) + 5000);

                            // Header: chevron + "Mod K" + swatch + summary. Width is computed from the widest label ("v
                            // Mod 15") so all rows align in a column regardless of font scale; auto-size would let
                            // single-digit rows shrink and break the column.
                            const bool expanded = s_expandedMod[slot] == static_cast<int>(k);
                            char hdr[24];
                            std::snprintf(hdr, sizeof(hdr), "%s Mod %2zu", expanded ? "v" : ">", k);
                            const float row_btn_w =
                                ImGui::CalcTextSize("v Mod 15").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
                            if (ImGui::Button(hdr, ImVec2(row_btn_w, 0.0f)))
                            {
                                s_expandedMod[slot] = expanded ? -1 : static_cast<int>(k);
                            }

                            // Color swatch. Sized to the row button's height so the two widgets sit on the same
                            // baseline regardless of font scale, and made clickable so the swatch acts as a second hit
                            // target for expanding the row (small fixed sizes were hard to hit in the standalone
                            // overlay).
                            ImGui::SameLine();
                            ImVec4 sw = ch.active() ? ImVec4(ch.r / 255.0f, ch.g / 255.0f, ch.b / 255.0f, 1.0f)
                                                    : ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
                            ImGui::PushStyleColor(ImGuiCol_Button, sw);
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sw);
                            const float sw_size = ImGui::GetFrameHeight();
                            if (ImGui::Button("##sw", ImVec2(sw_size, sw_size)))
                            {
                                s_expandedMod[slot] = expanded ? -1 : static_cast<int>(k);
                            }
                            ImGui::PopStyleColor(2);

                            // Summary text.
                            ImGui::SameLine();
                            if (ch.active())
                            {
                                int repairPct =
                                    (ch.repair_byte == 0xFF) ? 100 : 100 - ((ch.repair_byte * 100 + 63) / 127);
                                ui_text("RGB=(%u,%u,%u) rep=%d%%", ch.r, ch.g, ch.b, repairPct);
                            }
                            else
                            {
                                ui_text_disabled("(default)");
                            }

                            // Inline expanded body.
                            if (expanded)
                            {
                                ImGui::Indent(20.0f);
                                draw_channel_picker(k, ch);
                                ImGui::Unindent(20.0f);
                                ImGui::Separator();
                            }

                            ImGui::PopID();
                        }
                    }

                    ImGui::EndTabItem();
                } // end Dye tab

                // --- Color Override tab ---
                // Post-binding matInst colour injection. Different pipeline from dye records but presented in the same
                // popup as a unified per-slot picker. On first activation per popup session, auto-fires a single-pass
                // reinit so the swatch grid is populated without the user clicking Re-init -- one pass is enough; the
                // 3-pass ghost-filtered variant remains available via the Re-init button below. Honor "jump to Color
                // Override tab" set by the circular override chip on the row. One-shot:
                // clear the flag after consuming so subsequent tab clicks behave normally.
                ImGuiTabItemFlags coTabFlags = s_dyePopupJumpToColor[slot] ? ImGuiTabItemFlags_SetSelected : 0;
                s_dyePopupJumpToColor[slot] = false;
                if (Transmog::flag_color_override().load(std::memory_order_acquire) &&
                    ImGui::BeginTabItem("Color Override", nullptr, coTabFlags))
                {
                    draw_color_override_tab_body(slot, detected, detectedReady, ui, dyeSlot);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndPopup();
        }
        else
        {
            // Popup closed -- re-arm the auto-trigger so the next open will re-fire on still-empty slots.
            s_coAutoTriggered[slot] = false;
        }
    }

} // namespace Transmog
