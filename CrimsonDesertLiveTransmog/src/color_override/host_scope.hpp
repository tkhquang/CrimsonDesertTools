#ifndef TRANSMOG_COLOR_OVERRIDE_HOST_SCOPE_HPP
#define TRANSMOG_COLOR_OVERRIDE_HOST_SCOPE_HPP

/**
 * @file host_scope.hpp
 * @brief Host-scoped gate for the dye-setter substitute.
 * @details The engine frame-render walks every host, the player plus roughly 30 NPCs, on every apply. The
 *          SetterSubstitute detour fires on every 4-byte material-property write inside the apply window, so without
 *          a gate the user RGB bleeds onto every visible character.
 *
 *          The gate hooks the per-host owner-container vfuncs that walk the matInst lists, reached through the
 *          HostScopeVfunc1 and HostScopeVfunc2 anchors. RCX at entry IS the owner container, and the hook pushes it
 *          to TLS along with the iter RSP. The setter sits deeper in the stack, reads that TLS, and permits the
 *          substitute only when the current host belongs to the elected player set.
 *
 *          Election is a hit-frequency histogram. The player has several sub-actor hosts that fire many times per
 *          apply, while NPCs LOD-cull down to one host each. Past an election floor of k hits, any parent with
 *          hits >= top * 0.1 is admitted, which admits the dual-parent player and tolerates LOD variance. Below the
 *          floor, and on cold start, the gate stays permissive so the render pass that opens the apply window
 *          survives.
 */

#include <DetourModKit/hook.hpp>

#include <cstdint>

namespace Transmog::ColorOverride::HostScope
{
    /**
     * @brief Decides whether the current host belongs to the elected player set.
     * @param setter_rsp The setter current RSP, which lets the gate detect that the iter already returned and no live
     *        owner covers this frame.
     * @return True (process) when no iter ran yet on a cold start, when the iter returned and the setter call frame is
     *         not covered by one, when the cluster has not elected yet, or when the current owner IS in the elected
     *         player set. False (skip) when the call is inside an iter and the owner is freed or junk, or inside an
     *         iter that elected and the owner is NOT in the player set.
     * @note Callback-safe: a bounded lock-free table scan with no allocation.
     */
    bool is_current_host_player_owned(std::uintptr_t setter_rsp) noexcept;

    /**
     * @brief Resets the cluster histogram.
     * @details The apply path calls this at the start of each apply window, so the election builds from fresh data
     *          instead of accumulating across applies.
     * @note Best-effort: a bounded relaxed store loop that concurrent iter fires race against harmlessly.
     */
    void begin_apply_window() noexcept;

    /**
     * @brief Installs the mid-hooks on the per-host owner-container vfuncs.
     * @param hooks Hook stack that takes ownership of both installed hooks.
     * @return True when both hooks installed and armed cleanly.
     * @details Idempotent.
     * @note Setup/control-plane only: it resolves anchors and patches code.
     */
    bool init(DetourModKit::hook::HookStack &hooks);

    /// Per-window diagnostic counters surfaced by snapshot_stats().
    struct Stats
    {
        std::uint64_t entered{}; // iter-hook fired
        std::uint64_t player{};  // matched elected player parent
        std::uint64_t npc{};     // not elected, so a substitute here bleeds
        std::uint64_t freed{};   // RCX looked like junk
        std::uint64_t stale{};   // gate fell back permissive because the iter returned
    };

    /**
     * @brief Reads the diagnostic counters.
     * @return A snapshot of the per-window counters. The five loads are independent, so the fields can straddle a
     *         concurrent fire.
     * @note Callback-safe: five relaxed atomic loads.
     */
    Stats snapshot_stats() noexcept;
} // namespace Transmog::ColorOverride::HostScope

#endif // TRANSMOG_COLOR_OVERRIDE_HOST_SCOPE_HPP
