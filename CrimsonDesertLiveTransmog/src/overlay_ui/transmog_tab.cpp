// overlay_ui/transmog_tab.cpp
//
// Top-of-Transmog-tab UI: header, global toggles, character picker,
// presets section.  Each function corresponds to one depth-1 banner
// in draw_overlay_content and ends with the trailing separator that
// closes its block.

#include "overlay_ui/transmog_tab.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"

#include "constants.hpp"
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
#include <cstdio>
#include <string>

namespace Transmog
{

void draw_header(bool pending, bool pendingSave)
{
    ImGui::TextUnformatted(MOD_NAME);
    ImGui::SameLine();
    ui_text_disabled("v%s", MOD_VERSION);

    // Standalone-only UI scale combo. Stacks on the init-time
    // auto-DPI baseline. Session-only -- not persisted.
    if (s_standaloneMode)
    {
        static constexpr float kScales[] = {
            0.5f, 0.75f, 0.85f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
        static constexpr const char *kLabels[] = {
            "0.5x", "0.75x", "0.85x", "1.0x",
            "1.25x", "1.5x", "1.75x", "2.0x" };
        constexpr int kCount =
            static_cast<int>(sizeof(kScales) / sizeof(kScales[0]));
        int sel = 3; // default points at 1.0x
        for (int i = 0; i < kCount; ++i)
            if (s_uiScale == kScales[i]) { sel = i; break; }
        // Separate from the version/mod-title group with a gap +
        // visible "UI Scale" label so users discover the control.
        // Combo width must accommodate the longest label ("1.75x")
        // plus the arrow glyph; scales with font size so the value
        // text isn't clipped at any UI-scale step.
        ImGui::SameLine(0.0f, 24.0f);
        ui_text("UI Scale:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::Combo("##uiScale", &sel, kLabels, kCount, -1))
            s_uiScale = kScales[sel];
        if (ImGui::IsItemHovered())
            ui_tooltip(
                "UI scale (standalone overlay).\n"
                "Stacks on top of the auto-resolution scale.\n"
                "Glyphs may blur at higher values.");
    }

    if (pending)
    {
        ImGui::SameLine();
        ui_text_colored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                        "  [PENDING -- click Apply All]");
    }

    if (pendingSave)
    {
        ImGui::SameLine();
        ui_text_colored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                        "  [UNSAVED -- click Save]");
    }

    ImGui::Separator();
}

void draw_global_toggles()
{
    bool enabled = flag_enabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enabled", &enabled))
    {
        flag_enabled().store(enabled, std::memory_order_relaxed);
        if (enabled)
            manual_apply();
        else
            manual_clear();
    }

    ImGui::Checkbox("Instant Apply", &s_autoApply);
    if (ImGui::IsItemHovered())
        ui_tooltip("Apply changes immediately on hover, "
                   "pick, toggle, and clear -- no Apply All "
                   "needed");
    ImGui::SameLine();
    ui_text_disabled("(?)");
    if (ImGui::IsItemHovered())
        ui_tooltip("Prefab picker (Prefabs checkbox inside the popup) "
                   "does NOT honour Instant Apply this release. Click "
                   "a prefab row to commit it; hover-preview is items-"
                   "only for now.");

    ImGui::SameLine();
    ImGui::Checkbox("Keep Search Text", &s_keepSearchText);
    if (ImGui::IsItemHovered())
        ui_tooltip("Preserve the search field when "
                   "re-opening a slot picker");

    ImGui::Separator();
}

void draw_character_picker(PresetManager &pm)
{
    // Presets are stored per character (Kliff / Damiane / Oongka).
    // Selecting a character swaps the active preset list to that
    // character's, re-applies its active preset, and clears the
    // drop-detection state so the next apply pass does not confuse
    // the previous character's cached itemIds with the new one's.
    //
    // Auto-detection of the currently-controlled character from a1
    // is deferred: the type-byte at *(actor+0x88)+1 is role-based
    // (controlled vs background), not character-based, and the
    // static WorldSystem -> ActorManager -> UserActor chain did not
    // update live on control-swap in our 1.03.01 probe. Until that
    // RE lands, the user picks the character manually here.

    // Fixed stack buffer -- the game has a small, closed roster of
    // playable characters (Kliff / Damiane / Oongka at time of
    // writing). k_maxChars is oversized so adding a new playable
    // character later doesn't need a code change here beyond
    // adding its preset in the JSON.
    constexpr std::size_t k_maxChars = 8;
    const auto names = pm.character_names();
    const std::size_t n =
        (names.size() < k_maxChars) ? names.size() : k_maxChars;
    if (n > 0)
    {
        const char *cstrs[k_maxChars]{};
        int selectedIdx = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            cstrs[i] = names[i].c_str();
            if (names[i] == pm.editing_character())
                selectedIdx = static_cast<int>(i);
        }

        // Dropdown writes the editing character only. The pin
        // engages automatically when editing differs from
        // controlled; picking the controlled character clears
        // the pin. Apply is cross-body when pinned: the
        // controlled body wears the editing character's preset
        // items via PWS / carrier substitution. Gendered or
        // character-specific items may not have a renderable
        // variant on the controlled body and silently no-op.
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Character##char_picker",
                         &selectedIdx,
                         cstrs,
                         static_cast<int>(n), -1))
        {
            const auto &pick =
                names[static_cast<std::size_t>(selectedIdx)];
            if (pick != pm.editing_character())
            {
                pm.set_editing_character(pick);
                for (auto &m : slot_mappings())
                {
                    m.active = false;
                    m.targetItemId = 0;
                }
                pm.apply_to_state();
                last_applied_ids().fill(0);
                real_damaged().fill(false);
                last_applied_real_ids().fill(0);
                last_applied_carrier_ids().fill(0);
                manual_apply();
                pm.save();
            }
        }
        if (ImGui::IsItemHovered())
        {
            if (pm.editing_pinned())
                ui_tooltip("Editing pinned: this character's preset is "
                           "loaded into the slot rows, but the body on "
                           "screen is whoever you control in-game. "
                           "Cross-body apply -- some items (gender-"
                           "specific, weapons) may not render. Pick "
                           "the controlled character to unpin.");
            else
                ui_tooltip("Pick a different character to edit their "
                           "preset while controlling someone else. "
                           "The controlled body will wear the picked "
                           "character's preset items (cross-body "
                           "apply -- partial coverage expected).");
        }
        if (pm.editing_pinned())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Unpin##char_unpin"))
            {
                pm.clear_editing_pin();
                for (auto &m : slot_mappings())
                {
                    m.active = false;
                    m.targetItemId = 0;
                }
                pm.apply_to_state();
                last_applied_ids().fill(0);
                real_damaged().fill(false);
                last_applied_real_ids().fill(0);
                last_applied_carrier_ids().fill(0);
                manual_apply();
                pm.save();
            }
            if (ImGui::IsItemHovered())
                ui_tooltip("Drop the editing pin and follow the "
                           "controlled character again.");
        }

        // Body-kind override. Lets body-swap mod users mark e.g.
        // Kliff as Female so the picker shows the female-body-
        // token pool. "Auto" defers to the hardcoded default
        // (Kliff/Oongka = Male, Damiane = Female).
        //
        // Saved per character in presets.json. Only affects the
        // picker filter -- no effect on apply / render behaviour.
        static constexpr const char *k_bodyItems[] = {
            "Auto", "Male", "Female", "Both",
        };
        constexpr int k_bodyCount =
            static_cast<int>(sizeof(k_bodyItems) / sizeof(k_bodyItems[0]));

        const std::string currentBody =
            pm.body_kind_of(pm.editing_character());
        int bodyIdx = 0;
        for (int i = 0; i < k_bodyCount; ++i)
        {
            if (currentBody == k_bodyItems[i])
            {
                bodyIdx = i;
                break;
            }
        }
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("Body##body_kind",
                         &bodyIdx,
                         k_bodyItems,
                         k_bodyCount, -1))
        {
            pm.set_body_kind_of(pm.editing_character(),
                                k_bodyItems[bodyIdx]);
        }
        if (ImGui::IsItemHovered())
            ui_tooltip("Override the body type used for picker "
                       "filtering. Use if you run a body-swap "
                       "mod that changes this character's "
                       "skeleton (e.g. Kliff -> Female).");
    }

    ImGui::Separator();
}

