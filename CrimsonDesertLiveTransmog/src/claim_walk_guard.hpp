#ifndef TRANSMOG_CLAIM_WALK_GUARD_HPP
#define TRANSMOG_CLAIM_WALK_GUARD_HPP

#include <DetourModKit/hook.hpp>

/**
 * @file claim_walk_guard.hpp
 * @brief Null-owner guard for the engine's claim-vector walks.
 *
 * The engine erases the appearance-claim vector on an assembly node (data at `node+0x58`, count at `node+0x60`,
 * stride 16, owning pointer at `entry+0x08`) NON-ATOMICALLY. The erase releases the owner, nulls the slot, shifts the
 * tail down one entry at a time and nulls each source slot as it moves, then decrements the count once the whole
 * shift finishes. For the duration of that shift, entries inside `[0, count)` hold a NULL owner.
 *
 * The engine's walkers do not tolerate that. They load `*(entry+0x08)` and immediately dereference it at `+0x28`
 * with no null check, so a walk that overlaps an erase faults on a null read. Nothing guards against the overlap:
 * neither the erase, nor the walkers, nor the tear-down above them takes a lock. The engine survives it because its
 * scheduler never runs its own erases and walks concurrently.
 *
 * LT drives equips and tear-downs from its apply worker, and engine scene-graph code runs inline on whichever thread
 * calls it. Those erases therefore overlap the engine's own job-thread walks, and the fault is reachable. It is not
 * specific to any one removal primitive - every path that erases a claim opens the same window, including the
 * erases the engine performs inside a plain equip.
 *
 * The engine's job scheduler is not open to us, and an atomic erase means a reimplementation of refcounted removal
 * against several engine globals. This guard takes the other side: it makes the WALK tolerate the window, which is
 * the smaller and safer intervention. A null owner means "this entry is being erased", and a skip matches what the
 * walker observes a moment later, once the count catches up.
 *
 * The guard is a managed mid-hook rather than a hand-written stub. A stub costs less per entry - these sites sit
 * inside per-entry loops that run during the frame, and a mid-hook pays a full context save and restore where a stub
 * pays two instructions - but a stub also owns its own lifetime, and on unload it leaves the patched site jumping
 * into freed memory. Teardown removes the mid-hook with the rest of the hook table, and the callback itself is a
 * single compare, so the cost stays a fixed prologue rather than anything that scales with the walk.
 */

namespace Transmog::ClaimWalkGuard
{
    /**
     * @brief Install the guard at every claim-walk site found in the host module.
     * @param hooks The mod's hook stack. It receives every installed guard, so teardown restores the sites
     *              newest-first while the code pages are still mapped.
     * @return true when the call patches at least one site.
     * @note Safe to call more than once. A later call is a no-op.
     * @note Setup/control-plane only: scans the host image and patches code. Never call it under the loader lock.
     */
    [[nodiscard]] bool install(DetourModKit::hook::HookStack &hooks) noexcept;

    /// Number of sites patched by @ref install.
    [[nodiscard]] unsigned patched_site_count() noexcept;
} // namespace Transmog::ClaimWalkGuard

#endif // TRANSMOG_CLAIM_WALK_GUARD_HPP
