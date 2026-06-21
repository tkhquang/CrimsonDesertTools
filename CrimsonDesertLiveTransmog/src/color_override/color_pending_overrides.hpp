#pragma once

// Pending-overrides map: persisted user picks that haven't yet been matched against a live SwatchTable row.
//
// Loaded at game start (or preset / character switch) from `Preset::swatch_overrides`. The setter hook consults this
// map on every successful `lookup_or_insert` -- if the captured `(slot, submesh_name, token_id)` matches a pending
// entry, the row's `override_active` flag is flipped on and its RGB is set to the persisted value. The entry is *not*
// consumed (kept in the map) so that subsequent re-captures (e.g. after engine teardown) re-apply automatically.
//
// Slot-keyed so wipe_slot can clear just that slot's pending entries when the user changes the slot's target item.

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
    void set(int slot, const std::string &submesh_name, const std::string &token_name, std::uint8_t r, std::uint8_t g,
             std::uint8_t b) noexcept;

    /**
     * Look up a pending RGB for a captured row. Returns true and fills `r/g/b` on hit. The match is name-based:
     *   - `submesh_name` exact-match against the entry's submesh
     *   - `token_id` must equal the entry's resolved token id (resolved on insert via `TokenTable::token_id_for_name`;
     *     entries whose token name didn't resolve at insert time are re-tried on lookup so late-bootstrap snapshots
     *     still find their match)
     */
    bool lookup(int slot, const char *submesh_name, std::uint16_t token_id, std::uint8_t &r, std::uint8_t &g,
                std::uint8_t &b) noexcept;

    /**
     * Coarse-grained existence check the setter calls before its `lookup_or_insert` work, so unaffected slots skip the
     * map access on every fire. `false` when the slot has no entries.
     */
    bool slot_has_pending(int slot) noexcept;

    /**
     * Slot-agnostic, NON-CONSUMING lookup. Scans every slot's pending vector and returns the first entry whose
     * `(submesh_name, token_id)` matches, regardless of which slot the JSON stored it under.
     *
     * The setter's substitute path consults this before `resolve_slot()` runs, so a preset's saved colour overrides
     * apply to any matInst write whose submesh-name + token match, without requiring CarrierSet to have already bound
     * the matInst's content_hash. This handles non-transmog-slot matInsts (hair, face, body) which aren't bound during
     * a transmog-slot apply.
     *
     * Non-consuming on purpose: subsequent engine writes to the same property must keep substituting until the user
     * explicitly removes the override via `erase_by_token_id`.
     */
    bool lookup_any_slot(const char *submesh_name, std::uint16_t token_id, std::uint8_t &r, std::uint8_t &g,
                         std::uint8_t &b) noexcept;

    /**
     * Global "is there ANY pending entry anywhere?" hot-path filter. Returns true when at least one slot has pending
     * data. Companion to `lookup_any_slot` -- callers gate on this before paying the slot-scan cost.
     */
    bool has_any() noexcept;

    /**
     * Insert/update keyed by token_id. Used by the picker write path which has the row's `SwatchEntry::token_id` in
     * hand and can skip the name->id resolution that `set()` does. Mirroring picker edits into the pending map keeps
     * the slot-agnostic substitute path serving the live user colour rather than the JSON-loaded value.
     */
    void set_by_token_id(int slot, const std::string &submesh_name, std::uint16_t token_id, std::uint8_t r,
                         std::uint8_t g, std::uint8_t b) noexcept;

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
        std::uint64_t entries_total; // sum across all slots
        std::uint64_t lookups_hit;
        std::uint64_t lookups_miss;
    };
    Stats snapshot_stats() noexcept;
} // namespace Transmog::ColorOverride::PendingOverrides
