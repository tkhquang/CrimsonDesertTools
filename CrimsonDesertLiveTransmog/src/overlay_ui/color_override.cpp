// overlay_ui/color_override.cpp
//
// Per-slot Color Override tab body. Drawn inside the dye-popup tab bar's "Color Override" tab item; the caller owns the
// BeginTabItem / EndTabItem pair.

#include "overlay_ui/color_override.hpp"
#include "overlay_ui/helpers.hpp"
#include "overlay_ui/state.hpp"

#include "color_override/color_carrier_set.hpp"
#include "color_override/color_matinst_owner.hpp"
#include "color_override/color_override.hpp"
#include "color_override/color_pending_overrides.hpp"
#include "color_override/color_reinit.hpp"
#include "color_override/color_state.hpp"
#include "color_override/color_swatch_table.hpp"
#include "color_override/color_token_discovery.hpp"
#include "color_override/color_token_table.hpp"
#include "color_override/matinst_probe.hpp"
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
#include <tuple>
#include <vector>

namespace Transmog
{

    void draw_color_override_tab_body(std::size_t slot, std::size_t detected, bool detectedReady, SlotUIState &ui,
                                      ColorOverride::DyeSlot &dyeSlot)
    {
        if (!s_coAutoTriggered[slot] && detected == 0 && !Transmog::ColorOverride::Reinit::any_slot_reinit_active())
        {
            Transmog::flag_enabled().store(true, std::memory_order_relaxed);
            Transmog::ColorOverride::Reinit::start_slot_reinit_once(static_cast<int>(slot));
            s_coAutoTriggered[slot] = true;
        }

        // --- Per-slot color override picker ---
        // Each transmog slot maps to one or more dye-able regions ("swatches"). The hook auto-detects how many regions
        // the currently equipped item has from the per-(slot, channel) write counter, and captures each region's
        // asset-default color so the UI displays it as a starting value.
        //
        //   Slot toggle ('Dye'): master enable for
        //                        all swatches
        //   Per-swatch checkbox: enable user override
        //                        for that swatch
        //   Color picker: shows asset default until
        //                 toggled, then pre-fills
        //                 user RGB and lets them edit
        ImGui::BeginDisabled(!detectedReady);
        if (ImGui::Checkbox("Dye##dye_on", &dyeSlot.slot_enabled))
        {
            // The Dye master toggle gates the substitution path (dye_override.cpp: !slot_enabled -> return false,
            // engine default writes through). To make the toggle visually take effect we need the engine to actually
            // write again; bare manual_apply_slot short-circuits as a same-state apply. Route through commit-retick so
            // the engine tears down and re-instantiates the carrier, producing fresh writes that respect the new
            // slot_enabled value.
            if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
            {
                flag_enabled().store(true, std::memory_order_relaxed);
                Transmog::ColorOverride::Reinit::schedule_color_commit_retick(static_cast<int>(slot));
            }
            Transmog::dye_dirty().store(true, std::memory_order_release);
        }
        ImGui::EndDisabled();
        if (!detectedReady && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("No apply yet -- swatches unknown.\n"
                                   "Run Re-init to capture the slot's dye\n"
                                   "swatches before toggling the master Dye\n"
                                   "switch.");
            ImGui::EndTooltip();
        }

        // Per-slot single-pass swatch re-init. Drives one clear + apply cycle (~1.5s) and keeps whatever rows were
        // captured. Use when the swatch list is empty and you'd otherwise untick-retick by hand. One pass is enough for
        // the common case.
        ImGui::SameLine(0.0f, 6.0f);
        const bool reinitActive = Transmog::ColorOverride::Reinit::is_slot_reinit_active(static_cast<int>(slot));
        if (reinitActive)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::BeginDisabled(true);
            ImGui::SmallButton("Re-init...##sw_reinit_busy");
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
        }
        else
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            const bool reinitClicked = ImGui::SmallButton("Re-init##sw_reinit");
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Single clear + apply (~1.5s) to capture\n"
                                       "the slot's swatches. Use when the list\n"
                                       "looks empty or stale.\n\n"
                                       "Replaces the manual 'untick / retick'\n"
                                       "loop -- you can leave this slot alone\n"
                                       "while it runs.");
                ImGui::EndTooltip();
            }
            if (reinitClicked)
            {
                flag_enabled().store(true, std::memory_order_relaxed);
                Transmog::ColorOverride::Reinit::start_slot_reinit_once(static_cast<int>(slot));
            }

            // Reset slot button stays enabled even when `detected == 0` so the user can recover from stale rows in
            // exactly the state where Re-init would otherwise stall.
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            const bool topResetClicked = ImGui::SmallButton("Reset slot##sw_reset_top");
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Wipe ALL captured swatches for THIS slot,\n"
                                       "clear the post-reinit lock + frozen-hidden\n"
                                       "rows, then trigger a fresh apply.\n\n"
                                       "Use when the slot is stuck with stale rows\n"
                                       "(e.g. JSON-restored entries that no longer\n"
                                       "match the current item) and Re-init can't\n"
                                       "make progress.");
                ImGui::EndTooltip();
            }
            if (topResetClicked)
            {
                Transmog::ColorOverride::SwatchTable::wipe_swatch_table_for_slot(static_cast<int>(slot));
                Transmog::ColorOverride::SwatchTable::clear_dye_state_for_slot(static_cast<int>(slot));
                // Drop any persisted overrides for this slot that are queued via the setter's pending map -- otherwise
                // the next engine write would re-substitute the saved colour on top of the freshly-wiped row.
                Transmog::ColorOverride::PendingOverrides::clear_slot(static_cast<int>(slot));
                // Reset is a pending change: the JSON still has the old swatch rows. Flip dirty so the top "Save *"
                // button surfaces, but don't auto-write JSON -- user commits explicitly.
                Transmog::dye_dirty().store(true, std::memory_order_release);
                if (s_autoApply)
                {
                    flag_enabled().store(true, std::memory_order_relaxed);
                    manual_apply_slot(slot);
                }
            }
        }

        // Auto-detected swatch count drives whether to show the swatch UI. Pre-apply (detected==0) we render nothing
        // here so the disabled-Dye-checkbox tooltip can explain the state without an empty CollapsingHeader pushing the
        // Apply button down.
        if (detectedReady)
        {
            // Collapse the per-slot swatch UI behind a CollapsingHeader so users who only want to swap items don't get
            // a wall of dye controls pushing the Apply button down. Default-closed first time; ImGui remembers
            // per-header state across frames after that. The body below is one indent level shallower than strict
            // nesting would dictate to keep the wrap purely additive.
            char dyeHdrBuf[160];
            // Count rows with a user-picked colour. The badge shows "M rows (N coloured)" where M is the full captured
            // set and N is how many the user has explicitly picked a colour for.
            std::size_t coloured = 0;
            for (const auto &sw : dyeSlot.swatches)
            {
                if (sw.override_active)
                    ++coloured;
            }
            const bool hasOverride = coloured > 0;
            // Mid-retick badge (b): show "(applying...)" while a colour-commit retick cycle is in-flight on this slot.
            // The cycle takes ~1.9s; this badge tells the user the engine hasn't finished yet.
            const bool reticking =
                Transmog::ColorOverride::Reinit::is_color_commit_retick_active(static_cast<int>(slot));
            // Master-disabled badge: if the user unticked the
            // Dye checkbox, the swatch list is inert. Surface that clearly in the closed header.
            const bool masterDisabled = !dyeSlot.slot_enabled;
            // Plain text label -- the popup tab is the container, so there is no CollapsingHeader ID-stability concern.
            if (coloured > 0)
            {
                std::snprintf(dyeHdrBuf, sizeof(dyeHdrBuf), "%zu rows (%zu coloured)%s%s%s", detected, coloured,
                              hasOverride ? " *" : "", reticking ? " (applying)" : "", masterDisabled ? " (off)" : "");
            }
            else
            {
                std::snprintf(dyeHdrBuf, sizeof(dyeHdrBuf), "%zu rows%s%s", detected, reticking ? " (applying)" : "",
                              masterDisabled ? " (off)" : "");
            }
            ui_text("%s", dyeHdrBuf);
            ImGui::Separator();
            {
                auto clampByte = [](float v) -> std::uint8_t
                {
                    if (v < 0.0f)
                        v = 0.0f;
                    if (v > 1.0f)
                        v = 1.0f;
                    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
                };

                // Per-submesh + per-token granularity gives ~9 swatches per submesh, which gets unwieldy fast.
                // Layout:
                //   - Group rows by (submesh_stable_id, template_id) into collapsible "Region N" cards.
                //   - Within each region, group by layer (tint / mask / detail / hair / misc).
                //   - For triplet layers (R/G/B suffix) show one colour picker by default with the R/G/B channels
                //     linked, mirroring merchant "Pure X" semantics. Untick "link" to expose three independent
                //     per-channel pickers.
                //   - Each row is labelled, so no "Show all" noise-hiding filter is needed. Rows still waiting for
                //     their first capture (default_captured == false) are skipped silently; they're inert until the
                //     engine writes them.

                // Pass 1: group swatch indices by submesh, then by layer. Each region's `layerSlot[layer][channel]`
                // holds the swatch index in dyeSlot.swatches, or -1.
                //
                // Layers: 0=tint, 1=mask, 2=detail, 3=hair, 4=scratch. `layerSlot[L][0..2]` = R/G/B siblings for that
                // family. `layerSingletons[L]` = ch=-1 tokens (e.g. `_dyeingDetailLayerColorBlend`, `_hairDyeingColor`)
                // rendered under the same family header. `miscIndices` = truly unknown tokens (layer=-1).
                struct RegionView
                {
                    std::uint64_t stable_id = 0;
                    std::uint16_t tpl = 0;
                    int layerSlot[5][3]{};
                    std::vector<int> layerSingletons[5];
                    std::vector<int> miscIndices;
                };
                auto initRegion = [](RegionView &rv)
                {
                    for (int L = 0; L < 5; ++L)
                        for (int C = 0; C < 3; ++C)
                            rv.layerSlot[L][C] = -1;
                };

                std::map<std::uint64_t, RegionView> regions;
                // Slot-level "recolor all" cascade list: every visible swatch in the slot.
                std::vector<int> slotAllIndices;
                slotAllIndices.reserve(detected);

                for (std::size_t s = 0; s < detected; ++s)
                {
                    auto &sw = dyeSlot.swatches[s];
                    // Skip rows that haven't captured a default yet; picking on them would write zeros.
                    // (override_active rows always pass below.)
                    if (!sw.default_captured && !sw.override_active)
                        continue;

                    slotAllIndices.push_back(static_cast<int>(s));

                    // Group rows by hash-of-submesh-name rather than by submesh_stable_id. Two SwatchOverride entries
                    // can share submesh_name but carry different submesh_stable_ids: JSON-loaded placeholders
                    // synthesise stable_id via FNV(name) in populate_from_persisted, while engine-inserted rows (via
                    // the setter's lookup_or_insert) carry the live matInst's engine-side stable_id. Without this
                    // name-based key the picker shows the same submesh as two separate regions.
                    auto fnv1a64_name = [](const char *p) noexcept -> std::uint64_t
                    {
                        std::uint64_t h = 0xcbf29ce484222325ULL;
                        while (*p)
                        {
                            h ^= static_cast<std::uint8_t>(*p++);
                            h *= 0x100000001b3ULL;
                        }
                        return h;
                    };
                    const std::uint64_t sid =
                        (sw.submesh_name[0] != '\0') ? fnv1a64_name(sw.submesh_name) : sw.submesh_stable_id;
                    auto &rv = regions[sid];
                    if (rv.stable_id == 0)
                    {
                        initRegion(rv);
                        rv.stable_id = sid;
                        rv.tpl = sw.template_id;
                    }
                    const int layer = Transmog::ColorOverride::TokenTable::token_layer(sw.token_id);
                    const int ch = Transmog::ColorOverride::TokenTable::token_channel(sw.token_id);
                    if (layer >= 0 && layer <= 4)
                    {
                        if (ch >= 0 && ch <= 2)
                            rv.layerSlot[layer][ch] = static_cast<int>(s);
                        else
                            rv.layerSingletons[layer].push_back(static_cast<int>(s));
                    }
                    else
                    {
                        rv.miscIndices.push_back(static_cast<int>(s));
                    }
                }

                // Render swatch sub-block: override toggle, picker, tooltip. Reused for both linked and per-channel
                // rendering. `cascadeIdx[]` lists the indices that should receive the colour edit; first index is the
                // "primary" displayed.
                auto renderSwatchControls = [&](const int *cascadeIdx, int cascadeCount, const char *channelLabel)
                {
                    if (cascadeCount <= 0 || cascadeIdx[0] < 0)
                        return;
                    auto &primary = dyeSlot.swatches[cascadeIdx[0]];
                    ImGui::PushID(cascadeIdx[0] + 200);

                    if (channelLabel && channelLabel[0])
                    {
                        ImGui::TextUnformatted(channelLabel);
                        ImGui::SameLine(0.0f, 4.0f);
                    }

                    const bool prevActive = primary.override_active;
                    bool active = prevActive;
                    // Bordered checkbox per UX request.
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    const bool toggled = ImGui::Checkbox("##sw_on", &active);
                    ImGui::PopStyleVar();
                    if (toggled)
                    {
                        for (int k = 0; k < cascadeCount; ++k)
                        {
                            int idx = cascadeIdx[k];
                            if (idx < 0)
                                continue;
                            auto &row = dyeSlot.swatches[idx];
                            // Skip tick-on for rows whose def was never captured -- otherwise we'd set row.r/g/b =
                            // (0,0,0) and the substitute path would write black for that token, shifting the rendered
                            // colour away from the engine's natural blend. Affects assets whose engine pipeline emits
                            // writes for only a subset of the 9-prop chord (e.g. orcumer armours ship _tintColor +
                            // _detail but no _dyeingColorMask). Tick-off (active=false) always runs so the user can
                            // always un-override.
                            if (active && !row.default_captured)
                                continue;
                            row.override_active = active;
                            if (active && !prevActive)
                            {
                                row.r = row.def_r;
                                row.g = row.def_g;
                                row.b = row.def_b;
                            }
                            // Mirror to PendingOverrides so the slot-agnostic substitute path picks up this row's new
                            // state. Active=> write current RGB into pending; inactive=>erase so substitute stops
                            // firing for this (submesh, token).
                            if (active)
                                mirror_override_to_pending(static_cast<int>(slot), static_cast<std::size_t>(idx), row.r,
                                                           row.g, row.b);
                            else
                                erase_override_from_pending(static_cast<int>(slot), static_cast<std::size_t>(idx));
                        }
                        // Override-active checkbox toggle is a colour change (rows go default to user override or
                        // vice-versa), so retick unconditionally with the same semantics as the picker commits below.
                        if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
                        {
                            flag_enabled().store(true, std::memory_order_relaxed);
                            Transmog::ColorOverride::Reinit::schedule_color_commit_retick(static_cast<int>(slot));
                        }
                        // Tick toggle flips override_active, which also flips whether get_persistable_state includes
                        // the row. The picker applies overrides live but does NOT write JSON -- dye_dirty lights up the
                        // top-level "Save *" button so the user commits to disk explicitly. Preset switch with
                        // dirty=true discards pending edits.
                        Transmog::dye_dirty().store(true, std::memory_order_release);
                    }
                    ImGui::SameLine(0.0f, 2.0f);

                    const bool showUser = active;
                    float rgb[3];
                    if (showUser)
                    {
                        rgb[0] = primary.r / 255.0f;
                        rgb[1] = primary.g / 255.0f;
                        rgb[2] = primary.b / 255.0f;
                    }
                    else
                    {
                        rgb[0] = primary.def_r / 255.0f;
                        rgb[1] = primary.def_g / 255.0f;
                        rgb[2] = primary.def_b / 255.0f;
                    }

                    // True circle swatch: ImGui's `ColorEdit3` / `ColorButton` use `RenderFrame` whose `PathRect`
                    // clamps rounding to dim/2 - 1, leaving a ~2px flat zone in the middle even with `FrameRounding =
                    // 999`. To get a real circle we draw it ourselves with `ImDrawList::AddCircleFilled`, using an
                    // InvisibleButton for hit-testing in the override-on state and a Dummy for the display-only off
                    // state. Trade-off: lost ColorEdit3's right-click options menu / drag-drop, gained a real circle
                    // and full off-state opacity.
                    const float diameter = ImGui::GetFrameHeight();
                    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                    const ImVec2 sw_center(cursorPos.x + diameter * 0.5f, cursorPos.y + diameter * 0.5f);
                    const float sw_radius = diameter * 0.5f - 1.0f;
                    const ImVec4 sw_colVec(rgb[0], rgb[1], rgb[2], 1.0f);
                    const ImU32 sw_colU = ImGui::ColorConvertFloat4ToU32(sw_colVec);
                    const ImU32 sw_borderU = ImGui::GetColorU32(ImGuiCol_Border);

                    // The swatch itself is always clickable. If the user clicks it while override is OFF, auto-tick
                    // override (cascade across linked rows), pre-fill from captured default, then open the picker.
                    // End-state matches ticking the checkbox first, just one click instead of two.
                    const bool sw_clicked = ImGui::InvisibleButton("##sw_btn", ImVec2(diameter, diameter));
                    const bool sw_hovered = ImGui::IsItemHovered();

                    ImDrawList *sw_dl = ImGui::GetWindowDrawList();
                    sw_dl->AddCircleFilled(sw_center, sw_radius, sw_colU, 32);
                    sw_dl->AddCircle(sw_center, sw_radius, sw_borderU, 32, 1.0f);

                    // Default-colour reference dot next to the active picker swatch. Shows the engine-captured default
                    // (def_r/g/b) so the user has a quick visual anchor for "what was the asset's original colour?" --
                    // useful when dialing in a custom colour or deciding whether to revert an override. Display-only,
                    // non-interactive. Skipped if no default has been captured yet (placeholders pre-promote or rows
                    // never touched by the engine).
                    if (primary.default_captured)
                    {
                        ImGui::SameLine(0.0f, 4.0f);
                        const float refDiam = diameter * 0.70f;
                        const ImVec2 refCursor = ImGui::GetCursorScreenPos();
                        // Centre the smaller circle vertically against the larger picker swatch.
                        const float yOff = (diameter - refDiam) * 0.5f;
                        const ImVec2 refCenter(refCursor.x + refDiam * 0.5f, refCursor.y + yOff + refDiam * 0.5f);
                        const float refRadius = refDiam * 0.5f - 1.0f;
                        const ImVec4 refColVec(primary.def_r / 255.0f, primary.def_g / 255.0f, primary.def_b / 255.0f,
                                               1.0f);
                        const ImU32 refColU = ImGui::ColorConvertFloat4ToU32(refColVec);
                        ImGui::Dummy(ImVec2(refDiam, diameter));
                        const bool refHovered = ImGui::IsItemHovered();
                        sw_dl->AddCircleFilled(refCenter, refRadius, refColU, 24);
                        sw_dl->AddCircle(refCenter, refRadius, sw_borderU, 24, 1.0f);
                        if (refHovered)
                        {
                            char refTip[48];
                            std::snprintf(refTip, sizeof(refTip), "Asset default #%02X%02X%02X", primary.def_r,
                                          primary.def_g, primary.def_b);
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(refTip);
                            ImGui::EndTooltip();
                        }
                    }

                    if (sw_clicked)
                    {
                        if (!showUser)
                        {
                            for (int k = 0; k < cascadeCount; ++k)
                            {
                                int idx = cascadeIdx[k];
                                if (idx < 0)
                                    continue;
                                auto &row = dyeSlot.swatches[idx];
                                // Same gate as the checkbox path: rows without a captured def must stay un-overridden,
                                // else substitute writes 0,0,0 and the rendered colour shifts.
                                if (!row.default_captured)
                                    continue;
                                row.override_active = true;
                                row.r = row.def_r;
                                row.g = row.def_g;
                                row.b = row.def_b;
                                // Mirror to pending so the substitute path agrees with the row's new default RGB.
                                mirror_override_to_pending(static_cast<int>(slot), static_cast<std::size_t>(idx), row.r,
                                                           row.g, row.b);
                            }
                            if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
                            {
                                flag_enabled().store(true, std::memory_order_relaxed);
                                // Tear-down + reapply: engine short-circuits a same-state single apply, so we toggle
                                // m.active off->on around a wait window. See dye_override.cpp SlotReinitState
                                // (CommitRetick). Unconditional on swatch click (override flips on); no s_autoApply
                                // gate.
                                Transmog::ColorOverride::Reinit::schedule_color_commit_retick(static_cast<int>(slot));
                            }
                            // Auto-tick on click flipped override_active. The picker writes live but does NOT touch
                            // JSON; dye_dirty lights up the top-level "Save *" button so the user commits to disk
                            // explicitly. Preset switch with dirty=true discards pending edits.
                            Transmog::dye_dirty().store(true, std::memory_order_release);
                        }
                        ImGui::OpenPopup("##sw_picker");
                    }
                    if (ImGui::BeginPopup("##sw_picker"))
                    {
                        // Re-seed the picker each frame from primary's current values so live edits through the picker
                        // reflect back into the swatch correctly across frames.
                        float pickerRgb[3] = {
                            primary.r / 255.0f,
                            primary.g / 255.0f,
                            primary.b / 255.0f,
                        };
                        if (ImGui::ColorPicker3("##sw_pick", pickerRgb,
                                                ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview))
                        {
                            const std::uint8_t r = clampByte(pickerRgb[0]);
                            const std::uint8_t g = clampByte(pickerRgb[1]);
                            const std::uint8_t b = clampByte(pickerRgb[2]);
                            for (int k = 0; k < cascadeCount; ++k)
                            {
                                int idx = cascadeIdx[k];
                                if (idx < 0)
                                    continue;
                                auto &row = dyeSlot.swatches[idx];
                                row.r = r;
                                row.g = g;
                                row.b = b;
                                // Mirror to pending so the substitute path picks up the user's edit on the next engine
                                // write.
                                mirror_override_to_pending(static_cast<int>(slot), static_cast<std::size_t>(idx), r, g,
                                                           b);
                            }
                            // The picker applies overrides live but does NOT write JSON; dye_dirty lights up the
                            // top-level "Save *" button so the user commits to disk explicitly. Preset switch with
                            // dirty=true discards pending edits.
                            Transmog::dye_dirty().store(true, std::memory_order_release);
                            // Trigger a single-pass tear-down + reapply so the engine re-builds the carrier matInst
                            // with the new colour. Fires unconditionally on every colour commit -- independent of the
                            // "Instant Apply" checkbox which governs hover/pick-apply only. Coalesces with any
                            // in-flight retick so a 60Hz drag fires ~1 retick per ~1.9s cycle, not 60.
                            if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
                            {
                                flag_enabled().store(true, std::memory_order_relaxed);
                                Transmog::ColorOverride::Reinit::schedule_color_commit_retick(static_cast<int>(slot));
                            }
                        }
                        ImGui::EndPopup();
                    }
                    if (sw_hovered)
                    {
                        ImGui::BeginTooltip();
                        const char *tokenName = Transmog::ColorOverride::TokenTable::token_label_for(primary.token_id);
                        char tokenBuf[64];
                        if (tokenName)
                        {
                            std::snprintf(tokenBuf, sizeof(tokenBuf), "%s (0x%04X)", tokenName, primary.token_id);
                        }
                        else
                        {
                            std::snprintf(tokenBuf, sizeof(tokenBuf), "0x%04X", primary.token_id);
                        }
                        char tipBuf[256];
                        if (cascadeCount > 1)
                        {
                            std::snprintf(tipBuf, sizeof(tipBuf),
                                          "Linked R/G/B  %s\n"
                                          "submesh 0x%016llX  tpl 0x%04X\n"
                                          "asset def #%02X%02X%02X  blend=%u/255",
                                          tokenBuf, static_cast<unsigned long long>(primary.submesh_stable_id),
                                          static_cast<unsigned>(primary.template_id), primary.def_r, primary.def_g,
                                          primary.def_b, primary.def_a);
                        }
                        else
                        {
                            std::snprintf(tipBuf, sizeof(tipBuf),
                                          "%s\n"
                                          "submesh 0x%016llX  tpl 0x%04X\n"
                                          "asset def #%02X%02X%02X  blend=%u/255",
                                          tokenBuf, static_cast<unsigned long long>(primary.submesh_stable_id),
                                          static_cast<unsigned>(primary.template_id), primary.def_r, primary.def_g,
                                          primary.def_b, primary.def_a);
                        }
                        ImGui::TextUnformatted(tipBuf);
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                };

                // Batch recolor helper. Renders a clickable circle and on click opens a picker popup. Each value change
                // cascades the chosen RGB to every index in `cascade`, ticking `override_active = true`.
                //
                // displayColor is derived from cascade[0] each frame (current r/g/b if override active, else captured
                // def). This is the same trick the per-swatch picker uses: writes propagate through cascade[0] to
                // displayColor next frame, so picker drag stays smooth. If we re-seeded from a fixed colour (e.g. the
                // cluster's def) each frame, the wheel would snap back every frame and drag would jitter.
                auto recolorBatch = [&](const char *idTag, const std::vector<int> &cascade)
                {
                    if (cascade.empty())
                        return;
                    const auto &first = dyeSlot.swatches[cascade[0]];
                    const ImVec4 displayColor =
                        first.override_active
                            ? ImVec4(first.r / 255.0f, first.g / 255.0f, first.b / 255.0f, 1.0f)
                            : ImVec4(first.def_r / 255.0f, first.def_g / 255.0f, first.def_b / 255.0f, 1.0f);

                    ImGui::PushID(idTag);
                    const float bDiameter = ImGui::GetFrameHeight();
                    const ImVec2 bCursor = ImGui::GetCursorScreenPos();
                    const ImVec2 bCenter(bCursor.x + bDiameter * 0.5f, bCursor.y + bDiameter * 0.5f);
                    const float bRadius = bDiameter * 0.5f - 1.0f;
                    const ImU32 bColU = ImGui::ColorConvertFloat4ToU32(displayColor);
                    const ImU32 bBorderU = ImGui::GetColorU32(ImGuiCol_Border);
                    const bool bClicked = ImGui::InvisibleButton("##rb_btn", ImVec2(bDiameter, bDiameter));
                    ImDrawList *bDl = ImGui::GetWindowDrawList();
                    bDl->AddCircleFilled(bCenter, bRadius, bColU, 32);
                    bDl->AddCircle(bCenter, bRadius, bBorderU, 32, 1.0f);
                    if (bClicked)
                        ImGui::OpenPopup("##rb_pop");
                    if (ImGui::BeginPopup("##rb_pop"))
                    {
                        float prgb[3] = {
                            displayColor.x,
                            displayColor.y,
                            displayColor.z,
                        };
                        if (ImGui::ColorPicker3("##rb_pick", prgb,
                                                ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview))
                        {
                            const std::uint8_t pr = clampByte(prgb[0]);
                            const std::uint8_t pg = clampByte(prgb[1]);
                            const std::uint8_t pb = clampByte(prgb[2]);
                            for (int idx : cascade)
                            {
                                if (idx < 0)
                                    continue;
                                auto &row = dyeSlot.swatches[idx];
                                row.override_active = true;
                                row.r = pr;
                                row.g = pg;
                                row.b = pb;
                                // Mirror to pending so the substitute path picks up the recolor-all edit.
                                mirror_override_to_pending(static_cast<int>(slot), static_cast<std::size_t>(idx), pr,
                                                           pg, pb);
                            }
                            if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
                            {
                                flag_enabled().store(true, std::memory_order_relaxed);
                                // Unconditional retick on batch-recolor commit; no s_autoApply gate (matches per-row
                                // picker semantics).
                                Transmog::ColorOverride::Reinit::schedule_color_commit_retick(static_cast<int>(slot));
                            }
                            // Mark dirty for the explicit Save commit -- see the matching call site in
                            // renderSwatchControls for the full rationale.
                            Transmog::dye_dirty().store(true, std::memory_order_release);
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                };

                // --- Slot-level "Recolor all" + auto-clusters ---
                // Single picker drives every visible swatch in the slot. Lets users do "make this whole helm red"
                // without touching individual rows.
                ImGui::TextUnformatted("Recolor all:");
                ImGui::SameLine(0.0f, 6.0f);
                recolorBatch("slot_all", slotAllIndices);

                // "Reset slot": escape hatch for cross-slot bleed (e.g. Kairos cloak + boots sharing one material means
                // whichever slot applies first absorbs both submeshes' rows; the other slot is permanently empty).
                // Wipes the swatch table, clears freeze
                // + carrier state, then triggers a fresh apply
                // for this slot. User picks for this slot are lost; cross-slot pollution is gone.
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                const bool resetClicked = ImGui::SmallButton("Reset slot##sw_reset");
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Wipe ALL captured swatches for THIS slot.\n"
                                           "Use when cross-slot bleed has misattributed\n"
                                           "rows from another transmog item (common with\n"
                                           "Kairos and other shared-material sets).\n\n"
                                           "Triggers a fresh apply on this slot, which\n"
                                           "re-captures cleanly. Loses any colours you've\n"
                                           "picked for this slot.");
                    ImGui::EndTooltip();
                }
                if (resetClicked)
                {
                    Transmog::ColorOverride::SwatchTable::wipe_swatch_table_for_slot(static_cast<int>(slot));
                    Transmog::ColorOverride::SwatchTable::clear_dye_state_for_slot(static_cast<int>(slot));
                    Transmog::ColorOverride::PendingOverrides::clear_slot(static_cast<int>(slot));
                    Transmog::dye_dirty().store(true, std::memory_order_release);
                    if (s_autoApply)
                    {
                        flag_enabled().store(true, std::memory_order_relaxed);
                        manual_apply_slot(slot);
                    }
                }

                // "Revert": soft reset that keeps the captured swatch list intact (no Re-init needed afterwards), but
                // flips every row's override_active=false so the engine's defaults render. Distinct from "Reset slot"
                // which wipes the captured list entirely.
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                const bool revertClicked = ImGui::SmallButton("Revert to default##sw_revert");
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Revert ALL user overrides on THIS slot\n"
                                           "back to engine defaults. Keeps the\n"
                                           "captured swatch list intact (no Re-init\n"
                                           "needed afterwards).\n\n"
                                           "Use to compare your dye picks against\n"
                                           "the original asset colours, or to undo\n"
                                           "a slot's colour edits without losing\n"
                                           "the captured row structure.");
                    ImGui::EndTooltip();
                }
                if (revertClicked)
                {
                    for (auto &sw : dyeSlot.swatches)
                        sw.override_active = false;
                    // Drop the slot's queued pending-overrides map. PendingOverrides holds the JSON-loaded user picks
                    // and gets consulted on every successful lookup_or_insert -- when an engine write hits a matched
                    // row it calls set_override_active(true). Without clearing here, the schedule_color_commit_retick
                    // below would fire engine writes that immediately re-enable every JSON override we just cleared.
                    // With clearing, the retick
                    // sees empty pending -> override stays off
                    // -> substitute bails -> engine natural
                    // colour flows. Re-loading the preset (or switching away without saving) restores PendingOverrides
                    // from JSON, so the revert stays a pending change until Save commits.
                    Transmog::ColorOverride::PendingOverrides::clear_slot(static_cast<int>(slot));
                    if (!Transmog::ColorOverride::Reinit::any_slot_reinit_active())
                    {
                        flag_enabled().store(true, std::memory_order_relaxed);
                        Transmog::ColorOverride::Reinit::schedule_color_commit_retick(static_cast<int>(slot));
                    }
                    Transmog::dye_dirty().store(true, std::memory_order_release);
                }

                // Auto-detected clusters: rows that share the same (layer, def_r, def_g, def_b). One picker drives all
                // rows in the cluster. Wrapped in a collapsible tree node so users who don't want the suggestions can
                // fold them away without us silently hiding them. Default-open since the suggestions are the whole
                // point of the cluster bar. Advanced view toggle, per-character preference. Default off: render a
                // merchant-like flat list, one picker per UNIQUE def colour (cluster of >=1 member). Tick to reveal
                // per-region per-token trees with full shader-property granularity.
                bool advancedView = Transmog::ColorOverride::dye_advanced_view_get();
                if (ImGui::Checkbox("Advanced view##dye_adv", &advancedView))
                {
                    Transmog::ColorOverride::dye_advanced_view_set(advancedView);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Show per-region per-token trees with full\n"
                                           "shader-property granularity beneath the\n"
                                           "merchant-like flat picker list.\n\n"
                                           "Default off -- most users get the same UX\n"
                                           "as the merchant dye UI: one picker per\n"
                                           "distinct asset-default colour. Tick this\n"
                                           "only if you need to dye individual tokens\n"
                                           "(e.g. tint vs. mask vs. detail layers\n"
                                           "separately).");
                    ImGui::EndTooltip();
                }

                // "Show only modified" filter: per-slot session toggle that hides rows / clusters / regions whose
                // `override_active` is false. Useful when an item exposes 30+ swatches and the user wants to revisit
                // only the colours they actually changed.
                {
                    auto &uiSlot = s_slotUI[slot];
                    ImGui::SameLine();
                    ImGui::Checkbox("Modified only##dye_mod", &uiSlot.showOnlyModified);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Hide rows / regions whose colour is\n"
                                               "still at the captured engine default.\n"
                                               "Untick to see every swatch this slot\n"
                                               "exposed during apply.");
                        ImGui::EndTooltip();
                    }
                }

                ImGui::Separator();

                if (!advancedView)
                {
                    // Merchant-style region-channel picker. Per region: 1-3 ColorPicker3s, one per mask-texture channel
                    // suffix (R/G/B). Each picker cascade-writes its color to every captured swatch sharing that
                    // channel-suffix family in this region -- mirroring the engine merchant's `sub_14274A3C0` chord (9
                    // token writes per channel: dyeingColorMask + detail layers + tintColor + 5 mask-overlay layers).
                    //
                    // The R/G/B suffixes are NOT R/G/B colour components -- they're the engine's 3 mask-texture
                    // channels, each holding a full RGB colour for one physical region of the mesh. Materials with
                    // masks active on only some channels get fewer pickers (channels with no captured rows are hidden).
                    const bool filterMod = s_slotUI[slot].showOnlyModified;
                    for (auto &kv : regions)
                    {
                        auto &rv = kv.second;

                        // Walk swatches in this region and bucket by channel_kind (0=R, 1=G, 2=B, else=-1). Misc tokens
                        // with no detectable channel are skipped here; they show in Advanced view for power users.
                        //
                        // Layer iteration order matches the engine merchant function sub_14274A3C0. The merchant writes
                        // a 9-prop "chord" per channel in this priority order:
                        //   0  _dyeingColorMask         <-- L=1
                        //   1  _dyeingDetailLayerColorMask <-- L=2
                        //   2..7 _detail* / _dyeingCustom* mask layers
                        //   8  _tintColor              <-- L=0
                        // `primary` in renderSwatchControls is cascadeIdx[0], so whatever lands first in chRows[ch]
                        // drives the picker's displayed reference colour. PC armour ships _dyeingColorMask (L=1) so the
                        // picker shows the merchant's primary base; orcumer / mob assets ship only detail+tint, so the
                        // picker falls back to _dyeingDetailLayerColorMask (L=2).
                        //   L=1 (mask)    first
                        //   L=2 (detail)  second
                        //   L=0 (tint)    third
                        //   L=4 (scratch) fourth
                        //   L=3 (hair)    fifth
                        // Cascade still writes to every captured token sharing the channel suffix -- only the displayed
                        // reference changes. Per-layer singletons (ch=-1 tokens like `_dyeingDetailLayerColorBlend`)
                        // never make it into the merchant chord -- they surface only in Advanced view.
                        std::vector<int> chRows[3];
                        static const int k_layerOrder[5] = {1, 2, 0, 4, 3};
                        for (int Li = 0; Li < 5; ++Li)
                        {
                            const int L = k_layerOrder[Li];
                            for (int C = 0; C < 3; ++C)
                            {
                                const int sIdx = rv.layerSlot[L][C];
                                if (sIdx < 0)
                                    continue;
                                chRows[C].push_back(sIdx);
                            }
                        }

                        // Hair singletons surfaced in simple view too -- merchants don't ship a hair picker, but users
                        // want one-click hair recolor from the same panel as armor (no need to flip into Advanced).
                        const auto &hairRows = rv.layerSingletons[3];
                        const bool anyChannel =
                            !chRows[0].empty() || !chRows[1].empty() || !chRows[2].empty() || !hairRows.empty();
                        if (!anyChannel)
                            continue;

                        // "Modified only" filter: skip region if no channel / hair row has an override.
                        if (filterMod)
                        {
                            bool anyOverridden = false;
                            for (int ch = 0; ch < 3 && !anyOverridden; ++ch)
                                for (int idx : chRows[ch])
                                    if (dyeSlot.swatches[idx].override_active)
                                    {
                                        anyOverridden = true;
                                        break;
                                    }
                            if (!anyOverridden)
                                for (int idx : hairRows)
                                    if (dyeSlot.swatches[idx].override_active)
                                    {
                                        anyOverridden = true;
                                        break;
                                    }
                            if (!anyOverridden)
                                continue;
                        }

                        // Region header: prefer captured submesh name; fall back to hair-aware label or "Region N"
                        // exactly as the Advanced-mode header does (kept consistent so a user toggling views doesn't
                        // see different labels for the same region).
                        const char *sm = nullptr;
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int C = 0; C < 3 && !sm; ++C)
                            {
                                const int sIdx = rv.layerSlot[L][C];
                                if (sIdx >= 0 && dyeSlot.swatches[sIdx].submesh_name[0] != '\0')
                                    sm = dyeSlot.swatches[sIdx].submesh_name;
                            }
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int sIdx2 : rv.layerSingletons[L])
                                if (dyeSlot.swatches[sIdx2].submesh_name[0] != '\0')
                                {
                                    sm = dyeSlot.swatches[sIdx2].submesh_name;
                                    break;
                                }
                        if (!sm && !rv.miscIndices.empty() &&
                            dyeSlot.swatches[rv.miscIndices.front()].submesh_name[0] != '\0')
                            sm = dyeSlot.swatches[rv.miscIndices.front()].submesh_name;
                        char hdrBuf[128];
                        if (sm)
                        {
                            std::snprintf(hdrBuf, sizeof(hdrBuf), "%s", sm);
                        }
                        else
                        {
                            bool hasHair = !rv.layerSingletons[3].empty();
                            if (!hasHair)
                                for (int C = 0; C < 3 && !hasHair; ++C)
                                    if (rv.layerSlot[3][C] >= 0)
                                        hasHair = true;
                            std::snprintf(hdrBuf, sizeof(hdrBuf),
                                          hasHair ? "[Hair -- hidden under helm]" : "[Unnamed]");
                        }

                        ImGui::PushID(static_cast<int>(rv.stable_id) ^ static_cast<int>(rv.stable_id >> 32) ^ 0xC0DE);
                        // Collapsible region header. Closed by default to match Advanced mode -- users expand only the
                        // regions they want to touch. The header text matches the
                        // Advanced-mode `(tpl 0xXXXX)` format so toggling views keeps the same labels.
                        char treeBuf[160];
                        std::snprintf(treeBuf, sizeof(treeBuf), "%s  (tpl 0x%04X)", hdrBuf, rv.tpl);
                        // Default-open: the Color Override tab is now the wrapper, so collapsing each submesh by
                        // default just hides what the user came to see.
                        const bool open = ImGui::TreeNodeEx(treeBuf, ImGuiTreeNodeFlags_SpanAvailWidth |
                                                                         ImGuiTreeNodeFlags_DefaultOpen);
                        // Channel-coverage marker. Stays on the header row so it's visible whether the user expands the
                        // region or not. See `dye_picker_compute_channel_gap_tip`.
                        {
                            int present[3][3] = {{0}};
                            for (int L = 0; L < 3; ++L)
                                for (int C = 0; C < 3; ++C)
                                    present[L][C] = (rv.layerSlot[L][C] >= 0) ? 1 : 0;
                            char gapTip[600];
                            if (dye_picker_compute_channel_gap_tip(present, gapTip, sizeof(gapTip)))
                            {
                                ImGui::SameLine();
                                ui_text_colored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "(!)");
                                if (ImGui::IsItemHovered())
                                {
                                    char fullTip[900];
                                    std::snprintf(fullTip, sizeof(fullTip),
                                                  "This submesh's shader doesn't "
                                                  "expose all dye channels.\n"
                                                  "You can still edit the channels "
                                                  "that ARE present, but missing "
                                                  "channels keep their baked default.\n"
                                                  "This limits how dark / bright you "
                                                  "can drive the rendered color.\n"
                                                  "\n"
                                                  "Gaps:\n%s",
                                                  gapTip);
                                    ui_tooltip(fullTip);
                                }
                            }
                        }
                        if (!open)
                        {
                            ImGui::PopID();
                            continue;
                        }

                        // Render one circle-swatch + popup picker per active channel via the shared
                        // renderSwatchControls helper. The helper already implements:
                        //   - bordered override-active checkbox
                        //   - manually-drawn circle swatch button (click to open popup ColorPicker3)
                        //   - auto-tick + retick on click
                        //   - persistence save on commit
                        // We pass it the channel's full cascade list so a single picker commit ripples to all 1-9
                        // captured shader tokens that share this channel suffix.
                        static const char *k_channelLabels[3] = {"R", "G", "B"};
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            if (chRows[ch].empty())
                                continue;
                            renderSwatchControls(chRows[ch].data(), static_cast<int>(chRows[ch].size()),
                                                 k_channelLabels[ch]);
                        }
                        // Hair singletons (any `_hair*` token -- `_hairDyeingColor`, `_hairDyeingScratch`, etc.). Each
                        // one gets its own row so the user can recolor hair / hair-overlay independently from a single
                        // cascade.
                        for (int idx : hairRows)
                        {
                            int single[1] = {idx};
                            renderSwatchControls(single, 1, "hair");
                        }
                        ImGui::TreePop();
                        ImGui::PopID();
                    }
                }

                // Render each region as a tree node, gated on the per-character advanced toggle. Default-off gives the
                // merchant-style flat cluster view; tick to reveal full per-region per-token control.
                if (advancedView)
                    for (auto &kv : regions)
                    {
                        auto &rv = kv.second;
                        auto &rstate = ui.regionUI[rv.stable_id];

                        // "Show only modified" filter: skip region if no swatch inside it is user-overridden. Filter
                        // runs BEFORE PushID -- continue does not need a matching PopID.
                        if (ui.showOnlyModified)
                        {
                            bool anyOverridden = false;
                            for (int L = 0; L < 5 && !anyOverridden; ++L)
                                for (int C = 0; C < 3 && !anyOverridden; ++C)
                                {
                                    const int sIdx = rv.layerSlot[L][C];
                                    if (sIdx >= 0 && dyeSlot.swatches[sIdx].override_active)
                                        anyOverridden = true;
                                }
                            for (int L = 0; L < 5 && !anyOverridden; ++L)
                                for (int sIdx : rv.layerSingletons[L])
                                    if (dyeSlot.swatches[sIdx].override_active)
                                    {
                                        anyOverridden = true;
                                        break;
                                    }
                            if (!anyOverridden)
                                for (int sIdx : rv.miscIndices)
                                    if (dyeSlot.swatches[sIdx].override_active)
                                    {
                                        anyOverridden = true;
                                        break;
                                    }
                            if (!anyOverridden)
                                continue;
                        }

                        ImGui::PushID(static_cast<int>(rv.stable_id) ^ static_cast<int>(rv.stable_id >> 32));
                        // Regions default-open: the Color Override tab is now the wrapper, so collapsing each submesh
                        // by default just hides what the user came to see.
                        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
                        char headerBuf[128];
                        // Header label: prefer the raw `_subMeshName` captured at apply time (e.g.
                        // `cd_phm_00_hel_00_0377_01`) so distinct submeshes that parse to the same friendly label
                        // remain distinguishable (the engine emits multiple variants like `..._0377_01` / `..._0377_04`
                        // that all map to "Helm #0377" under any prefix-only scheme). The leading `cd_` is stripped to
                        // save space. Falls back to "Region N" only when capture failed (Material has no parent
                        // wrapper, or wrapper has the empty-string sentinel at +0x28).
                        const char *sm = nullptr;
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int C = 0; C < 3 && !sm; ++C)
                            {
                                const int sIdx = rv.layerSlot[L][C];
                                if (sIdx >= 0 && dyeSlot.swatches[sIdx].submesh_name[0] != '\0')
                                {
                                    sm = dyeSlot.swatches[sIdx].submesh_name;
                                }
                            }
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int sIdx2 : rv.layerSingletons[L])
                                if (dyeSlot.swatches[sIdx2].submesh_name[0] != '\0')
                                {
                                    sm = dyeSlot.swatches[sIdx2].submesh_name;
                                    break;
                                }
                        if (!sm && !rv.miscIndices.empty() &&
                            dyeSlot.swatches[rv.miscIndices.front()].submesh_name[0] != '\0')
                        {
                            sm = dyeSlot.swatches[rv.miscIndices.front()].submesh_name;
                        }
                        if (sm)
                        {
                            std::snprintf(headerBuf, sizeof(headerBuf), "%s  (tpl 0x%04X)", sm, rv.tpl);
                        }
                        else
                        {
                            // No submesh name captured -- the Material has no parent SkinnedMeshMaterialWrapper (or
                            // wrapper holds the empty-string module sentinel at +0x28). Most commonly this is the
                            // player's HAIR material:
                            // SkinnedMeshHair shader (tpl 0x3ADC on v1.06) is bound via a different parent path than
                            // armor materials. Hair re-renders every player frame so the setter captures it into every
                            // slot's carrier set during the 3-second apply window. Coloring has no visible effect under
                            // full-face helms (hair occluded) -- visible on head/face slots without a helm.
                            //
                            // Detect via layer-3 tokens (hair family) and label clearly so users don't waste time
                            // picking colors on it.
                            bool hasHair = !rv.layerSingletons[3].empty();
                            if (!hasHair)
                                for (int C = 0; C < 3 && !hasHair; ++C)
                                    if (rv.layerSlot[3][C] >= 0)
                                        hasHair = true;
                            if (hasHair)
                            {
                                std::snprintf(headerBuf, sizeof(headerBuf), "[Hair -- hidden under helm]  (tpl 0x%04X)",
                                              rv.tpl);
                            }
                            else
                            {
                                std::snprintf(headerBuf, sizeof(headerBuf), "[Unnamed]  (tpl 0x%04X)", rv.tpl);
                            }
                        }
                        const bool open = ImGui::TreeNodeEx(headerBuf, flags);
                        // Channel-coverage marker -- same audit as the simple-mode picker (see
                        // `dye_picker_compute_channel_gap_tip`).
                        {
                            int present[3][3] = {{0}};
                            for (int L = 0; L < 3; ++L)
                                for (int C = 0; C < 3; ++C)
                                    present[L][C] = (rv.layerSlot[L][C] >= 0) ? 1 : 0;
                            char gapTip[600];
                            if (dye_picker_compute_channel_gap_tip(present, gapTip, sizeof(gapTip)))
                            {
                                ImGui::SameLine();
                                ui_text_colored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "(!)");
                                if (ImGui::IsItemHovered())
                                {
                                    char fullTip[900];
                                    std::snprintf(fullTip, sizeof(fullTip),
                                                  "This submesh's shader doesn't expose "
                                                  "all dye channels.\n"
                                                  "You can still edit the channels that "
                                                  "ARE present, but missing channels "
                                                  "keep their baked default -- limiting "
                                                  "how dark / bright you can drive the "
                                                  "rendered color.\n"
                                                  "\n"
                                                  "Gaps:\n%s",
                                                  gapTip);
                                    ui_tooltip(fullTip);
                                }
                            }
                        }
                        if (open)
                        {
                            // Iterate the 5 known families in a stable display order: tint, mask, detail, scratch,
                            // hair. Each family renders its R/G/B triplet (when present) followed by any per-family
                            // singleton properties (e.g. `_dyeingDetailLayerColorBlend`, `_hairDyeingColor`) under the
                            // same header so the user sees the full family together rather than scattered into a misc
                            // bucket.
                            static const int k_advLayerOrder[5] = {0, 1, 2, 4, 3};
                            for (int Li = 0; Li < 5; ++Li)
                            {
                                const int L = k_advLayerOrder[Li];
                                int idxR = rv.layerSlot[L][0];
                                int idxG = rv.layerSlot[L][1];
                                int idxB = rv.layerSlot[L][2];
                                int present = 0;
                                if (idxR >= 0)
                                    ++present;
                                if (idxG >= 0)
                                    ++present;
                                if (idxB >= 0)
                                    ++present;
                                const auto &singletons = rv.layerSingletons[L];
                                if (present == 0 && singletons.empty())
                                    continue;

                                const char *layerName = Transmog::ColorOverride::TokenTable::layer_long_name(L);

                                if (present == 1)
                                {
                                    // Single-channel layer: no link UI (nothing to link to). Show a "<layer>·<channel>"
                                    // label + swatch.
                                    const int onlyCh = (idxR >= 0) ? 0 : (idxG >= 0) ? 1 : 2;
                                    const int onlyIdx = (onlyCh == 0) ? idxR : (onlyCh == 1) ? idxG : idxB;
                                    char labelBuf[48];
                                    std::snprintf(labelBuf, sizeof(labelBuf), "%s\xC2\xB7%s", layerName,
                                                  Transmog::ColorOverride::TokenTable::channel_short_name(onlyCh));
                                    ImGui::TextUnformatted(labelBuf);
                                    ImGui::SameLine(0.0f, 6.0f);
                                    int single[1] = {onlyIdx};
                                    renderSwatchControls(single, 1, "");
                                    ImGui::NewLine();
                                }
                                else if (present >= 2)
                                {
                                    // 2 or 3 channels present: show link toggle. Linked mode drives all PRESENT
                                    // channels with one colour (merchant behaviour); unlinked exposes a per-channel row
                                    // for each present channel.
                                    char layerHdrBuf[48];
                                    char chList[8];
                                    std::size_t chLen = 0;
                                    if (idxR >= 0 && chLen + 1 < sizeof(chList))
                                        chList[chLen++] = 'R';
                                    if (idxG >= 0 && chLen + 1 < sizeof(chList))
                                    {
                                        if (chLen)
                                            chList[chLen++] = '+';
                                        chList[chLen++] = 'G';
                                    }
                                    if (idxB >= 0 && chLen + 1 < sizeof(chList))
                                    {
                                        if (chLen)
                                            chList[chLen++] = '+';
                                        chList[chLen++] = 'B';
                                    }
                                    chList[chLen] = '\0';
                                    std::snprintf(layerHdrBuf, sizeof(layerHdrBuf), "%s \xC2\xB7 %s", layerName,
                                                  chList);
                                    ImGui::TextUnformatted(layerHdrBuf);
                                    ImGui::SameLine(0.0f, 6.0f);

                                    bool &linked = rstate.linkRgb[L];
                                    ImGui::PushID(L * 31 + 7);
                                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                                    ImGui::Checkbox("link##lnk", &linked);
                                    ImGui::PopStyleVar();
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::BeginTooltip();
                                        ImGui::TextUnformatted("When ON: pick one colour, all "
                                                               "present R/G/B channel-suffix\n"
                                                               "Properties get it (merchant "
                                                               "behaviour).\nUntick to edit "
                                                               "each channel independently.");
                                        ImGui::EndTooltip();
                                    }
                                    ImGui::SameLine(0.0f, 6.0f);

                                    if (linked)
                                    {
                                        int cascade[3];
                                        int n = 0;
                                        if (idxR >= 0)
                                            cascade[n++] = idxR;
                                        if (idxG >= 0)
                                            cascade[n++] = idxG;
                                        if (idxB >= 0)
                                            cascade[n++] = idxB;
                                        renderSwatchControls(cascade, n, "");
                                    }
                                    else
                                    {
                                        int order[3] = {idxR, idxG, idxB};
                                        bool firstShown = true;
                                        for (int c = 0; c < 3; ++c)
                                        {
                                            if (order[c] < 0)
                                                continue;
                                            if (!firstShown)
                                                ImGui::SameLine(0.0f, 8.0f);
                                            firstShown = false;
                                            int single[1] = {order[c]};
                                            renderSwatchControls(
                                                single, 1, Transmog::ColorOverride::TokenTable::channel_short_name(c));
                                        }
                                    }
                                    ImGui::PopID();
                                    ImGui::NewLine();
                                }

                                // Family singletons: tokens classified into this layer but with no R/G/B suffix
                                // (channel=-1). Show the actual shader-property name so it isn't confused with the
                                // R/G/B triplet.
                                for (int sIdx : singletons)
                                {
                                    auto &sw = dyeSlot.swatches[sIdx];
                                    const char *propName =
                                        Transmog::ColorOverride::TokenTable::token_label_for(sw.token_id);
                                    char nameBuf[64];
                                    if (propName && propName[0])
                                    {
                                        std::snprintf(nameBuf, sizeof(nameBuf), "%s", propName);
                                    }
                                    else
                                    {
                                        std::snprintf(nameBuf, sizeof(nameBuf), "%s \xC2\xB7 0x%04X", layerName,
                                                      sw.token_id & 0xFFFFu);
                                    }
                                    ImGui::TextUnformatted(nameBuf);
                                    ImGui::SameLine(0.0f, 6.0f);
                                    int single[1] = {sIdx};
                                    renderSwatchControls(single, 1, "");
                                    ImGui::NewLine();
                                }
                            }
                            // Tokens whose prefix didn't match any known family (layer == -1). Show the shader-property
                            // name when the interner captured it -- only fall back to the "misc 0xXXXX" hex label when
                            // we have no name at all. A resolvable name is more useful than a bare hex blob.
                            for (int idx : rv.miscIndices)
                            {
                                auto &mSw = dyeSlot.swatches[idx];
                                const char *propName =
                                    Transmog::ColorOverride::TokenTable::token_label_for(mSw.token_id);
                                char labelBuf[64];
                                if (propName && propName[0])
                                {
                                    std::snprintf(labelBuf, sizeof(labelBuf), "%s", propName);
                                }
                                else
                                {
                                    std::snprintf(labelBuf, sizeof(labelBuf), "misc 0x%04X", mSw.token_id & 0xFFFFu);
                                }
                                ImGui::TextUnformatted(labelBuf);
                                ImGui::SameLine(0.0f, 6.0f);
                                int single[1] = {idx};
                                renderSwatchControls(single, 1, "");
                                ImGui::NewLine();
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
            }
        }
    }

} // namespace Transmog
