// overlay_ui/helpers.hpp
//
// Declarations of overlay-internal helpers shared across the overlay UI translation units.
//
// The variadic Text/TextDisabled/TextColored helpers are decl-in-header, def-in-cpp on purpose: reshade_overlay.hpp's
// namespace-ImGui inline thunks for `Text(fmt, ...)` etc. are inline-variadic, MSVC cannot COMDAT-fold inline variadic
// bodies, and the resulting strong copy collides at link time with imgui_widgets.obj's strong definition (LNK2005).
// Centralising them as plain extern functions sidesteps the surface entirely: each TU sees the declaration only, no
// per-TU inline-variadic emission, no duplicate strong symbol.

#ifndef TRANSMOG_OVERLAY_UI_HELPERS_HPP
#define TRANSMOG_OVERLAY_UI_HELPERS_HPP

#include "shared_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

struct ImVec4;

namespace Transmog
{

    // Variadic text helpers. Each forwards to ImGui's *V counterpart so formatting and (for the styled variants)
    // push/pop StyleColor stays inside the upstream non-C-variadic implementation.
    void ui_text(const char *fmt, ...);
    void ui_text_disabled(const char *fmt, ...);
    void ui_text_colored(const ImVec4 &col, const char *fmt, ...);

    // Multi-call tooltip wrapper. ImGui v1.92.5 does not expose SetTooltipUnformatted as a function-table slot, so the
    // three-call sequence (BeginTooltip / TextUnformatted / EndTooltip) is the cheapest available path.
    void ui_tooltip(const char *text);

    // Case-insensitive substring search. Returns true if `needle` is empty or found anywhere in `hay`.
    [[nodiscard]] bool name_contains_ci(const std::string &hay, const char *needle) noexcept;

    // Mirror a picker-committed override into PendingOverrides so the slot-agnostic substitute path in
    // color_override/setter_substitute.cpp picks up the user's edit on the next engine write.
    //
    // Caller passes the picker's slot index + row index. The helper reads `submesh_name` from the row's
    // `SwatchOverride` and `token_id` from the row's `SwatchEntry`, then writes to PendingOverrides. Silently no-ops if
    // either field is missing.
    void mirror_override_to_pending(int slot, std::size_t idx, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

    // Erase any pending entry matching this row's (submesh, token). Called when the user reverts a row to engine
    // default or un-ticks the override checkbox.
    void erase_override_from_pending(int slot, std::size_t idx) noexcept;

    // Compare staged slot_mappings vs last_applied_ids to detect unsaved picker/checkbox edits. Returns true if ANY
    // slot's effective target differs from what's currently committed to the game (or has the force-apply flag set
    // after a re-pick on the same carrier id).
    [[nodiscard]] bool has_pending_changes() noexcept;

    // Returns true when slot_mappings differs from the active preset's persisted slots, i.e. the user edited rows in
    // the overlay but has not committed the change back into the JSON preset via Save. Used to tint the Save button so
    // unsaved work is visible. Prefab picks are session-only and require special-case comparison via
    // SlotUIState::priorCarrierActive / priorCarrierItemId.
    [[nodiscard]] bool has_pending_save() noexcept;

    // Force the carrier item for each slot that has a session-only prefab pick to the active character's default
    // carrier (from carrier_defaults.hpp). PWS swaps are keyed by the source wrapper that THAT character's body emits,
    // so the matching carrier must be resident at apply time or the swap silently no-ops.
    void force_active_character_carrier_for_picked_slots();

    // Clear all picked-prefab UI state and deactivate the body-mesh hook. Called BEFORE preset switches (so the swap is
    // torn down before the new preset's items are equipped, otherwise the hook would still substitute against the old
    // src wrappers) and on Capture Outfit (which replaces the current state with the live equipped outfit, so
    // session-only prefab picks must surrender too).
    //
    // Returns a per-slot mask indicating which slots had a prefab pick. The caller is responsible for
    // post-apply_to_state lastIds reconciliation: for each cleared slot, if the new preset's carrier equals lastIds[i],
    // zero lastIds[i] so the apply pass tears down the prior body-mesh fake (the natural-pipeline cleanup hook would
    // not fire otherwise). When carriers differ, lastIds stays intact and the regular tear_down_fake path runs.
    std::array<bool, k_slotCount> clear_all_picked_prefabs_and_deactivate();

    // Audit a submesh's dye-property channel coverage and emit a tooltip body listing gaps. Used by the dye picker to
    // render a (!) next to submeshes whose shader doesn't expose the full RGB triple for one or more dye families.
    //
    // `present[layer][channel]`: 1 if a SwatchEntry exists for that (layer, channel) on the submesh, 0 otherwise.
    //   layer 0 = _tintColor
    //   layer 1 = _dyeingColorMask
    //   layer 2 = _dyeingDetailLayerColorMask
    //   channel 0 = R, 1 = G, 2 = B
    //
    // Hair (layer 3) is intentionally excluded -- its presence is context-dependent. Detail-mask absence is also
    // silenced because many assets simply don't use that family; only partial coverage of detail-mask is flagged
    // (genuine baked-channel lock).
    //
    // Returns true if at least one gap was found. On true, `out` holds bullet lines.
    [[nodiscard]] bool dye_picker_compute_channel_gap_tip(const int present[3][3], char *out, std::size_t cap);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_HELPERS_HPP
