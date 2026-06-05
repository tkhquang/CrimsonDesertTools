// overlay_ui.cpp -- Transmog overlay UI top-level glue.
//
// This TU + the overlay_ui/ section files are compiled against the
// ReShade SDK's <imgui.h> (a copy of the upstream v1.92.5 header) plus
// reshade_overlay.hpp's function-table wrappers.  Every `ImGui::Foo(...)`
// call routes through `imgui_function_table_instance()` -- a pointer
// set either by ReShade (when our addon registers) or by us in
// standalone init via `lt_get_imgui_function_table()` from the
// populator TU.
//
// The CMake `overlay_ui_obj` OBJECT lib is the boundary: it gets ONLY
// the ReShade SDK include path so the function-table ImGui:: thunks are
// the only `ImGui::` symbols visible.  Adding imgui_lib's include path
// here would let the real, non-thunk ImGui:: symbols leak in and produce
// LNK2005 against imgui_widgets.obj (variadic format thunks cannot be
// COMDAT-folded under MSVC).
//
// Variadic ImGui helpers (Text / TextDisabled / TextColored) live in
// overlay_ui/helpers.{hpp,cpp} as plain extern functions for the same
// reason: a per-TU inline-variadic emission would re-trigger LNK2005.
// Non-variadic ImGui:: thunks are safe to call directly.
//
// Public entry points (`draw_overlay`, `init_reshade_overlay`,
// `shutdown_reshade_overlay`, `is_reshade_overlay_active`) sit at the
// bottom of the file outside the anonymous namespace.

#include "overlay.hpp"
#include "overlay_ui/color_override.hpp"
#include "overlay_ui/dye_popup.hpp"
#include "overlay_ui/footer.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/item_picker_popup.hpp"
#include "overlay_ui/state.hpp"
#include "overlay_ui/transmog_tab.hpp"
#include "color_override/color_pending_overrides.hpp"
#include "color_override/color_reinit.hpp"
#include "color_override/color_swatch_table.hpp"
#include "color_override/color_token_table.hpp"
#include "prefab_wrapper_swap.hpp"
#include "constants.hpp"
#include "dye_record_inject.hpp"
#include "generated/dye_color_table.hpp"
#include "generated/material_palette_table.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

