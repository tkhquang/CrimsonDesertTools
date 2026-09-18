// overlay_ui/helpers.hpp
//
// Declarations of overlay-internal helpers shared across the overlay UI translation units.
//
// The variadic Text/TextDisabled/TextColored helpers are decl-in-header, def-in-cpp on purpose: reshade_overlay.hpp's
// namespace-ImGui inline thunks for `Text(fmt, ...)` etc. are inline-variadic, MSVC cannot COMDAT-fold inline variadic
// bodies, and the resulting strong copy collides at link time with imgui_widgets.obj's strong definition (LNK2005).
// One set of plain extern functions sidesteps the surface entirely: each TU sees the declaration only, so there is no
// per-TU inline-variadic emission and no duplicate strong symbol.

#ifndef TRANSMOG_OVERLAY_UI_HELPERS_HPP
#define TRANSMOG_OVERLAY_UI_HELPERS_HPP

#include "shared_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

struct ImVec4;

namespace Transmog
{

    /**
     * @brief Formats one line of overlay text.
     * @param fmt printf-style format string.
     * @details Forwards to ImGui's TextV, so the format step stays inside the upstream non-C-variadic implementation.
     */
    void ui_text(const char *fmt, ...);

    /**
     * @brief Formats one line of dimmed overlay text.
     * @param fmt printf-style format string.
     * @details Forwards to ImGui's TextDisabledV, so the format step stays inside the upstream implementation.
     */
    void ui_text_disabled(const char *fmt, ...);

    /**
     * @brief Formats one line of overlay text in @p col.
     * @param col Text color.
     * @param fmt printf-style format string.
     * @details Forwards to ImGui's TextColoredV, so the format step and the StyleColor push/pop pair both stay
     *          inside the upstream implementation.
     */
    void ui_text_colored(const ImVec4 &col, const char *fmt, ...);

    /**
     * @brief Shows @p text as a tooltip on the item just drawn.
     * @param text Tooltip body, drawn unformatted.
     * @details ImGui v1.92.5 exposes no SetTooltipUnformatted function-table slot, so the three-call sequence
     *          (BeginTooltip, TextUnformatted, EndTooltip) is the cheapest path available.
     */
    void ui_tooltip(const char *text);

    /**
     * @brief Case-insensitive substring search over an ASCII name.
     * @param hay The string to search.
     * @param needle The substring to find. An empty needle matches everything.
     * @return True when @p needle is empty or occurs anywhere in @p hay.
     * @note The fold is ASCII-only and locale-independent, which is what the rest of the catalog assumes.
     */
    [[nodiscard]] bool name_contains_ci(std::string_view hay, std::string_view needle) noexcept;

    /**
     * @brief Mirrors a picker-committed override into pending_overrides.
     * @param slot Picker slot index.
     * @param idx Row index inside that slot.
     * @param r Red component of the picked color.
     * @param g Green component of the picked color.
     * @param b Blue component of the picked color.
     * @details The slot-agnostic substitute path in color_override/setter_substitute.cpp reads pending_overrides on
     *          the next engine write, so the mirror is what makes the edit reach the renderer. The helper reads
     *          `submesh_name` from the row's SwatchOverride and `token_id` from its SwatchEntry.
     * @note A missing field makes this a silent no-op.
     */
    void mirror_override_to_pending(int slot, std::size_t idx, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

    /**
     * @brief Erases the pending entry that matches this row's (submesh, token) pair.
     * @param slot Picker slot index.
     * @param idx Row index inside that slot.
     * @details The user reverts a row to the engine default, or un-ticks its override checkbox, and this drops the
     *          matching pending entry.
     */
    void erase_override_from_pending(int slot, std::size_t idx) noexcept;

    /**
     * @brief Reports whether the overlay holds picker or checkbox edits that no apply pass committed yet.
     * @return True when any slot's effective target differs from what the game currently holds, or when a re-pick on
     *         the same carrier id set that slot's force-apply flag.
     * @details Compares staged slot_mappings against last_applied_ids.
     */
    [[nodiscard]] bool has_pending_changes() noexcept;

    /**
     * @brief Reports whether the overlay holds edits that no Save wrote back into the JSON preset.
     * @return True when slot_mappings differs from the active preset's persisted slots.
     * @details The Save button tints on true, so unsaved work stays visible. Prefab picks are session-only, so the
     *          comparison reads SlotUIState::prior_carrier_active and prior_carrier_item_id for a slot that holds one.
     */
    [[nodiscard]] bool has_pending_save() noexcept;

    /**
     * @brief Forces every slot that holds a session-only prefab pick onto the active character's default carrier.
     * @details The defaults come from carrier_defaults.hpp. pws swaps key on the source wrapper that THAT
     *          character's body emits, so the matching carrier must be resident at apply time or the swap silently
     *          no-ops.
     */
    void force_active_character_carrier_for_picked_slots();

    /**
     * @brief Clears all picked-prefab UI state and deactivates the body-mesh hook.
     * @return A per-slot mask naming the slots that held a prefab pick.
     * @details A preset switch calls this BEFORE it equips the new preset's items, so the tear-down lands before the
     *          equip and the hook cannot substitute against the old src wrappers. Capture Outfit calls it too,
     *          because it replaces the current state with the live equipped outfit and session-only prefab picks
     *          must surrender to that.
     * @note The caller owns the post-apply_to_state last_ids reconciliation. For each cleared slot whose new preset
     *       carrier equals last_ids[i], zero last_ids[i] so the apply pass tears down the prior body-mesh fake,
     *       because the natural-pipeline cleanup hook does not fire for it. When the carriers differ, last_ids stays
     *       intact and the regular tear_down_fake path runs.
     */
    std::array<bool, SLOT_COUNT> clear_all_picked_prefabs_and_deactivate();

    /**
     * @brief Audits a submesh's dye-property channel coverage and writes a tooltip body that lists the gaps.
     * @param present [layer][channel] map: 1 when a SwatchEntry exists for that pair on the submesh, 0 otherwise.
     *        Layer 0 is _tintColor, layer 1 is _dyeingColorMask, layer 2 is _dyeingDetailLayerColorMask. Channel 0
     *        is R, 1 is G, 2 is B.
     * @param out Destination buffer for the bullet lines.
     * @param cap Capacity of @p out in bytes.
     * @return True when the audit found at least one gap. Only then does @p out hold bullet lines.
     * @details The dye picker draws a (!) next to a submesh whose shader does not expose the full RGB triple for one
     *          or more dye families.
     * @note Hair (layer 3) is excluded on purpose, because its presence depends on context. A wholly absent
     *       detail-mask family is silent too, because many assets do not use that family. Only partial detail-mask
     *       coverage is flagged, which is a genuine baked-channel lock.
     */
    [[nodiscard]] bool dye_picker_compute_channel_gap_tip(const int present[3][3], char *out, std::size_t cap);

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_UI_HELPERS_HPP
