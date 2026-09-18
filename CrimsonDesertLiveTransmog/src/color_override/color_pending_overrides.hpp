#ifndef TRANSMOG_COLOR_OVERRIDE_COLOR_PENDING_OVERRIDES_HPP
#define TRANSMOG_COLOR_OVERRIDE_COLOR_PENDING_OVERRIDES_HPP

/**
 * @file color_pending_overrides.hpp
 * @brief Pending-overrides map: persisted user picks not yet matched against a live SwatchTable row.
 * @details The map loads at game start, and on a preset or character switch, from `Preset::swatch_overrides`. The
 *          setter hook consults it on every successful `lookup_or_insert`. When the captured
 *          `(slot, submesh_name, token_id)` matches a pending entry, the row `override_active` flag flips on and its
 *          RGB takes the persisted value.
 *
 *          The two lookups differ in what they do to the entry, and the difference is deliberate:
 *          - `lookup()` CONSUMES the entry, so a later un-tick or revert is not undone by the next engine write.
 *          - `lookup_any_slot()` leaves the entry in place, so every later engine write to the same property keeps
 *            substituting until `erase_by_token_id` removes it.
 *
 *          The map is slot-keyed, so wipe_slot clears one slot worth of pending entries when the user changes that
 *          slot target item.
 */

#include "shared_state.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Transmog::ColorOverride::PendingOverrides
{
    using ::Transmog::k_slotCount;

    /**
     * Insert a pending override. `submesh_name` and `token_name` form the match key; the setter looks them up by name +
     * resolved token id. RGB is the persisted user pick.
     */
    void
    set(int slot,
        const std::string &submesh_name,
        const std::string &token_name,
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b) noexcept;

    /**
     * @brief Looks up a pending RGB for a captured row, and CONSUMES the entry on a hit.
     * @param slot Slot the captured row belongs to.
     * @param submesh_name Submesh name of the captured row, matched exactly.
     * @param token_id Resolved token id of the captured row.
     * @param r Receives the persisted red channel on a hit.
     * @param g Receives the persisted green channel on a hit.
     * @param b Receives the persisted blue channel on a hit.
     * @return True on a hit, with @p r, @p g and @p b filled.
     * @details The insert resolves the token id through `TokenTable::token_id_for_name`. An entry whose token name did
     *          not resolve then is re-tried here, so a late-bootstrap snapshot still finds its match. A hit ERASES the
     *          entry: the substitute path calls `set_override_active(true)` right after, so an entry left in place
     *          re-enables the override on every later engine write and makes "revert to default" and a per-row un-tick
     *          impossible. The user RGB lives in SwatchOverride once applied, so this map is the one-time bridge from
     *          JSON to live state, not a backing store.
     * @note Callback-safe in the sense that it allocates nothing.
     * @warning It takes the module-wide entry mutex and mutates the slot vector, both on the setter detour path.
     */
    bool lookup(
        int slot,
        const char *submesh_name,
        std::uint16_t token_id,
        std::uint8_t &r,
        std::uint8_t &g,
        std::uint8_t &b
    ) noexcept;

    /**
     * Coarse-grained existence check the setter calls before its `lookup_or_insert` work, so unaffected slots skip the
     * map access on every fire. `false` when the slot has no entries.
     */
    bool slot_has_pending(int slot) noexcept;

    /**
     * Slot-agnostic, NON-CONSUMING lookup. Scans every slot's pending vector and returns the first entry whose
     * `(submesh_name, token_id)` matches, regardless of which slot the JSON stored it under.
     *
     * The setter's substitute path consults this before `resolve_slot()` runs, so a preset's saved color overrides
     * apply to any matInst write whose submesh-name + token match, without requiring CarrierSet to have already bound
     * the matInst's content_hash. This handles non-transmog-slot matInsts (hair, face, body) which are not bound during
     * a transmog-slot apply.
     *
     * Non-consuming on purpose: subsequent engine writes to the same property must keep substituting until the user
     * explicitly removes the override via `erase_by_token_id`.
     */
    bool lookup_any_slot(
        const char *submesh_name,
        std::uint16_t token_id,
        std::uint8_t &r,
        std::uint8_t &g,
        std::uint8_t &b
    ) noexcept;

    /**
     * Global "is there ANY pending entry anywhere?" hot-path filter. Returns true when at least one slot has pending
     * data. Companion to `lookup_any_slot` - callers gate on this before paying the slot-scan cost.
     */
    bool has_any() noexcept;

    /**
     * Insert/update keyed by token_id. Used by the picker write path which has the row's `SwatchEntry::token_id` in
     * hand and can skip the name->id resolution that `set()` does. Mirroring picker edits into the pending map keeps
     * the slot-agnostic substitute path serving the live user color rather than the JSON-loaded value.
     */
    void set_by_token_id(
        int slot,
        const std::string &submesh_name,
        std::uint16_t token_id,
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b
    ) noexcept;

    /**
     * Remove the pending entry matching (submesh, token_id) on `slot`. Companion to `set_by_token_id`. Called when the
     * user reverts a row to its engine default or un-ticks the override checkbox so the substitute stops firing for it.
     * No-op when nothing matches.
     */
    void erase_by_token_id(int slot, const char *submesh_name, std::uint16_t token_id) noexcept;

    /**
     * Drop every pending entry for the slot. Called from `wipe_slot` on transmog target change.
     */
    void clear_slot(int slot) noexcept;

    /**
     * Drop every entry across every slot. Called on preset switch before `restore_swatches_from` repopulates from the
     * new preset.
     */
    void clear_all() noexcept;

    /// Diagnostic counters for logging.
    struct Stats
    {
        std::uint64_t entries_total{}; // sum across all slots
        std::uint64_t lookups_hit{};
        std::uint64_t lookups_miss{};
    };
    Stats snapshot_stats() noexcept;
} // namespace Transmog::ColorOverride::PendingOverrides

#endif // TRANSMOG_COLOR_OVERRIDE_COLOR_PENDING_OVERRIDES_HPP