// <imgui.h> here is the ReShade SDK stub (upstream v1.92.5).
// <reshade.hpp> pulls in reshade_overlay.hpp, which exposes the
// function-table ImGui:: thunks plus register_addon / register_overlay.
// Do NOT include reshade_overlay.hpp directly: it has no include guard,
// and a second include re-defines every inline namespace-ImGui thunk.
#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Transmog
{

namespace {

// All ImGui widget calls for the transmog UI.  Called by both the
// ReShade tab callback and the standalone window wrapper.

static void draw_overlay_content()
{
    using namespace Transmog;

    // Standalone-only: apply the user's scale override to the
    // font. Stacks on top of the init-time auto-DPI scale from
    // dx_overlay.cpp. Title bar catches up next frame (drawn in
    // the preceding Begin call); content reflects it immediately.
    if (s_standaloneMode)
        ImGui::GetIO().FontGlobalScale = s_uiScale;

    // Drive every slot's reinit state machine forward. Cheap when
    // every slot is Idle (just atomic loads).
    Transmog::ColorOverride::Reinit::tick();

    auto &pm = PresetManager::instance();
    // Lazy retry of populate_from_persisted for any active-preset
    // slot whose saved overrides failed to seed at load time (token
    // names not yet interned by the engine). No-op once every slot
    // has resolved; see PresetManager::reseed_unresolved_persisted_swatches.
    pm.reseed_unresolved_persisted_swatches();
    // When auto-apply is on, picks are applied immediately so
    // there's never a meaningful "pending" state.  Suppress the
    // badge and yellow button tint to reduce visual noise.
    const bool pending =
        !s_autoApply && has_pending_changes();
    const bool pendingSave = has_pending_save();

    draw_header(pending, pendingSave);
    draw_global_toggles();

    draw_character_picker(pm);

    draw_presets_section(pm);

    // --- Per-slot controls ---

    if (ImGui::CollapsingHeader("Slot Details", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto &mappings = slot_mappings();
        const auto &table = ItemNameTable::instance();
        const bool tableReady = table.ready();

        if (!tableReady)
            ui_text_disabled("Item catalog not ready -- hex entry only.");

        ui_text_disabled("Toggle which slots the next Apply All will touch.");

        {
            // "All" tracks ENABLED slots only -- disabled slots
            // (multi-prefab non-armor, duplicate-tag; see
            // slot_metadata.hpp `enabled` doc) are forced off in the
            // dispatcher, so including them in the "All" check would
            // make the box stay unticked forever.
            bool allActive = true;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
                if (!mappings[i].active)
                {
                    allActive = false;
                    break;
                }
            }
            if (ImGui::Checkbox("All", &allActive))
            {
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    if (!Transmog::slot_enabled(i))
                        continue;
                    mappings[i].active = allActive;
                }
                if (s_autoApply)
                {
                    flag_enabled().store(true,
                                         std::memory_order_relaxed);
                    manual_apply();
                }
            }
            if (!s_autoApply)
            {
                ImGui::SameLine();
                ui_text_disabled("(pending -- Apply All to commit)");
            }
        }

        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            // Skip disabled slots entirely -- the row, picker popup,
            // and label-sync logic below would all be misleading for
            // a slot that never reaches the dispatcher. The slot's
            // mapping is left as-is on disk so re-enabling later
            // restores prior selections without preset migration.
            if (!Transmog::slot_enabled(i))
                continue;

            auto &m = mappings[i];
            auto &ui = s_slotUI[i];
            const char *slotLabel = slot_name(static_cast<TransmogSlot>(i));

            // Lazy sync: when the PWS module has a target selection
            // for this slot but the UI label is empty, derive the
            // label from the catalog. Covers boot-time auto-apply
            // (preset's stored prefabName resolved by PWS after the
            // heap walk completes) and any other path that mutates
            // PWS selections without going through the picker.
            {
                namespace PWS = Transmog::PrefabWrapperSwap;
                const auto tslot = static_cast<TransmogSlot>(i);
                const int tgtIdx = PWS::selection_tgt_index(tslot);
                if (tgtIdx >= 0 && ui.pickedPrefabName.empty())
                {
                    const auto &cat = PWS::slot_catalog(tslot);
                    if (static_cast<std::size_t>(tgtIdx) < cat.size())
                        ui.pickedPrefabName = cat[tgtIdx].name;
                }
                else if (tgtIdx < 0 && !ui.pickedPrefabName.empty())
                {
                    ui.pickedPrefabName.clear();
                }
            }

            ImGui::PushID(static_cast<int>(i) + 100);

            if (ImGui::Checkbox(slotLabel, &m.active))
            {
                // Toggling active alone may not change the staged id
                // -- e.g. an active-none slot (active=true, targetId=0)
                // unticked back to inactive both yield staged=0, so
                // has_pending_changes would miss the toggle and the
                // Apply All button would stay grayed out. Set the
                // force-apply flag so the UI sees a pending change and
                // the dispatcher's slotNeedsWork picks the slot up
                // (its hasActiveNone path already handles the apply
                // semantics; the flag just guarantees the slot is
                // considered).
                force_apply_pending()[i] = true;
                if (s_autoApply)
                {
                    flag_enabled().store(true, std::memory_order_relaxed);
                    manual_apply_slot(i);
                }
            }


            // Slot-label column width: pick the longest slot name
            // ("TwoHandWeapon" plus a trailing pad) and measure it
            // with the live font. Content-derived rather than
            // DPI-linear so the column stays tight at 4K instead
            // of opening a 500px gap. Reshade mode keeps a fixed
            // 170px because its host UI applies its own scaling.
            const float slotColW = s_standaloneMode
                ? (ImGui::CalcTextSize("TwoHandWeapon  ").x
                   + ImGui::GetStyle().FramePadding.x * 2.0f)
                : 170.0f;
            ImGui::SameLine(slotColW);

            // --- Picker button ---
            {
                std::string pickerLabel;
                // When the user has picked a body-mesh prefab on this
                // slot, surface the prefab name on the button instead
                // of the carrier item's display name -- the carrier
                // is now an implementation detail (Kairos's gear
                // forced behind the scenes to feed the source wrapper).
                const bool showPrefabLabel = !ui.pickedPrefabName.empty();
                if (showPrefabLabel)
                {
                    pickerLabel = ui.pickedPrefabName + " [prefab]";
                }
                else if (m.targetItemId == 0)
                {
                    pickerLabel = "(none)";
                }
                else if (tableReady)
                {
                    auto internalName = table.name_of(m.targetItemId);
                    auto dispName = table.display_name_of(internalName);
                    if (!dispName.empty())
                        pickerLabel = dispName;
                    else if (!internalName.empty())
                        pickerLabel = internalName;
                    else
                        pickerLabel = "(unknown)";
                }
                else
                {
                    pickerLabel = "(catalog N/A)";
                }

                char btnLabel[192];
                if (showPrefabLabel)
                    std::snprintf(btnLabel, sizeof(btnLabel),
                                  "%s##pick", pickerLabel.c_str());
                else
                    std::snprintf(btnLabel, sizeof(btnLabel),
                                  "%s  [0x%04X]##pick", pickerLabel.c_str(),
                                  m.targetItemId);

                // Picker button width: ~32 glyphs at the live font
                // plus padding. Display names that exceed it are
                // truncated by ImGui's button text rendering. The
                // previous DPI-linear value (520 * scale) ballooned
                // to ~1360px at 4K.
                const float bw = s_standaloneMode
                    ? (ImGui::CalcTextSize("M").x * 32.0f
                       + ImGui::GetStyle().FramePadding.x * 2.0f)
                    : 380.0f;
                ImGui::SetNextItemWidth(bw);
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,
                                    ImVec2(0.02f, 0.5f));
                // Tint the button cyan when a body-mesh prefab is
                // picked so the override is visually distinct from
                // an ordinary carrier slot.
                if (showPrefabLabel)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.30f, 0.85f, 1.00f, 1.0f));
                }
                if (ImGui::Button(btnLabel, ImVec2(bw, 0.0f)))
                {
                    if (tableReady)
                    {
                        if (!s_keepSearchText)
                            ui.searchBuf[0] = '\0';
                        ImGui::OpenPopup("##slot_picker");
                    }
                }
                if (showPrefabLabel)
                    ImGui::PopStyleColor(); // Text
                ImGui::PopStyleVar(); // ButtonTextAlign

                // outPrefabIdx contract from the popup:
                //   -1: popup did NOT touch the body-mesh (real item
                //       or "(none) item" was picked) -- leave any
                //       prior body-mesh selection alone.
                //   -2: explicit "clear body-mesh override" pick.
                //   >=0: prefab N picked.
                int prefabIdx = -1;
                if (tableReady &&
                    draw_item_picker_popup(
                        "##slot_picker", ui,
                        static_cast<TransmogSlot>(i),
                        m.targetItemId,
                        s_autoApply, i,
                        &prefabIdx))
                {
                    std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "%04X",
                                  m.targetItemId);

                    namespace PWS = Transmog::PrefabWrapperSwap;
                    const auto tslot = static_cast<TransmogSlot>(i);
                    const int curSrc = PWS::selection_src_index(tslot);

                    if (prefabIdx >= 0 || prefabIdx == -2)
                    {
                        // Body-mesh interaction. Selection only --
                        // the swap map is rebuilt on the next apply
                        // via notify_apply_starting (apply-only
                        // lifecycle, mirroring the carrier pattern).
                        PWS::set_selection(
                            tslot, curSrc,
                            (prefabIdx >= 0) ? prefabIdx : -1);

                        // Update the per-slot UI label state. On a
                        // prefab pick (>= 0) we pull the name straight
                        // out of the catalog; on an explicit clear
                        // (-2) we drop the label so the row reverts
                        // to showing the carrier item.
                        if (prefabIdx >= 0)
                        {
                            const auto &cat = PWS::slot_catalog(tslot);
                            if (static_cast<std::size_t>(prefabIdx) <
                                cat.size())
                            {
                                ui.pickedPrefabName =
                                    cat[static_cast<std::size_t>(prefabIdx)]
                                        .name;
                            }
                            // Force the carrier into the ACTIVE
                            // CHARACTER's carrier so the source wrapper
                            // (e.g. cd_phw_00_ub_00_0163_index01 for
                            // Damiane chest) becomes resident and the
                            // swap-map hook can match it. Without this
                            // step, picking a prefab while controlling
                            // Wellsknight leaves the engine with
                            // Wellsknight wrappers and the hook never
                            // fires. (Used to be hardcoded to Kliff;
                            // now per-character so Damiane / Oongka get
                            // their own carriers, not Kliff's.)
                            force_active_character_carrier_for_picked_slots();
                        }
                        else
                        {
                            // Explicit body-mesh clear (-2): drop the
                            // label AND restore the slot's carrier to
                            // whatever the user had before the prefab
                            // pick auto-borrowed Kliff. Without this
                            // restore, the slot stays stuck on the
                            // Kliff plate carrier (e.g. 0x1521 for
                            // helm) instead of reverting to the user's
                            // underlying preset gear (e.g. Wellsknight
                            // 0x1520).
                            ui.pickedPrefabName.clear();
                            if (ui.priorCarrierSaved)
                            {
                                m.active = ui.priorCarrierActive;
                                m.targetItemId = ui.priorCarrierItemId;
                                ui.priorCarrierSaved = false;
                            }
                        }

                        // Set the force-apply flag UNCONDITIONALLY
                        // (regardless of s_autoApply). When the new
                        // carrier id equals the prior one (e.g.
                        // re-picking a body-mesh prefab on the same
                        // Kliff carrier 0x1521), staged/lastIds match,
                        // so without this flag has_pending_changes
                        // returns false -- the Apply All button would
                        // stay grayed out and a manual apply would
                        // hit the dispatcher's `targetId == prevId`
                        // early-out anyway. Setting the flag ensures
                        // (a) the Apply All button activates and
                        // (b) the dispatcher bypasses the early-out
                        // while leaving lastIds[i] intact, so Phase A
                        // `tear_down_fake` runs against 0x1521 and
                        // the engine's natural-pipeline hook
                        // substitutes src->tgt to clean up the prior
                        // body-mesh target wrapper. When ids differ
                        // (e.g. body-mesh clear that restored
                        // Wellsknight 0x1520 over the prior Kliff
                        // 0x1521), the dispatcher's own change
                        // detection covers it -- the flag is harmless.
                        if (last_applied_ids()[i] == m.targetItemId)
                            force_apply_pending()[i] = true;
                        if (s_autoApply)
                        {
                            flag_enabled().store(
                                true, std::memory_order_relaxed);
                            // Body-mesh picks require the natpipe-hook
                            // to substitute the source wrapper for the
                            // editing character's body. That hook reads
                            // s_swapMapPerChar[s_activeCharIdx-1] at
                            // entry, and only the full multi-actor apply
                            // path drives s_activeCharIdx per CCOIA
                            // (PresetManager::apply_to_state binds it
                            // for each character in the sweep). The
                            // single-slot path runs against the
                            // controlled actor only -- when the user
                            // edits Damiane/Oongka while controlling
                            // Kliff, the slot path byte-patches Kliff
                            // and never triggers a body re-bind for the
                            // editing character, so the substitution
                            // never fires and the carrier renders
                            // natural (e.g. Demenisian Uniform).
                            manual_apply();
                        }
                        DMK::Logger::get_instance().info(
                            "[picker] slot={} BODY-MESH "
                            "{}={} (srcSeeded={}) -- carrier=0x{:04X} "
                            "auto-applied",
                            slotLabel,
                            (prefabIdx >= 0) ? "prefabIdx" : "cleared",
                            (prefabIdx >= 0) ? prefabIdx : 0,
                            curSrc >= 0, m.targetItemId);
                    }
                    else
                    {
                        // Real item or "(none) item" pick. The carrier
                        // itemId was already updated by the popup. We
                        // also drop any prior body-mesh prefab label
                        // for this slot -- picking a real item is the
                        // user's signal to revert to plain carrier
                        // mode. The existing body-mesh swap selection
                        // is also cleared via reactivate so the two
                        // states stay mutually exclusive.
                        const bool hadPrior = !ui.pickedPrefabName.empty();
                        if (hadPrior)
                        {
                            ui.pickedPrefabName.clear();
                            PWS::set_selection(tslot, curSrc, -1);
                            // Drop the saved prior-carrier snapshot.
                            // The user explicitly picked a new real
                            // item, so the popup-set m.targetItemId is
                            // authoritative -- restoring the prior
                            // carrier here would clobber the fresh
                            // pick.
                            ui.priorCarrierSaved = false;
                        }
                        // Set the force-apply flag UNCONDITIONALLY so
                        // has_pending_changes also returns true when
                        // the user clears a prefab pick onto a real
                        // item that happens to share the prior
                        // auto-borrowed Kliff carrier id (e.g. helm
                        // 0x1521 → 0x1521). Without it the Apply All
                        // button would stay grayed out. Different-id
                        // case is covered by the dispatcher's normal
                        // change detection -- flag is harmless.
                        if (hadPrior &&
                            last_applied_ids()[i] == m.targetItemId)
                            force_apply_pending()[i] = true;
                        if (s_autoApply)
                        {
                            flag_enabled().store(
                                true, std::memory_order_relaxed);
                            manual_apply_slot(i);
                        }

                        const auto name = (m.targetItemId == 0)
                                              ? std::string("(none)")
                                              : table.name_of(m.targetItemId);
                        const auto cat =
                            (m.targetItemId == 0)
                                ? TransmogSlot::Count
                                : table.category_of(m.targetItemId);
                        const char *catName =
                            (m.targetItemId == 0)
                                ? "clear"
                            : (cat == static_cast<TransmogSlot>(i))
                                ? "slot-match"
                                : "slot-MISMATCH";
                        DMK::Logger::get_instance().info(
                            "[picker] slot={} id=0x{:04X} name='{}' "
                            "category={} ({}) -- pending Apply All",
                            slotLabel, m.targetItemId,
                            name.empty() ? "<unknown>" : name,
                            static_cast<int>(cat), catName);
                    }
                }
            }

            // --- Hex input ---
            ImGui::SameLine();

            if (!ui.editing)
                std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "%04X",
                              m.targetItemId);

            // When a body-mesh prefab is active on this slot the
            // carrier itemId is auto-borrowed (Kairos plate) and the
            // user shouldn't be editing it directly -- show the hex
            // disabled+dim with an explanatory tooltip so it's clear
            // the body-mesh prefab is the source of truth, and the
            // hex is a peek at the underlying carrier.
            const bool prefabActive = !ui.pickedPrefabName.empty();
            ImGui::BeginDisabled(prefabActive);
            // Tight fit for 4-hex-digit ids ("FFFF"): measure the
            // glyph run + frame padding + a small cursor margin.
            // Auto-adapts to font / DPI / style changes; the prior
            // fixed 64px was either clipping (at higher UI scale)
            // or wasting space (at default).
            {
                const float hexW =
                    ImGui::CalcTextSize("FFFF").x
                    + ImGui::GetStyle().FramePadding.x * 2.0f
                    + 4.0f;
                ImGui::SetNextItemWidth(hexW);
            }
            if (ImGui::InputText(
                    "##hex", ui.hexBuf, sizeof(ui.hexBuf),
                    ImGuiInputTextFlags_CharsHexadecimal |
                        ImGuiInputTextFlags_CharsUppercase, nullptr, nullptr))
            {
                unsigned long val = std::strtoul(ui.hexBuf, nullptr, 16);
                m.targetItemId = static_cast<uint16_t>(val);
            }
            ui.editing = !prefabActive && ImGui::IsItemActive();
            ImGui::EndDisabled();
            if (prefabActive && ImGui::IsItemHovered())
            {
                ui_tooltip(
                    "Auto-borrowed carrier id (Kairos plate). The "
                    "body-mesh prefab is the visual override; this "
                    "hex is just the engine plumbing. Clear the "
                    "prefab via the picker's '(none) prefab' to "
                    "edit the carrier directly.");
            }

            // Quick-clear button.
            ImGui::SameLine();
            if (ImGui::SmallButton("X##clr") && m.targetItemId != 0)
            {
                m.targetItemId = 0;
                std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "0000");
                // X clears both carrier and any body-mesh override on
                // this slot -- the user's intent is "remove everything
                // visible from this slot". Mirrors the (none) pick.
                if (!ui.pickedPrefabName.empty())
                {
                    namespace PWS = Transmog::PrefabWrapperSwap;
                    const auto tslot = static_cast<TransmogSlot>(i);
                    const int curSrc = PWS::selection_src_index(tslot);
                    ui.pickedPrefabName.clear();
                    PWS::set_selection(tslot, curSrc, -1);
                }
                if (s_autoApply)
                {
                    flag_enabled().store(true, std::memory_order_relaxed);
                    manual_apply_slot(i);
                }
            }

            // Cross-class limitation marker for weapon slots. Each
            // weapon prefab carries its own bone-binding + attachment-
            // socket data (which skeleton bone it parents to, which
            // socket transform it uses, hand offsets, sheath pose).
            // The engine resolves those bindings from the REAL equipped
            // item's class -- not from the LT swap target -- so a
            // mismatched-class transmog renders into the wrong socket
            // (or no socket) and goes invisible. Tooltip emphasises the
            // practical rule: match the prefab's class to whatever
            // weapon family the player has equipped right now.
            {
                const auto tslotMarker = static_cast<TransmogSlot>(i);
                const bool isWeaponSlot =
                    tslotMarker == TransmogSlot::MainHand ||
                    tslotMarker == TransmogSlot::OffHand ||
                    tslotMarker == TransmogSlot::Ranged ||
                    tslotMarker == TransmogSlot::SubWeapon ||
                    tslotMarker == TransmogSlot::TwoHandWeapon;
                if (isWeaponSlot)
                {
                    ImGui::SameLine();
                    ui_text_disabled("(?)");
                    if (ImGui::IsItemHovered())
                    {
                        const bool isTwoHand =
                            tslotMarker == TransmogSlot::TwoHandWeapon;
                        ui_tooltip(
                            isTwoHand
                            ? "EXPERIMENTAL.\n"
                              "\n"
                              "Heads-up: in this game, tools (pickaxe,\n"
                              "mallet, etc.) share the same engine slot\n"
                              "as 2H weapons. Setting a transmog here\n"
                              "may also affect the tool you have\n"
                              "equipped. Investigation ongoing.\n"
                              "\n"
                              "Best practice: pick a transmog from the\n"
                              "SAME weapon family you have equipped.\n"
                              "Sword on a sword, spear on a spear, 1H\n"
                              "on a 1H, 2H on a 2H -- always renders\n"
                              "correctly, drawn or sheathed.\n"
                              "\n"
                              "Why the limit exists:\n"
                              "Each weapon prefab carries its own\n"
                              "bone-binding and attachment-socket data\n"
                              "(which bone it parents to, which socket\n"
                              "transform it uses, hand offsets, sheath\n"
                              "pose). The engine looks those bindings\n"
                              "up from the REAL equipped item's class --\n"
                              "not from this transmog target -- so a\n"
                              "mismatched-class swap lands in the wrong\n"
                              "socket and goes invisible.\n"
                              "\n"
                              "Examples:\n"
                              " - 2H prefab while a 1H is equipped:\n"
                              "   mesh shows on the back; the real 1H\n"
                              "   goes invisible when drawn.\n"
                              " - Spear/hammer prefab while a sword is\n"
                              "   equipped: invisible when drawn\n"
                              "   (sheathed on back still renders).\n"
                              " - Same family (sword<->sword, etc.):\n"
                              "   renders correctly in every pose."
                            : "EXPERIMENTAL.\n"
                              "\n"
                              "Best practice: pick a transmog from the\n"
                              "SAME weapon family you have equipped.\n"
                              "Sword on a sword, spear on a spear, 1H\n"
                              "on a 1H, 2H on a 2H -- always renders\n"
                              "correctly, drawn or sheathed.\n"
                              "\n"
                              "Why the limit exists:\n"
                              "Each weapon prefab carries its own\n"
                              "bone-binding and attachment-socket data\n"
                              "(which bone it parents to, which socket\n"
                              "transform it uses, hand offsets, sheath\n"
                              "pose). The engine looks those bindings\n"
                              "up from the REAL equipped item's class --\n"
                              "not from this transmog target -- so a\n"
                              "mismatched-class swap lands in the wrong\n"
                              "socket and goes invisible.\n"
                              "\n"
                              "Examples:\n"
                              " - 2H prefab while a 1H is equipped:\n"
                              "   mesh shows on the back; the real 1H\n"
                              "   goes invisible when drawn.\n"
                              " - Spear/hammer prefab while a sword is\n"
                              "   equipped: invisible when drawn\n"
                              "   (sheathed on back still renders).\n"
                              " - Same family (sword<->sword, etc.):\n"
                              "   renders correctly in every pose.");
                    }
                }
            }

            // Lantern "(none)" gotcha. Unlike armor, the Lantern slot
            // controls a light source on the engine side, not just a
            // visual. Clearing it to (none) doesn't merely hide the
            // mesh -- the equipped item's light emission goes with
            // it, leaving the player without any handheld light.
            // Mirrors the weapon-slot (?) marker pattern.
            {
                const auto tslotL = static_cast<TransmogSlot>(i);
                if (tslotL == TransmogSlot::Lantern
                    && m.targetItemId == 0)
                {
                    ImGui::SameLine();
                    ui_text_colored(
                        ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                        "(!)");
                    if (ImGui::IsItemHovered())
                    {
                        ui_tooltip(
                            "Heads-up: Lantern set to (none) does\n"
                            "MORE than hide the mesh -- the engine\n"
                            "removes the light source too. Your\n"
                            "character will emit NO light, even at\n"
                            "night or in dungeons.\n"
                            "\n"
                            "If you wanted to hide the lantern\n"
                            "visually but keep the light, pick a\n"
                            "different lantern prefab here instead\n"
                            "of clearing the slot.");
                    }
                }
            }

            // --- Dye picker (per-slot per-mod color override) ---
            //
            // Dye record bytes +7/+8/+9 are
            // literal R/G/B of the picked shade. Each slot has up to
            // 16 ARMOR_MOD records (channels). The popup lists all
            // 16 mods as inline-expandable rows; expanding a row
            // reveals the color picker + repair slider for that
            // mod alone. Selection writes to the active preset's
            // PresetSlot::dye[idx]; the dispatch loop reads the
            // 16-element array each apply.
            //
            // Only shown for armor slots (Helm/Chest/Cloak/Gloves/
            // Boots). Weapons + accessories don't go through the
            // ARMOR_MOD pipeline so the picker has nothing to drive.
            draw_dye_popup(i);

            ImGui::PopID();
        }
    }

    ImGui::Separator();

    draw_action_buttons(pending, pendingSave, pm);
    draw_status_footer();
}

} // anonymous namespace

