// overlay_ui/item_picker_popup.cpp
//
// Draws the search-filterable picker popup for one slot.  Used by the
// per-slot loop in draw_overlay_content (transmog tab) for both item
// and body-mesh-prefab selection.

#include "overlay_ui/item_picker_popup.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"

#include "carrier_defaults.hpp"
#include "color_override/color_picker_state.hpp"
#include "color_override/color_reinit.hpp"
#include "item_name_table.hpp"
#include "prefab_wrapper_swap.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
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

// Draw a search-filterable picker popup for one slot. Returns true if
// the user committed a selection this frame (caller is responsible for
// persisting it). When autoApply is true, hovering an item starts a
// debounce timer; once it expires the slot-scoped apply fires via
// manual_apply_slot so only the hovered slot re-equips.
[[nodiscard]] bool draw_item_picker_popup(const char *popupId,
                                   SlotUIState &ui,
                                   Transmog::TransmogSlot slotCategory,
                                   uint16_t &targetItemId,
                                   bool autoApply,
                                   std::size_t slotIdx,
                                   int *outPrefabIdx)
{
    bool committed = false;
    if (!ImGui::BeginPopup(popupId))
        return false;

    // Reset hover-debounce state on first frame so stale state from
    // a previous open doesn't suppress or prematurely fire an apply.
    if (ImGui::IsWindowAppearing())
    {
        ui.hoverPendingId = targetItemId;
        ui.hoverAppliedId = targetItemId;
        ui.hoverStartMs = 0;
    }

    ui_text_disabled("Item picker");
    ImGui::Separator();

    // --- Filter toggles ---
    //
    // Exact: only show items whose auto-detected category matches
    //        this slot (helm-suffixed items for the helm slot, etc.)
    // Hide variants: hide items flagged by has_variant_meta() (see
    //        item_name_table.cpp::k_descVariantMetaOffset for the
    //        per-version descriptor offset); all tested samples in
    //        that bucket failed to render via runtime transmog, so
    //        hiding them by default keeps the picker free of
    //        non-functional items.
    // Body-aware filters only matter for the 5 armor slots (Helm/Chest/
    // Cloak/Gloves/Boots) where the body-mesh family is gender-specific
    // (cd_phm_* vs cd_phw_*). Accessory + weapon slots share their
    // mesh families across genders, so the filter has no effect there
    // and the checkboxes just create confusion -- hide them.
    const bool isArmorSlotPickerHeader =
        Transmog::slot_meta(slotCategory).partShowHashKey != nullptr;
    if (!ui.prefabMode)
    {
        ImGui::Checkbox("Exact", &ui.exactFilter);
        if (isArmorSlotPickerHeader)
        {
            ImGui::SameLine();
            ImGui::Checkbox("Safe only", &ui.hideIncompatible);
            ImGui::SameLine();
            ImGui::Checkbox("Hide variants", &ui.hideVariants);
            ImGui::SameLine();
            ImGui::Checkbox("Hide cross-body", &ui.hideBodyMismatch);
            if (ImGui::IsItemHovered())
                ui_tooltip("Hide items whose body type (male/female) "
                           "doesn't match the active character. Cross-body "
                           "items may render with broken meshes.");
        }
    }
    else
    {
        ui_text_disabled("Prefabs mode (cross-slot)");
        ImGui::SameLine();
        ImGui::Checkbox("Exact##prefab_exact", &ui.prefabExactFilter);
        if (ImGui::IsItemHovered())
            ui_tooltip(
                "Limit the list to prefabs whose body-mesh family "
                "matches this slot. Untick to browse the full cross-"
                "slot catalog.");
    }

    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
        ui.navIndex = -1;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f));
    const bool searchEdited = ImGui::InputTextWithHint(
        "##search", "Search by name or id...",
        ui.searchBuf, sizeof(ui.searchBuf));
    ImGui::PopStyleVar();
    if (searchEdited)
        ui.navIndex = 0;

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##search"))
    {
        ui.searchBuf[0] = '\0';
        ui.navIndex = 0;
    }

    // Navigation buttons: move the highlight through the visible
    // list.  Placed next to the search bar so they're always
    // reachable without scrolling.  Enter commits the highlighted
    // item.
    ImGui::SameLine();
    ui.navMoved = false;
    {
        const int maxNav = ui.lastVisibleCount - 1;
        if (ImGui::SmallButton("^##nav_up"))
        {
            ui.navIndex = (ui.navIndex > 0) ? ui.navIndex - 1 : 0;
            ui.navMoved = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("v##nav_down"))
        {
            ui.navIndex = (ui.navIndex < maxNav)
                              ? ui.navIndex + 1
                              : (maxNav >= 0 ? maxNav : 0);
            ui.navMoved = true;
        }
    }
    // Prefabs toggle: only meaningful for slots that have a body-mesh
    // carrier (default source from k_defaultSrcByEnumSlot OR an INI
    // Pair_N_Source). Accessory slots without a mesh carrier
    // (Earring1/2, Necklace, Ring1/2) get -1 from selection_src_index
    // so the toggle is hidden -- there's no point browsing prefabs
    // from a slot that can never receive one. Force prefabMode off
    // for those slots so a pre-existing true value doesn't blank
    // the popup.
    {
        const bool slotHasCarrier =
            (Transmog::PrefabWrapperSwap::selection_src_index(
                 slotCategory) >= 0);
        if (slotHasCarrier)
        {
            ImGui::SameLine();
            ImGui::Checkbox("Prefabs##picker_prefab_mode",
                            &ui.prefabMode);
            if (ImGui::IsItemHovered())
                ui_tooltip("Browse all body-mesh prefabs across every "
                           "slot. The search bar filters across the "
                           "merged prefab list. Picking applies to the "
                           "prefab's native slot.");
        }
        else if (ui.prefabMode)
        {
            ui.prefabMode = false;
        }
    }

    const auto &table = Transmog::ItemNameTable::instance();
    const auto &entries = table.sorted_entries();

    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float lineH = ImGui::GetTextLineHeight();
    // Two-line row: display name + smaller internal name line.
    const float twoLineH = lineH * 2.0f + 4.0f;

    // Fixed-height scrollable region so the popup doesn't resize
    // per keystroke as the filter narrows.
    //
    // Width is content-derived in standalone mode: ~60 'M' glyphs
    // at the current font is enough for the longest item names
    // without leaving a wide empty band. The previous DPI-linear
    // value scaled past 1800px on 4K screens and dominated the
    // row. Reshade mode keeps a fixed 560px because its host UI
    // already constrains the parent window.
    const float popupW = s_standaloneMode
        ? (ImGui::CalcTextSize("M").x * 60.0f) : 560.0f;
    ImGui::BeginChild("##itemlist",
                      ImVec2(popupW, twoLineH * 12.0f),
                      true);

    // Increase vertical spacing between items for easier click/hover
    // targets.  Pushed INSIDE BeginChild so the popup-level spacing
    // stays default -- otherwise the popup's own scroll region may
    // capture mouse wheel events instead of the child.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x,
                               rowH * 0.35f));

    // "None" / clear entry at the top of the scroll region.
    // Hover-preview shows bare head/body (empty slot) via the
    // active+none path in apply_single_slot. Suppressed in
    // prefabMode where the popup is a prefab-browser only.
    if (!ui.prefabMode)
    {
        const bool selected = (targetItemId == 0);
        if (ImGui::Selectable("(none -- id 0)##picker_none", selected, 0, ImVec2(0, 0)))
        {
            targetItemId = 0;
            committed = true;
            ImGui::CloseCurrentPopup();
        }
        if (selected)
            ImGui::SetItemDefaultFocus();
        if (autoApply && ImGui::IsItemHovered() &&
            ui.hoverPendingId != 0)
        {
            ui.hoverPendingId = 0;
            ui.hoverStartMs = Transmog::steady_ms();
        }
    }

    // Hoist draw-list pointer outside the loop -- it is stable for
    // the entire BeginChild region and avoids a per-item lookup.
    ImDrawList *const dl = ImGui::GetWindowDrawList();

    // Semantic color constants for the two-line item display.
    static constexpr ImU32 k_colorCrashRisk = IM_COL32(255, 89, 89, 255);
    static constexpr ImU32 k_colorCarrier = IM_COL32(128, 217, 255, 255);
    // Orange -- carrier path works but body type mismatched. Item
    // will equip and may display with minor mesh artifacts; we show
    // it when the user disables "Hide cross-body".
    static constexpr ImU32 k_colorCrossBody = IM_COL32(255, 176, 64, 255);
    // Amber -- item has humanoid-range classifier tokens but none are
    // in the male/female body set (e.g. NPC variants like
    // Antumbra/Badran gloves with only `0x012F`). Equip goes through
    // the carrier path but render fidelity is inconsistent -- some
    // items resolve correctly, others show as broken meshes.
    static constexpr ImU32 k_colorAmbiguous = IM_COL32(230, 210, 120, 255);
    static constexpr ImU32 k_colorDimmed = IM_COL32(160, 160, 160, 200);

    // Active character's body kind (CE-verified 2026-04-21). Stable
    // for the entire popup invocation: PresetManager state cannot
    // mutate between picker frames on the render thread, so resolving
    // it once here avoids ~6k redundant PresetManager + string
    // comparisons per frame inside the filter loop. Armor rule
    // classifier tokens partition the catalog into disjoint body
    // families:
    //   Male:   {0x0018, 0x0058, 0x02E3}
    //   Female: {0x0072, 0x0382, 0x0300}
    //   Horse / pet / wagon / dragon: separate token pools with
    //     zero overlap with humanoid. Flagged as NonHumanoid and
    //     hidden unconditionally -- these never render on a human
    //     skeleton (horse saddles, cat backpacks, etc.).
    using BK = Transmog::ItemNameTable::BodyKind;
    BK charBody;
    {
        const auto &pm = Transmog::PresetManager::instance();
        // The picker filter follows the editing character: the user
        // is populating that character's preset and wants to see
        // items from that body's catalog. Falls back to the
        // controlled character automatically when editing is
        // unpinned because the two axes are identical in that state.
        const std::string ov =
            pm.body_kind_of(pm.editing_character());
        if (ov == "Male")
            charBody = BK::Male;
        else if (ov == "Female")
            charBody = BK::Female;
        else if (ov == "Both")
            charBody = BK::Both;
        else // "Auto" or unrecognised
            charBody =
                Transmog::ItemNameTable::body_kind_for_character(
                    pm.editing_character());
    }

    // Two-pass filter+draw. Pass 1 collects matching catalog indices;
    // pass 2 renders. The split lets the post-loop nav-clamp see the
    // full visible count without rescanning, and supports unbounded
    // result sets (the catalog has more chest items than any single
    // hardcoded cap could safely truncate).
    //
    // Not using ImGuiListClipper here is a deliberate choice, not a
    // restriction: measured ~6k Selectables per popup frame stay well
    // inside frame budget, and the unclipped loop keeps post-loop
    // nav-clamp logic simple (it needs the full visible count, which
    // a clipper hides behind its internal stepping).
    //
    // s_filtered is thread_local + static so the storage is reused
    // across frames without per-frame allocation. Safe because only
    // one picker popup can be open at a time (single call site, no
    // nested-popup re-entry path exists).
    static thread_local std::vector<std::size_t> s_filtered;
    s_filtered.clear();
    s_filtered.reserve(entries.size());
    int filteredByCategory = 0;
    int filteredByUnsafe = 0;
    int shown = 0;
    // Body-aware filters (Safe / Hide variants / Hide cross-body) only
    // apply to the 5 armor slots (Helm/Chest/Cloak/Gloves/Boots) where
    // the body-mesh family is gender-specific (cd_phm_* vs cd_phw_*).
    // Earrings/Necklace/Rings/Lantern/Glasses/Mask/Backpack/Bracelet
    // and weapons share their mesh families across genders, so the
    // gender / body-kind filter just hides perfectly-good options.
    // partShowHashKey is non-null exactly for the 5 armor slots in
    // slot_metadata.hpp -- reuse that table as the gate.
    const bool isArmorSlot =
        Transmog::slot_meta(slotCategory).partShowHashKey != nullptr;

    if (!ui.prefabMode)
    {
    for (std::size_t idx = 0; idx < entries.size(); ++idx)
    {
        const auto &e = entries[idx];
        // Exact filter accepts the slot itself OR its picker partner
        // (Earring1/2, Ring1/2, MainHand/OffHand share items because
        // their descriptor typeCodes are identical -- see
        // ItemNameTable::category_of which returns the lower-indexed
        // half for these pairs).
        if (ui.exactFilter &&
            !Transmog::slots_share_picker(e.category, slotCategory))
        {
            ++filteredByCategory;
            continue;
        }
        const bool nonHumanoid = (e.bodyKind == BK::NonHumanoid);
        const bool incompatible =
            (e.category == Transmog::TransmogSlot::Count) || nonHumanoid;
        if (isArmorSlot && ui.hideIncompatible && incompatible)
        {
            ++filteredByUnsafe;
            continue;
        }
        const bool ambiguousBody = (e.bodyKind == BK::Ambiguous);
        const bool bodyMatches =
            !nonHumanoid && (
                ambiguousBody ||
                (e.bodyKind == BK::Generic) ||
                (e.bodyKind == BK::Both) ||
                (charBody == BK::Generic) ||
                (e.bodyKind == charBody));
        if (isArmorSlot && ui.hideBodyMismatch && !bodyMatches)
        {
            ++filteredByUnsafe;
            continue;
        }
        if (isArmorSlot && ui.hideVariants && e.hasVariantMeta)
        {
            ++filteredByUnsafe;
            continue;
        }
        if (!name_contains_ci(e.name, ui.searchBuf) &&
            !name_contains_ci(e.displayName, ui.searchBuf))
            continue;
        s_filtered.push_back(idx);
    }

    shown = static_cast<int>(s_filtered.size());

    // Pass 2: render the filtered rows. bodyMatches is recomputed
    // here rather than cached from pass 1 because the per-item flag
    // also drives the colour tier and the carrier/crash-risk tag --
    // recomputation is cheaper than a parallel side-vector allocation.
    for (int row = 0; row < shown; ++row)
    {
        const auto &e = entries[s_filtered[row]];
        const bool nonHumanoid = (e.bodyKind == BK::NonHumanoid);
        const bool ambiguousBody = (e.bodyKind == BK::Ambiguous);
        // Body-mesh family is gender-specific only for the 5 armor
        // slots (cd_phm_* vs cd_phw_*). Accessory + weapon slots share
        // their meshes across genders, so a "BODY MISMATCH" or
        // "UNCERTAIN BODY" tag would be misleading. Treat every entry
        // as body-matching for non-armor slots so the colour/tag flow
        // below collapses to plain `usesCarrier` (cyan) or untagged.
        const bool bodyMatches =
            !isArmorSlot ||
            (!nonHumanoid && (
                ambiguousBody ||
                (e.bodyKind == BK::Generic) ||
                (e.bodyKind == BK::Both) ||
                (charBody == BK::Generic) ||
                (e.bodyKind == charBody)));

        // Tag items with visible badges:
        //   - "CRASH RISK"  -> !isPlayerCompatible (red)
        //   - "carrier"     -> hasVariantMeta, rendered via carrier
        //                      + char-class bypass (cyan)
        const char *tag = nullptr;
        // NPC variant items (hasVariantMeta) are humanoid and now
        // render via carrier + char-class bypass.  True crash risks
        // are non-player items WITHOUT variant meta (horse tack, etc).
        const bool usesCarrier = e.hasVariantMeta;
        // Colour tiers:
        //   red    CRASH RISK         -- rule list rejects this body,
        //                               no carrier path rescue.
        //   orange BODY MISMATCH      -- cross-body carrier, will equip
        //                               but mesh likely breaks.
        //   amber  UNCERTAIN BODY     -- humanoid-range but no body-
        //                               set match (e.g. 0x012F-only
        //                               NPC variants); render may or
        //                               may not resolve correctly.
        //   blue   carrier            -- NPC variant, same-body match,
        //                               reliable carrier path.
        const bool crashRisk = !bodyMatches && !e.hasVariantMeta;
        const bool crossBodyCarrier = !bodyMatches && e.hasVariantMeta;
        // Only flag ambiguous when it's actually a carrier item and
        // not already bucketed as cross-body (it isn't -- ambiguous
        // passes bodyMatches above). Suppressed entirely on non-armor
        // slots where body kind is irrelevant.
        const bool ambiguousCarrier =
            isArmorSlot && ambiguousBody && e.hasVariantMeta;
        if (crashRisk)
            tag = "non-player -- CRASH RISK";
        else if (crossBodyCarrier)
            tag = "carrier -- BODY MISMATCH";
        else if (ambiguousCarrier)
            tag = "carrier -- UNCERTAIN BODY";
        else if (usesCarrier)
            tag = "carrier";

        const bool isNavTarget = (row == ui.navIndex);
        const bool highlighted =
            (targetItemId == e.id) || isNavTarget;

        // Two-line selectable: hidden label, custom text overlay.
        char hiddenId[32];
        std::snprintf(hiddenId, sizeof(hiddenId),
                      "##picker_%04X", e.id);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        if (crashRisk)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.4f, 0.1f, 0.1f, 0.6f));
        else if (crossBodyCarrier)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.45f, 0.28f, 0.08f, 0.6f));
        else if (ambiguousCarrier)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.38f, 0.32f, 0.12f, 0.6f));
        else if (usesCarrier)
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.1f, 0.2f, 0.35f, 0.6f));
        if (ImGui::Selectable(hiddenId, highlighted,
                              0, ImVec2(0, twoLineH)))
        {
            targetItemId = e.id;
            committed = true;
            ImGui::CloseCurrentPopup();
        }

        // Overlay text on top of the selectable region.
        const bool hasDisplay = !e.displayName.empty();
        const char *primaryName =
            hasDisplay ? e.displayName.c_str() : e.name.c_str();

        // Line 1: display name (or internal name fallback) + tag.
        char line1[256];
        if (tag)
            std::snprintf(line1, sizeof(line1), "%s  (%s)",
                          primaryName, tag);
        else
            std::snprintf(line1, sizeof(line1), "%s", primaryName);

        const ImU32 mainColor = crashRisk          ? k_colorCrashRisk
                                : crossBodyCarrier ? k_colorCrossBody
                                : ambiguousCarrier ? k_colorAmbiguous
                                : usesCarrier      ? k_colorCarrier
                                : ImGui::GetColorU32(ImGuiCol_Text);

        dl->AddText(ImVec2(pos.x + 2.0f, pos.y + 1.0f),
                    mainColor, line1);

        // Line 2: internal name + hex id (dimmed).
        char line2[256];
        if (hasDisplay)
            std::snprintf(line2, sizeof(line2),
                          "  %s  [0x%04X]", e.name.c_str(), e.id);
        else
            std::snprintf(line2, sizeof(line2),
                          "  [0x%04X]", e.id);
        dl->AddText(ImVec2(pos.x + 2.0f, pos.y + lineH + 2.0f),
                    k_colorDimmed, line2);

        // Scroll the nav-highlighted row into view and handle
        // Enter to commit.
        if (isNavTarget)
        {
            if (ui.navMoved)
                ImGui::SetScrollHereY();
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            {
                targetItemId = e.id;
                committed = true;
                ImGui::CloseCurrentPopup();
            }
            // Feed nav target into hover-apply debounce so
            // button navigation previews items when auto-apply
            // is on.
            if (autoApply && ui.hoverPendingId != e.id)
            {
                ui.hoverPendingId = e.id;
                ui.hoverStartMs = Transmog::steady_ms();
            }
        }
        else if (targetItemId == e.id)
        {
            // Scroll to the currently selected item on popup
            // open so it's visible without manual scrolling.
            ImGui::SetItemDefaultFocus();
        }

        // Live preview: record the mouse-hovered item and start
        // the debounce timer.  Sync nav cursor to mouse so the
        // highlight follows the cursor.
        if (autoApply && ImGui::IsItemHovered() &&
            ui.hoverPendingId != e.id)
        {
            ui.hoverPendingId = e.id;
            ui.hoverStartMs = Transmog::steady_ms();
            ui.navIndex = row;
        }

        if (crashRisk || crossBodyCarrier || ambiguousCarrier || usesCarrier)
            ImGui::PopStyleColor(); // Header color
    }

    if (shown == 0)
    {
        if (ui.exactFilter && filteredByCategory > 0)
            ui_text_disabled("no matches in this category -- "
                            "uncheck Exact to widen");
        else if ((ui.hideIncompatible || ui.hideVariants ||
                  ui.hideBodyMismatch) &&
                 filteredByUnsafe > 0)
            ui_text_disabled("no matches -- uncheck filters "
                            "to show more items");
        else
            ui_text_disabled("no matches");
    }
    } // if (!ui.prefabMode)

    // --- Body-Mesh Prefab section ---
    //
    // Mirrors the carrier picker but for raw body-mesh prefabs. Picking
    // an entry here is equivalent to setting a body-mesh override on
    // the slot: the engine still equips the player's actual carrier
    // item, but the body-mesh hook substitutes the wrapper at apply
    // time so the visible mesh becomes the chosen prefab. The source
    // wrapper is auto-derived from the slot's INI source pair (the
    // Kliff carrier defaults). When outPrefabIdx is null the caller
    // doesn't want this section -- skip it so legacy callers see no
    // change.
    if (outPrefabIdx != nullptr)
    {
        namespace PWS = Transmog::PrefabWrapperSwap;

        ImGui::Separator();
        if (ui.prefabMode)
        {
            // --- Cross-slot prefab browser ---
            //
            // Walks every TransmogSlot's catalog, applies the search
            // filter, and labels each entry with its native slot.
            // Clicking applies the prefab to its NATIVE slot (not the
            // popup slot) -- the popup is just an entry point for
            // browsing the merged catalog. This way the user can pick
            // any prefab from any slot's picker.
            ui_text_disabled(
                "All Prefabs (search filters across every slot)");

            // After populate_slot_catalogs unified the per-slot vectors
            // (every slot now holds the FULL prefab set), iterating
            // every slot would push each prefab 20x with a meaningless
            // [Type] tag. Iterate slot 0 only and derive the visual
            // [Type] label per row from the prefab name's prefix
            // (slot_metadata's prefabPrefixMale/Female columns).
            //
            // Pass 1 builds a flat index of catalog positions matching
            // the search filter; pass 2 manually clips to the visible
            // scroll window (see manual virtualization comment below).
            const auto &cat0 = PWS::slot_catalog(
                static_cast<Transmog::TransmogSlot>(0));
            const std::size_t catalogedTotal = cat0.size();

            static thread_local std::vector<std::uint32_t> s_prefabFlat;
            s_prefabFlat.clear();
            for (std::size_t pi = 0; pi < cat0.size(); ++pi)
            {
                if (!name_contains_ci(cat0[pi].name, ui.searchBuf))
                    continue;
                if (ui.prefabExactFilter)
                {
                    auto derived = Transmog::slot_for_prefab_name(
                        cat0[pi].name);
                    if (!derived.has_value() ||
                        !Transmog::slots_share_prefab_family(
                            *derived, slotCategory))
                        continue;
                }
                s_prefabFlat.push_back(
                    static_cast<std::uint32_t>(pi));
            }
            const std::size_t totalShown = s_prefabFlat.size();

            // Derive a slot label from a prefab name. Substring-based
            // via slot_for_prefab_name() so NPC families (cd_nhw_*,
            // cd_nhm_*), other player roles (cd_pdm_*, cd_pgm_*, ...)
            // and accessory families (cd_t0000_*) all classify
            // correctly. See slot_metadata.hpp for the priority table.
            auto label_for_prefab = [&](const std::string &name) -> const char * {
                if (auto slot = Transmog::slot_for_prefab_name(name))
                    return Transmog::slot_meta(*slot).displayName;
                return "?";
            };

            // Manual virtualization rather than ImGuiListClipper:
            // the clipper's symbols clash between imgui_lib and the
            // ReShade SDK function-table thunks (it sits on the
            // boundary the overlay_ui_obj OBJECT lib is meant to
            // exclude).  Skip off-screen rows by emitting a top/
            // bottom Dummy spacer of the right height and only call
            // Selectable() on the visible window.  Net cost per
            // frame: O(visible_rows) instead of O(totalShown);
            // measured ~5000 emits -> ~30 on the live catalog.
            const float prefabRowH = ImGui::GetTextLineHeightWithSpacing();
            const float scrollY = ImGui::GetScrollY();
            const float winH = ImGui::GetWindowHeight();
            const int total = static_cast<int>(totalShown);
            int firstVis = static_cast<int>(scrollY / prefabRowH) - 1;
            int lastVis = static_cast<int>(
                (scrollY + winH) / prefabRowH) + 2;
            if (firstVis < 0) firstVis = 0;
            if (lastVis > total) lastVis = total;
            if (firstVis > total) firstVis = total;

            if (firstVis > 0)
                ImGui::Dummy(ImVec2(0.0f, firstVis * prefabRowH));
            {
                for (int row = firstVis; row < lastVis; ++row)
                {
                    const auto pi = static_cast<std::size_t>(
                        s_prefabFlat[static_cast<std::size_t>(row)]);
                    const auto &pe = cat0[pi];
                    const char *natName = label_for_prefab(pe.name);
                    char pickerId[224];
                    if (pe.is_loaded)
                    {
                        std::snprintf(
                            pickerId, sizeof(pickerId),
                            "[%s] %s##all_prefab_%zu",
                            natName, pe.name.c_str(), pi);
                    }
                    else
                    {
                        std::snprintf(
                            pickerId, sizeof(pickerId),
                            "[%s] %s  (unloaded)##all_prefab_%zu",
                            natName, pe.name.c_str(), pi);
                    }
                    if (ImGui::Selectable(pickerId, false, 0, ImVec2(0, 0)))
                    {
                        if (!pe.is_loaded)
                            continue;
                        // Per-slot catalogs were identical right after
                        // populate_slot_catalogs seeded them from the
                        // shared StringInfo walk, but
                        // enumerate_loader_registry_into_catalog adds
                        // slot-specific NPC entries (helm-only to Helm,
                        // chest-only to Chest, etc.) and re-sorts each
                        // slot independently. Indexing a popup-slot
                        // selection by cat0's pi therefore points at a
                        // DIFFERENT prefab in the popup slot's catalog
                        // and the row label drifts off the row the user
                        // actually clicked. Resolve by name through
                        // adopt_into_slot_and_select, which finds the
                        // matching entry in slotCategory's catalog (or
                        // inserts a copy when the prefab is unique to
                        // cat0) and returns the index that is valid for
                        // slotCategory specifically.
                        const auto adoptedIdx =
                            PWS::adopt_into_slot_and_select(
                                slotCategory,
                                static_cast<Transmog::TransmogSlot>(0),
                                static_cast<int>(pi));
                        if (adoptedIdx < 0)
                            continue;
                        // Tell the caller a prefab was picked on the
                        // popup slot so its existing
                        // pickedPrefabName / carrier-borrow path runs.
                        if (outPrefabIdx)
                            *outPrefabIdx = adoptedIdx;
                        committed = true;
                        DMK::Logger::get_instance().info(
                            "[picker] prefabs-mode pick: popupSlot={} "
                            "label={} adoptedIdx={} name='{}'",
                            Transmog::slot_name(slotCategory),
                            natName, adoptedIdx, pe.name.c_str());
                        ImGui::CloseCurrentPopup();
                    }
                    // Instant Apply intentionally NOT wired for prefab
                    // mode this release -- click-to-pick remains the
                    // only commit path here. Hover-apply needs extra
                    // carrier-borrow plumbing that's a follow-up.
                }
            }
            if (lastVis < total)
                ImGui::Dummy(ImVec2(0.0f, (total - lastVis) * prefabRowH));
            if (catalogedTotal == 0)
            {
                ui_text_disabled(
                    "no prefabs cataloged yet -- catalog may still "
                    "be populating, try Refresh");
            }
            else if (totalShown == 0)
            {
                ui_text_disabled("no matches");
            }
        }
        else
        {
            // Prefab catalog rendering is gated to prefabMode=true
            // (the cross-slot browser block above). The per-slot prefab
            // list otherwise bloats the items dropdown, so when
            // prefabMode is OFF we render no prefab rows; only the
            // clear-override option appears when an active prefab
            // selection exists, so it can be undone without flipping
            // the prefabMode toggle on first.
            const int curTgt = PWS::selection_tgt_index(slotCategory);
            if (curTgt >= 0)
            {
                char clrLabel[64];
                std::snprintf(clrLabel, sizeof(clrLabel),
                              "(clear active prefab override)##prefab_clr");
                if (ImGui::Selectable(clrLabel, false, 0, ImVec2(0, 0)))
                {
                    if (outPrefabIdx) *outPrefabIdx = -2;
                    committed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    }

    // Clamp nav index to the actual visible count so stale values
    // from a wider filter don't point past the end of the list.
    ui.lastVisibleCount = shown;
    if (ui.navIndex >= shown)
        ui.navIndex = shown > 0 ? shown - 1 : -1;

    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::EndChild();

    // --- Hover-apply debounce ---
    // Fire manual_apply_slot() only after the cursor has rested on
    // the same item for k_hoverDebounceMs.  Uses the slot-scoped
    // apply path so only this slot re-equips -- other slots are
    // untouched and don't flicker.
    if (autoApply &&
        ui.hoverPendingId != ui.hoverAppliedId &&
        ui.hoverStartMs != 0 &&
        (Transmog::steady_ms() - ui.hoverStartMs) >= k_hoverDebounceMs)
    {
        targetItemId = ui.hoverPendingId;
        ui.hoverAppliedId = ui.hoverPendingId;
        Transmog::flag_enabled().store(true, std::memory_order_relaxed);
        Transmog::manual_apply_slot(slotIdx);
    }

    ImGui::EndPopup();

    return committed;
}
} // namespace Transmog
