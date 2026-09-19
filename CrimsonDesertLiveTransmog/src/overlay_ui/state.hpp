// overlay_ui/state.hpp
//
// Module-scope state shared between the overlay UI translation units.
//
// These are inline namespace-scope variables (C++17), so every TU that includes this header observes the same
// storage. The types live next to the state because several section files consume the per-slot SlotUIState struct,
// and one definition here spares them the forward-declaration churn.
//
// The `s_` prefix marks a name as module-internal even though it lives in `namespace Transmog`. An anonymous
// namespace cannot span translation units, so it is not an option here. An inline variable at Transmog scope gives
// equivalent linkage and makes each cross-TU access explicit.

#ifndef TRANSMOG_OVERLAY_UI_STATE_HPP
#define TRANSMOG_OVERLAY_UI_STATE_HPP

#include "shared_state.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace Transmog
{
    /**
     * @brief Font this mod added to the HOST atlas so its own widgets can draw the selected locale, as `ImFont *`.
     *
     * @details One font for EVERY locale, not only the scripted ones, so the tab keeps one set of metrics across a
     *          switch. Null only while none has resolved, or after the host cleared its atlas. The overlay pushes it
     *          around this mod's content only, which leaves the host's own UI on the host's font. It is a `void *`
     *          so this header stays free of the ImGui headers.
     */
    inline void *s_host_locale_font = nullptr;

    /// Size `s_host_locale_font` was rasterized at. The overlay pushes exactly this, never ImGui's size fallback.
    inline float s_host_locale_font_size = 0.0f;

    /**
     * @brief Window title for the standalone host wrapper.
     * @details The ### separator gives ImGui a stable id, so a later change to the visible title keeps window state.
     */
    inline constexpr const char *WINDOW_TITLE = "Transmog###TransmogMain";

    /**
     * @brief Minimum time in milliseconds the cursor must rest on a picker item before hover-apply fires.
     * @details This is the one debounce invariant for the picker. It stops a scroll through the list from firing an
     *          apply cycle per row.
     */
    inline constexpr std::int64_t HOVER_DEBOUNCE_MS = 300;

    /**
     * @brief True in the standalone overlay, false in the ReShade addon tab.
     * @details The host wrapper sets this before draw_overlay_content runs. The standalone overlay uses a wider
     *          layout for high DPI and the addon tab a compact one. FontGlobalScale cannot stand in for this flag,
     *          because ReShade at 4K also sets FontGlobalScale above 1.
     */
    inline bool s_standalone_mode = false;

    /**
     * @brief Session-only UI-scale multiplier on top of the init-time auto-DPI scale.
     * @details dx_overlay.cpp owns the auto-DPI scale. A value of 1.0 means no override, and the user picks 1.25,
     *          1.5 and so on from the header combo. Only the standalone overlay reads this, because ReShade does its
     *          own scaling. Nothing persists it, so the user re-picks on the next launch.
     */
    inline float s_ui_scale = 1.0f;

    /**
     * @brief Instant Apply mode: a hover, pick, slot toggle or clear applies at once, with no Apply All click.
     * @details Off by default, because each action drives a tear-down and SlotPopulator cycle. The cross-slot prefab
     *          browser ignores this flag, because hover-apply on a prefab needs carrier-borrow plumbing the mod does
     *          not have. Click-to-pick still works in prefab mode.
     */
    inline bool s_auto_apply = false;

    /**
     * @brief True to keep the picker search text across opens.
     * @details The user re-opens the same slot and carries on from the same filter. False clears the box on open.
     */
    inline bool s_keep_search_text = true;

    /// Preset-list height in rows, so it survives a font or UI-scale change. Persisted in presets.json.
    inline float s_preset_rows = 10.0f;

    /**
     * @brief Dye picker region UI state: per-submesh collapse plus the per-layer R/G/B link toggle.
     * @details Each unique submesh stable_id owns one entry in the slot's region_ui map.
     */
    struct RegionUIState
    {
        bool collapsed = false;
        // Per-layer "link R/G/B" toggle. Index 0..4 = tint/mask/detail/hair/scratch. When true, the default, an edit
        // to any of the three R/G/B-suffix swatches in this layer cascades to the other two, which mirrors the
        // merchant "Pure X" semantics. Untick it to split the layer into independent per-channel control.
        std::array<bool, 5> link_rgb{{true, true, true, true, true}};
    };

    /**
     * @brief Per-slot overlay UI state: picker filters, hover-debounce bookkeeping and dye view preferences.
     */
    struct SlotUIState
    {
        char hex_buf[8]{"0000"};
        bool editing = false;
        char search_buf[64]{};
        // "Exact" lists only items whose auto-detected category matches THIS slot. On by default, so the dropdown
        // shows roughly 350 relevant items instead of all 6024.
        bool exact_filter = true;
        bool hide_incompatible = true;  // hide crash-risk + non-equipment
        bool hide_variants = false;     // hide NPC variants (carrier items)
        bool hide_body_mismatch = true; // hide items whose body type does not
                                        // match the active character's
        // Prefab-only mode. When true the picker hides the items list and its filters and shows ALL body-mesh
        // prefabs across every slot, each labeled with its native slot. A pick applies to the prefab's native slot,
        // which is not always the popup slot, so the user can browse the whole prefab catalog from any slot.
        bool prefab_mode = false;
        // Prefab-mode-only Exact filter. When true the prefab-mode list shows only prefabs whose derived slot
        // (slot_for_prefab_name) matches the popup's slot category. On by default, so Helm's picker in prefab mode
        // opens on Helm-family prefabs alone. Untick it to see the full cross-slot catalog.
        bool prefab_exact_filter = true;
        // Prefab-mode-only "keep open after pick" toggle. When true the picker stays open after a prefab commit, so
        // the user can try alternatives without a re-open. On by default for the cross-slot browser, because most
        // users step through several candidates per slot.
        bool prefab_keep_open_on_pick = true;
        // Hover-apply debounce state: which item the cursor sits on, and when it first landed there. The apply fires
        // only after the cursor rests on the same item for HOVER_DEBOUNCE_MS, which states the debounce invariant.
        std::uint16_t hover_pending_id = 0;
        std::uint16_t hover_applied_id = 0;
        std::int64_t hover_start_ms = 0;
        // Prefab-mode mirror of the hover-apply debounce above, keyed by prefab name (cd_phm_* / cd_phw_*), because
        // the prefab catalog carries no compact integer id. Only the Up/Down nav cursor (GUI buttons or arrow keys)
        // feeds this debounce. Mouse hover stays unwired on purpose: a body-mesh preview fans out to a full
        // multi-actor manual_apply, which is too disruptive to chain off casual cursor motion. The apply runs through
        // commit_prefab_at(preview_only=true), so the popup survives the re-equips.
        //
        // The `hover*` spelling stays, rather than `nav*`, for cross-reading parity with the items-mode
        // hover_pending_id/hover_applied_id/hover_start_ms above. Both serve the same debounce role. Only the trigger
        // source differs: hover plus nav for items, nav alone for prefabs.
        std::string hover_pending_prefab;
        std::string hover_applied_prefab;
        std::int64_t hover_prefab_start_ms = 0;
        // Button-driven navigation index into the visible (filtered) list. -1 means no highlight. The Up and Down
        // buttons move it and Enter commits the highlighted item. last_visible_count holds the previous frame's
        // visible count, which clamps the index.
        int nav_index = -1;
        int last_visible_count = 0;
        bool nav_moved = false; // true only on the frame a nav button was pressed
        // A prefab pick from this slot's picker surfaces the prefab name on the slot button, so the UI reads
        // "[prefab] cd_nhw_no_ub_20027" rather than the carrier item's display name. An empty string means no prefab
        // pick. This is session-only, matching prefab-wrapper-swap selections, and nothing writes it to JSON.
        std::string picked_prefab_name;

        // Snapshot of the slot's carrier state BEFORE the first prefab pick auto-borrowed Kliff's plate item. The
        // first pick captures it and sets prior_carrier_saved. The "(no body-mesh override)" entry restores it when the
        // user clears the prefab. That restore puts back the original Wellsknight, or whatever the active preset
        // held, so the slot reverts visually instead of staying stuck on the Kliff plate carrier.
        bool prior_carrier_saved = false;
        bool prior_carrier_active = false;
        std::uint16_t prior_carrier_item_id = 0;

        // Dye UI: per-region collapse state plus the per-layer R/G/B link toggles, keyed by submesh stable_id. This
        // is session-only and resets on a slot-target change. `std::map` gives stable iteration and hashes no u64
        // that this code does not own.
        std::map<std::uint64_t, RegionUIState> region_ui{};

        // Dye UI: "Show only modified" filter. When true the picker hides every cluster or region whose rows all sit
        // at their captured engine default (`override_active == false`), so a user with many swatches sees only the
        // rows they touched. This is session-only, because the filter is a viewing preference, not preset state.
        bool show_only_modified = false;
    };

    /** @brief Per-slot overlay UI state, indexed by the slot loop index. */
    inline SlotUIState s_slot_ui[SLOT_COUNT]{};

    /**
     * @brief One-shot auto-trigger for the per-slot Color Override reinit.
     * @details A popup that opens on a slot with an empty swatch grid (detected == 0), while no reinit runs, fires a
     *          single-pass reinit, so the user never has to click Re-init. The popup-closed branch in overlay_ui.cpp
     *          clears the flag, which re-arms it.
     */
    inline bool s_co_auto_triggered[SLOT_COUNT] = {};

    /** @brief Scratch buffer holding the text the preset rename dialog edits. */
    inline char s_rename_preset_buf[64]{};
    /** @brief True while the preset rename dialog is open. */
    inline bool s_rename_active = false;
    /** @brief Index of the preset the rename dialog edits, or -1 when none. */
    inline int s_rename_index = -1;

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_STATE_HPP
