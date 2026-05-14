#pragma once

// Per-slot 3-pass swatch reinit + single-pass color-commit retick.
//
// The state machine drives an automatic
//   teardown (untick + apply) -> wait -> retick (untick + apply) -> wait
// cycle, repeats it three times, and intersects the identity tuples
// captured across all three passes. Rows whose identity didn't appear
// in every pass are marked frozen_hidden -- the UI hides them and the
// setter substitute path bails on them. The intersection prunes
// transient ghost rows that only appear on some passes (e.g. world
// detail meshes the engine briefly binds during slot rebuild).
//
// schedule_color_commit_retick() reuses the same machine in
// single-pass mode (mode=ModeCommitRetick): no capture, no pruning,
// no lock -- just a tear-down + reapply so the engine re-instantiates
// the carrier matInsts and the substitute path picks up freshly-
// committed user colours from SwatchOverride.
//
// `tick()` MUST be called once per UI frame from the overlay render
// path. It's cheap when every slot is Idle (no syscalls, just atomic
// loads). The state machine relies on Transmog::manual_apply_slot
// for the per-slot apply trigger.

#include "shared_state.hpp"

#include <cstdint>

namespace Transmog::ColorOverride::Reinit
{
    using ::Transmog::k_slotCount;

    /// Drive every slot's state machine forward. Call once per UI
    /// frame from the overlay render path.
    void tick() noexcept;

    /// Begin a 3-pass reinit on `slot`. Returns false if the slot
    /// has no active transmog target or is already mid-reinit.
    bool start_slot_reinit(int slot) noexcept;

    /// Begin a single-pass reinit on `slot`. Same teardown + capture
    /// cycle as the 3-pass version but Finalize-after-one-pass, so
    /// captured rows are kept as-is (no ghost intersection). Used as
    /// the auto-trigger when the user first switches to the Color
    /// Override tab on an empty slot -- one pass is enough to
    /// populate the swatch grid; users can still hit Re-init for the
    /// 3-pass ghost-filtered variant.
    bool start_slot_reinit_once(int slot) noexcept;

    /// Schedule a single-pass tear-down + reapply on `slot` so the
    /// engine re-instantiates the carrier and the setter picks up
    /// freshly-committed user colours. Coalesced if already in flight.
    bool schedule_color_commit_retick(int slot) noexcept;

    bool is_slot_reinit_active(int slot) noexcept;
    bool is_color_commit_retick_active(int slot) noexcept;

    /// 0..2 -- pass index for the 3-pass cycle. UI shows pass+1.
    int  slot_reinit_pass(int slot) noexcept;

    bool any_slot_reinit_active() noexcept;

    /// Cancel any in-flight reinit (e.g. when slot is wiped due to
    /// transmog target change). Resets to Idle without finalize.
    void cancel(int slot) noexcept;

    /// Notify the reinit layer that `slot`'s transmog target changed.
    /// Wipes the swatch table when the new target id differs from
    /// the previously-stored one (so a fresh capture run can start).
    void notify_transmog_target(int slot,
                                std::uint32_t newTargetItemId) noexcept;

    /// Zero the cached "last applied target item id" tracking for
    /// every slot. Called from `ColorOverride::reset_all()` so that
    /// the next `notify_transmog_target` after a preset/character
    /// switch sees `last==0` and skips the stale-row wipe -- which
    /// would otherwise destroy placeholders seeded by
    /// `SwatchTable::populate_from_persisted` immediately before
    /// `transmog_apply` runs.
    void reset_target_tracking() noexcept;
}
