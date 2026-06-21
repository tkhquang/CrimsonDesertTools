#pragma once

// Runtime color-substitute path for materials whose dye-record propagation does not reach the per-property setter
// (typically monster-mesh transmog carriers like `cd_m0001_00_no_phm_ub_12002`).
//
// The publisher hook captures the carrier matInst set during the apply window; the setter hook then substitutes the
// user's chosen RGB for any write whose `(submesh, token)` matches a saved override in the active preset. Companion to
// `DyeRecordInject`, which drives the engine's native ARMOR_MOD record path for items the bench can dye normally.

#include <cstddef>
#include <cstdint>

namespace Transmog::ColorOverride
{
    /**
     * Cap on per-slot SwatchEntry rows. The engine emits dye-color writes against 90+ distinct token IDs per item
     * across all matInst variants in a single slot; 256 holds the working set without growth pressure. Storage cost: 5
     * slots * 256 rows * ~16 B = ~20 KB.
     */
    inline constexpr std::size_t k_dyeSwatchesPerSlot = 256;

    /**
     * Install all sub-hooks. Returns true when ALL hooks installed cleanly. Failures are logged and individual hooks
     * become inert without affecting the rest of the subsystem.
     */
    bool init();

    /**
     * Mark the LT apply window for `slot`. Publisher inserts and setter substitutes are gated by this. Call from
     * `transmog_apply.cpp` around the engine's slotPopulator invocation.
     */
    void mark_apply_begin(int slot) noexcept;
    void mark_apply_end() noexcept;

    /**
     * Wipe ALL captured state for `slot`: carrier set, hash set, swatch rows, owner-map slot entries, freeze flags,
     * apply-window timers. Call from `apply_single_slot` when the target item changes so the new captures aren't mixed
     * with stale rows.
     */
    void wipe_slot(int slot) noexcept;

    /**
     * Wipe ALL state across every slot. Call from PresetManager when the preset / character changes.
     */
    void reset_all() noexcept;

    /// Diagnostic dump.
    void log_counters() noexcept;
} // namespace Transmog::ColorOverride
