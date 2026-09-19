// overlay_ui/item_picker_popup.cpp
//
// Draws the search-filterable picker popup for one slot. Used by the per-slot loop in draw_overlay_content (transmog
// tab) for both item and body-mesh-prefab selection.

#include "overlay_ui/item_picker_popup.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"

#include "carrier_defaults.hpp"
#include "color_override/color_reinit.hpp"
#include "item_name_table.hpp"
#include "lang.hpp"
#include "prefab_wrapper_swap.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"
#include "transmog_map.hpp"

#include <DetourModKit/defines.hpp>
#include <DetourModKit/logger.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Transmog
{

    bool draw_item_picker_popup(
        const char *popup_id,
        SlotUIState &ui,
        Transmog::TransmogSlot slot_category,
        uint16_t &target_item_id,
        bool auto_apply,
        std::size_t slot_idx,
        int *out_prefab_idx
    )
    {
        bool committed = false;
        if (!ImGui::BeginPopup(popup_id))
            return false;

        // Reset hover-debounce state on the first frame, so stale state from a previous open does not suppress or
        // prematurely fire an apply.
        if (ImGui::IsWindowAppearing())
        {
            ui.hover_pending_id = target_item_id;
            ui.hover_applied_id = target_item_id;
            ui.hover_start_ms = 0;
            // Prefab-mode equivalent: seed both pending and applied with the popup-slot's current pick so the anchor
            // row does not trigger a redundant re-apply on open.
            ui.hover_pending_prefab = ui.picked_prefab_name;
            ui.hover_applied_prefab = ui.picked_prefab_name;
            ui.hover_prefab_start_ms = 0;
        }

        ui_text_disabled(lang::t("picker.title", "Item picker"));
        ImGui::Separator();

        // Filter toggles
        //
        // Exact: only show items whose auto-detected category matches
        //        this slot (helm-suffixed items for the helm slot, etc.)
        // Hide variants: hide items flagged by has_variant_meta() (see
        //        item_name_table.cpp::DESC_VARIANT_META_OFFSET for the
        //        per-version descriptor offset); all tested samples in
        //        that bucket failed to render via runtime transmog, so
        //        hiding them by default keeps the picker free of
        //        non-functional items.
        // Body-aware filters only matter for the 5 armor slots (Helm/Chest/ Cloak/Gloves/Boots) where the body-mesh
        // family is gender-specific (cd_phm_* vs cd_phw_*). Accessory + weapon slots share their mesh families across
        // genders, so the filter has no effect there and the checkboxes just create confusion - hide them.
        const bool is_armor_slot_picker_header = Transmog::slot_meta(slot_category).part_show_hash_key != nullptr;
        if (!ui.prefab_mode)
        {
            ImGui::Checkbox(lang::t("picker.exact", "Exact"), &ui.exact_filter);
            if (is_armor_slot_picker_header)
            {
                ImGui::SameLine();
                ImGui::Checkbox(lang::t("picker.safe_only", "Safe only"), &ui.hide_incompatible);
                ImGui::SameLine();
                ImGui::Checkbox(lang::t("picker.hide_variants", "Hide variants"), &ui.hide_variants);
                ImGui::SameLine();
                ImGui::Checkbox(lang::t("picker.hide_cross_body", "Hide cross-body"), &ui.hide_body_mismatch);
                if (ImGui::IsItemHovered())
                    ui_tooltip(
                        lang::t(
                            "picker.hide_cross_body.tip",
                            "Hide items whose body type (male/female) does not match the active character. "
                            "Cross-body items may render with broken meshes."
                        )
                    );
            }
        }
        else
        {
            ui_text_disabled(lang::t("picker.prefabs_mode", "Prefabs mode (cross-slot)"));
            ImGui::SameLine();
            ImGui::Checkbox(lang::t("picker.prefab_exact", "Exact##prefab_exact"), &ui.prefab_exact_filter);
            if (ImGui::IsItemHovered())
                ui_tooltip(
                    lang::t(
                        "picker.prefab_exact.tip",
                        "Limit the list to prefabs whose body-mesh family matches this slot. "
                        "Untick to browse the full cross-slot catalog."
                    )
                );
            ImGui::SameLine();
            ImGui::Checkbox(
                lang::t("picker.prefab_keep_open", "Keep open##prefab_keep_open"),
                &ui.prefab_keep_open_on_pick
            );
            if (ImGui::IsItemHovered())
                ui_tooltip(
                    lang::t(
                        "picker.prefab_keep_open.tip",
                        "Keep the picker open after each pick so you can quickly compare prefabs. "
                        "Untick to close on click."
                    )
                );
            // In-picker Apply button so users with Instant Apply off can commit a pick without leaving the popup. ImGui
            // closes a popup on outside clicks AND consumes the click, so the main Apply All below the picker cannot be
            // reached without re-opening. This button fires the same manual_apply() the footer's Apply All uses;
            // staying inside the popup keeps both the click consumption and the popup persistence happy.
            ImGui::SameLine();
            if (ImGui::SmallButton(lang::t("picker.prefab_apply", "Apply##prefab_apply")))
            {
                Transmog::flag_enabled().store(true, std::memory_order_relaxed);
                Transmog::manual_apply();
            }
            if (ImGui::IsItemHovered())
                ui_tooltip(
                    lang::t(
                        "picker.prefab_apply.tip",
                        "Apply all pending changes without closing the picker. Same as the main Apply All button."
                    )
                );
        }

        // Content-derived in both overlay modes: a fixed width stops tracking the text once this mod scales its
        // own tab, and a DPI-linear one runs past 1800px at 4K. 40 'M' is the tuned width, around 72 characters in
        // a proportional face. Measure before changing it: Segoe UI's 'M' is 14.0px at the 15.6px base, so 46
        // already lands at 644px. The floor only binds in a MONOSPACE face, where 40 'M' is 40 characters.
        const float popup_w = (std::max)(ImGui::CalcTextSize("M").x * 40.0f, ui_text_columns(56.0f));

        // Derive the search field from popup_w, never from a constant of its own: ui_px scales a constant by the
        // live font while popup_w scales by the 'M' advance, so the row would drift and push its trailing controls
        // outside the popup. Measure the labels actually drawn, translations included, with ImGui's hide-after-`##`
        // rule so the id is not counted. Prefabs is reserved for even where it is hidden, so the width holds.
        const ImGuiStyle &picker_style = ImGui::GetStyle();
        const auto visible_w = [](const char *label) { return ImGui::CalcTextSize(label, nullptr, true).x; };
        const auto button_w = [&picker_style, &visible_w](const char *label)
        { return visible_w(label) + picker_style.FramePadding.x * 2.0f; };
        const float trailing_w = button_w(lang::t("picker.clear_search", "Clear##search")) + button_w("^##nav_up") +
                                 button_w("v##nav_down") + ImGui::GetFrameHeight() + picker_style.ItemInnerSpacing.x +
                                 visible_w("Prefabs##picker_prefab_mode") + picker_style.ItemSpacing.x * 4.0f;
        ImGui::SetNextItemWidth((std::max)(popup_w - trailing_w, ui_text_columns(12.0f)));
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
            ui.nav_index = -1;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f));
        const char *search_hint = lang::t("picker.search_hint", "Search by name or id...");
        const bool search_edited =
            ImGui::InputTextWithHint("##search", search_hint, ui.search_buf, sizeof(ui.search_buf));
        ImGui::PopStyleVar();
        if (search_edited)
            ui.nav_index = 0;

        ImGui::SameLine();
        if (ImGui::SmallButton(lang::t("picker.clear_search", "Clear##search")))
        {
            ui.search_buf[0] = '\0';
            ui.nav_index = 0;
        }

        // Navigation buttons: move the highlight through the visible list. Placed next to the search bar so they are
        // always reachable without scrolling. Enter commits the highlighted item.
        ImGui::SameLine();
        ui.nav_moved = false;
        {
            const int max_nav = ui.last_visible_count - 1;
            // Up/Down arrow keys mirror the GUI nav buttons. ImGui's single-line InputText (the search box) does not
            // consume vertical arrows, so this is safe even when the search box has keyboard focus.
            const bool key_up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
            const bool key_down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
            if (ImGui::SmallButton("^##nav_up") || key_up)
            {
                ui.nav_index = (ui.nav_index > 0) ? ui.nav_index - 1 : 0;
                ui.nav_moved = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("v##nav_down") || key_down)
            {
                ui.nav_index = (ui.nav_index < max_nav) ? ui.nav_index + 1 : (max_nav >= 0 ? max_nav : 0);
                ui.nav_moved = true;
            }
        }
        // Prefabs toggle: only meaningful for slots that have a body-mesh carrier. The source comes from the slot's
        // carrier item in carrier_defaults.hpp, resolved to a rig mesh at runtime. A slot with no carrier named there
        // gets -1 from selection_src_index and the toggle is hidden - there is no point browsing prefabs from a slot
        // that can never receive one. Force prefab_mode off for those slots so a pre-existing true value does not blank
        // the popup.
        {
            const bool slot_has_carrier = (Transmog::prefab_wrapper_swap::selection_src_index(slot_category) >= 0);
            if (slot_has_carrier)
            {
                ImGui::SameLine();
                // "Prefabs" stays English in every language: it is the term the asset names, the logs and the
                // community all use, so a translated one would only break the connection to them.
                ImGui::Checkbox("Prefabs##picker_prefab_mode", &ui.prefab_mode);
                if (ImGui::IsItemHovered())
                    ui_tooltip(
                        lang::t(
                            "picker.prefabs_mode.tip",
                            "Browse all body-mesh prefabs across every slot. "
                            "The search bar filters across the merged prefab list. "
                            "Picking applies to the prefab's native slot."
                        )
                    );
            }
            else if (ui.prefab_mode)
            {
                ui.prefab_mode = false;
            }
        }

        const auto &table = Transmog::ItemNameTable::instance();
        // Hold the snapshot for the whole popup. A catalog reload on the worker retires the shared list, and this
        // reference is what keeps the one being drawn alive until the frame ends.
        const auto entries_snapshot = table.sorted_entries();
        const auto &entries = *entries_snapshot;

        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const float line_h = ImGui::GetTextLineHeight();
        // Two-line row: display name + smaller internal name line.
        const float two_line_h = line_h * 2.0f + 4.0f;

        // Fixed-height scrollable region so the popup does not resize per keystroke as the filter narrows.
        ImGui::BeginChild("##itemlist", ImVec2(popup_w, two_line_h * 12.0f), true);

        // Increase vertical spacing between items for easier click/hover targets. Pushed INSIDE BeginChild so the
        // popup-level spacing stays default - otherwise the popup's own scroll region may capture mouse wheel events
        // instead of the child.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, row_h * 0.35f));

        // "None" / clear entry at the top of the scroll region. Hover-preview shows bare head/body (empty slot) via the
        // active+none path in apply_single_slot. Suppressed in prefab_mode where the popup is a prefab-browser only.
        if (!ui.prefab_mode)
        {
            const bool selected = (target_item_id == 0);
            if (ImGui::Selectable(lang::t("picker.none_item", "(none - id 0)##picker_none"), selected, 0, ImVec2(0, 0)))
            {
                target_item_id = 0;
                committed = true;
                ImGui::CloseCurrentPopup();
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            if (auto_apply && ImGui::IsItemHovered() && ui.hover_pending_id != 0)
            {
                ui.hover_pending_id = 0;
                ui.hover_start_ms = Transmog::steady_ms();
            }
        }

        // Hoist draw-list pointer outside the loop - it is stable for the entire BeginChild region and avoids a
        // per-item lookup.
        ImDrawList *const dl = ImGui::GetWindowDrawList();

        // Semantic color constants for the two-line item display.
        static constexpr ImU32 color_crash_risk = IM_COL32(255, 89, 89, 255);
        static constexpr ImU32 color_carrier = IM_COL32(128, 217, 255, 255);
        // Orange - carrier path works but body type mismatched. Item will equip and may display with minor mesh
        // artifacts; we show it when the user disables "Hide cross-body".
        static constexpr ImU32 color_cross_body = IM_COL32(255, 176, 64, 255);
        // Amber - item has humanoid-range classifier tokens but none are in the male/female body set (e.g. NPC
        // variants like Antumbra/Badran gloves with only `0x012F`). Equip goes through the carrier path but render
        // fidelity is inconsistent - some items resolve correctly, others show as broken meshes.
        static constexpr ImU32 color_ambiguous = IM_COL32(230, 210, 120, 255);
        static constexpr ImU32 color_dimmed = IM_COL32(160, 160, 160, 200);

        // Active character's body kind. Stable for the entire popup invocation: PresetManager state cannot mutate
        // between picker frames on the render thread, so resolving it once here avoids ~6k redundant PresetManager +
        // string comparisons per frame inside the filter loop. The catalog's body families come from the
        // equip-eligibility column of the display_names TSV (item_name_table.cpp::m_body_by_name):
        //   Horse / pet / wagon / dragon: separate token pools with
        //     zero overlap with humanoid. Flagged as NonHumanoid and
        //     hidden unconditionally - these never render on a human
        //     skeleton (horse saddles, cat backpacks, etc.).
        using BK = Transmog::ItemNameTable::BodyKind;
        BK char_body;
        {
            const auto &pm = Transmog::PresetManager::instance();
            // The picker filter follows the editing character: the user is populating that character's preset and wants
            // to see items from that body's catalog. Falls back to the controlled character automatically when editing
            // is unpinned because the two axes are identical in that state.
            const std::string ov = pm.body_kind_of(pm.editing_character());
            if (ov == "Male")
                char_body = BK::Male;
            else if (ov == "Female")
                char_body = BK::Female;
            else if (ov == "Both")
                char_body = BK::Both;
            else // "Auto" or unrecognized
                char_body = Transmog::ItemNameTable::body_kind_for_character(pm.editing_character());
        }

        // Two-pass filter+draw. Pass 1 collects matching catalog indices; pass 2 renders. The split lets the post-loop
        // nav-clamp see the full visible count without rescanning, and supports unbounded result sets (the catalog has
        // more chest items than any single hardcoded cap could safely truncate).
        //
        // Not using ImGuiListClipper here is a deliberate choice, not a restriction: measured ~6k Selectables per popup
        // frame stay well inside frame budget, and the unclipped loop keeps post-loop nav-clamp logic simple (it needs
        // the full visible count, which a clipper hides behind its internal stepping).
        //
        // s_filtered is a function-local static, so the storage survives across frames with no per-frame
        // allocation. Only one picker popup is ever open at a time (single call site, no nested-popup re-entry path
        // exists), so one instance of the buffer serves every caller.
        static std::vector<std::size_t> s_filtered;
        s_filtered.clear();
        s_filtered.reserve(entries.size());
        int filtered_by_category = 0;
        int filtered_by_unsafe = 0;
        int shown = 0;
        // Body-aware filters (Safe / Hide variants / Hide cross-body) only apply to the 5 armor slots
        // (Helm/Chest/Cloak/Gloves/Boots) where the body-mesh family is gender-specific (cd_phm_* vs cd_phw_*).
        // Earrings/Necklace/Rings/Lantern/Glasses/Mask/Backpack/Bracelet and weapons share their mesh families across
        // genders, so the gender / body-kind filter just hides perfectly-good options. part_show_hash_key is non-null
        // exactly for the 5 armor slots in slot_metadata.hpp - reuse that table as the gate.
        const bool is_armor_slot = Transmog::slot_meta(slot_category).part_show_hash_key != nullptr;

        if (!ui.prefab_mode)
        {
            for (std::size_t idx = 0; idx < entries.size(); ++idx)
            {
                const auto &e = entries[idx];
                // Exact filter accepts the slot itself OR its picker partner (Earring1/2, Ring1/2, MainHand/OffHand
                // share items because their descriptor typeCodes are identical - see ItemNameTable::category_of which
                // returns the lower-indexed half for these pairs).
                if (ui.exact_filter && !Transmog::slots_share_picker(e.category, slot_category))
                {
                    ++filtered_by_category;
                    continue;
                }
                const bool non_humanoid = (e.body_kind == BK::NonHumanoid);
                const bool incompatible = (e.category == Transmog::TransmogSlot::Count) || non_humanoid;
                if (is_armor_slot && ui.hide_incompatible && incompatible)
                {
                    ++filtered_by_unsafe;
                    continue;
                }
                const bool ambiguous_body = (e.body_kind == BK::Ambiguous);
                const bool body_matches =
                    !non_humanoid && (ambiguous_body || (e.body_kind == BK::Generic) || (e.body_kind == BK::Both) ||
                                      (char_body == BK::Generic) || (e.body_kind == char_body));
                if (is_armor_slot && ui.hide_body_mismatch && !body_matches)
                {
                    ++filtered_by_unsafe;
                    continue;
                }
                if (is_armor_slot && ui.hide_variants && e.has_variant_meta)
                {
                    ++filtered_by_unsafe;
                    continue;
                }
                // search_name is set only for a pre-shaped locale, where display_name holds presentation forms
                // that no typed query can match. It is empty everywhere else, and an empty needle never matches.
                if (!name_contains_ci(e.name, ui.search_buf) && !name_contains_ci(e.display_name, ui.search_buf) &&
                    !name_contains_ci(e.search_name, ui.search_buf))
                {
                    continue;
                }
                s_filtered.push_back(idx);
            }

            shown = static_cast<int>(s_filtered.size());

            // Anchor nav on the popup-slot's current item pick so Up/Down starts stepping from the user's prior
            // selection rather than the top of the filtered list. Only runs on frames where nav_index was freshly reset
            // (popup-open via IsWindowAppearing, or filter edit). Falls back to row 0 when there is no selection
            // (target_item_id==0) or the selection is hidden by the current filters / search.
            if (ui.nav_index == -1 && shown > 0)
            {
                int anchor_row = 0;
                if (target_item_id != 0)
                {
                    for (int row = 0; row < shown; ++row)
                    {
                        if (entries[s_filtered[static_cast<std::size_t>(row)]].id == target_item_id)
                        {
                            anchor_row = row;
                            break;
                        }
                    }
                }
                ui.nav_index = anchor_row;
                // Piggyback on the existing nav-move scroll path so the popup scrolls the anchor row into view on first
                // appearance. nav_moved is a "highlight just shifted, scroll to it" signal regardless of whether the
                // shift came from a button or from the anchor block.
                ui.nav_moved = true;
            }

            // Pass 2: render the filtered rows. body_matches is recomputed here rather than cached from pass 1 because
            // the per-item flag also drives the color tier and the carrier/crash-risk tag - recomputation is cheaper
            // than a parallel side-vector allocation.
            for (int row = 0; row < shown; ++row)
            {
                const auto &e = entries[s_filtered[row]];
                const bool non_humanoid = (e.body_kind == BK::NonHumanoid);
                const bool ambiguous_body = (e.body_kind == BK::Ambiguous);
                // Body-mesh family is gender-specific only for the 5 armor slots (cd_phm_* vs cd_phw_*). Accessory +
                // weapon slots share their meshes across genders, so a "BODY MISMATCH" or "UNCERTAIN BODY" tag would be
                // misleading. Treat every entry as body-matching for non-armor slots so the color/tag flow below
                // collapses to plain `uses_carrier` (cyan) or untagged.
                const bool body_matches =
                    !is_armor_slot ||
                    (!non_humanoid && (ambiguous_body || (e.body_kind == BK::Generic) || (e.body_kind == BK::Both) ||
                                       (char_body == BK::Generic) || (e.body_kind == char_body)));

                // Tag items with visible badges:
                //   - "CRASH RISK"  -> !is_player_compatible (red)
                //   - "carrier"     -> has_variant_meta, rendered via carrier
                //                      + char-class bypass (cyan)
                const char *tag = nullptr;
                // NPC variant items (has_variant_meta) are humanoid and now render via carrier + char-class bypass.
                // True crash risks are non-player items WITHOUT variant meta (horse tack, etc).
                const bool uses_carrier = e.has_variant_meta;
                // Color tiers:
                //   red    CRASH RISK         - rule list rejects this body,
                //                               no carrier path rescue.
                //   orange BODY MISMATCH      - cross-body carrier, will equip
                //                               but mesh likely breaks.
                //   amber  UNCERTAIN BODY     - humanoid-range but no body-
                //                               set match (e.g. 0x012F-only
                //                               NPC variants); render may or
                //                               may not resolve correctly.
                //   blue   carrier            - NPC variant, same-body match,
                //                               reliable carrier path.
                const bool crash_risk = !body_matches && !e.has_variant_meta;
                const bool cross_body_carrier = !body_matches && e.has_variant_meta;
                // Only flag ambiguous when it is actually a carrier item and not already bucketed as cross-body (it
                // is not - ambiguous passes body_matches above). Suppressed entirely on non-armor slots where body kind
                // is irrelevant.
                const bool ambiguous_carrier = is_armor_slot && ambiguous_body && e.has_variant_meta;
                if (crash_risk)
                    tag = lang::t("picker.badge.crash_risk", "non-player - CRASH RISK");
                else if (cross_body_carrier)
                    tag = lang::t("picker.badge.body_mismatch", "carrier - BODY MISMATCH");
                else if (ambiguous_carrier)
                    tag = lang::t("picker.badge.body_unknown", "carrier - UNCERTAIN BODY");
                else if (uses_carrier)
                    tag = lang::t("picker.badge.carrier", "carrier");

                const bool is_nav_target = (row == ui.nav_index);
                const bool highlighted = (target_item_id == e.id) || is_nav_target;

                // Two-line selectable: hidden label, custom text overlay.
                char hidden_id[32];
                std::snprintf(hidden_id, sizeof(hidden_id), "##picker_%04X", e.id);
                ImVec2 pos = ImGui::GetCursorScreenPos();
                if (crash_risk)
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.4f, 0.1f, 0.1f, 0.6f));
                else if (cross_body_carrier)
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.45f, 0.28f, 0.08f, 0.6f));
                else if (ambiguous_carrier)
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.38f, 0.32f, 0.12f, 0.6f));
                else if (uses_carrier)
                    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.1f, 0.2f, 0.35f, 0.6f));
                if (ImGui::Selectable(hidden_id, highlighted, 0, ImVec2(0, two_line_h)))
                {
                    target_item_id = e.id;
                    committed = true;
                    ImGui::CloseCurrentPopup();
                }

                // Overlay text on top of the selectable region.
                const bool has_display = !e.display_name.empty();
                const char *primary_name = has_display ? e.display_name.c_str() : e.name.c_str();

                // Line 1: display name (or internal name fallback) + tag.
                char line1[256];
                if (tag)
                    std::snprintf(line1, sizeof(line1), "%s  (%s)", primary_name, tag);
                else
                    std::snprintf(line1, sizeof(line1), "%s", primary_name);

                const ImU32 main_color = crash_risk           ? color_crash_risk
                                         : cross_body_carrier ? color_cross_body
                                         : ambiguous_carrier  ? color_ambiguous
                                         : uses_carrier       ? color_carrier
                                                              : ImGui::GetColorU32(ImGuiCol_Text);

                dl->AddText(ImVec2(pos.x + 2.0f, pos.y + 1.0f), main_color, line1);

                // Line 2: internal name + hex id (dimmed).
                char line2[256];
                if (has_display)
                    std::snprintf(line2, sizeof(line2), "  %s  [0x%04X]", e.name.c_str(), e.id);
                else
                    std::snprintf(line2, sizeof(line2), "  [0x%04X]", e.id);
                dl->AddText(ImVec2(pos.x + 2.0f, pos.y + line_h + 2.0f), color_dimmed, line2);

                // Scroll the nav-highlighted row into view and handle
                // Enter to commit.
                if (is_nav_target)
                {
                    if (ui.nav_moved)
                        ImGui::SetScrollHereY();
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
                    {
                        target_item_id = e.id;
                        committed = true;
                        ImGui::CloseCurrentPopup();
                    }
                    // Feed nav target into hover-apply debounce so button navigation previews items when auto-apply is
                    // on.
                    if (auto_apply && ui.hover_pending_id != e.id)
                    {
                        ui.hover_pending_id = e.id;
                        ui.hover_start_ms = Transmog::steady_ms();
                    }
                }
                else if (target_item_id == e.id)
                {
                    // Scroll to the currently selected item on popup open so it is visible without manual scrolling.
                    ImGui::SetItemDefaultFocus();
                }

                // Live preview: record the mouse-hovered item and start the debounce timer. Sync nav cursor to mouse so
                // the highlight follows the cursor.
                if (auto_apply && ImGui::IsItemHovered() && ui.hover_pending_id != e.id)
                {
                    ui.hover_pending_id = e.id;
                    ui.hover_start_ms = Transmog::steady_ms();
                    ui.nav_index = row;
                }

                if (crash_risk || cross_body_carrier || ambiguous_carrier || uses_carrier)
                    ImGui::PopStyleColor(); // Header color
            }

            if (shown == 0)
            {
                if (ui.exact_filter && filtered_by_category > 0)
                    ui_text_disabled(
                        lang::t("picker.no_matches_exact", "no matches in this category - uncheck Exact to widen")
                    );
                else if ((ui.hide_incompatible || ui.hide_variants || ui.hide_body_mismatch) && filtered_by_unsafe > 0)
                    ui_text_disabled(
                        lang::t("picker.no_matches_filters", "no matches - uncheck filters to show more items")
                    );
                else
                    ui_text_disabled(lang::t("picker.no_matches", "no matches"));
            }
        }

        // Body-Mesh Prefab section
        //
        // Mirrors the carrier picker but for raw body-mesh prefabs. Picking an entry here is equivalent to setting a
        // body-mesh override on the slot: the engine still equips the player's actual carrier item, but the body-mesh
        // hook substitutes the wrapper at apply time so the visible mesh becomes the chosen prefab. The source wrapper
        // comes from the slot's INI source pair, that is the Kliff carrier defaults. A null out_prefab_idx means the
        // caller wants no prefab section, so the whole block is skipped for it.
        if (out_prefab_idx != nullptr)
        {
            namespace pws = Transmog::prefab_wrapper_swap;

            ImGui::Separator();
            if (ui.prefab_mode)
            {
                // Cross-slot prefab browser
                //
                // Walks every TransmogSlot's catalog, applies the search filter, and labels each entry with its native
                // slot. Clicking applies the prefab to its NATIVE slot (not the popup slot) - the popup is just an
                // entry point for browsing the merged catalog. This way the user can pick any prefab from any slot's
                // picker. After populate_slot_catalogs unified the per-slot vectors (every slot now holds the FULL
                // prefab set), iterating every slot would push each prefab 20x with a meaningless [Type] tag. Iterate
                // slot 0 only and derive the visual [Type] label per row from the prefab NAME, via
                // slot_metadata.hpp's slot_for_prefab_name() substring/role classifier.
                //
                // Pass 1 builds a flat index of catalog positions matching the search filter; pass 2 manually clips to
                // the visible scroll window (see manual virtualization comment below).
                const auto &cat0 = pws::slot_catalog(static_cast<Transmog::TransmogSlot>(0));
                const std::size_t cataloged_total = cat0.size();

                static std::vector<std::uint32_t> s_prefab_flat;
                s_prefab_flat.clear();
                for (std::size_t pi = 0; pi < cat0.size(); ++pi)
                {
                    if (!name_contains_ci(cat0[pi].name, ui.search_buf))
                        continue;
                    if (ui.prefab_exact_filter)
                    {
                        auto derived = Transmog::slot_for_prefab_name(cat0[pi].name);
                        if (!derived.has_value() || !Transmog::slots_share_prefab_family(*derived, slot_category))
                            continue;
                    }
                    s_prefab_flat.push_back(static_cast<std::uint32_t>(pi));
                }
                const std::size_t total_shown = s_prefab_flat.size();

                // Anchor nav on the popup-slot's current prefab pick so Up/Down starts stepping from the user's prior
                // selection rather than the top of the filtered prefab list. Mirrors the items-mode anchor block above.
                // Falls back to row 0 when there is no selection or the picked prefab was filtered out by Exact /
                // search.
                if (ui.nav_index == -1 && total_shown > 0)
                {
                    int anchor_row = 0;
                    if (!ui.picked_prefab_name.empty())
                    {
                        for (std::size_t i = 0; i < total_shown; ++i)
                        {
                            const auto pi = static_cast<std::size_t>(s_prefab_flat[i]);
                            if (cat0[pi].name == ui.picked_prefab_name)
                            {
                                anchor_row = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    ui.nav_index = anchor_row;
                    // Piggyback on the scroll-into-view path so the virtualized window opens centered on the anchor row
                    // rather than at the top.
                    ui.nav_moved = true;
                }

                // Header label with live counts. Shows "(matched / total)" when the search/filter narrows the visible
                // set; collapses to "(total)" when everything is shown.
                {
                    char hdr[96];
                    if (total_shown == cataloged_total)
                        std::snprintf(hdr, sizeof(hdr), "All Prefabs across all slots (%zu)", cataloged_total);
                    else
                        std::snprintf(
                            hdr,
                            sizeof(hdr),
                            "All Prefabs across all slots (%zu / %zu)",
                            total_shown,
                            cataloged_total
                        );
                    ui_text_disabled(hdr);
                }

                // Derive a slot label from a prefab name. Substring-based via slot_for_prefab_name() so NPC families
                // (cd_nhw_*, cd_nhm_*), other player roles (cd_pdm_*, cd_pgm_*, ...) and accessory families
                // (cd_t0000_*) all classify correctly. See slot_metadata.hpp for the priority table.
                auto label_for_prefab = [&](const std::string &name) -> const char *
                {
                    if (auto slot = Transmog::slot_for_prefab_name(name))
                        return Transmog::slot_meta(*slot).display_name;
                    return "?";
                };

                // Manual virtualization rather than ImGuiListClipper:
                // the clipper's symbols clash between imgui_lib and the ReShade SDK function-table thunks (it sits on
                // the boundary the overlay_ui_obj OBJECT lib is meant to exclude). Skip off-screen rows by emitting a
                // top/ bottom Dummy spacer of the right height and only call
                // Selectable() on the visible window. Net cost per frame: O(visible_rows) instead of O(total_shown);
                // measured ~5000 emits -> ~30 on the live catalog.
                const float prefab_row_h = ImGui::GetTextLineHeightWithSpacing();
                float scroll_y = ImGui::GetScrollY();
                const float win_h = ImGui::GetWindowHeight();
                const int total = static_cast<int>(total_shown);

                // Scroll-into-view for nav-button-moved highlight. Done BEFORE the visible-range computation so the
                // moved-to row actually lands inside [first_vis, last_vis) and renders with the selected highlight on
                // the same frame.
                if (ui.nav_moved && ui.nav_index >= 0 && ui.nav_index < total)
                {
                    const float nav_top_y = ui.nav_index * prefab_row_h;
                    const float nav_bot_y = nav_top_y + prefab_row_h;
                    if (nav_top_y < scroll_y)
                    {
                        ImGui::SetScrollY(nav_top_y);
                        scroll_y = nav_top_y;
                    }
                    else if (nav_bot_y > scroll_y + win_h)
                    {
                        const float new_scroll_y = nav_bot_y - win_h;
                        ImGui::SetScrollY(new_scroll_y);
                        scroll_y = new_scroll_y;
                    }
                }

                int first_vis = static_cast<int>(scroll_y / prefab_row_h) - 1;
                int last_vis = static_cast<int>((scroll_y + win_h) / prefab_row_h) + 2;
                if (first_vis < 0)
                    first_vis = 0;
                if (last_vis > total)
                    last_vis = total;
                if (first_vis > total)
                    first_vis = total;

                // Commit helper shared by mouse click, keyboard Enter, and nav-driven auto-apply. Returns true on a
                // successful adopt. preview_only=true is the auto-apply path: it lets the caller run its full commit
                // chain (pws::set_selection
                // + force-carrier + manual_apply) but suppresses the popup
                // close so the user can keep auditioning prefabs. Click /
                // Enter pass preview_only=false and still honor KeepOpen.
                auto commit_prefab_at = [&](std::size_t pi, bool preview_only = false) -> bool
                {
                    // Per-slot catalogs were identical right after populate_slot_catalogs seeded them from the shared
                    // StringInfo walk, but enumerate_loader_registry_into_catalog adds slot-specific NPC entries
                    // (helm-only to Helm, chest-only to Chest, etc.) and re-sorts each slot independently. Indexing a
                    // popup-slot selection by cat0's pi therefore points at a DIFFERENT prefab in the popup slot's
                    // catalog and the row label drifts off the row the user actually clicked. Resolve by name through
                    // adopt_into_slot_and_select, which finds the matching entry in slot_category's catalog (or inserts
                    // a copy when the prefab is unique to cat0) and returns the index that is valid for slot_category
                    // specifically.
                    const auto &pe = cat0[pi];
                    if (!pe.is_loaded)
                        return false;
                    const auto adopted_idx = pws::adopt_into_slot_and_select(
                        slot_category,
                        static_cast<Transmog::TransmogSlot>(0),
                        static_cast<int>(pi)
                    );
                    if (adopted_idx < 0)
                        return false;
                    if (out_prefab_idx)
                        *out_prefab_idx = adopted_idx;
                    committed = true;
                    DMK::log().info(
                        "[picker] prefabs-mode pick: popupSlot={} label={} adoptedIdx={} name='{}'",
                        Transmog::slot_name(slot_category),
                        label_for_prefab(pe.name),
                        adopted_idx,
                        pe.name.c_str()
                    );
                    if (!preview_only && !ui.prefab_keep_open_on_pick)
                        ImGui::CloseCurrentPopup();
                    return true;
                };

                if (first_vis > 0)
                    ImGui::Dummy(ImVec2(0.0f, first_vis * prefab_row_h));
                {
                    for (int row = first_vis; row < last_vis; ++row)
                    {
                        const auto pi = static_cast<std::size_t>(s_prefab_flat[static_cast<std::size_t>(row)]);
                        const auto &pe = cat0[pi];
                        const char *nat_name = label_for_prefab(pe.name);
                        // Sized for a TRANSLATED label. "(unloaded)" is longer in UTF-8 in most languages, and a
                        // truncation here would cut the trailing `##all_prefab_<n>` id off the label, which
                        // collapses every row onto one ImGui id.
                        char picker_id[256];
                        if (pe.is_loaded)
                        {
                            std::snprintf(
                                picker_id,
                                sizeof(picker_id),
                                "[%s] %s##all_prefab_%zu",
                                nat_name,
                                pe.name.c_str(),
                                pi
                            );
                        }
                        else
                        {
                            std::snprintf(
                                picker_id,
                                sizeof(picker_id),
                                lang::t("picker.prefab_row_unloaded", "[%s] %s  (unloaded)##all_prefab_%zu"),
                                nat_name,
                                pe.name.c_str(),
                                pi
                            );
                        }
                        // Highlight the prefab currently selected on the popup slot OR the row the Up/Down nav buttons
                        // last moved to. Matches the items list's combined "current selection + nav cursor" highlight
                        // rule.
                        const bool is_nav_target = (row == ui.nav_index);
                        const bool is_current = !ui.picked_prefab_name.empty() && ui.picked_prefab_name == pe.name;
                        const bool selected = is_current || is_nav_target;
                        if (ImGui::Selectable(picker_id, selected, 0, ImVec2(0, 0)))
                        {
                            commit_prefab_at(pi);
                        }
                        // Auto-apply on nav only - when the Up/Down nav cursor (GUI buttons or arrow keys) lands on a
                        // new row, feed its name into the prefab-mode hover-apply debounce. After HOVER_DEBOUNCE_MS of
                        // dwell the commit fires with preview_only=true so the popup stays open. Mouse hover does NOT
                        // trigger here: a body-mesh preview fans out to a full multi-actor manual_apply, which is too
                        // disruptive to chain off casual cursor motion.
                        if (auto_apply && is_nav_target && ui.hover_pending_prefab != pe.name)
                        {
                            ui.hover_pending_prefab = pe.name;
                            ui.hover_prefab_start_ms = Transmog::steady_ms();
                        }
                    }
                }
                if (last_vis < total)
                    ImGui::Dummy(ImVec2(0.0f, (total - last_vis) * prefab_row_h));

                // Keyboard Enter commits the nav-targeted row. Mirrors the items-list Enter behavior above, routed
                // through commit_prefab_at so the keep-open policy applies.
                if (ui.nav_index >= 0 && ui.nav_index < total &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
                {
                    const auto pi = static_cast<std::size_t>(s_prefab_flat[static_cast<std::size_t>(ui.nav_index)]);
                    commit_prefab_at(pi);
                }

                // Auto-apply debounce. Fires the prefab commit when the nav cursor has rested on the same row for
                // HOVER_DEBOUNCE_MS. preview_only=true keeps the popup open across the re-equip so the user can
                // continue browsing. The cat0 scan resolves the pending name back to a catalog index because the row
                // loop has already ended and pi is no longer in scope. A side-cache of pi next to the name would skip
                // this scan but the worst-case cost (~5000 entries x ~3 Hz = ~15us per 300ms) does not justify adding
                // another SlotUIState field.
                if (auto_apply && ui.hover_pending_prefab != ui.hover_applied_prefab &&
                    !ui.hover_pending_prefab.empty() && ui.hover_prefab_start_ms != 0 &&
                    (Transmog::steady_ms() - ui.hover_prefab_start_ms) >= HOVER_DEBOUNCE_MS)
                {
                    std::size_t target_pi = 0;
                    bool found = false;
                    for (std::size_t i = 0; i < cat0.size(); ++i)
                    {
                        if (cat0[i].name == ui.hover_pending_prefab)
                        {
                            target_pi = i;
                            found = true;
                            break;
                        }
                    }
                    if (found && commit_prefab_at(target_pi, true))
                    {
                        ui.hover_applied_prefab = ui.hover_pending_prefab;
                    }
                }

                // Hand the visible count off to the outer clamp so the nav buttons + clamp logic at end-of-function
                // operate on the prefab-mode count instead of the (zeroed) items count.
                shown = total;
                if (cataloged_total == 0)
                {
                    ui_text_disabled(
                        lang::t(
                            "picker.no_prefabs",
                            "no prefabs cataloged yet - catalog may still be populating, try Refresh"
                        )
                    );
                }
                else if (total_shown == 0)
                {
                    ui_text_disabled(lang::t("picker.no_matches", "no matches"));
                }
            }
            else
            {
                // Prefab catalog rendering is gated to prefab_mode=true (the cross-slot browser block above). The
                // per-slot prefab list otherwise bloats the items dropdown, so when prefab_mode is OFF we render no
                // prefab rows; only the clear-override option appears when an active prefab selection exists, so it can
                // be undone without flipping the prefab_mode toggle on first.
                const int cur_tgt = pws::selection_tgt_index(slot_category);
                if (cur_tgt >= 0)
                {
                    // Straight through rather than into a fixed buffer: a translated label is longer than the
                    // English one and a buffer sized for English would silently truncate it, taking the `##id`
                    // with it.
                    if (ImGui::Selectable(
                            lang::t("picker.clear_prefab_override", "(clear active prefab override)##prefab_clr"),
                            false,
                            0,
                            ImVec2(0, 0)
                        ))
                    {
                        if (out_prefab_idx)
                            *out_prefab_idx = -2;
                        committed = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }

        // Clamp nav index to the actual visible count so stale values from a wider filter do not point past the end of
        // the list.
        ui.last_visible_count = shown;
        if (ui.nav_index >= shown)
            ui.nav_index = shown > 0 ? shown - 1 : -1;

        ImGui::PopStyleVar(); // ItemSpacing
        ImGui::EndChild();

        // Hover-apply debounce
        // Fire manual_apply_slot() only after the cursor has rested on the same item for HOVER_DEBOUNCE_MS. Uses the
        // slot-scoped apply path so only this slot re-equips - other slots are untouched and do not flicker.
        if (auto_apply && ui.hover_pending_id != ui.hover_applied_id && ui.hover_start_ms != 0 &&
            (Transmog::steady_ms() - ui.hover_start_ms) >= HOVER_DEBOUNCE_MS)
        {
            target_item_id = ui.hover_pending_id;
            ui.hover_applied_id = ui.hover_pending_id;
            Transmog::flag_enabled().store(true, std::memory_order_relaxed);
            Transmog::manual_apply_slot(slot_idx);
        }

        ImGui::EndPopup();

        return committed;
    }
} // namespace Transmog
