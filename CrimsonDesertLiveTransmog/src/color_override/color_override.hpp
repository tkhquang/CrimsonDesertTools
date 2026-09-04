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
     * Cap on per-slot SwatchEntry rows.
     *
     * A slot holds TWO populations that share this table: the rows the engine captures live, and the rows a saved
     * palette seeds on load. They ADD UP rather than overlap, so the cap has to cover both at once. Overflow is not
     * an error the user sees -- the seed drops the rows that do not fit, each one a color the picker then cannot
     * drive, and the only signal is a wall of "[swatch-seed] slot N full" warnings.
     *
     * Palette size scales with COLOR TOKENS, not with mesh count: a single submesh occupies one row per token it
     * exposes, which can run to a couple of dozen rows on its own. A fully dyed chest slot is the worst case and can
     * need several hundred rows. Size this for that slot plus live-capture headroom, never for the average.
     *
     * Storage cost is real and worth knowing before raising it again: the cap multiplies TWO static tables, the
     * SwatchTable rows and the picker-side DyeSlot rows, across every one of k_slotCount slots. At 512 that is
     * roughly 1.2 MB of static data. Still cheap next to silently losing colors, but it is not free.
     */
    inline constexpr std::size_t k_dyeSwatchesPerSlot = 512;

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
