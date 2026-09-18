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

    void draw_color_override_tab_body(
        std::size_t slot,
        std::size_t detected,
        bool detected_ready,
        SlotUIState &ui,
        color_override::DyeSlot &dye_slot
    )
    {
        if (!s_co_auto_triggered[slot] && detected == 0 && !Transmog::color_override::reinit::any_slot_reinit_active())
        {
            Transmog::flag_enabled().store(true, std::memory_order_relaxed);
            Transmog::color_override::reinit::start_slot_reinit_once(static_cast<int>(slot));
            s_co_auto_triggered[slot] = true;
        }

        // Per-slot color override picker
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
        ImGui::BeginDisabled(!detected_ready);
        if (ImGui::Checkbox("Dye##dye_on", &dye_slot.slot_enabled))
        {
            // The Dye master toggle gates the substitution path (dye_override.cpp: !slot_enabled -> return false,
            // engine default writes through). To make the toggle visually take effect we need the engine to actually
            // write again; bare manual_apply_slot short-circuits as a same-state apply. Route through commit-retick so
            // the engine tears down and re-instantiates the carrier, producing fresh writes that respect the new
            // slot_enabled value.
            if (!Transmog::color_override::reinit::any_slot_reinit_active())
            {
                flag_enabled().store(true, std::memory_order_relaxed);
                Transmog::color_override::reinit::schedule_color_commit_retick(static_cast<int>(slot));
            }
            Transmog::dye_dirty().store(true, std::memory_order_release);
        }
        ImGui::EndDisabled();
        if (!detected_ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                "No apply yet - swatches unknown.\n"
                "Run Re-init to capture the slot's dye\nswatches before toggling the master Dye\nswitch."
            );
            ImGui::EndTooltip();
        }

        // Per-slot single-pass swatch re-init. Drives one clear + apply cycle (~1.5s) and keeps whatever rows were
        // captured. Use when the swatch list is empty and you'd otherwise untick-retick by hand. One pass is enough for
        // the common case.
        ImGui::SameLine(0.0f, 6.0f);
        const bool reinit_active = Transmog::color_override::reinit::is_slot_reinit_active(static_cast<int>(slot));
        if (reinit_active)
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
            const bool reinit_clicked = ImGui::SmallButton("Re-init##sw_reinit");
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(
                    "Single clear + apply (~1.5s) to capture\n"
                    "the slot's swatches. Use when the list\n"
                    "looks empty or stale.\n\n"
                    "Replaces the manual 'untick / retick'\nloop - you can leave this slot alone\nwhile it runs."
                );
                ImGui::EndTooltip();
            }
            if (reinit_clicked)
            {
                flag_enabled().store(true, std::memory_order_relaxed);
                Transmog::color_override::reinit::start_slot_reinit_once(static_cast<int>(slot));
            }

            // Reset slot button stays enabled even when `detected == 0` so the user can recover from stale rows in
            // exactly the state where Re-init would otherwise stall.
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            const bool top_reset_clicked = ImGui::SmallButton("Reset slot##sw_reset_top");
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(
                    "Wipe ALL captured swatches for THIS slot,\n"
                    "clear the post-reinit lock + frozen-hidden\n"
                    "rows, then trigger a fresh apply.\n\n"
                    "Use when the slot is stuck with stale rows\n"
                    "(e.g. JSON-restored entries that no longer\n"
                    "match the current item) and Re-init cannot\nmake progress."
                );
                ImGui::EndTooltip();
            }
            if (top_reset_clicked)
            {
                Transmog::color_override::swatch_table::wipe_swatch_table_for_slot(static_cast<int>(slot));
                Transmog::color_override::swatch_table::clear_dye_state_for_slot(static_cast<int>(slot));
                // Drop any persisted overrides for this slot that are queued via the setter's pending map - otherwise
                // the next engine write would re-substitute the saved color on top of the freshly-wiped row.
                Transmog::color_override::pending_overrides::clear_slot(static_cast<int>(slot));
                // Reset is a pending change: the JSON still has the old swatch rows. Flip dirty so the top "Save *"
                // button surfaces, but do not auto-write JSON - user commits explicitly.
                Transmog::dye_dirty().store(true, std::memory_order_release);
                // Re-render the slot. Reset changes only COLOR state and leaves the transmog target untouched, so a
                // plain manual_apply_slot hits the `targetId == prev_id` early-out in apply_single_slot_transmog and
                // returns with no rebuild, which leaves a wiped slot on its old override color.
                //
                // force_apply_pending is the documented bypass for exactly this case (the dye popup sets it for the
                // same reason). It is read-and-cleared by the apply, and it costs exactly ONE rebuild.
                //
                // Deliberately NOT schedule_color_commit_retick, which is what "Revert to default" uses: that is a
                // two-phase cycle (TeardownApply unticks the slot, TeardownWait, then CarrierApply re-applies), so it
                // empties the slot for over a second and reads as a double remount. Revert needs those phases to make
                // the engine re-emit its natural colors; Reset does not, because the swatch table is wiped and the
                // single rebuild below re-captures it.
                //
                // Not gated on s_auto_apply: this is an explicit button press, and re-capturing is the whole point of
                // the button (see its tooltip). Auto-apply governs whether EDITS apply on their own.
                force_apply_pending()[static_cast<std::size_t>(slot)] = true;
                flag_enabled().store(true, std::memory_order_relaxed);
                manual_apply_slot(slot);
            }
        }

        // Auto-detected swatch count drives whether to show the swatch UI. Pre-apply (detected==0) we render nothing
        // here so the disabled-Dye-checkbox tooltip can explain the state without an empty CollapsingHeader pushing the
        // Apply button down.
        if (detected_ready)
        {
            // Collapse the per-slot swatch UI behind a CollapsingHeader so users who only want to swap items do not get
            // a wall of dye controls that push the Apply button down. ImGui remembers per-header state across
            // frames. The body below sits one indent level shallower than strict nesting dictates, which keeps the
            // wrap purely additive.
            char dye_hdr_buf[160];
            // Count rows with a user-picked color. The badge shows "M rows (N colored)" where M is the full captured
            // set and N is how many the user has explicitly picked a color for.
            std::size_t colored = 0;
            for (const auto &sw : dye_slot.swatches)
            {
                if (sw.override_active)
                    ++colored;
            }
            const bool has_override = colored > 0;
            // Mid-retick badge (b): show "(applying...)" while a color-commit retick cycle is in-flight on this slot.
            // The cycle takes ~1.9s; this badge tells the user the engine has not finished yet.
            const bool reticking =
                Transmog::color_override::reinit::is_color_commit_retick_active(static_cast<int>(slot));
            // Master-disabled badge: if the user unticked the
            // Dye checkbox, the swatch list is inert. Surface that clearly in the closed header.
            const bool master_disabled = !dye_slot.slot_enabled;
            // Plain text label - the popup tab is the container, so there is no CollapsingHeader ID-stability concern.
            if (colored > 0)
            {
                std::snprintf(
                    dye_hdr_buf,
                    sizeof(dye_hdr_buf),
                    "%zu rows (%zu colored)%s%s%s",
                    detected,
                    colored,
                    has_override ? " *" : "",
                    reticking ? " (applying)" : "",
                    master_disabled ? " (off)" : ""
                );
            }
            else
            {
                std::snprintf(
                    dye_hdr_buf,
                    sizeof(dye_hdr_buf),
                    "%zu rows%s%s",
                    detected,
                    reticking ? " (applying)" : "",
                    master_disabled ? " (off)" : ""
                );
            }
            ui_text("%s", dye_hdr_buf);
            ImGui::Separator();
            {
                auto clamp_byte = [](float v) -> std::uint8_t
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
                //   - For triplet layers (R/G/B suffix) show one color picker by default with the R/G/B channels
                //     linked, mirroring merchant "Pure X" semantics. Untick "link" to expose three independent
                //     per-channel pickers.
                //   - Each row is labelled, so no "Show all" noise-hiding filter is needed. Rows still waiting for
                //     their first capture (default_captured == false) are skipped silently; they are inert until the
                //     engine writes them.

                // Pass 1: group swatch indices by submesh, then by layer. Each region's `layer_slot[layer][channel]`
                // holds the swatch index in dye_slot.swatches, or -1.
                //
                // Layers: 0=tint, 1=mask, 2=detail, 3=hair, 4=scratch. `layer_slot[L][0..2]` = R/G/B siblings for that
                // family. `layer_singletons[L]` = ch=-1 tokens (e.g. `_dyeingDetailLayerColorBlend`,
                // `_hairDyeingColor`) rendered under the same family header. `misc_indices` = truly unknown tokens
                // (layer=-1).
                struct RegionView
                {
                    std::uint64_t stable_id = 0;
                    std::uint16_t tpl = 0;
                    int layer_slot[5][3]{};
                    std::vector<int> layer_singletons[5];
                    std::vector<int> misc_indices;
                };
                auto init_region = [](RegionView &rv) -> void
                {
                    for (int L = 0; L < 5; ++L)
                        for (int C = 0; C < 3; ++C)
                            rv.layer_slot[L][C] = -1;
                };

                std::map<std::uint64_t, RegionView> regions;
                // Slot-level "recolor all" cascade list: every visible swatch in the slot.
                std::vector<int> slot_all_indices;
                slot_all_indices.reserve(detected);

                for (std::size_t s = 0; s < detected; ++s)
                {
                    auto &sw = dye_slot.swatches[s];
                    // Skip rows that have not captured a default yet; picking on them would write zeros.
                    // (override_active rows always pass below.)
                    if (!sw.default_captured && !sw.override_active)
                        continue;

                    slot_all_indices.push_back(static_cast<int>(s));

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
                        init_region(rv);
                        rv.stable_id = sid;
                        rv.tpl = sw.template_id;
                    }
                    const int layer = Transmog::color_override::token_table::token_layer(sw.token_id);
                    const int ch = Transmog::color_override::token_table::token_channel(sw.token_id);
                    if (layer >= 0 && layer <= 4)
                    {
                        if (ch >= 0 && ch <= 2)
                            rv.layer_slot[layer][ch] = static_cast<int>(s);
                        else
                            rv.layer_singletons[layer].push_back(static_cast<int>(s));
                    }
                    else
                    {
                        rv.misc_indices.push_back(static_cast<int>(s));
                    }
                }

                // Render swatch sub-block: override toggle, picker, tooltip. Reused for both linked and per-channel
                // rendering. `cascade_idx[]` lists the indices that receive the color edit. The first index is the
                // "primary" one that the row displays.
                auto render_swatch_controls =
                    [&](const int *cascade_idx, int cascade_count, const char *channel_label) -> void
                {
                    if (cascade_count <= 0 || cascade_idx[0] < 0)
                        return;
                    auto &primary = dye_slot.swatches[cascade_idx[0]];
                    ImGui::PushID(cascade_idx[0] + 200);

                    if (channel_label && channel_label[0])
                    {
                        ImGui::TextUnformatted(channel_label);
                        ImGui::SameLine(0.0f, 4.0f);
                    }

                    const bool prev_active = primary.override_active;
                    bool active = prev_active;
                    // The border separates the override toggle from the swatch next to it.
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    const bool toggled = ImGui::Checkbox("##sw_on", &active);
                    ImGui::PopStyleVar();
                    if (toggled)
                    {
                        for (int k = 0; k < cascade_count; ++k)
                        {
                            int idx = cascade_idx[k];
                            if (idx < 0)
                                continue;
                            auto &row = dye_slot.swatches[idx];
                            // Skip tick-on for rows whose def was never captured - otherwise we'd set row.r/g/b =
                            // (0,0,0) and the substitute path would write black for that token, shifting the rendered
                            // color away from the engine's natural blend. Affects assets whose engine pipeline emits
                            // writes for only a subset of the 9-prop chord (e.g. orcumer armours ship _tintColor +
                            // _detail but no _dyeingColorMask). Tick-off (active=false) always runs so the user can
                            // always un-override.
                            if (active && !row.default_captured)
                                continue;
                            row.override_active = active;
                            if (active && !prev_active)
                            {
                                row.r = row.def_r;
                                row.g = row.def_g;
                                row.b = row.def_b;
                            }
                            // Mirror to pending_overrides so the slot-agnostic substitute path picks up this row's new
                            // state. Active=> write current RGB into pending; inactive=>erase so substitute stops
                            // firing for this (submesh, token).
                            if (active)
                                mirror_override_to_pending(
                                    static_cast<int>(slot),
                                    static_cast<std::size_t>(idx),
                                    row.r,
                                    row.g,
                                    row.b
                                );
                            else
                                erase_override_from_pending(static_cast<int>(slot), static_cast<std::size_t>(idx));
                        }
                        // Override-active checkbox toggle is a color change (rows go default to user override or
                        // vice-versa), so retick unconditionally with the same semantics as the picker commits below.
                        if (!Transmog::color_override::reinit::any_slot_reinit_active())
                        {
                            flag_enabled().store(true, std::memory_order_relaxed);
                            Transmog::color_override::reinit::schedule_color_commit_retick(static_cast<int>(slot));
                        }
                        // Tick toggle flips override_active, which also flips whether get_persistable_state includes
                        // the row. The picker applies overrides live but does NOT write JSON - dye_dirty lights up the
                        // top-level "Save *" button so the user commits to disk explicitly. Preset switch with
                        // dirty=true discards pending edits.
                        Transmog::dye_dirty().store(true, std::memory_order_release);
                    }
                    ImGui::SameLine(0.0f, 2.0f);

                    const bool show_user = active;
                    float rgb[3];
                    if (show_user)
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
                    const ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                    const ImVec2 sw_center(cursor_pos.x + diameter * 0.5f, cursor_pos.y + diameter * 0.5f);
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

                    // Default-color reference dot next to the active picker swatch. Shows the engine-captured default
                    // (def_r/g/b) so the user has a quick visual anchor for "what was the asset's original color?" -
                    // useful when dialing in a custom color or deciding whether to revert an override. Display-only,
                    // non-interactive. Skipped if no default has been captured yet (placeholders pre-promote or rows
                    // never touched by the engine).
                    if (primary.default_captured)
                    {
                        ImGui::SameLine(0.0f, 4.0f);
                        const float ref_diam = diameter * 0.70f;
                        const ImVec2 ref_cursor = ImGui::GetCursorScreenPos();
                        // Center the smaller circle vertically against the larger picker swatch.
                        const float y_off = (diameter - ref_diam) * 0.5f;
                        const ImVec2 ref_center(ref_cursor.x + ref_diam * 0.5f, ref_cursor.y + y_off + ref_diam * 0.5f);
                        const float ref_radius = ref_diam * 0.5f - 1.0f;
                        const ImVec4
                            ref_col_vec(primary.def_r / 255.0f, primary.def_g / 255.0f, primary.def_b / 255.0f, 1.0f);
                        const ImU32 ref_col_u = ImGui::ColorConvertFloat4ToU32(ref_col_vec);
                        ImGui::Dummy(ImVec2(ref_diam, diameter));
                        const bool ref_hovered = ImGui::IsItemHovered();
                        sw_dl->AddCircleFilled(ref_center, ref_radius, ref_col_u, 24);
                        sw_dl->AddCircle(ref_center, ref_radius, sw_borderU, 24, 1.0f);
                        if (ref_hovered)
                        {
                            char ref_tip[48];
                            std::snprintf(
                                ref_tip,
                                sizeof(ref_tip),
                                "Asset default #%02X%02X%02X",
                                primary.def_r,
                                primary.def_g,
                                primary.def_b
                            );
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(ref_tip);
                            ImGui::EndTooltip();
                        }
                    }

                    if (sw_clicked)
                    {
                        if (!show_user)
                        {
                            for (int k = 0; k < cascade_count; ++k)
                            {
                                int idx = cascade_idx[k];
                                if (idx < 0)
                                    continue;
                                auto &row = dye_slot.swatches[idx];
                                // Same gate as the checkbox path: rows without a captured def must stay un-overridden,
                                // else substitute writes 0,0,0 and the rendered color shifts.
                                if (!row.default_captured)
                                    continue;
                                row.override_active = true;
                                row.r = row.def_r;
                                row.g = row.def_g;
                                row.b = row.def_b;
                                // Mirror to pending so the substitute path agrees with the row's new default RGB.
                                mirror_override_to_pending(
                                    static_cast<int>(slot),
                                    static_cast<std::size_t>(idx),
                                    row.r,
                                    row.g,
                                    row.b
                                );
                            }
                            if (!Transmog::color_override::reinit::any_slot_reinit_active())
                            {
                                flag_enabled().store(true, std::memory_order_relaxed);
                                // Tear-down + reapply: engine short-circuits a same-state single apply, so we toggle
                                // m.active off->on around a wait window. See dye_override.cpp SlotReinitState
                                // (CommitRetick). Unconditional on swatch click (override flips on); no s_auto_apply
                                // gate.
                                Transmog::color_override::reinit::schedule_color_commit_retick(static_cast<int>(slot));
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
                        float picker_rgb[3] = {
                            primary.r / 255.0f,
                            primary.g / 255.0f,
                            primary.b / 255.0f,
                        };
                        if (ImGui::ColorPicker3(
                                "##sw_pick",
                                picker_rgb,
                                ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview
                            ))
                        {
                            const std::uint8_t r = clamp_byte(picker_rgb[0]);
                            const std::uint8_t g = clamp_byte(picker_rgb[1]);
                            const std::uint8_t b = clamp_byte(picker_rgb[2]);
                            for (int k = 0; k < cascade_count; ++k)
                            {
                                int idx = cascade_idx[k];
                                if (idx < 0)
                                    continue;
                                auto &row = dye_slot.swatches[idx];
                                row.r = r;
                                row.g = g;
                                row.b = b;
                                // Mirror to pending so the substitute path picks up the user's edit on the next engine
                                // write.
                                mirror_override_to_pending(
                                    static_cast<int>(slot),
                                    static_cast<std::size_t>(idx),
                                    r,
                                    g,
                                    b
                                );
                            }
                            // The picker applies overrides live but does NOT write JSON; dye_dirty lights up the
                            // top-level "Save *" button so the user commits to disk explicitly. Preset switch with
                            // dirty=true discards pending edits.
                            Transmog::dye_dirty().store(true, std::memory_order_release);
                            // Trigger a single-pass tear-down + reapply so the engine re-builds the carrier matInst
                            // with the new color. Fires unconditionally on every color commit - independent of the
                            // "Instant Apply" checkbox which governs hover/pick-apply only. Coalesces with any
                            // in-flight retick so a 60Hz drag fires ~1 retick per ~1.9s cycle, not 60.
                            if (!Transmog::color_override::reinit::any_slot_reinit_active())
                            {
                                flag_enabled().store(true, std::memory_order_relaxed);
                                Transmog::color_override::reinit::schedule_color_commit_retick(static_cast<int>(slot));
                            }
                        }
                        ImGui::EndPopup();
                    }
                    if (sw_hovered)
                    {
                        ImGui::BeginTooltip();
                        const char *token_name =
                            Transmog::color_override::token_table::token_label_for(primary.token_id);
                        char token_buf[64];
                        if (token_name)
                        {
                            std::snprintf(token_buf, sizeof(token_buf), "%s (0x%04X)", token_name, primary.token_id);
                        }
                        else
                        {
                            std::snprintf(token_buf, sizeof(token_buf), "0x%04X", primary.token_id);
                        }
                        char tip_buf[256];
                        if (cascade_count > 1)
                        {
                            std::snprintf(
                                tip_buf,
                                sizeof(tip_buf),
                                "Linked R/G/B  %s\n"
                                "submesh 0x%016llX  tpl 0x%04X\nasset def #%02X%02X%02X  blend=%u/255",
                                token_buf,
                                static_cast<unsigned long long>(primary.submesh_stable_id),
                                static_cast<unsigned>(primary.template_id),
                                primary.def_r,
                                primary.def_g,
                                primary.def_b,
                                primary.def_a
                            );
                        }
                        else
                        {
                            std::snprintf(
                                tip_buf,
                                sizeof(tip_buf),
                                "%s\nsubmesh 0x%016llX  tpl 0x%04X\nasset def #%02X%02X%02X  blend=%u/255",
                                token_buf,
                                static_cast<unsigned long long>(primary.submesh_stable_id),
                                static_cast<unsigned>(primary.template_id),
                                primary.def_r,
                                primary.def_g,
                                primary.def_b,
                                primary.def_a
                            );
                        }
                        ImGui::TextUnformatted(tip_buf);
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                };

                // Batch recolor helper. Renders a clickable circle and on click opens a picker popup. Each value change
                // cascades the chosen RGB to every index in `cascade`, ticking `override_active = true`.
                //
                // display_color is derived from cascade[0] each frame (current r/g/b if override active, else captured
                // def). This is the same trick the per-swatch picker uses: writes propagate through cascade[0] to
                // display_color next frame, so picker drag stays smooth. If we re-seeded from a fixed color (e.g. the
                // cluster's def) each frame, the wheel would snap back every frame and drag would jitter.
                auto recolor_batch = [&](const char *id_tag, const std::vector<int> &cascade) -> void
                {
                    if (cascade.empty())
                        return;
                    const auto &first = dye_slot.swatches[cascade[0]];
                    const ImVec4 display_color =
                        first.override_active
                            ? ImVec4(first.r / 255.0f, first.g / 255.0f, first.b / 255.0f, 1.0f)
                            : ImVec4(first.def_r / 255.0f, first.def_g / 255.0f, first.def_b / 255.0f, 1.0f);

                    ImGui::PushID(id_tag);
                    const float b_diameter = ImGui::GetFrameHeight();
                    const ImVec2 b_cursor = ImGui::GetCursorScreenPos();
                    const ImVec2 bCenter(b_cursor.x + b_diameter * 0.5f, b_cursor.y + b_diameter * 0.5f);
                    const float b_radius = b_diameter * 0.5f - 1.0f;
                    const ImU32 b_col_u = ImGui::ColorConvertFloat4ToU32(display_color);
                    const ImU32 b_border_u = ImGui::GetColorU32(ImGuiCol_Border);
                    const bool b_clicked = ImGui::InvisibleButton("##rb_btn", ImVec2(b_diameter, b_diameter));
                    ImDrawList *b_dl = ImGui::GetWindowDrawList();
                    b_dl->AddCircleFilled(bCenter, b_radius, b_col_u, 32);
                    b_dl->AddCircle(bCenter, b_radius, b_border_u, 32, 1.0f);
                    if (b_clicked)
                        ImGui::OpenPopup("##rb_pop");
                    if (ImGui::BeginPopup("##rb_pop"))
                    {
                        float prgb[3] = {
                            display_color.x,
                            display_color.y,
                            display_color.z,
                        };
                        if (ImGui::ColorPicker3(
                                "##rb_pick",
                                prgb,
                                ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview
                            ))
                        {
                            const std::uint8_t pr = clamp_byte(prgb[0]);
                            const std::uint8_t pg = clamp_byte(prgb[1]);
                            const std::uint8_t pb = clamp_byte(prgb[2]);
                            for (int idx : cascade)
                            {
                                if (idx < 0)
                                    continue;
                                auto &row = dye_slot.swatches[idx];
                                row.override_active = true;
                                row.r = pr;
                                row.g = pg;
                                row.b = pb;
                                // Mirror to pending so the substitute path picks up the recolor-all edit.
                                mirror_override_to_pending(
                                    static_cast<int>(slot),
                                    static_cast<std::size_t>(idx),
                                    pr,
                                    pg,
                                    pb
                                );
                            }
                            if (!Transmog::color_override::reinit::any_slot_reinit_active())
                            {
                                flag_enabled().store(true, std::memory_order_relaxed);
                                // Unconditional retick on batch-recolor commit; no s_auto_apply gate (matches per-row
                                // picker semantics).
                                Transmog::color_override::reinit::schedule_color_commit_retick(static_cast<int>(slot));
                            }
                            // Mark dirty for the explicit Save commit - see the matching call site in
                            // render_swatch_controls for the full rationale.
                            Transmog::dye_dirty().store(true, std::memory_order_release);
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                };

                // Slot-level "Recolor all" + auto-clusters
                // Single picker drives every visible swatch in the slot. Lets users do "make this whole helm red"
                // without touching individual rows.
                ImGui::TextUnformatted("Recolor all:");
                ImGui::SameLine(0.0f, 6.0f);
                recolor_batch("slot_all", slot_all_indices);

                // "Reset slot": escape hatch for cross-slot bleed (e.g. Kairos cloak + boots sharing one material means
                // whichever slot applies first absorbs both submeshes' rows; the other slot is permanently empty).
                // Wipes the swatch table, clears freeze
                // + carrier state, then triggers a fresh apply
                // for this slot. User picks for this slot are lost; cross-slot pollution is gone.
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                const bool reset_clicked = ImGui::SmallButton("Reset slot##sw_reset");
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(
                        "Wipe ALL captured swatches for THIS slot.\n"
                        "Use when cross-slot bleed has misattributed\n"
                        "rows from another transmog item (common with\n"
                        "Kairos and other shared-material sets).\n\n"
                        "Triggers a fresh apply on this slot, which\n"
                        "re-captures cleanly. Loses any colors you have\npicked for this slot."
                    );
                    ImGui::EndTooltip();
                }
                if (reset_clicked)
                {
                    Transmog::color_override::swatch_table::wipe_swatch_table_for_slot(static_cast<int>(slot));
                    Transmog::color_override::swatch_table::clear_dye_state_for_slot(static_cast<int>(slot));
                    Transmog::color_override::pending_overrides::clear_slot(static_cast<int>(slot));
                    Transmog::dye_dirty().store(true, std::memory_order_release);
                    // Same re-render path as the Reset slot button at the top of this panel - see the comment there
                    // for why manual_apply_slot needs the force flag, and why the commit-retick is not used here.
                    force_apply_pending()[static_cast<std::size_t>(slot)] = true;
                    flag_enabled().store(true, std::memory_order_relaxed);
                    manual_apply_slot(slot);
                }

                // "Revert": soft reset that keeps the captured swatch list intact (no Re-init needed afterwards), but
                // flips every row's override_active=false so the engine's defaults render. Distinct from "Reset slot"
                // which wipes the captured list entirely.
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                const bool revert_clicked = ImGui::SmallButton("Revert to default##sw_revert");
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(
                        "Revert ALL user overrides on THIS slot\n"
                        "back to engine defaults. Keeps the\n"
                        "captured swatch list intact (no Re-init\n"
                        "needed afterwards).\n\n"
                        "Use to compare your dye picks against\n"
                        "the original asset colors, or to undo\n"
                        "a slot's color edits without losing\nthe captured row structure."
                    );
                    ImGui::EndTooltip();
                }
                if (revert_clicked)
                {
                    for (auto &sw : dye_slot.swatches)
                        sw.override_active = false;
                    // Drop the slot's queued pending-overrides map. pending_overrides holds the JSON-loaded user picks
                    // and gets consulted on every successful lookup_or_insert - when an engine write hits a matched
                    // row it calls set_override_active(true). Without clearing here, the schedule_color_commit_retick
                    // below would fire engine writes that immediately re-enable every JSON override we just cleared.
                    // With clearing, the retick
                    // sees empty pending -> override stays off
                    // -> substitute bails -> engine natural
                    // color flows. Re-loading the preset (or switching away without saving) restores pending_overrides
                    // from JSON, so the revert stays a pending change until Save commits.
                    Transmog::color_override::pending_overrides::clear_slot(static_cast<int>(slot));
                    if (!Transmog::color_override::reinit::any_slot_reinit_active())
                    {
                        flag_enabled().store(true, std::memory_order_relaxed);
                        Transmog::color_override::reinit::schedule_color_commit_retick(static_cast<int>(slot));
                    }
                    Transmog::dye_dirty().store(true, std::memory_order_release);
                }

                // Auto-detected clusters: rows that share the same (layer, def_r, def_g, def_b). One picker drives all
                // rows in the cluster. Wrapped in a collapsible tree node so users who do not want the suggestions can
                // fold them away without us silently hiding them. Default-open since the suggestions are the whole
                // point of the cluster bar. Advanced view toggle, per-character preference. Default off: render a
                // merchant-like flat list, one picker per UNIQUE def color (cluster of >=1 member). Tick to reveal
                // per-region per-token trees with full shader-property granularity.
                bool advanced_view = Transmog::color_override::dye_advanced_view_get();
                if (ImGui::Checkbox("Advanced view##dye_adv", &advanced_view))
                {
                    Transmog::color_override::dye_advanced_view_set(advanced_view);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(
                        "Show per-region per-token trees with full\n"
                        "shader-property granularity beneath the\n"
                        "merchant-like flat picker list.\n\n"
                        "Default off - most users get the same UX\n"
                        "as the merchant dye UI: one picker per\n"
                        "distinct asset-default color. Tick this\n"
                        "only if you need to dye individual tokens\n(e.g. tint vs. mask vs. detail layers\nseparately)."
                    );
                    ImGui::EndTooltip();
                }

                // "Show only modified" filter: per-slot session toggle that hides rows / clusters / regions whose
                // `override_active` is false. Useful when an item exposes 30+ swatches and the user wants to revisit
                // only the colors they actually changed.
                {
                    auto &ui_slot = s_slot_ui[slot];
                    ImGui::SameLine();
                    ImGui::Checkbox("Modified only##dye_mod", &ui_slot.show_only_modified);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(
                            "Hide rows / regions whose color is\n"
                            "still at the captured engine default.\n"
                            "Untick to see every swatch this slot\nexposed during apply."
                        );
                        ImGui::EndTooltip();
                    }
                }

                ImGui::Separator();

                if (!advanced_view)
                {
                    // Merchant-style region-channel picker. Per region: 1-3 ColorPicker3s, one per mask-texture channel
                    // suffix (R/G/B). Each picker cascade-writes its color to every captured swatch sharing that
                    // channel-suffix family in this region - mirroring the engine merchant's `sub_14274A3C0` chord (9
                    // token writes per channel: dyeingColorMask + detail layers + tintColor + 5 mask-overlay layers).
                    //
                    // The R/G/B suffixes are NOT R/G/B color components - they are the engine's 3 mask-texture
                    // channels, each holding a full RGB color for one physical region of the mesh. Materials with
                    // masks active on only some channels get fewer pickers (channels with no captured rows are hidden).
                    const bool filter_mod = s_slot_ui[slot].show_only_modified;
                    for (auto &kv : regions)
                    {
                        auto &rv = kv.second;

                        // Walk swatches in this region and bucket by channel_kind (0=R, 1=G, 2=B, else=-1). Misc tokens
                        // with no detectable channel are skipped here; they show in Advanced view for power users.
                        //
                        // Layer iteration order matches the engine merchant function sub_14274A3C0. The merchant writes
                        // a 9-prop "chord" per channel in this priority order:
                        //   0  _dyeingColorMask         <- L=1
                        //   1  _dyeingDetailLayerColorMask <- L=2
                        //   2..7 _detail* / _dyeingCustom* mask layers
                        //   8  _tintColor              <- L=0
                        // `primary` in render_swatch_controls is cascade_idx[0], so whatever lands first in ch_rows[ch]
                        // drives the picker's displayed reference color. PC armor ships _dyeingColorMask (L=1) so the
                        // picker shows the merchant's primary base; orcumer / mob assets ship only detail+tint, so the
                        // picker falls back to _dyeingDetailLayerColorMask (L=2).
                        //   L=1 (mask)    first
                        //   L=2 (detail)  second
                        //   L=0 (tint)    third
                        //   L=4 (scratch) fourth
                        //   L=3 (hair)    fifth
                        // Cascade still writes to every captured token sharing the channel suffix - only the displayed
                        // reference changes. Per-layer singletons (ch=-1 tokens like `_dyeingDetailLayerColorBlend`)
                        // never make it into the merchant chord - they surface only in Advanced view.
                        std::vector<int> ch_rows[3];
                        static const int layer_order[5] = {1, 2, 0, 4, 3};
                        for (int Li = 0; Li < 5; ++Li)
                        {
                            const int L = layer_order[Li];
                            for (int C = 0; C < 3; ++C)
                            {
                                const int s_idx = rv.layer_slot[L][C];
                                if (s_idx < 0)
                                    continue;
                                ch_rows[C].push_back(s_idx);
                            }
                        }

                        // Hair singletons surfaced in simple view too - merchants do not ship a hair picker, but users
                        // want one-click hair recolor from the same panel as armor (no need to flip into Advanced).
                        const auto &hair_rows = rv.layer_singletons[3];
                        const bool any_channel =
                            !ch_rows[0].empty() || !ch_rows[1].empty() || !ch_rows[2].empty() || !hair_rows.empty();
                        if (!any_channel)
                            continue;

                        // "Modified only" filter: skip region if no channel / hair row has an override.
                        if (filter_mod)
                        {
                            bool any_overridden = false;
                            for (int ch = 0; ch < 3 && !any_overridden; ++ch)
                                for (int idx : ch_rows[ch])
                                    if (dye_slot.swatches[idx].override_active)
                                    {
                                        any_overridden = true;
                                        break;
                                    }
                            if (!any_overridden)
                                for (int idx : hair_rows)
                                    if (dye_slot.swatches[idx].override_active)
                                    {
                                        any_overridden = true;
                                        break;
                                    }
                            if (!any_overridden)
                                continue;
                        }

                        // Region header: prefer captured submesh name; fall back to hair-aware label or "Region N"
                        // exactly as the Advanced-mode header does (kept consistent so a user toggling views does not
                        // see different labels for the same region).
                        const char *sm = nullptr;
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int C = 0; C < 3 && !sm; ++C)
                            {
                                const int s_idx = rv.layer_slot[L][C];
                                if (s_idx >= 0 && dye_slot.swatches[s_idx].submesh_name[0] != '\0')
                                    sm = dye_slot.swatches[s_idx].submesh_name;
                            }
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int s_idx2 : rv.layer_singletons[L])
                                if (dye_slot.swatches[s_idx2].submesh_name[0] != '\0')
                                {
                                    sm = dye_slot.swatches[s_idx2].submesh_name;
                                    break;
                                }
                        if (!sm && !rv.misc_indices.empty() &&
                            dye_slot.swatches[rv.misc_indices.front()].submesh_name[0] != '\0')
                            sm = dye_slot.swatches[rv.misc_indices.front()].submesh_name;
                        char hdr_buf[128];
                        if (sm)
                        {
                            std::snprintf(hdr_buf, sizeof(hdr_buf), "%s", sm);
                        }
                        else
                        {
                            bool has_hair = !rv.layer_singletons[3].empty();
                            if (!has_hair)
                                for (int C = 0; C < 3 && !has_hair; ++C)
                                    if (rv.layer_slot[3][C] >= 0)
                                        has_hair = true;
                            std::snprintf(
                                hdr_buf,
                                sizeof(hdr_buf),
                                has_hair ? "[Hair - hidden under helm]" : "[Unnamed]"
                            );
                        }

                        ImGui::PushID(static_cast<int>(rv.stable_id) ^ static_cast<int>(rv.stable_id >> 32) ^ 0xC0DE);
                        // Collapsible region header. Closed by default to match Advanced mode - users expand only the
                        // regions they want to touch. The header text matches the
                        // Advanced-mode `(tpl 0xXXXX)` format so toggling views keeps the same labels.
                        char tree_buf[160];
                        std::snprintf(tree_buf, sizeof(tree_buf), "%s  (tpl 0x%04X)", hdr_buf, rv.tpl);
                        // Default-open: the Color Override tab is the wrapper, so a submesh collapsed by default
                        // hides what the user came to see.
                        const bool open = ImGui::TreeNodeEx(
                            tree_buf,
                            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen
                        );
                        // Channel-coverage marker. It stays on the header row, so it is visible whether the user
                        // expands the region or not. See `dye_picker_compute_channel_gap_tip`.
                        {
                            int present[3][3] = {{0}};
                            for (int L = 0; L < 3; ++L)
                                for (int C = 0; C < 3; ++C)
                                    present[L][C] = (rv.layer_slot[L][C] >= 0) ? 1 : 0;
                            char gap_tip[600];
                            if (dye_picker_compute_channel_gap_tip(present, gap_tip, sizeof(gap_tip)))
                            {
                                ImGui::SameLine();
                                ui_text_colored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "(!)");
                                if (ImGui::IsItemHovered())
                                {
                                    char full_tip[900];
                                    std::snprintf(
                                        full_tip,
                                        sizeof(full_tip),
                                        "This submesh's shader does not "
                                        "expose all dye channels.\n"
                                        "You can still edit the channels "
                                        "that ARE present, but missing "
                                        "channels keep their baked default.\n"
                                        "This limits how dark / bright you can drive the rendered color.\n\nGaps:\n%s",
                                        gap_tip
                                    );
                                    ui_tooltip(full_tip);
                                }
                            }
                        }
                        if (!open)
                        {
                            ImGui::PopID();
                            continue;
                        }

                        // Render one circle-swatch + popup picker per active channel via the shared
                        // render_swatch_controls helper. The helper already implements:
                        //   - bordered override-active checkbox
                        //   - manually-drawn circle swatch button (click to open popup ColorPicker3)
                        //   - auto-tick + retick on click
                        //   - persistence save on commit
                        // We pass it the channel's full cascade list so a single picker commit ripples to all 1-9
                        // captured shader tokens that share this channel suffix.
                        static const char *channel_labels[3] = {"R", "G", "B"};
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            if (ch_rows[ch].empty())
                                continue;
                            render_swatch_controls(
                                ch_rows[ch].data(),
                                static_cast<int>(ch_rows[ch].size()),
                                channel_labels[ch]
                            );
                        }
                        // Hair singletons (any `_hair*` token - `_hairDyeingColor`, `_hairDyeingScratch`, etc.). Each
                        // one gets its own row so the user can recolor hair / hair-overlay independently from a single
                        // cascade.
                        for (int idx : hair_rows)
                        {
                            int single[1] = {idx};
                            render_swatch_controls(single, 1, "hair");
                        }
                        ImGui::TreePop();
                        ImGui::PopID();
                    }
                }

                // Render each region as a tree node, gated on the per-character advanced toggle. Default-off gives the
                // merchant-style flat cluster view; tick to reveal full per-region per-token control.
                if (advanced_view)
                    for (auto &kv : regions)
                    {
                        auto &rv = kv.second;
                        auto &rstate = ui.region_ui[rv.stable_id];

                        // "Show only modified" filter: skip region if no swatch inside it is user-overridden. Filter
                        // runs BEFORE PushID - continue does not need a matching PopID.
                        if (ui.show_only_modified)
                        {
                            bool any_overridden = false;
                            for (int L = 0; L < 5 && !any_overridden; ++L)
                                for (int C = 0; C < 3 && !any_overridden; ++C)
                                {
                                    const int s_idx = rv.layer_slot[L][C];
                                    if (s_idx >= 0 && dye_slot.swatches[s_idx].override_active)
                                        any_overridden = true;
                                }
                            for (int L = 0; L < 5 && !any_overridden; ++L)
                                for (int s_idx : rv.layer_singletons[L])
                                    if (dye_slot.swatches[s_idx].override_active)
                                    {
                                        any_overridden = true;
                                        break;
                                    }
                            if (!any_overridden)
                                for (int s_idx : rv.misc_indices)
                                    if (dye_slot.swatches[s_idx].override_active)
                                    {
                                        any_overridden = true;
                                        break;
                                    }
                            if (!any_overridden)
                                continue;
                        }

                        ImGui::PushID(static_cast<int>(rv.stable_id) ^ static_cast<int>(rv.stable_id >> 32));
                        // Regions default-open: the Color Override tab is now the wrapper, so collapsing each submesh
                        // by default just hides what the user came to see.
                        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
                        char header_buf[128];
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
                                const int s_idx = rv.layer_slot[L][C];
                                if (s_idx >= 0 && dye_slot.swatches[s_idx].submesh_name[0] != '\0')
                                {
                                    sm = dye_slot.swatches[s_idx].submesh_name;
                                }
                            }
                        for (int L = 0; L < 5 && !sm; ++L)
                            for (int s_idx2 : rv.layer_singletons[L])
                                if (dye_slot.swatches[s_idx2].submesh_name[0] != '\0')
                                {
                                    sm = dye_slot.swatches[s_idx2].submesh_name;
                                    break;
                                }
                        if (!sm && !rv.misc_indices.empty() &&
                            dye_slot.swatches[rv.misc_indices.front()].submesh_name[0] != '\0')
                        {
                            sm = dye_slot.swatches[rv.misc_indices.front()].submesh_name;
                        }
                        if (sm)
                        {
                            std::snprintf(header_buf, sizeof(header_buf), "%s  (tpl 0x%04X)", sm, rv.tpl);
                        }
                        else
                        {
                            // No submesh name captured - the Material has no parent SkinnedMeshMaterialWrapper (or
                            // wrapper holds the empty-string module sentinel at +0x28). Most commonly this is the
                            // player's HAIR material:
                            // SkinnedMeshHair shader (tpl 0x3ADC on v1.06) is bound via a different parent path than
                            // armor materials. Hair re-renders every player frame so the setter captures it into every
                            // slot's carrier set during the 3-second apply window. Coloring has no visible effect under
                            // full-face helms (hair occluded) - visible on head/face slots without a helm.
                            //
                            // Detect via layer-3 tokens (hair family) and label clearly so users do not waste time
                            // picking colors on it.
                            bool has_hair = !rv.layer_singletons[3].empty();
                            if (!has_hair)
                                for (int C = 0; C < 3 && !has_hair; ++C)
                                    if (rv.layer_slot[3][C] >= 0)
                                        has_hair = true;
                            if (has_hair)
                            {
                                std::snprintf(
                                    header_buf,
                                    sizeof(header_buf),
                                    "[Hair - hidden under helm]  (tpl 0x%04X)",
                                    rv.tpl
                                );
                            }
                            else
                            {
                                std::snprintf(header_buf, sizeof(header_buf), "[Unnamed]  (tpl 0x%04X)", rv.tpl);
                            }
                        }
                        const bool open = ImGui::TreeNodeEx(header_buf, flags);
                        // Channel-coverage marker - same audit as the simple-mode picker (see
                        // `dye_picker_compute_channel_gap_tip`).
                        {
                            int present[3][3] = {{0}};
                            for (int L = 0; L < 3; ++L)
                                for (int C = 0; C < 3; ++C)
                                    present[L][C] = (rv.layer_slot[L][C] >= 0) ? 1 : 0;
                            char gap_tip[600];
                            if (dye_picker_compute_channel_gap_tip(present, gap_tip, sizeof(gap_tip)))
                            {
                                ImGui::SameLine();
                                ui_text_colored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "(!)");
                                if (ImGui::IsItemHovered())
                                {
                                    char full_tip[900];
                                    std::snprintf(
                                        full_tip,
                                        sizeof(full_tip),
                                        "This submesh's shader does not expose "
                                        "all dye channels.\n"
                                        "You can still edit the channels that "
                                        "ARE present, but missing channels "
                                        "keep their baked default - limiting "
                                        "how dark / bright you can drive the rendered color.\n\nGaps:\n%s",
                                        gap_tip
                                    );
                                    ui_tooltip(full_tip);
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
                            static const int adv_layer_order[5] = {0, 1, 2, 4, 3};
                            for (int Li = 0; Li < 5; ++Li)
                            {
                                const int L = adv_layer_order[Li];
                                int idx_r = rv.layer_slot[L][0];
                                int idx_g = rv.layer_slot[L][1];
                                int idx_b = rv.layer_slot[L][2];
                                int present = 0;
                                if (idx_r >= 0)
                                    ++present;
                                if (idx_g >= 0)
                                    ++present;
                                if (idx_b >= 0)
                                    ++present;
                                const auto &singletons = rv.layer_singletons[L];
                                if (present == 0 && singletons.empty())
                                    continue;

                                const char *layer_name = Transmog::color_override::token_table::layer_long_name(L);

                                if (present == 1)
                                {
                                    // Single-channel layer: no link UI (nothing to link to). Show a "<layer>·<channel>"
                                    // label + swatch.
                                    const int only_ch = (idx_r >= 0) ? 0 : (idx_g >= 0) ? 1 : 2;
                                    const int only_idx = (only_ch == 0) ? idx_r : (only_ch == 1) ? idx_g : idx_b;
                                    char label_buf[48];
                                    std::snprintf(
                                        label_buf,
                                        sizeof(label_buf),
                                        "%s\xC2\xB7%s",
                                        layer_name,
                                        Transmog::color_override::token_table::channel_short_name(only_ch)
                                    );
                                    ImGui::TextUnformatted(label_buf);
                                    ImGui::SameLine(0.0f, 6.0f);
                                    int single[1] = {only_idx};
                                    render_swatch_controls(single, 1, "");
                                    ImGui::NewLine();
                                }
                                else if (present >= 2)
                                {
                                    // 2 or 3 channels present: show link toggle. Linked mode drives all PRESENT
                                    // channels with one color (merchant behavior); unlinked exposes a per-channel row
                                    // for each present channel.
                                    char layer_hdr_buf[48];
                                    char ch_list[8];
                                    std::size_t ch_len = 0;
                                    if (idx_r >= 0 && ch_len + 1 < sizeof(ch_list))
                                        ch_list[ch_len++] = 'R';
                                    if (idx_g >= 0 && ch_len + 1 < sizeof(ch_list))
                                    {
                                        if (ch_len)
                                            ch_list[ch_len++] = '+';
                                        ch_list[ch_len++] = 'G';
                                    }
                                    if (idx_b >= 0 && ch_len + 1 < sizeof(ch_list))
                                    {
                                        if (ch_len)
                                            ch_list[ch_len++] = '+';
                                        ch_list[ch_len++] = 'B';
                                    }
                                    ch_list[ch_len] = '\0';
                                    std::snprintf(
                                        layer_hdr_buf,
                                        sizeof(layer_hdr_buf),
                                        "%s \xC2\xB7 %s",
                                        layer_name,
                                        ch_list
                                    );
                                    ImGui::TextUnformatted(layer_hdr_buf);
                                    ImGui::SameLine(0.0f, 6.0f);

                                    bool &linked = rstate.link_rgb[L];
                                    ImGui::PushID(L * 31 + 7);
                                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                                    ImGui::Checkbox("link##lnk", &linked);
                                    ImGui::PopStyleVar();
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::BeginTooltip();
                                        ImGui::TextUnformatted(
                                            "When ON: pick one color, all "
                                            "present R/G/B channel-suffix\n"
                                            "Properties get it (merchant "
                                            "behavior).\nUntick to edit each channel independently."
                                        );
                                        ImGui::EndTooltip();
                                    }
                                    ImGui::SameLine(0.0f, 6.0f);

                                    if (linked)
                                    {
                                        int cascade[3];
                                        int n = 0;
                                        if (idx_r >= 0)
                                            cascade[n++] = idx_r;
                                        if (idx_g >= 0)
                                            cascade[n++] = idx_g;
                                        if (idx_b >= 0)
                                            cascade[n++] = idx_b;
                                        render_swatch_controls(cascade, n, "");
                                    }
                                    else
                                    {
                                        int order[3] = {idx_r, idx_g, idx_b};
                                        bool first_shown = true;
                                        for (int c = 0; c < 3; ++c)
                                        {
                                            if (order[c] < 0)
                                                continue;
                                            if (!first_shown)
                                                ImGui::SameLine(0.0f, 8.0f);
                                            first_shown = false;
                                            int single[1] = {order[c]};
                                            render_swatch_controls(
                                                single,
                                                1,
                                                Transmog::color_override::token_table::channel_short_name(c)
                                            );
                                        }
                                    }
                                    ImGui::PopID();
                                    ImGui::NewLine();
                                }

                                // Family singletons: tokens classified into this layer but with no R/G/B suffix
                                // (channel=-1). Show the actual shader-property name so it is not confused with the
                                // R/G/B triplet.
                                for (int s_idx : singletons)
                                {
                                    auto &sw = dye_slot.swatches[s_idx];
                                    const char *propName =
                                        Transmog::color_override::token_table::token_label_for(sw.token_id);
                                    char nameBuf[64];
                                    if (propName && propName[0])
                                    {
                                        std::snprintf(nameBuf, sizeof(nameBuf), "%s", propName);
                                    }
                                    else
                                    {
                                        std::snprintf(
                                            nameBuf,
                                            sizeof(nameBuf),
                                            "%s \xC2\xB7 0x%04X",
                                            layer_name,
                                            sw.token_id & 0xFFFFu
                                        );
                                    }
                                    ImGui::TextUnformatted(nameBuf);
                                    ImGui::SameLine(0.0f, 6.0f);
                                    int single[1] = {s_idx};
                                    render_swatch_controls(single, 1, "");
                                    ImGui::NewLine();
                                }
                            }
                            // Tokens whose prefix matched no known family (layer == -1). Show the shader-property
                            // name when the interner captured it - only fall back to the "misc 0xXXXX" hex label when
                            // we have no name at all. A resolvable name is more useful than a bare hex blob.
                            for (int idx : rv.misc_indices)
                            {
                                auto &m_sw = dye_slot.swatches[idx];
                                const char *propName =
                                    Transmog::color_override::token_table::token_label_for(m_sw.token_id);
                                char label_buf[64];
                                if (propName && propName[0])
                                {
                                    std::snprintf(label_buf, sizeof(label_buf), "%s", propName);
                                }
                                else
                                {
                                    std::snprintf(label_buf, sizeof(label_buf), "misc 0x%04X", m_sw.token_id & 0xFFFFu);
                                }
                                ImGui::TextUnformatted(label_buf);
                                ImGui::SameLine(0.0f, 6.0f);
                                int single[1] = {idx};
                                render_swatch_controls(single, 1, "");
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
