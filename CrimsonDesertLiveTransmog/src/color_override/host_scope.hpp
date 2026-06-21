#pragma once

// host_scope: host-scoped gate for the dye-setter substitute.
//
// Problem: the engine's frame-render walks every host (player + ~30 NPCs) every apply.
// ColorOverride::SetterSubstitute's setter detour fires on every 4-byte material-property write inside the apply
// window, so without a gate the user's RGB bleeds onto every visible character.
//
// Fix: hook the per-host owner-container vfuncs that walk the matInst lists (sub_14204FD40 / sub_142050690 on v1.06).
// RCX at entry IS the owner container -- push it to TLS along with the iter's RSP. The setter (deeper in the stack)
// reads the TLS, and the gate only permits the substitute when the current host belongs to the elected player set.
//
// Election is a hit-frequency histogram: the player has multiple sub-actor hosts that fire many times per apply, NPCs
// LOD-cull down to 1 host each. After an election floor of k hits, any parent with hits >= top * 0.1 is admitted
// (admits dual-parent player + tolerates LOD variance). Below floor or on cold start the gate stays permissive so the
// apply-window opening render pass isn't dropped.

#include <cstdint>

namespace Transmog::ColorOverride::HostScope
{
    // Setter-side gate. Pass the setter's current RSP so the gate can detect "iter has already returned" (= no live
    // owner).
    //
    // Returns true (process) on:
    //   - no iter has run yet (cold start of the DLL)
    //   - iter has returned (setter_rsp >= tl_ownerRsp) -- permissive because the setter's call frame isn't covered by
    //     an iter
    //   - cluster has not yet elected (warm-up below floor)
    //   - current owner IS in the elected player set
    // Returns false (skip) on:
    //   - inside an iter, owner is freed/junk (tl_ownerParent == 0)
    //   - inside an iter, election fired, owner is NOT in player set
    bool is_current_host_player_owned(std::uintptr_t setter_rsp) noexcept;

    // Reset the cluster histogram. Call from transmog_apply at the start of each apply window so the election is built
    // from fresh data instead of accumulating across applies.
    void begin_apply_window() noexcept;

    // Install MidHooks on the per-host owner-container vfuncs. Idempotent. Returns true when both hooks installed
    // cleanly.
    bool init();

    // Per-window diagnostic counters surfaced via snapshot_stats().
    struct Stats
    {
        std::uint64_t entered; // iter-hook fired
        std::uint64_t player;  // matched elected player parent
        std::uint64_t npc;     // not elected (would bleed)
        std::uint64_t freed;   // RCX looked junk
        std::uint64_t stale;   // gate fell back permissive (iter returned)
    };
    Stats snapshot_stats() noexcept;
} // namespace Transmog::ColorOverride::HostScope