// --- Public entry points ------------------------------------------------

void draw_overlay()
{
    // Auto-fit window so 4K screens don't crop content on first open.
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(k_windowTitle))
    {
        ImGui::End();
        return;
    }
    s_standaloneMode = true;
    draw_overlay_content();
    ImGui::End();
}

// --- ReShade addon path -------------------------------------------------

static HMODULE s_reshadeModule = nullptr;
static bool s_reshadeActive = false;

static void draw_reshade_overlay(reshade::api::effect_runtime *)
{
    // Drawn directly inside the ReShade addon tab; no Begin/End wrapper.
    draw_overlay_content();
}

bool init_reshade_overlay(HMODULE hModule)
{
    if (!reshade::register_addon(hModule))
        return false;
    reshade::register_overlay("Transmog", &draw_reshade_overlay);
    s_reshadeModule = hModule;
    s_reshadeActive = true;
    return true;
}

void shutdown_reshade_overlay()
{
    if (!s_reshadeActive)
        return;
    reshade::unregister_overlay("Transmog", &draw_reshade_overlay);
    reshade::unregister_addon(s_reshadeModule);
    s_reshadeActive = false;
    s_reshadeModule = nullptr;
}

bool is_reshade_overlay_active() { return s_reshadeActive; }

} // namespace Transmog
