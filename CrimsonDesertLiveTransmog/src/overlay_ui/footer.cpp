// overlay_ui/footer.cpp
//
// Bottom-of-Transmog-tab UI: action buttons row + status footer. Lives at draw_overlay_content depth 1; closures
// captured via the caller-supplied (pending, pending_save, PresetManager) tuple.

#include "overlay_ui/footer.hpp"
#include "overlay_ui/helpers.hpp"
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
        if (ImGui::Button(pending ? "Apply All *" : "Apply All", ImVec2(0, 0)))
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
            ui_text_colored(ImVec4(0.85f, 0.75f, 0.20f, 1.0f), "(loading body-mesh catalog...)");
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear All", ImVec2(0, 0)))
        {
            flag_enabled().store(false, std::memory_order_relaxed);
            manual_clear();
        }

        ImGui::SameLine();

        if (ImGui::Button("Capture Outfit", ImVec2(0, 0)))
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
        if (ImGui::Button(pending_save ? "Save *" : "Save", ImVec2(0, 0)))
        {
            pm.replace_current_from_state();
            pm.save();
        }
        if (pending_save)
            ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ui_tooltip(
                pending_save ? "Commit current slot rows into the active preset (unsaved edits pending)."
                             : "Commit current slot rows into the active preset."
            );

        ImGui::EndDisabled();

        if (!world_ready)
        {
            ImGui::SameLine();
            ui_text_disabled("(waiting for world load)");
        }
    }

    void draw_status_footer()
    {
        ImGui::Separator();
        ui_text_disabled("Status");

        if (slot_populator_fn())
            ui_text_colored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "SlotPopulator: READY");
        else
            ui_text_colored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "SlotPopulator: UNAVAILABLE");

        auto &mappings = slot_mappings();
        int active_count = 0;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (mappings[i].active && mappings[i].target_item_id != 0)
                ++active_count;
        }

        ui_text("Active slots: %d / %zu", active_count, SLOT_COUNT);
    }

} // namespace Transmog
