#pragma once

/**
 * @file claim_walk_guard.hpp
 * @brief Null-owner guard for the engine's claim-vector walks.
 *
 * The appearance-claim vector on an assembly node (data at `node+0x58`, count at `node+0x60`, stride 16, owning
 * pointer at `entry+0x08`) is erased NON-ATOMICALLY. The erase releases the owner, nulls the slot, shifts the tail
 * down one entry at a time -- nulling each source slot as it moves -- and only decrements the count once the whole
 * shift has finished. For the duration of that shift, entries inside `[0, count)` hold a NULL owner.
 *
 * The engine's walkers do not tolerate that. They load `*(entry+0x08)` and immediately dereference it at `+0x28`
 * with no null check, so a walk that overlaps an erase faults on a null read. Nothing guards against the overlap:
 * neither the erase, nor the walkers, nor the tear-down above them takes a lock. The engine gets away with it
 * because its own erases and walks are scheduled as jobs that do not run concurrently.
 *
 * LT drives equips and tear-downs from its apply worker, and engine scene-graph code runs inline on whichever thread
 * calls it. Those erases therefore overlap the engine's own job-thread walks, and the fault is reachable. It is not
 * specific to any one removal primitive -- every path that erases a claim opens the same window, including the
 * erases the engine performs inside a plain equip.
 *
 * Joining the engine's job scheduler is not available to us, and making its erase atomic would mean reimplementing
 * refcounted removal against several engine globals. This guard takes the other side: it makes the WALK tolerate the
 * window, which is the smaller and safer intervention. A null owner means "this entry is being erased", and skipping
 * it matches what the walker would have observed a moment later, once the count caught up.
 *
 * The guard is a managed mid-hook rather than a hand-written stub. A stub would be cheaper per entry -- these sites
 * sit inside per-entry loops that run during the frame, and a mid-hook pays a full context save and restore where a
 * stub pays two instructions -- but a stub also owns its own lifetime, and on unload the patched site is left
 * jumping into freed memory. The mid-hook is torn down with the rest of the hook table, and the callback itself is a
 * single compare, so the cost stays a fixed prologue rather than anything that scales with the walk.
 */

namespace Transmog::ClaimWalkGuard
{
    /**
     * @brief Install the guard at every claim-walk site found in the host module.
     * @return true if at least one site was patched.
     * @note Safe to call more than once; later calls are no-ops.
     */
    [[nodiscard]] bool install() noexcept;

    /// Number of sites patched by @ref install.
    [[nodiscard]] unsigned patched_site_count() noexcept;
} // namespace Transmog::ClaimWalkGuard
