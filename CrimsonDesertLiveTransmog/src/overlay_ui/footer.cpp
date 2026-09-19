// overlay_ui/footer.cpp
//
// Bottom-of-Transmog-tab UI: action buttons row + status footer. Lives at draw_overlay_content depth 1; closures
// captured via the caller-supplied (pending, pending_save, PresetManager) tuple.

#include "overlay_ui/footer.hpp"
#include "overlay_ui/helpers.hpp"

#include "lang.hpp"
#include "prefab_wrapper_swap.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <atomic>
#include <cstddef>

namespace Transmog
{

    void draw_action_buttons(bool pending, bool pending_save, PresetManager &pm)
    {
        // Gate on WorldSystem so we do not spam "player not found" before the first world load.
        const bool world_ready = Transmog::is_world_ready();
        ImGui::BeginDisabled(!world_ready);

        if (pending)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.55f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.65f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.72f, 0.20f, 1.0f));
        }
        // Body-mesh catalog is populated asynchronously by a boot thread (heap walk ~1-5s). If the user has any
        // body-mesh selection while the catalog is still loading, block Apply - the swap map cannot resolve source
        // wrappers without the catalog and an apply would silently produce a no-op (or worse, partial substitution).
        namespace pws = Transmog::prefab_wrapper_swap;
        const bool catalog_loading = pws::has_any_selection() && !pws::is_catalog_populated();

        ImGui::BeginDisabled(catalog_loading);
        const char *apply_label =
            pending ? lang::t("footer.apply_all.pending", "Apply All *") : lang::t("footer.apply_all", "Apply All");
        if (ImGui::Button(apply_label, ImVec2(0, 0)))
        {
            flag_enabled().store(true, std::memory_order_relaxed);
            manual_apply();
        }
        ImGui::EndDisabled();
        if (pending)
            ImGui::PopStyleColor(3);

        if (catalog_loading)
        {
            ImGui::SameLine();
            ui_text_colored(
                ImVec4(0.85f, 0.75f, 0.20f, 1.0f),
                lang::t("footer.catalog_loading", "(loading body-mesh catalog...)")
            );
        }

        ImGui::SameLine();

        if (ImGui::Button(lang::t("footer.clear_all", "Clear All"), ImVec2(0, 0)))
        {
            flag_enabled().store(false, std::memory_order_relaxed);
            manual_clear();
        }

        ImGui::SameLine();

        if (ImGui::Button(lang::t("footer.capture_outfit", "Capture Outfit"), ImVec2(0, 0)))
        {
            // Capture replaces the current state with the live equipped outfit, so any session-only prefab picks must
            // surrender too - otherwise the cyan label hides the captured gear and a later "(none) prefab" would
            // restore the stale pre-capture carrier from `prior_carrier_item_id`. Clears pws + s_slot_ui state in one
            // shot; capture_outfit then writes fresh mappings.
            clear_all_picked_prefabs_and_deactivate();
            capture_outfit();
        }

        ImGui::SameLine();

        if (pending_save)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.40f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.52f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.60f, 0.25f, 1.0f));
        }
        const char *save_label =
            pending_save ? lang::t("footer.save.pending", "Save *") : lang::t("footer.save", "Save");
        if (ImGui::Button(save_label, ImVec2(0, 0)))
        {
            pm.replace_current_from_state();
            pm.save();
        }
        if (pending_save)
            ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ui_tooltip(
                pending_save ? lang::t(
                                   "footer.save.tip_pending",
                                   "Commit current slot rows into the active preset (unsaved edits pending)."
                               )
                             : lang::t("footer.save.tip", "Commit current slot rows into the active preset.")
            );

        ImGui::EndDisabled();

        if (!world_ready)
        {
            ImGui::SameLine();
            ui_text_disabled(lang::t("footer.waiting_world", "(waiting for world load)"));
        }
    }

    void draw_status_footer()
    {
        // Report the abnormal case only. Without SlotPopulator nothing can apply at all and every button above
        // fails silently, which is the one fact the rows themselves do not already show. A healthy session renders
        // nothing here rather than spending a row per frame to say so.
        if (slot_populator_fn())
            return;

        ImGui::Separator();
        ui_text_colored(
            ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
            lang::t(
                "footer.populator_unavailable",
                "SlotPopulator unavailable - transmog cannot be applied on this game build"
            )
        );
    }

} // namespace Transmog