void draw_presets_section(PresetManager &pm)
{
    if (!ImGui::CollapsingHeader("Presets",
                                 ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Separator();
        return;
    }

    auto &presetList = pm.presets();
    const int activeIdx = pm.active_preset_index();
    const int count = pm.preset_count();

    for (int i = 0; i < count; ++i)
    {
        ImGui::PushID(i);

        const bool isActive = (i == activeIdx);

        if (s_renameActive && s_renameIndex == i)
        {
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputText("##rename", s_renamePresetBuf,
                                 sizeof(s_renamePresetBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue,
                                 nullptr, nullptr))
            {
                pm.set_active_preset(i);
                if (auto *p = pm.active_preset_mut())
                    p->name = s_renamePresetBuf;
                pm.set_active_preset(activeIdx);
                pm.save();
                s_renameActive = false;
                s_renameIndex = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("OK", ImVec2(0, 0)))
            {
                pm.set_active_preset(i);
                if (auto *p = pm.active_preset_mut())
                    p->name = s_renamePresetBuf;
                pm.set_active_preset(activeIdx);
                pm.save();
                s_renameActive = false;
                s_renameIndex = -1;
            }
        }
        else
        {
            char label[128];
            std::snprintf(label, sizeof(label), "%s [%d]##preset",
                          presetList[static_cast<std::size_t>(i)]
                              .name.c_str(),
                          i);

            if (ImGui::Selectable(label, isActive, 0, ImVec2(0, 0)))
            {
                // Tear down any active body-mesh prefab picks
                // BEFORE switching presets. Otherwise the hook
                // keeps substituting the old src wrappers while
                // the new preset's items are being equipped, which
                // produces stale visuals across the transition.
                const auto hadPick =
                    clear_all_picked_prefabs_and_deactivate();
                pm.set_active_preset(i);
                pm.apply_to_state();
                // Post-apply_to_state reconciliation: for each
                // slot where a body-mesh pick was just cleared AND
                // the new preset's carrier equals last_applied
                // (the early-out case), set the force-apply flag
                // so the dispatcher bypasses the `targetId ==
                // prevId` early-out while leaving lastIds intact.
                // Phase A `tear_down_fake` then runs against the
                // prior carrier and the natpipe-hook cleans up the
                // prior body-mesh tgt wrapper. When carriers
                // differ the regular tear_down_fake path already
                // handles cleanup naturally, so the flag is
                // harmless.
                auto &lastIds = last_applied_ids();
                auto &mods = slot_mappings();
                for (std::size_t s = 0; s < k_slotCount; ++s)
                {
                    if (hadPick[s]
                        && mods[s].targetItemId == lastIds[s])
                    {
                        force_apply_pending()[s] = true;
                    }
                }
                manual_apply();
                pm.save();
            }

            if (ImGui::IsItemHovered()
                && ImGui::IsMouseDoubleClicked(0))
            {
                s_renameActive = true;
                s_renameIndex = i;
                std::snprintf(s_renamePresetBuf,
                              sizeof(s_renamePresetBuf), "%s",
                              presetList[static_cast<std::size_t>(i)]
                                  .name.c_str());
            }
        }

        ImGui::PopID();
    }

    if (count == 0)
        ui_text_disabled("No presets -- use Append to create one");

    ImGui::Spacing();

    if (ImGui::Button("Append", ImVec2(0, 0)))
    {
        pm.append_from_state();
        manual_apply();
    }
    if (ImGui::IsItemHovered())
        ui_tooltip("Append a new preset with every slot ticked + "
                   "none (hides all armor), then apply it.");

    ImGui::SameLine();

    // Copy forks the active preset's saved state into a brand-new
    // preset, ignoring any unsaved edits in slot_mappings. Use to
    // get a clean clone to start altering on. For forking the
    // current pending edits, use "Save as New" instead.
    if (ImGui::Button("Copy", ImVec2(0, 0)))
    {
        pm.duplicate_current();
        manual_apply();
    }
    if (ImGui::IsItemHovered())
        ui_tooltip("Clone the active preset's saved state into a "
                   "new preset (pending edits are discarded). Use "
                   "to start altering a clean copy without touching "
                   "the source.");

    ImGui::SameLine();

    // Save as New forks the current pending state (slot_mappings +
    // in-place dye/swatch) into a new preset, leaving the active
    // preset's saved rows untouched. Lets the user alter freely
    // mid-edit and bottle the result up without overwriting.
    if (ImGui::Button("Save as New", ImVec2(0, 0)))
    {
        pm.save_as_new_from_state();
        manual_apply();
    }
    if (ImGui::IsItemHovered())
        ui_tooltip("Save the current pending state (including "
                   "unsaved picks) as a new preset. The active "
                   "preset's saved rows are left unchanged.");

    ImGui::SameLine();

    if (ImGui::Button("Remove", ImVec2(0, 0)) && count > 0)
    {
        pm.remove_current();
        // remove_current auto-selects a neighbouring preset.
        // Apply it so the visible outfit matches the new active
        // preset instead of leaving the old (now-deleted) one on
        // screen.
        if (pm.preset_count() > 0)
            manual_apply();
        else
            manual_clear();
    }

    ImGui::SameLine();

    if (ImGui::Button("Prev", ImVec2(0, 0)) && count > 1)
    {
        pm.prev_preset();
        manual_apply();
    }

    ImGui::SameLine();

    if (ImGui::Button("Next", ImVec2(0, 0)) && count > 1)
    {
        pm.next_preset();
        manual_apply();
    }

    if (count > 0)
        ui_text("Active: %d / %d", activeIdx + 1, count);

    ImGui::Separator();
}

} // namespace Transmog
