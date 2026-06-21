#pragma once

// Mid-hook on the engine's per-property 4-byte property setter.
//
// The dye-record-inject path (writes ARMOR_MOD records to dst+120 via the DyeCopier hook) propagates colors only for
// items the engine treats as natively dyeable. For monster carriers like `cd_m0001_00_no_phm_ub_12002` the engine never
// advances those records to the per-property shader setter, so dye colors never render through that route.
//
// This module redirects `ctx.r8` (the source pointer the setter reads from) to a per-thread BGRA buffer pre-loaded with
// the user's chosen RGB. The setter then writes the substituted bytes into the destination material property, bypassing
// the entire
// dye-record -> shader chain. Substitution is gated on the
// LT-apply window, host scope, and a per-row `override_active` flag in the swatch table, so unrelated property writes
// pass through unchanged.
//
// The setter target is AOB-resolved via `k_setterByteCandidates` in `aob_resolver.hpp`; the cascade's three patterns
// cover patch-day prologue reflows.

#include <cstdint>

namespace Transmog::ColorOverride::SetterSubstitute
{
    /**
     * Install the setter mid-hook. Returns true on success; failure (AOB cascade exhausted or hook install rejected)
     * leaves the substitute inert.
     */
    bool init() noexcept;

    /**
     * Mark the LT-apply window. Set true immediately before invoking the engine's slot-populator from
     * transmog_apply.cpp and clear once it returns. The detour only substitutes while the window is open (plus a short
     * trailing tail), so unrelated render passes (NPCs, UI) keep their own colors.
     */
    void set_apply_window(bool active) noexcept;

    /**
     * Stash the slot index currently being applied. Read by the setter's resolve_slot path as the primary ownership
     * source. Call BEFORE invoking apply_transmog for the slot; the value PERSISTS across calls and is overwritten by
     * the next call. Do not pair with `set_active_slot(-1)` -- the engine queues property writes that fire
     * asynchronously during the apply-window tail, and clearing the slot too early would orphan those writes.
     */
    void set_active_slot(int slot) noexcept;
} // namespace Transmog::ColorOverride::SetterSubstitute
