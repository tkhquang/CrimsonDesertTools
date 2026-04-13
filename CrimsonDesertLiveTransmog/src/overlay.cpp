#include "overlay.hpp"
#include "constants.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade.hpp>
#pragma warning(pop)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace Transmog
{
    static HMODULE s_overlayModule = nullptr;
    static bool s_overlayRegistered = false;

    // --- Per-slot UI state ---

    struct SlotUIState
    {
        char hexBuf[8]{"0000"};
        bool editing = false;
        char searchBuf[64]{};
        // "Exact" means: only list items whose auto-detected category
        // matches THIS slot. Defaults to ON so the dropdown shows ~350
        // relevant items instead of all 6024.
        bool exactFilter = true;
        bool hideIncompatible = true;   // hide crash-risk + non-equipment
        bool hideVariants = false; // hide NPC variants (carrier items)
    };

    static SlotUIState s_slotUI[k_slotCount]{};

    // --- Item picker helpers ---

    // Case-insensitive substring search. Returns true if `needle` is empty
    // or found anywhere in `hay`.
    static bool name_contains_ci(const std::string &hay, const char *needle) noexcept
    {
        if (!needle || needle[0] == '\0')
            return true;
        const auto nlen = std::strlen(needle);
        if (nlen > hay.size())
            return false;
        for (std::size_t i = 0; i + nlen <= hay.size(); ++i)
        {
            std::size_t k = 0;
            for (; k < nlen; ++k)
            {
                const auto a = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(hay[i + k])));
                const auto b = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(needle[k])));
                if (a != b)
                    break;
            }
            if (k == nlen)
                return true;
        }
        return false;
    }

    // Draw a search-filterable picker popup for one slot. Returns true if
    // the user committed a selection this frame (caller is responsible for
    // persisting it).
    static bool draw_item_picker_popup(const char *popupId,
                                       SlotUIState &ui,
                                       TransmogSlot slotCategory,
                                       uint16_t &targetItemId)
    {
        bool committed = false;
        if (!ImGui::BeginPopup(popupId))
            return false;

        ImGui::TextDisabled("Item picker");
        ImGui::Separator();

        // --- Filter toggles ---
        //
        // Exact: only show items whose auto-detected category matches
        //        this slot (helm-suffixed items for the helm slot, etc.)
        // Hide variants: hide items with variant metadata at desc+0x3A0;
        //        all tested samples in that bucket failed to render via
        //        runtime transmog, so hiding them by default keeps the
        //        picker free of non-functional items.
        ImGui::Checkbox("Exact", &ui.exactFilter);
        ImGui::SameLine();
        ImGui::Checkbox("Safe only", &ui.hideIncompatible);
        ImGui::SameLine();
        ImGui::Checkbox("Hide variants", &ui.hideVariants);

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##search", "search by name...",
                                 ui.searchBuf, sizeof(ui.searchBuf));

        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##search"))
            ui.searchBuf[0] = '\0';

        const auto &table = ItemNameTable::instance();
        const auto &entries = table.sorted_entries();

        // Fixed-height scrollable region so the popup doesn't resize per
        // keystroke as the filter narrows.
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##itemlist",
                          ImVec2(320.0f, rowH * 14.0f),
                          true);

        // "None" / clear entry — maps to id 0 so the user can disable a
        // slot without typing hex.
        {
            const bool selected = (targetItemId == 0);
            if (ImGui::Selectable("(none -- id 0)##picker_none", selected))
            {
                targetItemId = 0;
                committed = true;
                ImGui::CloseCurrentPopup();
            }
        }

        int shown = 0;
        int filteredByCategory = 0;
        int filteredByUnsafe = 0;
        constexpr int k_maxShown = 512;
        for (const auto &e : entries)
        {
            if (ui.exactFilter && e.category != slotCategory)
            {
                ++filteredByCategory;
                continue;
            }
            const bool nonEquipment =
                (e.category == TransmogSlot::Count);
            const bool incompatible =
                (!e.isPlayerCompatible && !e.hasVariantMeta) ||
                nonEquipment;
            const bool npcVariant = e.hasVariantMeta;
            if (ui.hideIncompatible && incompatible)
            {
                ++filteredByUnsafe;
                continue;
            }
            if (ui.hideVariants && npcVariant)
            {
                ++filteredByUnsafe;
                continue;
            }
            if (!name_contains_ci(e.name, ui.searchBuf))
                continue;
            if (shown >= k_maxShown)
                break;

            // Tag items with visible badges:
            //   - "CRASH RISK"  -> !isPlayerCompatible (red)
            //   - "carrier"     -> hasVariantMeta, rendered via carrier
            //                      + char-class bypass (cyan)
            char label[200];
            const char *tag = nullptr;
            // NPC variant items (hasVariantMeta) are humanoid and now
            // render via carrier + char-class bypass. True crash risks
            // are non-player items WITHOUT variant meta (horse tack, etc).
            const bool usesCarrier = e.hasVariantMeta;
            const bool crashRisk =
                !e.isPlayerCompatible && !e.hasVariantMeta;
            if (crashRisk)
                tag = "non-player -- CRASH RISK";
            else if (usesCarrier)
                tag = "carrier";

            if (tag)
                std::snprintf(label, sizeof(label),
                              "%s  [0x%04X]  (%s)##picker",
                              e.name.c_str(), e.id, tag);
            else
                std::snprintf(label, sizeof(label), "%s  [0x%04X]##picker",
                              e.name.c_str(), e.id);

            if (crashRisk)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            else if (usesCarrier)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.5f, 0.85f, 1.0f, 1.0f));

            const bool selected = (targetItemId == e.id);
            if (ImGui::Selectable(label, selected))
            {
                targetItemId = e.id;
                committed = true;
                ImGui::CloseCurrentPopup();
            }

            if (crashRisk || usesCarrier)
                ImGui::PopStyleColor();

            ++shown;
        }

        if (shown == 0)
        {
            if (ui.exactFilter && filteredByCategory > 0)
                ImGui::TextDisabled("no matches in this category -- "
                                    "uncheck Exact to widen");
            else if ((ui.hideIncompatible || ui.hideVariants) &&
                     filteredByUnsafe > 0)
                ImGui::TextDisabled("no matches -- uncheck filters "
                                    "to show more items");
            else
                ImGui::TextDisabled("no matches");
        }
        else if (shown >= k_maxShown)
        {
            ImGui::TextDisabled("(truncated -- narrow your search)");
        }

        ImGui::EndChild();
        ImGui::EndPopup();

        return committed;
    }

    // --- Preset UI state ---

    static char s_newCharName[64]{};
    static char s_renamePresetBuf[64]{};
    static bool s_renameActive = false;
    static int s_renameIndex = -1;

    // --- Overlay draw callback ---

    // Compare staged slot_mappings vs last_applied_ids to detect unsaved
    // picker/checkbox edits. Returns true if ANY slot's effective target
    // (targetItemId when active, else 0) differs from what's currently
    // committed to the game. Lets the overlay show a "[PENDING]" badge
    // so users don't close the GUI and lose track of their edits.
    static bool has_pending_changes() noexcept
    {
        const auto &mappings = slot_mappings();
        const auto &lastIds = last_applied_ids();
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const uint16_t staged =
                mappings[i].active ? mappings[i].targetItemId : uint16_t{0};
            if (staged != lastIds[i])
                return true;
        }
        return false;
    }

    static void draw_overlay(reshade::api::effect_runtime *)
    {
        auto &pm = PresetManager::instance();
        const bool pending = has_pending_changes();

        // --- Header ---

        ImGui::TextUnformatted(MOD_NAME);
        ImGui::SameLine();
        ImGui::TextDisabled("v%s", MOD_VERSION);

        // Pending-changes badge: loud yellow text in the header so it's
        // impossible to miss even if the user scrolls past the action
        // buttons or closes the menu mid-edit.
        if (pending)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                               "  [PENDING -- click Apply All]");
        }

        ImGui::Separator();

        // --- Global toggles ---

        // Enabled acts like the Numpad5 toggle hotkey: flipping the
        // checkbox off immediately tears down any active transmog and
        // restores real equipment; flipping it on re-applies the current
        // slot mappings. Before this fix the checkbox only stopped FUTURE
        // applies and left any currently-drawn fakes on the player.
        bool enabled = flag_enabled().load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Enabled", &enabled))
        {
            flag_enabled().store(enabled, std::memory_order_relaxed);
            if (enabled)
                manual_apply();
            else
                manual_clear();
        }

        // --- Player Only / Character selector ---
        //
        // Hidden for the v1 ship: this release targets Kliff only and
        // the player-only / multi-character UI confuses users into
        // thinking cross-character transmog is supported. Preserved as
        // comments so a future multi-char build can re-enable them.
        //
        // bool playerOnly = flag_player_only().load(std::memory_order_relaxed);
        // if (ImGui::Checkbox("Player Only", &playerOnly))
        //     flag_player_only().store(playerOnly, std::memory_order_relaxed);
        //
        // if (ImGui::CollapsingHeader("Character", ImGuiTreeNodeFlags_DefaultOpen))
        // {
        //     auto names = pm.character_names();
        //     const auto &activeName = pm.active_character();
        //     for (auto &name : names)
        //     {
        //         bool isCurrent = (name == activeName);
        //         if (isCurrent)
        //             ImGui::PushStyleColor(ImGuiCol_Button,
        //                                   ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        //         if (ImGui::Button(name.c_str()))
        //         {
        //             pm.set_active_character(name);
        //             pm.apply_to_state();
        //             manual_apply();
        //             pm.save();
        //         }
        //         if (isCurrent)
        //             ImGui::PopStyleColor();
        //         ImGui::SameLine();
        //     }
        //     ImGui::NewLine();
        //     ImGui::SetNextItemWidth(120.0f);
        //     ImGui::InputText("##newchar", s_newCharName, sizeof(s_newCharName));
        //     ImGui::SameLine();
        //     if (ImGui::Button("Add Character") && s_newCharName[0] != '\0')
        //     {
        //         pm.set_active_character(s_newCharName);
        //         pm.save();
        //         s_newCharName[0] = '\0';
        //     }
        // }

        ImGui::Separator();

        // --- Presets ---

        if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto &presetList = pm.presets();
            int activeIdx = pm.active_preset_index();
            int count = pm.preset_count();

            // Preset selector.
            for (int i = 0; i < count; ++i)
            {
                ImGui::PushID(i);

                bool isActive = (i == activeIdx);

                // Rename mode for this preset.
                if (s_renameActive && s_renameIndex == i)
                {
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::InputText("##rename", s_renamePresetBuf,
                                         sizeof(s_renamePresetBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        if (auto *p = pm.active_preset_mut())
                        {
                            // We need to access by index, not active.
                            // The preset list is const ref, so we use a workaround.
                        }
                        // Apply rename via set_active then mutate.
                        pm.set_active_preset(i);
                        if (auto *p = pm.active_preset_mut())
                            p->name = s_renamePresetBuf;
                        pm.set_active_preset(activeIdx);
                        pm.save();
                        s_renameActive = false;
                        s_renameIndex = -1;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("OK"))
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
                    // Selectable preset entry.
                    char label[128];
                    std::snprintf(label, sizeof(label), "%s [%d]##preset",
                                  presetList[static_cast<std::size_t>(i)].name.c_str(), i);

                    if (ImGui::Selectable(label, isActive))
                    {
                        pm.set_active_preset(i);
                        pm.apply_to_state();
                        manual_apply();
                        pm.save();
                    }

                    // Double-click to rename.
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    {
                        s_renameActive = true;
                        s_renameIndex = i;
                        std::snprintf(s_renamePresetBuf, sizeof(s_renamePresetBuf), "%s",
                                      presetList[static_cast<std::size_t>(i)].name.c_str());
                    }
                }

                ImGui::PopID();
            }

            if (count == 0)
                ImGui::TextDisabled("No presets — use Append to create one");

            ImGui::Spacing();

            // Preset action buttons.
            if (ImGui::Button("Append"))
            {
                // Snapshot the player's currently-equipped real gear into
                // slot_mappings BEFORE creating the new preset. Without
                // this, Append would clone the active preset's mappings
                // (or the last picker edit) instead of the real loadout.
                capture_outfit();
                pm.append_from_state();
                manual_apply();
            }

            ImGui::SameLine();

            if (ImGui::Button("Replace") && count > 0)
                pm.replace_current_from_state();

            ImGui::SameLine();

            if (ImGui::Button("Remove") && count > 0)
                pm.remove_current();

            ImGui::SameLine();

            if (ImGui::Button("Prev") && count > 1)
            {
                pm.prev_preset();
                manual_apply();
            }

            ImGui::SameLine();

            if (ImGui::Button("Next") && count > 1)
            {
                pm.next_preset();
                manual_apply();
            }

            // Show active preset info.
            if (count > 0)
                ImGui::Text("Active: %d / %d", activeIdx + 1, count);
        }

        ImGui::Separator();

        // --- Per-slot controls ---

        if (ImGui::CollapsingHeader("Slot Details", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto &mappings = slot_mappings();
            const auto &table = ItemNameTable::instance();
            const bool tableReady = table.ready();

            if (!tableReady)
                ImGui::TextDisabled("Item catalog not ready — hex entry only.");

            // Tip line so the short checkbox labels below don't leave
            // users guessing what "active" means.
            ImGui::TextDisabled("Toggle which slots the next Apply All will touch.");

            // "All" master checkbox: bulk-toggles all per-slot active
            // flags without triggering an apply. Reflects "all on" when
            // every slot is checked; clicking it flips them to the
            // opposite uniform state. Changes stay pending until the
            // user explicitly clicks Apply All below.
            {
                bool allActive = true;
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    if (!mappings[i].active)
                    {
                        allActive = false;
                        break;
                    }
                }
                if (ImGui::Checkbox("All", &allActive))
                {
                    for (std::size_t i = 0; i < k_slotCount; ++i)
                        mappings[i].active = allActive;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(pending -- Apply All to commit)");
            }

            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                auto &m = mappings[i];
                auto &ui = s_slotUI[i];
                const char *slotLabel = slot_name(static_cast<TransmogSlot>(i));

                ImGui::PushID(static_cast<int>(i) + 100);

                ImGui::Checkbox(slotLabel, &m.active);

                ImGui::SameLine(120.0f);

                // --- Picker button (name + id) ---
                //
                // Shows the live-resolved name for the current itemId.
                // Clicking opens a search-filterable popup. Falls back to
                // "(unknown)" if the id is not in the catalog (e.g. stale
                // preset from an older patch).
                {
                    std::string pickerLabel;
                    if (m.targetItemId == 0)
                    {
                        pickerLabel = "(none)";
                    }
                    else if (tableReady)
                    {
                        auto name = table.name_of(m.targetItemId);
                        if (name.empty())
                            pickerLabel = "(unknown)";
                        else
                            pickerLabel = std::move(name);
                    }
                    else
                    {
                        pickerLabel = "(catalog N/A)";
                    }

                    char btnLabel[192];
                    std::snprintf(btnLabel, sizeof(btnLabel),
                                  "%s  [0x%04X]##pick", pickerLabel.c_str(),
                                  m.targetItemId);

                    ImGui::SetNextItemWidth(240.0f);
                    if (ImGui::Button(btnLabel, ImVec2(260.0f, 0.0f)))
                    {
                        if (tableReady)
                        {
                            ui.searchBuf[0] = '\0';
                            ImGui::OpenPopup("##slot_picker");
                        }
                    }

                    if (tableReady &&
                        draw_item_picker_popup(
                            "##slot_picker", ui,
                            static_cast<TransmogSlot>(i),
                            m.targetItemId))
                    {
                        // Picker commits stay PENDING in slot_mappings.
                        // The visual does not change until the user
                        // explicitly clicks Apply All -- no auto-apply.
                        // This lets users edit all 5 slots and then push
                        // them as a batch, avoiding flicker and partial
                        // transmog states.
                        //
                        // Keep the hex field in sync so toggling back to
                        // manual entry doesn't show a stale value.
                        std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "%04X",
                                      m.targetItemId);

                        // Focused debug log for "picked but no visual"
                        // cases: prints slot, id, resolved name, and
                        // whether the classifier thinks the item belongs
                        // in this slot. Demoted to debug since picks are
                        // now deferred -- no longer a live event.
                        {
                            const auto name = (m.targetItemId == 0)
                                ? std::string("(none)")
                                : table.name_of(m.targetItemId);
                            const auto cat =
                                ItemNameTable::classify_slot(name);
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

                // --- Manual hex override (kept for power users /
                // catalog misses) ---
                ImGui::SameLine();

                if (!ui.editing)
                    std::snprintf(ui.hexBuf, sizeof(ui.hexBuf), "%04X",
                                  m.targetItemId);

                ImGui::SetNextItemWidth(64.0f);
                if (ImGui::InputText(
                        "##hex", ui.hexBuf, sizeof(ui.hexBuf),
                        ImGuiInputTextFlags_CharsHexadecimal |
                            ImGuiInputTextFlags_CharsUppercase))
                {
                    unsigned long val = std::strtoul(ui.hexBuf, nullptr, 16);
                    m.targetItemId = static_cast<uint16_t>(val);
                }
                ui.editing = ImGui::IsItemActive();

                ImGui::PopID();
            }
        }

        ImGui::Separator();

        // --- Action buttons ---

        // Gate every button that touches live player state on the
        // WorldSystem chain being populated. Before the first world
        // load the player pointer is 0 and firing apply/clear just
        // spams "player not found" into the log. The disabled state
        // flips to enabled organically on the first BatchEquip fire.
        const bool worldReady = Transmog::is_world_ready();
        ImGui::BeginDisabled(!worldReady);

        // Tint the Apply All button yellow when there are pending edits
        // so the call to action is obvious. The textual "[PENDING]" in
        // the header + this colored button give the user two signals.
        if (pending)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.75f, 0.55f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.90f, 0.65f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(1.00f, 0.72f, 0.20f, 1.0f));
        }
        if (ImGui::Button(pending ? "Apply All *" : "Apply All"))
        {
            flag_enabled().store(true, std::memory_order_relaxed);
            manual_apply();
        }
        if (pending)
            ImGui::PopStyleColor(3);

        ImGui::SameLine();

        if (ImGui::Button("Clear All"))
        {
            // Mirror the Clear hotkey: also flip the flag so a later
            // toggle via Enabled checkbox or Numpad5 applies first-press.
            flag_enabled().store(false, std::memory_order_relaxed);
            manual_clear();
        }

        ImGui::SameLine();

        if (ImGui::Button("Capture Outfit"))
            capture_outfit();

        ImGui::SameLine();

        if (ImGui::Button("Save"))
        {
            pm.replace_current_from_state();
            pm.save();
        }

        ImGui::EndDisabled();

        if (!worldReady)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(waiting for world load)");
        }

        // --- Status footer ---

        ImGui::Separator();
        ImGui::TextDisabled("Status");

        if (slot_populator_fn())
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "SlotPopulator: READY");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "SlotPopulator: UNAVAILABLE");

        auto &mappings = slot_mappings();
        int activeCount = 0;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (mappings[i].active && mappings[i].targetItemId != 0)
                ++activeCount;
        }

        ImGui::Text("Active slots: %d / %zu", activeCount, k_slotCount);
        ImGui::Text("Character: %s", pm.active_character().c_str());
    }

    // --- Public interface ---

    bool init_overlay(HMODULE hModule)
    {
        if (!reshade::register_addon(hModule))
            return false;

        reshade::register_overlay("Transmog", &draw_overlay);
        s_overlayModule = hModule;
        s_overlayRegistered = true;

        return true;
    }

    void shutdown_overlay()
    {
        if (!s_overlayRegistered)
            return;

        reshade::unregister_overlay("Transmog", &draw_overlay);
        reshade::unregister_addon(s_overlayModule);
        s_overlayRegistered = false;
        s_overlayModule = nullptr;
    }

} // namespace Transmog
